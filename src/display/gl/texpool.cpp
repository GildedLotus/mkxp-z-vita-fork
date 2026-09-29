/*
** texpool.cpp
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

#include "texpool.h"
#include "exception.h"
#include "sharedstate.h"
#include "glstate.h"
#include "boost-hash.h"
#include "debugwriter.h"

#include <list>
#include <utility>
#include <assert.h>
#include <string.h>

typedef std::pair<uint16_t, uint16_t> Size;

static uint32_t byteCount(Size &s)
{
	return s.first * s.second * 4;
}

struct CacheNode
{
	TEXFBO obj;
	std::list<TEXFBO>::iterator prioIter;

	bool operator==(const CacheNode &o) const
	{
		return obj == o.obj;
	}
};

typedef std::list<CacheNode> CNodeList;

struct TexPoolPrivate
{
	/* Contains all cached TexFBOs, grouped by size */
	BoostHash<Size, CNodeList> poolHash;

	/* Contains all cached TexFBOs, sorted by release time */
	std::list<TEXFBO> priorityQueue;

	/* Maximal allowed cache memory */
	const uint32_t maxMemSize;

	/* Current amound of memory consumed by the cache */
	uint32_t memSize;

	/* Current amount of TexFBOs cached */
	uint16_t objCount;

	/* Has this pool been disabled? */
	bool disabled;

	TexPoolPrivate(uint32_t maxMemSize)
	    : maxMemSize(maxMemSize),
	      memSize(0),
	      objCount(0),
	      disabled(false)
	{}
};

TexPool::TexPool(uint32_t maxMemSize)
{
	p = new TexPoolPrivate(maxMemSize);
}

TexPool::~TexPool()
{
	std::list<TEXFBO>::iterator iter;

	for (iter = p->priorityQueue.begin();
	     iter != p->priorityQueue.end();
	     ++iter)
	{
		TEXFBO obj = *iter;
		TEXFBO::fini(obj);
		--p->objCount;
	}

	assert(p->objCount == 0);

	delete p;
}

TEXFBO TexPool::request(int width, int height)
{
#ifdef MKXPZ_SOFTWARE_BITMAPS
	/* Every TEXFBO this pool hands out is a render target: it calls
	 * TEXFBO::init + linkFBO on a miss, and caches the surface between users
	 * on a hit. The software Bitmap, window base and tile atlas paths removed its
	 * last caller under this backend, so a call arriving here is a regression
	 * that would quietly start consuming the small fixed set of render
	 * surfaces the whole process gets. Refuse it
	 * where it happens rather than three frames later in the driver. */
	throw Exception(Exception::MKXPError,
	                "software_bitmaps: TexPool::request(%d, %d): this backend "
	                "must not allocate render targets outside the fixed set "
	                "reserved at boot",
	                width, height);
#else
	CacheNode cnode;
	TEXFBO::trace("pool request", cnode.obj, width, height);
	Size size(width, height);

	/* See if we can statisfy request from cache */
	CNodeList &bucket = p->poolHash[size];

	if (!bucket.empty())
	{
		/* Found one! */
		cnode = bucket.back();
		bucket.pop_back();

		p->priorityQueue.erase(cnode.prioIter);

		p->memSize -= byteCount(size);
		--p->objCount;

//		Debug() << "TexPool: <?+> (" << width << height << ")";

		TEXFBO::trace("pool HIT", cnode.obj);
		return cnode.obj;
	}

	int maxSize = glState.caps.maxTexSize;
	if (width > maxSize || height > maxSize)
		throw Exception(Exception::MKXPError,
		                "Texture dimensions [%d, %d] exceed hardware capabilities",
		                width, height);

	TEXFBO::trace("pool MISS", cnode.obj, width, height);
	/* Nope, create it instead */
	TEXFBO::init(cnode.obj);
	TEXFBO::allocEmpty(cnode.obj, width, height);
	TEXFBO::linkFBO(cnode.obj);

//	Debug() << "TexPool: <?-> (" << width << height << ")";

	return cnode.obj;
#endif
}

void TexPool::release(TEXFBO &obj)
{
	TEXFBO::trace("pool release", obj);
	if (obj.tex == TEX::ID(0) || obj.fbo == FBO::ID(0))
	{
		TEXFBO::fini(obj);
		return;
	}

	if (p->disabled)
	{
		/* If we're disabled, delete without caching */
//		Debug() << "TexPool: <!#> (" << obj.width << obj.height << ")";
		TEXFBO::fini(obj);
		return;
	}

	Size size(obj.width, obj.height);

	uint32_t newMemSize = p->memSize + byteCount(size);

	/* If caching this object would spill over the allowed memory budget,
	 * delete least used objects until we're good again */
	while (newMemSize > p->maxMemSize)
	{
		if (p->objCount == 0)
			break;

//		Debug() << "TexPool: <!~> Size:" << p->memSize;

		/* Retrieve object with lowest priority for deletion */
		CacheNode last;
		last.obj = p->priorityQueue.back();
		Size removedSize(last.obj.width, last.obj.height);

		CNodeList &bucket = p->poolHash[removedSize];

		std::list<CacheNode>::iterator toRemove =
		        std::find(bucket.begin(), bucket.end(), last);
		assert(toRemove != bucket.end());
		bucket.erase(toRemove);

		p->priorityQueue.pop_back();

		TEXFBO::trace("pool EVICT", last.obj);
		TEXFBO::fini(last.obj);

		newMemSize -= byteCount(removedSize);
		p->memSize -= byteCount(removedSize);
		--p->objCount;

//		Debug() << "TexPool: <!-> (" << last.obj.width << last.obj.height << ")";
	}

	/* Returning an object must also work while unwinding an allocation failure. */
	try
	{
		CNodeList &bucket = p->poolHash[size];
		p->priorityQueue.push_front(obj);
		CacheNode cnode;
		cnode.obj = obj;
		cnode.prioIter = p->priorityQueue.begin();
		try { bucket.push_back(cnode); }
		catch (...) { p->priorityQueue.pop_front(); throw; }
	}
	catch (...)
	{
		TEXFBO::fini(obj);
		return;
	}
	p->memSize = newMemSize;

	++p->objCount;
	TEXFBO::trace("pool CACHED", obj);

//	Debug() << "TexPool: <!+> (" << obj.width << obj.height << ") Current size:" << p->memSize;
}

void TexPool::disable()
{
	p->disabled = true;
}


