// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vita_launcher.c — see vita_launcher.h.
 */
#include "vita_launcher.h"

#include <SDL2/SDL_ttf.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "game_scan.h"
#include "launch_args.h"
#include "launcher_model.h"
#include "launcher_state.h"
#include "preflight.h"
#include "vita_loadexec.h"

#ifdef __vita__
#include "vita_glue.h"
#endif

/*
 * 256 entries is about 160 KB. It belongs in static storage: the Vita's main
 * thread stack is 1 MiB (VITA_GLUE_MAIN_STACK_BYTES) and this is the one
 * allocation in the launcher big enough to matter. One launcher per process,
 * so one array.
 */
static GameEntry g_entries[GAME_SCAN_MAX_ENTRIES];
static unsigned char g_rtp_missing[GAME_SCAN_MAX_ENTRIES];

/* ---- tracing ----------------------------------------------------------- */

static void default_trace(const char *msg)
{
#ifdef __vita__
    vita_glue_trace(msg);
#else
    (void)msg;
#endif
}

static void trace_fmt(LauncherTraceFn trace, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void trace_fmt(LauncherTraceFn trace, const char *fmt, ...)
{
    char line[GAME_SCAN_PATH_MAX + GAME_SCAN_TITLE_MAX + 96];
    va_list ap;

    if (!trace)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = '\0';
    trace(line);
}

#ifdef __vita__
static void set_pool_hint(const char *hint, unsigned mib)
{
    char bytes[16];

    snprintf(bytes, sizeof(bytes), "%u", mib * 1024u * 1024u);
    SDL_SetHint(hint, bytes);
}
#endif

/* ---- options ----------------------------------------------------------- */

void vita_launcher_options_default(VitaLauncherOptions *opt)
{
    if (!opt)
        return;
    memset(opt, 0, sizeof(*opt));
    opt->games_root = VITA_LAUNCHER_GAMES_ROOT;
    opt->rtp_root = VITA_LAUNCHER_RTP_ROOT;
    opt->fonts_dir = VITA_LAUNCHER_FONTS_DIR;
    opt->state_path = VITA_LAUNCHER_STATE_PATH;
    opt->error_path = VITA_LAUNCHER_ERROR_PATH;
    opt->prev_error_path = VITA_LAUNCHER_PREV_ERROR_PATH;
    opt->exec_path = VITA_LAUNCHER_EXEC_PATH;
}

static void fill_defaults(VitaLauncherOptions *dst,
                          const VitaLauncherOptions *src)
{
    vita_launcher_options_default(dst);
    if (!src)
        return;
    if (src->games_root)
        dst->games_root = src->games_root;
    if (src->rtp_root)
        dst->rtp_root = src->rtp_root;
    if (src->fonts_dir)
        dst->fonts_dir = src->fonts_dir;
    if (src->state_path)
        dst->state_path = src->state_path;
    if (src->error_path)
        dst->error_path = src->error_path;
    if (src->prev_error_path)
        dst->prev_error_path = src->prev_error_path;
    if (src->exec_path)
        dst->exec_path = src->exec_path;
    dst->trace = src->trace;
    dst->post_session_open = src->post_session_open;
    dst->pre_loadexec = src->pre_loadexec;
    dst->pre_loadexec_ctx = src->pre_loadexec_ctx;
}

/* ---- session ----------------------------------------------------------- */

int vita_launcher_session_open(VitaLauncherSession *s,
                               const VitaLauncherOptions *opt)
{
    VitaLauncherOptions o;

    if (!s)
        return 0;
    memset(s, 0, sizeof(*s));
    fill_defaults(&o, opt);
    s->trace = o.trace ? o.trace : default_trace;

    if (TTF_Init() != 0) {
        trace_fmt(s->trace, "launcher: TTF_Init FAILED: %s", TTF_GetError());
        return 0;
    }
    s->ttf_ready = 1;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

#ifdef __vita__
    /* SDL_CreateWindow runs vglInit; without hints the backend falls back
     * to SDL's 96/64/16 MiB. An SDL_VITA_VGL_* environment variable wins. */
    set_pool_hint(VITA_GLUE_VGL_HINT_RAM, VITA_GLUE_VGL_LAUNCHER_RAM_MIB);
    set_pool_hint(VITA_GLUE_VGL_HINT_CDRAM, VITA_GLUE_VGL_LAUNCHER_CDRAM_MIB);
    set_pool_hint(VITA_GLUE_VGL_HINT_PHYCONT, VITA_GLUE_VGL_LAUNCHER_PHYCONT_MIB);
    vita_glue_vgl_init_timing("launcher", 0);
#endif
    s->window = SDL_CreateWindow("mkxp-z launcher", SDL_WINDOWPOS_UNDEFINED,
                                 SDL_WINDOWPOS_UNDEFINED, LAUNCHER_VIEW_W,
                                 LAUNCHER_VIEW_H,
                                 SDL_WINDOW_OPENGL |
                                     SDL_WINDOW_FULLSCREEN_DESKTOP);
#ifdef __vita__
    vita_glue_vgl_init_timing("launcher", 1);
#endif
    if (!s->window) {
        trace_fmt(s->trace, "launcher: SDL_CreateWindow FAILED: %s",
                  SDL_GetError());
        vita_launcher_session_close(s);
        return 0;
    }
    s->trace("launcher: SDL_CreateWindow ok");

    /* Main thread, like src/main.cpp: the context is made current on the
     * thread that created it and nothing else ever binds it. */
    s->gl_ctx = SDL_GL_CreateContext(s->window);
    if (!s->gl_ctx) {
        trace_fmt(s->trace, "launcher: SDL_GL_CreateContext FAILED: %s",
                  SDL_GetError());
        vita_launcher_session_close(s);
        return 0;
    }
    s->trace("launcher: SDL_GL_CreateContext ok");
    if (SDL_GL_SetSwapInterval(1) != 0)
        trace_fmt(s->trace, "launcher: swap interval 1 refused: %s",
                  SDL_GetError());

    if (!launcher_gl_init(&s->gl, LAUNCHER_VIEW_W, LAUNCHER_VIEW_H,
                          s->trace)) {
        vita_launcher_session_close(s);
        return 0;
    }
    if (!launcher_view_init(&s->view, o.fonts_dir, s->trace)) {
        vita_launcher_session_close(s);
        return 0;
    }

    /* Raw joystick, not the game controller layer: the Vita's button order
     * is fixed and known and a mapping database is one more
     * thing that can be absent on the card. */
    SDL_JoystickEventState(SDL_ENABLE);
    if (SDL_NumJoysticks() > 0)
        s->joystick = SDL_JoystickOpen(0);
    trace_fmt(s->trace, "launcher: joysticks=%d opened=%d", SDL_NumJoysticks(),
              s->joystick ? 1 : 0);

    /* The whole set exists now. This is the cold sample of the
     * headroom measurement: what the process has left with exactly one
     * texture, one VBO, one program and the flip buffers taken. */
    if (o.post_session_open)
        o.post_session_open(o.pre_loadexec_ctx);
    return 1;
}

void vita_launcher_session_present(VitaLauncherSession *s, int upload)
{
    if (!s || !s->window)
        return;
    if (upload)
        launcher_gl_upload(&s->gl, launcher_view_pixels(&s->view));
    launcher_gl_draw(&s->gl);
    SDL_GL_SwapWindow(s->window);
}

static void present_pending(VitaLauncherSession *s, int changed)
{
    if (changed) {
        launcher_gl_upload(&s->gl, launcher_view_pixels(&s->view));
        s->presents = 2;
    }
    if ((!s->gl.pending_pixels && s->presents == 0) ||
        (s->retry_delay &&
         (unsigned int)(SDL_GetTicks() - s->retry_started) < s->retry_delay)) {
        SDL_Delay(8);
        return;
    }

    vita_launcher_session_present(s, 0);
    if (s->gl.pending_pixels) {
        /* Failed uploads must not spend either buffer's refresh. Canvas
         * changes coalesce during backoff without accelerating retries. */
        s->presents = 2;
        s->retry_started = SDL_GetTicks();
        if (!s->retry_delay)
            s->retry_delay = 16;
        else if (s->retry_delay < 256)
            s->retry_delay *= 2;
    } else {
        s->retry_delay = 0;
        if (s->presents > 0)
            s->presents--;
    }
}

void vita_launcher_session_close(VitaLauncherSession *s)
{
    if (!s)
        return;

    if (s->joystick) {
        SDL_JoystickClose(s->joystick);
        s->joystick = NULL;
    }
    /* GL objects go while the context is still current; the view's fonts go
     * before TTF_Quit. */
    if (s->gl_ctx)
        launcher_gl_shutdown(&s->gl, s->trace);
    launcher_view_shutdown(&s->view);
    if (s->gl_ctx) {
        SDL_GL_DeleteContext(s->gl_ctx);
        s->gl_ctx = NULL;
    }
    if (s->window) {
        SDL_DestroyWindow(s->window);
        s->window = NULL;
    }
    if (s->ttf_ready) {
        TTF_Quit();
        s->ttf_ready = 0;
    }
    s->presents = 0;
    s->retry_delay = 0;
}

/* ---- last-error.txt --------------------------------- */

/* One logged line of the report. 512 is the crash-safe sink's line budget
 * (VITA_GLUE_LOG_SYNC_LINE_MAX), so one report line costs one sceIoWrite,
 * and it is what the engine's own logger uses on the writing side. The
 * screen and the rotated file hold the line whole either way. */
#define ERROR_LOG_LINE_MAX 512

int vita_launcher_take_last_error(const char *error_path,
                                  const char *prev_error_path,
                                  LastErrorReport *out, LauncherTraceFn trace)
{
    char line[ERROR_LOG_LINE_MAX];
    int i;

    if (!last_error_take(error_path, prev_error_path, out)) {
        if (out && out->io_error) {
            trace_fmt(trace, "launcher: last-error read failed; pending "
                             "reports retained at '%s' and its backup", error_path);
            return 1;
        }
        return 0;
    }

    /*
     * Into the log before it reaches the screen. The player that wrote this
     * file is gone, so this is the only run that will ever see it, and a
     * launcher with no Japanese face — or nobody watching the screen at all
     * — must not be the reason the diagnosis is lost a second time. One
     * line per source line, like vitaLogMessage does on the writing side.
     */
    trace_fmt(trace, "launcher: last-error kind='%s' title='%s' bytes=%lu "
                     "v1=%d truncated=%d",
              out->kind, out->title, out->bytes, out->v1, out->truncated);
    for (i = 0; last_error_line(out, i, line, sizeof(line)); i++)
        trace_fmt(trace, "launcher: last-error | %s", line);
    if (out->io_error)
        trace_fmt(trace, "launcher: last-error could not be rotated; "
                         "pending report retained at '%s'", out->kept);
    else if (out->kept[0])
        trace_fmt(trace, "launcher: last-error rotated to '%s'", out->kept);
    return 1;
}

static void last_error_message(const LastErrorReport *report, char *heading,
                               size_t heading_cap, char *body, size_t body_cap)
{
    size_t used = 0;

    if (report->io_error && !report->kept[0]) {
        snprintf(heading, heading_cap, "Could not read the last report");
        snprintf(body, body_cap, "The pending report files were retained. "
                 "Check the storage and restart the launcher to retry.");
        return;
    }
    last_error_heading(report, heading, heading_cap);
    if (report->io_error) {
        int n = snprintf(body, body_cap, "The report could not be moved. "
                         "It is still pending and may appear again next boot.\n\n");
        if (n < 0 || (size_t)n >= body_cap)
            return;
        used = (size_t)n;
    }
    last_error_body(report, body + used, body_cap - used);
}

/* ---- scan -------------------------------------------------------------- */

static int dir_exists(const char *path)
{
    struct stat st;

    if (!path || !path[0])
        return 0;
    if (stat(path, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

static const char *rtp_folder_for(int rgss_version)
{
    switch (rgss_version) {
    case 1:  return "XP";
    case 2:  return "VX";
    case 3:  return "VXAce";
    default: return NULL;
    }
}

/* One hex byte per title byte: the screen can
 * only be trusted when a Japanese face is installed, the log always can. */
static void trace_title_hex(LauncherTraceFn trace, int index,
                            const char *title)
{
    char hex[3 * 64 + 1];
    size_t i, len, out = 0;
    int non_ascii = 0;

    if (!trace || !title)
        return;
    len = strlen(title);
    for (i = 0; i < len; i++) {
        if ((unsigned char)title[i] >= 0x80) {
            non_ascii = 1;
            break;
        }
    }
    if (!non_ascii)
        return;
    if (len > 64)
        len = 64;
    for (i = 0; i < len; i++) {
        int w = snprintf(hex + out, sizeof(hex) - out, "%02x",
                         (unsigned char)title[i]);
        if (w < 0 || (size_t)w >= sizeof(hex) - out)
            break;
        out += (size_t)w;
    }
    hex[out] = '\0';
    trace_fmt(trace, "launcher: [%d] title utf8 hex=%s", index, hex);
}

/*
 * Scan, log the summary and one line per entry, and work out which games
 * want an RTP that is not on the card. Returns the entry count.
 */
static int scan_games(const VitaLauncherOptions *o, LauncherTraceFn trace,
                      char *subtitle, size_t subtitle_cap, int *scan_failed)
{
    GameScanStats stats;
    unsigned int t0, ms;
    int count, i;

    memset(&stats, 0, sizeof(stats));
    memset(g_rtp_missing, 0, sizeof(g_rtp_missing));

    t0 = SDL_GetTicks();
    count = game_scan(o->games_root, g_entries, GAME_SCAN_MAX_ENTRIES, &stats);
    *scan_failed = stats.incomplete;
    ms = SDL_GetTicks() - t0;

    trace_fmt(trace,
              "launcher: scan root='%s' games=%d skipped=%d truncated=%d "
              "ms=%u incomplete=%d",
              o->games_root, count,
              stats.skipped_no_ini + stats.skipped_too_long +
                  stats.skipped_bad_ini + stats.skipped_ambiguous_ini +
                  stats.skipped_bad_name,
              stats.truncated, ms, stats.incomplete);
    if (stats.skipped_bad_ini)
        trace_fmt(trace,
                  "launcher: scan skipped %d game%s with an unreadable Game.ini; "
                  "last='%s'",
                  stats.skipped_bad_ini, stats.skipped_bad_ini == 1 ? "" : "s",
                  stats.last_bad_ini_folder);
    if (stats.skipped_ambiguous_ini)
        trace_fmt(trace,
                  "launcher: scan skipped %d folder%s with several .ini files "
                  "naming games, so no single name is the game; last='%s'",
                  stats.skipped_ambiguous_ini,
                  stats.skipped_ambiguous_ini == 1 ? "" : "s",
                  stats.last_ambiguous_folder);
    if (stats.root_missing)
        trace_fmt(trace, "launcher: scan root '%s' could not be opened",
                  o->games_root);

    for (i = 0; i < count; i++) {
        const GameEntry *e = &g_entries[i];

        if (e->flags & GAME_ENTRY_HAS_EXECNAME)
            trace_fmt(trace,
                      "launcher: [%d] folder='%s' title='%s' execName='%s' "
                      "rgss=%d flags=0x%x",
                      i, e->folder, e->title, e->exec_name, e->rgss_version,
                      (unsigned)e->flags);
        else
            trace_fmt(trace,
                      "launcher: [%d] folder='%s' title='%s' rgss=%d flags=0x%x",
                      i, e->folder, e->title, e->rgss_version,
                      (unsigned)e->flags);
        trace_title_hex(trace, i, e->title);

        if (e->flags & GAME_ENTRY_WANTS_RTP) {
            const char *name = rtp_folder_for(e->rgss_version);
            char path[GAME_SCAN_PATH_MAX];

            if (name && o->rtp_root) {
                if ((int)snprintf(path, sizeof(path), "%s/%s", o->rtp_root,
                                  name) < (int)sizeof(path))
                    g_rtp_missing[i] = (unsigned char)(dir_exists(path) ? 0 : 1);
            }
        }
    }

    if (stats.incomplete)
        snprintf(subtitle, subtitle_cap, "Scan failed: %s", o->games_root);
    else
        snprintf(subtitle, subtitle_cap, "%d game%s in %s", count,
                 count == 1 ? "" : "s", o->games_root);
    subtitle[subtitle_cap - 1] = '\0';
    return count;
}

/* ---- input ------------------------------------------------------------- */

/*
 * SDL's Vita button order (confirmed by hardware traces):
 *   0 Triangle  1 Circle  2 Cross  3 Square  4 L  5 R
 *   6 Down  7 Left  8 Up  9 Right  10 Select  11 Start
 * Cross AND Circle both confirm: the launcher must not care which region the
 * console came from.
 */
#define PAD_BUTTON_COUNT 12

static int logical_for_pad(int button)
{
    switch (button) {
    case 0:  return LAUNCHER_BTN_RESCAN;
    case 1:  return LAUNCHER_BTN_CONFIRM; /* Circle */
    case 2:  return LAUNCHER_BTN_CONFIRM; /* Cross  */
    case 3:  return LAUNCHER_BTN_CANCEL;  /* Square */
    case 4:  return LAUNCHER_BTN_PAGE_UP;
    case 5:  return LAUNCHER_BTN_PAGE_DOWN;
    case 6:  return LAUNCHER_BTN_DOWN;
    case 7:  return LAUNCHER_BTN_LEFT;
    case 8:  return LAUNCHER_BTN_UP;
    case 9:  return LAUNCHER_BTN_RIGHT;
    default: return -1;                   /* Select, Start: unmapped */
    }
}

#define STICK_THRESHOLD 16000

/* One bit per physical source of a logical button: bit b for pad button b,
 * bit 16 for the stick. Two physical buttons can drive one logical one —
 * Cross and Circle both confirm, and the stick doubles the d-pad — so the
 * bit has to identify WHICH one, not just "some pad button". */
/* The index is masked because SDL hands us whatever the pad reports and a
 * shift by >= 32 is undefined; only buttons 0..9 map to a logical button
 * anyway, so the fold can never collide with one that matters. */
#define SRC_PAD(button) (1u << ((unsigned)(button) & 15u))
#define SRC_STICK       (1u << 16)

/*
 * A logical button is down while ANY source holds it. Without this, letting
 * go of the stick while the d-pad is still held would tell the model the
 * button was released and stop the auto-repeat mid-hold. All timing still
 * lives in launcher_model; this only decides when an edge exists.
 */
typedef struct InputState {
    unsigned int src[LAUNCHER_BTN_COUNT];
    int stick_dir; /* -1 up, 0 centre, +1 down */
} InputState;

static LauncherAction input_edge(InputState *in, LauncherModel *m,
                                 int logical, unsigned int source, int down,
                                 unsigned int now_ms)
{
    unsigned int before;

    if (logical < 0 || logical >= LAUNCHER_BTN_COUNT)
        return LAUNCHER_ACTION_NONE;

    before = in->src[logical];
    if (down)
        in->src[logical] |= source;
    else
        in->src[logical] &= ~source;

    if (!before && in->src[logical])
        return launcher_model_button(m, (LauncherButton)logical, 1, now_ms);
    if (before && !in->src[logical])
        return launcher_model_button(m, (LauncherButton)logical, 0, now_ms);
    return LAUNCHER_ACTION_NONE;
}

/* Buttons physically down right now, as a LAUNCHER_BTN_BIT mask. The model
 * refuses to act on any of them until it has seen a release: the launcher is
 * reached by LoadExec from a game the user just quit, and the button that
 * quit it is very likely still held. */
static unsigned int held_mask_now(SDL_Joystick *joy, InputState *in)
{
    unsigned int mask = 0;
    int b;

    if (!joy)
        return 0;
    SDL_JoystickUpdate();
    for (b = 0; b < PAD_BUTTON_COUNT; b++) {
        int logical = logical_for_pad(b);

        if (logical < 0)
            continue;
        if (SDL_JoystickGetButton(joy, b)) {
            mask |= LAUNCHER_BTN_BIT(logical);
            in->src[logical] |= SRC_PAD(b);
        }
    }
    return mask;
}

static LauncherAction handle_stick(InputState *in, LauncherModel *m,
                                   int axis_value, unsigned int now_ms)
{
    int dir = 0;
    LauncherAction a = LAUNCHER_ACTION_NONE, b;

    if (axis_value <= -STICK_THRESHOLD)
        dir = -1;
    else if (axis_value >= STICK_THRESHOLD)
        dir = 1;

    if (dir == in->stick_dir)
        return LAUNCHER_ACTION_NONE;

    if (in->stick_dir < 0)
        a = input_edge(in, m, LAUNCHER_BTN_UP, SRC_STICK, 0, now_ms);
    else if (in->stick_dir > 0)
        a = input_edge(in, m, LAUNCHER_BTN_DOWN, SRC_STICK, 0, now_ms);

    in->stick_dir = dir;

    if (dir < 0)
        b = input_edge(in, m, LAUNCHER_BTN_UP, SRC_STICK, 1, now_ms);
    else if (dir > 0)
        b = input_edge(in, m, LAUNCHER_BTN_DOWN, SRC_STICK, 1, now_ms);
    else
        b = LAUNCHER_ACTION_NONE;

    return b != LAUNCHER_ACTION_NONE ? b : a;
}

/* ---- the loop ---------------------------------------------------------- */

typedef struct LauncherRun {
    VitaLauncherSession session;
    VitaLauncherOptions opt;
    LauncherModel model;
    InputState input;
    LauncherListInfo list;
    LastErrorReport last_error; /* ~1.5 KB: static storage, not the stack */
    PreflightSummary preflight;
    unsigned preflight_page;
    unsigned preflight_started;
    int preflight_active, preflight_ready;
    char subtitle[GAME_SCAN_PATH_MAX + 32];
    int count;
} LauncherRun;

static int begin_preflight(LauncherRun *run, const char *game_path)
{
    char path[GAME_SCAN_PATH_MAX + sizeof(PREFLIGHT_FILE)];
    int n = snprintf(path, sizeof(path), "%s/%s", game_path, PREFLIGHT_FILE);

    if (n < 0 || (size_t)n >= sizeof(path) ||
        preflight_load(path, &run->preflight) != 1)
        return 0;
    run->preflight_active = 1;
    run->preflight_ready = !run->input.src[LAUNCHER_BTN_CONFIRM];
    run->preflight_started = SDL_GetTicks();
    run->preflight_page = 0;
    launcher_view_compose_preflight(&run->session.view, &run->preflight, 0);
    return 1;
}

static LauncherAction preflight_input(LauncherRun *run, int logical,
                                      unsigned source, int down, unsigned now)
{
    unsigned before;
    PreflightButton button;
    PreflightAction action;

    if (logical < 0 || logical >= LAUNCHER_BTN_COUNT)
        return LAUNCHER_ACTION_NONE;
    before = run->input.src[logical];
    /* Track every physical source, but freeze the list and its repeat timers. */
    input_edge(&run->input, NULL, logical, source, down, now);
    if (!run->input.src[LAUNCHER_BTN_CONFIRM] ||
        (unsigned)(now - run->preflight_started) >= 1500u)
        run->preflight_ready = 1;
    if (!run->preflight_ready || before || !run->input.src[logical])
        return LAUNCHER_ACTION_NONE;
    switch (logical) {
    case LAUNCHER_BTN_CONFIRM: button = PREFLIGHT_CONFIRM; break;
    case LAUNCHER_BTN_CANCEL:  button = PREFLIGHT_CANCEL; break;
    case LAUNCHER_BTN_LEFT:    button = PREFLIGHT_PREVIOUS; break;
    case LAUNCHER_BTN_RIGHT:   button = PREFLIGHT_NEXT; break;
    default: return LAUNCHER_ACTION_NONE;
    }
    action = preflight_step(&run->preflight, button, &run->preflight_page);
    if (action == PREFLIGHT_CONTINUE)
        return LAUNCHER_ACTION_LAUNCH;
    if (action == PREFLIGHT_BACK) {
        int b;
        run->preflight_active = 0;
        for (b = 0; b < LAUNCHER_BTN_COUNT; b++) {
            run->model.held[b] = run->input.src[b] != 0;
            run->model.armed[b] = !run->model.held[b];
        }
        return LAUNCHER_ACTION_DISMISS;
    }
    return action == PREFLIGHT_REDRAW ? LAUNCHER_ACTION_REDRAW : LAUNCHER_ACTION_NONE;
}

/* Called by vita_loadexec.c with everything still live; see the header. */
static void run_pre_loadexec(void *ctx)
{
    LauncherRun *run = (LauncherRun *)ctx;
    char line[160];

    if (!run)
        return;
    run->session.trace(
        launcher_gl_budget_line(&run->session.gl, line, sizeof(line)));
    if (run->opt.pre_loadexec)
        run->opt.pre_loadexec(run->opt.pre_loadexec_ctx);
}

/* Called by vita_loadexec.c after run_pre_loadexec: everything this process
 * holds goes back to the OS before the eboot is re-executed. */
static void run_shutdown(void *ctx)
{
    LauncherRun *run = (LauncherRun *)ctx;

    if (!run)
        return;
    run->session.trace("launcher: releasing window, context and GL objects");
    vita_launcher_session_close(&run->session);
    SDL_Quit();
}

/*
 * Scan (or rescan) and republish the list. Returns the index the cursor
 * should prefer — the game remembered in launcher-last.txt if it is still
 * on the card, or -1. On a rescan the cached row surfaces are dropped:
 * entry 4 is very likely a different game now.
 */
static int rescan(LauncherRun *run)
{
    char remembered[GAME_SCAN_PATH_MAX];
    int preferred = -1;

    run->count = scan_games(&run->opt, run->session.trace, run->subtitle,
                            sizeof(run->subtitle), &run->list.scan_failed);
    if (launcher_state_load(run->opt.state_path, remembered,
                            sizeof(remembered)) == 0)
        preferred = launcher_state_index_of(remembered, g_entries, run->count);

    launcher_view_drop_rows(&run->session.view);

    run->list.entries = g_entries;
    run->list.count = run->count;
    run->list.rtp_missing = g_rtp_missing;
    run->list.games_root = run->opt.games_root;
    run->list.subtitle = run->subtitle;
    return preferred;
}

/*
 * A LoadExec that comes back has already been through the teardown hook:
 * there is no window, no context and no SDL left. Build it all again so the
 * user is told, rather than left on a black screen with the answer only in
 * the log. Returns 1 if there is something to draw on.
 */
static int reopen_after_failed_launch(LauncherRun *run, const char *heading,
                                      const char *body)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) < 0)
        return 0;
    if (!vita_launcher_session_open(&run->session, &run->opt))
        return 0;

    /* The session is new; everything that points into it must be rebuilt. */
    memset(&run->input, 0, sizeof(run->input));
    (void)rescan(run);
    launcher_model_init(&run->model, run->count, LAUNCHER_VIEW_ROWS, -1,
                         held_mask_now(run->session.joystick, &run->input));
    launcher_model_show_message(&run->model);
    launcher_view_compose_message(&run->session.view, heading, body);
    return 1;
}

int vita_launcher_run(const VitaLauncherOptions *opt)
{
    static LauncherRun run; /* ~1 KB of model + view pointers; see g_entries */
    VitaLoadExecConfig lx;
    char message[LAST_ERROR_BODY_MAX];
    unsigned int held;
    int preferred;
    int need_upload = 1;
    int running = 1;

    memset(&run, 0, sizeof(run));
    fill_defaults(&run.opt, opt);

    if (!vita_launcher_session_open(&run.session, &run.opt))
        return -1;

    memset(&lx, 0, sizeof(lx));
    lx.exec_path = run.opt.exec_path;
    lx.state_path = run.opt.state_path;
    lx.pre_exec = run_pre_loadexec;
    lx.shutdown = run_shutdown;
    lx.ctx = &run;
    lx.trace = run.session.trace;
    vita_loadexec_configure(&lx);

    preferred = rescan(&run);

    held = held_mask_now(run.session.joystick, &run.input);
    launcher_model_init(&run.model, run.count, LAUNCHER_VIEW_ROWS, preferred,
                        held);
    trace_fmt(run.session.trace, "launcher: input held-at-boot mask=0x%x",
              held);

    /* A read or rotation failure leaves the pending report for a later boot. */
    if (vita_launcher_take_last_error(run.opt.error_path,
                                      run.opt.prev_error_path,
                                      &run.last_error, run.session.trace)) {
        char heading[LAST_ERROR_HEADING_MAX];

        last_error_message(&run.last_error, heading, sizeof(heading),
                            message, sizeof(message));
        launcher_view_compose_message(&run.session.view, heading, message);
        launcher_model_show_message(&run.model);
    } else {
        launcher_view_compose_list(&run.session.view, &run.list, &run.model);
    }

    while (running) {
        SDL_Event ev;
        LauncherAction action = LAUNCHER_ACTION_NONE;
        unsigned int now = SDL_GetTicks();

        while (SDL_PollEvent(&ev)) {
            LauncherAction a = LAUNCHER_ACTION_NONE;

            switch (ev.type) {
            case SDL_JOYBUTTONDOWN:
            case SDL_JOYBUTTONUP:
                if (run.preflight_active)
                    a = preflight_input(&run, logical_for_pad(ev.jbutton.button),
                                        SRC_PAD(ev.jbutton.button),
                                        ev.type == SDL_JOYBUTTONDOWN, now);
                else
                    a = input_edge(&run.input, &run.model,
                                   logical_for_pad(ev.jbutton.button),
                                   SRC_PAD(ev.jbutton.button),
                                   ev.type == SDL_JOYBUTTONDOWN, now);
                break;
            case SDL_JOYAXISMOTION:
                if (ev.jaxis.axis == 1)
                    a = handle_stick(&run.input,
                                     run.preflight_active ? NULL : &run.model,
                                     ev.jaxis.value, now);
                break;
            case SDL_QUIT:
                running = 0;
                break;
            default:
                break;
            }
            if (a != LAUNCHER_ACTION_NONE)
                action = a;
            /* Present a screen transition before consuming its next edges. */
            if (!running || a == LAUNCHER_ACTION_LAUNCH ||
                a == LAUNCHER_ACTION_RESCAN || a == LAUNCHER_ACTION_DISMISS)
                break;
        }

        if (!running)
            break;
        if (action == LAUNCHER_ACTION_NONE && !run.preflight_active)
            action = launcher_model_tick(&run.model, now);

        switch (action) {
        case LAUNCHER_ACTION_LAUNCH: {
            int index = launcher_model_selected(&run.model);
            int rc;

            if (index < 0 || index >= run.count)
                break;
            if (!run.preflight_active && begin_preflight(&run, g_entries[index].path)) {
                need_upload = 1;
                break;
            }
            run.preflight_active = 0;
            /* Returns only on failure; everything after this line is the
             * error path, and by then the teardown hook has already taken
             * the window, the context and SDL itself. */
            rc = vita_loadexec_game(g_entries[index].path,
                                    g_entries[index].exec_name);
            trace_fmt(run.session.trace,
                      "launcher: launch FAILED rc=0x%08x path='%s'",
                      (unsigned)rc, g_entries[index].path);
            if (rc == VITA_LOADEXEC_ERR_ARGS || rc == VITA_LOADEXEC_ERR_CONFIG) {
                /* Refused before the teardown hook: the window, context and
                 * GL objects are still ours, so opening a second session on
                 * top of them would leak one per press. */
                snprintf(message, sizeof(message),
                         "%s\n\nThe game cannot be started from this launcher "
                         "(rc 0x%08x).",
                         g_entries[index].path, (unsigned)rc);
                launcher_model_show_message(&run.model);
                launcher_view_compose_message(&run.session.view,
                                              "Could not start the game",
                                              message);
                need_upload = 1;
                break;
            }
            snprintf(message, sizeof(message),
                     "%s\n\nsceAppMgrLoadExec returned 0x%08x. The game was "
                     "not started.",
                     g_entries[index].path, (unsigned)rc);
            if (!reopen_after_failed_launch(&run, "Could not start the game",
                                            message))
                return rc; /* nothing left to draw on; the log has it */
            vita_loadexec_configure(&lx);
            need_upload = 1;
            break;
        }
        case LAUNCHER_ACTION_RESCAN: {
            /* rescan() before the call, not inside its argument list: it
             * writes run.count, and argument evaluation order is not
             * sequenced. */
            int again = rescan(&run);

            launcher_model_set_count(&run.model, run.count, again);
            launcher_view_compose_list(&run.session.view, &run.list,
                                       &run.model);
            need_upload = 1;
            break;
        }
        case LAUNCHER_ACTION_DISMISS:
        case LAUNCHER_ACTION_REDRAW:
            if (run.preflight_active)
                launcher_view_compose_preflight(&run.session.view, &run.preflight,
                                                 run.preflight_page);
            else
                launcher_view_compose_list(&run.session.view, &run.list,
                                           &run.model);
            need_upload = 1;
            break;
        case LAUNCHER_ACTION_NONE:
        default:
            break;
        }

        present_pending(&run.session, need_upload);
        need_upload = 0;
    }

    vita_launcher_session_close(&run.session);
    return 0;
}
