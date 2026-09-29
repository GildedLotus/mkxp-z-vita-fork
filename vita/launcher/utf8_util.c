// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * utf8_util.c — see utf8_util.h.
 */
#include "utf8_util.h"

#include <string.h>

#define UTF8_REPLACEMENT "\xEF\xBF\xBD" /* U+FFFD, 3 bytes */

size_t utf8_seq_len(unsigned char lead)
{
    if (lead < 0x80)
        return 1;
    if (lead < 0xC2) /* 0x80..0xBF continuation, 0xC0/0xC1 overlong leads */
        return 0;
    if (lead < 0xE0)
        return 2;
    if (lead < 0xF0)
        return 3;
    if (lead < 0xF5)
        return 4;
    return 0; /* 0xF5..0xFF: beyond U+10FFFF */
}

size_t utf8_truncate_bytes(const char *in, char *out, size_t cap)
{
    size_t i = 0, used = 0;

    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!in)
        return 0;

    while (in[i]) {
        size_t n = utf8_seq_len((unsigned char)in[i]);

        if (n == 0)
            n = 1; /* not our problem here: copy the stray byte, keep moving */
        if (used + n + 1 > cap)
            break;
        /* A truncated tail sequence must not be copied half. */
        {
            size_t k;
            for (k = 0; k < n; k++) {
                if (in[i + k] == '\0')
                    return used; /* out is already NUL-terminated */
            }
        }
        memcpy(out + used, in + i, n);
        used += n;
        i += n;
        out[used] = '\0';
    }

    out[used] = '\0';
    return used;
}

/* Append one already-encoded sequence if it fits. Returns 0 on success. */
static int put_bytes(char *out, size_t cap, size_t *used, const char *seq,
                     size_t n)
{
    if (*used + n + 1 > cap)
        return -1;
    memcpy(out + *used, seq, n);
    *used += n;
    return 0;
}

size_t utf8_sanitize(const char *in, size_t in_len, char *out, size_t cap)
{
    size_t i = 0, used = 0;

    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!in)
        return 0;

    while (i < in_len) {
        unsigned char b = (unsigned char)in[i];
        size_t n, k;
        unsigned long cp;
        int bad = 0;

        if (b == '\0')
            break;

        if (b < 0x20 || b == 0x7F) {
            if (put_bytes(out, cap, &used, " ", 1) != 0)
                break;
            i++;
            continue;
        }
        if (b < 0x80) {
            if (put_bytes(out, cap, &used, (const char *)&b, 1) != 0)
                break;
            i++;
            continue;
        }

        n = utf8_seq_len(b);
        if (n == 0 || i + n > in_len) {
            bad = 1;
        } else {
            switch (n) {
            case 2:  cp = (unsigned long)(b & 0x1F); break;
            case 3:  cp = (unsigned long)(b & 0x0F); break;
            default: cp = (unsigned long)(b & 0x07); break;
            }
            for (k = 1; k < n; k++) {
                unsigned char c = (unsigned char)in[i + k];
                if ((c & 0xC0) != 0x80) {
                    bad = 1;
                    break;
                }
                cp = (cp << 6) | (unsigned long)(c & 0x3F);
            }
            if (!bad) {
                /*
                 * Overlong, surrogate, out of range. The 2-byte overlongs
                 * (C0/C1 leads) are already rejected by utf8_seq_len, so only
                 * the 3- and 4-byte forms need checking here.
                 */
                if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
                    cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                    bad = 1;
            }
        }

        if (bad) {
            if (put_bytes(out, cap, &used, UTF8_REPLACEMENT, 3) != 0)
                break;
            i++; /* resynchronise one byte at a time */
            continue;
        }

        if (cp >= 0x80 && cp <= 0x9F) { /* C1 controls */
            if (put_bytes(out, cap, &used, " ", 1) != 0)
                break;
            i += n;
            continue;
        }

        if (put_bytes(out, cap, &used, in + i, n) != 0)
            break;
        i += n;
    }

    out[used] = '\0';
    return used;
}

int utf8_truncate_to_width(const char *text, int max_w, Utf8MeasureFn measure,
                           void *ctx, const char *ellipsis, char *out,
                           size_t cap)
{
    size_t tlen, elen, boundary, best;
    int found;

    if (!text || !out || !measure || cap == 0)
        return -1;
    out[0] = '\0';
    if (!ellipsis)
        ellipsis = "";
    tlen = strlen(text);
    elen = strlen(ellipsis);

    /* Whole string, if it fits the width and the buffer. */
    if (tlen + 1 <= cap) {
        memcpy(out, text, tlen + 1);
        if (measure(out, ctx) <= max_w)
            return 0;
        out[0] = '\0';
    }

    /*
     * Walk code point boundaries in order and keep the last prefix whose
     * prefix+ellipsis still measures within max_w. measure() may be
     * non-monotonic across a single glyph (kerning), so nothing is
     * interpolated and no binary search is attempted. The candidate is built
     * in `out` itself, so the only size limit is the caller's cap.
     */
    best = 0;
    boundary = 0;
    found = 0;
    for (;;) {
        size_t n;

        if (boundary + elen + 1 <= cap) {
            memcpy(out, text, boundary);
            memcpy(out + boundary, ellipsis, elen);
            out[boundary + elen] = '\0';
            if (measure(out, ctx) <= max_w) {
                best = boundary;
                found = 1;
            }
        }

        if (boundary >= tlen)
            break;
        n = utf8_seq_len((unsigned char)text[boundary]);
        if (n == 0 || boundary + n > tlen)
            n = 1; /* stray byte: advance one, never past the terminator */
        boundary += n;
    }

    if (!found) {
        out[0] = '\0'; /* not even the ellipsis fits */
        return 1;
    }

    memcpy(out, text, best);
    memcpy(out + best, ellipsis, elen);
    out[best + elen] = '\0';
    return 1;
}
