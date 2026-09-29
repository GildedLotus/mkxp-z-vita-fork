/*
** soundemitter.cpp
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

#include "soundemitter.h"

#include "sharedstate.h"
#include "filesystem.h"
#include "exception.h"
#include "config.h"
#include "util.h"
#include "debugwriter.h"
#include "sharedmidistate.h"
#include "midi-se.h"

#include <SDL_sound.h>

#define SE_CACHE_MEM (10*1024*1024) // 10 MB
/* Whole-effect decode ceiling in decoded PCM bytes. The MIDI SE path keeps
 * its own 4 MiB ceiling (midisource.cpp); this one bounds the SDL_sound
 * path, which used Sound_DecodeAll and therefore allocated the entire
 * decoded effect -- tens of MiB for a minutes-long clip, while the rest of
 * the cache stayed resident -- before the cache limit could reject it.
 * An effect that cannot fit the whole cache is refused at this bound. */
#define SE_DECODE_BUDGET (10*1024*1024) // 10 MB
/* Encoded-source size at or under which tryRead() slurps the file into one
 * heap buffer before decoding; larger sources keep the streamed file path. */
#define SE_SOURCE_SLURP_MAX (16*1024*1024) // 16 MB

struct SoundBuffer
{
	/* Uniquely identifies this or equal buffer */
	std::string key;

	AL::Buffer::ID alBuffer;

	/* Link into the buffer cache priority list */
	IntruListLink<SoundBuffer> link;

	/* Buffer byte count */
	uint32_t bytes;

	/* Reference count */
	uint8_t refCount;

	SoundBuffer()
	    : link(this),
	      refCount(1)

	{
		alBuffer = AL::Buffer::gen();
	}

	static SoundBuffer *ref(SoundBuffer *buffer)
	{
		++buffer->refCount;

		return buffer;
	}

	static void deref(SoundBuffer *buffer)
	{
		if (--buffer->refCount == 0)
			delete buffer;
	}

private:
	~SoundBuffer()
	{
		AL::Buffer::del(alBuffer);
	}
};

/* Before: [a][b][c][d], After (index=1): [a][c][d][b] */
static void
arrayPushBack(std::vector<size_t> &array, size_t size, size_t index)
{
	size_t v = array[index];

	for (size_t t = index; t < size-1; ++t)
		array[t] = array[t+1];

	array[size-1] = v;
}

SoundEmitter::SoundEmitter(const Config &conf)
    : bufferBytes(0),
      srcCount(conf.SE.sourceCount),
      alSrcs(srcCount),
      atchBufs(srcCount),
      srcPrio(srcCount)
{
	for (size_t i = 0; i < srcCount; ++i)
	{
		alSrcs[i] = AL::Source::gen();
		atchBufs[i] = 0;
		srcPrio[i] = i;
	}
}

SoundEmitter::~SoundEmitter()
{
	for (size_t i = 0; i < srcCount; ++i)
	{
		AL::Source::stop(alSrcs[i]);
		AL::Source::del(alSrcs[i]);

		if (atchBufs[i])
			SoundBuffer::deref(atchBufs[i]);
	}

	BufferHash::const_iterator iter;
	for (iter = bufferHash.cbegin(); iter != bufferHash.cend(); ++iter)
		SoundBuffer::deref(iter->second);
}

void SoundEmitter::play(const std::string &filename,
                        int volume,
                        int pitch)
{
	float _volume = clamp<int>(volume, 0, 100) / 100.0f;
	float _pitch  = clamp<int>(pitch, 50, 150) / 100.0f;

	SoundBuffer *buffer = allocateBuffer(filename);

	if (!buffer)
		return;

	/* Try to find first free source */
	size_t i;
	for (i = 0; i < srcCount; ++i)
		if (AL::Source::getState(alSrcs[srcPrio[i]]) != AL_PLAYING)
			break;

	/* If we didn't find any, try to find the lowest priority source
	 * with the same buffer to overtake */
	if (i == srcCount)
		for (size_t j = 0; j < srcCount; ++j)
			if (atchBufs[srcPrio[j]] == buffer)
				i = j;

	/* If we didn't find any, overtake the one with lowest priority */
	if (i == srcCount)
		i = 0;

	size_t srcIndex = srcPrio[i];

	/* Only detach/reattach if it's actually a different buffer */
	bool switchBuffer = (atchBufs[srcIndex] != buffer);

	/* Push the used source to the back of the priority list */
	arrayPushBack(srcPrio, srcCount, i);

	AL::Source::ID src = alSrcs[srcIndex];
	AL::Source::stop(src);

	if (switchBuffer)
		AL::Source::detachBuffer(src);

	SoundBuffer *old = atchBufs[srcIndex];

	if (old)
		SoundBuffer::deref(old);

	atchBufs[srcIndex] = SoundBuffer::ref(buffer);

	if (switchBuffer)
		AL::Source::attachBuffer(src, buffer->alBuffer);

	AL::Source::setVolume(src, _volume);
	AL::Source::setPitch(src, _pitch);

	AL::Source::play(src);
}

void SoundEmitter::stop()
{
	for (size_t i = 0; i < srcCount; i++)
		AL::Source::stop(alSrcs[i]);
}

static int SDL_RWopsCloseNoop(SDL_RWops *ops) {
	return 0;
}

struct SoundOpenHandler : FileSystem::OpenHandler
{
	SoundBuffer *buffer;
	bool midiSilent = false;
	/* The decoded effect exceeds SE_DECODE_BUDGET: the same every time. */
	bool overBudget = false;
	std::string errorMsg;

	SoundOpenHandler()
	    : buffer(0)
	{}

	bool tryRead(SDL_RWops &ops, const char *ext)
	{
		char signature[4];
		bool midi = SDL_RWread(&ops, signature, 1, 4) == 4 && !memcmp(signature, "MThd", 4);
		SDL_RWseek(&ops, 0, RW_SEEK_SET);
		if (midi)
		{
			shState->midiState().initIfNeeded(shState->config());
			if (!HAVE_FLUID)
			{
				SDL_RWclose(&ops);
				midiSilent = true;
				return true;
			}
			try
			{
				std::vector<int16_t> pcm;
				renderMidiSE(ops, pcm);
				buffer = new SoundBuffer;
				buffer->bytes = pcm.size() * sizeof(int16_t);
				AL::Buffer::uploadData(buffer->alBuffer, AL_FORMAT_STEREO16,
				                       pcm.data(), buffer->bytes, SYNTH_SAMPLERATE);
				return true;
			}
			catch (const Exception &e) { errorMsg = e.msg; return false; }
		}

		/* Read the encoded source into one bounded heap buffer and decode
		 * from memory: decoding straight from the file ops costs one card
		 * access per STREAM_BUF_SIZE read, thousands per effect, which
		 * stalled the menu close that first plays it. A source over
		 * SE_SOURCE_SLURP_MAX, or any failed slurp step, leaves src at the
		 * file ops, rewound, and the path below unchanged. The buffer
		 * outlives the sample reading it and is freed on every exit. */
		uint8_t *blob = 0;
		SDL_RWops *src = &ops;

		const Sint64 srcBytes = SDL_RWsize(&ops);
		if (srcBytes > 0 && srcBytes <= SE_SOURCE_SLURP_MAX)
		{
			uint8_t *data = static_cast<uint8_t*>(SDL_malloc(static_cast<size_t>(srcBytes)));

			if (data && SDL_RWread(&ops, data, 1, static_cast<size_t>(srcBytes)) == static_cast<size_t>(srcBytes))
			{
				SDL_RWops *mem = SDL_RWFromConstMem(data, static_cast<size_t>(srcBytes));

				if (mem)
				{
					/* The file source is spent; this handler owns closing it. */
					SDL_RWclose(&ops);
					blob = data;
					src = mem;
				}
			}

			if (!blob)
			{
				SDL_free(data);
				SDL_RWseek(&ops, 0, RW_SEEK_SET);
			}
		}

		/* A copy of the source ops with a no-op close function,
		 * so we can reuse it if we need to change the format. */
		SDL_RWops unclosableOps = *src;
		unclosableOps.close = SDL_RWopsCloseNoop;

		/* Every exit, a throw from pcm.insert() or new SoundBuffer included,
		 * frees the sample before unclosableOps goes out of scope (SDL_sound
		 * keeps it registered until then), then the source ops -- file or
		 * memory, owned by this handler either way -- and the slurp buffer
		 */
		struct Decode
		{
			Sound_Sample *sample;
			SDL_RWops *src;
			uint8_t *blob;
			~Decode()
			{
				if (sample)
					Sound_FreeSample(sample);
				SDL_RWclose(src);
				SDL_free(blob);
			}
		} decode = { 0, src, blob };

		Sound_Sample *&sample = decode.sample;
		sample = Sound_NewSample(&unclosableOps, ext, 0, STREAM_BUF_SIZE);

		if (!sample)
			return false;

		bool validFormat = true;

		switch (sample->actual.format)
		{
			// OpenAL Soft doesn't support S32 formats.
			// https://github.com/kcat/openal-soft/issues/934
			case AUDIO_S32LSB :
			case AUDIO_S32MSB :
				validFormat = false;
		}

		if (!validFormat)
		{
			/* Same retry SDLSoundSource does for the streaming path
			 * (sdlsoundsource.cpp): free the sample, rewind the ops the
			 * no-op close left open, and decode as float32 instead. Without
			 * it formatSampleSize() reports 4 bytes, chooseALFormat() maps
			 * that to AL_FORMAT_*_FLOAT32, and integer PCM reaches the
			 * device labelled as floats. */
			Sound_FreeSample(sample);
			sample = 0;
			SDL_RWseek(&unclosableOps, 0, RW_SEEK_SET);

			Sound_AudioInfo desired;
			SDL_memset(&desired, '\0', sizeof (Sound_AudioInfo));
			desired.format = AUDIO_F32SYS;

			sample = Sound_NewSample(&unclosableOps, ext, &desired, STREAM_BUF_SIZE);

			if (!sample)
				return false;
		}

		/* Decode incrementally under SE_DECODE_BUDGET instead of
		 * Sound_DecodeAll, which grows its scratch with one SDL_realloc
		 * per chunk and can only be rejected by the cache limit below
		 * after the whole effect has been allocated. Sound_Decode hands
		 * over one bufferful at a time, overwriting sample->buffer, so
		 * keep appending until the decoder reports EOF or an error;
		 * refuse cleanly once the budget is exceeded. Nothing is uploaded;
		 * allocateBuffer() reports the refusal once per filename and
		 * remembers it, so the file is not read again on every play. */
		std::vector<uint8_t> pcm;

		while (!(sample->flags & (SOUND_SAMPLEFLAG_EOF | SOUND_SAMPLEFLAG_ERROR)))
		{
			const uint32_t decoded = Sound_Decode(sample);

			if (decoded > SE_DECODE_BUDGET - pcm.size())
			{
				overBudget = true;
				errorMsg = "decoded PCM exceeds the SE byte budget";
				return false;
			}

			const uint8_t *chunk = static_cast<const uint8_t*>(sample->buffer);
			pcm.insert(pcm.end(), chunk, chunk + decoded);
		}

		buffer = new SoundBuffer;

		uint8_t sampleSize = formatSampleSize(sample->actual.format);
		uint32_t sampleCount = pcm.size() / sampleSize;
		buffer->bytes = sampleSize * sampleCount;

		ALenum alFormat = chooseALFormat(sampleSize, sample->actual.channels);

		AL::Buffer::uploadData(buffer->alBuffer, alFormat, pcm.data(),
							   buffer->bytes, sample->actual.rate);

		return true;
	}
};

/* Filenames allocateBuffer() has already reported as undecodable.
 *
 * A failed decode caches nothing, so without this record every single
 * Audio.se_play on an undecodable file reopens it and prints again. The
 * reasoning, the bound and the absence of a lock are the same as for
 * ALStream::reportedDecodeFailures (alstream.cpp): SoundEmitter::play() is
 * reached only from Audio::sePlay on the RGSS thread, and a missing file
 * leaves through NoFileError before it gets here. */
std::set<std::string> SoundEmitter::reportedDecodeFailures;

/* Filenames refused for decoding past SE_DECODE_BUDGET. The refusal is a
 * property of the file, and finding it out again means reading up to
 * SE_SOURCE_SLURP_MAX from the card and decoding 10 MB on every play
 * Same thread and lifetime as the set above. */
std::set<std::string> SoundEmitter::overBudgetFiles;

void SoundEmitter::forgetDecodeFailures()
{
	reportedDecodeFailures.clear();
	overBudgetFiles.clear();
}

SoundBuffer *SoundEmitter::allocateBuffer(const std::string &filename)
{
	SoundBuffer *buffer = bufferHash.value(filename, 0);

	if (buffer)
	{
		/* Buffer still in cache.
		 * Promote to the front of the priority list, the same end
		 * freshly decoded entries enter at: hits used to append to
		 * the tail, which is the end eviction removes from, so
		 * replaying an effect made it the next eviction victim. */
		buffers.remove(buffer->link);
		buffers.prepend(buffer->link);

		return buffer;
	}
	else
	{
		if (overBudgetFiles.count(filename))
			return 0;

		/* Buffer not in cache, needs to be loaded */
		SoundOpenHandler handler;
		shState->fileSystem().openRead(handler, filename.c_str());
		buffer = handler.buffer;

		if (!buffer)
		{
			if (handler.overBudget)
				overBudgetFiles.insert(filename);

			/* Once per filename, not once per attempt. The text is
			 * unchanged, so a log grep written against stock mkxp-z
			 * still matches. */
			if (!handler.midiSilent && reportedDecodeFailures.insert(filename).second)
			{
				char buf[512];
				snprintf(buf, sizeof(buf), "Unable to decode sound: %s: %s",
				         filename.c_str(), handler.errorMsg.empty() ? Sound_GetError() : handler.errorMsg.c_str());
				Debug() << buf;
			}

			return 0;
		}

		buffer->key = filename;
		uint32_t wouldBeBytes = bufferBytes + buffer->bytes;

		/* If memory limit is reached, delete lowest priority buffer
		 * until there is room or no buffers left */
		while (wouldBeBytes > SE_CACHE_MEM && !buffers.isEmpty())
		{
			SoundBuffer *last = buffers.tail();
			bufferHash.remove(last->key);
			buffers.remove(last->link);

			wouldBeBytes -= last->bytes;

			SoundBuffer::deref(last);
		}

		bufferHash.insert(filename, buffer);
		buffers.prepend(buffer->link);

		bufferBytes = wouldBeBytes;

		return buffer;
	}
}
