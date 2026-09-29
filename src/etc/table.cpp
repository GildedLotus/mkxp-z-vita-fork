/*
** table.cpp
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

#include "table.h"

#include <string.h>
#include <algorithm>

#include "serial-util.h"
#include "exception.h"
#include "util.h"

/* The number of cells a Table of these dimensions holds, or an ArgumentError
 * if they cannot describe one. Both places a Table's dimensions are ever set
 * -- the constructor below and resize() -- size their vector through this.
 *
 * Upstream wrote `x*y*z` in int at both of them, and nothing bounds the three
 * arguments: `Table.new(65536, 65536)` reaches the constructor straight from a
 * script, with no memory pressure of any kind, and the product is 2^32. Signed
 * overflow is undefined; in practice it wraps to 0, so the object ends up
 * claiming 65536x65536x1 cells over a vector holding none, `Table#[]=` passes
 * its own bounds check against the stored dimensions, and the write lands
 * outside the allocation. `Table.new(46341, 46341)` wraps negative instead,
 * which std::vector sees as a size_t near 2^64. An earlier fix closed the same
 * arithmetic on the Marshal path (deserialize below); this is the direct
 * script path it left open.
 *
 * Neither multiply here can overflow: a dimension is an int, so x*y is at most
 * (2^31-1)^2 < 2^62 in int64_t, and the second multiply runs only on a count
 * already known to be no larger than the limit.
 *
 * The limit is a cell count, and it bounds every int expression this class
 * computes as well as the allocation:
 *
 *   - data has at most cellLimit entries, so the `xs*ys*z + xs*y + x` that
 *     at(), get() and set() index with is below 2^24 for every in-range
 *     subscript, and the same expression in resize()'s copy loop is too;
 *   - `xs*ys` on its own is bounded, which is why a zero z does not exempt the
 *     first multiply: serialSize() and serialize() both evaluate `xs*ys*zs`
 *     left to right, so Table.new(100000, 100000, 0) would overflow int there
 *     while holding no cells at all;
 *   - serialSize()'s `20 + count*2` is at most 20 + 2^25.
 *
 * 2^24 cells is 32 MiB of int16_t, and resize() holds the old vector and the
 * new one at once, so the worst peak is 64 MiB of the 128 MiB newlib heap
 * (VITA_GLUE_NEWLIB_HEAP_BYTES, vita/glue/vita_glue.h) that the engine, MRI
 * and every software Bitmap also come out of. The largest table any RPG Maker
 * editor writes is a 500x500 map's three layers -- 750000 cells, 22x under the
 * limit -- so this rejects nothing a game can legitimately ask for. Below the
 * limit nothing else changes: a table the heap cannot fit still reaches the
 * allocator, and the std::bad_alloc it comes back with is still a Ruby
 * NoMemoryError through the guard layer. */
static int tableCellCount(int x, int y, int z)
{
	static const int64_t cellLimit = (int64_t) 1 << 24;

	if (x < 0 || y < 0 || z < 0)
		throw Exception(Exception::ArgumentError,
		                "Table: negative dimension (%d, %d, %d)", x, y, z);

	int64_t count = (int64_t) x * y;

	if (count <= cellLimit)
		count *= z;

	if (count > cellLimit)
		throw Exception(Exception::ArgumentError,
		                "Table: %dx%dx%d is larger than the limit of %d cells",
		                x, y, z, (int) cellLimit);

	return (int) count;
}

/* Init normally */
Table::Table(int x, int y /*= 1*/, int z /*= 1*/)
    : xs(x), ys(y), zs(z),
      data(tableCellCount(x, y, z))
{}

Table::Table(const Table &other)
    : xs(other.xs), ys(other.ys), zs(other.zs),
      data(other.data)
{}

int16_t Table::get(int x, int y, int z) const
{
	return data[xs*ys*z + xs*y + x];
}

void Table::set(int16_t value, int x, int y, int z)
{
	if (x < 0 || x >= xs
	||  y < 0 || y >= ys
	||  z < 0 || z >= zs)
	{
		return;
	}

	data[xs*ys*z + xs*y + x] = value;

	modified();
}

void Table::resize(int x, int y, int z)
{
	if (x == xs && y == ys && z == zs)
		return;

	/* Computed before anything moves: on the throwing path the table still has
	 * the dimensions and the cells it had, which is what lets the guarded
	 * binding method report Table#resize(65536, 65536) as an ArgumentError and
	 * leave the script's table usable. */
	std::vector<int16_t> newData(tableCellCount(x, y, z));

	for (int k = 0; k < std::min(z, zs); ++k)
		for (int j = 0; j < std::min(y, ys); ++j)
			for (int i = 0; i < std::min(x, xs); ++i)
				newData[x*y*k + x*j + i] = at(i, j, k);

	data.swap(newData);

	xs = x;
	ys = y;
	zs = z;

	return;
}

void Table::resize(int x, int y)
{
	resize(x, y, zs);
}

void Table::resize(int x)
{
	resize(x, ys, zs);
}

/* Serializable */
int Table::serialSize() const
{
	/* header + data */
	return 20 + (xs * ys * zs) * 2;
}

void Table::serialize(char *buffer) const
{
	/* Table dimensions: we don't care
	 * about them but RMXP needs them */
	int dim = 1;
	int size = xs * ys * zs;

	if (ys > 1)
		dim = 2;

	if (zs > 1)
		dim = 3;

	writeInt32(&buffer, dim);
	writeInt32(&buffer, xs);
	writeInt32(&buffer, ys);
	writeInt32(&buffer, zs);
	writeInt32(&buffer, size);

	memcpy(buffer, dataPtr(data), sizeof(int16_t)*size);
}


Table *Table::deserialize(const char *data, int len)
{
	if (len < 20)
		throw Exception(Exception::RGSSError, "Marshal: Table: bad file format");

	readInt32(&data);
	int x = readInt32(&data);
	int y = readInt32(&data);
	int z = readInt32(&data);
	int size = readInt32(&data);

	/* All five fields come out of the file. Upstream compared `size != x*y*z`
	 * and `len != 20 + x*y*z*2` in int, and three attacker-controlled int32s do
	 * not fit an int product: the multiplication is undefined on overflow, and
	 * a wrapped product can agree with both stored fields, after which the
	 * memcpy below copies `size` cells into a vector sized by the wrapped
	 * value. So: reject a negative dimension outright, then compute the count
	 * in int64_t, saturating at a size no length can describe -- which is what
	 * keeps the second multiply in range too, since a dimension of zero is
	 * legal (Table.new(10, 10, 0) round-trips) and the saturation therefore
	 * cannot short-circuit on a large intermediate product. Every blob a
	 * serialize() can produce still passes: len is an int, so a real table has
	 * at most (len - 20) / 2 cells. */
	if (x < 0 || y < 0 || z < 0)
		throw Exception(Exception::RGSSError, "Marshal: Table: bad file format");

	static const int64_t countCap = (int64_t) 1 << 31;

	int64_t count = (int64_t) x * y;

	if (count > countCap)
		count = countCap;

	count *= z;

	if (count > countCap)
		count = countCap;

	if (count != size)
		throw Exception(Exception::RGSSError, "Marshal: Table: bad file format");

	if ((int64_t) len != 20 + count*2)
		throw Exception(Exception::RGSSError, "Marshal: Table: bad file format");

	/* The checks above leave `count` no larger than (len - 20) / 2, so a header
	 * that reaches here describes a table this build could also have built from
	 * a script -- unless it is bigger than the cell limit tableCellCount above
	 * enforces, in which case the constructor rejects it rather than allocating
	 * it. Nothing that was already "bad file format" changes type: this second
	 * gate only ever fires on a header the size and length checks accepted. */
	Table *t = new Table(x, y, z);
	memcpy(dataPtr(t->data), data, sizeof(int16_t)*size);

	return t;
}
