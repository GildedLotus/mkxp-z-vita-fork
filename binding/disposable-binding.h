/*
** disposable-binding.h
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

#ifndef DISPOSABLEBINDING_H
#define DISPOSABLEBINDING_H

#include "disposable.h"
#include "binding-util.h"
#include "graphics.h"

/* Every .dispose in every game arrives here. Disposable::dispose
 * (src/util/disposable.h) takes GFX_LOCK, releases the object's resources and
 * then emits the wasDisposed signal, whose slots do arbitrary work including
 * allocation; an earlier fix made it release the lock for every C++ type rather
 * than for Exception alone, and this is the other half of that fix -- the frame
 * that has to turn what dispose() rethrows into a Ruby exception instead of
 * letting it reach Ruby's C frames, where ARM EABI has no unwind information
 * and std::terminate() runs. */
template<class C>
RB_METHOD_GUARD(disposableDispose)
{
	RB_UNUSED_PARAM;
	
	C *d = getPrivateDataNoRaise<C>(self);

	if (!d)
		return Qnil;

	d->dispose();

	return Qnil;
}
RB_METHOD_GUARD_END

template<class C>
RB_METHOD(disposableIsDisposed)
{
	RB_UNUSED_PARAM;

	C *d = getPrivateDataNoRaise<C>(self);

	if (!d)
		return Qtrue;

	return rb_bool_new(d->isDisposed());
}

template<class C>
static void disposableBindingInit(VALUE klass)
{
	_rb_define_method(klass, "dispose", disposableDispose<C>);
	_rb_define_method(klass, "disposed?", disposableIsDisposed<C>);
}

template<class C>
inline void
checkDisposed(VALUE self)
{
	if (disposableIsDisposed<C>(0, 0, self) == Qtrue)
		raiseDisposedAccess(self);
}

#endif // DISPOSABLEBINDING_H
