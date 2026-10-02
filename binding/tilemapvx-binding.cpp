/*
 ** tilemapvx-binding.cpp
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

#include "bitmap.h"
#include "sharedstate.h"
#include "table.h"
#include "tilemapvx.h"
#include "viewport.h"

#include "binding-types.h"
#include "binding-util.h"
#include "disposable-binding.h"

DEF_TYPE_CUSTOMNAME(TilemapVX, "Tilemap");

DEF_TYPE_CUSTOMFREE(BitmapArray, RUBY_TYPED_NEVER_FREE);

RB_METHOD_GUARD(tilemapVXInitialize) {
    TilemapVX *t = 0;

    /* Get parameters */
    VALUE viewportObj = Qnil;
    Viewport *viewport = 0;

    rb_get_args(argc, argv, "|o", &viewportObj RB_ARG_END);

    if (!NIL_P(viewportObj))
        viewport = getPrivateDataCheck<Viewport>(viewportObj, ViewportType);

    /* Construct object */
    GFX_GUARD_ALL(t = new TilemapVX(viewport);)

    /* The GC owns t from here, before the first Ruby call that can raise:
     * a raise is a longjmp, so nothing below would ever delete it. */
    setPrivateData(self, t);

    /* Nothing below touches GL, so none of it runs under the lock: a Ruby
     * raise is a longjmp and would never reach a GFX_UNLOCK. */
    rb_iv_set(self, "viewport", viewportObj);

    /* Dispose the old bitmap array if we're reinitializing.
     * See the comment in setPrivateData for more info. */
    VALUE autotilesObj = rb_iv_get(self, "bitmap_array");
    if (autotilesObj != Qnil)
        setPrivateData(autotilesObj, 0);
    
    wrapProperty(self, &t->getBitmapArray(), "bitmap_array", BitmapArrayType,
                 rb_const_get(rb_cObject, rb_intern("Tilemap")));
    
    autotilesObj = rb_iv_get(self, "bitmap_array");
    
    VALUE ary = rb_ary_new2(9);
    for (int i = 0; i < 9; ++i)
        rb_ary_push(ary, Qnil);
    
    rb_iv_set(autotilesObj, "array", ary);
    
    /* Circular reference so both objects are always
     * alive at the same time */
    rb_iv_set(autotilesObj, "tilemap", self);

    return self;
}
RB_METHOD_GUARD_END

RB_METHOD(tilemapVXGetBitmapArray) {
    RB_UNUSED_PARAM;
    
    return rb_iv_get(self, "bitmap_array");
}

RB_METHOD_GUARD(tilemapVXUpdate) {
    RB_UNUSED_PARAM;

    TilemapVX *t = getPrivateData<TilemapVX>(self);

    /* update() throws Exception from guardDisposed. No lock: this method
     * never took one, and a Ruby raise cannot run under GFX_LOCK. */
    t->update();

    return Qnil;
}
RB_METHOD_GUARD_END

DEF_GFX_PROP_OBJ_REF(TilemapVX, Viewport, Viewport, "viewport")
DEF_GFX_PROP_OBJ_REF(TilemapVX, Table, MapData, "map_data")
DEF_GFX_PROP_OBJ_REF(TilemapVX, Table, FlashData, "flash_data")
DEF_GFX_PROP_OBJ_REF(TilemapVX, Table, Flags, "flags")

DEF_GFX_PROP_B(TilemapVX, Visible)

DEF_GFX_PROP_I(TilemapVX, OX)
DEF_GFX_PROP_I(TilemapVX, OY)

RB_METHOD_GUARD(tilemapVXBitmapsSet) {
    TilemapVX::BitmapArray *a = getPrivateDataNoRaise<TilemapVX::BitmapArray>(self);

    if (!a)
        return self;

    int i;
    VALUE bitmapObj;

    rb_get_args(argc, argv, "io", &i, &bitmapObj RB_ARG_END);

    Bitmap *bitmap = getPrivateDataCheck<Bitmap>(bitmapObj, BitmapType);

    /* Ruby store first: the array is what keeps the bitmap alive, so the
     * native slot must never point at one it does not hold. A failing store
     * (negative index, frozen array) raises before set() and changes
     * nothing. set() assigns the slot before anything in it can throw, so
     * after a native failure the stored bitmap is the one the slot holds. */
    VALUE ary = rb_iv_get(self, "array");
    rb_ary_store(ary, i, bitmapObj);

    GFX_GUARD_ALL(a->set(i, bitmap);)
    return self;
}
RB_METHOD_GUARD_END

RB_METHOD(tilemapVXBitmapsGet) {
    int i;
    rb_get_args(argc, argv, "i", &i RB_ARG_END);
    
    if (i < 0 || i > 8)
        return Qnil;
    
    VALUE ary = rb_iv_get(self, "array");
    
    return rb_ary_entry(ary, i);
}

void tilemapVXBindingInit() {
    VALUE klass = rb_define_class("Tilemap", rb_cObject);
    rb_define_alloc_func(klass, classAllocate<&TilemapVXType>);
    
    disposableBindingInit<TilemapVX>(klass);
    
    _rb_define_method(klass, "initialize", tilemapVXInitialize);
    _rb_define_method(klass, "bitmaps", tilemapVXGetBitmapArray);
    _rb_define_method(klass, "update", tilemapVXUpdate);
    
    INIT_PROP_BIND(TilemapVX, Viewport, "viewport");
    INIT_PROP_BIND(TilemapVX, MapData, "map_data");
    INIT_PROP_BIND(TilemapVX, FlashData, "flash_data");
    INIT_PROP_BIND(TilemapVX, Visible, "visible");
    INIT_PROP_BIND(TilemapVX, OX, "ox");
    INIT_PROP_BIND(TilemapVX, OY, "oy");
    
    if (rgssVer == 3) {
        INIT_PROP_BIND(TilemapVX, Flags, "flags");
    } else {
        INIT_PROP_BIND(TilemapVX, Flags, "passages");
    }
    
    klass = rb_define_class_under(klass, "BitmapArray", rb_cObject);
    rb_define_alloc_func(klass, classAllocate<&BitmapArrayType>);
    
    _rb_define_method(klass, "[]=", tilemapVXBitmapsSet);
    _rb_define_method(klass, "[]", tilemapVXBitmapsGet);
}
