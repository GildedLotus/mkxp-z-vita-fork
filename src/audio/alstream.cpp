/*
** alstream.cpp
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

#include "alstream.h"

#include "sharedstate.h"
#include "sharedmidistate.h"
#include "eventthread.h"
#include "filesystem.h"
#include "exception.h"
#include "aldatasource.h"
#include "fluid-fun.h"
#include "sdl-util.h"
#include "debugwriter.h"

#include <SDL_mutex.h>
#include <SDL_thread.h>
#include <SDL_timer.h>

ALStream::ALStream(LoopMode loopMode,
		           const std::string &threadId)
	: looped(loopMode == Looped),
	  state(Closed),
	  source(0),
	  thread(0),
	  preemptPause(false),
      pitch(1.0f),
	  procFrames(0),
	  wrapMark()
{
	alSrc = AL::Source::gen();

	AL::Source::setVolume(alSrc, 1.0f);
	AL::Source::setPitch(alSrc, 1.0f);
	AL::Source::detachBuffer(alSrc);

	for (int i = 0; i < STREAM_BUFS; ++i)
		alBuf[i] = AL::Buffer::gen();

	pauseMut = SDL_CreateMutex();

	threadName = std::string("al_stream (") + threadId + ")";
}

ALStream::~ALStream()
{
	close();

	AL::Source::clearQueue(alSrc);
	AL::Source::del(alSrc);

	for (int i = 0; i < STREAM_BUFS; ++i)
		AL::Buffer::del(alBuf[i]);

	SDL_DestroyMutex(pauseMut);
}

void ALStream::close()
{
	checkStopped();

	switch (state)
	{
	case Playing:
	case Paused:
		stopStream();
	case Stopped:
		closeSource();
		state = Closed;
	case Closed:
		return;
	}
}

void ALStream::open(const std::string &filename)
{
	openSource(filename);

	state = Stopped;
}

void ALStream::stop()
{
	checkStopped();

	switch (state)
	{
	case Closed:
	case Stopped:
		return;
	case Playing:
	case Paused:
		stopStream();
	}

	state = Stopped;
}

void ALStream::play(double offset)
{
	if (!source)
		return;

	checkStopped();

	switch (state)
	{
	case Closed:
	case Playing:
		return;
	case Stopped:
		if (!startStream(offset))
			return;
		break;
	case Paused :
		resumeStream();
	}

	state = Playing;
}

void ALStream::pause()
{
	checkStopped();

	switch (state)
	{
	case Closed:
	case Stopped:
	case Paused:
		return;
	case Playing:
		pauseStream();
	}

	state = Paused;
}

void ALStream::setVolume(float value)
{
	AL::Source::setVolume(alSrc, value);
}

void ALStream::setPitch(float value)
{
	/* If the source supports setting pitch natively,
	 * we don't have to do it via OpenAL */
	if (source && source->setPitch(value))
		AL::Source::setPitch(alSrc, 1.0f);
	else
		AL::Source::setPitch(alSrc, value);
}

ALStream::State ALStream::queryState()
{
	checkStopped();

	return state;
}

double ALStream::queryOffset()
{
	if (state == Closed || !source)
		return 0;

	const uint64_t frames = procFrames.load(std::memory_order_relaxed);

	double procOffset = static_cast<double>(frames) / source->sampleRate();

	// TODO: getSecOffset returns a float, we should improve precision to double.
	return procOffset + AL::Source::getSecOffset(alSrc);
}

void ALStream::closeSource()
{
	delete source;
}

struct ALStreamOpenHandler : FileSystem::OpenHandler
{
	bool looped;
	ALDataSource *source;
	std::string errorMsg;
	bool midiSilent = false;

	ALStreamOpenHandler(bool looped)
	    : looped(looped), source(0)
	{}

	bool tryRead(SDL_RWops &ops, const char *ext)
	{
		/* Try to read ogg file signature */
		char sig[5] = { 0 };
		SDL_RWread(&ops, sig, 1, 4);
		SDL_RWseek(&ops, 0, RW_SEEK_SET);

		try
		{
			if (!strcmp(sig, "OggS"))
			{
				source = createVorbisSource(ops, looped);
				return true;
			}

			if (!strcmp(sig, "MThd"))
			{
				shState->midiState().initIfNeeded(shState->config());

				if (HAVE_FLUID)
				{
					source = createMidiSource(ops, looped);
					return true;
				}
				SDL_RWclose(&ops);
				midiSilent = true;
				return true;
			}

			source = createSDLSource(ops, ext, STREAM_BUF_SIZE, looped);
		}
		catch (const Exception &e)
		{
			/* All source constructors will close the passed ops
			 * before throwing errors */
			errorMsg = e.msg;
			return false;
		}

		return true;
	}
};

/* Filenames openSource() has already reported as undecodable.
 *
 * Upstream prints the line below on every attempt. SDL_sound's MIDI decoder
 * is off on this port (MIDI playback is handled separately by the static
 * TinySoundFont path), and an RPG Maker XP game whose soundtrack is the XP
 * RTP's .mid files
 * reopens its BGM on every map change. Alternating between two maps reprints
 * the same two lines forever, and the Vita's log sink is synchronous: two
 * sceIoSyncByFd per line, paid on the thread that changed the map.
 *
 * One record for every stream, so the log holds one line per file rather
 * than one per stream that tried it. It only ever grows by files that exist
 * and cannot be decoded -- a missing file leaves openRead() by way of
 * NoFileError, above -- so it is bounded by the game's own audio directory.
 *
 * No lock: openSource() is reached only through AudioStream::play(), whose
 * only callers are Audio::bgmPlay / bgsPlay / mePlay on the RGSS thread. The
 * MeWatch and the two fade threads reach ALStream::play / stop / queryState,
 * never open. Audio::reset(), which clears this, is that same thread. */
std::set<std::string> ALStream::reportedDecodeFailures;

void ALStream::forgetDecodeFailures()
{
	reportedDecodeFailures.clear();
}

void ALStream::openSource(const std::string &filename)
{
	ALStreamOpenHandler handler(looped);
	try
	{
		shState->fileSystem().openRead(handler, filename.c_str());
	} catch (const Exception &e)
	{
		/* If no file was found then we leave the stream open.
		 * A PHYSFSError means we found a match but couldn't
		 * open the file, so we'll close it in that case. */
		if (e.type != Exception::NoFileError)
			close();
		
		throw e;
	}

	close();

	/* Once per filename, not once per attempt. The text is unchanged, so a
	 * log grep written against stock mkxp-z still matches. */
	if (!handler.source && !handler.midiSilent && reportedDecodeFailures.insert(filename).second)
	{
		char buf[512];
		snprintf(buf, sizeof(buf), "Unable to decode audio stream: %s: %s",
		         filename.c_str(), handler.errorMsg.c_str());

		Debug() << buf;
	}
	
	source = handler.source;
	needsRewind.clear();
}

void ALStream::stopStream()
{
	threadTermReq.set();

	if (thread)
	{
		SDL_WaitThread(thread, 0);
		thread = 0;
		needsRewind.set();
	}

	/* Need to stop the source _after_ the thread has terminated,
	 * because it might have accidentally started it again before
	 * seeing the term request */
	AL::Source::stop(alSrc);
	AL::Source::clearQueue(alSrc);

	procFrames.store(0, std::memory_order_relaxed);
}

bool ALStream::startStream(double offset)
{
	AL::Source::clearQueue(alSrc);

	preemptPause = false;
	streamInited.clear();
	sourceExhausted.clear();
	threadFinished.clear();
	threadTermReq.clear();
	for (int i = 0; i < STREAM_BUFS; ++i)
		wrapMark[i] = false;

	startOffset = offset;
	procFrames.store(static_cast<uint64_t>(offset * source->sampleRate()),
	                 std::memory_order_relaxed);

	thread = createSDLThread
		<ALStream, &ALStream::streamData>(this, threadName);
	if (!thread)
	{
		threadFinished.set();
		stopStream();
		return false;
	}
	return true;
}

void ALStream::pauseStream()
{
	SDL_LockMutex(pauseMut);

	if (AL::Source::getState(alSrc) != AL_PLAYING)
		preemptPause = true;
	else
		AL::Source::pause(alSrc);

	SDL_UnlockMutex(pauseMut);
}

void ALStream::resumeStream()
{
	SDL_LockMutex(pauseMut);

	if (preemptPause)
		preemptPause = false;
	else
		AL::Source::play(alSrc);

	SDL_UnlockMutex(pauseMut);
}

void ALStream::checkStopped()
{
	/* A failed worker can finish before it queues anything, or while paused. */
	if ((state == Playing || state == Paused) && threadFinished)
	{
		stopStream();
		state = Stopped;
		return;
	}

	/* This only concerns the scenario where
	 * state is still 'Playing', but the stream
	 * has already ended on its own (EOF, Error) */
	if (state != Playing)
		return;

	/* If streaming thread hasn't queued up
	 * buffers yet there's not point in querying
	 * the AL source */
	if (!streamInited)
		return;

	/* If alSrc isn't playing, but we haven't
	 * exhausted the data source yet, we're just
	 * having a buffer underrun */
	if (!sourceExhausted)
		return;

	if (AL::Source::getState(alSrc) == AL_PLAYING)
		return;

	stopStream();
	state = Stopped;
}

/* thread func */
void ALStream::streamData()
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* Above Ruby, before anything else this thread does. This is the thread that decodes and
	 * refills; the OpenAL mixer that drains what it queues is raised
	 * separately, by _oal_thread_priority in src/main.cpp, to this same
	 * 112. A mixer that outranks the interpreter but a decoder that does
	 * not just moves the underrun one step back: the mixer wakes on time
	 * and finds no queued buffer.
	 *
	 * It has to be called HERE and not where the thread is created --
	 * SDL_SetThreadPriority is sceKernelChangeThreadPriority(0, ...), and
	 * the 0 is "self". SDL creates Vita threads with priority 0, which
	 * sceKernelCreateThread reads as "the creator's priority", and the
	 * creator here is whoever called ALStream::play: the rgss thread, or
	 * AudioPrivate::meWatchFun when it un-ducks a BGM after an ME. Without
	 * this line a refill thread's priority is whatever that caller happened
	 * to have, which for the rgss thread is the Ruby interpreter's.
	 *
	 * The result is not checked because it cannot fail: the only error
	 * sceKernelChangeThreadPriority has for a self-directed call is an
	 * illegal priority, and SDL maps HIGH to 112, inside the 64..191 user
	 * range. It is also cheap enough to sit on the start path -- one
	 * syscall per stream start, not per buffer. */
	SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

	struct Finish
	{
		ALStream &stream;
		~Finish()
		{
			AL::Source::stop(stream.alSrc);
			AL::Source::clearQueue(stream.alSrc);
			stream.threadFinished.set();
		}
	} finish = {*this};

	/* Fill up queue */
	bool firstBuffer = true;
	ALDataSource::Status status;

	if (threadTermReq)
		return;

	//if (needsRewind)
		source->seekToOffset(startOffset);

	for (int i = 0; i < STREAM_BUFS; ++i)
	{
		if (threadTermReq)
			return;

		AL::Buffer::ID buf = alBuf[i];

		status = source->fillBuffer(buf);

		if (status == ALDataSource::Error)
		{
			Debug() << "audio stream read error while priming; stream stopped";
			return;
		}

		AL::Source::queueBuffer(alSrc, buf);

		if (firstBuffer)
		{
			resumeStream();

			firstBuffer = false;
			streamInited.set();
		}

		if (threadTermReq)
			return;

		/* The prefill crosses the loop point just like a refill does; a
		 * wrap buffer retired without this mark never resets procFrames,
		 * and queryOffset() then runs one loop length ahead of the audible
		 * position until the next wrap. */
		wrapMark[i] = status == ALDataSource::WrapAround;

		if (status == ALDataSource::EndOfStream)
		{
			sourceExhausted.set();
			break;
		}
	}

	/* Wait for buffers to be consumed, then
	 * refill and queue them up again */
	while (true)
	{
		shState->rtData().syncPoint.passSecondarySync();

		ALint procBufs = AL::Source::getProcBufferCount(alSrc);

		while (procBufs--)
		{
			if (threadTermReq)
				break;

			AL::Buffer::ID buf = AL::Source::unqueueBuffer(alSrc);

			/* If something went wrong, try again later */
			if (buf == AL::Buffer::ID(0))
				break;

			int slot = 0;
			while (slot < STREAM_BUFS - 1 && !(alBuf[slot] == buf))
				++slot;

			if (wrapMark[slot])
			{
				/* Reset the processed sample count so
				 * querying the playback offset returns 0.0 again */
				procFrames.store(source->loopStartFrames(),
				                 std::memory_order_relaxed);
				wrapMark[slot] = false;
			}
			else
			{
				/* Add the frame count contained in this
				 * buffer to the total count */
				ALint bits = AL::Buffer::getBits(buf);
				ALint size = AL::Buffer::getSize(buf);
				ALint chan = AL::Buffer::getChannels(buf);

				if (bits != 0 && chan != 0)
					procFrames.fetch_add(((size / (bits / 8)) / chan),
					                     std::memory_order_relaxed);
			}

			if (sourceExhausted)
				continue;

			status = source->fillBuffer(buf);

			if (status == ALDataSource::Error)
			{
				Debug() << "audio stream read error; stream stopped";
				sourceExhausted.set();
				return;
			}

			AL::Source::queueBuffer(alSrc, buf);

			/* In case of buffer underrun,
			 * start playing again */
			if (AL::Source::getState(alSrc) == AL_STOPPED)
				AL::Source::play(alSrc);

			/* If this was the last buffer before the data
			 * source loop wrapped around again, mark it as
			 * such so we can catch it and reset the processed
			 * sample count once it gets unqueued */
			wrapMark[slot] = status == ALDataSource::WrapAround;

			if (status == ALDataSource::EndOfStream)
				sourceExhausted.set();
		}

		if (threadTermReq || (sourceExhausted &&
		    AL::Source::getInteger(alSrc, AL_BUFFERS_QUEUED) == 0))
			break;

		SDL_Delay(AUDIO_SLEEP);
	}
}
