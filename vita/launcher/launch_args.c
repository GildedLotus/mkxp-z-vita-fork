// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * launch_args.c — see launch_args.h.
 */
#include "launch_args.h"

#include <string.h>

#define GAME_FLAG     "--game"
#define GAME_FLAG_LEN 6
#define EXEC_FLAG     "--execName"
#define EXEC_FLAG_LEN 10

int launch_path_is_valid(const char *path)
{
    size_t i, len, comp;

    if (!path)
        return 0;
    len = strlen(path);
    if (len == 0 || len + 1 > GAME_SCAN_PATH_MAX)
        return 0;

    /* No control bytes anywhere: this string crosses a process boundary. */
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];

        if (c < 0x20 || c == 0x7F)
            return 0;
    }

    /* "<dev>:/" prefix, <dev> = letter followed by letters or digits. */
    if (!((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')))
        return 0;
    i = 1;
    while (path[i] && path[i] != ':') {
        char c = path[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9')))
            return 0;
        i++;
    }
    if (path[i] != ':' || path[i + 1] != '/')
        return 0;

    /* No "." or ".." component after the device root. */
    comp = i + 2;
    for (;;) {
        size_t end = comp;

        while (path[end] && path[end] != '/')
            end++;
        if (end - comp == 1 && path[comp] == '.')
            return 0;
        if (end - comp == 2 && path[comp] == '.' && path[comp + 1] == '.')
            return 0;
        if (!path[end])
            break;
        comp = end + 1;
    }
    return 1;
}

int launch_exec_name_is_valid(const char *name)
{
    size_t i, len;

    if (!name)
        return 0;
    len = strlen(name);
    if (len == 0 || len + 1 > GAME_SCAN_EXEC_MAX)
        return 0;

    /* Same process-boundary rule as the path, plus no separator: the engine
     * joins this onto the game folder for every ini/archive open. */
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\')
            return 0;
    }
    return 1;
}

LaunchMode launch_args_parse(int argc, char *const argv[], LaunchRequest *out)
{
    int i;

    if (!out)
        return LAUNCH_MODE_INVALID;
    memset(out, 0, sizeof(*out));
    out->mode = LAUNCH_MODE_DEFAULT;

    if (!argv || argc <= 1)
        return out->mode;

    /*
     * argv[0] is the kernel's own empty string after LoadExec and the program
     * path on a host run; it is never a flag, so the scan starts at 1.
     */
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *value = NULL;

        if (!a)
            break; /* a NULL element ends the vector */

        if (strcmp(a, GAME_FLAG) == 0) {
            if (i + 1 < argc)
                value = argv[i + 1];
        } else if (strncmp(a, GAME_FLAG "=", GAME_FLAG_LEN + 1) == 0) {
            value = a + GAME_FLAG_LEN + 1;
        } else {
            continue;
        }

        /* First occurrence wins, valid or not. */
        if (value && launch_path_is_valid(value)) {
            memcpy(out->game_path, value, strlen(value) + 1);
            out->mode = LAUNCH_MODE_GAME;
        } else {
            out->game_path[0] = '\0';
            out->mode = LAUNCH_MODE_INVALID;
        }
        return out->mode;
    }

    return out->mode;
}

int launch_args_build_game(const char *game_path, const char *exec_name,
                           char *storage, size_t cap, char *argv_out[5])
{
    size_t plen, used;
    int with_exec;

    if (!argv_out)
        return -1;
    argv_out[0] = NULL;
    argv_out[1] = NULL;
    argv_out[2] = NULL;
    argv_out[3] = NULL;
    argv_out[4] = NULL;

    if (!storage || !launch_path_is_valid(game_path))
        return -1;

    /* An empty name means "Game" and stays silent; a nonempty one must be
     * a basename the engine can open, or the launch is refused whole. */
    with_exec = exec_name && exec_name[0];
    if (with_exec && !launch_exec_name_is_valid(exec_name))
        return -1;

    plen = strlen(game_path);
    used = GAME_FLAG_LEN + 1 + plen + 1;
    if (with_exec)
        used += EXEC_FLAG_LEN + 1 + strlen(exec_name) + 1;
    if (used > cap)
        return -1;

    memcpy(storage, GAME_FLAG, GAME_FLAG_LEN + 1);
    memcpy(storage + GAME_FLAG_LEN + 1, game_path, plen + 1);
    argv_out[0] = storage;                       /* "--game" */
    argv_out[1] = storage + GAME_FLAG_LEN + 1;   /* the path */
    if (with_exec) {
        size_t off = GAME_FLAG_LEN + 1 + plen + 1;

        memcpy(storage + off, EXEC_FLAG, EXEC_FLAG_LEN + 1);
        memcpy(storage + off + EXEC_FLAG_LEN + 1, exec_name,
               strlen(exec_name) + 1);
        argv_out[2] = storage + off;             /* "--execName" */
        argv_out[3] = storage + off + EXEC_FLAG_LEN + 1; /* the name */
    }
    /* argv_out was nulled slot by slot, so the terminator is already there
     * at slot 2 (no exec name) or slot 4. */
    return 0;
}
