/*
** table-binding.cpp
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
#include "serializable-binding.h"
#include "table.h"
#include <algorithm>

static int num2TableSize(VALUE v) {
  int i = NUM2INT(v);
  return std::max(0, i);
}

static void parseArgsTableSizes(int argc, VALUE *argv, int *x, int *y, int *z) {
  *y = *z = 1;

  switch (argc) {
  case 3:
    *z = num2TableSize(argv[2]);
    /* fall through */
  case 2:
    *y = num2TableSize(argv[1]);
    /* fall through */
  case 1:
    *x = num2TableSize(argv[0]);
    break;
  default:
    rb_error_arity(argc, 1, 3);
  }
}

/* Cells share the newlib heap with Bitmap pixels, so MRI is told about them.
 * Every Table is Ruby-owned and changes size only in the methods below, which
 * charge the difference; tableFree returns exactly the current size. */
static ssize_t tableBytes(const Table *t) {
  return t ? (ssize_t)((int64_t)t->xSize() * t->ySize() * t->zSize() * sizeof(int16_t)) : 0;
}

/* rb_gc_adjust_memory_usage only counts: MRI tests its malloc limit on the
 * next xmalloc, and a Table.new loop makes none. A one-byte xmalloc runs that
 * test now. */
static void tableCharge(ssize_t diff) {
  rb_gc_adjust_memory_usage(diff);
  if (diff > 0)
    ruby_xfree(ruby_xmalloc(1));
}

static void tableFree(void *inst) {
  Table *t = static_cast<Table *>(inst);
  const ssize_t bytes = tableBytes(t);
  delete t;
  rb_gc_adjust_memory_usage(-bytes);
}

DEF_TYPE_CUSTOMFREE(Table, tableFree);

/* The database-load path, and one of the larger allocations a script makes: a
 * Table is a std::vector<int16_t> of x*y*z cells (src/etc/table.cpp), a stock
 * VX Ace map is three 100x100 layers, and resize() holds the old vector and the
 * new one at once, so the peak is twice the table. It comes out of the same
 * SceLibc heap as every software Bitmap, so std::bad_alloc here is an ordinary
 * outcome -- and unguarded it crossed a Ruby C frame with no unwind
 * information, so the player terminated instead of raising. */
RB_METHOD_GUARD(tableInitialize) {
  int x, y, z;

  parseArgsTableSizes(argc, argv, &x, &y, &z);

  Table *t = getPrivateDataNoRaise<Table>(self);
  const ssize_t before = tableBytes(t);
  if (t) {
    t->resize(x, y, z);
  } else {
    t = new Table(x, y, z);

    setPrivateData(self, t);
  }
  tableCharge(tableBytes(t) - before);

  return self;
}
RB_METHOD_GUARD_END

/* The other half of tableInitialize, and the one upstream left bare. It reaches
 * the same std::vector<int16_t> allocation -- resize() holds the old vector and
 * the new one at once, so the peak is both of them -- which makes std::bad_alloc
 * an ordinary outcome here; and src/etc/table.cpp now also throws
 * Exception::ArgumentError at it for a product no int can describe, which is
 * what `t.resize(65536, 65536)` asks for. Unguarded, either one crossed a Ruby C
 * frame with no unwind information and terminated the player. Guarded, the first
 * is a NoMemoryError and the second an ArgumentError -- and the table still has
 * the cells it had, because the count is computed before the new vector
 * exists. */
RB_METHOD_GUARD(tableResize) {
  Table *t = getPrivateData<Table>(self);

  int x, y, z;
  parseArgsTableSizes(argc, argv, &x, &y, &z);

  const ssize_t before = tableBytes(t);
  t->resize(x, y, z);
  tableCharge(tableBytes(t) - before);

  return Qnil;
}
RB_METHOD_GUARD_END

#define TABLE_SIZE(d, D)                                                       \
  RB_METHOD(table##D##Size) {                                                  \
    RB_UNUSED_PARAM                                                            \
    Table *t = getPrivateData<Table>(self);                                    \
    return INT2NUM(t->d##Size());                                              \
  }

TABLE_SIZE(x, X)
TABLE_SIZE(y, Y)
TABLE_SIZE(z, Z)

RB_METHOD_GUARD(tableGetAt) {
  Table *t = getPrivateData<Table>(self);

  int x, y, z;
  x = y = z = 0;

  x = NUM2INT(argv[0]);
  if (argc > 1)
    y = NUM2INT(argv[1]);
  if (argc > 2)
    z = NUM2INT(argv[2]);

  if (argc > 3)
    throw Exception(Exception::ArgumentError, "wrong number of arguments");

  if (x < 0 || x >= t->xSize() || y < 0 || y >= t->ySize() || z < 0 ||
      z >= t->zSize()) {
    return Qnil;
  }

  short result = t->get(x, y, z);

  return INT2FIX(result); /* short always fits in a Fixnum */
}
RB_METHOD_GUARD_END

RB_METHOD_GUARD(tableSetAt) {
  Table *t = getPrivateData<Table>(self);

  int x, y, z, value;
  x = y = z = 0;

  if (argc < 2)
    throw Exception(Exception::ArgumentError, "wrong number of arguments");

  switch (argc) {
  default:
  case 2:
    x = NUM2INT(argv[0]);
    value = NUM2INT(argv[1]);

    break;
  case 3:
    x = NUM2INT(argv[0]);
    y = NUM2INT(argv[1]);
    value = NUM2INT(argv[2]);

    break;
  case 4:
    x = NUM2INT(argv[0]);
    y = NUM2INT(argv[1]);
    z = NUM2INT(argv[2]);
    value = NUM2INT(argv[3]);

    break;
  }

  t->set(value, x, y, z);

  return argv[argc - 1];
}
RB_METHOD_GUARD_END

MARSH_LOAD_FUN(Table)
INITCOPY_FUN(Table)

/* Both generic bodies install a new Table with setPrivateData, whose dfree
 * already returned the charge of the one it replaced. */
static VALUE tableChargeInstalled(VALUE obj, const Table *before) {
  const Table *t = getPrivateDataNoRaise<Table>(obj);
  if (t != before)
    tableCharge(tableBytes(t));
  return obj;
}

RB_METHOD(tableLoad) { return tableChargeInstalled(TableLoad(argc, argv, self), 0); }

RB_METHOD(tableInitializeCopy) {
  const Table *before = getPrivateDataNoRaise<Table>(self);
  return tableChargeInstalled(TableInitializeCopy(argc, argv, self), before);
}


/* Reached from the allocation function, not from initialize: Ruby calls
 * TableAllocatePreInit (CLASS_ALLOCATE_PRE_INIT below) for every Table it
 * builds, including every one Marshal restores. Raising from an allocator is
 * ordinary for Ruby -- it is a C function called from rb_obj_alloc, and the
 * raise propagates like any other -- whereas letting a C++ exception out of it
 * terminated the process. */
RB_METHOD_GUARD(tableInitializeDefault) {
  Table *t = new Table(0, 0, 0);

  setPrivateData(self, t);

  return self;
}
RB_METHOD_GUARD_END

CLASS_ALLOCATE_PRE_INIT(Table, tableInitializeDefault);

void tableBindingInit() {
  VALUE klass = rb_define_class("Table", rb_cObject);
  rb_define_alloc_func(klass, TableAllocatePreInit);

  serializableBindingInit<Table>(klass);

  rb_define_class_method(klass, "_load", tableLoad);

  _rb_define_method(klass, "initialize", tableInitialize);
  _rb_define_method(klass, "initialize_copy", tableInitializeCopy);
  _rb_define_method(klass, "resize", tableResize);
  _rb_define_method(klass, "xsize", tableXSize);
  _rb_define_method(klass, "ysize", tableYSize);
  _rb_define_method(klass, "zsize", tableZSize);
  _rb_define_method(klass, "[]", tableGetAt);
  _rb_define_method(klass, "[]=", tableSetAt);
}
