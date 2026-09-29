// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * launcher_model.c — see launcher_model.h.
 */
#include "launcher_model.h"

#include <string.h>

/* Wrap-safe "has `now` reached `deadline`?" over a free-running ms counter. */
static int time_reached(unsigned int now, unsigned int deadline)
{
    return (int)(now - deadline) >= 0;
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/* Restore  0 <= top <= max(0, count - rows)  and  top <= selected < top+rows. */
static void reframe(LauncherModel *m)
{
    int max_top;

    if (m->count <= 0) {
        m->selected = 0;
        m->top = 0;
        return;
    }
    m->selected = clamp_int(m->selected, 0, m->count - 1);

    max_top = m->count - m->rows;
    if (max_top < 0)
        max_top = 0;

    if (m->selected < m->top)
        m->top = m->selected;
    else if (m->selected >= m->top + m->rows)
        m->top = m->selected - m->rows + 1;

    m->top = clamp_int(m->top, 0, max_top);
}

void launcher_model_init(LauncherModel *m, int count, int rows,
                         int preferred_index, unsigned int held_mask)
{
    int b;

    if (!m)
        return;
    memset(m, 0, sizeof(*m));
    m->count = count > 0 ? count : 0;
    m->rows = rows > 0 ? rows : 1;
    m->screen = LAUNCHER_SCREEN_LIST;
    m->selected = (preferred_index >= 0) ? preferred_index : 0;

    for (b = 0; b < LAUNCHER_BTN_COUNT; b++) {
        int down = (held_mask & LAUNCHER_BTN_BIT(b)) != 0;

        m->held[b] = (unsigned char)down;
        /* Held at start-up = held over from the previous process: not armed
         * until we have seen it released. Everything else is armed now. */
        m->armed[b] = (unsigned char)(down ? 0 : 1);
    }

    reframe(m);
}

void launcher_model_set_count(LauncherModel *m, int count, int preferred_index)
{
    if (!m)
        return;
    m->count = count > 0 ? count : 0;
    m->selected = (preferred_index >= 0) ? preferred_index : 0;
    m->top = 0;
    reframe(m);
}

void launcher_model_show_message(LauncherModel *m)
{
    if (!m)
        return;
    m->screen = LAUNCHER_SCREEN_MESSAGE;
}

int launcher_model_selected(const LauncherModel *m)
{
    if (!m || m->count <= 0)
        return -1;
    return m->selected;
}

int launcher_model_input_armed(const LauncherModel *m)
{
    int b;

    if (!m)
        return 0;
    for (b = 0; b < LAUNCHER_BTN_COUNT; b++) {
        if (!m->armed[b])
            return 0;
    }
    return 1;
}

static int is_navigation(LauncherButton b)
{
    switch (b) {
    case LAUNCHER_BTN_UP:
    case LAUNCHER_BTN_DOWN:
    case LAUNCHER_BTN_LEFT:
    case LAUNCHER_BTN_RIGHT:
    case LAUNCHER_BTN_PAGE_UP:
    case LAUNCHER_BTN_PAGE_DOWN:
        return 1;
    default:
        return 0;
    }
}

/*
 * Move the cursor. Single steps wrap around (the list is a ring); paging
 * clamps, because a page that wraps past the end reads as a random jump.
 * Returns 1 if anything changed.
 */
static int navigate(LauncherModel *m, LauncherButton b)
{
    int before_sel = m->selected, before_top = m->top;

    if (m->count <= 0)
        return 0;

    switch (b) {
    case LAUNCHER_BTN_UP:
        m->selected = (m->selected > 0) ? m->selected - 1 : m->count - 1;
        break;
    case LAUNCHER_BTN_DOWN:
        m->selected = (m->selected + 1 < m->count) ? m->selected + 1 : 0;
        break;
    case LAUNCHER_BTN_LEFT:
    case LAUNCHER_BTN_PAGE_UP:
        m->selected = clamp_int(m->selected - m->rows, 0, m->count - 1);
        break;
    case LAUNCHER_BTN_RIGHT:
    case LAUNCHER_BTN_PAGE_DOWN:
        m->selected = m->rows > m->count - 1 - m->selected
                          ? m->count - 1 : m->selected + m->rows;
        break;
    default:
        return 0;
    }
    reframe(m);
    return m->selected != before_sel || m->top != before_top;
}

LauncherAction launcher_model_button(LauncherModel *m, LauncherButton button,
                                     int down, unsigned int now_ms)
{
    /* Via int: the enum's underlying type may be unsigned (GCC on ARM), so
     * comparing the enum value itself against 0 is a compile error. */
    int index = (int)button;

    if (!m || index < 0 || index >= LAUNCHER_BTN_COUNT)
        return LAUNCHER_ACTION_NONE;

    if (!down) {
        /* A release always arms: the button is demonstrably free now. */
        m->held[button] = 0;
        m->armed[button] = 1;
        return LAUNCHER_ACTION_NONE;
    }

    /*
     * A down edge for a button we already believe is down is either an OS key
     * repeat or the front end re-reporting a button that was held over from
     * the previous process (launcher_model_init's held_mask). Either way it is
     * not a new press: swallow it, and leave `armed` alone so the release is
     * still what arms the button.
     */
    if (m->held[button])
        return LAUNCHER_ACTION_NONE;
    m->held[button] = 1;
    m->repeat_at[button] = now_ms + LAUNCHER_REPEAT_DELAY_MS;

    if (m->screen == LAUNCHER_SCREEN_MESSAGE) {
        if (button == LAUNCHER_BTN_CONFIRM || button == LAUNCHER_BTN_CANCEL) {
            m->screen = LAUNCHER_SCREEN_LIST;
            return LAUNCHER_ACTION_DISMISS;
        }
        return LAUNCHER_ACTION_NONE;
    }

    switch (button) {
    case LAUNCHER_BTN_CONFIRM:
        /* Nothing to launch, or a button is still held from the last process. */
        if (m->count <= 0 || !launcher_model_input_armed(m))
            return LAUNCHER_ACTION_NONE;
        return LAUNCHER_ACTION_LAUNCH;
    case LAUNCHER_BTN_RESCAN:
        return LAUNCHER_ACTION_RESCAN;
    case LAUNCHER_BTN_CANCEL:
        /* The launcher is the root screen: there is nowhere to go back to. */
        return LAUNCHER_ACTION_NONE;
    default:
        break;
    }

    return navigate(m, button) ? LAUNCHER_ACTION_REDRAW : LAUNCHER_ACTION_NONE;
}

LauncherAction launcher_model_tick(LauncherModel *m, unsigned int now_ms)
{
    int b, moved = 0;

    if (!m || m->screen != LAUNCHER_SCREEN_LIST)
        return LAUNCHER_ACTION_NONE;

    for (b = 0; b < LAUNCHER_BTN_COUNT; b++) {
        if (!m->held[b] || !m->armed[b])
            continue;
        if (!is_navigation((LauncherButton)b))
            continue;
        if (!time_reached(now_ms, m->repeat_at[b]))
            continue;
        /* One step per tick: a late tick must not fire a burst of steps. */
        m->repeat_at[b] = now_ms + LAUNCHER_REPEAT_RATE_MS;
        if (navigate(m, (LauncherButton)b))
            moved = 1;
    }

    return moved ? LAUNCHER_ACTION_REDRAW : LAUNCHER_ACTION_NONE;
}
