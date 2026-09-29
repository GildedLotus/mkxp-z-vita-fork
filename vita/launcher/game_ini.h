// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * game_ini.h — RPG Maker Game.ini reader for the Vita launcher.
 *
 * Launcher mode scans games/, reads each Game.ini, and shows
 * the Title. Two facts drove this module:
 *
 *  1. mkxp-z @ pin 826929ee does NOT parse Library=. Config::readGameINI
 *     (src/config.cpp) guesses rgssVersion from the Scripts= file extension
 *     (.rvdata -> 2, .rvdata2 -> 3, anything else -> 1). Library= is not
 *     a reliable version signal for this pin. The launcher therefore parses
 *     Library= itself and uses it as a fallback when Scripts= is missing or ambiguous.
 *
 *  2. Game.ini titles on Japanese releases are CP932/Shift_JIS. mkxp-z
 *     transcodes via uchardet+iconv (src/util/encoding.h). VitaSDK newlib
 *     exports the iconv API but its CES tables are empty,
 *     so iconv_open fails and the title falls back to "mkxp-z". The launcher
 *     must not depend on iconv; this module ships a self-contained CP932
 *     decoder (sjis_table.h).
 *
 * No dependencies beyond libc. Safe to compile host-side.
 * Deliberately self-contained: it
 * never includes utf8_util.h even though it has to do a little UTF-8
 * boundary work of its own.
 *
 * The parser is section-aware and whitespace-tolerant so it
 * agrees with mkxp-z's own INI reader (src/util/iniconfig.cpp) on real files.
 * See the comment above game_ini_parse for the exact rules and the three
 * places where this reader is deliberately more forgiving than mkxp-z.
 */
#ifndef VITA_GAME_INI_H
#define VITA_GAME_INI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result of reading one Game.ini. New fields go at the END of this struct. */
typedef struct GameIni {
    char title[256];     /* Valid UTF-8, NUL-terminated; bad bytes -> U+FFFD. */
    char scripts[256];   /* Scripts= with '\\' -> '/' (raw bytes, not decoded). */
    char library[128];   /* Library= raw (e.g. "RGSS104E.dll"). */
    int rgss_version;    /* 1/2/3. 0 only if detect failed (caller defaults to 1). */
    int title_was_sjis;  /* 1 if Title= needed CP932 -> UTF-8 decode. */
    int rtp_wanted;      /* 1 if any of RTP/RTP1/RTP2/RTP3 in [Game] is non-empty
                          * (the launcher shows "RTP missing" when
                          * the matching rtp/ folder is absent). */
} GameIni;

/*
 * Decode CP932/Shift_JIS to UTF-8.
 *
 *   in / in_len   input bytes (may contain ASCII + SJIS double-byte).
 *   out / out_cap output buffer; NUL-terminated whenever out != NULL and
 *                 out_cap > 0, including overflow (keeps the valid prefix).
 *
 * Returns the number of UTF-8 bytes written (excluding NUL), or -1 if the
 * output would overflow or out is NULL / out_cap is zero.
 * Invalid sequences are replaced with U+FFFD rather
 * than failing, so a mostly-ASCII title with a few bad bytes still displays.
 */
int sjis_to_utf8(const char *in, size_t in_len, char *out, size_t out_cap);

/*
 * Map a Library= value to an RGSS version.
 *
 * Matches the DLL names RPG Maker actually ships:
 *   RGSS100E.dll / RGSS102E.dll / RGSS104E.dll  -> 1
 *   RGSS200E.dll / RGSS202E.dll                 -> 2
 *   RGSS301.dll                                 -> 3
 * Case-insensitive. Returns 0 if the name does not look like an RGSS DLL.
 */
int rgss_version_from_library(const char *library);

/*
 * Map a Scripts= path to an RGSS version, matching mkxp-z's rule exactly
 * (config.cpp readGameINI): .rvdata -> 2, .rvdata2 -> 3, anything else
 * present -> 1, empty -> 0 (unknown).
 */
int rgss_version_from_scripts(const char *scripts);

/*
 * Read a Game.ini from a filesystem path (host or Vita — uses fopen, which
 * newlib maps onto ux0:/ etc.).
 *
 * Detection order (see header comment):
 *   1. Scripts extension  (same as mkxp-z; authoritative when present)
 *   2. Library= name      (fallback when Scripts is empty)
 *   3. 0                  (caller defaults to 1)
 *
 * Returns 0 on success (file read + parsed; title may still be empty),
 * -1 on an invalid argument, I/O failure, or a file exceeding 4096 bytes.
 * `out` is cleared on failure; incomplete metadata is never parsed.
 */
int game_ini_read(const char *path, GameIni *out);

/*
 * Same as game_ini_read but takes the file contents as a memory buffer.
 * This entry point is the pure parse, with no file access.
 *
 * Parsing rules, chosen to agree with mkxp-z's own reader
 * (src/util/iniconfig.cpp + Config::readGameINI) on real files:
 *
 *   - Lines split on '\n'; a trailing '\r' is part of the trimmed whitespace.
 *   - A line is trimmed of " \t\n\v\f\r" at both ends before anything else
 *     (mkxp-z's `trim` character set).
 *   - "[name]" opens a section; the name runs to the LAST ']' on the line,
 *     exactly as mkxp-z slices it. Section names compare case-insensitively.
 *   - ONLY keys inside [Game] are read. Keys before any section header, or
 *     inside a later section, are ignored — The Witch's House ships a second
 *     section whose Title= used to win here.
 *   - "key = value": the split is at the FIRST '=', then key and value are
 *     trimmed independently, so "Title = Foo" yields "Foo".
 *   - Keys compare case-insensitively; a later duplicate inside [Game] wins,
 *     which is what mkxp-z's property map does.
 *   - A leading UTF-8 BOM is skipped.
 *   - ';' and '#' start a comment line.
 *   - A decoded title longer than the field is cut on a UTF-8 code point
 *     boundary, never mid-sequence and never emptied.
 *
 * Deliberately more forgiving than mkxp-z in three places, all noted in
 * the first real-game report: mkxp-z treats only '#' as a comment, drops the final
 * line of a file that does not end in a newline, and does not know about the
 * BOM. Returns 0; the title may still be empty.
 */
int game_ini_parse(const char *data, size_t len, GameIni *out);

#ifdef __cplusplus
}
#endif

#endif /* VITA_GAME_INI_H */
