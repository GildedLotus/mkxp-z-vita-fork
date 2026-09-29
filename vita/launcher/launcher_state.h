// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * launcher_state.h — "put the cursor back where it was".
 *
 * The launcher and the game are separate processes: every launch destroys all
 * in-memory state. One line in one file is the whole persistence layer —
 * the path of the game that was launched last, written before
 * sceAppMgrLoadExec and read back on the next launcher boot.
 *
 * The publication policy (temp, flush, sync, close, publish with backup,
 * read-side fallback) lives in vita/glue/vita_publish.h and is shared with
 * the other generation-preserving writers. Reading tries .bak when the
 * current file is missing or corrupt, but never after an I/O failure.
 * No usable generation means "no preference"; it cannot prevent startup.
 *
 * The stored path is NOT required to be a valid LoadExec argument: it is only
 * matched against a fresh scan, and the path that actually reaches LoadExec is
 * re-validated by launch_args_build_game(). What is rejected here is what
 * could not have been written by this module: empty, over-long, or carrying
 * control bytes.
 *
 * C99, libc only (stdio, string). No allocation, no globals.
 */
#ifndef VITA_LAUNCHER_STATE_H
#define VITA_LAUNCHER_STATE_H

#include <stddef.h>

#include "game_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Read the remembered path into `out` (cap bytes; GAME_SCAN_PATH_MAX is
 * always enough). Trailing CR/LF and surrounding spaces are stripped; only
 * the first line is used.
 *
 * Returns 0 when a usable path was read, -1 otherwise (missing file, empty,
 * too long for `cap`, control bytes, or I/O failure). `out` is NUL-terminated
 * and empty on every failure.
 */
int launcher_state_load(const char *path, char *out, size_t cap);

/*
 * Remember `game_path`. Write and close "<path>.tmp", move a valid `path`
 * to "<path>.bak", then publish the temporary file. A completed temporary
 * file is retained if either rename fails; readers never use .tmp.
 *
 * Returns 0 on success, -1 on a bad argument or any I/O failure. A failure
 * leaves the previous value recoverable at `path` or "<path>.bak". These
 * generations protect replacement failures, not proven power-loss durability.
 */
int launcher_state_save(const char *path, const char *game_path);

/*
 * Index of `game_path` in a scanned list, or -1 if it is not there any more
 * (the card changed, the folder was renamed, the file was never written).
 *
 * Exact match first; then an ASCII case-insensitive match, because ux0: is
 * exFAT — case-insensitive and case-preserving — so the same folder can come
 * back from readdir with different case after a host copy.
 */
int launcher_state_index_of(const char *game_path, const GameEntry *entries,
                            int count);

#ifdef __cplusplus
}
#endif

#endif /* VITA_LAUNCHER_STATE_H */
