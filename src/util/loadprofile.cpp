// SPDX-License-Identifier: GPL-3.0-or-later
// Diagnostic PNG stage timing leaves codec results and pixel operations unchanged.
// Inflate is re-measured over a captured copy of the compressed stream because
// the linker wrap does not reach libpng's internal zlib calls on the static
// link; the PNG stage therefore also includes the capture copy and that
// measurement pass, and only when profiling is active.
#ifdef __vita__
#include "frameprofile.h"
#include <SDL.h>
#include <zlib.h>
#include <stdlib.h>
#include <string.h>

extern "C" SDL_Surface *__real_IMG_LoadPNG_RW(SDL_RWops *);

namespace {

const size_t CaptureMax = 8 * 1024 * 1024;

struct Capture {
    SDL_RWops *orig = 0;
    unsigned char *data = 0;
    size_t used = 0, cap = 0;
    bool invalid = true;
};

Sint64 SDLCALL captureSize(SDL_RWops *ctx) {
    Capture &c = *static_cast<Capture *>(ctx->hidden.unknown.data1);
    return c.orig->size(c.orig);
}

Sint64 SDLCALL captureSeek(SDL_RWops *ctx, Sint64 offset, int whence) {
    Capture &c = *static_cast<Capture *>(ctx->hidden.unknown.data1);
    if (whence != RW_SEEK_CUR || offset != 0)
        c.invalid = true; // Decode is sequential; SDL_RWtell is seek(0, CUR), only that stays valid.
    return c.orig->seek(c.orig, offset, whence);
}

size_t SDLCALL captureRead(SDL_RWops *ctx, void *ptr, size_t size, size_t maxn) {
    Capture &c = *static_cast<Capture *>(ctx->hidden.unknown.data1);
    size_t got = c.orig->read(c.orig, ptr, size, maxn);
    if (!c.invalid && got) {
        const unsigned long long want = (unsigned long long)got * size;
        const unsigned long long room = c.cap - c.used;
        const unsigned long long take = want < room ? want : room;
        memcpy(c.data + c.used, ptr, (size_t)take);
        c.used += (size_t)take;
        if (take < want)
            c.invalid = true; // Over the cap: decode is timed, the inflate pass is not.
    }
    return got;
}

/* Concatenate the IDAT payloads in place and return their length; 0 unless the
 * capture starts at the signature, which the sequential decode guarantees. */
size_t extractIDAT(unsigned char *data, size_t used) {
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    size_t out = 0;
    if (used >= 8 && !memcmp(data, signature, 8)) {
        size_t at = 8;
        while (used - at >= 12) {
            const unsigned long len = ((unsigned long)data[at] << 24) | ((unsigned long)data[at + 1] << 16) |
                                      ((unsigned long)data[at + 2] << 8) | data[at + 3];
            if (len > used - at - 12)
                break;
            const size_t body = at + 8;
            if (len && !memcmp(data + at + 4, "IDAT", 4)) {
                memmove(data + out, data + body, len);
                out += len;
            }
            at = body + len + 4;
        }
    }
    return out;
}

/* A second inflate of the captured stream is the inflate stage: no I/O, pure
 * zlib cost, and independent of any symbol interception. */
void timeInflate(unsigned char *idat, size_t len) {
    unsigned char *scratch = (unsigned char *)malloc(16 * 1024);
    if (!scratch)
        return;
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit(&zs) == Z_OK) {
        zs.next_in = idat;
        zs.avail_in = (uInt)len;
        {
            FrameProfile::AssetStageScope scope(FrameProfile::AssetStageScope::Inflate);
            int status;
            do {
                zs.next_out = scratch;
                zs.avail_out = 16 * 1024;
                status = inflate(&zs, Z_NO_FLUSH);
            } while (status == Z_OK);
        }
        inflateEnd(&zs);
    }
    free(scratch);
}

SDL_RWops *startCapture(SDL_RWops *rw, Capture &capture) {
    const Sint64 reported = rw->size(rw);
    if (reported == 0)
        return rw;
    const size_t cap = reported > 0 && (unsigned long long)reported <= CaptureMax
                           ? (size_t)reported : CaptureMax;
    capture.orig = rw;
    capture.data = (unsigned char *)malloc(cap);
    if (!capture.data)
        return rw;
    capture.cap = cap;
    capture.invalid = false;
    SDL_RWops *filter = SDL_AllocRW();
    if (!filter) {
        free(capture.data);
        capture.data = 0;
        return rw;
    }
    filter->type = SDL_RWOPS_UNKNOWN;
    filter->hidden.unknown.data1 = &capture;
    filter->size = captureSize;
    filter->seek = captureSeek;
    filter->read = captureRead;
    return filter;
}

}

extern "C" SDL_Surface *__wrap_IMG_LoadPNG_RW(SDL_RWops *rw) {
    Capture capture;
    SDL_RWops *filtered = rw;
    if (rw && FrameProfile::assetActive() && FrameProfile::state.imageToken)
        filtered = startCapture(rw, capture);
    SDL_Surface *surface;
    {
        FrameProfile::AssetStageScope scope(FrameProfile::AssetStageScope::PNG);
        surface = __real_IMG_LoadPNG_RW(filtered);
        if (filtered != rw && !capture.invalid)
            timeInflate(capture.data, extractIDAT(capture.data, capture.used));
    }
    if (filtered != rw) {
        free(capture.data);
        SDL_FreeRW(filtered);
    }
    return surface;
}
#endif
