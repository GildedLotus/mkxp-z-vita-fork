/*
** window.cpp
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

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "frameprofile.h"
#endif
#include "window.h"

#include "viewport.h"
#include "sharedstate.h"
#include "bitmap.h"
#include "etc.h"
#include "etc-internal.h"
#include "tilequad.h"

#include "gl-util.h"
#include "quad.h"
#include "quadarray.h"
#include "texpool.h"
#include "glstate.h"

#include "sigslot/signal.hpp"

#ifdef MKXPZ_SOFTWARE_BITMAPS
/* CPU-composed window base. The base texture stops being a
 * render target: it is composed from the windowskin's CPU pixels (the
 * authoritative ones) and uploaded whole-level. The
 * pipeline model itself is declared in window.h and shared with windowvx.cpp;
 * everything below is inside #ifdef, so with the option off this file
 * preprocesses to stock mkxp-z. */
#include <SDL_surface.h>

#include "exception.h"

/* The shade-free Normal/KeepDestAlpha quads go
 * through the swraster nearest kernel (swraster.cpp, copied in by the
 * configure script). */
#include "swraster.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>

namespace SoftBase
{

namespace
{

/* Fixed point, not double: a fragment channel is a byte
 * with 8 fraction bits, so 1.0 is kOne = 255 << 8 and every step below is the
 * double formulation's expression scaled by an integer. Byte inputs enter
 * exactly (b << 8), so unfiltered unshaded quads write what the doubles did;
 * only the Q12 bilinear weights and the quantised shade can move a byte, by
 * at most 1. */
const int kOne = 255 << 8;

inline int clampUnit(int v)
{
	return v < 0 ? 0 : (v > kOne ? kOne : v);
}

/* round(v * scale), clamped, once per quad or per tap: never in the loop. */
inline int quantise(double v, double scale, int lo, int hi)
{
	const int q = (int)std::floor(v * scale + 0.5);
	return q < lo ? lo : (q > hi ? hi : q);
}

/* round((x*a + d*(1-a)) * 255) for x, a in kOne units and a destination
 * byte d. The convex sum is at most kOne^2 + kHalf < 2^32, and the divisor
 * is a constant, so the compiler multiplies instead of dividing. */
const uint32_t kBlendDiv = (uint32_t)kOne * 256;

inline uint8_t blendByte(uint32_t x, uint32_t a, uint32_t d)
{
	return (uint8_t)((x * a + (d << 8) * ((uint32_t)kOne - a) + kBlendDiv / 2)
	                 / kBlendDiv);
}

inline int clampInt(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* One axis of the sampler, precomputed once per destination column and row.
 * The Cortex-A9 has no hardware integer divide and no fast double divide, so
 * nothing inside the pixel loop may divide. */
struct Tap
{
	int i0, i1;
	uint32_t w; /* weight of i1 in 1/4096ths */
};

void buildTaps(std::vector<Tap> &taps, int dstFrom, int dstCount,
               int posOrigin, int posLen, int texOrigin, int texLen,
               int limit, bool smooth)
{
	const double scale = (double)texLen / (double)posLen;
	taps.resize((size_t)dstCount);

	for (int i = 0; i < dstCount; ++i)
	{
		/* The interpolated texture coordinate at this destination pixel's
		 * centre, in windowskin texels: simple.vert carries texCoord in
		 * pixels and only normalises it by texSizeInv. */
		const double u = ((double)(dstFrom + i - posOrigin) + 0.5) * scale
		               + (double)texOrigin;
		Tap &tap = taps[(size_t)i];

		if (!smooth)
		{
			/* GL_NEAREST: floor(u), clamped to the texture. */
			tap.i0 = tap.i1 = clampInt((int)std::floor(u), 0, limit);
			tap.w = 0;
			continue;
		}

		/* GL_LINEAR: the two texels straddling u - 0.5, CLAMP_TO_EDGE. */
		const double f = u - 0.5;
		const int base = (int)std::floor(f);
		tap.w = (uint32_t)quantise(f - (double)base, 4096.0, 0, 4096);
		tap.i0 = clampInt(base, 0, limit);
		tap.i1 = clampInt(base + 1, 0, limit);
	}
}

/* Shade quantised once per quad: tone in kOne units, gray in 1/4096ths,
 * opacity in 1/65536ths. 'identity' is the shade that provably does nothing,
 * which is every frame quad of both window types and every background quad
 * at back opacity 255. */
struct ShadeQ
{
	int r, g, b;
	uint32_t gray, opacity;
	bool identity;

	explicit ShadeQ(const Shade &s)
	    : r(quantise(s.r, kOne, -kOne, kOne)),
	      g(quantise(s.g, kOne, -kOne, kOne)),
	      b(quantise(s.b, kOne, -kOne, kOne)),
	      gray((uint32_t)quantise(s.gray, 4096.0, 0, 4096)),
	      opacity((uint32_t)quantise(s.opacity, 65536.0, 0, 65536)),
	      identity(s.r == 0 && s.g == 0 && s.b == 0 &&
	               s.gray == 0 && s.opacity == 1)
	{}
};

/* shader/plane.frag, color.a == 0 and flash.a == 0, on 0..kOne channels.
 * BT.601 luma weights in 1/65536ths (19595 + 38470 + 7471 = 65536); every
 * product stays below 2^32. */
inline void shadeFragment(int *frag, const ShadeQ &shade)
{
	const uint32_t gray = shade.gray;

	if (gray != 0)
	{
		const uint32_t luma = ((uint32_t)frag[0] * 19595u +
		                       (uint32_t)frag[1] * 38470u +
		                       (uint32_t)frag[2] * 7471u + 32768u) >> 16;

		for (int ch = 0; ch < 3; ++ch)
			frag[ch] = (int)(((uint32_t)frag[ch] * (4096u - gray) +
			                  luma * gray + 2048u) >> 12);
	}

	frag[0] += shade.r;
	frag[1] += shade.g;
	frag[2] += shade.b;
	frag[3] = (int)(((uint32_t)frag[3] * shade.opacity + 32768u) >> 16);
}

} // namespace

void clear(const Surface &dst)
{
	if (!dst.px)
		return;

	for (int y = 0; y < dst.h; ++y)
		std::memset(dst.px + (size_t)y * (size_t)dst.stride, 0, (size_t)dst.w * 4);
}

void drawQuad(const Surface &dst, const IntRect &pos,
              const Surface &src, const IntRect &tex,
              const Shade &shade, Blend blend, bool smooth)
{
	if (!dst.px || !src.px)
		return;
	if (dst.w <= 0 || dst.h <= 0 || src.w <= 0 || src.h <= 0)
		return;
	if (pos.w <= 0 || pos.h <= 0 || tex.w <= 0 || tex.h <= 0)
		return;

	/* A 1:1 quad samples exact texel centres, where GL_LINEAR is GL_NEAREST.
	 * Every frame, corner and tile quad of both window types is 1:1; only the
	 * stretched background actually filters. */
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	FrameProfile::Scope profileQuad(FrameProfile::Compose, 0, false, false);
#endif
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	if (profileQuad.enabled()) ++FrameProfile::state.batch.quads;
#endif
	if (tex.w == pos.w && tex.h == pos.h)
		smooth = false;

	/* Destination pixels outside the buffer are dropped, exactly as the
	 * viewport clips them for the GPU pass. */
	const int x0 = std::max(pos.x, 0);
	const int y0 = std::max(pos.y, 0);
	const int x1 = std::min(pos.x + pos.w, dst.w);
	const int y1 = std::min(pos.y + pos.h, dst.h);

	if (x1 <= x0 || y1 <= y0)
		return;

	const ShadeQ sh(shade);

	/* A shade-free 1:1 quad in one of the
	 * alpha-accumulating blends is exactly swraster's simple_blit -- the
	 * frame/border passes of both window types, and the VX tiled
	 * background at back opacity 255 with no tone. The kernel is
	 * byte-identical to the generic loop below: with an identity shade the
	 * fragment is the raw source byte, and byte operands reach no rounding
	 * tie, so its constant-65025 round-half-up writes what this loop's
	 * fixed point writes. The containment test keeps every quads' taps inside
	 * the skin, so the loop's CLAMP_TO_EDGE behaviour stays out of scope;
	 * anything filtered, shaded or clipped against the source edge falls
	 * through to the generic path unchanged. */
	if (!smooth && tex.w == pos.w && tex.h == pos.h &&
	    sh.identity && blend != Replace)
	{
		const int sx0 = tex.x + (x0 - pos.x);
		const int sy0 = tex.y + (y0 - pos.y);
		if (sx0 >= 0 && sy0 >= 0 &&
		    sx0 + (x1 - x0) <= src.w && sy0 + (y1 - y0) <= src.h)
		{
			const swraster::Surface sdst = { dst.px, dst.w, dst.h, dst.stride };
			const swraster::Surface ssrc = { src.px, src.w, src.h, src.stride };
			swraster::simple_blit(sdst,
			                      swraster::Rect{x0, y0, x1 - x0, y1 - y0},
			                      ssrc,
			                      swraster::Rect{sx0, sy0, x1 - x0, y1 - y0},
			                      blend == KeepDestAlpha);
			return;
		}
	}

	const bool plainCopy = !smooth && blend == Replace &&
	                       shade.gray == 0 && shade.r == 0 && shade.g == 0 &&
	                       shade.b == 0 && shade.opacity == 1;

	std::vector<Tap> cols, rows;
	buildTaps(cols, x0, x1 - x0, pos.x, pos.w, tex.x, tex.w, src.w - 1, smooth);
	buildTaps(rows, y0, y1 - y0, pos.y, pos.h, tex.y, tex.h, src.h - 1, smooth);

	/* Raw pointers, not operator[]: the production build carries
	 * -D_GLIBCXX_ASSERTIONS, so every indexed access is a live bounds check
	 * inside the pixel loop. */
	const Tap *colTap = cols.data();
	const Tap *rowTap = rows.data();

	for (int y = y0; y < y1; ++y)
	{
		const Tap &row = rowTap[y - y0];
		const uint8_t *srow0 = src.px + (size_t)row.i0 * (size_t)src.stride;
		const uint8_t *srow1 = src.px + (size_t)row.i1 * (size_t)src.stride;
		uint8_t *drow = dst.px + (size_t)y * (size_t)dst.stride;

		if (plainCopy)
		{
			for (int x = x0; x < x1; ++x)
				std::memcpy(drow + (size_t)x * 4,
				            srow0 + (size_t)colTap[x - x0].i0 * 4, 4);
			continue;
		}

		for (int x = x0; x < x1; ++x)
		{
			const Tap &col = colTap[x - x0];
			int frag[4];

			if (!smooth)
			{
				const uint8_t *s = srow0 + (size_t)col.i0 * 4;

				for (int ch = 0; ch < 4; ++ch)
					frag[ch] = s[ch] << 8;
			}
			else
			{
				const uint8_t *p00 = srow0 + (size_t)col.i0 * 4;
				const uint8_t *p01 = srow0 + (size_t)col.i1 * 4;
				const uint8_t *p10 = srow1 + (size_t)col.i0 * 4;
				const uint8_t *p11 = srow1 + (size_t)col.i1 * 4;
				const uint32_t wx = col.w, wy = row.w;

				/* Q12 x Q12 weights: at most 255 << 24 plus the rounding
				 * half, below 2^32, reduced to a byte with 8 fraction bits. */
				for (int ch = 0; ch < 4; ++ch)
				{
					const uint32_t top = p00[ch] * (4096u - wx) + p01[ch] * wx;
					const uint32_t bot = p10[ch] * (4096u - wx) + p11[ch] * wx;

					frag[ch] = (int)((top * (4096u - wy) + bot * wy + 32768u) >> 16);
				}
			}

			if (!sh.identity)
				shadeFragment(frag, sh);

			uint8_t *d = drow + (size_t)x * 4;

			if (blend == Replace)
			{
				/* round(clamp01(f) * 255), f = frag / kOne. */
				for (int ch = 0; ch < 4; ++ch)
					d[ch] = (uint8_t)((clampUnit(frag[ch]) + 128) >> 8);

				continue;
			}

			/* GL clamps a fragment to 0..1 before blending it into a
			 * fixed-point colour buffer. */
			const uint32_t sa = (uint32_t)clampUnit(frag[3]);

			/* A fully transparent fragment leaves the destination exactly as
			 * it was (blendByte returns d when a == 0). Windowskin frames are
			 * mostly transparent, so this skips real work. */
			if (sa == 0)
				continue;

			for (int ch = 0; ch < 3; ++ch)
				d[ch] = blendByte((uint32_t)clampUnit(frag[ch]), sa, d[ch]);

			/* BlendKeepDestAlpha writes GL_ZERO, GL_ONE for alpha. */
			if (blend == Normal)
				d[3] = blendByte((uint32_t)kOne, sa, d[3]);
		}
	}
}

/* The GPU limit is 4096 and only the device's own config can set
 * caps.maxTexSize past it (a game cannot); the base is a CPU buffer plus a
 * texture, so its pixels are bounded too, in 64 bits so the product cannot
 * wrap. */
static const int kMaxBaseDim = 4096;
static const long long kMaxBaseBytes = 16LL * 1024 * 1024;

void checkBaseSize(int w, int h)
{
	const int maxSize = std::min(glState.caps.maxTexSize, kMaxBaseDim);

	if (w < 0 || h < 0 || w > maxSize || h > maxSize ||
	    (long long) w * (long long) h * 4 > kMaxBaseBytes)
		throw Exception(Exception::MKXPError,
		                "Texture dimensions [%d, %d] exceed hardware capabilities",
		                w, h);
}

void allocBase(std::vector<uint8_t> &px, int w, int h)
{
	checkBaseSize(w, h);

	/* checkBaseSize bounds the pixel count, so this product cannot wrap even
	 * where size_t is 32 bits. */
	const size_t bytes = (size_t) w * (size_t) h * 4;

	try
	{
		px.assign(bytes, 0);
	}
	catch (const std::bad_alloc &)
	{
		throw Exception(Exception::MKXPError,
		                "Failed to allocate a %dx%d window base (%u bytes)",
		                w, h, (unsigned) bytes);
	}
	catch (const std::length_error &)
	{
		throw Exception(Exception::MKXPError,
		                "Failed to allocate a %dx%d window base (%u bytes)",
		                w, h, (unsigned) bytes);
	}
}

} // namespace SoftBase

#endif

template<typename T>
struct Sides
{
	T l, r, t, b;
};

template<typename T>
struct Corners
{
	T tl, tr, bl, br;
};

static const IntRect backgroundSrc(0, 0, 128, 128);

static const IntRect cursorSrc(128, 64, 32, 32);

static const IntRect pauseAniSrc[] =
{
	IntRect(160, 64, 16, 16),
	IntRect(176, 64, 16, 16),
	IntRect(160, 80, 16, 16),
	IntRect(176, 80, 16, 16)
};

static const Sides<IntRect> bordersSrc =
{
	IntRect(128, 16, 16, 32),
	IntRect(176, 16, 16, 32),
	IntRect(144,  0, 32, 16),
	IntRect(144, 48, 32, 16)
};

static const Corners<IntRect> cornersSrc =
{
	IntRect(128,  0, 16, 16),
	IntRect(176,  0, 16, 16),
	IntRect(128, 48, 16, 16),
	IntRect(176, 48, 16, 16)
};

static const Sides<IntRect> scrollArrowSrc =
{
	IntRect(144, 24,  8, 16),
	IntRect(168, 24,  8, 16),
	IntRect(152, 16, 16,  8),
	IntRect(152, 40, 16,  8)
};

/* Cycling */
static const uint8_t cursorAniAlpha[] =
{
	/* Fade out */
	0xFF, 0xF7, 0xEF, 0xE7, 0xDF, 0xD7, 0xCF, 0xC7,
	0xBF, 0xB7, 0xAF, 0xA7, 0x9F, 0x97, 0x8F, 0x87,
	/* Fade in */
	0x7F, 0x87, 0x8F, 0x97, 0x9F, 0xA7, 0xAF, 0xB7,
	0xBF, 0xC7, 0xCF, 0xD7, 0xDF, 0xE7, 0xEF, 0xF7
};

static elementsN(cursorAniAlpha);

/* Cycling */
static const uint8_t pauseAniQuad[] =
{
	0, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 1, 1, 1, 1, 1, 1,
	2, 2, 2, 2, 2, 2, 2, 2,
	3, 3, 3, 3, 3, 3, 3, 3
};

static elementsN(pauseAniQuad);

/* No cycle */
static const uint8_t pauseAniAlpha[] =
{
	0x00, 0x20, 0x40, 0x60,
	0x80, 0xA0, 0xC0, 0xE0,
	0xFF
};

static elementsN(pauseAniAlpha);

/* Points to an array of quads which it doesn't own.
 * Useful for setting alpha of quads stored inside
 * bigger arrays */
struct QuadChunk
{
	Vertex *vert;
	int count; /* In quads */

	QuadChunk()
	    : vert(0), count(0)
	{}

	void setAlpha(float value)
	{
		for (int i = 0; i < count*4; ++i)
			vert[i].color.w = value;
	}
};

/* Vocabulary:
 *
 * Base: 'Base' layer of window; includes background and borders.
 *   Drawn at z+0.
 *
 * Controls: 'Controls' layer of window; includes scroll arrows,
 *   pause animation, cursor rectangle and contents bitmap.
 *   Drawn at z+2.
 *
 * Scroll arrows: Arrows that appear automatically when a part of
 *   the contents bitmap is not visible in either upper, lower, left
 *   or right direction.
 *
 * Pause: Animation that displays an animating icon in the bottom
 *   center of the window, usually indicating user input is awaited,
 *   such as when text is displayed.
 *
 * Cursor: Blinking rectangle that usually displays a selection to
 *   the user.
 *
 * Contents: User settable bitmap that is drawn inside the window,
 *   clipped to a 16 pixel smaller rectangle. Position is adjusted
 *   with OX/OY.
 *
 * BaseTex: If the window has an opacity <255, we have to prerender
 *   the base to a texture and draw that. Otherwise, we can draw the
 *   quad array directly to the screen.
 */

struct WindowPrivate
{
#ifdef MKXPZ_SOFTWARE_BITMAPS
	/* Composition is deferred while the window is hidden, so the private
	 * needs to ask the element whether it is. */
	Window *const owner;
#endif

	Bitmap *windowskin;

	Bitmap *contents;
	Bitmap *realContents;

	sigslot::connection windowskinDispCon;
	sigslot::connection contentsDispCon;

	bool bgStretch;
	Rect *cursorRect;
	bool active;
	bool pause;

	sigslot::connection cursorRectCon;

	Vec2i sceneOffset;

	Vec2i position;
	Vec2i size;
	Vec2i contentsOffset;
	Vec2i realContentsOffset;

	NormValue opacity;
	NormValue backOpacity;
	NormValue contentsOpacity;

	bool baseVertDirty;
	bool opacityDirty;
	bool baseTexDirty;

	ColorQuadArray baseQuadArray;

	/* Used when opacity < 255 */
	TEXFBO baseTex;
	bool useBaseTex;

#ifdef MKXPZ_SOFTWARE_BITMAPS
	/* THE base pixels. baseTex keeps its width/height and its texture id --
	 * the texture is an upload-only sampling cache -- and baseTex.fbo stays 0
	 * forever: no render target, no TexPool. */
	std::vector<uint8_t> basePixels;

	/* Back opacity currently baked into basePixels; -1 = nothing baked yet.
	 * updateBaseAlpha() is also what Window::setOpacity goes through, and
	 * plain opacity is applied to baseTexQuad at draw time, so it must not
	 * drag a CPU compose plus a whole-level upload into every frame of a
	 * window fade. */
	int bakedBackOpacity;

	/* Where the base is when it was composed on the GPU;
	 * invalid while it lives in basePixels/baseTex instead. */
	BaseSlot gpuSlot;
#endif

	QuadChunk backgroundVert;

	Quad baseTexQuad;

	struct WindowControls : public ViewportElement
	{
		WindowPrivate *p;

		WindowControls(WindowPrivate *p,
		               Viewport *viewport = 0)
		    : ViewportElement(viewport),
		      p(p)
		{
			setZ(2);
		}

		void draw()
		{
			p->drawControls();
		}

		void release()
		{
			unlink();
		}

		ABOUT_TO_ACCESS_NOOP
	};

	WindowControls controlsElement;

	ColorQuadArray controlsQuadArray;
	int controlsQuadCount;

	Quad contentsQuad;

	QuadChunk pauseAniVert;
	QuadChunk cursorVert;

	uint8_t cursorAniAlphaIdx;
	uint8_t pauseAniAlphaIdx;
	uint8_t pauseAniQuadIdx;

	bool controlsVertDirty;

	EtcTemps tmp;

	sigslot::connection prepareCon;

	bool contentsVisible;

#ifdef MKXPZ_SOFTWARE_BITMAPS
	WindowPrivate(Window *owner, Viewport *viewport = 0)
	    : owner(owner),
	      windowskin(0),
#else
	WindowPrivate(Viewport *viewport = 0)
	    : windowskin(0),
#endif
	      contents(0),
	      realContents(0),
	      bgStretch(true),
	      cursorRect(&tmp.rect),
	      active(true),
	      pause(false),
	      opacity(255),
	      backOpacity(255),
	      contentsOpacity(255),
	      baseVertDirty(true),
	      opacityDirty(true),
	      baseTexDirty(true),
#ifdef MKXPZ_SOFTWARE_BITMAPS
	      bakedBackOpacity(-1),
#endif
	      controlsElement(this, viewport),
	      cursorAniAlphaIdx(0),
	      pauseAniAlphaIdx(0),
	      pauseAniQuadIdx(0),
	      controlsVertDirty(true)
	{
		refreshCursorRectCon();

		controlsQuadArray.resize(14);
		cursorVert.count = 9;
		pauseAniVert.count = 1;

		prepareCon = shState->prepareDraw.connect
		        (&WindowPrivate::prepare, this);
	}

	~WindowPrivate()
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* The only GL object a window owns is its sampling texture, and only
		 * if it was ever composed. Nothing to give back to a pool. */
		if (baseTex.tex != TEX::ID(0))
			TEX::del(baseTex.tex);
		GPUBudget::releaseWindowBase(gpuSlot);
#else
		shState->texPool().release(baseTex);
#endif
		cursorRectCon.disconnect();
		prepareCon.disconnect();

		windowskinDisposal();
		contentsDisposal();
	}

	void windowskinDisposal()
	{
		windowskin = 0;
		windowskinDispCon.disconnect();
	}

	void contentsDisposal()
	{
		if (contents != realContents)
		{
			delete contents;
		}
		realContents = contents = 0;
		contentsDispCon.disconnect();
	}

	void updateChild()
	{
		if (!contentsOpacity)
		{
			contentsVisible = false;
			return;
		}
		
		if (contents == realContents || !contents->getChildInfo())
		{
			contentsOffset = realContentsOffset;
			contentsVisible = true;
			return;
		}
		
		ChildPublic &shared = *contents->getChildInfo();
		
		shared.realOffset = realContentsOffset;
		shared.offset = contentsOffset;
		shared.x = position.x;
		shared.y = position.y;
		shared.width = size.y;
		shared.height = size.y;
		
		contents->childUpdate();
		
		contentsOffset = Vec2i(shared.offset.x, shared.offset.y);
		contentsVisible = shared.isVisible;
	}

	void markControlVertDirty()
	{
		controlsVertDirty = true;
	}

	void refreshCursorRectCon()
	{
		cursorRectCon.disconnect();
		cursorRectCon = cursorRect->valueChanged.connect
		        (&WindowPrivate::markControlVertDirty, this);
	}

	void buildBaseVert()
	{
		int w = size.x;
		int h = size.y;

		IntRect bgRect(2, 2, w - 4, h - 4);

		Sides<IntRect> borderRects;
		borderRects.l = IntRect(0,    8,    16,   h-16);
		borderRects.r = IntRect(w-16, 8,    16,   h-16);
		borderRects.t = IntRect(8,    0,    w-16, 16  );
		borderRects.b = IntRect(8,    h-16, w-16, 16  );

		Corners<IntRect> cornerRects;
		cornerRects.tl = IntRect(0,    0,    16, 16);
		cornerRects.tr = IntRect(w-16, 0,    16, 16);
		cornerRects.bl = IntRect(0,    h-16, 16, 16);
		cornerRects.br = IntRect(w-16, h-16, 16, 16);

		/* Required quad count; 64 bits so a huge window cannot wrap it (it
		 * still fits a 32-bit size_t), and resize() refuses one the array
		 * cannot hold. */
		long long count = 0;

		/* Background */
		const int bgCount = bgStretch ? 1 :
		        TileQuads::twoDimCount(128, 128, bgRect.w, bgRect.h);

		count += bgCount;

		/* Borders (sides) */
		count += (long long) TileQuads::oneDimCount(32, w-16) * 2;
		count += (long long) TileQuads::oneDimCount(32, h-16) * 2;

		/* Corners */
		count += 4;

		/* Our vertex array */
		baseQuadArray.resize((size_t) count);
		backgroundVert.count = bgCount;
		Vertex *vert = baseQuadArray.vertices.data();

		int i = 0;
		backgroundVert.vert = &vert[i];

		/* Background */
		if (bgStretch)
		{
			Quad::setTexRect(&vert[i*4], backgroundSrc);
			Quad::setPosRect(&vert[i*4], bgRect);
			i += 1;
		}
		else
		{
			i += TileQuads::build(backgroundSrc, bgRect, &vert[i*4]);
		}

		/* Borders */
		i += TileQuads::buildH(bordersSrc.t, w-16, 8,    0,    &vert[i*4]);
		i += TileQuads::buildH(bordersSrc.b, w-16, 8,    h-16, &vert[i*4]);
		i += TileQuads::buildV(bordersSrc.l, h-16, 0,    8,    &vert[i*4]);
		i += TileQuads::buildV(bordersSrc.r, h-16, w-16, 8,    &vert[i*4]);

		/* Corners */
		i += Quad::setTexPosRect(&vert[i*4], cornersSrc.tl, cornerRects.tl);
		i += Quad::setTexPosRect(&vert[i*4], cornersSrc.tr, cornerRects.tr);
		i += Quad::setTexPosRect(&vert[i*4], cornersSrc.bl, cornerRects.bl);
		i += Quad::setTexPosRect(&vert[i*4], cornersSrc.br, cornerRects.br);

		for (int j = 0; j < count*4; ++j)
			vert[j].color = Vec4(1, 1, 1, 1);


		FloatRect texRect = FloatRect(0, 0, size.x, size.y);
		baseTexQuad.setTexPosRect(texRect, texRect);

		opacityDirty = true;
		baseTexDirty = true;
	}

	void updateBaseAlpha()
	{
		/* This is always applied unconditionally */
		backgroundVert.setAlpha(backOpacity.norm);

		baseTexQuad.setColor(Vec4(1, 1, 1, opacity.norm));

#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* Only backOpacity is baked into the base; plain opacity rides on
		 * baseTexQuad's vertex colour and needs no recompose. */
		if (backOpacity != bakedBackOpacity)
			baseTexDirty = true;
#else
		baseTexDirty = true;
#endif
	}

	void ensureBaseTexReady()
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* Stock refused an oversized base here, inside TexPool::request; say
		 * so at the same moment, before anything composes. */
		SoftBase::checkBaseSize(size.x, size.y);

		/* The base is a CPU buffer sized exactly to the window; there is no
		 * pool and no pow2 rounding, because the cost that matters is the
		 * whole-level upload (about 1 ms + 13 ns/px) and not an allocation.
		 * The texture itself is created by the first compose. */
		if (baseTex.width == size.x && baseTex.height == size.y)
			return;

		baseTex.width = size.x;
		baseTex.height = size.y;
		basePixels.clear();
		bakedBackOpacity = -1;
		baseTexDirty = true;
		return;
#else
		/* Make sure texture is big enough */
		int newW = baseTex.width;
		int newH = baseTex.height;
		bool resizeNeeded = false;

		if (size.x > baseTex.width)
		{
			newW = findNextPow2(size.x);
			resizeNeeded = true;
		}
		if (size.y > baseTex.height)
		{
			newH = findNextPow2(size.y);
			resizeNeeded = true;
		}

		if (!resizeNeeded)
			return;

		shState->texPool().release(baseTex);
		baseTex = shState->texPool().request(newW, newH);

		baseTexDirty = true;
#endif
	}

#ifdef MKXPZ_SOFTWARE_BITMAPS
	/* One quad of the built baseQuadArray, read back as the rects the GPU
	 * pass would have interpolated. Quad::setPosRect writes top-left,
	 * top-right, bottom-right, bottom-left. */
	static IntRect quadPos(const Vertex *v)
	{
		return IntRect((int)v[0].pos.x, (int)v[0].pos.y,
		               (int)(v[1].pos.x - v[0].pos.x),
		               (int)(v[3].pos.y - v[0].pos.y));
	}

	static IntRect quadTex(const Vertex *v)
	{
		return IntRect((int)v[0].texPos.x, (int)v[0].texPos.y,
		               (int)(v[1].texPos.x - v[0].texPos.x),
		               (int)(v[3].texPos.y - v[0].texPos.y));
	}

	/* CPU twin of redrawBaseTex(): the same quads, the same shader
	 * (simpleAlpha, whose only effect is the per-vertex alpha that carries
	 * backOpacity), the same two blend states, composed into basePixels and
	 * uploaded whole-level. No framebuffer is bound and no pool is touched. */
	void composeBaseTex()
	{
		if (nullOrDisposed(windowskin))
			return;

		/* Below 16x16 buildBaseVert() counts border quads that TileQuads
		 * refuses to build, which shifts every later quad; stock draws the
		 * stale slots. Compose nothing rather than that garbage. */
		if (size.x < 16 || size.y < 16)
			return;

		SDL_Surface *skin = windowskin->surface();

		if (!skin || skin->w <= 0 || skin->h <= 0)
			return;

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		FrameProfile::Scope profileCompose(FrameProfile::Compose);
#endif
		const SoftBase::Surface src = { (uint8_t*) skin->pixels,
		                                skin->w, skin->h, skin->pitch };

		SoftBase::allocBase(basePixels, baseTex.width, baseTex.height);

		const SoftBase::Surface dst = { basePixels.data(), baseTex.width,
		                                baseTex.height, baseTex.width * 4 };

		const Vertex *vert = baseQuadArray.vertices.data();
		const size_t quads = baseQuadArray.count();
		size_t q = 0;

		/* The background is drawn with blending off so its alpha survives
		 * unmultiplied; the frame is drawn over it with BlendNormal. */
		for (; q < quads && q < (size_t) backgroundVert.count; ++q)
		{
			SoftBase::Shade shade;
			shade.opacity = vert[q*4].color.w;

			SoftBase::drawQuad(dst, quadPos(&vert[q*4]), src, quadTex(&vert[q*4]),
			                   shade, SoftBase::Replace, true);
		}

		for (; q < quads; ++q)
		{
			SoftBase::Shade shade;
			shade.opacity = vert[q*4].color.w;

			SoftBase::drawQuad(dst, quadPos(&vert[q*4]), src, quadTex(&vert[q*4]),
			                   shade, SoftBase::Normal, true);
		}

		uploadBaseTex();
		bakedBackOpacity = backOpacity;
	}

	/* Whole-level upload into a plain texture. Never attached to a
	 * framebuffer, so it never owns one of the driver's render surfaces. */
	void uploadBaseTex()
	{
		TEX::ScopedBinding binding;
		if (baseTex.tex == TEX::ID(0))
		{
			baseTex.tex = TEX::gen();
			if (baseTex.tex == TEX::ID(0))
				throw TEX::UploadError();
			TEX::bind(baseTex.tex);
			TEX::setRepeat(false);
			TEX::setSmooth(false);
		}
		else
		{
			TEX::bind(baseTex.tex);
		}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		FrameProfile::Scope profileUpload(FrameProfile::Upload, 0, false, false);
#endif
		if (!TEX::uploadImageChecked(baseTex.width, baseTex.height, basePixels.data(), GL_RGBA))
			throw TEX::UploadError();
	}

	/* The stock pass below, drawn into this window's atlas slot instead of
	 * a pooled render target. False when there is no
	 * slot to be had; the CPU twin then composes as before. */
	bool composeBaseGpu()
	{
		if (nullOrDisposed(windowskin) || size.x < 16 || size.y < 16 ||
		    !GPUBudget::placeWindowBase(gpuSlot, size.x, size.y))
		{
			GPUBudget::releaseWindowBase(gpuSlot);
			baseTexQuad.setTexRect(FloatRect(0, 0, size.x, size.y));
			return false;
		}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		FrameProfile::Scope profileCompose(FrameProfile::Compose);
#endif
		GPUBudget::composeWindowBase(gpuSlot, drawBaseLayers, this);
		baseTexQuad.setTexRect(FloatRect(gpuSlot.innerX(), gpuSlot.innerY(),
		                                 size.x, size.y));
		bakedBackOpacity = backOpacity;

		if (baseTex.tex != TEX::ID(0))
			TEX::del(baseTex.tex);
		baseTex.tex = TEX::ID(0);
		std::vector<uint8_t>().swap(basePixels);

		return true;
	}

	static void drawBaseLayers(void *ctx, const Vec2i &origin)
	{
		WindowPrivate *p = static_cast<WindowPrivate*>(ctx);

		SimpleAlphaShader &shader = shState->shaders().simpleAlpha;
		shader.bind();
		shader.applyViewportProj();
		shader.setTranslation(origin);

		p->windowskin->bindTex(shader);
		TEX::setSmooth(true);

		glState.blend.pushSet(false);
		p->baseQuadArray.draw(0, p->backgroundVert.count);

		glState.blend.set(true);
		glState.blendMode.pushSet(BlendNormal);
		p->baseQuadArray.draw(p->backgroundVert.count,
		                      p->baseQuadArray.count() - p->backgroundVert.count);

		glState.blendMode.pop();
		glState.blend.pop();
		TEX::setSmooth(false);
	}
#endif

	void redrawBaseTex()
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		if (!composeBaseGpu())
			composeBaseTex();
		return;
#else
		/* Discard old buffer */
		TEX::bind(baseTex.tex);
		TEX::allocEmpty(baseTex.width, baseTex.height);
		TEX::unbind();

		FBO::bind(baseTex.fbo);
		glState.viewport.pushSet(IntRect(0, 0, baseTex.width, baseTex.height));
		glState.clearColor.pushSet(Vec4());

		SimpleAlphaShader &shader = shState->shaders().simpleAlpha;
		shader.bind();
		shader.applyViewportProj();
		shader.setTranslation(Vec2i());

		/* Clear texture */
		FBO::clear();

		/* Repaint base */
		windowskin->bindTex(shader);
		TEX::setSmooth(true);

		/* We need to blit the background without blending,
		 * because we want to retain its correct alpha value.
		 * Otherwise it would be mutliplied by the backgrounds 0 alpha */
		glState.blend.pushSet(false);

		baseQuadArray.draw(0, backgroundVert.count);

		/* Now draw the rest (ie. the frame) with blending */
		glState.blend.pop();
		glState.blendMode.pushSet(BlendNormal);

		baseQuadArray.draw(backgroundVert.count, baseQuadArray.count()-backgroundVert.count);

		glState.clearColor.pop();
		glState.blendMode.pop();
		glState.viewport.pop();
		TEX::setSmooth(false);
#endif
	}

	void buildControlsVert()
	{
		int i = 0;
		Vertex *vert = controlsQuadArray.vertices.data();

		/* Cursor */
		if (!cursorRect->isEmpty())
		{
			/* Effective cursor rect has 16 xy offset to window */
			IntRect effectRect(cursorRect->x+16, cursorRect->y+16,
			                   cursorRect->width, cursorRect->height);
			cursorVert.vert = &vert[i*4];
			TileQuads::buildFrameSource(cursorSrc, cursorVert.vert);
			i += TileQuads::buildFrame(effectRect, cursorVert.vert);
		}

		/* Scroll arrow position: Top Bottom X, Left Right Y */
		const Vec2i scroll = (size - Vec2i(16)) / 2;

		Sides<IntRect> scrollArrows;

		scrollArrows.l = IntRect(4, scroll.y, 8, 16);
		scrollArrows.r = IntRect(size.x - 12, scroll.y, 8, 16);
		scrollArrows.t = IntRect(scroll.x, 4, 16, 8);
		scrollArrows.b = IntRect(scroll.x, size.y - 12, 16, 8);

		if (contents)
		{
			if (realContentsOffset.x > 0)
				i += Quad::setTexPosRect(&vert[i*4], scrollArrowSrc.l, scrollArrows.l);

			if (realContentsOffset.y > 0)
				i += Quad::setTexPosRect(&vert[i*4], scrollArrowSrc.t, scrollArrows.t);

			if ((size.x - 32) < (realContents->width() - realContentsOffset.x))
				i += Quad::setTexPosRect(&vert[i*4], scrollArrowSrc.r, scrollArrows.r);

			if ((size.y - 32) < (realContents->height() - realContentsOffset.y))
				i += Quad::setTexPosRect(&vert[i*4], scrollArrowSrc.b, scrollArrows.b);
		}

		/* Pause animation */
		if (pause)
		{
			pauseAniVert.vert = &vert[i*4];
			i += Quad::setTexPosRect(&vert[i*4], pauseAniSrc[pauseAniQuad[pauseAniQuadIdx]],
			                         FloatRect((size.x - 16) / 2, size.y - 16, 16, 16));
		}

		controlsQuadArray.commit();
		controlsQuadCount = i;
	}

	void prepare()
	{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		FrameProfile::OperationScope profileOperation(FrameProfile::WindowPrepare);
#endif
		if (size.x <= 0 || size.y <= 0)
			return;

		bool updateBaseQuadArray = false;

		updateChild();

		if (baseVertDirty)
		{
			buildBaseVert();
			baseVertDirty = false;
			updateBaseQuadArray = true;
		}

		if (opacityDirty)
		{
			updateBaseAlpha();
			opacityDirty = false;
			updateBaseQuadArray = true;
		}

		if (updateBaseQuadArray)
			baseQuadArray.commit();

		/* If opacity has effect, we must prerender to a texture
		 * and then draw this texture instead of the quad array */
		useBaseTex = opacity < 255;

#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* An opaque window draws its quads directly; give the slot back and
		 * recompose if it fades again. */
		if (!useBaseTex && gpuSlot.valid())
		{
			GPUBudget::releaseWindowBase(gpuSlot);
			baseTexDirty = true;
		}
#endif

		if (useBaseTex)
		{
			ensureBaseTexReady();

			if (baseTexDirty)
			{
#ifdef MKXPZ_SOFTWARE_BITMAPS
				/* Composing costs real CPU and a whole-level upload, so a
				 * hidden window keeps its dirty flag and is composed on the
				 * frame it is revealed.
				 *
				 * opacity == 0 is the same bargain: the
				 * base rides on baseTexQuad's vertex alpha, so at zero not
				 * one composed pixel can reach the screen. Custom HUDs in
				 * both XP and Ace set `self.opacity = 0` and keep the window
				 * around for its contents. */
				if (owner->getVisible() && opacity > 0)
				{
					try
					{
						redrawBaseTex();
						baseTexDirty = false;
					}
					catch (const TEX::UploadError &) { /* Retry next prepare. */ }
				}
#else
				redrawBaseTex();
				baseTexDirty = false;
#endif
			}
		}
	}

	void drawBase()
	{
		if (nullOrDisposed(windowskin))
			return;

		if (size == Vec2i(0, 0))
			return;

		SimpleAlphaShader &shader = shState->shaders().simpleAlpha;
		shader.bind();
		shader.applyViewportProj();
		shader.setTranslation(position + sceneOffset);

		if (useBaseTex)
		{
#ifdef MKXPZ_SOFTWARE_BITMAPS
			/* Nothing composed yet (the window was hidden when prepare ran,
			 * has opacity 0, or has no windowskin): draw no base rather than
			 * an untextured quad, and at opacity 0 do not draw a fully
			 * transparent one either. The controls are a separate element
			 * and still draw. */
			if (baseTexDirty || opacity == 0 ||
			    (!gpuSlot.valid() && baseTex.tex == TEX::ID(0)))
				return;

			if (gpuSlot.valid())
			{
				const TEXFBO &atlas = GPUBudget::windowBaseAtlas();
				shader.setTexSize(Vec2i(atlas.width, atlas.height));
				TEX::bind(atlas.tex);
				TEX::setSmooth(false);
				baseTexQuad.draw();
				return;
			}
#endif
			shader.setTexSize(Vec2i(baseTex.width, baseTex.height));

			TEX::bind(baseTex.tex);
			baseTexQuad.draw();
		}
		else
		{
			windowskin->bindTex(shader);
			TEX::setSmooth(true);

			baseQuadArray.draw();

			TEX::setSmooth(false);
		}
	}

	void drawControls()
	{
		if (nullOrDisposed(windowskin) && nullOrDisposed(realContents))
			return;

		if (size == Vec2i(0, 0))
			return;

		if (controlsVertDirty)
		{
			buildControlsVert();
			updateControls();
			controlsVertDirty = false;
		}

		/* Effective on screen coordinates */
		const Vec2i efPos = position + sceneOffset;

		const IntRect windowRect(efPos, size);
		const IntRect contentsRect(efPos + Vec2i(16), size - Vec2i(32));

		glState.scissorTest.pushSet(true);
		glState.scissorBox.push();
		glState.scissorBox.setIntersect(windowRect);

		SimpleAlphaShader &shader = shState->shaders().simpleAlpha;
		shader.bind();
		shader.applyViewportProj();

		if (!nullOrDisposed(windowskin))
		{
			shader.setTranslation(efPos);

			/* Draw arrows / cursors */
			windowskin->bindTex(shader);
			TEX::setSmooth(true);

			controlsQuadArray.draw(0, controlsQuadCount);

			TEX::setSmooth(false);
		}

		if (!nullOrDisposed(contents) && contentsVisible)
		{
			/* Draw contents bitmap */
			glState.scissorBox.setIntersect(contentsRect);

			shader.setTranslation(efPos + (Vec2i(16) - realContentsOffset));

			contents->bindTex(shader);
			contentsQuad.draw();
		}

		glState.scissorBox.pop();
		glState.scissorTest.pop();
	}

	void updateControls()
	{
		bool updateArray = false;

		if (active && cursorVert.vert)
		{
			float alpha = cursorAniAlpha[cursorAniAlphaIdx] / 255.0f;

			cursorVert.setAlpha(alpha);

			updateArray = true;
		}

		if (pause && pauseAniVert.vert)
		{
			float alpha = pauseAniAlpha[pauseAniAlphaIdx] / 255.0f;
			FloatRect frameRect = pauseAniSrc[pauseAniQuad[pauseAniQuadIdx]];

			pauseAniVert.setAlpha(alpha);
			Quad::setTexRect(pauseAniVert.vert, frameRect);

			updateArray = true;
		}

		if (updateArray)
			controlsQuadArray.commit();
	}

	void stepAnimations()
	{
		if (++cursorAniAlphaIdx == cursorAniAlphaN)
			cursorAniAlphaIdx = 0;

		if (pauseAniAlphaIdx < pauseAniAlphaN-1)
			++pauseAniAlphaIdx;

		if (++pauseAniQuadIdx == pauseAniQuadN)
			pauseAniQuadIdx = 0;
	}
};

Window::Window(Viewport *viewport)
	: ViewportElement(viewport)
{
#ifdef MKXPZ_SOFTWARE_BITMAPS
	p = new WindowPrivate(this, viewport);
#else
	p = new WindowPrivate(viewport);
#endif
	onGeometryChange(scene->getGeometry());
}

Window::~Window()
{
	dispose();
}

void Window::update()
{
	guardDisposed();

	p->updateControls();
	p->stepAnimations();
}

DEF_ATTR_SIMPLE(Window, X,          int,     p->position.x)
DEF_ATTR_SIMPLE(Window, Y,          int,     p->position.y)
DEF_ATTR_SIMPLE(Window, CursorRect, Rect&,  *p->cursorRect)

DEF_ATTR_RD_SIMPLE(Window, Windowskin,      Bitmap*, p->windowskin)
DEF_ATTR_RD_SIMPLE(Window, Contents,        Bitmap*, p->realContents)
DEF_ATTR_RD_SIMPLE(Window, Stretch,         bool,    p->bgStretch)
DEF_ATTR_RD_SIMPLE(Window, Active,          bool,    p->active)
DEF_ATTR_RD_SIMPLE(Window, Pause,           bool,    p->pause)
DEF_ATTR_RD_SIMPLE(Window, Width,           int,     p->size.x)
DEF_ATTR_RD_SIMPLE(Window, Height,          int,     p->size.y)
DEF_ATTR_RD_SIMPLE(Window, OX,              int,     p->realContentsOffset.x)
DEF_ATTR_RD_SIMPLE(Window, OY,              int,     p->realContentsOffset.y)
DEF_ATTR_RD_SIMPLE(Window, Opacity,         int,     p->opacity)
DEF_ATTR_RD_SIMPLE(Window, BackOpacity,     int,     p->backOpacity)
DEF_ATTR_RD_SIMPLE(Window, ContentsOpacity, int,     p->contentsOpacity)

void Window::setWindowskin(Bitmap *value)
{
	guardDisposed();

	p->windowskin = value;

	/* The base is painted from the windowskin, so a new skin invalidates it --
	 * on both backends. Stock dirties the base in
	 * buildBaseVert(), updateBaseAlpha() and ensureBaseTexReady() but never
	 * here, and got away with it because updateBaseAlpha() re-renders on every
	 * opacity change, which a message window does 48 times per fade: the stale
	 * base was overwritten before anybody could look at it. A game that swaps
	 * $game_system.windowskin_name without touching opacity or size -- RMXP's
	 * own Window_Base#update does exactly that -- keeps the old skin on screen
	 * for as long as opacity stays below 255, because below 255 the window
	 * draws baseTex and not the quads. WindowVX::setWindowskin has always
	 * dirtied here; this is that line, in its XP twin.
	 *
	 * Under MKXPZ_SOFTWARE_BITMAPS it is load-bearing rather than merely
	 * correct: that backend re-composes only on a *back* opacity change, so nothing else would ever pick the new skin up, and a skin
	 * assigned after a compose that bailed for want of one would never be
	 * composed at all. An earlier change added the line under the option; this is the
	 * same line with the option gone. */
	p->baseTexDirty = true;

	p->windowskinDispCon.disconnect();

	if (nullOrDisposed(value))
	{
		p->windowskin = 0;
		return;
	}

	value->ensureNonMega();
	
	p->windowskinDispCon = value->wasDisposed.connect(&WindowPrivate::windowskinDisposal, p);
}

void Window::setContents(Bitmap *value)
{
	guardDisposed();

	if (p->realContents == value)
		return;

	if (p->contents != p->realContents)
		delete p->contents;

	p->contents = value;
	p->realContents = value;
	p->controlsVertDirty = true;

	p->contentsDispCon.disconnect();

	if (nullOrDisposed(value))
	{
		p->realContents = p->contents = 0;
		return;
	}

	p->contentsDispCon = value->wasDisposed.connect(&WindowPrivate::contentsDisposal, p);

	if (value->isMega())
	{
		p->contents = value->spawnChild();
		
		ChildPublic &shared = *p->contents->getChildInfo();
		shared.sceneRect = &scene->getGeometry().rect;
		shared.sceneOrig = &scene->getGeometry().orig;
	}

	p->contentsQuad.setTexPosRect(value->rect(), value->rect());
}

void Window::setStretch(bool value)
{
	guardDisposed();

	if (value == p->bgStretch)
		return;

	p->bgStretch = value;
	p->baseVertDirty = true;
}

void Window::setActive(bool value)
{
	guardDisposed();

	if (p->active == value)
		return;

	p->active = value;
	p->cursorAniAlphaIdx = 0;
}

void Window::setPause(bool value)
{
	guardDisposed();

	if (p->pause == value)
		return;

	p->pause = value;
	p->pauseAniAlphaIdx = 0;
	p->pauseAniQuadIdx = 0;
	p->controlsVertDirty = true;
}

void Window::setWidth(int value)
{
	guardDisposed();

	if (p->size.x == value)
		return;

	p->size.x = value;
	p->baseVertDirty = true;
}

void Window::setHeight(int value)
{
	guardDisposed();

	if (p->size.y == value)
		return;

	p->size.y = value;
	p->baseVertDirty = true;
}

void Window::setOX(int value)
{
	guardDisposed();

	if (p->realContentsOffset.x == value)
		return;

	p->realContentsOffset.x = value;
	p->controlsVertDirty = true;
}

void Window::setOY(int value)
{
	guardDisposed();

	if (p->realContentsOffset.y == value)
		return;

	p->realContentsOffset.y = value;
	p->controlsVertDirty = true;
}

void Window::setOpacity(int value)
{
	guardDisposed();

	if (p->opacity == value)
		return;

	p->opacity = value;
	p->opacityDirty = true;
}

void Window::setBackOpacity(int value)
{
	guardDisposed();

	if (p->backOpacity == value)
		return;

	p->backOpacity = value;
	p->opacityDirty = true;
}

void Window::setContentsOpacity(int value)
{
	guardDisposed();

	if (p->contentsOpacity == value)
		return;

	p->contentsOpacity = value;
	p->contentsQuad.setColor(Vec4(1, 1, 1, p->contentsOpacity.norm));
}

void Window::initDynAttribs()
{
	p->cursorRect = new Rect;

	p->refreshCursorRectCon();
}

void Window::draw()
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	FrameProfile::OperationScope profileOperation(FrameProfile::WindowDraw);
#endif
	p->drawBase();
}

void Window::onGeometryChange(const Scene::Geometry &geo)
{
	p->sceneOffset = geo.offset();
}

void Window::setZ(int value)
{
	ViewportElement::setZ(value);

	p->controlsElement.setZ(value + 2);
}

void Window::setVisible(bool value)
{
	ViewportElement::setVisible(value);

	p->controlsElement.setVisible(value);
}

void Window::onViewportChange()
{
	p->controlsElement.setScene(*this->scene);
}

void Window::releaseResources()
{
	p->controlsElement.release();

	unlink();

	delete p;
}
