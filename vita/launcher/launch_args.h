// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * launch_args.h — the argv contract between the launcher and the game
 * process.
 *
 * Both processes are the same eboot. The launcher calls
 *
 *     sceAppMgrLoadExec("app0:/eboot.bin", {"--game", "<abs path>", NULL}, NULL)
 *
 * and the game calls it back with argv = NULL. Two facts from hardware
 * (LoadExec argc) fix the
 * shape of this header:
 *
 *  - The kernel supplies its own EMPTY argv[0]. Whatever the caller passes
 *    starts at argv[1]. So the builder must NOT emit a program name, and the
 *    parser must not give argv[1] any special meaning of its own — mkxp-z's
 *    Config::read already reads argv[1] as "debug"/"test"/"btest".
 *  - A LiveArea boot is argc == 1 with argv[0] == "".
 *
 * Hence: the parser scans ALL of argv[1..], accepts "--game <path>" and
 * "--game=<path>", and the first occurrence wins.
 *
 * A path is only accepted if it is a device-absolute "<dev>:/…" path of at
 * most GAME_SCAN_PATH_MAX - 1 bytes with no ".." component and no control
 * bytes. That is not decoration: the string is handed to another process and
 * then to fopen/chdir, and the launcher must never turn a malformed card into
 * a way out of ux0:/data/mkxp-z/games.
 *
 * C99, libc only. No allocation, no globals.
 */
#ifndef VITA_LAUNCH_ARGS_H
#define VITA_LAUNCH_ARGS_H

#include <stddef.h>

#include "game_scan.h" /* GAME_SCAN_PATH_MAX — one budget, one definition */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAUNCH_MODE_DEFAULT = 0, /* no --game: launcher (or a pinned player) */
    LAUNCH_MODE_GAME    = 1, /* --game <valid path> */
    LAUNCH_MODE_INVALID = -1 /* --game present but its path is unusable */
} LaunchMode;

typedef struct LaunchRequest {
    LaunchMode mode;
    char game_path[GAME_SCAN_PATH_MAX]; /* empty unless mode == GAME */
} LaunchRequest;

/*
 * Storage the caller must provide for launch_args_build_game(). The strings
 * must outlive the sceAppMgrLoadExec call, so this belongs in static memory.
 * "--game\0" + path + "\0" fits in GAME_SCAN_PATH_MAX + 8; "--execName\0" +
 * name + "\0" adds GAME_SCAN_EXEC_MAX + 12 more.
 */
#define LAUNCH_ARGS_STORAGE_MAX \
    (GAME_SCAN_PATH_MAX + GAME_SCAN_EXEC_MAX + 20)

/*
 * Parse argv. Never reads argv[0]; stops at the first NULL element. `out` is
 * always fully written (zeroed first), so the caller can ignore the return
 * value and read out->mode.
 */
LaunchMode launch_args_parse(int argc, char *const argv[], LaunchRequest *out);

/*
 * 1 if `path` may be handed to another process: "<dev>:/…" where <dev> starts
 * with a letter and continues with letters or digits, at most
 * GAME_SCAN_PATH_MAX - 1 bytes, no "." or ".." path component, no byte below
 * 0x20 and no 0x7F. Otherwise 0.
 */
int launch_path_is_valid(const char *path);

/*
 * 1 if `name` may be handed to another process as an --execName value
 *: nonempty, at most GAME_SCAN_EXEC_MAX - 1 bytes, no byte
 * below 0x20, no 0x7F and no separator, because the engine concatenates it
 * onto the game folder before opening <name>.ini and the archive. Otherwise 0.
 */
int launch_exec_name_is_valid(const char *name);

/*
 * Build {"--game", game_path, NULL} into `argv_out`, or, when `exec_name` is
 * nonempty and valid, {"--game", game_path, "--execName", exec_name, NULL},
 * with every string living in `storage` (cap bytes,
 * LAUNCH_ARGS_STORAGE_MAX is always enough). No program name: the kernel
 * prepends its own empty argv[0].
 *
 * Returns 0 on success and -1 if the path or the name is invalid or the
 * storage too small; on failure argv_out[0] is set to NULL.
 */
int launch_args_build_game(const char *game_path, const char *exec_name,
                           char *storage, size_t cap, char *argv_out[5]);

#ifdef __cplusplus
}
#endif

#endif /* VITA_LAUNCH_ARGS_H */
