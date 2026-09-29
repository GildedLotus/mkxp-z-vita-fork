/*
** quad.h
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

#ifndef QUAD_H
#define QUAD_H

#include "config.h"
#include "graphics.h"
#include "vertex.h"
#include "gl-util.h"
#include "gl-meta.h"
#include "sharedstate.h"
#include "global-ibo.h"
#include "shader.h"

#ifdef MKXPZ_SOFTWARE_BITMAPS
#include <stdint.h>
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "vita_glue.h"
#include <stdio.h>
#endif

/* ==========================================================================
 * Quad geometry comes from CLIENT MEMORY, not from a per-Quad VBO
 *
 * Stock mkxp-z gives every Quad -- every Sprite, both halves of every Window
 * and WindowVX, the two in ScreenScene, the one in GraphicsPrivate and the
 * global gpQuad -- its own GL_ARRAY_BUFFER holding four vertices, 128 bytes.
 * On the Vita that has two costs:
 *
 *   1. Every buffer object takes a slot from the fixed GPU object pools that
 *      render surfaces also come from. Forty sprites and five windows spend
 *      fifty of them on 6 KiB of vertices, and a Sprite created mid-game is a
 *      post-seal allocation, which is exactly what the seal exists to notice.
 *
 *   2. Re-uploading a Quad's buffer after it was drawn in the current frame
 *      can force a mid-frame flush and wait. Every Quad is one buffer, so
 *      that is per Quad, per change, every frame.
 *
 * A client-side array costs neither. With nothing bound to GL_ARRAY_BUFFER,
 * glVertexAttribPointer takes a real pointer and the driver copies the four
 * vertices into its own vertex storage inside the draw call. No object is
 * created, and nothing is left for a later glDeleteBuffers to free underneath
 * a frame the GPU is still reading.
 *
 * The INDICES stay in the global IBO: an element buffer stays bound, and
 * client vertices plus an index buffer object is a supported path. QuadArray
 * and GlobalIBO keep their buffers: a tilemap uploads thousands of vertices
 * once and draws them many times, which is what a VBO is for.
 *
 * Attribute enables are strictly scoped to the draw. An attribute left
 * enabled after this Quad is gone would point the next draw at freed client
 * memory, and an attribute the next draw does not want must fall back to its
 * current-state value. So bindArrays()/unbindArrays() are symmetric and
 * leave GL exactly as GLMeta::vaoUnbind() would: array buffer 0, element
 * buffer 0, every one of this vertex format's attributes disabled.
 * ========================================================================== */

struct Quad
{
	Vertex vert[4];
#ifdef MKXPZ_SOFTWARE_BITMAPS
	/* The index buffer only; the vertices never leave this object. Cached at
	 * construction exactly as the stock VAO cached it -- SharedState creates
	 * the global IBO and sizes it for one quad before the first Quad exists
	 * (sharedstate.cpp initInstance). */
	IBO::ID ibo;
#else
	VBO::ID vbo;
	GLMeta::VAO vao;
	bool vboDirty;
#endif

	template<typename V>
	static void setPosRect(V *vert, const FloatRect &r)
	{
		int i = 0;
		vert[i++].pos = r.topLeft();
		vert[i++].pos = r.topRight();
		vert[i++].pos = r.bottomRight();
		vert[i++].pos = r.bottomLeft();
	}

	template<typename V>
	static void setTexRect(V *vert, const FloatRect &r)
	{
		int i = 0;
		vert[i++].texPos = r.topLeft();
		vert[i++].texPos = r.topRight();
		vert[i++].texPos = r.bottomRight();
		vert[i++].texPos = r.bottomLeft();
	}

	template<typename V>
	static int setTexPosRect(V *vert, const FloatRect &tex, const FloatRect &pos)
	{
		setPosRect(vert, pos);
		setTexRect(vert, tex);

		return 1;
	}

	template<typename V>
	static void setColor(V *vert, const Vec4 &c)
	{
		for (int i = 0; i < 4; ++i)
			vert[i].color = c;
	}

#ifdef MKXPZ_SOFTWARE_BITMAPS
	Quad()
	    : ibo(shState->globalIBO().ibo)
	{
		setColor(Vec4(1, 1, 1, 1));
	}

	/* No GL object is owned, so there is nothing to give back. */
	~Quad()
	{}
#else
	Quad()
	    : vbo(VBO::gen()),
	      vboDirty(true)
	{
		GLMeta::vaoFillInVertexData<Vertex>(vao);
		vao.vbo = vbo;
		vao.ibo = shState->globalIBO().ibo;

		GLMeta::vaoInit(vao, true);
		VBO::allocEmpty(sizeof(Vertex[4]), GL_DYNAMIC_DRAW);
		GLMeta::vaoUnbind(vao);

		setColor(Vec4(1, 1, 1, 1));
	}

	~Quad()
	{
		GLMeta::vaoFini(vao);
		VBO::del(vbo);
	}

	void updateBuffer()
	{
		VBO::bind(vbo);
		VBO::uploadSubData(0, sizeof(Vertex[4]), vert);
		VBO::unbind();
	}
#endif

	void setPosRect(const FloatRect &r)
	{
		setPosRect(vert, r);
		markDirty();
	}

	void setTexRect(const FloatRect &r)
	{
		setTexRect(vert, r);
		markDirty();
	}

	void setTexPosRect(const FloatRect &tex, const FloatRect &pos)
	{
		setTexPosRect(vert, tex, pos);
		markDirty();
	}

	void setColor(const Vec4 &c)
	{
		for (int i = 0; i < 4; ++i)
			vert[i].color = c;

		markDirty();
	}

#ifdef MKXPZ_SOFTWARE_BITMAPS
	/* Nothing is staged anywhere, so nothing can be stale: the draw reads
	 * vert[] directly. Kept as a no-op so the setters above are the same
	 * source on both backends. */
	void markDirty()
	{}

	/* GL_ARRAY_BUFFER MUST be 0 here: with a buffer bound, the last argument
	 * of glVertexAttribPointer is an offset into that buffer instead of a
	 * pointer, and the driver would sample whatever the previous QuadArray
	 * left behind. VertexAttribute::offset is a byte offset produced by
	 * offsetof() in vertex.cpp, so the client pointer is the vertex base plus
	 * that offset. */
	void bindArrays()
	{
		VBO::unbind();
		IBO::bind(ibo);

		const VertexAttribute *attr = VertexTraits<Vertex>::attr;
		const GLsizei attrCount = VertexTraits<Vertex>::attrCount;
		const char *base = (const char*) vert;

		for (GLsizei i = 0; i < attrCount; ++i)
		{
			const VertexAttribute &va = attr[i];

			gl.EnableVertexAttribArray(va.index);
			gl.VertexAttribPointer(va.index, va.size, va.type, GL_FALSE,
			                       (GLsizei) sizeof(Vertex),
			                       base + (uintptr_t) va.offset);
		}
	}

	void unbindArrays()
	{
		const VertexAttribute *attr = VertexTraits<Vertex>::attr;
		const GLsizei attrCount = VertexTraits<Vertex>::attrCount;

		for (GLsizei i = 0; i < attrCount; ++i)
			gl.DisableVertexAttribArray(attr[i].index);

		IBO::unbind();
	}

	void draw()
	{
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
		{
			char tb[160];
			snprintf(tb, sizeof(tb),
			         "trace: Quad::draw vbo=client ibo=%u",
			         ibo.gl);
			vita_glue_trace(tb);
		}
#endif
		bindArrays();
		gl.DrawElements(GL_TRIANGLES, 6, _GL_INDEX_TYPE, 0);
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
		vitaGlTraceErr("Quad::draw DrawElements");
#endif
		unbindArrays();
	}
#else
	void markDirty()
	{
		vboDirty = true;
	}

	void draw()
	{
		if (vboDirty)
		{
			updateBuffer();
			vboDirty = false;
		}

#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
		{
			char tb[160];
			snprintf(tb, sizeof(tb),
			         "trace: Quad::draw vbo=%u ibo=%u",
			         vbo.gl, vao.ibo.gl);
			vita_glue_trace(tb);
		}
#endif
		GLMeta::vaoBind(vao);
		gl.DrawElements(GL_TRIANGLES, 6, _GL_INDEX_TYPE, 0);
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
		vitaGlTraceErr("Quad::draw DrawElements");
#endif
		GLMeta::vaoUnbind(vao);
	}
#endif
};

#endif // QUAD_H
