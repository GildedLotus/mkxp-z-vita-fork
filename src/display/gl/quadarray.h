/*
** quadarray.h
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

#ifndef QUADARRAY_H
#define QUADARRAY_H

#include "vertex.h"
#include "gl-util.h"
#include "gl-meta.h"
#include "sharedstate.h"
#include "global-ibo.h"
#include "shader.h"
#include "exception.h"

#include <vector>
#include <stdint.h>

#ifdef MKXPZ_SOFTWARE_BITMAPS
/* The driver copies client attributes during DrawElements; unlike rewriting
 * a live VBO this never waits on the buffer's last use.
 * Rebase each chunk onto index zero: at most 32 KiB (Vertex) of the driver's
 * vertex storage per draw, even for a large tiled Plane.
 * Keep the global IBO and the order of every quad/triangle unchanged.
 * One chunk is also the whole index reservation an array needs: commit()
 * reserves QuadClientChunk indices, never the array's, because no draw ever
 * addresses a later index range. */
enum { QuadClientChunk = 256 };
template<class VertexType>
inline void drawQuadVertices(const VertexType *vertices, size_t offset, size_t count)
{
	if (!count)
		return;

	VBO::unbind();
	IBO::bind(shState->globalIBO().ibo);
	const VertexAttribute *attr = VertexTraits<VertexType>::attr;
	const GLsizei attrCount = VertexTraits<VertexType>::attrCount;
	for (GLsizei i = 0; i < attrCount; ++i)
		gl.EnableVertexAttribArray(attr[i].index);

	while (count)
	{
		const size_t chunk = count < QuadClientChunk ? count : QuadClientChunk;
		const char *base = (const char *)(vertices + offset * 4);
		for (GLsizei i = 0; i < attrCount; ++i)
		{
			const VertexAttribute &va = attr[i];
			gl.VertexAttribPointer(va.index, va.size, va.type, GL_FALSE,
			                       sizeof(VertexType), base + (uintptr_t)va.offset);
		}
		gl.DrawElements(GL_TRIANGLES, chunk * 6, _GL_INDEX_TYPE, 0);
		offset += chunk;
		count -= chunk;
	}

	for (GLsizei i = 0; i < attrCount; ++i)
		gl.DisableVertexAttribArray(attr[i].index);
	IBO::unbind();
}
#endif

#ifdef MKXPZ_SOFTWARE_BITMAPS
/* Never re-specify a drawn VBO. Keep the old name until every new
 * segment is uploaded; VBO::del retains its storage for two real swaps. */
template<class VertexType>
inline void replaceTileQuadBuffer(VBO::ID &current,
                                  const std::vector<VertexType> &ground,
                                  const std::vector<VertexType> *layers = 0,
                                  size_t layerCount = 0)
{
	const size_t maxVertices = ((INDEX_T_MAX - 1) / 6) * 4;
	size_t count = 0;
	for (size_t i = 0; i <= layerCount; ++i)
	{
		const size_t n = (i ? layers[i - 1] : ground).size();
		if (n % 4 || n > maxVertices - count)
			throw Exception(Exception::MKXPError, "Tilemap: vertex count exceeds index buffer");
		count += n;
	}
	shState->ensureQuadIBO(count / 4);
	if (!count)
	{
		VBO::del(current);
		current = VBO::ID(0);
		return;
	}
	if (gl.GetError() != GL_NO_ERROR)
		throw Exception(Exception::MKXPError, "Tilemap: GL error before VBO allocation");
	const VBO::ID fresh = VBO::gen();
	try
	{
		if (!fresh.gl)
			throw Exception(Exception::MKXPError, "Tilemap: no VBO name");
		VBO::bind(fresh);
		VBO::allocEmpty(count * sizeof(VertexType), GL_STATIC_DRAW);
		if (gl.GetError() != GL_NO_ERROR)
			throw Exception(Exception::MKXPError, "Tilemap: VBO allocation failed");
		size_t offset = 0;
		for (size_t i = 0; i <= layerCount; ++i)
		{
			const std::vector<VertexType> &v = i ? layers[i - 1] : ground;
			const size_t bytes = v.size() * sizeof(VertexType);
			if (bytes)
				VBO::uploadSubData(offset, bytes, &v[0]);
			offset += bytes;
		}
		if (gl.GetError() != GL_NO_ERROR)
			throw Exception(Exception::MKXPError, "Tilemap: VBO upload failed");
		VBO::unbind();
	}
	catch (...)
	{
		VBO::unbind();
		VBO::del(fresh);
		throw;
	}
	VBO::del(current);
	current = fresh;
}
#endif

/* A 32-bit size_t wraps `size * 4` for a large count and hands the geometry
 * builders a tiny vector to write past; no array here needs more than this. */
enum { QuadArrayMaxQuads = 1 << 16 };

template<class VertexType>
struct QuadArray
{
	std::vector<VertexType> vertices;

#ifndef MKXPZ_SOFTWARE_BITMAPS
	VBO::ID vbo;
	GLMeta::VAO vao;
#endif

	size_t quadCount;
	GLsizeiptr vboSize;

	QuadArray()
	    : quadCount(0),
	      vboSize(-1)
	{
#ifndef MKXPZ_SOFTWARE_BITMAPS
		vbo = VBO::gen();

		GLMeta::vaoFillInVertexData<VertexType>(vao);
		vao.vbo = vbo;
		vao.ibo = shState->globalIBO().ibo;

		GLMeta::vaoInit(vao);
#endif
	}

	~QuadArray()
	{
#ifndef MKXPZ_SOFTWARE_BITMAPS
		GLMeta::vaoFini(vao);
		VBO::del(vbo);
#endif
	}

	void resize(size_t size)
	{
		if (size > QuadArrayMaxQuads)
			throw Exception(Exception::MKXPError, "Quad array too large");

		vertices.resize(size * 4);
		quadCount = size;
	}

	void clear()
	{
		vertices.clear();
		quadCount = 0;
	}

	/* This needs to be called after the final 'append()' call
	 * and previous to the first 'draw()' call. */
	void commit()
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* A tiled Plane is many chunks, not one index range: reserve only the
		 * chunk the draw will actually address. Reserving quadCount here
		 * aborts a 4x4 Plane over 544x416 (14144 quads) in the IBO assert. */
		shState->ensureQuadIBO(quadCount < QuadClientChunk ? quadCount : QuadClientChunk);
#else
		VBO::bind(vbo);

		GLsizeiptr size = vertices.size() * sizeof(VertexType);

		if (size > vboSize)
		{
			/* New data exceeds already allocated size.
			 * Reallocate VBO. */
			VBO::uploadData(size, dataPtr(vertices), GL_DYNAMIC_DRAW);
			vboSize = size;

			shState->ensureQuadIBO(quadCount);
		}
		else
		{
			/* New data fits in allocated size */
			VBO::uploadSubData(0, size, dataPtr(vertices));
		}

		VBO::unbind();
#endif
	}

	void draw(size_t offset, size_t count)
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		drawQuadVertices(dataPtr(vertices), offset, count);
#else
		GLMeta::vaoBind(vao);

		const char *_offset = (const char*) 0 + offset * 6 * sizeof(index_t);
		gl.DrawElements(GL_TRIANGLES, count * 6, _GL_INDEX_TYPE, _offset);

		GLMeta::vaoUnbind(vao);
#endif
	}

	void draw()
	{
		draw(0, quadCount);
	}

	size_t count() const
	{
		return quadCount;
	}
};

typedef QuadArray<Vertex> ColorQuadArray;
typedef QuadArray<SVertex> SimpleQuadArray;

#endif // QUADARRAY_H
