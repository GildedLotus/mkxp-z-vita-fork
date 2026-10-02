/*
** font.cpp
**
** This file is part of mkxp.
**
** Copyright (C) 2013 - 2021 Amaryllis Kulla <ancurio@mapleshrine.eu>
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

#include "font.h"

#include "sharedstate.h"
#include "filesystem.h"
#include "exception.h"
#include "boost-hash.h"
#include "util.h"
#include "config.h"
#include "encoding.h"

#include "debugwriter.h"

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "vita_glue.h"
#endif

#include <string>
#include <utility>
#include <algorithm>
#include <cctype>
#include <array>
#include <unordered_map>

#include <SDL_ttf.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_IDS_H

#include "liberation.ttf.xxd"

#define BUNDLED_FONT liberation

#define BUNDLED_FONT_DECL(FONT) \
	extern unsigned char ___assets_##FONT##_ttf[]; \
	extern unsigned int ___assets_##FONT##_ttf_len;

BUNDLED_FONT_DECL(liberation)

#define BUNDLED_FONT_D(f) ___assets_## f ##_ttf
#define BUNDLED_FONT_L(f) ___assets_## f ##_ttf_len

// Go fuck yourself CPP
#define BNDL_F_D(f) BUNDLED_FONT_D(f)
#define BNDL_F_L(f) BUNDLED_FONT_L(f)

/* Dirty hack to get the FT_Face.
 * SDL_ttf will probably never move it from the beginning of the struct. */
#define TTF_FONT_TO_FT_FACE(font) (*reinterpret_cast<FT_Face *>(font))

static SDL_RWops *openBundledFont()
{
    return SDL_RWFromConstMem(BNDL_F_D(BUNDLED_FONT), BNDL_F_L(BUNDLED_FONT));
}


/* <name, size> */
typedef std::pair<std::string, int> FontSizeKey;
/* <name, ppem> */
typedef std::pair<std::string, int> FontPPEMKey;

struct FontSet
{
	/* 'Regular' style */
	std::string regular;

	/* Any other styles (used in case no 'Regular' exists) */
	std::string other;

	/* 'Regular' style obtained via SFNT */
	std::string sfnt_regular;

	/* Any other styles obtained via SFNT */
	std::string sfnt_other;

	const std::string *operator->() const noexcept
	{
		if (!sfnt_regular.empty())
			return &sfnt_regular;
		if (!sfnt_other.empty())
			return &sfnt_other;
		if (!regular.empty())
			return &regular;
		return &other;
	}
};

struct SharedFontStatePrivate
{
	/* Maps: font family name, To: substituted family name,
	 * as specified via configuration file / arguments */
	BoostHash<std::string, std::string> subs;

	/* Maps: font family name, To: set of physical
	 * font filenames located in "Fonts/" */
	BoostHash<std::string, FontSet> sets;

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* Requested families getFont has already reported. Font
	 * objects ask for their face on every draw, so the resolution line is
	 * printed the first time a family is asked for and never again; without
	 * that it would be one line per text draw. Keyed by the request, not by
	 * the answer, so two names landing on one face are both visible. */
	BoostSet<std::string> loggedFamilies;
#endif

	/* Pool of font size to ppem values */
	BoostHash<FontSizeKey, int> size_to_ppem;

	/* Pool of already opened fonts; once opened, they are reused
	 * and never closed until the termination of the program */
	BoostHash<FontPPEMKey, std::array<TTF_Font*, 2>> ppem_to_font;

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* Heap blocks the pooled fonts above read their glyphs out of
	 * SDL_RWFromConstMem does not own the memory it wraps and
	 * TTF_CloseFont's freesrc frees only the RWops, so a block has to outlive
	 * its font -- and a pooled font is never closed before the program ends,
	 * so that means living exactly as long as this struct. Ownership moves
	 * here only once the font is in the pool; ~SharedFontState releases them
	 * after the last TTF_CloseFont. */
	std::vector<void*> fontBuffers;
	// Non-owning exact-path index; bytes remain owned by fontBuffers.
	std::vector<std::pair<std::string, std::pair<void *, int>>> fontBytesByPath;
	// Sum of fontBuffers, held under MKXPZ_VITA_FONT_SLURP_TOTAL.
	size_t fontBufferBytes = 0;
#endif
    
    /* Internal default font family that is used anytime an
     * empty/invalid family is requested */
    std::string defaultFamily;

	float fontScale;
	bool fontKerning;
	int fontHinting;
};

/* How far a chain of `fontSub` entries is followed before giving up. Bounded
 * so a cycle ("a>b" together with "b>a") terminates. */
#define FONT_SUB_MAX_HOPS 4

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
/* The three faces vita/mkxp-z-vpk/fonts ships, spelled the way
 * initFontSetCB registers them: the lower-cased FACE family name, never a
 * file name. Nothing here assumes they are present -- every use is guarded,
 * because app0:/fonts is assembled at package time and a card copy may hold
 * any subset. */
#define VITA_FONT_LATIN    "liberation sans"
#define VITA_FONT_JP_FIXED "vl gothic"
#define VITA_FONT_JP_PROP  "vl pgothic"

/* True when `name` carries any byte outside ASCII.
 *
 * Family names reach us as bytes and are lower-cased byte-wise, so this is
 * the whole test for "this name is not a Latin one": a Shift_JIS or UTF-8
 * Japanese family name always has a byte >= 0x80 and a Windows Latin family
 * name never does. */
static bool vitaNonAsciiFamily(const std::string &name)
{
	for (size_t i = 0; i < name.size(); ++i)
		if ((unsigned char)name[i] >= 0x80)
			return true;

	return false;
}
#endif

/* Resolve `family` -- which must already be lower-cased -- to a family that
 * some physical font file was registered under, or to "" when there is none.
 *
 * A registered family always wins. Substitutions are consulted only for a
 * family nothing was registered under, and a substitution whose target is
 * itself unregistered is passed over rather than obeyed; "" tells the caller
 * to fall back to the built-in font, exactly as an unknown family did before.
 *
 * Neither map may grow here. BoostHash::operator[] is std::map's, so a miss
 * would insert an empty value and leave a phantom family behind that a later
 * `contains()` would then believe; every operator[] below is short-circuited
 * behind its own contains(). */
static std::string resolveFamily(SharedFontStatePrivate *p, std::string family)
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	const std::string requested = family;
#endif

	for (int hop = 0; hop <= FONT_SUB_MAX_HOPS; ++hop)
	{
		if (p->sets.contains(family) && !p->sets[family]->empty())
			return family;

		if (!p->subs.contains(family))
			break;

		family = p->subs[family];
	}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* Last resort for a Japanese face nobody has heard of.
	 *
	 * The substitution table below covers the families Windows ships, but
	 * RPG Maker projects name whatever font their author had installed, and
	 * a name that is not ASCII is a name whose glyphs the Latin-only
	 * built-in font certainly does not have -- it would draw the whole
	 * string as tofu. VL Gothic is a wrong face but a readable one, and
	 * readable beats correct-and-blank.
	 *
	 * Only after the chain above has failed: a family that IS registered,
	 * or that substitutes onto one, has already returned. */
	{
		const std::string jp(VITA_FONT_JP_FIXED);

		if (vitaNonAsciiFamily(requested) &&
		    p->sets.contains(jp) && !p->sets[jp]->empty())
			return jp;
	}
#endif

	return std::string();
}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
/* Defined in fontres-telemetry.h, included with the font buffering code below. */
static void fontResInstall(void);
static void fontResShutdown(void);
#endif

SharedFontState::SharedFontState(const Config &conf)
{
	p = new SharedFontStatePrivate;

	/* Parse font substitutions */
	for (size_t i = 0; i < conf.fontSubs.size(); ++i)
	{
		const std::string &raw = conf.fontSubs[i];
		size_t sepPos = raw.find_first_of('>');

		if (sepPos == std::string::npos)
			continue;

		std::string from = raw.substr(0, sepPos);
		std::string to   = raw.substr(sepPos+1);

		p->subs.insert(from, to);
	}
	
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* Built-in fallbacks for the Windows families this device cannot have
	 *
	 *
	 * RPG Maker projects name the fonts their author had installed, and
	 * those are Microsoft's -- none of which may be redistributed. What the
	 * VPK can ship is a metric-compatible Latin face and a Japanese one, so
	 * every such name is pointed at whichever of the two is closer.
	 *
	 * This table lives here rather than in mkxp.json because it is a
	 * property of what the package ships, not of any one game: a deployed
	 * game.json can neither be expected to carry twenty entries nor to stay
	 * in step when the bundled set changes.
	 *
	 * BOTH sides must be lower-case. `sets` is keyed by the lower-cased face
	 * name (initFontSetCB) and every lookup lower-cases its request, so a
	 * mapping spelled with capitals -- on either side -- can never match;
	 * that is true of the `fontSub` entries parsed above as well.
	 *
	 * A config entry wins. fontSub is read before this runs and an existing
	 * key is left alone, so a game or the device profile can still redirect
	 * any of these names. */
	{
		static const struct { const char *from; const char *to; } bundledSubs[] =
		{
			/* Latin: Liberation Sans is metric-compatible with Arial, and
			 * it is the only Latin face in the package, so the serif and
			 * monospace families land on it too -- the wrong shape, but the
			 * right glyphs. */
			{ "arial",                 VITA_FONT_LATIN    },
			{ "helvetica",             VITA_FONT_LATIN    },
			{ "verdana",               VITA_FONT_LATIN    },
			{ "tahoma",                VITA_FONT_LATIN    },
			{ "trebuchet ms",          VITA_FONT_LATIN    },
			{ "segoe ui",              VITA_FONT_LATIN    },
			{ "calibri",               VITA_FONT_LATIN    },
			{ "microsoft sans serif",  VITA_FONT_LATIN    },
			{ "ms sans serif",         VITA_FONT_LATIN    },
			{ "lucida sans unicode",   VITA_FONT_LATIN    },
			{ "comic sans ms",         VITA_FONT_LATIN    },
			{ "times new roman",       VITA_FONT_LATIN    },
			{ "georgia",               VITA_FONT_LATIN    },
			{ "garamond",              VITA_FONT_LATIN    },
			{ "book antiqua",          VITA_FONT_LATIN    },
			{ "palatino linotype",     VITA_FONT_LATIN    },
			{ "courier new",           VITA_FONT_LATIN    },
			{ "courier",               VITA_FONT_LATIN    },
			{ "consolas",              VITA_FONT_LATIN    },
			{ "lucida console",        VITA_FONT_LATIN    },

			/* Japanese, fixed pitch. Each family appears twice: once under
			 * its ASCII name and once under the Japanese one the font
			 * itself carries, because a project saves whichever spelling
			 * the editor showed. UmePlus Gothic is mkxp-z's own RGSS2
			 * default (Font::initDefaults). */
			{ "ms gothic",             VITA_FONT_JP_FIXED },
			/* "ＭＳ ゴシック" */
			{ "\xEF\xBC\xAD\xEF\xBC\xB3\x20\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF",
			                           VITA_FONT_JP_FIXED },
			{ "ms mincho",             VITA_FONT_JP_FIXED },
			/* "ＭＳ 明朝" */
			{ "\xEF\xBC\xAD\xEF\xBC\xB3\x20\xE6\x98\x8E\xE6\x9C\x9D",
			                           VITA_FONT_JP_FIXED },
			{ "umeplus gothic",        VITA_FONT_JP_FIXED },

			/* Japanese, proportional. VL PGothic is the proportional cut of
			 * the same design, so these keep their spacing. */
			{ "ms pgothic",            VITA_FONT_JP_PROP  },
			/* "ＭＳ Ｐゴシック" */
			{ "\xEF\xBC\xAD\xEF\xBC\xB3\x20\xEF\xBC\xB0\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF",
			                           VITA_FONT_JP_PROP  },
			{ "ms ui gothic",          VITA_FONT_JP_PROP  },
			{ "ms pmincho",            VITA_FONT_JP_PROP  },
			/* "ＭＳ Ｐ明朝" */
			{ "\xEF\xBC\xAD\xEF\xBC\xB3\x20\xEF\xBC\xB0\xE6\x98\x8E\xE6\x9C\x9D",
			                           VITA_FONT_JP_PROP  },
			{ "meiryo",                VITA_FONT_JP_PROP  },
			/* "メイリオ" */
			{ "\xE3\x83\xA1\xE3\x82\xA4\xE3\x83\xAA\xE3\x82\xAA",
			                           VITA_FONT_JP_PROP  },
			{ "meiryo ui",             VITA_FONT_JP_PROP  },
			{ "yu gothic",             VITA_FONT_JP_PROP  },
			/* "游ゴシック" */
			{ "\xE6\xB8\xB8\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF",
			                           VITA_FONT_JP_PROP  },
			{ "umeplus p gothic",      VITA_FONT_JP_PROP  },

			/* Second hop for everything above it: a package or a card that
			 * carries only VL-Gothic-Regular.ttf still answers a
			 * proportional request. With both files installed "vl pgothic"
			 * is a registered family, so resolveFamily returns it and this
			 * entry is never consulted. */
			{ VITA_FONT_JP_PROP,       VITA_FONT_JP_FIXED },
		};

		for (size_t i = 0; i < sizeof(bundledSubs) / sizeof(bundledSubs[0]); ++i)
			if (!p->subs.contains(bundledSubs[i].from))
				p->subs.insert(bundledSubs[i].from, bundledSubs[i].to);
	}
#endif

	p->fontScale = conf.fontScale;
	if (p->fontScale < 0.1f)
	{
		p->fontScale = 1.0f;
	}
	p->fontKerning = conf.fontKerning;
	p->fontHinting = conf.fontHinting;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	fontResInstall();
#endif
}

SharedFontState::~SharedFontState()
{
	BoostHash<FontPPEMKey, std::array<TTF_Font*, 2>>::const_iterator iter;
	for (iter = p->ppem_to_font.cbegin(); iter != p->ppem_to_font.cend(); ++iter)
	{
		for (int i=0; i < iter->second.size(); i++)
			if (iter->second[i] != 0)
				TTF_CloseFont(iter->second[i]);
	}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* Every pooled font is closed by now, so nothing reads these any more.
	 * TTF_CloseFont's freesrc released the SDL_RWFromConstMem wrappers, never
	 * the blocks behind them; this is where those go. */
	for (size_t i = 0; i < p->fontBuffers.size(); ++i)
		SDL_free(p->fontBuffers[i]);
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	fontResShutdown();
#endif

	delete p;
}

static std::string decodeSfntName(const FT_SfntName &aname)
{
	if (!aname.string || !aname.string_len)
		return {};
	std::string str((const char *)aname.string, aname.string_len);
	// All Microsoft name records use UTF-16BE, including legacy encoding IDs.
	if (aname.platform_id == TT_PLATFORM_APPLE_UNICODE ||
	    aname.platform_id == TT_PLATFORM_MICROSOFT)
		return Encoding::decodeUTF16BE(str);
	if (aname.platform_id == TT_PLATFORM_MACINTOSH && aname.encoding_id == TT_MAC_ID_ROMAN)
		return Encoding::decodeMacRoman(str);
	if (aname.platform_id == TT_PLATFORM_MACINTOSH && aname.encoding_id == TT_MAC_ID_JAPANESE) {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		return Encoding::decodeShiftJIS(str);
#else
		try { return Encoding::convertString(str, "SHIFT_JIS"); }
		catch (const Exception &) { return {}; }
#endif
	}
	return {};
}

bool SharedFontState::initFontSetCB(SDL_RWops &ops,
                                    const std::string &filename)
{
	TTF_Font *font = TTF_OpenFontRW(&ops, 0, 0);

	if (!font)
		return false;

	std::string family = TTF_FontFaceFamilyName(font);
	std::string style = TTF_FontFaceStyleName(font);

	std::transform(family.begin(), family.end(), family.begin(),
		[](unsigned char c){ return std::tolower(c); });

	FontSet &set = p->sets[family];

	if (style == "Regular" && set.regular.empty())
		set.regular = filename;
	else if (style != "Regular" && set.other.empty())
		set.other = filename;

	bool complete = true;
	FT_Face face = TTF_FONT_TO_FT_FACE(font);

	if (FT_IS_SFNT(face))
	{
		std::unordered_map<uint64_t, std::pair<std::string, std::string>> name_map;

		for (unsigned int i = 0, name_count = FT_Get_Sfnt_Name_Count(face); i < name_count; ++i)
		{
			FT_SfntName aname;
			if (FT_Get_Sfnt_Name(face, i, &aname))
			{
				complete = false;
				continue;
			}
			if (aname.name_id != TT_NAME_ID_FONT_FAMILY && aname.name_id != TT_NAME_ID_FONT_SUBFAMILY)
				continue;
			/* FreeType can report success with an empty name after an I/O failure. */
			if (!aname.string || !aname.string_len)
			{
				complete = false;
				continue;
			}
			std::string decoded = decodeSfntName(aname);
			if (decoded.empty())
				continue;
			uint64_t key = ((uint64_t)aname.platform_id << 32) |
			               ((uint64_t)aname.encoding_id << 16) | aname.language_id;
			switch (aname.name_id)
			{
				case TT_NAME_ID_FONT_FAMILY:
					name_map[key].first = decoded;
					break;
				case TT_NAME_ID_FONT_SUBFAMILY:
					name_map[key].second = decoded;
					break;
			}
		}

		for (const auto &entry : name_map)
		{
			const std::string &sfnt_family_raw = entry.second.first;
			const std::string &sfnt_style = entry.second.second;
			if (sfnt_family_raw.empty())
				continue;

			std::string sfnt_family(sfnt_family_raw);

			std::transform(sfnt_family.begin(), sfnt_family.end(), sfnt_family.begin(),
				[](unsigned char c){ return std::tolower(c); });

			FontSet &set = p->sets[sfnt_family];

			if (sfnt_style == "Regular" && set.sfnt_regular.empty())
				set.sfnt_regular = filename;
			else if (sfnt_style != "Regular" && set.sfnt_other.empty())
				set.sfnt_other = filename;
		}
	}

	TTF_CloseFont(font);
	return complete;
}

// https://github.com/wine-mirror/wine/blob/dc34fef45d491516fa8eaee45b2ae40faa7b0bfe/dlls/win32u/freetype.c

/* The following code was derived from Wine to emulate
 * Windows's font size selection behavior. */
 
/* We're not currently using yMax and yMin for anything,
 * but it could be useful later. */
typedef struct {
	TTF_Font *font;
	int ppem;
	short yMax;
	short yMin;
} Font_Container;

#define BYTE uint8_t
#define WORD uint16_t
#define DWORD uint32_t
#define UINT unsigned int
#define SHORT short
#define USHORT unsigned short
#define GDI_ERROR ~0u

#define MS_MAKE_TAG(ch0, ch1, ch2, ch3)                                                 \
                    ((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) |       \
                    ((uint32_t)(uint8_t)(ch2) << 16) | ((uint32_t)(uint8_t)(ch3) << 24))
#define MS_VDMX_TAG MS_MAKE_TAG('V', 'D', 'M', 'X')

/* Wine's code suggests the tables are stored in big endian format. */
#define RTLUSHORTBYTESWAP(x) (uint16_t)((x >> 8) | (x << 8))
#define RTLULONGBYTESWAP(x) (((uint32_t)RTLUSHORTBYTESWAP((uint16_t)x) << 16) | RTLUSHORTBYTESWAP((uint16_t)(x >> 16)))

#if SDL_BYTEORDER == SDL_BIG_ENDIAN
#define GET_BE_WORD(x) (x)
#else
#define GET_BE_WORD(x) RTLUSHORTBYTESWAP(x)
#endif

static unsigned int freetype_get_font_data( Font_Container *font, uint32_t table,
                                            unsigned int offset, void *buf, unsigned int cbData)
{
	FT_Face ft_face = *(reinterpret_cast<FT_Face *>( font->font ));
	FT_ULong len;
	FT_Error err;

	if (!FT_IS_SFNT(ft_face)) return GDI_ERROR;

	if(!buf)
		len = 0;
	else
		len = cbData;

	/* MS tags differ in endianness from FT ones */
	table = RTLULONGBYTESWAP( table );

	/* make sure value of len is the value freetype says it needs */
	if (buf && len)
	{
		FT_ULong needed = 0;
		err = FT_Load_Sfnt_Table(ft_face, table, offset, NULL, &needed);
		if(!err && needed < len)
			len = needed;
	}
	err = FT_Load_Sfnt_Table(ft_face, table, offset, (FT_Byte*)buf, &len);
	if (err) /* Can't find table */
		return GDI_ERROR;
	return (int)len;
}

typedef struct {
	uint16_t version;
	uint16_t numRecs;
	uint16_t numRatios;
} VDMX_Header;

typedef struct {
	uint8_t bCharSet;
	uint8_t xRatio;
	uint8_t yStartRatio;
	uint8_t yEndRatio;
} Ratios;

typedef struct {
	uint16_t recs;
	uint8_t startsz;
	uint8_t endsz;
} VDMX_group;

typedef struct {
	uint16_t yPelHeight;
	uint16_t yMax;
	uint16_t yMin;
} VDMX_vTable;

static int load_VDMX(Font_Container *font, int height)
{
	VDMX_Header hdr;
	VDMX_group group;
	uint8_t devXRatio, devYRatio;
	unsigned short numRatios;
	unsigned int result, offset = -1;
	int i, ppem = 0;

	result = freetype_get_font_data(font, MS_VDMX_TAG, 0, &hdr, sizeof(hdr));

	if(result == GDI_ERROR) /* no vdmx table present, use linear scaling */
		return ppem;

	/* FIXME: need the real device aspect ratio */
	devXRatio = 1;
	devYRatio = 1;

	numRatios = GET_BE_WORD(hdr.numRatios);

	for(i = 0; i < numRatios; i++) {
		Ratios ratio;

		offset = sizeof(hdr) + (i * sizeof(Ratios));
		freetype_get_font_data(font, MS_VDMX_TAG, offset, &ratio, sizeof(Ratios));
		offset = -1;

		if (!ratio.bCharSet)
			continue;

		if((ratio.xRatio == 0 &&
			ratio.yStartRatio == 0 &&
			ratio.yEndRatio == 0) ||
		   (devXRatio == ratio.xRatio &&
			devYRatio >= ratio.yStartRatio &&
			devYRatio <= ratio.yEndRatio))
		{
			uint16_t group_offset;

			offset = sizeof(hdr) + numRatios * sizeof(ratio) + i * sizeof(group_offset);
			freetype_get_font_data(font, MS_VDMX_TAG, offset, &group_offset, sizeof(group_offset));
			offset = GET_BE_WORD(group_offset);
			break;
		}
	}

	if(offset == -1) return 0;

	if(freetype_get_font_data(font, MS_VDMX_TAG, offset, &group, sizeof(group)) != GDI_ERROR) {
		uint16_t recs;
		std::vector<VDMX_vTable> vTable;

		recs = GET_BE_WORD(group.recs);

		vTable.resize(recs);
		result = freetype_get_font_data(font, MS_VDMX_TAG, offset + sizeof(group), &vTable[0], recs * sizeof(VDMX_vTable));
		if(result == GDI_ERROR) /* Failed to retrieve vTable */
			return 0;

		for(i = 0; i < recs; i++) {
			VDMX_vTable &entry = vTable[i];
			short yMax = GET_BE_WORD(entry.yMax);
			short yMin = GET_BE_WORD(entry.yMin);
			ppem = GET_BE_WORD(entry.yPelHeight);

			if(yMax + -yMin == height) {
				font->yMax = yMax;
				font->yMin = yMin;
				break;
			}
			if(yMax + -yMin > height) {
				if(--i < 0) {
					ppem = 0;
					return 0; /* failed */
				}
				VDMX_vTable &entry = vTable[i];
				font->yMax = GET_BE_WORD(entry.yMax);
				font->yMin = GET_BE_WORD(entry.yMin);
				ppem = GET_BE_WORD(entry.yPelHeight);
				break;
			}
		}
		if(!font->yMax) /* ppem not found for height */
			ppem = 0;
	}

	return ppem;
}

/* Some fonts have large usWinDescent values, as a result of storing signed short
   in unsigned field. That's probably caused by sTypoDescent vs usWinDescent confusion in
   some font generation tools. */
static inline USHORT get_fixed_windescent(USHORT windescent)
{
    return abs((SHORT)windescent);
}

static int calc_ppem_for_height(Font_Container *font, int height)
{
	FT_Face ft_face = *(reinterpret_cast<FT_Face *>( font->font ));
	TT_OS2 *pOS2;
	TT_HoriHeader *pHori;

	int ppem;
	const int MAX_PPEM = (1 << 16) - 1;

	pOS2 = (TT_OS2 *)FT_Get_Sfnt_Table(ft_face, FT_SFNT_OS2);
	pHori = (TT_HoriHeader *)FT_Get_Sfnt_Table(ft_face, FT_SFNT_HHEA);

	if(height == 0)
		height = 16;

	/* Calc. height of EM square:
	 *
	 * For +ve lfHeight we have
	 * lfHeight = (winAscent + winDescent) * ppem / units_per_em
	 * Re-arranging gives:
	 * ppem = units_per_em * lfheight / (winAscent + winDescent)
	 *
	 * For -ve lfHeight we have
	 * |lfHeight| = ppem
	 * [i.e. |lfHeight| = (winAscent + winDescent - il) * ppem / units_per_em
	 * with il = winAscent + winDescent - units_per_em]
	 *
	 */

	if(height > 0) {
		USHORT windescent = get_fixed_windescent(pOS2->usWinDescent);
		int units;

		if(pOS2->usWinAscent + windescent == 0)
		{
			font->yMax = pHori->Ascender;
			font->yMin = pHori->Descender;
			units = pHori->Ascender - pHori->Descender;
		} else {
			font->yMax = pOS2->usWinAscent;
			font->yMin = -windescent;
			units = pOS2->usWinAscent + windescent;
		}
		ppem = (int)FT_MulDiv(ft_face->units_per_EM, height, units);

		/* If rounding ends up getting a font exceeding height, choose a smaller ppem */
		if(ppem > 1 && FT_MulDiv(units, ppem, ft_face->units_per_EM) > height)
			--ppem;

		if(ppem > MAX_PPEM) {
			//WARN("Ignoring too large height %d, ppem %d\n", height, ppem);
			ppem = 1;
		}
	}
	else if(height >= -MAX_PPEM)
		ppem = -height;
	else {
		//WARN("Ignoring too large height %d\n", height);
		ppem = 1;
	}

	return ppem;
}
/* /wine */

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)

/* Largest font this build reads into memory in one piece. The biggest font
 * any RTP ships is VL Gothic at about 5 MiB, and Blank Dream's own
 * cinecaption2.28.ttf is 1.7 MiB, so 8 MiB covers the corpus with room to
 * spare while still bounding the extra resident copy per open font. Left
 * overridable for host-compilable builds.
 * SDL_RWFromConstMem takes an int length, hence the upper bound. */
#ifndef MKXPZ_VITA_FONT_SLURP_MAX
#define MKXPZ_VITA_FONT_SLURP_MAX (8 * 1024 * 1024)
#endif
/* Every pooled block lives until shutdown, and past the 64-path reuse index
 * each new size of a font slurps its own copy; past this many pooled bytes a
 * font streams instead. Three fonts at the per-file cap. */
#ifndef MKXPZ_VITA_FONT_SLURP_TOTAL
#define MKXPZ_VITA_FONT_SLURP_TOTAL (24 * 1024 * 1024)
#endif
#include "fontres-telemetry.h"
#ifndef MKXPZ_VITA_FONT_REUSE
#define MKXPZ_VITA_FONT_REUSE 1
#endif


/* Swap a live PhysFS font stream for one the face can read without touching
 * ux0: again.
 *
 * FreeType reads a font lazily for the whole life of the face: hmtx, loca and
 * glyf are three separate seek-and-read pairs per first-time glyph, and down
 * on the device each pair is an sceIoLseek plus an sceIoRead. Buffering the
 * stream is no answer -- the three tables sit tens to hundreds of kilobytes
 * apart, so any buffer smaller than the file just thrashes -- so the file is
 * read once, here, into one block.
 *
 * On success the returned RWops reads from *bufOut, `ops` has been closed,
 * and the caller owns *bufOut until the font opened from it reaches the pool.
 * On any refusal -- no length, over the cap, out of memory, a short read, no
 * RWops -- *bufOut stays 0 and `ops` comes back open and rewound, so the
 * caller streams exactly as it did before. */
static SDL_RWops *vitaSlurpFont(SDL_RWops *ops, const char *name, void **bufOut,
                                size_t pooledBytes)
{
	static_assert(MKXPZ_VITA_FONT_SLURP_MAX > 0 &&
	              MKXPZ_VITA_FONT_SLURP_MAX <= 0x7fffffff,
	              "MKXPZ_VITA_FONT_SLURP_MAX must fit SDL_RWFromConstMem's int");

	*bufOut = 0;

	const Sint64 size = SDL_RWsize(ops);
	size_t got = 0;

	if (size > 0 && size <= MKXPZ_VITA_FONT_SLURP_MAX &&
	    pooledBytes <= MKXPZ_VITA_FONT_SLURP_TOTAL &&
	    (size_t)size <= MKXPZ_VITA_FONT_SLURP_TOTAL - pooledBytes)
	{
		void *buf = SDL_malloc((size_t)size);

		if (buf)
		{
			while (got < (size_t)size)
			{
				const size_t n = SDL_RWread(ops, (Uint8*)buf + got, 1,
				                            (size_t)size - got);

				if (n == 0)
					break;

				got += n;
			}

			if (got == (size_t)size)
			{
				if (g_fontRes.active) {
					g_fontRes.cur.slurp_reason = 0;
					g_fontRes.cur.slurp_bytes = (unsigned long)size;
				}
				SDL_RWops *mem = SDL_RWFromConstMem(buf, (int)size);

				if (mem)
				{
					SDL_RWclose(ops);
					*bufOut = buf;

					return mem;
				}
			}

			SDL_free(buf);
		}
	}

	/* Streaming after all. Whatever was consumed has to be handed back:
	 * TTF_OpenFontRW reads the face from wherever the stream now stands. */
	if (got != 0)
		SDL_RWseek(ops, 0, RW_SEEK_SET);
	if (g_fontRes.active) {
		g_fontRes.cur.slurp_reason = 1;
		g_fontRes.cur.slurp_bytes = (unsigned long)size;
	}

	Debug() << "font streamed '" + std::string(name ? name : "") + "' ("
	           + std::to_string((long long)size) + ")";

	return ops;
}

#endif

_TTF_Font *SharedFontState::getFont(std::string family,
                                    int size, float hiresMult, int outline_size)
{
	std::transform(family.begin(), family.end(), family.begin(),
		[](unsigned char c){ return std::tolower(c); });

	if (family.empty())
		family = p->defaultFamily;

	/* Find out which font asset actually answers this request: the family
	 * itself when it exists, else the first existing substitution target,
	 * else "" -- the built-in font. */
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	const std::string requested = family;
#endif
	family = resolveFamily(p, family);

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* One line per requested family, the first time it is asked for
	 * Which face a game actually got is otherwise invisible:
	 * the registry is built from whatever happened to be mounted, the
	 * request is rewritten by the table, and nothing downstream says which
	 * file was opened. `requested` is the lower-cased name after the empty
	 * -> default_font_family step, i.e. the key resolution ran on.
	 *
	 * The subscript is safe: a non-empty answer from resolveFamily is
	 * always a family already in `sets`. The built-in face is always
	 * Liberation Sans. */
	if (!p->loggedFamilies.contains(requested))
	{
		p->loggedFamilies.insert(requested);

		if (family.empty())
			Debug() << "font: '" + requested + "' -> built-in Liberation Sans";
		else
			Debug() << "font: '" + requested + "' -> '" + family + "' ("
			           + p->sets[family]->c_str() + ")";
	}
#endif

	FontSizeKey key(family, size);

	TTF_Font *font;
	int &ppem = p->size_to_ppem[key];
	int ppemMult;
	
	if (ppem != 0)
	{
		ppemMult = std::max<int>(ppem * hiresMult, 1);
		auto &group = p->ppem_to_font[FontPPEMKey(family, ppemMult)];
		if(outline_size == 0)
			font = group[0];
		else
			font = group[1];
		
		if (font)
		{
			if(outline_size && TTF_GetFontOutline(font) != outline_size)
				TTF_SetFontOutline(font, outline_size);
			return font;
		}
	}
	
	/* Not in pool; open new handle */
	SDL_RWops *ops;

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	frBegin(family.c_str(), size, outline_size);
	g_fontRes.cur.copies = (unsigned)p->fontBuffers.size();
#endif
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* Bytes behind `ops` when it is a memory stream. Owned here until the
	 * font reaches the pool below, and freed here if it never does. */
	void *slurped = 0;
	int slurpedSize = 0;
#endif

	if (family.empty())
	{
		/* Built-in font */
		ops = openBundledFont();
	}
	else
	{
		/* Use 'other' path as alternative in case
		 * we have no 'regular' styled font asset.
		 * resolveFamily() only returns a non-empty name for a family that
		 * is already in the map, so this operator[] cannot grow it. */
		const FontSet &req = p->sets[family];
		const char *path = req->c_str();

		bool cached = false;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		if (MKXPZ_VITA_FONT_REUSE)
			for (const auto &entry : p->fontBytesByPath)
				if (entry.first == path) {
					ops = SDL_RWFromConstMem(entry.second.first, entry.second.second);
					cached = true;
					break;
				}
#endif
		if (!cached) ops = SDL_AllocRW();
		if (!ops)
		{
			p->size_to_ppem.remove(key);
			throw Exception(Exception::SDLError, "%s", SDL_GetError());
		}
		if (!cached) {
			try{
				shState->fileSystem().openReadRaw(*ops, path, true);
			} catch (const Exception &e) {
				SDL_FreeRW(ops);
				p->size_to_ppem.remove(key);
				throw e;
			}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
			/* The bundled font above is already memory-backed; this is the only
			 * branch that hands FreeType a device stream. */
			ops = vitaSlurpFont(ops, path, &slurped, p->fontBufferBytes);
			if (slurped) slurpedSize = (int)SDL_RWsize(ops);
			if (g_fontRes.active) {
				g_fontRes.cur.copy_bytes = slurped ? g_fontRes.cur.slurp_bytes : 0;
				if (slurped)
					g_fontRes.cur.copies = (unsigned)p->fontBuffers.size() + 1;
			}
#endif
		}
	}

	/* Try to compute the size the same way Windows does. */
	font = TTF_OpenFontRW(ops, 1, 0);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	if (g_fontRes.active) {
		frHeap(&g_fontRes.cur.h1);
		g_fontRes.cur.stage = FR_TTF;
		if (!font)
			frAttribute(slurped, slurped ? (size_t)g_fontRes.cur.slurp_bytes : 0);
	}
#endif

	if (font)
	{
		FT_Face face = TTF_FONT_TO_FT_FACE(font);
		/* This is should always be true, but we may as well check... */
		if (FT_IS_SCALABLE( face ))
		{
			if (ppem == 0)
			{
				Font_Container c = { 0 };
				c.font = font;
				c.ppem = load_VDMX(&c, size);
				if (!c.ppem)
					c.ppem = calc_ppem_for_height( &c, size );

				ppem = std::max<int>(c.ppem * p->fontScale, 1);
				ppemMult = std::max<int>(ppem * hiresMult, 1);
			}
			if (TTF_SetFontSize(font, ppemMult))
			{
				TTF_CloseFont(font);
				font = 0;
			}
		} else {
			/* Someone must have renamed a non-scalable font file to ttf or otf.
			 * Wine has a scaling setup for these, but I'll just fall back to
			 * the mkxp method for now. */
			if (ppem == 0)
			{
				ppem = std::max<int>(size * p->fontScale, 5);
				ppemMult = std::max<int>(ppem * hiresMult, 1);
			}
			if (TTF_SetFontSize(font, ppemMult))
			{
				TTF_CloseFont(font);
				font = 0;
			}
		}
		if (font)
		{
			/* RGSS doesn't use font hinting */
			TTF_SetFontHinting(font, p->fontHinting);
		}
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		if (g_fontRes.active) frHeap(&g_fontRes.cur.h2);
#endif
	}
	
	if (!font)
	{
		p->size_to_ppem.remove(key);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		/* No pool entry ever took these over, and freesrc released the RWops
		 * that wrapped them, not the block itself. */
		SDL_free(slurped);
		frFinish(FR_REFUSED, 0, 0);
#endif
		throw Exception(Exception::SDLError, "%s", SDL_GetError());
	}
	
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	if (g_fontRes.active) frHeap(&g_fontRes.cur.h3);
#endif
	auto &group = p->ppem_to_font[FontPPEMKey(family, std::max<int>(ppem * hiresMult, 1))];
	/* An uncached requested size can resolve to an already pooled ppem.
	 * Existing Font wrappers still own references to that slot's font. */
	TTF_Font *pooled = group[outline_size != 0];
	if (pooled)
	{
		TTF_CloseFont(font);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		SDL_free(slurped);
#endif
		if (outline_size && TTF_GetFontOutline(pooled) != outline_size)
			TTF_SetFontOutline(pooled, outline_size);
		return pooled;
	}
	if(outline_size == 0)
	{
		group[0] = font;
	} else {
		if(TTF_GetFontOutline(font) != outline_size)
			TTF_SetFontOutline(font, outline_size);
		group[1] = font;
	}
	
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* The font is pooled now, so its bytes belong to the pool. (This grows a
	 * vector next to the map the line above just grew; both are out of memory
	 * together or neither is.) */
	if (slurped) {
		p->fontBuffers.push_back(slurped);
		p->fontBufferBytes += (size_t)slurpedSize;
		const std::string &path = *p->sets[family].operator->();
		if (MKXPZ_VITA_FONT_REUSE && p->fontBytesByPath.size() < 64 && path.size() < 512) {
			try { p->fontBytesByPath.push_back({path, {slurped, slurpedSize}}); }
			catch (const std::bad_alloc &) {} // Optional index; the pool still owns the bytes.
		}
	}
#endif
	
	if (!p->fontKerning)
		TTF_SetFontKerning(font, 0);
	
	return font;
}

bool SharedFontState::fontPresent(std::string family) const
{
	std::transform(family.begin(), family.end(), family.begin(),
		[](unsigned char c){ return std::tolower(c); });

	return !resolveFamily(p, family).empty();
}

_TTF_Font *SharedFontState::openBundled(int size)
{
	SDL_RWops *ops = openBundledFont();

	return TTF_OpenFontRW(ops, 1, size);
}

void SharedFontState::setDefaultFontFamily(const std::string &family) {
    /* Every lookup lower-cases its request and the registry is keyed
     * lower-cased, so a mixed-case default stored verbatim could never match
     * an installed family. */
    p->defaultFamily = family;
    std::transform(p->defaultFamily.begin(), p->defaultFamily.end(),
                   p->defaultFamily.begin(),
                   [](unsigned char c){ return std::tolower(c); });
}

static bool pickExistingFontName(const std::vector<std::string> &names,
                          std::string &out,
                          const SharedFontState &sfs)
{
	/* Note: In RMXP, a names array with no existing entry
	 * results in no text being drawn at all (same for "" and []);
	 * we can't replicate this in mkxp due to the default substitute. */

	for (size_t i = 0; i < names.size(); ++i)
	{
		if (sfs.fontPresent(names[i]))
		{
			if (out == names[i])
				return false;
			out = names[i];
			return true;
		}
		else
		{
			if (i == 0)
			{
				Debug() << "Primary font not found:" << names[i];
			}
			else
			{
				Debug() << "Fallback font not found:" << names[i];
			}
		}
	}

	if (out[0] == '\0')
		return false;
	out = "";
	return true;
}


struct FontPrivate
{
	std::string name;
	int size;
	float hiresMult;
	bool bold;
	bool italic;
	bool outline;
	bool shadow;
	Color *color;
	Color *outColor;

	Color colorTmp;
	Color outColorTmp;

	static std::string defaultName;
	static int defaultSize;
	static bool defaultBold;
	static bool defaultItalic;
	static bool defaultOutline;
	static bool defaultShadow;
	static Color *defaultColor;
	static Color *defaultOutColor;

	static Color defaultColorTmp;
	static Color defaultOutColorTmp;

	static std::vector<std::string> initialDefaultNames;

	/* The actual font is opened as late as possible
	 * (when it is queried by a Bitmap), prior it is
	 * set to null */
	TTF_Font *sdlFont;
	TTF_Font *sdlFontOutline;
    
    bool isSolid;

	FontPrivate(int size)
	    : size(size),
	      hiresMult(1.0f),
	      bold(defaultBold),
	      italic(defaultItalic),
	      outline(defaultOutline),
	      shadow(defaultShadow),
	      color(&colorTmp),
	      outColor(&outColorTmp),
	      colorTmp(*defaultColor),
	      outColorTmp(*defaultOutColor),
	      sdlFont(0),
	      sdlFontOutline(0),
          isSolid(false)
	{}

	FontPrivate(const FontPrivate &other)
	    : name(other.name),
	      size(other.size),
	      hiresMult(1.0f),
	      bold(other.bold),
	      italic(other.italic),
	      outline(other.outline),
	      shadow(other.shadow),
	      color(&colorTmp),
	      outColor(&outColorTmp),
	      colorTmp(*other.color),
	      outColorTmp(*other.outColor),
	      sdlFont(other.sdlFont),
	      sdlFontOutline(other.sdlFontOutline),
          isSolid(false)
	{}

	void operator=(const FontPrivate &o)
	{
		if (size != o.size || name != o.name)
		{
			sdlFont = 0;
			sdlFontOutline = 0;
		}
		if (hiresMult == o.hiresMult)
		{
			sdlFont = sdlFont == 0 ? o.sdlFont : sdlFont;
			sdlFontOutline = sdlFontOutline == 0 ? o.sdlFontOutline : sdlFontOutline;
		}

		 name     =  o.name;
		 size     =  o.size;
		 bold     =  o.bold;
		 italic   =  o.italic;
		 outline  =  o.outline;
		 shadow   =  o.shadow;
		*color    = *o.color;
		*outColor = *o.outColor;

        isSolid = o.isSolid;
	}
};

std::string FontPrivate::defaultName     = "Arial";
int         FontPrivate::defaultSize     = 22;
bool        FontPrivate::defaultBold     = false;
bool        FontPrivate::defaultItalic   = false;
bool        FontPrivate::defaultOutline  = false; /* Inited at runtime */
bool        FontPrivate::defaultShadow   = false; /* Inited at runtime */
Color      *FontPrivate::defaultColor    = &FontPrivate::defaultColorTmp;
Color      *FontPrivate::defaultOutColor = &FontPrivate::defaultOutColorTmp;

Color FontPrivate::defaultColorTmp(255, 255, 255, 255);
Color FontPrivate::defaultOutColorTmp(0, 0, 0, 128);

std::vector<std::string> FontPrivate::initialDefaultNames;

bool Font::isSolid() const {
    return p->isSolid;
}

bool Font::doesExist(const char *name)
{
	if (!name)
		return false;

	return shState->fontState().fontPresent(name);
}

Font::Font(const std::vector<std::string> *names,
           int size)
{
	p = new FontPrivate(size ? size : FontPrivate::defaultSize);

	if (names)
		setName(*names);
	else
		p->name = FontPrivate::defaultName;
}

Font::Font(const Font &other)
{
	p = new FontPrivate(*other.p);
}

Font::~Font()
{
	delete p;
}

const Font &Font::operator=(const Font &o)
{
	*p = *o.p;

	return o;
}

void Font::setName(const std::vector<std::string> &names)
{
	if (pickExistingFontName(names, p->name, shState->fontState()))
	{
		p->sdlFont = 0;
		p->sdlFontOutline = 0;
	}
	p->isSolid = strcmp(p->name.c_str(), "") && shState->config().fontIsSolid(p->name.c_str());
}

void Font::setSize(int value, bool checkIllegal)
{
	if (p->size == value)
		return;

	/* Catch illegal values (according to RMXP) */
	if (value < 6 || value > 96) {
		if (checkIllegal) {
			throw Exception(Exception::ArgumentError, "%s", "bad value for size");
		}
	}

	p->size = value;
	p->sdlFont = 0;
	p->sdlFontOutline = 0;
}

void Font::setHiresMult(float value)
{
	if (p->hiresMult == value)
		return;

	p->hiresMult = value;
	p->sdlFont = 0;
	p->sdlFontOutline = 0;
}

static void guardDisposed() {}

DEF_ATTR_RD_SIMPLE(Font, Size, int, p->size)

DEF_ATTR_SIMPLE(Font, Bold,     bool,    p->bold)
DEF_ATTR_SIMPLE(Font, Italic,   bool,    p->italic)
DEF_ATTR_SIMPLE(Font, Shadow,   bool,    p->shadow)
DEF_ATTR_SIMPLE(Font, Outline,  bool,    p->outline)
DEF_ATTR_SIMPLE(Font, Color,    Color&, *p->color)
DEF_ATTR_SIMPLE(Font, OutColor, Color&, *p->outColor)

DEF_ATTR_SIMPLE_STATIC(Font, DefaultSize,     int,     FontPrivate::defaultSize)
DEF_ATTR_SIMPLE_STATIC(Font, DefaultBold,     bool,    FontPrivate::defaultBold)
DEF_ATTR_SIMPLE_STATIC(Font, DefaultItalic,   bool,    FontPrivate::defaultItalic)
DEF_ATTR_SIMPLE_STATIC(Font, DefaultShadow,   bool,    FontPrivate::defaultShadow)
DEF_ATTR_SIMPLE_STATIC(Font, DefaultOutline,  bool,    FontPrivate::defaultOutline)
DEF_ATTR_SIMPLE_STATIC(Font, DefaultColor,    Color&, *FontPrivate::defaultColor)
DEF_ATTR_SIMPLE_STATIC(Font, DefaultOutColor, Color&, *FontPrivate::defaultOutColor)

void Font::setDefaultName(const std::vector<std::string> &names,
                          const SharedFontState &sfs)
{
	pickExistingFontName(names, FontPrivate::defaultName, sfs);
}

const std::vector<std::string> &Font::getInitialDefaultNames()
{
	return FontPrivate::initialDefaultNames;
}

void Font::initDynAttribs()
{
	p->color = new Color(p->colorTmp);

	if (rgssVer >= 3)
		p->outColor = new Color(p->outColorTmp);;
}

void Font::initDefaultDynAttribs()
{
	FontPrivate::defaultColor = new Color(FontPrivate::defaultColorTmp);

	if (rgssVer >= 3)
		FontPrivate::defaultOutColor = new Color(FontPrivate::defaultOutColorTmp);
}

void Font::initDefaults(const SharedFontState &sfs)
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	vita_glue_trace("trace: Font::initDefaults enter");
#endif
	std::vector<std::string> &names = FontPrivate::initialDefaultNames;

	switch (rgssVer)
	{
	case 1 :
		// FIXME: Japanese version has "MS PGothic" instead
		names.push_back("Arial");
		break;

	case 2 :
		names.push_back("UmePlus Gothic");
		names.push_back("MS Gothic");
		names.push_back("Courier New");
		FontPrivate::defaultSize = 20;
		break;

	default:
	case 3 :
		names.push_back("VL Gothic");
		FontPrivate::defaultSize = 24;
	}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	vita_glue_trace("trace: Font::initDefaults setDefaultName");
#endif
	setDefaultName(names, sfs);

	FontPrivate::defaultOutline = (rgssVer >= 3 ? true : false);
	FontPrivate::defaultShadow  = (rgssVer == 2 ? true : false);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	vita_glue_trace("trace: Font::initDefaults leave");
#endif
}

_TTF_Font *Font::getSdlFont(int outline_size)
{
	_TTF_Font **font;
	if (outline_size == 0)
		font = &p->sdlFont;
	else
		font = &p->sdlFontOutline;

	if (!*font)
		*font = shState->fontState().getFont(p->name.c_str(),
		                                     p->size, p->hiresMult, outline_size);

	if(outline_size && TTF_GetFontOutline(*font) != outline_size)
		TTF_SetFontOutline(*font, outline_size);

	int style = TTF_STYLE_NORMAL;

	if (p->bold)
		style |= TTF_STYLE_BOLD;

	if (p->italic)
		style |= TTF_STYLE_ITALIC;

	TTF_SetFontStyle(*font, style);

	return *font;
}
