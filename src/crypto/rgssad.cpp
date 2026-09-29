/*
** rgssad.cpp
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

#include "rgssad.h"
#include "boost-hash.h"

#include <stdint.h>
#include <string.h>

#include <string>

/* Equivalent Linear Congruential Generator (LCG) constants for iteration 2^n
 * all the way up to 2^32/4 (the largest dword offset possible in
 * RGSS{AD,[23]A}).
 *
 * This table can be easily calculated by taking the original LCG parameter
 * m[0] and a[0] and write them down as LCG_TABLE[0]. Then for the rest of
 * the 29 entries, LGC_TABLE[n] = {m[n], a[n]} where m[n] = pow(m[n-1], 2)
 * and a[n] = a[n-1] * (m[n-1] + 1). */
constexpr static uint32_t LCG_TABLE[30][2] = {
    {0x00000007, 0x00000003}, {0x00000031, 0x00000018},
    {0x00000961, 0x000004b0}, {0x0057f6c1, 0x002bfb60},
    {0xa5057d81, 0xd282bec0}, {0x6e913b01, 0x37489d80},
    {0xc0bb7601, 0x605dbb00}, {0x1bdaec01, 0x8ded7600},
    {0x0145d801, 0x80a2ec00}, {0x28cbb001, 0x1465d800},
    {0xea976001, 0x754bb000}, {0x392ec001, 0x1c976000},
    {0x025d8001, 0x012ec000}, {0x44bb0001, 0x225d8000},
    {0x89760001, 0xc4bb0000}, {0x12ec0001, 0x89760000},
    {0x25d80001, 0x12ec0000}, {0x4bb00001, 0x25d80000},
    {0x97600001, 0x4bb00000}, {0x2ec00001, 0x97600000},
    {0x5d800001, 0x2ec00000}, {0xbb000001, 0x5d800000},
    {0x76000001, 0xbb000000}, {0xec000001, 0x76000000},
    {0xd8000001, 0xec000000}, {0xb0000001, 0xd8000000},
    {0x60000001, 0xb0000000}, {0xc0000001, 0x60000000},
    {0x80000001, 0xc0000000}, {0x00000001, 0x80000000},
};

struct RGSS_entryData
{
	int64_t offset;
	uint64_t size;
	uint32_t startMagic;
};

struct RGSS_entryHandle
{
	const RGSS_entryData data;
	uint32_t currentMagic;
	uint64_t currentOffset;
	PHYSFS_Io *io;

	RGSS_entryHandle(const RGSS_entryData &data, PHYSFS_Io *archIo)
	    : data(data),
	      currentMagic(data.startMagic),
	      currentOffset(0)
	{
		/* May come back null: PHYSFS_Io::duplicate is allowed to fail,
		 * so every caller has to check before using the handle. */
		io = archIo->duplicate(archIo);
	}

	/* `io` is owned by this handle. A copy would hand the same stream to
	 * a second owner and have both destructors destroy it; RGSS_ioDuplicate
	 * goes through the constructor above instead. */
	RGSS_entryHandle(const RGSS_entryHandle &) = delete;
	RGSS_entryHandle &operator=(const RGSS_entryHandle &) = delete;

	~RGSS_entryHandle()
	{
		if (io)
			io->destroy(io);
	}
};

struct RGSS_archiveData
{
	PHYSFS_Io *archiveIo;
	std::string mountName;
	RGSS_archiveData *next = nullptr;
	uint32_t entryCount = 0, indexNodes = 0, pathBytes = 0;

	/* Maps: file path
	 * to:   entry data */
	BoostHash<std::string, RGSS_entryData> entryHash;

	/* Maps: directory path,
	 * to:   list of contained entries */
	BoostHash<std::string, BoostSet<std::string> > dirHash;
};

// Intrusive registration adds no file/OS handles and disappears on unmount.
static RGSS_archiveData *mountedArchives = nullptr;

static void
registerArchive(RGSS_archiveData *data, const char *name)
{
	if (!name || strlen(name) >= 1024)
		return;
	data->mountName = name;
	data->next = mountedArchives;
	mountedArchives = data;
}

static int
RGSS_entryType(const RGSS_archiveData *data, const char *path)
{
	if (data->entryHash.contains(path)) return PHYSFS_FILETYPE_REGULAR;
	if (data->dirHash.contains(path)) return PHYSFS_FILETYPE_DIRECTORY;
	return RGSS_PATH_ABSENT;
}

int
RGSS_pathType(const char *archive, const char *path)
{
	for (const RGSS_archiveData *data = mountedArchives; data; data = data->next)
		if (data->mountName == archive) return RGSS_entryType(data, path);
	return RGSS_UNKNOWN_ARCHIVE;
}

struct RGSS_ioSource
{
	PHYSFS_Io *io;
	int64_t readBytes(void *dest, size_t len)
	{
		return io->read(io, dest, len);
	}
};

template<typename Src>
static bool
readUint32From(Src &src, uint32_t &result)
{
	/* Zeroed because a short read leaves bytes untouched and they are
	 * still folded into `result` below. Unsigned because `(char)0x80` is
	 * negative and shifting a negative value is undefined; widened to
	 * uint32_t as well so nothing rests on the promoted int being able to
	 * hold the result. Same value as before for every complete read. */
	unsigned char buff[4] = {0, 0, 0, 0};
	int64_t count = src.readBytes(buff, 4);

	result = ((uint32_t) buff[0] << 0x00) |
	         ((uint32_t) buff[1] << 0x08) |
	         ((uint32_t) buff[2] << 0x10) |
	         ((uint32_t) buff[3] << 0x18) ;

	return (count == 4);
}

static bool
readUint32(PHYSFS_Io *io, uint32_t &result)
{
	RGSS_ioSource src = { io };
	return readUint32From(src, result);
}

/* The version 1/2 index interleaves entry headers with entry data, so it
 * cannot be slurped whole. Headers and names arrive as 4-byte and 1-byte
 * reads, and each PHYSFS_Io read is one card access on the Vita (Middens'
 * 300 MB archive spent ~12 s of tiny reads in this loop). This reader
 * refills one bounded buffer per span of sequential index bytes; the
 * entry-payload skip seeks inside the buffered window for free and drops
 * the buffer only when a payload ends past it. A failed allocation
 * degrades to unbuffered reads, never to a failed mount. Read counts keep
 * PHYSFS_Io::read's contract, including short counts at end of file. */
struct RGSS_indexReader
{
	static const size_t CAPACITY = 64 * 1024;

	PHYSFS_Io *io;
	unsigned char *buffer;
	/* Archive offset of buffer[0]; with no buffer, the logical position. */
	int64_t bufferStart;
	/* Underlying position, tracked only while a buffer is held; it equals
	 * the logical position at every loop top, because each iteration ends
	 * with either a refill at end of file or a seek past the buffer. */
	int64_t physical;
	size_t bufferLen, bufferPos;

	explicit RGSS_indexReader(PHYSFS_Io *archiveIo)
	    : io(archiveIo), buffer(0), bufferStart(8), physical(8),
	      bufferLen(0), bufferPos(0)
	{
		/* verifyHeader consumed exactly the 8 header bytes unbuffered, so
		 * a file this archiver does not claim is left as it found it. */
		buffer = static_cast<unsigned char*>(
		    PHYSFS_getAllocator()->Malloc(CAPACITY));
	}

	~RGSS_indexReader()
	{
		if (buffer)
			PHYSFS_getAllocator()->Free(buffer);
	}

	RGSS_indexReader(const RGSS_indexReader &) = delete;
	RGSS_indexReader &operator=(const RGSS_indexReader &) = delete;

	bool seek(int64_t offset)
	{
		if (buffer && offset >= bufferStart &&
		    offset <= bufferStart + static_cast<int64_t>(bufferLen))
		{
			bufferPos = static_cast<size_t>(offset - bufferStart);
			return true;
		}
		if (!io->seek(io, static_cast<PHYSFS_uint64>(offset)))
			return false;
		physical = offset;
		bufferStart = offset;
		bufferLen = bufferPos = 0;
		return true;
	}

	int64_t tell() const
	{
		return bufferStart + static_cast<int64_t>(bufferPos);
	}

	int64_t readBytes(void *dest, size_t len)
	{
		if (!buffer)
		{
			int64_t count = io->read(io, dest, len);
			if (count > 0)
				bufferStart += count;
			return count;
		}

		unsigned char *out = static_cast<unsigned char*>(dest);
		size_t done = 0;
		while (done < len)
		{
			if (bufferPos == bufferLen)
			{
				int64_t target = bufferStart + static_cast<int64_t>(bufferLen);
				if (physical != target &&
				    !io->seek(io, static_cast<PHYSFS_uint64>(target)))
					break;
				physical = target;
				bufferStart = target;
				PHYSFS_sint64 got = io->read(io, buffer, CAPACITY);
				if (got <= 0)
				{
					if (done == 0 && got < 0)
						return -1;
					break;
				}
				bufferLen = static_cast<size_t>(got);
				bufferPos = 0;
			}
			size_t take = bufferLen - bufferPos;
			if (take > len - done)
				take = len - done;
			memcpy(out + done, buffer + bufferPos, take);
			bufferPos += take;
			done += take;
		}
		return static_cast<int64_t>(done);
	}
};

#define RGSS_HEADER "RGSSAD"
#define RGSS_MAGIC 0xDEADCAFE

#define PHYSFS_ALLOC(type) \
	static_cast<type*>(PHYSFS_getAllocator()->Malloc(sizeof(type)))

#define IO_READ(io, dest, size) (io->read(io, dest, size) == size)

static inline uint32_t
advanceMagic(uint32_t &magic)
{
	uint32_t old = magic;

	magic = magic * 7 + 3;

	return old;
}

static inline uint32_t
advanceMagicN(uint32_t &magic, uint32_t n) {
    uint32_t old = magic;
    int table_index = 0;

    while (n != 0) {
        if (n & 1) {
            magic = magic * LCG_TABLE[table_index][0] + LCG_TABLE[table_index][1];
        }
        n >>= 1;
        table_index++;
    }

    return old;
}

static PHYSFS_sint64
RGSS_ioRead(PHYSFS_Io *self, void *buffer, PHYSFS_uint64 len)
{
	RGSS_entryHandle *entry = static_cast<RGSS_entryHandle*>(self->opaque);

	PHYSFS_Io *io = entry->io;
	uint64_t offs = entry->currentOffset;
	uint64_t toRead = std::min<uint64_t>(entry->data.size - offs, len);
	if (toRead == 0)
		return 0;

	if (!io->seek(io, entry->data.offset + offs))
	{
		PHYSFS_setErrorCode(PHYSFS_ERR_IO);
		return -1;
	}

	uint8_t *bytes = static_cast<uint8_t*>(buffer);
	PHYSFS_sint64 count = io->read(io, bytes, toRead);
	if (count < 0)
	{
		memset(bytes, 0, toRead);
		PHYSFS_setErrorCode(PHYSFS_ERR_IO);
		return -1;
	}
	if (static_cast<uint64_t>(count) < toRead)
	{
		memset(bytes + count, 0, toRead - count);
		PHYSFS_setErrorCode(PHYSFS_ERR_CORRUPT);
	}

	uint64_t remaining = count;
	while (remaining && (offs & 3))
	{
		*bytes++ ^= entry->currentMagic >> (8 * (offs & 3));
		--remaining;
		if ((++offs & 3) == 0)
			advanceMagic(entry->currentMagic);
	}

	/* The caller's buffer may be unaligned, even at an aligned file offset. */
	while (remaining >= 4)
	{
		uint32_t dword;
		memcpy(&dword, bytes, 4);
		dword ^= advanceMagic(entry->currentMagic);
		memcpy(bytes, &dword, 4);
		bytes += 4;
		remaining -= 4;
	}
	for (unsigned i = 0; i < remaining; ++i)
		bytes[i] ^= entry->currentMagic >> (8 * i);

	entry->currentOffset += count;
	return count;
}

static int
RGSS_ioSeek(PHYSFS_Io *self, PHYSFS_uint64 offset)
{
	RGSS_entryHandle *entry = static_cast<RGSS_entryHandle*>(self->opaque);

	/* Entry sizes are uint32_t on disk: this also bounds LCG_TABLE indices. */
	if (offset > entry->data.size)
	{
		PHYSFS_setErrorCode(PHYSFS_ERR_IO);
		return 0;
	}
	if (!entry->io->seek(entry->io, entry->data.offset + offset))
	{
		PHYSFS_setErrorCode(PHYSFS_ERR_IO);
		return 0;
	}

	if (offset < entry->currentOffset)
	{
		entry->currentOffset = 0;
		entry->currentMagic = entry->data.startMagic;
	}

	uint64_t dwordsSought = offset / 4 - entry->currentOffset / 4;
	advanceMagicN(entry->currentMagic, static_cast<uint32_t>(dwordsSought));
	entry->currentOffset = offset;
	return 1;
}

static PHYSFS_sint64
RGSS_ioTell(PHYSFS_Io *self)
{
	const RGSS_entryHandle *entry = static_cast<RGSS_entryHandle*>(self->opaque);

	return entry->currentOffset;
}

static PHYSFS_sint64
RGSS_ioLength(PHYSFS_Io *self)
{
	const RGSS_entryHandle *entry = static_cast<RGSS_entryHandle*>(self->opaque);

	return entry->data.size;
}

static PHYSFS_Io*
RGSS_ioDuplicate(PHYSFS_Io *self)
{
	const RGSS_entryHandle *entry = static_cast<RGSS_entryHandle*>(self->opaque);

	/* Copying the handle would share `io` between the original and the
	 * duplicate, and destroying either would free it under the other.
	 * Take a stream of our own and carry the decrypt cursor across, which
	 * is what leaves the duplicate reading where the original stands. */
	RGSS_entryHandle *entryDup = new RGSS_entryHandle(entry->data, entry->io);

	if (!entryDup->io)
	{
		delete entryDup;
		PHYSFS_setErrorCode(PHYSFS_ERR_IO);
		return 0;
	}

	entryDup->currentMagic = entry->currentMagic;
	entryDup->currentOffset = entry->currentOffset;

	PHYSFS_Io *dup = PHYSFS_ALLOC(PHYSFS_Io);
	if (!dup)
	{
		delete entryDup;
		PHYSFS_setErrorCode(PHYSFS_ERR_OUT_OF_MEMORY);
		return 0;
	}
	*dup = *self;
	dup->opaque = entryDup;

	return dup;
}

static void
RGSS_ioDestroy(PHYSFS_Io *self)
{
	RGSS_entryHandle *entry = static_cast<RGSS_entryHandle*>(self->opaque);

	delete entry;

	PHYSFS_getAllocator()->Free(self);
}

static const PHYSFS_Io RGSS_IoTemplate =
{
    0, /* version */
    0, /* opaque */
    RGSS_ioRead,
    0, /* write */
    RGSS_ioSeek,
    RGSS_ioTell,
    RGSS_ioLength,
    RGSS_ioDuplicate,
    0, /* flush */
    RGSS_ioDestroy
};

static void
processDirectories(RGSS_archiveData *data, BoostSet<std::string> &topLevel,
                   char *nameBuf, uint32_t nameLen)
{
	/* Check for top level entries */
	for (uint32_t i = 0; i < nameLen; ++i)
	{
		bool slash = nameBuf[i] == '/';
		if (!slash && i+1 < nameLen)
			continue;

		if (slash)
			nameBuf[i] = '\0';

		topLevel.insert(nameBuf);

		if (slash)
			nameBuf[i] = '/';

		break;
	}

	/* Check for more entries */
	for (uint32_t i = nameLen; i > 0; i--)
		if (nameBuf[i] == '/')
		{
			nameBuf[i] = '\0';

			const char *dir = nameBuf;
			const char *entry = &nameBuf[i+1];

			BoostSet<std::string> &entryList = data->dirHash[dir];
			entryList.insert(entry);
		}
}

static bool
validEntryRange(const RGSS_entryData &entry, int64_t minimum, int64_t archiveSize)
{
	return minimum >= 0 && entry.offset >= minimum && entry.offset <= archiveSize &&
	       entry.size <= static_cast<uint64_t>(archiveSize - entry.offset);
}

static bool
indexEntry(RGSS_archiveData *data, BoostSet<std::string> &topLevel,
           const RGSS_entryData &entry, char *name, uint32_t nameLen)
{
	uint32_t directories = 0;
	for (uint32_t i = 0; i < nameLen; ++i)
		if (name[i] == '/')
			++directories;

	/* Charge even duplicate paths: bound table rows, map/set nodes and copied
	 * strings independently, including the expansion of deeply nested names. */
	uint32_t nodes = 2 + 2 * directories;
	uint32_t bytes = (nameLen + 1) * (2 + directories);
	if (data->entryCount == 16384 || data->indexNodes > 65536 - nodes ||
	    data->pathBytes > 8 * 1024 * 1024 - bytes)
		return false;

	++data->entryCount;
	data->indexNodes += nodes;
	data->pathBytes += bytes;
	data->entryHash.insert(name, entry);
	processDirectories(data, topLevel, name, nameLen);
	return true;
}

static bool
verifyHeader(PHYSFS_Io *io, char version)
{
	char header[8];

	if (!IO_READ(io, header, sizeof(header)))
		return false;

	if (strcmp(header, RGSS_HEADER))
		return false;

	if (header[7] != version)
		return false;

	return true;
}

static void*
RGSS_openArchive(PHYSFS_Io *io, const char *name, int forWrite, int *claimed)
{
	if (forWrite)
		return NULL;

	/* Version 1 */
	if (!verifyHeader(io, 1))
		return NULL;
	else
		*claimed = 1;

	int64_t archiveSize = io->length(io);
	if (archiveSize < 8)
	{
		PHYSFS_setErrorCode(PHYSFS_ERR_CORRUPT);
		return NULL;
	}

	RGSS_archiveData *data = new RGSS_archiveData;
	data->archiveIo = io;

	uint32_t magic = RGSS_MAGIC;

	/* Top level entry list */
	BoostSet<std::string> &topLevel = data->dirHash[""];

	RGSS_indexReader reader(io);

	while (true)
	{
		int64_t tableOffset = reader.tell();
		if (tableOffset == archiveSize)
			break;
		if (tableOffset < 8 || tableOffset > archiveSize)
			goto error;

		uint32_t nameLen;
		if (!readUint32From(reader, nameLen))
			goto error;

		nameLen ^= advanceMagic(magic);

		static char nameBuf[512];

		/* nameLen is a decrypted field of the archive, i.e. any 32 bit
		 * number at all once the file is corrupt. The terminator below
		 * needs one byte of its own, so 512 is already one too many.
		 * A bad archive has to fail the mount, not run off nameBuf. */
		if (nameLen >= sizeof(nameBuf))
			goto error;

		for (uint32_t i = 0; i < nameLen; ++i)
		{
			char c;

			/* Stop on a truncated name rather than decrypt
			 * whatever `c` happens to hold. */
			if (reader.readBytes(&c, 1) != 1)
				goto error;

			nameBuf[i] = c ^ (advanceMagic(magic) & 0xFF);
			if (nameBuf[i] == '\\')
				nameBuf[i] = '/';
		}

		nameBuf[nameLen] = '\0';

		uint32_t entrySize;

		if (!readUint32From(reader, entrySize))
			goto error;

		entrySize ^= advanceMagic(magic);

		RGSS_entryData entry;
		entry.offset = reader.tell();
		entry.size = entrySize;
		entry.startMagic = magic;

		if (!validEntryRange(entry, 8, archiveSize) ||
		    !reader.seek(entry.offset + entry.size) ||
		    !indexEntry(data, topLevel, entry, nameBuf, nameLen))
			goto error;
	}

	registerArchive(data, name);
	return data;
error:
	PHYSFS_setErrorCode(PHYSFS_ERR_CORRUPT);
	delete data;
	return NULL;
}

static PHYSFS_EnumerateCallbackResult
RGSS_enumerateFiles(void *opaque, const char *dirname,
                    PHYSFS_EnumerateCallback cb,
                    const char *origdir, void *callbackdata)
{
	RGSS_archiveData *data = static_cast<RGSS_archiveData*>(opaque);

	std::string _dirname(dirname);

	if (!data->dirHash.contains(_dirname))
		return PHYSFS_ENUM_STOP;

	const BoostSet<std::string> &entries = data->dirHash[_dirname];

	BoostSet<std::string>::const_iterator iter;
	for (iter = entries.cbegin(); iter != entries.cend(); ++iter)
	{
		PHYSFS_EnumerateCallbackResult result = cb(callbackdata, origdir, iter->c_str());
		if (result == PHYSFS_ENUM_ERROR)
			PHYSFS_setErrorCode(PHYSFS_ERR_APP_CALLBACK);
		if (result != PHYSFS_ENUM_OK)
			return result;
	}

	return PHYSFS_ENUM_OK;
}

static PHYSFS_Io*
RGSS_openRead(void *opaque, const char *filename)
{
	RGSS_archiveData *data = static_cast<RGSS_archiveData*>(opaque);

	if (!data->entryHash.contains(filename))
		return 0;

	RGSS_entryHandle *entry =
	        new RGSS_entryHandle(data->entryHash[filename], data->archiveIo);

	if (!entry->io)
	{
		delete entry;
		PHYSFS_setErrorCode(PHYSFS_ERR_IO);
		return 0;
	}

	PHYSFS_Io *io = PHYSFS_ALLOC(PHYSFS_Io);
	if (!io)
	{
		delete entry;
		PHYSFS_setErrorCode(PHYSFS_ERR_OUT_OF_MEMORY);
		return 0;
	}

	*io = RGSS_IoTemplate;
	io->opaque = entry;

	return io;
}

static int
RGSS_stat(void *opaque, const char *filename, PHYSFS_Stat *stat)
{
	RGSS_archiveData *data = static_cast<RGSS_archiveData*>(opaque);

	const int type = RGSS_entryType(data, filename);

	if (type == RGSS_PATH_ABSENT)
	{
		PHYSFS_setErrorCode(PHYSFS_ERR_NOT_FOUND);
		return 0;
	}

	stat->modtime    =
	stat->createtime =
	stat->accesstime = 0;
	stat->readonly   = 1;

	if (type == PHYSFS_FILETYPE_REGULAR)
	{
		const RGSS_entryData &entry = data->entryHash[filename];

		stat->filesize = entry.size;
		stat->filetype = PHYSFS_FILETYPE_REGULAR;
	}
	else
	{
		stat->filesize = 0;
		stat->filetype = PHYSFS_FILETYPE_DIRECTORY;
	}

	return 1;
}

static void
RGSS_closeArchive(void *opaque)
{
	RGSS_archiveData *data = static_cast<RGSS_archiveData*>(opaque);
	RGSS_archiveData **link = &mountedArchives;
	while (*link && *link != data) link = &(*link)->next;
	if (*link) *link = data->next;
	delete data;
}

static PHYSFS_Io*
RGSS_noop1(void*, const char*)
{
	return 0;
}

static int
RGSS_noop2(void*, const char*)
{
	return 0;
}

const PHYSFS_Archiver RGSS1_Archiver =
{
	0,
	{
		"RGSSAD",
		"RGSS encrypted archive format",
		"", /* Author */
		"", /* Website */
		0 /* symlinks not supported */
	},
	RGSS_openArchive,
	RGSS_enumerateFiles,
	RGSS_openRead,
	RGSS_noop1, /* openWrite */
	RGSS_noop1, /* openAppend */
	RGSS_noop2, /* remove */
	RGSS_noop2, /* mkdir */
	RGSS_stat,
	RGSS_closeArchive
};

const PHYSFS_Archiver RGSS2_Archiver =
{
	0,
	{
		"RGSS2A",
		"RGSS2 encrypted archive format",
		"", /* Author */
		"", /* Website */
		0 /* symlinks not supported */
	},
	RGSS_openArchive,
	RGSS_enumerateFiles,
	RGSS_openRead,
	RGSS_noop1, /* openWrite */
	RGSS_noop1, /* openAppend */
	RGSS_noop2, /* remove */
	RGSS_noop2, /* mkdir */
	RGSS_stat,
	RGSS_closeArchive
};

static bool
readUint32AndXor(PHYSFS_Io *io, uint32_t &result, uint32_t key)
{
	if (!readUint32(io, result))
		return false;

	result ^= key;

	return true;
}

static void*
RGSS3_openArchive(PHYSFS_Io *io, const char *name, int forWrite, int *claimed)
{
	if (forWrite)
		return NULL;

	/* Version 3 */
	if (!verifyHeader(io, 3))
		return NULL;
	else
		*claimed = 1;

	int64_t archiveSize = io->length(io);
	uint32_t baseMagic;

	if (archiveSize < 16 || !readUint32(io, baseMagic))
	{
		PHYSFS_setErrorCode(PHYSFS_ERR_CORRUPT);
		return NULL;
	}
	int64_t firstDataOffset = archiveSize;

	baseMagic = (baseMagic * 9) + 3;

	RGSS_archiveData *data = new RGSS_archiveData;
	data->archiveIo = io;

	/* Top level entry list */
	BoostSet<std::string> &topLevel = data->dirHash[""];

	while (true)
	{
		uint32_t offset, size, magic, nameLen;

		if (!readUint32AndXor(io, offset, baseMagic))
			goto error;

		/* Zero offset means entry list has ended */
		if (offset == 0)
		{
			int64_t tableEnd = io->tell(io);
			if (tableEnd < 16 || tableEnd > firstDataOffset)
				goto error;
			break;
		}

		if (!readUint32AndXor(io, size, baseMagic))
			goto error;

		if (!readUint32AndXor(io, magic, baseMagic))
			goto error;

		if (!readUint32AndXor(io, nameLen, baseMagic))
			goto error;

		char nameBuf[512];

		/* Same bound as the version 1 path: nameLen comes out of the
		 * file, and the terminator below claims the last byte. */
		if (nameLen >= sizeof(nameBuf))
		{
			PHYSFS_setErrorCode(PHYSFS_ERR_CORRUPT);
			goto error;
		}

		if (!IO_READ(io, nameBuf, nameLen))
			goto error;

		for (uint32_t i = 0; i < nameLen; ++i)
		{
			nameBuf[i] ^= ((baseMagic >> 8*(i%4)) & 0xFF);

			if (nameBuf[i] == '\\')
				nameBuf[i] = '/';
		}

		nameBuf[nameLen] = '\0';

		RGSS_entryData entry;
		entry.offset = offset;
		entry.size = size;
		entry.startMagic = magic;

		if (!validEntryRange(entry, 12, archiveSize))
			goto error;
		firstDataOffset = std::min(firstDataOffset, entry.offset);
		if (!indexEntry(data, topLevel, entry, nameBuf, nameLen))
			goto error;

		continue;

	error:
		PHYSFS_setErrorCode(PHYSFS_ERR_CORRUPT);
		delete data;
		return NULL;
	}

	registerArchive(data, name);
	return data;
}

const PHYSFS_Archiver RGSS3_Archiver =
{
	0,
	{
		"RGSS3A",
		"RGSS3 encrypted archive format",
		"", /* Author */
		"", /* Website */
		0 /* symlinks not supported */
	},
	RGSS3_openArchive,
	RGSS_enumerateFiles,
	RGSS_openRead,
	RGSS_noop1, /* openWrite */
	RGSS_noop1, /* openAppend */
	RGSS_noop2, /* remove */
	RGSS_noop2, /* mkdir */
	RGSS_stat,
	RGSS_closeArchive
};
