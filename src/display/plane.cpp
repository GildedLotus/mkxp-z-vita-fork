/*
** plane.cpp
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

#include "plane.h"
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "vita_glue.h"
#include <cstdio>
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "frameprofile.h"
#endif
#include "sharedstate.h"
#include "bitmap.h"
#include "etc.h"
#include "util.h"

#include "gl-util.h"
#include "quad.h"
#include "quadarray.h"
#include "transform.h"
#include "etc-internal.h"
#include "shader.h"
#include "glstate.h"

#include "sigslot/signal.hpp"

static float fwrap(float value, float range)
{
	float res = fmod(value, range);
	return res < 0 ? res + range : res;
}

/* Client-array draws chunk their index ranges (gl/quadarray.h), so the index
 * buffer no longer bounds a tiled Plane; the vertex array is the only bound.
 * 64 Ki quads is 4 MiB of SVertex, and still covers a 1x1 bitmap tiled over
 * 255x255. Rejected tilings draw nothing instead of aborting on allocation. */
enum { MaxPlaneQuads = 65536 };

/* Tile counts and wrapped offsets for a bitmap scaled to (sw, sh) covering a
 * vpw x vph viewport at Plane offset (ox, oy). False when the tiling is
 * undefined or unbounded: a zero or negative extent is never an fmod()
 * divisor, and a non-finite count or more than MaxPlaneQuads quads cannot be
 * drawn. */
static bool planeTileCounts(float sw, float sh, float ox, float oy,
                            int vpw, int vph, float &wox, float &woy,
                            size_t &tilesX, size_t &tilesY)
{
	if (!(sw > 0.0f) || !(sh > 0.0f))
		return false;

	/* Plane offset wrapped by scaled bitmap dims */
	wox = fwrap(ox, sw);
	woy = fwrap(oy, sh);

	/* Amount the scaled bitmap is tiled (repeated) */
	float tilesXf = ceil((vpw - sw + wox) / sw) + 1;
	float tilesYf = ceil((vph - sh + woy) / sh) + 1;

	if (!(tilesXf > 0.0f) || !(tilesYf > 0.0f) ||
	    tilesXf > MaxPlaneQuads || tilesYf > MaxPlaneQuads ||
	    tilesXf * tilesYf > MaxPlaneQuads)
		return false;

	tilesX = size_t(tilesXf);
	tilesY = size_t(tilesYf);
	return true;
}

struct PlanePrivate
{
	Bitmap *bitmap;
	Bitmap *realBitmap;

	sigslot::connection bitmapDispCon;

	NormValue opacity;
	BlendType blendType;
	Color *color;
	Tone *tone;

	float ox, oy;
	int realOX, realOY;
	float zoomX, zoomY;
	float realZoomX, realZoomY;
	
	bool isVisible;

	Scene::Geometry sceneGeo;

	bool quadSourceDirty;
	bool quadTracePending = true;

	SimpleQuadArray qArray;

	EtcTemps tmp;

	sigslot::connection prepareCon;

	PlanePrivate()
	    : bitmap(0),
	      realBitmap(0),
	      opacity(255),
	      blendType(BlendNormal),
	      color(&tmp.color),
	      tone(&tmp.tone),
	      ox(0), oy(0),
	      realOX(0), realOY(0),
	      realZoomX(1), realZoomY(1),
	      zoomX(1), zoomY(1),
	      isVisible(true),
	      quadSourceDirty(false)
	{
		prepareCon = shState->prepareDraw.connect
		        (&PlanePrivate::prepare, this);

		qArray.resize(1);
	}

	~PlanePrivate()
	{
		prepareCon.disconnect();
		
		bitmapDisposal();
	}

	void bitmapDisposal()
	{
        if (bitmap != realBitmap)
        {
            delete bitmap;
        }
        realBitmap = bitmap = 0;
		bitmapDispCon.disconnect();
	}

	void updateQuadSource()
	{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		FrameProfile::OperationScope profileOperation(FrameProfile::PlaneQuad);
#endif
		quadTracePending = true;
		if (gl.npot_repeat)
		{
			FloatRect srcRect;
			srcRect.x = (sceneGeo.orig.x + ox) / zoomX;
			srcRect.y = (sceneGeo.orig.y + oy) / zoomY;
			srcRect.w = sceneGeo.rect.w / zoomX;
			srcRect.h = sceneGeo.rect.h / zoomY;

			Quad::setTexRect(&qArray.vertices[0], srcRect);
			qArray.commit();

			return;
		}

		if (nullOrDisposed(bitmap))
			return;

		/* Scaled (zoomed) bitmap dimensions */
		float sw = bitmap->width()  * zoomX;
		float sh = bitmap->height() * zoomY;

		/* Amount the scaled bitmap is tiled (repeated), and the wrapped
		 * offsets the tiles are placed from. A zero extent has already hidden
		 * the Plane; a NaN or unbounded one is refused before it sizes the
		 * vertex array. */
		float wox, woy;
		size_t tilesX, tilesY;
		if (!planeTileCounts(sw, sh, sceneGeo.orig.x + ox, sceneGeo.orig.y + oy,
		                     sceneGeo.rect.w, sceneGeo.rect.h,
		                     wox, woy, tilesX, tilesY))
		{
			qArray.clear();
			return;
		}

		FloatRect tex = bitmap->rect();

		qArray.resize(tilesX * tilesY);

		for (size_t y = 0; y < tilesY; ++y)
			for (size_t x = 0; x < tilesX; ++x)
			{
				SVertex *vert = &qArray.vertices[(y*tilesX + x) * 4];
				FloatRect pos(sceneGeo.rect.x + x*sw - wox,
				              sceneGeo.rect.y + y*sh - woy, sw, sh);

				Quad::setTexPosRect(vert, tex, pos);
			}

		qArray.commit();
	}

	void updateChild()
	{
		/* An invisible Plane has no geometry to update: returning here keeps
		 * zero zoom out of the tiling arithmetic and zero opacity from
		 * uploading quads the draw will skip anyway. */
		if (!opacity || !realZoomX || !realZoomY)
		{
			isVisible = false;
			return;
		}
		
		if (bitmap == realBitmap)
		{
			ox = realOX;
			oy = realOY;
			zoomX = realZoomX;
			zoomY = realZoomY;
			isVisible = true;
			return;
		}
		
		ChildPublic &shared = *bitmap->getChildInfo();
		
		shared.sceneRect = &sceneGeo.rect;
		shared.sceneOrig = &sceneGeo.orig;
		
		// Unlike Sprites, ox/oy in Planes is unaffected by zoom. So we treat it as x/y like Sprites instead.
		shared.x = -realOX;
		shared.y = -realOY;
		shared.realZoom = Vec2(realZoomX, realZoomY);
		
		shared.width = sceneGeo.rect.w;
		shared.height = sceneGeo.rect.h;
		bitmap->childUpdate();
		
		isVisible = shared.isVisible;
		
		if (!isVisible)
		{
			return;
		}
		
		if (ox != shared.offset.x || oy != shared.offset.y ||
		    zoomX != shared.zoom.x || zoomY != shared.zoom.y)
			quadSourceDirty = true;
		
		// Leaving these as floats increases precision when zoomed
		ox = shared.offset.x;
		oy = shared.offset.y;
		
		zoomX = shared.zoom.x;
		zoomY = shared.zoom.y;
		
		
	}

	void prepare()
	{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		FrameProfile::OperationScope profileOperation(FrameProfile::PlanePrepare);
#endif
		if (nullOrDisposed(bitmap))
			return;
		
		updateChild();
		
		if (!isVisible)
			return;
		
		if (quadSourceDirty)
		{
			updateQuadSource();
			quadSourceDirty = false;
		}
	}
};

Plane::Plane(Viewport *viewport)
    : ViewportElement(viewport)
{
	p = new PlanePrivate();

	onGeometryChange(scene->getGeometry());
}

DEF_ATTR_RD_SIMPLE(Plane, Bitmap,    Bitmap*, p->realBitmap)
DEF_ATTR_RD_SIMPLE(Plane, OX,        int,     p->realOX)
DEF_ATTR_RD_SIMPLE(Plane, OY,        int,     p->realOY)
DEF_ATTR_RD_SIMPLE(Plane, ZoomX,     float,   p->realZoomX)
DEF_ATTR_RD_SIMPLE(Plane, ZoomY,     float,   p->realZoomY)
DEF_ATTR_RD_SIMPLE(Plane, BlendType, int,     p->blendType)

DEF_ATTR_SIMPLE(Plane, Opacity,   int,     p->opacity)
DEF_ATTR_SIMPLE(Plane, Color,     Color&, *p->color)
DEF_ATTR_SIMPLE(Plane, Tone,      Tone&,  *p->tone)

Plane::~Plane()
{
	dispose();
}

void Plane::setBitmap(Bitmap *value)
{
	guardDisposed();

	if (p->bitmap != p->realBitmap)
		delete p->bitmap;

	p->bitmap = value;
	p->realBitmap = value;

	/* Tiling depends on the bitmap's dimensions: a replacement retiles even
	 * when no other property changes (and a nil bitmap clears stale quads). */
	p->quadSourceDirty = true;

	p->bitmapDispCon.disconnect();

	if (nullOrDisposed(value))
	{
		p->realBitmap = p->bitmap = 0;
		return;
	}

	p->bitmapDispCon = value->wasDisposed.connect(&PlanePrivate::bitmapDisposal, p);

	if (value->isMega())
	{
		p->bitmap = value->spawnChild();
		p->bitmap->getChildInfo()->wrap = true;
	}
}

void Plane::setOX(int value)
{
	guardDisposed();

	if (p->realOX == value)
	        return;

	p->realOX = value;
	p->quadSourceDirty = true;
}

void Plane::setOY(int value)
{
	guardDisposed();

	if (p->realOY == value)
	        return;

	p->realOY = value;
	p->quadSourceDirty = true;
}

void Plane::setZoomX(float value)
{
	guardDisposed();

	// RGSS hangs if you set this below 0
	value = std::max(value, 0.0f);

	if (p->realZoomX == value)
	        return;

	p->realZoomX = value;
	p->quadSourceDirty = true;
}

void Plane::setZoomY(float value)
{
	guardDisposed();

	// RGSS hangs if you set this below 0
	value = std::max(value, 0.0f);

	if (p->realZoomY == value)
	        return;

	p->realZoomY = value;
	p->quadSourceDirty = true;
}

void Plane::setBlendType(int value)
{
	guardDisposed();

	switch (value)
	{
	default :
	case BlendNormal :
		p->blendType = BlendNormal;
		return;
	case BlendAddition :
		p->blendType = BlendAddition;
		return;
	case BlendSubstraction :
		p->blendType = BlendSubstraction;
		return;
	}
}

void Plane::initDynAttribs()
{
	p->color = new Color;
	p->tone = new Tone;
}

void Plane::draw()
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	FrameProfile::OperationScope profileOperation(FrameProfile::PlaneDraw);
#endif
	if (nullOrDisposed(p->bitmap))
		return;

	if (!p->isVisible)
		return;

	ShaderBase *base;

	if (p->color->hasEffect() || p->tone->hasEffect() || p->opacity != 255)
	{
		PlaneShader &shader = shState->shaders().plane;

		shader.bind();
		shader.applyViewportProj();
		shader.setTone(p->tone->norm);
		shader.setColor(p->color->norm);
		shader.setFlash(Vec4());
		shader.setOpacity(p->opacity.norm);

		base = &shader;
	}
	else
	{
		SimpleShader &shader = shState->shaders().simple;

		shader.bind();
		shader.applyViewportProj();
		shader.setTranslation(Vec2i());

		base = &shader;
	}

	glState.blendMode.pushSet(p->blendType);

	p->bitmap->bindTex(*base);

	if (gl.npot_repeat)
		TEX::setRepeat(true);

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	if (p->quadTracePending && vita_glue_gpu_telemetry_enabled() && vita_measure_mode != 4)
	{
		const unsigned long long measuredStart = vita_measure_mode ? vita_measure_plane_begin() : 0;
		GLint wrapS = 0, wrapT = 0;
		/* vitaGL does not export glGetTexParameteriv: the wrap
		 * fields above stay 0 in its telemetry line. */
		char line[320];
		snprintf(line, sizeof(line),
		         "vita-plane: repeat=%d bitmap=%dx%d zoom=%.6g,%.6g offset=%.6g,%.6g "
		         "rect=%d,%d,%d,%d origin=%d,%d quads=%u wrap=%x,%x",
		         int(gl.npot_repeat), p->bitmap->width(), p->bitmap->height(),
		         p->zoomX, p->zoomY, p->ox, p->oy,
		         p->sceneGeo.rect.x, p->sceneGeo.rect.y, p->sceneGeo.rect.w, p->sceneGeo.rect.h,
		         p->sceneGeo.orig.x, p->sceneGeo.orig.y, unsigned(p->qArray.count()),
		         unsigned(wrapS), unsigned(wrapT));
		vita_glue_trace(line);
		if (vita_measure_mode) vita_measure_plane_end(measuredStart);
	}
#endif
	p->quadTracePending = false;

	p->qArray.draw();

	if (gl.npot_repeat)
		TEX::setRepeat(false);

	glState.blendMode.pop();
}

void Plane::onGeometryChange(const Scene::Geometry &geo)
{
	if (gl.npot_repeat)
		Quad::setPosRect(&p->qArray.vertices[0], FloatRect(geo.rect));

	p->sceneGeo = geo;
	p->quadSourceDirty = true;
}

void Plane::releaseResources()
{
	unlink();

	delete p;
}
