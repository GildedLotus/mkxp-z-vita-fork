// SPDX-License-Identifier: GPL-3.0-or-later
/* overlay: see overlay.h for the contract.
 *
 * Nothing here divides or takes a modulus by anything but a compile-time
 * constant -- the Cortex-A9 has no hardware integer divide and these loops
 * run over 65 536 pixels. Row addressing is
 * y * OVERLAY_W + x, a shift and an add.
 */
#include "overlay.h"

#include <limits.h>
#include <string.h>

#include "font8x8.h"

/* The live surface, and the copy that was last uploaded. Both static: the
 * module never allocates, so it cannot fail at the moment it is needed most
 * (reporting that something else already failed). */
static uint32_t s_px[OVERLAY_PIXELS];
static uint32_t s_uploaded[OVERLAY_PIXELS];

/* Pixels of s_px that differ from s_uploaded. Maintained incrementally on
 * every write -- two comparisons, no scan -- so overlay_dirty() is O(1) and
 * a clear-and-rebuild that reproduces the same image ends up clean. */
static int s_diff;

/* Set until the first overlay_mark_uploaded(): the texture has no contents
 * yet, so even an all-transparent surface has to be sent once. */
static int s_never_uploaded = 1;

static void put(int index, uint32_t argb)
{
    uint32_t old = s_px[index];
    int before, after;

    if (old == argb)
        return;

    before = (old != s_uploaded[index]);
    s_px[index] = argb;
    after = (argb != s_uploaded[index]);
    s_diff += after - before;
}

void overlay_init(void)
{
    int i;

    for (i = 0; i < OVERLAY_PIXELS; ++i)
        s_px[i] = OVERLAY_TRANSPARENT;
    memset(s_uploaded, 0, sizeof(s_uploaded));
    s_diff = 0;
    s_never_uploaded = 1;
}

void overlay_clear(void)
{
    int i;

    for (i = 0; i < OVERLAY_PIXELS; ++i)
        put(i, OVERLAY_TRANSPARENT);
}

void overlay_box(int x, int y, int w, int h, uint32_t argb)
{
    int x0, y0, x1, y1, row, col, base;

    if (w <= 0 || h <= 0)
        return;

    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    /* x + w overflows for a wild caller, so clip by comparison instead of by
     * addition: a bad HUD line must not scribble over the heap. */
    x1 = (x > OVERLAY_W - w) ? OVERLAY_W : x + w;
    y1 = (y > OVERLAY_H - h) ? OVERLAY_H : y + h;
    if (x0 >= x1 || y0 >= y1)
        return;

    for (row = y0; row < y1; ++row) {
        base = row * OVERLAY_W;
        for (col = x0; col < x1; ++col)
            put(base + col, argb);
    }
}

int overlay_text_ex(int x, int y, const char *text, uint32_t argb)
{
    const unsigned char *p = (const unsigned char *)text;
    int pen = x;

    if (!text)
        return pen;

    for (; *p; ++p) {
        unsigned char c = *p;
        const uint8_t *glyph;
        int row;

        if (c < OVERLAY_FONT_FIRST || c > OVERLAY_FONT_LAST)
            c = '?';
        glyph = overlay_font8x8[c - OVERLAY_FONT_FIRST];

        /* Wholly off the surface: still advances, still draws nothing. */
        if (pen > -OVERLAY_GLYPH_W && pen < OVERLAY_W &&
            y > -OVERLAY_GLYPH_H && y < OVERLAY_H) {
            for (row = 0; row < OVERLAY_GLYPH_H; ++row) {
                int py = y + row;
                uint8_t bits = glyph[row];
                int base, col;

                if (bits == 0 || py < 0 || py >= OVERLAY_H)
                    continue;
                base = py * OVERLAY_W;
                for (col = 0; col < OVERLAY_GLYPH_W; ++col) {
                    int px = pen + col;

                    if (!(bits & (0x80u >> col)))
                        continue;
                    if (px < 0 || px >= OVERLAY_W)
                        continue;
                    put(base + px, argb);
                }
            }
        }

        if (pen > INT_MAX - OVERLAY_GLYPH_W)
            return INT_MAX;
        pen += OVERLAY_GLYPH_W;
    }

    return pen;
}

int overlay_text(int x, int y, const char *text)
{
    return overlay_text_ex(x, y, text, OVERLAY_WHITE);
}

int overlay_text_width(const char *text)
{
    int n = 0;

    if (!text)
        return 0;
    while (text[n])
        ++n;
    return n * OVERLAY_GLYPH_W;
}

const uint32_t *overlay_pixels(void)
{
    return s_px;
}

void overlay_copy_rgba8888(uint8_t *dst, int dstStride)
{
    int row, col;

    if (!dst || dstStride < OVERLAY_W * 4)
        return;

    for (row = 0; row < OVERLAY_H; ++row) {
        const uint32_t *src = s_px + row * OVERLAY_W;
        uint8_t *out = dst + (size_t)row * (size_t)dstStride;

        for (col = 0; col < OVERLAY_W; ++col) {
            uint32_t v = src[col];

            out[0] = (uint8_t)(v >> 16);
            out[1] = (uint8_t)(v >> 8);
            out[2] = (uint8_t)v;
            out[3] = (uint8_t)(v >> 24);
            out += 4;
        }
    }
}

int overlay_dirty(void)
{
    return s_never_uploaded || s_diff != 0;
}

void overlay_mark_uploaded(void)
{
    memcpy(s_uploaded, s_px, sizeof(s_uploaded));
    s_diff = 0;
    s_never_uploaded = 0;
}
