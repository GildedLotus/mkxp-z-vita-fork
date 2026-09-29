// SPDX-License-Identifier: GPL-3.0-or-later
/*
** vita-rtp.h
**
** This file is part of the PS Vita port of mkxp-z.
**
** Which RTP directories a Vita boot should try to mount, decided from the
** RTP list the config produced and the RGSS version the game runs at.
**
** Pure and header-only on purpose: no I/O, no engine headers, nothing but
** <string> and <vector>. The one thing that has to touch the filesystem --
** turning a relative entry into an absolute path -- is passed in, so this
** file compiles on its own on the host.
**
** Why the Vita needs it at all:
**
**   * PhysFS here calls sceIo* directly, with no notion of a process current
**     directory, so a relative RTP entry could never resolve. Entries are
**     made absolute against the game folder (which is the CWD by the time
**     SharedState mounts) before they are handed over.
**   * The player cannot be repacked per game by the launcher, so when
**     nobody configured an RTP the engine picks the one that matches the RGSS
**     version: <root>/XP, <root>/VX or <root>/VXAce.
**   * "none" is how a config says "this game wants no RTP at all", which an
**     empty list can no longer mean now that empty means "use the default".
**
** The version parameter is spelled rgssVersion and must stay that way:
** sharedstate.h, which every caller in the engine has already included,
** does `#define rgssVer SharedState::rgssVersion`, so a parameter called
** rgssVer would expand to a qualified name and the header would not parse.
** Only a real Vita build catches it -- a host-compilable build never sees that macro.
*/

#ifndef VITA_RTP_H
#define VITA_RTP_H

#include <string>
#include <vector>

/* Where the installed RTPs live. Overridable for host-compilable builds;
 * src/config.cpp hard-codes the same literal as VITA_RTP_ROOT, so keep the
 * two spellings in step. */
#ifndef MKXPZ_VITA_RTP_ROOT
#define MKXPZ_VITA_RTP_ROOT "ux0:/data/mkxp-z/rtp"
#endif

namespace VitaRtp {

/* mkxp_fs::normalizePath(p, false, true) at the call site: relative to the
 * game folder, lexically normalized, separators unified. */
typedef std::string (*Absolutize)(const std::string &);

struct Candidate
{
	/* Absolute, ready for FileSystem::addPath. */
	std::string path;

	/* True when this path IS the per-version default location -- whether it
	 * was synthesized here or arrived in the config, because
	 * vitaAutoRtp fills that same path in when the directory exists. The flag
	 * only picks the word the boot trace uses, so it describes the location,
	 * not who named it. */
	bool isDefault;
};

/* An entry that opts the game out of every RTP. ASCII case only: a config is
 * not a place for Turkish dotless-i surprises, and this has to agree with the
 * literal a per-game deployment writes. */
inline bool isNone(const std::string &entry)
{
	static const char none[] = "none";

	if (entry.size() != sizeof(none) - 1)
		return false;

	for (size_t i = 0; i < entry.size(); ++i)
	{
		char c = entry[i];

		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');

		if (c != none[i])
			return false;
	}

	return true;
}

/* The directory the RGSS version installs its RTP into, or "" when the
 * version is not one of the three that has one. rgssVersion is already
 * resolved to 1, 2 or 3 by readGameINI() before SharedState runs; anything
 * else (including the 0 the caller passes for a customScript player, which
 * has no Game.ini and no RTP of its own) asks for no default. */
inline std::string defaultPath(int rgssVersion, const char *root)
{
	const char *name;

	if (!root || !*root)
		return std::string();

	switch (rgssVersion)
	{
	case 1:
		name = "/XP";
		break;
	case 2:
		name = "/VX";
		break;
	case 3:
		name = "/VXAce";
		break;
	default:
		return std::string();
	}

	return std::string(root) + name;
}

/* Whether the config asked for no RTP at all. Only meaningful when
 * candidates() came back empty; "none" next to a real entry drops the "none"
 * and keeps the entry. */
inline bool disabled(const std::vector<std::string> &configured)
{
	for (size_t i = 0; i < configured.size(); ++i)
		if (isNone(configured[i]))
			return true;

	return false;
}

/* The RTP directories to try, in mount order.
 *
 *   * "none" entries are dropped and never mounted;
 *   * every other entry is absolutized, keeping the configured order, with
 *     exact duplicates (after absolutizing) dropped and no default added;
 *   * with nothing configured and no "none", exactly one candidate: the
 *     per-version default.
 *
 * Empty strings are not entries. fillStringVec keeps a "" the JSON carried,
 * and absolutizing one yields the game folder, which is already mounted.
 */
inline std::vector<Candidate> candidates(const std::vector<std::string> &configured,
                                         int rgssVersion, const char *root,
                                         Absolutize absolutize)
{
	std::vector<Candidate> out;
	const std::string fallback = defaultPath(rgssVersion, root);
	std::string standard;
	bool sawReal = false;
	bool sawNone = false;
	size_t i;

	if (!fallback.empty())
		standard = absolutize ? absolutize(fallback) : fallback;

	for (i = 0; i < configured.size(); ++i)
	{
		const std::string &entry = configured[i];

		if (isNone(entry))
		{
			sawNone = true;
			continue;
		}

		if (entry.empty())
			continue;

		sawReal = true;

		Candidate candidate;
		candidate.path = absolutize ? absolutize(entry) : entry;
		candidate.isDefault = false;

		bool duplicate = false;
		for (size_t j = 0; j < out.size(); ++j)
			if (out[j].path == candidate.path)
				duplicate = true;

		if (!duplicate)
			out.push_back(candidate);
	}

	if (!sawReal && !sawNone && !standard.empty())
	{
		Candidate candidate;
		candidate.path = standard;
		candidate.isDefault = true;
		out.push_back(candidate);
	}

	/* A configured path that resolves to the per-version default location is
	 * the default: that is exactly what vitaAutoRtp writes into the list when
	 * the directory exists, and a log that called it "explicit" would send a
	 * reader looking for a config entry nobody wrote. */
	if (!standard.empty())
		for (i = 0; i < out.size(); ++i)
			if (out[i].path == standard)
				out[i].isDefault = true;

	return out;
}

}

#endif /* VITA_RTP_H */
