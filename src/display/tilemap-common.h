/*
** tilemap-common.h
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

#ifndef TILEMAPCOMMON_H
#define TILEMAPCOMMON_H

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "frameprofile.h"
#endif
#include "table.h"
#include "gl-util.h"
#include "gl-meta.h"
#include "sharedstate.h"
#include "global-ibo.h"
#include "glstate.h"
#include "shader.h"
#include "vertex.h"
#include "quad.h"
#include "quadarray.h"
#include "etc-internal.h"

#include <stdint.h>
#include <assert.h>
#include <vector>

#include "sigslot/signal.hpp"

#ifdef MKXPZ_SOFTWARE_BITMAPS
#include <string.h>
#include <SDL_surface.h>

/* CPU tile atlas.
 *
 * Render surfaces are a small fixed budget per process, first come first
 * served. A tile
 * atlas is created on the first map load -- the latest possible moment, when
 * the pool is emptiest -- so it cannot be one. With CPU-authoritative Bitmaps
 * every atlas source already has its pixels in client memory, so
 * the atlas is assembled here with row memcpys and handed to the driver as a
 * single WHOLE-LEVEL upload. Never a sub-rect: a sub-rect update of a live
 * texture stalls 16 ms on this driver, a whole level costs 1 ms + 13 ns/px.
 *
 * The atlas dimensions and every blit rectangle are the stock ones; only the
 * machinery that moves the pixels changes, so the vertex/texcoord code that
 * reads the atlas is untouched. */
struct SoftAtlas
{
	/* RGBA8888, tightly packed (pitch == w*4), row 0 on top. Zero-filled on
	 * construction, which is the transparent-black FBO::clear() that both
	 * stock atlas builders open with. */
	std::vector<uint8_t> px;
	int w, h;

	SoftAtlas(int width, int height)
	    : px((size_t)(width > 0 ? width : 0) * (size_t)(height > 0 ? height : 0) * 4, 0),
	      w(width > 0 ? width : 0),
	      h(height > 0 ? height : 0)
	{}
};

/* One 1:1 REPLACE copy: the CPU twin of
 *     GLMeta::blitSource(src); GLMeta::blitRectangle(srcRect, dstPos);
 * Both stock builders blit with blending disabled (gl-meta.cpp, blitRectangle
 * does glState.blend.pushSet(false)), so the destination is overwritten,
 * alpha included -- not composited.
 *
 * srcRect is clipped to the source surface and dstPos moves with it; the
 * result is then clipped to the atlas. Sampling outside the source is the one
 * place this differs from the GL path, which would clamp to the edge texel;
 * clipping is what every caller here actually means, and no shipped tileset
 * reaches the difference (every rect is derived from the source's own size).
 */
static inline void
softAtlasBlit(SoftAtlas &dst, const SDL_Surface *src,
              IntRect s, Vec2i d)
{
	if (!src || s.w <= 0 || s.h <= 0)
		return;

	assert(src->format->BytesPerPixel == 4);

	/* Clip against the source; the copy is 1:1, so the destination origin
	 * moves by exactly what the source origin lost. */
	if (s.x < 0) { d.x -= s.x; s.w += s.x; s.x = 0; }
	if (s.y < 0) { d.y -= s.y; s.h += s.y; s.y = 0; }
	if (s.x + s.w > src->w) s.w = src->w - s.x;
	if (s.y + s.h > src->h) s.h = src->h - s.y;

	/* Clip against the atlas. */
	if (d.x < 0) { s.x -= d.x; s.w += d.x; d.x = 0; }
	if (d.y < 0) { s.y -= d.y; s.h += d.y; d.y = 0; }
	if (d.x + s.w > dst.w) s.w = dst.w - d.x;
	if (d.y + s.h > dst.h) s.h = dst.h - d.y;

	if (s.w <= 0 || s.h <= 0)
		return;

	const uint8_t *sp = (const uint8_t *)src->pixels
	                  + (size_t)s.y * (size_t)src->pitch + (size_t)s.x * 4;
	uint8_t *dp = &dst.px[((size_t)d.y * (size_t)dst.w + (size_t)d.x) * 4];
	const size_t bytes = (size_t)s.w * 4;

	for (int y = 0; y < s.h; ++y)
	{
		memcpy(dp, sp, bytes);
		sp += src->pitch;
		dp += (size_t)dst.w * 4;
	}
}

/* Hand a finished CPU atlas to the driver: one whole-level TexImage2D, and
 * the texture binding is left exactly as it was found, because both builders
 * run from prepareDraw, in the middle of everybody else's GL state. */
static inline void
softAtlasUpload(const SoftAtlas &atlas, TEX::ID tex)
{
	if (atlas.w <= 0 || atlas.h <= 0)
		return;

	TEX::ScopedBinding binding;
	if (tex == TEX::ID(0))
		throw TEX::UploadError();
	TEX::bind(tex);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	FrameProfile::Scope profileUpload(FrameProfile::Upload, 0, false, false);
#endif
	if (!TEX::uploadImageChecked(atlas.w, atlas.h, atlas.px.data(), GL_RGBA))
		throw TEX::UploadError();
}
#endif /* MKXPZ_SOFTWARE_BITMAPS */

static inline int
wrap(int value, int range)
{
	int res = value % range;
	return res < 0 ? res + range : res;
}

static inline Vec2i
wrap(const Vec2i &value, int range)
{
	return Vec2i(wrap(value.x, range),
	             wrap(value.y, range));
}

static inline int16_t
tableGetWrapped(const Table &t, int x, int y, int z = 0)
{
	return t.get(wrap(x, t.xSize()),
	             wrap(y, t.ySize()),
	             z);
}

/* Calculate the tile x/y on which this pixel x/y lies */
static inline Vec2i
getTilePos(const Vec2i &pixelPos)
{
	/* Round the pixel position down to the nearest top left
	 * tile boundary, by masking off the lower 5 bits (2^5 = 32).
	 * Then divide by 32 to convert into tile units. */
	return (pixelPos & ~(32-1)) / 32;
}

enum AtSubPos
{
	TopLeft          = 0,
	TopRight         = 1,
	BottomLeft       = 2,
	BottomRight      = 3,
	BottomLeftTable  = 4,
	BottomRightTable = 5
};

static inline void
atSelectSubPos(FloatRect &pos, int i)
{
	switch (i)
	{
	case TopLeft:
		return;
	case TopRight:
		pos.x += 16;
		return;
	case BottomLeft:
		pos.y += 16;
		return;
	case BottomRight:
		pos.x += 16;
		pos.y += 16;
		return;
	case BottomLeftTable:
		pos.y += 24;
		return;
	case BottomRightTable:
		pos.x += 16;
		pos.y += 24;
		return;
	default:
		assert(!"Unreachable");
	}
}

struct FlashMap
{
	FlashMap()
		: dirty(false),
	      data(0),
	      allocQuads(0)
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		vao.vbo = VBO::ID(0);
#else
		vao.vbo = VBO::gen();
#endif
		vao.ibo = shState->globalIBO().ibo;
		GLMeta::vaoFillInVertexData<CVertex>(vao);

		GLMeta::vaoInit(vao);
	}

	~FlashMap()
	{
		GLMeta::vaoFini(vao);
		VBO::del(vao.vbo);
		dataCon.disconnect();
	}

	Table *getData() const
	{
		return data;
	}

	void setData(Table *value)
	{
		if (data == value)
			return;

		data = value;
		dataCon.disconnect();
		dirty = true;

		if (!data)
			return;

        
        
        dataCon = data->modified.connect(&FlashMap::setDirty, this);
	}

	void setViewport(const IntRect &value)
	{
		viewp = value;
		dirty = true;
	}

	void prepare()
	{
		if (!dirty)
			return;

		rebuildBuffer();
		dirty = false;
	}

	void draw(float alpha, const Vec2i &trans)
	{
		const size_t count = quadCount();

		if (count == 0)
			return;

		GLMeta::vaoBind(vao);
		glState.blendMode.pushSet(BlendAddition);

		FlashMapShader &shader = shState->shaders().flashMap;
		shader.bind();
		shader.applyViewportProj();
		shader.setAlpha(alpha);
		shader.setTranslation(trans);

		gl.DrawElements(GL_TRIANGLES, count * 6, _GL_INDEX_TYPE, 0);

		glState.blendMode.pop();

		GLMeta::vaoUnbind(vao);
	}

private:
	void setDirty()
	{
		dirty = true;
	}

	size_t quadCount() const
	{
		return vertices.size() / 4;
	}

	bool sampleFlashColor(Vec4 &out, int x, int y) const
	{
		int16_t packed = tableGetWrapped(*data, x, y);

		if (packed == 0)
			return false;

		const float max = 0xF;

		float b = ((packed & 0x000F) >> 0) / max;
		float g = ((packed & 0x00F0) >> 4) / max;
		float r = ((packed & 0x0F00) >> 8) / max;

		out = Vec4(r, g, b, 1);

		return true;
	}

	void rebuildBuffer()
	{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		FrameProfile::OperationScope profileOperation(FrameProfile::Flash);
#endif
		vertices.clear();

		if (!data)
		{
#ifdef MKXPZ_SOFTWARE_BITMAPS
			replaceTileQuadBuffer(vao.vbo, vertices);
#endif
			return;
		}

		for (int x = 0; x < viewp.w; ++x)
			for (int y = 0; y < viewp.h; ++y)
			{
				Vec4 color;

				if (!sampleFlashColor(color, x+viewp.x, y+viewp.y))
					continue;

				FloatRect posRect(x*32, y*32, 32, 32);

				CVertex v[4];
				Quad::setPosRect(v, posRect);
				Quad::setColor(v, color);

				for (size_t i = 0; i < 4; ++i)
					vertices.push_back(v[i]);
			}

#ifdef MKXPZ_SOFTWARE_BITMAPS
		replaceTileQuadBuffer(vao.vbo, vertices);
#else
		if (vertices.size() == 0)
			return;

		VBO::bind(vao.vbo);

		if (quadCount() > allocQuads)
		{
			allocQuads = quadCount();
			VBO::allocEmpty(sizeof(CVertex) * vertices.size());
		}

		VBO::uploadSubData(0, sizeof(CVertex) * vertices.size(), dataPtr(vertices));

		VBO::unbind();

		/* Ensure global IBO size */
		shState->ensureQuadIBO(quadCount());
#endif
	}

	bool dirty;

	Table *data;
	sigslot::connection dataCon;

	IntRect viewp;

	GLMeta::VAO vao;
	size_t allocQuads;
	std::vector<CVertex> vertices;
};

#endif // TILEMAPCOMMON_H
