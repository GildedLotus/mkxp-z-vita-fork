// SPDX-License-Identifier: GPL-3.0-or-later
/* Static FluidFunctions implementation. Included only by fluid-fun.cpp.
 * Samples/presets are shared; all voices/channels are reserved during load. */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <new>
#include <memory>
#include <string>
#include <vector>

static const size_t midiFontLimit = 8 * 1024 * 1024;
static const size_t midiHeapLimit = 24 * 1024 * 1024;
static const int midiVoiceLimit = 64;
static size_t midiHeapUsed = 0, midiHeapPeak = 0, midiAllocations = 0;
static const char *midiFailure = "MIDI: synthesiser allocation failed (24 MiB budget)";
union MidiAllocation { size_t size; std::max_align_t alignment; };
static void *midiResize(void *memory, size_t size)
{
	MidiAllocation *old = memory ? static_cast<MidiAllocation*>(memory) - 1 : nullptr;
	size_t before = old ? old->size : 0;
	if (size > midiHeapLimit - sizeof(MidiAllocation)) return nullptr;
	size += sizeof(MidiAllocation);
	if (size > midiHeapLimit - (midiHeapUsed - before)) return nullptr;
	MidiAllocation *next = static_cast<MidiAllocation*>(std::realloc(old, size));
	if (!next) return nullptr;
	next->size = size;
	midiHeapUsed = midiHeapUsed - before + size;
	midiHeapPeak = std::max(midiHeapPeak, midiHeapUsed);
	++midiAllocations;
	return next + 1;
}
static void *midiAllocate(size_t size) { return midiResize(nullptr, size); }
static void midiFree(void *memory)
{
	if (!memory) return;
	MidiAllocation *header = static_cast<MidiAllocation*>(memory) - 1;
	midiHeapUsed -= header->size;
	std::free(header);
}
#define TSF_MALLOC midiAllocate
#define TSF_REALLOC midiResize
#define TSF_FREE midiFree
#define TSF_NO_STDIO
#define TSF_STATIC
#define TSF_IMPLEMENTATION
#include <tsf.h>

const char *midiSynthError() { return midiFailure; }

std::string findMidiSoundFont(const std::string &configured, const char *game,
                             const char *global, const char *bundle)
{
	if (configured == "off") return {};
	if (!configured.empty()) return configured;
	for (const char *folder : {game, global, bundle})
	{
		std::unique_ptr<DIR, int(*)(DIR*)> handle(opendir(folder), closedir);
		DIR *directory = handle.get();
		if (!directory) continue;
		std::string base(folder);
		if (!base.empty() && base.back() != '/') base += '/';
		std::string first;
		while (dirent *entry = readdir(directory))
		{
			std::string name(entry->d_name);
			if (name.size() < 4) continue;
			std::string ext = name.substr(name.size() - 4);
			for (char &c : ext) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
			if (ext != ".sf2" || (!first.empty() && name >= first)) continue;
			struct stat info;
			if (stat((base + name).c_str(), &info) == 0 && S_ISREG(info.st_mode))
				first = name;
		}
		if (!first.empty()) return base + first;
	}
	return {};
}

static uint32_t midiLE(const unsigned char *p, int width)
{
	uint32_t result = 0;
	for (int i = 0; i < width; ++i) result |= uint32_t(p[i]) << (i * 8);
	return result;
}
struct MidiSFTable { const unsigned char *data = nullptr; size_t count = 0; };
/* TSF assumes valid hydra indices. Validate the bounded RIFF before its loader
 * follows those indices; reject duplicate chunks and unsupported compressed SF3. */
static bool midiFontStructure(const std::vector<unsigned char> &bytes)
{
	const char *names[] = {"phdr", "pbag", "pmod", "pgen", "inst", "ibag", "imod", "igen", "shdr"};
	const int widths[] = {38, 4, 10, 4, 22, 4, 10, 4, 46};
	MidiSFTable tables[9];
	size_t samples = 0;
	if (bytes.size() < 12 || memcmp(bytes.data(), "RIFF", 4) ||
	    memcmp(bytes.data() + 8, "sfbk", 4) || midiLE(bytes.data() + 4, 4) != bytes.size() - 8) return false;
	for (size_t i = 12; i < bytes.size();)
	{
		if (bytes.size() - i < 8) return false;
		size_t length = midiLE(bytes.data() + i + 4, 4), begin = i + 8;
		if (length > bytes.size() - begin || (length & 1)) return false;
		if (memcmp(bytes.data() + i, "LIST", 4)) return false;
		else
		{
			if (length < 4) return false;
			bool pdta = !memcmp(bytes.data() + begin, "pdta", 4);
			bool sdta = !memcmp(bytes.data() + begin, "sdta", 4);
			for (size_t j = begin + 4; j < begin + length;)
			{
				if (begin + length - j < 8) return false;
				size_t n = midiLE(bytes.data() + j + 4, 4);
				if (n > begin + length - j - 8 || (n & 1)) return false;
				if (!memcmp(bytes.data() + j, "LIST", 4) || !memcmp(bytes.data() + j, "RIFF", 4)) return false;
				if (sdta && !memcmp(bytes.data() + j, "smpl", 4))
				{
					if (samples) return false;
					samples = n / 2;
				}
				if (pdta) for (int k = 0; k < 9; ++k)
					if (!memcmp(bytes.data() + j, names[k], 4))
					{
						if (tables[k].data || n % widths[k] || !n || n / widths[k] > 16384) return false;
						tables[k] = {bytes.data() + j + 8, n / widths[k]};
					}
				j += n + 8;
			}
		}
		i = begin + length;
	}
	for (const MidiSFTable &t : tables) if (!t.data) return false;
	if (!samples || tables[0].count < 2 || tables[0].count > 513 || tables[4].count < 2 || tables[8].count < 2) return false;
	auto indices = [&](int table, int offset, int target) {
		uint32_t previous = 0;
		for (size_t i = 0; i < tables[table].count; ++i)
		{
			uint32_t value = midiLE(tables[table].data + i * widths[table] + offset, 2);
			if (value < previous || value >= tables[target].count) return false;
			previous = value;
		}
		return true;
	};
	if (!indices(0, 24, 1) || !indices(4, 20, 5) || !indices(1, 0, 3) ||
	    !indices(1, 2, 2) || !indices(5, 0, 7) || !indices(5, 2, 6)) return false;
	for (int table : {3, 7}) for (size_t i = 0; i < tables[table].count; ++i)
	{
		const unsigned char *p = tables[table].data + i * 4;
		uint32_t op = midiLE(p, 2), value = midiLE(p + 2, 2);
		/* This upstream pin shifts signed coarse offsets; negative ones are undefined. */
		if ((op == 4 || op == 12 || op == 45 || op == 50) && (value & 0x8000)) return false;
		if ((op == 41 && value >= tables[4].count - 1) ||
		    (op == 53 && value >= tables[8].count - 1)) return false;
	}
	// Count expanded regions before TSF allocates or walks the cross product.
	std::vector<size_t> sampleEnds(tables[7].count + 1, 0), instrumentRegions(tables[4].count, 0);
	for (size_t i = 0; i < tables[7].count; ++i)
		sampleEnds[i + 1] = sampleEnds[i] + (midiLE(tables[7].data + i * 4, 2) == 53);
	for (size_t i = 0; i + 1 < tables[4].count; ++i)
	{
		size_t first = midiLE(tables[4].data + i * 22 + 20, 2);
		size_t last = midiLE(tables[4].data + (i + 1) * 22 + 20, 2);
		instrumentRegions[i] = sampleEnds[midiLE(tables[5].data + last * 4, 2)] -
		                       sampleEnds[midiLE(tables[5].data + first * 4, 2)];
	}
	size_t regions = 0;
	for (size_t i = 0; i < tables[3].count; ++i)
		if (midiLE(tables[3].data + i * 4, 2) == 41)
		{
			regions += instrumentRegions[midiLE(tables[3].data + i * 4 + 2, 2)];
			if (regions > 4096) return false;
		}
	for (size_t i = 0; i + 1 < tables[8].count; ++i)
	{
		const unsigned char *p = tables[8].data + i * 46;
		uint32_t start = midiLE(p + 20, 4), end = midiLE(p + 24, 4);
		if (start >= end || end >= samples || midiLE(p + 36, 4) < 1000 || midiLE(p + 36, 4) > 192000 || (midiLE(p + 44, 2) & ~7u)) return false;
	}
	return true;
}

struct _fluid_hashtable_t { tsf *font = nullptr; double gain = 1.0, rate = 44100.0; };
struct _fluid_synth_t
{
	fluid_settings_t *settings;
	tsf *font = nullptr;
	tsf_channel initial[16];
};
static fluid_settings_t *midi_new_settings() { return new (std::nothrow) fluid_settings_t; }
static void midi_delete_settings(fluid_settings_t *s) { if (s) { tsf_close(s->font); delete s; } }
static int midi_settings_setnum(fluid_settings_t *s, const char *name, double value)
{
	if (!strcmp(name, "synth.gain")) s->gain = value;
	else if (!strcmp(name, "synth.sample-rate")) s->rate = value;
	return 0;
}
static int midi_settings_setint(fluid_settings_t*, const char*, int) { return 0; }
static int midi_settings_setstr(fluid_settings_t*, const char*, const char*) { return 0; }
static fluid_synth_t *midi_new_synth(fluid_settings_t *s)
{
	auto *result = new (std::nothrow) fluid_synth_t;
	if (result) result->settings = s;
	return result;
}
static void midi_delete_synth(fluid_synth_t *s) { if (s) { tsf_close(s->font); delete s; } }
static int midi_synth_sfload(fluid_synth_t *s, const char *filename, int) try
{
	if (!s->settings->font)
	{
		FILE *file = fopen(filename, "rb");
		if (!file) { midiFailure = "MIDI: cannot open SoundFont"; return -1; }
		long size = -1;
		if (!fseek(file, 0, SEEK_END)) size = ftell(file);
		if (size <= 0 || size > long(midiFontLimit) || fseek(file, 0, SEEK_SET))
		{
			fclose(file); midiFailure = "MIDI: invalid SoundFont size (8 MiB ceiling)"; return -1;
		}
		std::vector<unsigned char> bytes;
		try { bytes.resize(size); } catch (const std::bad_alloc&) { fclose(file); return -1; }
		bool read = fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
		fclose(file);
		if (!read || !midiFontStructure(bytes))
		{ midiFailure = "MIDI: malformed or unsupported SoundFont"; return -1; }
		s->settings->font = tsf_load_memory(bytes.data(), int(bytes.size()));
		if (!s->settings->font) { midiFailure = "MIDI: SoundFont exceeds synthesis budget or is invalid"; return -1; }
		/* Bound region fan-out and validate the final generator-adjusted offsets. */
		size_t regionCount = 0;
		for (int i = 0; i < s->settings->font->presetNum; ++i)
		{
			tsf_preset &preset = s->settings->font->presets[i];
			regionCount += preset.regionNum;
			for (int j = 0; j < preset.regionNum; ++j)
			{
				tsf_region &r = preset.regions[j];
				if (regionCount > 4096 || r.offset >= r.end ||
				    (r.loop_mode && (r.loop_start < r.offset || r.loop_start >= r.loop_end || r.loop_end >= r.end)))
				{
					tsf_close(s->settings->font); s->settings->font = nullptr;
					midiFailure = "MIDI: invalid SoundFont regions or above 4096 regions"; return -1;
				}
			}
		}
	}
	s->font = tsf_copy(s->settings->font);
	if (!s->font || !tsf_set_max_voices(s->font, midiVoiceLimit) || !tsf_channel_set_presetindex(s->font, 15, 0)) return -1;
	tsf_set_output(s->font, TSF_STEREO_INTERLEAVED, int(s->settings->rate), -10.0f);
	tsf_set_volume(s->font, float(s->settings->gain) * 0.3f);
	for (int c = 0; c < 16; ++c) tsf_channel_set_presetnumber(s->font, c, 0, c == 9);
	memcpy(s->initial, s->font->channels->channels, sizeof(s->initial));
	return 0;
}
catch (const std::bad_alloc&)
{ midiFailure = "MIDI: cannot allocate SoundFont loading buffers"; return -1; }
static int midi_synth_system_reset(fluid_synth_t *s)
{
	/* tsf_reset frees channels. Restore preallocated state instead. */
	for (int i = 0; i < s->font->voiceNum; ++i) s->font->voices[i].playingPreset = -1;
	s->font->voicePlayIndex = 0;
	memcpy(s->font->channels->channels, s->initial, sizeof(s->initial));
	return 0;
}
static int midi_synth_write_s16(fluid_synth_t *s, int len, void *left, int loff, int lstep,
                               void *right, int roff, int rstep)
{
	short block[256 * 2];
	for (int i = 0; i < len;)
	{
		int n = std::min(256, len - i);
		tsf_render_short(s->font, block, n, 0);
		for (int j = 0; j < n; ++j, ++i)
		{
			static_cast<short*>(left)[loff + i * lstep] = block[j * 2];
			static_cast<short*>(right)[roff + i * rstep] = block[j * 2 + 1];
		}
	}
	return 0;
}
static int midi_synth_noteon(fluid_synth_t *s, int c, int key, int vel)
{
	if (c < 0 || c >= 16 || key < 0 || key > 127 || vel < 0 || vel > 127) return -1;
	if (vel && tsf_active_voice_count(s->font) == midiVoiceLimit)
	{
		tsf_voice *oldest = s->font->voices;
		for (int i = 1; i < midiVoiceLimit; ++i)
			if (s->font->voices[i].playIndex < oldest->playIndex) oldest = &s->font->voices[i];
		oldest->playingPreset = -1;
	}
	return tsf_channel_note_on(s->font, c, key, vel / 127.0f) ? 0 : -1;
}
static int midi_synth_noteoff(fluid_synth_t *s, int c, int key)
{ tsf_channel_note_off(s->font, c, key); return 0; }
static int midi_synth_channel_pressure(fluid_synth_t*, int, int) { return 0; }
static int midi_synth_pitch_bend(fluid_synth_t *s, int c, int value)
{ return tsf_channel_set_pitchwheel(s->font, c, value) ? 0 : -1; }
static int midi_synth_cc(fluid_synth_t *s, int c, int ctrl, int value)
{ return tsf_channel_midi_control(s->font, c, ctrl, value) ? 0 : -1; }
static int midi_synth_program_change(fluid_synth_t *s, int c, int program)
{ return tsf_channel_set_presetnumber(s->font, c, program, c == 9) ? 0 : -1; }
