// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <SDL_atomic.h>
#include <SDL_gamecontroller.h>
#include <SDL_scancode.h>
#include <cstring>

struct VitaSettingsInput {
    uint8_t keys[SDL_NUM_SCANCODES] = {};
    uint8_t buttons[SDL_CONTROLLER_BUTTON_MAX] = {};
    int axes[SDL_CONTROLLER_AXIS_MAX] = {};
    bool acceptRequest = false;

    bool axisHeld(int axis, bool negative, bool action, int threshold,
                  bool active = false) const {
        // Match CtrlAxisBinding: movement/triggers keep the configured gate.
        if (action && axis <= SDL_CONTROLLER_AXIS_RIGHTY) {
            if (threshold < 9830) threshold = 9830;
            if (active) threshold -= threshold >> 2;
        }
        return (negative ? -axes[axis] : axes[axis]) > threshold;
    }

    bool neutral(int threshold, bool action = false) const {
        for (auto key : keys) if (key) return false;
        for (auto button : buttons) if (button) return false;
        for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i)
            if (axisHeld(i, false, action, threshold, true) ||
                axisHeld(i, true, action, threshold, true)) return false;
        return true;
    }
};

/* EventThread publishes complete snapshots; only the GL owner consumes them.
 * A bounded queue retains taps between frames without adding kernel objects. */
class VitaSettingsHandoff {
    struct Lock {
        SDL_SpinLock &guard;
        explicit Lock(SDL_SpinLock &g) : guard(g) { SDL_AtomicLock(&guard); }
        ~Lock() { SDL_AtomicUnlock(&guard); }
    };
    bool pending = false, active = false;
    VitaSettingsInput latest, queue[32];
    unsigned head = 0, count = 0;
    SDL_SpinLock guard = 0;
    void append(const VitaSettingsInput &input) {
        if (count == 32) { head = (head + 1) & 31; --count; }
        queue[(head + count++) & 31] = input;
    }
public:
    void request(bool acceptIfActive = true) {
        Lock lock(guard);
        if (active && acceptIfActive) {
            VitaSettingsInput command = latest;
            command.acceptRequest = true;
            append(command);
        }
        else if (!active) pending = true;
    }
    bool begin(VitaSettingsInput &initial) {
        Lock lock(guard);
        if (!pending || active) return false;
        pending = false;
        active = true;
        head = count = 0;
        initial = latest;
        return true;
    }
    void publish(const VitaSettingsInput &input) {
        Lock lock(guard);
        if (!std::memcmp(latest.keys, input.keys, sizeof(input.keys)) &&
            !std::memcmp(latest.buttons, input.buttons, sizeof(input.buttons)) &&
            !std::memcmp(latest.axes, input.axes, sizeof(input.axes))) return;
        latest = input;
        if (!active) return;
        append(input);
    }
    bool next(VitaSettingsInput &input) {
        Lock lock(guard);
        if (!count) return false;
        input = queue[head];
        head = (head + 1) & 31;
        --count;
        return true;
    }
    void finish() {
        Lock lock(guard);
        pending = active = false;
        head = count = 0;
    }
};
