/*
** soundemitter.h
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

#ifndef SOUNDEMITTER_H
#define SOUNDEMITTER_H

#include "intrulist.h"
#include "al-util.h"
#include "boost-hash.h"

#include <set>
#include <string>
#include <vector>

struct SoundBuffer;
struct Config;

struct SoundEmitter
{
	typedef BoostHash<std::string, SoundBuffer*> BufferHash;

	IntruList<SoundBuffer> buffers;
	BufferHash bufferHash;

	/* Byte count sum of all cached / playing buffers */
	uint32_t bufferBytes;

	const size_t srcCount;
	std::vector<AL::Source::ID> alSrcs;
	std::vector<SoundBuffer*> atchBufs;

	/* Indices of sources, sorted by priority (lowest first) */
	std::vector<size_t> srcPrio;

	SoundEmitter(const Config &conf);
	~SoundEmitter();

	void play(const std::string &filename,
	          int volume,
	          int pitch);

	void stop();

	/* Forget which files have already reported a decode failure, so the
	 * next attempt at each of them is reported once more. Audio::reset()
	 * calls this alongside ALStream::forgetDecodeFailures(). */
	static void forgetDecodeFailures();

private:
	/* Filenames allocateBuffer() has already complained about. Kept apart
	 * from ALStream's record on purpose: the two paths say different
	 * things about the same file. See the definition in soundemitter.cpp. */
	static std::set<std::string> reportedDecodeFailures;
	/* Files refused past the SE decode budget; not re-read on each play. */
	static std::set<std::string> overBudgetFiles;

	SoundBuffer *allocateBuffer(const std::string &filename);
};

#endif // SOUNDEMITTER_H
