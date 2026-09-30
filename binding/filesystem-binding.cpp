/*
 ** filesystem-binding.cpp
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

#include "src/config.h"

#include "binding-util.h"

#include "filesystem.h"
#include "sharedstate.h"
#include "src/util/util.h"

#if RAPI_FULL > 187
#include "ruby/encoding.h"
#include "ruby/intern.h"
#else
#include "intern.h"
#endif

#if RAPI_MAJOR >= 2
#include <ruby/thread.h>
#endif

#if RAPI_FULL >= 270
static VALUE stringForceUTF8(RB_BLOCK_CALL_FUNC_ARGLIST(arg, callback_arg));
#endif

static void fileIntFreeInstance(void *inst) {
    SDL_RWops *ops = static_cast<SDL_RWops *>(inst);
    
    SDL_RWclose(ops);
    SDL_FreeRW(ops);
}

#if RAPI_FULL > 187
DEF_TYPE_CUSTOMFREE(FileInt, fileIntFreeInstance);
#else
DEF_ALLOCFUNC_CUSTOMFREE(FileInt, fileIntFreeInstance);
#endif

static VALUE fileIntForPath(const char *path, bool rubyExc) {
    VALUE klass = rb_const_get(rb_cObject, rb_intern("FileInt"));
    
    VALUE obj = rb_obj_alloc(klass);
    
    SDL_RWops *ops = SDL_AllocRW();
    if (!ops)
        throw Exception(Exception::SDLError, "%s", SDL_GetError());

    try {
        shState->fileSystem().openReadRaw(*ops, path);
    } catch (...) {
        SDL_FreeRW(ops);
        throw;
    }
    
    setPrivateData(obj, ops);
    
    return obj;
}

typedef struct {
    SDL_RWops *ops;
    char *dst;
    int length;
    int done;
} fileIntReadCbArgs;

/* SDL_RWread may return fewer bytes than asked before EOF; 0 is EOF or error. */
void call_RWread_cb(fileIntReadCbArgs *args) {
    while (args->done < args->length) {
        size_t got = SDL_RWread(args->ops, args->dst + args->done, 1,
                                args->length - args->done);
        if (got == 0)
            break;
        args->done += (int)got;
    }
}

RB_METHOD(fileIntRead) {
    
    int length = -1;
    rb_get_args(argc, argv, "|i", &length RB_ARG_END);
    
    SDL_RWops *ops = getPrivateData<SDL_RWops>(self);
    const bool whole = length == -1;
    
    if (whole) {
        Sint64 cur = SDL_RWtell(ops);
        Sint64 end = SDL_RWseek(ops, 0, SEEK_END);
        
        // Sometimes SDL_RWseek will fail for no reason
        // with encrypted archives, so let's just ask
        // for the size up front
        if (end < 0)
            end = ops->size(ops);
        
        length = end - cur;
        SDL_RWseek(ops, cur, SEEK_SET);
        if (end < 0 || length < 0)
            rb_raise(rb_eIOError, "cannot determine file size");
    } else if (length < 0) {
        rb_raise(rb_eArgError, "negative length %d given", length);
    }
    
    // Like IO#read, a whole read of an empty file is "", so Marshal says why.
    if (length == 0)
        return whole ? rb_str_new(0, 0) : Qnil;
    
    VALUE data = rb_str_new(0, length);
    
    
    
    fileIntReadCbArgs cbargs {ops, RSTRING_PTR(data), length, 0};
#if RAPI_MAJOR >= 2
    rb_thread_call_without_gvl([](void* args) -> void* {
        call_RWread_cb((fileIntReadCbArgs*)args);
        return 0;
    }, (void*)&cbargs, 0, 0);
#else
    call_RWread_cb(&cbargs);
#endif
    RB_GC_GUARD(data);
    
    // A whole-file read that stops early would hand unread bytes to Marshal.
    if (cbargs.done < length) {
        if (whole)
            rb_raise(rb_eIOError, "short read: %d of %d bytes", cbargs.done, length);
        if (cbargs.done == 0)
            return Qnil;
        rb_str_set_len(data, cbargs.done);
    }
    
    return data;
}

RB_METHOD(fileIntClose) {
    RB_UNUSED_PARAM;
    
    SDL_RWops *ops = getPrivateData<SDL_RWops>(self);
    SDL_RWclose(ops);
    
    return Qnil;
}

RB_METHOD(fileIntGetByte) {
    RB_UNUSED_PARAM;
    
    SDL_RWops *ops = getPrivateData<SDL_RWops>(self);
    
    unsigned char byte;
    size_t result = SDL_RWread(ops, &byte, 1, 1);
    
    return (result == 1) ? rb_fix_new(byte) : Qnil;
}

RB_METHOD(fileIntBinmode) {
    RB_UNUSED_PARAM;
    
    return Qnil;
}

#if RAPI_FULL <= 187
RB_METHOD(fileIntPos) {
    SDL_RWops *ops = getPrivateData<SDL_RWops>(self);
    
    long long pos = SDL_RWtell(ops); // Will return -1 if it doesn't work
    return LL2NUM(pos);
}
#endif

VALUE
kernelLoadDataInt(const char *filename, bool rubyExc, bool raw) {
    //rb_gc_start();
    
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    VALUE args[] = {fileIntForPath(filename, rubyExc), raw ? Qtrue : Qfalse};
    return rb_ensure([](VALUE opaque) -> VALUE {
        VALUE *args = reinterpret_cast<VALUE *>(opaque);
        VALUE data = fileIntRead(0, 0, args[0]);
        if (args[1] == Qtrue)
            return data;
        // Game data is the legacy text boundary: untagged strings are UTF-8.
        VALUE load[] = {data, rb_proc_new(stringForceUTF8, Qnil)};
        return rb_funcall2(rb_const_get(rb_cObject, rb_intern("Marshal")),
                           rb_intern("load"), 2, load);
    }, reinterpret_cast<VALUE>(args), [](VALUE port) -> VALUE {
        return rb_funcall(port, rb_intern("close"), 0);
    }, args[0]);
#else
    VALUE port = fileIntForPath(filename, rubyExc);
    VALUE result;
    if (!raw) {
        VALUE marsh = rb_const_get(rb_cObject, rb_intern("Marshal"));
        
        // FIXME need to catch exceptions here with begin rescue
        VALUE data = fileIntRead(0, 0, port);
        result = rb_funcall2(marsh, rb_intern("load"), 1, &data);
    } else {
        result = fileIntRead(0, 0, port);
    }
    
    rb_funcall2(port, rb_intern("close"), 0, NULL);
    
    return result;
#endif
}

RB_METHOD_GUARD(kernelLoadData) {
    RB_UNUSED_PARAM;
    
    VALUE filename;
    VALUE raw;
    rb_scan_args(argc, argv, "11", &filename, &raw);
    SafeStringValue(filename);
    
    bool rawv;
    rb_bool_arg(raw, &rawv);
    return kernelLoadDataInt(RSTRING_PTR(filename), true, rawv);
}
RB_METHOD_GUARD_END

RB_METHOD(kernelSaveData) {
    RB_UNUSED_PARAM;
    
    VALUE obj;
    VALUE filename;
    
    rb_get_args(argc, argv, "oS", &obj, &filename RB_ARG_END);
    
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    /* Stock's write, plus the close stock leaks when dump raises.
     * The file is opened before serialization, so a _dump hook that changes
     * the working directory cannot move the write to another slot. */
    VALUE args[] = {obj, rb_file_open_str(filename, "wb")};
    rb_ensure([](VALUE opaque) -> VALUE {
        VALUE *args = reinterpret_cast<VALUE *>(opaque);
        return rb_funcall2(rb_const_get(rb_cObject, rb_intern("Marshal")),
                           rb_intern("dump"), 2, args);
    }, reinterpret_cast<VALUE>(args), [](VALUE file) -> VALUE {
        return rb_io_close(file);
    }, args[1]);
    return Qnil;
#else
    VALUE file = rb_file_open_str(filename, "wb");
    
    VALUE marsh = rb_const_get(rb_cObject, rb_intern("Marshal"));
    
    VALUE v[] = {obj, file};
    rb_funcall2(marsh, rb_intern("dump"), ARRAY_SIZE(v), v);
    
    rb_io_close(file);
    
    return Qnil;
#endif
}
#if RAPI_FULL > 187
#if RAPI_FULL < 270
static VALUE stringForceUTF8(VALUE arg)
#else
static VALUE stringForceUTF8(RB_BLOCK_CALL_FUNC_ARGLIST(arg, callback_arg))
#endif
{
    if (RB_TYPE_P(arg, RUBY_T_STRING) && ENCODING_IS_ASCII8BIT(arg))
        rb_enc_associate_index(arg, rb_utf8_encindex());
    
    return arg;
}

#if !defined(__vita__) && !defined(MKXPZ_HOST_PORT_LOGIC)
#if RAPI_FULL < 270
static VALUE customProc(VALUE arg, VALUE proc) {
    VALUE obj = stringForceUTF8(arg);
    obj = rb_funcall2(proc, rb_intern("call"), 1, &obj);
    
    return obj;
}
#endif

RB_METHOD(_marshalLoad) {
    RB_UNUSED_PARAM;
#if RAPI_FULL < 270
    VALUE port, proc = Qnil;
    rb_get_args(argc, argv, "o|o", &port, &proc RB_ARG_END);
#else
    VALUE port;
    rb_get_args(argc, argv, "o", &port RB_ARG_END);
#endif
    
    VALUE utf8Proc;
#if RAPI_FULL < 270
    if (NIL_P(proc))
        
        utf8Proc = rb_proc_new(RUBY_METHOD_FUNC(stringForceUTF8), Qnil);
    else
        utf8Proc = rb_proc_new(RUBY_METHOD_FUNC(customProc), proc);
#else
    utf8Proc = rb_proc_new(stringForceUTF8, Qnil);
#endif
    
    VALUE marsh = rb_const_get(rb_cObject, rb_intern("Marshal"));
    
    VALUE v[] = {port, utf8Proc};
    return rb_funcall2(marsh, rb_intern("_mkxp_load_alias"), ARRAY_SIZE(v), v);
}
#endif
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
/* config.cpp: the one validity rule for a settings generation (nonempty,
 * bounded, parses to an object), shared with the boot merge and CFG[]. */
bool vitaSettingsFileValid(const char *path);

static VALUE settingsFileValid(VALUE, VALUE path) {
    return vitaSettingsFileValid(StringValueCStr(path)) ? Qtrue : Qfalse;
}
#endif

void fileIntBindingInit() {
    VALUE klass = rb_define_class("FileInt", rb_cIO);
#if RAPI_FULL > 187
    rb_define_alloc_func(klass, classAllocate<&FileIntType>);
#else
    rb_define_alloc_func(klass, FileIntAllocate);
#endif
    
    _rb_define_method(klass, "read", fileIntRead);
    _rb_define_method(klass, "getbyte", fileIntGetByte);
#if RAPI_FULL <= 187
    // Ruby doesn't see this as an initialized stream,
    // so either that has to be fixed or necessary
    // IO functions have to be overridden
    rb_define_alias(klass, "getc", "getbyte");
    _rb_define_method(klass, "pos", fileIntPos);
#endif
    _rb_define_method(klass, "binmode", fileIntBinmode);
    _rb_define_method(klass, "close", fileIntClose);
    
    _rb_define_module_function(rb_mKernel, "load_data", kernelLoadData);
    _rb_define_module_function(rb_mKernel, "save_data", kernelSaveData);
    
#if RAPI_FULL > 187
#if defined(MKXPZ_HOST_PORT_LOGIC) && !defined(__vita__)
    rb_load(rb_str_new_cstr("settings_file.rb"), 0); // Mapped package rubyLoadpaths.
#endif
#if !defined(__vita__) && !defined(MKXPZ_HOST_PORT_LOGIC)
    /* Ports keep MRI's Marshal.load (proc, freeze:, binary strings); only
     * load_data applies the legacy UTF-8 retag. */
    VALUE marsh = rb_const_get(rb_cObject, rb_intern("Marshal"));
    rb_define_alias(rb_singleton_class(marsh), "_mkxp_load_alias", "load");
    _rb_define_module_function(marsh, "load", _marshalLoad);
#endif
#endif
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#ifdef __vita__
    /* The settings writer below needs this module whatever a game preloads.
     * It wraps nothing: a game's saves are upstream's, above and in Ruby. */
    rb_load(rb_str_new_cstr("app0:/preload/settings_file.rb"), 0);
#endif
    VALUE settingsModule = rb_const_get(rb_cObject, rb_intern("VitaSettingsFile"));
    rb_define_singleton_method(settingsModule, "valid?",
                               RUBY_METHOD_FUNC(settingsFileValid), 1);
    rb_funcall(settingsModule, rb_intern("recover"), 1,
               rb_utf8_str_new_cstr(shState->config().userConfPath.c_str()));
#endif
}
