/*
** binding-util.cpp
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

#include "binding-util.h"

#include "exception.h"
#include "sharedstate.h"
#include "src/util/util.h"

#include <assert.h>
#include <stdarg.h>
#include <string.h>

RbData *getRbData() { return static_cast<RbData *>(shState->bindingData()); }

struct {
  RbException id;
  const char *name;
} static customExc[] = {
    {MKXP, "MKXPError"}, {PHYSFS, "PHYSFSError"}, {SDL, "SDLError"}};

RbData::RbData() {
  for (size_t i = 0; i < ARRAY_SIZE(customExc); ++i)
    exc[customExc[i].id] = rb_define_class(customExc[i].name, rb_eException);

  exc[RGSS] = rb_define_class("RGSSError", rb_eStandardError);
  exc[Reset] =
      rb_define_class(rgssVer >= 3 ? "RGSSReset" : "Reset", rb_eException);

  exc[ErrnoENOENT] = rb_const_get(rb_const_get(rb_cObject, rb_intern("Errno")),
                                  rb_intern("ENOENT"));
  exc[IOError] = rb_eIOError;
  exc[TypeError] = rb_eTypeError;
  exc[ArgumentError] = rb_eArgError;
  exc[SystemExit] = rb_eSystemExit;
  exc[RuntimeError] = rb_eRuntimeError;
}

RbData::~RbData() {}

/* Indexed with Exception::Type */
const RbException excToRbExc[] = {
    RGSS,        /* RGSSError          */
    Reset,       /* Reset/RGSSReset */
    ErrnoENOENT, /* NoFileError        */
    IOError,

    TypeError,   ArgumentError, SystemExit, RuntimeError,

    PHYSFS, /* PHYSFSError */
    SDL,    /* SDLError    */
    MKXP    /* MKXPError   */
};

/* Built during static initialisation, long before the heap can be under any
 * pressure, and never freed: every other way of reporting an exhausted heap
 * allocates (Exception's constructor alone does msg.resize(512)) and so fails
 * a second time, from inside a catch block, where the failure escapes the
 * binding method and terminates the process. Neither message contains a
 * printf directive. */
static Exception outOfMemoryExc(Exception::MKXPError,
                                "out of memory (native allocation failed)");
static Exception unknownFailureExc(Exception::MKXPError,
                                   "unknown native failure");

Exception *const gOutOfMemoryExc = &outOfMemoryExc;
Exception *const gUnknownFailureExc = &unknownFailureExc;

Exception *copyExcForRaise(const Exception &e, bool &oom) {
  try {
    return new Exception(e);
  } catch (...) {
    oom = true;
    return 0;
  }
}

static void raiseRbExcOwned(Exception *exc, bool owned) {
  VALUE str = rb_str_new2(exc->msg.c_str());
  RbData *data = getRbData();
  VALUE excClass = data->exc[excToRbExc[exc->type]];

  if (owned)
    delete exc;

  rb_exc_raise(rb_class_new_instance(1, &str, excClass));
}

void raiseRbExc(Exception *exc) { raiseRbExcOwned(exc, true); }

void raiseRbExcStatic(Exception *exc) { raiseRbExcOwned(exc, false); }

void raiseDisposedAccess(VALUE self) {
#if RAPI_FULL > 187
  const char *klassName = RTYPEDDATA_TYPE(self)->wrap_struct_name;
#else
  const char *klassName = rb_obj_classname(self);
#endif
  char buf[32];

  strncpy(buf, klassName, sizeof(buf));
  buf[0] = tolower(buf[0]);

  rb_raise(getRbData()->exc[RGSS], "disposed %s", buf);
}

/* A Ruby boundary in its own right: every binding method reaches this before
 * it does anything else, and many of its callers are plain RB_METHODs with no
 * guard of their own, so what leaves here reaches Ruby's frames directly.
 * rb_float_arg, rb_int_arg and rb_bool_arg throw Exception for a wrong
 * argument type, and Exception's constructor resizes a std::string to 512
 * bytes -- so on an exhausted heap what a wrong argument produces is a
 * std::bad_alloc, which the Exception-only guard this used to have did not
 * catch. */
int rb_get_args(int argc, VALUE *argv, const char *format, ...) {
  RB_GUARD_VARS
  try{
    char c;
    VALUE *arg = argv;
    va_list ap;
    bool opt = false;
    int argI = 0;

    va_start(ap, format);

    while ((c = *format++)) {
      switch (c) {
      case '|':
        break;
      default:
        // FIXME print num of needed args vs provided
        if (argc <= argI && !opt)
          rb_raise(rb_eArgError, "wrong number of arguments");

        break;
      }

      if (argI >= argc)
        break;

      switch (c) {
      case 'o': {
        if (argI >= argc)
          break;

        VALUE *obj = va_arg(ap, VALUE *);

        *obj = *arg++;
        ++argI;

        break;
      }

      case 'S': {
        if (argI >= argc)
          break;

        VALUE *str = va_arg(ap, VALUE *);
        VALUE tmp = *arg;

        if (!RB_TYPE_P(tmp, RUBY_T_STRING))
          rb_raise(rb_eTypeError, "Argument %d: Expected string", argI);

        *str = tmp;
        ++argI;

        break;
      }

      case 's': {
        if (argI >= argc)
          break;

        const char **s = va_arg(ap, const char **);
        int *len = va_arg(ap, int *);

        VALUE tmp = *arg;

        if (!RB_TYPE_P(tmp, RUBY_T_STRING))
          rb_raise(rb_eTypeError, "Argument %d: Expected string", argI);

        *s = RSTRING_PTR(tmp);
        *len = RSTRING_LEN(tmp);
        ++argI;

        break;
      }

      case 'z': {
        if (argI >= argc)
          break;

        const char **s = va_arg(ap, const char **);

        VALUE tmp = *arg++;

        if (!RB_TYPE_P(tmp, RUBY_T_STRING))
          rb_raise(rb_eTypeError, "Argument %d: Expected string", argI);

        *s = RSTRING_PTR(tmp);
        ++argI;

        break;
      }

      case 'f': {
        if (argI >= argc)
          break;

        double *f = va_arg(ap, double *);
        VALUE fVal = *arg++;

        rb_float_arg(fVal, f, argI);

        ++argI;
        break;
      }

      case 'i': {
        if (argI >= argc)
          break;

        int *i = va_arg(ap, int *);
        VALUE iVal = *arg++;

        rb_int_arg(iVal, i, argI);

        ++argI;
        break;
      }

      case 'b': {
        if (argI >= argc)
          break;

        bool *b = va_arg(ap, bool *);
        VALUE bVal = *arg++;

        rb_bool_arg(bVal, b, argI);

        ++argI;
        break;
      }

      case 'n': {
        if (argI >= argc)
          break;

        ID *sym = va_arg(ap, ID *);

        VALUE symVal = *arg++;

        if (!SYMBOL_P(symVal))
          rb_raise(rb_eTypeError, "Argument %d: Expected symbol", argI);

        *sym = SYM2ID(symVal);
        ++argI;

        break;
      }

      case '|':
        opt = true;
        break;

      default:
        rb_raise(rb_eFatal, "invalid argument specifier %c", c);
      }
    }

#ifndef NDEBUG

    /* Pop remaining arg pointers off
     * the stack to check for RB_ARG_END */
    format--;

    while ((c = *format++)) {
      switch (c) {
      case 'o':
      case 'S':
        va_arg(ap, VALUE *);
        break;

      case 's':
        va_arg(ap, const char **);
        va_arg(ap, int *);
        break;

      case 'z':
        va_arg(ap, const char **);
        break;

      case 'f':
        va_arg(ap, double *);
        break;

      case 'i':
        va_arg(ap, int *);
        break;

      case 'b':
        va_arg(ap, bool *);
        break;
      }
    }

    // FIXME print num of needed args vs provided
    if (!c && argc > argI)
      rb_raise(rb_eArgError, "wrong number of arguments");

    /* Verify correct termination */
    void *argEnd = va_arg(ap, void *);
    (void)argEnd;
    assert(argEnd == RB_ARG_END_VAL);

#endif

    va_end(ap);

    return argI;

  /* Raising here is probably fine, right?
   * If any methods allocate something with a destructor before
   * calling this then they can probably be fixed to not do that. */
  RB_GUARD_REPORT

  return 0;
}

#if RAPI_MAJOR >= 2
#include <ruby/thread.h>

typedef struct gvl_guard_args {
	Exception *exc;
	void *(*func)(void *);
	void *args;
	/* Reported as a flag rather than as an object, so that nothing is
	 * allocated inside the without-GVL frame. */
	bool oom;
} gvl_guard_args;

/* Nothing may leave this frame either: it is called by Ruby's
 * rb_thread_call_without_gvl (thread.c), another frame with no unwind
 * information on ARM EABI. Every C++ type is caught here and rethrown by
 * drop_gvl_guard on the Ruby side. */
static void *gvl_guard(void *args) {
	gvl_guard_args *gvl_args = (gvl_guard_args*)args;
	try{
		return gvl_args->func(gvl_args->args);
	} catch (const Exception &e) {
		gvl_args->exc = copyExcForRaise(e, gvl_args->oom);
	} catch (const std::bad_alloc &) {
		gvl_args->oom = true;
	} catch (...) {
		gvl_args->exc = copyExcForRaise(*gUnknownFailureExc, gvl_args->oom);
	}
	return 0;
}

void *drop_gvl_guard(void *(*func)(void *), void *args,
                            rb_unblock_function_t *ubf, void *data2) {
	gvl_guard_args gvl_args = {0, func, args, false};

	void *ret = rb_thread_call_without_gvl(&gvl_guard, &gvl_args, ubf, data2);

	Exception *&exc = gvl_args.exc;
	if (gvl_args.oom){
		/* Rethrown as itself rather than as an Exception, so the caller's
		 * RB_METHOD_GUARD_END reports it without allocating. Throwing
		 * bad_alloc needs no heap: the runtime keeps an emergency buffer for
		 * exactly this case. */
		delete exc;
		throw std::bad_alloc();
	}
	if (exc){
		Exception e(*exc);
		delete exc;
		throw e;
	}
	return ret;
}

#endif
