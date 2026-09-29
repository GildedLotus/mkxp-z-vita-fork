// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "settings_input.h"
#include "keybindings.h"
#include "config.h"
#include <SDL_keyboard.h>
#include <cstdio>
#include <exception>

/* Text/state only. The renderer supplies the retained scene and reserved HUD. */
class VitaSettingsMenu {
    BDescVec navigation, draft;
    VitaSettingsInput previous;
    const int threshold = JAXIS_THRESHOLD;
    bool actionAxes[SDL_CONTROLLER_AXIS_MAX][2] = {};
    bool previousActionAxes[SDL_CONTROLLER_AXIS_MAX][2] = {};
    int slots[12][4];
    int row = 0, slot = 0;
    bool capturing = false, armed = false, closed = false, save = false;
    const char *warning = "";
    char text[12][63] = {};
    const char *lines[12];

    static Input::ButtonCode code(int i) {
        static const Input::ButtonCode codes[] = {
            Input::Up, Input::Down, Input::L, Input::Left, Input::Right, Input::R,
            Input::A, Input::B, Input::C, Input::X, Input::Y, Input::Z
        };
        return codes[i];
    }
    static const char *name(int i) {
        static const char *names[] = {"Up", "Down", "L", "Left", "Right", "R",
            "A", "B", "C", "X", "Y", "Z", "Reset defaults", "Cancel", "Accept"};
        return names[i];
    }
    static bool reserved(const SourceDesc &s) {
        return s.type == CButton && (s.d.cb == SDL_CONTROLLER_BUTTON_START ||
                                    s.d.cb == SDL_CONTROLLER_BUTTON_BACK);
    }
    static bool actionAxis(Input::ButtonCode target) {
        return target != Input::None && target != Input::Up && target != Input::Down &&
               target != Input::Left && target != Input::Right;
    }
    void updateAxes(const VitaSettingsInput &input) {
        std::memcpy(previousActionAxes, actionAxes, sizeof(actionAxes));
        for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i)
            for (int d = 0; d < 2; ++d)
                actionAxes[i][d] = input.axisHeld(i, d != 0, true, threshold, actionAxes[i][d]);
    }
    bool held(const SourceDesc &s, Input::ButtonCode target,
              const VitaSettingsInput &input, bool old) const {
        if (s.type == Key)
            return s.d.scan >= 0 && s.d.scan < SDL_NUM_SCANCODES && input.keys[s.d.scan];
        if (s.type == CButton)
            return s.d.cb >= 0 && s.d.cb < SDL_CONTROLLER_BUTTON_MAX &&
                   !reserved(s) && input.buttons[s.d.cb];
        if (s.type == CAxis && s.d.ca.axis >= 0 && s.d.ca.axis < SDL_CONTROLLER_AXIS_MAX) {
            const int dir = s.d.ca.dir == Negative;
            if (actionAxis(target))
                return old ? previousActionAxes[s.d.ca.axis][dir] : actionAxes[s.d.ca.axis][dir];
            return input.axisHeld(s.d.ca.axis, dir != 0, false, threshold);
        }
        return false;
    }
    bool mapped(Input::ButtonCode target, const VitaSettingsInput &input, bool old) const {
        for (const auto &d : navigation)
            if (d.target == target && held(d.src, target, input, old)) return true;
        return false;
    }
    bool edge(Input::ButtonCode target, const VitaSettingsInput &input) const {
        return mapped(target, input, false) && !mapped(target, previous, true);
    }
    bool button(int b, const VitaSettingsInput &input) const {
        return input.buttons[b] && !previous.buttons[b];
    }
    void indexSlots() {
        for (auto &r : slots) for (int &s : r) s = -1;
        for (size_t i = 0; i < draft.size(); ++i) {
            if (reserved(draft[i].src)) draft[i].src = SourceDesc{};
            for (int r = 0; r < 12; ++r) if (draft[i].target == code(r)) {
                for (int &s : slots[r]) if (s < 0) { s = int(i); break; }
            }
        }
    }
    SourceDesc source(int r, int s) const {
        return slots[r][s] < 0 ? SourceDesc{} : draft[slots[r][s]].src;
    }
    void replace(SourceDesc src) {
        int &i = slots[row][slot];
        if (i < 0) { i = int(draft.size()); draft.push_back({src, code(row)}); }
        else draft[i].src = src;
        warning = "";
    }
    bool duplicate() const {
        for (size_t i = 0; i < draft.size(); ++i)
            for (size_t j = i + 1; j < draft.size(); ++j)
                if (draft[i].src.type != Invalid && draft[i].target != draft[j].target &&
                    draft[i].src == draft[j].src) return true;
        return false;
    }
    static void describe(const SourceDesc &src, char *out, size_t n) {
        static const char *buttons[] = {"Cross", "Circle", "Square", "Triangle",
            "Select", "Guide", "Start", "L3", "R3", "L", "R", "DUp", "DDown", "DLeft", "DRight"};
        static const char *axes[] = {"LX", "LY", "RX", "RY", "LT", "RT"};
        if (src.type == Key) {
            const char *key = SDL_GetScancodeName(src.d.scan);
            if (std::strlen(key) < n) std::snprintf(out, n, "%s", key);
            else std::snprintf(out, n, "Key %d", int(src.d.scan));
        }
        else if (src.type == CButton && src.d.cb >= 0 && src.d.cb < 15)
            std::snprintf(out, n, "%s", std::strlen(buttons[src.d.cb]) < n ? buttons[src.d.cb] : "Tri.");
        else if (src.type == CAxis && src.d.ca.axis >= 0 && src.d.ca.axis < 6)
            std::snprintf(out, n, "%s%c", axes[src.d.ca.axis], src.d.ca.dir == Negative ? '-' : '+');
        else std::snprintf(out, n, "--");
    }
    void capture(const VitaSettingsInput &input) {
        if (button(SDL_CONTROLLER_BUTTON_START, input) || button(SDL_CONTROLLER_BUTTON_BACK, input)) {
            warning = "Start / Select are reserved. Choose another input.";
            return;
        }
        if (button(SDL_CONTROLLER_BUTTON_B, input) ||
            (input.keys[SDL_SCANCODE_ESCAPE] && !previous.keys[SDL_SCANCODE_ESCAPE])) {
            capturing = false; warning = "Capture cancelled"; return;
        }
        if (!armed) {
            armed = input.neutral(threshold, actionAxis(code(row)));
            return;
        }
        SourceDesc src{};
        for (int i = 0; i < SDL_NUM_SCANCODES; ++i) if (input.keys[i] && !previous.keys[i]) {
            src.type = Key; src.d.scan = SDL_Scancode(i);
            if (src.d.scan == SDL_SCANCODE_RSHIFT) src.d.scan = SDL_SCANCODE_LSHIFT;
            if (src.d.scan == SDL_SCANCODE_KP_ENTER) src.d.scan = SDL_SCANCODE_RETURN;
            break;
        }
        for (int i = 0; src.type == Invalid && i < SDL_CONTROLLER_BUTTON_MAX; ++i)
            if (button(i, input)) { src.type = CButton; src.d.cb = SDL_GameControllerButton(i); }
        for (int i = 0; src.type == Invalid && i < SDL_CONTROLLER_AXIS_MAX; ++i) {
            for (int d = 0; d < 2 && src.type == Invalid; ++d) {
                const bool action = actionAxis(code(row));
                const bool active = action ? actionAxes[i][d] : input.axisHeld(i, d != 0, false, threshold);
                const bool wasActive = action ? previousActionAxes[i][d] : previous.axisHeld(i, d != 0, false, threshold);
                if (active && !wasActive) {
                    src.type = CAxis; src.d.ca.axis = SDL_GameControllerAxis(i);
                    src.d.ca.dir = d ? Negative : Positive;
                }
            }
        }
        if (src.type != Invalid && !reserved(src)) { replace(src); capturing = false; }
    }
public:
    VitaSettingsMenu(const BDescVec &bindings, const VitaSettingsInput &initial)
        : navigation(bindings), draft(bindings), previous(initial) { indexSlots(); updateAxes(initial); }

    void sample(const VitaSettingsInput &input, const Config &config) {
        updateAxes(input);
        if (input.acceptRequest) { requestAccept(); return; }
        if (closed || save) { previous = input; return; }
        if (capturing) { capture(input); previous = input; return; }
        // Physical controls take precedence when a remap conflicts with recovery navigation.
        int action = -1;
        const int raw[] = {SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
            SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
            SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_Y};
        const Input::ButtonCode mappedCodes[] = {Input::Up, Input::Down, Input::Left,
            Input::Right, Input::C, Input::B, Input::X};
        for (int i = 0; i < 7 && action < 0; ++i) if (button(raw[i], input)) action = i;
        for (int i = 0; i < 7 && action < 0; ++i) if (edge(mappedCodes[i], input)) action = i;
        if (action == 0) row = row == 0 ? 14 : row - 1;
        if (action == 1) row = row == 14 ? 0 : row + 1;
        if (action == 2) slot = (slot + 3) & 3;
        if (action == 3) slot = (slot + 1) & 3;
        if (action == 4) {
            if (row < 12) { capturing = true; armed = false; warning = "Release, then press the new input. Circle cancels."; }
            else if (row == 12) { draft = genDefaultBindings(config); indexSlots(); warning = "Defaults restored; Accept to save."; }
            else if (row == 13) closed = true;
            else save = true;
        }
        if (action == 5) closed = true;
        if (action == 6 && row < 12) replace(SourceDesc{});
        previous = input;
    }
    void requestAccept() {
        if (capturing) warning = "Start / Select are reserved. Choose another input.";
        else if (!closed) save = true;
    }
    bool done() const { return closed; }
    bool wantsSave() const { return save; }
    bool awaitingInput() const { return capturing; }
    BDescVec bindings() const {
        BDescVec out;
        for (const auto &d : draft) if (!reserved(d.src)) out.push_back(d);
        return out;
    }
    bool persist(const Config &config) {
        save = false;
        if (config.customDataPath.empty()) { warning = "Save failed: no binding folder. Retry or Cancel."; return false; }
        try { storeBindings(bindings(), config); }
        catch (const std::exception &) { warning = "Save failed. Previous bindings kept. Retry or Cancel."; return false; }
        closed = true;
        return true;
    }
    const char *const *render() {
        for (int i = 0; i < 12; ++i) { text[i][0] = 0; lines[i] = text[i]; }
        std::snprintf(text[0], 63, "SETTINGS - GAME PAUSED     %s", capturing ? "CHOOSE INPUT" : "Bindings");
        const int top = row < 3 ? 0 : (row > 11 ? 8 : row - 3);
        for (int i = 0; i < 7; ++i) {
            int r = top + i;
            if (r >= 12) std::snprintf(text[i + 1], 63, "%c %s", r == row ? '>' : ' ', name(r));
            else {
                char fields[4][14];
                for (int s = 0; s < 4; ++s) {
                    // The last column has nine cells before the right gutter, including brackets.
                    char value[12]; describe(source(r, s), value, s == 3 ? 8 : sizeof(value));
                    std::snprintf(fields[s], sizeof(fields[s]), r == row && s == slot ? "[%s]" : " %s ", value);
                }
                std::snprintf(text[i + 1], 63, "%c %-5s %-13s%-13s%-13s%-13s", r == row ? '>' : ' ', name(r), fields[0], fields[1], fields[2], fields[3]);
            }
        }
        std::snprintf(text[8], 63, "%s", warning);
        if (!warning[0] && row < 12 && source(row, slot).type == Key)
            std::snprintf(text[8], 63, "Key: %s", SDL_GetScancodeName(source(row, slot).d.scan));
        std::snprintf(text[9], 63, "%s", duplicate() ? "Warning: one input has multiple actions." : "");
        std::snprintf(text[10], 63, "D-pad: row/slot  Cross: bind/action  Triangle: clear");
        std::snprintf(text[11], 63, "Circle: cancel  Start: accept  Start+Select: quit");
        for (auto &line : text) {
            size_t n = std::strlen(line);
            while (n && line[n - 1] == ' ') line[--n] = 0;
        }
        return lines;
    }
};
