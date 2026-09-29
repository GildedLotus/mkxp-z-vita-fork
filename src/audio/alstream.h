/*
** alstream.h
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

#ifndef ALSTREAM_H
#define ALSTREAM_H

#include "al-util.h"
#include "sdl-util.h"

#include <atomic>
#include <set>
#include <string>
#include <SDL_rwops.h>

struct ALDataSource;

#define STREAM_BUFS 3

/* State-machine like audio playback stream.
 * This class is NOT thread safe */
struct ALStream
{
	enum State
	{
		Closed,
		Stopped,
		Playing,
		Paused
	};

	bool looped;
	State state;

	ALDataSource *source;
	SDL_Thread *thread;

	std::string threadName;

	SDL_mutex *pauseMut;
	bool preemptPause;

	/* When this flag isn't set and alSrc is
	 * in 'STOPPED' state, stream isn't over
	 * (it just hasn't started yet) */
	AtomicFlag streamInited;
	AtomicFlag sourceExhausted;
	AtomicFlag threadFinished;

	AtomicFlag threadTermReq;

	AtomicFlag needsRewind;
	double startOffset;

	float pitch;

	AL::Source::ID alSrc;
	AL::Buffer::ID alBuf[STREAM_BUFS];

	/* Frames the device has already finished with: the fixed part of the
	 * offset queryOffset() reports, and the only member of this class two
	 * threads touch without a lock between them.
	 *
	 * stopStream() and startStream() write it with the stream thread joined
	 * or not yet created, and every caller of those holds AudioStream's
	 * stream lock, which is also what queryOffset() is read under. The
	 * stream thread is the gap: streamData() adds each retired buffer's
	 * frame count and resets the counter at a loop point, and it takes no
	 * lock at all.
	 *
	 * Atomic so that what the reader gets is a value that was really
	 * stored. This is a 32 bit ARM: a plain uint64_t is two instructions
	 * either way, so an unsynchronised read can splice the low half of one
	 * value onto the high half of another, and at 22050 Hz a torn high word
	 * is an offset wrong by tens of thousands of seconds. Relaxed
	 * throughout -- nothing is published through this counter except the
	 * counter itself. */
	std::atomic<uint64_t> procFrames;
	/* Per alBuf slot: that buffer ends at the loop point, so its retirement
	 * resets procFrames. One slot per buffer, not one "last" buffer: a loop
	 * shorter than a buffer makes every queued buffer a wrap buffer, and a
	 * single mark was overwritten before the marked buffer retired
	 * Stream thread only, and reset before it starts. */
	bool wrapMark[STREAM_BUFS];

	struct
	{
		ALenum format;
		ALsizei freq;
	} stream;

	enum LoopMode
	{
		Looped,
		NotLooped
	};

	ALStream(LoopMode loopMode,
	         const std::string &threadId);
	~ALStream();

	void close();
	void open(const std::string &filename);
	void stop();
	void play(double offset = 0);
	void pause();

	void setVolume(float value);
	void setPitch(float value);
	State queryState();
	double queryOffset();
	bool queryNativePitch();

	/* Forget which files have already reported a decode failure, so the
	 * next attempt at each of them is reported once more. Audio::reset()
	 * calls this: Audio.__reset__ starts the game's audio over. */
	static void forgetDecodeFailures();

private:
	/* Filenames openSource() has already complained about. See the
	 * definition in alstream.cpp for why it is one record for all
	 * streams and why it needs no lock. */
	static std::set<std::string> reportedDecodeFailures;

	void closeSource();
	void openSource(const std::string &filename);

	void stopStream();
	bool startStream(double offset);
	void pauseStream();
	void resumeStream();

	void checkStopped();

	/* thread func */
	void streamData();
};

#endif // ALSTREAM_H
