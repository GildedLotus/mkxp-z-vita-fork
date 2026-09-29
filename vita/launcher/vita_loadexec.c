// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vita_loadexec.c — see vita_loadexec.h.
 */
#include "vita_loadexec.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "launch_args.h"
#include "launcher_state.h"

#ifdef __vita__
#include <psp2/appmgr.h>
#include <unistd.h> /* fsync */
#endif

/*
 * Static, not stack: the kernel reads argv while it is replacing this
 * process, long after any frame this call was made from would have returned.
 * One storage block and one vector, reused by every launch — there is at
 * most one hand-over per process by construction.
 */
static char g_argv_storage[LAUNCH_ARGS_STORAGE_MAX];
static char *g_argv[5];

static VitaLoadExecConfig g_cfg;
static int g_configured;

void vita_loadexec_configure(const VitaLoadExecConfig *cfg)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_configured = 0;
    if (!cfg || !cfg->exec_path || !cfg->exec_path[0])
        return;
    g_cfg = *cfg;
    g_configured = 1;
}

static void trace_line(const char *msg)
{
    if (g_cfg.trace && msg)
        g_cfg.trace(msg);
}

static void trace_fmt(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

static void trace_fmt(const char *fmt, ...)
{
    char line[GAME_SCAN_PATH_MAX + 64];
    va_list ap;

    if (!g_cfg.trace)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = '\0';
    g_cfg.trace(line);
}

/*
 * Everything that must be true on the card before the process stops
 * existing. fflush(NULL) drains every stream in this process; the two fsyncs
 * push the log's own descriptor through the filesystem cache. Return values
 * are deliberately ignored: stdout may be a TTY (host build) or already
 * redirected onto a file by the glue (Vita), and neither case has a recovery.
 */
static void flush_everything(void)
{
    fflush(NULL);
#ifdef __vita__
    (void)fsync(fileno(stdout));
    (void)fsync(fileno(stderr));
#endif
}

/* pre_exec runs with the GL set live; shutdown releases it; then the flush.
 * The order is the contract — see the header. */
static void hand_over_prologue(void)
{
    if (g_cfg.pre_exec)
        g_cfg.pre_exec(g_cfg.ctx);
    if (g_cfg.shutdown)
        g_cfg.shutdown(g_cfg.ctx);
    flush_everything();
}

int vita_loadexec_preview_game_argv(const char *game_path,
                                    const char *exec_name, char *out[5])
{
    if (!out)
        return -1;
    if (launch_args_build_game(game_path, exec_name, g_argv_storage,
                               sizeof(g_argv_storage), g_argv) != 0) {
        out[0] = NULL;
        out[1] = NULL;
        out[2] = NULL;
        out[3] = NULL;
        out[4] = NULL;
        return -1;
    }
    out[0] = g_argv[0];
    out[1] = g_argv[1];
    out[2] = g_argv[2];
    out[3] = g_argv[3];
    out[4] = g_argv[4];
    return 0;
}

#ifdef __vita__

static int do_loadexec(char *const *argv)
{
    int rc;

    /* SceAppMgr takes a non-const char *const * and does not modify it. */
    rc = sceAppMgrLoadExec(g_cfg.exec_path, (char *const *)argv, NULL);

    /* Only reached when the hand-over did not happen. */
    trace_fmt("launcher: LoadExec FAILED rc=0x%08x", (unsigned)rc);
    return rc;
}

int vita_loadexec_game(const char *game_path, const char *exec_name)
{
    if (!g_configured) {
        trace_line("launcher: LoadExec refused: not configured");
        return VITA_LOADEXEC_ERR_CONFIG;
    }

    /* Build first: a path this module cannot pass on is not worth writing a
     * bookmark for, and the caller can still stay on the list screen. */
    if (launch_args_build_game(game_path, exec_name, g_argv_storage,
                               sizeof(g_argv_storage), g_argv) != 0) {
        trace_fmt("launcher: LoadExec refused: invalid path '%s'",
                  game_path ? game_path : "(null)");
        return VITA_LOADEXEC_ERR_ARGS;
    }

    if (g_argv[2])
        trace_fmt("launcher: LoadExec --game '%s' --execName '%s'",
                  g_argv[1], g_argv[3]);
    else
        trace_fmt("launcher: LoadExec --game '%s'", g_argv[1]);

    /* The bookmark, before anything is torn down: after the call there is no
     * process left to write it, and a failed write must not stop the
     * launch — it only costs the cursor position. */
    if (g_cfg.state_path) {
        if (launcher_state_save(g_cfg.state_path, g_argv[1]) != 0)
            trace_fmt("launcher: state save FAILED path='%s'",
                      g_cfg.state_path);
    }

    hand_over_prologue();
    return do_loadexec(g_argv);
}

int vita_loadexec_launcher(void)
{
    if (!g_configured) {
        trace_line("launcher: LoadExec refused: not configured");
        return VITA_LOADEXEC_ERR_CONFIG;
    }

    /* argv == NULL, not an empty vector: the kernel then supplies exactly
     * what a LiveArea boot supplies (argc == 1, argv[0] == ""), which is the
     * launcher branch of launch_args_parse. The storage is cleared so a
     * later reader cannot mistake the previous launch's strings for this
     * call's arguments. */
    memset(g_argv_storage, 0, sizeof(g_argv_storage));
    memset(g_argv, 0, sizeof(g_argv));

    trace_line("launcher: LoadExec --launcher (argv=NULL)");

    hand_over_prologue();
    return do_loadexec(NULL);
}

#else /* !__vita__ */

/*
 * Host build. sceAppMgrLoadExec does not exist, so both entry points report
 * that rather than pretending. The hooks still run in the documented order,
 * which keeps the front end host-compilable.
 */
int vita_loadexec_game(const char *game_path, const char *exec_name)
{
    if (!g_configured)
        return VITA_LOADEXEC_ERR_CONFIG;
    if (launch_args_build_game(game_path, exec_name, g_argv_storage,
                               sizeof(g_argv_storage), g_argv) != 0)
        return VITA_LOADEXEC_ERR_ARGS;
    trace_fmt("launcher: LoadExec --game '%s'", g_argv[1]);
    if (g_cfg.state_path)
        (void)launcher_state_save(g_cfg.state_path, g_argv[1]);
    hand_over_prologue();
    trace_line("launcher: LoadExec unavailable on this platform");
    return VITA_LOADEXEC_ERR_HOST;
}

int vita_loadexec_launcher(void)
{
    if (!g_configured)
        return VITA_LOADEXEC_ERR_CONFIG;
    memset(g_argv, 0, sizeof(g_argv));
    trace_line("launcher: LoadExec --launcher (argv=NULL)");
    hand_over_prologue();
    trace_line("launcher: LoadExec unavailable on this platform");
    return VITA_LOADEXEC_ERR_HOST;
}

#endif /* __vita__ */
