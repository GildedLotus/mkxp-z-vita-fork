/*
** sharedmidistate.h
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

#ifndef SHAREDMIDISTATE_H
#define SHAREDMIDISTATE_H

#include "config.h"
#include "debugwriter.h"
#include "fluid-fun.h"
#include "exception.h"
#include <cstring>
#include <new>

#include <assert.h>
#include <vector>
#include <string>

#define SYNTH_INIT_COUNT 2
#define SYNTH_SAMPLERATE 44100

struct Synth
{
	fluid_synth_t *synth;
	bool inUse;
};

struct SharedMidiState
{
	bool inited;
	std::vector<Synth> synths;
	std::string soundFont;
	fluid_settings_t *flSettings;

	SharedMidiState(const Config &conf)
	    : inited(false),
	      soundFont(conf.midi.soundFont),
	      flSettings(nullptr)
	{}

	~SharedMidiState()
	{
		/* We might have initialized, but if the consecutive libfluidsynth
		 * load failed, no resources will have been allocated */
		if (!flSettings)
			return;

		for (size_t i = 0; i < synths.size(); ++i)
		{
			assert(!synths[i].inUse);
			fluid.delete_synth(synths[i].synth);
		}

		fluid.delete_settings(flSettings);
	}

	void initIfNeeded(const Config &conf)
	{
		if (inited)
			return;

		inited = true;
		try
		{

			initFluidFunctions();

			if (!HAVE_FLUID)
				return;

#ifdef MKXPZ_TSF
			if (soundFont == "off")
			{
				Debug() << "MIDI disabled: midiSoundFont is off";
				memset(&fluid, 0, sizeof(fluid));
				return;
			}
			soundFont = findMidiSoundFont(soundFont);
			if (soundFont.empty())
			{
				Debug() << "MIDI disabled: no user SoundFont found";
				memset(&fluid, 0, sizeof(fluid));
				return;
			}
			synths.reserve(8);
#endif
			flSettings = fluid.new_settings();
			if (!flSettings)
			{
				Debug() << "MIDI disabled: cannot allocate settings";
				memset(&fluid, 0, sizeof(fluid));
				return;
			}
			fluid.settings_setnum(flSettings, "synth.gain", 1.0f);
			fluid.settings_setnum(flSettings, "synth.sample-rate", SYNTH_SAMPLERATE);
			fluid.settings_setint(flSettings, "synth.chorus.active", conf.midi.chorus);
			fluid.settings_setint(flSettings, "synth.reverb.active", conf.midi.reverb);

			for (size_t i = 0; i < SYNTH_INIT_COUNT; ++i)
				if (!addSynth(false))
				{
#ifdef MKXPZ_TSF
					Debug() << midiSynthError();
#else
					Debug() << "MIDI disabled: cannot load synthesiser/SoundFont";
#endif
					for (Synth &entry : synths) fluid.delete_synth(entry.synth);
					synths.clear();
					fluid.delete_settings(flSettings);
					flSettings = nullptr;
					memset(&fluid, 0, sizeof(fluid));
					return;
				}
		}
		catch (const std::bad_alloc&)
		{
			for (Synth &entry : synths) fluid.delete_synth(entry.synth);
			synths.clear();
			if (flSettings) fluid.delete_settings(flSettings);
			flSettings = nullptr;
			memset(&fluid, 0, sizeof(fluid));
			Debug() << "MIDI disabled: cannot allocate initial state";
		}
	}

	fluid_synth_t *allocateSynth()
	{
		assert(HAVE_FLUID);
		assert(inited);

		size_t i;

		for (i = 0; i < synths.size(); ++i)
			if (!synths[i].inUse)
				break;

		if (i < synths.size())
		{
			fluid_synth_t *syn = synths[i].synth;
			fluid.synth_system_reset(syn);
			synths[i].inUse = true;

			return syn;
		}
		else
		{
#ifdef MKXPZ_TSF
			if (synths.size() >= 8)
				throw Exception(Exception::MKXPError, "MIDI: eight simultaneous synths exhausted");
#endif
			fluid_synth_t *syn = addSynth(true);
			if (!syn) throw Exception(Exception::MKXPError, "MIDI: cannot allocate synthesiser");
			return syn;
		}
	}

	void releaseSynth(fluid_synth_t *synth)
	{
		size_t i;

		for (i = 0; i < synths.size(); ++i)
			if (synths[i].synth == synth)
				break;

		assert(i < synths.size());

		synths[i].inUse = false;
	}

private:
	fluid_synth_t *addSynth(bool usedNow)
	{
		fluid_synth_t *syn = fluid.new_synth(flSettings);

		if (!syn) return nullptr;
		if (!soundFont.empty())
		{
			if (fluid.synth_sfload(syn, soundFont.c_str(), 1) < 0)
			{
				fluid.delete_synth(syn);
				return nullptr;
			}
		}
		else
			Debug() << "Warning: No soundfont specified, sound might be mute";

		Synth synth;
		synth.inUse = usedNow;
		synth.synth = syn;
		synths.push_back(synth);

		return syn;
	}
};

#endif // SHAREDMIDISTATE_H
