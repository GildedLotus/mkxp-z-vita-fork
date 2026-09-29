// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vita_boot.cpp — see vita_boot.h.
 *
 * Three rules shape this file:
 *
 *  1. The decision is pure. vita_boot_mode_from() takes argv and the text of
 *     the root config and returns a struct; it touches no file, no log and no
 *     global. Everything that reads the card or writes a line is a thin shell
 *     around it, so the truth table the whole product hangs on is host-compilable.
 *
 *  2. One producer for last-error.txt. Every report this file makes goes
 *     through vitaWriteLastErrorTo() in src/vita_fatal.cpp. The format lives
 *     there and nowhere else, and the launcher's consumer (rename to
 *     last-error.prev.txt) is the only reader.
 *
 *  3. Nothing here may fail a boot. A missing root config, an unwritable
 *     breadcrumb, a full memory card: each is logged and the process carries
 *     on. The one thing this file must never do is leave the user on a black
 *     screen with no way back to the list.
 */

#include "vita_boot.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <exception>
#include <string>

#include "launch_args.h"
#include "last_error.h"
#include "vita_launcher.h"
#include "vita_loadexec.h"

/* The engine's own JSON5 parser, so "is this player pinned" is decided by the
 * same code that will later read the same file (src/config.cpp). Both of these
 * resolve through the mkxp-z src/ include directory (see src/meson.build). */
#include "util/json5pp.hpp"
#include "vita_fatal.h"

#include "vita_glue.h"

namespace json = json5pp;

/* ---- paths, all overridable for host builds ------------------------------ */

/* The report directory. Defaults to src/vita_fatal.h's, and everything below
 * passes it explicitly to vitaWriteLastErrorTo(), so one -D moves the whole
 * set consistently and the shipped value cannot drift from the writer's. */
#ifndef VITA_BOOT_DIR
#define VITA_BOOT_DIR VITA_FATAL_DIR
#endif

/* The root configuration mkxp-z reads first. CWD is SDL_GetBasePath() by the
 * time Config::read() runs, but this decision is taken before any chdir, so
 * the path is spelled out. */
#ifndef VITA_BOOT_ROOT_CONFIG
#define VITA_BOOT_ROOT_CONFIG "app0:/mkxp.json"
#endif

/* The two logs. A launcher boot must not consume the dead game's log
 * generations, and rotation is keyed per path (vita_glue.c rotate_logs). */
#ifndef VITA_BOOT_GAME_LOG
#define VITA_BOOT_GAME_LOG VITA_GLUE_LOG_PATH
#endif
#ifndef VITA_BOOT_LAUNCHER_LOG
#define VITA_BOOT_LAUNCHER_LOG VITA_GLUE_LOG_DIR "/launcher.log"
#endif

#define VITA_BOOT_RUNNING_NAME "launch-in-progress.txt"

/* Enough for the largest thing read whole here: app0:/mkxp.json. The tracked
 * template is about 3.5 KB of comments; a config larger than this is reported
 * as unreadable rather than partially parsed. */
#define BOOT_CONFIG_MAX 65536u

/* One breadcrumb line: two device paths and the labels around them. */
#define BOOT_CRUMB_MAX (2 * GAME_SCAN_PATH_MAX + 64)

/* ---- process state ------------------------------------------------------ */

/*
 * One decision per process, taken once in main() before anything else exists.
 * It is deliberately file-scope rather than threaded through the engine:
 * mkxp_main() is stock mkxp-z with three call sites added, and each of them
 * would otherwise need a parameter that means nothing off this device.
 */
static VitaBootDecision g_decision;
static int g_decided;
static const char *g_root_status = "absent";

/* ---- tracing ------------------------------------------------------------ */

static void boot_trace(const char *msg)
{
    if (!msg)
        return;
#ifdef __vita__
    vita_glue_trace(msg);
#else
    /* Host builds have no glue and no log. Never printf: see the long note in
     * vita_glue.h — not one printf line has ever reached a Vita log. */
    fputs(msg, stderr);
    fputc('\n', stderr);
#endif
}

static void boot_tracef(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

static void boot_tracef(const char *fmt, ...)
{
    char line[2 * GAME_SCAN_PATH_MAX + 160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = '\0';
    boot_trace(line);
}

/* ---- small helpers ------------------------------------------------------ */

static void copy_bounded(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (!dst || cap == 0)
        return;
    dst[0] = '\0';
    if (!src)
        return;
    n = strlen(src);
    if (n > cap - 1)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/*
 * Fold every control byte to '?'. The value this is applied to came off a
 * memory card and is on its way to a log line and a text renderer; a newline
 * in it would forge a second log line, and a lone 0x1b would reach SDL_ttf.
 */
static void fold_controls(char *s)
{
    size_t i;

    if (!s)
        return;
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c < 0x20 || c == 0x7F)
            s[i] = '?';
    }
}

/* The value of the first --game / --game=... in argv, whatever its shape.
 * launch_args_parse() deliberately does not hand back a rejected path — it is
 * a parser, not a reporter — so the reason for the rejection is recovered
 * here, for the message screen. */
static void rejected_game_value(int argc, char *const argv[], char *out,
                                size_t cap)
{
    static const char flag[] = "--game";
    const size_t flag_len = sizeof(flag) - 1;
    int i;

    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (!argv)
        return;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!a)
            break;
        if (strcmp(a, flag) == 0) {
            copy_bounded(out, cap, (i + 1 < argc) ? argv[i + 1] : "");
            break;
        }
        if (strncmp(a, flag, flag_len) == 0 && a[flag_len] == '=') {
            copy_bounded(out, cap, a + flag_len + 1);
            break;
        }
    }
    fold_controls(out);
}

static const char *mode_name(int mode)
{
    switch (mode) {
    case VITA_BOOT_MODE_GAME:   return "game";
    case VITA_BOOT_MODE_PINNED: return "pinned";
    default:                    return "launcher";
    }
}

static const char *reason_name(int reason)
{
    switch (reason) {
    case VITA_BOOT_REASON_ARG_GAME:      return "arg";
    case VITA_BOOT_REASON_ARG_INVALID:   return "bad-arg";
    case VITA_BOOT_REASON_CONFIG_PINNED: return "config";
    case VITA_BOOT_REASON_CONFIG_BROKEN: return "config-unreadable";
    default:                             return "no-args";
    }
}

/* ---- the pure decision --------------------------------------------------- */

/* A string member of a JSON object, or "" when it is absent or not a string.
 * The lookup is const so a missing key cannot insert a null the way
 * as_object()["key"] would. */
static std::string conf_string(const json::value &conf, const char *key)
{
    if (!conf.is_object())
        return std::string();

    const json::value::object_type &obj = conf.as_object();
    json::value::object_type::const_iterator it = obj.find(key);

    if (it == obj.end() || !it->second.is_string())
        return std::string();
    return it->second.as_string();
}

void vita_boot_mode_from(int argc, char *const argv[],
                         const char *root_config_text, VitaBootDecision *out)
{
    LaunchRequest req;
    json::value conf(0);
    bool parsed = false;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));

    /* 1 and 2: the command line decides first and alone. */
    launch_args_parse(argc, argv, &req);
    if (req.mode == LAUNCH_MODE_GAME) {
        out->mode = VITA_BOOT_MODE_GAME;
        out->reason = VITA_BOOT_REASON_ARG_GAME;
        copy_bounded(out->game_path, sizeof(out->game_path), req.game_path);
        return;
    }
    if (req.mode == LAUNCH_MODE_INVALID) {
        out->mode = VITA_BOOT_MODE_LAUNCHER;
        out->reason = VITA_BOOT_REASON_ARG_INVALID;
        rejected_game_value(argc, argv, out->arg_value,
                            sizeof(out->arg_value));
        return;
    }

    /* No configuration at all is not an error: it is an unpinned player, and
     * an unpinned player is the launcher. */
    if (!root_config_text || !root_config_text[0]) {
        out->mode = VITA_BOOT_MODE_LAUNCHER;
        out->reason = VITA_BOOT_REASON_NO_ARGS;
        return;
    }

    try {
        std::string text(root_config_text);

        /* json5pp cannot swallow a byte-order mark, and a config written on a
         * PC very often has one. src/config.cpp's Vita reader strips it for
         * the same reason. */
        if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
            (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
            text.erase(0, 3);

        conf = json::parse5(text);
        parsed = conf.is_object();
    }
    catch (const std::exception &) {
        parsed = false;
    }
    catch (...) {
        parsed = false;
    }

    if (!parsed) {
        /* Reported by vita_boot_log_decision(), once there is a log. Never
         * fatal: a config nobody can read must not be able to strand the
         * device with no way back to the list. */
        out->mode = VITA_BOOT_MODE_LAUNCHER;
        out->reason = VITA_BOOT_REASON_CONFIG_BROKEN;
        return;
    }

    /* The games root travels with the launcher VPK's own config
     * (packaging can set it in the root mkxp.json). Ignored unless it is a
     * path this device could actually hand to another process. */
    {
        std::string root = conf_string(conf, "vitaGamesRoot");

        if (!root.empty() && launch_path_is_valid(root.c_str()))
            copy_bounded(out->games_root, sizeof(out->games_root),
                         root.c_str());
    }

    /* 3: the pin. Either key alone is enough — a customScript player has no gameFolder of its own worth the name, and a
     * deployed game has no customScript. */
    if (!conf_string(conf, "gameFolder").empty() ||
        !conf_string(conf, "customScript").empty()) {
        out->mode = VITA_BOOT_MODE_PINNED;
        out->reason = VITA_BOOT_REASON_CONFIG_PINNED;
        return;
    }

    /* 4 */
    out->mode = VITA_BOOT_MODE_LAUNCHER;
    out->reason = VITA_BOOT_REASON_NO_ARGS;
}

/* ---- reading the root config -------------------------------------------- */

/*
 * Read a file into `buf`. Returns 1 on success, 0 when absent, -1 on I/O
 * failure, and sets *truncated (when given) if it did not fit. Uses stdio:
 * this runs before SDL_Init, PhysFS and any chdir.
 */
static int slurp(const char *path, char *buf, size_t cap, int *truncated)
{
    FILE *f;
    size_t n;
    int failed, more = 0;

    if (truncated)
        *truncated = 0;
    if (!buf || cap == 0)
        return -1;
    buf[0] = '\0';
    f = fopen(path, "rb");
    if (!f)
        return errno == ENOENT ? 0 : -1;
    n = fread(buf, 1, cap - 1, f);
    failed = ferror(f) || (n < cap - 1 && !feof(f));
    if (!failed && n == cap - 1) {
        more = fgetc(f) != EOF;
        failed = ferror(f) || (!more && !feof(f));
    }
    if (fclose(f) != 0)
        failed = 1;
    if (failed) {
        buf[0] = '\0';
        return -1;
    }
    if (truncated)
        *truncated = more;
    buf[n] = '\0';
    return 1;
}

int vita_boot_decide(int argc, char *argv[])
{
    /* Static: 64 KiB does not belong on a 1 MiB main-thread stack
     * (VITA_GLUE_MAIN_STACK_BYTES), and this runs exactly once. */
    static char text[BOOT_CONFIG_MAX];
    int have, truncated;

    have = slurp(VITA_BOOT_ROOT_CONFIG, text, sizeof(text), &truncated);
    vita_boot_mode_from(argc, argv, (have > 0 && !truncated) ? text : NULL,
                        &g_decision);

    /*
     * A failed read/close or a config too large to read whole is unreadable,
     * never absent: a truncated document whose surviving prefix happened to parse
     * would decide the mode from half a file, and the missing half is exactly
     * where a gameFolder could have been. The command line still wins, and a
     * rejected --game is still the more specific thing to say.
     */
    if ((have < 0 || truncated) && g_decision.mode != VITA_BOOT_MODE_GAME &&
        g_decision.reason != VITA_BOOT_REASON_ARG_INVALID) {
        g_decision.mode = VITA_BOOT_MODE_LAUNCHER;
        g_decision.reason = VITA_BOOT_REASON_CONFIG_BROKEN;
    }

    /* "absent" and "unpinned" produce the same mode but are very different
     * things to read in a log six weeks later. */
    if (have == 0)
        g_root_status = "absent";
    else if (have < 0)
        g_root_status = "unreadable";
    else if (truncated)
        g_root_status = "too-large";
    else if (g_decision.reason == VITA_BOOT_REASON_CONFIG_BROKEN)
        g_root_status = "unreadable";
    else
        g_root_status = "ok";
    g_decided = 1;
    return g_decision.mode;
}

int vita_boot_is_launcher(void)
{
    return (g_decided && g_decision.mode == VITA_BOOT_MODE_LAUNCHER) ? 1 : 0;
}

const char *vita_boot_log_path(void)
{
    if (!g_decided)
        return NULL; /* vita_glue_boot() reads NULL as "the default log" */
    return g_decision.mode == VITA_BOOT_MODE_LAUNCHER ? VITA_BOOT_LAUNCHER_LOG
                                                      : VITA_BOOT_GAME_LOG;
}

/* ---- the breadcrumb ------------------------------------------------------ */

static const char *running_path(void)
{
    return VITA_BOOT_DIR "/" VITA_BOOT_RUNNING_NAME;
}

int vita_boot_mark_running(const char *game_path)
{
    const char *path = running_path();
    FILE *f = fopen(path, "wb");
    int ok;

    if (!f) {
        boot_tracef("vita-boot: cannot write the breadcrumb '%s'", path);
        return 0;
    }
    /* One line, and the log path is in it: the launcher's whole job with this
     * file is to tell the user which game died and where to read about it. */
    ok = fprintf(f, "game=%s log=%s\n", game_path ? game_path : "(unknown)",
                 VITA_BOOT_GAME_LOG) > 0;
    if (fclose(f) != 0)
        ok = 0;
    if (ok)
        boot_tracef("vita-boot: breadcrumb '%s'", path);
    else
        boot_tracef("vita-boot: breadcrumb '%s' was not written", path);
    return ok;
}

void vita_boot_clear_running(void)
{
    (void)remove(running_path());
}

/*
 * The launcher half. Read the breadcrumb and retire it only after a report
 * was published or a previously published report was found.
 *
 * A report already on the card wins: a game that wrote last-error.txt and was
 * THEN killed diagnosed itself better than "it did not exit cleanly" ever
 * could, and overwriting it would destroy the only copy. The breadcrumb still
 * goes, and the log line still records the unclean exit.
 */
static void consume_breadcrumb(void)
{
    const char *path = running_path();
    char crumb[BOOT_CRUMB_MAX];
    char body[BOOT_CRUMB_MAX + 256];
    const char *existing = VITA_BOOT_DIR "/" VITA_FATAL_NAME;
    const char *reports[] = {existing, VITA_BOOT_DIR "/" VITA_FATAL_NAME ".bak"};
    int have = slurp(path, crumb, sizeof(crumb), NULL);

    /* A breadcrumb that does not fit is still a breadcrumb: its prefix names
     * the game. Unlike the root config, it is only a display preview. */
    if (have <= 0) {
        if (have < 0)
            boot_tracef("vita-boot: could not read breadcrumb '%s'; keeping it", path);
        return;
    }
    fold_controls(crumb);

    boot_tracef("vita-boot: previous run did not exit cleanly (%s)", crumb);

    for (const char *report : reports) {
        LastErrorReport probe;
        int valid = last_error_read(report, &probe);
        if (valid > 0) {
            boot_tracef("vita-boot: keeping the report already in '%s'", report);
            (void)remove(path);
            return;
        }
        if (valid < 0) {
            boot_tracef("vita-boot: could not read report '%s'; keeping breadcrumb", report);
            return;
        }
    }

    snprintf(body, sizeof(body),
             "The last game did not exit cleanly, so it could not say "
             "why.\n\n%s\n\nThe log of that run is still on the card and "
             "was not overwritten by this one.",
             crumb);
    body[sizeof(body) - 1] = '\0';
    if (!vitaWriteLastErrorTo(VITA_BOOT_DIR, VITA_BOOT_KIND_UNCLEAN,
                              "The last game did not exit cleanly", body)) {
        boot_tracef("vita-boot: could not write the report into '%s'; keeping breadcrumb",
                    existing);
        return;
    }
    (void)remove(path);
}

/* ---- the decision, out loud ---------------------------------------------- */

void vita_boot_log_decision(void)
{
    if (!g_decided) {
        boot_trace("vita-boot: mode=pinned reason=not-decided "
                   "(vita_boot_decide was never called)");
        return;
    }

    boot_tracef("vita-boot: mode=%s reason=%s config=%s game='%s' log='%s'",
                mode_name(g_decision.mode), reason_name(g_decision.reason),
                g_root_status, g_decision.game_path, vita_boot_log_path());
    if (g_decision.games_root[0])
        boot_tracef("vita-boot: games root '%s' (from %s)",
                    g_decision.games_root, VITA_BOOT_ROOT_CONFIG);

    switch (g_decision.mode) {
    case VITA_BOOT_MODE_GAME:
        vita_boot_mark_running(g_decision.game_path);
        break;

    case VITA_BOOT_MODE_LAUNCHER:
        /* The two things the user has to be told, in the order that leaves
         * the most useful one on screen: a rejected argument or a broken
         * config is about THIS boot, so it is written first and a breadcrumb
         * from the previous run then defers to it. */
        if (g_decision.reason == VITA_BOOT_REASON_ARG_INVALID) {
            char body[GAME_SCAN_PATH_MAX + 256];

            boot_tracef("vita-boot: --game path rejected: '%s'",
                        g_decision.arg_value);
            snprintf(body, sizeof(body),
                     "A game was requested that this player cannot open:\n\n"
                     "%s\n\nA game path must be an absolute device path such "
                     "as ux0:/data/mkxp-z/games/MyGame, with no '..' in it.",
                     g_decision.arg_value[0] ? g_decision.arg_value
                                             : "(an empty path)");
            body[sizeof(body) - 1] = '\0';
            (void)vitaWriteLastErrorTo(VITA_BOOT_DIR, VITA_BOOT_KIND_BAD_ARG,
                                       "That game could not be started", body);
        }
        else if (g_decision.reason == VITA_BOOT_REASON_CONFIG_BROKEN) {
            boot_tracef("vita-boot: root config '%s' could not be parsed; "
                        "treating this player as unpinned",
                        VITA_BOOT_ROOT_CONFIG);
            (void)vitaWriteLastErrorTo(
                VITA_BOOT_DIR, VITA_BOOT_KIND_BAD_ARG,
                "The player's own configuration could not be read",
                VITA_BOOT_ROOT_CONFIG " is not readable as JSON5. The "
                "launcher started anyway; games on the memory card are "
                "unaffected.");
        }
        consume_breadcrumb();
        break;

    default:
        /* PINNED is every player built before the launcher existed. It writes no
         * breadcrumb and consumes none: the evidence of a crash belongs to
         * the launcher, and a diagnostic run must not eat it. */
        break;
    }
}

/* ---- the launcher -------------------------------------------------------- */

int vita_launcher_main(void)
{
    VitaLauncherOptions opt;

    vita_launcher_options_default(&opt);

    /*
     * The one default this overrides. vita_launcher.h points its consumer at
     * logs/last-error.txt while src/vita_fatal.cpp's writer — the only
     * producer — writes VITA_FATAL_DIR/last-error.txt, one directory up. The
     * two halves were written separately and never met until now; binding
     * the consumer to the producer's path here is what makes them one file,
     * without a second writer and without editing either side.
     */
    opt.error_path = VITA_BOOT_DIR "/" VITA_FATAL_NAME;
    if (g_decision.games_root[0])
        opt.games_root = g_decision.games_root;

    /* trace stays NULL: vita_launcher.c's own default is vita_glue_trace. */
    return vita_launcher_run(&opt);
}

/* ---- reporting ----------------------------------------------------------- */

void vita_boot_report_error(const char *title, const char *msg)
{
    const char *text = msg ? msg : "(no message)";

    vitaLogMessage("vita-boot: fatal: ", text);
    if (!vitaWriteLastErrorTo(VITA_BOOT_DIR, VITA_FATAL_KIND_INIT,
                              title ? title : "mkxp-z", text))
        boot_trace("vita-boot: the fatal report could not be written to the "
                   "memory card");
}

/* ---- the tail of main() -------------------------------------------------- */

/* The engine confirms this only after shutdown, local destructors and the
 * watchdog join. An acknowledgement or a zero return code is insufficient. */
extern "C" int vita_boot_cleanup_complete(void);

int vita_boot_finish(int rc)
{
    VitaLoadExecConfig lx;
    int lrc;

    if (!g_decided || g_decision.mode != VITA_BOOT_MODE_GAME) {
        /* LAUNCHER hands the process over itself, from its own loop, with its
         * own GL objects released first. PINNED exits to LiveArea exactly as
         * every player before the launcher did. */
        return rc;
    }

    if (rc == VITA_BOOT_RC_WEDGED || !vita_boot_cleanup_complete()) {
        boot_trace("vita-boot: cleanup is incomplete or wedged; exiting without a "
                   "relaunch and leaving the breadcrumb in place");
        return VITA_BOOT_RC_WEDGED;
    }

    vita_boot_clear_running();

    memset(&lx, 0, sizeof(lx));
    lx.exec_path = VITA_LAUNCHER_EXEC_PATH;
    lx.state_path = NULL; /* the bookmark belongs to the --game direction */
    lx.trace = boot_trace;
    vita_loadexec_configure(&lx);

    boot_tracef("vita-boot: returning to launcher (rc=%d)", rc);
    lrc = vita_loadexec_launcher();

    /* Only reached when the hand-over did not happen. */
    boot_tracef("vita-boot: LoadExec back to the launcher FAILED rc=0x%08x",
                (unsigned)lrc);
    return rc ? rc : 1;
}
