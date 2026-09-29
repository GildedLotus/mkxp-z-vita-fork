// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vita_loadexec.h — handing the process over.
 *
 * The launcher and the game are the same eboot. Starting a game is not a
 * fork or a dlopen: it is sceAppMgrLoadExec("app0:/eboot.bin", {"--game",
 * "<path>", NULL}, NULL), which replaces this process. Coming back is the
 * same call with argv == NULL. The mechanism works on hardware both from a
 * process with no GL context and with the GL driver loaded and a live GLES2
 * context.
 *
 * Three things have to be true at the instant of the call, and the order is
 * the whole content of this module:
 *
 *  1. The argv strings must outlive the call. The kernel reads them while it
 *     tears this process down, so they live in static storage here, built by
 *     launch_args_build_game(). A stack buffer is a use-after-free that
 *     happens to work most of the time.
 *  2. The bookmark must already be on the card. launcher-last.txt is what
 *     puts the cursor back on the game the user just played, and after the
 *     call there is no process left to write it.
 *  3. Everything buffered must be on the card too. fflush(NULL) then fsync
 *     of stdout and stderr: the log is the only evidence, and a
 *     breadcrumb still sitting in a FILE buffer when the process is replaced
 *     never existed.
 *
 * The GL/SDL teardown between 2 and 3 is the caller's, through the shutdown
 * hook below — this file must not include SDL. That keeps it host-compilable
 * and keeps the launcher's GL set owned by the file that made it.
 *
 * C99; libc + psp2/appmgr.h + the launcher core library. No SDL, no GL, no
 * mkxp-z, no Ruby.
 */
#ifndef VITA_LOADEXEC_H
#define VITA_LOADEXEC_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VITA_LAUNCHER_TRACE_FN_DEFINED
#define VITA_LAUNCHER_TRACE_FN_DEFINED
typedef void (*LauncherTraceFn)(const char *msg);
#endif

/* Returned only when the hand-over did not happen. A successful call never
 * returns at all. */
#define VITA_LOADEXEC_ERR_ARGS   (-1) /* path rejected by launch_path_is_valid */
#define VITA_LOADEXEC_ERR_CONFIG (-2) /* vita_loadexec_configure not called */
#define VITA_LOADEXEC_ERR_HOST   (-3) /* built for a host: no sceAppMgrLoadExec */

typedef struct VitaLoadExecConfig {
    /* The eboot to re-run — "app0:/eboot.bin". NULL is not accepted. */
    const char *exec_path;

    /* Where to remember the game being launched, e.g.
     * "ux0:/data/mkxp-z/logs/launcher-last.txt". NULL skips the bookmark.
     * Only the --game direction writes it. */
    const char *state_path;

    /*
     * Last call made while the GL set, the window and the SDL flip buffers
     * are all still live — after the bookmark is on the card, before the
     * teardown. A diagnostic harness prints its sync-object headroom
     * canary here: the number has to be taken with the launcher's own objects still held, or it answers
     * a question nobody asked. May be NULL.
     */
    void (*pre_exec)(void *ctx);

    /*
     * Release everything the caller owns: texture/VBO/program,
     * SDL_GL_DeleteContext, SDL_DestroyWindow, TTF_Quit, SDL_Quit. Runs
     * after pre_exec and before the flush. May be NULL (the kernel would
     * reclaim it all anyway; doing it explicitly is what keeps the driver's
     * firmware sync pool from being a question at the hand-over).
     */
    void (*shutdown)(void *ctx);

    void *ctx;              /* passed to both hooks */
    LauncherTraceFn trace;  /* may be NULL */
} VitaLoadExecConfig;

/* Copy `cfg` into this module's static state. Call once, before either
 * launch function. A NULL or exec_path-less config disarms both. */
void vita_loadexec_configure(const VitaLoadExecConfig *cfg);

/*
 * Replace this process with the same eboot running `game_path`.
 *
 * `exec_name` is NULL or empty for a Game.ini game; otherwise it is the exact
 * basename from the scan and the argv gains "--execName <name>",
 * which the player's config layer applies as execName so the engine opens
 * <name>.ini instead of Game.ini.
 *
 * Traces "launcher: LoadExec --game '<path>'" before the call, and
 * "launcher: LoadExec FAILED rc=0x%08x" if it comes back. Returns only on
 * failure: a negative SceAppMgrErrorCode, or one of the VITA_LOADEXEC_ERR_*
 * codes above.
 */
int vita_loadexec_game(const char *game_path, const char *exec_name);

/*
 * Replace this process with the same eboot and no arguments — the game's way
 * back to the launcher. argv is NULL, which is exactly what a LiveArea boot
 * looks like (argc == 1, argv[0] == ""), so the relaunched process takes the
 * launcher branch of launch_args_parse.
 */
int vita_loadexec_launcher(void);

/*
 * The argv this module WOULD pass for `game_path` and `exec_name`, without
 * calling anything. Fills `out` with pointers into the same static storage
 * the real call uses and returns 0, or returns -1 and sets out[0] = NULL.
 * Exists for host builds and for a dry-run trace; calling it overwrites
 * the storage, so do not interleave it with a real launch.
 */
int vita_loadexec_preview_game_argv(const char *game_path,
                                    const char *exec_name, char *out[5]);

#ifdef __cplusplus
}
#endif

#endif /* VITA_LOADEXEC_H */
