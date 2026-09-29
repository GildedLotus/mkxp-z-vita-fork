// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MIDI_SE_H
#define MIDI_SE_H
#include <cstdint>
#include <vector>
struct SDL_RWops;
// Closes ops on success and failure; renders only on the loading thread.
void renderMidiSE(SDL_RWops &ops, std::vector<int16_t> &pcm);
#endif
