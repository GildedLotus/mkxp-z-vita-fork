/*
** window.h
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

#ifndef WINDOW_H
#define WINDOW_H

#include "viewport.h"
#include "disposable.h"

#include "util.h"

class Bitmap;
struct Rect;

struct WindowPrivate;

#ifdef MKXPZ_SOFTWARE_BITMAPS

#include <stdint.h>

#include <vector>

/* CPU-composed window base.
 *
 * Window and WindowVX both prerender their "base" -- background plus frame --
 * into a texture they own. Every render target draws on a small fixed
 * pool of GPU surfaces for the whole process, and a VX Ace scene holds five
 * to fifteen windows, so those render targets have to go. Under this option the base is
 * composed in client memory and uploaded as a plain, sample-only texture.
 *
 * SoftBase models exactly the fragment pipeline the two prerender passes use,
 * so the composed buffer is the image the GPU produced:
 *
 *   sampling  GL_NEAREST / GL_LINEAR with the texel-centre convention and
 *             CLAMP_TO_EDGE against the whole windowskin *texture*. Not
 *             against the source rect: the stretched background deliberately
 *             samples a sub-rect (bgStretchSrc is inset by one pixel) and GL
 *             filters across its edges.
 *   shading   shader/plane.frag with color.a == 0 and flash.a == 0, which is
 *             how WindowVX binds it. Shade::None is shader/simple.frag, and
 *             opacity alone is shader/simpleAlpha.frag, which is all Window
 *             ever uses.
 *   blending  the three GLBlendMode states these passes select.
 *
 * Declared here and implemented in window.cpp; windowvx.cpp includes this
 * header for it so there is one copy of the pipeline. */
namespace SoftBase
{

/* Non-premultiplied RGBA8888 (R,G,B,A in memory order, SDL's ABGR8888 on
 * little-endian -- the Bitmap format), row 0 on top, stride in bytes. */
struct Surface
{
	uint8_t *px;
	int w, h, stride;
};

/* shader/plane.frag with color and flash disabled:
 *     luma = dot(rgb, vec3(.299, .587, .114))
 *     rgb  = mix(rgb, vec3(luma), gray)
 *     rgb += vec3(r, g, b)
 *     a   *= opacity
 * Shade() is the identity. */
struct Shade
{
	float r, g, b, gray, opacity;

	Shade()
	    : r(0), g(0), b(0), gray(0), opacity(1)
	{}
};

enum Blend
{
	/* GL_BLEND off: the fragment replaces the destination outright. */
	Replace,
	/* BlendNormal: src.a over, destination alpha accumulates. */
	Normal,
	/* BlendKeepDestAlpha: colour blends, destination alpha is preserved. */
	KeepDestAlpha
};

/* Transparent black over the whole surface (FBO::clear with clearColor 0). */
void clear(const Surface &dst);

/* Stock reached TexPool::request for the base texture, which refused a size
 * the hardware cannot hold; composing on the CPU took that check away with it
 * Both are restored here, in one place, because the
 * failure modes are ugly: glTexImage2D fails with GL_INVALID_VALUE and nobody
 * looks (a stale or opaque-black base), std::length_error / std::bad_alloc
 * from the buffer is not an mkxp Exception and escapes the Ruby boundary into
 * std::terminate, and size_t is 32 bits on the Vita, so w*h*4 can wrap and
 * leave a Surface claiming pixels it does not own.
 *
 * checkBaseSize throws Exception(MKXPError) -- the message TexPool used -- for
 * a dimension past glState.caps.maxTexSize; allocBase sizes a compose buffer
 * to w*h*4 cleared bytes and turns any allocation failure into the same. */
void checkBaseSize(int w, int h);
void allocBase(std::vector<uint8_t> &px, int w, int h);

/* One textured quad, exactly as the base passes draw it: 'pos' is in
 * destination pixels, 'tex' in windowskin pixels. A quad whose source and
 * destination sizes match never filters (GL_LINEAR at exact texel centres is
 * GL_NEAREST), so 'smooth' only matters for the stretched background. */
void drawQuad(const Surface &dst, const IntRect &pos,
              const Surface &src, const IntRect &tex,
              const Shade &shade, Blend blend, bool smooth);

}

#endif // MKXPZ_SOFTWARE_BITMAPS

class Window : public ViewportElement, public Disposable
{
public:
	Window(Viewport *viewport = 0);
	~Window();

	void update();

	DECL_ATTR( Windowskin,      Bitmap* )
	DECL_ATTR( Contents,        Bitmap* )
	DECL_ATTR( Stretch,         bool    )
	DECL_ATTR( CursorRect,      Rect&   )
	DECL_ATTR( Active,          bool    )
	DECL_ATTR( Pause,           bool    )
	DECL_ATTR( X,               int     )
	DECL_ATTR( Y,               int     )
	DECL_ATTR( Width,           int     )
	DECL_ATTR( Height,          int     )
	DECL_ATTR( OX,              int     )
	DECL_ATTR( OY,              int     )
	DECL_ATTR( Opacity,         int     )
	DECL_ATTR( BackOpacity,     int     )
	DECL_ATTR( ContentsOpacity, int     )

	void initDynAttribs();

private:
	WindowPrivate *p;

	void draw();
	void onGeometryChange(const Scene::Geometry &);
	void setZ(int value);
	void setVisible(bool value);

	void onViewportChange();

	void releaseResources();
	const char *klassName() const { return "window"; }

	ABOUT_TO_ACCESS_DISP
};

#endif // WINDOW_H
