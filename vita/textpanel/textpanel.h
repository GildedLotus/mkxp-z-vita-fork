// SPDX-License-Identifier: GPL-3.0-or-later
/* textpanel: the one CPU text core every on-screen Vita panel consumes.
 *
 * After the boot seal every piece of text the player
 * sees on a Vita has to be laid out and rasterised on the CPU into a single
 * texture: the settings menu, the FPS line, the
 * error/message panel and the frame-profile HUD.
 * This is that layer, built once. It is C++17, standard library plus
 * swraster.h only: no SDL, no GL, no mkxp-z headers, no locale functions, no
 * I/O, no statics. Nothing here throws: an allocation failure comes back as
 * an empty result, never as an exception crossing into Ruby or SDL.
 *
 * Callers supply the two platform-specific pieces as plain function pointers:
 * Measure (how wide is this run of bytes?) and Glyphs (rasterise this line).
 * On the device those wrap SDL_ttf; on the host they are fixed-advance stubs,
 * which is why the whole file is provable without a device
 *
 *
 * See vita/overlay/ for the other half of the story: a fixed 512x128 ARGB
 * scratch surface with a baked 8x8 ASCII font, for HUD text that must work
 * when the font stack itself is broken.
 */
#ifndef TEXTPANEL_H
#define TEXTPANEL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "swraster.h"

namespace textpanel {

/* ------------------------------------------------------------------ */
/* UTF-8 hygiene                                                      */
/* ------------------------------------------------------------------ */

/* Appended (after a LF) when sanitizeUtf8 hits maxBytes. */
extern const char kTruncationMarker[];

/* Turn arbitrary bytes -- a Ruby String that may hold Shift_JIS, a truncated
 * log line, a 200-line backtrace -- into well-formed UTF-8 that the rest of
 * this file and SDL_ttf can be handed without checking anything again.
 *
 *   * Decoding follows Unicode 15 Table 3-7 exactly: C0/C1 and F5..FF are
 *     never leads, E0 wants A0..BF, ED wants 80..9F (no surrogates), F0 wants
 *     90..BF, F4 wants 80..8F (nothing above U+10FFFF). Overlong forms are
 *     therefore unrepresentable rather than specially detected.
 *   * A byte sequence that is not a valid encoding is replaced by one U+FFFD
 *     per *maximal subpart* (the WHATWG/Unicode recommended practice), and
 *     scanning resumes at the first byte the subpart did not cover. So
 *     "C0 80" yields two U+FFFD (C0 is not a lead, then 80 is a lone
 *     continuation) and "E0 80 80" yields three; a sequence truncated by the
 *     end of the buffer yields exactly one.
 *   * CRLF and a lone CR both become LF. TAB becomes four spaces. Every other
 *     C0 control, NUL and DEL are dropped.
 *   * At most maxBytes bytes of content are produced, always ending on a
 *     codepoint boundary; if anything was dropped for that reason,
 *     kTruncationMarker is appended (so the result may exceed maxBytes by the
 *     length of the marker, and by that alone).
 *
 * bytes == nullptr or len == 0 gives an empty string. NUL bytes inside the
 * buffer are data, not terminators: len is the only length that matters. */
std::string sanitizeUtf8(const char *bytes, size_t len, size_t maxBytes = 16384);

/* True if any codepoint is at or above U+2E80 (CJK Radicals Supplement and
 * everything after it), i.e. if the bundled Latin font cannot render this
 * text and a CJK family has to be found instead.
 *
 * Note U+FFFD is above the threshold, so text that survived sanitizeUtf8 with
 * replacement characters in it reports true. That is deliberate: the CJK
 * families carry U+FFFD, several Latin faces do not, and mojibake is exactly
 * the case where the wider font is wanted. Malformed bytes are likewise
 * treated as U+FFFD here rather than being decoded leniently. */
bool needsCjkFont(const std::string &text);

/* ------------------------------------------------------------------ */
/* Measuring and wrapping                                             */
/* ------------------------------------------------------------------ */

/* How many bytes of [text, text+len) fit in maxWidthPx, always rounded down
 * to a codepoint boundary. On the device this wraps TTF_MeasureUTF8; len == 0
 * or maxWidthPx <= 0 must give 0. fit may return anything at all -- wrap()
 * clamps the result to len, snaps it back to a codepoint boundary and still
 * guarantees progress if it returns 0. */
struct Measure {
    size_t (*fit)(void *ctx, const char *text, size_t len, int maxWidthPx);
    void *ctx;
};

/* Greedy line breaking at maxWidthPx.
 *
 *   * Hard newlines are honoured and empty lines are kept: "a\n\nb" gives
 *     three lines, "a\n" gives two (the second empty). An empty input gives
 *     no lines at all.
 *   * A line is broken after an ASCII space, which is dropped at the break;
 *     or between two codepoints when either of them is at or above U+2E80
 *     (CJK has no spaces to break at). The rightmost of the two candidates
 *     wins; a space wins a tie, because it costs nothing to drop.
 *   * A token too long to fit on a line of its own is hard-broken at the fit
 *     boundary.
 *   * Every line but such a forced one fits, and every line advances by at
 *     least one codepoint even if fit keeps returning 0 (a degenerate measure
 *     or a maxWidthPx narrower than a single glyph), so this always
 *     terminates and never loops on a zero-width line.
 *   * Only ASCII spaces at break points and the newlines themselves are ever
 *     dropped: concatenating the result minus those bytes preserves every
 *     other codepoint and its order.
 *
 * Not recursive, and the only allocation is the output itself. */
std::vector<std::string> wrap(const std::string &text, int maxWidthPx,
                              const Measure &measure);

/* ------------------------------------------------------------------ */
/* Script error formatting                                            */
/* ------------------------------------------------------------------ */

/* Whatever the binding managed to extract from the Ruby exception. Every
 * field is optional: a nil script name, a blank message and an empty
 * backtrace are all normal, and none of them may change the shape of the
 * output into something a panel cannot lay out. */
struct ScriptError {
    std::string className;
    std::string message;
    std::string scriptName;
    std::string line;
    std::vector<std::string> backtrace;
};

/* Multi-line panel body:
 *
 *   Script 'Main' line 42: NoMethodError
 *
 *   undefined method `foo' for nil:NilClass
 *
 *   Backtrace:
 *     from Scene_Map:117:in `update'
 *     ... and 8 more
 *
 * The first line drops whichever of the script name and line number is empty
 * and reads just "NoMethodError" when both are; an empty class name reads
 * "Error"; an empty message reads "(no message)". At most maxBacktrace
 * entries are listed, followed by a count of the rest (maxBacktrace == 0
 * lists none and still reports the count). The text is not wrapped and not
 * sanitised -- run sanitizeUtf8 over the fields first, wrap() over the
 * result. */
std::string formatScriptError(const ScriptError &error,
                              size_t maxBacktrace = 32);

/* The same error on one line, for a log or a title bar: no backtrace, and
 * every LF in the message becomes a space. */
std::string formatOneLine(const ScriptError &error);

/* ------------------------------------------------------------------ */
/* Layout                                                             */
/* ------------------------------------------------------------------ */

enum Style {
    Heading,
    Body,
    Dim,
    Footer
};

/* One rasterisable run, positioned in panel coordinates (top-left origin,
 * the same space as the destination surface compose() is given). */
struct Line {
    std::string text;
    Style style;
    int x;
    int y;
};

/* Default is the whole Vita screen. All measurements are pixels. */
struct PanelSpec {
    int width = 960;
    int height = 544;
    int margin = 32;
    int headingH = 34;
    int lineH = 24;
    int footerH = 28;
};

struct Panel {
    std::vector<Line> lines;
    /* Body lines that did not fit, 0 when everything did; saturates at INT_MAX. */
    int omitted = 0;
};

/* Prefix of the Dim line that replaces the body lines that did not fit;
 * the whole line reads "... N more lines - see the log" (with a real U+2026
 * and U+2014). */
extern const char kOmittedPrefix[];
extern const char kOmittedSuffix[];

/* Lay a heading, a body and a footer into spec.
 *
 * The heading is wrapped with headingMeasure, the body and the footer with
 * bodyMeasure, all at spec.width - 2*margin. The heading starts at the top
 * margin, the body directly under it, the footer is bottom-anchored above the
 * bottom margin. Body lines that would run into the footer are dropped and
 * replaced by a single Dim line naming how many went (Panel::omitted), so the
 * panel never overflows and never silently loses the fact that it did.
 *
 * Margins clamp to [0, min(width, height)/2] before arithmetic. Nonpositive
 * dimensions leave no drawable rows; nonpositive advances suppress their
 * section. Only complete rows fit: excess heading/footer rows are dropped.
 *
 * Empty strings contribute no lines, so a body-only panel is just
 * layout("", body, "", ...). */
Panel layout(const std::string &heading, const std::string &body,
             const std::string &footer, const PanelSpec &spec,
             const Measure &bodyMeasure, const Measure &headingMeasure);

/* ------------------------------------------------------------------ */
/* Compose                                                            */
/* ------------------------------------------------------------------ */

/* Panel colours, RGBA8888 non-premultiplied, the swraster::Surface format.
 * The panel is opaque so it can be drawn with blending on or off and give the
 * same pixels (the engine draws it with BlendNormal, which is the only
 * variant warmed at boot). */
inline constexpr swraster::Color kBackground = {16, 18, 24, 255};
inline constexpr swraster::Color kAccent = {88, 150, 224, 255};
/* Width of the accent bar down the left edge. */
inline constexpr int kAccentW = 6;

/* Rasterise one line. render returns a surface in swraster's RGBA8888 layout
 * that compose() draws at the line's position and then hands straight back to
 * release. A surface with a null px is "this line could not be rendered" and
 * is skipped without being released; anything else is released exactly once,
 * including a surface with a zero or negative size. render itself may be
 * null, in which case compose draws the background and nothing else. */
struct Glyphs {
    swraster::Surface (*render)(void *ctx, const char *utf8, Style style);
    void (*release)(void *ctx, swraster::Surface surface);
    void *ctx;
};

/* Fill dst with kBackground, draw the accent bar, then composite every line
 * at its own position with swraster::composite_text at full opacity.
 *
 * Nothing is written outside dst: both the fills and the composites are
 * clipped by swraster, so a rasteriser that returns a surface far larger than
 * the panel, or a line positioned off the edge, costs pixels but cannot
 * corrupt memory. dst may be larger or smaller than spec; only
 * spec.width x spec.height of it is painted. */
void compose(const swraster::Surface &dst, const Panel &panel,
             const PanelSpec &spec, const Glyphs &glyphs);

/* ------------------------------------------------------------------ */
/* Dismissal                                                          */
/* ------------------------------------------------------------------ */

/* "Press any button" for a pad, with the two traps that costs on a Vita: the
 * button that raised the panel may still be held when it opens, and the
 * release must be consumed by the panel so it does not leak into the game (or
 * into the launcher) afterwards.
 *
 *   1. Arming: the panel ignores buttons until every button is up, or until
 *      750 ms have passed -- a stuck or permanently held button must not make
 *      the panel undismissable.
 *   2. Waiting: the first frame with any button down is the press.
 *   3. Dismissing: the release dismisses. If the button is still down 1000 ms
 *      after the press, that dismisses too.
 *
 * So a button held from before the panel opened and never released dismisses
 * it after 1750 ms, and an ordinary press-release dismisses on the release
 * with no raw button state left set. An autoDismissMs other than 0 dismisses
 * that many milliseconds after the panel opened whatever the buttons do
 * (the unattended-hardware-run marker).
 *
 * Times are milliseconds from any monotonic source, SDL_GetTicks included:
 * deltas are computed unsigned so the 49-day wrap is harmless, and a
 * timestamp that goes backwards is treated as no time passing rather than as
 * an instant timeout. No allocation, no statics; update() is the only
 * mutator. */
class DismissLatch
{
public:
    enum State {
        Waiting,
        Dismissed
    };

    explicit DismissLatch(uint32_t autoDismissMs = 0);

    /* Restart the latch, with nowMs as the moment the panel opened. Optional:
     * the first update() opens the latch at its own nowMs if this was never
     * called. */
    void open(uint32_t nowMs);

    /* anyButtonDown is the OR of every button the pad reports this frame. */
    State update(bool anyButtonDown, uint32_t nowMs);

    State state() const { return mState; }
    /* Whether step 1 has completed; for tests and for logging why a panel is
     * still up. */
    bool armed() const { return mArmed; }

private:
    enum Phase {
        PhaseArming,
        PhaseWaiting,
        PhasePressed,
        PhaseDone
    };

    uint32_t mAutoDismissMs;
    uint32_t mOpenedMs;
    uint32_t mPressedMs;
    Phase mPhase;
    State mState;
    bool mOpened;
    bool mArmed;
};

} // namespace textpanel

#endif // TEXTPANEL_H
