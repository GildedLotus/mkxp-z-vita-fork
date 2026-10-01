// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vita_glue.h — Vita platform bootstrap shared by packaging and mkxp-z main.
 *
 * Call vita_glue_boot once, before
 * SDL_Init.
 *
 * Order inside vita_glue_boot:
 *   1. mkdir + rotate the previous runs' logs + redirect stdout/stderr →
 *      ux0:/data/mkxp-z/logs/<log>
 *   2. boot clocks 444/222/222/166; log requested + applied
 *   3. log sceKernelGetFreeMemorySize (user/cdram/phycont) and the newlib
 *      heap's own occupancy (vita_glue_log_heap)
 *   4. point vitaGL at a writable shader cache root per shipped GXP set and
 *      at the shipped cache (app0:/shader_cache).
 *
 * Link notes:
 *   - vita_glue.c defines `sceLibcHeapSize`, `sceUserMainThreadStackSize`
 *     and `_newlib_heap_size_user` (see the heap notes below).
 *     Do not also define them in the consumer.
 *   - Needs -I to the vitaGL include tree and the vitaGL link closure.
 *   - Vita stubs: SceIofilemgr, SceKernelThreadMgr, SceLibKernel,
 *     ScePower, SceProcessmgr, SceSysmem, SceGxm.
 *
 * printf: Vita newlib does not honour %zu — use %u + (unsigned).
 *
 * printf, more generally: nothing in this repo may use it to say anything
 * Not one printf line this file ever emitted reached a
 * log — on hardware or on Vita3K — and the reason is structural, not a
 * flush that was missed:
 *
 *   - VitaSDK's newlib is BUILT with __DYNAMIC_REENT__: every lib_a-*.o of
 *     stdio resolves its stream through __getreent(), which is a real
 *     per-thread lookup (TLS slot 0x89 into a 256-entry reent_list, see
 *     lib_a-threading.o). So printf writes to __getreent()->_stdout.
 *   - The INSTALLED headers do not define __DYNAMIC_REENT__ for
 *     arm-vita-eabi, so in application code `stdout` expands to
 *     _impure_ptr->_stdout — newlib's single static impure_data.
 *   - _init_vita_reent gives the main thread reent_list[0] and builds
 *     _newlib_global_reent as a third, separate struct. impure_data is not
 *     any of them, on any thread.
 *
 * So freopen(path,"a",stdout), setvbuf(stdout,...) and the log-sync hook on
 * stdout->_write all operate on impure_data's FILE, which fputs(msg,stdout)
 * then writes to and printf never touches: printf's stream is the calling
 * thread's own FILE, still on fd 1, which on a Vita app is the SceLibc TTY.
 * (Established from the shipped libc.a and headers, not from a device run.
 * It is consistent with everything observed: the GLES bring-up's own vfprintf to an
 * explicit FILE* reached its log, its printf lines did not, and mkxp-z's
 * Debug() reaches the log because libstdc++'s std::cerr was likewise
 * compiled against these headers.)
 *
 * The fix is not to repair stdout but to stop depending on it: every line
 * the glue formats goes through vita_glue_trace, which writes with
 * fputs(stdout)+fflush or, with VITA_GLUE_LOG_SYNC_MARKER, with
 * sceIoWrite plus the sync policy below. The internal helper glue_logf() is the
 * printf-shaped front door to that sink.
 */

#ifndef VITA_GLUE_H
#define VITA_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- defaults ---------------------------------------------------------- */

/* Expensive per-frame GL/input diagnostics. Meson: -Dvita_frame_trace=true.
 * Default builds omit formatting, GL error/status polling and log flushes
 * from render/input paths. Boot breadcrumbs and normal error logs remain.
 * Standalone probes may opt in with -DVITA_GLUE_FRAME_TRACE=1. */
#ifndef VITA_GLUE_FRAME_TRACE
#define VITA_GLUE_FRAME_TRACE 0
#endif

/* Log directory / default combined stdout+stderr file. */
#define VITA_GLUE_LOG_DIR    "ux0:/data/mkxp-z/logs"
#define VITA_GLUE_LOG_PATH   "ux0:/data/mkxp-z/logs/mkxp-z.log"

/* Generations of previous runs kept beside the live log.
 * vita_glue_init_log renames mkxp-z.log → mkxp-z.1.log → mkxp-z.2.log →
 * mkxp-z.3.log before it truncates; a failed rotation appends instead, so a
 * crash-relaunch preserves the crash log and back-to-back runs stay readable
 * without a USB round trip in between. The live log keeps its name, and
 * the whole logs directory is what a bug report attaches. Values
 * above 9 are not supported (the suffix is a single digit). */
#define VITA_GLUE_LOG_ROTATE_KEEP 3

/* Boot clocks (MHz). Matches GLES bring-up. */
#define VITA_GLUE_CPU_MHZ    444
#define VITA_GLUE_BUS_MHZ    222
#define VITA_GLUE_GPU_MHZ    222
#define VITA_GLUE_XBAR_MHZ   166

/*
 * ---- heaps -------------------------------------------
 *
 * NEWLIB heap: VitaSDK's _init_vita_heap (libc.a, lib_a-sbrk.o) takes ONE
 * SCE_KERNEL_MEMBLOCK_TYPE_USER_RW memblock of _newlib_heap_size_user bytes
 * before main, and _sbrk_r only ever hands out slices of it: the heap never
 * grows and an exhausted one returns ENOMEM forever. This is where the
 * engine lives: operator new, std::vector, SDL_malloc, MRI's ruby_xmalloc
 * and the cpuSurface pixel buffers. The cpu_bitmap_bytes counter in the
 * vita-gpu: telemetry line counts bytes of THIS heap.
 *
 * vitaGL's RAM, CDRAM and phycont pools are separate memblocks sized at
 * vglInit (VitaVglPools below); no SceLibc heap is used (sceLibcHeapSize is 0).
 */

/*
 * Newlib heap: the engine's heap (see above). VitaSDK's default when
 * _newlib_heap_size_user is left undefined is 0x8000000 — the weak symbol's
 * address is tested, not its value, so an absent definition silently selects
 * 128 MiB. Pinned here so the number the engine actually runs on is a repo
 * fact that boot logs, not a toolchain default that a VitaSDK bump can move
 * under us. 160 MiB: the vitaGL RAM pool shrank by 40 MiB
 * (96 -> 56) and this heap takes 32 of it, keeping 8 MiB of margin (device
 * measurement: about 89 MiB of user memory stayed free at 128 MiB). This heap is
 * taken before main, so a size the kernel refuses leaves every malloc failing
 * with no log of why (vita-heap: arena=0 at boot is that failure).
 *
 * Defined in vita_glue.c; consumers must not redefine it.
 */
#define VITA_GLUE_NEWLIB_HEAP_BYTES (160u * 1024u * 1024u)

/* Main-thread stack headroom. Defined in vita_glue.c. */
#define VITA_GLUE_MAIN_STACK_BYTES (1u * 1024u * 1024u)

/* CRT reads these symbols before main. Declared so consumers can log them.
 * _newlib_heap_size_user is newlib's own weak hook, not a VitaSDK header
 * symbol; vita_glue.c supplies the strong definition that satisfies it. */
extern unsigned int sceLibcHeapSize;
extern unsigned int sceUserMainThreadStackSize;
extern unsigned int _newlib_heap_size_user;

/* ---- result codes ------------------------------------------------------ */

#define VITA_GLUE_OK           0
#define VITA_GLUE_ERR_LOG    (-1)  /* could not open/redirect the log file */
#define VITA_GLUE_ERR_CLOCKS (-2)  /* a clock Set* returned < 0 */

/* ---- API --------------------------------------------------------------- */

/*
 * Full boot sequence (steps 1–4 above). Call before SDL_Init.
 *
 *   log_path   NULL → VITA_GLUE_LOG_PATH
 *   module_dir unused (no driver modules to load); pass NULL
 *
 * Returns VITA_GLUE_OK, or a negative VITA_GLUE_ERR_*. Clock Set* failures
 * are logged but do not abort the boot (the OS may clamp; the applied value
 * is what matters).
 */
int vita_glue_boot(const char *log_path, const char *module_dir);

/*
 * One-line breadcrumb. Default: stdout + fflush(stdout).
 * With VITA_GLUE_LOG_SYNC_MARKER, queues known routine telemetry and writes
 * critical breadcrumbs synchronously through sceIoWrite (see below).
 * Safe from any thread after
 * vita_glue_init_log. Use between boot steps so a crash still leaves
 * a "last thing that ran" trail in ux0:/data/mkxp-z/logs/mkxp-z.log.
 */
void vita_glue_trace(const char *msg);

/* Opt-in crash-safe log sink. Create this empty marker before launch; absent
 * keeps the stdio+fflush path. Independent of texture-trace.enabled.
 *
 * The sink serializes on one recursive kernel mutex. It has to be a kernel mutex and not the spin lock
 * sigslot uses: the lock is held across sceIoWrite + sceIoSyncByFd, which
 * block for milliseconds, so a spinning waiter would burn a core. One object
 * for the process is not a kernel-object budget concern — 0026's problem was
 * thousands of them, one per signal.
 *
 * Lines written through the stdout/stderr hook are buffered until their
 * newline, so a Debug() line — which libstdc++ emits as a body write and a
 * separate std::endl write into an _IONBF stream — costs one sceIoWrite and
 * one sceIoSyncByFd instead of two of each. Complete numeric vita-frame/counts,
 * vita-slow, vita-asset/window/counts, vita-buffer-delete and vita-plane records enter a
 * fixed 256-slot ring (128 KiB payload + 1 KiB lengths). Producers only try a
 * memory gate once: full/busy/unavailable queues drop and count the record,
 * never take the I/O mutex or perform I/O. The next emitted record reports
 * drops. One boot-created native worker with a 64 KiB registered stack drains
 * a snapshot of up to 256 records, then syncs that batch on its own thread.
 * Idle polling and failed-sync retries wait 10 ms; no extra lock/event is needed.
 * All other records, including boot, errors, crash probes, shutdown and stdio,
 * drain earlier queued records under the sink mutex before writing and syncing.
 * A critical call can therefore still wait for card I/O. Concurrent telemetry
 * accepted after its drain boundary follows it. The close/atexit path stops and
 * joins the worker, drains the ring and partial stdio line, then syncs/closes.
 * Abrupt native crashes may lose queued/unsynced telemetry; pre-abort critical
 * breadcrumbs still attempt the ordered drain and synchronous write.
 *
 * Exit ordering: the final hand-off logging and this stop
 * perform synchronous card I/O after engine cleanup, so the engine keeps its
 * storage-independent shutdown watchdog armed through both and calls
 * vita_glue_shutdown_log() (bounded) instead of relying on atexit alone. */
#define VITA_GLUE_LOG_SYNC_MARKER "ux0:/data/mkxp-z/log-sync.enabled"

/* Cap for every wait in the exit-path stop. Card syncs of 0.3-6 s have been
 * observed; beyond the cap a stalled writer costs queued telemetry, never
 * the process exit — the engine's watchdog bounds the remaining tail. */
#define VITA_GLUE_LOG_STOP_TIMEOUT_US (5000u * 1000u)

/* Bytes held while a line through the stdio hook is still being assembled.
 * A longer line is emitted in chunks of this size, never dropped. */
#define VITA_GLUE_LOG_SYNC_LINE_MAX 512u
#define VITA_GLUE_LOG_ASYNC_SLOTS 256u
#define VITA_GLUE_LOG_WRITER_STACK (64u * 1024u)
/* Longest log path the sink remembers for reopening (bytes with the NUL). */
#define VITA_GLUE_LOG_PATH_MAX 256u

/* Controlled native-abort probe for the sink above.
 * Diagnostic only: with the marker present, vita_glue_init_log arms one
 * 64 KiB kernel thread that polls the trigger file every POLL_US; when one
 * appears it is removed (a failed remove disarms instead of aborting), one
 * breadcrumb goes through the sink, and abort() follows. Absent marker, the
 * whole feature costs one file stat at boot — inert in product builds. A
 * trigger an earlier run left behind is consumed before arming, so a stale
 * file cannot abort a boot before its diagnostic has logged anything.
 *
 * The probe works in BOTH sink modes on purpose: with log-sync.enabled every
 * numbered line is already written and synced when the trigger is created, so
 * the log collected after the native abort shows whether per-line sync kept
 * the tail; without it the same run measures what the default stdio+fflush
 * path loses. The abort lands on the probe thread, not the RGSS thread — a
 * native crash is asynchronous to log writes, and that is the condition the
 * sink has to survive. */
#define VITA_GLUE_LOG_ABORT_PROBE_MARKER "ux0:/data/mkxp-z/log-abort-probe.enabled"
#define VITA_GLUE_LOG_ABORT_PROBE_TRIGGER "ux0:/data/mkxp-z/log-abort-probe.trigger"
#define VITA_GLUE_LOG_ABORT_PROBE_POLL_US (100u * 1000u)
#define VITA_GLUE_LOG_ABORT_PROBE_STACK (64u * 1024u)
void vita_glue_log_abort_probe_arm(void);

/* Opt-in GPU kernel-object telemetry.
 * Create this marker before launch; remove it to disable on next launch.
 * Everything it turns on is on the normal log and cheap:
 *   - one line at the end of boot and one per scene change, "vita-gpu:
 *     textures=<n> vbos=<n> surfaces=<n> cpu_bitmap_bytes=<n>
 *     deferred_pending=<n> scene=<n>", from counters kept in the TEX/VBO/FBO
 *     gen+del wrappers;
 *   - the headroom canary, "vita-gpu: <where> headroom surfaces=<n>", which
 *     creates throw-away render surfaces until the firmware sync pool refuses
 *     one and is the only userland view of the remaining budget. <where> is
 *     "boot", "scene" (every scene change) or "freeze" (after every freeze
 *     composite);
 *   - "vita-gpu: glerror=0x<hhhh> at=<site> frame=<n> suppressed=<n>", one
 *     drain of glGetError per real frame and at every freeze, transition and
 *     scene change (first 16 occurrences, then every 256th);
 *   - "vita-gpu: BEGIN|END <what> frame=<n>" around the freeze composite and
 *     the transition, and "vita-gpu: upload <w>x<h> bytes=<n> ms=<n>" for
 *     every whole-level texture upload above 1 MiB;
 *   - "vita-gpu: deferred drain why=<why> objects=<n> bytes=<n> frame=<n>"
 *     and "vita-gpu: respec ..." from the deferred-release queue.
 * Read once, in vita_glue_init_log, like the other markers. */
#define VITA_GLUE_GPU_TELEMETRY_MARKER "ux0:/data/mkxp-z/gpu-telemetry.enabled"
int vita_glue_gpu_telemetry_enabled(void);

/* Boot-only gate: empty/invalid contents mean 60 logical frames; valid 1..3600.
 * Reports are additionally limited to one batch per second. No GL resources. */
#define VITA_GLUE_FRAME_PROFILE_MARKER "ux0:/data/mkxp-z/frame-profile.enabled"
#define VITA_FRAME_BUCKETS 12
extern unsigned vita_glue_frame_profile_interval;
/* CPU-raster profile counters, filled by the engine from
 * swraster at each summary. Op order is fixed and mirrors
 * swraster.h's ProfileOp. */
#define VITA_RASTER_OPS 10
typedef struct VitaRasterProfile {
	unsigned long long calls[VITA_RASTER_OPS], pixels[VITA_RASTER_OPS];
	unsigned long long dst_alpha_calls[3], dst_alpha_pixels[3];
} VitaRasterProfile;
typedef struct VitaFrameSummary {
	double us[VITA_FRAME_BUCKETS];
	unsigned count[VITA_FRAME_BUCKETS];
	double wall_us, max_us, p50_us, p95_us;
	unsigned frames, skipped, frozen, late, quads;
	unsigned long long upload_bytes;
	unsigned gpu[6]; /* frame-sampled maxima: textures/VBOs/surfaces/CPU/pending/bytes */
	VitaRasterProfile raster;
} VitaFrameSummary;
double vita_glue_frame_profile_now_us(void);
int vita_glue_frame_profile_thread(void);
void vita_glue_frame_profile_log(const VitaFrameSummary *summary);

/* Test-only, boot-reserved one-shot recorder. 0=absent; 1=F off;
 * 2=existing F; 3=deferred F reports; 4=existing F, plane telemetry off. */
#define VITA_MEASURE_MARKER "ux0:/data/mkxp-z/frame-debt.measure"
#define VITA_MEASURE_OUTPUT_PREFIX "ux0:/data/mkxp-z/frame-debt-"
extern unsigned vita_measure_mode;
void vita_measure_begin(int internal);
void vita_measure_end(unsigned flags);
unsigned vita_measure_enter(unsigned bucket);
void vita_measure_leave(unsigned previous);
void vita_measure_delay(long long before, long long after, long long elapsed,
                        long long target, unsigned long long frequency, unsigned flags);
void vita_measure_reset(void);
unsigned long long vita_measure_plane_begin(void);
void vita_measure_plane_end(unsigned long long start);
int vita_measure_defer(const VitaFrameSummary *summary);


/* Boot-cached opt-in; first transition then at most once per five seconds.
 * Call due() only at Graphics.transition, then log() if it returns true. */
#define VITA_GLUE_MEMORY_LEDGER_MARKER "ux0:/data/mkxp-z/memory-ledger.enabled"
#define VITA_GLUE_MEMORY_LEDGER_PERIOD_US 5000000ull
extern int vita_glue_memory_ledger_enabled;
typedef struct VitaMemoryResources {
	unsigned long long cpu_pixels, texture_bytes, buffer_bytes, pending_bytes;
	unsigned textures, vbos, surfaces, pending;
} VitaMemoryResources;
int vita_glue_memory_ledger_due(void);
void vita_glue_memory_ledger_log(const VitaMemoryResources *resources);

/* vitaGL pools. Compiled only under
 * MKXPZ_VITAGL_BACKEND. The three sizes reach the SDL VITA_VGL backend as
 * hints before SDL_CreateWindow loads vitaGL; a request outside [min, max]
 * or beyond the boot free memory of its kernel pool falls back to the default
 * and is logged. Requests are whole MiB, 0 selecting the default. */
#define VITA_GLUE_VGL_HINT_RAM     "SDL_VITA_VGL_RAM_POOL"
#define VITA_GLUE_VGL_HINT_CDRAM   "SDL_VITA_VGL_CDRAM_POOL"
#define VITA_GLUE_VGL_HINT_PHYCONT "SDL_VITA_VGL_PHYCONT_POOL"
/* Defaults from device measurements (vita/docs/config.md "vitaGL pool
 * sizes"): vglInit fixes 36.6 MiB of RAM, CDRAM peaked at 36.7 MiB. The
 * launcher draws one textured quad, so it asks for less. */
#define VITA_GLUE_VGL_RAM_MIB              56u
#define VITA_GLUE_VGL_CDRAM_MIB            64u
#define VITA_GLUE_VGL_PHYCONT_MIB          4u
#define VITA_GLUE_VGL_LAUNCHER_RAM_MIB     40u
#define VITA_GLUE_VGL_LAUNCHER_CDRAM_MIB   24u
#define VITA_GLUE_VGL_LAUNCHER_PHYCONT_MIB 4u
typedef struct VitaVglPools {
	unsigned ram, cdram, phycont; /* bytes */
} VitaVglPools;
unsigned vita_glue_vgl_pool_bytes(const char *name, int request_mib, unsigned default_mib,
                                  unsigned min_mib, unsigned max_mib,
                                  unsigned long long free_bytes);
void vita_glue_vgl_pools(int ram_mib, int cdram_mib, int phycont_mib, VitaVglPools *out);
/* One "vita-vgl-pool:" line per call when the memory ledger is enabled; it ends
 * with the circular vertex pool watermark (vitagl-0012). */
void vita_glue_vgl_pool_ledger(const char *tag);
/* gl_init split: end=0 just before SDL_CreateWindow (which runs vglInit and
 * allocates the pools), end=1 just after; the second call always logs
 * "vita-boot: vgl_init tag=<tag> window_us=<n>" and the pool totals. */
void vita_glue_vgl_init_timing(const char *tag, int end);

/* Opt-in: make a GPU-budget seal violation abort the process.
 *
 * The seal closes boot; after it, creating a render surface, a shader program
 * or a VAO is a regression: the port reserves them at boot. An assert()
 * would be live, because the player ships WITHOUT -DNDEBUG, and would kill
 * the game for the person playing it.
 *
 * The shipping behaviour is one synced log line per violating site, and
 * the game continues; the exception is a late render surface, which the
 * engine refuses with a Ruby-visible error because it cannot create one
 * after boot. Create this marker to get the old abort back for a bisecting
 * run, where a core dump at the violation is worth more than a live game.
 * Read once, in vita_glue_init_log, like the other markers. */
#define VITA_GLUE_GPU_SEAL_ABORT_MARKER "ux0:/data/mkxp-z/gpu-seal-abort.enabled"
int vita_glue_gpu_seal_abort_enabled(void);

/*
 * Opt-in audio telemetry.
 *
 * Audio is not missing on this player -- the ELF links the whole OpenAL-soft
 * Vita backend and every SDL_sound decoder, alcOpenDevice succeeds and the
 * mixer thread sits in sceAudioOutOutput -- but the log used to say
 * nothing about it beyond "ALC context current", so "the game is silent" had
 * no evidence to work from. Two things close that, and only the second is
 * gated on this marker:
 *
 *  - unconditional, one line per boot (src/main.cpp): the
 *    "vita-audio: alc ..." census of what the device actually negotiated.
 *    It costs a handful of alcGetIntegerv calls at boot and reports, among
 *    the rest, the sceAudioOut grain -- the sample count the Vita backend
 *    rounded up to a multiple of 64 and handed to sceAudioOutOpenPort.
 *
 *  - with this marker present: setenv("ALSOFT_LOGLEVEL", "3", 1) before
 *    alcOpenDevice, which is the last moment OpenAL-soft reads it
 *    (alc_initconfig runs once, on the first call into ALC). OpenAL then
 *    writes its own "AL lib: ..." lines -- the config file it loaded, every
 *    key it found, the backend it chose and the Post-reset format/rate/update
 *    size -- to stderr, which vita_glue_init_log has already pointed at the
 *    player log. That is where the real output format comes from; the census
 *    line cannot report it, because ALC_FORMAT_CHANNELS_SOFT and
 *    ALC_FORMAT_TYPE_SOFT are loopback-only in OpenAL-soft 1.19.1.
 *
 * TRACE is the most verbose level OpenAL has, so treat this as a diagnostic
 * run and not a shipping default: alSourcePlay / alSourceStop / buffer and
 * source creation each log a line. Read once, in vita_glue_init_log, like
 * every other marker.
 *
 * Tuning is deliberately NOT here and is not gated on this marker. The Vita
 * OpenAL build reads the first of app0:/alsoft.conf and
 * ux0:/data/openal/alsoft.conf that it can open, so frequency, period_size,
 * periods and the rest stay changeable on the device without a rebuild.
 * vita/mkxp-z-vpk/alsoft.conf is the packaged one and carries the rest of
 * that story.
 */
#define VITA_GLUE_AUDIO_TELEMETRY_MARKER "ux0:/data/mkxp-z/audio-telemetry.enabled"
int vita_glue_audio_telemetry_enabled(void);

/*
 * Opt-in: free memory on GC.
 *
 * The port's plan asks for this sample "in debug builds". It ships ONE binary
 * and gates every diagnostic on a marker instead, so "debug build" here
 * means "the marker is present". Read once, in vita_glue_init_log, like all
 * the others; absent means the engine never installs the GC hook at all, so
 * the cost of the feature in a normal run is zero — not "a disabled call
 * per collection".
 *
 * vita_glue_gc_event() is called from a Ruby GC tracepoint (
 * RUBY_INTERNAL_EVENT_GC_END_SWEEP). It counts every collection but logs at
 * most one line per VITA_GLUE_GC_MEMORY_PERIOD_MS of process time, always
 * including the first, so a game that collects hundreds of times a second
 * cannot turn the log into the thing that is slowing it down:
 *
 *   vita-gc: count=<total> +<since the last line> window_ms=<ms>
 *            size_user=<n> size_cdram=<n> size_phycont=<n>
 *
 * count and + are what make the rate readable: the line is a sample, but no
 * collection is missing from the counter. This is elapsed time between samples, NOT time spent in GC.
 * The first line reports "window_ms=0" —
 * its window starts at that first collection, there is nothing before it.
 *
 * The caller is inside the collector. Everything here therefore stays on the
 * stack (one snprintf buffer), takes no Ruby lock and creates no Ruby object;
 * the query itself is a kernel call and the write is the same sink every
 * other glue line uses.
 */
#define VITA_GLUE_GC_MEMORY_MARKER "ux0:/data/mkxp-z/gc-memory.enabled"
#define VITA_GLUE_GC_MEMORY_PERIOD_MS 1000u
int vita_glue_gc_memory_enabled(void);
void vita_glue_gc_event(void);

/* Opt-in lifecycle diagnostics, independent of per-frame diagnostics.
 * Create this marker before launch; remove it to disable on next launch.
 * All resource calls and scope changes belong to the RGSS/GL thread.
 * No GL queries or driver synchronization are performed. */
#define VITA_GLUE_TEXTURE_TRACE_MARKER "ux0:/data/mkxp-z/texture-trace.enabled"
#define VITA_GLUE_TEXTURE_TRACE_LIMIT 2048u
void vita_glue_texture_trace(const char *stage, unsigned texture,
                             unsigned framebuffer, int width, int height);
void vita_glue_texture_scope_enter(void);
void vita_glue_texture_scope_leave(void);
int vita_glue_texture_scope_active(void);

/* Once per launch, only after numeric Bitmap's existing clear failed and
 * texture tracing is enabled. Call begin before read-only GL state queries,
 * then probe for GXM sizing. No GPU work is submitted. A separately staged
 * marker requests an intentional diagnostic core before cleanup; abort is
 * allowed only after the marker is successfully removed. */
#define VITA_GLUE_FBO_FAILURE_DUMP_MARKER "ux0:/data/mkxp-z/fbo-failure-dump.enabled"

/* System.vita_kernel_object_headroom briefly takes every kernel semaphore it
 * can (up to 512), so the engine registers it only when this marker exists at
 * launch. Diagnostics test respond_to? first. */
#define VITA_GLUE_KERNEL_PROBE_MARKER "ux0:/data/mkxp-z/kernel-object-probe.enabled"
int vita_glue_fbo_failure_probe_begin(void);
void vita_glue_fbo_failure_probe(int width, int height);

/*
 * Step 1 alone: mkdir parents, rotate the previous runs' logs one generation
 * up (VITA_GLUE_LOG_ROTATE_KEEP), then open log_path and attach stdout/stderr
 * in unbuffered append mode. Safe to call more than once (later call wins),
 * but every call rotates, so call it once per process. Rotation failures
 * preserve the current log by appending, are reported, and never fail boot.
 * First line of each run's banner is the running title id, so the product player
 * and a diagnostic player sharing this path can be told apart.
 * Returns VITA_GLUE_OK or VITA_GLUE_ERR_LOG.
 */
int vita_glue_init_log(const char *log_path);

/*
 * Exit-path stop for the sink above: joins the writer,
 * drains the ring and the partial stdio line, then syncs and closes, with
 * the join and the relock capped at VITA_GLUE_LOG_STOP_TIMEOUT_US. On a cap
 * the writer and its descriptor are left exactly as they stalled, for the
 * shutdown watchdog that must still be armed around this call. Idempotent
 * and safe when the sink never started. Returns 0 when fully stopped, -1
 * when a cap was hit.
 */
int vita_glue_shutdown_log(void);

/*
 * Step 2 alone: request 444/222/222/166, then report what the OS applied.
 * Two lines, in this order, through vita_glue_trace — never printf, which
 * is why the question "is 444 MHz actually applied" can only be
 * answered from these lines:
 *
 *   vita_glue: clocks requested cpu=444 bus=222 gpu=222 xbar=166 MHz
 *   vita_glue: clocks applied cpu=<n> bus=<n> gpu=<n> xbar=<n> MHz
 *              rc=<8>/<8>/<8>/<8>
 *
 * followed by one line per domain the OS clamped:
 *
 *   vita_glue: WARNING clock below request: <domain> applied=<n>
 *
 * All four Set* calls run before any Get*, so the applied line is the state
 * the process actually runs at rather than four snapshots taken while the
 * remaining domains were still moving.
 *
 * Returns VITA_GLUE_OK, or VITA_GLUE_ERR_CLOCKS if any Set* returned < 0.
 * A clamp is not an error: the OS is allowed to refuse a frequency and the
 * applied value is what ships.
 */
int vita_glue_set_boot_clocks(void);

/*
 * The four values of the last "clocks applied" line — cpu, bus, gpu, xbar
 * in MHz, in that order — for callers that want to show them rather than
 * grep a log (the on-screen overlay). All zero until
 * vita_glue_set_boot_clocks has run. A NULL argument is ignored.
 */
void vita_glue_get_applied_clocks(int out[4]);

/*
 * Suspend has no notification on this platform:
 * the Vita SDL port never posts SDL_APP_* (its only senders are the Android,
 * WinRT and GDK backends) and SCE_POWER_CB_APP_SUSPEND is outside
 * SCE_POWER_CB_VALID_MASK_NON_SYSTEM, so a non-system title cannot learn
 * before sleep. The player therefore repairs after the fact: EventThread's
 * loop calls this once per iteration and, on 1, drives the SDL foreground
 * entry points mkxp-z already handles. Polling needs no kernel object —
 * preferred over scePowerRegisterCallback for exactly that.
 *
 * One non-blocking sceAppMgrReceiveSystemEvent call; returns 1 only when it
 * reports SCE_APPMGR_SYSTEMEVENT_ON_RESUME. Any other answer — no event,
 * another system event, an error — returns 0. Event/main thread only.
 *
 * Diagnosis: the first real suspend/resume survived without
 * this poll ever returning 1, so with the marker below present at launch every
 * non-empty receive answer logs its id once, through the shared sink:
 *
 *   vita-lifecycle: system-event id=0x10000003
 *
 * Absent marker, the poll costs nothing beyond the receive itself. The gap
 * detector in EventThread is the repair path that does not
 * depend on the event arriving at all.
 */
#define VITA_GLUE_SYSTEM_EVENT_TRACE_MARKER "ux0:/data/mkxp-z/system-event-trace.enabled"
int vita_glue_poll_resume(void);

/*
 * The engine's gap detector calls this once per detected resume, before it
 * logs anything, on the event thread. On the device,
 * after a 185-230 s standby the log gained no further line and BGM stopped,
 * while a fresh open worked, so descriptors held across the suspend are the
 * suspect. This (1) bumps vita_glue_resume_epoch(), which lets readers that
 * hold a file open re-open it before their next read, and (2) reopens the
 * log sink's descriptor, then writes 'vita-log-sync: reopened after resume
 * rc=0x00000000 ...'. The sink also reopens by itself on any failed write
 * ('... after write failure rc=0x...'), at most once per write, so a resume
 * the detector misses is covered too. Only with VITA_GLUE_LOG_SYNC_MARKER.
 * In both modes it also swaps fresh append handles on the log into fd 1, fd 2
 * and the freopen'd stdout/stderr descriptors (Ruby's STDOUT/STDERR are fd
 * 1/2, stale after standby) and logs 'vita-stdio: reopened
 * after resume ...'. With -Wl,--wrap=_write_r a failed write to
 * one of those descriptors is repaired and retried once, then reported as
 * written, so it never raises into Ruby.
 */
void vita_glue_resume_notify(void);

/* Count of resumes announced through vita_glue_resume_notify; any thread. */
unsigned vita_glue_resume_epoch(void);

/*
 * Kernel-wide monotonic time in milliseconds: sceKernelGetSystemTimeWide/1000.
 * One of the gap detector's two clocks (the gap detector samples both): process
 * time — SDL_GetTicks64's source on Vita (SDL:src/timer/vita/SDL_systimer.c)
 * — may exclude suspended time, which would hide exactly the gap the detector
 * exists to catch; whether the kernel-wide tick advances across a real
 * suspend is unknown (the first real sleep produced no gap
 * line at all). One userland stub syscall, no allocation, no kernel object.
 * Event thread.
 */
long long vita_glue_kernel_time_ms(void);

/*
 * Wall time in milliseconds: sceRtcGetCurrentTick, divided by the RTC tick
 * rate (1 MHz, so /1000 to ms; a constant divide). The gap detector's other
 * clock: the RTC is wall time and is expected to advance
 * through standby, which the kernel-wide tick above may not. The loop's gap
 * line pairs the two readings, 'vita-lifecycle: gap rtc_ms= kernel_ms=', so
 * one device sleep answers which clock moved. Returns 0 when the read
 * fails: a zero sample means "no clock" to the loop, so a dead RTC only
 * disables the RTC half of the gap detection. One userland stub syscall
 * (link -lSceRtc_stub), no allocation, no kernel object. Event thread.
 */
long long vita_glue_rtc_time_ms(void);

/*
 * Wall time in milliseconds from gettimeofday (newlib -> sceKernelLibcGettimeofday),
 * the clock behind Ruby's Time.now. Device evidence (185 s standby): Time.now +275.1 s while CLOCK_MONOTONIC (process time, the
 * same source as SDL_GetTicks64) moved +90.5 s, so this is the clock known to
 * cross a sleep. The vitaGL gap detector samples it beside the
 * two above. Returns 0 when the read fails (the loop treats 0 as "no clock").
 * Event thread.
 */
long long vita_glue_wall_time_ms(void);

/*
 * 1 when VITA_GLUE_SYSTEM_EVENT_TRACE_MARKER was present at launch. Gates the
 * event loop's 5 s 'vita-lifecycle: beat' diagnostic. Read-only
 * flag, set once by vita_glue_init_log; any thread.
 */
int vita_glue_lifecycle_trace_enabled(void);


/*
 * Step 3 alone: print a labeled sceKernelGetFreeMemorySize block
 * (size_user / size_cdram / size_phycont). Never fails; the rc is logged.
 * Call again later (e.g. around GC) for a second sample.
 */
void vita_glue_log_free_memory(const char *tag);

/*
 * Occupancy of the NEWLIB heap — heap 1 in the free-memory report above, the one the
 * engine, MRI and the CPU bitmaps share. sceKernelGetFreeMemorySize cannot
 * see inside it: that heap is one memblock the CRT already took, so
 * size_user stays flat while the engine fills it and an allocation failure
 * is invisible until something crashes. mallinfo() is the only view.
 *
 *   vita-heap: tag=<tag> arena=<n> used=<n> free=<n> keepcost=<n>
 *              limit=<n> headroom=<n> bytes
 *
 *   arena     bytes sbrk has handed to malloc so far (mallinfo.arena)
 *   used      allocated (uordblks)   free  free inside arena (fordblks)
 *   keepcost  top-most releasable chunk (keepcost)
 *   limit     VITA_GLUE_NEWLIB_HEAP_BYTES — the whole heap
 *   headroom  limit - arena: what sbrk can still grow by. THIS is the
 *             number that reaches zero before an out-of-memory crash;
 *             `free` only describes fragmentation inside what is held.
 *
 * arena = 0 in the boot sample means the CRT's memblock allocation failed
 * and every malloc in the process is already returning NULL.
 *
 * No allocation, no GL, no kernel call; safe from any thread (newlib takes
 * its malloc lock). Not free, though: newlib rebuilds mallinfo by walking
 * every free chunk in every bin, so this belongs at boot and at scene
 * changes, never in a frame path.
 */
void vita_glue_log_heap(const char *tag);

#ifdef __cplusplus
}
#endif

#endif /* VITA_GLUE_H */
