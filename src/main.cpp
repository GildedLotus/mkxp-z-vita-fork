/*
** main.cpp
**
** This file is part of mkxp.
**
** Copyright (C) 2013 - 2021 Amaryllis Kulla <ancurio@mapleshrine.eu>
**
** mkxp is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 2 of the License, or
** (at your option) any later version.
**
** mkxp is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with mkxp.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "bootprofile.h"

#ifndef MKXPZ_BUILD_XCODE
#include "icon.png.xxd"
#endif

#include <alc.h>
#include <alext.h>

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_sound.h>
#include <SDL_ttf.h>

#include <assert.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <regex>

#include "binding.h"
#include "sharedstate.h"
#include "eventthread.h"
#include "util/debugwriter.h"
#include "util/exception.h"
#include "display/gl/gl-debug.h"
#include "display/gl/gl-fun.h"

#include "filesystem/filesystem.h"

#include "system/system.h"

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "vita_glue.h"
/* setenv, for ALSOFT_LOGLEVEL below. <stdlib.h> and not <cstdlib>: setenv is
 * POSIX, and newlib only declares it from the header's __POSIX_VISIBLE arm. */
#include <stdlib.h>
#endif
#include <cstdio>

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#if !defined(__vita__)
#include "hf_s.h"
#else
/* Native SDK declarations and boot facade. The host uses explicit
 * lifecycle callbacks; no registered stacks or device exits are modelled.
 */
#include "vita_fatal.h"
/* std::exception, for the boundary around mkxp_main() at the bottom of this
 * file: an exception that leaves main() is std::terminate, and on this device
 * that is a process kill with nothing in the log. */
#include <exception>
#include <atomic>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#endif
#if defined(__vita__) && defined(MKXPZ_VITA_LAUNCHER)
/* One eboot, three modes. Configure copies vita/launcher sources into
 * this directory, so the header resolves through the src/ include path.
 * existing src/ include directory. */
#include "vita_boot.h"
#else
/*
 * Built without the launcher. This eboot is then always the pinned player, so
 * the boot wrapper collapses to its identities and main() below is the same
 * code in both configurations — one shutdown path to read, one to test.
 */
#define VITA_BOOT_RC_WEDGED 70
static inline int vita_boot_decide(int, char **) { return 0; }
static inline int vita_boot_is_launcher() { return 0; }
static inline const char *vita_boot_log_path() { return NULL; }
static inline void vita_boot_log_decision() {}
static inline int vita_launcher_main() { return 0; }
static inline void vita_boot_clear_running() {}
static inline void vita_boot_report_error(const char *title, const char *msg) {
  vitaLogMessage("vita-boot: fatal: ", msg);
  vitaWriteLastError(VITA_FATAL_KIND_INIT, title, msg);
}
static inline int vita_boot_finish(int rc) { return rc; }
#endif
#endif

#if defined(__WIN32__)
#include "resource.h"
#include <Winsock2.h>
#include "util/win-consoleutils.h"

// Try to work around buggy GL drivers that tend to be in Optimus laptops
// by forcing MKXP to use the dedicated card instead of the integrated one
#include <windows.h>
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

#ifdef MKXPZ_STEAM
#include "steamshim_child.h"
#endif

#ifdef MKXPZ_BUILD_XCODE
#include <Availability.h>
#include "TouchBar.h"
#if !defined(__MAC_10_15) || __MAC_OS_X_VERSION_MAX_ALLOWED < __MAC_10_15
#define MKXPZ_INIT_GL_LATER
#endif
#endif

#ifndef MKXPZ_INIT_GL_LATER
#define GLINIT_SHOWERROR(s) showInitError(s)
#else
#define GLINIT_SHOWERROR(s) rgssThreadError(threadData, s)
#endif

static void rgssThreadError(RGSSThreadData *rtData, const std::string &msg);
static void showInitError(const std::string &msg);

static inline const char *glGetStringInt(GLenum name) {
  return (const char *)gl.GetString(name);
}

static void printGLInfo() {
    const std::string renderer(glGetStringInt(GL_RENDERER));
    const std::string version(glGetStringInt(GL_VERSION));
    std::regex rgx("ANGLE \\((.+), ANGLE Metal Renderer: (.+), Version (.+)\\)");
        
    std::smatch matches;
    if (std::regex_search(renderer, matches, rgx)) {
        
        Debug() << "Backend           :" << "Metal";
        Debug() << "Metal Device      :" << matches[2] << "(" + matches[1].str() + ")";
        Debug() << "Renderer Version  :" << matches[3].str();
        
    std::smatch vmatches;
        if (std::regex_search(version, vmatches, std::regex("\\(ANGLE (.+) git hash: .+\\)"))) {
            Debug() << "ANGLE Version     :" << vmatches[1].str();
        }
        return;
    }
    
  Debug() << "Backend      :" << "OpenGL";
  Debug() << "GL Vendor    :" << glGetStringInt(GL_VENDOR);
  Debug() << "GL Renderer  :" << renderer;
  Debug() << "GL Version   :" << version;
  Debug() << "GLSL Version :" << glGetStringInt(GL_SHADING_LANGUAGE_VERSION);
}

static SDL_GLContext initGL(SDL_Window *win, Config &conf,
                            RGSSThreadData *threadData);

#ifdef __vita__
/*
 * The priority the OpenAL mixer thread is created at.
 *
 * VitaSDK's OpenAL backend reads this symbol in
 * ALCvitaPlayback_ALCbackend_start, where it is a WEAK REFERENCE the backend
 * tests by address before dereferencing -- the same
 * define-it-here-and-the-library-picks-it-up idiom vita/glue/vita_glue.c uses
 * for sceLibcHeapSize and _newlib_heap_size_user. Left undefined the address
 * is 0, and the backend then calls sceKernelGetThreadInfo on whoever opened
 * the device and takes that thread's priority MINUS ONE. On this player that
 * caller is the rgss thread, so stock behaviour puts the mixer exactly one
 * step above the Ruby interpreter -- not a policy, an accident of who
 * happened to call alcCreateContext.
 *
 * One step is not enough. The mixer is woken by sceAudioOutOutput's own
 * blocking cadence and has to be runnable the moment a grain is due; a
 * script-heavy frame that keeps Ruby runnable for tens of milliseconds is
 * exactly the case this has to survive, and at rgss-minus-1 the scheduler is
 * free to keep running Ruby.
 *
 * 112 is SDL's VITA_THREAD_PRIORITY_HIGH (SDL's src/thread/vita/
 * SDL_systhread.c), and it is the same number the stream refill threads and
 * the ME watch thread ask for from inside themselves via
 * SDL_SetThreadPriority (src/audio/alstream.cpp, src/audio/audio.cpp). The
 * decoder that fills the queue has to outrank Ruby for the same reason the
 * mixer does, so the whole audio path sits on one rung and the engine's
 * threads keep their relative order among themselves. 64 -- SDL's
 * TIME_CRITICAL and the top of the user range -- is deliberately left to the
 * system.
 *
 * The backend clamps whatever it reads: 1..63 is raised to 64, 191 and above
 * is lowered to 191, and 0 keeps sceKernelCreateThread's "priority of the
 * calling thread" meaning. 64..190 passes through untouched, so 112 arrives
 * as 112.
 *
 * _oal_thread_affinity is deliberately NOT defined. Undefined it stays 0,
 * which is "any CPU", and that is the right default until a measurement says
 * a particular core is better.
 *
 * The braces are not decoration: `extern "C" int x = 112;` reads to gcc as a
 * declaration marked extern that also has an initializer, and it says so
 * (-Wextern-initializer). Inside a linkage block the same definition is a
 * plain definition that happens to carry C linkage, which is what is meant.
 */
extern "C" {
int _oal_thread_priority = 112;
}
#endif
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
/* One line, every boot: what the ALC device actually negotiated. Unconditional, with no marker in front of it, because this is
 * the line that turns "the game is silent" from a guess into a report. It
 * costs a handful of ALC queries once, on the boot path, and nothing
 * afterwards.
 *
 * period_approx is the sceAudioOut grain, and it is approximate on purpose:
 * ALC_REFRESH is Frequency / UpdateSize computed with integer division, so
 * inverting it does not come back exactly. At 44100 Hz with a 1024 sample
 * grain refresh reads 43 and 44100 / 43 is 1025. Read it knowing the Vita
 * backend rounds the grain UP to a multiple of 64 before handing it to
 * sceAudioOutOpenPort -- 1025 means 1024. The exact number is in OpenAL's own
 * "Post-reset: ..." line, which ux0:/data/mkxp-z/audio-telemetry.enabled
 * turns on.
 *
 * fmt is walked out of ALC_ALL_ATTRIBUTES rather than asked for directly, for
 * two reasons. ALC_FORMAT_CHANNELS_SOFT and ALC_FORMAT_TYPE_SOFT are the
 * loopback arm of OpenAL-soft 1.19.1's attribute list, so on a playback
 * device they are simply absent and this field reads n/a; and querying either
 * one singly on a playback device raises ALC_INVALID_DEVICE, which
 * alsoft.conf's trap-alc-error would turn into a trap. A census must not be
 * able to stop the program it is describing. The walk costs the same and
 * prints them the day a backend does report them.
 *
 * The two extensions are not decoration. AL_EXT_FLOAT32 is what carries 32
 * bit PCM to the device as AL_FORMAT_*_FLOAT32 (src/audio/al-util.h), and
 * ALC_SOFT_pause_device is what a resume path has to have. If either ever
 * reads 0, the failures downstream of it stop being mysterious.
 */
static void vitaLogAlcCensus(ALCdevice *dev) {
  ALCint freq = 0, refresh = 0, mono = 0, stereo = 0, size = 0;
  ALCint attrs[64];
  char fmt[32] = "n/a";
  char line[320];
  const char *vendor, *version, *name;

  alcGetIntegerv(dev, ALC_FREQUENCY, 1, &freq);
  alcGetIntegerv(dev, ALC_REFRESH, 1, &refresh);
  alcGetIntegerv(dev, ALC_MONO_SOURCES, 1, &mono);
  alcGetIntegerv(dev, ALC_STEREO_SOURCES, 1, &stereo);

  /* Only ask for the whole list when it fits: OpenAL writes every attribute
   * it has, so a device reporting more than this buffer holds is skipped
   * rather than overrun. */
  alcGetIntegerv(dev, ALC_ATTRIBUTES_SIZE, 1, &size);
  if (size > 0 && size <= (ALCint)(sizeof(attrs) / sizeof(attrs[0]))) {
    ALCint chans = 0, type = 0, i;

    memset(attrs, 0, sizeof(attrs));
    alcGetIntegerv(dev, ALC_ALL_ATTRIBUTES, size, attrs);
    for (i = 0; i + 1 < size && attrs[i] != 0; i += 2) {
      if (attrs[i] == ALC_FORMAT_CHANNELS_SOFT)
        chans = attrs[i + 1];
      else if (attrs[i] == ALC_FORMAT_TYPE_SOFT)
        type = attrs[i + 1];
    }
    if (chans || type)
      snprintf(fmt, sizeof(fmt), "0x%04x/0x%04x", (unsigned)chans,
               (unsigned)type);
  }

  vendor = alGetString(AL_VENDOR);
  version = alGetString(AL_VERSION);
  name = alcGetString(dev, ALC_DEVICE_SPECIFIER);

  /* mixer_prio is read out of the variable rather than printed as a literal:
   * the value is only worth reporting if the definition above is the one the
   * backend actually linked against, and reading it is the only way this line
   * can say so. It is the priority REQUESTED -- the backend's clamp is
   * documented above and does not move 112 -- and it is in front of the
   * quoted strings so a long vendor/device name cannot truncate it away. */
  snprintf(line, sizeof(line),
           "vita-audio: alc freq=%d refresh=%d period_approx=%d mono=%d "
           "stereo=%d fmt=%s ext=float32:%d,pause:%d mixer_prio=%d "
           "vendor=\"%s\" version=\"%s\" device=\"%s\"",
           (int)freq, (int)refresh, refresh > 0 ? (int)(freq / refresh) : -1,
           (int)mono, (int)stereo, fmt,
           alIsExtensionPresent("AL_EXT_FLOAT32") ? 1 : 0,
           alcIsExtensionPresent(dev, "ALC_SOFT_pause_device") ? 1 : 0,
           _oal_thread_priority,
           vendor ? vendor : "?", version ? version : "?", name ? name : "?");
  vita_glue_trace(line);
}
#endif

#ifdef __vita__
// Kept alive through SDL_Quit and mkxp_main's local destructors.
enum VitaShutdownPhase {
  VitaShutdownIdle, VitaShutdownRuby, VitaShutdownNative, VitaShutdownContext,
  VitaShutdownWorkerDone, VitaShutdownGL, VitaShutdownEvents,
  VitaShutdownAudio, VitaShutdownWindow, VitaShutdownSound,
  VitaShutdownFonts, VitaShutdownImage, VitaShutdownSDL, VitaShutdownLocals,
  VitaShutdownStopped, VitaShutdownAborted, VitaShutdownRubyAborted,
  VitaShutdownAbandoned
};
static std::atomic<int> vitaShutdownState{VitaShutdownIdle};
static SceUID vitaShutdownThread = -1, vitaShutdownReporter = -1;
static bool vitaCleanupReached = false;
static bool vitaCleanupConfirmed = false;
static bool vitaShutdownOwnsMarker = false;
static_assert(ATOMIC_INT_LOCK_FREE == 2, "Shutdown state must not need a lock");

extern "C" int vita_boot_cleanup_complete(void) {
  return vitaCleanupConfirmed;
}

static void vitaShutdownProgress(VitaShutdownPhase phase) {
  int state = vitaShutdownState.load();
  while (state < VitaShutdownStopped && state < phase &&
         !vitaShutdownState.compare_exchange_weak(state, phase)) {}
}

static void vitaShutdownBegin() {
  vitaShutdownProgress(VitaShutdownNative);
}

static bool vitaShutdownAppend(SceUID fd, const char *text) {
  size_t left = strlen(text);
  while (left) {
    int written = sceIoWrite(fd, text, left);
    if (written <= 0)
      return false;
    text += written;
    left -= written;
  }
  return true;
}

static int vitaShutdownReport(SceSize, void *) {
  int state;
  for (;;) {
    state = vitaShutdownState.load();
    if (state == VitaShutdownStopped)
      return 0;
    if (state >= VitaShutdownAborted)
      break;
    sceKernelDelayThread(10000);
  }
  /* This worker may block on the card. The exit enforcer never joins it on
   * abort; the running marker remains evidence if any operation stalls. */
  SceUID fd = sceIoOpen(VITA_FATAL_DIR "/" VITA_FATAL_NAME,
                       SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
  if (fd >= 0) {
    const char *header = VITA_FATAL_MAGIC "\nkind: stuck\ntitle: mkxp-z\n---\n";
    int length = sceIoLseek32(fd, 0, SCE_SEEK_END);
    bool written = false;
    if (length >= 0 && !(state == VitaShutdownAbandoned && length != 0)) {
      written = length != 0 || vitaShutdownAppend(fd, header);
      written = written && vitaShutdownAppend(fd,
          state == VitaShutdownRubyAborted
              ? "\nphase: ruby\nShutdown watchdog: Ruby shutdown exceeded 60 seconds.\n"
                "The player exited without returning to the launcher.\n"
              : "\nShutdown watchdog: native cleanup stalled or failed.\n"
                "The player exited without returning to the launcher.\n");
    }
    int synced = sceIoSyncByFd(fd, 0);
    int closed = sceIoClose(fd);
    // Retire the Ruby timeout breadcrumb only after its report is complete.
    if (state == VitaShutdownRubyAborted && vitaShutdownOwnsMarker &&
        written && synced >= 0 && closed >= 0)
      vita_boot_clear_running();
  }
  return 0;
}

static int vitaShutdownWatchdog(SceSize, void *) {
  int previous = VitaShutdownIdle;
  SceInt64 began = 0;
  for (;;) {
    int state = vitaShutdownState.load();
    if (state == VitaShutdownStopped)
      return 0;
    SceInt64 now = sceKernelGetSystemTimeWide();
    if (state != previous) {
      previous = state;
      began = now;
    }
    if (state >= VitaShutdownAborted) {
      // One second for best-effort evidence; this thread performs no file I/O.
      if (now - began >= 1000000)
        return sceKernelExitProcess(VITA_BOOT_RC_WEDGED);
    } else if (state != VitaShutdownIdle &&
               now - began >= (state == VitaShutdownRuby ? 60000000 : 10000000)) {
      int aborted = state == VitaShutdownRuby
                        ? VitaShutdownRubyAborted : VitaShutdownAborted;
      vitaShutdownState.compare_exchange_strong(state, aborted);
    }
    sceKernelDelayThread(10000);
  }
}

static SceUID vitaShutdownCreate(const char *name, int (*entry)(SceSize, void *)) {
  SceUID thread = sceKernelCreateThread(name, entry, 0, 64 * 1024, 0, 0, NULL);
  if (thread >= 0) {
    if (sceKernelStartThread(thread, 0, NULL) >= 0)
      return thread;
    if (sceKernelDeleteThread(thread) < 0)
      sceKernelExitProcess(VITA_BOOT_RC_WEDGED);
  }
  return -1;
}

static bool vitaShutdownStart() {
  // Two process-wide native threads, 128 KiB total; no SDL/pthread resources.
  vitaShutdownThread = vitaShutdownCreate("mkxpz-shutdown", vitaShutdownWatchdog);
  if (vitaShutdownThread < 0)
    return false; // No enforcer: do not risk blocking on a diagnostic write.
  vitaShutdownReporter = vitaShutdownCreate("mkxpz-shutdown-report", vitaShutdownReport);
  return vitaShutdownReporter >= 0;
}

static int vitaShutdownAbandon() {
  if (vitaShutdownThread < 0)
    return sceKernelExitProcess(VITA_BOOT_RC_WEDGED);
  vitaShutdownProgress(VitaShutdownAbandoned);
  for (;;)
    sceKernelDelayThread(10000);
}

/* Phase 1 of the disarm: engine cleanup reached and not
 * aborted. Enough for the boot wrapper to clear the breadcrumb and attempt
 * the hand-off, but the enforcer stays armed: the final hand-off logging
 * and the bounded log-writer stop that follow all do synchronous card I/O
 * and must remain under its window. */
static bool vitaShutdownConfirm() {
  if (!vitaCleanupReached || vitaShutdownThread < 0 || vitaShutdownReporter < 0)
    return false;
  int state = vitaShutdownState.load();
  if (state >= VitaShutdownAborted)
    return false;
  vitaCleanupConfirmed = true;
  return true;
}

/* Phase 2, called last: the final logging and the writer stop are done, so
 * the enforcer itself can stand down. */
static bool vitaShutdownComplete() {
  if (!vitaCleanupConfirmed || vitaShutdownThread < 0 || vitaShutdownReporter < 0)
    return false;
  int state = vitaShutdownState.load();
  do {
    if (state >= VitaShutdownAborted)
      return false;
  } while (!vitaShutdownState.compare_exchange_weak(state, VitaShutdownStopped));
  for (SceUID thread : {vitaShutdownReporter, vitaShutdownThread}) {
    SceUInt timeout = 1000000;
    if (sceKernelWaitThreadEnd(thread, NULL, &timeout) < 0 ||
        sceKernelDeleteThread(thread) < 0) {
      // The enforcer is stopping: use the storage-free exit directly.
      sceKernelExitProcess(VITA_BOOT_RC_WEDGED);
      return false;
    }
  }
  vitaShutdownThread = vitaShutdownReporter = -1;
  return true;
}
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
/* SDL's Vita backend discards sceKernelStartThread's return value. An atomic
 * handshake also prevents a delayed worker from entering after cancellation. */
static SDL_atomic_t rgssStartup = {0}; // 0 pending, 1 entered, -1 cancelled

static bool rgssThreadReady(SDL_Thread *thread) {
  if (!thread) {
    vitaShutdownBegin();
    char message[512];
    snprintf(message, sizeof(message), "Could not create RGSS thread: %s",
             SDL_GetError());
    vita_boot_report_error("mkxp-z", message);
    return false;
  }

  for (int i = 0; i < 100; ++i) {
    if (SDL_AtomicGet(&rgssStartup) == 1)
      return true;
    SDL_Delay(10);
  }
  if (!SDL_AtomicCAS(&rgssStartup, 0, -1))
    return true;

  vitaShutdownBegin();
  vita_boot_report_error("mkxp-z", "The RGSS thread could not start.");
  /* Join even an unstarted SDL handle: the Vita backend waits and deletes
   * its native thread. A late entry sees cancellation and returns at once. */
  SDL_WaitThread(thread, 0);
  return false;
}
#endif

int rgssThreadFun(void *userdata) {
  RGSSThreadData *threadData = static_cast<RGSSThreadData *>(userdata);

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  if (!SDL_AtomicCAS(&rgssStartup, 0, 1))
    return 0;
  try {
  vita_glue_trace("trace: rgss thread entered");
#endif

#ifdef MKXPZ_INIT_GL_LATER
  threadData->glContext =
      initGL(threadData->window, threadData->config, threadData);
  BootProfile::end(BootProfile::GLInit);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  {
    char tb[128];
    snprintf(tb, sizeof(tb), "trace: initGL done glContext=%p",
             (void *)threadData->glContext);
    vita_glue_trace(tb);
  }
#endif
  if (!threadData->glContext)
    return 0;
#else
  SDL_GL_MakeCurrent(threadData->window, threadData->glContext);
#endif

  /* Setup AL context */
  static const ALCint attrs[] = {
    /* HRTF is explicitly disabled here because it results in poor-quality audio
     * when enabled (see https://github.com/mkxp-z/mkxp-z/issues/341). By
     * default, it's enabled when OpenAL Soft detects that the user is using
     * headphones for audio drivers that support detecting if the user is using
     * headphones, and disabled regardless of whether or not the user is using
     * headphones if the audio driver does not support this detection. The HRTF
     * is required for positional audio support, so we'll need to find a way
     * around the audio quality issues and inconsistent detection of whether or
     * not the user is using headphones once we have positional audio support. */
    ALC_HRTF_SOFT, ALC_FALSE,
    0
  };
  ALCcontext *alcCtx = alcCreateContext(threadData->alcDev, attrs);

  if (!alcCtx) {
    rgssThreadError(threadData, "Error creating OpenAL context");
    return 0;
  }

  alcMakeContextCurrent(alcCtx);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  vita_glue_trace("trace: ALC context current");
  /* alGetString needs a current context, so the census belongs here and not
   * beside alcOpenDevice. */
  vitaLogAlcCensus(threadData->alcDev);
#endif

#if !(defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC))
  try {
#endif
    SharedState::initInstance(threadData);
#if !(defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC))
  } catch (const Exception &exc) {
    rgssThreadError(threadData, exc.msg);
    alcDestroyContext(alcCtx);
    return 0;
  }
#endif
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  vita_glue_trace("trace: SharedState::initInstance done");
#endif

  /* Start script execution */
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  vita_glue_trace("trace: scriptBinding->execute (ruby_init next)");
#endif
  scriptBinding->execute();

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  vitaShutdownBegin();
#endif
  threadData->ethread->requestTerminate();
  SharedState::finiInstance();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  vitaShutdownProgress(VitaShutdownContext);
#endif
  alcDestroyContext(alcCtx);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  vitaShutdownProgress(VitaShutdownWorkerDone);
#endif

  threadData->rqTermAck.set();
  return 0;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  } catch (const Exception &exc) {
    vitaShutdownBegin();
    vita_boot_report_error("mkxp-z", exc.msg.c_str());
  } catch (const std::exception &exc) {
    vitaShutdownBegin();
    vita_boot_report_error("mkxp-z", exc.what());
  } catch (...) {
    vitaShutdownBegin();
    vita_boot_report_error("mkxp-z", "Unknown exception in the RGSS worker.");
  }
  /* No allocation on the failure path. Partial init/finalisation cannot be
   * safely retried; tell main to join, preserve the report and skip LoadExec. */
  threadData->rqTermAck.set();
  threadData->ethread->requestTerminate();
  return 1;
#endif
}

static void printRgssVersion(int ver) {
  const char *const makers[] = {"", "XP", "VX", "VX Ace"};

  char buf[128];
  snprintf(buf, sizeof(buf), "RGSS version %d (RPG Maker %s)", ver,
           makers[ver]);

  Debug() << buf;
}

static void rgssThreadError(RGSSThreadData *rtData, const std::string &msg) {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  vitaShutdownBegin();
  vita_boot_report_error("mkxp-z", msg.c_str());
#else
  rtData->rgssErrorMsg = msg;
#endif
  rtData->ethread->requestTerminate();
  rtData->rqTermAck.set();
}

static void showInitError(const std::string &msg) {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  vitaShutdownBegin();
#endif
  Debug() << msg;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  /* Every boot failure the player can name arrives here, and on a device this
   * is the only place it can survive the process: no console, no readable
   * stdout, no message box. Log it, then leave it on the card for the launcher
   * to show on the next start. Written BEFORE anything tries to draw, because
   * in this state nothing can. */
  vitaLogMessage("init-error: ", msg.c_str());
  vitaWriteLastError(VITA_FATAL_KIND_INIT, "mkxp-z", msg.c_str());
#else
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "mkxp-z", msg.c_str(), 0);
#endif
}

static void setupWindowIcon(const Config &conf, SDL_Window *win) {
  SDL_RWops *iconSrc;

  if (conf.iconPath.empty())
#ifndef MKXPZ_BUILD_XCODE
    iconSrc = SDL_RWFromConstMem(___assets_icon_png, ___assets_icon_png_len);
#else
    iconSrc = SDL_RWFromFile(mkxp_fs::getPathForAsset("icon", "png").c_str(), "rb");
#endif
  else
    iconSrc = SDL_RWFromFile(conf.iconPath.c_str(), "rb");

  SDL_Surface *iconImg = IMG_Load_RW(iconSrc, SDL_TRUE);

  if (iconImg) {
    SDL_SetWindowIcon(win, iconImg);
    SDL_FreeSurface(iconImg);
  }
}

static int mkxp_main(int argc, char *argv[]) {
#ifdef __vita__
    /* Vita platform bootstrap: log redirect, boot clocks,
     * free-memory sample. Must run before any
     * SDL_Init / EGL call. Non-fatal only for clocks.
     * The log file is chosen by the boot decision main() already took: a
     * launcher boot must not consume the dead game's log generations. */
    BootProfile::Scope bootGlue(BootProfile::Glue);
    if (vita_glue_boot(vita_boot_log_path(), NULL) != 0) {
        return 1;
    }
    /* First thing into that log: which mode this is and why. It also writes
     * the crash breadcrumb in game mode and consumes a previous run's in
     * launcher mode — both need an open log and neither needs anything else. */
    bootGlue.finish();
    vita_boot_log_decision();
    if (!vita_boot_is_launcher() && !vitaShutdownStart())
        return VITA_BOOT_RC_WEDGED;
    vita_glue_trace("vita_glue: vitaGL backend");
#endif

    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");

#ifdef GLES2_HEADER
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#endif

#ifdef __vita__
    /* Rear touch must never drive the mouse, even with an environment hint. */
    SDL_SetHintWithPriority(SDL_HINT_VITA_TOUCH_MOUSE_DEVICE, "0", SDL_HINT_OVERRIDE);
#endif

    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");

    /* initialize SDL first */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) < 0) {
      showInitError(std::string("Error initializing SDL: ") + SDL_GetError());
      return 0;
    }
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    {
        char tb[192];
        snprintf(tb, sizeof(tb),
                 "trace: SDL_Init ok joys=%d isGC0=%d",
                 SDL_NumJoysticks(),
                 SDL_NumJoysticks() > 0 ? (int)SDL_IsGameController(0) : -1);
        vita_glue_trace(tb);
    }
#endif
#ifdef __vita__
    /* Launcher mode ends here; everything below belongs to the game:
     * the ALC device, the RGSS thread and therefore Ruby — belongs to a game,
     * and the launcher must touch none of it: MRI cannot be torn down in
     * process, and every GL object alive at the hand-over is a firmware sync
     * object the game may not get back.
     */
    if (vita_boot_is_launcher())
        return vita_launcher_main();
#endif

    if (!EventThread::allocUserEvents()) {
      showInitError("Error allocating SDL user events");
      return 0;
    }

#ifndef WORKDIR_CURRENT
    char dataDir[512]{};
#if defined(__linux__)
    char *tmp{};
    tmp = getenv("SRCDIR");
    if (tmp) {
      strncpy(dataDir, tmp, sizeof(dataDir));
    }
#endif
    if (!dataDir[0]) {
        strncpy(dataDir, mkxp_fs::getDefaultGameRoot().c_str(), sizeof(dataDir));
    }
    mkxp_fs::setCurrentDirectory(dataDir);
#endif
    
    /* now we load the config */
    Config conf;
    conf.read(argc, argv);

#ifdef MKXPZ_VITAGL_BACKEND
    /* vitaGL is initialised by SDL's VGL backend inside SDL_CreateWindow;
     * its pool hints must be set after the config read and before then. */
    {
      VitaVglPools pools;
      char size[16];
      vita_glue_vgl_pools(conf.vitaglRamPoolMiB, conf.vitaglCdramPoolMiB,
                          conf.vitaglPhycontPoolMiB, &pools);
      snprintf(size, sizeof(size), "%u", pools.ram);
      SDL_SetHint(VITA_GLUE_VGL_HINT_RAM, size);
      snprintf(size, sizeof(size), "%u", pools.cdram);
      SDL_SetHint(VITA_GLUE_VGL_HINT_CDRAM, size);
      snprintf(size, sizeof(size), "%u", pools.phycont);
      SDL_SetHint(VITA_GLUE_VGL_HINT_PHYCONT, size);
    }
#endif

#if defined(__WIN32__)
    // Create a debug console in debug mode
    if (conf.winConsole) {
      if (setupWindowsConsole()) {
        reopenWindowsStreams();
      } else {
        char buf[200];
        snprintf(buf, sizeof(buf), "Error allocating console: %lu",
                GetLastError());
        showInitError(std::string(buf));
      }
    }
#endif

#ifdef MKXPZ_STEAM
    if (!STEAMSHIM_init()) {
      showInitError("Failed to initialize Steamworks. The application cannot "
                    "continue launching.");
      SDL_Quit();
      return 0;
    }
#endif

    if (conf.windowTitle.empty())
      conf.windowTitle = conf.game.title;

    assert(conf.rgssVersion >= 1 && conf.rgssVersion <= 3);
    printRgssVersion(conf.rgssVersion);

    int imgFlags = IMG_INIT_PNG | IMG_INIT_JPG;
    if (IMG_Init(imgFlags) != imgFlags) {
      showInitError(std::string("Error initializing SDL_image: ") +
                    SDL_GetError());
      SDL_Quit();

#ifdef MKXPZ_STEAM
      STEAMSHIM_deinit();
#endif

      return 0;
    }

    if (TTF_Init() < 0) {
      showInitError(std::string("Error initializing SDL_ttf: ") +
                    SDL_GetError());
      IMG_Quit();
      SDL_Quit();

#ifdef MKXPZ_STEAM
      STEAMSHIM_deinit();
#endif

      return 0;
    }

    if (Sound_Init() == 0) {
      showInitError(std::string("Error initializing SDL_sound: ") +
                    Sound_GetError());
      TTF_Quit();
      IMG_Quit();
      SDL_Quit();

#ifdef MKXPZ_STEAM
      STEAMSHIM_deinit();
#endif

      return 0;
    }
#if defined(__WIN32__)
    WSAData wsadata = {0};
    if (WSAStartup(0x101, &wsadata) || wsadata.wVersion != 0x101) {
      char buf[200];
      snprintf(buf, sizeof(buf), "Error initializing winsock: %08X",
               WSAGetLastError());
      showInitError(
          std::string(buf)); // Not an error worth ending the program over
    }
#endif

    SDL_Window *win;
    Uint32 winFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_ALLOW_HIGHDPI;

    if (conf.winResizable)
      winFlags |= SDL_WINDOW_RESIZABLE;
    if (conf.fullscreen)
      winFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    
#ifdef GLES2_HEADER
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    // LoadLibrary properly initializes EGL, it won't work otherwise.
    // Doesn't completely do it though, needs a small patch to SDL
#ifdef MKXPZ_BUILD_XCODE
    SDL_setenv("ANGLE_DEFAULT_PLATFORM", (conf.preferMetalRenderer) ? "metal" : "opengl", true);
    SDL_GL_LoadLibrary("@rpath/libEGL.dylib");
#endif
#endif
    
    BootProfile::begin(BootProfile::GLInit);
    win = SDL_CreateWindow(conf.windowTitle.c_str(), SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED, conf.defScreenW,
                           conf.defScreenH, winFlags);

    if (!win) {
      showInitError(std::string("Error creating window: ") + SDL_GetError());

#ifdef MKXPZ_STEAM
      STEAMSHIM_deinit();
#endif
      return 0;
    }
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vita_glue_trace("trace: SDL_CreateWindow ok (EGL display path)");
#endif
    
#ifdef MKXPZ_BUILD_XCODE
    {
        std::string downloadsPath = "/Users/" + mkxp_sys::getUserName() + "/Downloads";
        
        if (mkxp_fs::getCurrentDirectory().find(downloadsPath) == 0) {
            showInitError(conf.game.title +
                          " cannot run from the Downloads directory.\n\n" +
                          "Please move the application to the Applications folder (or anywhere else) " +
                          "and try again.");
#ifdef MKXPZ_STEAM
            STEAMSHIM_deinit();
#endif
            return 0;
        }
    }
#endif
    
#if defined(MKXPZ_BUILD_XCODE)
#define DEBUG_FSELECT_MSG "Select the folder from which to load game files. This is the folder containing the game's INI."
#define DEBUG_FSELECT_PROMPT "Load Game"
    if (conf.manualFolderSelect) {
        std::string dataDirStr = mkxp_fs::selectPath(win, DEBUG_FSELECT_MSG, DEBUG_FSELECT_PROMPT);
        if (!dataDirStr.empty()) {
            conf.gameFolder = dataDirStr;
            mkxp_fs::setCurrentDirectory(dataDirStr.c_str());
            Debug() << "Current directory set to" << dataDirStr;
            conf.read(argc, argv);
            conf.readGameINI();
        }
    }
#endif

    /* OSX and Windows have their own native ways of
     * dealing with icons; don't interfere with them */
#ifdef __LINUX__
    setupWindowIcon(conf, win);
#else
    (void)setupWindowIcon;
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    /* OpenAL-soft reads ALSOFT_LOGLEVEL and its config file exactly once, in
     * alc_initconfig, and alcOpenDevice below is the first call that reaches
     * it -- so this is the only place raising the level can still work. TRACE
     * is the most verbose level OpenAL has: it names the alsoft.conf it
     * loaded and every key it found in it, the backend it chose, the
     * Post-reset format/rate/update size, and a line per source and buffer
     * created afterwards. That last part is why it is behind a marker.
     * vita_glue_init_log has already pointed stderr at the player log, and
     * OpenAL writes there, so no plumbing is needed for the lines to arrive.
     * Absent marker: nothing here runs and the environment is untouched. */
    if (vita_glue_audio_telemetry_enabled())
      setenv("ALSOFT_LOGLEVEL", "3", 1);
#endif

    ALCdevice *alcDev = alcOpenDevice(0);

    if (!alcDev) {
      showInitError("Could not detect an available audio device.");
      SDL_DestroyWindow(win);
      TTF_Quit();
      IMG_Quit();
      SDL_Quit();

#ifdef MKXPZ_STEAM
      STEAMSHIM_deinit();
#endif
      return 0;
    }
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vita_glue_trace("trace: alcOpenDevice ok");
#endif

    SDL_DisplayMode mode;
    SDL_GetDisplayMode(0, 0, &mode);

    /* Can't sync to display refresh rate if its value is unknown */
    if (!mode.refresh_rate)
      conf.syncToRefreshrate = false;

    EventThread eventThread;

#ifndef MKXPZ_INIT_GL_LATER
    SDL_GLContext glCtx = initGL(win, conf, 0);
    BootProfile::end(BootProfile::GLInit);
#else
    SDL_GLContext glCtx = NULL;
#endif
#ifdef MKXPZ_VITAGL_BACKEND
    vita_glue_vgl_pool_ledger("boot");
#endif

    RGSSThreadData rtData(&eventThread, argv[0], win, alcDev, mode.refresh_rate,
                          mkxp_sys::getScalingFactor(), conf, glCtx);

    int winW, winH, drwW, drwH;
    SDL_GetWindowSize(win, &winW, &winH);
    rtData.windowSizeMsg.post(Vec2i(winW, winH));
    
    SDL_GL_GetDrawableSize(win, &drwW, &drwH);
    rtData.drawableSizeMsg.post(Vec2i(drwW, drwH));

    /* Load and post key bindings */
    rtData.bindingUpdateMsg.post(loadBindings(conf));
    
#ifdef MKXPZ_BUILD_XCODE
    // Create Touch Bar
    initTouchBar(win, conf);
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    // Arm before unwinding rtData, eventThread and conf if event processing throws.
    struct ShutdownOnReturn {
      ~ShutdownOnReturn() { vitaShutdownBegin(); }
    } shutdownOnReturn;
#endif

    /* Start RGSS thread */
    /* Vita: default SDL thread stack overflows in SharedState /
     * ruby_setup. 8 MiB matches the pthread probe.
     *
     * MRI's Fiber machine stacks are carved out of the LOW end
     * of this thread's own registered stack, because a Vita thread that
     * enters a syscall with SP outside the stack it registered is stopped
     * with 0x10006 -- which is what killed the rgss thread at the first
     * sceIoLseek made from an RGSS3 interpreter Fiber. So this thread now
     * asks for its own 8 MiB PLUS the arena plus the guard between them:
     *
     *   8 MiB   VITA_FIBER_ARENA_RESERVE_BYTES  the thread's own stack
     * + 128 KiB VITA_FIBER_ARENA_GUARD_BYTES    empty; a main-context stack
     *                                           overflow is a SystemStackError
     *                                           before it reaches a fiber
     * + 16 MiB  VITA_FIBER_ARENA_MAX_BYTES      49 fiber stacks of 328 KiB
     * = 24.125 MiB, under SDL's VITA_THREAD_STACK_SIZE_MAX of 32 MiB.
     */
#if !defined(__vita__) && defined(MKXPZ_HOST_PORT_LOGIC)
    SDL_AtomicSet(&rgssStartup, 0);
#endif
#ifdef __vita__
    SDL_AtomicSet(&rgssStartup, 0);
    SDL_Thread *rgssThread =
        SDL_CreateThreadWithStackSize(rgssThreadFun, "rgss",
                                      8 * 1024 * 1024 + 128 * 1024 +
                                          16 * 1024 * 1024,
                                      &rtData);
#else
    SDL_Thread *rgssThread = SDL_CreateThread(rgssThreadFun, "rgss", &rtData);
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    const bool workerReady = rgssThreadReady(rgssThread);
#else
    const bool workerReady = rgssThread != NULL;
    if (!workerReady)
      showInitError("Could not create RGSS thread.");
#endif

    if (workerReady) {
      /* Start event processing unless the worker has already finished. */
      if (!rtData.rqTermAck)
        eventThread.process(rtData);

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
      /* execute() includes ensure/at_exit saves after rqTermAck. Bound that
       * Ruby window separately; only its return starts native cleanup. */
      vitaShutdownProgress(VitaShutdownRuby);
      rtData.rqTerm.set();
      int workerResult = 0;
      SDL_WaitThread(rgssThread, &workerResult);
      if (workerResult != 0)
        return VITA_BOOT_RC_WEDGED;
#else
      /* Request RGSS thread to stop */
      rtData.rqTerm.set();

      /* Wait for RGSS thread response */
      for (int i = 0; i < 1000; ++i) {
        /* We can stop waiting when the request was ack'd */
        if (rtData.rqTermAck) {
          Debug() << "RGSS thread ack'd request after" << i * 10 << "ms";
          break;
        }

        /* Give RGSS thread some time to respond */
        SDL_Delay(10);
      }

      /* If RGSS thread ack'd request, wait for it to shutdown,
       * otherwise abandon hope and just end the process as is. */
      if (rtData.rqTermAck) {
        int workerResult = 0;
        SDL_WaitThread(rgssThread, &workerResult);
      } else
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR, conf.game.title.c_str(),
            std::string("The RGSS script seems to be stuck. "+conf.game.title+" will now force quit.").c_str(),
            win);
#endif
    }

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vitaShutdownBegin();
#endif
    if (!rtData.rgssErrorMsg.empty()) {
      Debug() << rtData.rgssErrorMsg;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
      /* rgssThreadError()'s message: OpenAL context creation, SharedState
       * construction, initGL. The game never started, so this is what the
       * launcher shows on the next boot. */
      vitaLogMessage("rgss-error: ", rtData.rgssErrorMsg.c_str());
      vitaWriteLastError(VITA_FATAL_KIND_INIT, conf.game.title.c_str(),
                         rtData.rgssErrorMsg.c_str());
#else
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, conf.game.title.c_str(),
                               rtData.rgssErrorMsg.c_str(), win);
#endif
    }

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vitaShutdownProgress(VitaShutdownGL);
#endif
    if (rtData.glContext)
      SDL_GL_DeleteContext(rtData.glContext);

    /* Clean up any remainin events */
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vitaShutdownProgress(VitaShutdownEvents);
#endif
    eventThread.cleanup();

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vita_glue_trace("Shutting down.");
#else
    Debug() << "Shutting down.";
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vitaShutdownProgress(VitaShutdownAudio);
    if (alcCloseDevice(alcDev) == ALC_FALSE) {
      vita_boot_report_error("mkxp-z", "The audio device did not close during shutdown.");
      return VITA_BOOT_RC_WEDGED;
    }
#else
    alcCloseDevice(alcDev);
#endif
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vitaShutdownProgress(VitaShutdownWindow);
#endif
    SDL_DestroyWindow(win);

#if defined(__WIN32__)
    if (wsadata.wVersion)
      WSACleanup();
#endif

#ifdef MKXPZ_STEAM
    STEAMSHIM_deinit();
#endif
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vitaShutdownProgress(VitaShutdownSound);
#endif
    Sound_Quit();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vitaShutdownProgress(VitaShutdownFonts);
#endif
    TTF_Quit();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vitaShutdownProgress(VitaShutdownImage);
#endif
    IMG_Quit();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vitaShutdownProgress(VitaShutdownSDL);
#endif
    SDL_Quit();

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    vitaShutdownProgress(VitaShutdownLocals);
    vitaCleanupReached = true;
#endif
    return workerReady ? 0 : 1;
}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
/*
 * The boot wrapper. Three jobs, and none of
 * them belongs inside mkxp_main():
 *
 *  1. Decide the mode BEFORE anything exists — before the log is opened, so
 *     the decision can choose which log to open.
 *  2. Catch what mkxp_main() lets escape. A C++ exception leaving main() is
 *     std::terminate, which on this device is a process kill with an empty
 *     log; catching it is the difference between "the game closed" and a
 *     sentence the user can read on the next launcher screen.
 *  3. Hand the process back to the launcher. It has to happen here, on the
 *     main thread, after mkxp-z's own shutdown has closed the AL device and
 *     released the GL context, and after the breadcrumb is gone.
 */
int main(int argc, char *argv[]) {
    int rc;

#ifdef MKXPZ_VITA_LAUNCHER
    vitaShutdownOwnsMarker = vita_boot_decide(argc, argv) == VITA_BOOT_MODE_GAME;
#else
    vita_boot_decide(argc, argv);
#endif

    try {
        rc = mkxp_main(argc, argv);
    }
    catch (const Exception &exc) {
        vitaShutdownBegin();
        vita_boot_report_error("mkxp-z", exc.msg.c_str());
        rc = VITA_BOOT_RC_WEDGED;
    }
    catch (const std::exception &exc) {
        vitaShutdownBegin();
        vita_boot_report_error("mkxp-z", exc.what());
        rc = VITA_BOOT_RC_WEDGED;
    }
    catch (...) {
        vitaShutdownBegin();
        vita_boot_report_error("mkxp-z", "Unknown exception in the player.");
        rc = VITA_BOOT_RC_WEDGED;
    }

    if (!vita_boot_is_launcher() && rc == VITA_BOOT_RC_WEDGED)
        return vitaShutdownAbandon();

    /* Order matters: confirm cleanup
     * while keeping the storage-independent watchdog armed, run the final
     * hand-off logging under it, then stop the async log writer — bounded —
     * and only then disarm. Every step here performs synchronous card I/O;
     * a stalled write must cost log evidence, never an unbounded exit. */
    if (!vita_boot_is_launcher() && !vitaShutdownConfirm())
        return vitaShutdownAbandon();
    rc = vita_boot_finish(rc);
    vita_glue_shutdown_log();
    if (!vita_boot_is_launcher() && !vitaShutdownComplete())
        return vitaShutdownAbandon();
    return rc;
}
#else
int main(int argc, char *argv[]) { return mkxp_main(argc, argv); }
#endif

static SDL_GLContext initGL(SDL_Window *win, Config &conf,
                            RGSSThreadData *threadData) {
  SDL_GLContext glCtx{};

  /* Setup GL context. Must be done in main thread since macOS 10.15 */
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    
  if (conf.debugMode)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);

  glCtx = SDL_GL_CreateContext(win);

  if (!glCtx) {
    GLINIT_SHOWERROR(std::string("Could not create OpenGL context: ") + SDL_GetError());
    return 0;
  }
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  {
    char tb[128];
    snprintf(tb, sizeof(tb), "trace: SDL_GL_CreateContext ok ctx=%p", (void *)glCtx);
    vita_glue_trace(tb);
  }
#endif

  try {
    initGLFunctions();
  } catch (const Exception &exc) {
    GLINIT_SHOWERROR(exc.msg);
    SDL_GL_DeleteContext(glCtx);

    return 0;
  }
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  vita_glue_trace("trace: initGLFunctions ok");
#endif

  if (!conf.enableBlitting)
    gl.BlitFramebuffer = 0;

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  vita_glue_trace("trace: initGL clear/swap next");
#endif
  gl.ClearColor(0, 0, 0, 1);
  gl.Clear(GL_COLOR_BUFFER_BIT);
  SDL_GL_SwapWindow(win);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  vita_glue_trace("trace: initGL clear/swap done");
#endif

  printGLInfo();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  vita_glue_trace("trace: printGLInfo done");
#endif

  bool vsync = conf.vsync || conf.syncToRefreshrate;
  SDL_GL_SetSwapInterval(vsync ? 1 : 0);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  vita_glue_trace("trace: initGL leave");
#endif

  // GLDebugLogger dLogger;
  return glCtx;
}
