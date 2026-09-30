/*
 ** bitmap.cpp
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

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "frameprofile.h"
#endif
#include "bitmap.h"

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <SDL_rect.h>
#include <SDL_surface.h>

#include <pixman.h>

#include "gl-util.h"
#include "gl-meta.h"
#include "quad.h"
#include "quadarray.h"
#include "transform.h"
#include "exception.h"

#include "sharedstate.h"
#include "glstate.h"
#include "texpool.h"
#include "shader.h"
#include "filesystem.h"
#include "font.h"
#include "eventthread.h"
#include "graphics.h"
#include "system.h"
#include "util/util.h"

#include "debugwriter.h"

#include "sigslot/signal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <memory>

extern "C" {
#include "libnsgif/libnsgif.h"
}

#ifdef MKXPZ_SOFTWARE_BITMAPS
/* CPU-authoritative Bitmap backend.
 *
 * Every Bitmap keeps its pixels in client memory; all mutation runs through
 * vita/swraster; the GL texture is a lazily created, lazily uploaded,
 * disposable, SAMPLE-ONLY cache. A Bitmap never calls FBO::gen,
 * TEXFBO::linkFBO or TexPool, so it never owns one of the
 * capped render surfaces.
 *
 * "Mega surfaces" keep their stock meaning under this backend: only bitmaps
 * larger than the driver's maximum texture size (or explicitly forced) are
 * "true mega" and keep today's restrictions. Every other CPU-backed Bitmap is
 * drawable exactly like a stock GPU-backed one. */
#include "swraster.h"
#include <vector>

#define GUARD_MEGA \
{ \
if (p->trueMega) \
throw Exception(Exception::MKXPError, \
"Operation not supported for mega surfaces"); \
}
#else
#define GUARD_MEGA \
{ \
if (p->megaSurface) \
throw Exception(Exception::MKXPError, \
"Operation not supported for mega surfaces"); \
}
#endif

#define GUARD_ANIMATED \
{ \
if (p->animation.enabled) \
throw Exception(Exception::MKXPError, \
"Operation not supported for animated bitmaps"); \
}

#define GUARD_UNANIMATED \
{ \
if (!p->animation.enabled) \
throw Exception(Exception::MKXPError, \
"Operation not supported for static bitmaps"); \
}

#define OUTLINE_SIZE 1

#ifndef INT16_MAX
#define INT16_MAX 32767
#endif

/* Normalize (= ensure width and
 * height are positive) */
static IntRect normalizedRect(const IntRect &rect)
{
    int64_t x = rect.x, y = rect.y, w = rect.w, h = rect.h;
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    const int64_t right = std::min<int64_t>(x + w, INT32_MAX);
    const int64_t bottom = std::min<int64_t>(y + h, INT32_MAX);
    x = std::max<int64_t>(x, INT32_MIN);
    y = std::max<int64_t>(y, INT32_MIN);
    /* An INT_MIN extent can still span 2^31. Trim only off-bitmap negative
     * coordinates so both the extent and the bitmap intersection are valid. */
    x = std::max(x, right - INT32_MAX);
    y = std::max(y, bottom - INT32_MAX);
    return IntRect((int)x, (int)y, (int)(right - x), (int)(bottom - y));
}

#ifdef MKXPZ_SOFTWARE_BITMAPS
static inline swraster::Rect swRect(const IntRect &r)
{
    return swraster::Rect{ r.x, r.y, r.w, r.h };
}

/* mkxp's mega fillRect truncates (bitmap.cpp:451-459), which turns a Ruby
 * Color of 19 into 18 after the round trip through a float 19/255. The GL path
 * this backend replaces goes through glClearColor + UNORM conversion, i.e.
 * round-to-nearest, so round here. */
static inline uint8_t swByte(float v)
{
    return (uint8_t)lround(clamp<float>(v, 0, 1) * 255.0f);
}

static inline swraster::Color swColor(const Vec4 &c)
{
    return swraster::Color{ swByte(c.x), swByte(c.y), swByte(c.z), swByte(c.w) };
}

/* enableHires makes every op recurse into a second, larger Bitmap. It is off
 * on the platform this backend exists for and is not implemented here; fail
 * loudly at construction instead of silently degrading. */
static void swGuardHires()
{
    if (shState->config().enableHires)
        throw Exception(Exception::MKXPError,
                        "software_bitmaps: high-resolution texture replacement "
                        "is not supported");
}
#endif


// libnsgif loading callbacks, taken pretty much straight from their tests

/* Bound both the input and the retained decoded frames of one GIF. */
static const size_t gifByteLimit = 64u * 1024u * 1024u;

static void *gif_bitmap_create(int width, int height)
{
    if (width <= 0 || height <= 0 || width > glState.caps.maxTexSize ||
        height > glState.caps.maxTexSize ||
        (size_t)width > gifByteLimit / 4 / (size_t)height)
        return nullptr;
    return calloc((size_t)width * (size_t)height, 4);
}


static void gif_bitmap_set_opaque(void *bitmap, bool opaque)
{
    (void) opaque;  /* unused */
    (void) bitmap;  /* unused */
    assert(bitmap);
}


static bool gif_bitmap_test_opaque(void *bitmap)
{
    (void) bitmap;  /* unused */
    assert(bitmap);
    return false;
}


static unsigned char *gif_bitmap_get_buffer(void *bitmap)
{
    assert(bitmap);
    return (unsigned char *)bitmap;
}


static void gif_bitmap_destroy(void *bitmap)
{
    assert(bitmap);
    free(bitmap);
}


static void gif_bitmap_modified(void *bitmap)
{
    (void) bitmap;  /* unused */
    assert(bitmap);
    return;
}

// --------------------

#ifdef MKXPZ_SOFTWARE_BITMAPS
/* ===== Bitmap texture cache: budget and LRU BEGIN =====
 *
 * BitmapPrivate::ensureTexture() creates one GL texture per Bitmap the first
 * time something samples it, and originally nothing ever released it
 * while the Bitmap lived -- no LRU, no frame-age eviction, no byte budget.
 * RGSS's Cache module (004-Cache.rb) never releases a Bitmap either, so the
 * live texture count tracked the union of every graphic the game had EVER
 * drawn: window skin, iconset, every face, every character sheet, every
 * picture, every battler, every system graphic. It grew monotonically for the
 * whole session.
 *
 * Dropping a cache texture is ALWAYS safe under this backend: the CPU pixels
 * in cpuSurface / animation.cpuFrames are authoritative, texDirty already
 * forces the re-upload, and the cost of being wrong is one whole-level upload
 * -- about 1 ms + 13 ns/px, i.e. 3.0 ms for a full 544x416 screen and 4.2 ms
 * for a 256x256 sheet.
 *
 * THE BUDGET, and why these three numbers (all measured on the device):
 *
 *   The firmware sync-object pool is about 64 per process and is shared by
 *   textures, VBOs and render surfaces, first come first served; 8 render
 *   surfaces x (3 + 1 texture) = 32 sync objects is everything the
 *   application gets after context infrastructure, and with 64 resident
 *   textures alive the measured number of render surfaces still obtainable is
 *   ZERO. What makes that survivable: everything
 *   that fails HARD on an empty pool -- render surfaces, shader code-heap
 *   segments, the default VAO -- is claimed at boot, and
 *   textures created after the pool empties run
 *   with a NULL sync object and are measured to render and update correctly.
 *
 *   So this budget is NOT trying to keep the pool from emptying: the Blank
 *   Dream crash log shows it already over-subscribed in a game that had
 *   barely started (textures=10 vbos=22 surfaces=3, about 44 sync objects
 *   wanted out of about 32). What it does is end the UNBOUNDED half of that
 *   -- the union-over-time, which the Cache module guarantees will otherwise
 *   reach every graphic in the game.
 *
 *   kTexCacheMaxTextures = 48 (a map scene with many events samples far more distinct Bitmaps
 *   than a title screen; retune from the hot= counter): about 5x the live count that same log recorded
 *   for a running scene, so an ordinary frame never reaches the cap, while a
 *   long session cannot grow past it. A frame that really does sample more
 *   than 32 distinct Bitmaps will evict something it is still drawing; that
 *   case is counted separately as hot= in the telemetry line below precisely
 *   so this number can be retuned from the log instead of guessed.
 *
 *   kTexCacheMaxBytes = 24 MiB: the GPU-side mirror of the CPU pixels.
 *   cpu_bitmap_bytes was 2.7 MiB in that log and 32 full-screen 544x416
 *   pictures would be 29 MiB, so this binds only when the working set is
 *   unusually large; it bounds what the cache can cost against the 96 MiB
 *   USER_MAIN_RW / 96 MiB CDRAM ceilings. A single Bitmap larger
 *   than the whole budget is admitted over it rather than emptying the cache
 *   for nothing (see cacheMakeRoom).
 *
 *   kTexCacheIdleFrames = 240: at the 20-30 fps this port measures, 8 to 12
 *   seconds of never being drawn. Long enough that nothing in a scene the
 *   game is still showing ages out, short enough that the graphics of a scene
 *   the game has LEFT are gone before the next one fills the cache. Being
 *   wrong costs exactly one re-upload.
 *
 *   The idle sweep is pressure-gated: Blank Dream held a
 *   menu 6 s, the sweep dropped the whole map working set under the idle
 *   horizon, and closing the menu re-uploaded 12.9 MB in one frame.
 *   kTexCacheIdleFloorBytes and kTexCacheIdleFloorCount are what an idle
 *   stretch defends: at or under EITHER floor -- resident bytes or entry
 *   count -- aged entries stay resident, and the sweep drops them only
 *   while the cache is over a floor. The hard caps, the pressure drain and
 *   shutdown are unchanged.
 */
struct BitmapPrivate;

static const unsigned kTexCacheMaxTextures = 48;
static const uint64_t kTexCacheMaxBytes    = 24u * 1024u * 1024u;
static const uint64_t kTexCacheIdleFrames  = 240;
static const uint64_t kTexCacheIdleFloorBytes = 12u * 1024u * 1024u;
static const unsigned kTexCacheIdleFloorCount = 36;

/* The LRU itself. 'newest' is the most recently sampled Bitmap, 'oldest' is
 * the first victim; every entry's lruNewer/lruOlder walk that order, which is
 * also sorted by lastSampledFrame because an entry is only ever pushed at the
 * newest end. A Bitmap is linked into the list exactly while it holds a cache
 * texture (cacheLinked <=> gl.tex.gl != 0) -- the invariant every function
 * below maintains. Single-threaded, like the rest of the GL side of the
 * engine: every entry point runs on the RGSS thread with the context current. */
static BitmapPrivate *texCacheNewest = 0;
static BitmapPrivate *texCacheOldest = 0;
static unsigned texCacheLive = 0;
static uint64_t texCacheBytes = 0;

/* Reported by cacheReport(); see there for why this is its own line. */
static uint64_t texCacheEvicted = 0;      /* dropped, any reason            */
static uint64_t texCacheEvictedAged = 0;  /* ... of those, by the idle sweep */
static uint64_t texCacheEvictedHot = 0;   /* ... of those, sampled THIS frame */
static unsigned texCacheHighLive = 0;
static uint64_t texCacheHighBytes = 0;
static uint64_t texCacheSweptFrame = (uint64_t)-1;
static uint64_t texCacheReportedFrame = (uint64_t)-1;

/* eviction attribution for the frame-profile vita-texcache
 * line. The window counters cover one summary interval and clear at each
 * flush (logTexCacheWindow) or engine reset (resetTexCacheWindow);
 * texCacheEvicted above are lifetime totals. The reason names who ordered
 * the drop: the idle sweep, cacheMakeRoom under the count cap,
 * cacheMakeRoom under the byte cap with count room left (both caps at once
 * name the count cap: it alone forces the drop), or Bitmap's dispose path.
 * The counters sit on the same cold path as texCacheEvicted and are read
 * only by the flush. */
enum TexCacheEvictReason
{
    texCacheEvictIdle, texCacheEvictCount, texCacheEvictBytes,
    texCacheEvictRelease, texCacheEvictReasonCount
};
static uint64_t texCacheWinCount[texCacheEvictReasonCount];
static uint64_t texCacheWinBytes[texCacheEvictReasonCount];

static void cacheProfileNote(TexCacheEvictReason reason, uint64_t bytes)
{
    ++texCacheWinCount[reason];
    texCacheWinBytes[reason] += bytes;
}

/* FrameProfile::restart() clears an interrupted window; the name lives in
 * that namespace with the other profile hooks. */
namespace FrameProfile
{
void resetTexCacheWindow()
{
    std::memset(texCacheWinCount, 0, sizeof(texCacheWinCount));
    std::memset(texCacheWinBytes, 0, sizeof(texCacheWinBytes));
}

#if defined(__vita__)
/* One vita-texcache line per frame-profile summary window:
 * the resident cache at the flush plus that window's evictions by reason
 * as count/bytes. The point is saying WHICH policy dropped the map working
 * set across a scene change (Blank Dream re-uploads 12.9 MB at menu close
 * with no decode). Reached only through the frame-profile marker, and the
 * caller excludes measurement mode; clears the window it printed. */
void logTexCacheWindow(unsigned long long serial)
{
    static const char *const names[texCacheEvictReasonCount] =
        { "idle", "count", "cap", "release" };
    char line[224];
    size_t at = snprintf(line, sizeof(line),
                         "vita-texcache: f=%llu live=%u bytes=%llu",
                         serial, texCacheLive, (unsigned long long)texCacheBytes);
    for (unsigned i = 0; i < texCacheEvictReasonCount && at < sizeof(line); ++i)
    {
        at += snprintf(line + at, sizeof(line) - at, " %s=%u/%u", names[i],
                       texCacheWinCount[i] > 999999u ? 999999u
                       : (unsigned)texCacheWinCount[i],
                       texCacheWinBytes[i] > 999999999ull ? 999999999ull
                       : (unsigned long long)texCacheWinBytes[i]);
    }
    vita_glue_trace(line);
    resetTexCacheWindow();
}
#endif
}
/* ===== Bitmap texture cache: budget and LRU END ===== */
#endif

struct BitmapPrivate
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    unsigned assetToken = 0;
#endif
    Bitmap *self;
    
    struct {
        int width;
        int height;
        
        bool enabled;
        bool playing;
        bool needsReset;
        bool loop;
        std::vector<TEXFBO> frames;
#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* One CPU buffer per frame; the single texture in BitmapPrivate::gl
         * caches whichever frame was sampled last. 'frames' stays empty. */
        std::vector<SDL_Surface *> cpuFrames;
#endif
        float fps;
        int lastFrame;
        double startTime, playTime;

        inline size_t frameCount() const {
#ifdef MKXPZ_SOFTWARE_BITMAPS
            return cpuFrames.size();
#else
            return frames.size();
#endif
        }

        inline unsigned int currentFrameIRaw() {
            if (!std::isfinite(fps) || fps <= 0) return lastFrame;
            return floor(lastFrame + (playTime / (1 / fps)));
        }
        
        unsigned int currentFrameI() {
            if (!playing || needsReset) return lastFrame;
            int i = currentFrameIRaw();
            return (loop) ? fmod(i, frameCount()) : (i > (int)frameCount() - 1) ? (int)frameCount() - 1 : i;
        }
        
        inline TEXFBO &currentFrame() {
            int i = currentFrameI();
            return frames[i];
        }
        
        inline void play() {
            playing = true;
            needsReset = true;
        }
        
        inline void stop() {
            lastFrame = currentFrameI();
            playing = false;
        }
        
        inline void seek(int frame) {
            /* frameCount() is an EXCLUSIVE bound: clamping to it lets
             * gotoAndStop(frame_count) -- the natural thing for a script to
             * write -- leave lastFrame one past the end, and stop() then
             * hands that value straight back out of currentFrameI(). Under
             * MKXPZ_SOFTWARE_BITMAPS pixels() indexes cpuFrames with it and
             * dereferences the garbage word as an SDL_Surface *
             * (ASan-proven); stock indexes a vector<TEXFBO> by
             * value and merely draws a bogus frame. Both are wrong.
             * currentFrameI(), stop(), nextFrame() and previousFrame()
             * already clamp to frameCount() - 1; seek() was the only
             * producer of the out-of-range value. */
            lastFrame = frameCount() ? clamp(frame, 0, (int)frameCount() - 1) : 0;
        }
        
        void updateTimer() {
            if (needsReset) {
                lastFrame = currentFrameI();
                playTime = 0;
                startTime = shState->runTime();
                needsReset = false;
                return;
            }
            
            playTime = shState->runTime() - startTime;
            return;
        }
    } animation;
    
    sigslot::connection prepareCon;
    
    TEXFBO gl;
    
    Font *font;
    
    /* "Mega surfaces" are a hack to allow Tilesets to be used
     * whose Bitmaps don't fit into a regular texture. They're
     * kept in RAM and will throw an error if they're used in
     * any context other than as Tilesets */
    SDL_Surface *megaSurface;
    
    /* A cached version of the bitmap in client memory, for
     * getPixel calls. Is invalidated any time the bitmap
     * is modified */
    SDL_Surface *surface;
    SDL_PixelFormat *format;
    
    /* The 'tainted' area describes which parts of the
     * bitmap are not cleared, ie. don't have 0 opacity.
     * If we're blitting / drawing text to a cleared part
     * with full opacity, we can disregard any old contents
     * in the texture and blit to it directly, saving
     * ourselves the expensive blending calculation. */
     
    /* pixman_region16_t supports bitmaps whose largest
     * dimension is no more than 32767 pixels.
     * Be certain to set pixmanUseRegion32 in the
     * constructor for larger bitmaps. */
    pixman_region16_t tainted;
    pixman_region32_t tainted32;
    bool pixmanUseRegion32;

    // For high-resolution texture replacement.
    Bitmap *selfHires;
    Bitmap *selfLores;
    bool assumingRubyGC;
    
    // Child bitmaps are created by Planes, Sprites, and Windows for mega surfaces
    ChildPrivate *pChild;

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* THE pixels. Always allocated for a non-animated Bitmap; an animated one
     * leaves this null and keeps one buffer per frame in animation.cpuFrames. */
    SDL_Surface *cpuSurface;

    /* Larger than glState.caps.maxTexSize, or constructed with forceMega:
     * keeps the stock mega restrictions (not drawable, no texture at all). */
    bool trueMega;

    /* The sampling cache in 'gl' is stale. Whole-level dirty is what the
     * upload actually honours; dirtyRect is kept alongside purely so a later
     * change can turn this into a sub-upload. */
    bool texDirty;
    IntRect dirtyRect;

    /* Animation frame currently held by the cache texture; -1 = none. */
    int cachedFrame;

    /* This Bitmap's place in the texture cache LRU.
     * cacheLinked is true exactly while gl.tex.gl is non-zero; cacheBytes is
     * what this entry contributes to texCacheBytes, and lastSampledFrame is
     * the GPUBudget real-swap counter at the last sample, hit or miss. */
    BitmapPrivate *lruNewer, *lruOlder;
    bool cacheLinked;
    uint64_t cacheBytes;
    uint64_t lastSampledFrame;
#endif

    BitmapPrivate(Bitmap *self)
    : self(self),
    megaSurface(0),
    selfHires(0),
    selfLores(0),
    surface(0),
    pChild(0),
    assumingRubyGC(false),
    pixmanUseRegion32(false)
#ifdef MKXPZ_SOFTWARE_BITMAPS
    , cpuSurface(0),
    trueMega(false),
    texDirty(false),
    dirtyRect(0, 0, 0, 0),
    cachedFrame(-1),
    lruNewer(0),
    lruOlder(0),
    cacheLinked(false),
    cacheBytes(0),
    lastSampledFrame(0)
#endif
    {
        std::unique_ptr<SDL_PixelFormat, decltype(&SDL_FreeFormat)> ownedFormat(
            SDL_AllocFormat(SDL_PIXELFORMAT_ABGR8888), SDL_FreeFormat);
        if (!ownedFormat)
            throw Exception(Exception::SDLError, "Error creating Bitmap format: %s", SDL_GetError());
        
        animation.width = 0;
        animation.height = 0;
        animation.enabled = false;
        animation.playing = false;
        animation.needsReset = false;
        animation.loop = true;
        animation.playTime = 0;
        animation.startTime = 0;
        animation.fps = 0;
        animation.lastFrame = 0;
        
        prepareCon = shState->prepareDraw.connect(&BitmapPrivate::prepare, this);
        
        font = &shState->defaultFont();
        pixman_region_init(&tainted);
        format = ownedFormat.release();
    }
    
    ~BitmapPrivate()
    {
#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* Belt and braces. Bitmap::releaseResources is
         * the only path that destroys a live Bitmap and it drops the cache
         * entry first; the constructors' error paths reach `delete p` before
         * any texture can exist. Unlinking here anyway means a future path
         * that forgets can leak a texture name -- it can never leave the LRU
         * holding a pointer to freed memory. */
        cacheForget();
#endif
        prepareCon.disconnect();
        SDL_FreeFormat(format);
        if (pixmanUseRegion32)
            pixman_region32_fini(&tainted32);
        else
            pixman_region_fini(&tainted);
    }
    
    TEXFBO &getGLTypes() {
#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* One TEXFBO per Bitmap whose .fbo is permanently 0: the texture is an
         * upload-only sampling cache, never a render target. Animated bitmaps
         * reuse the same cache for whichever frame is current. */
        return gl;
#else
        return (animation.enabled) ? animation.currentFrame() : gl;
#endif
    }

#ifdef MKXPZ_SOFTWARE_BITMAPS
    SDL_Surface *allocPixels(int w, int h) const
    {
        SDL_Surface *s = SDL_CreateRGBSurface(0, w, h, format->BitsPerPixel,
                                              format->Rmask, format->Gmask,
                                              format->Bmask, format->Amask);
        if (!s)
            throw Exception(Exception::SDLError, "Error creating Bitmap pixels: %s",
                            SDL_GetError());
        /* Nothing here goes through SDL's blitter any more, but keep the stock
         * mega-surface setting so a surface handed out by megaSurface() to
         * Tilemap behaves identically. */
        SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);
        countPixels(s, +1);
        return s;
    }

    /* Live CPU pixel bytes, for the gpu-telemetry line.
     * This backend trades render surfaces for client memory, so the bytes are
     * the other half of that trade and belong in the same report. Every
     * Bitmap pixel buffer is either allocated by allocPixels() above or
     * adopted verbatim from the image decoder in initFromSurface(); those two
     * and the three frees in removeFrame()/releaseResources() are all of it. */
    static void countPixels(const SDL_Surface *s, int sign)
    {
        if (!s)
            return;

        const int64_t bytes = (int64_t)s->h * (int64_t)s->pitch;
        GPUBudget::cpuPixels(sign < 0 ? -bytes : bytes);
    }

    static void freePixels(SDL_Surface *s)
    {
        countPixels(s, -1);
        SDL_FreeSurface(s);
    }

    using PixelOwner = std::unique_ptr<SDL_Surface, decltype(&freePixels)>;

    SDL_Surface *copyPixels(SDL_Surface *src) const
    {
        SDL_Surface *dst = allocPixels(src->w, src->h);
        for (int y = 0; y < src->h; ++y)
            memcpy((uint8_t *)dst->pixels + (size_t)y * dst->pitch,
                   (const uint8_t *)src->pixels + (size_t)y * src->pitch,
                   (size_t)src->w * 4);
        return dst;
    }

    /* Pixels backing the *current* state of this Bitmap. */
    SDL_Surface *pixels()
    {
        if (animation.enabled)
        {
            if (animation.cpuFrames.empty())
                return 0;
            return animation.cpuFrames[animation.currentFrameI()];
        }

        return cpuSurface;
    }

    swraster::Surface swSurface()
    {
        SDL_Surface *s = pixels();
        if (!s)
            throw Exception(Exception::MKXPError, "Bitmap has no CPU pixels");

        return swraster::Surface{ (uint8_t *)s->pixels, s->w, s->h, s->pitch };
    }

    void markDirty(const IntRect &)
    {
        /* Uploads are whole-level; extreme sub-rectangle hints need no arithmetic. */
        texDirty = true;
        dirtyRect = IntRect(0, 0, gl.width, gl.height);
    }

    bool uploadWholeLevel(SDL_Surface *src)
    {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileUpload(FrameProfile::Upload, 0, false, false);
#endif
        if (src->pitch == src->w * 4)
        {
            return TEX::uploadImageChecked(src->w, src->h, src->pixels, GL_RGBA);
        }

        /* SDL always gives pitch == w*4 for 32bpp, but never hand a padded
         * buffer to a driver with no GL_UNPACK_ROW_LENGTH. */
        std::vector<uint8_t> packed((size_t)src->w * (size_t)src->h * 4);
        for (int y = 0; y < src->h; ++y)
            memcpy(&packed[(size_t)y * src->w * 4],
                   (const uint8_t *)src->pixels + (size_t)y * src->pitch,
                   (size_t)src->w * 4);
        return TEX::uploadImageChecked(src->w, src->h, packed.data(), GL_RGBA);
    }

    /* ---- texture cache bookkeeping --------------------
     * Pointer work on the intrusive LRU declared above this class. Nothing
     * here divides, allocates or touches GL except cacheDrop(), which is the
     * single deletion-policy call site. */

    void cacheListRemove()
    {
        if (lruNewer)
            lruNewer->lruOlder = lruOlder;
        else
            texCacheNewest = lruOlder;

        if (lruOlder)
            lruOlder->lruNewer = lruNewer;
        else
            texCacheOldest = lruNewer;

        lruNewer = 0;
        lruOlder = 0;
    }

    void cacheListPushNewest()
    {
        lruNewer = 0;
        lruOlder = texCacheNewest;

        if (texCacheNewest)
            texCacheNewest->lruNewer = this;

        texCacheNewest = this;

        if (!texCacheOldest)
            texCacheOldest = this;
    }

    /* Charge this Bitmap's cache texture and make it the most recently
     * sampled entry. `bytes` is the level that is about to be uploaded, so a
     * re-upload re-charges rather than accumulating. */
    void cacheNote(uint64_t bytes)
    {
        if (cacheLinked)
        {
            texCacheBytes -= cacheBytes;
            cacheListRemove();
        }
        else
        {
            ++texCacheLive;
            cacheLinked = true;
        }

        cacheBytes = bytes;
        texCacheBytes += bytes;
        lastSampledFrame = GPUBudget::frameCounter();
        cacheListPushNewest();

        if (texCacheLive > texCacheHighLive || texCacheBytes > texCacheHighBytes)
        {
            if (texCacheLive > texCacheHighLive)
                texCacheHighLive = texCacheLive;
            if (texCacheBytes > texCacheHighBytes)
                texCacheHighBytes = texCacheBytes;

            cacheReport("grow");
        }
    }

    /* A sample that found the cache already good: no GL, just the LRU
     * position and the frame stamp. ensureTexture() returns early on that
     * path, so without this a Bitmap drawn every single frame would look
     * untouched to the idle sweep and be evicted out from under the game. */
    void cacheTouch()
    {
        if (!cacheLinked)
            return;

        lastSampledFrame = GPUBudget::frameCounter();

        if (texCacheNewest == this)
            return;

        cacheListRemove();
        cacheListPushNewest();
    }

    /* Leave the cache without touching GL. */
    void cacheForget()
    {
        if (!cacheLinked)
            return;

        cacheListRemove();
        cacheLinked = false;
        --texCacheLive;
        texCacheBytes -= cacheBytes;
        cacheBytes = 0;
    }

    /* THE deletion-policy call site for a Bitmap cache texture: budget
     * eviction, the idle sweep and Bitmap::releaseResources() all end here,
     * so whatever the boot/GL policy settles on for texture deletion
     * is changed in ONE place. Today that policy is the deferred
     * queue -- TEX::del enqueues the name and GPUBudget::frameAdvance()
     * releases it deferSwaps[DeferTexture] REAL swaps later -- which is also
     * what makes it safe to evict a texture whose id an earlier call in this
     * same frame already handed to a shader (sprite.cpp setPattern,
     * graphics.cpp setTransMap): the GL object outlives the frame drawing
     * with it. cacheMakeRoom() still prefers older victims and refuses the
     * bound one, so eviction is also sane with deferSwaps[] set to 0. */
    void cacheDrop()
    {
        cacheForget();

        if (gl.tex.gl)
        {
            /* the name is recycled, not retired. The next
             * Bitmap of this size re-specifies it in place -- the upload
             * path measured at the whole-level rate, several times the
             * fresh-object upload a replacement name would pay. Overflow
             * and drains retire it through the deferred queue. */
            GPUBudget::texRecycleOffer(gl.tex.gl, gl.width, gl.height);
            gl.tex = TEX::ID(0);
        }

        /* The CPU pixels are authoritative and unchanged; the next sample
         * re-creates the texture and re-uploads the whole level. */
        texDirty = true;
        cachedFrame = -1;
    }

    /* Two entries are off limits to any eviction: the one being admitted, and
     * whatever texture is bound right now -- ensureTexture() restores that
     * binding when it returns, and with immediate deletion the restore would
     * bind a name the driver has retired. */
    static bool cacheEvictable(const BitmapPrivate *entry,
                               const BitmapPrivate *keep, GLuint bound)
    {
        return entry != keep && !(bound && entry->gl.tex.gl == bound);
    }

    /* Drop what the game has stopped drawing while the cache is over one of
     * its idle floors (kTexCacheIdleFloorBytes / kTexCacheIdleFloorCount);
     * at or below both, an aged entry stays resident. Runs once per real
     * frame, from the first sample of that frame that has any work to do.
     * The walk stops at the first young entry because the list is sorted by
     * lastSampledFrame, and at the floors because going below them buys
     * nothing: that memory is still inside the budget, and dropping would
     * only cost a re-upload. */
    static void cacheSweepIdle(uint64_t now, BitmapPrivate *keep, GLuint bound)
    {
        unsigned dropped = 0;
        BitmapPrivate *entry = texCacheOldest;

        while (entry)
        {
            BitmapPrivate *newer = entry->lruNewer;

            if (cacheEvictable(entry, keep, bound))
            {
                if (now - entry->lastSampledFrame < kTexCacheIdleFrames)
                    break;

                if (texCacheBytes <= kTexCacheIdleFloorBytes &&
                    texCacheLive <= kTexCacheIdleFloorCount)
                    break;

                ++texCacheEvicted;
                ++texCacheEvictedAged;
                ++dropped;
                cacheProfileNote(texCacheEvictIdle, entry->cacheBytes);
                entry->cacheDrop();
            }

            entry = newer;
        }

        if (dropped)
            cacheReport("aged");
    }

    /* Make room for one more cache texture of `incoming` bytes on behalf of
     * `keep`, least-recently-sampled first. The count cap is hard: on return
     * admitting `keep` cannot take texCacheLive past kTexCacheMaxTextures.
     * Returns how many entries were dropped, because a drop can reach GL --
     * see the caller. */
    static unsigned cacheMakeRoom(uint64_t incoming, BitmapPrivate *keep, GLuint bound)
    {
        /* A Bitmap bigger than the entire byte budget cannot be made to fit,
         * and emptying the cache for it would only destroy the working set
         * and upload it all again next frame. It is admitted over the byte
         * budget; the count cap still holds. */
        const uint64_t wantBytes = incoming > kTexCacheMaxBytes ? 0 : incoming;
        const uint64_t now = GPUBudget::frameCounter();
        unsigned dropped = 0;

        for (;;)
        {
            /* What the cache would hold with `keep` admitted. It pays for
             * itself, so an entry that is already linked does not count
             * twice. */
            const unsigned held = texCacheLive - (keep->cacheLinked ? 1u : 0u);
            const uint64_t bytes =
                texCacheBytes - (keep->cacheLinked ? keep->cacheBytes : (uint64_t)0);

            if (held + 1u <= kTexCacheMaxTextures && bytes + wantBytes <= kTexCacheMaxBytes)
                break;

            BitmapPrivate *victim = texCacheOldest;
            while (victim && !cacheEvictable(victim, keep, bound))
                victim = victim->lruNewer;

            if (!victim)
                break;

            /* Evicting something this very frame is drawing is the thrash
             * case the budget is meant never to reach: counted on its own so
             * a hardware log says whether kTexCacheMaxTextures is too small
             * for a real game rather than leaving it to be guessed. */
            if (victim->lastSampledFrame == now)
                ++texCacheEvictedHot;

            ++texCacheEvicted;
            ++dropped;
            cacheProfileNote(held + 1u > kTexCacheMaxTextures ?
                             texCacheEvictCount : texCacheEvictBytes,
                             victim->cacheBytes);

            const unsigned was = texCacheLive;
            victim->cacheDrop();

            /* Every iteration must make room. A spin here would be a frozen
             * game on the device, so the loop refuses to trust that it does
             * rather than depending on cacheDrop() for its own termination. */
            if (texCacheLive >= was)
                break;
        }

        if (dropped)
            cacheReport("budget");

        return dropped;
    }

    /* One line through the same vita-gpu: sink as GPUBudget::logCounts, at
     * most one per frame: the live count, the bytes, the budget both are held
     * to, what eviction has cost and the peak -- the numbers needed
     * to see that the pool is actually bounded. It is a line of its own
     * rather than four more fields on logCounts()'s line because that line
     * lives in gl-meta.cpp; folding it in is a possible
     * follow-up. Telemetry-marker only: without
     * ux0:/data/mkxp-z/gpu-telemetry.enabled this is one cached bool read. */
    static void cacheReport(const char *why)
    {
        if (!GPUBudget::telemetryEnabled())
            return;

        const uint64_t now = GPUBudget::frameCounter();

        if (texCacheReportedFrame == now)
            return;

        texCacheReportedFrame = now;

        char line[256];
        snprintf(line, sizeof(line),
                 "vita-gpu: bitmap-cache live=%u bytes=%u budget=%u "
                 "budget_bytes=%u evicted=%u aged=%u hot=%u peak=%u "
                 "peak_bytes=%u why=%s frame=%u",
                 texCacheLive, (unsigned)texCacheBytes, kTexCacheMaxTextures,
                 (unsigned)kTexCacheMaxBytes, (unsigned)texCacheEvicted,
                 (unsigned)texCacheEvictedAged, (unsigned)texCacheEvictedHot,
                 texCacheHighLive, (unsigned)texCacheHighBytes,
                 why, (unsigned)now);
        GPUBudget::syncedLine(line);
    }

    /* The one flush-before-sample entry point. Creates the sampling texture on
     * first use and re-uploads the whole level when the CPU pixels moved on.
     * Never generates or binds a framebuffer: whole-level TEX::uploadImage is
     * the cheap upload path, and a texture that is never attached
     * to an FBO never owns a render surface.
     *
     * The previously bound texture is restored: call sites such as
     * shader.setTransMap(bm->getGLTypes().tex) evaluate this *before* the
     * shader binds its own samplers. */
    void ensureTexture()
    {
        if (trueMega)
            return;

        SDL_Surface *src = pixels();
        if (!src)
            return;

        /* once per REAL frame, from the first Bitmap
         * sampled in it, age out what the game has stopped drawing while
         * the cache is over one of its idle floors. This is above the
         * cache-hit return on purpose -- a scene that keeps sampling the
         * same handful of Bitmaps uploads nothing at all, and an over-floor
         * union left over from the scene before it still has to go. No
         * bound-texture protection is needed here, unlike in
         * cacheMakeRoom(): an entry this sweep can reach has not been
         * sampled for kTexCacheIdleFrames, so nothing in flight is drawing
         * with it. */
        const uint64_t now = GPUBudget::frameCounter();

        if (texCacheSweptFrame != now)
        {
            texCacheSweptFrame = now;
            cacheSweepIdle(now, this, 0);
        }

        const int frame = animation.enabled ? (int)animation.currentFrameI() : 0;
        if (gl.tex.gl && !texDirty && frame == cachedFrame)
        {
            /* Still a sample: the cache stays hot and the sweep above must
             * see this Bitmap as drawn in this frame. */
            cacheTouch();
            return;
        }

        texDirty = true;
        GLint prevTex = 0;
        ::gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

        /* Make room for this level before anything is created. */
        const uint64_t bytes = (uint64_t)src->w * (uint64_t)src->h * 4u;

        if (cacheMakeRoom(bytes, this, (GLuint)prevTex))
        {
            /* An eviction reaches TEX::del, and under the deferred-release policy a TEX::del
             * can drain the deferred queue when it is over pressure -- which
             * releases names queued by EARLIER frames, one of which may be
             * the binding sampled above. Restoring a retired name would not
             * fail: it would CREATE an empty texture object, which on this
             * driver is another sync object out of the pool this cache exists
             * to bound. Re-read instead; the query only happens on the rare
             * frame that evicted something. */
            ::gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        }

        TEX::ScopedBinding binding(prevTex);
        TEX::drainStaleErrors("texture-prepare");
        if (!gl.tex.gl)
        {
            /* a pooled name of this exact size uploads by
             * re-specifying in place, the fast path measured on the device, while a
             * fresh name pays the slow first specification. The sampler
             * state is re-issued either way: a pooled name carries the
             * parameters its previous owner set. */
            gl.tex = TEX::ID(GPUBudget::texRecycleTake(src->w, src->h));
            if (!gl.tex.gl)
            {
                gl.tex = TEX::gen();
                if (!gl.tex.gl)
                    throw TEX::UploadError();
            }
            TEX::bind(gl.tex);
            TEX::setRepeat(false);
            TEX::setSmooth(false);
        }
        else
        {
            TEX::bind(gl.tex);
        }

        /* cacheLinked <=> gl.tex.gl, so the charge goes on as soon as the
         * name exists -- before the upload, which can throw. */
        cacheNote(bytes);

        if (!uploadWholeLevel(src))
            throw TEX::UploadError();

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::assetUpload(assetToken, bytes);
#endif
        texDirty = false;
        cachedFrame = frame;
        dirtyRect = IntRect(0, 0, 0, 0);

    }

    /* The movie loop replaces the CPU pixels every frame and needs the new
     * frame sampled immediately. ensureTexture() would re-specify the very
     * level the previous frame drew; on the device a
     * same-name per-frame respecify kept the picture at frame 1. So
     * each frame uploads into a name this owner has never written and the sampled name goes back through the
     * one drop policy, whose pool retires what it cannot hold two real
     * swaps behind. Bounded: one live name plus the pool's fixed
     * caps, and this method exists for the one Bitmap the movie loop
     * drives. */
    void refreshFrameNow()
    {
        if (trueMega)
            return;

        SDL_Surface *src = pixels();
        if (!src)
            return;

        texDirty = true;
        GLint prevTex = 0;
        ::gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

        const uint64_t bytes = (uint64_t)src->w * (uint64_t)src->h * 4u;

        if (cacheMakeRoom(bytes, this, (GLuint)prevTex))
            ::gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

        TEX::ScopedBinding binding(prevTex);
        /* The single drop policy hands the sampled name to the recycle
         * pool; the replacement is a name nothing has ever sampled. */
        cacheDrop();

        TEX::drainStaleErrors("texture-refresh");
        gl.tex = TEX::gen();
        if (!gl.tex.gl)
            throw TEX::UploadError();

        TEX::bind(gl.tex);
        TEX::setRepeat(false);
        TEX::setSmooth(false);

        cacheNote(bytes);

        if (!uploadWholeLevel(src))
            throw TEX::UploadError();

        texDirty = false;
        cachedFrame = animation.enabled ? (int)animation.currentFrameI() : 0;
        dirtyRect = IntRect(0, 0, 0, 0);
    }
#endif

    void prepare()
    {
        if (!animation.enabled || !animation.playing) return;
        
        animation.updateTimer();
    }
    
    void allocSurface()
    {
        surface = SDL_CreateRGBSurface(0, getGLTypes().width, getGLTypes().height, format->BitsPerPixel,
                                       format->Rmask, format->Gmask,
                                       format->Bmask, format->Amask);
    }
    
    /* The tainted-region algebra exists purely to let the GPU path skip the
     * expensive read-modify-write blend on untainted destinations. A software
     * blitter always has the destination in hand and always blends, so under
     * MKXPZ_SOFTWARE_BITMAPS these become no-ops and pixman does no work per
     * blit. The regions themselves stay initialised so the destructor and the
     * clone constructor are unchanged. */
    void clearTaintedArea()
    {
#ifdef MKXPZ_SOFTWARE_BITMAPS
        return;
#else
        if( pixmanUseRegion32)
        {
            pixman_region32_fini(&tainted32);
            pixman_region32_init(&tainted32);
        }
        else
        {
            pixman_region_fini(&tainted);
            pixman_region_init(&tainted);
        }
#endif
    }
    
    void addTaintedArea(const IntRect &rect)
    {
#ifdef MKXPZ_SOFTWARE_BITMAPS
        (void)rect;
        return;
#else
        IntRect norm = normalizedRect(rect);
        if (!norm.w || !norm.h) return;
        if (pixmanUseRegion32)
        {
            pixman_region32_union_rect
            (&tainted32, &tainted32, norm.x, norm.y, norm.w, norm.h);
        }
        else
        {
            pixman_region_union_rect
            (&tainted, &tainted, norm.x, norm.y, norm.w, norm.h);
        }
#endif
    }
    
    void substractTaintedArea(const IntRect &rect)
    {
#ifdef MKXPZ_SOFTWARE_BITMAPS
        (void)rect;
        return;
#else
        if (!touchesTaintedArea(rect))
            return;
        
        if (pixmanUseRegion32)
        {
            pixman_region32_t m_reg;
            pixman_region32_init_rect(&m_reg, rect.x, rect.y, rect.w, rect.h);
            
            pixman_region32_subtract(&tainted32, &m_reg, &tainted32);
            
            pixman_region32_fini(&m_reg);
        }
        else
        {
            pixman_region16_t m_reg;
            pixman_region_init_rect(&m_reg, rect.x, rect.y, rect.w, rect.h);
            
            pixman_region_subtract(&tainted, &m_reg, &tainted);
            
            pixman_region_fini(&m_reg);
        }
#endif
    }
    
    bool touchesTaintedArea(const IntRect &rect)
    {
#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* Always blend. */
        (void)rect;
        return true;
#else
        pixman_region_overlap_t result;
        if (pixmanUseRegion32)
        {
            pixman_box32_t box;
            box.x1 = rect.x;
            box.y1 = rect.y;
            box.x2 = rect.x + rect.w;
            box.y2 = rect.y + rect.h;
            
            result = pixman_region32_contains_rectangle(&tainted32, &box);
        }
        else
        {
            pixman_box16_t box;
            box.x1 = rect.x;
            box.y1 = rect.y;
            box.x2 = rect.x + rect.w;
            box.y2 = rect.y + rect.h;
            
            result = pixman_region_contains_rectangle(&tainted, &box);
        }
        
        return result != PIXMAN_REGION_OUT;
#endif
    }
    
    void bindTexture(ShaderBase &shader, bool substituteLoresSize = true)
    {
#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* Sampling always goes through the upload cache, animated or not. */
        (void)substituteLoresSize;
        ensureTexture();
        TEX::bind(gl.tex);
        shader.setTexSize(Vec2i(gl.width, gl.height));
        return;
#else
        if (selfHires) {
            selfHires->bindTex(shader, substituteLoresSize);
            return;
        }

        if (animation.enabled) {
            if (selfLores) {
                Debug() << "BUG: High-res BitmapPrivate bindTexture for animations not implemented";
            }

            TEXFBO cframe = animation.currentFrame();
            TEX::bind(cframe.tex);
            shader.setTexSize(Vec2i(cframe.width, cframe.height));
            return;
        }
        TEX::bind(gl.tex);
        if (selfLores && substituteLoresSize) {
            shader.setTexSize(Vec2i(selfLores->width(), selfLores->height()));
        }
        else {
            shader.setTexSize(Vec2i(gl.width, gl.height));
        }
#endif
    }
    
    void bindFBO()
    {
#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* A software Bitmap owns no render target; .fbo is 0 forever and
         * binding it would target the default framebuffer. Reaching here means
         * a GPU mutation path was left wired to a Bitmap. */
        assert(false && "software Bitmap has no render target");
        throw Exception(Exception::MKXPError,
                        "software_bitmaps: Bitmap has no render target");
#else
        FBO::bind((animation.enabled) ? animation.currentFrame().fbo : gl.fbo);
#endif
    }
    
    void pushSetViewport(ShaderBase &shader) const
    {
        glState.viewport.pushSet(IntRect(0, 0, gl.width, gl.height));
        shader.applyViewportProj();
    }
    
    void popViewport() const
    {
        glState.viewport.pop();
    }
    
    void blitQuad(Quad &quad)
    {
        glState.blend.pushSet(false);
        quad.draw();
        glState.blend.pop();
    }
    
    void fillRect(const IntRect &rect,
                  const Vec4 &color)
    {
#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* REPLACE semantics, matching the scissored glClear this replaces. */
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileRaster(FrameProfile::Raster);
#endif
        swraster::fill(swSurface(), swRect(rect), swColor(color));
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        profileRaster.stop();
#endif
        markDirty(rect);
#else
        if (megaSurface)
        {
            uint8_t r, g, b, a;
            r = clamp<float>(color.x, 0, 1) * 255.0f;
            g = clamp<float>(color.y, 0, 1) * 255.0f;
            b = clamp<float>(color.z, 0, 1) * 255.0f;
            a = clamp<float>(color.w, 0, 1) * 255.0f;
            SDL_FillRect(megaSurface, &rect, SDL_MapRGBA(format, r, g, b, a));
        }
        else
        {
            const IntRect norm = normalizedRect(rect);
            if (!norm.w || !norm.h) return;
            bindFBO();
            
            glState.scissorTest.pushSet(true);
            glState.scissorBox.pushSet(norm);
            glState.clearColor.pushSet(color);
            
            FBO::clear();
            
            glState.clearColor.pop();
            glState.scissorBox.pop();
            glState.scissorTest.pop();
        }
#endif
    }
    
    static void ensureFormat(SDL_Surface *&surf, Uint32 format)
    {
        if (surf->format->format == format)
            return;
        
        SDL_Surface *surfConv = SDL_ConvertSurfaceFormat(surf, format, 0);
        if (!surfConv)
            throw Exception(Exception::SDLError, "Error converting Bitmap surface: %s",
                            SDL_GetError());
        SDL_FreeSurface(surf);
        surf = surfConv;
    }
    
    void onModified(bool freeSurface = true)
    {
#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* Safety net: every mutation invalidates the sampling cache even if a
         * call site forgot markDirty(). Uploads are whole-level, so only the
         * sub-upload hint (dirtyRect) degrades, never correctness. */
        texDirty = true;
#endif
        if (surface && freeSurface)
        {
            SDL_FreeSurface(surface);
            surface = 0;
        }
        
        self->modified();
    }
};

struct BitmapOpenHandler : FileSystem::OpenHandler
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    unsigned assetToken = 0;
#endif
    struct GifDeleter {
        void operator()(gif_animation *value) const {
            gif_finalise(value);
            delete value;
        }
    };
    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> surface{nullptr, SDL_FreeSurface};
    std::string error;
    /* The decoder borrows these bytes until gif_finalise(). */
    std::unique_ptr<unsigned char[]> gif_data;
    std::unique_ptr<gif_animation, GifDeleter> gif;

    bool tryRead(SDL_RWops &ops, const char *ext)
    {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileDecode(FrameProfile::Decode);
#endif
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        assetToken = FrameProfile::assetActive() ? uintptr_t(ops.hidden.unknown.data2) : 0;
        FrameProfile::AssetDecodeScope assetDecode(profileDecode, assetToken);
        FrameProfile::AssetImageScope imageScope(assetToken);
#endif
        struct StreamCloser {
            SDL_RWops &ops;
            ~StreamCloser() { ops.close(&ops); }
        } closer{ops};
        gif.reset();
        gif_data.reset();
        surface.reset();
        error.clear();

        if (!IMG_isGIF(&ops)) {
            surface.reset(IMG_LoadTyped_RW(&ops, 0, ext));
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            assetDecode.result(surface ? surface->w : 0, surface ? surface->h : 0);
#endif
            return !!surface;
        }

        const Sint64 length = ops.size(&ops);
        if (length <= 0 || (uint64_t)length > gifByteLimit) {
            error = "Invalid or oversized GIF input";
            return false;
        }
        if (ops.seek(&ops, 0, RW_SEEK_SET) != 0) {
            error = "Failed to seek GIF input";
            return false;
        }
        std::unique_ptr<unsigned char[]> data(new unsigned char[(size_t)length]);
        if (ops.read(&ops, data.get(), 1, (size_t)length) != (size_t)length) {
            error = "Failed to read GIF input";
            return false;
        }

        std::unique_ptr<gif_animation, GifDeleter> decoded(new gif_animation{});
        gif_bitmap_callback_vt callbacks = {
            gif_bitmap_create, gif_bitmap_destroy, gif_bitmap_get_buffer,
            gif_bitmap_set_opaque, gif_bitmap_test_opaque, gif_bitmap_modified
        };
        gif_create(decoded.get(), &callbacks);
        int status;
        do {
            status = gif_initialise(decoded.get(), (size_t)length, data.get());
            if (status != GIF_OK && status != GIF_WORKING) {
                error = "Failed to initialize GIF (Error " + std::to_string(status) + ")";
                return false;
            }
        } while (status != GIF_OK);

        const size_t frameBytes = (size_t)decoded->width * (size_t)decoded->height * 4;
        if (!frameBytes || !decoded->frame_count || !decoded->frame_count_partial ||
            decoded->frame_count_partial > gifByteLimit / frameBytes) {
            error = "Invalid or oversized GIF animation";
            return false;
        }
        status = gif_decode_frame(decoded.get(), 0);
        if (status != GIF_OK) {
            error = "Failed to decode first GIF frame (Error " + std::to_string(status) + ")";
            return false;
        }
        gif_data = std::move(data);
        gif = std::move(decoded);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        assetDecode.result(gif->width, gif->height);
#endif
        return true;
    }
};

Bitmap::Bitmap(const char *filename)
: p(nullptr)
{
    std::string hiresPrefix = "Hires/";
    std::string filenameStd = filename;
    std::unique_ptr<Bitmap> hiresBitmap;
#ifdef MKXPZ_SOFTWARE_BITMAPS
    swGuardHires();
#endif
    if (shState->config().enableHires && filenameStd.compare(0, hiresPrefix.size(), hiresPrefix) != 0) {
        std::string hiresFilename = hiresPrefix + filenameStd;
        try {
            hiresBitmap.reset(new Bitmap(hiresFilename.c_str()));
            hiresBitmap->setLores(this);
        }
        catch (const Exception &) {
            Debug() << "No high-res Bitmap found at" << hiresFilename;
            hiresBitmap.reset();
        }
    }

    BitmapOpenHandler handler;
    shState->fileSystem().openRead(handler, filename);
    if (!handler.error.empty())
        throw Exception(Exception::SDLError, "Error loading image '%s': %s", filename, handler.error.c_str());
    if (!handler.gif && !handler.surface)
        throw Exception(Exception::SDLError, "Error loading image '%s': %s", filename, SDL_GetError());

    if (!handler.gif) {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::AssetImageScope imageScope(handler.assetToken);
#endif
        const bool forceMega = hiresBitmap && hiresBitmap->isMega();
        initFromSurface(handler.surface.release(), hiresBitmap.release(), forceMega);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        p->assetToken = handler.assetToken;
#endif
        return;
    }

    gif_animation &gif = *handler.gif;
    if (gif.width > (uint32_t)glState.caps.maxTexSize || gif.height > (uint32_t)glState.caps.maxTexSize)
        throw Exception(Exception::MKXPError, "Animation too large (%ix%i, max %ix%i)",
                        gif.width, gif.height, glState.caps.maxTexSize, glState.caps.maxTexSize);

    try {
        p = new BitmapPrivate(this);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        p->assetToken = handler.assetToken;
#endif
        p->selfHires = hiresBitmap.release();
        p->gl.width = gif.width;
        p->gl.height = gif.height;
        if (gif.width > INT16_MAX || gif.height > INT16_MAX) {
            p->pixmanUseRegion32 = true;
            pixman_region_fini(&p->tainted);
            pixman_region32_init(&p->tainted32);
        }

        if (gif.frame_count == 1) {
#ifdef MKXPZ_SOFTWARE_BITMAPS
            p->cpuSurface = p->allocPixels(gif.width, gif.height);
            for (unsigned y = 0; y < gif.height; ++y)
                memcpy((uint8_t *)p->cpuSurface->pixels + (size_t)y * p->cpuSurface->pitch,
                       (const uint8_t *)gif.frame_image + (size_t)y * gif.width * 4,
                       (size_t)gif.width * 4);
            p->markDirty(rect());
#else
            p->gl = shState->texPool().request(gif.width, gif.height);
            TEX::bind(p->gl.tex);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            FrameProfile::Scope profileDirectUpload(FrameProfile::Upload, 0, false, false);
#endif
            TEX::uploadImage(gif.width, gif.height, gif.frame_image, GL_RGBA);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            profileDirectUpload.stop();
#endif
            if (p->selfHires)
                p->gl.selfHires = &p->selfHires->getGLTypes();
#endif
        }
        else {
            p->animation.width = gif.width;
            p->animation.height = gif.height;
            const unsigned delay = gif.frames[gif.decoded_frame].frame_delay;
            p->animation.fps = delay ? 100.0f / delay : std::max(1, shState->graphics().getFrameRate());
            p->animation.loop = gif.loop_count >= 0;
#ifdef MKXPZ_SOFTWARE_BITMAPS
            p->animation.cpuFrames.reserve(gif.frame_count_partial);
#else
            p->animation.frames.reserve(gif.frame_count_partial);
#endif
            for (unsigned i = 0; i < gif.frame_count_partial; ++i) {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                FrameProfile::Scope profileDecode(FrameProfile::Decode);
                FrameProfile::AssetDecodeScope assetDecode(profileDecode, p->assetToken);
#endif
                const int status = i ? gif_decode_frame(&gif, i) : GIF_OK;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                if (status == GIF_OK) assetDecode.result(gif.width, gif.height);
#endif
                if (status != GIF_OK)
                    throw Exception(Exception::MKXPError, "Failed to decode GIF frame %i out of %i (Status %i)",
                                    i + 1, gif.frame_count_partial, status);
#ifdef MKXPZ_SOFTWARE_BITMAPS
                BitmapPrivate::PixelOwner frame(p->allocPixels(gif.width, gif.height), BitmapPrivate::freePixels);
                for (unsigned y = 0; y < gif.height; ++y)
                    memcpy((uint8_t *)frame->pixels + (size_t)y * frame->pitch,
                           (const uint8_t *)gif.frame_image + (size_t)y * gif.width * 4,
                           (size_t)gif.width * 4);
                p->animation.cpuFrames.push_back(frame.get());
                frame.release();
#else
                p->animation.frames.push_back(shState->texPool().request(gif.width, gif.height));
                TEX::bind(p->animation.frames.back().tex);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                FrameProfile::Scope profileDirectUpload(FrameProfile::Upload, 0, false, false);
#endif
                TEX::uploadImage(gif.width, gif.height, gif.frame_image, GL_RGBA);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                profileDirectUpload.stop();
#endif
#endif
            }
            p->animation.enabled = true;
#ifdef MKXPZ_SOFTWARE_BITMAPS
            p->texDirty = true;
#endif
        }
        p->addTaintedArea(rect());
    }
    catch (...) {
        if (p) {
#ifndef MKXPZ_SOFTWARE_BITMAPS
            /* releaseResources uses this flag to distinguish the GPU owners. */
            p->animation.enabled = !p->animation.frames.empty();
#endif
            releaseResources();
            p = nullptr;
        }
        throw;
    }
}

Bitmap::Bitmap(int width, int height, bool isHires)
{
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
    {
        char tb[160];
        snprintf(tb, sizeof(tb), "trace: Bitmap::Bitmap w/h %d %d", width, height);
        vita_glue_trace(tb);
    }
#endif
    if (width <= 0 || height <= 0)
        throw Exception(Exception::RGSSError, "failed to create bitmap");

#ifdef MKXPZ_SOFTWARE_BITMAPS
    (void)isHires;
    swGuardHires();

    p = new BitmapPrivate(this);
    try {
        p->cpuSurface = p->allocPixels(width, height);
    } catch (const Exception &e) {
        delete p;
        p = nullptr;
        throw e;
    }

    /* Only genuinely oversized bitmaps keep the stock mega restrictions; every
     * other CPU-backed Bitmap is drawable. */
    p->trueMega = width > glState.caps.maxTexSize || height > glState.caps.maxTexSize;
    p->gl.width = width;
    p->gl.height = height;

    if (width > INT16_MAX || height > INT16_MAX)
    {
        p->pixmanUseRegion32 = true;
        pixman_region_fini(&p->tainted);
        pixman_region32_init(&p->tainted32);
    }

    clear();
    return;
#else
    Bitmap *hiresBitmap = nullptr;

    if (shState->config().enableHires && !isHires) {
        // Create a high-res version as well.
        double scalingFactor = shState->config().textureScalingFactor;
        int hiresWidth = (int)lround(scalingFactor * width);
        int hiresHeight = (int)lround(scalingFactor * height);
        hiresBitmap = new Bitmap(hiresWidth, hiresHeight, true);
        hiresBitmap->setLores(this);
    }

    if (width > glState.caps.maxTexSize || height > glState.caps.maxTexSize || (hiresBitmap && hiresBitmap->isMega()))
    {
        p = new BitmapPrivate(this);
        SDL_Surface *surface = SDL_CreateRGBSurface(0, width, height, p->format->BitsPerPixel,
                                                    p->format->Rmask,
                                                    p->format->Gmask,
                                                    p->format->Bmask,
                                                    p->format->Amask);
        if (!surface)
            throw Exception(Exception::SDLError, "Error creating Bitmap: %s",
                            SDL_GetError());
        p->megaSurface = surface;
        SDL_SetSurfaceBlendMode(p->megaSurface, SDL_BLENDMODE_NONE);
    }
    else
    {
        TEXFBO tex;
        TEXFBO::trace("Bitmap request BEGIN", tex, width, height);
        try {
            tex = shState->texPool().request(width, height);
        } catch (const Exception &e) {
            if (hiresBitmap)
                delete hiresBitmap;
            throw e;
        }
        
        TEXFBO::trace("Bitmap request END / private BEGIN", tex);
        try {
            p = new BitmapPrivate(this);
        } catch (...) {
            shState->texPool().release(tex);
            delete hiresBitmap;
            throw;
        }
        TEXFBO::trace("Bitmap private END", tex);
        p->gl = tex;
        p->selfHires = hiresBitmap;
        if (p->selfHires != nullptr) {
            p->gl.selfHires = &p->selfHires->getGLTypes();
        }
    }
    
    if (width > INT16_MAX || height > INT16_MAX)
    {
        p->pixmanUseRegion32 = true;
        pixman_region_fini(&p->tainted);
        pixman_region32_init(&p->tainted32);
    }
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    // RGSS thread only. Balance the constructor-only clear trace on exceptions.
    struct InitialClearTrace {
        InitialClearTrace() { vita_glue_texture_scope_enter(); }
        ~InitialClearTrace() { vita_glue_texture_scope_leave(); }
    } initialClearTrace;
#endif
    TEXFBO::trace("Bitmap initial clear BEGIN", p->gl, width, height);
    clear();
    TEXFBO::trace("Bitmap initial clear END", p->gl, width, height);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    // Validate only after the existing clear has realized the render surface.
    // Checking every TEXFBO at attachment would allocate surfaces for images
    // that are only sampled, increasing driver memory pressure.
    if (!p->megaSurface) {
        const GLenum status = ::glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            const GLenum error = gl.GetError();
            char message[224];
            snprintf(message, sizeof(message),
                     "vita-fbo: Bitmap %dx%d allocation failed: status=0x%x error=0x%x tex=%u fbo=%u",
                     width, height, (unsigned)status, (unsigned)error,
                     p->gl.tex.gl, p->gl.fbo.gl);
            vita_glue_trace(message);
            vita_glue_log_free_memory("vita-fbo: allocation failure");

            if (vita_glue_fbo_failure_probe_begin()) {
                // Read the actual binding and attachment without rebinding or
                // rechecking completeness (which can realize a render surface).
                GLint bound = 0, kind = 0, name = 0, level = -1;
                gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
                ::glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
                if (kind == GL_TEXTURE) {
                    ::glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
                    ::glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                        GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
                }
                snprintf(message, sizeof(message),
                         "vita-fbo-probe: bound=%u expected_fbo=%u attachment_type=0x%x name=%u expected_tex=%u level=%d",
                         (unsigned)bound, p->gl.fbo.gl, (unsigned)kind,
                         (unsigned)name, p->gl.tex.gl, (int)level);
                vita_glue_trace(message);
                vita_glue_fbo_failure_probe(width, height);
            }

            // The driver can retain a freed render-surface pointer on failure.
            // Delete the bound FBO directly: its delete path installs the
            // default framebuffer without locking the failed surface. Do not
            // glBindFramebuffer first, and never return this object to cache.
            TEXFBO::fini(p->gl);
            FBO::boundFramebufferID = FBO::ID(0);
            TEXFBO::clear(p->gl);
            releaseResources();
            p = nullptr;
            throw Exception(Exception::MKXPError,
                            "Bitmap render target allocation failed (%dx%d, status 0x%x, error 0x%x)",
                            width, height, (unsigned)status, (unsigned)error);
        }
        TEXFBO::trace("Bitmap framebuffer COMPLETE", p->gl);
    }
#endif
#endif
}

Bitmap::Bitmap(void *pixeldata, int width, int height)
{
#ifdef MKXPZ_SOFTWARE_BITMAPS
    swGuardHires();

    p = new BitmapPrivate(this);
    try {
        p->cpuSurface = p->allocPixels(width, height);
    } catch (const Exception &e) {
        delete p;
        p = nullptr;
        throw e;
    }

    memcpy(p->cpuSurface->pixels, pixeldata, (size_t)width * (size_t)height * 4);

    p->trueMega = width > glState.caps.maxTexSize || height > glState.caps.maxTexSize;
    p->gl.width = width;
    p->gl.height = height;
    p->markDirty(IntRect(0, 0, width, height));

    if (width > INT16_MAX || height > INT16_MAX)
    {
        p->pixmanUseRegion32 = true;
        pixman_region_fini(&p->tainted);
        pixman_region32_init(&p->tainted32);
    }
    p->addTaintedArea(rect());
    return;
#else
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
                                                        SDL_PIXELFORMAT_ABGR8888);
    
    if (!surface)
        throw Exception(Exception::SDLError, "Error creating Bitmap: %s",
                        SDL_GetError());
    
    memcpy(surface->pixels, pixeldata, (size_t)width * (size_t)height * 4);
    
    if (surface->w > glState.caps.maxTexSize || surface->h > glState.caps.maxTexSize)
    {
        p = new BitmapPrivate(this);
        p->megaSurface = surface;
        SDL_SetSurfaceBlendMode(p->megaSurface, SDL_BLENDMODE_NONE);
    }
    else
    {
        TEXFBO tex;
        
        try
        {
            tex = shState->texPool().request(surface->w, surface->h);
        }
        catch (const Exception &e)
        {
            SDL_FreeSurface(surface);
            throw e;
        }
        
        try {
            p = new BitmapPrivate(this);
        } catch (...) {
            shState->texPool().release(tex);
            SDL_FreeSurface(surface);
            throw;
        }
        p->gl = tex;
        
        TEX::bind(p->gl.tex);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileDirectUpload(FrameProfile::Upload, 0, false, false);
#endif
        TEX::uploadImage(p->gl.width, p->gl.height, surface->pixels, GL_RGBA);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        profileDirectUpload.stop();
#endif
        
        SDL_FreeSurface(surface);
    }
    
    if (width > INT16_MAX || height > INT16_MAX)
    {
        p->pixmanUseRegion32 = true;
        pixman_region_fini(&p->tainted);
        pixman_region32_init(&p->tainted32);
    }
    p->addTaintedArea(rect());
#endif
}

// frame is -2 for "any and all", -1 for "current", anything else for a specific frame
Bitmap::Bitmap(const Bitmap &other, int frame)
{
    other.guardDisposed();
    if (frame > -2) other.ensureAnimated();
    
    if (other.hasHires()) {
        Debug() << "BUG: High-res Bitmap from animation not implemented";
    }

    p = new BitmapPrivate(this);

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* A clone is a memcpy: no GPU round trip, no render target. */
    p->trueMega = other.p->trueMega;
    p->gl.width = other.width();
    p->gl.height = other.height();

    try {
        if (!other.isAnimated() || frame >= -1) {
            SDL_Surface *src;
            if (!other.isAnimated() || frame == -1)
                src = other.p->pixels();
            else
                src = other.p->animation.cpuFrames[
                    clamp(frame, 0, (int)other.p->animation.cpuFrames.size() - 1)];

            if (!src)
                throw Exception(Exception::MKXPError, "Bitmap has no CPU pixels");

            p->cpuSurface = p->copyPixels(src);
            p->markDirty(rect());
        }
        else {
            p->animation.fps = other.getAnimationFPS();
            p->animation.width = other.width();
            p->animation.height = other.height();
            p->animation.lastFrame = 0;
            p->animation.playTime = 0;
            p->animation.startTime = 0;
            p->animation.loop = other.getLooping();

            p->animation.cpuFrames.reserve(other.p->animation.cpuFrames.size());
            for (SDL_Surface *sourceframe : other.p->animation.cpuFrames) {
                BitmapPrivate::PixelOwner copy(p->copyPixels(sourceframe), BitmapPrivate::freePixels);
                p->animation.cpuFrames.push_back(copy.get());
                copy.release();
            }

            p->animation.enabled = true;
            p->texDirty = true;
        }
    } catch (...) {
        releaseResources();
        p = nullptr;
        throw;
    }
#else
    if (other.isMega())
    {
        p->megaSurface = SDL_ConvertSurfaceFormat(other.p->megaSurface, p->format->format, 0);
    }
    // TODO: Clean me up
    else if (!other.isAnimated() || frame >= -1) {
        try {
            p->gl = shState->texPool().request(other.width(), other.height());
        } catch (const Exception &e) {
            delete p;
            throw e;
        }
        
        GLMeta::blitBegin(p->gl, false, SameScale);
        // Blit just the current frame of the other animated bitmap
        if (!other.isAnimated() || frame == -1) {
            GLMeta::blitSource(other.getGLTypes(), SameScale);
        }
        else {
            auto &frames = other.getFrames();
            GLMeta::blitSource(frames[clamp(frame, 0, (int)frames.size() - 1)], SameScale);
        }
        GLMeta::blitRectangle(rect(), rect());
        GLMeta::blitEnd();
    }
    else {
        try {
            p->animation.frames.reserve(other.getFrames().size());
        } catch (...) {
            delete p;
            p = nullptr;
            throw;
        }
        p->animation.enabled = true;
        p->animation.fps = other.getAnimationFPS();
        p->animation.width = other.width();
        p->animation.height = other.height();
        p->animation.lastFrame = 0;
        p->animation.playTime = 0;
        p->animation.startTime = 0;
        p->animation.loop = other.getLooping();
        
        for (TEXFBO &sourceframe : other.getFrames()) {
            TEXFBO newframe;
            try {
                newframe = shState->texPool().request(p->animation.width, p->animation.height);
            } catch (...) {
                releaseResources();
                p = nullptr;
                throw;
            }
            
            GLMeta::blitBegin(newframe, false, SameScale);
            GLMeta::blitSource(sourceframe, SameScale);
            GLMeta::blitRectangle(rect(), rect());
            GLMeta::blitEnd();
            
            p->animation.frames.push_back(newframe);
        }
    }
#endif

    if (width() > INT16_MAX || height() > INT16_MAX)
    {
        p->pixmanUseRegion32 = true;
        pixman_region_fini(&p->tainted);
        pixman_region32_init(&p->tainted32);
        pixman_region32_copy(&p->tainted32, &other.p->tainted32);
    }
    else
    {
        pixman_region_copy(&p->tainted, &other.p->tainted);
    }
}

Bitmap::Bitmap(TEXFBO &other)
{
#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* The one genuine GPU -> Bitmap direction (Graphics::snapToBitmap). Read
     * the engine-owned render target back once into CPU pixels; every later
     * blit from this Bitmap is then software. */
    if (other.selfHires != nullptr)
        throw Exception(Exception::MKXPError,
                        "software_bitmaps: high-resolution snapshots are not supported");

    std::unique_ptr<BitmapPrivate> owned(new BitmapPrivate(this));
    p = owned.get();
    BitmapPrivate::PixelOwner pixels(p->allocPixels(other.width, other.height),
                                     BitmapPrivate::freePixels);

    p->gl.width = other.width;
    p->gl.height = other.height;

    GLenum error = GL_NO_ERROR;
    {
        struct ReadBinding {
            FBO::ID previous;
            ~ReadBinding() { FBO::bind(previous); }
        } binding = { FBO::boundFramebufferID };

        /* ReadPixels uses explicit pixel bounds, not the viewport stack.
         * Graphics' error scope retains any composition errors across this
         * local readback boundary, including errors consumed by tracing. */
        while (gl.GetError() != GL_NO_ERROR) {}
        FBO::bind(other.fbo);
        error = gl.GetError();
        if (error == GL_NO_ERROR) {
            gl.ReadPixels(0, 0, other.width, other.height, GL_RGBA, GL_UNSIGNED_BYTE,
                          pixels->pixels);
            error = gl.GetError();
        }
        while (gl.GetError() != GL_NO_ERROR) {}
    }
    const GLenum restoreError = gl.GetError();
    if (error == GL_NO_ERROR) error = restoreError;
    while (gl.GetError() != GL_NO_ERROR) {}
    if (error != GL_NO_ERROR)
        throw Exception(Exception::MKXPError,
                        "Bitmap snapshot readback failed (GL error 0x%x)", (unsigned)error);

    p->markDirty(rect());

    if (width() > INT16_MAX || height() > INT16_MAX)
    {
        p->pixmanUseRegion32 = true;
        pixman_region_fini(&p->tainted);
        pixman_region32_init(&p->tainted32);
    }
    p->addTaintedArea(rect());
    p->cpuSurface = pixels.release();
    owned.release();
    return;
#else
    Bitmap *hiresBitmap = nullptr;

    if (other.selfHires != nullptr) {
        // Create a high-res version as well.
        hiresBitmap = new Bitmap(*other.selfHires);
        hiresBitmap->setLores(this);
    }

    p = new BitmapPrivate(this);
    p->selfHires = hiresBitmap;

    try {
        p->gl = shState->texPool().request(other.width, other.height);
    } catch (const Exception &e) {
        delete p;
        throw e;
    }

    if (p->selfHires != nullptr) {
        p->gl.selfHires = &p->selfHires->getGLTypes();
    }

    // Skip blitting to lores texture, since only the hires one will be displayed.
    if (p->selfHires == nullptr) {
        GLMeta::blitBegin(p->gl, false, SameScale);
        GLMeta::blitSource(other, SameScale);
        GLMeta::blitRectangle(rect(), rect());
        GLMeta::blitEnd();
    }

    if (width() > INT16_MAX || height() > INT16_MAX)
    {
        p->pixmanUseRegion32 = true;
        pixman_region_fini(&p->tainted);
        pixman_region32_init(&p->tainted32);
    }
    p->addTaintedArea(rect());
#endif
}

Bitmap::Bitmap(SDL_Surface *imgSurf, SDL_Surface *imgSurfHires, bool forceMega)
{
    Bitmap *hiresBitmap = nullptr;

#ifdef MKXPZ_SOFTWARE_BITMAPS
    if (imgSurfHires != nullptr)
        throw Exception(Exception::MKXPError,
                        "software_bitmaps: high-resolution surfaces are not supported");
#endif
    if (imgSurfHires != nullptr) {
        // Create a high-res version as well.
        hiresBitmap = new Bitmap(imgSurfHires, nullptr);
        hiresBitmap->setLores(this);
    }

    initFromSurface(imgSurf, hiresBitmap, forceMega);
}

Bitmap::~Bitmap()
{
    dispose();

    loresDispCon.disconnect();
}

void Bitmap::initFromSurface(SDL_Surface *imgSurf, Bitmap *hiresBitmap, bool forceMega)
{
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
    {
        char tb[160];
        snprintf(tb, sizeof(tb),
                 "trace: Bitmap::initFromSurface %dx%d forceMega=%d",
                 imgSurf ? imgSurf->w : -1, imgSurf ? imgSurf->h : -1, (int)forceMega);
        vita_glue_trace(tb);
    }
#endif
    std::unique_ptr<Bitmap> ownedHires(hiresBitmap);
    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> ownedSurface(imgSurf, SDL_FreeSurface);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    /* The load path's convert stage: counts the check too, which is free when
     * the decode already produced ABGR8888. */
    { FrameProfile::AssetStageScope convert(FrameProfile::AssetStageScope::Convert);
#endif
    BitmapPrivate::ensureFormat(imgSurf, SDL_PIXELFORMAT_ABGR8888);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    }
#endif
    ownedSurface.release();
    ownedSurface.reset(imgSurf);

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* The decoded surface simply becomes the Bitmap's pixels; no upload until
     * something samples it. Mega stays exactly the stock condition. */
    p = new BitmapPrivate(this);
    p->selfHires = ownedHires.release();
    p->cpuSurface = ownedSurface.release();
    BitmapPrivate::countPixels(p->cpuSurface, +1);
    SDL_SetSurfaceBlendMode(p->cpuSurface, SDL_BLENDMODE_NONE);
    p->trueMega = imgSurf->w > glState.caps.maxTexSize ||
                  imgSurf->h > glState.caps.maxTexSize || forceMega;
    p->gl.width = imgSurf->w;
    p->gl.height = imgSurf->h;
    p->markDirty(IntRect(0, 0, imgSurf->w, imgSurf->h));
#else
    if (imgSurf->w > glState.caps.maxTexSize || imgSurf->h > glState.caps.maxTexSize || forceMega)
    {
        /* Mega surface */

        p = new BitmapPrivate(this);
        p->selfHires = ownedHires.release();
        p->megaSurface = ownedSurface.release();
        SDL_SetSurfaceBlendMode(p->megaSurface, SDL_BLENDMODE_NONE);
    }
    else
    {
        /* Regular surface */
        TEXFBO tex;
        
        tex = shState->texPool().request(imgSurf->w, imgSurf->h);
        
        try {
            p = new BitmapPrivate(this);
        } catch (...) {
            shState->texPool().release(tex);
            throw;
        }
        p->selfHires = ownedHires.release();
        p->gl = tex;
        if (p->selfHires != nullptr) {
            p->gl.selfHires = &p->selfHires->getGLTypes();
        }
        
        TEX::bind(p->gl.tex);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileDirectUpload(FrameProfile::Upload, 0, false, false);
#endif
        TEX::uploadImage(p->gl.width, p->gl.height, imgSurf->pixels, GL_RGBA);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        profileDirectUpload.stop();
#endif
    }
#endif

    if (width() > INT16_MAX || height() > INT16_MAX)
    {
        p->pixmanUseRegion32 = true;
        pixman_region_fini(&p->tainted);
        pixman_region32_init(&p->tainted32);
    }
    p->addTaintedArea(rect());
}

/* "Child" bitmaps are a hack to support mega surfaces in Windows, Planes, and Sprites.
 * They determine which part of the parent will be visible, manually shrink it if necessary,
 * and send back new values for zoom and offsets. */

struct ChildPrivate
{
    Bitmap *self;
    Bitmap *parent;
    
    ChildPublic shared;
    
    sigslot::connection dirtyCon;
    sigslot::connection disposeCon;
    
    Vec2i parentPos;
    IntRect srcRect;
    IntRect oldSrcRect;
    bool dirty;
    Vec2 maxShrink;
    Vec2 currentZoom;
    Vec2 currentShrink;
    bool mirrored;
    int currentBushDepth;
    Transform *trans;
    IntRect oldVR;
    Vec2i oldOff;
    
    
    ChildPrivate(Bitmap *self, Bitmap *parent)
    : self(self),
    parent(parent),
    dirty(true),
    mirrored(false)
    {
        shared.width = parent->width();
        shared.height = parent->height();
        
        shared.realSrcRect.w = parent->width();
        shared.realSrcRect.h = parent->height();
        shared.srcRect.w = parent->width();
        shared.srcRect.h = parent->height();
        oldSrcRect = shared.realSrcRect;
        
        maxShrink.x = (float)self->width() / parent->width();
        maxShrink.y = (float)self->height() / parent->height();
        currentZoom.x = 1.0f;
        currentZoom.y = 1.0f;
        currentShrink.x = 1.0f;
        currentShrink.y = 1.0f;
        
        dirtyCon = parent->modified.connect(&ChildPrivate::childDirty, this);
        disposeCon = parent->wasDisposed.connect(&ChildPrivate::parentDisposed, this);
    }
    
    ~ChildPrivate()
    {
        dirtyCon.disconnect();
        disposeCon.disconnect();
    }
    
    void childDirty()
    {
        dirty = true;
    }
    
    void parentDisposed()
    {
        self->dispose();
    }
};

Bitmap *Bitmap::spawnChild()
{
    Bitmap *child;
    if(p->selfHires)
    {
        int childWidth = std::min(p->selfHires->width(), glState.caps.maxTexSize);
        int childHeight = std::min(p->selfHires->height(), glState.caps.maxTexSize);
        double scalingFactor = std::max(p->selfHires->width() / width(), p->selfHires->height() / height());
        double maxRatio = std::min((double)childWidth / shState->graphics().width(),
                                   (double)childHeight / shState->graphics().height());
        scalingFactor = std::min(maxRatio, scalingFactor);
        int loresWidth = (int)lround(scalingFactor * childWidth);
        int loresHeight = (int)lround(scalingFactor * childHeight);
        child = new Bitmap(loresWidth, loresHeight, true);
        Bitmap *hires = new Bitmap(childWidth, childHeight, true);
        hires->setLores(child);
        child->p->selfHires = hires;
    }
    else
    {
        int childWidth = std::min(width(), glState.caps.maxTexSize);
        int childHeight = std::min(height(), glState.caps.maxTexSize);
        child = new Bitmap(childWidth, childHeight, true);
    }
    
    
    child->p->pChild = new ChildPrivate(child, this);
    
    return child;
}

ChildPublic *Bitmap::getChildInfo()
{
    if (p->pChild)
        return &p->pChild->shared;
    return 0;
}

void Bitmap::childUpdate()
{
    if (!p->pChild)
        return;
    
    ChildPrivate *pChild = p->pChild;
    
    bool isWindow = pChild->shared.realZoom.x == -1.0f;
    bool isPlane = pChild->shared.wrap;
    bool isSprite = !isWindow && !isPlane;
    
    if (!pChild->shared.realZoom.x || !pChild->shared.realZoom.y)
    {
        pChild->shared.isVisible = false;
        return;
    }
    
    IntRect viewportRect(0, 0, shState->graphics().width(), shState->graphics().height());
    
    if (!SDL_IntersectRect(&viewportRect, pChild->shared.sceneRect, &viewportRect))
    {
        pChild->shared.zoom.x = pChild->shared.realZoom.x;
        pChild->shared.zoom.y = pChild->shared.realZoom.y;
        pChild->shared.isVisible = false;
        return;
    }
    
    if (isWindow)
    {
        viewportRect.x = pChild->shared.sceneRect->x;
        viewportRect.y = pChild->shared.sceneRect->y;
        IntRect window(pChild->shared.x + viewportRect.x - pChild->shared.sceneOrig->x,
                       pChild->shared.y + viewportRect.y - pChild->shared.sceneOrig->y,
                       pChild->shared.width, pChild->shared.height);
        if (!SDL_IntersectRect(&viewportRect, &window, &viewportRect))
        {
            pChild->shared.isVisible = false;
            return;
        }
        viewportRect.x = std::min(0, window.x);
        viewportRect.y = std::min(0, window.y);
    }
    
    bool updateNeeded = pChild->dirty;
    
    IntRect visibleRect = viewportRect;
    
    Vec2 realZoom(abs(pChild->shared.realZoom.x), abs(pChild->shared.realZoom.y));
    Vec2 shrink(1.0f, 1.0f);
    
    IntRect adjustedSrcRect = pChild->shared.realSrcRect;
    if (isSprite)
    {
        if (pChild->shared.realSrcRect.x < 0)
            adjustedSrcRect.w += pChild->shared.realSrcRect.x;
        if (pChild->shared.realSrcRect.y < 0)
            adjustedSrcRect.h += pChild->shared.realSrcRect.y;
        adjustedSrcRect.x = clamp(adjustedSrcRect.x, 0, pChild->parent->width());
        adjustedSrcRect.y = clamp(adjustedSrcRect.y, 0, pChild->parent->height());
        adjustedSrcRect.w = clamp(adjustedSrcRect.w, 0, pChild->shared.width - adjustedSrcRect.x);
        adjustedSrcRect.h = clamp(adjustedSrcRect.h, 0, pChild->shared.height - adjustedSrcRect.y);
        
        if (!adjustedSrcRect.w || !adjustedSrcRect.h)
        {
            pChild->shared.isVisible = false;
            return;
        }
    }
    else
        adjustedSrcRect = pChild->shared.realSrcRect;
    
    if (isPlane || isSprite)
    {
        visibleRect.x = pChild->shared.x - pChild->shared.sceneOrig->x + std::min(pChild->shared.sceneRect->x, 0);
        visibleRect.y = pChild->shared.y - pChild->shared.sceneOrig->y + std::min(pChild->shared.sceneRect->y, 0);
        
        if (pChild->shared.angle)
        {
            // rotate visibleRect clockwise around visibleRect.x and visibleRect.y
            FloatRect tmpRect = rotate_rect(visibleRect.pos(), -pChild->shared.angle,
                                       IntRect(Vec2i(),visibleRect.size()));
            tmpRect.x = floor(-tmpRect.x) + visibleRect.x;
            tmpRect.y = floor(-tmpRect.y) + visibleRect.y;
            visibleRect = tmpRect;
        }
        
        if (pChild->shared.waveAmp > 0)
        {
            /* At the moment the wave gets rotated too, which isn't what RGSS does.
               If that's ever fixed, then this needs to be moved to before the rotation. */
            
            /* The edge of the wave can still poke through sometimes for some reason,
               so we provide an extra 1 pixel buffer to ensure it can't happen. */
            visibleRect.x += pChild->shared.waveAmp + 1;
            visibleRect.w += pChild->shared.waveAmp * 2 + 2;
        }
        
        // maxShrink is the point at which the entire parent fits into the child
        Vec2 maxShrink;
        if (isSprite)
        {
            maxShrink.x = std::min((float)width() / adjustedSrcRect.w, 1.0f);
            maxShrink.y = std::min((float)height() / adjustedSrcRect.h, 1.0f);
        }
        else // Planes can just use the cached values
        {
            maxShrink = pChild->maxShrink;
        }
        shrink.x = clamp(std::min(width(), adjustedSrcRect.w) * realZoom.x / visibleRect.w, maxShrink.x, 1.0f);
        shrink.y = clamp(std::min(height(), adjustedSrcRect.h) * realZoom.y / visibleRect.h, maxShrink.y, 1.0f);
        
        // Uncomment to force max shrink for testing
        /*
        shrink.x = std::min(pChild->maxShrink.x, 1.0f);
        shrink.y = std::min(pChild->maxShrink.y, 1.0f);
        //*/
        
        pChild->shared.zoom.x = realZoom.x / shrink.x;
        pChild->shared.zoom.y = realZoom.y / shrink.y;
        if(!(shrink == pChild->currentShrink))
            updateNeeded = true;
        
        visibleRect.x = round(visibleRect.x / realZoom.x);
        visibleRect.y = round(visibleRect.y / realZoom.y);
        visibleRect.w = ceil(visibleRect.w / realZoom.x);
        visibleRect.h = ceil(visibleRect.h / realZoom.y);
        if (pChild->shared.wrap)
        {
            visibleRect.x = -wrapRange(-visibleRect.x, 0, adjustedSrcRect.w);
            visibleRect.y = -wrapRange(-visibleRect.y, 0, adjustedSrcRect.h);
        }
    }
    
    int realOX = pChild->shared.realOffset.x;
    int realOY = pChild->shared.realOffset.y;
    
    if (isSprite)
    {
        if (pChild->shared.realSrcRect.x < 0)
            realOX += pChild->shared.realSrcRect.x;
        if (pChild->shared.realSrcRect.y < 0)
            realOY += pChild->shared.realSrcRect.y;
    }
    
    
    // If none of this has changed, then we can just return now
    if (!updateNeeded && pChild->oldVR == visibleRect && pChild->oldOff == Vec2i(realOX, realOY) &&
        (pChild->shared.wrap ||
         (pChild->mirrored == pChild->shared.mirrored && pChild->shared.realSrcRect == pChild->oldSrcRect))
       )
    {
        return;
    }
    pChild->oldOff = Vec2i(realOX, realOY);
    pChild->oldVR = visibleRect;
    
    if (!isPlane)
    {
        // Double the visibleRect.pos, because I should be using a
        // zeroed out position for the visibleRect but doing this is easier
        IntRect tmpSourceRect(visibleRect.pos() * 2 - Vec2i(realOX, realOY),
                              adjustedSrcRect.size());
        if (!SDL_HasIntersection(&visibleRect, &tmpSourceRect))
        {
            pChild->shared.isVisible = false;
            return;
        }
        if (pChild->shared.angle)
        {
            // Rotating the viewport leaves triangles on all sides that are considered in bounds.
            // By also rotating the source rect and comparing it to the unrotated viewport, we can
            // be certain if the sprite is visible or not.
            tmpSourceRect.x = floor(-realOX * realZoom.x);
            tmpSourceRect.y = floor(-realOY * realZoom.y);
            tmpSourceRect.w = ceil(tmpSourceRect.w * realZoom.x);
            tmpSourceRect.h = ceil(tmpSourceRect.h * realZoom.x);
            FloatRect tmpRect = rotate_rect(Vec2i(), pChild->shared.angle, tmpSourceRect);
            Vec2i origin(pChild->shared.x - pChild->shared.sceneOrig->x + std::min(pChild->shared.sceneRect->x, 0),
                         pChild->shared.y - pChild->shared.sceneOrig->y + std::min(pChild->shared.sceneRect->y, 0));
            tmpRect.x = floor(tmpRect.x) + origin.x;
            tmpRect.y = floor(tmpRect.y) + origin.y;
            tmpSourceRect = tmpRect;
            
            if (!SDL_HasIntersection(&viewportRect, &tmpSourceRect))
            {
                pChild->shared.isVisible = false;
                return;
            }
        }
    }
    
    pChild->shared.isVisible = true;
    
    int selfWidth = round(width() / shrink.x);
    int selfHeight = round(height() / shrink.y);
    
    int overflowX = std::max(selfWidth - visibleRect.w, 0);
    int overflowY = std::max(selfHeight - visibleRect.h, 0);
    
    int minOX = pChild->parentPos.x;
    int minOY = pChild->parentPos.y;
    int maxOX = minOX + overflowX;
    int maxOY = minOY + overflowY;
    int maxOX2 = wrapRange(maxOX, 0, adjustedSrcRect.w);
    int maxOY2 = wrapRange(maxOY, 0, adjustedSrcRect.h);
    
    int adjustedrealOX = -visibleRect.x + realOX;
    int adjustedrealOY = -visibleRect.y + realOY;
    
    // The position in the srcRect that the child pulls from. Initialized to the previous run's result.
    Vec2i newParentPos = pChild->parentPos;
    
    if (pChild->shared.wrap)
    {
        adjustedrealOX = wrapRange(adjustedrealOX, 0, adjustedSrcRect.w);
        adjustedrealOY = wrapRange(adjustedrealOY, 0, adjustedSrcRect.h);
    }
    
    for (int i = 0; i < 2; i++)
    {
        if (updateNeeded || (adjustedrealOX < minOX && (!pChild->shared.wrap || maxOX2 == maxOX || adjustedrealOX > maxOX2)) || adjustedrealOX > maxOX)
        {
            if (selfWidth >= adjustedSrcRect.w)
                newParentPos.x = 0;
            else
                newParentPos.x = adjustedrealOX - overflowX / 2;
            if (!pChild->shared.wrap)
                newParentPos.x = clamp(newParentPos.x, 0,
                                       std::max(adjustedSrcRect.w - selfWidth,0));
        }
        if (updateNeeded || (adjustedrealOY < minOY && (!pChild->shared.wrap || maxOY2 == maxOY || adjustedrealOY > maxOY2)) || adjustedrealOY > maxOY)
        {
            if (selfHeight >= adjustedSrcRect.h)
                newParentPos.y = 0;
            else
                newParentPos.y = adjustedrealOY - overflowY / 2;
            if (!pChild->shared.wrap)
                newParentPos.y = clamp(newParentPos.y, 0,
                                       std::max(adjustedSrcRect.h - selfHeight,0));
        }
        if (updateNeeded)
        {
            pChild->parentPos = newParentPos;
        }
        // If either x or y was updated, run through it again to update the other one
        if (newParentPos != pChild->parentPos)
            updateNeeded = true;
        else
            break;
    }
    
    
    if (!isSprite)
    {
        pChild->shared.offset.x = realOX - newParentPos.x;
        pChild->shared.offset.y =  realOY - newParentPos.y;
    }
    
    if (isPlane)
    {
        pChild->shared.offset.x = wrapRange(pChild->shared.offset.x - visibleRect.x, 0,
                                            adjustedSrcRect.w);
        pChild->shared.offset.y = wrapRange(pChild->shared.offset.y - visibleRect.y, 0,
                                            adjustedSrcRect.h);
        
        // Leaving this as a float (and making plane.cpp store it as a float)
        // makes positioning almost perfect when zoomed
        pChild->shared.offset.x = pChild->shared.offset.x * realZoom.x;
        pChild->shared.offset.y = pChild->shared.offset.y * realZoom.y;
        
        pChild->shared.offset.x -= pChild->shared.sceneOrig->x;
        pChild->shared.offset.y -= pChild->shared.sceneOrig->y;
        
        pChild->shared.offset.x += std::min(pChild->shared.sceneRect->x, 0);
        pChild->shared.offset.y += std::min(pChild->shared.sceneRect->y, 0);
    }
    else if (isSprite)
    {
        if (!updateNeeded && pChild->oldSrcRect != pChild->shared.realSrcRect)
        {
            if (pChild->srcRect.encloses(adjustedSrcRect))
            {
                pChild->shared.srcRect = IntRect(pChild->shared.realSrcRect.pos() - pChild->srcRect.pos(),
                                                 pChild->shared.realSrcRect.size());
                
                pChild->shared.srcRect.x = floor(pChild->shared.srcRect.x * shrink.x);
                pChild->shared.srcRect.y = floor(pChild->shared.srcRect.y * shrink.y);
                pChild->shared.srcRect.w = round(pChild->shared.srcRect.w * shrink.x);
                pChild->shared.srcRect.h = round(pChild->shared.srcRect.h * shrink.y);
            }
            else
                updateNeeded = true;
        }
        pChild->oldSrcRect = pChild->shared.realSrcRect;
        // Sprite stores the offsets as floats, and they get jittery when shrunk if we try to use ints,
        // so we just leave it as a float and it works perfectly.
        // We also use the srcRect to position the subimage for sprites instead of modifying the offset.
        // It makes positioning the wave and bush effect a lot simpler.
        pChild->shared.offset.x = pChild->shared.realOffset.x * shrink.x;
        pChild->shared.offset.y = pChild->shared.realOffset.y * shrink.y;
        
        if (pChild->shared.mirrored)
        {
            newParentPos.x = std::max(adjustedSrcRect.w - selfWidth, 0) - newParentPos.x;
        }
        
        if (pChild->mirrored != pChild->shared.mirrored && selfWidth != adjustedSrcRect.w)
            updateNeeded = true;
        pChild->mirrored = pChild->shared.mirrored;
    }
    
    if (updateNeeded)
    {
        if (pChild->shared.wrap)
        {
            newParentPos.x = wrapRange(newParentPos.x, 0, adjustedSrcRect.w);
            newParentPos.y = wrapRange(newParentPos.y, 0, adjustedSrcRect.h);
        }
        
        std::vector<IntRect> subrects;
        long locNum = 1;
        IntRect baseRect(newParentPos.x + adjustedSrcRect.x,
                         newParentPos.y + adjustedSrcRect.y,
                         std::min(selfWidth, adjustedSrcRect.w - newParentPos.x),
                         std::min(selfHeight, adjustedSrcRect.h - newParentPos.y));
        
        if (isSprite)
        {
            int deltaW = selfWidth - baseRect.w;
            int deltaH = selfHeight - baseRect.h;
            
            if (deltaW)
            {
                baseRect.x = clamp(baseRect.x - (int)ceil(deltaW / 2.0f), 0, pChild->parent->width() - selfWidth);
                baseRect.w = selfWidth;
            }
            if (deltaH)
            {
                baseRect.y = clamp(baseRect.y - (int)ceil(deltaH / 2.0f), 0, pChild->parent->height() - selfHeight);
                baseRect.h = selfHeight;
            }
            
            if (adjustedSrcRect.w > baseRect.w && pChild->mirrored)
            {
                float x = (pChild->shared.realSrcRect.x + pChild->shared.realSrcRect.w) - (baseRect.x + baseRect.w);
                pChild->shared.srcRect.x = (std::min(pChild->shared.realSrcRect.x, 0) - x) * shrink.x;
            }
            else
            {
                pChild->shared.srcRect.x = (pChild->shared.realSrcRect.x - baseRect.x) * shrink.x;
            }
            pChild->shared.srcRect.w = pChild->shared.realSrcRect.w * shrink.x;
            pChild->shared.srcRect.y = (pChild->shared.realSrcRect.y - baseRect.y) * shrink.y;
            pChild->shared.srcRect.h = pChild->shared.realSrcRect.h * shrink.y;
            
            pChild->srcRect = baseRect;
        }
        
        subrects.push_back(baseRect);
        if (pChild->shared.wrap && baseRect.w < selfWidth)
        {
            locNum *= 2;
            subrects.push_back(IntRect(0, baseRect.y,
                                                selfWidth - baseRect.w,
                                                baseRect.h));
        }
        if (pChild->shared.wrap && baseRect.h < selfHeight)
        {
            locNum *= 2;
            subrects.push_back(IntRect(baseRect.x, 0,
                                                baseRect.w,
                                                selfHeight - baseRect.h));
        }
        if (locNum == 4)
        {
            subrects.push_back(IntRect(0, 0,
                                                selfWidth - baseRect.w,
                                                selfHeight - baseRect.h));
        }
        
        clear();
        
        int bufferX = 0;
        int bufferY = 0;
        for (long i = 0; i < locNum; i++)
        {
            IntRect sourceRect = subrects[i];
            IntRect destRect(sourceRect.x == baseRect.x ? 0 : bufferX,
                             sourceRect.y == baseRect.y ? 0 : bufferY,
                             sourceRect.x == baseRect.x ? round(sourceRect.w * shrink.x) : width() - bufferX,
                             sourceRect.y == baseRect.y ? round(sourceRect.h * shrink.y) : height() - bufferY);
            if (!bufferX)
            {
                bufferX = destRect.w;
                bufferY = destRect.h;
            }
            stretchBlt(destRect, *pChild->parent, sourceRect, 255);
        }
        
        pChild->dirty = false;
        pChild->currentShrink = shrink;
    }
}

int Bitmap::width() const
{
    guardDisposed();
    
    if (p->megaSurface) {
        return p->megaSurface->w;
    }
    
    if (p->animation.enabled) {
        return p->animation.width;
    }
    
    return p->gl.width;
}

int Bitmap::height() const
{
    guardDisposed();
    
    if (p->megaSurface)
        return p->megaSurface->h;
    
    if (p->animation.enabled)
        return p->animation.height;
    
    return p->gl.height;
}

bool Bitmap::hasHires() const{
    guardDisposed();

    return p->selfHires;
}

DEF_ATTR_RD_SIMPLE(Bitmap, Hires, Bitmap*, p->selfHires)

void Bitmap::setHires(Bitmap *hires) {
    guardDisposed();

#ifdef MKXPZ_SOFTWARE_BITMAPS
    (void)hires;
    throw Exception(Exception::MKXPError,
                    "software_bitmaps: high-resolution texture replacement "
                    "is not supported");
#else
    hires->setLores(this);
    p->selfHires = hires;
#endif
}

void Bitmap::setLores(Bitmap *lores) {
    guardDisposed();

    p->selfLores = lores;
    loresDispCon = lores->wasDisposed.connect(&Bitmap::loresDisposal, this);

    if (p->font && p->font != &shState->defaultFont())
        p->font->setHiresMult((float)width() / (float)lores->width());
}

bool Bitmap::isMega() const{
    guardDisposed();

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* Every Bitmap is CPU-backed, but only genuinely oversized ones keep the
     * stock "mega" restrictions (not drawable, no texture). */
    return p->trueMega;
#else
    return p->megaSurface;
#endif
}

bool Bitmap::isAnimated() const {
    guardDisposed();
    
    return p->animation.enabled;
}

IntRect Bitmap::rect() const
{
    guardDisposed();
    
    return IntRect(0, 0, width(), height());
}

void Bitmap::blt(int x, int y,
                 const Bitmap &source, const IntRect &rect,
                 int opacity)
{
    if (source.isDisposed())
        return;
    
    stretchBlt(IntRect(x, y, abs(rect.w), abs(rect.h)),
               source, rect, opacity);
}

static bool shrinkRects(float &sourcePos, float &sourceLen, const int &sBitmapLen,
                         float &destPos, float &destLen, const int &dBitmapLen, bool normalize = false)
{
    float sStart = sourceLen > 0 ? sourcePos : sourceLen + sourcePos;
    float sEnd = sourceLen > 0 ? sourceLen + sourcePos : sourcePos;
    float sLength = sEnd - sStart;
    
    if (sStart >= 0 && sEnd < sBitmapLen)
        return false;
    
    if (sStart >= sBitmapLen || sEnd < 0)
        return true;
    
    float dStart = destLen > 0 ? destPos: destLen + destPos;
    float dEnd = destLen > 0 ? destLen + destPos : destPos;
    float dLength = dEnd - dStart;
    
    float delta = sEnd - sBitmapLen;
    float dDelta;
    if (delta > 0)
    {
        dDelta = (delta / sLength) * dLength;
        sLength -= delta;
        sEnd = sBitmapLen;
        dEnd -= dDelta;
        dLength -= dDelta;
    }
    if (sStart < 0)
    {
        dDelta = (sStart / sLength) * dLength;
        sLength += sStart;
        sStart = 0;
        dStart -= dDelta;
        dLength += dDelta;
    }
    
    if (!normalize)
    {
        sourcePos = sourceLen > 0 ? sStart : sEnd;
        sourceLen = sourceLen > 0 ? sLength : -sLength;
        destPos = destLen > 0  ? dStart : dEnd;
        destLen = destLen > 0 ? dLength : -dLength;
    }
    else
    {
        // Ensure the source rect has positive dimensions, for blitting from mega surfaces
        destPos = (destLen > 0 == sourceLen > 0) ? dStart : dEnd;
        destLen = (destLen > 0 == sourceLen > 0) ? dLength : -dLength;
        sourcePos = sStart;
        sourceLen = sLength;
    }
    
    return false;
}

static bool shrinkRects(int &sourcePos, int &sourceLen, const int &sBitmapLen,
                         int &destPos, int &destLen, const int &dBitmapLen)
{
    float fSourcePos = sourcePos;
    float fSourceLen = sourceLen;
    float fDestPos = destPos;
    float fDestLen = destLen;
    
    bool ret = shrinkRects(fSourcePos, fSourceLen, sBitmapLen, fDestPos, fDestLen, dBitmapLen, true);
    
    if (!ret)
        ret = shrinkRects(fDestPos, fDestLen, dBitmapLen, fSourcePos, fSourceLen, sBitmapLen);
    
    sourcePos = round(fSourcePos);
    sourceLen = round(fSourceLen);
    destPos = round(fDestPos);
    destLen = round(fDestLen);
    
    return ret || sourceLen == 0 || destLen == 0;
}

#ifndef MKXPZ_SOFTWARE_BITMAPS
static float bltNormOpacity(enum Bitmap::BitmapBltMode mode, int opacity)
{
    opacity = clamp(opacity, 0, 255);

    switch (mode)
    {
        case Bitmap::NORMAL:
            return (float)opacity / 255.0f;

        case Bitmap::KGL_SUBTRACT:
            return opacity >= 255 ? 1.0f : (float)opacity / 256.0f;
    }
}

static void bltFilter(enum Bitmap::BitmapBltMode mode, uint32_t &dst_pixel, uint32_t src_pixel, float norm_opacity)
{
    switch (mode)
    {
        case Bitmap::NORMAL:
            __builtin_unreachable();
            break;

        case Bitmap::KGL_SUBTRACT:
            for (size_t i = 0; i < 3; ++i)
            {
                uint8_t &old_component = ((uint8_t *)&dst_pixel)[i];
                uint8_t old_component_with_opacity = (uint8_t)std::round(norm_opacity * (float)old_component);
                uint8_t new_component = ((uint8_t *)&src_pixel)[i];
                old_component = new_component > old_component_with_opacity ? new_component - old_component_with_opacity : 0;
            }
            ((uint8_t *)&dst_pixel)[3] = 255;
            break;
    }
}

static uint32_t &getPixelAt(SDL_Surface *surf, SDL_PixelFormat *form, int x, int y)
{
    size_t offset = x*form->BytesPerPixel + y*surf->pitch;
    uint8_t *bytes = (uint8_t*) surf->pixels + offset;
    
    return *((uint32_t*) bytes);
}
#endif

void Bitmap::stretchBlt(IntRect destRect,
                        const Bitmap &source, IntRect sourceRect,
                        int opacity, bool smooth,
                        enum BitmapBltMode mode)
{
    guardDisposed();

    if (source.isDisposed())
        return;

#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
    {
        char tb[192];
        snprintf(tb, sizeof(tb),
                 "trace: Bitmap::stretchBlt dest=%d,%d %dx%d src=%d,%d %dx%d",
                 destRect.x, destRect.y, destRect.w, destRect.h,
                 sourceRect.x, sourceRect.y, sourceRect.w, sourceRect.h);
        vita_glue_trace(tb);
    }
#endif
    if (hasHires()) {
        int destX, destY, destWidth, destHeight;
        destX = destRect.x * p->selfHires->width() / width();
        destY = destRect.y * p->selfHires->height() / height();
        destWidth = destRect.w * p->selfHires->width() / width();
        destHeight = destRect.h * p->selfHires->height() / height();

        p->selfHires->stretchBlt(IntRect(destX, destY, destWidth, destHeight), source, sourceRect, opacity);
        return;
    }

    if (source.hasHires()) {
        int sourceX, sourceY, sourceWidth, sourceHeight;
        sourceX = sourceRect.x * source.getHires()->width() / source.width();
        sourceY = sourceRect.y * source.getHires()->height() / source.height();
        sourceWidth = sourceRect.w * source.getHires()->width() / source.width();
        sourceHeight = sourceRect.h * source.getHires()->height() / source.height();

        stretchBlt(destRect, *source.getHires(), IntRect(sourceX, sourceY, sourceWidth, sourceHeight), opacity);
        return;
    }

    opacity = clamp(opacity, 0, 255);
    
    if (opacity == 0)
        switch (mode) {
            case NORMAL:
                return;
            case KGL_SUBTRACT:
                break;
        }
    
#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* One software path for every combination: mega or not, surface source or
     * not, opacity 255 or not, 1:1 or scaled, mirrored or not. swraster::blit
     * is the RGSS "over" of shader/bitmapBlit.frag and does mkxp's shrinkRects
     * clipping itself, so do not pre-shrink here. The whole GPU
     * read-modify-write dance through gpTexFBO disappears with it. */
    {
        /* Stock returns before onModified() when the clipped result is empty,
         * and Tilemap/child bitmaps rebuild on that signal — so probe the same
         * clip on copies and bail identically. swraster::blit does the real
         * clipping itself, on the untouched rects. */
        IntRect probeSrc = sourceRect, probeDst = destRect;
        if (shrinkRects(probeSrc.x, probeSrc.w, source.width(),
                        probeDst.x, probeDst.w, width()))
            return;
        if (shrinkRects(probeSrc.y, probeSrc.h, source.height(),
                        probeDst.y, probeDst.h, height()))
            return;

        swraster::Surface dstPx = p->swSurface();
        swraster::Surface srcPx = source.p->swSurface();

        /* Both blits live in swraster and share its clipping, mirroring and
         * sampling: KGL_SUBTRACT is shader/kglSubtract.frag and honours
         * `smooth` exactly as the GL path's TEX::setSmooth(true) does.
         * swraster::subtract_blit applies bltNormOpacity's /256 factor for
         * this mode itself, and deliberately has no opacity == 0 early out
         * (the alpha write still lands). */
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileRasterBlend(FrameProfile::Raster);
#endif
        if (mode == KGL_SUBTRACT)
            swraster::subtract_blit(dstPx, swRect(destRect), srcPx,
                                    swRect(sourceRect), opacity, smooth);
        else
            swraster::blit(dstPx, swRect(destRect), srcPx, swRect(sourceRect),
                           opacity, smooth);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        profileRasterBlend.stop();
#endif

        p->addTaintedArea(destRect);
        p->markDirty(destRect);
        p->onModified();
        return;
    }
#else
    float normOpacity = bltNormOpacity(mode, opacity);

    if(shrinkRects(sourceRect.x, sourceRect.w, source.width(), destRect.x, destRect.w, width()))
        return;
    if(shrinkRects(sourceRect.y, sourceRect.h, source.height(), destRect.y, destRect.h, height()))
        return;
    
    SDL_Surface *srcSurf = source.megaSurface();
    SDL_Surface *blitTemp = 0;
    bool touchesTaintedArea = mode != NORMAL || opacity < 255 || p->touchesTaintedArea(destRect);
    bool unpack_subimage = srcSurf && gl.unpack_subimage;

    const bool scaleIsOne = sourceRect.w == destRect.w && sourceRect.h == destRect.h;
    if (scaleIsOne) {
        smooth = false;
    }

    if (p->megaSurface)
    {
        const IntRect normalDest = normalizedRect(destRect);
        if (!normalDest.w || !normalDest.h) return;
        if (!srcSurf)
        {
            source.createSurface();
            srcSurf = source.p->surface;
        }
        
        if (destRect.w < 0 || destRect.h < 0)
        {
            // SDL can't handle negative dimensions when blitting, so we have to do it manually
            blitTemp = SDL_CreateRGBSurface(0, sourceRect.w, sourceRect.h, p->format->BitsPerPixel,
                                                        p->format->Rmask, p->format->Gmask,
                                                        p->format->Bmask, p->format->Amask);
            
            bool flipW = destRect.w < 0;
            bool flipH = destRect.y < 0;
            
            for(int dx = 0, sx = (flipW ? sourceRect.x + sourceRect.w - 1 : sourceRect.x);
                dx < sourceRect.w; dx++, (flipW ? sx-- : sx++))
            {
                for(int dy = 0, sy = (flipH ? sourceRect.y + sourceRect.h - 1 : sourceRect.y);
                    dy < sourceRect.h; dy++, (flipH ? sy-- : sy++))
                {
                    uint32_t &srcPixel = getPixelAt(srcSurf, p->format, sx, sy);
                    uint32_t &destPixel = getPixelAt(blitTemp, p->format, dx, dy);
                    destPixel = srcPixel;
                }
            }
            srcSurf = blitTemp;
            sourceRect.x = sourceRect.y = 0;
            destRect = normalDest;
        }
        
        if (touchesTaintedArea)
            SDL_SetSurfaceBlendMode(srcSurf, SDL_BLENDMODE_BLEND);
        else
            SDL_SetSurfaceBlendMode(srcSurf, SDL_BLENDMODE_NONE);
        
        Uint8 tempAlpha;
        SDL_GetSurfaceAlphaMod(srcSurf, &tempAlpha);
        SDL_SetSurfaceAlphaMod(srcSurf, opacity);
        
        if(scaleIsOne)
            SDL_BlitSurface(srcSurf, &sourceRect, p->megaSurface, &destRect);
        else
            SDL_BlitScaled(srcSurf, &sourceRect, p->megaSurface, &destRect);
        
        SDL_SetSurfaceBlendMode(srcSurf, SDL_BLENDMODE_NONE);
        SDL_SetSurfaceAlphaMod(srcSurf, tempAlpha);
        
        // Delete the source surface if the source is an animation
        if (source.p->animation.enabled && source.p->surface)
        {
            SDL_FreeSurface(source.p->surface);
            source.p->surface = 0;
        }
    }
    else if (!srcSurf && !touchesTaintedArea)
    {
        /* Fast blit */
        // TODO: Use bitmapSmoothScaling/bitmapSmoothScalingDown configs for this.
        GLMeta::blitBegin(getGLTypes());
        GLMeta::blitSource(source.getGLTypes());
        GLMeta::blitRectangle(sourceRect, destRect, smooth);
        GLMeta::blitEnd();
    }
    else
    {
        if (srcSurf)
        {
            SDL_Rect srcRect = sourceRect;
            bool subImageFix = shState->config().subImageFix;
            bool srcRectTooBig = srcRect.w > glState.caps.maxTexSize ||
                                 srcRect.h > glState.caps.maxTexSize;
            bool srcSurfTooBig = !unpack_subimage && (
                                     srcSurf->w > glState.caps.maxTexSize || 
                                     srcSurf->h > glState.caps.maxTexSize
                                 );
            
            if (srcRectTooBig || srcSurfTooBig)
            {
                int error;
                if (srcRectTooBig)
                {
                    /* We have to resize it here anyway, so use software resizing */
                    blitTemp =
                        SDL_CreateRGBSurface(0, abs(destRect.w), abs(destRect.h), p->format->BitsPerPixel,
                                             p->format->Rmask, p->format->Gmask,
                                             p->format->Bmask, p->format->Amask);
                    if (!blitTemp)
                        throw Exception(Exception::SDLError, "Error creating temporary surface for blitting: %s",
                                        SDL_GetError());
                    
                    if (smooth)
                    {
                        if (mode == NORMAL)
                            error = SDL_SoftStretchLinear(srcSurf, &srcRect, blitTemp, 0);
                        else
                        {

                            double w_ratio = (double)srcRect.w / (double)destRect.w;
                            double h_ratio = (double)srcRect.h / (double)destRect.h;
                            for (size_t r = 0; r < (size_t)blitTemp->h; ++r)
                                for (size_t c = 0; c < (size_t)blitTemp->w; ++c)
                                {
                                    size_t src_c0 = (size_t)std::floor(w_ratio * c);
                                    size_t src_r0 = (size_t)std::floor(h_ratio * r);
                                    double src_w0 = w_ratio * c - src_c0;
                                    double src_h0 = h_ratio * r - src_r0;
                                    double src_00 = ((uint32_t *)srcSurf->pixels)[(size_t)srcSurf->w * ((size_t)srcRect.y + src_r0) + ((size_t)srcRect.x + src_c0)];
                                    double src_01 = src_c0 + 1 >= (size_t)srcRect.w
                                        ? src_00
                                        : ((uint32_t *)srcSurf->pixels)[(size_t)srcSurf->w * ((size_t)srcRect.y + src_r0) + ((size_t)srcRect.x + src_c0 + 1)];
                                    double src_10 = src_r0 + 1 >= (size_t)srcRect.h
                                        ? src_00
                                        : ((uint32_t *)srcSurf->pixels)[(size_t)srcSurf->w * ((size_t)srcRect.y + src_r0 + 1) + ((size_t)srcRect.x + src_c0)];
                                    double src_11 = src_c0 + 1 >= (size_t)srcRect.w
                                        ? src_10
                                        : (size_t)src_r0 + 1 >= (size_t)srcRect.h
                                        ? src_01
                                        : ((uint32_t *)srcSurf->pixels)[(size_t)srcSurf->w * ((size_t)srcRect.y + src_r0 + 1) + ((size_t)srcRect.x + src_c0 + 1)];
                                    uint32_t &dst_pixel = ((uint32_t *)blitTemp->pixels)[(size_t)blitTemp->w * r + c];
                                    uint32_t src_pixel = std::round(
                                        src_00 * (1. - src_w0) * (1. - src_h0)
                                            + src_01 * (1. - src_w0) * src_h0
                                            + src_10 * src_w0 * (1. - src_h0)
                                            + src_11 * src_w0 * src_h0
                                    );
                                    bltFilter(mode, dst_pixel, src_pixel, normOpacity);
                                }
                        }
                        smooth = false;
                    }
                    else
                    {
                        if (mode == NORMAL)
                        {
                            SDL_Rect tmpRect = {0, 0, blitTemp->w, blitTemp->h};
                            error = SDL_LowerBlitScaled(srcSurf, &srcRect, blitTemp, &tmpRect);
                        }
                        else
                        {
                            double w_ratio = (double)srcRect.w / (double)destRect.w;
                            double h_ratio = (double)srcRect.h / (double)destRect.h;
                            for (size_t r = 0; r < (size_t)blitTemp->h; ++r)
                                for (size_t c = 0; c < (size_t)blitTemp->w; ++c)
                                {
                                    uint32_t &dst_pixel = ((uint32_t *)blitTemp->pixels)[(size_t)blitTemp->w * r + c];
                                    uint32_t src_pixel = ((uint32_t *)srcSurf->pixels)[(size_t)srcSurf->w * ((size_t)srcRect.y + (size_t)std::round(h_ratio * r)) + ((size_t)srcRect.x + (size_t)std::round(w_ratio * c))];
                                    bltFilter(mode, dst_pixel, src_pixel, normOpacity);
                                }
                        }
                    }
                    unpack_subimage = false;
                }
                else
                {
                    /* Just crop it, let the shader resize it later */
                    blitTemp =
                        SDL_CreateRGBSurface(0, sourceRect.w, sourceRect.h, p->format->BitsPerPixel,
                                             p->format->Rmask, p->format->Gmask,
                                             p->format->Bmask, p->format->Amask);
                    if (!blitTemp)
                        throw Exception(Exception::SDLError, "Error creating temporary surface for blitting: %s",
                                        SDL_GetError());
                    
                    SDL_Rect tmpRect = {0, 0, blitTemp->w, blitTemp->h};
                    error = SDL_LowerBlit(srcSurf, &srcRect, blitTemp, &tmpRect);
                }
                
                if (error)
                {
                    SDL_FreeSurface(blitTemp);
                    throw Exception(Exception::SDLError, "Failed to blit surface: %s", SDL_GetError());
                }
                
                srcSurf = blitTemp;
                
                sourceRect.w = srcSurf->w;
                sourceRect.h = srcSurf->h;
                sourceRect.x = 0;
                sourceRect.y = 0;
            }
            
            if (!touchesTaintedArea)
            {
                if (!subImageFix &&
                    scaleIsOne &&
                    (unpack_subimage || (srcSurf->w == sourceRect.w && srcSurf->h == sourceRect.h))
                   )
                {
                    /* No scaling needed */
                    TEX::bind(getGLTypes().tex);
                    if (unpack_subimage)
                    {
                        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, srcSurf->w);
                        gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, sourceRect.x);
                        gl.PixelStorei(GL_UNPACK_SKIP_ROWS, sourceRect.y);
                    }
                    TEX::uploadSubImage(destRect.x, destRect.y,
                                        destRect.w, destRect.h,
                                        srcSurf->pixels, GL_RGBA);
                    
                    if (unpack_subimage)
                        GLMeta::subRectImageEnd();
                }
                else
                {
                    /* Resizing or subImageFix involved: need to use intermediary TexFBO */
                    TEXFBO *gpTF;
                    if (unpack_subimage)
                        gpTF = &shState->gpTexFBO(sourceRect.w, sourceRect.h);
                    else
                        gpTF = &shState->gpTexFBO(srcSurf->w, srcSurf->h);
                    TEX::bind(gpTF->tex);
                    
                    if (unpack_subimage)
                    {
                        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, srcSurf->w);
                        gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, sourceRect.x);
                        gl.PixelStorei(GL_UNPACK_SKIP_ROWS, sourceRect.y);
                        sourceRect.x = 0;
                        sourceRect.y = 0;
                        TEX::uploadSubImage(0, 0, sourceRect.w, sourceRect.h, srcSurf->pixels, GL_RGBA);
                        GLMeta::subRectImageEnd();
                    }
                    else
                    {
                        TEX::uploadSubImage(0, 0, srcSurf->w, srcSurf->h, srcSurf->pixels, GL_RGBA);
                    }
                    
                    GLMeta::blitBegin(getGLTypes());
                    GLMeta::blitSource(*gpTF);
                    GLMeta::blitRectangle(sourceRect, destRect, smooth);
                    GLMeta::blitEnd();
                }
            }
        }
        if (touchesTaintedArea)
        {
            /* We're touching a tainted area or still need to reduce opacity */
             
            /* Fragment pipeline */
            
            TEXFBO &gpTex = shState->gpTexFBO(abs(destRect.w), abs(destRect.h));
            Vec2i gpTexSize;
            
            GLMeta::blitBegin(gpTex, false, SameScale);
            GLMeta::blitSource(getGLTypes(), SameScale);
            GLMeta::blitRectangle(destRect, IntRect(0, 0, abs(destRect.w), abs(destRect.h)));
            GLMeta::blitEnd();
            
            int sourceWidth, sourceHeight;
            FloatRect bltSubRect;
            if (srcSurf)
            {
                if (unpack_subimage)
                {
                    shState->ensureTexSize(sourceRect.w, sourceRect.h, gpTexSize);
                }
                else
                {
                    shState->ensureTexSize(srcSurf->w, srcSurf->h, gpTexSize);
                }
                sourceWidth = gpTexSize.x;
                sourceHeight = gpTexSize.y;
                
                shState->bindTex();
                
                if (unpack_subimage)
                {
                    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, srcSurf->w);
                    gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, sourceRect.x);
                    gl.PixelStorei(GL_UNPACK_SKIP_ROWS, sourceRect.y);
                    sourceRect.x = 0;
                    sourceRect.y = 0;
                    
                    TEX::uploadSubImage(0, 0, sourceRect.w, sourceRect.h, srcSurf->pixels, GL_RGBA);
                    GLMeta::subRectImageEnd();
                }
                else
                {
                    TEX::uploadSubImage(0, 0, srcSurf->w, srcSurf->h, srcSurf->pixels, GL_RGBA);
                }
            }
            else
            {
                sourceWidth = source.width();
                sourceHeight = source.height();
            }
            bltSubRect = FloatRect((float) sourceRect.x / sourceWidth,
                                   (float) sourceRect.y / sourceHeight,
                                   ((float) sourceWidth / sourceRect.w) * ((float) abs(destRect.w) / gpTex.width),
                                   ((float) sourceHeight / sourceRect.h) * ((float) abs(destRect.h) / gpTex.height));
            
            BltShader &shader = mode == KGL_SUBTRACT ? shState->shaders().kglSubtract : shState->shaders().blt;
            shader.bind();
            if (srcSurf)
            {
                shader.setTexSize(gpTexSize);
            }
            else
            {
                source.p->bindTexture(shader, false);
            }
            shader.setSource();
            shader.setDestination(gpTex.tex);
            shader.setSubRect(bltSubRect);
            shader.setOpacity(normOpacity);
            
            Quad &quad = shState->gpQuad();
            quad.setTexPosRect(sourceRect, destRect);
            quad.setColor(Vec4(1, 1, 1, normOpacity));
            
            p->bindFBO();
            p->pushSetViewport(shader);
            
            if (smooth)
                TEX::setSmooth(true);
            
            p->blitQuad(quad);
            
            p->popViewport();
            
            if (smooth)
                TEX::setSmooth(false);
        }
    }
    
    if (blitTemp)
        SDL_FreeSurface(blitTemp);
    
    p->addTaintedArea(destRect);
    p->onModified();
#endif
}

void Bitmap::fillRect(int x, int y,
                      int width, int height,
                      const Vec4 &color)
{
    fillRect(IntRect(x, y, width, height), color);
}

void Bitmap::fillRect(const IntRect &rect, const Vec4 &color)
{
    guardDisposed();

    GUARD_ANIMATED;

#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
    {
        char tb[160];
        snprintf(tb, sizeof(tb),
                 "trace: Bitmap::fillRect %d,%d %dx%d",
                 rect.x, rect.y, rect.w, rect.h);
        vita_glue_trace(tb);
    }
#endif
    if (hasHires()) {
        int destX, destY, destWidth, destHeight;
        destX = rect.x * p->selfHires->width() / width();
        destY = rect.y * p->selfHires->height() / height();
        destWidth = rect.w * p->selfHires->width() / width();
        destHeight = rect.h * p->selfHires->height() / height();

        p->selfHires->fillRect(IntRect(destX, destY, destWidth, destHeight), color);
    }

    p->fillRect(rect, color);
    
    if (color.w == 0)
    /* Clear op */
        p->substractTaintedArea(rect);
    else
    /* Fill op */
        p->addTaintedArea(rect);
    
    p->onModified();
}

void Bitmap::gradientFillRect(int x, int y,
                              int width, int height,
                              const Vec4 &color1, const Vec4 &color2,
                              bool vertical)
{
    gradientFillRect(IntRect(x, y, width, height), color1, color2, vertical);
}

void Bitmap::gradientFillRect(const IntRect &rect,
                              const Vec4 &color1, const Vec4 &color2,
                              bool vertical)
{
    guardDisposed();
    
    GUARD_ANIMATED;
    
    if (rect.w <= 0 || rect.h <= 0 || rect.x >= width() || rect.y >= height() ||
        rect.w < -rect.x || rect.h < -rect.y)
        return;
    
    if (hasHires()) {
        int destX, destY, destWidth, destHeight;
        destX = rect.x * p->selfHires->width() / width();
        destY = rect.y * p->selfHires->height() / height();
        destWidth = rect.w * p->selfHires->width() / width();
        destHeight = rect.h * p->selfHires->height() / height();

        p->selfHires->gradientFillRect(IntRect(destX, destY, destWidth, destHeight), color1, color2, vertical);
    }


#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* swraster follows the GL path (a SimpleColorShader quad with per-vertex
     * colours): interpolation across pixel centres over the FULL rect, so
     * clipping never shifts the gradient phase. mkxp's mega fallback used
     * t = i/(n-1) per scanline and produced different colours. */
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    FrameProfile::Scope profileRaster(FrameProfile::Raster);
#endif
    swraster::gradient_fill(p->swSurface(), swRect(rect),
                            swColor(color1), swColor(color2), vertical);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    profileRaster.stop();
#endif
    p->markDirty(rect);
#else
    if (p->megaSurface)
    {
        float progress = 0.0f;
        float invProgress = 1.0f;
        Color c1 = color1;
        Color c2 = color2;
        int orig, end;
        uint8_t r, g, b, a;
        float max;
        SDL_Rect destRect = rect;
        int *current;
        if (vertical)
        {
            destRect.w = std::min(rect.w, width() - rect.x);
            destRect.h = 1;
            
            current = &destRect.y;
            orig = rect.y;
            max = rect.h - 1;
            end = std::min(rect.y + rect.h, height());
        }
        else
        {
            destRect.w = 1;
            destRect.h = std::min(rect.h, height() - rect.y);
            
            current = &destRect.x;
            orig = rect.x;
            max = rect.w - 1;
            end = std::min(rect.x + rect.w, width());
        }
        while (*current < end)
        {
            progress = (*current - orig) / max;
            invProgress = 1.0f - progress;
            r = round((c1.red * invProgress) + (c2.red * progress));
            g = round((c1.green * invProgress) + (c2.green * progress));
            b = round((c1.blue * invProgress) + (c2.blue * progress));
            a = round((c1.alpha * invProgress) + (c2.alpha * progress));
            Uint32 color = SDL_MapRGBA(p->format, r, g, b, a);
            
            SDL_FillRect(p->megaSurface, &destRect, color);
            
            (*current)++;
        }
    }
    else
    {
        SimpleColorShader &shader = shState->shaders().simpleColor;
        shader.bind();
        shader.setTranslation(Vec2i());
        
        Quad &quad = shState->gpQuad();
        
        if (vertical)
        {
            quad.vert[0].color = color1;
            quad.vert[1].color = color1;
            quad.vert[2].color = color2;
            quad.vert[3].color = color2;
        }
        else
        {
            quad.vert[0].color = color1;
            quad.vert[3].color = color1;
            quad.vert[1].color = color2;
            quad.vert[2].color = color2;
        }
        
        quad.setPosRect(rect);
        
        p->bindFBO();
        p->pushSetViewport(shader);
        
        p->blitQuad(quad);
        
        p->popViewport();
    }
#endif

    p->addTaintedArea(rect);
    
    p->onModified();
}

void Bitmap::clearRect(int x, int y, int width, int height)
{
    clearRect(IntRect(x, y, width, height));
}

void Bitmap::clearRect(const IntRect &rect)
{
    guardDisposed();
    
    GUARD_ANIMATED;
    
    if (hasHires()) {
        int destX, destY, destWidth, destHeight;
        destX = rect.x * p->selfHires->width() / width();
        destY = rect.y * p->selfHires->height() / height();
        destWidth = rect.w * p->selfHires->width() / width();
        destHeight = rect.h * p->selfHires->height() / height();

        p->selfHires->clearRect(IntRect(destX, destY, destWidth, destHeight));
    }

    p->fillRect(rect, Vec4());
    
    p->substractTaintedArea(rect);
    
    p->onModified();
}

void Bitmap::blur()
{
    guardDisposed();
    
    GUARD_ANIMATED;
    
    if (hasHires()) {
        p->selfHires->blur();
    }

    // TODO: Is there some kind of blur radius that we need to handle for high-res mode?

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* One horizontal then one vertical 3-tap box pass over the whole surface,
     * quantising between passes exactly like the GL aux texture did. The stock
     * mega path tiled through a temporary GPU Bitmap and recursed into the GPU
     * path; that recursion (and its aux TEXFBO) is gone. */
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    FrameProfile::Scope profileRaster(FrameProfile::Raster);
#endif
    swraster::blur(p->swSurface());
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    profileRaster.stop();
#endif
    p->markDirty(rect());
    p->onModified();
    return;
#else
    if(p->megaSurface)
    {
        int buffer = 5;
        
        int widthMult = 1;
        int tmpWidth = width();
        int bufferX = 0;
        
        int heightMult = 1;
        int tmpHeight = height();
        int bufferY = 0;
        
        if(width() > glState.caps.maxTexSize)
        {
            widthMult = ceil((float) width() / (glState.caps.maxTexSize - (buffer * 2)));
            tmpWidth = ceil((float) width() / widthMult) + (buffer * 2);
            bufferX = buffer;
        }
        if(height() > glState.caps.maxTexSize)
        {
            heightMult = ceil((float) height() / (glState.caps.maxTexSize - (buffer * 2)));
            tmpHeight = ceil((float) height() / heightMult) + (buffer * 2);
            bufferY = buffer;
        }
        
        Bitmap *tmp = new Bitmap(tmpWidth + (bufferX * 2), tmpHeight + (bufferY * 2), true);
        IntRect sourceRect = tmp->rect();
        IntRect destRect = {};
        
        pixman_region16_t originalTainted;
        pixman_region32_t originalTainted32;
        if (p->pixmanUseRegion32)
        {
            pixman_region32_init(&originalTainted32);
            pixman_region32_copy(&originalTainted32, &p->tainted32);
        }
        else
        {
            pixman_region_init(&originalTainted);
            pixman_region_copy(&originalTainted, &p->tainted);
        }
        for (int i = 0; i < widthMult; i++)
        {
            int tmpX = i ? bufferX : 0;
            sourceRect.x = (tmpWidth - tmpX) * i;
            destRect.x = sourceRect.x + tmpX;
            destRect.w = sourceRect.w - (bufferX * (i ? 2 : 1));
            
            for (int j = 0; j < heightMult; j++)
            {
                int tmpY = j ? bufferY : 0;
                sourceRect.y = (tmpHeight - tmpY) * j;
                destRect.y = sourceRect.y + tmpY;
                destRect.h = sourceRect.h - (bufferY * (j ? 2 : 1));
                
                tmp->clear();
                p->clearTaintedArea();
                
                IntRect tmpRect = tmp->rect();
                tmpRect.x = tmpRect.w - std::min(sourceRect.w, width() - sourceRect.x);
                tmpRect.y = tmpRect.h - std::min(sourceRect.h, height() - sourceRect.y);
                tmpRect.w = sourceRect.w;
                tmpRect.h = sourceRect.h;
                
                
                tmp->stretchBlt(tmpRect, *this, sourceRect, 255);
                tmp->blur();
                
                stretchBlt(destRect, *tmp, IntRect(tmpRect.x + tmpX, tmpRect.y + tmpY, destRect.w, destRect.h), 255);
            }
        }
        delete tmp;
        p->clearTaintedArea();
        if (p->pixmanUseRegion32)
        {
            pixman_region32_copy(&p->tainted32, &originalTainted32);
            pixman_region32_fini(&originalTainted32);
        }
        else
        {
            pixman_region_copy(&p->tainted, &originalTainted);
            pixman_region_fini(&originalTainted);
        }
    }
    else
    {
        Quad &quad = shState->gpQuad();
        FloatRect rect(0, 0, width(), height());
        quad.setTexPosRect(rect, rect);
        
        TEXFBO auxTex = shState->texPool().request(width(), height());
        
        BlurShader &shader = shState->shaders().blur;
        BlurShader::HPass &pass1 = shader.pass1;
        BlurShader::VPass &pass2 = shader.pass2;
        
        glState.blend.pushSet(false);
        glState.viewport.pushSet(IntRect(0, 0, width(), height()));
        
        TEX::bind(p->gl.tex);
        FBO::bind(auxTex.fbo);
        
        pass1.bind();
        pass1.setTexSize(Vec2i(width(), height()));
        pass1.applyViewportProj();
        
        quad.draw();
        
        TEX::bind(auxTex.tex);
        p->bindFBO();
        
        pass2.bind();
        pass2.setTexSize(Vec2i(width(), height()));
        pass2.applyViewportProj();
        
        quad.draw();
        
        glState.viewport.pop();
        glState.blend.pop();
        
        shState->texPool().release(auxTex);
        
        p->onModified();
    }
#endif
}

void Bitmap::radialBlur(int angle, int divisions)
{
    guardDisposed();
    
    GUARD_MEGA;
    GUARD_ANIMATED;

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* CPU transcription of the GL path below: `divisions`
     * rotations of the whole bitmap about its centre, evenly spaced over
     * [-angle/2, +angle/2], each sampled bilinearly and accumulated
     * additively at 1/divisions opacity, over stock's 5-quad cross (the
     * bitmap plus one mirrored copy across each edge). swraster::radial_blur
     * applies the same clamps the GL path does, so they stay in one place.
     *
     * Stock RGSS2 Spriteset_Battle#create_battleback calls this with
     * (90, 12) and RGSS3 create_blurry_background_bitmap with (120, 16)
     * whenever a map has no battleback, so it must work, not raise. It is
     * hundreds of milliseconds on the device at those sizes, but it runs
     * once per battle start, not per frame. */
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    FrameProfile::Scope profileRaster(FrameProfile::Raster);
#endif
    swraster::radial_blur(p->swSurface(), angle, divisions);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    profileRaster.stop();
#endif
    p->markDirty(rect());
    p->onModified();
    return;
#else
    if (hasHires()) {
        p->selfHires->radialBlur(angle, divisions);
        return;
    }

    angle     = clamp<int>(angle, 0, 359);
    divisions = clamp<int>(divisions, 2, 100);
    
    const int _width = width();
    const int _height = height();
    
    float angleStep = (float) angle / (divisions-1);
    float opacity   = 1.0f / divisions;
    float baseAngle = -((float) angle / 2);
    
    ColorQuadArray qArray;
    qArray.resize(5);
    
    std::vector<Vertex> &vert = qArray.vertices;
    
    int i = 0;
    
    /* Center */
    FloatRect texRect(0, 0, _width, _height);
    FloatRect posRect(0, 0, _width, _height);
    
    i += Quad::setTexPosRect(&vert[i*4], texRect, posRect);
    
    /* Upper */
    posRect = FloatRect(0, 0, _width, -_height);
    
    i += Quad::setTexPosRect(&vert[i*4], texRect, posRect);
    
    /* Lower */
    posRect = FloatRect(0, _height*2, _width, -_height);
    
    i += Quad::setTexPosRect(&vert[i*4], texRect, posRect);
    
    /* Left */
    posRect = FloatRect(0, 0, -_width, _height);
    
    i += Quad::setTexPosRect(&vert[i*4], texRect, posRect);
    
    /* Right */
    posRect = FloatRect(_width*2, 0, -_width, _height);
    
    i += Quad::setTexPosRect(&vert[i*4], texRect, posRect);
    
    for (int i = 0; i < 4*5; ++i)
        vert[i].color = Vec4(1, 1, 1, opacity);
    
    qArray.commit();
    
    TEXFBO newTex = shState->texPool().request(_width, _height);
    
    FBO::bind(newTex.fbo);
    
    glState.clearColor.pushSet(Vec4());
    FBO::clear();
    
    Transform trans;
    trans.setOrigin(Vec2(_width / 2.0f, _height / 2.0f));
    trans.setPosition(Vec2(_width / 2.0f, _height / 2.0f));
    
    glState.blendMode.pushSet(BlendAddition);
    
    SimpleMatrixShader &shader = shState->shaders().simpleMatrix;
    shader.bind();
    
    p->bindTexture(shader, false);
    TEX::setSmooth(true);
    
    p->pushSetViewport(shader);
    
    for (int i = 0; i < divisions; ++i)
    {
        trans.setRotation(baseAngle + i*angleStep);
        shader.setMatrix(trans.getMatrix());
        qArray.draw();
    }
    
    p->popViewport();
    
    TEX::setSmooth(false);
    
    glState.blendMode.pop();
    glState.clearColor.pop();
    
    shState->texPool().release(p->gl);
    p->gl = newTex;
    
    p->onModified();
#endif
}

void Bitmap::clear()
{
    guardDisposed();

    GUARD_ANIMATED;

#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
    vita_glue_trace("trace: Bitmap::clear");
#endif
    if (hasHires()) {
        p->selfHires->clear();
    }

#ifdef MKXPZ_SOFTWARE_BITMAPS
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    FrameProfile::Scope profileRaster(FrameProfile::Raster);
#endif
    swraster::clear(p->swSurface(), swRect(rect()));
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    profileRaster.stop();
#endif
    p->markDirty(rect());
#else
    if (p->megaSurface)
    {
        SDL_Rect fRect = rect();
        SDL_FillRect(p->megaSurface, &fRect, 0);
    }
    else
    {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        if (vita_glue_texture_scope_active())
            TEXFBO::trace("clear BindFramebuffer BEGIN", p->gl);
#endif
        p->bindFBO();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        if (vita_glue_texture_scope_active())
            TEXFBO::trace("clear BindFramebuffer END / ClearColor BEGIN", p->gl);
#endif

        
        glState.clearColor.pushSet(Vec4());
        
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        if (vita_glue_texture_scope_active())
            TEXFBO::trace("clear ClearColor END / Clear BEGIN", p->gl);
#endif
        FBO::clear();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        if (vita_glue_texture_scope_active())
            TEXFBO::trace("clear Clear END / restore ClearColor BEGIN", p->gl);
#endif

        
        glState.clearColor.pop();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        if (vita_glue_texture_scope_active())
            TEXFBO::trace("clear restore ClearColor END", p->gl);
#endif

    }
#endif

    p->clearTaintedArea();
    
    p->onModified();
}

void Bitmap::createSurface() const
{
#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* CPU pixels are always present; there is nothing to read back and no
     * render target to read it from. */
    return;
#else
    if (p->surface)
        return;
    p->allocSurface();
    
    p->bindFBO();
    
    glState.viewport.pushSet(IntRect(0, 0, width(), height()));
    
    gl.ReadPixels(0, 0, width(), height(), GL_RGBA, GL_UNSIGNED_BYTE, p->surface->pixels);
    
    glState.viewport.pop();
#endif
}

Color Bitmap::getPixel(int x, int y) const
{
    guardDisposed();
    
    GUARD_ANIMATED;
    
    if (hasHires()) {
        Debug() << "GAME BUG: Game is calling getPixel on low-res Bitmap; you may want to patch the game to improve graphics quality.";

        int xHires = x * p->selfHires->width() / width();
        int yHires = y * p->selfHires->height() / height();

        // We take the average color from the high-res Bitmap.
        // RGB channels skip fully transparent pixels when averaging.
        int w = p->selfHires->width() / width();
        int h = p->selfHires->height() / height();

        if (w >= 1 && h >= 1) {
            double rSum = 0.;
            double gSum = 0.;
            double bSum = 0.;
            double aSum = 0.;

            long long rgbCount = 0;
            long long aCount = 0;

            for (int thisX = xHires; thisX < xHires+w && thisX < p->selfHires->width(); thisX++) {
                for (int thisY = yHires; thisY < yHires+h && thisY < p->selfHires->height(); thisY++) {
                    Color thisColor = p->selfHires->getPixel(thisX, thisY);
                    if (thisColor.getAlpha() >= 1.0) {
                        rSum += thisColor.getRed();
                        gSum += thisColor.getGreen();
                        bSum += thisColor.getBlue();
                        rgbCount++;
                    }
                    aSum += thisColor.getAlpha();
                    aCount++;
                }
            }

            double rAvg = rSum / (double)rgbCount;
            double gAvg = gSum / (double)rgbCount;
            double bAvg = bSum / (double)rgbCount;
            double aAvg = aSum / (double)aCount;

            return Color(rAvg, gAvg, bAvg, aAvg);
        }
    }

    if (x < 0 || y < 0 || x >= width() || y >= height())
        return Vec4();

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* Never touches GL: no readback, no render target, no GFX lock needed. */
    {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileRaster(FrameProfile::Raster);
#endif
        swraster::Color c = swraster::get_pixel(p->swSurface(), x, y);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        profileRaster.stop();
#endif
        return Color(c.r, c.g, c.b, c.a);
    }
#else
    SDL_Surface *surf = nullptr;
    if (p->megaSurface)
        surf = p->megaSurface;
    else if (p->surface)
        surf = p->surface;
    else
    {
        createSurface();
        surf = p->surface;
    }
    
    uint32_t pixel = getPixelAt(surf, p->format, x, y);
    
    return Color((pixel >> p->format->Rshift) & 0xFF,
                 (pixel >> p->format->Gshift) & 0xFF,
                 (pixel >> p->format->Bshift) & 0xFF,
                 (pixel >> p->format->Ashift) & 0xFF);
#endif
}

void Bitmap::setPixel(int x, int y, const Color &color)
{
    guardDisposed();
    
    GUARD_ANIMATED;
    
    if (hasHires()) {
        Debug() << "GAME BUG: Game is calling setPixel on low-res Bitmap; you may want to patch the game to improve graphics quality.";

        int xHires = x * p->selfHires->width() / width();
        int yHires = y * p->selfHires->height() / height();

        int w = p->selfHires->width() / width();
        int h = p->selfHires->height() / height();

        if (w >= 1 && h >= 1) {
            for (int thisX = xHires; thisX < xHires+w && thisX < p->selfHires->width(); thisX++) {
                for (int thisY = yHires; thisY < yHires+h && thisY < p->selfHires->height(); thisY++) {
                    p->selfHires->setPixel(thisX, thisY, color);
                }
            }
        }
    }

    uint8_t pixel[] =
    {
        (uint8_t) clamp<double>(color.red,   0, 255),
        (uint8_t) clamp<double>(color.green, 0, 255),
        (uint8_t) clamp<double>(color.blue,  0, 255),
        (uint8_t) clamp<double>(color.alpha, 0, 255)
    };
    
#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* Bounds-checked and GL-free; the 1x1 texture sub-upload is gone. */
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    FrameProfile::Scope profileRaster(FrameProfile::Raster);
#endif
    swraster::set_pixel(p->swSurface(), x, y,
                        swraster::Color{ pixel[0], pixel[1], pixel[2], pixel[3] });
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    profileRaster.stop();
#endif
    p->addTaintedArea(IntRect(x, y, 1, 1));
    p->markDirty(IntRect(x, y, 1, 1));
    p->onModified(false);
    return;
#else
    if (!p->megaSurface)
    {
        TEX::bind(p->gl.tex);
        TEX::uploadSubImage(x, y, 1, 1, &pixel, GL_RGBA);
    }
    
    p->addTaintedArea(IntRect(x, y, 1, 1));
    
    SDL_Surface *surf = nullptr;
    if (p->megaSurface)
        surf = p->megaSurface;
    else
    {
        /* Setting just a single pixel is no reason to throw away the
         * whole cached surface; we can just apply the same change */
        
        if (p->surface)
            surf = p->surface;
    }
    
    if (surf)
    {
        uint32_t &surfPixel = getPixelAt(surf, p->format, x, y);
        surfPixel = SDL_MapRGBA(p->format, pixel[0], pixel[1], pixel[2], pixel[3]);
    }
    
    p->onModified(false);
#endif
}

bool Bitmap::getRaw(void *output, int output_size)
{
    if (output_size != width()*height()*4) return false;
    
    guardDisposed();
    
    if (hasHires()) {
        Debug() << "GAME BUG: Game is calling getRaw on low-res Bitmap; you may want to patch the game to improve graphics quality.";
    }

#ifdef MKXPZ_SOFTWARE_BITMAPS
    {
        SDL_Surface *src = p->pixels();
        if (!src)
            return false;

        for (int y = 0; y < src->h; ++y)
            memcpy((uint8_t *)output + (size_t)y * src->w * 4,
                   (const uint8_t *)src->pixels + (size_t)y * src->pitch,
                   (size_t)src->w * 4);
        return true;
    }
#else
    if (!p->animation.enabled && (p->surface || p->megaSurface)) {
        void *src = (p->megaSurface) ? p->megaSurface->pixels : p->surface->pixels;
        memcpy(output, src, output_size);
    }
    else {
        FBO::bind(getGLTypes().fbo);
        gl.ReadPixels(0,0,width(),height(),GL_RGBA,GL_UNSIGNED_BYTE,output);
    }
    return true;
#endif
}

void Bitmap::replaceRaw(void *pixel_data, int size)
{
    guardDisposed();
    
    if (hasHires()) {
        Debug() << "GAME BUG: Game is calling replaceRaw on low-res Bitmap; you may want to patch the game to improve graphics quality.";
    }

    int w = width();
    int h = height();
    int requiredsize = w*h*4;
    
    if (size != w*h*4)
        throw Exception(Exception::MKXPError, "Replacement bitmap data is not large enough (given %i bytes, need %i)", size, requiredsize);
    
#ifdef MKXPZ_SOFTWARE_BITMAPS
    {
        SDL_Surface *dst = p->pixels();
        if (!dst)
            throw Exception(Exception::MKXPError, "Bitmap has no CPU pixels");

        for (int y = 0; y < dst->h; ++y)
            memcpy((uint8_t *)dst->pixels + (size_t)y * dst->pitch,
                   (const uint8_t *)pixel_data + (size_t)y * dst->w * 4,
                   (size_t)dst->w * 4);
        p->markDirty(IntRect(0, 0, w, h));
    }
#else
    if (p->megaSurface)
    {
        // This should always be true
        if (p->megaSurface->format->BitsPerPixel == 32)
            memcpy(p->megaSurface->pixels, pixel_data, w*h*4);
    }
    else
    {
        TEX::bind(getGLTypes().tex);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileDirectUpload(FrameProfile::Upload, 0, false, false);
#endif
        TEX::uploadImage(w, h, pixel_data, GL_RGBA);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        profileDirectUpload.stop();
#endif
    }
#endif

    taintArea(IntRect(0,0,w,h));
    p->onModified();
}

#ifdef MKXPZ_SOFTWARE_BITMAPS
void Bitmap::refreshFrame()
{
    guardDisposed();

    p->refreshFrameNow();
}
#endif

void Bitmap::saveToFile(const char *filename)
{
    guardDisposed();
    
    if (hasHires()) {
        Debug() << "GAME BUG: Game is calling saveToFile on low-res Bitmap; you may want to patch the game to improve graphics quality.";
    }

    SDL_Surface *surf;

#ifdef MKXPZ_SOFTWARE_BITMAPS
    surf = p->pixels();
    if (!surf)
        throw Exception(Exception::MKXPError, "Bitmap has no CPU pixels");
#else
    if (p->surface || p->megaSurface) {
        surf = (p->surface) ? p->surface : p->megaSurface;
    }
    else {
        surf = SDL_CreateRGBSurface(0, width(), height(),p->format->BitsPerPixel, p->format->Rmask,p->format->Gmask,p->format->Bmask,p->format->Amask);
        
        if (!surf)
            throw Exception(Exception::SDLError, "Failed to prepare bitmap for saving: %s", SDL_GetError());
        
        getRaw(surf->pixels, surf->w * surf->h * 4);
    }
#endif

    // Try and determine the intended image format from the filename extension
    const char *period = strrchr(filename, '.');
    int filetype = 0;
    if (period) {
        period++;
        std::string ext;
        for (int i = 0; i < (int)strlen(period); i++) {
            ext += tolower(period[i]);
        }
        
        if (!ext.compare("png")) {
            filetype = 1;
        }
        else if (!ext.compare("jpg") || !ext.compare("jpeg")) {
            filetype = 2;
        }
    }
    
    std::string fn_normalized = shState->fileSystem().normalize(filename, 1, 1);
    int rc;
    switch (filetype) {
        case 2:
            rc = IMG_SaveJPG(surf, fn_normalized.c_str(), 90);
            break;
        case 1:
            rc = IMG_SavePNG(surf, fn_normalized.c_str());
            break;
        case 0: default:
            rc = SDL_SaveBMP(surf, fn_normalized.c_str());
            break;
    }
    
#ifndef MKXPZ_SOFTWARE_BITMAPS
    if (!p->surface && !p->megaSurface)
        SDL_FreeSurface(surf);
#endif

    if (rc) throw Exception(Exception::SDLError, "%s", SDL_GetError());
}

void Bitmap::hueChange(int hue)
{
    guardDisposed();
    
    GUARD_ANIMATED;
    
    if (hasHires()) {
        p->selfHires->hueChange(hue);
        return;
    }

    if ((hue % 360) == 0)
        return;

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* shader/hue.frag on CPU pixels, whole surface. As with blur, the stock
     * mega path recursed into the GPU path through a temporary Bitmap; both
     * that recursion and the swap-in TexPool texture are gone. */
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    FrameProfile::Scope profileRaster(FrameProfile::Raster);
#endif
    swraster::hue_change(p->swSurface(), swRect(rect()), hue);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    profileRaster.stop();
#endif
    p->markDirty(rect());
    p->onModified();
    return;
#else
    if (p->megaSurface)
    {
        int widthMult = ceil((float) width() / glState.caps.maxTexSize);
        int tmpWidth = ceil((float) width() / widthMult);
        int heightMult = ceil((float) height() / glState.caps.maxTexSize);
        int tmpHeight = ceil((float) height() / heightMult);
        
        Bitmap *tmp = new Bitmap(tmpWidth, tmpHeight, true);
        IntRect sourceRect = {0, 0, tmpWidth, tmpHeight};
        
        pixman_region16_t originalTainted;
        pixman_region32_t originalTainted32;
        if (p->pixmanUseRegion32)
        {
            pixman_region32_init(&originalTainted32);
            pixman_region32_copy(&originalTainted32, &p->tainted32);
        }
        else
        {
            pixman_region_init(&originalTainted);
            pixman_region_copy(&originalTainted, &p->tainted);
        }
        for (int i = 0; i < widthMult; i++)
        {
            for (int j = 0; j < heightMult; j++)
            {
                tmp->clear();
                p->clearTaintedArea();
                sourceRect.x = tmpWidth * i;
                sourceRect.y = tmpHeight * j;
                tmp->stretchBlt(tmp->rect(), *this, sourceRect, 255);
                tmp->hueChange(hue);
                stretchBlt(sourceRect, *tmp, tmp->rect(), 255);
            }
        }
        delete tmp;
        p->clearTaintedArea();
        if (p->pixmanUseRegion32)
        {
            pixman_region32_copy(&p->tainted32, &originalTainted32);
            pixman_region32_fini(&originalTainted32);
        }
        else
        {
            pixman_region_copy(&p->tainted, &originalTainted);
            pixman_region_fini(&originalTainted);
        }
    }
    else
    {
        TEXFBO newTex = shState->texPool().request(width(), height());
        
        FloatRect texRect(rect());
        
        Quad &quad = shState->gpQuad();
        quad.setTexPosRect(texRect, texRect);
        quad.setColor(Vec4(1, 1, 1, 1));
        
        HueShader &shader = shState->shaders().hue;
        shader.bind();
        /* Shader expects normalized value */
        shader.setHueAdjust(wrapRange(hue, 0, 360) / 360.0f);
        
        FBO::bind(newTex.fbo);
        p->pushSetViewport(shader);
        p->bindTexture(shader, false);
        
        p->blitQuad(quad);
        
        p->popViewport();
        
        TEX::unbind();
        
        shState->texPool().release(p->gl);
        p->gl = newTex;
    }
    
    p->onModified();
#endif
}

void Bitmap::drawText(int x, int y,
                      int width, int height,
                      const char *str, int align)
{
    drawText(IntRect(x, y, width, height), str, align);
}

static std::string fixupString(const char *str)
{
    std::string s(str);
    
    /* RMXP actually draws LF as a "missing gylph" box,
     * but since we might have accidentally converted CRs
     * to LFs when editing scripts on a Unix OS, treat them
     * as white space too */
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == '\r' || s[i] == '\n')
            s[i] = ' ';
    
    return s;
}

static void applyShadow(SDL_Surface *&in, const SDL_PixelFormat &fm, const SDL_Color &c, int offset)
{
    SDL_Surface *out = SDL_CreateRGBSurface
    (0, in->w+offset, in->h+offset, fm.BitsPerPixel, fm.Rmask, fm.Gmask, fm.Bmask, fm.Amask);
    
    /* The caller still owns 'in' and frees it when this throws */
    if (!out)
        throw Exception(Exception::SDLError, "Error creating text shadow: %s",
                        SDL_GetError());
    
    float fr = c.r / 255.0f;
    float fg = c.g / 255.0f;
    float fb = c.b / 255.0f;
    
    /* We allocate an output surface one pixel wider and higher than the input,
     * (implicitly) blit a copy of the input with RGB values set to black into
     * it with x/y offset by 1, then blend the input surface over it at origin
     * (0,0) using the bitmap blit equation (see shader/bitmapBlit.frag) */
    
    for (int y = 0; y < in->h+offset; ++y)
        for (int x = 0; x < in->w+offset; ++x)
        {
            /* src: input pixel, shd: shadow pixel */
            uint32_t src = 0, shd = 0;
            
            /* Output pixel location */
            uint32_t *outP = ((uint32_t*) ((uint8_t*) out->pixels + y*out->pitch)) + x;
            
            if (y < in->h && x < in->w)
                src = ((uint32_t*) ((uint8_t*) in->pixels + y*in->pitch))[x];
            
            if (y >= offset && x >= offset)
                shd = ((uint32_t*) ((uint8_t*) in->pixels + (y-offset)*in->pitch))[x-offset];
            
            /* Set shadow pixel RGB values to 0 (black) */
            shd &= fm.Amask;
            
            if (x < offset || y < offset)
            {
                *outP = src;
                continue;
            }
            
            if (x >= in->w || y >= in->h)
            {
                *outP = shd;
                continue;
            }
            
            /* Input and shadow alpha values */
            uint8_t srcA, shdA;
            srcA = (src & fm.Amask) >> fm.Ashift;
            shdA = (shd & fm.Amask) >> fm.Ashift;
            
            if (srcA == 255 || shdA == 0)
            {
                *outP = src;
                continue;
            }
            
            if (srcA == 0 && shdA == 0)
            {
                *outP = 0;
                continue;
            }
            
            float fSrcA = srcA / 255.0f;
            float fShdA = shdA / 255.0f;
            
            /* Because opacity == 1, co1 == fSrcA */
            float co2 = fShdA * (1.0f - fSrcA);
            /* Result alpha */
            float fa = fSrcA + co2;
            /* Temp value to simplify arithmetic below */
            float co3 = fSrcA / fa;
            
            /* Result colors */
            uint8_t r, g, b, a;
            
            r = clamp<float>(fr * co3, 0, 1) * 255.0f;
            g = clamp<float>(fg * co3, 0, 1) * 255.0f;
            b = clamp<float>(fb * co3, 0, 1) * 255.0f;
            a = clamp<float>(fa, 0, 1) * 255.0f;
            
            *outP = SDL_MapRGBA(&fm, r, g, b, a);
        }
    
    /* Store new surface in the input pointer */
    SDL_FreeSurface(in);
    in = out;
}

/* An implementation of the bitmap blit equation (see shader/bitmapBlit.frag),
 * modified for combining text with its outline. */
static inline void blendText(SDL_Surface *txtSrf, const SDL_Rect &inRect, const SDL_Color &inColor,
                           SDL_Surface *outSrf, const SDL_Rect &outRect, const SDL_Color &outColor, bool hasShadow)
{
    size_t offset = (inRect.x * txtSrf->format->BytesPerPixel) + (inRect.y * txtSrf->pitch);
    uint8_t *txtStart = (uint8_t*)txtSrf->pixels + offset;
    offset = (outRect.x * outSrf->format->BytesPerPixel) + (outRect.y * outSrf->pitch);
    uint8_t *outStart = (uint8_t*)outSrf->pixels + offset;
    
    // SDL_TTF sets every pixel to the same RGB value and just adjusts the alpha
    float txtR = inColor.r;
    float txtG = inColor.g;
    float txtB = inColor.b;
    float outR = outColor.r;
    float outG = outColor.g;
    float outB = outColor.b;
    
    /* SDL_ttf blends the glyphs together, which causes overlapping
     * transparent pixels to get too opaque. RGSS probably does it, too,
     * and you'd probably have to zoom in to see it, but if you do see it
     * then it looks kind of ugly so we'll fix it.
     * I don't know if it can actually happen for non-outline text,
     * but we'll handle it, too, just in case.
     * We don't do it for non-outline text if there's a shadow, because I'm not sure how to do this workaround with shadows. */
    uint32_t fullTxtPixel = SDL_MapRGBA(outSrf->format, inColor.r, inColor.g, inColor.b, inColor.a);
    uint32_t fullOutPixel = SDL_MapRGBA(outSrf->format, outColor.r, outColor.g, outColor.b, outColor.a);
    
    /* fa / inColor.a without a per-pixel divide (Cortex-A9 has none): for
     * 2 <= d <= 255 and fa < 2^16, (fa * ((2^32 - 1) / d + 1)) >> 32 == fa / d.
     * The blend branch needs 0 < txtA < inColor.a, so d >= 2 there. */
    const uint32_t alphaRecip = inColor.a >= 2 ? 0xFFFFFFFFu / inColor.a + 1 : 0;
    
    for (int i=0; i < inRect.h; ++i)
    {
        uint32_t *txtPixel = (uint32_t*)(txtStart + i*txtSrf->pitch);
        uint32_t *outPixel = (uint32_t*)(outStart + i*outSrf->pitch);
        for (int j=0; j < inRect.w; ++j)
        {
            uint8_t txtA = (*txtPixel >> txtSrf->format->Ashift) & 0xFF;
            uint8_t outA = (*outPixel >> outSrf->format->Ashift) & 0xFF;
            
            if (txtA >= inColor.a)
            {
                if (hasShadow)
                {
                    *outPixel = *txtPixel;
                }
                else
                {
                    *outPixel = fullTxtPixel;
                }
            } else if (outA == 0) {
                *outPixel = *txtPixel;
            } else if (txtA != 0) {
                /* Use the full text opacity instead of 255. */
                int32_t co1 = (int)txtA * inColor.a;
                int32_t co2 = (int)std::min(outA, outColor.a) * (inColor.a - txtA);
                
                /* Result alpha */
                int32_t fa = co1 + co2;
                
                /* Result colors */
                uint8_t r, g, b, a;
                
                float faInv = 1.0f / fa;
                float co3 = co1 * faInv;
                float co4 = co2 * faInv;

                if (hasShadow)
                {
                    txtR = (*txtPixel >> txtSrf->format->Rshift) & 0xFF;
                    txtG = (*txtPixel >> txtSrf->format->Gshift) & 0xFF;
                    txtB = (*txtPixel >> txtSrf->format->Bshift) & 0xFF;
                }

                // Adding a small number to combat floating point errors.
                r = std::min<int>((txtR * co3 + outR * co4) + 0.001f, 255);
                g = std::min<int>((txtG * co3 + outG * co4) + 0.001f, 255);
                b = std::min<int>((txtB * co3 + outB * co4) + 0.001f, 255);
                
                /* RGSS seems to not round, but our blit shader seemingly does. */
                a = (uint32_t)(((uint64_t)(uint32_t)fa * alphaRecip) >> 32);
                
                *outPixel = SDL_MapRGBA(outSrf->format, r, g, b, a);
            } else if (outA > outColor.a) {
                /* SDL_ttf blends the glyphs together, which causes overlapping
                 * transparent pixels to get too opaque. */
                *outPixel = fullOutPixel;
            }
            
            ++txtPixel;
            ++outPixel;
        }
    }
}

void Bitmap::drawText(const IntRect &rect, const char *str, int align)
{
    guardDisposed();

    GUARD_ANIMATED;

#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
    {
        char tb[192];
        snprintf(tb, sizeof(tb),
                 "trace: Bitmap::drawText rect=%d,%d %dx%d str=%.32s",
                 rect.x, rect.y, rect.w, rect.h, str ? str : "(null)");
        vita_glue_trace(tb);
    }
#endif
    // RGSS doesn't let you draw text backwards
    if (rect.w <= 0 || rect.h <= 0 || rect.x >= width() || rect.y >= height() ||
        rect.w < -rect.x || rect.h < -rect.y)
        return;
    
    if (hasHires()) {
        p->selfHires->guardDisposed();
        p->selfHires->setFont(getFont());

        int rectX = rect.x * p->selfHires->width() / width();
        int rectY = rect.y * p->selfHires->height() / height();
        int rectWidth = rect.w * p->selfHires->width() / width();
        int rectHeight = rect.h * p->selfHires->height() / height();

        p->selfHires->drawText(IntRect(rectX, rectY, rectWidth, rectHeight), str, align);

        return;
    }

    std::string fixed = fixupString(str);
    str = fixed.c_str();
    
    if (*str == '\0')
        return;
    
    if (str[0] == ' ' && str[1] == '\0')
        return;
    
    TTF_Font *sdlFont = p->font->getSdlFont(0);
    const Color &fontColor = p->font->getColor();
    const Color &outColor = p->font->getOutColor();
    
    SDL_Color c = fontColor.toSDLColor();
    
    if (c.a == 0)
        return;
    
    // RGSS crops the the text slightly if there's an outline
    int scaledOutlineSize = 0;
    SDL_Color co;
    if (p->font->getOutline()) {
        // Handle high-res for outline.
        if (p->selfLores) {
            scaledOutlineSize = OUTLINE_SIZE * width() / p->selfLores->width();
        } else {
            scaledOutlineSize = OUTLINE_SIZE;
        }
        
        /* RGSS's outline is drawn by blitting a complete set of text four times, offset diagonally.
         * However, this looks very ugly in hires mode, so instead we'll fake the effect by
         * precomputing the final outline and text colors. */
        co = outColor.toSDLColor();

        if (c.a != 255) {
            /* Report once per process, not once per text redraw: outlined
             * translucent text takes this path on every draw. */
            static bool reportedOutlineTranslucent = false;
            if (!reportedOutlineTranslucent) {
                reportedOutlineTranslucent = true;
                Debug() << "BUG: Bitmap drawText with outline and translucent text is broken";
            }
        }

        if (c.a != 255 || co.a != 255) {
            /* Step 1: Compute the outline alpha by layering it onto itself */
            uint8_t out_alpha = ((int)co.a * (int)c.a) / 255;
            
            int co1 = out_alpha * 255;
            int co2 = out_alpha * (255 - out_alpha);
            int fa = co1 + co2;
            co.a = (fa + 1 + (fa >> 8)) >> 8;
            /* Use this instead if we decide we want to round 
             * RGSS seems to not round, but our blit shader seemingly does. */
            //co.a = (fa + 128 + ((fa + 128) >> 8)) >> 8;
            
            if (c.a != 255) {
                /* Step 2: Compute the opacity of the outline that would have been drawn behind the text.
                 * In RGSS, there's a 1 pixel wide region at the edge of the text that only has
                 * 2 layers of outline instead of the 4 layers that's behind most of the text,
                 * which combined with the outlines having less opaque corners from how they're drawn
                 * slightly affects the appearance of the text. We can't replicate this in a way that
                 * looks nice in hires mode, however, this will have to be good enough. */
                uint8_t out_alpha_full = co.a; // compute outline alpha - 4 layers
                for (int i = 0; i < 2; ++i) {
                    int co1 = out_alpha * 255;
                    int co2 = out_alpha_full * (255 - out_alpha);
                    int fa = co1 + co2;
                    out_alpha_full = (fa + 1 + (fa >> 8)) >> 8;
                    /* Use this instead if we decide we want to round 
                     * RGSS seems to not round, but our blit shader seemingly does. */
                    //out_alpha_full = (fa + 128 + ((fa + 128) >> 8)) >> 8;
                }
                
                /* Step 3: Calculate the text color using out_alpha_full in place of co.a. */
                int co1 = c.a * 255;
                int co2 = out_alpha_full * (255 - c.a);
                int fa = co1 + co2;
                
                float faInv = 1.0f / fa;
                float co3 = co1 * faInv;
                float co4 = co2 * faInv;
                // Adding a small number to combat floating point errors.
                c.r = std::min<int>((c.r * co3 + co.r * co4) + 0.001f, 255);
                c.g = std::min<int>((c.g * co3 + co.g * co4) + 0.001f, 255);
                c.b = std::min<int>((c.b * co3 + co.g * co4) + 0.001f, 255);
                
                c.a = (fa + 1 + (fa >> 8)) >> 8;
                /* Use this instead if we decide we want to round 
                 * RGSS seems to not round, but our blit shader seemingly does. */
                //c.a = (fa + 128 + ((fa + 128) >> 8)) >> 8;
            }
        }
    }
    int doubleOutlineSize = scaledOutlineSize * 2;
    
    // Use the output of textSize to determine squeezing, since textSize tends to be used to determine
    // rect dimensions.
    // Also use it to determine position, because freetype sometimes treats the last character as
    // being a pixel wider than it should be, and which textSize is currently set to compensate for.
    int alignmentWidth, alignmentHeight;
    {
        const IntRect &text_size = textSize(str);
        alignmentWidth = text_size.w;
        alignmentHeight = text_size.h;
        
        if (!alignmentWidth)
            return;
    }
    
    // Trim the text to only fill double the rect width
    int charLimit = 0;
    float squeezeLimit = 0.5f;
    if (TTF_MeasureUTF8(sdlFont, str, std::min(width() - rect.x, rect.w) / squeezeLimit, nullptr, &charLimit) == 0)
    {
        if (charLimit != fixed.size())
        {
            /* TTF_MeasureUTF8 returns the charLimit in codepoints, not bytes,
             * so we have to calculate where that limit is ourselves.
             * Grabbing a few codepoints past the limit in case the next
             * character is a multi codepoint character.*/
            charLimit += 4;
            for(std::string::iterator it=fixed.begin(); it!=fixed.end() && *it != '\0'; ++it)
            {
                /* The first byte of a multibyte character starts with the first
                 * two bits set to 11, with subsequent bytes starting with 10.
                 * Single byte characters start with 0.*/
                if ((*it & 0xC0) != 0x80)
                {
                    if (charLimit-- == 0)
                    {
                        *it = '\0';
                        break;
                    }
                }
            }
        }
    }
    
    SDL_Surface *txtSurf;
    
    if (p->font->isSolid())
        txtSurf = TTF_RenderUTF8_Solid(sdlFont, str, c);
    else
        txtSurf = TTF_RenderUTF8_Blended(sdlFont, str, c);
    
    if (!txtSurf)
        throw Exception(Exception::SDLError, "Error creating text: %s",
                        SDL_GetError());
    
    try {
        p->ensureFormat(txtSurf, SDL_PIXELFORMAT_ABGR8888);
    } catch (...) {
        SDL_FreeSurface(txtSurf);
        throw;
    }
    
    if (p->font->getShadow())
    {
        int scaledShadowSize = 1;
        if (p->selfLores) {
            scaledShadowSize = scaledShadowSize * width() / p->selfLores->width();
        }

        try {
            applyShadow(txtSurf, *p->format, c, scaledShadowSize);
        } catch (...) {
            SDL_FreeSurface(txtSurf);
            throw;
        }
    }
    
    int alignX = rect.x;
    
    switch (align)
    {
        default:
        case Left :
            break;
            
        case Center :
            // Yes, half of the outline size.
            alignX += (rect.w - (alignmentWidth + scaledOutlineSize)) / 2;
            break;
            
        case Right :
            // I don't know why it's double the outline size, but it is.
            alignX += rect.w - alignmentWidth - doubleOutlineSize;
            break;
    }
    
    if (alignX < rect.x)
        alignX = rect.x;
    
    int alignY = rect.y + ((rect.h - alignmentHeight) / 2) - scaledOutlineSize;
    
    alignY = std::max(alignY, rect.y);
    
    /* FIXME: RGSS begins squeezing the text before it fills the rect.
     * While this is extremely undesirable, a number of games will understandably
     * have made the rects bigger to compensate, so we should probably match it */
    float squeeze = (float) rect.w / alignmentWidth;
    
    squeeze = clamp(squeeze, squeezeLimit, 1.0f);

    if (scaledOutlineSize)
    {
        SDL_Surface *outline;
        TTF_Font *sdlOutline;
        try {
            sdlOutline = p->font->getSdlFont(scaledOutlineSize);
        } catch (const Exception &e) {
            SDL_FreeSurface(txtSurf);
            throw e;
        }
        if (p->font->isSolid())
            outline = TTF_RenderUTF8_Solid(sdlOutline, str, co);
        else
            outline = TTF_RenderUTF8_Blended(sdlOutline, str, co);
        
        if (!outline) {
            SDL_FreeSurface(txtSurf);
            throw Exception(Exception::SDLError, "Error creating text outline: %s",
                            SDL_GetError());
        }
        
        try {
            p->ensureFormat(outline, SDL_PIXELFORMAT_ABGR8888);
        } catch (...) {
            SDL_FreeSurface(outline);
            SDL_FreeSurface(txtSurf);
            throw;
        }

        // Enterbrain's runtime crops the top row and left column of the text
        // when blitting it onto the outline. We allow the user to optionally
        // disable this cropping, since it's arguably quite ugly.
        int outlineCropUndo = shState->config().fontOutlineCrop ? 0 : scaledOutlineSize;

        /* outline should always be at least doubleOutlineSize bigger than txtSurf,
         * but we may as well validate it here anyway. */
        SDL_Rect inRect = {scaledOutlineSize - outlineCropUndo, scaledOutlineSize - outlineCropUndo,
                           std::min<int>({(int)(rect.w / squeeze) - doubleOutlineSize,
                                          txtSurf->w - scaledOutlineSize,
                                          outline->w - doubleOutlineSize
                                         }) + outlineCropUndo,
                           std::min<int>({rect.h - doubleOutlineSize,
                                          txtSurf->h - scaledOutlineSize,
                                          outline->h - doubleOutlineSize
                                         }) + outlineCropUndo};
        SDL_Rect outRect = {doubleOutlineSize - outlineCropUndo, doubleOutlineSize - outlineCropUndo, 0, 0};
        
        blendText(txtSurf, inRect, c, outline, outRect, co, p->font->getShadow());
        SDL_FreeSurface(txtSurf);
        txtSurf = outline;
    }
    
    IntRect destRect(alignX, alignY,
                    std::min(rect.w, (int)(txtSurf->w * squeeze)),
                    std::min(rect.h, txtSurf->h));
    
    destRect.w = std::min(destRect.w, width() - destRect.x);
    destRect.h = std::min(destRect.h, height() - destRect.y);
    
    IntRect sourceRect(scaledOutlineSize, scaledOutlineSize, destRect.w / squeeze, destRect.h);
    
    Bitmap txtBitmap(txtSurf, nullptr, true);
    bool smooth = squeeze != 1.0f;
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
    {
        char tb[160];
        snprintf(tb, sizeof(tb),
                 "trace: Bitmap::drawText stretchBlt dest=%d,%d %dx%d",
                 destRect.x, destRect.y, destRect.w, destRect.h);
        vita_glue_trace(tb);
    }
#endif
    stretchBlt(destRect, txtBitmap, sourceRect, 255, smooth);
}

/* http://www.lemoda.net/c/utf8-to-ucs2/index.html */
static uint16_t utf8_to_ucs2(const char *_input,
                             const char **end_ptr)
{
    const unsigned char *input =
    reinterpret_cast<const unsigned char*>(_input);
    *end_ptr = _input;
    
    if (input[0] == 0)
        return -1;
    
    if (input[0] < 0x80)
    {
        *end_ptr = _input + 1;
        
        return input[0];
    }
    
    if ((input[0] & 0xE0) == 0xE0)
    {
        if (input[1] == 0 || input[2] == 0)
            return -1;
        
        *end_ptr = _input + 3;
        
        return (input[0] & 0x0F)<<12 |
        (input[1] & 0x3F)<<6  |
        (input[2] & 0x3F);
    }
    
    if ((input[0] & 0xC0) == 0xC0)
    {
        if (input[1] == 0)
            return -1;
        
        *end_ptr = _input + 2;
        
        return (input[0] & 0x1F)<<6  |
        (input[1] & 0x3F);
    }
    
    return -1;
}

IntRect Bitmap::textSize(const char *str)
{
    guardDisposed();
    
    GUARD_ANIMATED;
    
    // TODO: High-res Bitmap textSize not implemented, but I think it's the same as low-res?
    // Need to double-check this.

    TTF_Font *sdlFont = p->font->getSdlFont(0);
    
    // freetype sometimes treats the last character of the string as being
    // a pixel wider than it should be. Adding a space at the end and then
    // removing it's width should make character-by-character text
    // more accurate.
    std::string fixed = fixupString(str) + " ";
    
    int w, h;
    TTF_SizeUTF8(sdlFont, fixed.c_str(), &w, &h);
    
    int ws;
    TTF_SizeUTF8(sdlFont, " ", &ws, 0);
    w -= ws;
    
    /* If str is one character long, *endPtr == 0 */
    const char *endPtr;
    uint16_t ucs2 = utf8_to_ucs2(str, &endPtr);
    
    /* For cursive characters, returning the advance
     * as width yields better results */
    if (p->font->getItalic() && *endPtr == '\0')
        TTF_GlyphMetrics(sdlFont, ucs2, 0, 0, 0, 0, &w);

    if (shState->config().fontHeightReporting == 0) {
        if(!w) {
            h = 0;
        } else {
            /* RGSS normalizes the reported heights.
             * Note that this may result in the bottoms
             * of some characters being cut off. */
             h = TTF_FontHeight(sdlFont);
        }
    }
    
    return IntRect(0, 0, w, h);
}

DEF_ATTR_RD_SIMPLE(Bitmap, Font, Font&, *p->font)

void Bitmap::setFont(Font &value)
{
    *p->font = value;
}

void Bitmap::setInitFont(Font *value)
{
    if (value != &shState->defaultFont()) {
        if (p->selfLores)
            value->setHiresMult((float)width() / (float)p->selfLores->width());
        else
            value->setHiresMult(1.0f);
    }

    p->font = value;
}

TEXFBO &Bitmap::getGLTypes() const
{
#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* THE flush-before-sample entry point. Four call sites read .tex straight
     * off this TEXFBO instead of going through bindTex(): sprite.cpp
     * (setPattern), graphics.cpp (setTransMap), tilemap.cpp (both atlas
     * blitSource calls) and gl/tileatlasvx.cpp. Flushing here covers all of
     * them, and .fbo stays 0 forever. */
    p->ensureTexture();
#endif
    return p->getGLTypes();
}

SDL_Surface *Bitmap::surface() const
{
    if (hasHires()) {
        Debug() << "BUG: Called surface() on low-res Bitmap; graphics quality will be degraded.";
    }

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* The stock "lazily read back for getPixel" cache is now the Bitmap's
     * authoritative pixels, so this is always available. binding/
     * bitmap-binding.cpp uses a non-null result to skip the GFX lock around
     * getPixel, which is exactly right: getPixel never touches GL. */
    return p->pixels();
#else
    return p->surface;
#endif
}

SDL_Surface *Bitmap::megaSurface() const
{
    if (hasHires()) {
        Debug() << "BUG: Called megaSurface() on low-res Bitmap; graphics quality will be degraded.";
    }

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* Keeps its stock meaning for outside callers (Tilemap uses it both to
     * reject unusable autotiles and to choose its CPU upload path): only a
     * genuinely oversized Bitmap reports one. */
    return p->trueMega ? p->cpuSurface : 0;
#else
    return p->megaSurface;
#endif
}

void Bitmap::ensureNonMega() const
{
    if (isDisposed())
        return;
    
    GUARD_MEGA;
}

void Bitmap::ensureNonAnimated() const
{
    if (isDisposed())
        return;
    
    GUARD_ANIMATED;
}

void Bitmap::ensureAnimated() const
{
    if (isDisposed())
        return;
    
    GUARD_UNANIMATED;
}

void Bitmap::stop()
{
    guardDisposed();
    
    GUARD_UNANIMATED;
    if (!p->animation.playing) return;
    
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap stop not implemented";
    }

    p->animation.stop();
}

void Bitmap::play()
{
    guardDisposed();
    
    GUARD_UNANIMATED;
    if (p->animation.playing) return;

    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap play not implemented";
    }

    p->animation.play();
}

bool Bitmap::isPlaying() const
{
    guardDisposed();
    
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap isPlaying not implemented";
    }

    if (!p->animation.playing)
        return false;
    
    if (p->animation.loop)
        return true;

    return p->animation.currentFrameIRaw() < p->animation.frameCount();
}

void Bitmap::gotoAndStop(int frame)
{
    guardDisposed();
    
    GUARD_UNANIMATED;
    
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap gotoAndStop not implemented";
    }

    p->animation.stop();
    p->animation.seek(frame);
}
void Bitmap::gotoAndPlay(int frame)
{
    guardDisposed();
    
    GUARD_UNANIMATED;
    
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap gotoAndPlay not implemented";
    }

    p->animation.stop();
    p->animation.seek(frame);
    p->animation.play();
}

int Bitmap::numFrames() const
{
    guardDisposed();
    
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap numFrames not implemented";
    }

    if (!p->animation.enabled) return 1;
    return (int)p->animation.frameCount();
}

int Bitmap::currentFrameI() const
{
    guardDisposed();
    
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap currentFrameI not implemented";
    }

    if (!p->animation.enabled) return 0;
    return p->animation.currentFrameI();
}

int Bitmap::addFrame(Bitmap &source, int position)
{
    guardDisposed();
    source.guardDisposed();
    
    GUARD_MEGA;
    
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap addFrame dest not implemented";
    }

    if (source.hasHires()) {
        Debug() << "BUG: High-res Bitmap addFrame source not implemented";
    }

    if (source.height() != height() || source.width() != width())
        throw Exception(Exception::MKXPError, "Animations with varying dimensions are not supported (%ix%i vs %ix%i)",
                        source.width(), source.height(), width(), height());
    
#ifdef MKXPZ_SOFTWARE_BITMAPS
    {
        SDL_Surface *srcPx = source.p->pixels();
        if (!srcPx)
            throw Exception(Exception::MKXPError, "Bitmap has no CPU pixels");

        BitmapPrivate::PixelOwner newframe(p->copyPixels(srcPx), BitmapPrivate::freePixels);
        auto &frames = p->animation.cpuFrames;
        frames.reserve(frames.size() + (p->animation.enabled ? 1 : 2));

        // Convert the bitmap into an animated bitmap if it isn't already one
        if (!p->animation.enabled) {
            p->animation.width = p->gl.width;
            p->animation.height = p->gl.height;
            p->animation.lastFrame = 0;
            p->animation.playTime = 0;
            p->animation.startTime = 0;

            if (!std::isfinite(p->animation.fps) || p->animation.fps <= 0)
                p->animation.fps = shState->graphics().getFrameRate();

            /* The static Bitmap's pixels become frame 0. */
            p->animation.cpuFrames.push_back(p->cpuSurface);
            p->cpuSurface = 0;
        }

        int ret;

        if (position < 0) {
            p->animation.cpuFrames.push_back(newframe.get());
            ret = (int)p->animation.cpuFrames.size();
        }
        else {
            p->animation.cpuFrames.insert(
                p->animation.cpuFrames.begin() +
                    clamp(position, 0, (int)p->animation.cpuFrames.size()),
                newframe.get());
            ret = position;
        }

        newframe.release();
        p->animation.enabled = true;

        /* Whichever frame the cache held may now be a different index. */
        p->cachedFrame = -1;
        p->texDirty = true;

        return ret;
    }
#else
    p->animation.frames.reserve(p->animation.frames.size() + (p->animation.enabled ? 1 : 2));
    struct FrameOwner {
        TEXFBO value;
        ~FrameOwner() { if (value.tex.gl) shState->texPool().release(value); }
    } owner{shState->texPool().request(source.width(), source.height())};
    TEXFBO &newframe = owner.value;
    
    if (source.surface()) {
        TEX::bind(newframe.tex);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        FrameProfile::Scope profileDirectUpload(FrameProfile::Upload, 0, false, false);
#endif
        TEX::uploadImage(source.width(), source.height(), source.surface()->pixels, GL_RGBA);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        profileDirectUpload.stop();
#endif
    }
    else {
        GLMeta::blitBegin(newframe, false, SameScale);
        GLMeta::blitSource(source.getGLTypes(), SameScale);
        GLMeta::blitRectangle(rect(), rect());
        GLMeta::blitEnd();
    }
    
    if (p->surface) {
        SDL_FreeSurface(p->surface);
        p->surface = 0;
    }
    // Convert the bitmap into an animated bitmap if it isn't already one
    if (!p->animation.enabled) {
        p->animation.width = p->gl.width;
        p->animation.height = p->gl.height;
        p->animation.lastFrame = 0;
        p->animation.playTime = 0;
        p->animation.startTime = 0;
        
        if (!std::isfinite(p->animation.fps) || p->animation.fps <= 0)
            p->animation.fps = shState->graphics().getFrameRate();
        
        p->animation.frames.push_back(p->gl);
        p->gl = TEXFBO();
    }
    
    int ret;
    
    if (position < 0) {
        p->animation.frames.push_back(newframe);
        ret = (int)p->animation.frames.size();
    }
    else {
        p->animation.frames.insert(p->animation.frames.begin() + clamp(position, 0, (int)p->animation.frames.size()), newframe);
        ret = position;
    }
    
    owner.value = TEXFBO();
    p->animation.enabled = true;
    return ret;
#endif
}

void Bitmap::removeFrame(int position) {
    guardDisposed();
    
    GUARD_UNANIMATED;
    
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap removeFrame not implemented";
    }

#ifdef MKXPZ_SOFTWARE_BITMAPS
    {
        /* An animation can hold no frame at all (a GIF whose partial decode
         * found none) and the last one cannot go either: the index below
         * would be -1 or 0 of an empty vector. */
        if (p->animation.cpuFrames.size() < 2)
            throw Exception(Exception::MKXPError,
                            "Cannot remove the only frame of an animated bitmap");

        int pos = (position < 0) ? (int)p->animation.cpuFrames.size() - 1
                                 : clamp(position, 0, (int)(p->animation.cpuFrames.size() - 1));
        BitmapPrivate::countPixels(p->animation.cpuFrames[pos], -1);
        SDL_FreeSurface(p->animation.cpuFrames[pos]);
        p->animation.cpuFrames.erase(p->animation.cpuFrames.begin() + pos);

        // Change the animated bitmap back to a normal one if there's only one frame left
        if (p->animation.cpuFrames.size() == 1) {
            p->animation.enabled = false;
            p->animation.playing = false;
            p->animation.width = 0;
            p->animation.height = 0;
            p->animation.lastFrame = 0;

            p->cpuSurface = p->animation.cpuFrames[0];
            p->animation.cpuFrames.erase(p->animation.cpuFrames.begin());

            taintArea(rect());
        }

        p->cachedFrame = -1;
        p->texDirty = true;
        return;
    }
#else
    if (p->animation.frames.size() < 2)
        throw Exception(Exception::MKXPError,
                        "Cannot remove the only frame of an animated bitmap");

    int pos = (position < 0) ? (int)p->animation.frames.size() - 1 : clamp(position, 0, (int)(p->animation.frames.size() - 1));
    shState->texPool().release(p->animation.frames[pos]);
    p->animation.frames.erase(p->animation.frames.begin() + pos);
    
    // Change the animated bitmap back to a normal one if there's only one frame left
    if (p->animation.frames.size() == 1) {
        
        p->animation.enabled = false;
        p->animation.playing = false;
        p->animation.width = 0;
        p->animation.height = 0;
        p->animation.lastFrame = 0;
        
        p->gl = p->animation.frames[0];
        p->animation.frames.erase(p->animation.frames.begin());
        
        FBO::bind(p->gl.fbo);
        taintArea(rect());
    }
#endif
}

void Bitmap::nextFrame()
{
    guardDisposed();
    
    GUARD_UNANIMATED;
    
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap nextFrame not implemented";
    }

    stop();
    if ((uint32_t)p->animation.lastFrame >= p->animation.frameCount() - 1)  {
        if (!p->animation.loop) return;
        p->animation.lastFrame = 0;
        return;
    }
    
    p->animation.lastFrame++;
}

void Bitmap::previousFrame()
{
    guardDisposed();
    
    GUARD_UNANIMATED;
    
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap previousFrame not implemented";
    }

    stop();
    if (p->animation.lastFrame <= 0) {
        if (!p->animation.loop) {
            p->animation.lastFrame = 0;
            return;
        }
        p->animation.lastFrame = (int)p->animation.frameCount() - 1;
        return;
    }
    
    p->animation.lastFrame--;
}

void Bitmap::setAnimationFPS(float FPS)
{
    guardDisposed();
    
    GUARD_MEGA;
    
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap setAnimationFPS not implemented";
    }

    bool restart = p->animation.playing;
    p->animation.stop();
    p->animation.fps = (FPS < 0) ? 0 : FPS;
    if (restart) p->animation.play();
}

std::vector<TEXFBO> &Bitmap::getFrames() const
{
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap getFrames not implemented";
    }

    return p->animation.frames;
}

float Bitmap::getAnimationFPS() const
{
    guardDisposed();
    
    GUARD_MEGA;
    
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap getAnimationFPS not implemented";
    }

    return p->animation.fps;
}

void Bitmap::setLooping(bool loop)
{
    guardDisposed();
    
    GUARD_MEGA;
    
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap setLooping not implemented";
    }

    p->animation.loop = loop;
}

bool Bitmap::getLooping() const
{
    guardDisposed();
    
    GUARD_MEGA;
    
    if (hasHires()) {
        Debug() << "BUG: High-res Bitmap getLooping not implemented";
    }

    return p->animation.loop;
}

void Bitmap::kglInvert()
{
    guardDisposed();
    GUARD_ANIMATED;

    if (hasHires()) {
        p->selfHires->kglInvert();
        return;
    }

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* The stock mega loop is the reference; it now runs for every Bitmap.
     * Rows step by the surface pitch rather than w*4: initFromSurface()
     * adopts the image decoder's surface verbatim, so the packed-pitch
     * invariant is owned by SDL_image, not by this file, and a padded
     * surface would skew every row. swSurface carries the
     * pitch and is what every other software op already uses. */
    if (p->pixels()) {
        const swraster::Surface cpu = p->swSurface();
        for (int y = 0; y < cpu.h; ++y) {
            uint8_t *row = cpu.px + (size_t)y * (size_t)cpu.stride;
            for (int x = 0; x < cpu.w; ++x) {
                for (size_t j = 0; j < 3; ++j) {
                    row[(size_t)x * 4 + j] = ~row[(size_t)x * 4 + j];
                }
            }
        }
        p->markDirty(rect());
    }
#else
    if (isMega()) {
        for (size_t i = 0; i < (size_t)p->megaSurface->w * (size_t)p->megaSurface->h; ++i) {
            for (size_t j = 0; j < 3; ++j) {
                ((uint8_t *)p->megaSurface->pixels)[4 * i + j] = ~((uint8_t *)p->megaSurface->pixels)[4 * i + j];
            }
        }
    } else {
        TEXFBO newTex = shState->texPool().request(width(), height());

        FloatRect texRect(rect());

        Quad &quad = shState->gpQuad();
        quad.setTexPosRect(texRect, texRect);
        quad.setColor(Vec4(1, 1, 1, 1));

        KglInvertShader &shader = shState->shaders().kglInvert;
        shader.bind();

        FBO::bind(newTex.fbo);
        p->pushSetViewport(shader);
        p->bindTexture(shader, false);

        p->blitQuad(quad);

        p->popViewport();

        TEX::unbind();

        shState->texPool().release(p->gl);
        p->gl = newTex;
    }
#endif

    p->onModified();
}

void Bitmap::kglCompressAlpha()
{
    guardDisposed();
    GUARD_ANIMATED;

    if (hasHires()) {
        p->selfHires->kglInvert();
        return;
    }

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* The stock mega loop is the reference; it now runs for every Bitmap.
     * Rows step by the surface pitch; see kglInvert. */
    if (p->pixels()) {
        const swraster::Surface cpu = p->swSurface();
        for (int y = 0; y < cpu.h; ++y) {
            uint8_t *row = cpu.px + (size_t)y * (size_t)cpu.stride;
            for (int x = 0; x < cpu.w; ++x) {
                uint8_t *px = row + (size_t)x * 4;
                for (size_t j = 0; j < 3; ++j) {
                    px[j] = std::round(((float)px[3] / 255.0f) * px[j]);
                }
                px[3] = 0;
            }
        }
        p->markDirty(rect());
    }
#else
    if (isMega()) {
        for (size_t i = 0; i < (size_t)p->megaSurface->w * (size_t)p->megaSurface->h; ++i) {
            for (size_t j = 0; j < 3; ++j) {
                ((uint8_t *)p->megaSurface->pixels)[4 * i + j] =
                    std::round(((float)((uint8_t *)p->megaSurface->pixels)[4 * i + 3] / 255.0f) * ((uint8_t *)p->megaSurface->pixels)[4 * i + j]);
            }
            ((uint8_t *)p->megaSurface->pixels)[4 * i + 3] = 0;
        }
    } else {
        TEXFBO newTex = shState->texPool().request(width(), height());

        FloatRect texRect(rect());

        Quad &quad = shState->gpQuad();
        quad.setTexPosRect(texRect, texRect);
        quad.setColor(Vec4(1, 1, 1, 1));

        KglCompressAlphaShader &shader = shState->shaders().kglCompressAlpha;
        shader.bind();

        FBO::bind(newTex.fbo);
        p->pushSetViewport(shader);
        p->bindTexture(shader, false);

        p->blitQuad(quad);

        p->popViewport();

        TEX::unbind();

        shState->texPool().release(p->gl);
        p->gl = newTex;
    }
#endif

    p->onModified();
}

int Bitmap::kglShadowShaderH(int x1, int x2, int y, bool soft)
{
    guardDisposed();
    GUARD_ANIMATED;

    if (hasHires()) {
        return p->selfHires->kglShadowShaderH(x1 * p->selfHires->width() / width(), x2 * p->selfHires->width() / width(), y * p->selfHires->height() / height(), soft);
    }

    int w = width();
    int h = height();

    if (y < 0 || y >= h || y == h / 2) {
        return 111;
    }

    if (w <= 0 || h <= 0) {
        return 1;
    }

    x1 = clamp(x1, 0, w - 1);
    x2 = clamp(x2, 0, w - 1);

    int x_center = w / 2;
    int y_center = h / 2;

    double slope1 = (double)(x1 - x_center) / (double)(y - y_center);
    double slope2 = (double)(x2 - x_center) / (double)(y - y_center);

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* The stock mega implementation is the reference; it now runs for every
     * Bitmap. Rows step by shadowPitch rather than w*4: initFromSurface()
     * adopts the image decoder's surface verbatim, so the packed-pitch
     * invariant is not this file's to assume. For the stock
     * branch shadowPitch is w*4, i.e. exactly what it used to compute. */
    if (p->pixels()) {
        const swraster::Surface cpu = p->swSurface();
        uint8_t *shadowbuffer = cpu.px;
        const size_t shadowPitch = (size_t)cpu.stride;
#else
    if (isMega()) {
        uint8_t *shadowbuffer = (uint8_t *)p->megaSurface->pixels;
        const size_t shadowPitch = (size_t)w * 4;
#endif

        int y_start, y_end;
        if (y < y_center) {
            y_start = 0;
            y_end = y - 1;
        } else {
            y_start = y;
            y_end = h - 1;
        }

        for (int i = y_start; i <= y_end; ++i) {
            double x_start_raw = std::round(slope1 * (double)(i - y_center) + (double)x_center);
            double x_end_raw = std::round(slope2 * (double)(i - y_center) + (double)x_center + 0.2f); // The original shader contains a +0.2 adjustment factor for some reason

            int x_start = (int)clamp(x_start_raw, 0., (double)w + 3.);
            int x_end = (int)clamp(x_end_raw, -4., x2 < x_center ? (double)x2 - 1. : (double)w - 1.); // This bounds check is incorrect but is consistent with the original shader

            if (x_start <= x_end) {
                std::memset(
                    shadowbuffer + (size_t)i * shadowPitch + (size_t)x_start * 4,
                    0,
                    (size_t)4 * ((size_t)x_end - (size_t)x_start + (size_t)1)
                );
            }

            if (soft) {
                for (int j = 3; j >= 1; --j) {
                    if (
                        (x1 < x_center ? x_start - j < 0 : x_start - j < x1) // This bounds check is incorrect but is consistent with the original shader
                            || (x2 < x_center ? x_start - j >= x2 : x_start - j >= w) // This bounds check is incorrect but is consistent with the original shader
                    ) {
                        continue;
                    }
                    uint8_t *pixel = shadowbuffer + (size_t)i * shadowPitch + (size_t)(x_start - j) * 4;
                    for (size_t k = 0; k < 3; ++k) {
                        pixel[k] = std::lround((double)pixel[k] * ((double)j / (double)4));
                    }
                }

                if (x2 != x_center) {
                    for (int j = 1; j <= 3; ++j) {
                        if (
                            (x1 < x_center ? x_end < -j : x_end < x1 - j) // This bounds check is incorrect but is consistent with the original shader
                                || (x2 < x_center ? x_end >= x2 - j : x_end >= w - j) // This bounds check is incorrect but is consistent with the original shader
                        ) {
                            continue;
                        }
                        uint8_t *pixel = shadowbuffer + (size_t)i * shadowPitch + (size_t)(x_end + j) * 4;
                        for (size_t k = 0; k < 3; ++k) {
                            pixel[k] = std::lround((double)pixel[k] * ((double)j / (double)4));
                        }
                    }
                }
            }
        }
#ifdef MKXPZ_SOFTWARE_BITMAPS
        p->markDirty(rect());
#endif
    } else {
#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* Unreachable: every Bitmap is CPU-backed. */
#elif defined(MKXPZ_NO_OPTIONAL_SHADERS)
        /* kglShadowH not built. Non-mega bitmaps have no CPU path here;
         * skip the effect rather than fail. Mega bitmaps take the branch above. */
        (void)x1; (void)x2; (void)y; (void)soft;
        (void)w; (void)h; (void)x_center; (void)y_center;
        (void)slope1; (void)slope2;
        Debug() << "kglShadowShaderH: optional shaders disabled; shadow skipped";
#else
        TEXFBO newTex = shState->texPool().request(width(), height());

        FloatRect texRect(rect());

        Quad &quad = shState->gpQuad();
        quad.setTexPosRect(texRect, texRect);
        quad.setColor(Vec4(1, 1, 1, 1));

        KglShadowShaderH &shader = shState->shaders().kglShadowH;
        shader.bind();
        shader.setParams(x1, x2, y, soft, w, h, x_center, y_center, slope1, slope2);

        FBO::bind(newTex.fbo);
        p->pushSetViewport(shader);
        p->bindTexture(shader, false);

        p->blitQuad(quad);

        p->popViewport();

        TEX::unbind();

        shState->texPool().release(p->gl);
        p->gl = newTex;
#endif
    }

    p->onModified();

    return 1;
}

int Bitmap::kglShadowShaderV(int y1, int y2, int x, bool wall, bool soft)
{
    guardDisposed();
    GUARD_ANIMATED;

    if (hasHires()) {
        return p->selfHires->kglShadowShaderV(y1 * p->selfHires->height() / height(), y2 * p->selfHires->height() / height(), x * p->selfHires->width() / width(), wall, soft);
    }

    int w = width();
    int h = height();

    if (x < 0 || x >= h || x == w / 2) {
        return 111;
    }

    if (w <= 0 || h <= 0) {
        return 1;
    }

    y1 = clamp(y1, 0, h - 1);
    y2 = clamp(y2, 0, h - 1);

    int x_center = w / 2;
    int y_center = h / 2;

    double slope1 = (double)(y1 - y_center) / (double)(x - x_center);
    double slope2 = (double)(y2 - y_center) / (double)(x - x_center);

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* See kglShadowShaderH: the stock mega implementation is the reference,
     * and rows step by the surface pitch. */
    if (p->pixels()) {
        const swraster::Surface cpu = p->swSurface();
        uint8_t *shadowbuffer = cpu.px;
        const size_t shadowPitch = (size_t)cpu.stride;
#else
    if (isMega()) {
        uint8_t *shadowbuffer = (uint8_t *)p->megaSurface->pixels;
        const size_t shadowPitch = (size_t)w * 4;
#endif

        int x_start, x_end;
        if (x < x_center) {
            x_start = 0;
            x_end = x - 1;
        } else {
            x_start = x;
            x_end = w - 1;
        }

        for (int i = x_start; i <= x_end; ++i) {
            double y_start_raw = std::round(slope1 * (double)(i - x_center) + (double)y_center);
            double y_end_raw = std::round(slope2 * (double)(i - x_center) + (double)y_center + 0.2f); // The original shader contains a +0.2 adjustment factor for some reason

            int y_start = wall && y1 >= y_center ? y1 : (int)clamp(y_start_raw, 0., (double)h + 3.);
            int y_end = wall && y2 >= y_center ? y2 - 1 : (int)clamp(y_end_raw, -4., y2 < y_center ? (double)y2 - 1. : (double)h - 1.); // This bounds check is incorrect but is consistent with the original shader

            if (y_start <= y_end) {
                for (int j = y_start; j <= y_end; ++j) {
                    std::memset(shadowbuffer + (size_t)j * shadowPitch + (size_t)i * 4, 0, 4);
                }
            }

            if (soft) {
                if (!wall || y1 < y_center) {
                    for (int j = 3; j >= 1; --j) {
                        if (
                            (y1 < y_center ? y_start - j < 0 : y_start - j < y1) // This bounds check is incorrect but is consistent with the original shader
                                || (y2 < y_center ? y_start - j >= y2 : y_start - j >= h) // This bounds check is incorrect but is consistent with the original shader
                        ) {
                            continue;
                        }
                        uint8_t *pixel = shadowbuffer + (size_t)(y_start - j) * shadowPitch + (size_t)i * 4;
                        for (size_t k = 0; k < 3; ++k) {
                            pixel[k] = std::lround((double)pixel[k] * ((double)j / (double)4));
                        }
                    }
                }

                if (y2 != y_center && (!wall || y2 < y_center)) {
                    for (int j = 1; j <= 3; ++j) {
                        if (
                            (y1 < y_center ? y_end < -j : y_end < y1 - j) // This bounds check is incorrect but is consistent with the original shader
                                || (y2 < y_center ? y_end >= y2 - j : y_end >= h - j) // This bounds check is incorrect but is consistent with the original shader
                        ) {
                            continue;
                        }
                        uint8_t *pixel = shadowbuffer + (size_t)(y_end + j) * shadowPitch + (size_t)i * 4;
                        for (size_t k = 0; k < 3; ++k) {
                            pixel[k] = std::lround((double)pixel[k] * ((double)j / (double)4));
                        }
                    }
                }
            }
        }
#ifdef MKXPZ_SOFTWARE_BITMAPS
        p->markDirty(rect());
#endif
    } else {
#ifdef MKXPZ_SOFTWARE_BITMAPS
        /* Unreachable: every Bitmap is CPU-backed. */
#elif defined(MKXPZ_NO_OPTIONAL_SHADERS)
        /* kglShadowV not built. Non-mega bitmaps have no CPU path here;
         * skip the effect rather than fail. Mega bitmaps take the branch above. */
        (void)y1; (void)y2; (void)x; (void)wall; (void)soft;
        (void)w; (void)h; (void)x_center; (void)y_center;
        (void)slope1; (void)slope2;
        Debug() << "kglShadowShaderV: optional shaders disabled; shadow skipped";
#else
        TEXFBO newTex = shState->texPool().request(width(), height());

        FloatRect texRect(rect());

        Quad &quad = shState->gpQuad();
        quad.setTexPosRect(texRect, texRect);
        quad.setColor(Vec4(1, 1, 1, 1));

        KglShadowShaderV &shader = shState->shaders().kglShadowV;
        shader.bind();
        shader.setParams(y1, y2, x, wall, soft, w, h, x_center, y_center, slope1, slope2);

        FBO::bind(newTex.fbo);
        p->pushSetViewport(shader);
        p->bindTexture(shader, false);

        p->blitQuad(quad);

        p->popViewport();

        TEX::unbind();

        shState->texPool().release(p->gl);
        p->gl = newTex;
#endif
    }

    p->onModified();

    return 1;
}

void Bitmap::bindTex(ShaderBase &shader, bool substituteLoresSize)
{
    // Hires mode is handled by p->bindTexture.

    p->bindTexture(shader, substituteLoresSize);
}

void Bitmap::taintArea(const IntRect &rect)
{
    if (hasHires()) {
        int destX, destY, destWidth, destHeight;
        destX = rect.x * p->selfHires->width() / width();
        destY = rect.y * p->selfHires->height() / height();
        destWidth = rect.w * p->selfHires->width() / width();
        destHeight = rect.h * p->selfHires->height() / height();

        p->selfHires->taintArea(IntRect(destX, destY, destWidth, destHeight));
    }

    p->addTaintedArea(rect);
}

int Bitmap::maxSize(){
    return glState.caps.maxTexSize;
}

void Bitmap::assumeRubyGC()
{
    p->assumingRubyGC = true;
}

void Bitmap::releaseResources()
{
    if (p->selfHires && !p->assumingRubyGC) {
        delete p->selfHires;
    }

#ifdef MKXPZ_SOFTWARE_BITMAPS
    /* A software Bitmap owns exactly one optional GL object: the sampling
     * texture, and only if something ever drew it. No FBO, nothing returned
     * to TexPool. */
    if (p->cpuSurface) {
        BitmapPrivate::countPixels(p->cpuSurface, -1);
        SDL_FreeSurface(p->cpuSurface);
    }
    p->cpuSurface = 0;

    for (SDL_Surface *frame : p->animation.cpuFrames) {
        BitmapPrivate::countPixels(frame, -1);
        SDL_FreeSurface(frame);
    }
    p->animation.cpuFrames.clear();
    p->animation.enabled = false;
    p->animation.playing = false;

    /* through the cache, so the LRU and the byte ledger
     * lose this entry too. cacheDrop() is the single deletion-policy call
     * site for a Bitmap cache texture. */
    if (p->cacheLinked)
        cacheProfileNote(texCacheEvictRelease, p->cacheBytes);
    p->cacheDrop();

    if (p->surface)
        SDL_FreeSurface(p->surface);
#else
    if (p->megaSurface)
        SDL_FreeSurface(p->megaSurface);
    if (p->surface)
        SDL_FreeSurface(p->surface);
    else if (p->animation.enabled) {
        p->animation.enabled = false;
        p->animation.playing = false;
        for (TEXFBO &tex : p->animation.frames)
            shState->texPool().release(tex);
    }
    else
        shState->texPool().release(p->gl);
#endif


    if (p->pChild)
    {
        delete p->pChild;
    }
    
    delete p;
}

void Bitmap::loresDisposal()
{
    loresDispCon.disconnect();
    dispose();
}
