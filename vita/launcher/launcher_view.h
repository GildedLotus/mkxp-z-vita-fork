// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * launcher_view.h — the launcher's whole picture, composed on the CPU
 *
 *
 * One 960x544 SDL_PIXELFORMAT_ABGR8888 surface is the launcher's entire
 * framebuffer. Everything — background, header, rows, selection bar, glyphs
 * — is blitted into it by the CPU, and launcher_gl.c uploads the result as
 * one whole texture level. There is no second surface, no render target and
 * no per-row texture, because a render surface costs three firmware sync
 * objects out of a process-wide pool of about 64 and
 * the launcher has to hand that pool to the game intact.
 *
 * The CPU cost that buys is real but bounded, and it is why row text is
 * cached: SDL_ttf rendering a line is far more expensive than blitting the
 * surface it produced, and the list only changes when the user presses
 * something. A redraw of a 10-row page after the cache is warm is ten
 * SDL_BlitSurface calls over at most 960x44 each, plus one fill per row.
 * The cache is dropped on rescan, which is the only event that can change
 * what a row says, and trimmed by launcher_view_trim_rows() to the pages
 * near the one on screen: a full card must not leave tens of MB of glyph
 * surfaces between the launcher and the clean process the LoadExec
 * hand-over depends on.
 *
 * Deliberately not here: any decision. The view draws what the model and the
 * scan already decided — which entries exist, which one is selected, which
 * page is showing. It reads no files (the "RTP missing" flags are computed
 * by the caller, which is the one doing I/O anyway), and it reaches the
 * screen only through this one surface: the three SDL shortcuts avoided here
 * — a 2D renderer, a window surface and the simple message box — are each
 * either a second GL context or a software framebuffer, and the SDL2 built
 * for this device hands out neither.
 *
 * C99 + SDL2 + SDL2_ttf + the launcher core library. No GL, no Vita headers,
 * no mkxp-z, no Ruby.
 */
#ifndef VITA_LAUNCHER_VIEW_H
#define VITA_LAUNCHER_VIEW_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "game_scan.h"
#include "launcher_model.h"
#include "preflight.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VITA_LAUNCHER_TRACE_FN_DEFINED
#define VITA_LAUNCHER_TRACE_FN_DEFINED
typedef void (*LauncherTraceFn)(const char *msg);
#endif

/* The Vita's panel, exactly. Not a preference: SDL_WINDOW_FULLSCREEN_DESKTOP
 * on this device is always 960x544, and a canvas of any other size would be
 * resampled by the driver on every draw. */
#define LAUNCHER_VIEW_W 960
#define LAUNCHER_VIEW_H 544

/* 10 rows of 44 px, under a header and above the footer hints. */
#define LAUNCHER_VIEW_ROWS   10
#define LAUNCHER_VIEW_ROW_H  44
#define LAUNCHER_VIEW_HEAD_H 56

/* Point sizes of the two faces. SDL_ttf's default DPI is 72, so a point is
 * a pixel here and these are 28 px and 18 px. */
#define LAUNCHER_VIEW_FONT_PT       28
#define LAUNCHER_VIEW_FONT_SMALL_PT 18

/* One cached text surface per column per entry. Selection is drawn as a bar
 * UNDER the text, never as a recolour, so a cached row stays valid whether
 * it is selected or not. */
enum {
    LAUNCHER_CELL_TITLE = 0,
    LAUNCHER_CELL_TAG,     /* XP / VX / VX Ace / ? */
    LAUNCHER_CELL_FOLDER,
    LAUNCHER_CELL_RTP,     /* "RTP missing", or absent */
    LAUNCHER_CELL_COUNT
};

typedef struct LauncherRowCache {
    SDL_Surface *cell[LAUNCHER_CELL_COUNT];
    int built;
} LauncherRowCache;

/* Ceiling on live cached-row bytes. A row is at most about 120 KB (full
 * column widths at the shipped faces), so the fence sits above the
 * three-page window launcher_view_trim_rows() keeps and only catches faces
 * whose metrics swell past that worst case. */
#define LAUNCHER_VIEW_ROW_CACHE_MAX_BYTES (4 * 1024 * 1024)

typedef struct LauncherView {
    SDL_Surface *canvas;     /* 960x544 ABGR8888; the only thing uploaded */
    TTF_Font *font;          /* 28 px */
    TTF_Font *font_small;    /* 18 px */
    char font_path[256];     /* the face that won, for the log and the report */
    int font_has_jp;         /* 1 if it provides U+3042, U+30A2 and U+4E00 */
    unsigned char *font_data; /* the winning face's file; both fonts read it from memory */
    LauncherRowCache rows[GAME_SCAN_MAX_ENTRIES];
    int row_bytes;           /* live pitch*h total across all cached rows */
} LauncherView;

/*
 * Allocate the canvas and pick the two faces out of `fonts_dir`.
 *
 * Font choice, in this order: the first face (by name) that answers
 * TTF_GlyphIsProvided32 for U+3042, U+30A2 and U+4E00 — hiragana, katakana
 * and a kanji, the three scripts a Japanese RPG Maker title needs; otherwise
 * the first face that opens at all. No file name is hard-coded, so the
 * packaging can drop a different face in without touching this code.
 *
 * Emits "launcher: font '<path>' jp=<0|1>". Returns 1 on success; 0 only if
 * the canvas could not be allocated or no face opened (the caller then has
 * no way to say anything on screen and should fall back to the log).
 */
int launcher_view_init(LauncherView *v, const char *fonts_dir,
                       LauncherTraceFn trace);

/* Free every cached row surface. Call on rescan — the entry at index i is
 * not the same game any more. */
void launcher_view_drop_rows(LauncherView *v);

/* Free every cached row the visible page does not need: everything outside
 * the page at `top` plus one page of margin each side, then the rows
 * farthest from the visible page while the live total exceeds
 * LAUNCHER_VIEW_ROW_CACHE_MAX_BYTES. The visible page itself is never
 * evicted. launcher_view_compose_list applies this after drawing. */
void launcher_view_trim_rows(LauncherView *v, int top, int count);

/* Everything the list screen draws that is not the cursor. The caller owns
 * every pointer; the view keeps none of them past the call. */
typedef struct LauncherListInfo {
    const GameEntry *entries;
    int count;
    /* One byte per entry: non-zero means the game asks for an RTP whose
     * folder is not on the card. NULL means "nothing is missing" — the
     * check is a stat(), and the view does no I/O. */
    const unsigned char *rtp_missing;
    const char *games_root; /* named by the empty state; may be NULL */
    const char *subtitle;   /* header right-hand line; may be NULL */
    int scan_failed;        /* show a retry notice instead of an empty list */
} LauncherListInfo;

/*
 * Compose the list screen: `info` is what to draw, `m` decides the page and
 * the selection. An empty list draws the explanation of the folder layout
 * instead of rows.
 */
void launcher_view_compose_list(LauncherView *v, const LauncherListInfo *info,
                                const LauncherModel *m);

/*
 * Compose the message screen: `heading`, then `body` wrapped to the width of
 * the panel, then "press X". Used for last-error.txt and for a failed LoadExec
 * (with the hex code in `body`).
 */
void launcher_view_compose_message(LauncherView *v, const char *heading,
                                   const char *body);

/* Scale the shared CPU overlay into the existing canvas; no new GPU objects. */
void launcher_view_compose_preflight(LauncherView *v,
                                     const PreflightSummary *summary, unsigned page);

/* Tightly packed RGBA bytes of the canvas — what launcher_gl_upload wants.
 * NULL before init. */
const void *launcher_view_pixels(const LauncherView *v);

/* Release the canvas, both faces and every cached row. Idempotent. */
void launcher_view_shutdown(LauncherView *v);

#ifdef __cplusplus
}
#endif

#endif /* VITA_LAUNCHER_VIEW_H */
