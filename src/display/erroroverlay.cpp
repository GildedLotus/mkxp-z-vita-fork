// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * erroroverlay.cpp -- see erroroverlay.h for the contract.
 *
 * The whole file is one story: take text that is already in the log, turn it
 * into 960x544 pixels on the CPU, put those pixels on the screen with the GL
 * objects boot already paid for, wait for a button, give everything back.
 *
 * WHAT IT IS ALLOWED TO SPEND
 * ---------------------------
 * Exactly one GL texture and about 2.1 MiB of CPU pixels, both freed before
 * it returns. The driver serves render surfaces, shader
 * programs, VAOs, textures and VBOs out of shared fixed pools, the engine
 * drains them at boot on purpose, and after GPUBudget::seal() the first
 * three classes can fail HARD while a texture keeps working. So: one glGenTextures, one whole-level glTexImage2D, and not one
 * framebuffer, program, VAO or buffer object. GPUBudget::creationSite() -- the
 * seal's own tripwire -- is never reached, because nothing here goes near the
 * three call sites that report to it.
 *
 * The draw is the one variant the driver has compiled code for:
 * GPUBudget::warmUpPrograms() draws one pixel per (program x target x blend
 * state) at boot, and a variant first needed mid-game wants a code-heap
 * segment out of a pool that by then has nothing left. That
 * is why this uses shaders().simple with BlendNormal ON, on the default
 * framebuffer, and NOT GLMeta::blitRectangle -- blitBegin turns blending OFF,
 * and nothing warms that combination.
 *
 * ORDER OF WORK, AND WHY THE ORDER IS THE DESIGN
 * ----------------------------------------------
 *   1. rqTerm     -- the player is already going away; drawing into a context
 *                    the main thread is about to tear down is how a clean
 *                    quit turns into a crash report.
 *   2. LOG        -- FIRST, and before a font is touched. Opening or drawing
 *                    with a registered family reads a file through PhysFS on
 *                    this thread (SharedFontState::getFont -> openReadRaw),
 *                    and rasterising runs FreeType over bytes a broken game
 *                    supplied. If either of those is what kills the process,
 *                    the text is already on disk.
 *   3. FONT       -- the CJK family only when the text needs it and only when
 *                    it is registered; otherwise the bundled Latin face,
 *                    which is memory-backed and touches no file system at all.
 *   4. LAYOUT     -- textpanel::layout with a TTF_MeasureUTF8 measure.
 *   5. COMPOSE    -- one CPU buffer, swraster + TTF_RenderUTF8_Blended.
 *   6. GL         -- generate, upload once, then draw the same texture every
 *                    frame until dismissed.
 *   7. RELEASE    -- pixels, texture and (if this function opened it) font.
 *
 * Any failure from 3 onwards is one `error-overlay: skipped (<reason>)` line
 * and a normal return. The caller is reporting a failure already; a panel
 * that raises inside a rescue handler takes the report with it.
 */

#include "erroroverlay.h"

#if (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && defined(MKXPZ_SOFTWARE_BITMAPS)

#include <SDL_pixels.h>
#include <SDL_surface.h>
#include <SDL_timer.h>
#include <SDL_ttf.h>
#include <SDL_video.h>

#include <sys/stat.h>

#include <new>
#include <stdio.h>
#include <string>

#include "etc-internal.h"
#include "eventthread.h"
#include "font.h"
#include "gl-fun.h"
#include "gl-util.h"
#include "glstate.h"
#include "quad.h"
#include "shader.h"
#include "sharedstate.h"
#include "swraster.h"
#include "textpanel.h"
#include "vita_fatal.h"
#include "vita_glue.h"

#if defined(MKXPZ_HOST_PORT_LOGIC) && !defined(__vita__)
#include "hf_v_paths.h"
#endif
#define VITA_PANEL_W 960
#define VITA_PANEL_H 544
#define VITA_PANEL_BYTES ((size_t)VITA_PANEL_W * VITA_PANEL_H * 4)

/* Two device-run knobs, both plain files. They are NOT declared in
 * vita/glue/vita_glue.h: they belong to this file alone. The
 * directory is overridable only so that a host-compilable build can point
 * the real code at a scratch directory.
 *
 *   overlay-autodismiss.enabled   dismiss after 5 s with no input at all, for
 *                                 an unattended hardware run.
 *   overlay-builtin-font.enabled  never look a family up; use the embedded
 *                                 face. For the run where the font registry
 *                                 itself is the suspect: with it present this
 *                                 file opens no file and reads no directory. */
#ifndef VITA_OVERLAY_MARKER_DIR
#define VITA_OVERLAY_MARKER_DIR "ux0:/data/mkxp-z"
#endif
#define VITA_OVERLAY_AUTODISMISS_MARKER \
	VITA_OVERLAY_MARKER_DIR "/overlay-autodismiss.enabled"
#define VITA_OVERLAY_BUILTIN_FONT_MARKER \
	VITA_OVERLAY_MARKER_DIR "/overlay-builtin-font.enabled"

/* Milliseconds the autodismiss marker waits before giving up on a human. */
#define VITA_PANEL_AUTODISMISS_MS 5000u

/* Point sizes. The CJK face is a size larger because its glyphs carry more
 * detail per em; both are well inside a 24 px line. */
#define VITA_PANEL_CJK_PT 22
#define VITA_PANEL_LATIN_PT 20

/* The registered family a Japanese error message needs (the font
 * setup registers it from app0:/fonts). Absent on a device that never got the font
 * pack, which is why every use of it is behind fontPresent(). */
#define VITA_PANEL_CJK_FAMILY "VL Gothic"

/* Bytes of one line handed to TTF_MeasureUTF8 at a time. A measurement pass
 * is linear in what it is given and wrap() asks about the whole remaining
 * paragraph each time it places a line, so an unbounded body would be
 * quadratic. No 896 px line holds a thousand bytes of renderable text at
 * these point sizes -- the narrowest glyph in either face is wider than one
 * pixel -- so this bound is a ceiling on work, not on layout. */
#define VITA_PANEL_MEASURE_MAX ((size_t)1024)

/* ------------------------------------------------------------------------ */
/* Logging                                                                  */
/* ------------------------------------------------------------------------ */

/* Every line this file emits carries one prefix, so a device log can be
 * grepped for the panel's whole story: what it was asked to show, what it
 * laid out, what it refused to do and how it ended. vitaLogMessage splits on
 * newlines and bounds the body itself. */
static void panelLog(const char *text)
{
	vitaLogMessage("error-overlay: ", text);
}

static void panelSkipped(const char *reason)
{
	char line[192];
	snprintf(line, sizeof(line), "skipped (%s)", reason ? reason : "?");
	panelLog(line);
}

/* ------------------------------------------------------------------------ */
/* Device-run markers                                                       */
/* ------------------------------------------------------------------------ */

/* A plain stat, which is all a marker is. No PhysFS, no SDL_RWops, no Vita
 * header: newlib's stat() takes a device path on this target (the launcher's
 * game_scan.c does the same). */
static bool panelMarkerPresent(const char *path)
{
	struct stat st;

	return path && ::stat(path, &st) == 0;
}

/* Both markers are read once and cached. The panel runs a redraw loop, and a
 * stat per frame on ux0: would be the most expensive thing in it. */
static bool panelAutoDismiss()
{
	static int cached = -1;

	if (cached < 0)
		cached = panelMarkerPresent(VITA_OVERLAY_AUTODISMISS_MARKER) ? 1 : 0;

	return cached != 0;
}

static bool panelBuiltinFontOnly()
{
	static int cached = -1;

	if (cached < 0)
		cached = panelMarkerPresent(VITA_OVERLAY_BUILTIN_FONT_MARKER) ? 1 : 0;

	return cached != 0;
}

/* ------------------------------------------------------------------------ */
/* Rasterising, the SDL_ttf half of textpanel                               */
/* ------------------------------------------------------------------------ */

/* textpanel is SDL-free by design: the caller supplies "how wide is this run
 * of bytes" and "rasterise this line" as function pointers, and this is the
 * device implementation of both. One live surface at a time -- compose()
 * renders, composites and releases one line before it asks for the next --
 * so `pending` is a single slot rather than a set. */
struct PanelRaster
{
	TTF_Font *font;
	SDL_Surface *pending;
	std::string scratch;

	PanelRaster()
	    : font(0), pending(0)
	{}
};

static SDL_Color panelStyleColor(textpanel::Style style)
{
	SDL_Color color;

	switch (style)
	{
	case textpanel::Heading:
		color.r = 235; color.g = 238; color.b = 245;
		break;
	case textpanel::Dim:
		color.r = 140; color.g = 150; color.b = 170;
		break;
	case textpanel::Footer:
		color.r = 150; color.g = 178; color.b = 220;
		break;
	case textpanel::Body:
	default:
		color.r = 208; color.g = 214; color.b = 226;
		break;
	}
	color.a = 255;

	return color;
}

/* textpanel::Measure::fit. TTF_MeasureUTF8 answers in CHARACTERS and wants a
 * NUL-terminated string; wrap() counts BYTES and hands out slices of a larger
 * buffer that are not terminated at all. Both gaps are closed here, and
 * neither of them may be papered over: a byte count read as a character count
 * breaks every line early on Latin text and in the middle of a codepoint on
 * Japanese text. */
static size_t panelMeasureFit(void *ctx, const char *text, size_t len,
                              int maxWidthPx)
{
	PanelRaster *raster = (PanelRaster *)ctx;

	if (!raster || !raster->font || !text || len == 0 || maxWidthPx <= 0)
		return 0;

	/* The bound is on work, not on layout (VITA_PANEL_MEASURE_MAX), and it
	 * is snapped back to a codepoint boundary so SDL_ttf never sees a
	 * truncated sequence. */
	size_t take = len < VITA_PANEL_MEASURE_MAX ? len : VITA_PANEL_MEASURE_MAX;
	while (take > 0 && take < len && ((unsigned char)text[take] & 0xC0) == 0x80)
		--take;
	if (take == 0)
		take = len < VITA_PANEL_MEASURE_MAX ? len : VITA_PANEL_MEASURE_MAX;

	try {
		raster->scratch.assign(text, take);
	} catch (...) {
		return 0;
	}

	int extent = 0;
	int count = 0;
	if (TTF_MeasureUTF8(raster->font, raster->scratch.c_str(), maxWidthPx,
	                    &extent, &count) != 0)
		return 0;
	if (count <= 0)
		return 0;

	/* Characters to bytes, by lead byte. The input is well-formed UTF-8
	 * (sanitizeUtf8 ran over it before layout), and a stray continuation
	 * byte still advances by one so this cannot stall. */
	size_t at = 0;
	for (int i = 0; i < count && at < take; ++i)
	{
		unsigned char b = (unsigned char)text[at];
		size_t step;

		if (b < 0x80 || (b >= 0x80 && b < 0xC0))
			step = 1;
		else if (b < 0xE0)
			step = 2;
		else if (b < 0xF0)
			step = 3;
		else
			step = 4;

		at += step;
	}

	return at > take ? take : at;
}

/* textpanel::Glyphs::render. TTF_RenderUTF8_Blended gives ARGB8888;
 * swraster wants RGBA8888 in memory order, which is SDL_PIXELFORMAT_ABGR8888
 * on this little-endian target. A null px means "this line could not be
 * rendered", which compose() skips without calling release. */
static swraster::Surface panelRenderLine(void *ctx, const char *utf8,
                                         textpanel::Style style)
{
	swraster::Surface out = {0, 0, 0, 0};
	PanelRaster *raster = (PanelRaster *)ctx;

	if (!raster || !raster->font || !utf8 || !utf8[0])
		return out;

	SDL_Surface *text = TTF_RenderUTF8_Blended(raster->font, utf8,
	                                           panelStyleColor(style));
	if (!text)
		return out;

	SDL_Surface *rgba = SDL_ConvertSurfaceFormat(text, SDL_PIXELFORMAT_ABGR8888,
	                                             0);
	SDL_FreeSurface(text);
	if (!rgba)
		return out;

	raster->pending = rgba;
	out.px = (uint8_t *)rgba->pixels;
	out.w = rgba->w;
	out.h = rgba->h;
	out.stride = rgba->pitch;

	return out;
}

static void panelReleaseLine(void *ctx, swraster::Surface surface)
{
	PanelRaster *raster = (PanelRaster *)ctx;

	(void)surface;

	if (!raster || !raster->pending)
		return;

	SDL_FreeSurface(raster->pending);
	raster->pending = 0;
}

/* ------------------------------------------------------------------------ */
/* Resources                                                                */
/* ------------------------------------------------------------------------ */

/* One destructor is the only exit. Every step below can fail and two of them
 * can throw (std::string, std::vector inside textpanel), and each of the four
 * things this function owns has a different owner on the other side of the
 * call: the pooled font belongs to SharedFontState and must NOT be closed, a
 * bundled handle belongs here and must be, the pixels are plain new[], and
 * the texture goes back through TEX::del so the deferred release
 * holds its storage until the frames that could still be reading it have
 * flipped. */
struct PanelResources
{
	PanelRaster raster;
	TTF_Font *font;
	bool pooledFont;
	uint8_t *pixels;
	TEX::ID tex;
	bool texLive;

	PanelResources()
	    : font(0), pooledFont(false), pixels(0), tex(0), texLive(false)
	{}

	~PanelResources()
	{
		if (raster.pending)
		{
			SDL_FreeSurface(raster.pending);
			raster.pending = 0;
		}

		if (texLive)
		{
			TEX::unbind();
			TEX::del(tex);
			texLive = false;
		}

		delete[] pixels;
		pixels = 0;

		if (font && !pooledFont)
			TTF_CloseFont(font);
		font = 0;
	}
};

/* Pick the face. The CJK family is asked for only when the text actually
 * needs it AND the family is registered, because getFont() on a registered
 * family opens a file through PhysFS and a missing one throws; the bundled
 * face is memory-backed (font.cpp's openBundledFont is an SDL_RWFromConstMem
 * over the xxd'd asset) and is what the panel falls back to for everything
 * else, including a CJK lookup that failed.
 *
 * With overlay-builtin-font.enabled present nothing here reads a file or
 * consults the registry at all -- fontPresent() and getFont() are never
 * called -- which is the point of the marker. */
static bool panelOpenFont(PanelResources &res, const std::string &text)
{
	if (!panelBuiltinFontOnly() && textpanel::needsCjkFont(text) &&
	    shState->fontState().fontPresent(VITA_PANEL_CJK_FAMILY))
	{
		try {
			res.font = shState->fontState().getFont(VITA_PANEL_CJK_FAMILY,
			                                        VITA_PANEL_CJK_PT, 1.0f);
			res.pooledFont = res.font != 0;
		} catch (...) {
			res.font = 0;
			res.pooledFont = false;
		}
	}

	if (!res.font)
	{
		res.pooledFont = false;
		try {
			res.font = SharedFontState::openBundled(VITA_PANEL_LATIN_PT);
		} catch (...) {
			res.font = 0;
		}
	}

	res.raster.font = res.font;

	return res.font != 0;
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */

/* The lowest button the pad reports down this frame, or -1. Read straight off
 * EventThread::controllerState, which the event thread fills whether or not
 * anything calls Input.update: the game is not
 * running its frame loop while this panel is up, so the RGSS Input layer has
 * nothing to say. */
static int panelPadButton()
{
	for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i)
		if (EventThread::controllerState.buttons[i])
			return i;

	return -1;
}

/* ------------------------------------------------------------------------ */
/* The draw                                                                 */
/* ------------------------------------------------------------------------ */

/* One frame of the panel, and the only GL this file issues after the upload.
 *
 * Five pieces of state are pushed and popped, so whatever the engine was in
 * the middle of survives a msgbox that returns into a running game. The
 * clear is what makes this a panel rather than an overlay: the frame
 * underneath is the one the game drew before it failed, and leaving it
 * showing through would be worse than useless.
 *
 * The orientation matches the present blit: flip the SOURCE tex coords and
 * keep a positive destination height, because a negative destination height
 * gives a black screen on this driver. Row 0 of the composed image therefore lands at the top
 * of the screen. */
static void panelDrawFrame(TEX::ID tex, SDL_Window *window, int winW, int winH)
{
	FBO::unbind();

	glState.viewport.pushSet(IntRect(0, 0, winW, winH));
	glState.scissorTest.pushSet(false);
	glState.blend.pushSet(true);
	glState.blendMode.pushSet(BlendNormal);
	glState.clearColor.pushSet(Vec4(0, 0, 0, 1));

	FBO::clear();

	SimpleShader &shader = shState->shaders().simple;
	shader.bind();
	shader.applyViewportProj();
	shader.setTranslation(Vec2i());
	shader.setTexOffsetX(0);
	shader.setTexSize(Vec2i(VITA_PANEL_W, VITA_PANEL_H));

	TEX::bind(tex);

	Quad &quad = shState->gpQuad();
	quad.setTexPosRect(FloatRect(0, VITA_PANEL_H, VITA_PANEL_W, -VITA_PANEL_H),
	                   FloatRect(0, 0, (float)winW, (float)winH));
	quad.draw();

	glState.clearColor.pop();
	glState.blendMode.pop();
	glState.blend.pop();
	glState.scissorTest.pop();
	glState.viewport.pop();

	if (window)
		SDL_GL_SwapWindow(window);
}

/* ------------------------------------------------------------------------ */
/* The panel                                                                */
/* ------------------------------------------------------------------------ */

static std::string panelFooter(bool fatal)
{
	std::string out = fatal ? "Press any button to quit."
	                        : "Press any button to continue.";
	out += "\nLog: ";
	out += VITA_GLUE_LOG_PATH;

	return out;
}

static void panelShow(const char *heading, const std::string &body, bool fatal)
{
	/* 1. The player is already leaving. Drawing into a context the main
	 *    thread is tearing down turns a clean quit into a crash report, and
	 *    waiting for a button press turns it into a hang. */
	if (!shState)
		return;
	if (shState->rtData().rqTerm)
	{
		panelSkipped("terminating");
		return;
	}

	/* 2. LOG, before a font is opened and before a GL object is asked for.
	 *    The caller logged this text too; that is not a duplicate for the
	 *    sake of it. Everything from here on runs FreeType over bytes a
	 *    broken game supplied and the GPU driver over a pool it drained at
	 *    boot, so this line is the last one guaranteed to reach the disk. */
	const std::string text = textpanel::sanitizeUtf8(body.data(), body.size());
	const char *title = heading ? heading : "Message";

	char head[192];
	snprintf(head, sizeof(head), "show \"%s\" fatal=%d bytes=%u", title,
	         fatal ? 1 : 0, (unsigned)text.size());
	panelLog(head);
	vitaLogMessage("error-overlay: ", text.c_str());

	PanelResources res;

	/* 3. FONT. */
	if (!panelOpenFont(res, text))
	{
		panelSkipped("no font");
		return;
	}

	/* 4. LAYOUT, against the face that was actually opened: a 22 pt CJK line
	 *    does not fit a spacing chosen for 20 pt Latin, and TTF_FontLineSkip
	 *    is the only honest source for it. */
	textpanel::PanelSpec spec;
	const int skip = TTF_FontLineSkip(res.font);
	if (skip > 0)
	{
		spec.lineH = skip;
		spec.headingH = skip + 10;
		spec.footerH = skip + 4;
	}

	textpanel::Measure measure = { panelMeasureFit, &res.raster };
	textpanel::Panel panel = textpanel::layout(title, text, panelFooter(fatal),
	                                           spec, measure, measure);

	char shown[96];
	snprintf(shown, sizeof(shown), "lines=%u omitted=%d",
	         (unsigned)panel.lines.size(), panel.omitted);
	panelLog(shown);

	/* 5. COMPOSE. One buffer, one allocation, no exception: an error panel
	 *    that cannot be drawn because the heap is exhausted is exactly the
	 *    case this must survive. */
	res.pixels = new (std::nothrow) uint8_t[VITA_PANEL_BYTES];
	if (!res.pixels)
	{
		panelSkipped("no memory for 960x544 pixels");
		return;
	}

	swraster::Surface dst;
	dst.px = res.pixels;
	dst.w = VITA_PANEL_W;
	dst.h = VITA_PANEL_H;
	dst.stride = VITA_PANEL_W * 4;

	textpanel::Glyphs glyphs = { panelRenderLine, panelReleaseLine,
	                             &res.raster };
	textpanel::compose(dst, panel, spec, glyphs);

	/* 6. GL. One texture, one whole-level upload, never a sub-rect: a
	 *    sub-rect upload is the PTLA transfer path, tracked by the texture's
	 *    own sync object and by nothing else, and this texture was created
	 *    out of an empty pool and has none.
	 *
	 *    The queue is drained first so that the check after the upload is
	 *    about the upload. GL error flags are sticky and the engine only
	 *    polls them under the telemetry marker, so whatever is pending here
	 *    probably belongs to the frame that failed -- refusing to draw the
	 *    error report because of the error it is reporting would be the
	 *    worst possible failure. Bounded because this is the one loop in the
	 *    file that a broken driver could otherwise own; GL has a handful of
	 *    flags and returns GL_NO_ERROR once they are read. Nothing is
	 *    swallowed: how many there were goes in the log. */
	if (gl.GetError)
	{
		unsigned stale = 0;
		for (int i = 0; i < 8; ++i)
		{
			if (gl.GetError() == GL_NO_ERROR)
				break;
			++stale;
		}
		if (stale != 0)
		{
			char pending[96];
			snprintf(pending, sizeof(pending),
			         "%u GL error(s) were already pending", stale);
			panelLog(pending);
		}
	}

	res.tex = TEX::gen();
	if (!res.tex.gl)
	{
		panelSkipped("no texture");
		return;
	}
	res.texLive = true;

	TEX::bind(res.tex);
	TEX::setRepeat(false);
	TEX::setSmooth(false);
	TEX::uploadImage(VITA_PANEL_W, VITA_PANEL_H, res.pixels, GL_RGBA);

	/* The upload is the one call here that can fail for a reason the engine
	 * cannot see coming (GL_OUT_OF_MEMORY out of the driver's host heap).
	 * Drawing an unspecified texture over the frame would replace a readable
	 * error with a screen of noise, so stop and leave the log to do it. */
	const GLenum uploadError = gl.GetError ? gl.GetError() : (GLenum)GL_NO_ERROR;
	if (uploadError != GL_NO_ERROR)
	{
		char why[96];
		snprintf(why, sizeof(why), "GL error 0x%x after upload",
		         (unsigned)uploadError);
		panelSkipped(why);
		return;
	}

	/* 7. WAIT. */
	SDL_Window *window = shState->sdlWindow();
	const uint32_t openedMs = SDL_GetTicks();

	textpanel::DismissLatch latch(panelAutoDismiss()
	                                  ? VITA_PANEL_AUTODISMISS_MS
	                                  : 0u);
	latch.open(openedMs);

	int pressed = -1;

	while (!shState->rtData().rqTerm)
	{
		const uint32_t frameMs = SDL_GetTicks();

		int winW = VITA_PANEL_W;
		int winH = VITA_PANEL_H;
		if (window)
			SDL_GetWindowSize(window, &winW, &winH);
		if (winW <= 0)
			winW = VITA_PANEL_W;
		if (winH <= 0)
			winH = VITA_PANEL_H;

		panelDrawFrame(res.tex, window, winW, winH);

		const int button = panelPadButton();
		if (button >= 0 && pressed < 0)
			pressed = button;

		if (latch.update(button >= 0, SDL_GetTicks()) ==
		    textpanel::DismissLatch::Dismissed)
			break;

		/* The swap paces the loop when the driver is flipping; when it is
		 * not -- a suspended window, a driver that returns immediately --
		 * nothing else would, and a spin here would cook the battery in the
		 * one situation where the player is reading rather than playing. */
		if (SDL_GetTicks() - frameMs < 5)
			SDL_Delay(12);
	}

	char ended[96];
	snprintf(ended, sizeof(ended), "dismissed button=%d after %ums", pressed,
	         (unsigned)(SDL_GetTicks() - openedMs));
	panelLog(ended);
}

void vitaShowPanel(const char *heading, const std::string &body, bool fatal)
{
	/* The outer boundary. panelShow() releases everything it owns through
	 * ~PanelResources whichever way it leaves, so this catch has nothing to
	 * clean up -- it exists so that an allocation failure deep inside
	 * textpanel or std::string cannot turn a reported error into an
	 * unreported abort inside a Ruby rescue handler. */
	try {
		panelShow(heading, body, fatal);
	} catch (...) {
		panelSkipped("exception");
	}
}

#else /* !(__vita__ && MKXPZ_SOFTWARE_BITMAPS) */

/* Desktop mkxp-z has a real message box and every other backend has a real
 * render target, so there is nothing for this to do and nothing it may cost.
 * Compiled everywhere all the same -- like src/vita_fatal.cpp -- so the one
 * declaration in erroroverlay.h is always satisfied. */
void vitaShowPanel(const char *heading, const std::string &body, bool fatal)
{
	(void)heading;
	(void)body;
	(void)fatal;
}

#endif
