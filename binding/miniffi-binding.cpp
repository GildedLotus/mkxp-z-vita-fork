// Most of the MiniFFI class was taken from Ruby 1.8's Win32API.c,
// it's just as basic but should work fine for the moment

#include <SDL.h>
#include <cstdint>
#include <new>
#include <string>

#include "filesystem/filesystem.h"
#include "miniffi.h"
#include "binding-util.h"

#if RAPI_MAJOR >= 2
#include <ruby/thread.h>
#endif

#if defined(__linux__) || defined(__APPLE__)
#define MVAL2RB(v) ULONG2NUM(v)
#define RB2MVAL(v) (mffi_value)NUM2ULONG(v)
#else
#ifdef __MINGW64__
#define MVAL2RB(v) ULL2NUM(v)
#define RB2MVAL(v) (mffi_value)NUM2ULL(v)
#else
#define MVAL2RB(v) UINT2NUM(v)
#define RB2MVAL(v) (mffi_value)NUM2UINT(v)
#endif
#endif

#define _T_VOID 0
#define _T_NUMBER 1
#define _T_POINTER 2
#define _T_INTEGER 3
#define _T_BOOL 4

/* Everything an initialized object is lives in this one record, out of reach
 * of Ruby: initialize builds a complete replacement and publishes it with one
 * pointer swap, so an object is either wholly old or wholly new. func is
 * always a resolved entry point; an object that never finished initialize has
 * no record at all. */
struct MiniFFIData {
    void *lib;
    void *func;
    /* Calls in flight, from argument conversion to the result. Only touched
     * with the GVL held, so reinitialize can refuse to unload code that is
     * executing or about to. */
    int active;
    int exports;
    VALUE libname;
    VALUE funcname;
    VALUE imports;
};

static void MiniFFIMark(void *p) {
    MiniFFIData *data = static_cast<MiniFFIData *>(p);
    rb_gc_mark(data->libname);
    rb_gc_mark(data->funcname);
    rb_gc_mark(data->imports);
}

static void MiniFFIFree(void *p) {
    MiniFFIData *data = static_cast<MiniFFIData *>(p);
    if (data->lib)
        SDL_UnloadObject(data->lib);
    delete data;
}

#if RAPI_FULL > 187
DEF_TYPE_CUSTOMFREE(MiniFFI, MiniFFIFree);
#else
static VALUE MiniFFIAllocate(VALUE klass) {
    return Data_Wrap_Struct(klass, MiniFFIMark, MiniFFIFree, 0);
}
#endif

static void *MiniFFI_GetFunctionHandle(void *libhandle, const char *func) {
    if (!libhandle)
        return 0;
    return SDL_LoadFunction(libhandle, func);
}

// MiniFFI.new(library, function[, imports[, exports]])
// Yields itself in blocks

RB_METHOD_GUARD(MiniFFI_initialize) {
    VALUE libname, func, imports, exports;
    rb_scan_args(argc, argv, "22", &libname, &func, &imports, &exports);
    SafeStringValue(libname);
    SafeStringValue(func);
    /* Reinitializing replaces the object's whole state or none of it. Every
     * step that can raise into Ruby (a Ruby raise is a longjmp: no C++
     * destructor runs and nothing native may be held) happens before anything
     * is loaded; the replacement is then built in one record and published by
     * one swap. These two are the cheap early refusals; the swap checks again,
     * because the conversions below can run Ruby code and let another thread
     * start a call or freeze the object. */
    if (OBJ_FROZEN(self))
        rb_error_frozen("MiniFFI");
    MiniFFIData *previous = getPrivateDataNoRaise<MiniFFIData>(self);
    if (previous && previous->active)
        throw Exception(Exception::RuntimeError,
                 "MiniFFI function is running; it cannot be reinitialized");

    VALUE ary_imports = rb_ary_new();
    VALUE *entry;
    switch (TYPE(imports)) {
        case T_NIL:
            break;
        case T_ARRAY:
            entry = RARRAY_PTR(imports);
            for (int i = 0; i < RARRAY_LEN(imports); i++) {
                SafeStringValue(entry[i]);
                switch (*(char *)RSTRING_PTR(entry[i])) {
                    case 'N':
                    case 'n':
                    case 'L':
                    case 'l':
                        rb_ary_push(ary_imports, INT2FIX(_T_NUMBER));
                        break;
                        
                    case 'P':
                    case 'p':
                        rb_ary_push(ary_imports, INT2FIX(_T_POINTER));
                        break;
                        
                    case 'I':
                    case 'i':
                        rb_ary_push(ary_imports, INT2FIX(_T_INTEGER));
                        break;
                        
                    case 'B':
                    case 'b':
                        rb_ary_push(ary_imports, INT2FIX(_T_BOOL));
                        break;
                }
            }
            break;
        default:
            SafeStringValue(imports);
            const char *s = RSTRING_PTR(imports);
            for (int i = 0; i < RSTRING_LEN(imports); i++) {
                switch (*s++) {
                    case 'N':
                    case 'n':
                    case 'L':
                    case 'l':
                        rb_ary_push(ary_imports, INT2FIX(_T_NUMBER));
                        break;
                        
                    case 'P':
                    case 'p':
                        rb_ary_push(ary_imports, INT2FIX(_T_POINTER));
                        break;
                        
                    case 'I':
                    case 'i':
                        rb_ary_push(ary_imports, INT2FIX(_T_INTEGER));
                        break;
                        
                    case 'B':
                    case 'b':
                        rb_ary_push(ary_imports, INT2FIX(_T_BOOL));
                        break;
                }
            }
            break;
    }
    
    if (MINIFFI_MAX_ARGS < RARRAY_LEN(ary_imports))
        throw Exception(Exception::RuntimeError, "too many parameters: %ld/%ld\n",
                 RARRAY_LEN(ary_imports), MINIFFI_MAX_ARGS);
    
    int ex = _T_VOID;
    if (NIL_P(exports)) {
        ex = _T_VOID;
    } else {
        SafeStringValue(exports);
        switch (*RSTRING_PTR(exports)) {
            case 'V':
            case 'v':
                ex = _T_VOID;
                break;
                
            case 'N':
            case 'n':
            case 'L':
            case 'l':
                ex = _T_NUMBER;
                break;
                
            case 'P':
            case 'p':
                ex = _T_POINTER;
                break;
                
            case 'I':
            case 'i':
                ex = _T_INTEGER;
                break;
                
            case 'B':
            case 'b':
                ex = _T_BOOL;
                break;
        }
    }

    /* From here nothing calls into Ruby until the swap, so only C++ exceptions
     * can leave and the record and library are released by hand on each. */
    MiniFFIData *data = new (std::nothrow) MiniFFIData{0, 0, 0, ex, libname, func, ary_imports};
    if (!data)
        throw std::bad_alloc();
    try {
#ifdef __APPLE__
        data->lib = SDL_LoadObject(mkxp_fs::normalizePath(RSTRING_PTR(libname), 1, 1).c_str());
#else
        data->lib = SDL_LoadObject(RSTRING_PTR(libname));
#endif
        data->func = MiniFFI_GetFunctionHandle(data->lib, RSTRING_PTR(func));
#ifdef __WIN32__
        if (data->lib && !data->func) {
            std::string func_a(RSTRING_PTR(func));
            func_a += 'A';
            data->func = SDL_LoadFunction(data->lib, func_a.c_str());
        }
#endif
        if (!data->func)
            throw Exception(Exception::RuntimeError, "%s", SDL_GetError());
    } catch (...) {
        MiniFFIFree(data);
        throw;
    }

    /* Nothing can run between this check and the swap, so it is final. */
    const bool frozen = OBJ_FROZEN(self);
    previous = getPrivateDataNoRaise<MiniFFIData>(self);
    if (frozen || (previous && previous->active)) {
        MiniFFIFree(data);
        if (frozen)
            rb_error_frozen("MiniFFI");
        throw Exception(Exception::RuntimeError,
                 "MiniFFI function is running; it cannot be reinitialized");
    }
    setPrivateData(self, data);
    if (rb_block_given_p())
        rb_yield(self);
    return Qnil;
}
RB_METHOD_GUARD_END

#if RAPI_MAJOR >= 2
typedef struct {
    MINIFFI_FUNC function;
    MiniFFIFuncArgs *args;
    int nparams;
} MFFICallCBArgs;

void* miniffi_call_cb(void *args) {
    MFFICallCBArgs *a = (MFFICallCBArgs*)args;
    return (void*)miniffi_call_intern(a->function, a->args, a->nparams);
    }
#endif

/* The call proper, with the object's record pinned by `active` (see
 * MiniFFI_call), so the library and everything the record holds stay valid
 * however much Ruby code the conversions below run. */
RB_METHOD_GUARD(MiniFFI_callPinned) {
    MiniFFIFuncArgs param;
#define params param.params
    MiniFFIData *funcData = getPrivateDataNoRaise<MiniFFIData>(self);
    MINIFFI_FUNC ApiFunction = (MINIFFI_FUNC)funcData->func;
    VALUE own_imports = funcData->imports;
    VALUE args;
    int items = rb_scan_args(argc, argv, "0*", &args);
    int nimport = RARRAY_LEN(own_imports);
    if (items != nimport)
        throw Exception(Exception::RuntimeError,
                 "wrong number of parameters: expected %d, got %d", nimport, items);
    
    for (int i = 0; i < nimport; i++) {
        VALUE str = rb_ary_entry(args, i);
        mffi_value lParam = 0;
        switch (FIX2INT(rb_ary_entry(own_imports, i))) {
            case _T_POINTER:
                if (NIL_P(str)) {
                    lParam = 0;
                } else if (FIXNUM_P(str)) {
                    lParam = RB2MVAL(str);
                } else {
                    StringValue(str);
                    rb_str_modify(str);
                    lParam = (mffi_value)RSTRING_PTR(str);
                }
                break;
                
            case _T_BOOL:
                rb_bool_arg(rb_ary_entry(args, i), (bool*)&lParam);
                break;
                
            case _T_INTEGER:
#if INTPTR_MAX == INT64_MAX
                lParam = RB2MVAL(rb_ary_entry(args, i)) & UINT32_MAX;
                break;
#endif
            case _T_NUMBER:
            default:
                lParam = RB2MVAL(rb_ary_entry(args, i));
                break;
        }
        params[i] = lParam;
    }
#if RAPI_MAJOR >= 2
    MFFICallCBArgs cb_args {ApiFunction, &param, nimport};
    mffi_value ret = (mffi_value)rb_thread_call_without_gvl(miniffi_call_cb, &cb_args, 0, 0);
#else
    mffi_value ret = miniffi_call_intern(ApiFunction, &param, nimport);
#endif
    
    switch (funcData->exports) {
        case _T_NUMBER:
        case _T_INTEGER:
            return MVAL2RB(ret);
            
        case _T_POINTER:
            return rb_utf8_str_new_cstr((char *)ret);
            
        case _T_BOOL:
            return rb_bool_new(ret);
            
        case _T_VOID:
        default:
            return MVAL2RB(0);
    }
}
RB_METHOD_GUARD_END

struct MiniFFICallFrame {
    MiniFFIData *data;
    int argc;
    VALUE *argv;
    VALUE self;
};

static VALUE MiniFFI_callBody(VALUE frame) {
    MiniFFICallFrame *f = reinterpret_cast<MiniFFICallFrame *>(frame);
    return MiniFFI_callPinned(f->argc, f->argv, f->self);
}

/* Runs on every way out, including the Thread#raise / Thread#kill / Timeout
 * unwind that rb_thread_call_without_gvl performs on return, which would
 * skip a plain decrement and leave the object refusing to reinitialize. */
static VALUE MiniFFI_callRelease(VALUE frame) {
    --reinterpret_cast<MiniFFICallFrame *>(frame)->data->active;
    return Qnil;
}

RB_METHOD_GUARD(MiniFFI_call) {
    MiniFFIData *funcData = getPrivateDataNoRaise<MiniFFIData>(self);
    /* An object that skipped or failed initialize (an override that skips
     * `super`, MiniFFI.allocate, a dup of one) has no record and so no entry
     * point: a Ruby error the game can rescue instead of a call through
     * garbage. */
    if (!funcData)
        throw Exception(Exception::RuntimeError,
                 "MiniFFI function was not initialized");
    MiniFFICallFrame frame = {funcData, argc, argv, self};
    ++funcData->active;
    return rb_ensure(MiniFFI_callBody, reinterpret_cast<VALUE>(&frame),
                     MiniFFI_callRelease, reinterpret_cast<VALUE>(&frame));
}
RB_METHOD_GUARD_END

void MiniFFIBindingInit() {
    VALUE cMiniFFI = rb_define_class("MiniFFI", rb_cObject);
#if RAPI_FULL > 187
    MiniFFIType.function.dmark = MiniFFIMark;
    rb_define_alloc_func(cMiniFFI, classAllocate<&MiniFFIType>);
#else
    rb_define_alloc_func(cMiniFFI, MiniFFIAllocate);
#endif
    _rb_define_method(cMiniFFI, "initialize", MiniFFI_initialize);
    _rb_define_method(cMiniFFI, "call", MiniFFI_call);
    rb_define_alias(cMiniFFI, "Call", "call");
    
    rb_define_const(rb_cObject, "Win32API", cMiniFFI);
}
