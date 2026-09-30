// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * last_error.h — the consumer of last-error.txt.
 *
 * The player writes one small file whenever it
 * dies with something to say: a Ruby exception out of showExc, a failed
 * init out of main.cpp. Until this module nothing read it. The file simply
 * accumulated: the launcher's error_path pointed at logs/last-error.txt
 * while src/vita_fatal.h writes ux0:/data/mkxp-z/last-error.txt, so the one
 * reader that existed looked in a folder the producer never writes to, and
 * every report survived forever unseen.
 *
 * The consumer is the launcher, not the player's boot path. Three reasons,
 * and they are the whole design decision:
 *
 *   - the launcher is the only part of this eboot that can draw when the
 *     report matters. The player writes the file at the moment no thread of
 *     its own can put anything on screen, and the process is gone a beat
 *     later; the next boot lands in the launcher (launcher
 *     mode), which already owns a message screen.
 *   - consuming in the player would race the producer: showExc writes this
 *     file during a run, so a player that also rotated it at boot would be
 *     both ends of the pipe and could rotate away a report it is about to
 *     replace.
 *   - the launcher can do both halves of the job — show it once AND
 *     rotate it — in one place, and nothing downstream has to remember
 *     whether the message was already seen.
 *
 * "Shown once" is a rename, never a flag: last-error.txt is moved to
 * last-error.prev.txt in the log directory, which is what a bug
 * report attaches. So the report a player saw on screen is also the report
 * that lands in the next log bundle, and the live name is free for the next
 * failure.
 *
 * Deliberately dumb. The file is produced by a process that was already
 * dying, so nothing here trusts it: a bounded read, a fixed set of buffers,
 * no allocation, no globals. Generation selection requires a complete v1
 * envelope; the display parser remains tolerant of unfamiliar text. Every
 * displayed byte is run through utf8_sanitize before reaching SDL_ttf.
 *
 * File format, v1 (src/vita_fatal.h in the patched tree):
 *
 *     mkxp-z-last-error v1\n
 *     kind: <script-error|init-error|stuck>\n
 *     title: <one line>\n
 *     bytes: <ten digits>\n
 *     ---\n
 *     <text, any length, newline-terminated>
 *
 * `bytes:` is the exact length of the text after the separator. A report that
 * declares one and does not match it was torn or damaged and is not shown;
 * reports from before the field existed carry none and are read as before.
 *
 * The magic, the separator and the kinds are duplicated below because this
 * module must not include an engine header. Keep them in step with the
 * engine's src/vita_fatal.h.
 *
 * C99, libc only (stdio, string). No allocation, no globals, no SDL.
 */
#ifndef VITA_LAUNCHER_LAST_ERROR_H
#define VITA_LAUNCHER_LAST_ERROR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the producer's format -------------------------------- */

#define LAST_ERROR_MAGIC      "mkxp-z-last-error v1"
#define LAST_ERROR_SEPARATOR  "---"
#define LAST_ERROR_LENGTH_KEY "bytes:"
#define LAST_ERROR_LENGTH_DIGITS 10
#define LAST_ERROR_NAME       "last-error.txt"
#define LAST_ERROR_PREV_NAME  "last-error.prev.txt"

#define LAST_ERROR_KIND_SCRIPT "script-error"
#define LAST_ERROR_KIND_INIT   "init-error"
#define LAST_ERROR_KIND_STUCK  "stuck"

/* ---- bounds ------------------------------------------------------------ */

/* Display preview bytes. Validation uses a HEADER_MAX + 1 byte window plus
 * EOF/final-byte probes; a runaway backtrace never requires an unbounded read.
 * HEADER_MAX matches the producer's full header, including a long title. */
#define LAST_ERROR_READ_MAX   4096u
#define LAST_ERROR_HEADER_MAX 8192u

/*
 * Lines of body text kept.
 *
 * The message screen's body area is 372 px tall (LIST_Y0 + LIST_H minus the
 * heading row) and the small face is 18 px, so about sixteen rendered rows
 * fit. The body spends four of them on the kind sentence, the closing line
 * and the blank lines between, and a report line wider than the 880 px
 * column wraps onto a second row — so eight source lines still fit when half
 * of them wrap, and nothing the reader needs falls off the bottom silently:
 * what does not fit is announced in the body and is in the rotated file and
 * the log in full.
 *
 * Eight is the message plus seven backtrace frames, which is what the top of
 * a Ruby backtrace is worth on a 544 px screen.
 */
#define LAST_ERROR_LINES_MAX  8

#define LAST_ERROR_KIND_MAX   32
#define LAST_ERROR_TITLE_MAX  160
#define LAST_ERROR_TEXT_MAX   1024
#define LAST_ERROR_PATH_MAX   256

/* What last_error_body() can produce: the text plus the kind sentence and
 * the closing line that names where the full report went. */
#define LAST_ERROR_BODY_MAX   (LAST_ERROR_TEXT_MAX + LAST_ERROR_PATH_MAX + 128)

/* What last_error_heading() can produce. */
#define LAST_ERROR_HEADING_MAX (LAST_ERROR_TITLE_MAX + 32)

/* ---- the report -------------------------------------------------------- */

typedef struct LastErrorReport {
    char kind[LAST_ERROR_KIND_MAX];    /* "script-error", or "" */
    char title[LAST_ERROR_TITLE_MAX];  /* one line, sanitised */
    char text[LAST_ERROR_TEXT_MAX];    /* body, newlines preserved */
    /* Where the consumed file now lives, so the screen and the log can say
     * where to look. Empty when it could not be kept. */
    char kept[LAST_ERROR_PATH_MAX];
    /* Bytes read from the file, at most LAST_ERROR_READ_MAX. */
    unsigned long bytes;
    int v1;               /* the v1 magic line was there */
    int truncated;        /* more report existed than `text` holds */
    int io_error;         /* read/close failed, or the pending file could not move */
} LastErrorReport;

/*
 * Parse `len` bytes of a report image into `out`. `data` need not be
 * NUL-terminated and may be anything at all.
 *
 * With the v1 magic: `kind` and `title` come from the header, `text` from
 * after the "---" separator. Without it — a foreign writer, a file from a
 * future format, a truncated one — the whole image becomes `text` and `v1`
 * is 0. Nothing is ever rejected: a report the launcher cannot read is
 * still the only thing the player left behind.
 *
 * Control bytes become spaces and invalid UTF-8 becomes U+FFFD (the file was
 * written by a crashing process and goes straight to a text renderer), except
 * that newlines in the body are preserved, because the backtrace is only
 * legible as lines.
 *
 * Returns 1 when `out` holds something worth showing, 0 when the image was
 * empty. Never allocates.
 */
int last_error_parse(const char *data, size_t len, LastErrorReport *out);

/* Read one generation without changing it. Returns 1 for a displayable v1
 * report with both header fields, a separator, body bytes and final newline
 * (and the declared length, when there is one);
 * 0 for absent/invalid content or invalid arguments; -1 for I/O failure.
 * Unknown kinds and empty titles/bodies are allowed, as in the producer.
 * On 0/-1, parsed fields are empty; -1 sets io_error. When the header
 * declares `bytes:` the body must be exactly that long, which is what detects
 * loss at a line boundary; a legacy v1 report from before the field carries
 * no length, so only its structure can be checked. */
int last_error_read(const char *path, LastErrorReport *out);

/*
 * Read and validate `path`, then "<path>.bak" if absent or invalid, and move
 * the valid generation out of the pending slot after a successful close:
 *
 *   1. rename to `prev_path` (the log directory, which the collect script
 *      pulls). `prev_path` may be NULL; one that does not fit
 *      LAST_ERROR_PATH_MAX, or that names the report itself, is treated as
 *      absent rather than followed into deleting the only copy.
 *   2. failing that, rename to last-error.prev.txt beside `path`, so a
 *      missing log directory still costs nothing but the tidier location.
 *   3. failing that, keep the pending source for a later boot to retry.
 *
 * Each destination is tried once: Vita libc may already have removed it
 * before failing a rename, so the source must survive. The writer's backup
 * is retired only after successful consumption elsewhere. The publication
 * policy itself (paths, move, read-side order) is vita/glue/vita_publish.h.
 *
 * `out->kept` names the rotated file, or the pending source after step 3.
 * `out->io_error` distinguishes read/close or rotation failure from absence.
 *
 * Returns 1 when a report was read (and `out` describes it), 0 when there
 * was no file, it was empty, an argument was NULL, or reading/closing failed.
 * Invalid generations remain untouched. An I/O failure stops recovery and
 * leaves all generations pending with empty parsed fields. A bounded display
 * preview never shortens the file.
 */
int last_error_take(const char *path, const char *prev_path,
                    LastErrorReport *out);

/*
 * The message screen's heading: "Last run failed: <title>", or "Last run
 * failed" when the report carries no title. Returns the bytes written.
 */
size_t last_error_heading(const LastErrorReport *r, char *out, size_t cap);

/*
 * The message screen's body: one plain sentence for the kind, the report
 * text, and a closing line naming the file the full report was moved to.
 * Returns the bytes written.
 */
size_t last_error_body(const LastErrorReport *r, char *out, size_t cap);

/*
 * Line `index` (0-based) of the report text, for a caller that logs the
 * report a line at a time. Returns 1 while there is such a line, 0 once
 * there is not. `out` is NUL-terminated on both.
 */
int last_error_line(const LastErrorReport *r, int index, char *out,
                    size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* VITA_LAUNCHER_LAST_ERROR_H */
