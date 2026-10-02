/*
** keybindings.h
**
** This file is part of mkxp.
**
** Copyright (C) 2014 - 2021 Amaryllis Kulla <ancurio@mapleshrine.eu>
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

#ifndef KEYBINDINGS_H
#define KEYBINDINGS_H

#include "input.h"

#include <SDL_scancode.h>
#include <SDL_gamecontroller.h>
#include <stdint.h>
#include <assert.h>
#include <vector>

enum AxisDir
{
	Negative,
	Positive
};

enum SourceType
{
	Invalid,
	Key,
    CButton,
    CAxis
};

struct SourceDesc
{
	SourceType type;

	union Data
	{
		/* Keyboard scancode */
		SDL_Scancode scan;
		/* Joystick button index */
		SDL_GameControllerButton cb;
		struct
		{
			/* Joystick axis index */
			SDL_GameControllerAxis axis;
			/* Joystick axis direction */
			AxisDir dir;
		} ca;
	} d;

	bool operator==(const SourceDesc &o) const
	{
		if (type != o.type)
			return false;

		switch (type)
		{
		case Invalid:
			return true;
		case Key:
			return d.scan == o.d.scan;
        case CButton:
            return d.cb == o.d.cb;
		case CAxis:
			return (d.ca.axis == o.d.ca.axis) && (d.ca.dir == o.d.ca.dir);
		default:
			assert(!"unreachable");
			return false;
		}
	}

	bool operator!=(const SourceDesc &o) const
	{
		return !(*this == o);
	}
};

/* Analog-stick gate, in SDL's int16 axis units.
 *
 * JAXIS_THRESHOLD_DEFAULT is stock mkxp-z's constant: half of the int16 axis
 * range. On the Vita that is NOT half of the stick's physical travel. SDL
 * 2.32.8's Vita joystick driver maps the pad's 0..255 byte to int16 through a
 * cubic Bezier whose first two control points are both (0,0)
 * (build/sdl2-src/src/joystick/vita/SDL_sysjoystick.c:79-124), so the low
 * half of the curve eases in and runs well under the straight line the
 * comment there claims ("use a linear curve"). Recomputed from that source,
 * the first raw byte whose mapped value clears 0x4000 is 192, i.e.
 * (192-128)/127 = 50.4 percent of travel -- which leaves both axes above the
 * gate only inside a ~30 degree window, so a diagonal on the left stick is
 * nearly unreachable and 8-way movement does not work.
 *
 * Config::controllerDeadzone (src/config.cpp, Vita only) names that fraction
 * and is resolved exactly once, at config-read time, into the value
 * jAxisThreshold() returns. BOTH places that gate on an axis read it through
 * the JAXIS_THRESHOLD spelling below -- CtrlAxisBinding::sourceActive() in
 * src/input/input.cpp (which caches it per binding, see there) and the
 * binding capture in vita/overlay/settings_menu.h -- so a binding captured in the
 * settings menu is captured at exactly the threshold the runtime applies.
 *
 * Off-device nothing calls setJAxisThreshold() and the value stays
 * JAXIS_THRESHOLD_DEFAULT: stock behaviour, bit for bit.
 */
#define JAXIS_THRESHOLD_DEFAULT 0x4000

/* Range Config::read clamps controllerDeadzone to. 0 would leave every axis
 * permanently active; 1.0 is unreachable, because the Bezier only returns
 * SDL_JOYSTICK_AXIS_MAX at full deflection and the test is strict. */
#define JAXIS_DEADZONE_MIN 0.05
#define JAXIS_DEADZONE_MAX 0.95

/* The one resolved threshold for the process. Constant-initialised, so it
 * costs no guard variable and already reads correctly before Config::read
 * runs (the RGSS thread builds its bindings after that, but the settings
 * menu does not have to care about the order). */
inline int16_t &jAxisThresholdRef()
{
	static int16_t resolved = JAXIS_THRESHOLD_DEFAULT;
	return resolved;
}

inline int16_t jAxisThreshold()
{
	return jAxisThresholdRef();
}

inline void setJAxisThreshold(double deadzone)
{
	/* Config::read clamps before calling; clamp again so no other caller can
	 * park the gate outside the usable band. Multiply only -- resolving this
	 * must never need a divide (Cortex-A9 has no hardware integer divide),
	 * and it runs once either way. */
	if (deadzone < JAXIS_DEADZONE_MIN)
		deadzone = JAXIS_DEADZONE_MIN;
	if (deadzone > JAXIS_DEADZONE_MAX)
		deadzone = JAXIS_DEADZONE_MAX;

	jAxisThresholdRef() =
		(int16_t)(deadzone * (double)SDL_JOYSTICK_AXIS_MAX + 0.5);
}

/* Every existing comparison keeps this spelling and now reads the resolved
 * value. Deadzone 0.5 resolves to exactly JAXIS_THRESHOLD_DEFAULT. */
#define JAXIS_THRESHOLD (jAxisThreshold())

struct BindingDesc
{
	SourceDesc src;
	Input::ButtonCode target;
};

typedef std::vector<BindingDesc> BDescVec;
struct Config;

BDescVec genDefaultBindings(const Config &conf);

void storeBindings(const BDescVec &d, const Config &conf);
BDescVec loadBindings(const Config &conf);

#endif // KEYBINDINGS_H
