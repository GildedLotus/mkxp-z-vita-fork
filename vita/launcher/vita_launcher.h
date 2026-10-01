// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vita_launcher.h — the launcher front end.
 *
 * The core library decides everything that can be decided
 * without a screen: what a game folder is, what its title says, where the
 * cursor was, what a keypress means. This file is the other half — a window,
 * a GLES2 context, a pad, and the loop that ties them to that library.
 *
 * What the caller must have done first, in this order, exactly as
 * src/main.cpp does it (the launcher runs in the same eboot as the player,
 * so it must not invent a second boot sequence):
 *
 *   1. vita_glue_boot(log_path, NULL)            — log, clocks, shader cache path
 *   2. SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER)
 *
 * (src/main.cpp sets SDL_HINT_OPENGL_ES_DRIVER only off-Vita: SDL's Vita
 * backend never reads it.)
 *
 * What this file then owns: TTF_Init, one window, one GL context, the GL set
 * of launcher_gl.c, the canvas of launcher_view.c, joystick 0, and the
 * hand-over through vita_loadexec.c.
 *
 * The GPU budget is the reason the structure looks the way it does. The
 * launcher is not a separate program that exits before the game starts: it
 * IS the game's process, re-executed. On an earlier GL driver every GL
 * object alive when sceAppMgrLoadExec was called was a firmware sync object
 * the player might not get back. The launcher still holds one texture, one
 * VBO and one program, and releases all three before the call; that was
 * measured on hardware.
 *
 * C99 + SDL2 + SDL2_ttf + the launcher core library. No mkxp-z headers, no
 * Ruby, no C++.
 */
#ifndef VITA_LAUNCHER_H
#define VITA_LAUNCHER_H

#include <stddef.h>

#include <SDL2/SDL.h>

#include "last_error.h"
#include "launcher_gl.h"
#include "launcher_view.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defaults, all overridable through VitaLauncherOptions. */
#define VITA_LAUNCHER_GAMES_ROOT "ux0:/data/mkxp-z/games"
#define VITA_LAUNCHER_RTP_ROOT   "ux0:/data/mkxp-z/rtp"
#define VITA_LAUNCHER_FONTS_DIR  "app0:/fonts"
#define VITA_LAUNCHER_STATE_PATH "ux0:/data/mkxp-z/logs/launcher-last.txt"

/*
 * The report the player leaves behind, and where it goes once it has been
 * shown. The first is not a preference: it is VITA_FATAL_DIR "/"
 * VITA_FATAL_NAME from src/vita_fatal.h, the path the engine's writer compiles
 * in; keep the two in step.
 * The second is in the log directory because everything there is what a
 * bug report attaches, so the report a player saw is included without anyone
 * asking for it.
 */
#define VITA_LAUNCHER_ERROR_PATH "ux0:/data/mkxp-z/last-error.txt"
#define VITA_LAUNCHER_PREV_ERROR_PATH \
    "ux0:/data/mkxp-z/logs/last-error.prev.txt"
#define VITA_LAUNCHER_EXEC_PATH  "app0:/eboot.bin"

typedef struct VitaLauncherOptions {
    const char *games_root;  /* scanned by game_scan()            */
    const char *rtp_root;    /* rtp/XP, rtp/VX, rtp/VXAce         */
    const char *fonts_dir;   /* enumerated for .ttf/.otf/.ttc     */
    const char *state_path;  /* launcher-last.txt                 */
    const char *error_path;  /* last-error.txt, shown once        */
    const char *prev_error_path; /* where it is rotated to        */
    const char *exec_path;   /* app0:/eboot.bin                   */

    /* One-line log sink. NULL means vita_glue_trace on the device and
     * silence anywhere else. */
    LauncherTraceFn trace;

    /*
     * Two seams for a diagnostic harness, both NULL in the product launcher. They exist
     * so the sync-object headroom canary can be taken twice with the SAME
     * objects held — a cold number and a hand-over number are only
     * comparable if the launcher's texture, VBO, program and flip buffers
     * are live for both.
     *
     *   post_session_open  right after the window, context and GL set exist
     *   pre_loadexec       immediately before the teardown that precedes
     *                      sceAppMgrLoadExec, everything still live
     */
    void (*post_session_open)(void *ctx);
    void (*pre_loadexec)(void *ctx);
    void *pre_loadexec_ctx; /* passed to both */
} VitaLauncherOptions;

/* Fill `opt` with the VITA_LAUNCHER_* defaults above and no hooks. */
void vita_launcher_options_default(VitaLauncherOptions *opt);

/*
 * Window, context, GL set, canvas, fonts and pad — everything that outlives
 * one screen. Exposed so that a diagnostic harness can draw a message screen of its
 * own on the identical GL set: a headroom measurement is only meaningful if
 * both phases hold the same objects.
 */
typedef struct VitaLauncherSession {
    SDL_Window *window;
    SDL_GLContext gl_ctx;
    LauncherGL gl;
    LauncherView view;
    SDL_Joystick *joystick;
    LauncherTraceFn trace;
    int ttf_ready;
    int presents;
    unsigned int retry_started;
    unsigned int retry_delay;
} VitaLauncherSession;

/*
 * ES 2.0 + double buffer, a 960x544 SDL_WINDOW_OPENGL |
 * SDL_WINDOW_FULLSCREEN_DESKTOP window, SDL_GL_CreateContext on the calling
 * (main) thread, swap interval 1, then launcher_gl_init and
 * launcher_view_init — and nothing else. The three SDL shortcuts avoided
 * here (a 2D renderer, a window surface, the simple message box) are each
 * either a second GL context or a software framebuffer, and the SDL2
 * built for this device hands out neither.
 *
 * Returns 1 on success. On failure the reason is traced and the session is
 * closed, so the caller may report and exit.
 */
int vita_launcher_session_open(VitaLauncherSession *s,
                               const VitaLauncherOptions *opt);

/* Upload the canvas (whole level) and draw one quad. `upload` may be 0 to
 * redraw the level already on the GPU — which is what a second present for
 * the other flip buffer needs, and it is why a still screen uploads nothing. */
void vita_launcher_session_present(VitaLauncherSession *s, int upload);

/* Release the GL set, the context, the window, the fonts, the canvas, the
 * pad and TTF. Idempotent. Does NOT call SDL_Quit — the caller owns SDL. */
void vita_launcher_session_close(VitaLauncherSession *s);

/*
 * Read, log and consume `error_path` — the whole of the launcher's side of
 *
 *
 * The parse, the bounds and the rotation live in last_error.c, which is C99
 * with no SDL and is tested on the host; this function is the seam that adds
 * the log lines. Every report is written into the current run's log (kind,
 * title and up to LAST_ERROR_LINES_MAX body lines, one trace line each)
 * BEFORE it is shown, so the diagnosis survives even when the screen cannot
 * say it — no Japanese face, no readable font, or nobody watching.
 *
 * A successful rename consumes the report. Read or rotation failures retain
 * the pending files for a later boot and set `out->io_error`.
 *
 * Returns 1 when there is a report or an I/O notice to show; `out` carries
 * the parsed report and retention status. Returns 0 when neither is needed.
 */
int vita_launcher_take_last_error(const char *error_path,
                                  const char *prev_error_path,
                                  LastErrorReport *out, LauncherTraceFn trace);

/*
 * Run the launcher until it hands the process to a game. Returns only when
 * it did not: 0 if the user could not be shown anything (nothing to run),
 * or a negative value if the session could not be built.
 */
int vita_launcher_run(const VitaLauncherOptions *opt);

#ifdef __cplusplus
}
#endif

#endif /* VITA_LAUNCHER_H */
