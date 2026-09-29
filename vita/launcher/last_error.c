// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * last_error.c — see last_error.h.
 */
#include "last_error.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "utf8_util.h"
#include "vita_publish.h"

/* Header lines read before giving up on finding the "---" separator. The
 * producer writes exactly three (magic, kind, title); a handful of slack
 * costs nothing and keeps a future field from swallowing the whole body. */
#define HEADER_LINES_MAX 8

/* ---- small helpers ------------------------------------------------------ */

/* Anything a reader would actually see. The sanitiser folds control bytes to
 * spaces, so "not empty" and "worth showing" are different questions. */
static int visible(const char *s)
{
    for (; *s != '\0'; s++) {
        if ((unsigned char)*s > ' ')
            return 1;
    }
    return 0;
}

/*
 * Length of the line starting at `p`, excluding its terminator; `*next`
 * receives the start of the following line, or `end` at the last one.
 *
 * A CRLF file loses its CR here rather than in the renderer, where a folded
 * control byte would be a trailing space on every line.
 */
static size_t line_at(const char *p, const char *end, const char **next)
{
    const char *q = p;

    while (q < end && *q != '\n')
        q++;
    *next = (q < end) ? q + 1 : end;
    if (q > p && q[-1] == '\r')
        return (size_t)(q - 1 - p);
    return (size_t)(q - p);
}

/*
 * Append `s` at `used`, cutting on a code point boundary if it does not fit.
 * `out` is NUL-terminated on every path; the caller has already checked that
 * cap > 0 and written at least the terminator.
 */
static size_t append(char *out, size_t cap, size_t used, const char *s)
{
    if (!s || used + 1 >= cap)
        return used;
    return used + utf8_truncate_bytes(s, out + used, cap - used);
}

/*
 * "<name>" at the start of `line` (the name carries its colon) -> the rest of
 * the line, sanitised, into `out`. The space after the colon is optional so a
 * hand-written file works too.
 */
static int field_after(const char *line, size_t len, const char *name,
                       char *out, size_t cap)
{
    size_t n = strlen(name);

    if (len < n || memcmp(line, name, n) != 0)
        return 0;
    while (n < len && (line[n] == ' ' || line[n] == '\t'))
        n++;
    utf8_sanitize(line + n, len - n, out, cap);
    return 1;
}

/* ---- parsing ------------------------------------------------------------ */

/*
 * The body, line by line, so the newlines the backtrace needs survive the
 * sanitiser that folds every other control byte. Stops at the line cap or
 * when the buffer is full; `out->truncated` says which side of the cap the
 * rest of the report is on.
 */
static void copy_text(const char *p, const char *end, LastErrorReport *out)
{
    size_t used = 0;
    int lines = 0;
    int full = 0;

    out->text[0] = '\0';
    while (p < end && lines < LAST_ERROR_LINES_MAX) {
        const char *next;
        size_t len = line_at(p, end, &next);

        if (lines > 0) {
            if (used + 2 > sizeof(out->text))
                break;
            out->text[used++] = '\n';
            out->text[used] = '\0';
        }
        if (used + 1 >= sizeof(out->text))
            break;
        used += utf8_sanitize(p, len, out->text + used,
                              sizeof(out->text) - used);
        /* A full sink is the only way the sanitiser stops early, and a line
         * cut in the middle is as truncated as a line never reached: the
         * source pointer alone cannot tell, because it has already stepped
         * over the whole line. A report that lands exactly on the cap is
         * called truncated too, which costs one honest sentence. */
        if (used + 1 >= sizeof(out->text))
            full = 1;
        lines++;
        p = next;
    }
    /* The producer always terminates its text, and a line that only just ran
     * out of room leaves its separator behind: either way a trailing blank
     * line would push the closing paragraph down for nothing. */
    while (used > 0 && out->text[used - 1] == '\n')
        out->text[--used] = '\0';
    if (p < end || full)
        out->truncated = 1;
}

int last_error_parse(const char *data, size_t len, LastErrorReport *out)
{
    const char *p, *end, *next;
    size_t magic;
    int header;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!data || len == 0)
        return 0;
    if (len > LAST_ERROR_READ_MAX)
        len = LAST_ERROR_READ_MAX;

    out->bytes = (unsigned long)len;
    p = data;
    end = data + len;

    magic = strlen(LAST_ERROR_MAGIC);
    if ((size_t)(end - p) >= magic &&
        memcmp(p, LAST_ERROR_MAGIC, magic) == 0 &&
        line_at(p, end, &next) == magic) {
        out->v1 = 1;
        p = next;
    }

    /* Header fields are matched by name, not by position: a v1 file this
     * launcher does not fully understand must still yield its kind and its
     * title, and an unknown line must not shift the ones that follow. */
    if (out->v1) {
        const char *body = p;
        int found = 0;

        for (header = 0; p < end && header < HEADER_LINES_MAX; header++) {
            size_t line = line_at(p, end, &next);

            if (line == strlen(LAST_ERROR_SEPARATOR) &&
                memcmp(p, LAST_ERROR_SEPARATOR, line) == 0) {
                p = next;
                found = 1;
                break;
            }
            if (!field_after(p, line, "kind:", out->kind, sizeof(out->kind)))
                (void)field_after(p, line, "title:", out->title,
                                  sizeof(out->title));
            p = next;
        }
        /* No separator: the header ran off the end of a truncated or foreign
         * file. Everything after the magic is body then — the fields picked
         * up above are still right, and nothing is dropped on the floor. */
        if (!found)
            p = body;
    }

    copy_text(p, end, out);
    return (visible(out->text) || visible(out->title) || visible(out->kind))
               ? 1
               : 0;
}

/* ---- consuming ---------------------------------------------------------- */

/* "<dir of path>/last-error.prev.txt". */
static int sibling_prev(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');
    size_t dir = slash ? (size_t)(slash - path) + 1 : 0;
    size_t name = strlen(LAST_ERROR_PREV_NAME);

    if (dir + name + 1 > cap)
        return 0;
    memcpy(out, path, dir);
    memcpy(out + dir, LAST_ERROR_PREV_NAME, name + 1);
    return 1;
}

static void rotate(const char *path, const char *prev_path,
                   LastErrorReport *out)
{
    char fallback[LAST_ERROR_PATH_MAX];

    out->kept[0] = '\0';
    if (prev_path && strlen(prev_path) + 1 <= sizeof(out->kept) &&
        vita_publish_move(path, prev_path) == 0) {
        memcpy(out->kept, prev_path, strlen(prev_path) + 1);
        return;
    }
    /* The log directory is made by the glue's ensure_log_dirs() long before
     * the launcher runs, so this is only reached on a card that has lost it —
     * and beside the report is still a place a USB pull will find. */
    if (sibling_prev(path, fallback, sizeof(fallback)) &&
        vita_publish_move(path, fallback) == 0) {
        memcpy(out->kept, fallback, strlen(fallback) + 1);
        return;
    }
    /* Re-showing a report is preferable to deleting its only surviving copy. */
    if (strlen(path) + 1 <= sizeof(out->kept))
        memcpy(out->kept, path, strlen(path) + 1);
    out->io_error = 1;
}

/* Validate the envelope separately from the deliberately tolerant renderer.
 * One byte after the separator is required even for the writer's empty body. */
static int complete_header(const char *raw, size_t n)
{
    const char *p = raw, *end = raw + n, *next;
    size_t magic = strlen(LAST_ERROR_MAGIC);
    int fields = 0, header;

    if (n <= magic || memcmp(raw, LAST_ERROR_MAGIC, magic) != 0 ||
        line_at(p, end, &next) != magic)
        return 0;
    p = next;
    for (header = 0; p < end && header < HEADER_LINES_MAX; header++) {
        size_t len = line_at(p, end, &next);
        if (len == strlen(LAST_ERROR_SEPARATOR) &&
            memcmp(p, LAST_ERROR_SEPARATOR, len) == 0)
            return fields == 3 && next < end;
        if (len >= 5 && memcmp(p, "kind:", 5) == 0)
            fields |= 1;
        if (len >= 6 && memcmp(p, "title:", 6) == 0)
            fields |= 2;
        p = next;
    }
    return 0;
}

int last_error_read(const char *path, LastErrorReport *out)
{
    char raw[LAST_ERROR_HEADER_MAX + 1];
    FILE *f;
    size_t n;
    int got, failed, more = 0, terminated;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!path || !path[0])
        return 0;

    f = fopen(path, "rb");
    if (!f) {
        out->io_error = errno != ENOENT;
        return out->io_error ? -1 : 0;
    }
    n = fread(raw, 1, sizeof(raw), f);
    failed = ferror(f) || (n < sizeof(raw) && !feof(f));
    if (!failed && n == sizeof(raw)) {
        more = fgetc(f) != EOF;
        failed = ferror(f) || (!more && !feof(f));
    }
    terminated = n > 0 && raw[n - 1] == '\n';
    if (!failed && more) {
        /* The preview cap is not an interrupted write. Check the actual tail
         * without streaming an unbounded backtrace through the launcher. */
        failed = fseek(f, -1, SEEK_END) != 0;
        if (!failed) {
            int tail = fgetc(f);
            failed = tail == EOF || ferror(f);
            terminated = tail == '\n';
        }
    }
    if (fclose(f) != 0)
        failed = 1;
    if (failed) {
        out->io_error = 1;
        return -1;
    }

    if (!terminated || memchr(raw, '\0', n) || !complete_header(raw, n))
        return 0;
    got = last_error_parse(raw, n, out);
    if (more || n > LAST_ERROR_READ_MAX)
        out->truncated = 1;
    if (!got)
        memset(out, 0, sizeof(*out));
    return got;
}

struct take_ctx {
    LastErrorReport *out;
    const char *selected;
};

/* vita_publish_read() contract: 1 usable, 0 absent-or-invalid, -1 I/O. */
static int take_try_read(const char *path, void *v)
{
    struct take_ctx *ctx = (struct take_ctx *)v;
    int got = last_error_read(path, ctx->out);

    if (got > 0)
        ctx->selected = path;
    return got;
}

int last_error_take(const char *path, const char *prev_path,
                    LastErrorReport *out)
{
    char backup[LAST_ERROR_PATH_MAX];
    struct take_ctx ctx;
    int got;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    ctx.out = out;
    ctx.selected = path;
    if (!path || !path[0] ||
        vita_publish_bak_path(backup, sizeof(backup), path) != 0)
        return 0;
    got = vita_publish_read(path, backup, take_try_read, &ctx);
    if (got <= 0)
        return 0;
    rotate(ctx.selected, prev_path, out);
    if (out->kept[0] && strcmp(out->kept, ctx.selected) != 0 &&
        strcmp(out->kept, backup) != 0)
        (void)remove(backup);
    return got;
}

/* ---- presentation ------------------------------------------------------- */

static const char *kind_sentence(const char *kind)
{
    if (strcmp(kind, LAST_ERROR_KIND_SCRIPT) == 0)
        return "A script raised an error.";
    if (strcmp(kind, LAST_ERROR_KIND_INIT) == 0)
        return "The engine could not start.";
    if (strcmp(kind, LAST_ERROR_KIND_STUCK) == 0)
        return "The game stopped responding.";
    return NULL;
}

size_t last_error_heading(const LastErrorReport *r, char *out, size_t cap)
{
    size_t used;

    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!r)
        return 0;

    used = append(out, cap, 0, "Last run failed");
    if (visible(r->title)) {
        used = append(out, cap, used, ": ");
        used = append(out, cap, used, r->title);
    }
    return used;
}

size_t last_error_body(const LastErrorReport *r, char *out, size_t cap)
{
    const char *sentence;
    size_t used = 0;

    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!r)
        return 0;

    sentence = kind_sentence(r->kind);
    if (sentence) {
        used = append(out, cap, used, sentence);
    } else if (visible(r->kind)) {
        /* An unrecognised kind is shown rather than hidden: this launcher is
         * older than that report, and the word itself is the clue. */
        used = append(out, cap, used, "The player reported: ");
        used = append(out, cap, used, r->kind);
    }
    if (used > 0 && visible(r->text))
        used = append(out, cap, used, "\n\n");
    used = append(out, cap, used, r->text);

    /* The closing paragraph is the one line that is always there, so it must
     * not open the screen with two blank rows when everything above it was. */
    if (used > 0)
        used = append(out, cap, used, "\n\n");
    if (r->truncated)
        used = append(out, cap, used, "Shortened for this screen. ");
    if (visible(r->kept)) {
        used = append(out, cap, used, "The full report is in ");
        used = append(out, cap, used, r->kept);
    } else {
        used = append(out, cap, used,
                      "The report could not be kept; the log has it.");
    }
    return used;
}

int last_error_line(const LastErrorReport *r, int index, char *out, size_t cap)
{
    const char *p, *end, *next;
    int i;

    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!r || index < 0)
        return 0;

    p = r->text;
    end = r->text + strlen(r->text);
    for (i = 0; p < end; i++) {
        size_t len = line_at(p, end, &next);

        if (i == index) {
            utf8_sanitize(p, len, out, cap);
            return 1;
        }
        p = next;
    }
    return 0;
}
