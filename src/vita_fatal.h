// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vita_fatal.h -- nothing is lost: message logging and the last-error
 * breadcrumb file.
 *
 * On a Vita there is no console, no stdout a player can read, and no working
 * message box: SDL's Vita backend has no ShowMessageBox implementation, so
 * every print / p / msgbox body and every engine error ("Unable to load
 * scripts from ...") used to vanish. Two entry points fix that, and both are
 * declared with C linkage so vita/launcher/vita_boot.cpp can call the same
 * implementation later instead of growing a second one:
 *
 *   vitaLogMessage()      one log line per source line, the prefix repeated
 *                         on each, at most VITA_FATAL_LOG_MAX bytes of the
 *                         body. Emitted through vita_glue_trace on Vita (so
 *                         the crash-safe log sink applies when it is enabled)
 *                         and to stderr everywhere else.
 *
 *   vitaWriteLastError()  one small file the launcher reads on the next boot
 *                         to say why the last game ended, for the case where
 *                         no thread could draw. Written to a .tmp, with the
 *                         previous report moved to .bak before publication.
 *                         Failed publication retains complete generations;
 *                         libc rename is not an atomic replacement on Vita.
 *
 * The launcher owns consumption: it renames last-error.txt to
 * logs/last-error.prev.txt after reading. Nothing here deletes it at boot.
 *
 * This header and vita_fatal.cpp depend on the C library only -- no mkxp-z
 * headers, no SDL, no GL, no kernel objects -- which keeps the real
 * source host-compilable.
 */

#ifndef VITA_FATAL_H
#define VITA_FATAL_H

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- file -------------------------------------------------------------- */

/* Directory the report is written to. Created by vita_glue_init_log's
 * ensure_log_dirs() long before any script can fail. Overridable only for
 * host-compilable builds. */
#ifndef VITA_FATAL_DIR
#define VITA_FATAL_DIR "ux0:/data/mkxp-z"
#endif

#define VITA_FATAL_NAME      "last-error.txt"
#define VITA_FATAL_TMP_NAME  "last-error.txt.tmp"

/* File format, v1:
 *
 *   mkxp-z-last-error v1\n
 *   kind: <script-error|init-error|stuck>\n
 *   title: <one line>\n
 *   ---\n
 *   <text, any length, always newline-terminated>
 *
 * Everything before the separator is a single line, so a reader can parse the
 * header with three getline()s and treat the rest as opaque text. kind and
 * title have their control bytes folded to spaces on the way in; they cannot
 * forge a header line. */
#define VITA_FATAL_MAGIC     "mkxp-z-last-error v1"
#define VITA_FATAL_SEPARATOR "---"

#define VITA_FATAL_KIND_SCRIPT "script-error"
#define VITA_FATAL_KIND_INIT   "init-error"
#define VITA_FATAL_KIND_STUCK  "stuck"

/* ---- bounds ------------------------------------------------------------ */

/* Bytes of a logged body that reach the log. A runaway p of a huge array is
 * truncated rather than allowed to fill ux0:. */
#define VITA_FATAL_LOG_MAX    (64u * 1024u)

/* One assembled log line, prefix included. A longer source line is emitted in
 * successive prefixed chunks, never dropped. Matches the crash-safe sink's
 * VITA_GLUE_LOG_SYNC_LINE_MAX so a line costs one sceIoWrite there. */
#define VITA_FATAL_LINE_MAX   512u

/* Longest prefix honoured; the rest is dropped so every line keeps room for
 * payload. */
#define VITA_FATAL_PREFIX_MAX 64u

/* The whole report header (magic + kind + title + separator) is assembled in
 * one buffer of this size. The text is streamed straight from the caller's
 * pointer and is not bounded by it. */
#define VITA_FATAL_HEADER_MAX 8192u

/* Longest "<dir>/<name>" the writer will build. */
#define VITA_FATAL_PATH_MAX   256u

/* ---- API --------------------------------------------------------------- */

/*
 * Log `utf8` one source line at a time, `prefix` (may be NULL) on every line.
 * A NULL body logs "(null)"; an empty body still logs the prefix, so "the
 * game printed nothing" is distinguishable from "the game did not print".
 * Never allocates, never throws, safe from any thread.
 */
void vitaLogMessage(const char *prefix, const char *utf8);

/*
 * Write the report into `dir` (NULL -> VITA_FATAL_DIR): <dir>/last-error.txt.tmp
 * first, then move last-error.txt to last-error.txt.bak and publish the .tmp.
 * The launcher can fall back to .bak if last-error.txt is absent or invalid.
 * A retained .tmp is published with the same backup sequence before reuse;
 * failure to publish it stops the new write without truncating the evidence.
 *
 * `kind` should be one of the VITA_FATAL_KIND_* strings; `title` is one line;
 * `text` is free-form and may be megabytes. NULL is accepted for any of them.
 *
 * Returns false on any I/O error -- including a directory that does not exist
 * or cannot be written. An incomplete write removes .tmp; a completed write
 * attempts publication even if sync or close fails. Failed publication keeps
 * .tmp and leaves the previous report in last-error.txt or .bak. Sync failure
 * is also logged; false does not mean the report is absent. Successful sync
 * is not a power-loss guarantee.
 * Never throws or allocates: the header lives in a fixed buffer and the text
 * is written straight from the caller's memory, even if the heap has failed.
 */
bool vitaWriteLastErrorTo(const char *dir, const char *kind, const char *title,
                          const char *text);

/* vitaWriteLastErrorTo(VITA_FATAL_DIR, ...). */
bool vitaWriteLastError(const char *kind, const char *title, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* VITA_FATAL_H */
