/*
 ** viewport-binding.cpp
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

#include "binding-types.h"
#include "binding-util.h"
#include "disposable-binding.h"
#include "flashable-binding.h"
#include "sceneelement-binding.h"
#include "sharedstate.h"
#include "viewport.h"

#if RAPI_FULL > 187
DEF_TYPE(Viewport);
#else
DEF_ALLOCFUNC(Viewport);
#endif

RB_METHOD_GUARD(viewportInitialize) {
    Viewport *v = 0;

    if (argc == 0 && rgssVer >= 3) {
        GFX_GUARD_ALL(v = new Viewport();)
    } else if (argc == 1) {
        /* The rect arg is only used to init the viewport,
         * and does NOT replace its 'rect' property */
        VALUE rectObj;
        Rect *rect;

        rb_get_args(argc, argv, "o", &rectObj RB_ARG_END);

        rect = getPrivateDataCheck<Rect>(rectObj, RectType);

        GFX_GUARD_ALL(v = new Viewport(rect);)
    } else {
        int x, y, width, height;

        rb_get_args(argc, argv, "iiii", &x, &y, &width, &height RB_ARG_END);
        GFX_GUARD_ALL(v = new Viewport(x, y, width, height);)
    }

    /* The GC owns v from here, before the first Ruby call that can raise:
     * a raise is a longjmp, so nothing below would ever delete it. */
    setPrivateData(self, v);

    GFX_GUARD_ALL(v->initDynAttribs();)

    /* Wrap property objects. None of this touches GL, so none of it runs
     * under the lock: a raise here would never reach a GFX_UNLOCK. */
    wrapProperty(self, &v->getRect(), "rect", RectType);
    wrapProperty(self, &v->getColor(), "color", ColorType);
    wrapProperty(self, &v->getTone(), "tone", ToneType);

    return self;
}
RB_METHOD_GUARD_END

DEF_GFX_PROP_OBJ_VAL(Viewport, Rect, Rect, "rect")
DEF_GFX_PROP_OBJ_VAL(Viewport, Color, Color, "color")
DEF_GFX_PROP_OBJ_VAL(Viewport, Tone, Tone, "tone")

DEF_GFX_PROP_I(Viewport, OX)
DEF_GFX_PROP_I(Viewport, OY)

void viewportBindingInit() {
    VALUE klass = rb_define_class("Viewport", rb_cObject);
#if RAPI_FULL > 187
    rb_define_alloc_func(klass, classAllocate<&ViewportType>);
#else
    rb_define_alloc_func(klass, ViewportAllocate);
#endif
    
    disposableBindingInit<Viewport>(klass);
    flashableBindingInit<Viewport>(klass);
    sceneElementBindingInit<Viewport>(klass);
    
    _rb_define_method(klass, "initialize", viewportInitialize);
    
    INIT_PROP_BIND(Viewport, Rect, "rect");
    INIT_PROP_BIND(Viewport, OX, "ox");
    INIT_PROP_BIND(Viewport, OY, "oy");
    INIT_PROP_BIND(Viewport, Color, "color");
    INIT_PROP_BIND(Viewport, Tone, "tone");
}
