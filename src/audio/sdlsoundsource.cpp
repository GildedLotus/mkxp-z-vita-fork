/*
** sdlsoundsource.cpp
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

#include "aldatasource.h"
#include "exception.h"

#include <SDL_sound.h>

static int SDL_RWopsCloseNoop(SDL_RWops *ops) {
	return 0;
}

/* How many further Sound_Decode calls one fillBuffer will make to complete a
 * short read (see joinShortRead). Ogg Vorbis needs exactly one. The bound is
 * here so that a source handing over a few bytes at a time cannot turn a
 * single buffer fill into thousands of decoder calls; reaching it costs
 * nothing but a partly filled AL buffer, and the rest follows on the next
 * call. */
#define SDLSOUND_JOIN_TRIES 16

struct SDLSoundSource : ALDataSource
{
	Sound_Sample *sample;
	SDL_RWops srcOps;
	SDL_RWops unclosableOps;
	uint8_t sampleSize;
	bool looped;

	ALenum alFormat;
	ALsizei alFreq;

	/* Scratch for joining short reads into one buffer, allocated the first
	 * time SDL_sound actually raises EAGAIN. A game whose BGM, BGS and ME are
	 * all WAV never pays for it: the WAV decoder knows how many bytes are
	 * left and ends on EOF instead. */
	uint8_t *joinBuf;
	uint32_t joinSize;

	SDLSoundSource(SDL_RWops &ops,
	               const char *extension,
	               uint32_t maxBufSize,
	               bool looped)
	    : srcOps(ops),
	      unclosableOps(ops),
	      looped(looped),
	      joinBuf(0),
	      joinSize(0)
	{
		/* A copy of srcOps with a no-op close function,
		 * so we can reuse the ops if we need to change the format. */
		unclosableOps.close = SDL_RWopsCloseNoop;
		
		sample = Sound_NewSample(&unclosableOps, extension, 0, maxBufSize);
		
		if (!sample)
		{
			SDL_RWclose(&srcOps);
			throw Exception(Exception::SDLError, "SDL_sound: %s", Sound_GetError());
		}

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
			// Unfortunately there's no way to change the desired format of a sample.
			// https://github.com/icculus/SDL_sound/issues/91
			// So we just have to close the sample (which closes the file too),
			// and retry with a new desired format.
			Sound_FreeSample(sample);
			SDL_RWseek(&unclosableOps, 0, RW_SEEK_SET);
			
			Sound_AudioInfo desired;
			SDL_memset(&desired, '\0', sizeof (Sound_AudioInfo));
			desired.format = AUDIO_F32SYS;

			sample = Sound_NewSample(&unclosableOps, extension, &desired, maxBufSize);

			if (!sample)
			{
				SDL_RWclose(&srcOps);
				throw Exception(Exception::SDLError, "SDL_sound: %s", Sound_GetError());
			}
		}

		sampleSize = formatSampleSize(sample->actual.format);

		alFormat = chooseALFormat(sampleSize, sample->actual.channels);
		alFreq = sample->actual.rate;
	}

	~SDLSoundSource()
	{
		Sound_FreeSample(sample);
		SDL_RWclose(&srcOps);
		SDL_free(joinBuf);
	}

	/* SDL_sound raises SOUND_SAMPLEFLAG_EAGAIN whenever a read came up short
	 * of a full buffer. That is neither an error nor the end of the stream:
	 * it means "that is all there was this time, ask again". Ogg Vorbis has
	 * no way of knowing how much is left, so stb_vorbis flags every short
	 * read, which in practice is the final buffer of every Ogg stream
	 *
	 *
	 * Sound_Decode always decodes to the front of sample->buffer, so asking
	 * again overwrites what it has just handed over. Copy the short result
	 * out first and append what the following decodes deliver, so one AL
	 * buffer still carries one bufferful of audio and the EOF that ends the
	 * stream arrives in the same call as the samples that precede it.
	 *
	 * Returns the total byte count and points 'data' at those bytes. */
	uint32_t joinShortRead(uint32_t decoded, const void **data)
	{
		/* Sound_Decode never returns more than the buffer size the sample
		 * was created with: SDL_sound sizes the conversion buffer from the
		 * same number (SDL_sound.c, init_sample). */
		const uint32_t capacity = sample->buffer_size;

		if (decoded == 0 || decoded >= capacity)
			return decoded;

		if (joinSize < capacity)
		{
			SDL_free(joinBuf);
			joinBuf = (uint8_t*) SDL_malloc(capacity);
			joinSize = joinBuf ? capacity : 0;
		}

		/* Out of memory: those bytes are real audio either way, so queue
		 * them on their own rather than drop them. */
		if (!joinBuf)
			return decoded;

		SDL_memcpy(joinBuf, sample->buffer, decoded);
		*data = joinBuf;

		for (int tries = 0; tries < SDLSOUND_JOIN_TRIES; ++tries)
		{
			if (decoded >= capacity)
				break;

			if (!(sample->flags & SOUND_SAMPLEFLAG_EAGAIN))
				break;

			/* Sound_Decode refuses to run again after either of these, and
			 * both are for the caller to answer. */
			if (sample->flags & (SOUND_SAMPLEFLAG_EOF | SOUND_SAMPLEFLAG_ERROR))
				break;

			uint32_t more = Sound_Decode(sample);

			if (more > capacity - decoded)
				more = capacity - decoded;

			if (more == 0)
				break;

			SDL_memcpy(joinBuf + decoded, sample->buffer, more);
			decoded += more;
		}

		return decoded;
	}

	Status fillBuffer(AL::Buffer::ID alBuffer)
	{
		uint32_t decoded = Sound_Decode(sample);
		const void *data = sample->buffer;

		if (sample->flags & SOUND_SAMPLEFLAG_EAGAIN)
			decoded = joinShortRead(decoded, &data);

		if (sample->flags & SOUND_SAMPLEFLAG_ERROR)
			return ALDataSource::Error;

		/* Still nothing after the retries, and the stream has not ended:
		 * the source is stuck. Give up, as stock mkxp-z did, rather than
		 * queue empty buffers forever. */
		if (decoded == 0 && (sample->flags & SOUND_SAMPLEFLAG_EAGAIN))
			return ALDataSource::Error;

		AL::Buffer::uploadData(alBuffer, alFormat, data, decoded, alFreq);

		if (sample->flags & SOUND_SAMPLEFLAG_EOF)
		{
			if (looped)
			{
				Sound_Rewind(sample);
				return ALDataSource::WrapAround;
			}
			else
			{
				return ALDataSource::EndOfStream;
			}
		}

		return ALDataSource::NoError;
	}

	int sampleRate()
	{
		return sample->actual.rate;
	}

	void seekToOffset(double seconds)
	{
		if (seconds <= 0)
		{
			Sound_Rewind(sample);
		}
		else
		{
			// Unfortunately there is no easy API in SDL_sound for seeking with better precision than 1ms.
			// TODO: Work around this by flooring instead of rounding, and then manually consuming the remaining samples.
			Sound_Seek(sample, static_cast<uint32_t>(lround(seconds * 1000)));
		}
	}

	uint32_t loopStartFrames()
	{
		/* Loops from the beginning of the file */
		return 0;
	}

	bool setPitch(float)
	{
		return false;
	}
};

ALDataSource *createSDLSource(SDL_RWops &ops,
                              const char *extension,
			                  uint32_t maxBufSize,
			                  bool looped)
{
	return new SDLSoundSource(ops, extension, maxBufSize, looped);
}
