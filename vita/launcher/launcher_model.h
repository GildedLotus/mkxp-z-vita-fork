// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * launcher_model.h — the launcher's list state machine.
 *
 * All of the launcher's timing and edge logic lives here, away from SDL, so
 * it can be proven on the host with no window, no pad and no clock: the
 * caller passes the time in. The front end only translates
 * SDL_JOYBUTTONDOWN/UP and stick deflection into the button enum below, calls
 * launcher_model_tick() once a frame, and obeys the returned action.
 *
 * The one rule that is not obvious: CONFIRM is ignored until every button has
 * been seen released once. The launcher is reached by sceAppMgrLoadExec from
 * a game the user just quit — the button that quit the game is very likely
 * still held when the launcher's first frame runs, and it must not
 * immediately relaunch that same game. launcher_model_init() therefore takes
 * the buttons that are physically held at start-up; everything else is armed
 * straight away, so a clean boot responds to the very first press.
 *
 * C99, libc only (string.h). No allocation, no globals, no I/O.
 */
#ifndef VITA_LAUNCHER_MODEL_H
#define VITA_LAUNCHER_MODEL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAUNCHER_BTN_UP = 0,
    LAUNCHER_BTN_DOWN,
    LAUNCHER_BTN_LEFT,  /* same as PAGE_UP: a vertical list has no columns */
    LAUNCHER_BTN_RIGHT, /* same as PAGE_DOWN */
    LAUNCHER_BTN_CONFIRM,
    LAUNCHER_BTN_CANCEL,
    LAUNCHER_BTN_RESCAN,
    LAUNCHER_BTN_PAGE_UP,
    LAUNCHER_BTN_PAGE_DOWN,
    LAUNCHER_BTN_COUNT
} LauncherButton;

#define LAUNCHER_BTN_BIT(b) (1u << (unsigned)(b))

typedef enum {
    LAUNCHER_ACTION_NONE = 0, /* nothing happened; do not redraw */
    LAUNCHER_ACTION_REDRAW,   /* the view changed */
    LAUNCHER_ACTION_LAUNCH,   /* launch launcher_model_selected() */
    LAUNCHER_ACTION_RESCAN,   /* rescan, then launcher_model_set_count() */
    LAUNCHER_ACTION_DISMISS   /* the message screen was acknowledged */
} LauncherAction;

typedef enum {
    LAUNCHER_SCREEN_LIST = 0,
    LAUNCHER_SCREEN_MESSAGE
} LauncherScreen;

#define LAUNCHER_REPEAT_DELAY_MS 400u /* hold this long before repeating */
#define LAUNCHER_REPEAT_RATE_MS  80u  /* then one step every this long */

typedef struct LauncherModel {
    int count;    /* entries in the list */
    int rows;     /* visible rows; always >= 1 */
    int top;      /* first visible index; 0 <= top <= max(0, count - rows) */
    int selected; /* invariant: top <= selected < top + rows */
    LauncherScreen screen;

    /* Per-button state. `armed` is the held-over-button guard. */
    unsigned char held[LAUNCHER_BTN_COUNT];
    unsigned char armed[LAUNCHER_BTN_COUNT];
    unsigned int repeat_at[LAUNCHER_BTN_COUNT]; /* ms, valid while held */
} LauncherModel;

/*
 * `rows` is clamped to >= 1. `preferred_index` is clamped into the list (use
 * launcher_state_index_of()'s result, or -1 for "no preference").
 * `held_mask` is a bitmask of LAUNCHER_BTN_BIT(x) for the buttons physically
 * down right now — pass 0 when nothing is held.
 */
void launcher_model_init(LauncherModel *m, int count, int rows,
                         int preferred_index, unsigned int held_mask);

/* After a rescan: same screen and button state, new list. */
void launcher_model_set_count(LauncherModel *m, int count,
                              int preferred_index);

/* Put up the message screen (an error, or the empty-games explanation). */
void launcher_model_show_message(LauncherModel *m);

/*
 * One button edge. `down` is 1 for a press, 0 for a release. `now_ms` is a
 * free-running millisecond counter (SDL_GetTicks); wrap-around is handled.
 */
LauncherAction launcher_model_button(LauncherModel *m, LauncherButton button,
                                     int down, unsigned int now_ms);

/* Once a frame: emits the auto-repeat steps that have come due. */
LauncherAction launcher_model_tick(LauncherModel *m, unsigned int now_ms);

/* Selected index, or -1 when the list is empty. */
int launcher_model_selected(const LauncherModel *m);

/* 1 once every button has been seen released (or was never held). */
int launcher_model_input_armed(const LauncherModel *m);

#ifdef __cplusplus
}
#endif

#endif /* VITA_LAUNCHER_MODEL_H */
