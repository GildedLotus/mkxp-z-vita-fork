// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vita_boot.h — one eboot, three modes.
 *
 * The launcher and the player are the same binary. Which one this process is
 * depends on two things and nothing else: the arguments the kernel handed it,
 * and the root configuration the VPK shipped.
 *
 *   LAUNCHER  no usable --game and no pinned root config. Draw the list with
 *             SDL2 only; Ruby is never loaded, no Config is read, no audio
 *             device is opened. Starting a game is sceAppMgrLoadExec of the
 *             same eboot with {"--game", "<path>"}.
 *   GAME      --game <valid path>. The full player, and when it ends the
 *             process LoadExecs back to the launcher with argv = NULL.
 *   PINNED    no --game, but the root config names a gameFolder or a
 *             customScript: a player packaged for one game. It exits to
 *             LiveArea.
 *
 * Why a mode at all, instead of two binaries: MRI cannot be torn down and
 * re-initialised inside one process, so "quit to the launcher" has to be a
 * process replacement. And a second eboot would double a 12.6 MB package.
 *
 * ---- the crash breadcrumb --------------------------------------------------
 *
 * A game the system kills never returns from main(): vita_boot_finish() never
 * runs, no LoadExec happens, and the user lands back on LiveArea with no
 * explanation whatsoever. That is exactly what a Blank Dream run looks like
 * today. So GAME mode writes one line to
 *
 *     ux0:/data/mkxp-z/launch-in-progress.txt
 *
 * as soon as its log is open, and deletes it after confirmed cleanup just
 * before the hand-over back.
 * A launcher that finds the file on its next start knows the previous game
 * died, says so, names it, points at the log that is still on the card
 * (rotation is keyed per log path — vita_glue.c's rotate_logs() — so launcher
 * boots do not consume the game's generations). It deletes the breadcrumb
 * only after publishing a report or finding an existing report generation;
 * read or publication failures leave it for a later boot to retry.
 *
 * The engine's teardown watchdog bounds shutdown after a termination request.
 * It cannot guarantee a report after a kernel kill, so an unfinished cleanup
 * leaves this breadcrumb on the card. Acknowledgement alone is insufficient.
 *
 * ---- last-error.txt uses the shared report format --------------------------
 *
 * Everything this file reports to the user goes through vitaWriteLastError()
 * in src/vita_fatal.cpp. This wrapper never formats that report itself. The
 * engine watchdog also has a bounded emergency writer; the launcher consumes
 * reports by recoverable rotation. See vita/docs/launcher.md.
 *
 * C API, C++ implementation: the root configuration is parsed with the
 * engine's own json5pp, so "is this player pinned" is decided by the same
 * parser that will later read the same file.
 */

#ifndef VITA_BOOT_H
#define VITA_BOOT_H

#include "game_scan.h" /* GAME_SCAN_PATH_MAX — one path budget, one definition */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- modes -------------------------------------------------------------- */

typedef enum {
    VITA_BOOT_MODE_LAUNCHER = 0,
    VITA_BOOT_MODE_GAME     = 1,
    VITA_BOOT_MODE_PINNED   = 2
} VitaBootMode;

/* Why that mode. Reported in the decision log line and, for the two failure
 * reasons, on the launcher's message screen. */
typedef enum {
    VITA_BOOT_REASON_NO_ARGS       = 0, /* nothing on the command line     */
    VITA_BOOT_REASON_ARG_GAME      = 1, /* --game <valid path>             */
    VITA_BOOT_REASON_ARG_INVALID   = 2, /* --game present, path rejected   */
    VITA_BOOT_REASON_CONFIG_PINNED = 3, /* gameFolder or customScript set  */
    VITA_BOOT_REASON_CONFIG_BROKEN = 4  /* root config unreadable/unparsed */
} VitaBootReason;

typedef struct VitaBootDecision {
    int mode;   /* VitaBootMode   */
    int reason; /* VitaBootReason */

    /* The game to run. Empty unless mode == VITA_BOOT_MODE_GAME. */
    char game_path[GAME_SCAN_PATH_MAX];

    /* The rejected --game value, control bytes folded to '?' and truncated.
     * Empty unless reason == VITA_BOOT_REASON_ARG_INVALID. */
    char arg_value[GAME_SCAN_PATH_MAX];

    /* "vitaGamesRoot" from the root config, when it is a usable device path.
     * Empty otherwise, and the launcher then uses its own default. This is
     * how a packaged root mkxp.json reaches the front end
     * without a second configuration file. */
    char games_root[GAME_SCAN_PATH_MAX];
} VitaBootDecision;

/* ---- the return code that means "do not relaunch" ------------------------ */

/*
 * This code denotes incomplete or wedged cleanup, including a missing RGSS
 * acknowledgement or a watchdog failure. vita_boot_finish() also requires
 * explicit confirmation that shutdown, local destruction and watchdog join
 * completed before LoadExec. Otherwise the breadcrumb stays on the card for
 * the next launcher boot.
 *
 * 70 is out of the way of every `return 0` / `return 1` in main.cpp.
 */
#define VITA_BOOT_RC_WEDGED 70

/* ---- the two `kind` strings this file adds to the report format ---------- */

/* src/vita_fatal.h defines script-error, init-error and stuck. These two are
 * the boot wrapper's own, and they are plain strings for the same reason the
 * others are: the format is "one line of kind:", not an enum. */
#define VITA_BOOT_KIND_UNCLEAN "unclean-exit"
#define VITA_BOOT_KIND_BAD_ARG "bad-argument"

/* ---- the pure decision --------------------------------------------------- */

/*
 * Decide the mode from argv and the TEXT of the root configuration (NULL or
 * empty means "there is no root config"). Reads nothing, writes nothing, logs
 * nothing — which is what keeps the truth table host-compilable.
 *
 * The rule, in order:
 *   1. launch_args_parse() says GAME  -> GAME.
 *   2. it says INVALID                -> LAUNCHER, reason ARG_INVALID. A
 *      rejected path never falls back to the pin: the user asked for a
 *      specific game and must be told they did not get it, not handed a
 *      different one.
 *   3. the config has a non-empty gameFolder or customScript -> PINNED.
 *   4. otherwise                      -> LAUNCHER.
 *
 * Unparsable, unreadable or non-object config text counts as "not pinned"
 * (reason CONFIG_BROKEN) — a broken file must never be able to strand the
 * device on a black screen, and vita_boot_log_decision() reports it once the
 * log is open.
 */
void vita_boot_mode_from(int argc, char *const argv[],
                         const char *root_config_text, VitaBootDecision *out);

/* ---- the process-wide decision ------------------------------------------- */

/*
 * Read the root configuration off the card and run vita_boot_mode_from() over
 * it, storing the result for the rest of the process. Call once, first thing
 * in main(), BEFORE vita_glue_boot() — the decision is what chooses the log
 * file. Returns the mode. Nothing is logged yet (there is no log yet); the
 * whole decision comes out in vita_boot_log_decision().
 */
int vita_boot_decide(int argc, char *argv[]);

/* 1 when this process is the launcher. 0 before vita_boot_decide() has run,
 * so a build that never calls it behaves exactly as it did before. */
int vita_boot_is_launcher(void);

/* The log vita_glue_boot() should open: the launcher's own file in LAUNCHER
 * mode, the player log otherwise. NULL before vita_boot_decide() has run,
 * which is what vita_glue_boot() already treats as "use the default". */
const char *vita_boot_log_path(void);

/*
 * Everything the decision has to say, now that there is somewhere to say it.
 * Call immediately after vita_glue_boot(). It also performs the two deferred
 * side effects that need an open log:
 *
 *   GAME mode      writes the crash breadcrumb (vita_boot_mark_running).
 *   LAUNCHER mode  consumes a breadcrumb left by a previous run, turning it
 *                  into the launcher's message screen, and reports a rejected
 *                  --game or a broken root config the same way.
 */
void vita_boot_log_decision(void);

/* ---- the launcher -------------------------------------------------------- */

/*
 * Run the front end. Called from mkxp_main() after SDL_Init and before
 * anything else the engine does, so no Config is read, no OpenAL device is
 * opened, no RGSS thread is started and Ruby is never initialised.
 *
 * Returns only when the process was NOT handed over to a game.
 */
int vita_launcher_main(void);

/* ---- reporting and the hand-over back ------------------------------------ */

/*
 * One fatal report: logged, then written to last-error.txt through
 * src/vita_fatal.cpp's writer — never formatted here. `title` is one line
 * (the game title, or the engine's name); `msg` is free-form.
 */
void vita_boot_report_error(const char *title, const char *msg);

/*
 * The tail of main(). Returns the code main() should return.
 *
 *   GAME mode, confirmed cleanup  delete the breadcrumb, then
 *                                 sceAppMgrLoadExec back to the launcher.
 *                                 Only returns if that call failed.
 *   GAME mode, VITA_BOOT_RC_WEDGED or unconfirmed cleanup
 *                                 no LoadExec: teardown may still be active.
 *                                 The breadcrumb stays on the card.
 *   LAUNCHER / PINNED             nothing; the process exits to LiveArea.
 */
int vita_boot_finish(int rc);

/* ---- the breadcrumb ------------------------------------------------------ */

/* Write ux0:/data/mkxp-z/launch-in-progress.txt naming `game_path` and the log
 * this run is using. Returns 1 when the file is on the card. */
int vita_boot_mark_running(const char *game_path);

/* Remove it. Safe when it is not there. */
void vita_boot_clear_running(void);

#ifdef __cplusplus
}
#endif

#endif /* VITA_BOOT_H */
