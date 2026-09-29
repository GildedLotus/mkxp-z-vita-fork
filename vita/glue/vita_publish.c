// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vita_publish.c — see vita_publish.h.
 */
#include "vita_publish.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int suffix_path(char *out, size_t cap, const char *path, const char *suffix)
{
    size_t len;

    if (!out || !path || !suffix)
        return -1;
    len = strlen(path);
    if (len + strlen(suffix) + 1 > cap)
        return -1;
    memcpy(out, path, len);
    memcpy(out + len, suffix, strlen(suffix) + 1);
    return 0;
}

int vita_publish_tmp_path(char *out, size_t cap, const char *path)
{
    return suffix_path(out, cap, path, ".tmp");
}

int vita_publish_bak_path(char *out, size_t cap, const char *path)
{
    return suffix_path(out, cap, path, ".bak");
}

int vita_publish_finalize(FILE *f)
{
    int failed = 0;

    if (!f)
        return -1;
    if (fflush(f) != 0)
        failed = 1;
    if (!failed && fsync(fileno(f)) != 0)
        failed = 1;
    if (fclose(f) != 0)
        failed = 1;
    return failed ? -1 : 0;
}

int vita_publish_commit(const char *tmp, const char *final, const char *backup)
{
    if (!tmp || !final || !backup)
        return -1;
    if (access(final, F_OK) == 0) {
        /* rename() removes an occupied backup first; the source at
         * `final` survives a failure, so the previous generation is only
         * ever replaced by a strictly newer one. */
        if (rename(final, backup) != 0)
            return -1;
    } else if (errno != ENOENT) {
        return -1;
    }
    /* A failure here leaves the previous generation at `backup` and the
     * completed new one at `tmp`; readers fall back in that order. */
    return rename(tmp, final) == 0 ? 0 : -1;
}

int vita_publish_move(const char *from, const char *to)
{
    if (!from || !to || !to[0] || strcmp(from, to) == 0)
        return -1;
    return rename(from, to) == 0 ? 0 : -1;
}

int vita_publish_shift(const char *from, const char *to)
{
    struct stat st;

    if (!from || !to)
        return -1;
    if (stat(to, &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT)
        return -1;
    return rename(from, to) == 0 ? 0 : -1;
}

int vita_publish_read(const char *path, const char *backup,
                      int (*try_read)(const char *path, void *ctx), void *ctx)
{
    int got;

    if (!path || !try_read)
        return -1;
    got = try_read(path, ctx);
    if (got != 0 || !backup)
        return got;
    return try_read(backup, ctx);
}
