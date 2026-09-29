/*
** graphics.h
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

#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "util.h"

class Scene;
class Bitmap;
class Disposable;
struct RGSSThreadData;
struct GraphicsPrivate;
struct AtomicFlag;
struct THEORAPLAY_VideoFrame;
struct Movie;

class Graphics
{
public:
    double getDelta();
    double lastUpdate();
    
	void update(bool checkForShutdown = true);
	void freeze();
	void transition(int duration = 8,
	                const char *filename = "",
	                int vague = 40);
	void frameReset();

	DECL_ATTR( FrameRate,  int )
	DECL_ATTR( FrameCount, int )
	DECL_ATTR( Brightness, int )

	void wait(int duration);
	void fadeout(int duration);
	void fadein(int duration);

	Bitmap *snapToBitmap();

	int width() const;
	int height() const;
	int widthHires() const;
	int heightHires() const;
	bool isPingPongFramebufferActive() const;
    int displayContentWidth() const;
    int displayContentHeight() const;
    int displayWidth() const;
    int displayHeight() const;
	void resizeScreen(int width, int height);
    void resizeWindow(int width, int height, bool center=false);
	void drawMovieFrame(const THEORAPLAY_VideoFrame* video, Bitmap *videoBitmap);
	bool updateMovieInput(Movie *movie);
	void playMovie(const char *filename, int volume, bool skippable);
	void screenshot(const char *filename);

	void reset();
    void center();

	/* ---- The persistent on-screen overlay ------------
	 * One boot-reserved texture, composed on the CPU by vita/overlay and
	 * drawn over the finished frame after the screen blit and before the
	 * swap. The settings menu, the FPS/frame-time line
	 * and the frame-profile HUD all go through
	 * here, so none of them spends a GL object.
	 *
	 * overlaySetLines() takes one C string per LINE rather than one blob of
	 * text: a caller that wants to add fields passes a longer array, and the
	 * lines it did not write are bytes it never had to own or format. Each
	 * line lands in its own fixed slot, so no two consumers share a buffer.
	 * A call whose lines are identical to the current ones does nothing at
	 * all -- neither a re-compose nor an upload -- which is what makes it
	 * safe to call this every frame.
	 *
	 * Nothing is drawn until overlaySetVisible(true), and while the overlay
	 * is hidden the frame issues exactly the GL calls it did before this
	 * existed. Call all three from the thread that owns the GL context (the
	 * RGSS thread), which is the thread vita/overlay is composed on. The two
	 * setters enforce it: a call from any other thread changes nothing and
	 * reports itself once, and a -Dvita_frame_trace=true diagnostic build
	 * aborts on it.
	 *
	 * Without MKXPZ_SOFTWARE_BITMAPS there is no overlay texture and these
	 * are no-ops returning false, so callers need no #ifdef. */
	void overlaySetLines(const char *const *lines, int count);
	void overlaySetVisible(bool visible);
	bool overlayIsVisible() const;

    /* Non-standard extension */
    DECL_ATTR( Fullscreen, bool )
    DECL_ATTR( ShowCursor, bool )
    DECL_ATTR( Scale,    double )
    DECL_ATTR( Frameskip, bool )
    DECL_ATTR( FixedAspectRatio, bool )
    DECL_ATTR( SmoothScaling, int )
    DECL_ATTR( IntegerScaling, bool )
    DECL_ATTR( LastMileScaling, bool )
    DECL_ATTR( Threadsafe, bool )
    double averageFrameRate();

	/* <internal> */
	Scene *getScreen() const;
	/* Repaint screen with static image until exitCond
	 * is set. Observes reset flag on top of shutdown
	 * if "checkReset" */
	void repaintWait(const AtomicFlag &exitCond,
	                 bool checkReset = true);
    
    void lock(bool force = false);
    void unlock(bool force = false);

private:
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
    void runSettingsMenu(const struct VitaSettingsInput &initial);
    void repaintSettings();
#endif
	Graphics(RGSSThreadData *data);
	~Graphics();

	void addDisposable(Disposable *);
	void remDisposable(Disposable *);

	friend struct SharedStatePrivate;
	friend class Disposable;

	GraphicsPrivate *p;
};

#define GFX_LOCK shState->graphics().lock()
#define GFX_UNLOCK shState->graphics().unlock()

#endif // GRAPHICS_H
