/*
** gl-util.h
**
** This file is part of mkxp.
**
** Copyright (C) 2013 - 2021 Amaryllis Kulla <ancurio@mapleshrine.eu>
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

#ifndef GLUTIL_H
#define GLUTIL_H

#include "gl-fun.h"
#include "etc-internal.h"
#include "sharedstate.h"
#include "config.h"
#ifdef MKXPZ_SOFTWARE_BITMAPS
#include "exception.h"
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "vita_glue.h"
#include <stdio.h>
#endif

#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
static inline void vitaGlTrace(const char *msg)
{
	vita_glue_trace(msg);
}

static inline void vitaGlTrace2(const char *tag, int a, int b)
{
	char tb[160];
	snprintf(tb, sizeof(tb), "%s %d %d", tag, a, b);
	vita_glue_trace(tb);
}

static inline void vitaGlTraceErr(const char *tag)
{
	if (!gl.GetError)
		return;
	GLenum err = gl.GetError();
	if (err == GL_NO_ERROR)
		return;
	char tb[160];
	snprintf(tb, sizeof(tb), "trace: GL_ERROR 0x%x after %s", (unsigned)err, tag);
	vita_glue_trace(tb);
}

static inline void vitaGlCheckFBO(const char *tag)
{
	/* CheckFramebufferStatus is not in GL_FBO_FUN; use the linked
	 * GLES2 stub (gl-probe proved linked symbols work after MakeCurrent). */
	GLenum st = ::glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (st == GL_FRAMEBUFFER_COMPLETE)
		return;
	char tb[160];
	snprintf(tb, sizeof(tb), "trace: FBO_INCOMPLETE 0x%x after %s", (unsigned)st, tag);
	vita_glue_trace(tb);
}
#else
#define vitaGlTrace(msg) do { } while (0)
#define vitaGlTrace2(tag, a, b) do { } while (0)
#define vitaGlTraceErr(tag) do { } while (0)
#define vitaGlCheckFBO(tag) do { } while (0)
#endif

#ifdef MKXPZ_SOFTWARE_BITMAPS
#include <stdint.h>

/* Bounded GPU kernel-object budget.
 *
 * The GPU driver serves render surfaces, textures and VBOs out of shared
 * fixed pools, first come first served. Once a pool is empty, textures and
 * VBOs keep working -- pixel-correct, uploads included -- but a new render
 * surface, a new shader program and a new VAO can fail HARD.
 *
 * So boot order is the whole game. The engine's render surfaces are a fixed,
 * known set, and they are reserved and verified before anything else asks the
 * driver for an object; after boot nothing may create a surface, a program or
 * a VAO at all. This namespace is the accounting and the boot sequence. */
namespace GPUBudget
{
	/* Live GL object counts, maintained by the TEX / GenericBO / FBO gen+del
	 * wrappers below -- every creation and destruction in the engine goes
	 * through them, so these are exact, not estimates. Buffer objects are
	 * counted together (vbos) because the pools do not distinguish
	 * GL_ARRAY_BUFFER from GL_ELEMENT_ARRAY_BUFFER. */
	extern unsigned liveTextures;
	extern unsigned liveVBOs;
	extern unsigned liveSurfaces;

	/* Bytes of client-memory Bitmap pixels (the cpuSurface and
	 * animation frames). Not a GPU object; it is the number that says where
	 * the memory went instead. */
	extern uint64_t liveCpuBitmapBytes;

	/* Set by seal() at the end of boot. After this the engine must
	 * never create another render surface, shader program or VAO. */
	extern bool sealed;

	/* Creation-site guard for the three object classes that hard-fail on an
	 * empty pool. After seal() this logs one synced line per distinct site
	 * and lets the operation proceed, because the operation is legal GL and
	 * the shipping player has asserts LIVE -- aborting would kill the game
	 * for the player. Opt in to the old abort with
	 * ux0:/data/mkxp-z/gpu-seal-abort.enabled. A late render surface is the
	 * exception; see sealRefusesSurface(). */
	void creationSite(const char *what);

	/* CPU pixel accounting; delta may be negative. */
	void cpuPixels(int64_t deltaBytes);

	/* ux0:/data/mkxp-z/gpu-telemetry.enabled, read once at boot. */
	bool telemetryEnabled();

	/* One line: "vita-gpu: textures=.. vbos=.. surfaces=.. cpu_bitmap_bytes=..
	 * deferred_pending=.. scene=..". Emitted at the end of boot and once per
	 * scene change; a no-op unless telemetryEnabled(). */
	void logCounts(const char *scene);

	/* logCounts() with an auto-incrementing scene serial. Called from
	 * Graphics::freeze(), which is what RGSS runs at every scene change. */
	void logSceneChange();

	/* ==================================================================
	 * Frame instrumentation, seal
	 * semantics, and deferred GL object deletion.
	 *
	 * An early crash ended with a submitted render that never
	 * completed and a flip thread waiting on it for ever. None of the
	 * counters above can see that: they count what the ENGINE owns, not
	 * what the GPU is still reading. Everything below is either the
	 * instrumentation that catches it (the frame and swap watchdogs) or the
	 * lifetime rule that makes "the GPU is still reading it" impossible
	 * (deferred release).
	 * ================================================================== */

	/* One log line through the crash-safe sink when the log-sync marker is
	 * on, so a process the system stops still has its last line on disk.
	 * Off Vita this is a plain Debug() line. */
	void syncedLine(const char *line);

	/* Drain glGetError and report anything that was pending, tagged
	 * with the call site `at` and the real-swap counter. Rate-limited: the
	 * first 16, then every 256th, carrying the suppressed count. Reads no
	 * GL state and costs nothing unless telemetryEnabled(). */
	void pollGLError(const char *at);

	/* One line for a GL error that a checked upload consumed or found
	 * pending; unlike pollGLError() it does not need the telemetry marker,
	 * because it is the only record of why an upload was refused.
	 * Rate-limited like pollGLError(). */
	void noteGLError(const char *at, GLenum err);

	/* "vita-gpu: BEGIN <what> frame=<n>" / "... END ..." around the
	 * risky GPU points. Telemetry-marker only. */
	void breadcrumb(const char *phase, const char *what);

	/* One REAL SDL_GL_SwapWindow happened. Advances the frame counter the
	 * lines above report, and retires deferred deletions. */
	void frameAdvance();
	uint64_t frameCounter();

	/* The swap-completion watchdog. The flip chain is strictly
	 * one-in-flight and the driver blocks handing over a new flip until the
	 * previous one is displayed, so the wall-clock time inside
	 * SDL_GL_SwapWindow measures whether the PREVIOUS frame's render
	 * completed -- the only GPU-stall canary reachable from userland
	 * Bracket the swap with these. */
	void swapBegin();
	void swapEnd();

	/* Opt-in through ux0:/data/mkxp-z/gpu-seal-abort.enabled. When it is
	 * absent -- the shipping case -- a seal violation is a log line, not an
	 * abort(): the player is built WITHOUT -DNDEBUG, so the assert this
	 * replaces killed the game for the person playing it. */
	bool sealAbortEnabled();

	/* A late RENDER SURFACE is the one violation that does not continue:
	 * it is the object class that fails hard when a pool is empty. Returns true when the
	 * caller must refuse; logs the violation as a side effect. */
	bool sealRefusesSurface(const char *what);

	/* ---- Deferred release ------------------------------------------
	 * Three windows in which the driver hands GPU-visible memory back while
	 * the GPU may still be using it:
	 *
	 *   - an in-flight texture upload may be tracked by nothing but the
	 *     texture's own synchronisation object, and an object created after a
	 *     pool ran dry has none, so deleting or re-specifying it can silently
	 *     skip the wait that protects the transfer;
	 *   - glDeleteBuffers can free a VBO's device memory even when the
	 *     driver's wait for its last use timed out;
	 *   - re-specifying a live object at a new size ghosts it and allocates
	 *     fresh memory, and for an FBO attachment destroys and re-creates
	 *     the render surface.
	 *
	 * The RENDER side is safe without a synchronisation object: the driver
	 * defers the deletion of a texture an incomplete render still needs. It is
	 * the transfer side that has no fallback.
	 *
	 * Holding the release for two completed flips closes all three. */
	enum DeferKind
	{
		DeferTexture = 0,
		DeferBuffer,          /* VBO and IBO alike: the pool does not care */
		DeferFramebuffer,

		DeferKindCount
	};

	/* The two policy knobs, in one place and one line each.
	 *
	 *   deferSwaps[kind]          how many REAL swaps a release is held for
	 *                             (0 restores stock immediate deletion).
	 *                             Two is the PROVEN value, not a guess: the
	 *                             window has two flip buffers and at most
	 *                             one flip in flight, so after swap k
	 *                             returns only swap k-1's render is known
	 *                             complete.
	 *   syncBeforeRespecify[kind] whether a size-changing re-specification
	 *                             of a LIVE object drains the GPU first
	 */
	extern unsigned deferSwaps[DeferKindCount];
	extern bool syncBeforeRespecify[DeferKindCount];

	void deferDelete(DeferKind kind, GLuint id, uint64_t bytes);

	/* Release everything the queue holds. `waitForGPU` drains before
	 * releasing anything, which is what makes a drain safe at a point where
	 * no further swaps are coming (freeze, shutdown, or a loading screen
	 * that has filled the queue). The drain is glFinish, a real sceGxmFinish
	 * fence on vitaGL. */
	void deferDrain(const char *why, bool waitForGPU);

	unsigned deferPending();
	uint64_t deferPendingBytes();

	/* ---- bounded texture-object recycling ------------------
	 * Device measurements of both
	 * upload paths: re-specifying same-size storage
	 * IN PLACE runs at the whole-level rate (166-199 MB/s, no wait, even
	 * when a submitted frame has sampled the object), while the FIRST
	 * specification of a fresh texture object is the slow case (53-55
	 * MB/s) -- and every scene rebuild creates Bitmaps that pay it. A
	 * dropped Bitmap's sampling texture is therefore parked here instead
	 * of retired, and take() hands a same-size name to the next Bitmap so
	 * its whole-level upload re-specifies in place.
	 *
	 * Pooling HOLDS objects: nothing is released, so the two-swap rule is
	 * not in play until retirement. Overflow and drains retire through
	 * deferDelete above, and a drain rides the shutdown or pressure drain
	 * that already waits for the GPU. No freeze drain: while swaps are
	 * stopped nothing in the pool awaits retirement, and the names the
	 * previous scene left are exactly the ones the next scene re-samples
	 * (the sceneprof gate this pool exists for). */
	GLuint texRecycleTake(int width, int height);
	void texRecycleOffer(GLuint id, int width, int height);
	void texRecycleDrain(const char *why);

	/* Which object is bound where. A re-specification acts on the BOUND
	 * object rather than on an id the caller hands over, so recognising one
	 * needs the binding. */
	enum Slot
	{
		SlotTexture = 0,
		SlotArrayBuffer,
		SlotElementBuffer,

		SlotCount
	};

	void noteBind(Slot slot, GLuint id);

	/* Call immediately BEFORE a call that re-specifies the storage of the
	 * object bound in `slot`. When the size actually changes, the driver is
	 * about to free storage a submitted frame may still be reading. */
	void respecify(Slot slot, DeferKind kind, uint64_t bytes);

	/* Whole-level upload timing; only uploads above 1 MiB are logged. */
	void uploadBegin(uint64_t bytes);
	void uploadEnd(int width, int height, uint64_t bytes);

	/* ---- The fixed-surface registry is the single owner ----------------
	 * reserved() hands out a reference into it, but PingPong and
	 * GraphicsPrivate used to copy the TEXFBO -- so a re-specification
	 * updated one side and the other kept stale dimensions, and their
	 * destructors deleted surfaces the registry still believed in. */

	/* True when `fbo` is one of the surfaces reserved at boot. */
	bool isReservedFBO(GLuint fbo);

	/* A reserved surface failed its completeness check and has been taken
	 * apart by the failed-FBO discipline: drop it from the registry too, so
	 * no later reserved() hands out a dead id. */
	void fixedSurfaceLost(GLuint fbo);
}
#endif /* MKXPZ_SOFTWARE_BITMAPS */

/* Struct wrapping GLuint for some light type safety */
#define DEF_GL_ID \
struct ID \
{ \
	GLuint gl; \
	explicit ID(GLuint gl = 0)  \
	    : gl(gl)  \
	{}  \
	ID &operator=(const ID &o)  \
	{  \
		gl = o.gl;  \
		return *this; \
	}  \
	bool operator==(const ID &o) const  \
	{  \
		return gl == o.gl;  \
	}  \
	bool operator!=(const ID &o) const \
	{ \
		return !(*this == o); \
	} \
};

/* 2D Texture */
namespace TEX
{
	DEF_GL_ID

	inline ID gen()
	{
		ID id(0);
		gl.GenTextures(1, &id.gl);
#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* Name 0 is "no object": counting it would desync del(), which
		 * ignores it (TEX::del(ID(0)) is a legal no-op all over mkxp-z). */
		if (id.gl)
			++GPUBudget::liveTextures;
#endif

		return id;
	}

	static inline void del(ID id)
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* The name is retired now; the storage goes back to the driver
		 * only once the frames that could still be reading it have been
		 * flipped (GPUBudget::deferSwaps[DeferTexture]). */
		if (id.gl && GPUBudget::liveTextures)
			--GPUBudget::liveTextures;
		GPUBudget::deferDelete(GPUBudget::DeferTexture, id.gl, 0);
#else
		gl.DeleteTextures(1, &id.gl);
#endif
	}

#ifdef MKXPZ_SOFTWARE_BITMAPS
	/* The failed-FBO discipline has to delete the
	 * attachment there and then, while the framebuffer is still bound, so it
	 * cannot go through the queue. The same is true of the headroom canary's
	 * probes, which exist to be counted and dropped. */
	static inline void delNow(ID id)
	{
		gl.DeleteTextures(1, &id.gl);
		if (id.gl && GPUBudget::liveTextures)
			--GPUBudget::liveTextures;
	}
#endif

	static inline void bind(ID id)
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		GPUBudget::noteBind(GPUBudget::SlotTexture, id.gl);
#endif
		gl.BindTexture(GL_TEXTURE_2D, id.gl);
	}

	static inline void unbind()
	{
		bind(ID(0));
	}

#ifdef MKXPZ_SOFTWARE_BITMAPS
	struct UploadError : Exception
	{
		UploadError() : Exception(Exception::MKXPError, "Texture upload failed; CPU pixels retained")
		{
			/* A failed name allocation never reaches uploadImage. */
			for (GLenum err; (err = gl.GetError()) != GL_NO_ERROR;)
				GPUBudget::noteGLError("upload-failed", err);
		}
	};

	/* vitaGL keeps one sticky error flag that any earlier call can set, and
	 * the checked upload below refuses on any pending error. Drain (and log)
	 * what is already pending before this upload's own name, binding and
	 * parameter calls, so only errors those calls raise can refuse it. */
	static inline void drainStaleErrors(const char *at)
	{
		for (unsigned i = 0; i < 32; ++i)
		{
			const GLenum err = gl.GetError();
			if (err == GL_NO_ERROR)
				break;
			GPUBudget::noteGLError(at, err);
		}
	}

	/* Restore both GL and the storage-lifetime binding shadow on unwind. */
	class ScopedBinding
	{
		GLint previous;
	public:
		ScopedBinding() { gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &previous); }
		explicit ScopedBinding(GLint previous) : previous(previous) {}
		~ScopedBinding() { bind(ID((GLuint)previous)); }
		ScopedBinding(const ScopedBinding &) = delete;
		ScopedBinding &operator=(const ScopedBinding &) = delete;
	};
#endif

	static inline void noteUploadError(const char *at, GLenum err)
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		GPUBudget::noteGLError(at, err);
#else
		(void)at; (void)err;
#endif
	}

	static inline bool uploadImageImpl(GLsizei width, GLsizei height, const void *data, GLenum format, bool checked)
	{
		/* Include failures from name creation/binding/parameters. Check before
		 * telemetry can consume an error, and never accept a partial upload. */
		bool success = true;
		if (checked)
			for (GLenum err; (err = gl.GetError()) != GL_NO_ERROR;)
			{
				success = false;
				noteUploadError("upload-setup", err);
			}
		if (!success)
			return false;
		vitaGlTrace2("trace: TEX::uploadImage w/h", (int)width, (int)height);
#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* glTexImage2D on a level that already has storage releases
		 * that storage; when the size changes the driver cannot reuse it in
		 * place. respecify() is what makes that safe, and it is a no-op on
		 * the common case of a same-size whole-level refresh, which is the
		 * per-frame upload traffic this backend is built on. */
		const uint64_t bytes = (uint64_t)width * (uint64_t)height * 4u;
		GPUBudget::respecify(GPUBudget::SlotTexture, GPUBudget::DeferTexture,
		                     bytes);
		GPUBudget::uploadBegin(bytes);
#endif
		gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, format, GL_UNSIGNED_BYTE, data);
		if (checked)
			for (GLenum err; (err = gl.GetError()) != GL_NO_ERROR;)
			{
				success = false;
				noteUploadError("upload", err);
			}
#ifdef MKXPZ_SOFTWARE_BITMAPS
		GPUBudget::uploadEnd(width, height, bytes);
#endif
		if (!checked)
			vitaGlTraceErr("TEX::uploadImage");
		return success;
	}

	/* Legacy callers inspect GL errors themselves (including fatal overlays). */
	static inline void uploadImage(GLsizei width, GLsizei height, const void *data, GLenum format)
	{
		uploadImageImpl(width, height, data, format, false);
	}

	static inline bool uploadImageChecked(GLsizei width, GLsizei height, const void *data, GLenum format)
	{
		return uploadImageImpl(width, height, data, format, true);
	}

	static inline void uploadSubImage(GLint x, GLint y, GLsizei width, GLsizei height, const void *data, GLenum format)
	{
		gl.TexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, format, GL_UNSIGNED_BYTE, data);
	}

	static inline void allocEmpty(GLsizei width, GLsizei height)
	{
		vitaGlTrace2("trace: TEX::allocEmpty w/h", (int)width, (int)height);
#ifdef MKXPZ_SOFTWARE_BITMAPS
		GPUBudget::respecify(GPUBudget::SlotTexture, GPUBudget::DeferTexture,
		                     (uint64_t)width * (uint64_t)height * 4u);
#endif
		gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
		vitaGlTraceErr("TEX::allocEmpty");
	}

	static inline void setRepeat(bool mode)
	{
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, mode ? GL_REPEAT : GL_CLAMP_TO_EDGE);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, mode ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	}

	static inline void setSmooth(bool mode)
	{
		if (mode && shState->config().smoothScalingMipmaps) {
			gl.GenerateMipmap(GL_TEXTURE_2D);
			gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		} else {
			gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mode ? GL_LINEAR : GL_NEAREST);
		}

		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mode ? GL_LINEAR : GL_NEAREST);
	}
}

/* Framebuffer Object */
namespace FBO
{
	DEF_GL_ID

	extern ID boundFramebufferID;

	inline ID gen()
	{
		ID id;
		gl.GenFramebuffers(1, &id.gl);
#ifdef MKXPZ_SOFTWARE_BITMAPS
		if (id.gl)
			++GPUBudget::liveSurfaces;
#endif

		return id;
	}

	static inline void del(ID id)
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		if (id.gl && GPUBudget::liveSurfaces)
			--GPUBudget::liveSurfaces;
		GPUBudget::deferDelete(GPUBudget::DeferFramebuffer, id.gl, 0);
#else
		gl.DeleteFramebuffers(1, &id.gl);
#endif
	}

#ifdef MKXPZ_SOFTWARE_BITMAPS
	/* Immediate, for the failed-FBO discipline: the driver requires the dead
	 * framebuffer to be deleted while it is still bound. */
	static inline void delNow(ID id)
	{
		gl.DeleteFramebuffers(1, &id.gl);
		if (id.gl && GPUBudget::liveSurfaces)
			--GPUBudget::liveSurfaces;
	}
#endif

	static inline void bind(ID id)
	{
		boundFramebufferID = id;
		gl.BindFramebuffer(GL_FRAMEBUFFER, id.gl);
	}

	static inline void unbind()
	{
		bind(ID(0));
	}

	static inline void setTarget(TEX::ID target, unsigned colorAttach = 0)
	{
		vitaGlTrace2("trace: FBO::setTarget tex/attach", (int)target.gl, (int)colorAttach);
		gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + colorAttach, GL_TEXTURE_2D, target.gl, 0);
	}

	static inline void clear()
	{
		vitaGlTrace("trace: FBO::clear");
		gl.Clear(GL_COLOR_BUFFER_BIT);
		vitaGlTraceErr("FBO::clear");
	}
}

template<GLenum target>
struct GenericBO
{
	DEF_GL_ID

	static inline ID gen()
	{
		ID id;
		gl.GenBuffers(1, &id.gl);
#ifdef MKXPZ_SOFTWARE_BITMAPS
		if (id.gl)
			++GPUBudget::liveVBOs;
#endif

		return id;
	}

	static inline void del(ID id)
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		if (id.gl && GPUBudget::liveVBOs)
			--GPUBudget::liveVBOs;
		GPUBudget::deferDelete(GPUBudget::DeferBuffer, id.gl, 0);
#else
		gl.DeleteBuffers(1, &id.gl);
#endif
	}

	static inline void bind(ID id)
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* target is a template parameter, so this folds away. */
		GPUBudget::noteBind(target == GL_ARRAY_BUFFER
		                        ? GPUBudget::SlotArrayBuffer
		                        : GPUBudget::SlotElementBuffer,
		                    id.gl);
#endif
		gl.BindBuffer(target, id.gl);
	}

	static inline void unbind()
	{
		bind(ID(0));
	}

	static inline void uploadData(GLsizeiptr size, const GLvoid *data, GLenum usage = GL_STATIC_DRAW)
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* glBufferData re-specifies the whole store; QuadArray::commit and
		 * GlobalIBO::ensureSize both reach it when their buffer GROWS, which
		 * is exactly a live object whose storage is released underneath a
		 * frame that may still be reading it. */
		GPUBudget::respecify(target == GL_ARRAY_BUFFER
		                         ? GPUBudget::SlotArrayBuffer
		                         : GPUBudget::SlotElementBuffer,
		                     GPUBudget::DeferBuffer, (uint64_t)size);
#endif
		gl.BufferData(target, size, data, usage);
	}

	static inline void uploadSubData(GLintptr offset, GLsizeiptr size, const GLvoid *data)
	{
		gl.BufferSubData(target, offset, size, data);
	}

	static inline void allocEmpty(GLsizeiptr size, GLenum usage = GL_STATIC_DRAW)
	{
		uploadData(size, 0, usage);
	}
};

/* Vertex Buffer Object */
typedef struct GenericBO<GL_ARRAY_BUFFER> VBO;

/* Index Buffer Object */
typedef struct GenericBO<GL_ELEMENT_ARRAY_BUFFER> IBO;

#undef DEF_GL_ID

/* Convenience struct wrapping a framebuffer
 * and a 2D texture as its target */
struct TEXFBO
{
	TEX::ID tex;
	FBO::ID fbo;
	int width, height;

	TEXFBO *selfHires;

	TEXFBO()
	    : tex(0), fbo(0), width(0), height(0), selfHires(nullptr)
	{}

	bool operator==(const TEXFBO &other) const
	{
		return (tex == other.tex) && (fbo == other.fbo);
	}

	static inline void trace(const char *stage, const TEXFBO &obj, int w = -1, int h = -1)
	{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		vita_glue_texture_trace(stage, obj.tex.gl, obj.fbo.gl,
		                        w < 0 ? obj.width : w, h < 0 ? obj.height : h);
#else
		(void)stage; (void)obj; (void)w; (void)h;
#endif
	}

	static inline void init(TEXFBO &obj)
	{
		trace("init GenTextures BEGIN", obj);
		obj.tex = TEX::gen();
		trace("init GenTextures END / GenFramebuffers BEGIN", obj);
		obj.fbo = FBO::gen();
		trace("init GenFramebuffers END / BindTexture BEGIN", obj);
		TEX::bind(obj.tex);
		trace("init BindTexture END / TexParameter BEGIN", obj);
		TEX::setRepeat(false);
		TEX::setSmooth(false);
		trace("init TexParameter END", obj);
	}

	static inline void allocEmpty(TEXFBO &obj, int width, int height)
	{
		trace("alloc BindTexture BEGIN", obj, width, height);
		TEX::bind(obj.tex);
		trace("alloc BindTexture END / TexImage2D BEGIN", obj, width, height);
		TEX::allocEmpty(width, height);
		trace("alloc TexImage2D END", obj, width, height);
		obj.width = width;
		obj.height = height;
	}

	static inline void linkFBO(TEXFBO &obj)
	{
		vitaGlTrace2("trace: TEXFBO::linkFBO tex/fbo", (int)obj.tex.gl, (int)obj.fbo.gl);
		trace("link BindFramebuffer BEGIN", obj);
		FBO::bind(obj.fbo);
		trace("link BindFramebuffer END / FramebufferTexture2D BEGIN", obj);
		FBO::setTarget(obj.tex);
		trace("link FramebufferTexture2D END", obj);
		vitaGlCheckFBO("TEXFBO::linkFBO");
	}

	static inline void fini(TEXFBO &obj)
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* The reserved surfaces belong to GPUBudget's registry for the
		 * life of the process, and every holder of one holds a COPY.
		 * ~PingPong and ~GraphicsPrivate used to fini() those copies, which
		 * destroys surfaces the registry still believes in -- and on a
		 * reset, re-creates them out of a pool that has nothing left. */
		if (GPUBudget::isReservedFBO(obj.fbo.gl))
		{
			trace("fini SKIPPED (reserved)", obj);
			return;
		}
#endif
		trace("fini DeleteFramebuffers BEGIN", obj);
		FBO::del(obj.fbo);
		trace("fini DeleteFramebuffers END / DeleteTextures BEGIN", obj);
		TEX::del(obj.tex);
		trace("fini DeleteTextures END", obj);
	}

#ifdef MKXPZ_SOFTWARE_BITMAPS
	/* Release both names right now, bypassing the deferred queue. Only for
	 * objects that cannot be in flight: the headroom canary's probes. */
	static inline void finiNow(TEXFBO &obj)
	{
		FBO::delNow(obj.fbo);
		TEX::delNow(obj.tex);
	}
#endif

	static inline void clear(TEXFBO &obj)
	{
		obj.tex = TEX::ID(0);
		obj.fbo = FBO::ID(0);
		obj.width = obj.height = 0;
	}

#ifdef MKXPZ_SOFTWARE_BITMAPS
	/* The ONLY way a render surface is created under this backend. init + allocEmpty + linkFBO + clear, and then it asks
	 * glCheckFramebufferStatus whether any of that actually worked -- stock
	 * mkxp-z never checks, which is why an exhausted sync pool shows up as a
	 * data abort three calls later instead of an error here.
	 *
	 * On failure it runs the driver's failed-FBO discipline (see
	 * gl-meta.cpp) and throws Exception::MKXPError naming `what`. The FBO id
	 * is deleted on that path and must never be bound again; obj comes back
	 * zeroed, so a caller that catches cannot accidentally use it. */
	static void initChecked(TEXFBO &obj, int width, int height, const char *what);

	/* Re-specify an EXISTING checked surface at a new size: no GenTextures,
	 * no GenFramebuffers, no deletes -- the surface object and its firmware
	 * sync objects survive, which is the whole point. Same completeness check
	 * and same failure discipline as initChecked. */
	static void reallocChecked(TEXFBO &obj, int width, int height, const char *what);
#endif
};

#ifdef MKXPZ_SOFTWARE_BITMAPS
struct Config;

namespace GPUBudget
{
	/* The engine's fixed render surfaces. There are exactly this many, for
	 * the life of the process. */
	enum Surface
	{
		PingPong0 = 0,
		PingPong1,
		FrozenScene,
		IntegerScale,   /* only when the config asks for integer scaling */

		SurfaceCount
	};

	/* The textures reserved at boot ALONGSIDE those surfaces. There is
	 * exactly one, and this enum is where the budget for it lives: the
	 * persistent on-screen overlay, which the settings
	 * menu, the FPS line and the frame-profile HUD all share instead of each
	 * taking GPU objects of its own.
	 *
	 * It is NOT a render surface -- nothing is ever drawn INTO it, it has no
	 * FBO and it is never an attachment -- so it does not belong in enum
	 * Surface above; it is a sample-only texture, reserved early for the same
	 * reason the surfaces are. Textures come out of the same pools, and one
	 * taken after GPUBudget::seal() may find a pool empty and get no
	 * synchronisation object, which is what makes an in-flight
	 * upload to it unprotected (see the deferred-release notes above). Reserved
	 * before the seal it is an ordinary, fully synced texture for the life of
	 * the process.
	 *
	 * The reservation itself is in GraphicsPrivate (src/display/graphics.cpp),
	 * which is constructed between reserveFixedSurfaces() and seal(). A
	 * SECOND enumerator here would be another 256 KiB of GPU-visible memory
	 * and another sync object: a budget decision, not a detail, and
	 * graphics.cpp static_asserts the count to keep it one. */
	enum ReservedTexture
	{
		ScreenOverlayTexture = 0,

		ReservedTextureCount
	};

	/* Call once, with a current GL context, BEFORE anything else in the
	 * process allocates a GL object. Creates and verifies every fixed
	 * surface at the largest resolution the game can ever ask for, so
	 * Graphics.resize_screen never has to create one later. Throws
	 * Exception::MKXPError naming the surface that failed. */
	void reserveFixedSurfaces(const Config &conf, int rgssVersion);

	/* Hand a reserved surface to its owner. Asserts it was reserved. */
	const TEXFBO &reserved(Surface which);

	/* The same entry, writable, for the two owners that re-specify it
	 * (PingPong and GraphicsPrivate). They bind a REFERENCE to this rather
	 * than copying, so the registry and the engine cannot disagree about a
	 * surface's size or its ids. Legal before reserveFixedSurfaces() only
	 * for IntegerScale, which stays zeroed when the config did not ask for
	 * it and is read solely behind integerScaleStepApplicable(). */
	TEXFBO &reservedMutable(Surface which);

	/* True when reserveFixedSurfaces() created the integer-scale surface. */
	bool haveIntegerScaleSurface();

	/* One 1-pixel draw per (program x target x blend mode) so the driver
	 * builds every per-program code variant -- and takes any code-heap
	 * segment it needs -- while sync objects are still available. Any GL
	 * error here is fatal: Exception::MKXPError. Call after the shader set
	 * is compiled and before any game asset loads. */
	void warmUpPrograms();

	/* Diagnostic, telemetry-marker only: create throw-away 16x16 checked
	 * surfaces until one fails, log how many there were, delete them all.
	 * The only userland view of the remaining firmware budget.
	 *
	 * Also run at every scene change and after every
	 * freeze composite, not only at boot -- the early crash happened with the
	 * engine's own counters reading a perfectly healthy surfaces=3, and this
	 * is the only number that would have shown the pool draining. A periodic
	 * run must not look like a seal violation, must not disturb the live
	 * counters, must restore the binding it found, and disposes of a refused
	 * probe through the same failed-FBO discipline as a real surface.
	 * `where` labels the line: "boot", "scene", "freeze". */
	void headroomCanary(const char *where = "boot");

	/* Close boot. After this, creationSite() reports a new program, VAO
	 * or texture and continues, and a new render surface is refused. */
	void seal();
}
#endif

#endif // GLUTIL_H
