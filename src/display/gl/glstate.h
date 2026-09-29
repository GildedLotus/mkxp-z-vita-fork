/*
** glstate.h
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

#ifndef GLSTATE_H
#define GLSTATE_H

#include "etc.h"
#ifdef MKXPZ_SOFTWARE_BITMAPS
#include "gl-fun.h"
#endif

#include <stack>
#include <assert.h>

struct Config;

template<typename T>
struct GLProperty
{
	~GLProperty()
	{
		assert(stack.size() == 0);
	}

	/* Save values and stack depth without allocating, including legacy pushes
	 * made by nested viewport/window draws that can throw. */
	class Guard
	{
		GLProperty &property;
		T value;
		size_t depth;
		bool active = true, refresh;
	public:
		explicit Guard(GLProperty &p, bool forceRefresh = false) : property(p),
			value(p.current), depth(p.stack.size()), refresh(forceRefresh) {}
		Guard(const Guard &) = delete;
		Guard &operator=(const Guard &) = delete;
		~Guard() { restore(); }
		void release() { active = false; }
		void restore()
		{
			if (!active) return;
			while (property.stack.size() > depth) property.stack.pop();
			if (refresh) property.init(value); // Raw blend calls bypass the cache.
			else property.set(value);
			active = false;
		}
	};

	void init(const T &value)
	{
		current = value;
		apply(value);
	}

	void push() { stack.push(current); }
	void pop()  { set(stack.top()); stack.pop(); }
	const T &get()    { return current; }
	void set(const T &value)
	{
		if (value == current)
			return;

		init(value);
	}

	void pushSet(const T &value)
	{
		push();
		set(value);
	}

	void refresh()
	{
		apply(current);
	}
private:
	virtual void apply(const T &value) = 0;

	T current;
	std::stack<T> stack;
};


class GLClearColor : public GLProperty<Vec4>
{
	void apply(const Vec4 &);
};

class GLScissorBox : public GLProperty<IntRect>
{
public:
	/* Sets the intersection of the current box with value */
	void setIntersect(const IntRect &value);

private:
	void apply(const IntRect &value);
};

class GLScissorTest : public GLProperty<bool>
{
	void apply(const bool &value);
};

class GLBlendMode : public GLProperty<BlendType>
{
	void apply(const BlendType &value);
};

class GLBlend : public GLProperty<bool>
{
	void apply(const bool &value);
};

class GLViewport : public GLProperty<IntRect>
{
	void apply(const IntRect &value);
};

class GLProgram : public GLProperty<unsigned int> /* GLuint */
{
	void apply(const unsigned int &value);
};


class GLState
{
public:
	GLClearColor clearColor;
	GLScissorBox scissorBox;
	GLScissorTest scissorTest;
	GLBlendMode blendMode;
	GLBlend blend;
	GLViewport viewport;
	GLProgram program;

	struct Caps
	{
		int maxTexSize;

		Caps();

	} caps;

	GLState(const Config &conf);
};

/* Release on success to keep the renderer's normal final state. */
class GLStateGuard
{
	GLProperty<Vec4>::Guard clearColor;
	GLProperty<IntRect>::Guard scissorBox, viewport;
	GLProperty<bool>::Guard scissorTest, blend;
	GLProperty<BlendType>::Guard blendMode;
	GLProperty<unsigned int>::Guard program;
public:
	explicit GLStateGuard(GLState &s) : clearColor(s.clearColor),
		scissorBox(s.scissorBox), viewport(s.viewport), scissorTest(s.scissorTest),
		blend(s.blend), blendMode(s.blendMode, true), program(s.program) {}
	void restore()
	{
		program.restore(); blendMode.restore(); blend.restore(); scissorTest.restore();
		viewport.restore(); scissorBox.restore(); clearColor.restore();
	}
	void release()
	{
		clearColor.release(); scissorBox.release(); viewport.release();
		scissorTest.release(); blend.release(); blendMode.release(); program.release();
	}
};

#ifdef MKXPZ_SOFTWARE_BITMAPS
/* The RGSS thread owns this scope and the GL dispatch table. Remember errors
 * even when upload checks or tracing consume them during a composition. */
class GLRenderErrorScope
{
	static GLRenderErrorScope *active;
	GLRenderErrorScope *outer;
	_PFNGLGETERRORPROC previous, source;
	GLenum error;
public:
	GLRenderErrorScope();
	~GLRenderErrorScope();
	GLRenderErrorScope(const GLRenderErrorScope &) = delete;
	GLRenderErrorScope &operator=(const GLRenderErrorScope &) = delete;
	void check(const char *operation);
};
#endif

#endif // GLSTATE_H
