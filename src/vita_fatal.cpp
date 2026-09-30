// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vita_fatal.cpp -- see vita_fatal.h.
 *
 * Two rules shape everything below:
 *
 *  1. No heap. Both entry points run when something has already gone wrong,
 *     and one of the things that goes wrong on a 512 MiB console is the
 *     allocator. Every buffer here is automatic or static, the report text is
 *     written straight from the caller's pointer, and the file is opened with
 *     open(2) rather than fopen(3) so not even a FILE object has to be found.
 *
 *  2. No dependencies. C library only -- no mkxp-z headers, no SDL, no GL, no
 *     kernel objects -- so it is host-compilable
 *     and the launcher can link it without dragging the engine in.
 */

#include "vita_fatal.h"
#include "vita_publish.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
/* Only for the log sink; nothing else in this file is Vita-specific. */
#include "vita_glue.h"
#endif

/* ---- logging ------------------------------------------------------------ */

static void vitaFatalEmit(const char *line)
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    /* vita_glue_trace appends the newline and, with the crash-safe marker in
     * place, commits the line with sceIoWrite + sceIoSyncByFd. */
    vita_glue_trace(line);
#else
    fputs(line, stderr);
    fputc('\n', stderr);
#endif
}

void vitaLogMessage(const char *prefix, const char *utf8)
{
    char head[VITA_FATAL_PREFIX_MAX + 1];
    size_t headLen = 0;

    if (prefix)
        while (prefix[headLen] != '\0' && headLen < VITA_FATAL_PREFIX_MAX) {
            head[headLen] = prefix[headLen];
            ++headLen;
        }
    head[headLen] = '\0';

    if (!utf8)
        utf8 = "(null)";

    /* At least 512 - 1 - 64 bytes of payload per line, so a source line longer
     * than one log line always makes progress. */
    const size_t room = (size_t)VITA_FATAL_LINE_MAX - 1 - headLen;
    size_t budget = VITA_FATAL_LOG_MAX;
    const char *p = utf8;

    for (;;) {
        size_t len = 0;
        size_t offset = 0;

        while (len < budget && p[len] != '\0' && p[len] != '\n')
            ++len;

        /* do/while, not while: an empty source line is a line, and an empty
         * body still says "msgbox:" with nothing after it. */
        do {
            char line[VITA_FATAL_LINE_MAX];
            size_t take = len - offset;

            if (take > room)
                take = room;
            memcpy(line, head, headLen);
            memcpy(line + headLen, p + offset, take);
            line[headLen + take] = '\0';
            vitaFatalEmit(line);
            offset += take;
        } while (offset < len);

        p += len;
        budget -= len;
        if (budget == 0 || *p != '\n')
            break;
        ++p;                    /* the separator itself counts against the cap */
        --budget;
        if (budget == 0 || *p == '\0')
            break;              /* a trailing newline adds no empty tail line */
    }
}

/* ---- last-error report -------------------------------------------------- */

/* Append `src` verbatim. Stops at `cap` (leaving room for one more byte) and
 * returns false; `*len` always describes what was actually written. */
static bool vitaFatalAppend(char *buf, size_t cap, size_t *len, const char *src)
{
    size_t n = *len;

    for (; *src != '\0'; ++src) {
        if (n + 1 >= cap) {
            *len = n;
            return false;
        }
        buf[n++] = *src;
    }
    *len = n;
    return true;
}

/* Same, with every control byte folded to a space: a title carrying a newline
 * must not be able to forge "kind:" or the separator. */
static bool vitaFatalAppendField(char *buf, size_t cap, size_t *len,
                                 const char *src)
{
    size_t n = *len;

    for (; *src != '\0'; ++src) {
        const unsigned char c = (unsigned char)*src;

        if (n + 1 >= cap) {
            *len = n;
            return false;
        }
        buf[n++] = (c < 0x20u || c == 0x7fu) ? ' ' : (char)c;
    }
    *len = n;
    return true;
}

static bool vitaFatalPath(char *out, size_t cap, const char *dir,
                          const char *name)
{
    size_t n = 0;

    if (!vitaFatalAppend(out, cap, &n, dir))
        return false;
    /* "ux0:" is a device root, not a directory that wants a separator. */
    if (n > 0 && out[n - 1] != '/' && out[n - 1] != ':' &&
        !vitaFatalAppend(out, cap, &n, "/"))
        return false;
    if (!vitaFatalAppend(out, cap, &n, name))
        return false;
    out[n] = '\0';
    return true;
}

static bool vitaFatalWriteAll(int fd, const char *data, size_t len)
{
    while (len > 0) {
        const ssize_t written = write(fd, data, len);

        if (written <= 0) {
            if (written < 0 && errno == EINTR)
                continue;
            return false;
        }
        data += (size_t)written;
        len -= (size_t)written;
    }
    return true;
}

static bool vitaFatalPublish(const char *tmpPath, const char *finalPath,
                             const char *backupPath)
{
    // The publication policy (move-aside, publish, failure recoverability)
    // lives in vita_publish, shared by every generation-preserving writer.
    return vita_publish_commit(tmpPath, finalPath, backupPath) == 0;
}

/* A retained .tmp is only worth publishing when the write that made it ran to
 * the end: the magic line, a complete header ending in the separator line,
 * and a final newline (the writer always terminates the text). Anything else
 * is a torn write and must not replace a good report. */
static bool vitaFatalReportComplete(const char *path)
{
    static const char magic[] = VITA_FATAL_MAGIC "\n";
    static const char sep[] = "\n" VITA_FATAL_SEPARATOR "\n";
    char chunk[256];
    size_t seen = 0;
    size_t matched = 0;
    bool separated = false;
    char last = '\0';
    const int fd = open(path, O_RDONLY, 0);

    if (fd < 0)
        return false;
    for (;;) {
        const ssize_t got = read(fd, chunk, sizeof(chunk));

        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            (void)close(fd);
            return got == 0 && separated && last == '\n';
        }
        for (ssize_t i = 0; i < got; ++i) {
            const char c = chunk[i];

            if (seen < sizeof(magic) - 1 && c != magic[seen]) {
                (void)close(fd);
                return false;
            }
            ++seen;
            if (!separated) {
                if (c == sep[matched])
                    ++matched;
                else
                    matched = (c == sep[0]) ? 1 : 0;
                separated = matched == sizeof(sep) - 1;
            }
            last = c;
        }
    }
}

bool vitaWriteLastErrorTo(const char *dir, const char *kind, const char *title,
                          const char *text)
{
    /* One buffer for the process. The fatal path has a single writer by
     * construction -- showExc runs on the RGSS thread, the launcher's own calls
     * run before it exists -- and two simultaneous writers would already be
     * racing for the same .tmp file, which the rename cannot fix. */
    static char header[VITA_FATAL_HEADER_MAX];
    static const char kTail[] = "\n" VITA_FATAL_SEPARATOR "\n";

    char tmpPath[VITA_FATAL_PATH_MAX];
    char finalPath[VITA_FATAL_PATH_MAX];
    char backupPath[VITA_FATAL_PATH_MAX];
    size_t len = 0;
    size_t textLen;
    bool ok;
    int fd;

    if (!dir)
        dir = VITA_FATAL_DIR;
    if (!kind)
        kind = "unknown";
    if (!title)
        title = "";
    if (!text)
        text = "";

    if (!vitaFatalPath(tmpPath, sizeof(tmpPath), dir, VITA_FATAL_TMP_NAME) ||
        !vitaFatalPath(finalPath, sizeof(finalPath), dir, VITA_FATAL_NAME) ||
        !vitaFatalPath(backupPath, sizeof(backupPath), dir, VITA_FATAL_NAME ".bak"))
        return false;

    /* Reserve the separator up front so an absurd title truncates itself
     * instead of pushing the "---" out of the file. */
    const size_t fieldCap = sizeof(header) - (sizeof(kTail) - 1);

    (void)vitaFatalAppend(header, fieldCap, &len, VITA_FATAL_MAGIC "\nkind: ");
    (void)vitaFatalAppendField(header, fieldCap, &len, kind);
    (void)vitaFatalAppend(header, fieldCap, &len, "\ntitle: ");
    (void)vitaFatalAppendField(header, fieldCap, &len, title);
    (void)vitaFatalAppend(header, sizeof(header), &len, kTail);

    // A retained report must leave .tmp before another write can truncate it;
    // a torn one is discarded, never promoted over the last good report.
    if (access(tmpPath, F_OK) == 0) {
        if (!vitaFatalReportComplete(tmpPath)) {
            vitaLogMessage("fatal-report: ",
                           "discarding an incomplete retained report");
            (void)remove(tmpPath);
        } else if (!vitaFatalPublish(tmpPath, finalPath, backupPath)) {
            return false;
        }
    } else if (errno != ENOENT) {
        return false;
    }

    fd = open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;

    textLen = strlen(text);
    ok = vitaFatalWriteAll(fd, header, len);
    if (ok && textLen > 0)
        ok = vitaFatalWriteAll(fd, text, textLen);
    /* Always newline-terminated, so a reader's last getline() is a whole line
     * and a shell `cat` does not swallow the prompt. */
    if (ok && (textLen == 0 || text[textLen - 1] != '\n'))
        ok = vitaFatalWriteAll(fd, "\n", 1);
    const bool complete = ok;
    if (ok && fsync(fd) != 0) {
        vitaLogMessage("fatal-report: ",
                       "fsync failed; attempting publication of completed report");
        ok = false;
    }
    if (close(fd) != 0)
        ok = false;

    if (!complete) {
        (void)remove(tmpPath);
        return false;
    }

    // All bytes were written: publish even when sync/close reported an error.
    const bool published = vitaFatalPublish(tmpPath, finalPath, backupPath);
    return ok && published;
}

bool vitaWriteLastError(const char *kind, const char *title, const char *text)
{
    return vitaWriteLastErrorTo(VITA_FATAL_DIR, kind, title, text);
}
