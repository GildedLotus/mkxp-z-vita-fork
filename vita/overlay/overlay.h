// SPDX-License-Identifier: GPL-3.0-or-later
/* overlay: the persistent 512x128 HUD surface, composed entirely on the CPU.
 *
 * The Vita is a single-window platform with a sealed GPU budget: after boot
 * there is no second window, no SDL_Renderer, no new render surface and no
 * new shader program. Everything the player is shown
 * outside the game image -- the FPS line, the frame-profile HUD
 * and the settings menu -- therefore shares one
 * boot-reserved texture, and this is the buffer behind it (graphics.cpp owns
 * the GL side).
 *
 * Deliberately plain C with only standard C headers: no
 * GL, no SDL, no FreeType, no SDL_ttf, no allocation ever (the two buffers
 * are static), no file I/O, no floating point. That is what lets it draw when
 * the font stack is broken, when the RGSS thread is stalled, or before any
 * font has been registered -- exactly the moments something needs to say so.
 *
 * Not thread-safe and not meant to be: draw from the thread that owns the GL
 * context, in the same place the upload happens.
 *
 * For a modal panel with real (including Japanese) glyphs, wrapping and error
 * formatting, see vita/textpanel/ instead. This face is ASCII only.
 */
#ifndef VITA_OVERLAY_H
#define VITA_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One texture, 512 * 128 * 4 = 256 KiB. Both dimensions are powers of two so
 * the upload needs no NPOT support. */
#define OVERLAY_W 512
#define OVERLAY_H 128
#define OVERLAY_PIXELS (OVERLAY_W * OVERLAY_H)

/* Cell size and advance of the baked face in font8x8.h. 64 columns by 16
 * rows of text. */
#define OVERLAY_GLYPH_W 8
#define OVERLAY_GLYPH_H 8

/* Pixels are ARGB8888: a uint32_t holding 0xAARRGGBB, i.e. bytes B,G,R,A on
 * a little-endian machine (SDL_PIXELFORMAT_ARGB8888).
 *
 * NOTE for the GL side: that is NOT the byte order of a GL_RGBA upload, and
 * it is not swraster's RGBA8888 either. Two ways out, and the choice belongs
 * to upload as BGRA (the device does advertise
 * GL_EXT_texture_format_BGRA8888 and GL_IMG_texture_format_BGRA8888),
 * or run the buffer through
 * overlay_copy_rgba8888() into a staging buffer, which is a plain byte
 * shuffle and keeps the upload on the same GL_RGBA path as every other
 * texture in the engine. */
#define OVERLAY_ARGB(a, r, g, b)                  \
    (((uint32_t)(uint8_t)(a) << 24) |             \
     ((uint32_t)(uint8_t)(r) << 16) |             \
     ((uint32_t)(uint8_t)(g) << 8)  |             \
      (uint32_t)(uint8_t)(b))

#define OVERLAY_TRANSPARENT OVERLAY_ARGB(0, 0, 0, 0)
#define OVERLAY_WHITE       OVERLAY_ARGB(255, 255, 255, 255)
/* The HUD's own background: black at 3/4 opacity, legible over any game. */
#define OVERLAY_BACKDROP    OVERLAY_ARGB(192, 0, 0, 0)

/* Clear the surface and mark it as never uploaded. Safe to call more than
 * once; not required before the first draw (the buffers start zeroed), and
 * there is nothing to release afterwards. */
void overlay_init(void);

/* Every pixel to transparent black. */
void overlay_clear(void);

/* Filled rectangle, REPLACE (the colour's alpha is written, not blended),
 * clipped to the surface. A zero or negative w/h draws nothing. */
void overlay_box(int x, int y, int w, int h, uint32_t argb);

/* Draw one line of text with its top-left corner at (x, y) and return the pen
 * x the next glyph would use, so runs can be chained. The return value
 * saturates at INT_MAX when the advance is not representable.
 *
 * One byte is one glyph at OVERLAY_GLYPH_W pixels: no UTF-8 decoding, no
 * kerning, no wrapping, no newline handling. Every byte outside 0x20..0x7E --
 * including '\n' and '\t', and including each byte of a multi-byte UTF-8
 * sequence -- draws '?'. Callers split their own lines; vita/textpanel does
 * the real typography.
 *
 * Only the ink is written, so text composes over whatever is already there;
 * draw an overlay_box() first if it needs a background. text may be NULL. */
int overlay_text(int x, int y, const char *text);
int overlay_text_ex(int x, int y, const char *text, uint32_t argb);

/* Pixel width of what overlay_text would draw. */
int overlay_text_width(const char *text);

/* The live pixels, OVERLAY_H rows of OVERLAY_W packed ARGB8888 with no
 * padding. Never NULL, never reallocated: the address is stable for the whole
 * run, so the GL side may cache it. */
const uint32_t *overlay_pixels(void);

/* Convert the live pixels into a caller-owned RGBA8888 buffer (bytes R,G,B,A
 * -- GL_RGBA / GL_UNSIGNED_BYTE, and swraster's Surface layout). dstStride is
 * in bytes and must be at least OVERLAY_W * 4; padding bytes are not
 * touched. */
void overlay_copy_rgba8888(uint8_t *dst, int dstStride);

/* Non-zero while the live pixels differ from what was there at the last
 * overlay_mark_uploaded() -- which is the whole point of this module: the
 * upload is ~1.2 ms and must not happen on a frame that would re-send
 * identical bytes.
 *
 * It tracks *content*, not writes. Redrawing the same image pixel for pixel,
 * or clearing and rebuilding it from scratch, leaves the surface clean; only
 * a pixel whose final value differs from the uploaded one counts. Before the
 * first overlay_mark_uploaded() the surface always reports dirty, so the
 * initial whole-level upload happens even if the HUD is blank. */
int overlay_dirty(void);

/* Call immediately after uploading overlay_pixels(): the current content
 * becomes the reference overlay_dirty() compares against. */
void overlay_mark_uploaded(void);

#ifdef __cplusplus
}
#endif

#endif /* VITA_OVERLAY_H */
