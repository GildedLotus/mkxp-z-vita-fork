/*
** viewportelement-binding.h
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

#ifndef VIEWPORTELEMENTBINDING_H
#define VIEWPORTELEMENTBINDING_H

#include "viewport.h"
#include "sharedstate.h"
#include "binding-util.h"
#include "binding-types.h"
#include "graphics.h"

#include "sceneelement-binding.h"
#include "disposable-binding.h"

template<class C>
RB_METHOD(viewportElementGetViewport)
{
	RB_UNUSED_PARAM;

	checkDisposed<C>(self);

	return rb_iv_get(self, "viewport");
}

template<class C>
RB_METHOD_GUARD(viewportElementSetViewport)
{
	RB_UNUSED_PARAM;

	ViewportElement *ve = getPrivateData<C>(self);

	VALUE viewportObj = Qnil;
	Viewport *viewport = 0;

	rb_get_args(argc, argv, "o", &viewportObj RB_ARG_END);

	if (!NIL_P(viewportObj))
		viewport = getPrivateDataCheck<Viewport>(viewportObj, ViewportType);

	GFX_GUARD_EXC( ve->setViewport(viewport); );

	rb_iv_set(self, "viewport", viewportObj);

	return viewportObj;
}
RB_METHOD_GUARD_END

/* Callers that pass 'viewportObjOut' get the element back before its
 * "viewport" ivar is set, so that they can hand it to the GC (setPrivateData)
 * first: until something owns it, a raise out of rb_iv_set orphans it. The
 * argument is optional so that callers which take ownership only later keep
 * the old order. */
template<class C>
static C *
viewportElementInitialize(int argc, VALUE *argv, VALUE self,
                          VALUE *viewportObjOut = 0)
{
	/* Get parameters */
	VALUE viewportObj = Qnil;
	Viewport *viewport = 0;

	rb_get_args(argc, argv, "|o", &viewportObj RB_ARG_END);

	if (!NIL_P(viewportObj))
	{
		viewport = getPrivateDataCheck<Viewport>(viewportObj, ViewportType);
	}

	/* Balance the graphics lock if construction throws -- whatever it throws.
	 * std::bad_alloc is not an Exception (util/exception.h) and GFX_LOCK is a
	 * live recursive kernel mutex, so an escape past a narrower guard pins it
	 * for the rest of the process. Ruby calls remain outside the lock because
	 * their exceptions use a non-C++ unwind: a raise is a longjmp, which runs
	 * no catch block, so a GFX_UNLOCK behind one never executes. */
	C *ve = 0;
	GFX_GUARD_ALL(ve = new C(viewport);)

	if (viewportObjOut)
	{
		*viewportObjOut = viewportObj;
		return ve;
	}

	/* Set property objects */
	rb_iv_set(self, "viewport", viewportObj);
	return ve;
}

template<class C>
void
viewportElementBindingInit(VALUE klass)
{
	sceneElementBindingInit<C>(klass);

	_rb_define_method(klass, "viewport", viewportElementGetViewport<C>);

    //if (rgssVer >= 2)
	//{
	_rb_define_method(klass, "viewport=", viewportElementSetViewport<C>);
	//}
}

#endif // VIEWPORTELEMENTBINDING_H
