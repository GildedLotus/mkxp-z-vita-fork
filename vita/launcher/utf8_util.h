// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * utf8_util.h — UTF-8 helpers for the Vita launcher.
 *
 * Two jobs, both needed before a single glyph is drawn:
 *
 *  1. Sanitising. Titles come out of Game.ini (CP932 or UTF-8) and folder
 *     names come out of readdir() on exFAT. Neither is trusted. A stray byte
 *     must not reach SDL_ttf as a malformed sequence, and a control byte must
 *     not reach the renderer as a zero-width box: invalid sequences become
 *     U+FFFD, control bytes become a space.
 *
 *  2. Fitting a title into a row. The launcher draws one 960x544 canvas and
 *     truncates titles to the row width. Cutting UTF-8 at a byte offset can
 *     split a code point; utf8_truncate_to_width() only ever cuts on a code
 *     point boundary, and takes the width measurement as a callback so this
 *     module needs no font, no SDL and no SDL_ttf.
 *
 * C99, libc only (string.h). No allocation, no globals, no I/O.
 */
#ifndef VITA_UTF8_UTIL_H
#define VITA_UTF8_UTIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Length in bytes of the UTF-8 sequence that starts with `lead`, or 0 if
 * `lead` cannot start one (a continuation byte or an invalid lead).
 */
size_t utf8_seq_len(unsigned char lead);

/*
 * Copy `in` to `out`, cutting only on a code point boundary.
 *
 * At most `cap` - 1 bytes are written plus the NUL. If the cut lands inside a
 * multi-byte sequence the whole sequence is dropped. `in` is assumed to be
 * valid UTF-8 already (run utf8_sanitize first if it is not); an invalid lead
 * byte is treated as one byte so the copy still terminates.
 *
 * Returns the number of bytes written (excluding the NUL), or 0 if `cap` is 0.
 */
size_t utf8_truncate_bytes(const char *in, char *out, size_t cap);

/*
 * Sanitise `in_len` bytes into `out`:
 *   - invalid, overlong, surrogate and out-of-range sequences -> U+FFFD
 *   - C0 controls (< 0x20), DEL (0x7F) and C1 controls (U+0080..U+009F)
 *     -> a space
 *   - an embedded NUL ends the input (INI values and dirent names are C
 *     strings; the length is a cap, not a promise)
 *
 * Output is always NUL-terminated when `cap` > 0 and never cut inside a code
 * point. Returns the number of bytes written (excluding the NUL).
 */
size_t utf8_sanitize(const char *in, size_t in_len, char *out, size_t cap);

/*
 * Width of a NUL-terminated UTF-8 string in whatever unit the caller measures
 * in (pixels for the launcher, code points in a host build). `ctx` is passed
 * through untouched. Must not be NULL.
 */
typedef int (*Utf8MeasureFn)(const char *utf8, void *ctx);

/*
 * Fit `text` into `max_w` units, appending `ellipsis` (may be NULL or "") when
 * it does not fit. Cuts only on code point boundaries.
 *
 * The largest prefix P of `text` for which measure(P + ellipsis) <= max_w is
 * chosen. `measure` is called O(code points) times and may be non-linear
 * (kerning, proportional fonts) — no width is ever assumed or interpolated.
 *
 * Returns 1 if the text had to be shortened, 0 if it was copied whole, -1 on a
 * bad argument (NULL text/out/measure, cap == 0). `out` is NUL-terminated on
 * every non-negative return, and empty when not even the ellipsis fits.
 */
int utf8_truncate_to_width(const char *text, int max_w, Utf8MeasureFn measure,
                           void *ctx, const char *ellipsis, char *out,
                           size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* VITA_UTF8_UTIL_H */
