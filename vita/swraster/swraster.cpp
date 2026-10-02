// SPDX-License-Identifier: GPL-3.0-or-later
/* swraster: dependency-free software raster core for RGSS Bitmap operations.
 * See swraster.h for the API contract and README.md for semantics,
 * references, and known deviations.
 *
 * Derived from mkxp-z (GPL-2.0-or-later); reference: stock mkxp-z (src/display/bitmap.cpp, shader dir fragment shaders).
 * Clipping is a verbatim port of mkxp's shrinkRects (bitmap.cpp:1630-1704).
 */
#include "swraster.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

/* NEON kernels are compiled wherever the compiler advertises NEON (the Vita
 * builds with -mfpu=neon; arm64 hosts advertise it too, so host builds
 * exercise them as well).
 * -DSWRASTER_NO_SIMD selects the scalar reference instead, which every SIMD
 * kernel must match byte for byte. */
#if defined(__ARM_NEON) && !defined(SWRASTER_NO_SIMD)
#define SWR_NEON 1
#include <arm_neon.h>
#else
#define SWR_NEON 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#define SWR_INLINE inline __attribute__((always_inline))
#define SWR_RESTRICT __restrict
#else
#define SWR_INLINE inline
#define SWR_RESTRICT
#endif

namespace swraster {

/* ------------------------------------------------------------------ */
/* Frame-profiler counters. See swraster.h for the */
/* contract: one increment per call, after clipping, outside every     */
/* pixel loop. noinline+cold keeps the counting path out of the hot    */
/* functions' register allocation so the inner loops disassemble       */
/* identically with and without counters.                              */
/* ------------------------------------------------------------------ */

#if !defined(SWRASTER_NO_PROFILE)
static const unsigned *g_profileGate = 0;
static Profile g_profile;

static inline bool profileActive() { return g_profileGate && *g_profileGate; }

__attribute__((noinline, cold))
static void profileCount(int op, uint64_t pixels)
{
    ++g_profile.calls[op];
    g_profile.pixels[op] += pixels;
}

__attribute__((noinline, cold))
static void profileBlend(const Surface &dst, const Rect &rect, int op)
{
    profileCount(op, (uint64_t)(unsigned)rect.w * (uint64_t)(unsigned)rect.h);
    /* Destination-alpha class by corners-and-centre sample (swraster.h):
     * conservative toward DstAlphaMixed, the slow per-pixel class. */
    const int x1 = rect.x + rect.w - 1, xc = rect.x + rect.w / 2;
    const int y1 = rect.y + rect.h - 1, yc = rect.y + rect.h / 2;
    bool zero = true, full = true;
    for (int i = 0; i < 5; ++i) {
        const int x = i == 1 || i == 3 ? x1 : (i == 4 ? xc : rect.x);
        const int y = i == 2 || i == 3 ? y1 : (i == 4 ? yc : rect.y);
        const uint8_t a =
            dst.px[(size_t)y * (size_t)dst.stride + (size_t)x * 4 + 3];
        zero = zero && a == 0;
        full = full && a == 255;
        if (!zero && !full)
            break;
    }
    const int cls = zero ? DstAlphaClear : (full ? DstAlphaOpaque
                                                 : DstAlphaMixed);
    ++g_profile.dst_alpha_calls[cls];
    g_profile.dst_alpha_pixels[cls] +=
        (uint64_t)(unsigned)rect.w * (uint64_t)(unsigned)rect.h;
}
#else
static inline bool profileActive() { return false; }
static void profileCount(int, uint64_t) {}
static void profileBlend(const Surface &, const Rect &, int) {}
#endif

void profile_set_gate(const unsigned *gate)
{
#if !defined(SWRASTER_NO_PROFILE)
    g_profileGate = gate;
#else
    (void)gate;
#endif
}

void profile_reset()
{
#if !defined(SWRASTER_NO_PROFILE)
    g_profile = Profile{};
#endif
}

void profile_take(Profile *out)
{
#if !defined(SWRASTER_NO_PROFILE)
    *out = g_profile;
    g_profile = Profile{};
#else
    *out = Profile{};
#endif
}

namespace {

/* ------------------------------------------------------------------ */
/* Rect helpers                                                        */
/* ------------------------------------------------------------------ */

/* Returns false when empty. Normalize before clipping in widened arithmetic:
 * an INT_MIN extent or a mirrored origin need not fit in Rect until clipped. */
bool clipRect(const Surface &s, Rect &r, bool normalize = false)
{
    int64_t x1 = r.x, y1 = r.y;
    int64_t x2 = x1 + r.w, y2 = y1 + r.h;
    if (normalize) {
        if (x2 < x1) std::swap(x1, x2);
        if (y2 < y1) std::swap(y1, y2);
    }
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > s.w) x2 = s.w;
    if (y2 > s.h) y2 = s.h;
    if (x2 <= x1 || y2 <= y1)
        return false;
    r.x = (int)x1;
    r.y = (int)y1;
    r.w = (int)(x2 - x1);
    r.h = (int)(y2 - y1);
    return true;
}

/* ------------------------------------------------------------------ */
/* mkxp shrinkRects (bitmap.cpp:1630-1704), verbatim port.             */
/* Clips the source rect to the source bitmap and shrinks the          */
/* destination rect proportionally (and vice versa via the swapped     */
/* second call). Returns true when the blit degenerates to a no-op.    */
/* ------------------------------------------------------------------ */

bool shrinkRectsF(float &sourcePos, float &sourceLen, const int &sBitmapLen,
                  float &destPos, float &destLen, const int &dBitmapLen,
                  bool normalize = false)
{
    (void)dBitmapLen; /* unused, kept to match the mkxp signature */
    float sStart = sourceLen > 0 ? sourcePos : sourceLen + sourcePos;
    float sEnd = sourceLen > 0 ? sourceLen + sourcePos : sourcePos;
    float sLength = sEnd - sStart;

    if (sStart >= 0 && sEnd < sBitmapLen)
        return false;

    if (sStart >= sBitmapLen || sEnd < 0)
        return true;

    float dStart = destLen > 0 ? destPos : destLen + destPos;
    float dEnd = destLen > 0 ? destLen + destPos : destPos;
    float dLength = dEnd - dStart;

    float delta = sEnd - sBitmapLen;
    float dDelta;
    if (delta > 0) {
        dDelta = (delta / sLength) * dLength;
        sLength -= delta;
        sEnd = sBitmapLen;
        dEnd -= dDelta;
        dLength -= dDelta;
    }
    if (sStart < 0) {
        dDelta = (sStart / sLength) * dLength;
        sLength += sStart;
        sStart = 0;
        dStart -= dDelta;
        dLength += dDelta;
    }

    if (!normalize) {
        sourcePos = sourceLen > 0 ? sStart : sEnd;
        sourceLen = sourceLen > 0 ? sLength : -sLength;
        destPos = destLen > 0 ? dStart : dEnd;
        destLen = destLen > 0 ? dLength : -dLength;
    } else {
        /* Ensure the source rect has positive dimensions, for blitting
         * from mega surfaces */
        destPos = ((destLen > 0) == (sourceLen > 0)) ? dStart : dEnd;
        destLen = ((destLen > 0) == (sourceLen > 0)) ? dLength : -dLength;
        sourcePos = sStart;
        sourceLen = sLength;
    }

    return false;
}

bool shrinkRects(int &sourcePos, int &sourceLen, const int &sBitmapLen,
                 int &destPos, int &destLen, const int &dBitmapLen)
{
    float fSourcePos = (float)sourcePos;
    float fSourceLen = (float)sourceLen;
    float fDestPos = (float)destPos;
    float fDestLen = (float)destLen;

    bool ret = shrinkRectsF(fSourcePos, fSourceLen, sBitmapLen,
                            fDestPos, fDestLen, dBitmapLen, true);

    if (!ret)
        ret = shrinkRectsF(fDestPos, fDestLen, dBitmapLen,
                           fSourcePos, fSourceLen, sBitmapLen);

    sourcePos = (int)std::lround(fSourcePos);
    sourceLen = (int)std::lround(fSourceLen);
    destPos = (int)std::lround(fDestPos);
    destLen = (int)std::lround(fDestLen);

    return ret || sourceLen == 0 || destLen == 0;
}

/* ------------------------------------------------------------------ */
/* RGSS "over" blend (shader/bitmapBlit.frag) in integer math.         */
/*                                                                     */
/* With c1 = sa*op (0..65025) and A = c1*255 + da*(65025-c1):          */
/*   out.a   = A / 65025                                               */
/*   out.rgb = (c1*s*255 + da*(65025-c1)*d) / A          (A > 0)       */
/*   A == 0  = shader's resFrag.a == 0 case: out = (src.rgb, 0)        */
/* N <= 255*A <= 4228250625 fits uint32, so no overflow. Exact when    */
/* c1 == 0 (dst unchanged, or (src.rgb,0) when da == 0) and when       */
/* c1 == 65025 (dst = src).                                            */
/* ------------------------------------------------------------------ */

/* The three destination-alpha classes as separate always-inline kernels
 * Together they are the pre-blendOver, arithmetic
 * unchanged; splitting them lets a caller that already knows the class
 * skip both the test and the call. No-divide fast paths for two of the
 * three (Cortex-A9 has no hardware divide; only a / by a runtime value is
 * an __aeabi_uidiv library call -- a constant divisor is a shift or a
 * reciprocal multiply), both bit-identical to the generic formula
 * The third takes a table-seeded reciprocal once per pixel and multiplies:
 * no divide at all. */

/* da == 0: A = c1*255 divides N = c1*s*255 exactly, so out.rgb = src.rgb
 * and out.a = round(c1/255). c1 == 0 is the A == 0 case, and the same
 * expression yields it: (0 + 32512)/65025 == 0. */
SWR_INLINE void blendClearDst(uint8_t *d, const uint8_t *s, uint32_t c1)
{
    d[0] = s[0];
    d[1] = s[1];
    d[2] = s[2];
    d[3] = (uint8_t)((c1 * 255 + 32512) / 65025);
}

/* da == 255, c1 > 0: A = 255*65025, a compile-time constant, so the
 * division becomes a reciprocal multiply. 255 factors out of N, and the
 * A/2 rounding survives: (255*M + 8290687)/16581375 == (M + 32512)/65025
 * for M = c1*s + (65025-c1)*d, because 8290687 = 255*32512 + 127 and
 * 255*65024 + 127 < 16581375. out.a = (16581375 + 32512)/65025 = 255, so
 * the destination alpha is already correct and is not rewritten. */
SWR_INLINE void blendOpaqueDst(uint8_t *d, const uint8_t *s, uint32_t c1)
{
    const uint32_t inv = 65025 - c1;
    d[0] = (uint8_t)((c1 * s[0] + inv * d[0] + 32512) / 65025);
    d[1] = (uint8_t)((c1 * s[1] + inv * d[1] + 32512) / 65025);
    d[2] = (uint8_t)((c1 * s[2] + inv * d[2] + 32512) / 65025);
}

/* 2^47 / A for a runtime A, division-free, never above the true value and
 * within 1/257 + 2^-22 of it (relative). A is normalised to a in
 * [2^31, 2^32) by its leading zeros (8..16 for the reachable A), and its top
 * nine bits pick the entry floor(2^40 / (257+i)), the reciprocal of the
 * upper end of a's bin, so the estimate is low by less than one bin. */
struct RecipSeed {
    uint32_t v[256];
    constexpr RecipSeed() : v()
    {
        for (uint32_t i = 0; i < 256; ++i)
            v[i] = (uint32_t)((uint64_t(1) << 40) / (257 + i));
    }
};
constexpr RecipSeed kRecipSeed;

SWR_INLINE uint32_t reciprocal47(uint32_t A)
{
    const int s = __builtin_clz(A);
    return kRecipSeed.v[((A << s) >> 23) - 256] >> (16 - s);
}

/* n / A for a runtime A from R = reciprocal47(A). Exact for every A and n
 * the caller below can produce, i.e. for 65279 <= A <= 16581375 and
 * 0 <= n <= 255*A + A/2. With q = n / A and R <= 2^47/A,
 *
 *   est = (n*R) >> 47 <= q, and q - est <= 1 because
 *   n/A - n*R/2^47 = n*E/(A*2^47) < 1 for E = 2^47 - A*R,
 *
 * so n - est*A is q's remainder or that remainder plus A: one
 * compare-and-add finishes it. Nothing overflows: R < 2^32 (A >= 65279),
 * n*R < 255.5*2^47, est*A <= n < 2^32.
 *
 * The condition n_max*E < A*2^47 is the one step that cannot be read off by
 * eye; it was checked for EVERY A in the range, together with mutations of
 * the seed, the shift and the correction to prove the check bites, which
 * with the argument above proves the quotient exact for every pixel input. */
SWR_INLINE uint32_t divByRecip(uint32_t n, uint32_t A, uint32_t R)
{
    const uint32_t est = (uint32_t)(((uint64_t)n * R) >> 47);
    return est + (uint32_t)((n - est * A) >= A);
}

/* 0 < da < 255, c1 > 0: the generic formula. The three channels share one
 * divisor, so it is turned into a reciprocal once per pixel without a
 * divide (Cortex-A9 has no hardware divide: a runtime / is an
 * __aeabi_uidiv call). Bit-identical to the original three-division
 * kernel: see divByRecip. The other / below have compile-time divisors. */
SWR_INLINE void blendTranslucentDst(uint8_t *d, const uint8_t *s, uint32_t c1,
                                    uint32_t da)
{
    const uint32_t k = da * (65025 - c1);
    const uint32_t A = c1 * 255 + k;
    const uint32_t half = A / 2;
    const uint32_t R = reciprocal47(A);
    d[3] = (uint8_t)((A + 32512) / 65025);
    d[0] = (uint8_t)divByRecip(c1 * s[0] * 255 + k * d[0] + half, A, R);
    d[1] = (uint8_t)divByRecip(c1 * s[1] * 255 + k * d[1] + half, A, R);
    d[2] = (uint8_t)divByRecip(c1 * s[2] * 255 + k * d[2] + half, A, R);
}

/* One pixel, any class. Same results as the pre-blendOver.
 * c1 == 65025 (sa == op == 255) yields src exactly in every class --
 * blendTranslucentDst: k == 0, A == 255*65025, so each channel is
 * (s*A + A/2)/A == s and alpha (A + 32512)/65025 == 255 -- so it is a
 * copy. The forward row kernel already copies such runs; this spares the
 * mirrored and stretched loops the reciprocal. */
SWR_INLINE void blendOver(uint8_t *d, const uint8_t *s, uint32_t op)
{
    const uint32_t da = d[3];
    const uint32_t c1 = (uint32_t)s[3] * op; /* 0..65025 */
    if (c1 == 65025)
        std::memcpy(d, s, 4);
    else if (da == 0)
        blendClearDst(d, s, c1);
    else if (c1 == 0)
        return; /* sa == 0, da > 0: dst unchanged */
    else if (da == 255)
        blendOpaqueDst(d, s, c1);
    else
        blendTranslucentDst(d, s, c1, da);
}

#if SWR_NEON
/* ceil(2^46 / 65025). x / 65025 == ((2*x*M) >> 32) >> 15 for every x the
 * opaque-destination numerator can take, i.e. x <= 255*65025 + 32512;
 * checked exhaustively over that range. The obvious constant,
 * 1082163329, is wrong. */
const int32_t kDiv65025Magic = 1082179842;

/* Eight lanes of x / 65025, x in two uint32x4_t halves. vqdmulhq_s32 is
 * (2*a*b) >> 32 with saturation; nothing here saturates (x < 2^31 and
 * 2*x*M >> 32 < 2^24). */
SWR_INLINE uint16x8_t neonDiv65025(uint32x4_t lo, uint32x4_t hi, int32x4_t magic)
{
    int32x4_t qlo = vshrq_n_s32(vqdmulhq_s32(vreinterpretq_s32_u32(lo), magic), 15);
    int32x4_t qhi = vshrq_n_s32(vqdmulhq_s32(vreinterpretq_s32_u32(hi), magic), 15);
    return vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(qlo)),
                        vmovn_u32(vreinterpretq_u32_s32(qhi)));
}

/* True when all 8 lanes equal value. One vceq plus one 64-bit compare. */
SWR_INLINE bool neonAllEq(uint8x8_t v, uint8_t value)
{
    uint64x1_t m = vreinterpret_u64_u8(vceq_u8(v, vdup_n_u8(value)));
    return vget_lane_u64(m, 0) == ~(uint64_t)0;
}

/* 8 pixels, every destination alpha == 255. Bit-identical to
 * blendOpaqueDst per pixel, and c1 == 0 falls out of the formula
 * (inv == 65025, so d stays d) instead of needing the early return. */
SWR_INLINE void neonOpaqueDst8(uint8_t *d, const uint8x8x4_t &S, uint8x8x4_t D,
                               uint8x8_t vop, int32x4_t magic)
{
    const uint16x8_t c1 = vmull_u8(S.val[3], vop);
    const uint16x8_t inv = vsubq_u16(vdupq_n_u16(65025), c1);
    const uint32x4_t half = vdupq_n_u32(32512);
    for (int ch = 0; ch < 3; ++ch) {
        const uint16x8_t s16 = vmovl_u8(S.val[ch]);
        const uint16x8_t d16 = vmovl_u8(D.val[ch]);
        uint32x4_t lo = vmull_u16(vget_low_u16(c1), vget_low_u16(s16));
        uint32x4_t hi = vmull_u16(vget_high_u16(c1), vget_high_u16(s16));
        lo = vmlal_u16(lo, vget_low_u16(inv), vget_low_u16(d16));
        hi = vmlal_u16(hi, vget_high_u16(inv), vget_high_u16(d16));
        lo = vaddq_u32(lo, half);
        hi = vaddq_u32(hi, half);
        D.val[ch] = vmovn_u16(neonDiv65025(lo, hi, magic));
    }
    vst4_u8(d, D);
}

/* 8 pixels, every destination alpha == 255 AND every source alpha == 255:
 * c1 == 255*op, so the whole blend collapses to
 * (op*s + (255-op)*d + 128) round-divided by 255 in 16-bit lanes, with no
 * reciprocal at all. Exhaustively equal to the 65025 formula for every
 * op, s and d (identity 2 of the exhaustive host proof). */
SWR_INLINE void neonOpaqueDstOpaqueSrc8(uint8_t *d, const uint8x8x4_t &S,
                                        uint8x8x4_t D, uint8x8_t vop,
                                        uint8x8_t viop)
{
    const uint16x8_t k128 = vdupq_n_u16(128);
    for (int ch = 0; ch < 3; ++ch) {
        uint16x8_t u = vmull_u8(S.val[ch], vop);
        u = vmlal_u8(u, D.val[ch], viop);
        u = vaddq_u16(u, k128);
        D.val[ch] = vaddhn_u16(u, vshrq_n_u16(u, 8));
    }
    vst4_u8(d, D);
}

/* 8 pixels, every destination alpha == 0: rgb = src.rgb and
 * a = (u + (u>>8)) >> 8 for u = sa*op + 128, which equals
 * (c1*255 + 32512)/65025 for every c1 (identity 1). */
SWR_INLINE void neonClearDst8(uint8_t *d, uint8x8x4_t S, uint8x8_t vop)
{
    uint16x8_t u = vaddq_u16(vmull_u8(S.val[3], vop), vdupq_n_u16(128));
    S.val[3] = vaddhn_u16(u, vshrq_n_u16(u, 8));
    vst4_u8(d, S);
}

/* 8 pixels of simple_blit (the window compose's simpleAlpha blend).
 * rgb is neonOpaqueDst8's arithmetic unchanged -- the simple
 * blend weights the destination by (1 - sa) alone, never by the output
 * alpha, so the destination alpha does not enter it. Normal alpha is the
 * same round-trip with the destination alpha as the second operand:
 * c1*255 + inv*da == 255*(255*sa + (255-sa)*da) <= 255*65025, so both
 * numerators stay inside the domain kDiv65025Magic is proven over
 * sa == 0 and sa == 255 lanes fall out of
 * the formula (dst unchanged resp. src written), like the scalar kernel. */
SWR_INLINE void neonSimple8(uint8_t *d, const uint8x8x4_t &S, uint8x8x4_t D,
                            int32x4_t magic, bool keepDest)
{
    const uint16x8_t c1 = vmull_u8(S.val[3], vdup_n_u8(255));
    const uint16x8_t inv = vsubq_u16(vdupq_n_u16(65025), c1);
    const uint32x4_t half = vdupq_n_u32(32512);
    for (int ch = 0; ch < 3; ++ch) {
        const uint16x8_t s16 = vmovl_u8(S.val[ch]);
        const uint16x8_t d16 = vmovl_u8(D.val[ch]);
        uint32x4_t lo = vmull_u16(vget_low_u16(c1), vget_low_u16(s16));
        uint32x4_t hi = vmull_u16(vget_high_u16(c1), vget_high_u16(s16));
        lo = vmlal_u16(lo, vget_low_u16(inv), vget_low_u16(d16));
        hi = vmlal_u16(hi, vget_high_u16(inv), vget_high_u16(d16));
        lo = vaddq_u32(lo, half);
        hi = vaddq_u32(hi, half);
        D.val[ch] = vmovn_u16(neonDiv65025(lo, hi, magic));
    }
    if (!keepDest) {
        const uint16x4_t k255 = vdup_n_u16(255);
        const uint16x8_t da = vmovl_u8(D.val[3]);
        uint32x4_t lo = vmull_u16(vget_low_u16(c1), k255);
        uint32x4_t hi = vmull_u16(vget_high_u16(c1), k255);
        lo = vmlal_u16(lo, vget_low_u16(inv), vget_low_u16(da));
        hi = vmlal_u16(hi, vget_high_u16(inv), vget_high_u16(da));
        lo = vaddq_u32(lo, half);
        hi = vaddq_u32(hi, half);
        D.val[3] = vmovn_u16(neonDiv65025(lo, hi, magic));
    }
    vst4_u8(d, D);
}
#endif /* SWR_NEON */

/* A whole row, source walking forwards 1:1 -- the dominant blit case, and
 * the one every RGSS Bitmap#blt at native size takes. The
 * destination-alpha class is decided per 8-pixel block (NEON) or per run
 * (scalar) instead of per pixel, so the inner loops carry neither a class
 * test nor a call.
 *
 * SWR_RESTRICT is sound only because blit() snapshots an overlapping
 * source into a private buffer before the row loop (see rangesOverlap
 * there): d and s therefore never alias. Keep that snapshot. */
void blendRowForward(uint8_t *SWR_RESTRICT d, const uint8_t *SWR_RESTRICT s,
                     int n, uint32_t op)
{
    int i = 0;
#if SWR_NEON
    const uint8x8_t vop = vdup_n_u8((uint8_t)op);
    const uint8x8_t viop = vdup_n_u8((uint8_t)(255 - op));
    const int32x4_t magic = vdupq_n_s32(kDiv65025Magic);
    for (; i + 8 <= n; i += 8) {
        uint8_t *dp = d + (size_t)i * 4;
        const uint8_t *sp = s + (size_t)i * 4;
        const uint8x8x4_t S = vld4_u8(sp);
        if (op == 255 && neonAllEq(S.val[3], 255)) {
            /* c1 == 65025 for all 8: out == src whatever the destination
             * holds, in every one of the three classes. */
            std::memcpy(dp, sp, 32);
            continue;
        }
        const uint8x8x4_t D = vld4_u8(dp);
        if (neonAllEq(D.val[3], 0)) {
            neonClearDst8(dp, S, vop);
        } else if (neonAllEq(D.val[3], 255)) {
            if (neonAllEq(S.val[3], 255))
                neonOpaqueDstOpaqueSrc8(dp, S, D, vop, viop);
            else
                neonOpaqueDst8(dp, S, D, vop, magic);
        } else {
            for (int k = 0; k < 8; ++k)
                blendOver(dp + k * 4, sp + k * 4, op);
        }
    }
#else
    /* Scalar reference: destination-alpha runs. At full opacity a run of
     * opaque source pixels is a memcpy whatever the destination holds
     * (c1 == 65025 -> out == src), which is the pre-op == 255 fast
     * path, now reached from the same kernel. */
    while (i < n) {
        if (op == 255 && s[(size_t)i * 4 + 3] == 255) {
            int j = i + 1;
            while (j < n && s[(size_t)j * 4 + 3] == 255)
                ++j;
            std::memcpy(d + (size_t)i * 4, s + (size_t)i * 4,
                        (size_t)(j - i) * 4);
            i = j;
            continue;
        }
        const uint32_t da = d[(size_t)i * 4 + 3];
        if (da == 255) {
            int j = i + 1;
            while (j < n && d[(size_t)j * 4 + 3] == 255)
                ++j;
            for (; i < j; ++i) {
                const uint32_t c1 = (uint32_t)s[(size_t)i * 4 + 3] * op;
                if (c1)
                    blendOpaqueDst(d + (size_t)i * 4, s + (size_t)i * 4, c1);
            }
        } else if (da == 0) {
            int j = i + 1;
            while (j < n && d[(size_t)j * 4 + 3] == 0)
                ++j;
            for (; i < j; ++i)
                blendClearDst(d + (size_t)i * 4, s + (size_t)i * 4,
                              (uint32_t)s[(size_t)i * 4 + 3] * op);
        } else {
            const uint32_t c1 = (uint32_t)s[(size_t)i * 4 + 3] * op;
            if (c1)
                blendTranslucentDst(d + (size_t)i * 4, s + (size_t)i * 4, c1,
                                    da);
            ++i;
        }
    }
#endif
    /* NEON tail (the scalar loop above already ran to n). */
    for (; i < n; ++i)
        blendOver(d + (size_t)i * 4, s + (size_t)i * 4, op);
}

/* One row of simple_blit, source walking forwards 1:1 -- the window
 * compose's simpleAlpha blend, scalar reference build. The
 * destination-alpha classes of blendRowForward do not exist here: rgb
 * never reads them, so runs key on the SOURCE alpha. sa == 0 leaves the
 * pixel unchanged (Windowskin frames are mostly transparent) and sa == 255
 * writes src.rgb (plus alpha 255 unless keepDest); the 0 < sa < 255 lanes
 * pay the round-half-up with its constant divisor. SWR_RESTRICT is sound
 * for the same reason blendRowForward's is: simple_blit snapshots an
 * overlapping source. */
void simpleRowForward(uint8_t *SWR_RESTRICT d, const uint8_t *SWR_RESTRICT s,
                      int n, bool keepDest)
{
    int i = 0;
#if SWR_NEON
    const int32x4_t magic = vdupq_n_s32(kDiv65025Magic);
    for (; i + 8 <= n; i += 8) {
        uint8_t *dp = d + (size_t)i * 4;
        const uint8_t *sp = s + (size_t)i * 4;
        const uint8x8x4_t S = vld4_u8(sp);
        if (neonAllEq(S.val[3], 0))
            continue; /* sa == 0 for all 8: the destination is untouched */
        if (neonAllEq(S.val[3], 255)) {
            /* rgb = src.rgb; alpha 255 unless keepDest, which keeps the
             * loaded destination alpha. */
            if (keepDest) {
                uint8x8x4_t D = vld4_u8(dp);
                D.val[0] = S.val[0];
                D.val[1] = S.val[1];
                D.val[2] = S.val[2];
                vst4_u8(dp, D);
            } else {
                std::memcpy(dp, sp, 32);
            }
            continue;
        }
        const uint8x8x4_t D = vld4_u8(dp);
        neonSimple8(dp, S, D, magic, keepDest);
    }
#endif
    for (; i < n; ++i) {
        uint8_t *d4 = d + (size_t)i * 4;
        const uint8_t *s4 = s + (size_t)i * 4;
        const uint32_t sa = s4[3];
        if (sa == 0)
            continue;
        if (sa == 255) {
            d4[0] = s4[0];
            d4[1] = s4[1];
            d4[2] = s4[2];
            if (!keepDest)
                d4[3] = 255;
            continue;
        }
        const uint32_t c1 = sa * 255;
        const uint32_t inv = 65025 - c1;
        d4[0] = (uint8_t)((c1 * s4[0] + inv * d4[0] + 32512) / 65025);
        d4[1] = (uint8_t)((c1 * s4[1] + inv * d4[1] + 32512) / 65025);
        d4[2] = (uint8_t)((c1 * s4[2] + inv * d4[2] + 32512) / 65025);
        if (!keepDest)
            d4[3] = (uint8_t)((c1 * 255 + inv * d4[3] + 32512) / 65025);
    }
}

/* Shipped double blend, verbatim: the reference for blendOverD below and
 * its fallback for pixels that fast path cannot certify. */
__attribute__((noinline))
void blendOverDExact(uint8_t *d, const double *s, uint32_t op)
{
    double co1 = (s[3] * (double)op) / 65025.0;
    double co2 = ((double)d[3] / 255.0) * (1.0 - co1);
    double outa = co1 + co2;
    if (outa == 0.0) {
        d[0] = (uint8_t)std::floor(s[0] + 0.5);
        d[1] = (uint8_t)std::floor(s[1] + 0.5);
        d[2] = (uint8_t)std::floor(s[2] + 0.5);
        d[3] = 0;
        return;
    }
    d[3] = (uint8_t)std::floor(outa * 255.0 + 0.5);
    for (int ch = 0; ch < 3; ++ch) {
        /* s and d both in 0..255 scale: out = (co1*s + co2*d)/outa */
        double v = (co1 * s[ch] + co2 * (double)d[ch]) / outa;
        d[ch] = (uint8_t)std::floor(v + 0.5);
    }
}

/* True when t = v + 0.5 lies at least 2^-20 inside one integer bin, so
 * (int)t == floor(t) is the same byte for any evaluation of v within 2^-20. */
SWR_INLINE bool binCertified(double t)
{
    return std::fabs(t - (double)(int)t - 0.5) < 0.5 - 1.0 / 1048576.0;
}

/* One byte of blendOverDExact for d[3] == 0, sop > 0: alpha when `alpha`,
 * else the channel of sample v. Both are >= 0, so (int) is the floor. */
__attribute__((noinline))
uint8_t blendClearExact(double sop, double v, bool alpha)
{
    const double co1 = sop / 65025.0;
    return (uint8_t)(int)(alpha ? co1 * 255.0 + 0.5 : co1 * v / co1 + 0.5);
}

/* blendOverD for d[3] == 0, sop > 0. co2 is then a signed
 * zero, so the exact blend is outa = co1, alpha floor(co1*255 + 0.5) and
 * channel floor(co1*s/co1 + 0.5), each within 2^-44 of sop/255 or s plus
 * 0.5. The Q16 sampler only sends near-tie pixels here, which blendOverD's
 * general certification always rejects, so round each byte alone and divide
 * only for a tie. Out of line to keep the translucent path's code small. */
__attribute__((noinline))
void blendOverDClear(uint8_t *d, const double *s, double sop)
{
    const double ta = sop * (1.0 / 255.0) + 0.5;
    d[3] = binCertified(ta) ? (uint8_t)(int)ta : blendClearExact(sop, 0.0, true);
    for (int ch = 0; ch < 3; ++ch) {
        const double t = s[ch] + 0.5;
        d[ch] = binCertified(t) ? (uint8_t)(int)t : blendClearExact(sop, s[ch], false);
    }
}

/* blendOverDExact without its per-pixel divides and floor() calls:
 * the two constant divisors become multiplies by their reciprocals and the
 * three channel quotients share one reciprocal of outa. Every quantity is
 * >= 0 up to a cancellation in 1 - co1 that only appears where outa ~ 1,
 * so this and the exact path are each within ~2^-38 of the real-number
 * result, four orders below the 2^-20 bin margin. A pixel whose four results
 * are all inside a bin therefore rounds to the same bytes in both; any pixel
 * near a rounding boundary, which includes every exact tie the shipped
 * double math resolves one way or the other, takes the exact path.
 * outa == 0 needs no division to detect: co1 is zero only for
 * s[3]*op == 0, and then co2 is zero only for d[3] == 0. */
void blendOverD(uint8_t *d, const double *s, uint32_t op)
{
    const double sop = s[3] * (double)op;
    if (d[3] == 0) {
        if (sop == 0.0)
            blendOverDExact(d, s, op);
        else
            blendOverDClear(d, s, sop);
        return;
    }
    const double co1 = sop * (1.0 / 65025.0);
    const double co2 = ((double)d[3] * (1.0 / 255.0)) * (1.0 - co1);
    const double outa = co1 + co2;
    const double inv = 1.0 / outa;
    double t[4];
    t[3] = outa * 255.0 + 0.5;
    bool ok = binCertified(t[3]);
    for (int ch = 0; ch < 3; ++ch) {
        t[ch] = (co1 * s[ch] + co2 * (double)d[ch]) * inv + 0.5;
        ok &= binCertified(t[ch]);
    }
    if (!ok) {
        blendOverDExact(d, s, op);
        return;
    }
    for (int ch = 0; ch < 4; ++ch)
        d[ch] = (uint8_t)(int)t[ch];
}

struct SmoothTap {
    int lo, hi;
    uint32_t weight;
    int legacyLo, legacyHi;
    double legacyWeight;
};

/* Taps 0..n-1 of an axis of n destination samples over `src` texels, in
 * ascending order. Both maps are built once per axis, before
 * the pixel loop. Keep the shipped double coordinates as well: exact
 * rational ties can land on either side of a half-byte after the legacy
 * floating-point blend. The integer map is the shipped one, per tap
 *   N = clamp((2i+1)*src - n, 0, 2n(src-1)), lo = N / 2n,
 *   weight = ((N % 2n)*65536 + n) / 2n,
 * but N advances by a constant 2*src per tap, so the quotients and
 * remainders are carried along and the only divisions are the three of the
 * setup. */
void makeSmoothTaps(SmoothTap *out, int n, int src)
{
    const double ratio = (double)src / (double)n;
#if SWR_NEON
    const int64_t den = 2 * (int64_t)n, step = 2 * (int64_t)src;
    const int64_t stepQ = step / den, stepR = step % den;
    const int64_t stepW = stepR * 65536 / den, stepWR = stepR * 65536 % den;
    int64_t q, r = (int64_t)src - n;
    if (r >= 0) {
        q = r / den;
        r %= den;
    } else {
        q = -1;
        r += den;
    }
    int64_t W = (r * 65536 + den / 2) / den, WR = (r * 65536 + den / 2) % den;
#endif
    for (int i = 0; i < n; ++i) {
        SmoothTap &t = out[i];
        t = SmoothTap{};
        double f = ((double)i + 0.5) * ratio - 0.5;
        if (f < 0.0) f = 0.0;
        if (f > (double)(src - 1)) f = (double)(src - 1);
        t.legacyLo = (int)f; // Clamped nonnegative: truncation is the shipped floor.
        t.legacyHi = t.legacyLo + 1 < src ? t.legacyLo + 1 : src - 1;
        t.legacyWeight = f - (double)t.legacyLo;
#if SWR_NEON
        if (q < 0) {
            t.lo = 0;
        } else if (q >= src - 1) {
            t.lo = src - 1;
        } else {
            t.lo = (int)q;
            t.weight = (uint32_t)W;
        }
        t.hi = t.lo + 1 < src ? t.lo + 1 : src - 1;
        q += stepQ;
        r += stepR;
        W += stepW;
        WR += stepWR;
        if (WR >= den) {
            WR -= den;
            ++W;
        }
        if (r >= den) {
            r -= den;
            ++q;
            W -= 65536;
        }
#endif
    }
}

/* DELIBERATE legacy fallback for 0 < da < 255: that blend can amplify
 * sampling error by 255/da. The shipped sampler and blend stay verbatim;
 * only their coordinate setup is hoisted. Also resolves rounding ties
 * for da == 0/255 so the fixed path preserves the shipped bytes. */
__attribute__((noinline))
void blendSmoothLegacy(uint8_t *d, const uint8_t *srow0,
                       const uint8_t *srow1, const SmoothTap &x,
                       double wy, uint32_t op)
{
    const int x0 = x.legacyLo, x1 = x.legacyHi;
    const double wx = x.legacyWeight;
    const uint8_t *p00 = srow0 + (size_t)x0 * 4;
    const uint8_t *p01 = srow0 + (size_t)x1 * 4;
    const uint8_t *p10 = srow1 + (size_t)x0 * 4;
    const uint8_t *p11 = srow1 + (size_t)x1 * 4;
    double s[4];
    for (int ch = 0; ch < 4; ++ch) {
        double top = (double)p00[ch] * (1.0 - wx) + (double)p01[ch] * wx;
        double bot = (double)p10[ch] * (1.0 - wx) + (double)p11[ch] * wx;
        s[ch] = top * (1.0 - wy) + bot * wy;
    }
    blendOverD(d, s, op);
}

#if SWR_NEON
/* Q16 taps round within 1/131072 per axis; interpolation truncates <1
 * Q16 unit. 320 units bound sample error, including double coordinate
 * error for positive int-sized extents. Opacity rounding adds <1 unit.
 * Opaque blending adds at most 255*(320/255 + 0.5) + 1 units: <832 total.
 * Accept only intervals lying strictly inside one byte-rounding bin. */
SWR_INLINE bool smoothRoundCertified(uint32_t v, uint32_t error)
{
    uint32_t r = (v + 32768) & 65535;
    return r > error && r < 65536 - error;
}

SWR_INLINE bool blendSmoothFixed(uint8_t *d, const uint8_t *srow0,
                                 const uint8_t *srow1, const SmoothTap &x,
                                 uint32_t wy, uint32_t op)
{
    uint32_t s[4];
    const uint8_t *p00 = srow0 + (size_t)x.lo * 4;
    const uint8_t *p01 = srow0 + (size_t)x.hi * 4;
    const uint8_t *p10 = srow1 + (size_t)x.lo * 4;
    const uint8_t *p11 = srow1 + (size_t)x.hi * 4;
    for (int ch = 0; ch < 4; ++ch) {
        uint32_t top = p00[ch] * (65536 - x.weight) + p01[ch] * x.weight;
        uint32_t bot = p10[ch] * (65536 - x.weight) + p11[ch] * x.weight;
        s[ch] = (uint32_t)(((uint64_t)top * (65536 - wy) +
                            (uint64_t)bot * wy) >> 16);
    }
    bool exact = true;
    if (d[3] == 0) {
        s[3] = (s[3] * op + 127) / 255;
        for (int ch = 0; ch < 4; ++ch)
            exact &= smoothRoundCertified(s[ch], 321);
    } else {
        uint32_t a = (s[3] * op + 32512) / 65025;
        for (int ch = 0; ch < 3; ++ch) {
            s[ch] = (uint32_t)(((uint64_t)s[ch] * a +
                                ((uint64_t)d[ch] << 16) * (65536 - a)) >> 16);
            exact &= smoothRoundCertified(s[ch], 832);
        }
        s[3] = 255 * 65536;
    }
    if (exact)
        for (int ch = 0; ch < 4; ++ch)
            d[ch] = (uint8_t)((s[ch] + 32768) >> 16);
    return exact;
}
#endif

/* One call per row keeps ARM audits separate from clipping and nearest
 * blends. The fixed loop has no floating arithmetic or division calls. */
__attribute__((noinline))
void blendSmoothRow(uint8_t *d, const uint8_t *base, int stride,
                    const SmoothTap *xs, const SmoothTap &y, int n, uint32_t op)
{
    const uint8_t *legacy0 = base + (size_t)y.legacyLo * (size_t)stride;
    const uint8_t *legacy1 = base + (size_t)y.legacyHi * (size_t)stride;
#if SWR_NEON
    const uint8_t *row0 = base + (size_t)y.lo * (size_t)stride;
    const uint8_t *row1 = base + (size_t)y.hi * (size_t)stride;
#endif
    for (int i = 0; i < n; ++i, d += 4) {
#if SWR_NEON
        if ((d[3] == 0 || d[3] == 255) &&
            blendSmoothFixed(d, row0, row1, xs[i], y.weight, op))
            continue;
#endif
        blendSmoothLegacy(d, legacy0, legacy1, xs[i], y.legacyWeight, op);
    }
}

bool rangesOverlap(const Surface &a, const Surface &b)
{
    const uint8_t *a0 = a.px;
    const uint8_t *a1 = a.px + (size_t)(a.h - 1) * (size_t)a.stride + (size_t)a.w * 4;
    const uint8_t *b0 = b.px;
    const uint8_t *b1 = b.px + (size_t)(b.h - 1) * (size_t)b.stride + (size_t)b.w * 4;
    return a0 < b1 && b0 < a1;
}

uint8_t roundToByte(double v)
{
    return (uint8_t)std::floor(v + 0.5);
}

/* ------------------------------------------------------------------ */
/* The geometry prologue shared by blit() and subtract_blit().         */
/*                                                                     */
/* ONE copy on purpose. Before, the KGL_SUBTRACT blit had */
/* its own transcription of this over in bitmap.cpp, and it inherited  */
/* the shrinkRects defect below independently: the same */
/* bug had to be found and fixed twice. Anything that changes here     */
/* changes for both blits.                                             */
/*                                                                     */
/* On true, both rects are positive, non-empty and inside their own    */
/* surfaces, flipW/flipH carry the mirroring the caller asked for, and */
/* `smooth` has been cleared if the blit turned out to be 1:1.         */
/* ------------------------------------------------------------------ */
bool prepareBlitRects(const Surface &dst, Rect &dstRect, const Surface &src,
                      Rect &srcRect, bool &flipW, bool &flipH, bool &smooth)
{
    if (shrinkRects(srcRect.x, srcRect.w, src.w, dstRect.x, dstRect.w, dst.w))
        return false;
    if (shrinkRects(srcRect.y, srcRect.h, src.h, dstRect.y, dstRect.h, dst.h))
        return false;

    /* shrinkRects does NOT always normalise the source: shrinkRectsF returns
     * at its first guard (`sStart >= 0 && sEnd < sBitmapLen`) whenever the
     * source rect already lies inside the source bitmap, i.e. before the
     * branch that would have made sourceLen positive. So a negative SOURCE
     * width/height survives here whenever the rect does not touch an edge.
     * Transfer the sign to the destination exactly as
     * shrinkRectsF(normalize = true) does -- same span, mirrored; the
     * destination *position* must move to its far edge or the blit lands
     * off-screen. Without this, clipRect() below computes x2 = x + w < x,
     * calls the source region empty and silently drops the whole blit
     * (stock mirrors instead, by handing the
     * backwards FloatRect to Quad::setTexRect). */
    if (srcRect.w < 0) {
        srcRect.x += srcRect.w;
        srcRect.w = -srcRect.w;
        dstRect.x += dstRect.w;
        dstRect.w = -dstRect.w;
    }
    if (srcRect.h < 0) {
        srcRect.y += srcRect.h;
        srcRect.h = -srcRect.h;
        dstRect.y += dstRect.h;
        dstRect.h = -dstRect.h;
    }

    /* The source really does have positive dimensions now (the block above
     * is what makes that true); the destination carries the sign. Negative
     * destination width/height mirror the image (intended behaviour of the
     * mkxp stretchBlt mega path, bitmap.cpp:1826-1850). */
    flipW = dstRect.w < 0;
    flipH = dstRect.h < 0;

    /* shrinkRects rounds pos and len separately (lround), which can push a
     * rect up to 1px past the surface edge on exact .5 boundaries (mkxp
     * relies on SDL/GL to clip the result). Hard-clip here to guarantee
     * the no-out-of-bounds contract. */
    if (!clipRect(src, srcRect))
        return false;
    if (!clipRect(dst, dstRect, true))
        return false;

    if (dstRect.w <= 0 || dstRect.h <= 0 || srcRect.w <= 0 || srcRect.h <= 0)
        return false;

    /* mkxp scaleIsOne (bitmap.cpp:1813-1816) */
    if (srcRect.w == dstRect.w && srcRect.h == dstRect.h)
        smooth = false;

    return true;
}

/* A read-only view of the clipped source region for the blit loops.
 * RGSS allows a self-blt, so when the two buffers overlap the region is
 * snapshotted before any write lands -- which is also what makes the row
 * kernel's __restrict sound. Non-overlapping surfaces copy nothing. */
struct SourceView {
    const uint8_t *base;
    int stride;
    int ox, oy;
    std::vector<uint8_t> snapshot;
};

void takeSourceView(SourceView &v, const Surface &src, const Rect &srcRect,
                    const Surface &dst)
{
    v.base = src.px;
    v.stride = src.stride;
    v.ox = srcRect.x;
    v.oy = srcRect.y;
    if (!rangesOverlap(src, dst))
        return;

    v.snapshot.resize((size_t)srcRect.w * (size_t)srcRect.h * 4);
    for (int row = 0; row < srcRect.h; ++row)
        std::memcpy(v.snapshot.data() + (size_t)row * (size_t)srcRect.w * 4,
                    src.px + (size_t)(srcRect.y + row) * (size_t)src.stride +
                        (size_t)srcRect.x * 4,
                    (size_t)srcRect.w * 4);
    v.base = v.snapshot.data();
    v.stride = srcRect.w * 4;
    v.ox = 0;
    v.oy = 0;
}

} // namespace

/* ------------------------------------------------------------------ */
/* fill / clear                                                        */
/* ------------------------------------------------------------------ */

static void fillOp(const Surface &dst, Rect rect, Color c, int op)
{
    if (!dst.px || dst.w <= 0 || dst.h <= 0)
        return;
    if (!clipRect(dst, rect, true))
        return;
    if (profileActive())
        profileCount(op, (uint64_t)(unsigned)rect.w *
                             (uint64_t)(unsigned)rect.h);
    uint32_t v = (uint32_t)c.r | ((uint32_t)c.g << 8) |
                 ((uint32_t)c.b << 16) | ((uint32_t)c.a << 24);
    for (int y = 0; y < rect.h; ++y) {
        uint8_t *row = dst.px + (size_t)(rect.y + y) * (size_t)dst.stride +
                       (size_t)rect.x * 4;
        for (int x = 0; x < rect.w; ++x)
            std::memcpy(row + (size_t)x * 4, &v, 4);
    }
}

void fill(const Surface &dst, Rect rect, Color c)
{
    fillOp(dst, rect, c, ProfileFill);
}

void clear(const Surface &dst, Rect rect)
{
    fillOp(dst, rect, Color{0, 0, 0, 0}, ProfileClear);
}

/* ------------------------------------------------------------------ */
/* blit                                                                */
/* ------------------------------------------------------------------ */

void blit(const Surface &dst, Rect dstRect, const Surface &src, Rect srcRect,
          int opacity, bool smooth)
{
    if (!dst.px || !src.px || dst.w <= 0 || dst.h <= 0 ||
        src.w <= 0 || src.h <= 0)
        return;

    if (opacity < 0) opacity = 0;
    if (opacity > 255) opacity = 255;
    if (opacity == 0)
        return; /* mkxp NORMAL-mode early out (bitmap.cpp:1793) */

    /* Clipping, mirroring and the 1:1 smooth-off, shared with
     * subtract_blit() so the two blits cannot drift apart. */
    bool flipW = false, flipH = false;
    if (!prepareBlitRects(dst, dstRect, src, srcRect, flipW, flipH, smooth))
        return;

    /* Profiled calls sample the destination alpha before any write. */
    if (profileActive())
        profileBlend(dst, dstRect, ProfileBlit);

    const int sw = srcRect.w, sh = srcRect.h;
    const int dw = dstRect.w, dh = dstRect.h;

    /* All scratch (snapshot, LUTs) is sized before the first pixel write,
     * so one guard over the setup is the whole failure contract
     *: an allocation failure returns as the documented
     * no-op instead of throwing bad_alloc at the engine. A failed
     * snapshot must not fall through to an unsnapshotted self-blt, which
     * would corrupt pixels. */
    SourceView view;
    std::vector<int> sxLUT;
    std::vector<SmoothTap> smoothX, smoothY;
    try {
        takeSourceView(view, src, srcRect, dst);

        /* Nearest stretched path: source-x row LUT, computed once per blit
         * with the ORIGINAL per-pixel formula floor(((2*ix+1)*sw)/(2*dw)),
         * so the result is bit-identical by construction.
         * The old code ran that 64-bit division per pixel; Cortex-A9 has no
         * hardware divide, so every pixel cost an __aeabi_ldivmod call
         * (~4-5x the whole blit). Now it is one division per column per
         * blit. 1:1 in x needs no LUT: sx == ix, a plain pointer walk. */
        if (!smooth && sw != dw) {
            sxLUT.resize((size_t)dw);
            for (int dx = 0; dx < dw; ++dx) {
                int ix = flipW ? (dw - 1 - dx) : dx;
                sxLUT[(size_t)dx] =
                    (int)(((2 * (int64_t)ix + 1) * sw) / (2 * (int64_t)dw));
            }
        }
        if (smooth) {
            smoothX.resize((size_t)dw);
            makeSmoothTaps(smoothX.data(), dw, sw);
            if (flipW)
                std::reverse(smoothX.begin(), smoothX.end());
            smoothY.resize((size_t)dh);
            makeSmoothTaps(smoothY.data(), dh, sh);
        }
    } catch (...) {
        return;
    }
    const uint8_t *const srcBase = view.base;
    const int srcStride = view.stride;
    const int sox = view.ox, soy = view.oy;
    const uint32_t op = (uint32_t)opacity;

    for (int dy = 0; dy < dh; ++dy) {
        int iy = flipH ? (dh - 1 - dy) : dy;
        uint8_t *drow = dst.px + (size_t)(dstRect.y + dy) * (size_t)dst.stride +
                        (size_t)dstRect.x * 4;

        if (!smooth) {
            /* GL_NEAREST texel-centre convention: floor((i+0.5)*scale).
             * sy keeps the original formula; once per row, so the
             * division cost is negligible. */
            int sy = (int)(((2 * (int64_t)iy + 1) * sh) / (2 * (int64_t)dh));
            const uint8_t *srow = srcBase + (size_t)(soy + sy) * (size_t)srcStride +
                                  (size_t)sox * 4;

            if (!flipW && sw == dw) {
                /* 1:1, source walking forwards: the whole-row kernel
                 * (NEON 8-pixel blocks, or scalar destination-alpha
                 * runs). Subsumes the pre-opacity == 255 memcpy-run
                 * fast path, which it reaches through the same runs. */
                blendRowForward(drow, srow, dw, op);
            } else if (sw == dw) {
                /* 1:1 in x, mirrored: sx == dw-1-dx. Pointer walk, no
                 * per-pixel arithmetic; the flip is hoisted into the
                 * step. Source and destination run in opposite
                 * directions, so the row kernel does not apply. */
                const uint8_t *s = srow + (size_t)(dw - 1) * 4;
                uint8_t *d = drow;
                for (int dx = 0; dx < dw; ++dx) {
                    blendOver(d, s, op);
                    s -= 4;
                    d += 4;
                }
            } else {
                /* Stretched: sx from the precomputed LUT (built once
                 * per blit above); no division in the pixel loop. */
                const int *lut = sxLUT.data();
                uint8_t *d = drow;
                for (int dx = 0; dx < dw; ++dx, d += 4)
                    blendOver(d, srow + (size_t)lut[dx] * 4, op);
            }
        } else {
            const SmoothTap &y = smoothY[(size_t)iy];
            const uint8_t *base = srcBase + (size_t)soy * (size_t)srcStride +
                                  (size_t)sox * 4;
            blendSmoothRow(drow, base, srcStride, smoothX.data(), y, dw, op);
        }
    }
}

/* ------------------------------------------------------------------ */
/* simple_blit: the window base compose's simpleAlpha blend            */
/*                                                                     */
/* See swraster.h for the contract. The engine caller is               */
/* SoftBase::drawQuad's Normal/KeepDestAlpha passes at identity shade  */
/*; the kernel is byte-identical to that loop's */
/* double formulation because every operand is a byte.                 */
/* ------------------------------------------------------------------ */

void simple_blit(const Surface &dst, Rect dstRect, const Surface &src,
                 Rect srcRect, bool keepDestAlpha)
{
    if (!dst.px || !src.px || dst.w <= 0 || dst.h <= 0 ||
        src.w <= 0 || src.h <= 0)
        return;

    /* 1:1 is the contract: there is no sampler here, so a mismatched pair
     * is the same documented no-op an empty rect is. */
    if (dstRect.w != srcRect.w || dstRect.h != srcRect.h)
        return;
    if (dstRect.w <= 0 || dstRect.h <= 0)
        return;

    if (!clipRect(dst, dstRect))
        return;
    if (!clipRect(src, srcRect))
        return;
    /* Both rects clipped independently: trim to the common size anchored
     * at their top-left corners. For the engine's calls (each rect inside
     * its own surface) this never bites. */
    const int w = std::min(dstRect.w, srcRect.w);
    const int h = std::min(dstRect.h, srcRect.h);

    /* blit()'s failure contract: the only scratch is the
     * alias snapshot, sized before the first write, and a failed one is a
     * no-op rather than an unsnapshotted self-blt. No sampling LUTs: 1:1
     * walks pointers. */
    SourceView view;
    try {
        takeSourceView(view, src, srcRect, dst);
    } catch (...) {
        return;
    }

    for (int y = 0; y < h; ++y) {
        const uint8_t *srow = view.base +
                              (size_t)(view.oy + y) * (size_t)view.stride +
                              (size_t)view.ox * 4;
        uint8_t *drow = dst.px + (size_t)(dstRect.y + y) * (size_t)dst.stride +
                        (size_t)dstRect.x * 4;
        simpleRowForward(drow, srow, w, keepDestAlpha);
    }
}

/* ------------------------------------------------------------------ */
/* gradient_fill                                                       */
/* ------------------------------------------------------------------ */

namespace {

/* Exact incremental evaluation of the per-row (or per-column) colour
 *     v(i) = floor((a*(2n-2i-1) + b*(2i+1) + n) / (2n)),   i = 0..n-1
 * which is the closed form gradient_fill used before and is the
 * rounded interpolation across pixel centres the GL path produces.
 *
 * The numerator is N(i) = N(0) + i*step with step = 2*(b-a), so the
 * quotient and the remainder advance by a constant pair
 * (qs, rs) = floordivmod(step, 2n) and the whole scan costs two divisions
 * per channel ONCE instead of one per output row or column. On Cortex-A9
 * every one of those was an __aeabi_ldivmod call inside the loop
 * (no hardware integer divide, and these are 64-bit
 * anyway): a full-screen 544x416 vertical gradient went from 1664 dynamic
 * divisions to 8, and the horizontal one from 2176 to 8.
 *
 * Bit-identical by construction rather than within a tolerance: the same
 * integer N(i) is divided by the same den, only reached by adding instead
 * of multiplying, so the gradient is exact (tolerance 0).
 *
 * den, r and rs are int64 because `n` is the caller's FULL rect extent, not
 * the clipped one, and nothing bounds that to the surface -- 2*n overflows
 * int32 for a rect longer than 2^30. q and qs cannot overflow: q is a
 * channel value in 0..255 and |step| <= 510. The cost is four 64-bit adds
 * and four compares per ROW, against a memcpy of the row itself. */
struct GradientDDA {
    int32_t q[4], qs[4];
    int64_t r[4], rs[4];
    int64_t den;

    /* `first` is the index within the full rect that the clipped scan
     * starts at, so clipping moves where the gradient is sampled but never
     * shifts its phase. clipRect() guarantees 0 <= first <= n-1. */
    GradientDDA(const uint8_t *a, const uint8_t *b, int n, int64_t first)
    {
        den = 2 * (int64_t)n;
        for (int ch = 0; ch < 4; ++ch) {
            const int64_t step = 2 * ((int64_t)b[ch] - (int64_t)a[ch]);
            /* N(first). It is >= n > 0, so the truncating division below is
             * a floor, which is what the closed form did too. */
            const int64_t N = (int64_t)a[ch] * (den - 1) + (int64_t)b[ch] +
                              (int64_t)n + first * step;
            const int64_t quo = N / den;
            q[ch] = (int32_t)quo;
            r[ch] = N - quo * den;
            int64_t fq = step / den; /* truncates toward zero */
            int64_t fr = step - fq * den;
            if (fr < 0) { /* ...so bias it into a floor division */
                fr += den;
                fq -= 1;
            }
            qs[ch] = (int32_t)fq;
            rs[ch] = fr;
        }
    }

    /* The four channels as one 32-bit store, built byte by byte so this
     * does not quietly assume a byte order the rest of the file does not. */
    uint32_t value() const
    {
        const uint8_t v[4] = { (uint8_t)q[0], (uint8_t)q[1], (uint8_t)q[2],
                               (uint8_t)q[3] };
        uint32_t out;
        std::memcpy(&out, v, 4);
        return out;
    }

    /* r stays in [0, den) and rs is in [0, den), so one correction is
     * always enough. */
    void advance()
    {
        for (int ch = 0; ch < 4; ++ch) {
            q[ch] += qs[ch];
            r[ch] += rs[ch];
            if (r[ch] >= den) {
                r[ch] -= den;
                q[ch] += 1;
            }
        }
    }
};

} // namespace

void gradient_fill(const Surface &dst, Rect rect, Color c1, Color c2,
                   bool vertical)
{
    if (!dst.px || dst.w <= 0 || dst.h <= 0)
        return;
    if (rect.w <= 0 || rect.h <= 0)
        return; /* mkxp early-out (bitmap.cpp:2215-2217) */

    Rect clip = rect;
    if (!clipRect(dst, clip))
        return;

    if (profileActive())
        profileCount(ProfileGradientFill, (uint64_t)(unsigned)clip.w *
                                               (uint64_t)(unsigned)clip.h);

    const uint8_t *a = &c1.r;
    const uint8_t *b = &c2.r;

    if (vertical) {
        /* t = (y - rect.y + 0.5) / rect.h over the FULL rect. */
        GradientDDA dda(a, b, rect.h, (int64_t)clip.y - (int64_t)rect.y);
        for (int y = clip.y; y < clip.y + clip.h; ++y, dda.advance()) {
            const uint32_t v32 = dda.value();
            uint8_t *dst4 = dst.px + (size_t)y * (size_t)dst.stride +
                            (size_t)clip.x * 4;
            for (int x = 0; x < clip.w; ++x)
                std::memcpy(dst4 + (size_t)x * 4, &v32, 4);
        }
    } else {
        /* Horizontal: the colour is constant down each column, so write the
         * first clipped row and memcpy it down the rect. The pre-
         * loop walked column-major, touching a fresh cache line for every
         * pixel of a full-screen gradient -- 544 columns x 416 rows of
         * 4-byte stores 2176 bytes apart. No scratch buffer: the first row
         * IS the pattern, and stride >= w*4 puts the copies strictly after
         * it, so nothing overlaps. */
        uint8_t *row0 = dst.px + (size_t)clip.y * (size_t)dst.stride +
                        (size_t)clip.x * 4;
        GradientDDA dda(a, b, rect.w, (int64_t)clip.x - (int64_t)rect.x);
        for (int x = 0; x < clip.w; ++x, dda.advance()) {
            const uint32_t v32 = dda.value();
            std::memcpy(row0 + (size_t)x * 4, &v32, 4);
        }
        const size_t rowBytes = (size_t)clip.w * 4;
        for (int y = 1; y < clip.h; ++y)
            std::memcpy(row0 + (size_t)y * (size_t)dst.stride, row0, rowBytes);
    }
}

/* ------------------------------------------------------------------ */
/* hue_change (shader/hue.frag, evaluated exactly in integers)         */
/* ------------------------------------------------------------------ */

namespace {

/* identity 5: (x*34953) >> 21 == x/60 for
 * every x in 0..15330, which is the whole range of X+30 below. Spelled out
 * rather than left as `/ 60` so there is a constant for the identity test to
 * read out of this file and prove over its full domain -- the compiler would
 * pick its own reciprocal for a constant divisor, but then nothing would be
 * tied to anything. */
const uint32_t kDiv60Magic = 34953u;

SWR_INLINE uint32_t div60(uint32_t x) { return (x * kDiv60Magic) >> 21; }

#if SWR_NEON
/* One channel of the integer kernel below for 4 pixels in 32-bit lanes.
 * Bit-identical to the scalar loop: U < 1079*d < 3*period, so two
 * conditional subtractions finish the mod; vabd is |U - half| and vqsub
 * clamps X - flat at 0. */
SWR_INLINE uint32x4_t hueChannel4(uint32x4_t base, uint16x4_t d, uint16_t off,
                                  uint32x4_t period, uint32x4_t half,
                                  uint32x4_t flat)
{
    uint32x4_t U = vmlal_n_u16(base, d, off);
    U = vsubq_u32(U, vandq_u32(vcgeq_u32(U, period), period));
    U = vsubq_u32(U, vandq_u32(vcgeq_u32(U, period), period));
    uint32x4_t X = vminq_u32(vqsubq_u32(vabdq_u32(U, half), flat), flat);
    X = vmulq_n_u32(vaddq_u32(X, vdupq_n_u32(30)), kDiv60Magic);
    return vshrq_n_u32(X, 21);
}

/* 8 pixels: the four sextant cases as lane selects, then 3 channels. */
SWR_INLINE void hue8(uint8_t *p, const uint16_t off[3])
{
    uint8x8x4_t px = vld4_u8(p);
    const uint8x8_t mx8 = vmax_u8(vmax_u8(px.val[0], px.val[1]), px.val[2]);
    const uint8x8_t mn8 = vmin_u8(vmin_u8(px.val[0], px.val[1]), px.val[2]);
    const uint16x8_t d = vsubl_u8(mx8, mn8);
    const uint64x2_t any = vreinterpretq_u64_u16(d);
    if ((vgetq_lane_u64(any, 0) | vgetq_lane_u64(any, 1)) == 0)
        return; /* all achromatic: the scalar skip, 8 at a time */

    const int16x8_t r = vreinterpretq_s16_u16(vmovl_u8(px.val[0]));
    const int16x8_t g = vreinterpretq_s16_u16(vmovl_u8(px.val[1]));
    const int16x8_t b = vreinterpretq_s16_u16(vmovl_u8(px.val[2]));
    const int16x8_t ds = vreinterpretq_s16_u16(d);
    /* T = g-b | 2d-(r-b) | 6d-(b-g) | 4d+(r-g), chosen like the scalar ifs. */
    const int16x8_t t1 = vsubq_s16(g, b);
    const int16x8_t t2 = vsubq_s16(vshlq_n_s16(ds, 1), vsubq_s16(r, b));
    const int16x8_t t3 = vsubq_s16(vmulq_n_s16(ds, 6), vsubq_s16(b, g));
    const int16x8_t t4 = vaddq_s16(vshlq_n_s16(ds, 2), vsubq_s16(r, g));
    const int16x8_t tGb = vbslq_s16(vcgeq_s16(r, g), t1, t2);
    const int16x8_t tLt = vbslq_s16(vcgeq_s16(r, b), t3, t4);
    const uint16x8_t T =
        vreinterpretq_u16_s16(vbslq_s16(vcgeq_s16(g, b), tGb, tLt));

    const uint16x4_t dLo = vget_low_u16(d), dHi = vget_high_u16(d);
    const uint32x4_t baseLo = vmull_n_u16(vget_low_u16(T), 60);
    const uint32x4_t baseHi = vmull_n_u16(vget_high_u16(T), 60);
    const uint32x4_t perLo = vmull_n_u16(dLo, 360), perHi = vmull_n_u16(dHi, 360);
    const uint32x4_t halfLo = vmull_n_u16(dLo, 180), halfHi = vmull_n_u16(dHi, 180);
    const uint32x4_t flatLo = vmull_n_u16(dLo, 60), flatHi = vmull_n_u16(dHi, 60);
    const uint16x8_t mn = vmovl_u8(mn8);
    for (int ch = 0; ch < 3; ++ch) {
        const uint16x8_t ramp = vcombine_u16(
            vmovn_u32(hueChannel4(baseLo, dLo, off[ch], perLo, halfLo, flatLo)),
            vmovn_u32(hueChannel4(baseHi, dHi, off[ch], perHi, halfHi, flatHi)));
        px.val[ch] = vmovn_u16(vaddq_u16(mn, ramp));
    }
    vst4_u8(p, px);
}
#endif /* SWR_NEON */

} // namespace

/* shader/hue.frag, evaluated exactly, in integers.
 *
 * Write the shader's own rgb2hsv/hsv2rgb out with r, g, b as bytes and let
 * its two 1.0e-10 epsilons go to zero. With mx and mn the extreme channels
 * and d = mx - mn, rgb2hsv gives v = mx/255, s = d/mx and h = T/(6d), so
 *
 *   out_c = v*((1-s) + ramp_c*s) = (mn + d*ramp_c)/255,
 *   ramp_c = clamp(|fract(h + hueAdjust + k_c)*6 - 3| - 1, 0, 1),
 *
 * i.e. the whole HSV round trip is `mn + d*ramp_c` on the 0..255 scale: v,
 * s and both divisions by 255 cancel out of it. k = (1, 2/3, 1/3) are the
 * shader's per-channel phase offsets and T is the integer 6*h*d that
 * rgb2hsv's own branch structure picks -- the four cases below, with the
 * `g >= b` and `r >= p.x` ties resolved exactly as GLSL step() does.
 *
 * Multiplying through by 60*d clears the last fraction. Using
 * 6*fract(z) == (6z) mod 6, 6h == T/d, 6*hueAdjust == wrapped/60 and
 * 6*k == (6, 4, 2):
 *
 *   U_c   = (60*T + d*(wrapped + 60*6*k_c))  mod  360*d
 *   X_c   = clamp(|U_c - 180*d| - 60*d, 0, 60*d)        ( == 60*d*ramp_c )
 *   out_c = mn + (X_c + 30)/60                          ( == mn + round(d*ramp_c) )
 *
 * with 60*6*k == (360, 240, 120). Every term is a small non-negative
 * integer: no floating point, no floor, no fabs, no division by a runtime
 * value -- nothing a Cortex-A9 has to call a libcall for. The double
 * version this replaces cost 6 libcalls and 13 vdiv/vcvt per pixel in the
 * release object (by ARM disassembly).
 *
 * Ranges:
 * T is in [0, 6d), so 60*T < 360*d; d*(wrapped + 360) <= 719*d; the sum is
 * therefore < 1079*d < 3*(360*d) and three conditional subtractions always
 * suffice for the mod. X+30 <= 60*255 + 30 = 15330, the domain of
 * identity 5. Nothing here exceeds 1079*255 = 275145, so int32 is ample.
 *
 * WHY THIS IS +-1 AND NOT BIT-EXACT: the integer form is the EXACT real
 * value of the shader's formula, and the double version rounds that same
 * value to a byte. They can only disagree where the exact value lands on a
 * rounding tie -- X_c % 60 == 30, i.e. d*ramp_c exactly halfway between two
 * bytes -- and then by exactly 1. This was checked exhaustively
 * over every hue.
 */
void hue_change(const Surface &dst, Rect rect, int degrees)
{
    if (!dst.px || dst.w <= 0 || dst.h <= 0)
        return;
    if (rect.w <= 0 || rect.h <= 0)
        return;
    if ((degrees % 360) == 0)
        return; /* mkxp early-out (bitmap.cpp:2885) */

    /* wrapRange(hue, 0, 360) (bitmap.cpp:2949). The shader divides this by
     * 360; here it stays an integer number of degrees. */
    int wrapped = degrees % 360;
    if (wrapped < 0)
        wrapped += 360;

    if (!clipRect(dst, rect))
        return;

    if (profileActive())
        profileCount(ProfileHueChange, (uint64_t)(unsigned)rect.w *
                                            (uint64_t)(unsigned)rect.h);

    const int32_t kOff[3] = { wrapped + 360, wrapped + 240, wrapped + 120 };
#if SWR_NEON
    const uint16_t kOff16[3] = { (uint16_t)kOff[0], (uint16_t)kOff[1],
                                 (uint16_t)kOff[2] };
#endif

    for (int y = 0; y < rect.h; ++y) {
        uint8_t *row = dst.px + (size_t)(rect.y + y) * (size_t)dst.stride +
                       (size_t)rect.x * 4;
        int x = 0;
#if SWR_NEON
        for (; x + 8 <= rect.w; x += 8)
            hue8(row + (size_t)x * 4, kOff16);
#endif
        for (; x < rect.w; ++x) {
            uint8_t *p = row + (size_t)x * 4;
            /* An achromatic pixel is a fixed point of a hue rotation, so
             * skip the whole sextant evaluation for it.
             * Bit-exact, not an approximation, and it stays bit-exact under
             * the integer kernel: r == g == b makes d == 0, and then
             * X_c == clamp(|U_c| - 0, 0, 0) == 0 for every channel and every
             * hue, so out_c == mn + 0 == the input. The skip is therefore
             * pure speed -- the arithmetic below would give the same answer
             * -- which is also why nothing further down needs a d != 0
             * guard: it divides by 60, never by d.
             * That holds over
             * all 359 hues x 256 grey levels, against the unskipped
             * double-precision reference.
             *
             * Transparent black dominates a decoded RTP animation sheet,
             * which is the case that stalls, so this stays
             * worth having even now that the coloured path is cheap: it
             * still skips the branch tree, three sextant evaluations and
             * three stores. */
            if (p[0] == p[1] && p[1] == p[2])
                continue;
            const int32_t r = p[0], g = p[1], b = p[2];
            /* rgb2hsv's two step() branches, and the T = 6*h*d each picks.
             * mx/mn really are max/min: g >= b && r >= g gives r >= g >= b,
             * and so on around the four sextant pairs. */
            int32_t mx, mn, T;
            if (g >= b) {
                if (r >= g) {  /* q = (r, b, 0, g),      h = (g-b)/(6d)   */
                    mx = r; mn = b; T = g - b;
                } else {       /* q = (g, b, -1/3, r),   h = |(r-b)/(6d) - 1/3| */
                    mx = g; mn = r < b ? r : b; T = 2 * (mx - mn) - (r - b);
                }
            } else {
                if (r >= b) {  /* q = (r, g, -1, b),     h = |(b-g)/(6d) - 1|   */
                    mx = r; mn = g; T = 6 * (mx - mn) - (b - g);
                } else {       /* q = (b, g, 2/3, r),    h = (r-g)/(6d) + 2/3   */
                    mx = b; mn = r < g ? r : g; T = 4 * (mx - mn) + (r - g);
                }
            }
            const int32_t d = mx - mn;
            const int32_t period = 360 * d, half = 180 * d, flat = 60 * d;
            const int32_t base = 60 * T;
            for (int ch = 0; ch < 3; ++ch) {
                int32_t U = base + d * kOff[ch];
                if (U >= period) U -= period;
                if (U >= period) U -= period;
                if (U >= period) U -= period;
                int32_t X = U - half;
                if (X < 0) X = -X;
                X -= flat;
                if (X < 0) X = 0;
                if (X > flat) X = flat;
                p[ch] = (uint8_t)(mn + (int32_t)div60((uint32_t)(X + 30)));
            }
            /* alpha untouched */
        }
    }
}

/* ------------------------------------------------------------------ */
/* blur (shader/blur.frag + blurH.vert/blurV.vert)                     */
/* ------------------------------------------------------------------ */

namespace {

#if SWR_NEON
/* identity 4: (2*x*10923) >> 16 == x/3 for
 * every x in 0..766, which is the whole range a+b+c+1 can take. NEON has no
 * integer divide at all and vqdmulhq_s16 IS (2*a*b) >> 16, so this one
 * constant is the whole divide. Proven exhaustively, not eyeballed. */
const int16_t kBox3Magic = 10923;
#endif

/* out[i] = (a[i] + b[i] + c[i] + 1) / 3 over n bytes, which is exactly
 * round((a+b+c)/3) -- the 3-tap box of shader/blur.frag once the GL aux
 * texture has quantised it back to bytes.
 *
 * Both blur passes reduce to this one kernel because all four channels
 * average alike and the three taps are a fixed BYTE distance apart: the
 * horizontal pass hands it the same row at -4 / 0 / +4, the vertical pass
 * hands it three rows. That is the whole of the blur change --
 * the pre-loops paid a two-way edge test and a 4-iteration channel
 * loop per pixel, none of which survives here, and the kernel is a
 * straight-line byte stream a SIMD unit can eat 16 at a time.
 *
 * `out` never aliases a/b/c: the horizontal pass writes the scratch buffer
 * while reading the surface and the vertical pass writes the surface while
 * reading the scratch buffer, so SWR_RESTRICT is sound. a, b and c DO
 * overlap each other in the horizontal pass; they are only read. */
void box3Bytes(uint8_t *SWR_RESTRICT out, const uint8_t *a, const uint8_t *b,
               const uint8_t *c, size_t n)
{
    size_t i = 0;
#if SWR_NEON
    const int16x8_t third = vdupq_n_s16(kBox3Magic);
    const uint16x8_t one = vdupq_n_u16(1);
    for (; i + 16 <= n; i += 16) {
        const uint8x16_t va = vld1q_u8(a + i);
        const uint8x16_t vb = vld1q_u8(b + i);
        const uint8x16_t vc = vld1q_u8(c + i);
        /* Widen to 16-bit lanes: the sum is at most 766, so nothing
         * overflows and nothing saturates in the vqdmulh either. */
        uint16x8_t lo = vaddl_u8(vget_low_u8(va), vget_low_u8(vb));
        uint16x8_t hi = vaddl_u8(vget_high_u8(va), vget_high_u8(vb));
        lo = vaddq_u16(vaddw_u8(lo, vget_low_u8(vc)), one);
        hi = vaddq_u16(vaddw_u8(hi, vget_high_u8(vc)), one);
        const int16x8_t qlo = vqdmulhq_s16(vreinterpretq_s16_u16(lo), third);
        const int16x8_t qhi = vqdmulhq_s16(vreinterpretq_s16_u16(hi), third);
        vst1q_u8(out + i, vcombine_u8(vmovn_u16(vreinterpretq_u16_s16(qlo)),
                                      vmovn_u16(vreinterpretq_u16_s16(qhi))));
    }
#endif
    /* Scalar tail (and the whole kernel under -DSWRASTER_NO_SIMD): /3 is a
     * constant divisor, so it is a reciprocal multiply here too, not a
     * __aeabi_uidiv call. */
    for (; i < n; ++i)
        out[i] = (uint8_t)(((uint32_t)a[i] + b[i] + c[i] + 1) / 3);
}

} // namespace

void blur(const Surface &dst)
{
    if (!dst.px || dst.w <= 0 || dst.h <= 0)
        return;

    if (profileActive())
        profileCount(ProfileBlur, (uint64_t)(unsigned)dst.w *
                                      (uint64_t)(unsigned)dst.h);

    const int w = dst.w, h = dst.h;
    const size_t rowBytes = (size_t)w * 4;
    /* Three horizontal-pass rows, not a w*h copy: output row
     * y needs rows y-1..y+1, and row y+1 is computed before row y is
     * overwritten, so every horizontal pass still reads original pixels. */
    std::vector<uint8_t> tmp;
    try {
        tmp.resize(rowBytes * 3);
    } catch (...) {
        return; /* failure contract: no scratch, no pass, pixels untouched */
    }

    /* Horizontal pass of row y into its ring slot. The two clamp-to-edge
     * pixels are peeled (pixel 0 is (2*p0 + p1 + 1)/3, pixel w-1 is its
     * mirror) so the interior is one branch-free run; w == 1 is
     * (3p+1)/3 == p, a copy. */
    auto hpass = [&](int y) {
        const uint8_t *srow = dst.px + (size_t)y * (size_t)dst.stride;
        uint8_t *trow = tmp.data() + (size_t)(y % 3) * rowBytes;
        if (w == 1) {
            std::memcpy(trow, srow, 4);
            return;
        }
        for (int ch = 0; ch < 4; ++ch) {
            trow[ch] = (uint8_t)(((uint32_t)srow[ch] * 2 + srow[4 + ch] + 1) / 3);
            const size_t e = rowBytes - 4 + (size_t)ch;
            trow[e] = (uint8_t)(((uint32_t)srow[e] * 2 + srow[e - 4] + 1) / 3);
        }
        if (w > 2)
            box3Bytes(trow + 4, srow, srow + 4, srow + 8, rowBytes - 8);
    };

    /* Vertical pass into dst. Clamp-to-edge is a repeated row pointer, so
     * there is no edge case left to peel. */
    hpass(0);
    for (int y = 0; y < h; ++y) {
        const int ym = y > 0 ? y - 1 : 0;
        const int yp = y + 1 < h ? y + 1 : h - 1;
        if (yp != y)
            hpass(yp);
        box3Bytes(dst.px + (size_t)y * (size_t)dst.stride,
                  tmp.data() + (size_t)(ym % 3) * rowBytes,
                  tmp.data() + (size_t)(y % 3) * rowBytes,
                  tmp.data() + (size_t)(yp % 3) * rowBytes, rowBytes);
    }
}

/* ------------------------------------------------------------------ */
/* radial_blur (GL path of Bitmap::radialBlur, bitmap.cpp:2480-2575)   */
/*                                                                     */
/* Stock draws a 5-quad "cross" (the bitmap plus one mirrored copy      */
/* across each edge) `divisions` times through SimpleMatrixShader, each */
/* rotated about the bitmap centre by baseAngle + i*angleStep, with     */
/* GL_LINEAR sampling, vertex alpha 1/divisions and BlendAddition       */
/* (glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE)) into a   */
/* freshly cleared target that then replaces the bitmap. Fragment is    */
/* shader/simpleAlpha.frag = (tex.rgb, tex.a*opacity), so per division  */
/*   acc.rgb += tex.rgb * tex.a * opacity;  acc.a += tex.a * opacity    */
/*                                                                      */
/* This is the destination-driven inverse: for each output pixel centre */
/* invert the rotation, find which of the 5 quads (if any) contains the */
/* result, mirror into texture space, sample, accumulate.               */
/*                                                                      */
/* Geometry is EXACT integer arithmetic, on purpose. Transform          */
/* (transform.h:updateMatrix, scale 1, origin = position = centre) is   */
/*   x' = c*(x-cx) + s*(y-cy) + cx,  y' = -s*(x-cx) + c*(y-cy) + cy     */
/* so the inverse of the pixel centre (dx+0.5, dy+0.5) is               */
/*   mx = cx + c*X - s*Y,  my = cy + s*X + c*Y                          */
/* with X = dx+0.5-cx, Y = dy+0.5-cy. Doubling clears the halves:       */
/*   2*mx = w + c*(2dx+1-w) - s*(2dy+1-h)                               */
/* and with c, s rounded to Q30 that is an exact int64. Every region    */
/* test and texture coordinate below is therefore exact, and the double */
/* reference (which evaluates the same expression in double, also       */
/* exactly - 44 significant bits) agrees on                             */
/* every boundary pixel instead of disagreeing by a whole 1/divisions   */
/* step wherever a pixel centre lands within a rounding error of the    */
/* outer edge of the cross. Q30 is ~64x finer than the float32 the GL   */
/* path itself uses.                                                    */
/*                                                                      */
/* The sign of the rotation (i.e. whether row 0 is the top or the       */
/* bottom row in the GL framebuffer) does not matter: the rotation set  */
/* {-angle/2 + i*angle/(divisions-1)} is symmetric about 0, so mirroring */
/* y maps it onto itself.                                               */
/* ------------------------------------------------------------------ */

namespace {

/* M_PI/180 as a literal so host and device, library and reference,
 * all start from the same double. */
const double kPiOver180 = 0.01745329251994329576923690768489;

/* Q30: 2^30. cos/sin are rounded to this grid. */
const int64_t kOne30 = 1073741824;

/* Rotation i in degrees. Algebraically baseAngle + i*angleStep =
 * -angle/2 + i*angle/(divisions-1) (bitmap.cpp:2495-2497), written as one
 * correctly rounded division of two exactly representable integers so the
 * result cannot depend on expression order, FMA contraction or float vs
 * double intermediates. divisions >= 2, so the divisor is never 0. */
double radialDegrees(int angle, int divisions, int i)
{
    const int64_t num = (int64_t)angle * (2 * (int64_t)i - (divisions - 1));
    const int64_t den = 2 * (int64_t)(divisions - 1);
    return (double)num / (double)den;
}

int64_t floorDiv(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b) != 0 && ((a < 0) != (b < 0)))
        --q;
    return q;
}

int64_t ceilDiv(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b) != 0 && ((a < 0) == (b < 0)))
        ++q;
    return q;
}

int clampCut(int64_t v, int n)
{
    if (v < 0)
        return 0;
    if (v > (int64_t)n)
        return n;
    return (int)v;
}

/* f(dx) = f0 + dx*step over dx in [0, n). Fill the three half-open dx spans
 * where f lies in [-T, 0), [0, T) and [T, 2T) -- the mirrored-below, inside
 * and mirrored-above bands of one axis. Half-open everywhere, so the bands
 * are disjoint and a pixel centre exactly on a shared edge belongs to
 * exactly one of them (and the mirror mapping is continuous across those
 * edges anyway, so the choice is invisible).
 *
 * At most 4 integer divisions per axis per destination row per rotation:
 * outside every pixel loop, which is what the Cortex-A9 no-hardware-divide
 * rule is about. */
void triSpans(int64_t f0, int64_t step, int64_t T, int n, int lo[3], int hi[3])
{
    const int64_t bound[4] = { -T, 0, T, 2 * T };

    if (step == 0) {
        for (int k = 0; k < 3; ++k) {
            const bool in = f0 >= bound[k] && f0 < bound[k + 1];
            lo[k] = 0;
            hi[k] = in ? n : 0;
        }
        return;
    }

    int cut[4];
    if (step > 0) {
        /* cut[k] = first dx with f(dx) >= bound[k]; non-decreasing in k. */
        for (int k = 0; k < 4; ++k)
            cut[k] = clampCut(ceilDiv(bound[k] - f0, step), n);
        for (int k = 0; k < 3; ++k) {
            lo[k] = cut[k];
            hi[k] = cut[k + 1];
        }
    } else {
        /* f decreasing: cut[k] = first dx with f(dx) < bound[k];
         * non-increasing in k, so the band is [cut[k+1], cut[k]). */
        for (int k = 0; k < 4; ++k)
            cut[k] = clampCut(floorDiv(bound[k] - f0, step) + 1, n);
        for (int k = 0; k < 3; ++k) {
            lo[k] = cut[k + 1];
            hi[k] = cut[k];
        }
    }
}

/* One channel of a GL_LINEAR fetch, fixed point, three multiplies.
 * wx is Q16 (0..65535), wy is Q15 (0..32767); the result is Q8 (0..65280).
 *
 * Stage 1 brackets p00*(65536-wx) + p01*wx, which is >= 0 and <= 255*65536,
 * so the >> is on a non-negative int32. Stage 2 brackets
 * top*(32768-wy) + bot*wy <= 65280*32768 = 2139095040 < 2^31, likewise
 * non-negative. Weight quantisation costs at most 2^-15 of a channel, i.e.
 * 0.008 LSB before the 1/divisions average. */
inline int32_t bilerpChannel(const uint8_t *p00, const uint8_t *p01,
                             const uint8_t *p10, const uint8_t *p11,
                             int ch, int32_t wx, int32_t wy)
{
    const int32_t a = (int32_t)p00[ch], b = (int32_t)p01[ch];
    const int32_t c = (int32_t)p10[ch], d = (int32_t)p11[ch];
    const int32_t top = ((a << 16) + (b - a) * wx) >> 8;
    const int32_t bot = ((c << 16) + (d - c) * wx) >> 8;
    return ((top << 15) + (bot - top) * wy) >> 15;
}

/* Exact floor(n / k) for a divisor fixed for the whole call, so the final
 * pass has no division either (Granlund-Montgomery round-up reciprocal).
 * With m = floor(2^38/k) + 1 the error term e = m*k - 2^38 lies in (0, k],
 * and floor(n*m / 2^38) == floor(n/k) for every 0 <= n <= 2^38/e. Here
 * k = 256*divisions is in [512, 25600], so the bound is at least
 * 2^38/25600 = 10737418, and the largest n this is asked for is
 * 65280*100 + 12800 = 6540800. m <= 2^29+1 and n*m <= 2^52, so both fit. */
struct InvK {
    uint32_t half;
    uint32_t m;

    void set(uint32_t k)
    {
        half = k >> 1;
        m = (uint32_t)((1ULL << 38) / (uint64_t)k) + 1u;
    }

    /* round(n / k), round-half-up, matching floor(v + 0.5). */
    uint32_t divRound(uint32_t n) const
    {
        return (uint32_t)(((uint64_t)(n + half) * (uint64_t)m) >> 38);
    }
};

/* Which A band / B band a quad lives in, and how its texture coordinate is
 * mirrored out of the model position. TA = aSign*A + aOff (in "2*t, Q30"
 * units), likewise TB. Matches the five posRect/texRect pairs stock builds
 * at bitmap.cpp:2506-2530:
 *   centre posRect (0,0,w,h)     -> tx = mx,      ty = my
 *   upper  posRect (0,0,w,-h)    -> tx = mx,      ty = -my
 *   lower  posRect (0,2h,w,-h)   -> tx = mx,      ty = 2h - my
 *   left   posRect (0,0,-w,h)    -> tx = -mx,     ty = my
 *   right  posRect (2w,0,-w,h)   -> tx = 2w - mx, ty = my
 * i.e. the source mirrored across each of its four edges. */
struct RadialQuad {
    int aBand, bBand;
    int aSign, bSign;
    int aOffMul, bOffMul; /* offset = mul * (2*W2) resp. mul * (2*H2) */
};

const RadialQuad kRadialQuads[5] = {
    { 1, 1, +1, +1, 0, 0 }, /* centre */
    { 1, 0, +1, -1, 0, 0 }, /* upper  */
    { 1, 2, +1, -1, 0, 1 }, /* lower  */
    { 0, 1, -1, +1, 0, 0 }, /* left   */
    { 2, 1, -1, +1, 1, 0 }, /* right  */
};

} // namespace

void radial_blur(const Surface &dst, int angle, int divisions)
{
    if (!dst.px || dst.w <= 0 || dst.h <= 0)
        return;

    /* clamp<int>(angle, 0, 359) / clamp<int>(divisions, 2, 100),
     * bitmap.cpp:2491-2492. */
    if (angle < 0) angle = 0;
    if (angle > 359) angle = 359;
    if (divisions < 2) divisions = 2;
    if (divisions > 100) divisions = 100;

    if (profileActive())
        profileCount(ProfileRadialBlur, (uint64_t)(unsigned)dst.w *
                                            (uint64_t)(unsigned)dst.h);

    const int w = dst.w, h = dst.h;

    /* Every rotation samples the ORIGINAL pixels and the destination is
     * never read back: stock renders into a fresh TexPool texture and swaps
     * it in. Snapshot at a packed w*4 stride so the sampler needs no stride
     * multiply. All scratch is sized before the first write, under one
     * guard implementing the failure contract. */
    std::vector<uint8_t> src;
    std::vector<int64_t> cq, sq;
    std::vector<uint32_t> acc;
    try {
        src.resize((size_t)w * (size_t)h * 4);
        for (int y = 0; y < h; ++y)
            std::memcpy(src.data() + (size_t)y * (size_t)w * 4,
                        dst.px + (size_t)y * (size_t)dst.stride, (size_t)w * 4);
        cq.resize((size_t)divisions);
        sq.resize((size_t)divisions);
        /* One destination row of Q8 accumulators. Each is at most
         * 65280*divisions <= 6528000, so uint32 has 9 bits to spare, and
         * the row stays in L1 across all `divisions` passes over it. */
        acc.resize((size_t)w * 4);
    } catch (...) {
        return;
    }

    /* A fully opaque source -- which is what Graphics.snap_to_bitmap hands
     * the battleback code, the only caller vanilla RGSS has -- makes the
     * alpha fetch the constant 65280 and the premultiply the identity
     * (65280*v/65280 == v exactly), so that path is BIT-IDENTICAL to the
     * general one while dropping the alpha fetch (3 multiplies), the three
     * premultiplies and the three constant-divisor divides -- about half the
     * per-sample arithmetic. */
    bool opaque = true;
    for (size_t i = 3; i < src.size(); i += 4) {
        if (src[i] != 255) {
            opaque = false;
            break;
        }
    }

    for (int i = 0; i < divisions; ++i) {
        const double th = radialDegrees(angle, divisions, i) * kPiOver180;
        cq[(size_t)i] = (int64_t)std::llround(std::cos(th) * (double)kOne30);
        sq[(size_t)i] = (int64_t)std::llround(std::sin(th) * (double)kOne30);
    }

    const int64_t W2 = (int64_t)w << 31; /* 2*w in Q30 */
    const int64_t H2 = (int64_t)h << 31; /* 2*h in Q30 */

    InvK inv;
    inv.set(256u * (uint32_t)divisions);

    for (int dy = 0; dy < h; ++dy) {
        std::fill(acc.begin(), acc.end(), 0u);
        const int64_t Vy = 2 * (int64_t)dy + 1 - h;

        for (int i = 0; i < divisions; ++i) {
            const int64_t c = cq[(size_t)i], s = sq[(size_t)i];
            /* 2*mx and 2*my in Q30 at dx == 0, and their per-column step. */
            const int64_t A0 = ((int64_t)w << 30) + c * (1 - (int64_t)w) - s * Vy;
            const int64_t B0 = ((int64_t)h << 30) + s * (1 - (int64_t)w) + c * Vy;
            const int64_t dA = 2 * c, dB = 2 * s;

            int aLo[3], aHi[3], bLo[3], bHi[3];
            triSpans(A0, dA, W2, w, aLo, aHi);
            triSpans(B0, dB, H2, w, bLo, bHi);

            for (const RadialQuad &q : kRadialQuads) {
                int lo = aLo[q.aBand] > bLo[q.bBand] ? aLo[q.aBand] : bLo[q.bBand];
                int hi = aHi[q.aBand] < bHi[q.bBand] ? aHi[q.aBand] : bHi[q.bBand];
                if (hi <= lo)
                    continue;

                const int64_t A = A0 + (int64_t)lo * dA;
                const int64_t B = B0 + (int64_t)lo * dB;
                /* Texture coordinate as 2*t in Q30, biased by +2^31 so the
                 * GL_LINEAR split is a plain unsigned shift. Inside a band
                 * t lies in [0, w] (resp. [0, h]), so U = 2*(t + 0.5) in Q30
                 * is >= 2^31 > 0, and U >> 31 == floor(t + 0.5), i.e. one
                 * past the lower of the two texels GL samples (which is
                 * floor(t - 0.5)); the fraction below it is the weight. */
                int64_t U = (int64_t)q.aSign * A +
                            (int64_t)q.aOffMul * 2 * W2 - kOne30 + ((int64_t)1 << 31);
                int64_t V = (int64_t)q.bSign * B +
                            (int64_t)q.bOffMul * 2 * H2 - kOne30 + ((int64_t)1 << 31);
                const int64_t dU = (int64_t)q.aSign * dA;
                const int64_t dV = (int64_t)q.bSign * dB;

                uint32_t *a = acc.data() + (size_t)lo * 4;
                for (int dx = lo; dx < hi; ++dx, U += dU, V += dV, a += 4) {
                    const uint64_t uu = (uint64_t)U, vv = (uint64_t)V;
                    /* floor(tx + 0.5) in 0..w, floor(ty + 0.5) in 0..h. The
                     * two clamps are a contract guard, not geometry: the
                     * spans above cannot produce anything else, but "no op
                     * reads out of bounds" should hold structurally, the way
                     * blit hard-clips after shrinkRects. Clamping the
                     * unsigned shift catches a negative U too (it arrives
                     * here as a huge uu). A geometry bug is still caught --
                     * as a pixel mismatch against the reference. */
                    uint64_t sx = uu >> 31, sy = vv >> 31;
                    if (sx > (uint64_t)w) sx = (uint64_t)w;
                    if (sy > (uint64_t)h) sy = (uint64_t)h;
                    const int ix = (int)sx;
                    const int iy = (int)sy;
                    const int32_t wx = (int32_t)((uint32_t)(uu & 0x7FFFFFFFu) >> 15);
                    const int32_t wy = (int32_t)((uint32_t)(vv & 0x7FFFFFFFu) >> 16);

                    /* CLAMP_TO_EDGE on the two texels each axis straddles. */
                    const int xa = ix > 0 ? ix - 1 : 0;
                    const int xb = ix < w ? ix : w - 1;
                    const int ya = iy > 0 ? iy - 1 : 0;
                    const int yb = iy < h ? iy : h - 1;

                    const uint8_t *r0 = src.data() + (size_t)ya * (size_t)w * 4;
                    const uint8_t *r1 = src.data() + (size_t)yb * (size_t)w * 4;
                    const uint8_t *p00 = r0 + (size_t)xa * 4;
                    const uint8_t *p01 = r0 + (size_t)xb * 4;
                    const uint8_t *p10 = r1 + (size_t)xa * 4;
                    const uint8_t *p11 = r1 + (size_t)xb * 4;

                    if (opaque) {
                        a[0] += (uint32_t)bilerpChannel(p00, p01, p10, p11, 0, wx, wy);
                        a[1] += (uint32_t)bilerpChannel(p00, p01, p10, p11, 1, wx, wy);
                        a[2] += (uint32_t)bilerpChannel(p00, p01, p10, p11, 2, wx, wy);
                        a[3] += 65280u;
                    } else {
                        const uint32_t va =
                            (uint32_t)bilerpChannel(p00, p01, p10, p11, 3, wx, wy);
                        /* v*va <= 65280^2 = 4261478400 < 2^32; /65280 is a
                         * compile-time constant, so no runtime division. */
                        for (int ch = 0; ch < 3; ++ch) {
                            const uint32_t v =
                                (uint32_t)bilerpChannel(p00, p01, p10, p11, ch, wx, wy);
                            a[ch] += v * va / 65280u;
                        }
                        a[3] += va;
                    }
                }
            }
        }

        uint8_t *out = dst.px + (size_t)dy * (size_t)dst.stride;
        for (int dx = 0; dx < w; ++dx) {
            const uint32_t *a = acc.data() + (size_t)dx * 4;
            uint8_t *o = out + (size_t)dx * 4;
            o[0] = (uint8_t)inv.divRound(a[0]);
            o[1] = (uint8_t)inv.divRound(a[1]);
            o[2] = (uint8_t)inv.divRound(a[2]);
            o[3] = (uint8_t)inv.divRound(a[3]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* get_pixel / set_pixel                                               */
/* ------------------------------------------------------------------ */

Color get_pixel(const Surface &surf, int x, int y)
{
    if (!surf.px || x < 0 || y < 0 || x >= surf.w || y >= surf.h)
        return Color{0, 0, 0, 0};
    if (profileActive())
        profileCount(ProfileGetPixel, 1);
    const uint8_t *p = surf.px + (size_t)y * (size_t)surf.stride + (size_t)x * 4;
    return Color{p[0], p[1], p[2], p[3]};
}

void set_pixel(const Surface &surf, int x, int y, Color c)
{
    if (!surf.px || x < 0 || y < 0 || x >= surf.w || y >= surf.h)
        return;
    if (profileActive())
        profileCount(ProfileSetPixel, 1);
    uint8_t *p = surf.px + (size_t)y * (size_t)surf.stride + (size_t)x * 4;
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
}

/* ------------------------------------------------------------------ */
/* composite_text                                                      */
/* ------------------------------------------------------------------ */

void composite_text(const Surface &dst, Rect dstRect, const Surface &src,
                    int opacity)
{
    blit(dst, dstRect, src, Rect{0, 0, src.w, src.h}, opacity, false);
}

/* ------------------------------------------------------------------ */
/* subtract_blit (upstream shader/kglSubtract.frag)                             */
/*                                                                     */
/*   out.rgb = clamp(dst.rgb - factor*src.rgb, 0, 1);   out.a = 1      */
/*                                                                     */
/* with factor = mkxp bltNormOpacity(KGL_SUBTRACT, opacity)            */
/* (bitmap.cpp:1706-1718) = opacity/256, or exactly 1 at opacity 255.  */
/* Both are exact in binary, so the Q8 integer form of the nearest     */
/* path below is the same number as the float the GL path uploads, not */
/* an approximation.                                                   */
/*                                                                     */
/* Geometry, mirroring and sampling are blit()'s, through the same     */
/* helpers. The source ALPHA is never read (the shader does not), and  */
/* every destination pixel the blit covers ends up opaque.             */
/* ------------------------------------------------------------------ */

void subtract_blit(const Surface &dst, Rect dstRect, const Surface &src,
                   Rect srcRect, int opacity, bool smooth)
{
    if (!dst.px || !src.px || dst.w <= 0 || dst.h <= 0 ||
        src.w <= 0 || src.h <= 0)
        return;

    if (opacity < 0) opacity = 0;
    if (opacity > 255) opacity = 255;
    /* No opacity == 0 early out, deliberately: mkxp's early return covers
     * NORMAL only (bitmap.cpp:1791-1799), and this mode still forces
     * out.a = 1 over the whole destination rect at zero opacity. */

    bool flipW = false, flipH = false;
    if (!prepareBlitRects(dst, dstRect, src, srcRect, flipW, flipH, smooth))
        return;

    if (profileActive())
        profileCount(ProfileSubtractBlit,
                     (uint64_t)(unsigned)dstRect.w *
                         (uint64_t)(unsigned)dstRect.h);

    const int sw = srcRect.w, sh = srcRect.h;
    const int dw = dstRect.w, dh = dstRect.h;

    /* Q8: 256 is the factor's own denominator, so this is exact. */
    const int32_t mul = opacity >= 255 ? 256 : opacity;
    const double factor = (double)mul / 256.0;
    /* Hoisted out of the pixel loops: the same two doubles every sample
     * would otherwise recompute (a VFP divide each, and this CPU is slow at
     * those too), bit-identical because the operands never change. */
    const double xRatio = (double)sw / (double)dw;
    const double yRatio = (double)sh / (double)dh;

    /* blit()'s failure contract: snapshot and LUT are sized
     * before the first write, so one guard over the setup turns an
     * allocation failure into the documented no-op. */
    SourceView view;
    std::vector<int> sxLUT;
    try {
        takeSourceView(view, src, srcRect, dst);

        /* Nearest source-x LUT, as in blit(): one 64-bit division per column
         * per blit rather than one per pixel (Cortex-A9 has no hardware
         * integer divide, so each one in a pixel loop is an __aeabi_ldivmod
         * call). Same floor(((2*ix+1)*sw)/(2*dw)) texel-centre formula. */
        if (!smooth) {
            sxLUT.resize((size_t)dw);
            for (int dx = 0; dx < dw; ++dx) {
                int ix = flipW ? (dw - 1 - dx) : dx;
                sxLUT[(size_t)dx] =
                    (int)(((2 * (int64_t)ix + 1) * sw) / (2 * (int64_t)dw));
            }
        }
    } catch (...) {
        return;
    }
    const uint8_t *const srcBase = view.base;
    const int srcStride = view.stride;
    const int sox = view.ox, soy = view.oy;

    for (int dy = 0; dy < dh; ++dy) {
        const int iy = flipH ? (dh - 1 - dy) : dy;
        uint8_t *drow = dst.px + (size_t)(dstRect.y + dy) * (size_t)dst.stride +
                        (size_t)dstRect.x * 4;

        if (!smooth) {
            const int sy =
                (int)(((2 * (int64_t)iy + 1) * sh) / (2 * (int64_t)dh));
            const uint8_t *srow = srcBase + (size_t)(soy + sy) * (size_t)srcStride +
                                  (size_t)sox * 4;
            const int *lut = sxLUT.data();

            for (int dx = 0; dx < dw; ++dx) {
                const uint8_t *s = srow + (size_t)lut[dx] * 4;
                uint8_t *d = drow + (size_t)dx * 4;
                for (int ch = 0; ch < 3; ++ch) {
                    /* 256*dst - mul*src in Q8, clamped at 0, then rounded
                     * half up: (v + 128) >> 8. Both ends fit in int32
                     * (|v| <= 65280) and the shift is the only division. */
                    int32_t v = (int32_t)d[ch] * 256 - mul * (int32_t)s[ch];
                    if (v < 0) v = 0;
                    d[ch] = (uint8_t)((v + 128) >> 8);
                }
                d[3] = 255;
            }
        } else {
            /* GL_LINEAR texel-centre convention: (i+0.5)*scale - 0.5,
             * clamped to the source rect (clamp-to-edge) -- blit()'s
             * sampler, over RGB alone. Stock reaches this through
             * TEX::setSmooth(true) on the source texture before the
             * kglSubtract quad (bitmap.cpp:2137). */
            double fy = ((double)iy + 0.5) * yRatio - 0.5;
            if (fy < 0.0) fy = 0.0;
            if (fy > (double)(sh - 1)) fy = (double)(sh - 1);
            int y0 = (int)std::floor(fy);
            double wy = fy - (double)y0;
            int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
            const uint8_t *srow0 = srcBase + (size_t)(soy + y0) * (size_t)srcStride +
                                   (size_t)sox * 4;
            const uint8_t *srow1 = srcBase + (size_t)(soy + y1) * (size_t)srcStride +
                                   (size_t)sox * 4;

            for (int dx = 0; dx < dw; ++dx) {
                int ix = flipW ? (dw - 1 - dx) : dx;
                double fx = ((double)ix + 0.5) * xRatio - 0.5;
                if (fx < 0.0) fx = 0.0;
                if (fx > (double)(sw - 1)) fx = (double)(sw - 1);
                int x0 = (int)std::floor(fx);
                double wx = fx - (double)x0;
                int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;

                const uint8_t *p00 = srow0 + (size_t)x0 * 4;
                const uint8_t *p01 = srow0 + (size_t)x1 * 4;
                const uint8_t *p10 = srow1 + (size_t)x0 * 4;
                const uint8_t *p11 = srow1 + (size_t)x1 * 4;
                uint8_t *d = drow + (size_t)dx * 4;
                for (int ch = 0; ch < 3; ++ch) {
                    double top = (double)p00[ch] * (1.0 - wx) + (double)p01[ch] * wx;
                    double bot = (double)p10[ch] * (1.0 - wx) + (double)p11[ch] * wx;
                    double v = (double)d[ch] - factor * (top * (1.0 - wy) + bot * wy);
                    if (v < 0.0) v = 0.0;
                    if (v > 255.0) v = 255.0;
                    d[ch] = roundToByte(v);
                }
                d[3] = 255;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* bench */
/*                                                                     */
/* The device is the only valid benchmark for this library. Cortex-A9  */
/* has no hardware integer divide and a slow double divide, so a host  */
/* ranks these kernels differently from the Vita -- sometimes in the   */
/* opposite order. Every op class a swraster optimisation touches       */
/* therefore needs a "before" number measured here, on hardware.        */
/*                                                                     */
/* Two timing disciplines, both of which keep setup out of the figure: */
/*   - cheap ops at high iteration counts (1:1 blits, fill, gradient): */
/*     one timer around the whole loop. Where the loop needs a reset,  */
/*     the reset loop is timed on its own first and subtracted.        */
/*   - ops costing ~1 ms or more (translucent/mixed-alpha and smooth   */
/*     blits, blur, hue, memcpy, radial blur): the timer is started    */
/*     and stopped around the call itself and the reset runs outside   */
/*     it, so there is nothing to subtract.                            */
/* Either way the reported ms/op is the op alone.                      */
/* ------------------------------------------------------------------ */

namespace {

uint32_t benchChecksum(const Surface &s)
{
    uint32_t h = 2166136261u;
    for (int y = 0; y < s.h; ++y) {
        const uint8_t *row = s.px + (size_t)y * (size_t)s.stride;
        for (int x = 0; x < s.w * 4; ++x) {
            h ^= row[x];
            h *= 16777619u;
        }
    }
    return h;
}

/* The one line format the on-device benchmark parser understands. */
void benchEmit(void (*emit)(const char *line), const char *name, double ms,
               int iters, uint32_t chk)
{
    char line[192];
    std::snprintf(line, sizeof(line),
                  "swraster_bench %s %.4f ms/op (%d iters, chk=0x%08lx)",
                  name, ms, iters, (unsigned long)chk);
    emit(line);
}

/* Buffer pattern every case starts from: a varying RGB ramp, so no blend
 * shortcut can key off a constant colour, plus a per-case alpha. A negative
 * alpha means "alternate 0/128/255 pixel by pixel" -- the mixed-alpha case,
 * which drives all three blendOver branches inside one blit. */
void benchFillPattern(std::vector<uint8_t> &px, int rMul, int gMul, int bMul,
                      int alpha)
{
    static const uint8_t mixed[3] = { 0, 128, 255 };
    int m = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        px[i + 0] = (uint8_t)(i * (size_t)rMul);
        px[i + 1] = (uint8_t)(i * (size_t)gMul);
        px[i + 2] = (uint8_t)(i * (size_t)bMul);
        if (alpha < 0) {
            px[i + 3] = mixed[m];
            m = m == 2 ? 0 : m + 1;
        } else {
            px[i + 3] = (uint8_t)alpha;
        }
    }
}

/* Returns ms per blit. When reset is set (transparent-destination cases)
 * the destination is re-cleared before every timed blit and the separately
 * measured clear cost is subtracted, so the figure is the blit alone. */
double benchBlitMs(Surface &d, const Surface &s, int op, bool reset, int iters)
{
    namespace ch = std::chrono;
    const Rect full{0, 0, d.w, d.h};
    blit(d, full, s, full, op, false); /* warm-up */
    double overhead = 0.0;
    if (reset) {
        ch::steady_clock::time_point t0 = ch::steady_clock::now();
        for (int i = 0; i < iters; ++i)
            clear(d, full);
        overhead = ch::duration<double>(ch::steady_clock::now() - t0).count();
    }
    ch::steady_clock::time_point t0 = ch::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        if (reset)
            clear(d, full);
        blit(d, full, s, full, op, false);
    }
    double total = ch::duration<double>(ch::steady_clock::now() - t0).count();
    return (total - overhead) * 1000.0 / (double)iters;
}

/* Returns ms per blit for a case whose destination would stop belonging to
 * its own class after the first blit: one op-160 blit lifts a
 * da of 128 to 207 and then to 237, 248, ... so by iteration three a
 * "translucent destination" case would be measuring a nearly opaque one.
 * Also the timing shape for the stretched smooth blits, which cost whole
 * milliseconds each. The destination is restored from `pristine` before
 * every timed blit, OUTSIDE the timer, so nothing is subtracted; `chk` ends
 * up covering exactly one blit over a pristine destination. */
double benchBlitRestoreMs(Surface &d, std::vector<uint8_t> &dpix,
                          const std::vector<uint8_t> &pristine,
                          const Surface &s, Rect srcRect, int op, bool smooth,
                          int iters, uint32_t &chk)
{
    namespace ch = std::chrono;
    const Rect dstRect{0, 0, d.w, d.h};
    blit(d, dstRect, s, srcRect, op, smooth); /* warm-up */

    double total = 0.0;
    for (int i = 0; i < iters; ++i) {
        std::memcpy(dpix.data(), pristine.data(), dpix.size());
        ch::steady_clock::time_point t0 = ch::steady_clock::now();
        blit(d, dstRect, s, srcRect, op, smooth);
        total += ch::duration<double>(ch::steady_clock::now() - t0).count();
    }
    chk = benchChecksum(d);
    return total * 1000.0 / (double)iters;
}

/* blur and hue_change are destructive whole-surface ops with the same timing
 * shape: milliseconds each, and running one over its own output
 * would no longer measure the same input. Restore the pattern before every
 * timed run, outside the timer. `isBlur` selects blur, otherwise
 * hue_change(degrees). */
double benchSurfaceOpMs(bool isBlur, int w, int h, int degrees, int iters,
                        uint32_t &chk)
{
    namespace ch = std::chrono;
    std::vector<uint8_t> pattern((size_t)w * (size_t)h * 4);
    benchFillPattern(pattern, 11, 13, 17, 255);
    std::vector<uint8_t> px = pattern;
    Surface s{px.data(), w, h, w * 4};
    const Rect full{0, 0, w, h};

    if (isBlur)
        blur(s); /* warm-up */
    else
        hue_change(s, full, degrees);

    double total = 0.0;
    for (int i = 0; i < iters; ++i) {
        std::memcpy(px.data(), pattern.data(), px.size());
        ch::steady_clock::time_point t0 = ch::steady_clock::now();
        if (isBlur)
            blur(s);
        else
            hue_change(s, full, degrees);
        total += ch::duration<double>(ch::steady_clock::now() - t0).count();
    }
    chk = benchChecksum(s);
    return total * 1000.0 / (double)iters;
}

/* gradient_fill and fill are pure writers -- they never read the destination
 * -- so there is nothing to reset and the whole loop fits inside one timer.
 * One colour component follows the iteration index so every call writes a
 * different result and no store in the loop is dead. The
 * gradient endpoints are fixed, so the host and device tables
 * are the same measurement. `vertical` false is the column-major branch. */
double benchGradientMs(int w, int h, bool vertical, int iters, uint32_t &chk)
{
    namespace ch = std::chrono;
    std::vector<uint8_t> px((size_t)w * (size_t)h * 4);
    Surface d{px.data(), w, h, w * 4};
    const Rect full{0, 0, w, h};
    const Color c1{255, 0, 10, 255};

    gradient_fill(d, full, c1, Color{0, 200, 255, 40}, vertical); /* warm-up */

    ch::steady_clock::time_point t0 = ch::steady_clock::now();
    for (int i = 0; i < iters; ++i)
        gradient_fill(d, full, c1, Color{0, 200, 255, (uint8_t)(40 + i)},
                      vertical);
    double total = ch::duration<double>(ch::steady_clock::now() - t0).count();
    chk = benchChecksum(d);
    return total * 1000.0 / (double)iters;
}

double benchFillMs(int w, int h, int iters, uint32_t &chk)
{
    namespace ch = std::chrono;
    std::vector<uint8_t> px((size_t)w * (size_t)h * 4);
    Surface d{px.data(), w, h, w * 4};
    const Rect full{0, 0, w, h};

    fill(d, full, Color{12, 34, 56, 78}); /* warm-up */

    ch::steady_clock::time_point t0 = ch::steady_clock::now();
    for (int i = 0; i < iters; ++i)
        fill(d, full, Color{(uint8_t)i, 34, 56, 78});
    double total = ch::duration<double>(ch::steady_clock::now() - t0).count();
    chk = benchChecksum(d);
    return total * 1000.0 / (double)iters;
}

/* The memory floor: a straight std::memcpy of one 544x416
 * RGBA8888 buffer, which is what every blit figure above should be read as a
 * multiple of. One source byte is perturbed before each copy so no two
 * copies are identical and none can be elided; that store sits outside the
 * timer along with everything else that is not the copy. */
double benchMemcpyMs(int w, int h, int iters, uint32_t &chk)
{
    namespace ch = std::chrono;
    std::vector<uint8_t> spix((size_t)w * (size_t)h * 4);
    benchFillPattern(spix, 11, 13, 17, 255);
    std::vector<uint8_t> dpix(spix.size());

    std::memcpy(dpix.data(), spix.data(), dpix.size()); /* warm-up */

    double total = 0.0;
    for (int i = 0; i < iters; ++i) {
        if ((size_t)i < spix.size())
            spix[(size_t)i] = (uint8_t)i;
        ch::steady_clock::time_point t0 = ch::steady_clock::now();
        std::memcpy(dpix.data(), spix.data(), dpix.size());
        total += ch::duration<double>(ch::steady_clock::now() - t0).count();
    }
    Surface d{dpix.data(), w, h, w * 4};
    chk = benchChecksum(d);
    return total * 1000.0 / (double)iters;
}

/* Returns ms per radial_blur. The source is restored before
 * every timed run, outside the timer: radial_blur is destructive, and a
 * second run over a result whose uncovered corners went transparent would
 * no longer take the opaque-source path and so would not measure the same
 * thing. sa == 0 means "vary the source alpha" (general per-sample path);
 * otherwise every pixel gets that alpha. */
double benchRadialMs(int w, int h, int angle, int divisions, uint8_t sa,
                     int iters, uint32_t &chk)
{
    namespace ch = std::chrono;
    std::vector<uint8_t> pattern((size_t)w * (size_t)h * 4);
    for (size_t i = 0; i + 3 < pattern.size(); i += 4) {
        pattern[i + 0] = (uint8_t)(i * 11);
        pattern[i + 1] = (uint8_t)(i * 13);
        pattern[i + 2] = (uint8_t)(i * 17);
        pattern[i + 3] = sa ? sa : (uint8_t)(i * 19);
    }
    std::vector<uint8_t> px = pattern;
    Surface s{px.data(), w, h, w * 4};

    radial_blur(s, angle, divisions); /* warm-up */

    double total = 0.0;
    for (int i = 0; i < iters; ++i) {
        std::memcpy(px.data(), pattern.data(), px.size());
        ch::steady_clock::time_point t0 = ch::steady_clock::now();
        radial_blur(s, angle, divisions);
        total += ch::duration<double>(ch::steady_clock::now() - t0).count();
    }
    chk = benchChecksum(s);
    return total * 1000.0 / (double)iters;
}

} // namespace

/* The bench body. bench() wraps it so the library-wide failure contract
 * holds here too: the benchmark parser reads emitted lines one at a
 * time, so an allocation failure ends the report early instead of
 * letting bad_alloc unwind into its C caller. */
static void benchRun(void (*emit)(const char *line))
{
    /* da: destination alpha (0 = transparent, 255 = opaque fast path).
     * sa: source alpha. op: blit opacity. */
    struct Case {
        const char *name;
        int w, h, iters;
        uint8_t da, sa;
        int op;
    };
    static const Case cases[] = {
        { "blit_544x416_over_transparent_op255", 544, 416, 50, 0, 255, 255 },
        { "blit_544x416_over_transparent_op160", 544, 416, 50, 0, 255, 160 },
        { "blit_544x416_over_opaque_op255",      544, 416, 50, 255, 255, 255 },
        { "blit_544x416_over_opaque_op160",      544, 416, 50, 255, 255, 160 },
        { "blit_544x416_over_opaque_sa128_op160", 544, 416, 50, 255, 128, 160 },
        { "blit_96x96_over_transparent_op160",   96, 96, 1000, 0, 255, 160 },
        { "blit_96x96_over_opaque_op160",        96, 96, 1000, 255, 255, 160 },
        { "blit_24x24_over_transparent_op160",   24, 24, 20000, 0, 255, 160 },
        { "blit_24x24_over_opaque_op160",        24, 24, 20000, 255, 255, 160 },
    };

    for (const Case &c : cases) {
        std::vector<uint8_t> dpix((size_t)c.w * (size_t)c.h * 4);
        std::vector<uint8_t> spix((size_t)c.w * (size_t)c.h * 4);
        benchFillPattern(dpix, 3, 5, 7, c.da);
        benchFillPattern(spix, 11, 13, 17, c.sa);
        Surface d{dpix.data(), c.w, c.h, c.w * 4};
        Surface s{spix.data(), c.w, c.h, c.w * 4};
        double ms = benchBlitMs(d, s, c.op, c.da == 0, c.iters);
        benchEmit(emit, c.name, ms, c.iters, benchChecksum(d));
    }

    /* The destination-alpha classes the 1:1 table above cannot reach
     * blendOver has three branches: da == 0, da == 255 and the
     * general 0 < da < 255 path -- the only one that divides by a runtime
     * value, three __aeabi_uidiv calls per pixel on a CPU with no hardware
     * divide. Every case above pins da to 0 or 255, so the expensive branch
     * has never been measured. `translucent_da128` drives it alone;
     * `mixed_alpha` alternates all three down the buffer, which is both what
     * a sprite over a half-faded window looks like and what costs the branch
     * predictor. sa is 255 and opacity 160, so c1 is neither 0 nor 65025 and
     * no fast path in blit() applies either. */
    struct AlphaCase {
        const char *name;
        int da; /* < 0 = alternating 0/128/255 */
        int iters;
    };
    static const AlphaCase alphaCases[] = {
        { "blit_544x416_over_translucent_da128_op160", 128, 20 },
        { "blit_544x416_over_mixed_alpha_op160",        -1, 20 },
    };

    for (const AlphaCase &c : alphaCases) {
        const int w = 544, h = 416;
        std::vector<uint8_t> pristine((size_t)w * (size_t)h * 4);
        std::vector<uint8_t> spix(pristine.size());
        benchFillPattern(pristine, 3, 5, 7, c.da);
        benchFillPattern(spix, 11, 13, 17, 255);
        std::vector<uint8_t> dpix = pristine;
        Surface d{dpix.data(), w, h, w * 4};
        Surface s{spix.data(), w, h, w * 4};
        uint32_t chk = 0;
        double ms = benchBlitRestoreMs(d, dpix, pristine, s, Rect{0, 0, w, h},
                                       160, false, c.iters, chk);
        benchEmit(emit, c.name, ms, c.iters, chk);
    }

    /* Smooth (bilinear) stretch, 544x416 -> 640x480 at opacity 200
     *: the shape a 544x416 snapshot takes when a game scales it
     * to a 640x480 target, and the earlier P10i probe geometry so the two are
     * comparable. da == 0/255 use certified Q16 sampling; da == 128
     * deliberately keeps the shipped double sampler. Reset outside the
     * timer so every iteration measures its named destination class. */
    struct SmoothCase {
        const char *name;
        int sw, sh, dw, dh;
        int da; /* destination alpha */
        int sa; /* source alpha; < 0 = alternating 0/128/255 */
        int iters;
    };
    /* The last two: a translucent destination under a source
     * of varying alpha -- the fractional-alpha traffic the certified double
     * blend (blendOverD) carries, unlike da128's uniform sa == 255 -- and a
     * few pixels behind 2048 column taps, which is the tap builder alone. */
    static const SmoothCase smoothCases[] = {
        { "blit_smooth_544x416_to_640x480_da0",   544, 416, 640, 480, 0,   255, 3 },
        { "blit_smooth_544x416_to_640x480_da255", 544, 416, 640, 480, 255, 255, 3 },
        { "blit_smooth_544x416_to_640x480_da128", 544, 416, 640, 480, 128, 255, 3 },
        { "blit_smooth_544x416_to_640x480_da128_sa_mixed", 544, 416, 640, 480, 128, -1, 3 },
        { "blit_smooth_16x16_to_2048x8_da255",    16,  16,  2048, 8,  255, 255, 200 },
    };

    for (const SmoothCase &c : smoothCases) {
        std::vector<uint8_t> spix((size_t)c.sw * (size_t)c.sh * 4);
        std::vector<uint8_t> pristine((size_t)c.dw * (size_t)c.dh * 4);
        benchFillPattern(spix, 11, 13, 17, c.sa);
        for (size_t k = 0; k + 3 < pristine.size(); k += 4) {
            pristine[k + 0] = 10;
            pristine[k + 1] = 20;
            pristine[k + 2] = 30;
            pristine[k + 3] = (uint8_t)c.da;
        }
        std::vector<uint8_t> dpix = pristine;
        Surface d{dpix.data(), c.dw, c.dh, c.dw * 4};
        Surface s{spix.data(), c.sw, c.sh, c.sw * 4};
        uint32_t chk = 0;
        double ms = benchBlitRestoreMs(d, dpix, pristine, s,
                                       Rect{0, 0, c.sw, c.sh}, 200, true,
                                       c.iters, chk);
        benchEmit(emit, c.name, ms, c.iters, chk);
    }

    /* The whole-surface effect kernels and the two controls. */
    {
        uint32_t chk = 0;
        double ms;

        /* blur: two 3-tap box passes over a byte kernel (box3Bytes), 16
         * bytes per NEON step, edges peeled. hue_change: rgb2hsv/hsv2rgb
         * per pixel in double. Two angles, because an angle-keyed
         * optimisation (a lookup table, say) must not be able to look faster
         * than it is by being measured at one convenient angle. 512x512
         * matches the earlier P10k probe. */
        ms = benchSurfaceOpMs(true, 544, 416, 0, 5, chk);
        benchEmit(emit, "blur_544x416", ms, 5, chk);
        ms = benchSurfaceOpMs(false, 512, 512, 30, 5, chk);
        benchEmit(emit, "hue_512x512_deg30", ms, 5, chk);
        ms = benchSurfaceOpMs(false, 512, 512, 90, 5, chk);
        benchEmit(emit, "hue_512x512_deg90", ms, 5, chk);

        /* gradient_fill is an int64 DDA that divides eight times per call
         * and then only adds, and writes row-major in both directions. The
         * two directions are still different code (the horizontal one
         * builds a row and copies it down), so two cases -- and horizontal
         * is where the pre-column-major walk and its per-column
         * __aeabi_ldivmod calls were, so it is the one to watch. */
        ms = benchGradientMs(544, 416, true, 50, chk);
        benchEmit(emit, "gradient_544x416_vertical", ms, 50, chk);
        ms = benchGradientMs(544, 416, false, 50, chk);
        benchEmit(emit, "gradient_544x416_horizontal", ms, 50, chk);

        /* Controls at the same size as every 544x416 case above: fill is
         * this library's answer to the earlier P10a probe and memcpy to P10c, so a blit
         * number can be read as a multiple of the memory floor rather than
         * as a bare millisecond. */
        ms = benchFillMs(544, 416, 50, chk);
        benchEmit(emit, "fill_544x416", ms, 50, chk);
        ms = benchMemcpyMs(544, 416, 50, chk);
        benchEmit(emit, "memcpy_544x416", ms, 50, chk);
    }

    /* radial_blur at the two sizes/parameters vanilla RGSS2 and RGSS3 use
     * when a map has no battleback: RGSS3
     * Spriteset_Battle#create_blurry_background_bitmap does
     * radial_blur(120, 16) on a 544x416 snapshot, RGSS2
     * Spriteset_Battle#create_battleback does radial_blur(90, 12) on
     * 640x480. Both snapshots are opaque; the third case is the same work
     * with a varying source alpha, i.e. the general per-sample path.
     * Iteration counts are small on purpose -- this is hundreds of ms per
     * op on the device, and it runs once per battle start, not per frame. */
    struct RadialCase {
        const char *name;
        int w, h, angle, divisions, iters;
        uint8_t sa;
    };
    static const RadialCase radials[] = {
        { "radial_blur_544x416_a120_d16_opaque", 544, 416, 120, 16, 2, 255 },
        { "radial_blur_640x480_a90_d12_opaque",  640, 480, 90, 12, 1, 255 },
        { "radial_blur_544x416_a120_d16_alpha",  544, 416, 120, 16, 1, 0 },
    };

    for (const RadialCase &c : radials) {
        uint32_t chk = 0;
        double ms = benchRadialMs(c.w, c.h, c.angle, c.divisions, c.sa,
                                  c.iters, chk);
        benchEmit(emit, c.name, ms, c.iters, chk);
    }
}

void bench(void (*emit)(const char *line))
{
    if (!emit)
        return;
    try {
        benchRun(emit);
    } catch (...) {
    }
}

} // namespace swraster
