/*
** filesystem.cpp
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

#include "bootprofile.h"

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "frameprofile.h"
#endif
#include "filesystem.h"

#include "util/boost-hash.h"
#include "util/debugwriter.h"
#include "util/exception.h"
#include "util/util.h"
#include "display/font.h"
#include "crypto/rgssad.h"

#include "eventthread.h"
#include "sharedstate.h"
#include "vita_fatal.h"

#include <physfs.h>

#include <algorithm>
#include <exception>
#include <new>
#include <stack>
#include <set>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#ifdef __vita__
#include "vita-cache-types.h"
#endif

#ifdef __APPLE__
#include <iconv.h>
#endif

#ifdef __WIN32__
#include <direct.h>
#endif

struct SDLRWIoContext {
  SDL_RWops *ops;
  std::string filename;

  SDLRWIoContext(const char *filename)
      : ops(SDL_RWFromFile(filename, "r")), filename(filename) {
    if (!ops)
      throw Exception(Exception::SDLError, "Failed to open file: %s",
                      SDL_GetError());
  }

  ~SDLRWIoContext() { SDL_RWclose(ops); }
};

static PHYSFS_Io *createSDLRWIo(const char *filename);

static SDL_RWops *getSDLRWops(PHYSFS_Io *io) {
  return static_cast<SDLRWIoContext *>(io->opaque)->ops;
}

static PHYSFS_sint64 SDLRWIoRead(struct PHYSFS_Io *io, void *buf,
                                 PHYSFS_uint64 len) {
  return SDL_RWread(getSDLRWops(io), buf, 1, len);
}

static int SDLRWIoSeek(struct PHYSFS_Io *io, PHYSFS_uint64 offset) {
  return (SDL_RWseek(getSDLRWops(io), offset, RW_SEEK_SET) != -1);
}

static PHYSFS_sint64 SDLRWIoTell(struct PHYSFS_Io *io) {
  return SDL_RWseek(getSDLRWops(io), 0, RW_SEEK_CUR);
}

static PHYSFS_sint64 SDLRWIoLength(struct PHYSFS_Io *io) {
  return SDL_RWsize(getSDLRWops(io));
}

static struct PHYSFS_Io *SDLRWIoDuplicate(struct PHYSFS_Io *io) {
  SDLRWIoContext *ctx = static_cast<SDLRWIoContext *>(io->opaque);
  int64_t offset = io->tell(io);
  PHYSFS_Io *dup = createSDLRWIo(ctx->filename.c_str());

  if (dup)
    SDLRWIoSeek(dup, offset);

  return dup;
}

static void SDLRWIoDestroy(struct PHYSFS_Io *io) {
  delete static_cast<SDLRWIoContext *>(io->opaque);
  delete io;
}

static PHYSFS_Io SDLRWIoTemplate = {0,
                                    0, /* version, opaque */
                                    SDLRWIoRead,
                                    0, /* write */
                                    SDLRWIoSeek,
                                    SDLRWIoTell,
                                    SDLRWIoLength,
                                    SDLRWIoDuplicate,
                                    0, /* flush */
                                    SDLRWIoDestroy};

static PHYSFS_Io *createSDLRWIo(const char *filename) {
  SDLRWIoContext *ctx;

  try {
    ctx = new SDLRWIoContext(filename);
  } catch (const Exception &) {
    // PhysFS calls SDLRWIoDuplicate, so this handler runs in its C frames
    // and must not allocate: Debug() builds a stringstream.
    vitaLogMessage("Failed mounting ", filename);
    return 0;
  } catch (...) {
    return 0;  // PhysFS calls SDLRWIoDuplicate: nothing may unwind into it
  }

  PHYSFS_Io *io = new (std::nothrow) PHYSFS_Io;
  if (!io) {
    delete ctx;
    return 0;
  }
  *io = SDLRWIoTemplate;
  io->opaque = ctx;

  return io;
}

/* A read handle opened before a standby can be stale afterwards (BGM and the
 * log stopped on device). This keeps what a reopen needs:
 * the PhysFS path and the logical position. Handles opened without a path
 * keep the plain PHYSFS_File in data1. */
static const Uint32 SDL_RWOPS_PHYSFS_RECOVER = SDL_RWOPS_UNKNOWN + 11;

struct ReadRecover {
  PHYSFS_File *file;
  int64_t pos;
  unsigned epoch;
  char path[512];
};

static PHYSFS_File *openBufferedRead(const char *path);

static inline ReadRecover *sdlRecover(SDL_RWops *ops) {
  return ops->type == SDL_RWOPS_PHYSFS_RECOVER
             ? static_cast<ReadRecover *>(ops->hidden.unknown.data1)
             : nullptr;
}

static inline PHYSFS_File *sdlPHYS(SDL_RWops *ops) {
  ReadRecover *r = sdlRecover(ops);
  return r ? r->file : static_cast<PHYSFS_File *>(ops->hidden.unknown.data1);
}

static inline unsigned resumeEpoch() {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  return vita_glue_resume_epoch();
#else
  return 0;
#endif
}

/* Swap in a fresh handle at `pos`. A failed reopen keeps the old handle. */
static bool reopenRead(ReadRecover *r, int64_t pos, const char *why) {
  static unsigned reported = 0;
  const char *error = PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode());
  PHYSFS_File *fresh = openBufferedRead(r->path);

  if (fresh && pos > 0 && !PHYSFS_seek(fresh, pos)) {
    PHYSFS_close(fresh);
    fresh = nullptr;
  }
  if (reported < 16) {
    ++reported;
    Debug() << "read handle reopen" << (fresh ? "ok" : "FAILED") << why
            << r->path << "pos" << (long long)pos << "last error:"
            << (error ? error : "none");
  }
  if (!fresh)
    return false;
  PHYSFS_close(r->file);
  r->file = fresh;
  return true;
}

/* After a resume, handles that were open across it are replaced before
 * their next use, whether or not the old one still answers. */
static void reopenAfterResume(ReadRecover *r) {
  const unsigned epoch = resumeEpoch();

  if (r->epoch != epoch) {
    r->epoch = epoch;
    reopenRead(r, r->pos, "after resume");
  }
}

/* A read of nothing before the end of the file is a dead handle, not EOF. */
static bool readFailed(ReadRecover *r, PHYSFS_sint64 result, size_t want) {
  if (result < 0)
    return true;
  if (result > 0 || want == 0)
    return false;
  const PHYSFS_sint64 length = PHYSFS_fileLength(r->file);
  return length < 0 || r->pos < length;
}

static Sint64 SDL_RWopsSize(SDL_RWops *ops) {
  ReadRecover *r = sdlRecover(ops);
  PHYSFS_File *f = sdlPHYS(ops);

  if (!f)
    return -1;

  PHYSFS_sint64 length = PHYSFS_fileLength(f);

  if (length < 0 && r && reopenRead(r, r->pos, "size"))
    length = PHYSFS_fileLength(r->file);
  return length;
}

static Sint64 SDL_RWopsSeek(SDL_RWops *ops, int64_t offset, int whence) {
  ReadRecover *r = sdlRecover(ops);
  if (r)
    reopenAfterResume(r);
  PHYSFS_File *f = sdlPHYS(ops);

  if (!f)
    return -1;

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  const bool asset = FrameProfile::assetActive();
#endif
  int64_t base;

  switch (whence) {
  default:
  case RW_SEEK_SET:
    base = 0;
    break;
  case RW_SEEK_CUR:
    base = r ? r->pos : PHYSFS_tell(f);
    break;
  case RW_SEEK_END:
    base = PHYSFS_fileLength(f);
    if (base < 0 && r && reopenRead(r, r->pos, "seek end")) {
      f = r->file;
      base = PHYSFS_fileLength(f);
    }
    if (base < 0)
      return -1;
    break;
  }

  const int64_t target = base + offset;
  int result = PHYSFS_seek(f, target);

  if (!result && r && target >= 0) {
    /* A target past the end fails on a healthy handle too. */
    const PHYSFS_sint64 length = PHYSFS_fileLength(f);
    if ((length < 0 || target <= length) && reopenRead(r, target, "seek"))
      result = 1;
  }
  if (result && r)
    r->pos = target;

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  if (asset) {
    auto &c = FrameProfile::state.assets.get(uintptr_t(ops->hidden.unknown.data2));
    ++c.seeks; c.seekFail += !result;
  }
#endif
  return (result != 0) ? (r ? r->pos : PHYSFS_tell(f)) : -1;
}

static size_t SDL_RWopsRead(SDL_RWops *ops, void *buffer, size_t size,
                            size_t maxnum) {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  FrameProfile::Scope profileRead(FrameProfile::IO, 0, true);
#endif
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  const bool asset = FrameProfile::assetActive();
  const double before = asset ? FrameProfile::state.frameUS[FrameProfile::IO] : 0;
#endif
  ReadRecover *r = sdlRecover(ops);
  if (r)
    reopenAfterResume(r);
  PHYSFS_File *f = sdlPHYS(ops);

  PHYSFS_sint64 result = f ? PHYSFS_readBytes(f, buffer, size * maxnum) : -1;

  if (r && readFailed(r, result, size * maxnum) &&
      reopenRead(r, r->pos, "read"))
    result = PHYSFS_readBytes(r->file, buffer, size * maxnum);
  if (r && result > 0)
    r->pos += result;

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  profileRead.stop();
  if (asset) FrameProfile::state.assets.read(uintptr_t(ops->hidden.unknown.data2),
      size * maxnum, result, FrameProfile::state.frameUS[FrameProfile::IO] - before);
#endif
  return (result != -1) ? (result / size) : 0;
}

static size_t SDL_RWopsWrite(SDL_RWops *ops, const void *buffer, size_t size,
                             size_t num) {
  PHYSFS_File *f = sdlPHYS(ops);

  if (!f)
    return 0;

  PHYSFS_sint64 result = PHYSFS_writeBytes(f, buffer, size * num);

  return (result != -1) ? (result / size) : 0;
}

static int SDL_RWopsClose(SDL_RWops *ops) {
  PHYSFS_File *f = sdlPHYS(ops);

  if (!f)
    return -1;

  int result = PHYSFS_close(f);
  delete sdlRecover(ops);
  ops->hidden.unknown.data1 = 0;

  return (result != 0) ? 0 : -1;
}

static int SDL_RWopsCloseFree(SDL_RWops *ops) {
  int result = SDL_RWopsClose(ops);

  SDL_FreeRW(ops);

  return result;
}

/* Copies the first srcN characters from src into dst,
 * or the full string if srcN == -1. Never writes more
 * than dstMax, and guarantees dst to be null terminated.
 * Returns copied bytes (minus terminating null) */
static size_t strcpySafe(char *dst, const char *src, size_t dstMax, int srcN) {
  if (srcN < 0)
    srcN = strlen(src);

  size_t cpyMax = std::min<size_t>(dstMax - 1, srcN);

  memcpy(dst, src, cpyMax);
  dst[cpyMax] = '\0';

  return cpyMax;
}

/* Attempt to locate an extension string in a filename.
 * Either a pointer into the input string pointing at the
 * extension, or null is returned */
static const char *findExt(const char *filename) {
  size_t len;

  for (len = strlen(filename); len > 0; --len) {
    if (filename[len] == '/')
      return 0;

    if (filename[len] == '.')
      return &filename[len + 1];
  }

  return 0;
}

static PHYSFS_File *openBufferedRead(const char *path) {
  PHYSFS_File *handle = PHYSFS_openRead(path);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#ifndef MKXPZ_VITA_ARCHIVE_READAHEAD
#define MKXPZ_VITA_ARCHIVE_READAHEAD 1
#endif
  if (handle && MKXPZ_VITA_ARCHIVE_READAHEAD) {
    const char *mount = PHYSFS_getRealDir(path);
    if (mount && RGSS_pathType(mount, "") != RGSS_UNKNOWN_ARCHIVE) {
      // PhysFS owns this 64 KiB buffer until close; allocation failure streams.
      const PHYSFS_ErrorCode previous = PHYSFS_getLastErrorCode();
      if (!PHYSFS_setBuffer(handle, 64 * 1024))
        PHYSFS_setErrorCode(previous);
    }
  }
#endif
  return handle;
}

static void initReadOps(PHYSFS_File *handle, SDL_RWops &ops, bool freeOnClose,
                        const char *path = nullptr) {
  ops.size = SDL_RWopsSize;
  ops.seek = SDL_RWopsSeek;
  ops.read = SDL_RWopsRead;
  ops.write = SDL_RWopsWrite;

  if (freeOnClose)
    ops.close = SDL_RWopsCloseFree;
  else
    ops.close = SDL_RWopsClose;

  ops.type = SDL_RWOPS_PHYSFS;
  ops.hidden.unknown.data1 = handle;
  ops.hidden.unknown.data2 = nullptr;

  if (handle && path && strlen(path) < sizeof(ReadRecover::path)) {
    ReadRecover *r = new (std::nothrow) ReadRecover;
    if (r) {
      r->file = handle;
      r->pos = 0;
      r->epoch = resumeEpoch();
      strcpy(r->path, path);
      ops.type = SDL_RWOPS_PHYSFS_RECOVER;
      ops.hidden.unknown.data1 = r;
    }
  }
}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
static unsigned profileAssetOpen(const char *path, bool success, double us) {
  if (!FrameProfile::assetActive()) return 0;
  const char *mount = success ? PHYSFS_getRealDir(path) : nullptr;
  auto &assets = FrameProfile::state.assets;
  const bool archive = mount && RGSS_pathType(mount, "") != RGSS_UNKNOWN_ARCHIVE;
  const unsigned token = assets.identify(path, mount, assets.source(mount, archive));
  auto &c = assets.get(token); ++c.opens; c.openFail += !success; c.ioUS += us;
  return token;
}
#endif

static void strTolower(std::string &str) {
  for (size_t i = 0; i < str.size(); ++i)
    str[i] = tolower(static_cast<unsigned char>(str[i]));
}

const Uint32 SDL_RWOPS_PHYSFS = SDL_RWOPS_UNKNOWN + 10;

struct FileSystemPrivate {
  /* Maps: lower case full filepath,
   * To:   mixed case full filepath */
  BoostHash<std::string, std::string> pathCache;
  /* Maps: lower case directory path,
   * To:   list of lower case filenames */
  BoostHash<std::string, std::vector<std::string>> fileLists;

  /* This is for compatibility with games that take Windows'
   * case insensitivity for granted */
  bool wantPathCache;
  bool havePathCache;

  // Fallback lookups use a bounded FIFO of resolved directories, never file lists.
  static constexpr size_t directoryCaseLimit = 64;
  std::vector<std::pair<std::string, std::string>> directoryCase;
  size_t nextDirectoryCase = 0;

  void clearDirectoryCase() {
    directoryCase.clear();
    nextDirectoryCase = 0;
  }

  const char *cachedDirectoryCase(const std::string &lower) const {
    for (const auto &entry : directoryCase)
      if (entry.first == lower)
        return entry.second.c_str();
    return nullptr;
  }

  void rememberDirectoryCase(const std::string &lower, const char *actual) {
    auto entry = std::make_pair(lower, std::string(actual));
    if (directoryCase.size() < directoryCaseLimit) {
      directoryCase.reserve(directoryCaseLimit);
      directoryCase.push_back(std::move(entry));
    } else {
      directoryCase[nextDirectoryCase].swap(entry);
      nextDirectoryCase = (nextDirectoryCase + 1) % directoryCaseLimit;
    }
  }
};

static void throwPhysfsError(const char *desc) {
  PHYSFS_ErrorCode ec = PHYSFS_getLastErrorCode();
  const char *englishStr;
    if (ec == 0) {
        // Sometimes on Windows PHYSFS_init can return null
        // but the error code never changes
        englishStr = "unknown error";
    } else {
        englishStr = PHYSFS_getErrorByCode(ec);
    }

  throw Exception(Exception::PHYSFSError, "%s: %s", desc, englishStr);
}

FileSystem::FileSystem(const char *argv0, bool allowSymlinks) {
  if (PHYSFS_init(argv0) == 0)
    throwPhysfsError("Error initializing PhysFS");

  /* One error (=return 0) turns the whole product to 0 */

  int er = 1;

  er *= PHYSFS_registerArchiver(&RGSS1_Archiver);
  er *= PHYSFS_registerArchiver(&RGSS2_Archiver);
  er *= PHYSFS_registerArchiver(&RGSS3_Archiver);

  if (er == 0)
    throwPhysfsError("Error registering PhysFS RGSS archiver");

  p = new FileSystemPrivate;
  p->wantPathCache = false;
  p->havePathCache = false;

  if (allowSymlinks)
    PHYSFS_permitSymbolicLinks(1);
}

FileSystem::~FileSystem() {
  delete p;

  if (PHYSFS_deinit() == 0)
    Debug() << "PhyFS failed to deinit.";
}

void FileSystem::addPath(const char *path, const char *mountpoint, bool reload) {
  /* Try the normal mount first */
    int state = PHYSFS_mount(path, mountpoint, 1);
  if (!state) {
    /* If it didn't work, try mounting via a wrapped
     * SDL_RWops */
    PHYSFS_Io *io = createSDLRWIo(path);

    if (io)
      state = PHYSFS_mountIo(io, path, 0, 1);
  }
    if (!state) {
        PHYSFS_ErrorCode err = PHYSFS_getLastErrorCode();
        throw Exception(Exception::PHYSFSError, "Failed to mount %s (%s)", path, PHYSFS_getErrorByCode(err));
    }
    
    p->clearDirectoryCase();
    if (reload) reloadPathCache();
}

void FileSystem::removePath(const char *path, bool reload) {
    
    if (!PHYSFS_unmount(path)) {
        PHYSFS_ErrorCode err = PHYSFS_getLastErrorCode();
        throw Exception(Exception::PHYSFSError, "Failed to unmount %s (%s)", path, PHYSFS_getErrorByCode(err));
    }
    
    p->clearDirectoryCase();
    if (reload) reloadPathCache();
}

struct CacheEnumData {
  FileSystemPrivate *p;
  std::stack<std::vector<std::string> *> fileLists;
  // PhysFS emits duplicate directory names across mounts. Bound scratch memory;
  // once full, unrecorded directories simply use the original traversal.
  std::set<std::string> scannedDirectories;
  static constexpr size_t scannedDirectoryLimit = 64;
  // PhysFS is C without unwind tables on the Vita: an exception must never
  // cross its frames. A callback parks it here and the C++ caller rethrows.
  std::exception_ptr error;
#ifdef __vita__
  struct Entry { std::string directory, name; int type; };
  // One bounded continuation stack replaces recursive native directory handles.
  static constexpr size_t entryLimit = 4096;
  std::vector<Entry> entries;
  VitaCacheTypes nativeTypes;
  int entryType = -1;
  bool queueEntries = true;
#endif
  BootProfile::PathCacheIO *io = nullptr;

#ifdef __APPLE__
  iconv_t nfd2nfc;
  char buf[512];
#endif

  CacheEnumData(FileSystemPrivate *p) : p(p) {
#ifdef __APPLE__
    nfd2nfc = iconv_open("utf-8", "utf-8-mac");
#endif
  }

  ~CacheEnumData() {
#ifdef __APPLE__
    iconv_close(nfd2nfc);
#endif
  }

  /* Converts in-place */
  void toNFC(char *inout) {
#ifdef __APPLE__
    size_t srcSize = strlen(inout);
    size_t bufSize = sizeof(buf);
    char *bufPtr = buf;
    char *inoutPtr = inout;

    /* Reserve room for null terminator */
    --bufSize;

    iconv(nfd2nfc, &inoutPtr, &srcSize, &bufPtr, &bufSize);
    /* Null-terminate */
    *bufPtr = 0;
    strcpy(inout, buf);
#else
    (void)inout;
#endif
  }
};

#ifdef __vita__
static int queueCacheDirectory(const char *path, CacheEnumData &data);
#endif

static PHYSFS_EnumerateCallbackResult cacheEnumCB(void *d, const char *origdir,
                                                  const char *fname) {
  CacheEnumData &data = *static_cast<CacheEnumData *>(d);
  try {
  if (shState && shState->rtData().rqTerm)
    throw Exception(Exception::MKXPError, "Game close requested. Aborting path cache enumeration.");

  char fullPath[512];

  int length;
  if (!*origdir)
    length = snprintf(fullPath, sizeof(fullPath), "%s", fname);
  else
    length = snprintf(fullPath, sizeof(fullPath), "%s/%s", origdir, fname);

  if (length < 0 || static_cast<size_t>(length) >= sizeof(fullPath))
    return PHYSFS_ENUM_ERROR;

  /* Deal with OSX' weird UTF-8 standards */
  data.toNFC(fullPath);

  std::string mixedCase(fullPath);
  std::string lowerCase = mixedCase;
  strTolower(lowerCase);

  // Exact spelling only: differently cased virtual paths may hold other assets.
  if (data.scannedDirectories.count(mixedCase))
    return PHYSFS_ENUM_OK;

  PHYSFS_Stat stat;
#ifdef __vita__
  const int type = data.entryType;
  data.entryType = -1;
  if (type >= 0)
    stat.filetype = static_cast<PHYSFS_FileType>(type);
  else
#endif
  {
    const double start = BootProfile::now();
    const int result = PHYSFS_stat(fullPath, &stat);
    if (data.io) {
      ++data.io->statCount;
      data.io->statUS += BootProfile::now() - start;
    }
    if (!result)
      return PHYSFS_ENUM_ERROR;
  }

  if (stat.filetype == PHYSFS_FILETYPE_DIRECTORY) {
    // A case alias can overwrite translations from an earlier subtree. Forget
    // that subtree so a later repeat preserves the original traversal's winner.
    for (auto it = data.scannedDirectories.begin(); it != data.scannedDirectories.end();) {
      std::string prefix = it->substr(0, mixedCase.size());
      const bool different = prefix != mixedCase;
      strTolower(prefix);
      if (different && prefix == lowerCase &&
          (it->size() == mixedCase.size() || (*it)[mixedCase.size()] == '/'))
        it = data.scannedDirectories.erase(it);
      else
        ++it;
    }
    if (data.scannedDirectories.size() < CacheEnumData::scannedDirectoryLimit)
      data.scannedDirectories.insert(mixedCase);
    /* Create a new list for this directory */
    std::vector<std::string> &list = data.p->fileLists[lowerCase];

    /* Iterate over its contents */
    data.fileLists.push(&list);
#ifdef __vita__
    const int result = data.queueEntries ? queueCacheDirectory(fullPath, data) :
        PHYSFS_enumerate(fullPath, cacheEnumCB, d);
#else
    const int result = PHYSFS_enumerate(fullPath, cacheEnumCB, d);
#endif
    data.fileLists.pop();
    if (!result)
      return PHYSFS_ENUM_ERROR;
  } else {
    /* Get the file list for the directory we're currently
     * traversing and append this filename to it */
#ifdef __vita__
    std::string directory(origdir);
    strTolower(directory);
    std::vector<std::string> &list = data.queueEntries ?
        data.p->fileLists[directory] : *data.fileLists.top();
#else
    std::vector<std::string> &list = *data.fileLists.top();
#endif

    std::string lowerFilename(fname);
    strTolower(lowerFilename);
    list.push_back(lowerFilename);

    /* Add the lower -> mixed mapping of the file's full path */
    data.p->pathCache.insert(lowerCase, mixedCase);
  }

  return PHYSFS_ENUM_OK;
  } catch (...) {
    if (!data.error)
      data.error = std::current_exception();
    return PHYSFS_ENUM_ERROR;
  }
}

#ifdef __vita__
static int queueCacheDirectory(const char *path, CacheEnumData &data) {
  const size_t base = data.entries.size();
  struct Collect {
    CacheEnumData &data;
    VitaCacheTypes::Listings listings;
    bool overflow = false;
    std::exception_ptr error = nullptr;
    static PHYSFS_EnumerateCallbackResult entry(void *ptr, const char *dir, const char *name) {
      Collect &c = *static_cast<Collect *>(ptr);
      try {
      if (shState && shState->rtData().rqTerm)
        throw Exception(Exception::MKXPError, "Game close requested. Aborting path cache enumeration.");
      if (c.data.entries.size() == CacheEnumData::entryLimit) {
        c.overflow = true;
        return PHYSFS_ENUM_STOP;
      }
      if (strlen(dir) + (*dir ? 1 : 0) + strlen(name) >= 512)
        return PHYSFS_ENUM_ERROR;
      c.data.entries.push_back({dir, name, VitaCacheTypes::type(c.listings, name)});
      return PHYSFS_ENUM_OK;
      } catch (...) {
        c.error = std::current_exception();
        return PHYSFS_ENUM_ERROR;
      }
    }
  };
  bool overflow;
  {
    Collect collect{data, data.nativeTypes.list(path, *data.io)};
    const int listed = PHYSFS_enumerate(path, Collect::entry, &collect);
    if (collect.error) std::rethrow_exception(collect.error);
    if (!listed) return 0;
    overflow = collect.overflow;
  } // Native listing scratch and its handle are gone before any descent.
  if (overflow) {
    data.entries.resize(base);
    data.queueEntries = false;
    const int result = PHYSFS_enumerate(path, cacheEnumCB, &data);
    data.queueEntries = true;
    return result;
  }
  std::reverse(data.entries.begin() + base, data.entries.end());
  return 1;
}

static int walkCacheNative(CacheEnumData &data) {
  if (!queueCacheDirectory("", data)) return 0;
  while (!data.entries.empty()) {
    auto entry = std::move(data.entries.back());
    data.entries.pop_back();
    data.entryType = entry.type;
    if (cacheEnumCB(&data, entry.directory.c_str(), entry.name.c_str()) != PHYSFS_ENUM_OK)
      return 0;
  }
  return 1;
}
#endif

void FileSystem::createPathCache() {
  BootProfile::PathCacheIO io;
  BootProfile::Scope bootCache(BootProfile::PathCache, &io);
  Debug() << "Loading path cache...";
  p->wantPathCache = true;

  // Mount changes can make the previous cache stale, even if rebuilding fails.
  p->havePathCache = false;
  p->pathCache.clear();
  p->fileLists.clear();
  p->clearDirectoryCase();

  FileSystemPrivate pending = {};
  CacheEnumData data(&pending);
  data.io = &io;
  data.fileLists.push(&pending.fileLists[""]);
#ifdef __vita__
  const int result = walkCacheNative(data);
#else
  const int result = PHYSFS_enumerate("", cacheEnumCB, &data);
#endif
  if (data.error)
    std::rethrow_exception(data.error);
  if (!result) {
    Debug() << "Path cache failed; using uncached lookup.";
    return;
  }

  std::swap(p->pathCache, pending.pathCache);
  std::swap(p->fileLists, pending.fileLists);
  p->havePathCache = true;

  Debug() << "Path cache completed.";
}

void FileSystem::reloadPathCache() {
    if (!p->wantPathCache) {
        p->clearDirectoryCase();
        return;
    }
    
    createPathCache();
}

struct FontSetsCBData {
  FileSystemPrivate *p;
  SharedFontState *sfs;
  /* Exact lookup paths, local to one initialization. Overflow keeps scanning. */
  std::vector<std::string> processed;
  static const size_t maxProcessed = 64;
  std::exception_ptr error = nullptr;  // parked here: PhysFS frames cannot unwind
};

static PHYSFS_EnumerateCallbackResult fontSetEnumCB(void *data, const char *dir,
                                                    const char *fname) {
  FontSetsCBData *d = static_cast<FontSetsCBData *>(data);

  if (d->error)
    return PHYSFS_ENUM_ERROR;

  try {
  /* Only consider filenames with font extensions */
  const char *ext = findExt(fname);

  if (!ext)
    return PHYSFS_ENUM_OK;

  char lowExt[8];
  size_t i;

  for (i = 0; i < sizeof(lowExt) - 1 && ext[i]; ++i)
    lowExt[i] = tolower(ext[i]);
  lowExt[i] = '\0';

  if (strcmp(lowExt, "ttf") && strcmp(lowExt, "otf"))
    return PHYSFS_ENUM_OK;

  char filename[512];
  snprintf(filename, sizeof(filename), "%s/%s", dir, fname);

  if (std::find(d->processed.begin(), d->processed.end(), filename) !=
      d->processed.end())
    return PHYSFS_ENUM_OK;

  PHYSFS_File *handle = openBufferedRead(filename);

  if (!handle)
    return PHYSFS_ENUM_ERROR;

  SDL_RWops ops;
  initReadOps(handle, ops, false);

  const bool processed = d->sfs->initFontSetCB(ops, filename);

  SDL_RWclose(&ops);

  if (processed && d->processed.size() < FontSetsCBData::maxProcessed)
    d->processed.push_back(filename);

  return PHYSFS_ENUM_OK;
  } catch (...) {
    d->error = std::current_exception();
    return PHYSFS_ENUM_ERROR;
  }
}

/* Basically just a case-insensitive search
 * for the folder "Fonts"... */
static PHYSFS_EnumerateCallbackResult
findFontsFolderCB(void *data, const char *, const char *fname) {
  size_t i = 0;
  char buffer[512];
  const char *s = fname;

  while (*s && i < sizeof(buffer) - 1)
    buffer[i++] = tolower(*s++);

  buffer[i] = '\0';

  if (strcmp(buffer, "fonts") == 0)
    PHYSFS_enumerate(fname, fontSetEnumCB, data);

  return PHYSFS_ENUM_OK;
}

void FileSystem::initFontSets(SharedFontState &sfs) {
  FontSetsCBData d = {p, &sfs, {}};

  PHYSFS_enumerate("", findFontsFolderCB, &d);
  if (d.error)
    std::rethrow_exception(d.error);
}

struct OpenReadEnumData {
  FileSystem::OpenHandler &handler;
  SDL_RWops ops;

  /* The filename (without directory) we're looking for */
  const char *filename;
  size_t filenameN;

  /* Optional hash to translate full filepaths
   * (used with path cache) */
  BoostHash<std::string, std::string> *pathTrans;

  /* Number of files we've attempted to read and parse */
  size_t matchCount;
  bool stopSearching;

  /* In case of a PhysFS error, save it here so it
   * doesn't get changed before we get back into our code */
  const char *physfsError;

  /* An exception thrown by the handler, parked for openRead to rethrow. */
  std::exception_ptr error;

  OpenReadEnumData(FileSystem::OpenHandler &handler, const char *filename,
                   size_t filenameN,
                   BoostHash<std::string, std::string> *pathTrans)
      : handler(handler), filename(filename), filenameN(filenameN),
        pathTrans(pathTrans), matchCount(0), stopSearching(false),
        physfsError(0) {}
};

static bool pathPrefixMatches(const char *name, const char *lower, size_t length) {
  for (size_t i = 0; i < length; ++i)
    if (!name[i] || tolower(static_cast<unsigned char>(name[i])) !=
                        static_cast<unsigned char>(lower[i]))
      return false;
  return true;
}

struct DirectoryEnumData {
  const char *name;
  size_t length;
  char actual[512];
};

static PHYSFS_EnumerateCallbackResult
directoryEnumCB(void *d, const char *, const char *name) {
  DirectoryEnumData &data = *static_cast<DirectoryEnumData *>(d);
  if (!pathPrefixMatches(name, data.name, data.length) || name[data.length])
    return PHYSFS_ENUM_OK;
  if (data.length >= sizeof(data.actual))
    return PHYSFS_ENUM_ERROR;
  memcpy(data.actual, name, data.length + 1);
  return PHYSFS_ENUM_STOP;
}

static bool resolveDirectoryCase(FileSystemPrivate *p, const char *directory,
                                 char *actual, size_t size) {
  actual[0] = '\0';
  size_t used = 0;
  for (const char *part = directory; *part;) {
    const char *end = strchr(part, '/');
    const size_t length = end ? static_cast<size_t>(end - part) : strlen(part);
    if (length) {
      const std::string lower(directory, part - directory + length);
      const char *cached = p->cachedDirectoryCase(lower);
      if (cached) {
        if (strlen(cached) >= size)
          throw Exception(Exception::PHYSFSError, "Asset directory path is too long");
        used = strlen(cached);
        memcpy(actual, cached, used + 1);
      } else {
        DirectoryEnumData data = {part, length, {0}};
        if (!PHYSFS_enumerate(actual, directoryEnumCB, &data))
          throwPhysfsError("Enumerating asset directory");
        if (!data.actual[0])
          return false;
        const int written = snprintf(actual + used, size - used, "%s%s",
                                     used ? "/" : "", data.actual);
        if (written < 0 || static_cast<size_t>(written) >= size - used)
          throw Exception(Exception::PHYSFSError, "Asset directory path is too long");
        used += written;
        p->rememberDirectoryCase(lower, actual);
      }
    }
    if (!end)
      break;
    part = end + 1;
  }
  return true;
}

static PHYSFS_EnumerateCallbackResult
openReadEnumCB(void *d, const char *dirpath, const char *filename) {
  OpenReadEnumData &data = *static_cast<OpenReadEnumData *>(d);
  char buffer[512];
  const char *fullPath;

  try {
  if (data.stopSearching)
    return PHYSFS_ENUM_STOP;

  /* If there's not even a partial match, continue searching */
  if (!pathPrefixMatches(filename, data.filename, data.filenameN))
    return PHYSFS_ENUM_OK;

  char last = filename[data.filenameN];
  /* If fname matches up to a following '.' (meaning the rest is part
   * of the extension), or up to a following '\0' (full match), we've
   * found our file */
  if (last != '.' && last != '\0')
    return PHYSFS_ENUM_OK;

  const int length = snprintf(buffer, sizeof(buffer), "%s%s%s", dirpath,
                              *dirpath ? "/" : "", filename);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(buffer)) {
    data.stopSearching = true;
    data.physfsError = "Asset path is too long";
    return PHYSFS_ENUM_ERROR;
  }
  fullPath = buffer;

  /* If the path cache is active, translate from lower case
   * to mixed case path */
  if (data.pathTrans)
    fullPath = (*data.pathTrans)[fullPath].c_str();

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  FrameProfile::Scope profileOpen(FrameProfile::IO, 0, true);
#endif
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  const bool asset = FrameProfile::assetActive();
  const double before = asset ? FrameProfile::state.frameUS[FrameProfile::IO] : 0;
#endif
  PHYSFS_File *phys = openBufferedRead(fullPath);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  profileOpen.stop();
#endif
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  const unsigned token = profileAssetOpen(fullPath, phys != nullptr,
      asset ? FrameProfile::state.frameUS[FrameProfile::IO] - before : 0);
#endif

  if (!phys) {
    /* Failing to open this file here means there must
     * be a deeper rooted problem somewhere within PhysFS.
     * Just abort alltogether. */
    data.stopSearching = true;
    data.physfsError = PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode());

    return PHYSFS_ENUM_ERROR;
  }
  initReadOps(phys, data.ops, false, fullPath);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  data.ops.hidden.unknown.data2 = reinterpret_cast<void *>(uintptr_t(token));
#endif

  const char *ext = findExt(filename);

  if (data.handler.tryRead(data.ops, ext))
    data.stopSearching = true;

  ++data.matchCount;
  return PHYSFS_ENUM_OK;
  } catch (...) {
    data.error = std::current_exception();
    data.stopSearching = true;
    return PHYSFS_ENUM_ERROR;
  }
}

void FileSystem::openRead(OpenHandler &handler, const char *filename) {
  std::string filename_nm = normalize(filename, false, false);
  char buffer[512];
  if (filename_nm.size() >= sizeof(buffer))
    throw Exception(Exception::PHYSFSError, "Asset path is too long");
  size_t len = strcpySafe(buffer, filename_nm.c_str(), sizeof(buffer), -1);
  char *delim;

  for (size_t i = 0; i < len; ++i)
    buffer[i] = tolower(static_cast<unsigned char>(buffer[i]));

  /* Find the deliminator separating directory and file name */
  for (delim = buffer + len; delim > buffer; --delim)
    if (*delim == '/')
      break;

  const bool root = (delim == buffer);

  const char *file = buffer;
  const char *dir = "";

  if (!root) {
    /* Cut the buffer in half so we can use it
     * for both filename and directory path */
    *delim = '\0';
    file = delim + 1;
    dir = buffer;
  }
  OpenReadEnumData data(handler, file, len + buffer - delim - !root,
                        p->havePathCache ? &p->pathCache : 0);

  if (p->havePathCache) {
    /* Get the list of files contained in this directory
     * and manually iterate over them */
    const std::vector<std::string> &fileList = p->fileLists[dir];

    for (size_t i = 0; i < fileList.size(); ++i)
      openReadEnumCB(&data, dir, fileList[i].c_str());
  } else {
    char actualDir[512];
    if (!resolveDirectoryCase(p, dir, actualDir, sizeof(actualDir)))
      throw Exception(Exception::NoFileError, "%s", filename);
    if (!PHYSFS_enumerate(actualDir, openReadEnumCB, &data) && !data.physfsError)
      data.physfsError = PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode());
  }

  if (data.error)
    std::rethrow_exception(data.error);

  if (data.physfsError)
    throw Exception(Exception::PHYSFSError, "PhysFS: %s", data.physfsError);

  if (data.matchCount == 0)
    throw Exception(Exception::NoFileError, "%s", filename);
}

void FileSystem::openReadRaw(SDL_RWops &ops, const char *filename,
                             bool freeOnClose) {

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  FrameProfile::Scope profileOpen(FrameProfile::IO, 0, true);
#endif
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  const bool asset = FrameProfile::assetActive();
  const double before = asset ? FrameProfile::state.frameUS[FrameProfile::IO] : 0;
#endif
  const std::string path = normalize(filename, 0, 0);
  PHYSFS_File *handle = openBufferedRead(path.c_str());
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  profileOpen.stop();
#endif
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  const unsigned token = profileAssetOpen(path.c_str(), handle != nullptr,
      asset ? FrameProfile::state.frameUS[FrameProfile::IO] - before : 0);
#endif

  if (!handle)
    throw Exception(Exception::NoFileError, "%s", filename);

  initReadOps(handle, ops, freeOnClose, path.c_str());
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  ops.hidden.unknown.data2 = reinterpret_cast<void *>(uintptr_t(token));
#endif
    return;
}

std::string FileSystem::normalize(const char *pathname, bool preferred,
                            bool absolute) {
    return filesystemImpl::normalizePath(pathname, preferred, absolute);
}

bool FileSystem::exists(const char *filename) {
  // Ruby query callers have no exception guard; preserve best-effort results.
  try {
    const std::string normalized = normalize(filename, false, false);
    if (normalized.size() >= 512)
      return false;
    return PHYSFS_exists(desensitize(normalized.c_str()).c_str());
  } catch (...) {
    return false;
  }
}

std::string FileSystem::desensitize(const char *filename) {
  // Preserve best-effort results for query failures; the caller owns the bytes.
  try {
    std::string fn_lower = normalize(filename, false, false);
    if (fn_lower.size() >= 512)
      throw Exception(Exception::PHYSFSError, "Asset path is too long");
    strTolower(fn_lower);
    if (p->havePathCache && p->pathCache.contains(fn_lower))
      return p->pathCache[fn_lower];

    // Unlike openRead, exact-name queries do not supplement extensions.
    const size_t slash = fn_lower.rfind('/');
    const std::string directory = slash == std::string::npos ? "" : fn_lower.substr(0, slash);
    const char *name = fn_lower.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    // A published listing already proves an exact file miss. Directory names
    // are not in pathCache, so leave those queries to directory resolution.
    if (p->havePathCache && p->fileLists.contains(directory) &&
        !p->fileLists.contains(fn_lower))
      return filename;
    char actualDir[512];
    if (!resolveDirectoryCase(p, directory.c_str(), actualDir, sizeof(actualDir)))
      return filename;
    DirectoryEnumData data = {name, strlen(name), {0}};
    if (!PHYSFS_enumerate(actualDir, directoryEnumCB, &data))
      throwPhysfsError("Enumerating asset directory");
    if (!data.actual[0])
      return filename;
    char resolved[512];
    const int length = snprintf(resolved, sizeof(resolved),
                                "%s%s%s", actualDir, *actualDir ? "/" : "", data.actual);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(resolved))
      throw Exception(Exception::PHYSFSError, "Asset path is too long");
    return resolved;
  } catch (...) {
    return filename;
  }
}
