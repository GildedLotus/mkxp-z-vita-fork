// SPDX-License-Identifier: GPL-3.0-or-later
/* swraster: dependency-free software raster core for RGSS Bitmap operations.
 * Derived from mkxp-z (GPL-2.0-or-later).
 * CPU-authoritative pixel engine matching mkxp-z's GL shader semantics
 * C++17, standard library only: no SDL, no GL,
 * no mkxp-z headers. See README.md for exact semantics and references.
 *
 * Failure contract: an op that is empty or wholly clipped is a no-op, and
 * so is an op whose scratch-buffer allocation fails -- the entry point
 * returns with every pixel unchanged and no exception (std::bad_alloc
 * included) reaches the engine caller. bench stops
 * emitting instead of throwing.
 */
#ifndef SWRASTER_H
#define SWRASTER_H

#include <cstdint>

namespace swraster {

/* RGBA8888: bytes R,G,B,A in memory order (= SDL_PIXELFORMAT_ABGR8888 on
 * little-endian, mkxp-z's Bitmap format). NON-premultiplied alpha.
 * Row 0 is the top row. stride is in bytes and must be >= w*4; padding
 * bytes are never read or written. */
struct Surface {
    uint8_t *px;
    int w, h;
    int stride;
};

struct Rect {
    int x, y, w, h;
};

struct Color {
    uint8_t r, g, b, a;
};

/* REPLACE semantics (alpha is written, not blended) — mkxp fillRect is a
 * scissored glClear. Negative w/h are normalized (mirrored origin), matching
 * normalizedRect() in the GL path. Clipped to the surface; empty = no-op. */
void fill(const Surface &dst, Rect rect, Color c);

/* clear = fill with transparent black. */
void clear(const Surface &dst, Rect rect);

/* RGSS "over" blit, exactly shader/bitmapBlit.frag:
 *   co1 = src.a * opacity; co2 = dst.a * (1 - co1); out.a = co1 + co2;
 *   out.rgb = (out.a == 0) ? src.rgb : (co1*src.rgb + co2*dst.rgb) / out.a
 * (all in 0..1). opacity is clamped to 0..255; opacity == 0 is a no-op.
 *
 * dstRect.w/h != srcRect.w/h stretches. smooth=false: nearest. smooth=true:
 * bilinear with the GL_LINEAR/clamp-to-edge texel-centre convention
 * (sample at (i+0.5)*scale-0.5, clamped). 1:1 blits are always nearest.
 * Negative dstRect w/h mirror the image (mkxp stretchBlt mega path).
 * Source/destination rects may be partially or wholly outside their
 * surfaces: they are clipped and the other rect is shrunk proportionally
 * (mkxp shrinkRects). src and dst may alias (self-blt); overlap is handled
 * by snapshotting the clipped source region first (that snapshot is also
 * what makes the row kernel's __restrict sound -- see swraster.cpp).
 *
 * Build switch: -DSWRASTER_NO_SIMD selects the scalar kernels instead of
 * the NEON ones. Results are identical either way, byte for byte; the
 * scalar build is the reference for the NEON
 * build. Smooth blits use certified Q16 sampling on the NEON
 * dispatch, or the shipped double sampler with SWRASTER_NO_SIMD. */
void blit(const Surface &dst, Rect dstRect, const Surface &src, Rect srcRect,
          int opacity, bool smooth);

/* REPLACE linear gradient, GL path of Bitmap::gradientFillRect
 * (SimpleColorShader quad with per-vertex colours): per-channel (including
 * alpha) interpolation across pixel centres, t = (i+0.5)/n over the FULL
 * rect (clipping does not shift the gradient). rect.w<=0 or rect.h<=0 is a
 * no-op. */
void gradient_fill(const Surface &dst, Rect rect, Color c1, Color c2,
                   bool vertical);

/* shader/hue.frag: rgb2hsv/hsv2rgb as written there, hueAdjust =
 * wrapRange(degrees, 0, 360)/360.0 as used by Bitmap::hueChange
 * (bitmap.cpp:2949). Alpha untouched. degrees % 360 == 0 is a no-op. */
void hue_change(const Surface &dst, Rect rect, int degrees);

/* Bitmap::blur: one horizontal 3-tap box pass (x-1, x, x+1 averaged,
 * clamp-to-edge, all four channels) then one vertical 3-tap pass
 * (shader/blur.frag + blurH.vert/blurV.vert). Whole surface. */
void blur(const Surface &dst);

/* Bitmap::radialBlur (bitmap.cpp:2480-2575), CPU transcription of the GL
 * path. `divisions` rotations of the whole surface about its centre, evenly
 * spaced over [-angle/2, +angle/2] degrees, each sampled bilinearly
 * (GL_LINEAR + clamp-to-edge) and accumulated ADDITIVELY at 1/divisions
 * opacity:
 *   out.rgb += tex.rgb * tex.a / divisions;  out.a += tex.a / divisions
 * (shader/simpleAlpha.frag under BlendAddition = glBlendFuncSeparate(
 * GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE)). A translucent source therefore
 * darkens, exactly as stock does.
 *
 * The sampled plane is stock's 5-quad "cross": the source plus one mirrored
 * copy across each of its four edges. The four diagonal corners are not
 * covered; destination pixels that land there receive nothing for that
 * rotation, and a pixel outside the cross for every rotation ends up
 * transparent black, exactly as the freshly cleared GL target did.
 *
 * Whole surface, in place; every destination pixel is written. `angle` is
 * clamped to 0..359 and `divisions` to 2..100 (bitmap.cpp:2491-2492).
 * angle == 0 is the identity on an opaque surface. Within +-1 per channel of
 * a double evaluation of the formula above. */
void radial_blur(const Surface &dst, int angle, int divisions);

/* Bounds-checked pixel access. get out of bounds returns {0,0,0,0};
 * set out of bounds is a no-op; set is REPLACE. */
Color get_pixel(const Surface &surf, int x, int y);
void set_pixel(const Surface &surf, int x, int y, Color c);

/* Same "over" blend as blit with the whole source surface as source rect,
 * nearest. Separate entry point so the Bitmap integration can special-case
 * text composition later. No extra semantics. */
void composite_text(const Surface &dst, Rect dstRect, const Surface &src,
                    int opacity);

/* The window base compose's blend (shader/simpleAlpha.frag as
 * SoftBase::drawQuad evaluates it for an identity shade), 1:1 nearest
 * only -- the frame/border passes of both window types, and the VX tiled
 * background at back opacity 255 with no tone. Per pixel, all operands
 * bytes, with c1 = src.a * 255 and inv = 65025 - c1:
 *   out.rgb = (c1*src + inv*dst + 32512) / 65025
 *   out.a   = (c1*255 + inv*dst.a + 32512) / 65025   when !keepDestAlpha
 * Both are the round-half-up of the shader's src*sa + dst*(1-sa) resp.
 * sa + da*(1-sa); byte inputs reach no rounding tie, so this is exactly
 * what the engine's fixed-point compose writes. keepDestAlpha
 * is the BlendKeepDestAlpha mode: GL_ZERO,GL_ONE writes no alpha. A source
 * alpha of 0 leaves the destination unchanged and 255 replaces it, so no
 * caller-side special cases exist.
 *
 * dstRect and srcRect must be equal in w and h (the engine routes only
 * unmirrored native-size quads); a mismatch is a no-op, as is an empty
 * rect. Each rect clips to its own surface, then both trim to the common
 * size anchored at their top-left corners. src and dst may alias: the
 * source region is snapshotted first, as in blit. Not profile-counted:
 * the engine caller counts these quads itself (FrameProfile::Compose),
 * so the counters keep attributing only swraster-owned ops. */
void simple_blit(const Surface &dst, Rect dstRect, const Surface &src,
                 Rect srcRect, bool keepDestAlpha);

/* shader/kglSubtract.frag, mkxp's non-RGSS KGL_SUBTRACT blit mode
 * (Bitmap#kgl_subtract_rect):
 *   out.rgb = clamp(dst.rgb - factor*src.rgb, 0, 1);  out.a = 1
 * The source alpha is never read, and every destination pixel the blit
 * covers becomes opaque whatever it held.
 *
 * `opacity` is the RGSS 0..255 value; the factor is mkxp's bltNormOpacity
 * for this mode (bitmap.cpp:1706-1718), opacity/256 -- note 256, not 255 --
 * and exactly 1 at 255. opacity == 0 is NOT a no-op here: stock's early
 * return covers NORMAL only (bitmap.cpp:1791-1799), so the alpha write
 * still lands.
 *
 * Geometry is blit()'s, exactly: same shrinkRects clipping, same mirroring
 * on a negative source or destination rect, same stretch, same texel-centre
 * conventions (nearest, or bilinear GL_LINEAR/clamp-to-edge when `smooth`
 * and the blit scales -- stock gets that from TEX::setSmooth(true) at
 * bitmap.cpp:2137), same snapshot when src and dst alias. */
void subtract_blit(const Surface &dst, Rect dstRect, const Surface &src,
                   Rect srcRect, int opacity, bool smooth);

/* Frame-profiler counters. Dependency-free: the engine
 * installs a gate flag with profile_set_gate() (it passes the address of its
 * boot-cached frame-profile marker interval, so the marker stays the single
 * source of truth) and pulls the accumulated counters with profile_take()
 * at each frame-profile summary. A null gate, or *gate == 0, leaves the
 * counters inert: the increments below are guarded by one hoisted flag read
 * per call, placed after clipping at operation granularity -- once per call,
 * outside every pixel loop -- so the inner loops carry no counter code.
 * A -DSWRASTER_NO_PROFILE build compiles the counters out entirely.
 *
 * calls[]/pixels[] are exact per-operation totals: one call and its clipped
 * destination-pixel count per entry point, in ProfileOp order. No-op
 * early-outs (opacity-0 blits, empty or wholly clipped rects, out-of-bounds
 * pixel access, hue multiples of 360) return before the counter and are not
 * counted. composite_text delegates to blit() and is counted as ProfileBlit.
 * blit additionally classifies each call into the three destination-alpha classes by SAMPLING the clipped destination rect
 * at its four corners and centre: every sampled alpha 0 -> DstAlphaClear,
 * every one 255 -> DstAlphaOpaque, otherwise DstAlphaMixed (conservative:
 * any disagreement or in-between sample lands in the slow class). The class
 * of one call is therefore an estimate; the pixel totals are exact. Counters
 * only read pixels, never write them, so profiled output is byte-identical.
 * The RGSS thread is the only writer; no lock, allocation or kernel object.
 * bench() never installs a gate, so benchmark timings are unaffected. */
enum ProfileOp { ProfileFill, ProfileClear, ProfileBlit, ProfileGradientFill,
                 ProfileHueChange, ProfileBlur, ProfileRadialBlur,
                 ProfileGetPixel, ProfileSetPixel, ProfileSubtractBlit,
                 ProfileOpCount };
enum DstAlphaClass { DstAlphaClear, DstAlphaOpaque, DstAlphaMixed,
                     DstAlphaClassCount };
struct Profile {
    uint64_t calls[ProfileOpCount];
    uint64_t pixels[ProfileOpCount];
    uint64_t dst_alpha_calls[DstAlphaClassCount];
    uint64_t dst_alpha_pixels[DstAlphaClassCount];
};
void profile_set_gate(const unsigned *gate); /* null disables; read per call */
void profile_reset();                        /* discard accumulated counters */
void profile_take(Profile *out);             /* copy out, then clear */

/* Micro-benchmark entry point so these numbers can come from the Vita, not
 * the host. The host is not a proxy: it has hardware integer
 * divide and fast double divide, so it ranks these kernels differently --
 * in places in the opposite order. Calls emit once per op with a line:
 *   "swraster_bench <op> <ms_per_op> ms/op (<iters> iters, chk=0x........)"
 * The checksum covers the destination pixels so the work cannot be
 * optimised away. Timing is std::chrono::steady_clock, and every case keeps
 * its own setup out of the reported figure -- either by subtracting a
 * separately measured reset loop or by running the reset outside the timer.
 *
 * 24 ops, in this order (the list covers every class a
 * swraster optimisation can touch, so each has a "before" number):
 *   - RGSS-over 1:1 blits at the launcher and game sizes (544x416, 96x96, 24x24)
 *     over transparent and opaque destinations. The
 *     transparent cases re-clear before every timed blit and subtract the
 *     measured clear cost.
 *   - blit_544x416_over_translucent_da128_op160 and
 *     blit_544x416_over_mixed_alpha_op160: the 0 < dst.alpha < 255 blend
 *     class, the only branch of blendOver that divides by a runtime value,
 *     alone and interleaved with the other two.
 *   - blit_smooth_544x416_to_640x480_da{0,255,128}: the bilinear stretch.
 *   - blur_544x416, hue_512x512_deg30, hue_512x512_deg90,
 *     gradient_544x416_vertical, gradient_544x416_horizontal: the
 *     whole-surface effect kernels.
 *   - fill_544x416 and memcpy_544x416: controls,
 *     i.e. the memory floor every other figure should be read against.
 *   - the two radial_blur calls stock RGSS2/RGSS3 make at battle start
 *: 544x416 radial_blur(120, 16) (VX Ace) and 640x480
 *     radial_blur(90, 12) (VX), plus one non-opaque source so the general
 *     per-sample path is measured too.
 * Cases whose destination or source would drift out of its own class after
 * one run restore it between timed runs, outside the timer. */
void bench(void (*emit)(const char *line));

} // namespace swraster

#endif // SWRASTER_H
