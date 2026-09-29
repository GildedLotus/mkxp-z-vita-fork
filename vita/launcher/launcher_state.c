// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * launcher_state.c — see launcher_state.h.
 */
#include "launcher_state.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "vita_publish.h"

#define STATE_TMP_MAX    (GAME_SCAN_PATH_MAX + 8)

static char lower_ascii(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

static int casecmp_ascii(const char *a, const char *b)
{
    size_t i;

    for (i = 0;; i++) {
        unsigned char ca = (unsigned char)lower_ascii(a[i]);
        unsigned char cb = (unsigned char)lower_ascii(b[i]);

        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (ca == '\0')
            return 0;
    }
}

/* A path this module is willing to hand back or write down. */
static int is_storable(const char *path)
{
    size_t i, len;

    if (!path)
        return 0;
    len = strlen(path);
    if (len == 0 || len + 1 > GAME_SCAN_PATH_MAX)
        return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];

        if (c < 0x20 || c == 0x7F)
            return 0;
    }
    return 1;
}

static int state_read(const char *path, char *out, size_t cap)
{
    char buf[GAME_SCAN_PATH_MAX + 64];
    FILE *f;
    size_t n, len;

    if (!out || cap == 0)
        return -1;
    out[0] = '\0';
    if (!path)
        return -1;

    f = fopen(path, "rb");
    if (!f)
        return errno == ENOENT ? -1 : -2;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    {
        int failed = ferror(f) || (n < sizeof(buf) - 1 && !feof(f));
        if (fclose(f) != 0 || failed)
            return -2;
    }
    buf[n] = '\0';

    /* First line only. */
    {
        char *nl = strchr(buf, '\n');

        if (nl)
            *nl = '\0';
        else if (n == sizeof(buf) - 1)
            return -1; /* The first line did not fit; do not trim a prefix. */
    }
    /* Strip CR and surrounding spaces/tabs. */
    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == ' ' ||
                       buf[len - 1] == '\t'))
        buf[--len] = '\0';
    {
        size_t a = 0;

        while (buf[a] == ' ' || buf[a] == '\t')
            a++;
        if (a > 0) {
            memmove(buf, buf + a, len - a + 1);
            len -= a;
        }
    }

    /*
     * A first line longer than the field is corrupt, not truncatable: half a
     * path would match the wrong folder, so it is rejected outright.
     */
    if (!is_storable(buf) || len + 1 > cap)
        return -1;

    memcpy(out, buf, len + 1);
    return 0;
}

struct load_ctx {
    char *out;
    size_t cap;
};

/* vita_publish_read() contract: 1 usable, 0 absent-or-unusable, -1 I/O. */
static int state_try_load(const char *path, void *v)
{
    struct load_ctx *ctx = (struct load_ctx *)v;
    int result = state_read(path, ctx->out, ctx->cap);

    return result == -2 ? -1 : (result == 0 ? 1 : 0);
}

int launcher_state_load(const char *path, char *out, size_t cap)
{
    char backup[STATE_TMP_MAX];
    struct load_ctx ctx = { out, cap };
    int got;

    if (!path || vita_publish_bak_path(backup, sizeof(backup), path) != 0)
        return -1;
    got = vita_publish_read(path, backup, state_try_load, &ctx);
    return got == 1 ? 0 : -1;
}

int launcher_state_save(const char *path, const char *game_path)
{
    char tmp[STATE_TMP_MAX];
    char backup[STATE_TMP_MAX], previous[GAME_SCAN_PATH_MAX];
    FILE *f;
    size_t len;
    int prior;

    if (!path || !is_storable(game_path))
        return -1;
    if (vita_publish_tmp_path(tmp, sizeof(tmp), path) != 0 ||
        vita_publish_bak_path(backup, sizeof(backup), path) != 0)
        return -1;

    f = fopen(tmp, "wb");
    if (!f)
        return -1;
    len = strlen(game_path);
    if (fwrite(game_path, 1, len, f) != len || fputc('\n', f) == EOF) {
        fclose(f);
        remove(tmp);
        return -1;
    }
    if (vita_publish_finalize(f) != 0) {
        remove(tmp);
        return -1;
    }

    prior = state_read(path, previous, sizeof(previous));
    if (prior == -2)
        return -1; /* An I/O failure is not evidence that the current file is bad. */
    /* A failed publish leaves the previous generation readable as .bak. */
    return vita_publish_commit(tmp, path, backup);
}

int launcher_state_index_of(const char *game_path, const GameEntry *entries,
                            int count)
{
    int i;

    if (!game_path || !game_path[0] || !entries || count <= 0)
        return -1;

    for (i = 0; i < count; i++) {
        if (strcmp(entries[i].path, game_path) == 0)
            return i;
    }
    for (i = 0; i < count; i++) {
        if (casecmp_ascii(entries[i].path, game_path) == 0)
            return i;
    }
    return -1;
}
