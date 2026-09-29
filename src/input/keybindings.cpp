/*
** keybindings.cpp
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

#include "keybindings.h"

#include "config.h"
#include "util.h"
#include "vita_publish.h"

#include <stdio.h>

struct KbBindingData
{
	SDL_Scancode source;
	Input::ButtonCode target;

	void add(BDescVec &d) const
	{
		SourceDesc src;
		src.type = Key;
		src.d.scan = source;

		BindingDesc desc;
		desc.src = src;
		desc.target = target;

		d.push_back(desc);
	}
};

struct CtrlBindingData
{
    SDL_GameControllerButton source;
    Input::ButtonCode target;
    
    void add(BDescVec &d) const
    {
        SourceDesc src;
        src.type = CButton;
        src.d.cb = source;
        
        BindingDesc desc;
        desc.src = src;
        desc.target = target;
        
        d.push_back(desc);
    }
};

/* Common */
static const KbBindingData defaultKbBindings[] =
{
	{ SDL_SCANCODE_LEFT,   Input::Left  },
	{ SDL_SCANCODE_RIGHT,  Input::Right },
	{ SDL_SCANCODE_UP,     Input::Up    },
	{ SDL_SCANCODE_DOWN,   Input::Down  },
    
	{ SDL_SCANCODE_SPACE,  Input::C     },
	{ SDL_SCANCODE_RETURN, Input::C     },
	{ SDL_SCANCODE_ESCAPE, Input::B     },
	{ SDL_SCANCODE_KP_0,   Input::B     },
	{ SDL_SCANCODE_LSHIFT, Input::A     },
	{ SDL_SCANCODE_X,      Input::B     },
	{ SDL_SCANCODE_D,      Input::Z     },
	{ SDL_SCANCODE_Q,      Input::L     },
	{ SDL_SCANCODE_W,      Input::R     },
	{ SDL_SCANCODE_A,      Input::X     },
	{ SDL_SCANCODE_S,      Input::Y     }
};

/* RGSS1 */
static const KbBindingData defaultKbBindings1[] =
{
	{ SDL_SCANCODE_Z,      Input::A     },
	{ SDL_SCANCODE_C,      Input::C     },
};

/* RGSS2 and higher */
static const KbBindingData defaultKbBindings2[] =
{
	{ SDL_SCANCODE_Z,      Input::C     }
};

static elementsN(defaultKbBindings);
static elementsN(defaultKbBindings1);
static elementsN(defaultKbBindings2);

static const CtrlBindingData defaultCtrlBindings[] =
{
	{ SDL_CONTROLLER_BUTTON_X, Input::A  },
	{ SDL_CONTROLLER_BUTTON_B, Input::B  },
	{ SDL_CONTROLLER_BUTTON_A, Input::C },
	{ SDL_CONTROLLER_BUTTON_Y, Input::X  },
#if !(defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC))
	{ SDL_CONTROLLER_BUTTON_LEFTSTICK, Input::Y  },
	{ SDL_CONTROLLER_BUTTON_RIGHTSTICK, Input::Z },
#endif
	{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER, Input::L  },
	{ SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, Input::R  },
    
    { SDL_CONTROLLER_BUTTON_DPAD_UP, Input::Up },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN, Input::Down },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, Input::Left },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, Input::Right }
};

static elementsN(defaultCtrlBindings);

static void addAxisBinding(BDescVec &d, SDL_GameControllerAxis axis, AxisDir dir, Input::ButtonCode target)
{
	SourceDesc src;
	src.type = CAxis;
	src.d.ca.axis = axis;
	src.d.ca.dir = dir;

	BindingDesc desc;
	desc.src = src;
	desc.target = target;

	d.push_back(desc);
}

BDescVec genDefaultBindings(const Config &conf)
{
	BDescVec d;

	for (size_t i = 0; i < defaultKbBindingsN; ++i)
		defaultKbBindings[i].add(d);

	if (conf.rgssVersion == 1)
		for (size_t i = 0; i < defaultKbBindings1N; ++i)
			defaultKbBindings1[i].add(d);
	else
		for (size_t i = 0; i < defaultKbBindings2N; ++i)
			defaultKbBindings2[i].add(d);

	for (size_t i = 0; i < defaultCtrlBindingsN; ++i)
		defaultCtrlBindings[i].add(d);

	addAxisBinding(d, SDL_CONTROLLER_AXIS_LEFTX, Negative, Input::Left );
	addAxisBinding(d, SDL_CONTROLLER_AXIS_LEFTX, Positive, Input::Right);
	addAxisBinding(d, SDL_CONTROLLER_AXIS_LEFTY, Negative, Input::Up   );
	addAxisBinding(d, SDL_CONTROLLER_AXIS_LEFTY, Positive, Input::Down );

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* The handheld has no stick clicks. Keep movement and system buttons free. */
	addAxisBinding(d, SDL_CONTROLLER_AXIS_RIGHTX, Negative, Input::Y);
	addAxisBinding(d, SDL_CONTROLLER_AXIS_RIGHTX, Positive, Input::Z);
#endif

	return d;
}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include <unistd.h>
#include <stdexcept>
#include <string.h>
#endif

#include <errno.h>

#define FORMAT_VER 3

struct Header
{
    uint32_t formVer;
    uint32_t rgssVer;
    uint32_t count;
};

static void buildPath(const std::string &dir, uint32_t rgssVersion,
                      char *out, size_t outSize)
{
    snprintf(out, outSize, "%skeybindings.mkxp%u", dir.c_str(), rgssVersion);
}

static bool verifyDesc(const BindingDesc &desc)
{
    const Input::ButtonCode codes[] = {
        Input::None, Input::Down, Input::Left, Input::Right, Input::Up,
        Input::A, Input::B, Input::C, Input::X, Input::Y, Input::Z,
        Input::L, Input::R, Input::Shift, Input::Ctrl, Input::Alt,
        Input::F5, Input::F6, Input::F7, Input::F8, Input::F9
    };
    bool target = false;
    for (auto code : codes) if (desc.target == code) target = true;
    if (!target) return false;
    const SourceDesc &src = desc.src;
    switch (src.type) {
    case Invalid: return true;
    case Key: return src.d.scan >= 0 && src.d.scan < SDL_NUM_SCANCODES;
    case CButton: return src.d.cb >= 0 && src.d.cb < SDL_CONTROLLER_BUTTON_MAX;
    case CAxis:
        return src.d.ca.axis >= 0 && src.d.ca.axis < SDL_CONTROLLER_AXIS_MAX &&
               (src.d.ca.dir == Negative || src.d.ca.dir == Positive);
    default: return false;
    }
}

static bool readBindingFile(BDescVec &out, const std::string &path, uint32_t version,
                            bool *ioFailure = nullptr)
{
    if (ioFailure) *ioFailure = false;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        if (ioFailure) *ioFailure = errno != ENOENT;
        return false;
    }
    Header hd{};
    bool ok = fread(&hd, sizeof(hd), 1, f) == 1 &&
              hd.rgssVer == version && hd.count <= 1024;
    if (hd.formVer != FORMAT_VER) ok = false;
    BDescVec candidate;
    try {
        if (ok) {
            candidate.resize(hd.count);
            ok = !hd.count || fread(candidate.data(), sizeof(BindingDesc), hd.count, f) == hd.count;
            for (const auto &d : candidate) if (!verifyDesc(d)) ok = false;
            if (fgetc(f) != EOF || ferror(f)) ok = false;
        }
    } catch (...) { fclose(f); throw; }
    bool ioError = ferror(f) != 0;
    if (fclose(f) != 0) ioError = true;
    if (ioFailure) *ioFailure = ioError;
    if (ioError) ok = false;
    if (ok) out.swap(candidate);
    return ok;
}

static bool bindingFileMatches(const BDescVec &expected, const std::string &path, uint32_t version)
{
    BDescVec actual;
    if (!readBindingFile(actual, path, version) || actual.size() != expected.size()) return false;
    for (size_t i = 0; i < expected.size(); ++i)
        if (actual[i].target != expected[i].target || actual[i].src != expected[i].src) return false;
    return true;
}

static bool writeBindings(const BDescVec &d, const std::string &dir, uint32_t rgssVersion)
{
    if (dir.empty() || dir.size() > 960 || d.size() > 1024) return false;
    for (const auto &desc : d) {
        if (!verifyDesc(desc)) return false;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
        if (desc.src.type == CButton && (desc.src.d.cb == SDL_CONTROLLER_BUTTON_START ||
                                        desc.src.d.cb == SDL_CONTROLLER_BUTTON_BACK)) return false;
#endif
    }
    char path[1024];
    buildPath(dir, rgssVersion, path, sizeof(path));
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    char temp[sizeof(path) + 8], backup[sizeof(path) + 8], corrupt[sizeof(path) + 9];
    if (vita_publish_tmp_path(temp, sizeof(temp), path) != 0 ||
        vita_publish_bak_path(backup, sizeof(backup), path) != 0)
        return false;
    FILE *f = fopen(temp, "wb");
#else
    FILE *f = fopen(path, "wb");
#endif
    if (!f) return false;
    Header hd{FORMAT_VER, rgssVersion, static_cast<uint32_t>(d.size())};
    bool ok = fwrite(&hd, sizeof(hd), 1, f) == 1;
    if (ok && hd.count) ok = fwrite(d.data(), sizeof(BindingDesc), hd.count, f) == hd.count;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    // fflush + fsync + fclose, each checked; the stream is closed either way.
    if (vita_publish_finalize(f) != 0) ok = false;
    if (!ok || !bindingFileMatches(d, temp, rgssVersion)) return false;
    BDescVec old;
    bool readError = false;
    bool validOld = readBindingFile(old, path, rgssVersion, &readError);
    if (readError) return false;
    if (validOld) {
        // Publication policy lives in vita_publish: the
        // valid previous generation moves aside to .bak before .tmp publishes.
        if (vita_publish_commit(temp, path, backup) != 0) return false;
        return bindingFileMatches(d, path, rgssVersion);
    }
    // Do not rotate an invalid current file over a recoverable backup.
    FILE *bad = fopen(path, "rb");
    if (bad) {
        if (fclose(bad) != 0 ||
            snprintf(corrupt, sizeof(corrupt), "%s.corrupt", path) >= (int)sizeof(corrupt))
            return false;
        if (rename(path, corrupt) != 0)
            return false;
    } else if (errno != ENOENT) return false;
    if (rename(temp, path) != 0) return false;
    return bindingFileMatches(d, path, rgssVersion);
#else
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    return ok;
#endif
}

void storeBindings(const BDescVec &d, const Config &conf)
{
    if (writeBindings(d, conf.customDataPath, conf.rgssVersion)) return;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    // No path is an explicitly disabled store (e.g. a diagnostic customScript).
    if (!conf.customDataPath.empty()) throw std::runtime_error("Cannot persist controller bindings");
#endif
    fprintf(stderr, "Cannot persist controller bindings: no writable binding file\n");
}

static bool readBindings(BDescVec &out, const std::string &dir, uint32_t rgssVersion)
{
    if (dir.empty() || dir.size() > 960) return false;
    char path[1024];
    buildPath(dir, rgssVersion, path, sizeof(path));
    if (readBindingFile(out, path, rgssVersion)) return true;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    // Recovery never consumes the backup, including when a later write fails.
    return readBindingFile(out, std::string(path) + ".bak", rgssVersion);
#else
    return false;
#endif
}

BDescVec loadBindings(const Config &conf)
{
    BDescVec d;
    if (readBindings(d, conf.customDataPath, conf.rgssVersion)) return d;
    return genDefaultBindings(conf);
}
