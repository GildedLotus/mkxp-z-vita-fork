/*
 ** graphics-binding.cpp
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

#include "config.h"
#include "graphics.h"
#include "sharedstate.h"
#include "binding-util.h"
#include "binding-types.h"
#include "exception.h"
#ifdef MKXPZ_SOFTWARE_BITMAPS
#include "gl-util.h"
#endif

/* Every method in this file that touches the graphics lock takes it through
 * GFX_GUARD_ALL, never by hand. Two separate things go wrong with a bare
 * GFX_LOCK ... GFX_UNLOCK pair: a C++ exception steps over the unlock, and so
 * does a Ruby raise, which is a longjmp and runs no catch block at all. Either
 * one pins a live recursive kernel mutex for the rest of the process. That is
 * why the Ruby calls that used to sit between the two -- rb_float_new here and
 * in averageFrameRate -- are now outside it. */
RB_METHOD_GUARD(graphicsDelta) {
    RB_UNUSED_PARAM;
    double delta = 0;
    GFX_GUARD_ALL( delta = shState->graphics().getDelta(); );
    return rb_float_new(delta);
}
RB_METHOD_GUARD_END

/* The guard inside the lambda is GFX_GUARD_ALL for a reason of its own: this
 * body runs inside rb_thread_call_without_gvl, so what escapes it is caught by
 * gvl_guard and re-thrown on the Ruby side. gvl_guard handles std::bad_alloc
 * (binding-util.cpp), so the process no longer dies here -- but the lock is
 * released by the guard or not at all, and GFX_GUARD_EXC released it only for
 * Exception. Under MKXPZ_SOFTWARE_BITMAPS this path allocates for real: a
 * window's CPU base does basePixels.assign((size_t)w*h*4, 0) inside
 * prepare(), reached from the scene composite in Graphics::update. */
RB_METHOD_GUARD(graphicsUpdate)
{
    RB_UNUSED_PARAM;
    drop_gvl_guard([](void*) -> void* {
        GFX_GUARD_ALL( shState->graphics().update(); );
        return 0;
    }, 0, 0, 0);
    return Qnil;
}
RB_METHOD_GUARD_END

RB_METHOD_GUARD(graphicsAverageFrameRate)
{
    RB_UNUSED_PARAM;
    double rate = 0;
    GFX_GUARD_ALL( rate = shState->graphics().averageFrameRate(); );
    return rb_float_new(rate);
}
RB_METHOD_GUARD_END

#ifdef MKXPZ_SOFTWARE_BITMAPS
// Observe the existing retirement clock; Ruby allocation stays outside the lock.
RB_METHOD_GUARD(graphicsVitaSwapCount)
{
    RB_UNUSED_PARAM;
    uint64_t count = 0;
    GFX_GUARD_ALL( count = GPUBudget::frameCounter(); );
    return ULL2NUM(count);
}
RB_METHOD_GUARD_END
#endif

RB_METHOD_GUARD(graphicsFreeze)
{
    RB_UNUSED_PARAM;

    drop_gvl_guard([](void*) -> void* {
        GFX_GUARD_ALL( shState->graphics().freeze(); );
        return 0;
    }, 0, 0, 0);

    return Qnil;
}
RB_METHOD_GUARD_END

typedef struct {
    int duration;
    const char *filename;
    int vague;
} TransitionArgs;

RB_METHOD_GUARD(graphicsTransition)
{
    RB_UNUSED_PARAM;
    
    int duration = 8;
    const char *filename = "";
    int vague = 40;
    
    rb_get_args(argc, argv, "|izi", &duration, &filename, &vague RB_ARG_END);
    
    TransitionArgs args = {duration, filename, vague};
    
    drop_gvl_guard([](void *args) -> void* {
        TransitionArgs &a = *((TransitionArgs*)args);
        GFX_GUARD_ALL( shState->graphics().transition(a.duration,
                                                      a.filename,
                                                      a.vague
                                                     ); );
        return 0;
    }, &args, 0, 0);

    return Qnil;
}
RB_METHOD_GUARD_END

RB_METHOD_GUARD(graphicsFrameReset)
{
    RB_UNUSED_PARAM;

    GFX_GUARD_ALL( shState->graphics().frameReset(); );

    return Qnil;
}
RB_METHOD_GUARD_END

/* The three setter macros held the lock by hand as well. Nothing they call
 * throws today, but setBrightness reaches the scene and setScale and
 * setFullscreen reach the event thread, so a future throw here would have been
 * an abort with the lock pinned -- the same shape as resize_screen below, for
 * one token less. The getters take no lock and are left as they are. */
#define DEF_GRA_PROP_I(PropName) \
RB_METHOD(graphics##Get##PropName) \
{ \
RB_UNUSED_PARAM; \
return rb_fix_new(shState->graphics().get##PropName()); \
} \
RB_METHOD_GUARD(graphics##Set##PropName) \
{ \
RB_UNUSED_PARAM; \
int value; \
rb_get_args(argc, argv, "i", &value RB_ARG_END); \
GFX_GUARD_ALL( shState->graphics().set##PropName(value); ) \
return rb_fix_new(value); \
} \
RB_METHOD_GUARD_END

#define DEF_GRA_PROP_B(PropName) \
RB_METHOD(graphics##Get##PropName) \
{ \
RB_UNUSED_PARAM; \
return rb_bool_new(shState->graphics().get##PropName()); \
} \
RB_METHOD_GUARD(graphics##Set##PropName) \
{ \
RB_UNUSED_PARAM; \
bool value; \
rb_get_args(argc, argv, "b", &value RB_ARG_END); \
GFX_GUARD_ALL( shState->graphics().set##PropName(value); ) \
return rb_bool_new(value); \
} \
RB_METHOD_GUARD_END

#define DEF_GRA_PROP_F(PropName) \
RB_METHOD(graphics##Get##PropName) \
{ \
RB_UNUSED_PARAM; \
return rb_float_new(shState->graphics().get##PropName()); \
} \
RB_METHOD_GUARD(graphics##Set##PropName) \
{ \
RB_UNUSED_PARAM; \
double value; \
rb_get_args(argc, argv, "f", &value RB_ARG_END); \
GFX_GUARD_ALL( shState->graphics().set##PropName(value); ) \
return rb_float_new(value); \
} \
RB_METHOD_GUARD_END

RB_METHOD(graphicsWidth)
{
    RB_UNUSED_PARAM;
    
    return rb_fix_new(shState->graphics().width());
}

RB_METHOD(graphicsHeight)
{
    RB_UNUSED_PARAM;
    
    return rb_fix_new(shState->graphics().height());
}

RB_METHOD(graphicsDisplayWidth)
{
    RB_UNUSED_PARAM;
    
    return rb_fix_new(shState->graphics().displayWidth());
}

RB_METHOD(graphicsDisplayHeight)
{
    RB_UNUSED_PARAM;
    
    return rb_fix_new(shState->graphics().displayHeight());
}

RB_METHOD_GUARD(graphicsWait)
{
    RB_UNUSED_PARAM;
    
    int duration;
    rb_get_args(argc, argv, "i", &duration RB_ARG_END);
    drop_gvl_guard([](void* d) -> void* {
        GFX_GUARD_ALL( shState->graphics().wait(*(int*)d); );
        return 0;
    }, (int*)&duration, 0, 0);
    return Qnil;
}
RB_METHOD_GUARD_END

RB_METHOD_GUARD(graphicsFadeout)
{
    RB_UNUSED_PARAM;
    
    int duration;
    rb_get_args(argc, argv, "i", &duration RB_ARG_END);
    
    drop_gvl_guard([](void* d) -> void* {
        GFX_GUARD_ALL( shState->graphics().fadeout(*(int*)d); );
        return 0;
    }, (int*)&duration, 0, 0);
    
    return Qnil;
}
RB_METHOD_GUARD_END

RB_METHOD_GUARD(graphicsFadein)
{
    RB_UNUSED_PARAM;
    
    int duration;
    rb_get_args(argc, argv, "i", &duration RB_ARG_END);
    
    drop_gvl_guard([](void* d) -> void* {
        GFX_GUARD_ALL( shState->graphics().fadein(*(int*)d); );
        return 0;
    }, (int*)&duration, 0, 0);
    
    return Qnil;
}
RB_METHOD_GUARD_END

void bitmapInitProps(Bitmap *b, VALUE self);

RB_METHOD_GUARD(graphicsSnapToBitmap)
{
    RB_UNUSED_PARAM;

    /* Ask Ruby for the object BEFORE the Bitmap exists. wrapObject is
     * rb_const_get + rb_obj_alloc + setPrivateData, and the two Ruby calls
     * raise by longjmp: whatever this frame alone owned when they ran is
     * simply lost. snapToBitmap() allocates a whole screen-sized CPU bitmap
     * under MKXPZ_SOFTWARE_BITMAPS -- 905 KiB of a 16 MiB heap -- so that is
     * the leak worth closing. A Bitmap VALUE with no instance data yet is the
     * ordinary state between rb_obj_alloc and initialize (classAllocate wraps
     * a null pointer and freeInstance deletes null happily), and nothing can
     * reach this one before it is filled in. */
    VALUE obj = wrapObject((Bitmap*)0, BitmapType);

    Bitmap *result = 0;

    GFX_GUARD_ALL( result = shState->graphics().snapToBitmap(); );

    setPrivateData(obj, result);
    bitmapInitProps(result, obj);

    return obj;
}
RB_METHOD_GUARD_END

/* Graphics::resizeScreen re-specifies the screen surfaces through
 * TEXFBO::reallocChecked, which calls surfaceFailed() -- that is, throws --
 * when the FBO does not come back complete. Unguarded, a driver hiccup here
 * was std::terminate() with the GL lock held. The texture code owns what
 * reallocChecked does and when; this only changes how its failure is
 * reported: a Ruby error the game can see instead of a dead player. */
RB_METHOD_GUARD(graphicsResizeScreen)
{
    RB_UNUSED_PARAM;

    int width, height;
    rb_get_args(argc, argv, "ii", &width, &height RB_ARG_END);

    GFX_GUARD_ALL( shState->graphics().resizeScreen(width, height); );

    return Qnil;
}
RB_METHOD_GUARD_END

RB_METHOD_GUARD(graphicsResizeWindow)
{
    RB_UNUSED_PARAM;

    int width, height;
    bool center = false;
    rb_get_args(argc, argv, "ii|b", &width, &height, &center RB_ARG_END);

    GFX_GUARD_ALL( shState->graphics().resizeWindow(width, height, center); );

    return Qnil;
}
RB_METHOD_GUARD_END

RB_METHOD_GUARD(graphicsReset)
{
    RB_UNUSED_PARAM;

    GFX_GUARD_ALL( shState->graphics().reset(); );

    return Qnil;
}
RB_METHOD_GUARD_END

RB_METHOD(graphicsCenter)
{
    RB_UNUSED_PARAM;
    
    shState->graphics().center();
    return Qnil;
}

typedef struct {
    const char *filename;
    int volume;
    bool skippable;
} PlayMovieArgs;

void *playMovieInternal(void *args) {
    PlayMovieArgs *a = (PlayMovieArgs*)args;
    GFX_GUARD_ALL( shState->graphics().playMovie(a->filename, a->volume, a->skippable); );
    
    // Signals for shutdown or reset only make playMovie quit early,
    // so check again
    shState->checkShutdown();
    shState->checkReset();
    
    return 0;
}

RB_METHOD_GUARD(graphicsPlayMovie)
{
    RB_UNUSED_PARAM;
    
    VALUE filename, volumeArg, skippable;
    rb_scan_args(argc, argv, "12", &filename, &volumeArg, &skippable);
    SafeStringValue(filename);
    
    bool skip;
    rb_bool_arg(skippable, &skip);

    // TODO: Video control inputs (e.g. skip, pause)

    PlayMovieArgs args{};
    args.filename = RSTRING_PTR(filename);
    args.volume = (volumeArg == Qnil) ? 100 : NUM2INT(volumeArg);;
    args.skippable = skip;
    drop_gvl_guard(playMovieInternal, &args, 0, 0);
    
    return Qnil;
}
RB_METHOD_GUARD_END

void graphicsScreenshotInternal(const char *filename)
{
    GFX_GUARD_ALL(shState->graphics().screenshot(filename););
}

RB_METHOD_GUARD(graphicsScreenshot)
{
    RB_UNUSED_PARAM;

    VALUE filename;
    rb_scan_args(argc, argv, "1", &filename);
    SafeStringValue(filename);
    
    drop_gvl_guard([](void* fn) -> void* {
        graphicsScreenshotInternal((const char*)fn);
        return 0;
    }, (void*)RSTRING_PTR(filename), 0, 0);
    return Qnil;
}
RB_METHOD_GUARD_END

DEF_GRA_PROP_I(FrameRate)
DEF_GRA_PROP_I(FrameCount)
DEF_GRA_PROP_I(Brightness)

DEF_GRA_PROP_B(Fullscreen)
DEF_GRA_PROP_B(ShowCursor)
DEF_GRA_PROP_F(Scale)
DEF_GRA_PROP_B(Frameskip)
DEF_GRA_PROP_B(FixedAspectRatio)
DEF_GRA_PROP_I(SmoothScaling)
DEF_GRA_PROP_B(IntegerScaling)
DEF_GRA_PROP_B(LastMileScaling)
DEF_GRA_PROP_B(Threadsafe)

#define INIT_GRA_PROP_BIND(PropName, prop_name_s) \
{ \
_rb_define_module_function(module, prop_name_s, graphics##Get##PropName); \
_rb_define_module_function(module, prop_name_s "=", graphics##Set##PropName); \
}

void graphicsBindingInit()
{
    VALUE module = rb_define_module("Graphics");
    
    _rb_define_module_function(module, "delta", graphicsDelta);
    _rb_define_module_function(module, "update", graphicsUpdate);
    _rb_define_module_function(module, "freeze", graphicsFreeze);
    _rb_define_module_function(module, "transition", graphicsTransition);
    _rb_define_module_function(module, "frame_reset", graphicsFrameReset);
    _rb_define_module_function(module, "screenshot", graphicsScreenshot);
    
    _rb_define_module_function(module, "__reset__", graphicsReset);
    
    INIT_GRA_PROP_BIND( FrameRate,  "frame_rate"  );
    INIT_GRA_PROP_BIND( FrameCount, "frame_count" );
    _rb_define_module_function(module, "average_frame_rate", graphicsAverageFrameRate);
#ifdef MKXPZ_SOFTWARE_BITMAPS
    _rb_define_module_function(module, "vita_swap_count", graphicsVitaSwapCount);
#endif

    _rb_define_module_function(module, "width", graphicsWidth);
    _rb_define_module_function(module, "height", graphicsHeight);
    _rb_define_module_function(module, "display_width", graphicsDisplayWidth);
    _rb_define_module_function(module, "display_height", graphicsDisplayHeight);
    _rb_define_module_function(module, "wait", graphicsWait);
    _rb_define_module_function(module, "fadeout", graphicsFadeout);
    _rb_define_module_function(module, "fadein", graphicsFadein);
    _rb_define_module_function(module, "snap_to_bitmap", graphicsSnapToBitmap);
    _rb_define_module_function(module, "resize_screen", graphicsResizeScreen);
    _rb_define_module_function(module, "resize_window", graphicsResizeWindow);
    _rb_define_module_function(module, "center", graphicsCenter);
        
    INIT_GRA_PROP_BIND( Brightness, "brightness" );

    // end
    
    //if (rgssVer >= 3)
    //{
    _rb_define_module_function(module, "play_movie", graphicsPlayMovie);
    //}
    
    INIT_GRA_PROP_BIND( Fullscreen,       "fullscreen"         );
    INIT_GRA_PROP_BIND( ShowCursor,       "show_cursor"        );
    INIT_GRA_PROP_BIND( Scale,            "scale"              );
    INIT_GRA_PROP_BIND( Frameskip,        "frameskip"          );
    INIT_GRA_PROP_BIND( FixedAspectRatio, "fixed_aspect_ratio" );
    INIT_GRA_PROP_BIND( SmoothScaling,    "smooth_scaling"     );
    INIT_GRA_PROP_BIND( IntegerScaling,   "integer_scaling"    );
    INIT_GRA_PROP_BIND( LastMileScaling,  "last_mile_scaling"  );
    INIT_GRA_PROP_BIND( Threadsafe,       "thread_safe"        );
}
