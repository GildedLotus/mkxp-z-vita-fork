/*
** gl-meta.cpp
**
** This file is part of mkxp.
**
** Copyright (C) 2014 - 2021 Amaryllis Kulla <ancurio@mapleshrine.eu>
**
** mkxp is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 2 of the License, or
** (at your option) any later version.
**
** mkxp is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with mkxp.  If not, see <http://www.gnu.org/licenses/>.
*/

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "frameprofile.h"
#endif
#include "gl-meta.h"
#include "gl-fun.h"
#include "sharedstate.h"
#include "glstate.h"
#include "quad.h"
#include "config.h"
#include "etc.h"

#ifdef MKXPZ_SOFTWARE_BITMAPS
#include "shader.h"
#include "graphics.h"
#include "exception.h"
#include "debugwriter.h"
#include <SDL_timer.h>
#include <assert.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <new>
#include <string.h>
#include <vector>
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "vita_glue.h"
#include <stdio.h>
#endif

#ifdef MKXPZ_SOFTWARE_BITMAPS
GLRenderErrorScope *GLRenderErrorScope::active = nullptr;

GLRenderErrorScope::GLRenderErrorScope()
	: outer(active), previous(gl.GetError),
	  source(outer ? outer->source : gl.GetError), error(GL_NO_ERROR)
{
	/* Only errors predating the whole operation are stale. */
	while (gl.GetError() != GL_NO_ERROR) {}
	active = this;
	gl.GetError = []() -> GLenum {
		const GLenum value = active->source();
		for (GLRenderErrorScope *scope = active; scope; scope = scope->outer)
			if (scope->error == GL_NO_ERROR) scope->error = value;
		return value;
	};
}

GLRenderErrorScope::~GLRenderErrorScope()
{
	while (gl.GetError() != GL_NO_ERROR) {}
	gl.GetError = previous;
	active = outer;
}

void GLRenderErrorScope::check(const char *operation)
{
	while (gl.GetError() != GL_NO_ERROR) {}
	if (error != GL_NO_ERROR)
		throw Exception(Exception::MKXPError, "%s failed (GL error 0x%x)",
		                operation, (unsigned)error);
}
#endif

namespace FBO
{
	ID boundFramebufferID;
}

#ifdef MKXPZ_SOFTWARE_BITMAPS
/* ==========================================================================
 * Bounded GPU kernel-object budget
 *
 * Everything here exists because render surfaces come from a small fixed GPU
 * pool that every texture and VBO also drains, first come first served, and
 * a surface that cannot be created does not fail politely -- re-binding its
 * FBO poisons the context and the NEXT glBindFramebuffer data-aborts. See
 * the header comment in gl-util.h.
 * ========================================================================== */

namespace GPUBudget
{

unsigned liveTextures = 0;
unsigned liveVBOs = 0;
unsigned liveSurfaces = 0;
uint64_t liveCpuBitmapBytes = 0;
bool sealed = false;

/* The engine's fixed render surfaces, reserved once at boot. */
static TEXFBO fixedSurface[SurfaceCount];
static bool fixedReserved = false;
static bool integerScaleReserved = false;
static bool fixedStorageValid[SurfaceCount] = {};
static bool fixedResizeComplete = true;

bool fixedSurfacesValid()
{
	if (!fixedReserved)
		return true;
	if (!fixedResizeComplete)
		return false;
	const int count = integerScaleReserved ? SurfaceCount : IntegerScale;
	for (int i = 0; i < count; ++i)
		if (!fixedStorageValid[i])
			return false;
	return true;
}

static bool surfaceStorageValid(GLuint fbo)
{
	for (int i = 0; i < SurfaceCount; ++i)
		if (fbo && fixedSurface[i].fbo.gl == fbo)
			return fixedStorageValid[i];
	return true;
}

static void setSurfaceStorageValid(GLuint fbo, bool valid)
{
	for (int i = 0; i < SurfaceCount; ++i)
		if (fbo && fixedSurface[i].fbo.gl == fbo)
			fixedStorageValid[i] = valid;
}

/* Scene changes seen so far; the telemetry line's "scene=" field when the
 * caller has no better name (the engine cannot see RGSS scene classes). */
static unsigned sceneSerial = 0;

/* ==========================================================================
 * State for the frame instrumentation, the
 * seal policy and the deferred-release queue. Declared in gl-util.h.
 * ========================================================================== */

/* REAL SDL_GL_SwapWindow calls so far. Every line below carries it, because
 * "which frame" is the one question a crash dump cannot answer. */
static uint64_t realSwaps = 0;

/* Set while headroomCanary() is probing. Its probes ARE post-seal render
 * surfaces on purpose; they must not report themselves as violations, and
 * ordinary telemetry must never enter this diagnostic. */
static bool canaryRunning = false;

/* One synced line per distinct seal-violation site. Three sites exist in
 * the engine (render surface, shader program, VAO); the table is generous. */
static const size_t SEAL_SITES = 8;
static const char *sealSiteSeen[SEAL_SITES];
static size_t sealSiteCount = 0;

/* "first 16, then every 256th, carrying the suppressed count" -- the rate at
 * which a per-frame diagnostic can log without becoming the bottleneck. */
struct RateLimit
{
	uint64_t seen;
	unsigned suppressed;

	RateLimit() : seen(0), suppressed(0) {}

	bool allow()
	{
		++seen;
		/* 256 is a constant, so this is an AND, not a Cortex-A9 divide. */
		if (seen <= 16 || (seen % 256) == 0)
			return true;

		++suppressed;
		return false;
	}

	unsigned take()
	{
		const unsigned n = suppressed;
		suppressed = 0;
		return n;
	}
};

static RateLimit glErrorRate;
static RateLimit respecifyRate;

/* Milliseconds on the engine's own clock. Kept here so gl-util.h, which
 * every display TU includes, does not gain an SDL_timer dependency. */
static double nowMs()
{
	const Uint64 freq = SDL_GetPerformanceFrequency();

	if (!freq)
		return 0.0;

	return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)freq;
}

/* ---- Deferred release --------------------------------------------------- */

/* N = 2: the window has two flip buffers and exactly one flip may be in
 * flight, so after swap k returns, swap k-1's render is complete and swap k's
 * may not be. Two swaps is the first safe point. */
unsigned deferSwaps[DeferKindCount] = { 2, 2, 2 };
bool syncBeforeRespecify[DeferKindCount] = { true, true, true };

/* The GPU drain. vitaGL's glFinish is a real sceGxmFinish fence: it waits for
 * every submitted render, including an in-flight upload into a texture the
 * driver no longer tracks. Nothing is read back and no framebuffer is
 * rebound, so it is safe to call mid-composite. */
static void drainGPU()
{
	/* vitaGL's glFinish is a real sceGxmFinish fence: no 1x1 read-back and
	 * no FBO rebind. */
	glFinish();
}

/* Pending retirement is separate from live textures/cache residency. At most
 * 256 records (6144 bytes) and 16 MiB of recorded texture/buffer storage are
 * retained, even while swapping. Driver metadata/ghosts are not included.
 * A single larger object stays caller-owned until a synchronous drain; no
 * queue byte addition can overflow. Cleanup never allocates queue storage. */
static const size_t DEFER_MAX_ENTRIES = 256;
static const uint64_t DEFER_MAX_BYTES = 16u * 1024u * 1024u;

struct DeferredObject
{
	unsigned kind;
	GLuint id;
	uint64_t swapAtEnqueue;
	uint64_t bytes;
};

static DeferredObject deferQueue[DEFER_MAX_ENTRIES];
static size_t deferCount = 0;
static uint64_t deferBytes = 0;

/* id -> bytes of storage, per kind: what makes a re-specification
 * distinguishable from the same-size whole-level refresh that is this
 * backend's normal per-frame upload. */
static std::map<uint64_t, uint64_t> storageBytes;
static GLuint boundObject[SlotCount] = { 0, 0, 0 };

static uint64_t storageKey(DeferKind kind, GLuint id)
{
	return ((uint64_t)kind << 32) | (uint64_t)id;
}

static void releaseNow(const DeferredObject &obj)
{
	GLuint id = obj.id;

	if (obj.kind == (unsigned)DeferTexture)
		gl.DeleteTextures(1, &id);
	else if (obj.kind == (unsigned)DeferBuffer)
	{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		const bool profile = vita_glue_frame_profile_interval != 0;
		const double begin = profile ? nowMs() : 0;
#endif
		gl.DeleteBuffers(1, &id);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		if (profile)
		{
			const double elapsed = nowMs() - begin;
			char line[192];
			snprintf(line, sizeof(line),
			         "vita-buffer-delete: id=%u bytes=%llu swaps=%llu frame=%llu us=%llu",
			         (unsigned)id, (unsigned long long)obj.bytes,
			         (unsigned long long)(realSwaps - obj.swapAtEnqueue),
			         (unsigned long long)realSwaps,
			         (unsigned long long)(elapsed > 0 ? elapsed * 1000.0 : 0));
			syncedLine(line);
		}
#endif
	}
	else if (obj.kind == (unsigned)DeferFramebuffer)
		gl.DeleteFramebuffers(1, &id);
}

static void forgetBytes(uint64_t bytes)
{
	deferBytes = bytes > deferBytes ? 0 : deferBytes - bytes;
}

void syncedLine(const char *line)
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* One locked sceIoWrite + sceIoSyncByFd when the log-sync marker is on,
	 * so a process the system stops still has this line on disk -- which is
	 * exactly how a crashed run is read (vita_glue.h, VITA_GLUE_LOG_SYNC). */
	vita_glue_trace(line);
#else
	Debug() << line;
#endif
}

uint64_t frameCounter()
{
	return realSwaps;
}

void deferDelete(DeferKind kind, GLuint id, uint64_t bytes)
{
	if (!id)
		return;

	std::map<uint64_t, uint64_t>::iterator it =
	    storageBytes.find(storageKey(kind, id));
	if (!bytes && it != storageBytes.end())
		bytes = it->second;

	const DeferredObject obj = { (unsigned)kind, id, realSwaps, bytes };
	const bool deferred = deferSwaps[kind] != 0;

	/* Secure retirement before changing the ledger or bindings. The drain
	 * covers the incoming object too, including a single oversized store. */
	if (deferred && (deferCount == DEFER_MAX_ENTRIES ||
	                 bytes > DEFER_MAX_BYTES - deferBytes))
	{
		/* queue pressure is the engine's one pressure signal,
		 * and the recycle pool is live storage; park its names in the
		 * queue this drain then releases. No-op when the pool is empty. */
		texRecycleDrain("pressure");
		deferDrain("pressure", true);
	}

	if (deferred && bytes <= DEFER_MAX_BYTES)
	{
		deferQueue[deferCount++] = obj;
		deferBytes += bytes;
	}
	else
		releaseNow(obj);

	if (it != storageBytes.end())
		storageBytes.erase(it);

	/* Match deletion's implicit unbind now, not when retirement matures. */
	if (kind == DeferFramebuffer && FBO::boundFramebufferID.gl == id)
		FBO::unbind();

	if (kind == DeferTexture)
	{
		if (boundObject[SlotTexture] == id)
			boundObject[SlotTexture] = 0;
	}
	else if (kind == DeferBuffer)
	{
		if (boundObject[SlotArrayBuffer] == id)
			boundObject[SlotArrayBuffer] = 0;
		if (boundObject[SlotElementBuffer] == id)
			boundObject[SlotElementBuffer] = 0;
	}
}

unsigned deferPending()
{
	return (unsigned)deferCount;
}

uint64_t deferPendingBytes()
{
	return deferBytes;
}

/* ===== bounded texture recycle pool BEGIN =====
 * Rationale lives with the declarations in gl-util.h. Fixed storage,
 * no allocation on any path; overflow and drains retire through the
 * deferred queue, two real swaps behind the last sample. */
static const size_t RECYCLE_MAX_ENTRIES = 32;
static const uint64_t RECYCLE_MAX_BYTES = 16u * 1024u * 1024u;

struct RecycledTexture
{
	GLuint id;
	uint16_t width;
	uint16_t height;
	uint64_t bytes;
};

static RecycledTexture recycleQueue[RECYCLE_MAX_ENTRIES];
static size_t recycleCount = 0;
static uint64_t recycleBytes = 0;

/* Counted toward one vita-gpu line per frame with pool traffic. */
static unsigned recycleOffers = 0, recycleTakes = 0, recycleRetires = 0;
static bool recycleStatsDirty = false;

/* A parked name is still a live texture; it stops counting here, where it
 * really retires, as TEX::del's names do. */
static void recycleRetire(GLuint id, uint64_t bytes)
{
	if (id && liveTextures)
		--liveTextures;
	deferDelete(DeferTexture, id, bytes);
}

GLuint texRecycleTake(int width, int height)
{
	if (width <= 0 || height <= 0)
		return 0;

	/* Newest first: the name dropped most recently is the one a scene
	 * rebuild is most likely re-creating. */
	for (size_t i = recycleCount; i-- > 0; )
	{
		if (recycleQueue[i].width == (uint16_t)width &&
		    recycleQueue[i].height == (uint16_t)height)
		{
			const GLuint id = recycleQueue[i].id;
			recycleBytes -= recycleQueue[i].bytes;
			for (size_t j = i; j + 1 < recycleCount; ++j)
				recycleQueue[j] = recycleQueue[j + 1];
			--recycleCount;
			++recycleTakes;
			recycleStatsDirty = true;
			return id;
		}
	}

	return 0;
}

void texRecycleOffer(GLuint id, int width, int height)
{
	++recycleOffers;
	recycleStatsDirty = true;

	/* width/height beyond the 16-bit entry fields cannot be matched
	 * exactly by take(), so such a name is retired, not parked. */
	const bool sized = width > 0 && height > 0 &&
	                   width <= (int)0xffff && height <= (int)0xffff;
	const uint64_t bytes =
	    sized ? (uint64_t)width * (uint64_t)height * 4u : 0;

	if (!id || !sized || bytes > RECYCLE_MAX_BYTES)
	{
		/* Nothing worth parking: retire it the ordinary way. bytes=0
		 * makes deferDelete reuse the storage the respecify ledger
		 * recorded for the name. */
		recycleRetire(id, 0);
		return;
	}

		/* One entry per GL name, ever: a duplicate would retire twice. */
		for (size_t i = 0; i < recycleCount; ++i)
			if (recycleQueue[i].id == id)
				return;

		/* Room first: the append must never write past the fixed storage. */
		if (recycleCount == RECYCLE_MAX_ENTRIES)
		{
			const RecycledTexture oldest = recycleQueue[0];
			--recycleCount;
			recycleBytes -= oldest.bytes;
			for (size_t i = 0; i < recycleCount; ++i)
				recycleQueue[i] = recycleQueue[i + 1];
			++recycleRetires;
			recycleRetire(oldest.id, oldest.bytes);
		}

		recycleQueue[recycleCount].id = id;
		recycleQueue[recycleCount].width = (uint16_t)width;
		recycleQueue[recycleCount].height = (uint16_t)height;
		recycleQueue[recycleCount].bytes = bytes;
		++recycleCount;
		recycleBytes += bytes;

		/* Overflow beyond the byte budget retires the OLDEST names: an
		 * eviction is a release, and the frames that last sampled the name
		 * may still be in flight. */
		while (recycleBytes > RECYCLE_MAX_BYTES)
		{
			const RecycledTexture oldest = recycleQueue[0];
			--recycleCount;
			recycleBytes -= oldest.bytes;
			for (size_t i = 0; i < recycleCount; ++i)
				recycleQueue[i] = recycleQueue[i + 1];
			++recycleRetires;
			recycleRetire(oldest.id, oldest.bytes);
		}
}

void texRecycleDrain(const char *why)
{
	if (recycleCount == 0)
		return;

	const size_t entries = recycleCount;
	const uint64_t bytes = recycleBytes;

	/* deferDelete can observe queue pressure and call back in here;
	 * recycleCount only shrinks, so the re-entry terminates. */
	while (recycleCount > 0)
	{
		const RecycledTexture entry = recycleQueue[--recycleCount];
		recycleBytes -= entry.bytes;
		++recycleRetires;
		recycleRetire(entry.id, entry.bytes);
	}

	if (!telemetryEnabled())
		return;

	char line[160];
	snprintf(line, sizeof(line),
	         "vita-gpu: tex-recycle drain why=%s entries=%u bytes=%u "
	         "frame=%u",
	         why ? why : "?", (unsigned)entries, (unsigned)bytes,
	         (unsigned)frameCounter());
	syncedLine(line);
}
/* ===== bounded texture recycle pool END ===== */

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
void logMemoryLedger()
{
	if (!vita_glue_memory_ledger_due())
		return;
	VitaMemoryResources r = {};
	r.cpu_pixels = liveCpuBitmapBytes;
	r.textures = liveTextures;
	r.vbos = liveVBOs;
	r.surfaces = liveSurfaces;
	r.pending = (unsigned)deferCount;
	r.pending_bytes = deferBytes;
	for (const auto &entry : storageBytes)
	{
		const unsigned kind = (unsigned)(entry.first >> 32);
		if (kind == DeferTexture) r.texture_bytes += entry.second;
		if (kind == DeferBuffer) r.buffer_bytes += entry.second;
	}
	vita_glue_memory_ledger_log(&r);
}
#endif

void deferDrain(const char *why, bool waitForGPU)
{
	/* The wait comes first and happens even on an empty queue: at freeze it
	 * is the point of the call, and it is the highest-value instrument in
	 * this drain -- the driver retries a wait for a render for a very long
	 * time, so a render that never completes turns a silent process stop into a
	 * log that ends at this call's own BEGIN line. */
	if (waitForGPU)
	{
		/* A failed read proves no completion. Retain ownership and retry;
		 * a permanently broken driver may stall here, never free in flight. */
		do
		{
			while (gl.GetError() != GL_NO_ERROR) {}
			drainGPU();
		} while (gl.GetError() != GL_NO_ERROR);
	}

	if (deferCount == 0)
		return;

	const unsigned objects = (unsigned)deferCount;
	const uint64_t bytes = deferBytes;

	for (size_t i = 0; i < deferCount; ++i)
		releaseNow(deferQueue[i]);

	deferCount = 0;
	deferBytes = 0;

	if (!telemetryEnabled())
		return;

	char line[192];
	snprintf(line, sizeof(line),
	         "vita-gpu: deferred drain why=%s objects=%u bytes=%u frame=%u",
	         why ? why : "?", objects, (unsigned)bytes, (unsigned)realSwaps);
	syncedLine(line);
}

void frameAdvance()
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	if (vita_glue_frame_profile_interval && FrameProfile::state.running) {
		const unsigned values[] = { liveTextures, liveVBOs, liveSurfaces,
		    (unsigned)liveCpuBitmapBytes, (unsigned)deferCount, (unsigned)deferBytes };
		for (unsigned i = 0; i < 6; ++i)
			if (values[i] > FrameProfile::state.batch.gpu[i])
				FrameProfile::state.batch.gpu[i] = values[i];
	}
#endif
	++realSwaps;

	/* one line per frame that had pool traffic, so a hardware
	 * run can see the pool's occupancy and its reuse hit rate without any
	 * per-operation logging. */
	if (recycleStatsDirty)
	{
		recycleStatsDirty = false;

		if (telemetryEnabled())
		{
			char line[160];
			snprintf(line, sizeof(line),
			         "vita-gpu: tex-recycle pooled=%u bytes=%u offers=%u "
			         "takes=%u retires=%u frame=%u",
			         (unsigned)recycleCount, (unsigned)recycleBytes,
			         recycleOffers, recycleTakes, recycleRetires,
			         (unsigned)realSwaps);
			syncedLine(line);
		}

		recycleOffers = 0;
		recycleTakes = 0;
		recycleRetires = 0;
	}

	if (deferCount == 0)
		return;

	size_t kept = 0;

	for (size_t i = 0; i < deferCount; ++i)
	{
		const DeferredObject obj = deferQueue[i];

		if (realSwaps - obj.swapAtEnqueue >= deferSwaps[obj.kind])
		{
			releaseNow(obj);
			forgetBytes(obj.bytes);
		}
		else
		{
			deferQueue[kept++] = obj;
		}
	}

	deferCount = kept;
}

void noteBind(Slot slot, GLuint id)
{
	/* Gate both client-array and VAO draws, but allow the independent error panel. */
	if (slot == SlotElementBuffer && id && !fixedSurfacesValid())
		for (int i = 0; i < SurfaceCount; ++i)
			if (fixedSurface[i].fbo.gl &&
			    (FBO::boundFramebufferID == fixedSurface[i].fbo ||
			     boundObject[SlotTexture] == fixedSurface[i].tex.gl))
				throw Exception(Exception::MKXPError,
				                "Render targets unavailable after failed resize; retry Graphics.resize_screen");
	if ((unsigned)slot < (unsigned)SlotCount)
		boundObject[slot] = id;
}

void respecify(Slot slot, DeferKind kind, uint64_t bytes)
{
	/* Probes have scoped ownership and are never sampled or re-specified. */
	if (canaryRunning)
		return;

	if ((unsigned)slot >= (unsigned)SlotCount)
		return;

	const GLuint id = boundObject[slot];

	if (!id)
		return;

	const uint64_t key = storageKey(kind, id);
	std::map<uint64_t, uint64_t>::iterator it = storageBytes.find(key);

	if (it == storageBytes.end())
	{
		/* First storage for this object: nothing is being released. */
		storageBytes[key] = bytes;
		return;
	}

	if (it->second == bytes)
		/* Same-size whole-level refresh. The driver reuses the storage in
		 * place, and this is the per-frame upload traffic the whole backend
		 * is built on -- it has to stay free. */
		return;

	/* Size changes release storage the GPU may still reference. Unchanged
	 * GL names do NOT preserve firmware sync objects: an attached level's
	 * render surface is destroyed and recreated. Explicit screen resize still requires pool headroom. */
	if (syncBeforeRespecify[kind])
		drainGPU();

	const uint64_t was = it->second;
	it->second = bytes;

	if (!telemetryEnabled() || !respecifyRate.allow())
		return;

	char line[192];
	snprintf(line, sizeof(line),
	         "vita-gpu: respec kind=%u id=%u bytes=%u was=%u synced=%d "
	         "frame=%u suppressed=%u",
	         (unsigned)kind, (unsigned)id, (unsigned)bytes, (unsigned)was,
	         syncBeforeRespecify[kind] ? 1 : 0, (unsigned)realSwaps,
	         respecifyRate.take());
	syncedLine(line);
}

/* ---- Frame instrumentation ------------------------------------------- */

void pollGLError(const char *at)
{
	if (!telemetryEnabled())
		return;

	/* gl.GetError is never NULL after initGLFunctions() (it throws when the
	 * lookup fails), and the boot warm-up already calls it unguarded.
	 *
	 * glGetError reports one flag per call and clears it; drain the queue so
	 * the rest are not blamed on the next site. */
	for (unsigned i = 0; i < 8; ++i)
	{
		const GLenum err = gl.GetError();

		if (err == GL_NO_ERROR)
			break;

		if (!glErrorRate.allow())
			continue;

		char line[160];
		snprintf(line, sizeof(line),
		         "vita-gpu: glerror=0x%04x at=%s frame=%u suppressed=%u",
		         (unsigned)err, at ? at : "?", (unsigned)realSwaps,
		         glErrorRate.take());
		syncedLine(line);
	}
}

void noteGLError(const char *at, GLenum err)
{
	if (!glErrorRate.allow())
		return;

	char line[160];
	snprintf(line, sizeof(line),
	         "vita-gpu: glerror=0x%04x at=%s frame=%u suppressed=%u",
	         (unsigned)err, at ? at : "?", (unsigned)realSwaps,
	         glErrorRate.take());
	syncedLine(line);
}

/* A frame at 40 fps is 25 ms and at 60 fps 16.7 ms; the FPS limiter has
 * already slept before the swap, so anything past this is the driver waiting
 * for a render rather than for the display. */
static const double SWAP_WARN_MS = 200.0;
static double swapStartMs = 0.0;
static RateLimit swapWarnRate;

void swapBegin()
{
	if (!telemetryEnabled())
		return;

	swapStartMs = nowMs();
}

void swapEnd()
{
	if (!telemetryEnabled())
		return;

	const double ms = nowMs() - swapStartMs;

	if (ms < SWAP_WARN_MS || !swapWarnRate.allow())
		return;

	const int whole = (int)ms;
	const int hundredths = (int)((ms - (double)whole) * 100.0);

	char line[192];
	snprintf(line, sizeof(line),
	         "vita-gpu: slow swap ms=%d.%02d frame=%u suppressed=%u; the "
	         "previous frame's render had not completed",
	         whole, hundredths, (unsigned)realSwaps, swapWarnRate.take());
	syncedLine(line);
}

void breadcrumb(const char *phase, const char *what)
{
	if (!telemetryEnabled())
		return;

	char line[128];
	snprintf(line, sizeof(line), "vita-gpu: %s %s frame=%u",
	         phase ? phase : "?", what ? what : "?", (unsigned)realSwaps);
	syncedLine(line);
}

static const uint64_t UPLOAD_LOG_MIN_BYTES = 1024u * 1024u;
static double uploadStartMs = 0.0;

void uploadBegin(uint64_t bytes)
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	if (vita_glue_frame_profile_interval && FrameProfile::state.running) {
		FrameProfile::state.batch.upload_bytes += bytes;
		++FrameProfile::state.batch.count[FrameProfile::Upload];
	}
#endif
	if (!telemetryEnabled() || bytes < UPLOAD_LOG_MIN_BYTES)
		return;

	uploadStartMs = nowMs();
}

void uploadEnd(int width, int height, uint64_t bytes)
{
	if (!telemetryEnabled() || bytes < UPLOAD_LOG_MIN_BYTES)
		return;

	/* The measured cost of a whole-level upload is about 1 ms fixed plus
	 * 13 ns/px, so an 8 MiB tile atlas is tens of
	 * milliseconds -- worth naming in the log when a frame goes missing.
	 * Split into integer parts: %f on newlib is not worth the code. */
	const double ms = nowMs() - uploadStartMs;
	const int whole = ms > 0.0 ? (int)ms : 0;
	const int hundredths = ms > 0.0 ? (int)((ms - (double)whole) * 100.0) : 0;

	char line[160];
	snprintf(line, sizeof(line),
	         "vita-gpu: upload %dx%d bytes=%u ms=%d.%02d frame=%u",
	         width, height, (unsigned)bytes, whole, hundredths,
	         (unsigned)realSwaps);
	syncedLine(line);
}

/* ---- Seal semantics --------------------------------------------------- */

bool sealAbortEnabled()
{
	static int cached = -1;

	if (cached < 0)
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		cached = vita_glue_gpu_seal_abort_enabled() ? 1 : 0;
#else
		cached = 0;
#endif

	return cached != 0;
}

/* True the first time a given site violates the seal. */
static bool firstSealViolation(const char *what)
{
	const char *name = what ? what : "?";

	for (size_t i = 0; i < sealSiteCount; ++i)
		if (strcmp(sealSiteSeen[i], name) == 0)
			return false;

	if (sealSiteCount < SEAL_SITES)
		sealSiteSeen[sealSiteCount++] = name;

	return true;
}

/* ---- The fixed-surface registry is the single owner ------------------- */

bool isReservedFBO(GLuint fbo)
{
	if (!fbo)
		return false;

	for (int i = 0; i < SurfaceCount; ++i)
		if (fixedSurface[i].fbo.gl == fbo)
			return true;

	return false;
}

void fixedSurfaceLost(GLuint fbo)
{
	if (!fbo)
		return;

	for (int i = 0; i < SurfaceCount; ++i)
		if (fixedSurface[i].fbo.gl == fbo)
		{
			fixedStorageValid[i] = false;
			TEXFBO::clear(fixedSurface[i]);
		}
}

/* Stop the headroom canary from spinning on a driver that never refuses.
 * Hardware gives 8 on an empty process, so anything near this is a pass. */
static const size_t CANARY_LIMIT = 64;

/* Logical RGSS limits; hires attachments additionally obey the GPU limit. */
static const int MAX_SCREEN_W = 640;
static const int MAX_SCREEN_H = 480;

void creationSite(const char *what)
{
	if (!sealed || canaryRunning)
		return;

	/* An assert here would be live: the player ships WITHOUT -DNDEBUG, so
	 * a seal violation would kill the game for the person
	 * playing it. A late program link or a late texture is legal GL -- it
	 * may run with a NULL sync object, which is the documented normal regime
	 * after the pool empties -- so it
	 * is reported and allowed to proceed. One line per distinct site keeps a
	 * per-frame offender from becoming the bottleneck it is reporting.
	 *
	 * The abort is still available, opt-in, for a bisecting run:
	 * ux0:/data/mkxp-z/gpu-seal-abort.enabled. */
	if (firstSealViolation(what))
	{
		char line[192];
		snprintf(line, sizeof(line),
		         "vita-gpu: BUG: %s created after boot was sealed (frame=%u); "
		         "the firmware sync pool is first come first served",
		         what ? what : "?", (unsigned)realSwaps);
		syncedLine(line);
	}

	if (sealAbortEnabled())
		abort();
}

bool sealRefusesSurface(const char *what)
{
	if (!sealed || canaryRunning)
		return false;

	/* The one object class that does not get to continue. A texture or a
	 * program can live with a NULL sync; a render surface cannot exist at
	 * all once the pool is empty -- it comes back GL_FRAMEBUFFER_UNSUPPORTED
	 * and poisons its own FBO. Refusing it in the
	 * engine is how it stays a Ruby-visible error instead of a data abort
	 * two binds later. */
	creationSite(what);

	return true;
}

void cpuPixels(int64_t deltaBytes)
{
	if (deltaBytes < 0)
	{
		const uint64_t take = (uint64_t)(-deltaBytes);
		liveCpuBitmapBytes = take > liveCpuBitmapBytes ? 0
		                   : liveCpuBitmapBytes - take;
	}
	else
	{
		liveCpuBitmapBytes += (uint64_t)deltaBytes;
	}
}

bool telemetryEnabled()
{
	static int cached = -1;

	if (cached < 0)
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		cached = vita_glue_gpu_telemetry_enabled() ? 1 : 0;
#else
		cached = 0;
#endif

	return cached != 0;
}

void logCounts(const char *scene)
{
	if (!telemetryEnabled())
		return;

	char line[256];
	snprintf(line, sizeof(line),
	         "vita-gpu: textures=%u vbos=%u surfaces=%u cpu_bitmap_bytes=%u "
	         "deferred_pending=%u deferred_bytes=%u scene=%s",
	         liveTextures, liveVBOs, liveSurfaces,
	         (unsigned)liveCpuBitmapBytes, deferPending(),
	         (unsigned)deferPendingBytes(), scene ? scene : "?");
	syncedLine(line);

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* cpu_bitmap_bytes above says how much of the newlib heap the CPU
	 * Bitmaps hold; this says how much of that heap is left at all, which
	 * no other line in the log can show. Paired here so
	 * the two numbers are always the same sample. */
	vita_glue_log_heap(scene ? scene : "?");
#endif
}

void logSceneChange()
{
	if (!telemetryEnabled())
		return;

	char name[32];
	snprintf(name, sizeof(name), "%u", ++sceneSerial);
	logCounts(name);

}

Vec2i renderScreenSize(const Config &conf, int width, int height)
{
	if (width <= 0 || height <= 0 || width > MAX_SCREEN_W || height > MAX_SCREEN_H)
		throw Exception(Exception::ArgumentError, "Screen size must be within 1x1..640x480");
	const double scale = conf.enableHires ? conf.framebufferScalingFactor : 1.0;
	const double w = width * scale, h = height * scale;
	if (!std::isfinite(w) || !std::isfinite(h) || w < 0.5 || h < 0.5 || w > 4096 || h > 4096)
		throw Exception(Exception::ArgumentError, "Scaled screen size must be within 1x1..4096x4096");
	return Vec2i((int)lround(w), (int)lround(h));
}

Vec2i bootScreenSize(const Config &conf, int rgssVersion, bool scaled)
{
	/* readGameINI resolves the RGSS version before reservation. defScreenW/H
	 * configure the WINDOW, not RGSS extents. Unknown versions use the XP
	 * ceiling; later script-requested extents take the explicit resize path. */
	const bool vx = rgssVersion == 2 || rgssVersion == 3;
	const int w = vx ? 544 : 640;
	const int h = vx ? 416 : 480;
	const Vec2i physical = renderScreenSize(conf, w, h);
	return scaled ? physical : Vec2i(w, h);
}

/* The headroom canary is opt-in through a marker staged before launch.
 * One boot reads it -- the canary call
 * SharedState::initInstance makes before seal(), the first call after
 * reserveFixedSurfaces() -- and every later call, the per-freeze ones from
 * Graphics, costs one cached load instead of an fopen on ux0. -1 means "not read since the last boot". */
static int headroomMarkerState = -1;

static bool headroomCanaryMarker()
{
	/* Deliberately separate from ordinary telemetry; boot diagnostics only.
	 * A failed marker-file allocation simply leaves the canary disabled. */
	FILE *marker = fopen("ux0:/data/mkxp-z/gpu-headroom.enabled", "r");
	if (!marker)
		return false;
	fclose(marker);
	return true;
}

void reserveFixedSurfaces(const Config &conf, int rgssVersion)
{
	assert(!fixedReserved);
	/* A boot boundary: the next canary call re-reads the opt-in marker once. */
	headroomMarkerState = -1;
	const Vec2i size = bootScreenSize(conf, rgssVersion, true);
	const int w = size.x, h = size.y;

	/* Order inside this function is irrelevant -- order of the function is
	 * everything. It runs before the global IBO, before ShaderSet compiles a
	 * program, before the first Quad's VBO and before any game asset. */
	TEXFBO::initChecked(fixedSurface[PingPong0], w, h, "ping-pong 0");
	TEXFBO::initChecked(fixedSurface[PingPong1], w, h, "ping-pong 1");
	TEXFBO::initChecked(fixedSurface[FrozenScene], w, h, "frozenScene");

	if (conf.integerScaling.active)
	{
		/* A 4th surface is about 4 more of the ~32 sync objects an empty
		 * process starts with, which is why integer scaling is off by
		 * default on this platform. The config asked, so it is created
		 * here -- at boot, checked -- and the cost is logged. */
		Debug() << "vita-gpu: integerScaling active: reserving a 4th render"
		        << "surface (about 4 more firmware sync objects)";
		TEXFBO::initChecked(fixedSurface[IntegerScale], w, h,
		                    "integerScaleBuffer");
		integerScaleReserved = true;
	}

	fixedResizeComplete = true;
	fixedReserved = true;
	FBO::unbind();

	char line[160];
	snprintf(line, sizeof(line),
	         "vita-gpu: fixed render surfaces reserved=%d at %dx%d (rgss%d, max resize %dx%d)",
	         integerScaleReserved ? 4 : 3, w, h, rgssVersion,
	         MAX_SCREEN_W, MAX_SCREEN_H);
	Debug() << line;
}

const TEXFBO &reserved(Surface which)
{
	assert(fixedReserved);
	assert(which >= 0 && which < SurfaceCount);
	assert(which != IntegerScale || integerScaleReserved);

	return fixedSurface[which];
}

TEXFBO &reservedMutable(Surface which)
{
	/* Deliberately weaker than reserved(): GraphicsPrivate binds a
	 * reference to the IntegerScale slot in its constructor whether or not
	 * the config asked for integer scaling. An unreserved slot is a zeroed
	 * TEXFBO, and every read of it is behind integerScaleStepApplicable(). */
	assert(which >= 0 && which < SurfaceCount);

	return fixedSurface[which];
}

bool haveIntegerScaleSurface()
{
	return integerScaleReserved;
}

void resizeFixedSurfaces(const Vec2i &size, const Vec2i &integerSize)
{
	const int count = integerScaleReserved ? SurfaceCount : IntegerScale;
	Vec2i before[SurfaceCount];
	const char *names[] = { "ping-pong 0", "ping-pong 1", "frozenScene", "integerScaleBuffer" };
	for (int i = 0; i < count; ++i)
	{
		const Vec2i next = i == IntegerScale ? integerSize : size;
		if (next.x <= 0 || next.y <= 0 || next.x > 4096 || next.y > 4096)
			throw Exception(Exception::ArgumentError, "Render target size must be within 1x1..4096x4096");
		before[i] = Vec2i(fixedSurface[i].width, fixedSurface[i].height);
	}
	try
	{
		for (int i = 0; i < count; ++i)
		{
			const Vec2i next = i == IntegerScale ? integerSize : size;
			TEXFBO::reallocChecked(fixedSurface[i], next.x, next.y, names[i]);
		}
	}
	catch (...)
	{
		bool restored = true;
		for (int i = count - 1; i >= 0; --i)
			try { TEXFBO::reallocChecked(fixedSurface[i], before[i].x, before[i].y, names[i]); }
			catch (...) { restored = false; }
		if (!restored)
		{
			/* A complete attachment can still have uncommitted dimensions. */
			fixedResizeComplete = false;
			throw Exception(Exception::MKXPError, "Screen resize failed and the driver refused recovery");
		}
		throw;
	}
	fixedResizeComplete = true;
}

void warmUpPrograms()
{
	assert(fixedReserved);

	/* The driver compiles a per-program code variant the first time a
	 * program is used with a given pipeline state, and takes a code-heap
	 * segment for it out of the same pool the render surfaces came from. Doing that lazily means the first draw
	 * of a rarely used shader -- a transition, a hue rotation -- allocates
	 * mid-game, when the pool is empty, and hard-fails. So use every program
	 * once here, against both kinds of target and every blend mode the
	 * engine sets, one pixel each. */
	std::vector<ShaderBase*> programs;
	shaderSetEnumerate(shState->shaders(), programs);

	/* Every BlendType the engine can set -- the whole of enum BlendType in
	 * etc.h, which is also every case GLBlendMode::apply handles. */
	static const BlendType blendModes[] =
	    { BlendKeepDestAlpha, BlendNormal, BlendAddition, BlendSubstraction };
	static const size_t blendModeN = sizeof(blendModes) / sizeof(blendModes[0]);

	/* The fragment-variant key includes the blend equation, and with
	 * GL_BLEND OFF the driver forces that key to NONE|NONE regardless of the
	 * mode. Warming up with blending ENABLED alone
	 * would miss that, yet the engine draws blend-disabled in seven
	 * places (graphics.cpp ScreenScene::updateEffect and Graphics::
	 * transition, bitmap.cpp x2, window.cpp, windowvx.cpp, tilemap.cpp), so
	 * the first of those compiled a variant mid-game and could need a new
	 * code-heap segment out of an empty pool.
	 *
	 * Blend-off is therefore exactly ONE more state per program, not one per
	 * mode -- which is what keeps this bounded at
	 * programs x (1 + blendModeN) x 2 targets. */
	const size_t stateN = 1 + blendModeN;

	const FBO::ID targets[2] =
	    { fixedSurface[PingPong0].fbo, FBO::ID(0) };

	const double startMs = nowMs();

	Quad &quad = shState->gpQuad();
	quad.setTexPosRect(FloatRect(0, 0, 1, 1), FloatRect(0, 0, 1, 1));

	/* Start from a clean error state, or the first draw inherits the blame
	 * for something earlier in boot and turns it into a fatal error with the
	 * wrong message. Anything drained here is still reported. */
	for (unsigned drained = 0; drained < 16; ++drained)
	{
		const GLenum stale = gl.GetError();
		if (stale == GL_NO_ERROR)
			break;

		char tb[96];
		snprintf(tb, sizeof(tb),
		         "vita-gpu: GL error 0x%x was already pending before the "
		         "shader warm-up", (unsigned)stale);
		Debug() << tb;
	}

	glState.viewport.pushSet(IntRect(0, 0, 1, 1));
	glState.scissorTest.pushSet(false);
	glState.blend.pushSet(true);
	glState.blendMode.pushSet(BlendNormal);

	unsigned draws = 0;

	for (size_t t = 0; t < 2; ++t)
	{
		FBO::bind(targets[t]);

		for (size_t i = 0; i < programs.size(); ++i)
		{
			ShaderBase &shader = *programs[i];

			for (size_t s = 0; s < stateN; ++s)
			{
				/* State 0 is blend-off; the rest walk every BlendType. */
				const bool blendOn = (s != 0);

				glState.blend.set(blendOn);
				glState.blendMode.set(blendModes[blendOn ? s - 1 : 0]);

				shader.bind();
				shader.applyViewportProj();
				shader.setTexSize(Vec2i(1, 1));
				shader.setTranslation(Vec2i());

				quad.draw();
				++draws;

				GLenum err = gl.GetError();
				SimpleShader &simple = shState->shaders().simple;
				if (err == GL_NO_ERROR && &shader == &simple && simple.hasFinalPresentation()) {
					const int last = simple.hasFinalCoordinates() ? 3 : 1;
					for (int mode = 1; mode <= last && err == GL_NO_ERROR; ++mode) {
						SimpleShader::PresentationScope precision(simple,
						    SimpleShader::Presentation(mode, Vec2i(), Vec2i(1,1), Vec2i(1,1)));
						quad.draw();
						++draws;
						err = gl.GetError();
					}
				}
				if (err != GL_NO_ERROR)
				{
					glState.blendMode.pop();
					glState.blend.pop();
					glState.scissorTest.pop();
					glState.viewport.pop();
					FBO::unbind();

					throw Exception(Exception::MKXPError,
					                "software_bitmaps: shader warm-up failed at "
					                "program %u of %u, blend %s mode %d, target "
					                "%s: GL error 0x%x",
					                (unsigned)i, (unsigned)programs.size(),
					                blendOn ? "on" : "off",
					                (int)blendModes[blendOn ? s - 1 : 0],
					                t == 0 ? "render surface" : "window",
					                (unsigned)err);
				}
			}
		}
	}

	/* ScreenScene's viewport tone, colour and flash draws set the blend
	 * state RAW, behind GLState's back (graphics.cpp:632-652), so their
	 * variant keys are not reachable from the table above. Two of the three
	 * are genuinely new; the third (GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA,
	 * GL_ZERO/GL_ONE with GL_FUNC_ADD) is BlendKeepDestAlpha, already done.
	 * Only FlatColorShader is ever drawn with them. */
	static const struct RawBlend
	{
		GLenum equation;
		GLenum srcRGB, dstRGB, srcAlpha, dstAlpha;
	} rawBlends[] = {
	    { GL_FUNC_ADD,              GL_ONE, GL_ONE, GL_ZERO, GL_ONE },
	    { GL_FUNC_REVERSE_SUBTRACT, GL_ONE, GL_ONE, GL_ZERO, GL_ONE },
	};
	static const size_t rawBlendN = sizeof(rawBlends) / sizeof(rawBlends[0]);

	{
		ShaderBase &flat = shState->shaders().flatColor;

		for (size_t t = 0; t < 2; ++t)
		{
			FBO::bind(targets[t]);

			for (size_t r = 0; r < rawBlendN; ++r)
			{
				gl.BlendEquation(rawBlends[r].equation);
				gl.BlendFuncSeparate(rawBlends[r].srcRGB, rawBlends[r].dstRGB,
				                     rawBlends[r].srcAlpha,
				                     rawBlends[r].dstAlpha);

				flat.bind();
				flat.applyViewportProj();
				flat.setTexSize(Vec2i(1, 1));
				flat.setTranslation(Vec2i());

				quad.draw();
				++draws;

				const GLenum err = gl.GetError();
				if (err != GL_NO_ERROR)
				{
					glState.blendMode.refresh();
					glState.blendMode.pop();
					glState.blend.pop();
					glState.scissorTest.pop();
					glState.viewport.pop();
					FBO::unbind();

					throw Exception(Exception::MKXPError,
					                "software_bitmaps: shader warm-up failed at "
					                "raw blend %u of %u, target %s: GL error 0x%x",
					                (unsigned)r, (unsigned)rawBlendN,
					                t == 0 ? "render surface" : "window",
					                (unsigned)err);
				}
			}
		}

		/* Put the driver back in step with GLState's cached blend mode --
		 * exactly what ScreenScene::updateEffect does after its raw draws. */
		glState.blendMode.refresh();
	}

	glState.blendMode.pop();
	glState.blend.pop();
	glState.scissorTest.pop();
	glState.viewport.pop();

	/* The warm-up wrote one pixel into the window; do not leave it there. */
	FBO::unbind();
	glState.clearColor.pushSet(Vec4(0, 0, 0, 1));
	FBO::clear();
	glState.clearColor.pop();

	const double ms = nowMs() - startMs;
	const int whole = ms > 0.0 ? (int)ms : 0;
	const int hundredths = ms > 0.0 ? (int)((ms - (double)whole) * 100.0) : 0;

	char line[224];
	snprintf(line, sizeof(line),
	         "vita-gpu: shader warm-up ok programs=%u blend_modes=%u states=%u "
	         "raw_blends=%u targets=2 draws=%u ms=%d.%02d",
	         (unsigned)programs.size(), (unsigned)blendModeN, (unsigned)stateN,
	         (unsigned)rawBlendN, draws, whole, hundredths);
	syncedLine(line);
}

void headroomCanary(const char *where)
{
	if (headroomMarkerState < 0)
		headroomMarkerState = headroomCanaryMarker() ? 1 : 0;
	if (!headroomMarkerState)
		return;
	if (canaryRunning)
		return;

	struct ProbeScope
	{
		TEX::ScopedBinding textureBinding;
		const FBO::ID previous;
		TEXFBO probes[CANARY_LIMIT];

		ProbeScope() : previous(FBO::boundFramebufferID)
		{
			canaryRunning = true;
		}

		~ProbeScope()
		{
			/* Includes the slot whose initChecked threw. A poisoned FBO is
			 * detached while still bound; never bind a failed name again. */
			for (size_t i = 0; i < CANARY_LIMIT; ++i)
			{
				TEXFBO &probe = probes[i];
				if (!probe.tex.gl && !probe.fbo.gl)
					continue;
				if (probe.fbo.gl && FBO::boundFramebufferID == probe.fbo)
				{
					FBO::setTarget(TEX::ID(0));
					FBO::boundFramebufferID = FBO::ID(0);
				}
				TEXFBO::finiNow(probe);
			}
			FBO::bind(previous);
			canaryRunning = false;
		}
	} scope;

	size_t count = 0;
	for (; count < CANARY_LIMIT; ++count)
	{
		try
		{
			TEXFBO::initChecked(scope.probes[count], 16, 16, "headroom canary");
		}
		catch (const Exception &) { break; }
		catch (const std::bad_alloc &) { break; }
	}

	char line[160];
	snprintf(line, sizeof(line),
	         "vita-gpu: %s headroom surfaces=%u (about %u sync objects)",
	         where ? where : "boot", (unsigned)count, (unsigned)(4 * count));
	syncedLine(line);
}

void seal()
{
	sealed = true;
	logCounts("boot");
}

} /* namespace GPUBudget */

/* The failed-FBO discipline, in one place because getting it wrong is a data
 * abort rather than a bad frame.
 *
 * The driver caches a framebuffer's completeness on the FBO object. Once
 * that cache says "failed", binding the same FBO again poisons the context
 * and the NEXT glBindFramebuffer aborts. Detaching colour attachment 0 while the framebuffer is
 * still bound forces the cached status back to UNKNOWN; deleting the bound
 * framebuffer then makes the driver bind 0 itself, and mkxp-z's shadow copy
 * of the binding has to be corrected by hand to match -- it is what
 * Graphics::isPingPongFramebufferActive() reads, and leaving it pointing at a
 * deleted name tells the engine it is rendering into a surface that is gone. */
static void surfaceFailed(TEXFBO &obj, int width, int height,
                          const char *what, GLenum status, GLenum err)
{
	const FBO::ID deadFBO = obj.fbo;
	const TEX::ID deadTex = obj.tex;

	/* delNow, not del: the driver requires this sequence to happen while the
	 * framebuffer is still bound, so it cannot go through the
	 * deferred queue. A poisoned FBO is not "storage a frame might still be
	 * reading" -- it is a cached completeness status that has to be cleared
	 * before the next bind. */
	FBO::setTarget(TEX::ID(0));
	FBO::delNow(deadFBO);
	TEX::delNow(deadTex);
	FBO::boundFramebufferID = FBO::ID(0);

	/* Zeroing the caller's TEXFBO is not enough when the caller holds a
	 * copy: GPUBudget::fixedSurface[] kept the dead ids and handed them out
	 * again on the next reserved. */
	GPUBudget::fixedSurfaceLost(deadFBO.gl);

	TEXFBO::clear(obj);

	throw Exception(Exception::MKXPError,
	                "software_bitmaps: render surface '%s' (%dx%d) is not "
	                "usable: framebuffer status 0x%x, GL error 0x%x. The "
	                "GPU surface pool is exhausted or the size was "
	                "refused.",
	                what, width, height, (unsigned)status, (unsigned)err);
}

/* Clear to opaque black and ask whether any of it worked. Runs both before
 * SharedState exists (the boot reservation) and after (resize), so the clear
 * colour is set raw and the GLState cache is resynced only when there is one. */
static GLenum clearAndCheck(GLenum *errOut)
{
	gl.ClearColor(0.f, 0.f, 0.f, 1.f);
	FBO::clear();

	if (SharedState::instance)
		glState.clearColor.refresh();

	const GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
	*errOut = gl.GetError();

	return status;
}

void TEXFBO::initChecked(TEXFBO &obj, int width, int height, const char *what)
{
	/* After the seal this is the one creation site that refuses instead
	 * of reporting and continuing, and it refuses with the Ruby-visible
	 * Exception::MKXPError this function already throws on failure -- not an
	 * abort(). Nothing is created, so there is nothing to dispose of. */
	if (GPUBudget::sealRefusesSurface("render surface"))
		throw Exception(Exception::MKXPError,
		                "software_bitmaps: render surface '%s' (%dx%d) was "
		                "requested after boot was sealed. Every render surface "
		                "this backend can use is reserved at boot; the GPU "
		                "pool has nothing left to build another from.",
		                what, width, height);

	TEXFBO::init(obj);
	TEXFBO::allocEmpty(obj, width, height);
	TEXFBO::linkFBO(obj);

	GLenum err = GL_NO_ERROR;
	const GLenum status = clearAndCheck(&err);

	if (status == GL_FRAMEBUFFER_COMPLETE && err == GL_NO_ERROR)
	{
		GPUBudget::setSurfaceStorageValid(obj.fbo.gl, true);
		return;
	}

	surfaceFailed(obj, width, height, what, status, err);
}

static GLenum surfaceGLError()
{
	GLenum first = GL_NO_ERROR, err;
	while ((err = gl.GetError()) != GL_NO_ERROR)
		if (first == GL_NO_ERROR) first = err;
	return first;
}

static bool replaceSurfaceStorage(TEXFBO &obj, int width, int height,
                                  GLenum &status, GLenum &err)
{
	/* Clear cached completeness while still bound, including on rollback.
	 * No wrapper trace may consume allocation errors before this check. */
	GPUBudget::setSurfaceStorageValid(obj.fbo.gl, false);
	FBO::setTarget(TEX::ID(0));
	gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
	              GL_RGBA, GL_UNSIGNED_BYTE, 0);
	err = surfaceGLError();
	FBO::setTarget(obj.tex);
	status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
	const GLenum attachErr = surfaceGLError();
	if (err == GL_NO_ERROR) err = attachErr;
	if (status != GL_FRAMEBUFFER_COMPLETE || err != GL_NO_ERROR)
		return false;
	gl.ClearColor(0.f, 0.f, 0.f, 1.f);
	FBO::clear();
	if (SharedState::instance) glState.clearColor.refresh();
	err = surfaceGLError();
	GPUBudget::setSurfaceStorageValid(obj.fbo.gl, err == GL_NO_ERROR);
	return err == GL_NO_ERROR;
}

void TEXFBO::reallocChecked(TEXFBO &obj, int width, int height, const char *what)
{
	assert(obj.fbo != FBO::ID(0));
	assert(obj.tex != TEX::ID(0));
	if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
		throw Exception(Exception::ArgumentError, "Invalid render surface size %dx%d", width, height);
	if (obj.width == width && obj.height == height &&
	    GPUBudget::surfaceStorageValid(obj.fbo.gl))
		return;

	const int oldWidth = obj.width, oldHeight = obj.height;
	const FBO::ID previous = FBO::boundFramebufferID;
	if (surfaceGLError() != GL_NO_ERROR)
		throw Exception(Exception::MKXPError, "GL error before resizing '%s'", what);
	/* This drain also covers equal-area geometry changes. respecify's byte
	 * ledger alone cannot distinguish 640x416 from 416x640. */
	GPUBudget::drainGPU();
	if (surfaceGLError() != GL_NO_ERROR)
		throw Exception(Exception::MKXPError, "GPU drain failed before resizing '%s'", what);
	GPUBudget::deferDrain("surface resize", false);
	/* Retirement may delete the texture previously bound by the caller. */
	TEX::ScopedBinding textureBinding;
	TEX::bind(obj.tex);
	GPUBudget::respecify(GPUBudget::SlotTexture, GPUBudget::DeferTexture,
	                     (uint64_t)width * height * 4u);
	if (surfaceGLError() != GL_NO_ERROR)
		throw Exception(Exception::MKXPError, "GPU drain failed before resizing '%s'", what);

	FBO::bind(obj.fbo);
	GLenum status = GL_FRAMEBUFFER_COMPLETE, err = GL_NO_ERROR;
	if (!replaceSurfaceStorage(obj, width, height, status, err))
	{
		/* Never delete a reserved target. Invalidate the failed attachment
		 * before the drain can rebind it, then restore its old storage. */
		FBO::setTarget(TEX::ID(0));
		GPUBudget::drainGPU();
		GLenum restoreStatus = GL_FRAMEBUFFER_COMPLETE, restoreErr = GL_NO_ERROR;
		const bool restored = replaceSurfaceStorage(obj, oldWidth, oldHeight,
		                                             restoreStatus, restoreErr);
		if (!restored) FBO::setTarget(TEX::ID(0));
		FBO::bind(!restored && previous == obj.fbo ? FBO::ID(0) : previous);
		GPUBudget::respecify(GPUBudget::SlotTexture, GPUBudget::DeferTexture,
		                     (uint64_t)oldWidth * oldHeight * 4u);
		throw Exception(Exception::MKXPError,
		                "Resize '%s' to %dx%d failed (status 0x%x, GL 0x%x); %s",
		                what, width, height, (unsigned)status, (unsigned)err,
		                restored ? "previous size restored" : "driver refused previous storage");
	}
	obj.width = width;
	obj.height = height;
	FBO::bind(previous);
}
#endif /* MKXPZ_SOFTWARE_BITMAPS */

namespace GLMeta
{

void subRectImageUpload(GLint srcW, GLint srcX, GLint srcY,
                        GLint dstX, GLint dstY, GLsizei dstW, GLsizei dstH,
                        SDL_Surface *src, GLenum format)
{
	if (gl.unpack_subimage)
	{
		gl.PixelStorei(GL_UNPACK_ROW_LENGTH, srcW);
		gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, srcX);
		gl.PixelStorei(GL_UNPACK_SKIP_ROWS, srcY);

		TEX::uploadSubImage(dstX, dstY, dstW, dstH, src->pixels, format);
	}
	else
	{
		SDL_PixelFormat *form = src->format;
		SDL_Surface *tmp = SDL_CreateRGBSurface(0, dstW, dstH, form->BitsPerPixel,
		                                        form->Rmask, form->Gmask, form->Bmask, form->Amask);
		SDL_Rect srcRect = { srcX, srcY, dstW, dstH };

		SDL_BlitSurface(src, &srcRect, tmp, 0);

		TEX::uploadSubImage(dstX, dstY, dstW, dstH, tmp->pixels, format);

		SDL_FreeSurface(tmp);
	}
}

void subRectImageEnd()
{
	if (gl.unpack_subimage)
	{
		gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
		gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	}
}

#define HAVE_NATIVE_VAO gl.GenVertexArrays

static void vaoBindRes(VAO &vao)
{
	VBO::bind(vao.vbo);
	IBO::bind(vao.ibo);

	for (size_t i = 0; i < vao.attrCount; ++i)
	{
		const VertexAttribute &va = vao.attr[i];

		gl.EnableVertexAttribArray(va.index);
		gl.VertexAttribPointer(va.index, va.size, va.type, GL_FALSE, vao.vertSize, va.offset);
	}
}

void vaoInit(VAO &vao, bool keepBound)
{
	if (HAVE_NATIVE_VAO)
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* A native VAO object needs a PDS vertex
		 * program from a fixed GPU pool, and mkxp-z creates one per Quad --
		 * that is, per Sprite, Window and Plane, all through the game. On
		 * Vita the port keeps HAVE_NATIVE_VAO false for exactly this
		 * reason and this branch is dead; the guard is here so a platform
		 * that does take it cannot do so after boot unnoticed. */
		GPUBudget::creationSite("VAO");
#endif
		gl.GenVertexArrays(1, &vao.nativeVAO);
		gl.BindVertexArray(vao.nativeVAO);
#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* Empty tilemaps acquire their first VBO on regeneration. */
		if (vao.vbo.gl)
#endif
			vaoBindRes(vao);
		if (!keepBound)
			gl.BindVertexArray(0);
	}
	else
	{
		if (keepBound)
		{
			VBO::bind(vao.vbo);
			IBO::bind(vao.ibo);
		}
	}
}

void vaoFini(VAO &vao)
{
	if (HAVE_NATIVE_VAO)
		gl.DeleteVertexArrays(1, &vao.nativeVAO);
}

void vaoBind(VAO &vao)
{
	if (HAVE_NATIVE_VAO)
	{
		gl.BindVertexArray(vao.nativeVAO);
#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* Host native VAOs must follow the replacement tile VBO too. */
		vaoBindRes(vao);
#endif
	}
	else
		vaoBindRes(vao);
}

void vaoUnbind(VAO &vao)
{
	if (HAVE_NATIVE_VAO)
	{
		gl.BindVertexArray(0);
	}
	else
	{
		for (size_t i = 0; i < vao.attrCount; ++i)
			gl.DisableVertexAttribArray(vao.attr[i].index);

		VBO::unbind();
		IBO::unbind();
	}
}

#define HAVE_NATIVE_BLIT (gl.BlitFramebuffer && shState->config().smoothScaling <= Bilinear && shState->config().smoothScalingDown <= Bilinear)

int blitScaleIsSpecial(TEXFBO &target, bool targetPreferHires, const IntRect &targetRect, TEXFBO &source, const IntRect &sourceRect)
{
	int targetWidth = targetRect.w;
	int targetHeight = targetRect.h;

	int sourceWidth = sourceRect.w;
	int sourceHeight = sourceRect.h;

	if (targetPreferHires && target.selfHires != nullptr)
	{
		targetWidth *= target.selfHires->width;
		targetWidth /= target.width;

		targetHeight *= target.selfHires->height;
		targetHeight /= target.height;
	}

	if (source.selfHires != nullptr)
	{
		sourceWidth *= source.selfHires->width;
		sourceWidth /= source.width;

		sourceHeight *= source.selfHires->height;
		sourceHeight /= source.height;
	}

	if (targetWidth == sourceWidth && targetHeight == sourceHeight)
	{
		return SameScale;
	}

	if (targetWidth < sourceWidth && targetHeight < sourceHeight)
	{
		return DownScale;
	}

	return UpScale;
}

int smoothScalingMethod(int scaleIsSpecial)
{
	int method;

	switch (scaleIsSpecial)
	{
	case SameScale:
		return NearestNeighbor;
	case DownScale:
		method = shState->config().smoothScalingDown;
		break;
	default:
		method = shState->config().smoothScaling;
		break;
	}

#ifdef MKXPZ_NO_OPTIONAL_SHADERS
	/* Bicubic / Lanczos3 shaders are not built; clamp to Bilinear.
	 * Callers fall through to the SimpleShader default case. */
	if (method >= Bicubic)
		method = Bilinear;
#endif

	return method;
}

static void _blitBegin(FBO::ID fbo, const Vec2i &size, int scaleIsSpecial)
{
	if (HAVE_NATIVE_BLIT)
	{
		FBO::boundFramebufferID = fbo;
		gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo.gl);
	}
	else
	{
		FBO::bind(fbo);
		glState.viewport.pushSet(IntRect(0, 0, size.x, size.y));

		switch (smoothScalingMethod(scaleIsSpecial))
		{
#ifndef MKXPZ_NO_OPTIONAL_SHADERS
		case Bicubic:
		{
			BicubicShader &shader = shState->shaders().bicubic;
			shader.bind();
			shader.applyViewportProj();
			shader.setTranslation(Vec2i());
			shader.setTexSize(Vec2i(size.x, size.y));
			shader.setSharpness(shState->config().bicubicSharpness);
		}

			break;
		case Lanczos3:
		{
			Lanczos3Shader &shader = shState->shaders().lanczos3;
			shader.bind();
			shader.applyViewportProj();
			shader.setTranslation(Vec2i());
			shader.setTexSize(Vec2i(size.x, size.y));
		}

			break;
#ifdef MKXPZ_SSL
		case xBRZ:
		{
			XbrzShader &shader = shState->shaders().xbrz;
			shader.bind();
			shader.applyViewportProj();
			shader.setTranslation(Vec2i());
			shader.setTexSize(Vec2i(size.x, size.y));
			shader.setTargetScale(Vec2(1., 1.));
		}

			break;
#endif
#endif /* !MKXPZ_NO_OPTIONAL_SHADERS */
		default:
		{
			SimpleShader &shader = shState->shaders().simple;
			shader.bind();
			shader.applyViewportProj();
			shader.setTranslation(Vec2i());
			shader.setTexSize(Vec2i(size.x, size.y));
		}
		}
	}
}

int blitDstWidthLores = 1;
int blitDstWidthHires = 1;
int blitDstHeightLores = 1;
int blitDstHeightHires = 1;

int blitSrcWidthLores = 1;
int blitSrcWidthHires = 1;
int blitSrcHeightLores = 1;
int blitSrcHeightHires = 1;

void blitBegin(TEXFBO &target, bool preferHires, int scaleIsSpecial)
{
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
	{
		char tb[160];
		snprintf(tb, sizeof(tb),
		         "trace: GLMeta::blitBegin target=%dx%d fbo=%u preferHires=%d",
		         target.width, target.height, target.fbo.gl, (int)preferHires);
		vita_glue_trace(tb);
	}
#endif
	blitDstWidthLores = target.width;
	blitDstHeightLores = target.height;

	if (preferHires && target.selfHires != nullptr) {
		blitDstWidthHires = target.selfHires->width;
		blitDstHeightHires = target.selfHires->height;
		_blitBegin(target.selfHires->fbo, Vec2i(target.selfHires->width, target.selfHires->height), scaleIsSpecial);
	}
	else {
		blitDstWidthHires = blitDstWidthLores;
		blitDstHeightHires = blitDstHeightLores;
		_blitBegin(target.fbo, Vec2i(target.width, target.height), scaleIsSpecial);
	}
}

void blitBeginScreen(const Vec2i &size, int scaleIsSpecial)
{
	blitDstWidthLores = 1;
	blitDstWidthHires = 1;
	blitDstHeightLores = 1;
	blitDstHeightHires = 1;

	_blitBegin(FBO::ID(0), size, scaleIsSpecial);
}

void blitSource(TEXFBO &source, int scaleIsSpecial)
{
	blitSrcWidthLores = source.width;
	blitSrcHeightLores = source.height;
	if (source.selfHires != nullptr) {
		blitSrcWidthHires = source.selfHires->width;
		blitSrcHeightHires = source.selfHires->height;
	}
	else {
		blitSrcWidthHires = blitSrcWidthLores;
		blitSrcHeightHires = blitSrcHeightLores;
	}

	if (HAVE_NATIVE_BLIT)
	{
		gl.BindFramebuffer(GL_READ_FRAMEBUFFER, source.fbo.gl);
	}
	else
	{
		switch (smoothScalingMethod(scaleIsSpecial))
		{
#ifndef MKXPZ_NO_OPTIONAL_SHADERS
		case Bicubic:
		{
			BicubicShader &shader = shState->shaders().bicubic;
			shader.bind();
			shader.setTexSize(Vec2i(blitSrcWidthHires, blitSrcHeightHires));
		}

			break;
		case Lanczos3:
		{
			Lanczos3Shader &shader = shState->shaders().lanczos3;
			shader.bind();
			shader.setTexSize(Vec2i(blitSrcWidthHires, blitSrcHeightHires));
		}

			break;
#ifdef MKXPZ_SSL
		case xBRZ:
		{
			XbrzShader &shader = shState->shaders().xbrz;
			shader.bind();
			shader.setTexSize(Vec2i(blitSrcWidthHires, blitSrcHeightHires));
		}

			break;
#endif
#endif /* !MKXPZ_NO_OPTIONAL_SHADERS */
		default:
		{
			SimpleShader &shader = shState->shaders().simple;
			shader.bind();
			shader.setTexSize(Vec2i(blitSrcWidthHires, blitSrcHeightHires));
		}
		}
		if (source.selfHires != nullptr) {
			TEX::bind(source.selfHires->tex);
		}
		else {
			TEX::bind(source.tex);
		}
	}
}

void blitRectangle(const IntRect &src, const Vec2i &dstPos)
{
	blitRectangle(src, IntRect(dstPos.x, dstPos.y, src.w, src.h), false);
}

void blitRectangle(const IntRect &src, const IntRect &dst, bool smooth)
{
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
	{
		char tb[192];
		snprintf(tb, sizeof(tb),
		         "trace: GLMeta::blitRectangle src=%d,%d %dx%d dst=%d,%d %dx%d nativeBlit=%d",
		         src.x, src.y, src.w, src.h,
		         dst.x, dst.y, dst.w, dst.h,
		         (int)(HAVE_NATIVE_BLIT != 0));
		vita_glue_trace(tb);
	}
#endif
	// Handle high-res dest
	int scaledDstX = dst.x * blitDstWidthHires / blitDstWidthLores;
	int scaledDstY = dst.y * blitDstHeightHires / blitDstHeightLores;
	int scaledDstWidth = dst.w * blitDstWidthHires / blitDstWidthLores;
	int scaledDstHeight = dst.h * blitDstHeightHires / blitDstHeightLores;
	IntRect dstScaled(scaledDstX, scaledDstY, scaledDstWidth, scaledDstHeight);

	// Handle high-res source
	int scaledSrcX = src.x * blitSrcWidthHires / blitSrcWidthLores;
	int scaledSrcY = src.y * blitSrcHeightHires / blitSrcHeightLores;
	int scaledSrcWidth = src.w * blitSrcWidthHires / blitSrcWidthLores;
	int scaledSrcHeight = src.h * blitSrcHeightHires / blitSrcHeightLores;
	IntRect srcScaled(scaledSrcX, scaledSrcY, scaledSrcWidth, scaledSrcHeight);
	SimpleShader &simple = shState->shaders().simple;
	const Vec2i texture(blitSrcWidthHires, blitSrcHeightHires);
	const int presentation = simple.finalPresentationFor(FBO::boundFramebufferID.gl == 0,
	    smooth, srcScaled, dstScaled, glState.viewport.get(), texture, HAVE_NATIVE_BLIT != 0);

	if (HAVE_NATIVE_BLIT)
	{
		gl.BlitFramebuffer(srcScaled.x, srcScaled.y, srcScaled.x+srcScaled.w, srcScaled.y+srcScaled.h,
		                   dstScaled.x, dstScaled.y, dstScaled.x+dstScaled.w, dstScaled.y+dstScaled.h,
		                   GL_COLOR_BUFFER_BIT, smooth ? GL_LINEAR : GL_NEAREST);
	}
	else
	{
#if defined(MKXPZ_SSL) && !defined(MKXPZ_NO_OPTIONAL_SHADERS)
		if (shState->config().smoothScaling == xBRZ)
		{
			XbrzShader &shader = shState->shaders().xbrz;
			shader.setTargetScale(Vec2((float)(shState->config().xbrzScalingFactor), (float)(shState->config().xbrzScalingFactor)));
		}
#endif
		if (smooth)
			TEX::setSmooth(true);

		GLProperty<bool>::Guard blend(glState.blend);
		glState.blend.set(false);
		Quad &quad = shState->gpQuad();
		quad.setTexPosRect(srcScaled, dstScaled);
		{
			SimpleShader::PresentationScope precision(simple, presentation ? SimpleShader::Presentation(presentation,
			    Vec2i(dstScaled.x,dstScaled.y), Vec2i(dstScaled.w,dstScaled.h), texture) : SimpleShader::Presentation());
			quad.draw();
		}
		blend.restore();

		if (smooth)
			TEX::setSmooth(false);
	}
}

void blitEnd()
{
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
	vita_glue_trace("trace: GLMeta::blitEnd");
#endif
	blitDstWidthLores = 1;
	blitDstWidthHires = 1;
	blitDstHeightLores = 1;
	blitDstHeightHires = 1;

	blitSrcWidthLores = 1;
	blitSrcWidthHires = 1;
	blitSrcHeightLores = 1;
	blitSrcHeightHires = 1;

	if (!HAVE_NATIVE_BLIT) {
		glState.viewport.pop();
	}
}

}
