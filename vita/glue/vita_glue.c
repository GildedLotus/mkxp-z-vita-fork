// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vita_glue.c — Vita platform bootstrap.
 *
 * Keep this file free of EGL/SDL: it only prepares the process (log, clocks,
 * heaps, shader-cache path) so SDL_Init(VIDEO) can bring up vitaGL.
 *
 * Heaps: the sizing argument for the newlib heap lives in vita_glue.h next
 * to the constant. The CRT reads sceLibcHeapSize and _newlib_heap_size_user
 * before main, so defining them here fixes both for every binary that links
 * the glue.
 */

#include "vita_glue.h"
#include "vita_publish.h"
#ifndef VITA_MEASURE_SOURCE_SHA256
#define VITA_MEASURE_SOURCE_SHA256 "unconfigured"
#endif
/* Release version: configure -D stamps it from the repo root
 * VERSION file. Host compiles of this file have no configure and run as dev. */
#ifndef MKXPZ_VITA_VERSION
#define MKXPZ_VITA_VERSION "dev"
#endif

#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/appmgr.h>
#include <psp2/rtc.h>

#include <vitaGL.h> /* pool ledger, cache fallback path (vitagl-0005) */

#include <malloc.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/reent.h>
#include <sys/time.h>

/* CRT reads this before main. Do not also define it in the consumer.
 * No SceLibc heap is needed (nothing loads libc.suprx); the symbol stays
 * because the CRT and the converted metadata read it by name. */
unsigned int sceLibcHeapSize = 0;

/*
 * Newlib's sbrk heap: everything the engine, SDL and MRI allocate. The
 * symbol is a weak reference in VitaSDK's lib_a-sbrk.o and _init_vita_heap
 * branches on its ADDRESS, so leaving it undefined does not mean "no heap",
 * it means "128 MiB, decided by the toolchain". This definition makes the
 * size ours and lets vita_glue_init_log print it.
 */
unsigned int _newlib_heap_size_user = VITA_GLUE_NEWLIB_HEAP_BYTES;

/*
 * Main-thread stack. VitaSDK default is too small once C++ global ctors,
 * SDL, and later the mkxp-z event loop run on the main thread (the engine
 * puts Ruby+GL on a worker; main still needs headroom). 1 MiB matches the
 * the other Vita Ruby players. CRT reads this before main.
 */
unsigned int sceUserMainThreadStackSize = 1u * 1024u * 1024u;

#define MIB (1024 * 1024)

/* One formatted glue line (glue_logf). The longest one this file builds is a
 * module line: a path, bounded at 256 bytes by the buffers below, plus about
 * 45 bytes of decoration. 384 covers that and still leaves room inside
 * vita_glue_trace's own 512-byte buffer for the "vita-time: process=..."
 * prefix it may put in front of the result. */
#define GLUE_LOG_LINE_MAX 384

/* Initialized before worker creation; resource counters are RGSS-thread only. */
static int texture_trace_enabled;
static int gpu_telemetry_enabled;
unsigned vita_glue_frame_profile_interval;
int vita_glue_memory_ledger_enabled;
static unsigned memory_ledger_scene;
static unsigned long long memory_ledger_last_us;
static int memory_ledger_logged;
static int gpu_seal_abort_enabled;
static int audio_telemetry_enabled;
/* AppMgr event-id trace; read once by vita_glue_init_log,
 * consulted by vita_glue_poll_resume on the event thread afterwards. */
static int system_event_trace_enabled;
static unsigned texture_trace_count;
static unsigned texture_trace_scope_depth;
static int fbo_failure_probe_started;
static int log_sync_enabled;
/* Free memory on GC. All five are (re)set by
 * vita_glue_init_log; only vita_glue_gc_event writes them afterwards. */
static int gc_memory_enabled;
static int gc_memory_logged;
static unsigned gc_memory_count;
static unsigned gc_memory_logged_at;
static unsigned long long gc_memory_last_us;
/* The last "clocks applied" line, cpu/bus/gpu/xbar in MHz. */
static int applied_clocks[4];
static SceUID log_sync_fd = -1;
/* Sink lock and the partial line it protects (design note in vita_glue.h). */
static SceUID log_sync_mutex = -1;
static char log_sync_line[VITA_GLUE_LOG_SYNC_LINE_MAX];
static unsigned log_sync_line_used;

/* Queue gate protects memory only; producers try once, never wait. */
static struct {
	unsigned size;
	char bytes[VITA_GLUE_LOG_SYNC_LINE_MAX];
} log_async_ring[VITA_GLUE_LOG_ASYNC_SLOTS];
static unsigned log_async_gate, log_async_head, log_async_used;
static unsigned log_async_drops, log_async_quit;
static int log_async_accepting, log_async_atexit, log_async_started;
static SceUID log_async_thread = -1;

/* Sink recovery: a failed write reopens the log once and
 * retries; the next good write carries a one-line report. Sink lock held. */
static char log_sync_path[VITA_GLUE_LOG_PATH_MAX];
static int log_sync_cause, log_sync_notice;
static unsigned log_sync_reopens, log_sync_open_fails, log_sync_lost;

/* Standalone native probes link glue without MRI; the player pulls the arena
 * object through MRI's strong references. ABI: vita_fiber_arena.h. */
extern int vita_fiber_arena_live(unsigned *live, unsigned *capacity)
    __attribute__((weak));

/* ---- helpers ----------------------------------------------------------- */

/*
 * Every formatted line this file emits, and the reason there is no printf
 * left in it: see the printf note at the top of vita_glue.h. The line is
 * built on the stack and handed whole to vita_glue_trace, which is the only
 * writer here that has ever been observed to reach the log. No trailing
 * newline is needed — vita_glue_trace terminates the record.
 *
 * The format attribute keeps -Wformat pointed at these call sites, which
 * matters more than it did for printf: Vita newlib does not honour %zu, so
 * a size_t printed the obvious way silently produces nothing.
 */
__attribute__((format(printf, 1, 2)))
static void glue_logf(const char *fmt, ...)
{
	char line[GLUE_LOG_LINE_MAX];
	va_list args;

	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	vita_glue_trace(line);
}

static int ensure_log_dirs(void)
{
	static const char *const kDirs[] = {
		"ux0:/data",
		"ux0:/data/mkxp-z",
		"ux0:/data/mkxp-z/logs",
	};
	unsigned i;

	for (i = 0; i < sizeof(kDirs) / sizeof(kDirs[0]); i++)
		(void)sceIoMkdir(kDirs[i], 0777);
	return 0;
}

static int file_exists(const char *path)
{
	SceIoStat st;
	return sceIoGetstat(path, &st) >= 0;
}

/* ---- public API -------------------------------------------------------- */

/* Durable breadcrumb via sceIo — does not depend on stdio FILE* state.
 * Used before freopen and for lines that must survive a later truncate. */
static void raw_log(const char *path, const char *msg)
{
	SceUID fd;
	int n;

	if (!path || !msg)
		return;
	fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd < 0)
		return;
	n = (int)strlen(msg);
	(void)sceIoWrite(fd, msg, n);
	sceIoClose(fd);
}

/* Serializes the whole sink. newlib's FILE locking is compiled out on
 * VitaSDK (newlib.h leaves _RETARGETABLE_LOCKING undefined, so every
 * __lock_acquire is ((void)0) and _flockfile does nothing), and the RGSS
 * thread, the event thread and the OpenAL threads all reach std::cerr —
 * without this two lines splice inside one write loop. Created once and
 * never deleted: the sink lives as long as the process, and a delete would
 * race a thread that is already inside a lock. Recursive because
 * log_sync_emit takes the same lock the stdio hook is holding. */
static void log_sync_mutex_init(void)
{
	if (log_sync_mutex >= 0)
		return;
	/* Failure leaves the sink unlocked rather than silent: interleaved
	 * lines are recoverable, a missing log is not. */
	log_sync_mutex = sceKernelCreateMutex("mkxpz-log-sync",
	                                      SCE_KERNEL_MUTEX_ATTR_RECURSIVE,
	                                      0, NULL);
}

static void log_sync_lock(void)
{
	if (log_sync_mutex >= 0)
		(void)sceKernelLockMutex(log_sync_mutex, 1, NULL);
}

static void log_sync_unlock(void)
{
	if (log_sync_mutex >= 0)
		(void)sceKernelUnlockMutex(log_sync_mutex, 1);
}

/* Only complete, known trace records may defer sync. Stdio keeps the durable
 * default: its chunks may start partway through an error or contain a burst
 * of mixed records. Unknown formats and multiline traces also stay durable. */
static int log_sync_telemetry(const char *buf, unsigned n)
{
	static const char *const prefixes[] = {
		"vita-frame: n=", "vita-frame-counts: upload_bytes=",
		"vita-slow: f=", "vita-asset-window: f=",
		"vita-asset: f=", "vita-asset-counts: id=", "vita-buffer-delete: id=",
		"vita-plane: repeat=", "vita-texcache: f="
	};
	const char *end = (const char *)memchr(buf, '\n', n);
	unsigned i;
	if (!end || end != buf + n - 1)
		return 0;
	--n;
	/* vita_glue_trace's optional timestamp precedes the record type. */
	if (n > 19 && memcmp(buf, "vita-time: process=", 19) == 0) {
		buf += 19;
		while (buf < end && ((*buf >= '0' && *buf <= '9') || *buf == '.'))
			++buf;
		if (buf == end || *buf++ != ' ')
			return 0;
		n = (unsigned)(end - buf);
	}
	for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
		unsigned p = (unsigned)strlen(prefixes[i]);
		if (n > p && memcmp(buf, prefixes[i], p) == 0 &&
		    buf[p] >= '0' && buf[p] <= '9')
			return 1;
	}
	return 0;
}

/* One write loop; returns 0 once n bytes are out, else the failing rc. */
static int log_sync_write_all(const char *buf, unsigned n, unsigned *written)
{
	while (*written < n) {
		int count = (int)sceIoWrite(log_sync_fd, buf + *written, n - *written);
		if (count <= 0 || (unsigned)count > n - *written)
			return count < 0 ? count : -1;
		*written += (unsigned)count;
	}
	return 0;
}

/* A descriptor opened before standby can go stale, and the card's own
 * remount does not revive it. Open a fresh one before dropping the old, so
 * a failed open keeps the old descriptor in service. Caller holds the lock. */
static int log_sync_reopen_locked(int cause)
{
	SceUID fresh;

	if (!log_sync_path[0])
		return -1;
	log_sync_cause = cause;
	fresh = sceIoOpen(log_sync_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fresh < 0) {
		++log_sync_open_fails;
		return -1;
	}
	(void)sceIoClose(log_sync_fd);
	log_sync_fd = fresh;
	++log_sync_reopens;
	log_sync_notice = 1;
	return 0;
}

/* Report a reopen once the new descriptor takes a write; a failed report
 * stays pending. Written directly, never through the reopening path. */
static void log_sync_notice_locked(void)
{
	char line[144];
	unsigned written = 0;
	int n;

	if (!log_sync_notice)
		return;
	n = snprintf(line, sizeof(line),
	             "vita-log-sync: reopened after %s rc=0x%08x; reopens=%u "
	             "failed_opens=%u lost_records=%u\n",
	             log_sync_cause ? "write failure" : "resume",
	             (unsigned)log_sync_cause, log_sync_reopens,
	             log_sync_open_fails, log_sync_lost);
	if (n > 0 && (unsigned)n < sizeof(line) &&
	    log_sync_write_all(line, (unsigned)n, &written) == 0) {
		log_sync_notice = 0;
		log_sync_open_fails = log_sync_lost = 0;
	}
}

/* Crash-safe sink: loop sceIoWrite to completion, then sync durable records.
 * Short/invalid writes skip the sync so an incomplete record is not
 * treated as durable. A failed write reopens the file once and retries the
 * unwritten rest; when the reopen fails the record is lost and counted, and
 * the old descriptor stays for the next attempt. Sync failure returns an
 * error but leaves the fd open for later lines. Caller holds the sink lock. */
static int log_sync_write_locked(const char *buf, unsigned n, int sync)
{
	unsigned written = 0;
	int rc;

	if (log_sync_fd < 0 || !buf)
		return -1;
	if (n == 0)
		return 0;
	rc = log_sync_write_all(buf, n, &written);
	if (rc < 0) {
		if (log_sync_reopen_locked(rc) < 0) {
			++log_sync_lost;
			return -1;
		}
		log_sync_notice_locked();
		if (log_sync_write_all(buf, n, &written) < 0) {
			++log_sync_lost;
			return -1;
		}
	}
	/* Telemetry is synced by the native writer after each bounded batch. */
	if (sync && sceIoSyncByFd(log_sync_fd, 0) < 0)
		return -1;
	return 0;
}

static void log_async_lock(void)
{
	while (__atomic_exchange_n(&log_async_gate, 1, __ATOMIC_ACQUIRE))
		sceKernelDelayThread(100);
}

static void log_async_unlock(void)
{
	__atomic_store_n(&log_async_gate, 0, __ATOMIC_RELEASE);
}

static int log_async_enqueue(const char *buf, unsigned n)
{
	unsigned slot;
	if (__atomic_exchange_n(&log_async_gate, 1, __ATOMIC_ACQUIRE)) {
		__atomic_fetch_add(&log_async_drops, 1, __ATOMIC_RELAXED);
		return -1;
	}
	if (!log_async_accepting || log_async_used == VITA_GLUE_LOG_ASYNC_SLOTS ||
	    n > VITA_GLUE_LOG_SYNC_LINE_MAX) {
		__atomic_fetch_add(&log_async_drops, 1, __ATOMIC_RELAXED);
		log_async_unlock();
		return -1;
	}
	slot = (log_async_head + log_async_used) % VITA_GLUE_LOG_ASYNC_SLOTS;
	memcpy(log_async_ring[slot].bytes, buf, n);
	log_async_ring[slot].size = n;
	++log_async_used;
	log_async_unlock();
	return 0;
}

/* Caller holds the sink lock. Failed notices are retried on the next record;
 * a broken card must not suppress the following critical breadcrumb. */
static void log_async_report_locked(void)
{
	unsigned drops = __atomic_exchange_n(&log_async_drops, 0, __ATOMIC_RELAXED);
	if (drops) {
		char line[80];
		int n = snprintf(line, sizeof(line), "vita-log-sync: dropped=%u telemetry records\n", drops);
		if (log_sync_write_locked(line, (unsigned)n, 0) < 0)
			__atomic_fetch_add(&log_async_drops, drops, __ATOMIC_RELAXED);
	}
}

/* Snapshot the boundary under the queue gate, then release it before I/O.
 * Records accepted after that boundary follow the caller's critical record.
 * Only the sink-lock owner consumes; concurrent producers cannot extend a
 * drain indefinitely. A failed telemetry write never skips a critical write. */
static unsigned log_async_drain_locked(unsigned limit)
{
	unsigned count, left;
	log_async_lock();
	count = log_async_used < limit ? log_async_used : limit;
	log_async_unlock();
	for (left = count; left; --left) {
		char line[VITA_GLUE_LOG_SYNC_LINE_MAX];
		unsigned n;
		log_async_lock();
		n = log_async_ring[log_async_head].size;
		memcpy(line, log_async_ring[log_async_head].bytes, n);
		log_async_head = (log_async_head + 1) % VITA_GLUE_LOG_ASYNC_SLOTS;
		--log_async_used;
		log_async_unlock();
		log_async_report_locked();
		if (log_sync_write_locked(line, n, 0) < 0)
			__atomic_fetch_add(&log_async_drops, 1, __ATOMIC_RELAXED);
	}
	return count;
}

static int log_async_worker(SceSize args, void *argp)
{
	int pending_sync = 0;
	(void)args;
	(void)argp;
	while (!__atomic_load_n(&log_async_quit, __ATOMIC_ACQUIRE)) {
		unsigned count;
		log_sync_lock();
		count = log_async_drain_locked(VITA_GLUE_LOG_ASYNC_SLOTS);
		if (count) pending_sync = 1;
		/* Keep card flushes on this registered stack, outside the queue gate.
		 * Retry failed syncs even when idle, without spinning on an I/O error. */
		if (pending_sync && sceIoSyncByFd(log_sync_fd, 0) >= 0)
			pending_sync = 0;
		log_sync_unlock();
		if (!count || pending_sync)
			sceKernelDelayThread(10000);
	}
	return 0;
}

/* Emit one already-complete record. Callers that build a whole line
 * themselves (vita_glue_trace, boot_log_line) bypass the stdio line buffer. */
static int log_sync_emit(const char *buf, unsigned n)
{
	int rc;

	if (buf && log_sync_telemetry(buf, n))
		return log_async_enqueue(buf, n);
	log_sync_lock();
	(void)log_async_drain_locked(VITA_GLUE_LOG_ASYNC_SLOTS);
	log_async_report_locked();
	rc = log_sync_write_locked(buf, n, 1);
	log_sync_unlock();
	return rc;
}

/* Bytes of a line the stdio hook has seen without its terminator yet.
 * Cleared before the write so a failed emit drops that record instead of
 * re-sending it on the next line, which matches log_sync_write_locked's
 * "a short write was never durable" contract. Caller holds the lock. */
static int log_sync_flush_locked(void)
{
	unsigned n = log_sync_line_used;

	if (n == 0)
		return 0;
	log_sync_line_used = 0;
	return log_sync_write_locked(log_sync_line, n, 1);
}

/* Caller holds the lock. A line longer than the buffer is emitted in
 * buffer-sized chunks rather than dropped or truncated. */
static int log_sync_append_locked(const char *buf, unsigned n)
{
	while (n) {
		unsigned room, take;

		if (log_sync_line_used == VITA_GLUE_LOG_SYNC_LINE_MAX &&
		    log_sync_flush_locked() < 0)
			return -1;
		room = VITA_GLUE_LOG_SYNC_LINE_MAX - log_sync_line_used;
		take = n < room ? n : room;
		memcpy(log_sync_line + log_sync_line_used, buf, take);
		log_sync_line_used += take;
		buf += take;
		n -= take;
	}
	return 0;
}

/* FILE _write helper. Returns the byte count stdio expects, or -1.
 * Holds bytes until their newline: Debug() reaches this twice per line
 * (body, then std::endl into an _IONBF stream), and syncing each half cost
 * two sceIoSyncByFd per logged line — enough to distort the frame timings
 * the sink exists to measure. */
static int log_sync_stdio_write(void *cookie, const char *buf, int n)
{
	unsigned total, complete;
	int rc;

	(void)cookie;
	if (!buf || n <= 0)
		return n < 0 ? -1 : 0;
	if (log_sync_fd < 0)
		return -1;

	total = (unsigned)n;
	for (complete = total; complete && buf[complete - 1] != '\n'; complete--)
		;

	log_sync_lock();
	/* One boundary for the whole write, not between chunks of a long line. */
	(void)log_async_drain_locked(VITA_GLUE_LOG_ASYNC_SLOTS);
	log_async_report_locked();
	rc = log_sync_append_locked(buf, complete);
	if (rc == 0 && complete)
		rc = log_sync_flush_locked();
	if (rc == 0 && complete < total)
		rc = log_sync_append_locked(buf + complete, total - complete);
	log_sync_unlock();
	return rc < 0 ? -1 : n;
}

static _READ_WRITE_RETURN_TYPE
log_sync_file_write(struct _reent *reent, void *cookie, const char *buf,
                    _READ_WRITE_BUFSIZE_TYPE n)
{
	(void)reent;
	return (_READ_WRITE_RETURN_TYPE)log_sync_stdio_write(cookie, buf, (int)n);
}

/* Final stop. timeout_us == 0 is the historical wait-forever behavior, for
 * the startup reset where no writer exists yet; the exit paths pass
 * VITA_GLUE_LOG_STOP_TIMEOUT_US so a stalled card sync cannot wedge process
 * exit: the join and the relock are capped, and on a stall the writer and
 * its descriptor are left untouched for the caller's shutdown enforcer
 * Returns 0 when the writer is stopped and the
 * descriptor closed, -1 on a stalled or failed stop. */
static int log_sync_stop_bounded(unsigned timeout_us)
{
	SceUInt wait = timeout_us;
	int bounded = timeout_us != 0;

	log_async_lock();
	log_async_accepting = 0;
	log_async_unlock();
	__atomic_store_n(&log_async_quit, 1, __ATOMIC_RELEASE);
	/* Never join while holding the sink lock the writer needs to exit. */
	if (log_async_thread >= 0) {
		if (log_async_started &&
		    sceKernelWaitThreadEnd(log_async_thread, NULL,
		                           bounded ? &wait : NULL) < 0)
			/* Keep its descriptor and handle: unbounded callers retry,
			 * bounded callers hand the stall to the exit enforcer. */
			return -1;
		log_async_started = 0;
		if (sceKernelDeleteThread(log_async_thread) < 0)
			return -1;
		log_async_thread = -1;
	}
	/* The writer is dead; it can no longer release the sink lock a stalled
	 * sceIoWrite was holding, so the relock is capped too. */
	if (bounded) {
		if (log_sync_mutex >= 0 &&
		    sceKernelLockMutex(log_sync_mutex, 1, &wait) < 0)
			return -1;
	} else {
		log_sync_lock();
	}
	if (log_sync_fd >= 0) {
		(void)log_async_drain_locked(VITA_GLUE_LOG_ASYNC_SLOTS);
		log_async_report_locked();
		/* Whatever the hook was still assembling belongs in this log,
		 * not the next one. */
		(void)log_sync_flush_locked();
		(void)sceIoSyncByFd(log_sync_fd, 0);
		(void)sceIoClose(log_sync_fd);
		log_sync_fd = -1;
	}
	log_sync_line_used = 0;
	log_sync_enabled = 0;
	log_sync_unlock();
	return 0;
}

static void log_sync_stop(void)
{
	(void)log_sync_stop_bounded(0);
}

/* Process-exit cleanup can run after the engine's shutdown watchdog is
 * already gone, so it never waits forever: a stalled card costs telemetry,
 * not the exit. */
static void log_sync_stop_atexit(void)
{
	(void)log_sync_stop_bounded(VITA_GLUE_LOG_STOP_TIMEOUT_US);
}

static int log_sync_start(const char *path)
{
	log_sync_mutex_init();
	log_sync_stop();
	if (log_async_thread >= 0)
		return -1;
	if (!path || !path[0] || !file_exists(VITA_GLUE_LOG_SYNC_MARKER))
		return 0;
	log_sync_fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (log_sync_fd < 0)
		return -1;
	/* An over-long path leaves recovery off; the sink itself still works. */
	log_sync_path[0] = 0;
	if (strlen(path) < sizeof(log_sync_path))
		strcpy(log_sync_path, path);
	log_sync_cause = log_sync_notice = 0;
	log_sync_reopens = log_sync_open_fails = log_sync_lost = 0;
	log_sync_enabled = 1;
	/* No worker without serialization or an orderly exit hook. Critical logs
	 * still work on startup failure; telemetry drops instead of doing I/O. */
	if (log_sync_mutex < 0)
		return -1;
	if (!log_async_atexit) {
		if (atexit(log_sync_stop_atexit) != 0)
			return -1;
		log_async_atexit = 1;
	}
	__atomic_store_n(&log_async_quit, 0, __ATOMIC_RELEASE);
	log_async_thread = sceKernelCreateThread("mkxpz-log-writer", log_async_worker,
	                                       0x10000100, VITA_GLUE_LOG_WRITER_STACK,
	                                       0, 0, NULL);
	if (log_async_thread < 0)
		return -1;
	if (sceKernelStartThread(log_async_thread, 0, NULL) < 0) {
		if (sceKernelDeleteThread(log_async_thread) >= 0)
			log_async_thread = -1;
		return -1;
	}
	log_async_started = 1;
	log_async_lock();
	log_async_accepting = 1;
	log_async_unlock();
	return 0;
}

/* Route the existing stdout/stderr FILE objects (freopen identity preserved
 * so C++ iostreams keep working) through the synced sink. */
static void log_sync_attach_stdio(void)
{
	stdout->_write = log_sync_file_write;
	stderr->_write = log_sync_file_write;
}

/* ---- stdio descriptor recovery ----------------------- */

/* Newlib maps fd -> sceIo handle in __vita_fdmap (libc/sys/vita/io.c:16,
 * vitadescriptor.h:40-49 @2e428297); the layout below mirrors it. fd 1/2 are
 * "tty0:" handles opened by _init_vita_io (io.c:40-58); freopen leaves them
 * alone (the impure-data stdout it is given was never initialised, so
 * freopen.c:118-119 skips the close) and opens the log on fd 3+. A handle
 * held across standby goes stale (sceIoWrite rc 0x80010013 = ENODEV), so
 * Ruby's fd 1/2 writes raised. _write_r (syscalls.c:28-76) reads sce_uid
 * per call, so swapping it in place repairs the fd without moving it:
 * newlib has no dup2 and never hands out an fd below 3. */
struct stdio_fd_entry { int sce_uid, type, ref_count; char *filename; int flags; };
extern struct stdio_fd_entry *__vita_fdmap[];
extern SceKernelLwMutexWork _newlib_fd_mutex;
enum { STDIO_FD_FILE = 0, STDIO_FD_TTY = 3, STDIO_FD_TABLE = 256 };

static char stdio_path[VITA_GLUE_LOG_PATH_MAX];
static unsigned stdio_recovered, stdio_dropped;

static int stdio_fd_ours(int fd)
{
	return fd == 1 || fd == 2 || fd == stdout->_file || fd == stderr->_file;
}

static int stdio_fd_refresh(int fd)
{
	struct stdio_fd_entry *entry;
	SceUID fresh, old = -1;
	int swapped = 0;

	if (fd < 1 || fd >= STDIO_FD_TABLE)
		return 0;
	fresh = sceIoOpen(stdio_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fresh < 0)
		return 0;
	sceKernelLockLwMutex(&_newlib_fd_mutex, 1, NULL);
	entry = __vita_fdmap[fd];
	if (entry && entry->ref_count > 0 &&
	    (entry->type == STDIO_FD_TTY || entry->type == STDIO_FD_FILE)) {
		old = entry->sce_uid;
		entry->sce_uid = fresh;
		swapped = 1;
	}
	sceKernelUnlockLwMutex(&_newlib_fd_mutex, 1);
	if (!swapped) {
		(void)sceIoClose(fresh);
		return 0;
	}
	(void)sceIoClose(old);
	return 1;
}

/* Point fd 1, fd 2 and the freopen'd stdout/stderr descriptors at fresh
 * append handles on the log. Returns how many were repaired; *failed counts
 * the ones that could not be. No log line: this runs inside write paths. */
static unsigned stdio_fds_refresh(unsigned *failed)
{
	int fds[4] = { 1, 2, stdout->_file, stderr->_file };
	unsigned i, j, done = 0, bad = 0;

	if (stdio_path[0]) {
		for (i = 0; i < 4; ++i) {
			for (j = 0; j < i && fds[j] != fds[i]; ++j)
				;
			if (j < i)
				continue;
			if (stdio_fd_refresh(fds[i]))
				++done;
			else
				++bad;
		}
	}
	if (failed)
		*failed = bad;
	return done;
}

/* Linked with -Wl,--wrap=_write_r; a build without the flag
 * never calls it. A write to a stdio descriptor that fails gets one repair
 * and one retry, then reports success: a diagnostic stream must not raise
 * into Ruby. No logging here (a failing log write would recurse). */
extern ssize_t __real__write_r(struct _reent *, int, const void *, size_t)
    __attribute__((weak));

ssize_t __wrap__write_r(struct _reent *reent, int fd, const void *buf, size_t n)
{
	ssize_t rc = __real__write_r(reent, fd, buf, n);

	if (rc >= 0 || !stdio_path[0] || !stdio_fd_ours(fd))
		return rc;
	if (stdio_fds_refresh(NULL) != 0)
		rc = __real__write_r(reent, fd, buf, n);
	if (rc >= 0) {
		__atomic_fetch_add(&stdio_recovered, 1, __ATOMIC_RELAXED);
		return rc;
	}
	__atomic_fetch_add(&stdio_dropped, 1, __ATOMIC_RELAXED);
	reent->_errno = 0;
	return (ssize_t)n;
}

static void stdio_resume_recover(void)
{
	unsigned failed, done = stdio_fds_refresh(&failed);

	if (done || failed)
		glue_logf("vita-stdio: reopened after resume fds=%u failed=%u "
		          "recovered_writes=%u dropped_writes=%u", done, failed,
		          __atomic_load_n(&stdio_recovered, __ATOMIC_RELAXED),
		          __atomic_load_n(&stdio_dropped, __ATOMIC_RELAXED));
}

/* ---- end stdio descriptor recovery ---- */

int vita_glue_shutdown_log(void)
{
	return log_sync_stop_bounded(VITA_GLUE_LOG_STOP_TIMEOUT_US);
}

/* ---- controlled native-abort probe ------------------- */

static SceUID log_abort_probe_thread = -1;

/* One poll, in three outcomes: 0 keep waiting, 1 aborting, -1 disarmed.
 * Only the successful removal arms the abort — the same contract as the
 * fbo-failure-dump marker — so a trigger this process cannot consume must
 * never become a crash loop on a broken card. The breadcrumb deliberately
 * goes through the sink under test, so the collected log names the abort
 * point. Everything here runs on the probe thread's registered stack. */
static int log_abort_probe_poll(void)
{
	if (!file_exists(VITA_GLUE_LOG_ABORT_PROBE_TRIGGER))
		return 0;
	if (sceIoRemove(VITA_GLUE_LOG_ABORT_PROBE_TRIGGER) < 0) {
		glue_logf("vita-log-abort: trigger remove failed; probe disarmed");
		return -1;
	}
	vita_glue_trace("vita-log-abort: trigger consumed; abort() follows");
	abort();
	return 1;
}

static int log_abort_probe_worker(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	while (log_abort_probe_poll() == 0)
		sceKernelDelayThread(VITA_GLUE_LOG_ABORT_PROBE_POLL_US);
	return 0;
}

void vita_glue_log_abort_probe_arm(void)
{
	if (log_abort_probe_thread >= 0 ||
	    !file_exists(VITA_GLUE_LOG_ABORT_PROBE_MARKER))
		return;
	/* A trigger an earlier run left behind must not abort this boot before
	 * its diagnostic has emitted its lines. */
	if (file_exists(VITA_GLUE_LOG_ABORT_PROBE_TRIGGER)) {
		if (sceIoRemove(VITA_GLUE_LOG_ABORT_PROBE_TRIGGER) < 0) {
			glue_logf("vita-log-abort: stale trigger remove FAILED; not armed");
			return;
		}
		glue_logf("vita-log-abort: stale trigger removed before arming");
	}
	log_abort_probe_thread = sceKernelCreateThread("mkxpz-log-abort",
	                                               log_abort_probe_worker,
	                                               0x10000100,
	                                               VITA_GLUE_LOG_ABORT_PROBE_STACK,
	                                               0, 0, NULL);
	if (log_abort_probe_thread < 0) {
		glue_logf("vita-log-abort: thread create failed uid=0x%08X; not armed",
		          (unsigned)log_abort_probe_thread);
		log_abort_probe_thread = -1;
		return;
	}
	if (sceKernelStartThread(log_abort_probe_thread, 0, NULL) < 0) {
		glue_logf("vita-log-abort: thread start failed; probe not armed");
		if (sceKernelDeleteThread(log_abort_probe_thread) >= 0)
			log_abort_probe_thread = -1;
		return;
	}
	glue_logf("vita-log-abort: armed; poll=%ums trigger=%s",
	          (unsigned)(VITA_GLUE_LOG_ABORT_PROBE_POLL_US / 1000u),
	          VITA_GLUE_LOG_ABORT_PROBE_TRIGGER);
}

static void boot_log_line(const char *path, const char *msg)
{
	if (log_sync_enabled)
		(void)log_sync_emit(msg, (unsigned)strlen(msg));
	else
		raw_log(path, msg);
}

static void glue_fflush(void)
{
	if (!log_sync_enabled)
		fflush(NULL);
}

/* Breadcrumb for main.cpp / rgss thread. Default path: unbuffered stdout +
 * fflush(stdout). Marker path: sceIoWrite with critical-record sync,
 * independent of worker newlib reentrancy / fflush(NULL). */
void vita_glue_trace(const char *msg)
{
	char timed[512];
	if (!msg)
		return;
	if (strncmp(msg, "vita-boot:", 10) == 0 && !strstr(msg, "t_ms=")) {
		snprintf(timed, sizeof(timed), "vita-boot: t_ms=%u %s",
		         (unsigned)(sceKernelGetProcessTimeWide() / 1000u), msg + 10);
		msg = timed;
	}
	if (log_sync_enabled) {
		char line[512];
		unsigned pos = 0;
		int need_nl = msg[0] && msg[strlen(msg) - 1] != '\n';

		if (texture_trace_enabled) {
			unsigned long long us = sceKernelGetProcessTimeWide();
			int n = snprintf(line, sizeof(line), "vita-time: process=%u.%06u ",
			                 (unsigned)(us / 1000000), (unsigned)(us % 1000000));
			if (n > 0)
				pos = (unsigned)n < sizeof(line) ? (unsigned)n : sizeof(line) - 1;
		}
		while (*msg && pos + 1 < sizeof(line))
			line[pos++] = *msg++;
		if (need_nl) {
			if (pos + 1 < sizeof(line))
				line[pos++] = '\n';
			else if (pos > 0)
				line[pos - 1] = '\n';
		}
		(void)log_sync_emit(line, pos);
		return;
	}
	if (texture_trace_enabled) {
		unsigned long long us = sceKernelGetProcessTimeWide();
		char prefix[64];
		snprintf(prefix, sizeof(prefix), "vita-time: process=%u.%06u ",
		         (unsigned)(us / 1000000), (unsigned)(us % 1000000));
		/* Explicit stream, like the breadcrumb below. printf would not
		 * reach this FILE at all: it resolves __getreent()->_stdout
		 * inside newlib, never the _impure_ptr->_stdout that `stdout`
		 * means here — on any thread (vita_glue.h). */
		fputs(prefix, stdout);
	}
	fputs(msg, stdout);
	if (msg[0] && msg[strlen(msg) - 1] != '\n')
		fputc('\n', stdout);
	fflush(stdout);
}

void vita_glue_texture_trace(const char *stage, unsigned texture,
                             unsigned framebuffer, int width, int height)
{
	char line[224];
	if (!texture_trace_enabled || texture_trace_count >= VITA_GLUE_TEXTURE_TRACE_LIMIT)
		return;
	++texture_trace_count;
	snprintf(line, sizeof(line), "vita-texture: #%u %s tex=%u fbo=%u size=%dx%d",
	         texture_trace_count, stage, texture, framebuffer, width, height);
	vita_glue_trace(line);
	if (texture_trace_count == VITA_GLUE_TEXTURE_TRACE_LIMIT)
		vita_glue_trace("vita-texture: limit reached; resource trace stopped");
}

void vita_glue_texture_scope_enter(void)
{
	++texture_trace_scope_depth;
}

void vita_glue_texture_scope_leave(void)
{
	if (texture_trace_scope_depth)
		--texture_trace_scope_depth;
}

int vita_glue_texture_scope_active(void)
{
	return texture_trace_enabled && texture_trace_scope_depth != 0;
}

int vita_glue_gpu_telemetry_enabled(void)
{
	return gpu_telemetry_enabled;
}

/* Frame profiler: no work beyond the boot gate when absent. */
static unsigned frame_profile_marker(void)
{
	char text[32], *end;
	unsigned long frames;
	FILE *file = fopen(VITA_GLUE_FRAME_PROFILE_MARKER, "r");
	if (!file) return 0;
	const size_t length = fread(text, 1, sizeof(text) - 1, file);
	fclose(file);
	text[length] = 0;
	if (text[0] < '0' || text[0] > '9') return 60;
	frames = strtoul(text, &end, 10);
	while (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t') ++end;
	return !*end && frames >= 1 && frames <= 3600 ? (unsigned)frames : 60;
}

double vita_glue_frame_profile_now_us(void)
{
	return (double)sceKernelGetProcessTimeWide();
}

int vita_glue_frame_profile_thread(void)
{
	return sceKernelGetThreadId();
}

/* One RGSS writer; boot reservation and post-stop export are the only I/O
 * and allocation sites. Times are process-clock microseconds, debt is ticks. */
#define MEASURE_EVENTS 28672u
#define MEASURE_REPORTS 128u
typedef struct MeasureEvent {
	unsigned long long time, sequence;
	unsigned kind, flags;
	long long a, b, c, d, e;
} MeasureEvent;
typedef struct MeasureStore {
	MeasureEvent events[MEASURE_EVENTS];
	VitaFrameSummary reports[MEASURE_REPORTS];
} MeasureStore;
typedef char measure_size_check[(sizeof(MeasureStore) <= 2u*1024u*1024u) ? 1 : -1];
unsigned vita_measure_mode;
static MeasureStore *measure_store;
static unsigned measure_warmup, measure_settle, measure_ms, measure_count, measure_reports;
static int measure_clocks[4];
static unsigned measure_active, measure_done, measure_overflow, measure_incomplete;
static unsigned measure_depth, measure_bucket, measure_original_f;
static unsigned long long measure_sequence, measure_start, measure_stop, measure_last;
static double measure_script, measure_scene;
static char measure_id[33], measure_config[65];

static int measure_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

static int measure_decimal(const char *text, unsigned *out)
{
	unsigned value = 0;
	if (!*text) return 0;
	for (; *text; ++text) {
		if (*text < '0' || *text > '9') return 0;
		unsigned digit = (unsigned)(*text - '0');
		if (value > (UINT_MAX - digit) / 10u) return 0;
		value = value * 10u + digit;
	}
	*out = value;
	return 1;
}

static void measure_init(void)
{
	if (!strcmp(VITA_MEASURE_SOURCE_SHA256, "unconfigured")) return;
	char text[192], *token[6], *at = text;
	unsigned version, mode, warmup, ms;
	FILE *f = fopen(VITA_MEASURE_MARKER, "rb");
	if (!f) return;
	size_t n = fread(text, 1, sizeof(text)-1, f);
	int bad = ferror(f) || !feof(f);
	if (fclose(f) != 0) bad = 1;
	text[n] = 0;
	if (bad || memchr(text, 0, n)) return;
	for (unsigned i = 0; i < 6; ++i) {
		while (measure_space(*at)) ++at;
		if (!*at) return;
		token[i] = at;
		while (*at && !measure_space(*at)) ++at;
		if (*at) *at++ = 0;
	}
	while (measure_space(*at)) ++at;
	if (*at || !measure_decimal(token[0], &version) || !measure_decimal(token[1], &mode) ||
	    !measure_decimal(token[2], &warmup) || !measure_decimal(token[3], &ms) ||
	    version != 1 || mode < 1 || mode > 4 || warmup > 36000 || ms < 1000 || ms > 120000 ||
	    strlen(token[4]) > 32 || strlen(token[5]) != 64 ||
	    strspn(token[5], "0123456789abcdef") != 64 ||
	    strspn(token[4], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(token[4])) return;
	if (mode != 1 && !vita_glue_frame_profile_interval) return;
	measure_store = (MeasureStore *)malloc(sizeof(MeasureStore));
	if (!measure_store) return;
	measure_original_f = vita_glue_frame_profile_interval;
	vita_measure_mode = mode; measure_warmup = measure_settle = warmup; measure_ms = ms;
	strcpy(measure_id, token[4]); strcpy(measure_config, token[5]);
	if (mode == 1) vita_glue_frame_profile_interval = 0;
}

static void measure_event(unsigned kind, unsigned flags, long long a, long long b,
                          long long c, long long d, long long e)
{
	if (!measure_active) return;
	if (measure_count == MEASURE_EVENTS) { measure_overflow = 1; return; }
	MeasureEvent *r = &measure_store->events[measure_count++];
	r->time = sceKernelGetProcessTimeWide(); r->sequence = measure_sequence;
	r->kind = kind; r->flags = flags;
	r->a = a; r->b = b; r->c = c; r->d = d; r->e = e;
}

static void measure_charge(unsigned long long now)
{
	if (measure_bucket == 0) measure_script += now - measure_last;
	if (measure_bucket == 5) measure_scene += now - measure_last;
	measure_last = now;
}

unsigned vita_measure_enter(unsigned bucket)
{
	unsigned previous = measure_bucket;
	if (!measure_active) return previous;
	measure_charge(sceKernelGetProcessTimeWide());
	measure_bucket = bucket;
	if (bucket == 5) measure_event(7, 0, 0, 0, 0, 0, 0);
	return previous;
}

void vita_measure_leave(unsigned previous)
{
	if (!measure_active) return;
	measure_charge(sceKernelGetProcessTimeWide());
	if (measure_bucket == 8) measure_event(4, measure_depth == 0, 0, 0, 0, 0, 0);
	measure_bucket = previous;
}

void vita_measure_delay(long long before, long long after, long long elapsed,
                        long long target, unsigned long long frequency, unsigned flags)
{
	measure_event(3, flags, before, after, elapsed, target, (long long)frequency);
}
void vita_measure_reset(void) { measure_event(5, 0, 0, 0, 0, 0, 0); }
unsigned long long vita_measure_plane_begin(void)
{
	return measure_active ? sceKernelGetProcessTimeWide() : 0;
}
void vita_measure_plane_end(unsigned long long start)
{
	if (measure_active) measure_event(6, 0, sceKernelGetProcessTimeWide()-start, 0, 0, 0, 0);
}
int vita_measure_defer(const VitaFrameSummary *summary)
{
	if (vita_measure_mode != 3 || !measure_active) return 0;
	if (measure_reports == MEASURE_REPORTS) { measure_overflow = 1; return 1; }
	measure_store->reports[measure_reports++] = *summary;
	return 1;
}

/* Export-only formatting with checked short-write handling. */
static int measure_write(FILE *f, const char *format, ...)
{
	char line[768];
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof(line)) return 1;
	return fwrite(line, 1, (size_t)n, f) != (size_t)n;
}

static void measure_export(void)
{
	/* A footer alone cannot attest to a successful flush/close. Require the
	 * matching completion log; failed/new partial files are always retained. */
	char path[sizeof(VITA_MEASURE_OUTPUT_PREFIX) + sizeof(measure_id) + 4], status[256];
	int length = snprintf(path, sizeof(path), "%s%s.csv", VITA_MEASURE_OUTPUT_PREFIX, measure_id);
	if (length < 0 || (size_t)length >= sizeof(path)) {
		vita_glue_trace("frame-debt: export path failed; no output created"); return;
	}
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		snprintf(status, sizeof(status), "frame-debt: export not created path=%s; open failed (existing evidence unchanged)", path);
		vita_glue_trace(status); return;
	}
	FILE *f = fdopen(fd, "wb");
	if (!f) {
		int close_failed = close(fd) != 0;
		snprintf(status, sizeof(status), "frame-debt: incomplete artifact=%s; fdopen failed close_failed=%d", path, close_failed);
		vita_glue_trace(status); return;
	}
	int bad = 0;
	bad |= measure_write(f, "frame-debt-v1,source_sha256=%s,id=%s,config_sha256=%s,mode=%u,F=%u,L=%d,G=%d,clock=process_us,start=%llu,stop=%llu,requested_ms=%u,warmup_updates=%u,clocks=%d/%d/%d/%d,incomplete=1\n",
	        VITA_MEASURE_SOURCE_SHA256, measure_id, measure_config, vita_measure_mode, measure_original_f,
	        log_sync_enabled, gpu_telemetry_enabled, measure_start, measure_stop,
	        measure_ms, measure_settle, measure_clocks[0], measure_clocks[1], measure_clocks[2], measure_clocks[3]);
	bad |= measure_write(f, "time_us,update_sequence,kind,flags,a,b,c,d,e\n");
	for (unsigned i=0; i<measure_count; ++i) {
		const MeasureEvent *r = &measure_store->events[i];
		bad |= measure_write(f, "%llu,%llu,%u,%u,%lld,%lld,%lld,%lld,%lld\n", r->time,
		        r->sequence, r->kind, r->flags, r->a, r->b, r->c, r->d, r->e);
	}
	/* Numeric, lossless summary export avoids recursively invoking the sink. */
	for (unsigned i=0; i<measure_reports; ++i) {
		const VitaFrameSummary *s = &measure_store->reports[i];
		bad |= measure_write(f, "summary,%u,%u,%u,%u,%u,%u,%llu,%.17g,%.17g,%.17g,%.17g", i,
		        s->frames,s->skipped,s->frozen,s->late,s->quads,s->upload_bytes,
		        s->wall_us,s->max_us,s->p50_us,s->p95_us);
		for (unsigned j=0;j<VITA_FRAME_BUCKETS;++j) bad |= measure_write(f, ",%.17g,%u",s->us[j],s->count[j]);
		for (unsigned j=0;j<6;++j) bad |= measure_write(f, ",%u",s->gpu[j]);
		for (unsigned j=0;j<VITA_RASTER_OPS;++j) bad |= measure_write(f, ",%llu,%llu",s->raster.calls[j],s->raster.pixels[j]);
		for (unsigned j=0;j<3;++j) bad |= measure_write(f, ",%llu,%llu",s->raster.dst_alpha_calls[j],s->raster.dst_alpha_pixels[j]);
		bad |= measure_write(f, "\n");
	}
	bad |= ferror(f) != 0;
	bad |= measure_write(f, "end,events=%u,reports=%u,overflow=%u,incomplete=%u\n", measure_count,
	        measure_reports, measure_overflow, measure_incomplete || bad);
	if (fflush(f) != 0) bad = 1;
	if (fsync(fileno(f)) != 0) bad = 1;
	if (fclose(f) != 0) bad = 1;
	if (bad) measure_incomplete = 1;
	snprintf(status, sizeof(status), bad ? "frame-debt: incomplete artifact=%s; write/flush/sync/close failed" :
	         "frame-debt: export complete path=%s", path);
	vita_glue_trace(status);
}

void vita_measure_begin(int internal)
{
	if (!vita_measure_mode || measure_done) return;
	if (!measure_active) {
		if (measure_warmup) { --measure_warmup; return; }
		measure_active = 1;
		measure_start = measure_last = sceKernelGetProcessTimeWide();
		memcpy(measure_clocks, applied_clocks, sizeof(measure_clocks));
	}
	++measure_sequence;
	if (measure_depth++) measure_incomplete = 1;
	measure_charge(sceKernelGetProcessTimeWide()); measure_bucket = 7;
	measure_event(1, internal != 0, 0, 0, 0, 0, 0);
}
void vita_measure_end(unsigned flags)
{
	if (!measure_active) return;
	if (!measure_depth) { measure_incomplete = 1; return; }
	if (memcmp(measure_clocks, applied_clocks, sizeof(measure_clocks))) measure_incomplete = 1;
	measure_charge(sceKernelGetProcessTimeWide());
	measure_event(2, flags, (long long)measure_script, (long long)measure_scene, 0, 0, 0);
	measure_script = measure_scene = 0; measure_bucket = 0;
	if (--measure_depth) return;
	unsigned long long now = sceKernelGetProcessTimeWide();
	if (now - measure_start < (unsigned long long)measure_ms * 1000) return;
	measure_stop = now; measure_active = 0; measure_done = 1;
	measure_export();
}

static void frame_profile_ms(char *out, double us)
{
	/* Keep even a suspended frame bounded in the crash-safe sink. */
	if (us >= 99999999.0) { strcpy(out, ">99999"); return; }
	unsigned tenths = us > 0 ? (unsigned)(us / 100.0 + 0.5) : 0;
	snprintf(out, 16, "%u.%u", tenths / 10, tenths % 10);
}

/* Slash-joined decimal list of n counters into out (bounded by cap). */
static size_t frame_profile_u64_list(char *out, size_t cap,
                                     const unsigned long long *v, unsigned n)
{
	size_t at = 0;
	for (unsigned i = 0; i < n; ++i) {
		const int wrote = snprintf(out + at, cap - at, "%s%llu",
		                           i ? "/" : "", v[i]);
		if (wrote < 0)
			return at;
		at += (size_t)wrote;
		if (at >= cap)
			return cap - 1;
	}
	return at;
}

void vita_glue_frame_profile_log(const VitaFrameSummary *s)
{
	const unsigned long long emission_start = measure_active ? sceKernelGetProcessTimeWide() : 0;
	if (vita_measure_defer(s)) {
		measure_event(8, 1, sceKernelGetProcessTimeWide()-emission_start, measure_reports-1, 0, 0, 0);
		return;
	}
	char ms[VITA_FRAME_BUCKETS + 4][16], line[768];
	if (!s->frames) return;
	for (unsigned i = 0; i < VITA_FRAME_BUCKETS; ++i)
		frame_profile_ms(ms[i], s->us[i] / s->frames);
	frame_profile_ms(ms[12], s->wall_us / s->frames);
	frame_profile_ms(ms[13], s->p50_us);
	frame_profile_ms(ms[14], s->p95_us);
	frame_profile_ms(ms[15], s->max_us);
	snprintf(line, sizeof(line),
	         "vita-frame: n=%u script=%s gc=%s/%u raster=%s/%u compose=%s/%u "
	         "upload=%s/%u scene=%s blit=%s submit=%s swap=%s idle=%s io=%s decode=%s "
	         "wall=%s p50=%s p95=%s max=%s skip=%u frozen=%u late=%u fps=%u",
	         s->frames, ms[0], ms[1], s->count[1], ms[2], s->count[2], ms[3], s->count[3],
	         ms[4], s->count[4], ms[5], ms[6], ms[7], ms[8], ms[9], ms[10], ms[11],
	         ms[12], ms[13], ms[14], ms[15], s->skipped, s->frozen, s->late,
	         s->wall_us > 0 ? (unsigned)(s->frames * 1000000.0 / s->wall_us + 0.5) : 0);
	vita_glue_trace(line);
	size_t at = snprintf(line, sizeof(line),
	         "vita-frame-counts: upload_bytes=%llu quads=%u swaps=%u reads=%u decodes=%u "
	         "gpu_peak=%u/%u/%u/%u/%u/%u clocks=%d/%d/%d/%d",
	         s->upload_bytes, s->quads, s->count[8], s->count[10], s->count[11],
	         s->gpu[0], s->gpu[1], s->gpu[2], s->gpu[3], s->gpu[4], s->gpu[5],
	         applied_clocks[0], applied_clocks[1], applied_clocks[2], applied_clocks[3]);
	if (at < sizeof(line)) {
		at += snprintf(line + at, sizeof(line) - at, " raster_calls=");
		at += frame_profile_u64_list(line + at, sizeof(line) - at,
		                             s->raster.calls, VITA_RASTER_OPS);
	}
	if (at < sizeof(line)) {
		at += snprintf(line + at, sizeof(line) - at, " raster_px=");
		at += frame_profile_u64_list(line + at, sizeof(line) - at,
		                             s->raster.pixels, VITA_RASTER_OPS);
	}
	if (at < sizeof(line)) {
		at += snprintf(line + at, sizeof(line) - at, " dst_a_calls=");
		at += frame_profile_u64_list(line + at, sizeof(line) - at,
		                             s->raster.dst_alpha_calls, 3);
	}
	if (at < sizeof(line)) {
		at += snprintf(line + at, sizeof(line) - at, " dst_a_px=");
		at += frame_profile_u64_list(line + at, sizeof(line) - at,
		                             s->raster.dst_alpha_pixels, 3);
	}
	vita_glue_trace(line);
	if (measure_active) measure_event(8, 0, sceKernelGetProcessTimeWide()-emission_start, 0, 0, 0, 0);
}

static void memory_ledger_init(void)
{
	vita_glue_memory_ledger_enabled = file_exists(VITA_GLUE_MEMORY_LEDGER_MARKER);
	memory_ledger_scene = 0;
	memory_ledger_last_us = 0;
	memory_ledger_logged = 0;
}

int vita_glue_memory_ledger_due(void)
{
	unsigned long long now;
	if (!vita_glue_memory_ledger_enabled) return 0;
	++memory_ledger_scene;
	now = sceKernelGetProcessTimeWide();
	if (memory_ledger_logged && now >= memory_ledger_last_us &&
	    now - memory_ledger_last_us < VITA_GLUE_MEMORY_LEDGER_PERIOD_US)
		return 0;
	memory_ledger_logged = 1;
	memory_ledger_last_us = now;
	return 1;
}

void vita_glue_memory_ledger_log(const VitaMemoryResources *r)
{
	char line[480], kernel[40], fibers[24];
	struct mallinfo heap;
	SceKernelFreeMemorySizeInfo info;
	unsigned live = 0, capacity = 0, arena, limit;
	int rc;
	if (!vita_glue_memory_ledger_enabled) return;
	heap = mallinfo();
	arena = (unsigned)heap.arena;
	limit = _newlib_heap_size_user;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	rc = sceKernelGetFreeMemorySize(&info);
	if (rc < 0)
		strcpy(kernel, "na/na/na");
	else
		snprintf(kernel, sizeof(kernel), "%u/%u/%u", (unsigned)info.size_user,
		         (unsigned)info.size_cdram, (unsigned)info.size_phycont);
	if (!vita_fiber_arena_live || vita_fiber_arena_live(&live, &capacity))
		strcpy(fibers, "na/na");
	else
		snprintf(fibers, sizeof(fibers), "%u/%u", live, capacity);
	/* mallinfo.keepcost is the top chunk, NOT the largest free block.
	 * Storage records describe requested bytes, not driver residency. */
	snprintf(line, sizeof(line),
	         "vita-memory: scene=%u ms=%llu newlib=%u/%u/%u/%u/%u/na "
	         "libc=na/na services=na/na driver=na/na kernel=%s krc=%d "
	         "cpu_px=%llu tex=%llu buf=%llu gpu=%u/%u/%u retire=%u/%llu fibers=%s",
	         memory_ledger_scene, memory_ledger_last_us / 1000ull,
	         (unsigned)heap.uordblks, (unsigned)heap.fordblks, arena,
	         (unsigned)heap.keepcost, arena < limit ? limit - arena : 0,
	         kernel, rc, r->cpu_pixels, r->texture_bytes, r->buffer_bytes,
	         r->textures, r->vbos, r->surfaces, r->pending, r->pending_bytes, fibers);
	vita_glue_trace(line);
	vita_glue_vgl_pool_ledger("scene");
}

unsigned vita_glue_vgl_pool_bytes(const char *name, int request_mib, unsigned default_mib,
                                  unsigned min_mib, unsigned max_mib,
                                  unsigned long long free_bytes)
{
	char line[160];
	unsigned mib = default_mib;
	if (request_mib != 0) {
		if (request_mib >= (int)min_mib && request_mib <= (int)max_mib &&
		    (unsigned long long)request_mib * MIB <= free_bytes)
			mib = (unsigned)request_mib;
		else {
			snprintf(line, sizeof(line),
			         "vita-vgl-pool: %s request=%dMiB rejected (allowed %u..%u MiB, free %llu bytes); using %u MiB",
			         name, request_mib, min_mib, max_mib, free_bytes, default_mib);
			vita_glue_trace(line);
		}
	}
	return mib * MIB;
}

void vita_glue_vgl_pools(int ram_mib, int cdram_mib, int phycont_mib, VitaVglPools *out)
{
	SceKernelFreeMemorySizeInfo info;
	unsigned long long user = ~0ull, cdram = ~0ull, phycont = ~0ull;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (sceKernelGetFreeMemorySize(&info) >= 0) {
		user = (unsigned)info.size_user;
		cdram = (unsigned)info.size_cdram;
		phycont = (unsigned)info.size_phycont;
	}
	/* RAM max is vitaGL's uncached-RAM cap (mem_utils.c). */
	out->ram = vita_glue_vgl_pool_bytes("ram", ram_mib, 96, 16, 200, user);
	out->cdram = vita_glue_vgl_pool_bytes("cdram", cdram_mib, 64, 16, 112, cdram);
	out->phycont = vita_glue_vgl_pool_bytes("phycont", phycont_mib, 16, 4, 26, phycont);
}

void vita_glue_vgl_pool_ledger(const char *tag)
{
	char line[256];
	if (!vita_glue_memory_ledger_enabled) return;
	snprintf(line, sizeof(line),
	         "vita-vgl-pool: tag=%s scene=%u ram=%llu/%llu cdram=%llu/%llu "
	         "phycont=%llu/%llu cdlg=%llu/%llu",
	         tag ? tag : "?", memory_ledger_scene,
	         (unsigned long long)vglMemFree(VGL_MEM_RAM), (unsigned long long)vglMemTotal(VGL_MEM_RAM),
	         (unsigned long long)vglMemFree(VGL_MEM_VRAM), (unsigned long long)vglMemTotal(VGL_MEM_VRAM),
	         (unsigned long long)vglMemFree(VGL_MEM_PHYCONT), (unsigned long long)vglMemTotal(VGL_MEM_PHYCONT),
	         (unsigned long long)vglMemFree(VGL_MEM_BUDGET), (unsigned long long)vglMemTotal(VGL_MEM_BUDGET));
	vita_glue_trace(line);
}

int vita_glue_gpu_seal_abort_enabled(void)
{
	return gpu_seal_abort_enabled;
}

int vita_glue_audio_telemetry_enabled(void)
{
	return audio_telemetry_enabled;
}

/* Failure-only queries: no GXM initialization, allocations or GL mutations. */
int vita_glue_fbo_failure_probe_begin(void)
{
	if (!texture_trace_enabled || fbo_failure_probe_started)
		return 0;
	fbo_failure_probe_started = 1;
	return 1;
}

static void probe_render_target_size(int width, int height)
{
	SceGxmRenderTargetParams params;
	unsigned int bytes = 0xa5a5a5a5u;
	char line[192];
	int rc;

	if (width <= 0 || height <= 0 || width > 65535 || height > 65535) {
		vita_glue_trace("vita-fbo-probe: GXM size query skipped: dimensions out of range");
		return;
	}
	memset(&params, 0, sizeof(params));
	params.width = (unsigned short)width;
	params.height = (unsigned short)height;
	params.scenesPerFrame = 1;
	params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
	params.driverMemBlock = -1;
	rc = sceGxmGetRenderTargetMemSize(&params, &bytes);
	snprintf(line, sizeof(line),
	         "vita-fbo-probe: GXM size=%dx%d rc=0x%08x bytes=0x%08x output_unchanged=%d",
	         width, height, (unsigned)rc, bytes, bytes == 0xa5a5a5a5u);
	vita_glue_trace(line);
}

void vita_glue_fbo_failure_probe(int width, int height)
{
	char line[192];
	int rc;

	/* This API follows a successful probe_begin on the RGSS thread. */
	if (!texture_trace_enabled || !fbo_failure_probe_started)
		return;
	probe_render_target_size(width, height);
	probe_render_target_size(32, 32);

	/* Only a deliberately staged, successfully consumed marker permits a
	 * diagnostic abort, so the failing state is still in the core. */
	if (!file_exists(VITA_GLUE_FBO_FAILURE_DUMP_MARKER))
		return;
	rc = sceIoRemove(VITA_GLUE_FBO_FAILURE_DUMP_MARKER);
	if (rc < 0) {
		snprintf(line, sizeof(line),
		         "vita-fbo-probe: dump marker removal failed rc=0x%08x; continuing safe cleanup",
		         (unsigned)rc);
		vita_glue_trace(line);
		return;
	}
	vita_glue_trace("vita-fbo-probe: one-shot dump marker consumed; intentionally aborting before cleanup");
	fflush(NULL);
	abort();
}

/*
 * Name of generation `gen` of `path`: the suffix goes in front of the final
 * extension, so mkxp-z.log becomes mkxp-z.1.log and a path without an
 * extension simply gains ".1". Writes nothing and returns -1 when the name
 * would not fit or `gen` is not a single digit; the caller then leaves that
 * generation untouched rather than renaming onto a truncated name.
 */
static int rotated_log_name(char *out, unsigned size, const char *path,
                            unsigned gen)
{
	const char *dot = strrchr(path, '.');
	const char *slash = strrchr(path, '/');
	unsigned stem, ext;

	if (!dot || (slash && dot < slash))
		dot = path + strlen(path); /* no extension: append the suffix */
	stem = (unsigned)(dot - path);
	ext = (unsigned)strlen(dot);
	if (gen == 0 || gen > 9 || stem + 2 + ext + 1 > size)
		return -1;
	memcpy(out, path, stem);
	out[stem] = '.';
	out[stem + 1] = (char)('0' + gen);
	memcpy(out + stem + 2, dot, ext + 1);
	return 0;
}

/*
 * Shift the previous runs' logs one generation up so the truncating open
 * below cannot destroy them: generation KEEP is dropped,
 * KEEP-1 becomes KEEP, ..., and the live log becomes generation 1, leaving
 * the live name free for this run. Oldest first, so the only destination
 * that still holds a log when it is written is the oldest one.
 *
 * Nothing here may fail the boot. A generation whose source does not exist
 * yet is skipped silently (the normal case on early runs); a rename that
 * fails is counted and the reason is returned in `out`, which the caller
 * writes into the log once it is open. Return -1 on any failure so the
 * caller appends instead of risking the previous run's evidence.
 */
static int rotate_logs(const char *path, char *out, unsigned size)
{
	char dst[256], src[256];
	const char *from;
	unsigned gen, shifted = 0, failed = 0;
	int rc, last = 0;

	if (!file_exists(path)) {
		snprintf(out, size,
		         "vita_glue: log-rotate: no previous log, keep=%u\n",
		         (unsigned)VITA_GLUE_LOG_ROTATE_KEEP);
		return 0;
	}
	for (gen = VITA_GLUE_LOG_ROTATE_KEEP; gen >= 1; gen--) {
		if (rotated_log_name(dst, sizeof(dst), path, gen) < 0 ||
		    (gen > 1 &&
		     rotated_log_name(src, sizeof(src), path, gen - 1) < 0)) {
			++failed;
			last = VITA_GLUE_ERR_LOG;
			continue;
		}
		from = (gen == 1) ? path : src;
		if (!file_exists(from))
			continue; /* nothing has reached that generation yet */
		/* Only the oldest generation is ever deleted, and only once
		 * something exists to take its place: that is precisely the log
		 * this rotation drops. Younger destinations were vacated by the
		 * previous turn of this loop, so they are left alone — a rename
		 * that fails must not also cost a log rotation meant to keep.
		 * vita_publish_shift() refuses an occupied destination, so the
		 * delete is the only way the steady state (all KEEP generations
		 * exist) can proceed, and a refused rename keeps both files. */
		if (gen == VITA_GLUE_LOG_ROTATE_KEEP)
			(void)sceIoRemove(dst);
		rc = vita_publish_shift(from, dst);
		if (rc < 0) {
			++failed;
			last = rc;
		} else {
			++shifted;
		}
	}
	snprintf(out, size,
	         "vita_glue: log-rotate: shifted=%u failed=%u keep=%u rc=0x%08X\n",
	         shifted, failed, (unsigned)VITA_GLUE_LOG_ROTATE_KEEP,
	         (unsigned)last);
	return failed ? -1 : 0;
}

/* ---- title id ----------------------------------------------------------- */

/*
 * The app's own SFO descriptor, written into every VPK this repo packages by
 * vita-mksfoex (TITLE_ID=MKXPZ00xx) and mounted read-only with the app.
 *
 * sceAppMgrAppParamGetString reads this same descriptor, but calling it would
 * put -lSceAppMgr_stub on the link line of every glue consumer, and declaring
 * it weak does not help: an undefined weak reference never pulls a member out
 * of a stub archive, so the call would always see a null symbol (measured with
 * arm-vita-eabi-nm). Reading the file uses only the sceIo calls
 * the glue already depends on.
 *
 * Layout: 20-byte header (magic, version, key table start, data table start,
 * entry count) then one 16-byte index entry per parameter (key offset u16,
 * format u16, length u32, capacity u32, data offset u32).
 */
#define VITA_GLUE_SFO_PATH     "app0:sce_sys/param.sfo"
#define VITA_GLUE_SFO_MAGIC    0x46535000u /* "\0PSF" */
#define VITA_GLUE_SFO_UTF8     0x0204u
#define VITA_GLUE_SFO_HEADER   20u
#define VITA_GLUE_SFO_INDEX    16u
#define VITA_GLUE_SFO_MAX      4096u /* a packaged param.sfo is under 1 KiB */

static unsigned sfo_u16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned sfo_u32(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) |
	       ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* Bounded compare against a NUL-terminated key inside the key table. */
static int sfo_key_is(const unsigned char *sfo, unsigned bytes, unsigned at,
                      const char *key)
{
	unsigned i = 0;

	while (at + i < bytes && key[i] && sfo[at + i] == (unsigned char)key[i])
		i++;
	return !key[i] && at + i < bytes && sfo[at + i] == 0;
}

/*
 * Copy the UTF-8 parameter `key` out of an in-memory SFO. Returns 0 on
 * success. Every offset read from the file is range-checked against the bytes
 * actually read, so a truncated or corrupt descriptor fails instead of
 * reading past the buffer.
 */
static int sfo_lookup(const unsigned char *sfo, unsigned bytes,
                      const char *key, char *out, unsigned size)
{
	unsigned keys, data, count, i;

	if (size == 0 || bytes < VITA_GLUE_SFO_HEADER ||
	    sfo_u32(sfo) != VITA_GLUE_SFO_MAGIC)
		return -1;
	keys = sfo_u32(sfo + 8);
	data = sfo_u32(sfo + 12);
	count = sfo_u32(sfo + 16);
	if (keys >= bytes || data >= bytes ||
	    count > (bytes - VITA_GLUE_SFO_HEADER) / VITA_GLUE_SFO_INDEX)
		return -1;
	for (i = 0; i < count; i++) {
		const unsigned char *e =
			sfo + VITA_GLUE_SFO_HEADER + i * VITA_GLUE_SFO_INDEX;
		unsigned len = sfo_u32(e + 4);
		unsigned off = sfo_u32(e + 12);
		unsigned n;

		if (!sfo_key_is(sfo, bytes, keys + sfo_u16(e), key))
			continue;
		/* Compared against the space left, never added to it: a wild
		 * offset must not wrap back into the buffer. */
		if (sfo_u16(e + 2) != VITA_GLUE_SFO_UTF8 || len == 0 ||
		    off >= bytes - data || bytes - data - off < len)
			return -1;
		n = len < size ? len : size;
		memcpy(out, sfo + data + off, n);
		out[n - 1] = 0; /* also terminates a truncated or unterminated value */
		return 0;
	}
	return -1;
}

/*
 * Title id of the running app ("MKXPZ0001", "MKXPZ0035", ...), or "unknown".
 * The product player and the diagnostic player share VITA_GLUE_LOG_PATH, so a
 * collected log has to say which one wrote it; this is the first line of every
 * log. Failure at any step is inert: the id is simply unknown.
 */
static void app_title_id(char *out, unsigned size)
{
	unsigned char sfo[VITA_GLUE_SFO_MAX];
	unsigned got = 0, i;
	SceUID fd;

	if (size == 0)
		return;
	snprintf(out, size, "unknown");
	fd = sceIoOpen(VITA_GLUE_SFO_PATH, SCE_O_RDONLY, 0777);
	if (fd < 0)
		return;
	while (got < sizeof(sfo)) { /* a short read is not end of file */
		int n = (int)sceIoRead(fd, sfo + got, sizeof(sfo) - got);
		if (n <= 0 || (unsigned)n > sizeof(sfo) - got)
			break;
		got += (unsigned)n;
	}
	sceIoClose(fd);
	if (sfo_lookup(sfo, got, "TITLE_ID", out, size) < 0) {
		snprintf(out, size, "unknown");
		return;
	}
	/* Whatever the descriptor holds, this stays one printable line. */
	for (i = 0; out[i]; i++)
		if (out[i] < 0x20 || out[i] > 0x7e)
			out[i] = '?';
	if (!out[0])
		snprintf(out, size, "unknown");
}

int vita_glue_init_log(const char *log_path)
{
	const char *path = (log_path && log_path[0]) ? log_path : VITA_GLUE_LOG_PATH;
	char rotation[128];
	char title[64];
	int rotation_rc;
	SceUID fd;
	char line[224];

	ensure_log_dirs();
	log_sync_stop();
	if (log_async_thread >= 0)
		return VITA_GLUE_ERR_LOG;

	/* Keep the previous runs before anything truncates: two runs
	 * back to back, or a crash and its relaunch, must not cost the earlier
	 * log. Reported below, into the new log — never fatal. */
	rotation_rc = rotate_logs(path, rotation, sizeof(rotation));
	app_title_id(title, sizeof(title));

	/* Truncate once via sceIo, write the banner here, then attach
	 * stdout/stderr in append-only mode. The old freopen(stdout,"w")
	 * + freopen(stderr,"a") pair on the same path truncated glue lines
	 * on Vita newlib (observed hardware log was only mkxp-z Debug()).
	 * A failed rotation may have left the crash log here: preserve it. */
	fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT |
	              (rotation_rc < 0 ? SCE_O_APPEND : SCE_O_TRUNC), 0777);
	if (fd < 0)
		return VITA_GLUE_ERR_LOG;
	sceIoClose(fd);

	(void)log_sync_start(path);
	stdio_path[0] = 0;
	if (strlen(path) < sizeof(stdio_path))
		strcpy(stdio_path, path);

	/* First line of this run: who wrote it. Then which build, then how the
	 * previous ones fared. */
	snprintf(line, sizeof(line), "vita_glue: title id %s\n", title);
	boot_log_line(path, line);
	boot_log_line(path, "vita_glue: version " MKXPZ_VITA_VERSION "\n");
	boot_log_line(path, rotation);
	snprintf(line, sizeof(line),
		 "vita_glue: log file %s (%s + append stdio)\n",
		 path, rotation_rc < 0 ? "rotation failed; sceIo append" :
		                        "rotate + sceIo truncate");
	boot_log_line(path, line);
	/* Both configurable heaps, every run, before anything can fail: which
	 * allocator a later number belongs to is not inferable from the build. */
	snprintf(line, sizeof(line),
		 "vita_glue: sceLibcHeapSize = %u bytes (%u MiB) — SceLibc, the GLES2 driver's host heap\n",
		 sceLibcHeapSize, sceLibcHeapSize / MIB);
	boot_log_line(path, line);
	snprintf(line, sizeof(line),
		 "vita_glue: _newlib_heap_size_user = %u bytes (%u MiB) — newlib, the engine/MRI/CPU-bitmap heap\n",
		 _newlib_heap_size_user, _newlib_heap_size_user / MIB);
	boot_log_line(path, line);
	snprintf(line, sizeof(line),
		 "vita_glue: sceUserMainThreadStackSize = %u bytes\n",
		 sceUserMainThreadStackSize);
	boot_log_line(path, line);

	/* Append-only: capture mkxp-z Debug() -> std::cerr and later printf. */
	if (!freopen(path, "a", stdout))
		return VITA_GLUE_ERR_LOG;
	setvbuf(stdout, NULL, _IONBF, 0);
	if (!freopen(path, "a", stderr))
		return VITA_GLUE_ERR_LOG;
	setvbuf(stderr, NULL, _IONBF, 0);
	if (log_sync_enabled)
		log_sync_attach_stdio();

	texture_trace_enabled = file_exists(VITA_GLUE_TEXTURE_TRACE_MARKER);
	gpu_telemetry_enabled = file_exists(VITA_GLUE_GPU_TELEMETRY_MARKER);
	vita_glue_frame_profile_interval = frame_profile_marker();
	measure_init();
	memory_ledger_init();
	gpu_seal_abort_enabled = file_exists(VITA_GLUE_GPU_SEAL_ABORT_MARKER);
	audio_telemetry_enabled = file_exists(VITA_GLUE_AUDIO_TELEMETRY_MARKER);
	gc_memory_enabled = file_exists(VITA_GLUE_GC_MEMORY_MARKER);
	system_event_trace_enabled = file_exists(VITA_GLUE_SYSTEM_EVENT_TRACE_MARKER);
	gc_memory_logged = 0;
	gc_memory_count = gc_memory_logged_at = 0;
	gc_memory_last_us = 0;
	texture_trace_count = texture_trace_scope_depth = 0;
	fbo_failure_probe_started = 0;
	if (log_sync_enabled)
		vita_glue_trace(log_async_accepting
		               ? "vita-log-sync: enabled; telemetry queued; writer batch sync; critical records drain + sync"
		               : "vita-log-sync: writer unavailable; telemetry dropped; critical records sync");
	if (texture_trace_enabled)
		vita_glue_trace("vita-texture: enabled; process clock in seconds; limit=2048");
	if (gpu_telemetry_enabled)
		vita_glue_trace("vita-gpu: telemetry enabled; per-scene counters + frame instrumentation");
	if (vita_glue_frame_profile_interval)
		glue_logf("vita-frame: enabled; interval=%u min_period_ms=1000 gc=reserved", vita_glue_frame_profile_interval);
	if (gpu_seal_abort_enabled)
		vita_glue_trace("vita-gpu: seal violations will ABORT (gpu-seal-abort.enabled)");
	if (audio_telemetry_enabled)
		vita_glue_trace("vita-audio: telemetry enabled; ALSOFT_LOGLEVEL=3 before "
		                "alcOpenDevice, so OpenAL's own AL lib: lines join this log");
	if (gc_memory_enabled)
		glue_logf("vita-gc: free memory on GC enabled; at most one line per %u ms",
		          VITA_GLUE_GC_MEMORY_PERIOD_MS);
	if (system_event_trace_enabled)
		vita_glue_trace("vita-lifecycle: system-event trace enabled; every "
		                "non-empty AppMgr answer logs its id");
	/* One stat when the marker is absent; a poll thread and a boot breadcrumb
	 * only when the diagnostic is opted in. */
	vita_glue_log_abort_probe_arm();

	glue_logf("vita_glue: stdout/stderr attached (append) → %s", path);
	glue_logf("vita_glue: frame diagnostics %s",
	          VITA_GLUE_FRAME_TRACE ? "enabled (debug)" : "disabled");
	return VITA_GLUE_OK;
}

/*
 * Two lines, and they are the whole point of this function: every
 * CPU timing here would otherwise be dated from a 444 MHz core that no log has
 * ever confirmed, because the old implementation reported through printf.
 * Contract and formats in vita_glue.h.
 */
int vita_glue_set_boot_clocks(void)
{
	static const char *const kDomain[4] = { "cpu", "bus", "gpu", "xbar" };
	const int request[4] = {
		VITA_GLUE_CPU_MHZ, VITA_GLUE_BUS_MHZ,
		VITA_GLUE_GPU_MHZ, VITA_GLUE_XBAR_MHZ
	};
	int rc[4], applied[4];
	int i, fail = 0;

	glue_logf("vita_glue: clocks requested cpu=%d bus=%d gpu=%d xbar=%d MHz",
	          request[0], request[1], request[2], request[3]);

	rc[0] = scePowerSetArmClockFrequency(request[0]);
	rc[1] = scePowerSetBusClockFrequency(request[1]);
	rc[2] = scePowerSetGpuClockFrequency(request[2]);
	rc[3] = scePowerSetGpuXbarClockFrequency(request[3]);

	/* Read back only once every domain has been asked: the domains are not
	 * independent (the OS derives limits from the bus clock), so a Get*
	 * taken between two Set* calls can report a value that no longer holds
	 * by the time the boot finishes. */
	applied[0] = scePowerGetArmClockFrequency();
	applied[1] = scePowerGetBusClockFrequency();
	applied[2] = scePowerGetGpuClockFrequency();
	applied[3] = scePowerGetGpuXbarClockFrequency();

	for (i = 0; i < 4; i++)
		applied_clocks[i] = applied[i];

	glue_logf("vita_glue: clocks applied cpu=%d bus=%d gpu=%d xbar=%d MHz "
	          "rc=%08x/%08x/%08x/%08x",
	          applied[0], applied[1], applied[2], applied[3],
	          (unsigned)rc[0], (unsigned)rc[1], (unsigned)rc[2],
	          (unsigned)rc[3]);

	for (i = 0; i < 4; i++) {
		if (rc[i] < 0)
			fail = 1;
		/* A clamp is not a failure — it is the number every timing in
		 * this repo has been assuming, so it gets said out loud. */
		if (applied[i] < request[i])
			glue_logf("vita_glue: WARNING clock below request: %s applied=%d",
			          kDomain[i], applied[i]);
	}

	return fail ? VITA_GLUE_ERR_CLOCKS : VITA_GLUE_OK;
}

void vita_glue_get_applied_clocks(int out[4])
{
	int i;

	if (!out)
		return;
	for (i = 0; i < 4; i++)
		out[i] = applied_clocks[i];
}

/*
 * Resume poll (contract in vita_glue.h).
 *
 * sceAppMgrReceiveSystemEvent fills event and returns 0 when a system event
 * was taken, and a negative SceAppMgr error when the queue is empty, so every
 * answer that is not an ON_RESUME event collapses to 0 and the caller keeps
 * its one-call-per-iteration budget. The call never blocks and allocates
 * nothing: a 64-byte stack struct, one 64-byte line buffer and one userland
 * stub syscall; the line only formats behind the trace marker.
 */
int vita_glue_poll_resume(void)
{
	SceAppMgrSystemEvent event;
	char line[64];

	if (sceAppMgrReceiveSystemEvent(&event) < 0)
		return 0;
	if (system_event_trace_enabled)
	{
		snprintf(line, sizeof(line), "vita-lifecycle: system-event id=0x%08x",
		         (unsigned)event.systemEvent);
		vita_glue_trace(line);
	}
	return event.systemEvent == SCE_APPMGR_SYSTEMEVENT_ON_RESUME;
}

static unsigned glue_resume_epoch;

/* Contract in vita_glue.h: a resume was detected. */
void vita_glue_resume_notify(void)
{
	__atomic_fetch_add(&glue_resume_epoch, 1, __ATOMIC_RELEASE);
	log_sync_lock();
	if (log_sync_fd >= 0 && log_sync_reopen_locked(0) == 0) {
		log_sync_notice_locked();
		(void)sceIoSyncByFd(log_sync_fd, 0);
	}
	log_sync_unlock();
	stdio_resume_recover();
}

unsigned vita_glue_resume_epoch(void)
{
	return __atomic_load_n(&glue_resume_epoch, __ATOMIC_ACQUIRE);
}

/* Contract in vita_glue.h: the kernel-wide clock for the loop gap detector. */
long long vita_glue_kernel_time_ms(void)
{
	return (long long)(sceKernelGetSystemTimeWide() / 1000LL);
}

/* Contract in vita_glue.h: the wall-time clock for the loop gap detector.
 * A failed read leaves the zero the loop already treats as "no sample". */
long long vita_glue_rtc_time_ms(void)
{
	SceRtcTick tick = { 0, };

	if (sceRtcGetCurrentTick(&tick) < 0)
		return 0;
	return (long long)(tick.tick / 1000ULL);
}

/* Contract in vita_glue.h: the clock behind Time.now, which crosses standby. */
long long vita_glue_wall_time_ms(void)
{
	struct timeval tv;

	if (gettimeofday(&tv, NULL) != 0)
		return 0;
	return (long long)tv.tv_sec * 1000LL + (long long)(tv.tv_usec / 1000);
}

int vita_glue_lifecycle_trace_enabled(void)
{
	return system_event_trace_enabled;
}

void vita_glue_log_free_memory(const char *tag)
{
	SceKernelFreeMemorySizeInfo info;
	char line[224];
	int rc;

	memset(&info, 0, sizeof(info));
	info.size = (int)sizeof(info);
	rc = sceKernelGetFreeMemorySize(&info);
	if (rc < 0) {
		snprintf(line, sizeof(line), "%s: sceKernelGetFreeMemorySize rc=0x%08X",
		         tag ? tag : "free-memory", (unsigned)rc);
	} else {
		snprintf(line, sizeof(line), "%s: size_user=%u size_cdram=%u size_phycont=%u bytes",
		         tag ? tag : "free-memory", (unsigned)info.size_user,
		         (unsigned)info.size_cdram, (unsigned)info.size_phycont);
	}
	vita_glue_trace(line);
}

int vita_glue_gc_memory_enabled(void)
{
	return gc_memory_enabled;
}

/*
 * Called from inside Ruby's collector (the runtime patch installs the
 * GC_END_SWEEP tracepoint only when the marker is present). Contract, line
 * format and the reason for the rate limit are in vita_glue.h.
 *
 * Every collection is counted before the rate limit is applied, so the
 * "count=" field stays exact no matter how many lines are suppressed; what
 * a suppressed event costs is one 64-bit clock read and two compares.
 */
void vita_glue_gc_event(void)
{
	SceKernelFreeMemorySizeInfo info;
	unsigned long long now, elapsed;
	unsigned span, since;
	int rc;

	if (!gc_memory_enabled)
		return;
	++gc_memory_count;

	/* The window is compared in microseconds, not milliseconds: a 64-bit
	 * divide by 1000 is a call into __aeabi_uldivmod on this target (no
	 * hardware integer divide), and this runs inside every collection. The
	 * conversion happens on the line that is actually emitted — at most one
	 * a second — so a suppressed event costs one clock read, a subtract and
	 * a compare. */
	now = sceKernelGetProcessTimeWide();
	elapsed = now - gc_memory_last_us;
	if (gc_memory_logged &&
	    elapsed < (unsigned long long)VITA_GLUE_GC_MEMORY_PERIOD_MS * 1000u)
		return;

	/* The first line has no previous window, so it reports 0 ms. */
	span = gc_memory_logged ? (unsigned)(elapsed / 1000u) : 0u;
	since = gc_memory_count - gc_memory_logged_at;
	gc_memory_logged = 1;
	gc_memory_last_us = now;
	gc_memory_logged_at = gc_memory_count;

	memset(&info, 0, sizeof(info));
	info.size = (int)sizeof(info);
	rc = sceKernelGetFreeMemorySize(&info);
	if (rc < 0)
		glue_logf("vita-gc: count=%u +%u window_ms=%u sceKernelGetFreeMemorySize rc=0x%08X",
		          gc_memory_count, since, span, (unsigned)rc);
	else
		glue_logf("vita-gc: count=%u +%u window_ms=%u size_user=%u size_cdram=%u size_phycont=%u",
		          gc_memory_count, since, span, (unsigned)info.size_user,
		          (unsigned)info.size_cdram, (unsigned)info.size_phycont);
}

/* Contract and field meanings in vita_glue.h. `headroom` is the number to
 * watch: sceKernelGetFreeMemorySize cannot see inside this heap, so nothing
 * else in the logs falls when the engine fills it. */
void vita_glue_log_heap(const char *tag)
{
	struct mallinfo info = mallinfo();
	unsigned limit = _newlib_heap_size_user;
	unsigned arena = (unsigned)info.arena;
	char line[224];

	snprintf(line, sizeof(line),
	         "vita-heap: tag=%s arena=%u used=%u free=%u keepcost=%u limit=%u headroom=%u bytes",
	         tag ? tag : "heap", arena, (unsigned)info.uordblks,
	         (unsigned)info.fordblks, (unsigned)info.keepcost, limit,
	         arena < limit ? limit - arena : 0u);
	vita_glue_trace(line);
}

int vita_glue_boot(const char *log_path, const char *module_dir)
{
	int rc;

	(void)module_dir; /* no driver modules to load; the engine's boot call still passes NULL */

	rc = vita_glue_init_log(log_path);
	if (rc != VITA_GLUE_OK)
		return rc; /* nothing to log with; caller sees the code */
	glue_fflush();

	glue_logf("vita_glue: boot (log+trace)");
	glue_fflush();

	/* Clocks: a failure is logged but not fatal — the OS may refuse a
	 * frequency and the applied value is what ships. */
	(void)vita_glue_set_boot_clocks();
	glue_fflush();

	/* The two halves of the same question: what the kernel still has, and
	 * what is left inside the heap the CRT already took from it. */
	vita_glue_log_free_memory("free-memory @ boot");
	vita_glue_log_heap("boot");
	glue_fflush();

	/* Shipped GXP live in the VPK; must run before vglInit (SDL_Init). */
	vglSetShaderCacheFallbackPath("app0:/shader_cache");
	glue_logf("vita_glue: vitaGL backend, shaders from app0:/shader_cache");
	glue_fflush();

	glue_logf("vita_glue: boot complete — safe to call SDL_Init");
	glue_fflush();
	return VITA_GLUE_OK;
}
