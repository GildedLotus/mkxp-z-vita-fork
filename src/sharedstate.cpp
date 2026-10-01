/*
** sharedstate.cpp
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

#include "bootprofile.h"

#include "sharedstate.h"
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "frameprofile.h"
#endif

#include "util.h"
#include "filesystem.h"
#include "graphics.h"
#include "input.h"
#include "audio.h"
#include "glstate.h"
#include "shader.h"
#include "texpool.h"
#include "font.h"
#include "eventthread.h"
#include "gl-util.h"
#include "global-ibo.h"
#include "quad.h"
#include "binding.h"
#include "exception.h"
#include "sharedmidistate.h"

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "vita_glue.h"
#include "vita-rtp.h"
#endif

#include <unistd.h>
#include <stdio.h>
#include <string>
#include <chrono>

SharedState *SharedState::instance = 0;
int SharedState::rgssVersion = 0;
static GlobalIBO *_globalIBO = 0;

static const char *gameArchExt()
{
	if (rgssVer == 1)
		return ".rgssad";
	else if (rgssVer == 2)
		return ".rgss2a";
	else if (rgssVer == 3)
		return ".rgss3a";

	assert(!"unreachable");
	return 0;
}

struct SharedStatePrivate
{
	void *bindingData;
	SDL_Window *sdlWindow;
	Scene *screen;

	FileSystem fileSystem;

	EventThread &eThread;
	RGSSThreadData &rtData;
	Config &config;

	SharedMidiState midiState;

	Graphics graphics;
	Input input;
	Audio audio;

	GLState _glState;

	ShaderSet shaders;

	TexPool texPool;

	SharedFontState fontState;
	Font *defaultFont;

	TEX::ID globalTex;
	int globalTexW, globalTexH;
	bool globalTexDirty;

	TEXFBO gpTexFBO;

	TEXFBO atlasTex;

	Quad gpQuad;

	unsigned int stampCounter;
    
    std::chrono::time_point<std::chrono::steady_clock> startupTime;

	SharedStatePrivate(RGSSThreadData *threadData)
	    : bindingData(0),
	      sdlWindow(threadData->window),
	      fileSystem(threadData->argv0, threadData->config.allowSymlinks),
	      eThread(*threadData->ethread),
	      rtData(*threadData),
	      config(threadData->config),
	      midiState(threadData->config),
	      graphics(threadData),
	      input(*threadData),
	      audio(*threadData),
	      _glState(threadData->config),
	      fontState(threadData->config),
	      stampCounter(0)
	{}
	
	void init(RGSSThreadData *threadData)
	{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		vita_glue_trace("trace: SharedStatePrivate::init enter");
#endif
        startupTime = std::chrono::steady_clock::now();

		/* Shaders have been compiled in ShaderSet's constructor */
		if (gl.ReleaseShaderCompiler)
			gl.ReleaseShaderCompiler();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		vita_glue_trace("trace: SharedStatePrivate::init shaders compiled");
		shaderBootComplete();
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		/* Config::read already changed CWD into gameFolder. Resolving the
		 * configured value again would double a relative gameFolder.
		 * Newlib resolves relative fopen paths, but PhysFS calls sceIo
		 * directly, so give both the same absolute archive path. */
		const std::string gameRoot = mkxp_fs::getCurrentDirectory();
		std::string archPath = mkxp_fs::normalizePath(
		    (config.execName + gameArchExt()).c_str(), false, true);
#else
		std::string archPath = config.execName + gameArchExt();
#endif
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		{
			char tb[560];
			snprintf(tb, sizeof(tb), "trace: archive check '%s'",
			         archPath.c_str());
			vita_glue_trace(tb);
		}
#endif

		for (size_t i = 0; i < config.patches.size(); ++i)
			fileSystem.addPath(config.patches[i].c_str());

		/* Check if a game archive exists. The mount reads the RGSSAD index,
		 * a boot cost no other phase covered (Middens: ~12 s on device). */
		BootProfile::Scope bootArchive(BootProfile::Archive);
		FILE *tmp = fopen(archPath.c_str(), "rb");
		if (tmp)
		{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
			{
				char tb[560];
				snprintf(tb, sizeof(tb),
				         "trace: archive present '%s'; PHYSFS_mount next",
				         archPath.c_str());
				vita_glue_trace(tb);
			}
#endif
			fclose(tmp);
			fileSystem.addPath(archPath.c_str());
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
			{
				char tb[560];
				snprintf(tb, sizeof(tb), "trace: archive mounted '%s'",
				         archPath.c_str());
				vita_glue_trace(tb);
			}
#endif
		}
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		else
		{
			{
				char tb[560];
				snprintf(tb, sizeof(tb),
				         "trace: archive absent '%s' (loose game folder)",
				         archPath.c_str());
				vita_glue_trace(tb);
			}
		}
#endif
		bootArchive.finish();

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		{
			char tb[560];
			snprintf(tb, sizeof(tb), "trace: addPath gameFolder mount '%s'",
			         gameRoot.c_str());
			vita_glue_trace(tb);
		}
		fileSystem.addPath(gameRoot.c_str());
        FrameProfile::state.assets.mountGame(gameRoot.c_str());
#else
		fileSystem.addPath(".");
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		/* RTPs. Three things separate this from the stock
		 * loop in the #else branch:
		 *
		 *  - PhysFS here calls sceIo* directly and has no current directory,
		 *    so a relative entry could never resolve. Every candidate is made
		 *    absolute against the game folder (the CWD) first.
		 *  - addPath throws when the directory is not there, and a throw at
		 *    this point aborts the boot. An RTP that is not installed is a
		 *    missing-asset problem for the game to report, not a native exit.
		 *  - nothing in the log said which RTP was mounted.
		 *
		 * Config::read supplies installed defaults; customScript uses version 0
		 * to suppress automatic RTP selection. */
#if !defined(__vita__) && defined(MKXPZ_HOST_PORT_LOGIC)
#include "hf_f_paths.h"
#endif


		{
			const int rtpVersion =
			    config.customScript.empty() ? config.rgssVersion : 0;
			const std::vector<VitaRtp::Candidate> rtpCandidates =
			    VitaRtp::candidates(config.rtps, rtpVersion,
			                        MKXPZ_VITA_RTP_ROOT,
			                        [](const std::string &entry) {
				return mkxp_fs::normalizePath(entry.c_str(), false, true);
			});
			char tb[560];

			if (rtpCandidates.empty() && VitaRtp::disabled(config.rtps))
				vita_glue_trace("trace: RTP disabled by config");

            FrameProfile::state.assets.clearMounts();
			for (size_t i = 0; i < rtpCandidates.size(); ++i)
			{
				BootProfile::Scope bootRtp(BootProfile::RTP);
				const VitaRtp::Candidate &rtp = rtpCandidates[i];

				if (rtp.isDefault)
					snprintf(tb, sizeof(tb),
					         "trace: RTP candidate '%s' (default for RGSS%d)",
					         rtp.path.c_str(), rtpVersion);
				else
					snprintf(tb, sizeof(tb),
					         "trace: RTP candidate '%s' (explicit)",
					         rtp.path.c_str());
				vita_glue_trace(tb);

				try
				{
					fileSystem.addPath(rtp.path.c_str());
                    if (vita_glue_frame_profile_interval)
                        FrameProfile::state.assets.mountRTP(rtp.path.c_str());
					snprintf(tb, sizeof(tb), "trace: RTP mounted '%s'",
					         rtp.path.c_str());
					vita_glue_trace(tb);
				}
				catch (const Exception &e)
				{
					snprintf(tb, sizeof(tb),
					         "trace: RTP absent '%s' (%s); continuing without it",
					         rtp.path.c_str(), e.msg.c_str());
					vita_glue_trace(tb);
				}
			}
		}
#else
		for (size_t i = 0; i < config.rtps.size(); ++i)
			fileSystem.addPath(config.rtps[i].c_str());
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		/* Bundled fallback fonts.
		 *
		 * Everything mounted above is the game's: the patches, the game
		 * archive, the game folder, the RTPs. A game that ships no Fonts/ of
		 * its own therefore had exactly one face -- the Latin-only
		 * Liberation compiled into the binary -- so every Japanese string
		 * rendered as tofu, with no warning, on a device whose corpus is
		 * largely Japanese.
		 *
		 * Both directories mount AT "Fonts", which is the directory
		 * FileSystem::initFontSets looks for (case-insensitively) directly
		 * under the PhysFS root; the faces inside are then registered by
		 * face name exactly like a game's own. app0:/ itself is deliberately
		 * NOT mounted -- it also carries the Ruby wrappers, the preload
		 * scripts and the shader cache, and merging those into the game's
		 * namespace would let them answer a game's file lookups.
		 *
		 * Appended LAST on purpose, after the RTP branch above. PhysFS searches mounts in mount order and
		 * initFontSetCB keeps the first file it sees for a family, so a
		 * game's Fonts/ and an RTP's Fonts/ still win every name clash
		 * against the bundle. ux0: precedes app0: for the same reason: a
		 * card copy is how a user adds or replaces a fallback face without
		 * repacking the VPK.
		 *
		 * Neither directory has to exist -- a missing ux0:/data/mkxp-z/fonts
		 * is the normal case -- so a refused mount is reported and stepped
		 * over so missing optional fonts cannot abort startup. */
#if !defined(__vita__) && defined(MKXPZ_HOST_PORT_LOGIC)
		{
			static const char *const fontDirs[] = {
				MKXPZ_HOST_CARD_FONTS, MKXPZ_HOST_PACKAGE_FONTS };
#else
		/* Keep device declarations on their original lines for DWARF. */

		{
			static const char *const fontDirs[] = {
				"ux0:/data/mkxp-z/fonts",
				"app0:/fonts",
			};
#endif
			for (size_t i = 0; i < sizeof(fontDirs) / sizeof(fontDirs[0]); ++i)
			{
				char tb[560];

				try
				{
					fileSystem.addPath(fontDirs[i], "Fonts");
					snprintf(tb, sizeof(tb), "fonts: mounted %s at Fonts",
					         fontDirs[i]);
				}
				catch (const Exception &e)
				{
					/* Exception::msg is a fixed 512-byte buffer with the
					 * text NUL-terminated inside it, so it has to be read as
					 * a C string; the std::string carries the padding. */
					snprintf(tb, sizeof(tb), "fonts: not mounted %s (%s)",
					         fontDirs[i], e.msg.c_str());
				}

				vita_glue_trace(tb);
			}
		}
#endif

		if (config.pathCache)
			fileSystem.createPathCache();

		{
			BootProfile::Scope bootFonts(BootProfile::Fonts);
			fileSystem.initFontSets(fontState);
		}

#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* globalTex and gpTexFBO serve only the stock GPU blit paths
		 * (Bitmap::stretchBlt's two GPU routes, the stock tilemap atlas),
		 * which this backend compiles out, so neither is
		 * allocated. SharedState::gpTexFBO() aborts loudly if anything asks. */
#else
		globalTexW = 128;
		globalTexH = 64;

		globalTex = TEX::gen();
		TEX::bind(globalTex);
		TEX::setRepeat(false);
		TEX::setSmooth(false);
		TEX::allocEmpty(globalTexW, globalTexH);
		globalTexDirty = false;

		TEXFBO::init(gpTexFBO);
		/* Reuse starting values */
		TEXFBO::allocEmpty(gpTexFBO, globalTexW, globalTexH);
		TEXFBO::linkFBO(gpTexFBO);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		vita_glue_trace("trace: SharedStatePrivate::init gpTexFBO ok");
#endif
#endif

		/* RGSS3 games will call setup_midi, so there's
		 * no need to do it on startup */
		if (rgssVer <= 2)
			midiState.initIfNeeded(threadData->config);
	}

	~SharedStatePrivate()
	{
#ifndef MKXPZ_SOFTWARE_BITMAPS
		TEX::del(globalTex);
		TEXFBO::fini(gpTexFBO);
#endif
#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* The recycled atlas is texture-only under this backend. */
		TEX::del(atlasTex.tex);
#else
		TEXFBO::fini(atlasTex);
#endif
	}
};

void SharedState::initInstance(RGSSThreadData *threadData)
{
	/* This section is tricky because of dependencies:
	 * SharedState depends on GlobalIBO existing,
	 * Font depends on SharedState existing */

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	vita_glue_trace("trace: SharedState::initInstance enter");
#endif
	rgssVersion = threadData->config.rgssVersion;

#ifdef MKXPZ_SOFTWARE_BITMAPS
	/* The FIRST thing this process asks the GL driver
	 * for is the engine's complete, fixed set of render surfaces -- before
	 * the global IBO, before ShaderSet compiles a program, before the first
	 * Quad's VBO, and long before a game asset. Surfaces, textures and VBOs
	 * come from shared fixed pools, first come first served, and only
	 * surfaces fail hard when a pool runs out. Reserving first is the whole strategy; a
	 * failure here is a clean fatal error with a named surface instead of a
	 * data abort inside the first map load.
	 *
	 * instance is nulled first because initChecked resyncs the GLState clear
	 * colour when there is a SharedState, and a stale pointer from a previous
	 * instance would be followed. The stock assignment below is left in
	 * place. */
	SharedState::instance = 0;
	GPUBudget::reserveFixedSurfaces(threadData->config, rgssVersion);
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	vita_glue_trace("trace: SharedState::initInstance new GlobalIBO");
#endif
	_globalIBO = new GlobalIBO();
	_globalIBO->ensureSize(1);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	vita_glue_trace("trace: SharedState::initInstance GlobalIBO ok");
#endif

	SharedState::instance = 0;
	Font *defaultFont = 0;

	try
	{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		vita_glue_trace("trace: SharedState::initInstance new SharedState");
#endif
		SharedState::instance = new SharedState(threadData);

#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* ShaderSet's constructor compiled and linked every
		 * program the engine can use, inside SharedStatePrivate above, and
		 * nothing compiles lazily (the optional shaders are off on Vita).
		 * Use each of them once now, one pixel per blend mode per kind
		 * of target, so the driver builds its per-program code variants
		 * while there are still sync objects to build them with. */
		GPUBudget::warmUpPrograms();
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		vita_glue_trace("trace: SharedState::initInstance Font::initDefaults");
#endif
		Font::initDefaults(instance->p->fontState);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
		vita_glue_trace("trace: SharedState::initInstance new Font");
#endif
		defaultFont = new Font();
	}
	catch (const Exception &exc)
	{
		delete _globalIBO;
		delete SharedState::instance;
		delete defaultFont;

		throw exc;
	}

	SharedState::instance->p->defaultFont = defaultFont;

#ifdef MKXPZ_SOFTWARE_BITMAPS
	/* Measure what is left of the firmware pool (diagnostic, and
	 * the only userland view of it), then close boot. From here on a new
	 * render surface, shader program or VAO is a bug, and the creation-site
	 * guards say so. */
	GPUBudget::headroomCanary();
	GPUBudget::seal();
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	vita_glue_trace("trace: SharedState::initInstance leave");
#endif
}

void SharedState::finiInstance()
{
	delete SharedState::instance->p->defaultFont;

	delete SharedState::instance;

	delete _globalIBO;
}

void SharedState::setScreen(Scene &screen)
{
	p->screen = &screen;
}

#define GSATT(type, lower) \
	type SharedState :: lower() const \
	{ \
		return p->lower; \
	}

GSATT(void*, bindingData)
GSATT(SDL_Window*, sdlWindow)
GSATT(Scene*, screen)
GSATT(FileSystem&, fileSystem)
GSATT(EventThread&, eThread)
GSATT(RGSSThreadData&, rtData)
GSATT(Config&, config)
GSATT(Graphics&, graphics)
GSATT(Input&, input)
GSATT(Audio&, audio)
GSATT(GLState&, _glState)
GSATT(ShaderSet&, shaders)
GSATT(TexPool&, texPool)
GSATT(Quad&, gpQuad)
GSATT(SharedFontState&, fontState)
GSATT(SharedMidiState&, midiState)

void SharedState::setBindingData(void *data)
{
	p->bindingData = data;
}

void SharedState::ensureQuadIBO(size_t minSize)
{
	_globalIBO->ensureSize(minSize);
}

GlobalIBO &SharedState::globalIBO()
{
	return *_globalIBO;
}

void SharedState::bindTex()
{
	TEX::bind(p->globalTex);

	if (p->globalTexDirty)
	{
		TEX::allocEmpty(p->globalTexW, p->globalTexH);
		p->globalTexDirty = false;
	}
}

void SharedState::ensureTexSize(int minW, int minH, Vec2i &currentSizeOut)
{
	if (minW > p->globalTexW)
	{
		p->globalTexDirty = true;
		p->globalTexW = findNextPow2(minW);
	}

	if (minH > p->globalTexH)
	{
		p->globalTexDirty = true;
		p->globalTexH = findNextPow2(minH);
	}

	currentSizeOut = Vec2i(p->globalTexW, p->globalTexH);
}

TEXFBO &SharedState::gpTexFBO(int minW, int minH)
{
#ifdef MKXPZ_SOFTWARE_BITMAPS
	/* No users left under this backend, and nothing was allocated for it in
	 * init() -- p->gpTexFBO is all zeros, so its .fbo is 0, which IS the
	 * screen. Returning it would silently draw the general-purpose blit into
	 * the window. Abort instead, loudly, naming the caller's request. */
	throw Exception(Exception::MKXPError,
	                "software_bitmaps: SharedState::gpTexFBO(%d, %d) has no "
	                "backing render surface under this backend; the CPU path "
	                "should have handled this",
	                minW, minH);
#else
	bool needResize = false;

	if (minW > p->gpTexFBO.width)
	{
		p->gpTexFBO.width = findNextPow2(minW);
		needResize = true;
	}

	if (minH > p->gpTexFBO.height)
	{
		p->gpTexFBO.height = findNextPow2(minH);
		needResize = true;
	}

	if (needResize)
	{
		TEX::bind(p->gpTexFBO.tex);
		TEX::allocEmpty(p->gpTexFBO.width, p->gpTexFBO.height);
	}

	return p->gpTexFBO;
#endif
}

void SharedState::requestAtlasTex(int w, int h, TEXFBO &out)
{
	TEXFBO tex;

	if (w == p->atlasTex.width && h == p->atlasTex.height)
	{
		tex = p->atlasTex;
		p->atlasTex = TEXFBO();
	}
	else
	{
#ifdef MKXPZ_SOFTWARE_BITMAPS
		/* Texture only, never a render target. Tile
		 * atlases are assembled on the CPU and uploaded whole, and render surfaces are a
		 * small fixed budget -- all of them spoken for by the time the first
		 * map loads. No storage here: softAtlasUpload's whole-level upload
		 * specifies it, and the tilemaps draw nothing until that succeeds.
		 * A zeroed level first cost a memset and a doubled peak. */
		tex.tex = TEX::gen();
		TEX::bind(tex.tex);
		TEX::setRepeat(false);
		TEX::setSmooth(false);
		tex.width = w;
		tex.height = h;
#else
		TEXFBO::init(tex);
		TEXFBO::allocEmpty(tex, w, h);
		TEXFBO::linkFBO(tex);
#endif
	}

	out = tex;
}

void SharedState::releaseAtlasTex(TEXFBO &tex)
{
	/* No point in caching an invalid object */
	if (tex.tex == TEX::ID(0))
		return;

#ifdef MKXPZ_SOFTWARE_BITMAPS
	TEX::del(p->atlasTex.tex);
#else
	TEXFBO::fini(p->atlasTex);
#endif

	p->atlasTex = tex;
}

void SharedState::checkShutdown()
{
	if (!p->rtData.rqTerm)
		return;

	p->rtData.rqTermAck.set();
	p->texPool.disable();
	scriptBinding->terminate();
}

void SharedState::checkReset()
{
	if (!p->rtData.rqReset)
		return;

	p->rtData.rqReset.clear();
	scriptBinding->reset();
}

Font &SharedState::defaultFont() const
{
	return *p->defaultFont;
}

double SharedState::runTime() {
    if (!p) return 0;
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now - p->startupTime).count() / 1000.0 / 1000.0;
}

unsigned int SharedState::genTimeStamp()
{
	return p->stampCounter++;
}

SharedState::SharedState(RGSSThreadData *threadData)
{
	p = new SharedStatePrivate(threadData);
	SharedState::instance = this;
	try
	{
		p->init(threadData);
		p->screen = p->graphics.getScreen();
	}
	catch (const Exception &exc)
	{
		// If the "error" was the user quitting the game before the path cache finished building,
		// then just return
		if (rtData().rqTerm)
			return;
		
		delete p;
		SharedState::instance = 0;
		
		throw exc;
	}
}

SharedState::~SharedState()
{
	delete p;
}
