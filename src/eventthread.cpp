/*
 ** eventthread.cpp
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

#include "eventthread.h"

#include <SDL_events.h>
#include <SDL_messagebox.h>
#include <SDL_system.h>
#include <SDL_timer.h>
#include <SDL_thread.h>
#include <SDL_touch.h>
#include <SDL_rect.h>

#include <al.h>
#include <alc.h>
#include <alext.h>
#include <cmath>

#include "sharedstate.h"
#include "graphics.h"

#include "al-util.h"
#include "debugwriter.h"

#include "util/string-util.h"

#include <string.h>
#include <cstdio>

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "vita_glue.h"
#endif

typedef void (ALC_APIENTRY *LPALCDEVICEPAUSESOFT) (ALCdevice *device);
typedef void (ALC_APIENTRY *LPALCDEVICERESUMESOFT) (ALCdevice *device);

#define AL_DEVICE_PAUSE_FUN \
AL_FUN(DevicePause, LPALCDEVICEPAUSESOFT) \
AL_FUN(DeviceResume, LPALCDEVICERESUMESOFT)

struct ALCFunctions
{
#define AL_FUN(name, type) type name;
    AL_DEVICE_PAUSE_FUN
#undef AL_FUN
} static alc;

static void
initALCFunctions(ALCdevice *alcDev)
{
    if (!strstr(alcGetString(alcDev, ALC_EXTENSIONS), "ALC_SOFT_pause_device"))
        return;
    
    Debug() << "ALC_SOFT_pause_device present";
    
#define AL_FUN(name, type) alc. name = (type) alcGetProcAddress(alcDev, "alc" #name "SOFT");
    AL_DEVICE_PAUSE_FUN;
#undef AL_FUN
}

#define HAVE_ALC_DEVICE_PAUSE alc.DevicePause

uint8_t EventThread::keyStates[];
EventThread::ControllerState EventThread::controllerState;
EventThread::MouseState EventThread::mouseState;
EventThread::TouchState EventThread::touchState;
SDL_atomic_t EventThread::verticalScrollDistance;

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
SDL_atomic_t EventThread::rawSystemButtons;

struct VitaTouchMouse
{
    bool active = false;
    SDL_Point position = {0, 0};

    void update(const SDL_Rect &screen) const
    {
        if (!active)
            return;
        EventThread::MouseState &state = EventThread::mouseState;
        bool inside = SDL_PointInRect(&position, &screen);
        /* A one-pixel negative offset can truncate to game coordinate zero.
         * Use a whole screen outside, including when the game rect changes. */
        state.x = inside ? position.x : screen.x - (screen.w > 0 ? screen.w : 960);
        state.y = inside ? position.y : screen.y - (screen.h > 0 ? screen.h : 544);
        state.inWindow = inside;
        if (!inside)
            state.buttons[SDL_BUTTON_LEFT] = false;
    }

    void handle(const SDL_Event &event, const SDL_Rect &screen)
    {
        active = true;
        position.x = event.type == SDL_MOUSEMOTION ? event.motion.x : event.button.x;
        position.y = event.type == SDL_MOUSEMOTION ? event.motion.y : event.button.y;
        update(screen);
        if (event.type != SDL_MOUSEMOTION)
            EventThread::mouseState.buttons[SDL_BUTTON_LEFT] =
                event.type == SDL_MOUSEBUTTONDOWN && EventThread::mouseState.inWindow;
    }
};

struct VitaSystemChords
{
    enum Action { None = 0, ToggleFPS = 1, Settings = 2, Quit = 4 };
    bool startDown = false, selectDown = false;
    bool startConsumed = false, selectConsumed = false;
    bool quitRequested = false;
    Uint64 chordSince = 0;

    unsigned sampleChord(Uint64 now)
    {
        if (!quitRequested && startDown && selectDown && now - chordSince >= 2000)
        {
            quitRequested = true;
            startConsumed = selectConsumed = true;
            return Quit;
        }
        return None;
    }

    unsigned button(int button, bool pressed, Uint64 now)
    {
        unsigned action = sampleChord(now);
        bool &down = button == SDL_CONTROLLER_BUTTON_START ? startDown : selectDown;
        bool &consumed = button == SDL_CONTROLLER_BUTTON_START ? startConsumed : selectConsumed;
        if (pressed == down)
            return action;
        down = pressed;
        if (pressed)
            consumed = false;
        else if (!consumed && !quitRequested)
            action |= button == SDL_CONTROLLER_BUTTON_START ? Settings : ToggleFPS;

        if (startDown && selectDown)
        {
            startConsumed = selectConsumed = true;
            chordSince = now;
        }
        return action;
    }
};

static unsigned vitaControllerButton(VitaSystemChords &chords, int button,
                                     bool pressed, Uint64 now)
{
    if (button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX)
        return VitaSystemChords::None;
    if (button != SDL_CONTROLLER_BUTTON_START && button != SDL_CONTROLLER_BUTTON_BACK)
    {
        EventThread::controllerState.buttons[button] = pressed;
        return VitaSystemChords::None;
    }

    EventThread::controllerState.buttons[button] = false;
    unsigned action = chords.button(button, pressed, now);
    /* Keep pressex?/triggerex?/releaseex? raw; only RGSS bindings reserve these. */
    SDL_AtomicSet(&EventThread::rawSystemButtons,
                 (chords.startDown ? 1 << SDL_CONTROLLER_BUTTON_START : 0) |
                 (chords.selectDown ? 1 << SDL_CONTROLLER_BUTTON_BACK : 0));
    return action;
}
#endif

/* User event codes */
enum
{
    REQUEST_SETFULLSCREEN = 0,
    REQUEST_WINRESIZE,
    REQUEST_WINREPOSITION,
    REQUEST_WINRENAME,
    REQUEST_WINCENTER,
    REQUEST_MESSAGEBOX,
    REQUEST_SETCURSORVISIBLE,
    
    REQUEST_TEXTMODE,
    
    REQUEST_SETTINGS,
    
    UPDATE_FPS,
    UPDATE_SCREEN_RECT,
    
    EVENT_COUNT
};

static uint32_t usrIdStart;

bool EventThread::allocUserEvents()
{
    usrIdStart = SDL_RegisterEvents(EVENT_COUNT);
    
    if (usrIdStart == (uint32_t) -1)
        return false;
    
    return true;
}

EventThread::EventThread()
: ctrl(0),
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
  joy(0),
#endif
fullscreen(false),
showCursor(false)
{
    textInputLock = SDL_CreateMutex();
}

EventThread::~EventThread()
{
    SDL_DestroyMutex(textInputLock);
}

SDL_TimerID hideCursorTimerID = 0;
Uint32 cursorTimerCallback(Uint32 interval, void* param)
{
	EventThread *ethread = static_cast<EventThread*>(param);
	hideCursorTimerID = 0;
	ethread->requestShowCursor(ethread->getShowCursor());
	return 0;
}
void EventThread::cursorTimer()
{
	SDL_RemoveTimer(hideCursorTimerID);
	hideCursorTimerID = SDL_AddTimer(500, cursorTimerCallback, this);
}

/* SDL_FingerID is a signed 64-bit handle and touchState has MAX_FINGERS
 * slots. Returns -1 for any id that cannot address one, so no caller can
 * write outside the array — including an event that reaches a finger case
 * through a fallthrough, where tfinger aliases an unrelated union member. */
static int fingerIndex(const SDL_Event &event)
{
    const SDL_FingerID id = event.tfinger.fingerId;
    
    if (id < 0 || id >= MAX_FINGERS)
        return -1;
    
    return (int) id;
}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
/* SDL_WaitEventTimeout without SDL's fallback: the Vita video backend has no
 * WaitEventTimeout/SendWakeupEvent hook, so SDL pumps every 1 ms (about
 * 1 kHz of controller/touch reads on this thread). Poll every 4 ms instead,
 * well inside one 16.7 ms frame. Same contract: 1 with an event; 0 on timeout
 * or on an SDL error, which SDL_GetError then reports. */
static const Uint64 VITA_EVENT_POLL_MS = 4;

static int vitaWaitEventTimeout(SDL_Event *event, Uint64 timeoutMs)
{
    const Uint64 start = SDL_GetTicks64();

    for (;;)
    {
        if (SDL_PollEvent(event))
            return 1;

        const Uint64 waited = SDL_GetTicks64() - start;

        if (waited >= timeoutMs || *SDL_GetError())
            return 0;

        const Uint64 left = timeoutMs - waited;
        SDL_Delay((Uint32)(left < VITA_EVENT_POLL_MS ? left : VITA_EVENT_POLL_MS));
    }
}
#endif

void EventThread::process(RGSSThreadData &rtData)
{
    SDL_Event event;
    SDL_Window *win = rtData.window;
    UnidirMessage<Vec2i> &windowSizeMsg = rtData.windowSizeMsg;
    UnidirMessage<Vec2i> &drawableSizeMsg = rtData.drawableSizeMsg;
    
    initALCFunctions(rtData.alcDev);
    
    SDL_SetEventFilter(eventFilter, &rtData);
    
    fullscreen = rtData.config.fullscreen;
    int toggleFSMod = rtData.config.anyAltToggleFS ? KMOD_ALT : KMOD_LALT;
    
    bool displayingFPS = rtData.config.displayFPS;
    
    if (displayingFPS || rtData.config.printFPS)
        fps.sendUpdates.set();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    if (displayingFPS)
        fps.overlayVisible.set();
#endif

    bool cursorInWindow = false;
    /* Will be updated eventually */
    SDL_Rect gameScreen = { 0, 0, 0, 0 };
    
    /* SDL doesn't send an initial FOCUS_GAINED event */
    bool windowFocused = true;
    
    bool terminate = false;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    VitaSystemChords systemChords;
    VitaTouchMouse touchMouse;
    /* Last samples taken on a scheduled iteration; the resume line reports
     * the SDL one as ticks_before so a frozen stretch shows up as a large
     * gap. On device the gap detector samples two clocks end to end: the
     * RTC (wall time, expected to advance through standby) and the
     * kernel-wide system clock (may not; whether it does is exactly what
     * the gap line's paired values answer). SDL's clock is the only one on
     * host, so there it stands in for both. */
#if defined(__vita__)
    Uint64 vitaRtcBefore = (Uint64)vita_glue_rtc_time_ms();
    Uint64 vitaTicksBefore = (Uint64)vita_glue_kernel_time_ms();
#ifdef MKXPZ_VITAGL_BACKEND
    Uint64 vitaWallBefore = (Uint64)vita_glue_wall_time_ms();
#endif
#else
    Uint64 vitaRtcBefore = SDL_GetTicks64();
    Uint64 vitaTicksBefore = SDL_GetTicks64();
#ifdef MKXPZ_VITAGL_BACKEND
    Uint64 vitaWallBefore = SDL_GetTicks64();
#endif
#endif
#ifdef MKXPZ_VITAGL_BACKEND
    /* vitaGL build: gettimeofday is a third gap clock, the one
     * wall clock a device sleep is known to advance (Time.now +275 s over a
     * 185 s suspend while the process clock moved 90 s); the beat below is a
     * diagnostic behind the system-event-trace marker. */
    Uint64 beatProc = SDL_GetTicks64(), beatRtc = vitaRtcBefore,
           beatWall = vitaWallBefore, beatKernel = vitaTicksBefore;
    unsigned beatIters = 0;
    auto beatDelta = [](Uint64 now, Uint64 then) -> unsigned long long
    { return now > then ? (unsigned long long)(now - then) : 0ULL; };
#endif
    auto handleSystemActions = [&](unsigned actions)
    {
        if (actions & VitaSystemChords::ToggleFPS)
        {
            displayingFPS = !displayingFPS;
            if (displayingFPS)
            {
                fps.overlayVisible.set();
                fps.sendUpdates.set();
            }
            else
            {
                fps.overlayVisible.clear();
                if (!rtData.config.printFPS)
                    fps.sendUpdates.clear();
            }
        }
        if ((actions & VitaSystemChords::Settings) && rtData.config.enableSettings)
            requestSettingsMenu();
        if (actions & VitaSystemChords::Quit)
        {
            vita_glue_trace("vita-input: Start+Select held; requesting clean shutdown");
            requestTerminate();
            /* main owns the ack wait, teardown, log flush and launcher hand-over.
             * Exit even if the SDL quit event could not be queued. */
            terminate = true;
        }
    };
#endif
    
    SDL_JoystickUpdate();
    
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    /* Vita: SDL 2.32.8 VITA_JoystickGetGamepadMapping always
     * returns SDL_FALSE, so SDL has no mapping for the GUID returned by
     * SDL_CreateJoystickGUIDForName("PSVita Controller").
     * Result: SDL_IsGameController(0)==false, controller never opens,
     * EventThread only handles CONTROLLER* → Input.press? stays false.
     * Register a runtime mapping from the live GUID, then open. */
    {
        char tb[256];
        int nj = SDL_NumJoysticks();
        snprintf(tb, sizeof(tb),
                 "input: NumJoysticks=%d IsGC0=%d", nj,
                 nj > 0 ? (int)SDL_IsGameController(0) : -1);
        vita_glue_trace(tb);
        if (nj > 0) {
            const char *jname = SDL_JoystickNameForIndex(0);
            SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(0);
            char guidStr[33];
            SDL_JoystickGetGUIDString(guid, guidStr, sizeof(guidStr));
            snprintf(tb, sizeof(tb), "input: joy0 name='%s' guid=%s",
                     jname ? jname : "(null)", guidStr);
            vita_glue_trace(tb);
            
            /* ext_button_map (SDL_sysjoystick.c):
             *  0 Triangle, 1 Circle, 2 Cross, 3 Square, 4 L1, 5 R1,
             *  6 Down, 7 Left, 8 Up, 9 Right, 10 Select, 11 Start.
             * defaultCtrlBindings: A=C, B=B, X=A(dash), Y=X. */
            char map[512];
            snprintf(map, sizeof(map),
                     "%s,%s,"
                     "a:b2,b:b1,x:b3,y:b0,"
                     "back:b10,start:b11,"
                     "leftshoulder:b4,rightshoulder:b5,"
                     "dpup:b8,dpdown:b6,dpleft:b7,dpright:b9,"
                     "leftx:a0,lefty:a1,rightx:a2,righty:a3,"
                     "lefttrigger:a4,righttrigger:a5,"
                     "platform:Vita",
                     guidStr, jname ? jname : "PSVita Controller");
            int addRc = SDL_GameControllerAddMapping(map);
            snprintf(tb, sizeof(tb),
                     "input: AddMapping rc=%d err='%s' IsGC0=%d",
                     addRc, SDL_GetError(),
                     SDL_IsGameController(0));
            vita_glue_trace(tb);
        }
    }
#endif
    
    if (SDL_NumJoysticks() > 0 && SDL_IsGameController(0)) {
            ctrl = SDL_GameControllerOpen(0);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            {
                char tb[160];
                snprintf(tb, sizeof(tb),
                         "input: GameControllerOpen ctrl=%p name=%s",
                         (void *)ctrl,
                         ctrl ? SDL_GameControllerName(ctrl) : "(null)");
                vita_glue_trace(tb);
            }
#endif
    }
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    else if (SDL_NumJoysticks() > 0) {
        /* Fallback: open as raw joystick so JOYBUTTON events are generated.
         * Mapping should make the controller path succeed; this is belt. */
        joy = SDL_JoystickOpen(0);
        char tb[160];
        snprintf(tb, sizeof(tb),
                 "input: fallback JoystickOpen joy=%p buttons=%d",
                 (void *)joy,
                 joy ? SDL_JoystickNumButtons(joy) : -1);
        vita_glue_trace(tb);
    }
#endif
    
    char buffer[128];
    
    char pendingTitle[128];
    bool havePendingTitle = false;
    
    bool resetting = false;
    
    int winW, winH;
    int i, rc;
    
    SDL_DisplayMode dm = {0};
    
    SDL_GetWindowSize(win, &winW, &winH);
    
    // Just in case it's started when the window is opened
    // for some dumb reason
    SDL_StopTextInput();
    
    textInputBuffer.clear();
    while (true)
    {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        /* Nothing announces suspend on Vita (no SDL_APP_* from this backend,
         * no APP_SUSPEND callback for a non-system title), so the existing
         * foreground handler is driven after the fact. ON_RESUME was never
         * observed on device, so a gap over 2 s between
         * scheduled iterations triggers the same repair. The gap is measured
         * on two clocks: the RTC (wall time, which advances through standby)
         * and the kernel-wide system clock, whose standby behavior is
         * unknown -- a loop that waits at most 50 ms can only open such a
         * gap by freezing, and the line logs both values so one device sleep
         * answers which clock moved. The poll stays first so the
         * marker-gated event-id trace still runs; the filter runs here,
         * synchronously inside SDL_PushEvent, repairing the ALC device and
         * the threads; the rest is the Vita repair. */
#if defined(__vita__)
        Uint64 vitaRtcNow = (Uint64)vita_glue_rtc_time_ms();
        Uint64 vitaNow = (Uint64)vita_glue_kernel_time_ms();
#ifdef MKXPZ_VITAGL_BACKEND
        Uint64 vitaWallNow = (Uint64)vita_glue_wall_time_ms();
#endif
#else
        Uint64 vitaRtcNow = SDL_GetTicks64();
        Uint64 vitaNow = SDL_GetTicks64();
#ifdef MKXPZ_VITAGL_BACKEND
        Uint64 vitaWallNow = SDL_GetTicks64();
#endif
#endif
        int vitaResume = vita_glue_poll_resume();
        bool vitaNotified = false;
        Uint64 vitaRtcGap = vitaRtcNow > vitaRtcBefore ?
                            vitaRtcNow - vitaRtcBefore : 0;
        Uint64 vitaGap = vitaNow > vitaTicksBefore ?
                         vitaNow - vitaTicksBefore : 0;
#ifdef MKXPZ_VITAGL_BACKEND
        Uint64 vitaWallGap = beatDelta(vitaWallNow, vitaWallBefore);
#endif
        if ((vitaRtcBefore && vitaRtcGap > 2000) ||
            (vitaTicksBefore && vitaGap > 2000)
#ifdef MKXPZ_VITAGL_BACKEND
            || (vitaWallBefore && vitaWallGap > 2000)
#endif
            )
        {
            /* First, so the lines below reach a live descriptor: files held
             * open across the standby may be stale. */
            vita_glue_resume_notify();
            vitaNotified = true;
            /* Zero before-sample: no scheduled iteration has run yet. */
            char gb[96];
            snprintf(gb, sizeof(gb),
                     "vita-lifecycle: gap rtc_ms=%llu kernel_ms=%llu",
                     (unsigned long long)vitaRtcGap,
                     (unsigned long long)vitaGap);
            vita_glue_trace(gb);
#ifdef MKXPZ_VITAGL_BACKEND
            snprintf(gb, sizeof(gb), "vita-lifecycle: gap wall_ms=%llu",
                     (unsigned long long)vitaWallGap);
            vita_glue_trace(gb);
#endif
            vitaResume = 1;
        }
        if (vitaResume)
        {
            if (!vitaNotified)
                vita_glue_resume_notify();
            Uint64 vitaTicksAfter = SDL_GetTicks64();
            SDL_OnApplicationWillEnterForeground();
            SDL_OnApplicationDidBecomeActive();
            /* Buttons physically released during the sleep must not stay
             * latched, and the limiter must not sprint off the frozen gap. */
            resetInputStates();
            /* A flag, not a call into Graphics: SharedState can be
             * mid-destruction after the scripts end. */
            rtData.rqFrameReset.set();
            char tb[128];
            snprintf(tb, sizeof(tb),
                     "vita-lifecycle: resume (ticks_before=%llu ticks_after=%llu)",
                     (unsigned long long)vitaTicksBefore,
                     (unsigned long long)vitaTicksAfter);
            vita_glue_trace(tb);
        }
#ifdef MKXPZ_VITAGL_BACKEND
        ++beatIters;
        if (vita_glue_lifecycle_trace_enabled() &&
            beatDelta(SDL_GetTicks64(), beatProc) >= 5000)
        {
            /* Deltas since the previous beat: the one after a wake shows
             * which clocks crossed the sleep and that the loop iterated. */
            char bb[192];
            snprintf(bb, sizeof(bb),
                     "vita-lifecycle: beat iters=%u proc_ms=%llu rtc_ms=%llu "
                     "wall_ms=%llu kernel_ms=%llu rtc=%llu wall=%llu",
                     beatIters, beatDelta(SDL_GetTicks64(), beatProc),
                     beatDelta(vitaRtcNow, beatRtc),
                     beatDelta(vitaWallNow, beatWall),
                     beatDelta(vitaNow, beatKernel),
                     (unsigned long long)vitaRtcNow,
                     (unsigned long long)vitaWallNow);
            vita_glue_trace(bb);
            beatIters = 0;
            beatProc = SDL_GetTicks64();
            beatRtc = vitaRtcNow;
            beatWall = vitaWallNow;
            beatKernel = vitaNow;
        }
#endif
#if defined(__vita__)
        vitaRtcBefore = (Uint64)vita_glue_rtc_time_ms();
        vitaTicksBefore = (Uint64)vita_glue_kernel_time_ms();
#ifdef MKXPZ_VITAGL_BACKEND
        vitaWallBefore = (Uint64)vita_glue_wall_time_ms();
#endif
#else
        vitaRtcBefore = vitaTicksBefore = SDL_GetTicks64();
#ifdef MKXPZ_VITAGL_BACKEND
        vitaWallBefore = vitaRtcBefore;
#endif
#endif
        if (SDL_AtomicGet(&terminateRequested))
            break;
        VitaSettingsInput snapshot;
        memcpy(snapshot.keys, keyStates, sizeof(snapshot.keys));
        copyRawControllerButtons(snapshot.buttons);
        memcpy(snapshot.axes, controllerState.axes, sizeof(snapshot.axes));
        settingsMenu.publish(snapshot);
        handleSystemActions(systemChords.sampleChord(SDL_GetTicks64()));
        if (terminate)
            break;
        /* A quiet queue must not strand the hold when RGSS has stopped. */
        SDL_ClearError();
        if (!vitaWaitEventTimeout(&event, 50))
        {
            if (*SDL_GetError())
            {
                Debug() << "EventThread: Event error";
                break;
            }
            continue;
        }
#else
        if (!SDL_WaitEvent(&event))
        {
            Debug() << "EventThread: Event error";
            break;
        }
#endif
        /* Preselect and discard unwanted events here */
        switch (event.type)
        {
            case SDL_MOUSEBUTTONDOWN :
            case SDL_MOUSEBUTTONUP :
            case SDL_MOUSEMOTION :
                if (event.button.which == SDL_TOUCH_MOUSEID)
                {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                    if (rtData.config.vitaTouchMouse)
                        touchMouse.handle(event, gameScreen);
#endif
                    continue;
                }
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                touchMouse.active = false;
#endif
                break;
                
            case SDL_FINGERDOWN :
            case SDL_FINGERUP :
            case SDL_FINGERMOTION :
                if (event.tfinger.fingerId >= MAX_FINGERS)
                    continue;
                break;
        }
        
#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && VITA_GLUE_FRAME_TRACE
        {
            static int evLogCount = 0;
            bool interesting =
                event.type == SDL_CONTROLLERBUTTONDOWN ||
                event.type == SDL_CONTROLLERBUTTONUP ||
                event.type == SDL_CONTROLLERAXISMOTION ||
                event.type == SDL_CONTROLLERDEVICEADDED ||
                event.type == SDL_CONTROLLERDEVICEREMOVED ||
                event.type == SDL_JOYBUTTONDOWN ||
                event.type == SDL_JOYBUTTONUP ||
                event.type == SDL_JOYAXISMOTION ||
                event.type == SDL_JOYDEVICEADDED ||
                event.type == SDL_JOYDEVICEREMOVED ||
                event.type == SDL_KEYDOWN ||
                event.type == SDL_KEYUP;
            if (interesting || evLogCount < 32) {
                char tb[192];
                if (event.type == SDL_CONTROLLERBUTTONDOWN ||
                    event.type == SDL_CONTROLLERBUTTONUP)
                    snprintf(tb, sizeof(tb),
                             "input: EV type=%u cbutton=%d which=%d state=%d",
                             event.type, event.cbutton.button,
                             event.cbutton.which, event.cbutton.state);
                else if (event.type == SDL_JOYBUTTONDOWN ||
                         event.type == SDL_JOYBUTTONUP)
                    snprintf(tb, sizeof(tb),
                             "input: EV type=%u jbutton=%d which=%d state=%d",
                             event.type, event.jbutton.button,
                             event.jbutton.which, event.jbutton.state);
                else if (event.type == SDL_CONTROLLERAXISMOTION)
                    snprintf(tb, sizeof(tb),
                             "input: EV caxis=%d val=%d",
                             event.caxis.axis, event.caxis.value);
                else if (event.type == SDL_JOYAXISMOTION)
                    snprintf(tb, sizeof(tb),
                             "input: EV jaxis=%d val=%d",
                             event.jaxis.axis, event.jaxis.value);
                else if (event.type == SDL_CONTROLLERDEVICEADDED ||
                         event.type == SDL_CONTROLLERDEVICEREMOVED)
                    snprintf(tb, sizeof(tb),
                             "input: EV cdevice type=%u which=%d",
                             event.type, event.cdevice.which);
                else if (event.type == SDL_JOYDEVICEADDED ||
                         event.type == SDL_JOYDEVICEREMOVED)
                    snprintf(tb, sizeof(tb),
                             "input: EV jdevice type=%u which=%d",
                             event.type, event.jdevice.which);
                else
                    snprintf(tb, sizeof(tb),
                             "input: EV type=%u", event.type);
                vita_glue_trace(tb);
                evLogCount++;
            }
        }
#endif
        
        /* Now process the rest */
        switch (event.type)
        {
            case SDL_WINDOWEVENT :
                switch (event.window.event)
                {
                    case SDL_WINDOWEVENT_SIZE_CHANGED :
                        winW = event.window.data1;
                        winH = event.window.data2;
                        
                        int drwW, drwH;
                        SDL_GL_GetDrawableSize(win, &drwW, &drwH);
                        
                        windowSizeMsg.post(Vec2i(winW, winH));
                        drawableSizeMsg.post(Vec2i(drwW, drwH));
                        resetInputStates();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                        systemChords = VitaSystemChords();
#endif
                        break;
                        
                    case SDL_WINDOWEVENT_ENTER :
                        cursorInWindow = true;
                        mouseState.inWindow = true;
                        updateCursorState(cursorInWindow && windowFocused, gameScreen);
                        
                        break;
                        
                    case SDL_WINDOWEVENT_LEAVE :
                        cursorInWindow = false;
                        mouseState.inWindow = false;
                        updateCursorState(cursorInWindow && windowFocused, gameScreen);
                        
                        break;
                        
                    case SDL_WINDOWEVENT_CLOSE :
                        terminate = true;
                        
                        break;
                        
                    case SDL_WINDOWEVENT_FOCUS_GAINED :
                        windowFocused = true;
                        updateCursorState(cursorInWindow && windowFocused, gameScreen);
                        
                        break;
                        
                    case SDL_WINDOWEVENT_FOCUS_LOST :
                        windowFocused = false;
                        updateCursorState(cursorInWindow && windowFocused, gameScreen);
                        resetInputStates();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                        systemChords = VitaSystemChords();
#endif
                        
                        break;
                }
                break;
                
            case SDL_TEXTINPUT :
                lockText(true);
                if (textInputBuffer.size() < 512) {
                    textInputBuffer += event.text.text;
                }
                lockText(false);
                break;
                
            case SDL_QUIT :
                terminate = true;
                Debug() << "EventThread termination requested";
                
                break;
                
            case SDL_KEYDOWN :
                if (event.key.keysym.scancode == SDL_SCANCODE_RETURN &&
                    (event.key.keysym.mod & toggleFSMod))
                {
                    setFullscreen(win, !fullscreen);
                    if (!fullscreen && havePendingTitle)
                    {
                        SDL_SetWindowTitle(win, pendingTitle);
                        pendingTitle[0] = '\0';
                        havePendingTitle = false;
                    }
                    
                    break;
                }
                
                if (event.key.keysym.scancode == SDL_SCANCODE_F1 && rtData.config.enableSettings)
                {
                    // Do not open settings menu until initializing shared state.
                    // Opening before initializing shared state will crash (segmentation fault).
                    if (!shState)
                    {
                        break;
                    }

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                    settingsMenu.request(false);
#endif
                }
                
                if (event.key.keysym.scancode == SDL_SCANCODE_F2)
                {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                    handleSystemActions(VitaSystemChords::ToggleFPS);
#else
                    if (!displayingFPS)
                    {
                        
                        fps.sendUpdates.set();
                        displayingFPS = true;
                    }
                    else
                    {
                        displayingFPS = false;
                        
                        if (!rtData.config.printFPS)
                            fps.sendUpdates.clear();
                        
                        if (fullscreen)
                        {
                            /* Prevent fullscreen flicker */
                            strncpy(pendingTitle, rtData.config.windowTitle.c_str(),
                                    sizeof(pendingTitle));
                            havePendingTitle = true;
                            
                            break;
                        }
                        
                        SDL_SetWindowTitle(win, rtData.config.windowTitle.c_str());
                    }
#endif
                    
                    break;
                }
                
                if (event.key.keysym.scancode == SDL_SCANCODE_F12)
                {
                    if (!rtData.config.enableReset)
                        break;
                    
                    if (resetting)
                        break;
                    
                    resetting = true;
                    rtData.rqResetFinish.clear();
                    rtData.rqReset.set();
                    break;
                }
                
                keyStates[event.key.keysym.scancode] = true;
                break;
                
            case SDL_KEYUP :
                if (event.key.keysym.scancode == SDL_SCANCODE_F12)
                {
                    if (!rtData.config.enableReset)
                        break;
                    
                    resetting = false;
                    rtData.rqResetFinish.set();
                    break;
                }
                
                keyStates[event.key.keysym.scancode] = false;
                break;
                
            case SDL_CONTROLLERBUTTONDOWN:
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            case SDL_CONTROLLERBUTTONUP:
                if (ctrl && event.cbutton.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(ctrl)))
                    handleSystemActions(vitaControllerButton(systemChords, event.cbutton.button,
                                        event.type == SDL_CONTROLLERBUTTONDOWN, SDL_GetTicks64()));
#else
                controllerState.buttons[event.cbutton.button] = true;
                break;
                
            case SDL_CONTROLLERBUTTONUP:
                controllerState.buttons[event.cbutton.button] = false;
#endif
                break;
                
            case SDL_CONTROLLERAXISMOTION:
                controllerState.axes[event.caxis.axis] = event.caxis.value;
                break;
                
            case SDL_CONTROLLERDEVICEADDED:
                if (event.cdevice.which > 0)
                    break;
                
                ctrl = SDL_GameControllerOpen(0);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                {
                    char tb[128];
                    snprintf(tb, sizeof(tb),
                             "input: CONTROLLERDEVICEADDED open=%p", (void *)ctrl);
                    vita_glue_trace(tb);
                }
#endif
                break;
                
            case SDL_CONTROLLERDEVICEREMOVED:
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                if (!ctrl || event.cdevice.which != SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(ctrl)))
                    break;
                systemChords = VitaSystemChords();
#endif
                resetInputStates();
                ctrl = 0;
                break;
                
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
            /* Raw-joystick fallback: translate Vita pad indices into the
             * controllerState slots defaultCtrlBindings poll. See
             * ext_button_map in SDL 2.32.8 SDL_sysjoystick.c. */
            case SDL_JOYBUTTONDOWN:
            case SDL_JOYBUTTONUP:
            {
                /* SDL emits raw and mapped events for the same pad. */
                if (ctrl || !joy || event.jbutton.which != SDL_JoystickInstanceID(joy))
                    break;
                static const int joyToCtrl[] = {
                    SDL_CONTROLLER_BUTTON_Y,              /* 0 Triangle */
                    SDL_CONTROLLER_BUTTON_B,              /* 1 Circle   */
                    SDL_CONTROLLER_BUTTON_A,              /* 2 Cross    */
                    SDL_CONTROLLER_BUTTON_X,              /* 3 Square   */
                    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,   /* 4 L1       */
                    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,  /* 5 R1       */
                    SDL_CONTROLLER_BUTTON_DPAD_DOWN,      /* 6          */
                    SDL_CONTROLLER_BUTTON_DPAD_LEFT,      /* 7          */
                    SDL_CONTROLLER_BUTTON_DPAD_UP,        /* 8          */
                    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,     /* 9          */
                    SDL_CONTROLLER_BUTTON_BACK,           /* 10 Select  */
                    SDL_CONTROLLER_BUTTON_START           /* 11 Start   */
                };
                int jb = event.jbutton.button;
                if (jb >= 0 && jb < (int)(sizeof(joyToCtrl)/sizeof(joyToCtrl[0]))) {
                    handleSystemActions(vitaControllerButton(systemChords, joyToCtrl[jb],
                                        event.type == SDL_JOYBUTTONDOWN, SDL_GetTicks64()));
                }
                break;
            }
                
            case SDL_JOYAXISMOTION:
                if (ctrl || !joy || event.jaxis.which != SDL_JoystickInstanceID(joy))
                    break;
                /* Vita axes: 0=LX 1=LY 2=RX 3=RY 4=LT 5=RT */
                if (event.jaxis.axis == 0)
                    controllerState.axes[SDL_CONTROLLER_AXIS_LEFTX] = event.jaxis.value;
                else if (event.jaxis.axis == 1)
                    controllerState.axes[SDL_CONTROLLER_AXIS_LEFTY] = event.jaxis.value;
                else if (event.jaxis.axis == 2)
                    controllerState.axes[SDL_CONTROLLER_AXIS_RIGHTX] = event.jaxis.value;
                else if (event.jaxis.axis == 3)
                    controllerState.axes[SDL_CONTROLLER_AXIS_RIGHTY] = event.jaxis.value;
                break;

            case SDL_JOYDEVICEREMOVED:
                if (joy && event.jdevice.which == SDL_JoystickInstanceID(joy))
                {
                    systemChords = VitaSystemChords();
                    resetInputStates();
                    SDL_JoystickClose(joy);
                    joy = 0;
                }
                break;
#endif
                
            case SDL_MOUSEBUTTONDOWN :
                mouseState.buttons[event.button.button] = true;
                break;
                
            case SDL_MOUSEBUTTONUP :
                mouseState.buttons[event.button.button] = false;
                break;
                
            case SDL_MOUSEMOTION :
                mouseState.x = event.motion.x;
                mouseState.y = event.motion.y;
                cursorTimer();
                updateCursorState(cursorInWindow, gameScreen);
                break;
                
            case SDL_MOUSEWHEEL :
                /* Only consider vertical scrolling for now */
                SDL_AtomicAdd(&verticalScrollDistance, event.wheel.y);
                break;
                
            case SDL_FINGERDOWN :
                i = fingerIndex(event);
                if (i < 0)
                    break;
                /* A new touch reports its position too */
                touchState.fingers[i].down = true;
                /* fall through */
                
            case SDL_FINGERMOTION :
                i = fingerIndex(event);
                if (i < 0)
                    break;
                touchState.fingers[i].x = event.tfinger.x * winW;
                touchState.fingers[i].y = event.tfinger.y * winH;
                break;
                
            case SDL_FINGERUP :
                i = fingerIndex(event);
                if (i < 0)
                    break;
                memset(&touchState.fingers[i], 0, sizeof(touchState.fingers[0]));
                break;
                
            default :
                /* Handle user events */
                switch(event.type - usrIdStart)
                {
                    case REQUEST_SETFULLSCREEN :
                        setFullscreen(win, static_cast<bool>(event.user.code));
                        break;
                        
                    case REQUEST_WINRESIZE :
                        SDL_SetWindowSize(win, event.window.data1, event.window.data2);
                        rtData.rqWindowAdjust.clear();
                        break;
                        
                    case REQUEST_WINREPOSITION :
                        SDL_SetWindowPosition(win, event.window.data1, event.window.data2);
                        rtData.rqWindowAdjust.clear();
                        break;
                        
                    case REQUEST_WINCENTER :
                        rc = SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(win), &dm);
                        if (!rc)
                            SDL_SetWindowPosition(win,
                                                  (dm.w / 2) - (winW / 2),
                                                  (dm.h / 2) - (winH / 2));
                        rtData.rqWindowAdjust.clear();
                        break;
                        
                    case REQUEST_WINRENAME :
                        rtData.config.windowTitle = (const char*)event.user.data1;
                        SDL_SetWindowTitle(win, rtData.config.windowTitle.c_str());
                        break;
                        
                    case REQUEST_TEXTMODE :
                        if (event.user.code)
                        {
                            {
                                const SDL_Rect rect = {0, 0, 1, 1};
                                SDL_SetTextInputRect(&rect);
                            }
                            SDL_StartTextInput();
                            lockText(true);
                            textInputBuffer.clear();
                            lockText(false);
                        }
                        else
                        {
                            SDL_StopTextInput();
                            lockText(true);
                            textInputBuffer.clear();
                            lockText(false);
                        }
                        break;
                        
                    case REQUEST_MESSAGEBOX :
                    {
                        // Try to format the message with additional newlines
                        std::string message = copyWithNewlines((const char*) event.user.data1,
                                                               70);
                        SDL_ShowSimpleMessageBox(event.user.code,
                                                 rtData.config.windowTitle.c_str(),
                                                 message.c_str(), win);
                        free(event.user.data1);
                        msgBoxDone.set();
                        break;
                    }
                    case REQUEST_SETCURSORVISIBLE :
                        showCursor = event.user.code;
                        updateCursorState(cursorInWindow, gameScreen);
                        break;
                        
                    case REQUEST_SETTINGS :
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                        requestSettingsMenu();
#endif
                        break;
                        
                    case UPDATE_FPS :
                        if (rtData.config.printFPS)
                            Debug() << "FPS:" << event.user.code;
                        
                        if (!fps.sendUpdates)
                            break;
                        
                        snprintf(buffer, sizeof(buffer), "%s - %d FPS",
                                 rtData.config.windowTitle.c_str(), event.user.code);
                        
                        /* Updating the window title in fullscreen
                         * mode seems to cause flickering */
                        if (fullscreen)
                        {
                            strncpy(pendingTitle, buffer, sizeof(pendingTitle));
                            havePendingTitle = true;
                            
                            break;
                        }
                        
                        SDL_SetWindowTitle(win, buffer);
                        break;
                        
                    case UPDATE_SCREEN_RECT :
                        gameScreen.x = event.user.windowID;
                        gameScreen.y = event.user.code;
                        gameScreen.w = reinterpret_cast<intptr_t>(event.user.data1);
                        gameScreen.h = reinterpret_cast<intptr_t>(event.user.data2);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
                        touchMouse.update(gameScreen);
#endif
                        updateCursorState(cursorInWindow, gameScreen);
                        
                        break;
                }
        }
        
        if (terminate)
            break;
    }
    
    /* Just in case */
    rtData.syncPoint.resumeThreads();
    
    if (SDL_GameControllerGetAttached(ctrl))
        SDL_GameControllerClose(ctrl);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    if (joy)
    {
        SDL_JoystickClose(joy);
        joy = 0;
    }
#endif
    
}

int EventThread::eventFilter(void *data, SDL_Event *event)
{
    RGSSThreadData &rtData = *static_cast<RGSSThreadData*>(data);
    
    switch (event->type)
    {
        case SDL_APP_WILLENTERBACKGROUND :
            Debug() << "SDL_APP_WILLENTERBACKGROUND";
            
            if (HAVE_ALC_DEVICE_PAUSE)
                alc.DevicePause(rtData.alcDev);
            
            rtData.syncPoint.haltThreads();
            
            return 0;
            
        case SDL_APP_DIDENTERBACKGROUND :
            Debug() << "SDL_APP_DIDENTERBACKGROUND";
            return 0;
            
        case SDL_APP_WILLENTERFOREGROUND :
            Debug() << "SDL_APP_WILLENTERFOREGROUND";
            return 0;
            
        case SDL_APP_DIDENTERFOREGROUND :
            Debug() << "SDL_APP_DIDENTERFOREGROUND";
            
            if (HAVE_ALC_DEVICE_PAUSE)
                alc.DeviceResume(rtData.alcDev);
            
            rtData.syncPoint.resumeThreads();
            
            return 0;
            
        case SDL_APP_TERMINATING :
            Debug() << "SDL_APP_TERMINATING";
            return 0;
            
        case SDL_APP_LOWMEMORY :
            Debug() << "SDL_APP_LOWMEMORY";
            return 0;
            
            //	case SDL_RENDER_TARGETS_RESET :
            //		Debug() << "****** SDL_RENDER_TARGETS_RESET";
            //		return 0;
            
            //	case SDL_RENDER_DEVICE_RESET :
            //		Debug() << "****** SDL_RENDER_DEVICE_RESET";
            //		return 0;
    }
    
    return 1;
}

void EventThread::cleanup()
{
    SDL_Event event;
    
    while (SDL_PollEvent(&event))
        if ((event.type - usrIdStart) == REQUEST_MESSAGEBOX)
            free(event.user.data1);
}

void EventThread::resetInputStates()
{
    memset(&keyStates, 0, sizeof(keyStates));
    memset(&controllerState, 0, sizeof(controllerState));
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    SDL_AtomicSet(&rawSystemButtons, 0);
#endif
    memset(&mouseState.buttons, 0, sizeof(mouseState.buttons));
    memset(&touchState, 0, sizeof(touchState));
}

void EventThread::setFullscreen(SDL_Window *win, bool mode)
{
    SDL_SetWindowFullscreen
    (win, mode ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    fullscreen = mode;
}

void EventThread::updateCursorState(bool inWindow,
                                    const SDL_Rect &screen)
{
    SDL_Point pos = { mouseState.x, mouseState.y };
    bool inScreen = inWindow && SDL_PointInRect(&pos, &screen);
    
    if (inScreen)
        SDL_ShowCursor(showCursor || hideCursorTimerID ? SDL_TRUE : SDL_FALSE);
    else
        SDL_ShowCursor(SDL_TRUE);
}

void EventThread::requestTerminate()
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    /* The bounded wait observes this even if SDL drops the wakeup. */
    SDL_AtomicSet(&terminateRequested, 1);
#endif
    SDL_Event event;
    event.type = SDL_QUIT;
    SDL_PushEvent(&event);
}

void EventThread::requestFullscreenMode(bool mode)
{
    if (mode == fullscreen)
        return;
    
    SDL_Event event;
    event.type = usrIdStart + REQUEST_SETFULLSCREEN;
    event.user.code = static_cast<Sint32>(mode);
    SDL_PushEvent(&event);
}

void EventThread::requestWindowResize(int width, int height)
{
    shState->rtData().rqWindowAdjust.set();
    SDL_Event event;
    event.type = usrIdStart + REQUEST_WINRESIZE;
    event.window.data1 = width;
    event.window.data2 = height;
    SDL_PushEvent(&event);
}

void EventThread::requestWindowReposition(int x, int y)
{
    shState->rtData().rqWindowAdjust.set();
    SDL_Event event;
    event.type = usrIdStart + REQUEST_WINREPOSITION;
    event.window.data1 = x;
    event.window.data2 = y;
    SDL_PushEvent(&event);
}

void EventThread::requestWindowCenter()
{
    shState->rtData().rqWindowAdjust.set();
    SDL_Event event;
    event.type = usrIdStart + REQUEST_WINCENTER;
    SDL_PushEvent(&event);
}

void EventThread::requestWindowRename(const char *title)
{
    SDL_Event event;
    event.type = usrIdStart + REQUEST_WINRENAME;
    event.user.data1 = (void*)title;
    SDL_PushEvent(&event);
}

void EventThread::requestShowCursor(bool mode)
{
    SDL_Event event;
    event.type = usrIdStart + REQUEST_SETCURSORVISIBLE;
    event.user.code = mode;
    SDL_PushEvent(&event);
}

void EventThread::requestTextInputMode(bool mode)
{
    SDL_Event event;
    event.type = usrIdStart + REQUEST_TEXTMODE;
    event.user.code = mode;
    SDL_PushEvent(&event);
}

void EventThread::requestSettingsMenu()
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    settingsMenu.request();
#else
    SDL_Event event;
    event.type = usrIdStart + REQUEST_SETTINGS;
    SDL_PushEvent(&event);
#endif
}

void EventThread::showMessageBox(const char *body, int flags)
{
    msgBoxDone.clear();
    
    // mkxp has already been asked to quit.
    // Don't break things if the window wants to close
    if (shState->rtData().rqTerm)
        return;
    
    SDL_Event event;
    event.user.code = flags;
    event.user.data1 = SDL_strdup(body);
    event.type = usrIdStart + REQUEST_MESSAGEBOX;
    SDL_PushEvent(&event);
    
    /* Keep repainting screen while box is open */
    try{
        shState->graphics().repaintWait(msgBoxDone);
    }catch(...){}
    /* Prevent endless loops */
    resetInputStates();
}

bool EventThread::getFullscreen() const
{
    return fullscreen;
}

bool EventThread::getShowCursor() const
{
    return showCursor;
}

bool EventThread::getControllerConnected() const
{
    return ctrl != 0;
}

SDL_GameController *EventThread::controller() const
{
    return ctrl;
}

void EventThread::copyRawControllerButtons(uint8_t *buttons)
{
    memcpy(buttons, controllerState.buttons, SDL_CONTROLLER_BUTTON_MAX);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    const int systemButtons = SDL_AtomicGet(&rawSystemButtons);
    buttons[SDL_CONTROLLER_BUTTON_START] = (systemButtons & (1 << SDL_CONTROLLER_BUTTON_START)) != 0;
    buttons[SDL_CONTROLLER_BUTTON_BACK] = (systemButtons & (1 << SDL_CONTROLLER_BUTTON_BACK)) != 0;
#endif
}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
void EventThread::updateFPSOverlay()
{
    Graphics &graphics = shState->graphics();
    const bool visible = fps.overlayVisible;
    graphics.overlaySetVisible(visible);
    if (!visible)
        return;

    const Uint64 now = SDL_GetTicks64();
    if (fps.overlaySampled && now - fps.overlayUpdatedAt < 250)
        return;
    fps.overlaySampled = true;
    fps.overlayUpdatedAt = now;

    const double rate = graphics.averageFrameRate();
    char line[64];
    if (std::isfinite(rate) && rate > 0 && std::isfinite(1000.0 / rate))
        snprintf(line, sizeof(line), "FPS %.0f  frame %.1f ms", rate, 1000.0 / rate);
    else
        snprintf(line, sizeof(line), "FPS 0  frame -- ms");
    /* Line 0 belongs to FPS; profile lines can be appended independently. */
    const char *lines[] = {line};
    graphics.overlaySetLines(lines, 1);
}
#endif

void EventThread::notifyFrame()
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    updateFPSOverlay();
#endif
    if (!fps.sendUpdates)
        return;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    const Uint64 now = SDL_GetTicks64();
    if (fps.eventSent && now - fps.eventSentAt < 250)
        return;
    fps.eventSent = true;
    fps.eventSentAt = now;
#endif
    
    SDL_Event event;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    const double frames = std::round(shState->graphics().averageFrameRate());
    event.user.code = std::isfinite(frames) && frames > 0 && frames <= SDL_MAX_SINT32
                         ? static_cast<Sint32>(frames) : 0;
#else
    event.user.code = round(shState->graphics().averageFrameRate());
#endif
    event.user.type = usrIdStart + UPDATE_FPS;
    SDL_PushEvent(&event);
}

void EventThread::notifyGameScreenChange(const SDL_Rect &screen)
{
    /* We have to get a bit hacky here to fit the rectangle
     * data into the user event struct */
    SDL_Event event;
    event.type = usrIdStart + UPDATE_SCREEN_RECT;
    event.user.windowID = screen.x;
    event.user.code = screen.y;
    event.user.data1 = reinterpret_cast<void*>(screen.w);
    event.user.data2 = reinterpret_cast<void*>(screen.h);
    SDL_PushEvent(&event);
}

void EventThread::lockText(bool lock)
{
    lock ? SDL_LockMutex(textInputLock) : SDL_UnlockMutex(textInputLock);
}

void SyncPoint::haltThreads()
{
    if (mainSync.locked)
        return;
    
    /* Lock the reply sync first to avoid races */
    reply.lock();
    
    /* Lock main sync and sleep until RGSS thread
     * reports back */
    mainSync.lock();
    reply.waitForUnlock();
    
    /* Now that the RGSS thread is asleep, we can
     * safely put the other threads to sleep as well
     * without causing deadlocks */
    secondSync.lock();
}

void SyncPoint::resumeThreads()
{
    if (!mainSync.locked)
        return;
    
    mainSync.unlock(false);
    secondSync.unlock(true);
}

bool SyncPoint::mainSyncLocked()
{
    return mainSync.locked;
}

void SyncPoint::waitMainSync()
{
    reply.unlock(false);
    mainSync.waitForUnlock();
}

void SyncPoint::passSecondarySync()
{
    if (!secondSync.locked)
        return;
    
    secondSync.waitForUnlock();
}

SyncPoint::Util::Util()
{
    mut = SDL_CreateMutex();
    cond = SDL_CreateCond();
}

SyncPoint::Util::~Util()
{
    SDL_DestroyCond(cond);
    SDL_DestroyMutex(mut);
}

void SyncPoint::Util::lock()
{
    locked.set();
}

void SyncPoint::Util::unlock(bool multi)
{
    locked.clear();
    
    if (multi)
        SDL_CondBroadcast(cond);
    else
        SDL_CondSignal(cond);
}

void SyncPoint::Util::waitForUnlock()
{
    SDL_LockMutex(mut);
    
    while (locked)
        SDL_CondWait(cond, mut);
    
    SDL_UnlockMutex(mut);
}
