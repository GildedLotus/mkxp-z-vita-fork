/*
 ** binding-util.h
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

#ifndef BINDING_UTIL_H
#define BINDING_UTIL_H

#include <ruby.h>
#include <ruby/version.h>

#include "exception.h"

/* std::bad_alloc, for the guard macros below */
#include <new>

// The Vita runtime and its native extensions use the pinned MRI 3.1 ABI.
#if RUBY_API_VERSION_MAJOR != 3 || RUBY_API_VERSION_MINOR != 1
#error "The Vita bindings require MRI Ruby 3.1"
#endif

enum RbException {
    RGSS = 0,
    Reset,
    PHYSFS,
    SDL,
    MKXP,
    
    ErrnoENOENT,
    
    IOError,
    
    TypeError,
    ArgumentError,
    SystemExit,
    RuntimeError,
    
    RbExceptionsMax
};

struct RbData {
    VALUE exc[RbExceptionsMax];
    
    /* Input module (RGSS3) */
    VALUE buttoncodeHash;
    
    RbData();
    ~RbData();
};

RbData *getRbData();

struct Exception;

void raiseRbExc(Exception *exc);

/* raiseRbExc for an Exception the caller does not own: it raises the Ruby
 * exception without deleting its argument, so the pre-built ones below
 * survive the raise and can be used again. */
void raiseRbExcStatic(Exception *exc);

/* Pre-built during static initialisation, so reporting an exhausted heap
 * allocates nothing itself: Exception's constructor does msg.resize(512)
 * (util/exception.h), which on an exhausted heap throws a second bad_alloc --
 * from inside a catch block, where it would escape the binding method. */
extern Exception *const gOutOfMemoryExc;
extern Exception *const gUnknownFailureExc;

/* "new Exception(e)", except that it cannot throw. Returns 0 and sets oom
 * when the copy fails, so the caller reports with gOutOfMemoryExc instead of
 * letting a bad_alloc out of its catch block. */
Exception *copyExcForRaise(const Exception &e, bool &oom);

void *drop_gvl_guard(void *(*func)(void *), void *args,
                            rb_unblock_function_t *ubf, void *data2);

#define DECL_TYPE(Klass) extern rb_data_type_t Klass##Type

/* TODO: can mkxp use RUBY_TYPED_FREE_IMMEDIATELY here? */
#define DEF_TYPE_FLAGS 0

#define DEF_TYPE_CUSTOMNAME_AND_FREE(Klass, Name, Free)                        \
rb_data_type_t Klass##Type = {Name, {0, Free, 0, 0, 0}, 0, 0, DEF_TYPE_FLAGS}

#define DEF_TYPE_CUSTOMFREE(Klass, Free)                                       \
DEF_TYPE_CUSTOMNAME_AND_FREE(Klass, #Klass, Free)

#define DEF_TYPE_CUSTOMNAME(Klass, Name)                                       \
DEF_TYPE_CUSTOMNAME_AND_FREE(Klass, Name, freeInstance<Klass>)

#define DEF_TYPE(Klass) DEF_TYPE_CUSTOMNAME(Klass, #Klass)

template <rb_data_type_t *rbType> static VALUE classAllocate(VALUE klass) {
    return rb_data_typed_object_wrap(klass, 0, rbType);
}

#define CLASS_ALLOCATE_PRE_INIT(Name, initializeFunc)  \
static VALUE Name##AllocatePreInit(VALUE klass) {      \
  VALUE ret = classAllocate<& Name##Type>(klass);     \
                                                       \
  initializeFunc(0, 0, ret);                           \
                                                       \
  return ret;                                          \
}

template <class C> static void freeInstance(void *inst) {
    delete static_cast<C *>(inst);
}

void raiseDisposedAccess(VALUE self);

template <class C> inline C *getPrivateDataNoRaise(VALUE self) {
    return static_cast<C *>(RTYPEDDATA_DATA(self));
}

template <class C> inline C *getPrivateData(VALUE self) {
    C *c = getPrivateDataNoRaise<C>(self);
    
    if (!c) {
        //raiseRbExc(Exception(Exception::MKXPError, "No instance data for variable (missing call to super?)"));
        
        /* FIXME: MiniFFI and FileInt don't have default allocations
         * despite not being disposables. Should they be fixed,
         * or just left with a misleading error message? */
        raiseDisposedAccess(self);
    }
    return c;
}

template <class C>
static inline C *
getPrivateDataCheck(VALUE self, const rb_data_type_t &type)
{
    if (!rb_typeddata_is_kind_of(self, &type))
        rb_raise(rb_eTypeError, "Can't convert %s into %s", rb_obj_classname(self),
                 type.wrap_struct_name);
    
    void *obj = RTYPEDDATA_DATA(self);
    return static_cast<C *>(obj);
}

static inline void setPrivateData(VALUE self, void *p) {
    /* RGSS's behavior is to just leak memory if a disposable is reinitialized,
     * with the original disposable being left permanently instantiated,
     * but that's (1) bad, and (2) would currently cause memory access issues
     * when things like a sprite's src_rect inevitably get GC'd, so we're not
     * copying that. */
    // Free the old value if it already exists (initialize called twice?)
    if (RTYPEDDATA_DATA(self) && (RTYPEDDATA_DATA(self) != p)) {
        /* RUBY_TYPED_NEVER_FREE == 0, and we don't use
         * RUBY_TYPED_DEFAULT_FREE for our stuff, so just
         * checking if it's truthy should be fine */
        if (RTYPEDDATA_TYPE(self)->function.dfree)
            (*RTYPEDDATA_TYPE(self)->function.dfree)(RTYPEDDATA_DATA(self));
    }
    RTYPEDDATA_DATA(self) = p;
}

inline VALUE
wrapObject(void *p, const rb_data_type_t &type, VALUE underKlass = rb_cObject)
{
    VALUE klass = rb_const_get(underKlass, rb_intern(type.wrap_struct_name));
    VALUE obj = rb_obj_alloc(klass);
    
    setPrivateData(obj, p);
    
    return obj;
}

inline VALUE wrapProperty(VALUE self, void *prop, const char *iv,
                          const rb_data_type_t &type,
                          VALUE underKlass = rb_cObject) {
    VALUE propObj = wrapObject(prop, type, underKlass);
    
    rb_iv_set(self, iv, propObj);
    
    return propObj;
}

/* Implemented: oSszfibn| */
int rb_get_args(int argc, VALUE *argv, const char *format, ...);

/* Always terminate 'rb_get_args' with this */
#ifndef NDEBUG
#define RB_ARG_END_VAL ((void *)-1)
#define RB_ARG_END , RB_ARG_END_VAL
#else
#define RB_ARG_END
#endif

typedef VALUE (*RubyMethod)(int argc, VALUE *argv, VALUE self);

static inline void _rb_define_method(VALUE klass, const char *name,
                                     RubyMethod func) {
    rb_define_method(klass, name, RUBY_METHOD_FUNC(func), -1);
}

static inline void rb_define_class_method(VALUE klass, const char *name,
                                          RubyMethod func) {
    rb_define_singleton_method(klass, name, RUBY_METHOD_FUNC(func), -1);
}

static inline void _rb_define_module_function(VALUE module, const char *name,
                                              RubyMethod func) {
    rb_define_module_function(module, name, RUBY_METHOD_FUNC(func), -1);
}

/* Releases the graphics lock for every C++ type. std::bad_alloc is not an
 * Exception (util/exception.h), and GFX_LOCK is a live recursive kernel
 * mutex: an escape pins it for the rest of the process. */
#define GFX_GUARD_EXC(exp)                                               \
{                                                                        \
GFX_LOCK;                                                                \
try {                                                                    \
exp                                                                      \
} catch (const Exception &exc) {                                         \
GFX_UNLOCK;                                                              \
throw exc;                                                               \
} catch (...) {                                                          \
GFX_UNLOCK;                                                              \
throw;                                                                   \
}                                                                        \
GFX_UNLOCK;                                                              \
}

/* Same catches as GFX_GUARD_EXC. Construction sites name this one so the
 * bad_alloc path stays obvious next to the call. */
#define GFX_GUARD_ALL(exp)                                               \
{                                                                        \
GFX_LOCK;                                                                \
try {                                                                    \
exp                                                                      \
} catch (const Exception &exc) {                                         \
GFX_UNLOCK;                                                              \
throw exc;                                                               \
} catch (...) {                                                          \
GFX_UNLOCK;                                                              \
throw;                                                                   \
}                                                                        \
GFX_UNLOCK;                                                              \
}


template <class C>
static inline VALUE objectLoad(int argc, VALUE *argv, VALUE self) {
    const char *data;
    int dataLen;
    rb_get_args(argc, argv, "s", &data, &dataLen RB_ARG_END);
    
    VALUE obj = rb_obj_alloc(self);
    
    C *c = 0;
    
    c = C::deserialize(data, dataLen);
    
    setPrivateData(obj, c);
    
    return obj;
}

static inline VALUE rb_bool_new(bool value) { return value ? Qtrue : Qfalse; }

inline void rb_float_arg(VALUE arg, double *out, int argPos = 0) {
    switch (rb_type(arg)) {
        case RUBY_T_FLOAT:
            *out = RFLOAT_VALUE(arg);
            break;
            
        case RUBY_T_FIXNUM:
            *out = FIX2INT(arg);
            break;
            
        default:
            throw Exception(Exception::TypeError, "Argument %d: Expected float", argPos);
    }
}

inline void rb_int_arg(VALUE arg, int *out, int argPos = 0) {
    switch (rb_type(arg)) {
        case RUBY_T_FLOAT:
            // FIXME check int range?
            *out = NUM2LONG(arg);
            break;
            
        case RUBY_T_FIXNUM:
            *out = FIX2INT(arg);
            break;
            
        default:
            throw Exception(Exception::TypeError, "Argument %d: Expected fixnum", argPos);
    }
}

inline void rb_bool_arg(VALUE arg, bool *out, int argPos = 0) {
    switch (rb_type(arg)) {
        case RUBY_T_TRUE:
            *out = true;
            break;
            
        case RUBY_T_FALSE:
        case RUBY_T_NIL:
            *out = false;
            break;
            
        default:
            throw Exception(Exception::TypeError, "Argument %d: Expected bool", argPos);
    }
}

/* rb_check_argc and rb_error_arity are both
 * consistently called before any C++ objects are allocated,
 * so we can just call rb_raise directly in them */
inline void rb_check_argc(int actual, int expected) {
    if (actual != expected)
        rb_raise(rb_eArgError, "wrong number of arguments (%d for %d)", actual,
                 expected);
}


#define RB_METHOD(name) static VALUE name(int argc, VALUE *argv, VALUE self)

#define RB_UNUSED_PARAM                                                        \
{                                                                            \
(void)argc;                                                                \
(void)argv;                                                                \
(void)self;                                                                \
}

/* The state RB_GUARD_REPORT reports through, and the two arms that fill it.
 * Spelled as their own macros because the Ruby boundary is not only the
 * RB_METHOD_GUARD pair: rb_get_args is variadic and returns an int, and
 * serializableDump has to return the string it built, so neither can be a
 * guarded method -- but both are called straight from Ruby and owe it exactly
 * the same contract. One definition, three users. */
#define RB_GUARD_VARS                           \
    Exception *exc = 0;                         \
    bool oom = false;

/* Closes a try block and reports whatever left it as a Ruby raise. No C++
 * exception of any type may cross the Ruby boundary. On ARM EABI the Ruby C
 * frame that called us carries no unwind information -- Ruby's whole VM text
 * is one EXIDX_CANTUNWIND region -- so the unwinder returns _URC_FAILURE and
 * std::terminate() runs. std::bad_alloc is not an Exception, hence its own
 * arm; it reports through the pre-built gOutOfMemoryExc because the heap it
 * would allocate a report from is the one that just ran out.
 *
 * Every raise here is a longjmp, so nothing after it runs and nothing with a
 * destructor may be live at the call site. */
#define RB_GUARD_REPORT                         \
    } catch (const Exception &e) {              \
        exc = copyExcForRaise(e, oom);          \
    } catch (const std::bad_alloc &) {          \
        exc = 0; oom = true;                    \
    } catch (...) {                             \
        exc = copyExcForRaise(*gUnknownFailureExc, oom); \
    }                                           \
    if (oom) {                                  \
        raiseRbExcStatic(gOutOfMemoryExc);      \
    }                                           \
    if (exc) {                                  \
        raiseRbExc(exc);                        \
    }

/* Calling rb_raise inside the catch block
 * leaks memory even if we catch by value */
#define RB_METHOD_GUARD(name) RB_METHOD(name)   \
{                                               \
    RB_GUARD_VARS                               \
    try{                                        \

#define RB_METHOD_GUARD_END                     \
    RB_GUARD_REPORT                             \
    return Qnil;                                \
}

#define MARSH_LOAD_FUN(Typ)                                                    \
RB_METHOD_GUARD(Typ##Load) { return objectLoad<Typ>(argc, argv, self); } RB_METHOD_GUARD_END

#define INITCOPY_FUN(Klass)                                                    \
RB_METHOD_GUARD(Klass##InitializeCopy) {                                   \
VALUE origObj;                                                             \
rb_get_args(argc, argv, "o", &origObj RB_ARG_END);                         \
if (!OBJ_INIT_COPY(self, origObj)) /* When would this fail??*/             \
return self;                                                             \
Klass *orig = getPrivateData<Klass>(origObj);                              \
Klass *k = 0;                                                              \
k = new Klass(*orig);                                                      \
setPrivateData(self, k);                                                   \
return self;                                                               \
}                                                                          \
RB_METHOD_GUARD_END

/* Object property which is copied by reference, with allowed NIL
 * FIXME: Getter assumes prop is disposable,
 * because self.disposed? is not checked in this case.
 * Should make this more clear */

// --------------
// Do not wait for Graphics.update
// --------------
#define DEF_PROP_OBJ_REF(Klass, PropKlass, PropName, prop_iv)                  \
RB_METHOD(Klass##Get##PropName) {                                            \
RB_UNUSED_PARAM;                                                           \
return rb_iv_get(self, prop_iv);                                           \
}                                                                            \
RB_METHOD_GUARD(Klass##Set##PropName) {                                    \
RB_UNUSED_PARAM;                                                           \
rb_check_argc(argc, 1);                                                    \
Klass *k = getPrivateData<Klass>(self);                                    \
VALUE propObj = *argv;                                                     \
PropKlass *prop;                                                           \
if (NIL_P(propObj))                                                        \
prop = 0;                                                                \
else                                                                       \
prop = getPrivateDataCheck<PropKlass>(propObj, PropKlass##Type);         \
k->set##PropName(prop)                                                     \
rb_iv_set(self, prop_iv, propObj);                                         \
return propObj;                                                            \
}                                                                          \
RB_METHOD_GUARD_END

/* Object property which is copied by value, not reference */
#define DEF_PROP_OBJ_VAL(Klass, PropKlass, PropName, prop_iv)                  \
RB_METHOD(Klass##Get##PropName) {                                            \
RB_UNUSED_PARAM;                                                           \
checkDisposed<Klass>(self);                                                \
return rb_iv_get(self, prop_iv);                                           \
}                                                                            \
RB_METHOD_GUARD(Klass##Set##PropName) {                                    \
rb_check_argc(argc, 1);                                                    \
Klass *k = getPrivateData<Klass>(self);                                    \
VALUE propObj = *argv;                                                     \
PropKlass *prop;                                                           \
prop = getPrivateDataCheck<PropKlass>(propObj, PropKlass##Type);           \
k->set##PropName(*prop);                                                   \
return propObj;                                                            \
}                                                                          \
RB_METHOD_GUARD_END

#define DEF_PROP(Klass, type, PropName, arg_fun, value_fun)                \
RB_METHOD_GUARD(Klass##Get##PropName) {                                    \
RB_UNUSED_PARAM;                                                           \
Klass *k = getPrivateData<Klass>(self);                                    \
type value = 0;                                                            \
value = k->get##PropName();                                                \
return value_fun(value);                                                   \
}                                                                          \
RB_METHOD_GUARD_END                                                        \
RB_METHOD_GUARD(Klass##Set##PropName) {                                    \
rb_check_argc(argc, 1);                                                    \
Klass *k = getPrivateData<Klass>(self);                                    \
type value;                                                                \
rb_##arg_fun##_arg(*argv, &value);                                         \
k->set##PropName(value);                                                   \
return *argv;                                                              \
}                                                                          \
RB_METHOD_GUARD_END

#define DEF_PROP_I(Klass, PropName)                                            \
DEF_PROP(Klass, int, PropName, int, rb_fix_new)

#define DEF_PROP_F(Klass, PropName)                                            \
DEF_PROP(Klass, double, PropName, float, rb_float_new)

#define DEF_PROP_B(Klass, PropName)                                            \
DEF_PROP(Klass, bool, PropName, bool, rb_bool_new)

#define INIT_PROP_BIND(Klass, PropName, prop_name_s)                           \
{                                                                            \
_rb_define_method(klass, prop_name_s, Klass##Get##PropName);               \
_rb_define_method(klass, prop_name_s "=", Klass##Set##PropName);           \
}

// --------------
// Wait for Graphics.update
// --------------
#define DEF_GFX_PROP_OBJ_REF(Klass, PropKlass, PropName, prop_iv)                  \
RB_METHOD(Klass##Get##PropName) {                                            \
RB_UNUSED_PARAM;                                                           \
return rb_iv_get(self, prop_iv);                                           \
}                                                                            \
RB_METHOD_GUARD(Klass##Set##PropName) {                                    \
RB_UNUSED_PARAM;                                                           \
rb_check_argc(argc, 1);                                                    \
Klass *k = getPrivateData<Klass>(self);                                    \
VALUE propObj = *argv;                                                     \
PropKlass *prop;                                                           \
if (NIL_P(propObj))                                                        \
prop = 0;                                                                \
else                                                                       \
prop = getPrivateDataCheck<PropKlass>(propObj, PropKlass##Type);         \
GFX_GUARD_EXC(k->set##PropName(prop);)                                         \
rb_iv_set(self, prop_iv, propObj);                                         \
return propObj;                                                            \
}                                                                          \
RB_METHOD_GUARD_END

/* Object property which is copied by value, not reference */
#define DEF_GFX_PROP_OBJ_VAL(Klass, PropKlass, PropName, prop_iv)                  \
RB_METHOD(Klass##Get##PropName) {                                            \
RB_UNUSED_PARAM;                                                           \
checkDisposed<Klass>(self);                                                \
return rb_iv_get(self, prop_iv);                                           \
}                                                                            \
RB_METHOD_GUARD(Klass##Set##PropName) {                                    \
rb_check_argc(argc, 1);                                                    \
Klass *k = getPrivateData<Klass>(self);                                    \
VALUE propObj = *argv;                                                     \
PropKlass *prop;                                                           \
prop = getPrivateDataCheck<PropKlass>(propObj, PropKlass##Type);           \
GFX_GUARD_EXC(k->set##PropName(*prop);)                                        \
return propObj;                                                            \
}                                                                          \
RB_METHOD_GUARD_END

#define DEF_GFX_PROP(Klass, type, PropName, arg_fun, value_fun)            \
RB_METHOD_GUARD(Klass##Get##PropName) {                                    \
RB_UNUSED_PARAM;                                                           \
Klass *k = getPrivateData<Klass>(self);                                    \
type value = 0;                                                            \
value = k->get##PropName();                                                \
return value_fun(value);                                                   \
}                                                                            \
RB_METHOD_GUARD_END                                                        \
RB_METHOD_GUARD(Klass##Set##PropName) {                                    \
rb_check_argc(argc, 1);                                                    \
Klass *k = getPrivateData<Klass>(self);                                    \
type value;                                                                \
rb_##arg_fun##_arg(*argv, &value);                                         \
GFX_GUARD_EXC(k->set##PropName(value);)                                        \
return *argv;                                                              \
}                                                                          \
RB_METHOD_GUARD_END

#define DEF_GFX_PROP_I(Klass, PropName)                                            \
DEF_GFX_PROP(Klass, int, PropName, int, rb_fix_new)

#define DEF_GFX_PROP_F(Klass, PropName)                                            \
DEF_GFX_PROP(Klass, double, PropName, float, rb_float_new)

#define DEF_GFX_PROP_B(Klass, PropName)                                            \
DEF_GFX_PROP(Klass, bool, PropName, bool, rb_bool_new)

#endif // BINDING_UTIL_H
