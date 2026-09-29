// SPDX-License-Identifier: GPL-3.0-or-later
/* textpanel: see textpanel.h for the contract. Standard library plus
 * swraster only; nothing here throws, allocates statically, reads past a
 * given length, recurses, or divides by a non-constant inside a loop. */
#include "textpanel.h"

#include <climits>
#include <cstring>

namespace textpanel {

namespace {

/* First codepoint of the CJK Radicals Supplement: the point above which the
 * bundled Latin face is useless and a line may be broken between any two
 * codepoints. */
const uint32_t kCjkFirst = 0x2E80;
const uint32_t kReplacement = 0xFFFD;

const uint32_t kArmMs = 750;
const uint32_t kHoldMs = 1000;

/* ------------------------------------------------------------------ */
/* UTF-8                                                              */
/* ------------------------------------------------------------------ */

struct Decoded {
    uint32_t cp;   /* kReplacement when !valid */
    size_t len;    /* bytes consumed, always >= 1 */
    bool valid;
};

/* One codepoint from [p, p+avail), Unicode 15 Table 3-7. On failure the
 * length is the maximal subpart (the bytes that did form a prefix of a valid
 * sequence, at least one), so scanning resumes at the first byte this did not
 * account for and each bad sequence costs exactly one U+FFFD.
 *
 * avail == 0 is the caller's bug; it returns a one-byte replacement rather
 * than reading anything. */
Decoded decodeUtf8(const unsigned char *p, size_t avail)
{
    Decoded bad = {kReplacement, 1, false};
    if (avail == 0)
        return bad;

    unsigned char b0 = p[0];
    if (b0 < 0x80)
        return Decoded{b0, 1, true};

    unsigned char lo1, hi1;
    size_t need;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        need = 2; lo1 = 0x80; hi1 = 0xBF;
    } else if (b0 == 0xE0) {
        need = 3; lo1 = 0xA0; hi1 = 0xBF;        /* no overlongs */
    } else if (b0 == 0xED) {
        need = 3; lo1 = 0x80; hi1 = 0x9F;        /* no surrogates */
    } else if (b0 >= 0xE1 && b0 <= 0xEF) {
        need = 3; lo1 = 0x80; hi1 = 0xBF;
    } else if (b0 == 0xF0) {
        need = 4; lo1 = 0x90; hi1 = 0xBF;        /* no overlongs */
    } else if (b0 == 0xF4) {
        need = 4; lo1 = 0x80; hi1 = 0x8F;        /* nothing past U+10FFFF */
    } else if (b0 >= 0xF1 && b0 <= 0xF3) {
        need = 4; lo1 = 0x80; hi1 = 0xBF;
    } else {
        return bad;                              /* C0, C1, F5..FF, 80..BF */
    }

    uint32_t cp = b0 & (uint32_t)(0xFF >> (need + 1));
    for (size_t i = 1; i < need; ++i) {
        if (i >= avail) {
            bad.len = i;                         /* truncated by the buffer */
            return bad;
        }
        unsigned char b = p[i];
        unsigned char lo = (i == 1) ? lo1 : 0x80;
        unsigned char hi = (i == 1) ? hi1 : 0xBF;
        if (b < lo || b > hi) {
            bad.len = i;
            return bad;
        }
        cp = (cp << 6) | (uint32_t)(b & 0x3F);
    }
    return Decoded{cp, need, true};
}

/* Encode into buf (4 bytes are always enough) and return the length. */
size_t encodeUtf8(uint32_t cp, char *buf)
{
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Largest boundary <= at. Only continuation bytes are skipped, so on
 * well-formed input this moves at most three bytes. */
size_t snapToBoundary(const char *p, size_t avail, size_t at)
{
    if (at > avail)
        at = avail;
    while (at > 0 && at < avail && ((unsigned char)p[at] & 0xC0) == 0x80)
        --at;
    return at;
}

/* Locale-free unsigned decimal. */
std::string decimal(unsigned long long v)
{
    char buf[24];
    size_t n = 0;
    do {
        buf[n++] = (char)('0' + (int)(v % 10));   /* constant divisor */
        v /= 10;
    } while (v != 0 && n < sizeof(buf));
    std::string out;
    out.reserve(n);
    while (n > 0)
        out.push_back(buf[--n]);
    return out;
}

/* ------------------------------------------------------------------ */
/* Wrapping                                                           */
/* ------------------------------------------------------------------ */

/* One paragraph (no LF inside) appended to out as one or more lines. */
void wrapParagraph(const char *run, size_t n, int maxWidthPx,
                   const Measure &measure, std::vector<std::string> &out)
{
    if (n == 0) {
        out.push_back(std::string());
        return;
    }

    size_t pos = 0;
    while (pos < n) {
        const char *p = run + pos;
        size_t avail = n - pos;

        size_t f = measure.fit ? measure.fit(measure.ctx, p, avail, maxWidthPx)
                               : avail;
        if (f > avail)
            f = avail;
        f = snapToBoundary(p, avail, f);
        if (f >= avail) {
            out.push_back(std::string(p, avail));
            return;
        }

        /* One forward pass over the candidate region collects both break
         * rules. Boundaries and the space index are considered up to and
         * including f: the space (or the CJK boundary) at f is legal even
         * though the codepoint starting there does not fit, because the space
         * is dropped and the CJK break is before the offending codepoint. */
        size_t bestSpace = 0;   /* bytes on this line; 0 = no candidate */
        size_t bestCjk = 0;     /* boundary offset; 0 = no candidate */
        {
            size_t k = 0;
            uint32_t prevCp = 0;
            bool havePrev = false;
            while (k <= f && k < avail) {
                Decoded d = decodeUtf8((const unsigned char *)p + k, avail - k);
                if (k >= 1 && havePrev &&
                    (prevCp >= kCjkFirst || d.cp >= kCjkFirst))
                    bestCjk = k;
                if (k >= 1 && d.valid && d.cp == ' ')
                    bestSpace = k;
                prevCp = d.cp;
                havePrev = true;
                k += d.len;
            }
        }

        size_t lineEnd;
        size_t nextStart;
        if (bestSpace > 0 && bestSpace >= bestCjk) {
            lineEnd = bestSpace;            /* the space itself is dropped */
            nextStart = bestSpace + 1;
        } else if (bestCjk > 0) {
            lineEnd = bestCjk;
            nextStart = bestCjk;
        } else {
            lineEnd = f;                    /* hard break inside a token */
            nextStart = f;
        }

        if (nextStart == 0) {
            /* fit() said nothing fits at all. Emit one codepoint anyway: an
             * over-wide glyph is a cosmetic problem, a zero-progress loop is
             * a hang. */
            Decoded d = decodeUtf8((const unsigned char *)p, avail);
            lineEnd = d.len;
            nextStart = d.len;
        }

        out.push_back(std::string(p, lineEnd));
        pos += nextStart;
    }
}

/* ------------------------------------------------------------------ */
/* Error formatting                                                   */
/* ------------------------------------------------------------------ */

std::string errorLocation(const ScriptError &e)
{
    std::string cls = e.className.empty() ? std::string("Error") : e.className;
    std::string loc;
    if (!e.scriptName.empty()) {
        loc += "Script '";
        loc += e.scriptName;
        loc += "'";
    }
    if (!e.line.empty()) {
        if (!loc.empty())
            loc += " ";
        loc += "line ";
        loc += e.line;
    }
    if (loc.empty())
        return cls;
    loc += ": ";
    loc += cls;
    return loc;
}

} // namespace

const char kTruncationMarker[] = "\n[... truncated]";
const char kOmittedPrefix[] = "\xE2\x80\xA6 ";                       /* "… " */
const char kOmittedSuffix[] = " more lines \xE2\x80\x94 see the log"; /* " — " */

/* ------------------------------------------------------------------ */
/* sanitizeUtf8 / needsCjkFont                                        */
/* ------------------------------------------------------------------ */

std::string sanitizeUtf8(const char *bytes, size_t len, size_t maxBytes)
{
    std::string out;
    if (!bytes || len == 0)
        return out;

    try {
        out.reserve(len < maxBytes ? len : maxBytes);

        const unsigned char *p = (const unsigned char *)bytes;
        size_t i = 0;
        bool truncated = false;
        char enc[4];

        while (i < len) {
            Decoded d = decodeUtf8(p + i, len - i);
            size_t adv = d.len;
            const char *add = enc;
            size_t n = 0;

            if (d.valid && d.cp == 0x0D) {
                /* CRLF and a lone CR are both one LF. */
                if (i + 1 < len && p[i + 1] == 0x0A)
                    ++adv;
                enc[0] = '\n';
                n = 1;
            } else if (d.valid && d.cp == 0x0A) {
                enc[0] = '\n';
                n = 1;
            } else if (d.valid && d.cp == 0x09) {
                add = "    ";
                n = 4;
            } else if (d.valid && (d.cp < 0x20 || d.cp == 0x7F)) {
                i += adv;           /* other C0 controls, NUL and DEL: gone */
                continue;
            } else {
                n = encodeUtf8(d.cp, enc);
            }

            if (out.size() + n > maxBytes) {
                truncated = true;
                break;
            }
            out.append(add, n);
            i += adv;
        }

        if (truncated)
            out.append(kTruncationMarker);
    } catch (...) {
        return std::string();
    }

    return out;
}

bool needsCjkFont(const std::string &text)
{
    const unsigned char *p = (const unsigned char *)text.data();
    size_t len = text.size();
    size_t i = 0;
    while (i < len) {
        Decoded d = decodeUtf8(p + i, len - i);
        if (d.cp >= kCjkFirst)
            return true;
        i += d.len;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* wrap                                                               */
/* ------------------------------------------------------------------ */

std::vector<std::string> wrap(const std::string &text, int maxWidthPx,
                              const Measure &measure)
{
    std::vector<std::string> out;
    if (text.empty())
        return out;

    try {
        const char *base = text.data();
        const size_t total = text.size();
        size_t paraStart = 0;
        while (true) {
            size_t nl = text.find('\n', paraStart);
            size_t paraEnd = (nl == std::string::npos) ? total : nl;
            wrapParagraph(base + paraStart, paraEnd - paraStart, maxWidthPx,
                          measure, out);
            if (nl == std::string::npos)
                break;
            paraStart = nl + 1;
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* formatScriptError / formatOneLine                                  */
/* ------------------------------------------------------------------ */

std::string formatScriptError(const ScriptError &error, size_t maxBacktrace)
{
    std::string out;
    try {
        out = errorLocation(error);
        out += "\n\n";
        out += error.message.empty() ? std::string("(no message)")
                                     : error.message;

        if (!error.backtrace.empty()) {
            out += "\n\nBacktrace:";
            size_t shown = error.backtrace.size() < maxBacktrace
                               ? error.backtrace.size()
                               : maxBacktrace;
            for (size_t i = 0; i < shown; ++i) {
                out += "\n  from ";
                out += error.backtrace[i];
            }
            if (error.backtrace.size() > shown) {
                out += "\n  ... and ";
                out += decimal(error.backtrace.size() - shown);
                out += " more";
            }
        }
    } catch (...) {
        return std::string();
    }
    return out;
}

std::string formatOneLine(const ScriptError &error)
{
    std::string out;
    try {
        out = errorLocation(error);
        out += ": ";
        if (error.message.empty()) {
            out += "(no message)";
        } else {
            size_t at = out.size();
            out += error.message;
            for (size_t i = at; i < out.size(); ++i)
                if (out[i] == '\n' || out[i] == '\r')
                    out[i] = ' ';
        }
    } catch (...) {
        return std::string();
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* layout                                                             */
/* ------------------------------------------------------------------ */

Panel layout(const std::string &heading, const std::string &body,
             const std::string &footer, const PanelSpec &spec,
             const Measure &bodyMeasure, const Measure &headingMeasure)
{
    Panel panel;
    try {
        const int width = spec.width > 0 ? spec.width : 0;
        const int height = spec.height > 0 ? spec.height : 0;
        int margin = spec.margin > 0 ? spec.margin : 0;
        if (margin > width / 2)
            margin = width / 2;
        if (margin > height / 2)
            margin = height / 2;
        const int inner = width - 2 * margin;
        const int bottom = height - margin;

        std::vector<std::string> headLines = wrap(heading, inner, headingMeasure);
        std::vector<std::string> footLines = wrap(footer, inner, bodyMeasure);
        std::vector<std::string> bodyLines = wrap(body, inner, bodyMeasure);

        const int x = margin;
        int y = margin;
        size_t headCount = 0;
        if (inner > 0 && spec.headingH > 0)
            headCount = (bottom - y) / spec.headingH; /* DIV-OK: outside loops */
        if (headCount > headLines.size())
            headCount = headLines.size();

        for (size_t i = 0; i < headCount; ++i) {
            Line ln;
            ln.text = headLines[i];
            ln.style = Heading;
            ln.x = x;
            ln.y = y;
            panel.lines.push_back(ln);
            y += spec.headingH;
        }

        /* The footer is bottom-anchored; the body gets whatever is left
         * between the heading and it. */
        size_t footCount = 0;
        if (inner > 0 && spec.footerH > 0)
            footCount = (bottom - y) / spec.footerH; /* DIV-OK: outside loops */
        if (footCount > footLines.size())
            footCount = footLines.size();
        const int footerTop = bottom - (int)footCount * spec.footerH;

        int capacity = 0;
        if (inner > 0 && spec.lineH > 0 && footerTop > y)
            capacity = (footerTop - y) / spec.lineH;  /* DIV-OK: once, not in a loop */

        size_t keep = bodyLines.size();
        if (bodyLines.size() > (size_t)capacity) {
            /* One slot goes to the "N more lines" line, so the reader always
             * learns that the panel is not the whole story. */
            keep = capacity > 0 ? (size_t)(capacity - 1) : 0;
            const size_t omitted = bodyLines.size() - keep;
            panel.omitted = omitted > INT_MAX ? INT_MAX : (int)omitted;
        }

        for (size_t i = 0; i < keep; ++i) {
            Line ln;
            ln.text = bodyLines[i];
            ln.style = Body;
            ln.x = x;
            ln.y = y;
            panel.lines.push_back(ln);
            y += spec.lineH;
        }
        if (panel.omitted > 0 && capacity > 0) {
            Line ln;
            ln.text = kOmittedPrefix;
            ln.text += decimal((unsigned long long)panel.omitted);
            ln.text += kOmittedSuffix;
            ln.style = Dim;
            ln.x = x;
            ln.y = y;
            panel.lines.push_back(ln);
            y += spec.lineH;
        }

        int fy = footerTop;
        for (size_t i = 0; i < footCount; ++i) {
            Line ln;
            ln.text = footLines[i];
            ln.style = Footer;
            ln.x = x;
            ln.y = fy;
            panel.lines.push_back(ln);
            fy += spec.footerH;
        }
    } catch (...) {
        return Panel();
    }
    return panel;
}

/* ------------------------------------------------------------------ */
/* compose                                                            */
/* ------------------------------------------------------------------ */

void compose(const swraster::Surface &dst, const Panel &panel,
             const PanelSpec &spec, const Glyphs &glyphs)
{
    if (!dst.px || dst.w <= 0 || dst.h <= 0)
        return;

    swraster::fill(dst, swraster::Rect{0, 0, spec.width, spec.height},
                   kBackground);
    swraster::fill(dst, swraster::Rect{0, 0, kAccentW, spec.height}, kAccent);

    if (!glyphs.render)
        return;

    for (size_t i = 0; i < panel.lines.size(); ++i) {
        const Line &ln = panel.lines[i];
        swraster::Surface s = glyphs.render(glyphs.ctx, ln.text.c_str(),
                                            ln.style);
        if (!s.px)
            continue;                 /* nothing was allocated to release */
        if (s.w > 0 && s.h > 0)
            swraster::composite_text(dst, swraster::Rect{ln.x, ln.y, s.w, s.h},
                                     s, 255);
        if (glyphs.release)
            glyphs.release(glyphs.ctx, s);
    }
}

/* ------------------------------------------------------------------ */
/* DismissLatch                                                       */
/* ------------------------------------------------------------------ */

namespace {

/* Unsigned so the 49-day SDL_GetTicks wrap is a non-event; a timestamp that
 * moves backwards reads as no time passed rather than as a huge delta that
 * would fire every timeout at once. */
uint32_t elapsedMs(uint32_t from, uint32_t to)
{
    uint32_t d = to - from;
    return d > 0x80000000u ? 0u : d;
}

} // namespace

DismissLatch::DismissLatch(uint32_t autoDismissMs)
    : mAutoDismissMs(autoDismissMs),
      mOpenedMs(0),
      mPressedMs(0),
      mPhase(PhaseArming),
      mState(Waiting),
      mOpened(false),
      mArmed(false)
{
}

void DismissLatch::open(uint32_t nowMs)
{
    mOpenedMs = nowMs;
    mPressedMs = nowMs;
    mPhase = PhaseArming;
    mState = Waiting;
    mOpened = true;
    mArmed = false;
}

DismissLatch::State DismissLatch::update(bool anyButtonDown, uint32_t nowMs)
{
    if (!mOpened)
        open(nowMs);
    if (mState == Dismissed)
        return mState;

    if (mAutoDismissMs != 0 && elapsedMs(mOpenedMs, nowMs) >= mAutoDismissMs) {
        mPhase = PhaseDone;
        mState = Dismissed;
        return mState;
    }

    if (mPhase == PhaseArming) {
        if (!anyButtonDown || elapsedMs(mOpenedMs, nowMs) >= kArmMs) {
            mArmed = true;
            mPhase = PhaseWaiting;
        }
    }

    if (mPhase == PhaseWaiting) {
        /* A button still down at the forced arm counts as the press, so a
         * button held for ever still dismisses (at kArmMs + kHoldMs). */
        if (anyButtonDown) {
            mPressedMs = nowMs;
            mPhase = PhasePressed;
        }
    } else if (mPhase == PhasePressed) {
        if (!anyButtonDown || elapsedMs(mPressedMs, nowMs) >= kHoldMs) {
            mPhase = PhaseDone;
            mState = Dismissed;
        }
    }

    return mState;
}

} // namespace textpanel
