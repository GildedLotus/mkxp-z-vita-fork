// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * launcher_view.c — see launcher_view.h.
 */
#include "launcher_view.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "utf8_util.h"

/* Release version: configure -D stamps it from the repo root
 * VERSION file. Host compiles (ui-gallery) have no configure and run as dev. */
#ifndef MKXPZ_VITA_VERSION
#define MKXPZ_VITA_VERSION "dev"
#endif

/* ---- layout ------------------------------------------------------------ */

#define LIST_Y0   LAUNCHER_VIEW_HEAD_H
#define LIST_H    (LAUNCHER_VIEW_ROWS * LAUNCHER_VIEW_ROW_H)
#define FOOTER_Y  (LIST_Y0 + LIST_H)
#define FOOTER_H  (LAUNCHER_VIEW_H - FOOTER_Y)
#define MARGIN    16

/* Four columns, left to right, in canvas pixels. They add up to 960 with a
 * 16 px margin on each side; every cell is truncated to its own width, so a
 * long title can never push the engine tag off the panel. */
#define COL_TITLE_X   MARGIN
#define COL_TITLE_W   560
#define COL_TAG_X     592
#define COL_TAG_W     88
#define COL_FOLDER_X  688
#define COL_FOLDER_W  144
#define COL_RTP_X     840
#define COL_RTP_W     104

#define BODY_W (LAUNCHER_VIEW_W - 2 * MARGIN - 48)

/* The footer says what the buttons do in words. No PlayStation glyphs: the
 * shipped Latin face has none, and a missing glyph is a box, which is worse
 * than no hint at all. */
static const char *const kFooterHint =
    "X / O: play    Triangle: rescan    L / R: page";

static const char *const kMessageHint = "press X";

/* ---- colours ----------------------------------------------------------- */

typedef struct Rgb { Uint8 r, g, b; } Rgb;

static const Rgb kBackground = { 22, 24, 30 };
static const Rgb kHeaderBg   = { 38, 42, 54 };
static const Rgb kFooterBg   = { 30, 33, 42 };
static const Rgb kSelectBar  = { 40, 62, 100 };
static const Rgb kRule       = { 48, 52, 64 };

static const SDL_Color kTextBright = { 238, 240, 246, 255 };
static const SDL_Color kTextDim    = { 166, 174, 190, 255 };
static const SDL_Color kTextWarn   = { 232, 176, 88, 255 };

/* ---- small helpers ----------------------------------------------------- */

static void trace_fmt(LauncherTraceFn trace, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void trace_fmt(LauncherTraceFn trace, const char *fmt, ...)
{
    char line[320];
    va_list ap;

    if (!trace)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = '\0';
    trace(line);
}

static void fill(SDL_Surface *s, int x, int y, int w, int h, Rgb c)
{
    SDL_Rect r;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    SDL_FillRect(s, &r, SDL_MapRGBA(s->format, c.r, c.g, c.b, 255));
}

/*
 * Blit `src` at (x, y), clipped to the rectangle (cx, cy, cw, ch).
 * Everything this module draws goes through here, so nothing can spill out
 * of its column even if a width measurement is wrong.
 */
static void blit_clipped(SDL_Surface *dst, SDL_Surface *src, int x, int y,
                         int cx, int cy, int cw, int ch)
{
    SDL_Rect clip, prev, to;

    if (!dst || !src)
        return;

    SDL_GetClipRect(dst, &prev);
    clip.x = cx;
    clip.y = cy;
    clip.w = cw;
    clip.h = ch;
    SDL_SetClipRect(dst, &clip);

    to.x = x;
    to.y = y;
    to.w = src->w;
    to.h = src->h;
    SDL_BlitSurface(src, NULL, dst, &to);

    SDL_SetClipRect(dst, &prev);
}

/* Vertically centre a `h`-tall thing inside a `box_h`-tall box at `box_y`.
 * Shift, not divide: box_h is a variable and the A9 has no integer divide. */
static int centre_y(int box_y, int box_h, int h)
{
    int slack = box_h - h;

    if (slack <= 0)
        return box_y;
    return box_y + (slack >> 1);
}

typedef struct MeasureCtx { TTF_Font *font; } MeasureCtx;

static int measure_utf8(const char *utf8, void *ctx)
{
    TTF_Font *font = ((MeasureCtx *)ctx)->font;
    int w = 0, h = 0;

    if (!utf8 || !utf8[0])
        return 0;
    if (!font || TTF_SizeUTF8(font, utf8, &w, &h) != 0)
        return 1 << 24; /* unmeasurable: treat as "never fits", not "fits" */
    return w;
}

static const char *ellipsis_for(TTF_Font *font)
{
    /* U+2026 if the face has it (Liberation Sans does); three dots if not,
     * because a missing glyph renders as a box and a box is not an ellipsis. */
    if (font && TTF_GlyphIsProvided32(font, 0x2026u))
        return "\xE2\x80\xA6";
    return "...";
}

/*
 * Render `text` in `font`, shortened with an ellipsis to fit `max_w`.
 * Returns a new surface the caller owns, or NULL (empty text, or SDL_ttf
 * failed — a row with one missing cell is still a usable row).
 */
static SDL_Surface *render_fitted(TTF_Font *font, const char *text, int max_w,
                                  SDL_Color colour)
{
    char cut[GAME_SCAN_TITLE_MAX + 8];
    MeasureCtx ctx;

    if (!font || !text || !text[0] || max_w <= 0)
        return NULL;

    ctx.font = font;
    if (utf8_truncate_to_width(text, max_w, measure_utf8, &ctx,
                               ellipsis_for(font), cut, sizeof(cut)) < 0)
        return NULL;
    if (!cut[0])
        return NULL;
    return TTF_RenderUTF8_Blended(font, cut, colour);
}

static const char *engine_tag(int rgss_version)
{
    switch (rgss_version) {
    case 1:  return "XP";
    case 2:  return "VX";
    case 3:  return "VX Ace";
    default: return "?";
    }
}

/* ---- fonts ------------------------------------------------------------- */

#define FONT_MAX_CANDIDATES 24
#define FONT_NAME_MAX       128

static int has_font_suffix(const char *name)
{
    static const char *const kSuffix[] = { ".ttf", ".otf", ".ttc" };
    size_t len = strlen(name);
    unsigned i;

    for (i = 0; i < sizeof(kSuffix) / sizeof(kSuffix[0]); i++) {
        size_t sl = strlen(kSuffix[i]);
        size_t j;

        if (len <= sl)
            continue;
        for (j = 0; j < sl; j++) {
            char a = name[len - sl + j];
            char b = kSuffix[i][j];

            if (a >= 'A' && a <= 'Z')
                a = (char)(a + 32);
            if (a != b)
                break;
        }
        if (j == sl)
            return 1;
    }
    return 0;
}

/* Hiragana あ, katakana ア and the kanji 一: a face that draws all three can
 * draw a Japanese RPG Maker title. One of them is not enough — plenty of
 * Latin faces carry a stray kana. */
static int face_has_japanese(TTF_Font *font)
{
    return font &&
           TTF_GlyphIsProvided32(font, 0x3042u) &&
           TTF_GlyphIsProvided32(font, 0x30A2u) &&
           TTF_GlyphIsProvided32(font, 0x4E00u);
}

/* Bytewise sort so the choice is the same on every boot: readdir order on
 * exFAT is not a promise. */
static void sort_names(char names[][FONT_NAME_MAX], int n)
{
    int i, j;

    for (i = 1; i < n; i++) {
        char key[FONT_NAME_MAX];

        memcpy(key, names[i], FONT_NAME_MAX);
        j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) {
            memcpy(names[j + 1], names[j], FONT_NAME_MAX);
            j--;
        }
        memcpy(names[j + 1], key, FONT_NAME_MAX);
    }
}

/*
 * Open `dir`, and if that fails and the path carries a device prefix, try it
 * again without the slash after the colon.
 *
 * "app0:/fonts" and "app0:fonts" name the same directory, and this repo has
 * live examples of both spellings reaching sceIo (the earlier module loader
 * used "app0:module"; every data path is "ux0:/..."). Nothing here has ever
 * enumerated a directory on app0: before, so which spelling that path
 * handler accepts is genuinely unverified — and a launcher with no font is a
 * launcher nobody can read. Two opendir calls at boot is a cheap way to not
 * find out the hard way. `used` receives the spelling that worked, because
 * the file paths have to be built from the same one.
 */
static DIR *open_dir_either(const char *dir, char *used, size_t cap)
{
    const char *colon;
    DIR *d;

    used[0] = '\0';
    d = opendir(dir);
    if (d) {
        snprintf(used, cap, "%s", dir);
        return d;
    }

    colon = strchr(dir, ':');
    if (colon && colon[1] == '/') {
        snprintf(used, cap, "%.*s%s", (int)(colon - dir) + 1, dir, colon + 2);
        d = opendir(used);
        if (d)
            return d;
    }
    used[0] = '\0';
    return NULL;
}

static int open_fonts(LauncherView *v, const char *fonts_dir,
                      LauncherTraceFn trace)
{
    char names[FONT_MAX_CANDIDATES][FONT_NAME_MAX];
    char root[sizeof(v->font_path)];
    char path[sizeof(v->font_path)];
    char fallback[sizeof(v->font_path)];
    DIR *dir;
    struct dirent *de;
    int n = 0, i;

    fallback[0] = '\0';
    v->font_path[0] = '\0';
    v->font_has_jp = 0;

    if (!fonts_dir || !fonts_dir[0])
        return 0;

    dir = open_dir_either(fonts_dir, root, sizeof(root));
    if (!dir) {
        trace_fmt(trace, "launcher: fonts dir '%s' cannot be opened",
                  fonts_dir);
        return 0;
    }
    if (strcmp(root, fonts_dir) != 0)
        trace_fmt(trace, "launcher: fonts dir '%s' opened as '%s'", fonts_dir,
                  root);
    while (n < FONT_MAX_CANDIDATES && (de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        if (!has_font_suffix(de->d_name))
            continue;
        if (strlen(de->d_name) + 1 > FONT_NAME_MAX)
            continue;
        memset(names[n], 0, FONT_NAME_MAX);
        memcpy(names[n], de->d_name, strlen(de->d_name) + 1);
        n++;
    }
    closedir(dir);

    if (n == 0) {
        trace_fmt(trace, "launcher: no .ttf/.otf/.ttc under '%s'", fonts_dir);
        return 0;
    }
    sort_names(names, n);

    /* Two passes, one open per candidate each: the first face that can draw
     * Japanese wins outright; otherwise the first that opens at all. */
    for (i = 0; i < n; i++) {
        TTF_Font *probe;

        if ((int)snprintf(path, sizeof(path), "%s/%s", root, names[i]) >=
            (int)sizeof(path))
            continue;
        probe = TTF_OpenFont(path, LAUNCHER_VIEW_FONT_PT);
        if (!probe)
            continue;
        if (!fallback[0])
            memcpy(fallback, path, strlen(path) + 1);
        if (face_has_japanese(probe)) {
            v->font = probe;
            v->font_has_jp = 1;
            memcpy(v->font_path, path, strlen(path) + 1);
            break;
        }
        TTF_CloseFont(probe);
    }

    if (!v->font) {
        if (!fallback[0]) {
            trace_fmt(trace, "launcher: no face under '%s' could be opened",
                      fonts_dir);
            return 0;
        }
        v->font = TTF_OpenFont(fallback, LAUNCHER_VIEW_FONT_PT);
        if (!v->font)
            return 0;
        memcpy(v->font_path, fallback, strlen(fallback) + 1);
    }

    v->font_small = TTF_OpenFont(v->font_path, LAUNCHER_VIEW_FONT_SMALL_PT);
    if (!v->font_small) {
        /* One face is enough to boot: the small column just gets the big one
         * and the row looks cramped, which beats a launcher that will not
         * start. */
        v->font_small = v->font;
    }

    trace_fmt(trace, "launcher: font '%s' jp=%d", v->font_path,
              v->font_has_jp);
    return 1;
}

/* ---- init / shutdown --------------------------------------------------- */

int launcher_view_init(LauncherView *v, const char *fonts_dir,
                       LauncherTraceFn trace)
{
    if (!v)
        return 0;
    memset(v, 0, sizeof(*v));

    v->canvas = SDL_CreateRGBSurfaceWithFormat(0, LAUNCHER_VIEW_W,
                                               LAUNCHER_VIEW_H, 32,
                                               SDL_PIXELFORMAT_ABGR8888);
    if (!v->canvas) {
        trace_fmt(trace, "launcher: canvas allocation FAILED: %s",
                  SDL_GetError());
        return 0;
    }
    /* The canvas goes to the GPU as ONE tightly packed level, with
     * GL_UNPACK_ALIGNMENT 1 and no row stride to tell the driver about. At
     * 960 px of ABGR8888 the pitch is 3840 and SDL has no reason to pad it,
     * but "no reason to" is not a guarantee, and a padded pitch would upload
     * a picture skewed by a few pixels per row with nothing in the log to
     * say why. Check it once, here. */
    if (v->canvas->pitch != LAUNCHER_VIEW_W * 4) {
        trace_fmt(trace, "launcher: canvas pitch=%d, expected %d; refusing to "
                         "upload a padded surface",
                  v->canvas->pitch, LAUNCHER_VIEW_W * 4);
        SDL_FreeSurface(v->canvas);
        v->canvas = NULL;
        return 0;
    }

    /* The canvas is a source for one glTexSubImage2D, never a blit source:
     * blending it onto anything would be a second full-screen composite. */
    SDL_SetSurfaceBlendMode(v->canvas, SDL_BLENDMODE_NONE);
    fill(v->canvas, 0, 0, LAUNCHER_VIEW_W, LAUNCHER_VIEW_H, kBackground);

    if (!open_fonts(v, fonts_dir, trace)) {
        /* No face: the launcher can still draw bars and the log still says
         * everything, but nothing legible reaches the screen. The caller
         * decides whether that is fatal. */
        return 0;
    }
    return 1;
}

/* pitch * h is the malloc behind a surface; w*h alone would miss padding. */
static int surface_bytes(const SDL_Surface *s)
{
    return s ? s->pitch * s->h : 0;
}

static int row_bytes_of(const LauncherRowCache *row)
{
    int c, total = 0;

    for (c = 0; c < LAUNCHER_CELL_COUNT; c++)
        total += surface_bytes(row->cell[c]);
    return total;
}

static void free_row(LauncherView *v, int i)
{
    LauncherRowCache *row = &v->rows[i];
    int c;

    v->row_bytes -= row_bytes_of(row);
    for (c = 0; c < LAUNCHER_CELL_COUNT; c++) {
        if (row->cell[c]) {
            SDL_FreeSurface(row->cell[c]);
            row->cell[c] = NULL;
        }
    }
    row->built = 0;
}

void launcher_view_drop_rows(LauncherView *v)
{
    int i;

    if (!v)
        return;
    for (i = 0; i < GAME_SCAN_MAX_ENTRIES; i++)
        free_row(v, i);
    v->row_bytes = 0;
}

void launcher_view_shutdown(LauncherView *v)
{
    if (!v)
        return;
    launcher_view_drop_rows(v);
    if (v->font_small && v->font_small != v->font)
        TTF_CloseFont(v->font_small);
    v->font_small = NULL;
    if (v->font)
        TTF_CloseFont(v->font);
    v->font = NULL;
    if (v->canvas)
        SDL_FreeSurface(v->canvas);
    v->canvas = NULL;
}

const void *launcher_view_pixels(const LauncherView *v)
{
    if (!v || !v->canvas)
        return NULL;
    return v->canvas->pixels;
}

/* ---- rows -------------------------------------------------------------- */

/*
 * Build the four text surfaces of one entry, once. Everything here is CPU
 * memory: four small ARGB surfaces per visible entry, charged to
 * v->row_bytes as they are built, trimmed away by launcher_view_trim_rows
 * once the page turns, dropped wholesale by launcher_view_drop_rows when a
 * rescan makes them lies.
 */
static void build_row(LauncherView *v, int index, const GameEntry *e,
                      int rtp_missing)
{
    LauncherRowCache *row = &v->rows[index];

    if (row->built)
        return;

    row->cell[LAUNCHER_CELL_TITLE] =
        render_fitted(v->font, e->title, COL_TITLE_W, kTextBright);
    row->cell[LAUNCHER_CELL_TAG] =
        render_fitted(v->font_small, engine_tag(e->rgss_version), COL_TAG_W,
                      kTextDim);
    row->cell[LAUNCHER_CELL_FOLDER] =
        render_fitted(v->font_small, e->folder, COL_FOLDER_W, kTextDim);
    row->cell[LAUNCHER_CELL_RTP] =
        rtp_missing ? render_fitted(v->font_small, "RTP missing", COL_RTP_W,
                                    kTextWarn)
                    : NULL;
    row->built = 1;
    v->row_bytes += row_bytes_of(row);
}

/*
 * Cached rows are only good while the list stands still, and each one is
 * up to ~120 KB of CPU memory that the launch hand-over has to give back.
 * Keep the visible page plus one page of margin either side — what a burst
 * of Up/Down or L/R reaches before the next compose — then let the byte
 * fence evict the built rows farthest from the visible page while the total
 * is over the cap. The visible page is never evicted: its rows were just
 * drawn, and losing them would turn every redraw into a re-render.
 */
void launcher_view_trim_rows(LauncherView *v, int top, int count)
{
    int keep_lo, keep_hi, i;

    if (!v)
        return;
    if (count < 0)
        count = 0;
    if (count > GAME_SCAN_MAX_ENTRIES)
        count = GAME_SCAN_MAX_ENTRIES;
    if (top < 0)
        top = 0;
    if (top > count)
        top = count;

    keep_lo = top - LAUNCHER_VIEW_ROWS;
    keep_hi = top + 2 * LAUNCHER_VIEW_ROWS;
    if (keep_lo < 0)
        keep_lo = 0;
    if (keep_hi > count)
        keep_hi = count;

    for (i = 0; i < keep_lo; i++)
        if (v->rows[i].built)
            free_row(v, i);
    for (i = keep_hi; i < GAME_SCAN_MAX_ENTRIES; i++)
        if (v->rows[i].built)
            free_row(v, i);

    while (v->row_bytes > LAUNCHER_VIEW_ROW_CACHE_MAX_BYTES) {
        int centre = top + LAUNCHER_VIEW_ROWS / 2;
        int far = -1, far_dist = -1;

        for (i = 0; i < count; i++) {
            int dist;

            if (!v->rows[i].built ||
                (i >= top && i < top + LAUNCHER_VIEW_ROWS))
                continue;
            dist = i > centre ? i - centre : centre - i;
            if (dist > far_dist) {
                far_dist = dist;
                far = i;
            }
        }
        if (far < 0)
            break; /* only the visible page is left; the fence exempts it */
        free_row(v, far);
    }
}

static void draw_cell(LauncherView *v, SDL_Surface *text, int x, int y,
                      int w, int h)
{
    if (!text)
        return;
    blit_clipped(v->canvas, text, x, centre_y(y, h, text->h), x, y, w, h);
}

/* ---- header / footer --------------------------------------------------- */

static void draw_header(LauncherView *v, const char *subtitle,
                        const char *range)
{
    SDL_Surface *s;

    fill(v->canvas, 0, 0, LAUNCHER_VIEW_W, LAUNCHER_VIEW_HEAD_H, kHeaderBg);
    fill(v->canvas, 0, LAUNCHER_VIEW_HEAD_H - 1, LAUNCHER_VIEW_W, 1, kRule);

    /* The header names the build: render_fitted ellipsis-truncates to the
     * same 300 px box, so a longer version can never push the layout. */
    s = render_fitted(v->font, "mkxp-z " MKXPZ_VITA_VERSION, 300, kTextBright);
    if (s) {
        blit_clipped(v->canvas, s, MARGIN,
                     centre_y(0, LAUNCHER_VIEW_HEAD_H, s->h), MARGIN, 0, 300,
                     LAUNCHER_VIEW_HEAD_H);
        SDL_FreeSurface(s);
    }

    if (subtitle && subtitle[0]) {
        s = render_fitted(v->font_small, subtitle, 600, kTextDim);
        if (s) {
            blit_clipped(v->canvas, s, LAUNCHER_VIEW_W - MARGIN - s->w, 8,
                         340, 0, LAUNCHER_VIEW_W - MARGIN - 340,
                         LAUNCHER_VIEW_HEAD_H);
            SDL_FreeSurface(s);
        }
    }
    if (range && range[0]) {
        s = render_fitted(v->font_small, range, 600, kTextDim);
        if (s) {
            blit_clipped(v->canvas, s, LAUNCHER_VIEW_W - MARGIN - s->w, 30,
                         340, 0, LAUNCHER_VIEW_W - MARGIN - 340,
                         LAUNCHER_VIEW_HEAD_H);
            SDL_FreeSurface(s);
        }
    }
}

static void draw_footer(LauncherView *v, const char *hint)
{
    SDL_Surface *s;

    fill(v->canvas, 0, FOOTER_Y, LAUNCHER_VIEW_W, FOOTER_H, kFooterBg);
    fill(v->canvas, 0, FOOTER_Y, LAUNCHER_VIEW_W, 1, kRule);

    s = render_fitted(v->font_small, hint, LAUNCHER_VIEW_W - 2 * MARGIN,
                      kTextDim);
    if (s) {
        blit_clipped(v->canvas, s, MARGIN, centre_y(FOOTER_Y, FOOTER_H, s->h),
                     MARGIN, FOOTER_Y, LAUNCHER_VIEW_W - 2 * MARGIN, FOOTER_H);
        SDL_FreeSurface(s);
    }
}

/* Wrapped body text inside the list/message area. Not cached: it is drawn on
 * a screen the user is reading, not on one they are scrolling. */
static void draw_body(LauncherView *v, TTF_Font *font, const char *text,
                      int y, SDL_Color colour)
{
    SDL_Surface *s;

    if (!font || !text || !text[0])
        return;
    s = TTF_RenderUTF8_Blended_Wrapped(font, text, colour, (Uint32)BODY_W);
    if (!s)
        return;
    blit_clipped(v->canvas, s, MARGIN + 24, y, MARGIN, LIST_Y0,
                 LAUNCHER_VIEW_W - 2 * MARGIN, LIST_H);
    SDL_FreeSurface(s);
}

/* ---- screens ----------------------------------------------------------- */

static void compose_empty(LauncherView *v, const char *games_root, int scan_failed)
{
    char body[768];
    const char *root = (games_root && games_root[0])
                           ? games_root
                           : "ux0:/data/mkxp-z/games";

    if (scan_failed)
        snprintf(body, sizeof(body),
                 "Could not scan games in %s.\n\n"
                 "Check the storage and game files, then press Triangle to retry.", root);
    else
        snprintf(body, sizeof(body),
                 "No games found in %s\n\n"
                 "Put each game folder - the one that contains Game.ini - "
                 "directly in that folder:\n\n"
                 "    %s/MyGame/Game.ini\n\n"
                 "Copy the folder that holds Game.ini, not the folder that "
                 "contains that folder. Then press Triangle to rescan.",
                 root, root);
    body[sizeof(body) - 1] = '\0';

    draw_body(v, v->font_small, body, LIST_Y0 + 24, kTextDim);
}

void launcher_view_compose_list(LauncherView *v, const LauncherListInfo *info,
                                const LauncherModel *m)
{
    char range[96];
    int i, last, count, top, selected;

    if (!v || !v->canvas || !info || !m)
        return;

    count = info->count;
    if (count > GAME_SCAN_MAX_ENTRIES)
        count = GAME_SCAN_MAX_ENTRIES;
    top = m->top;
    selected = m->selected;

    fill(v->canvas, 0, 0, LAUNCHER_VIEW_W, LAUNCHER_VIEW_H, kBackground);

    range[0] = '\0';
    if (count > 0) {
        last = top + LAUNCHER_VIEW_ROWS;
        if (last > count)
            last = count;
        snprintf(range, sizeof(range), "showing %d-%d of %d", top + 1, last,
                 count);
    }
    draw_header(v, info->subtitle, range);
    draw_footer(v, kFooterHint);

    if (count <= 0 || !info->entries) {
        compose_empty(v, info->games_root, info->scan_failed);
        launcher_view_trim_rows(v, 0, 0);
        return;
    }

    for (i = 0; i < LAUNCHER_VIEW_ROWS; i++) {
        int index = top + i;
        int y = LIST_Y0 + i * LAUNCHER_VIEW_ROW_H;
        LauncherRowCache *row;

        if (index >= count)
            break;

        if (index == selected)
            fill(v->canvas, 0, y, LAUNCHER_VIEW_W, LAUNCHER_VIEW_ROW_H,
                 kSelectBar);
        else if (i > 0)
            fill(v->canvas, MARGIN, y, LAUNCHER_VIEW_W - 2 * MARGIN, 1, kRule);

        build_row(v, index, &info->entries[index],
                  info->rtp_missing ? info->rtp_missing[index] != 0 : 0);
        row = &v->rows[index];

        draw_cell(v, row->cell[LAUNCHER_CELL_TITLE], COL_TITLE_X, y,
                  COL_TITLE_W, LAUNCHER_VIEW_ROW_H);
        draw_cell(v, row->cell[LAUNCHER_CELL_TAG], COL_TAG_X, y, COL_TAG_W,
                  LAUNCHER_VIEW_ROW_H);
        draw_cell(v, row->cell[LAUNCHER_CELL_FOLDER], COL_FOLDER_X, y,
                  COL_FOLDER_W, LAUNCHER_VIEW_ROW_H);
        draw_cell(v, row->cell[LAUNCHER_CELL_RTP], COL_RTP_X, y, COL_RTP_W,
                  LAUNCHER_VIEW_ROW_H);
    }

    launcher_view_trim_rows(v, top, count);
}

void launcher_view_compose_message(LauncherView *v, const char *heading,
                                   const char *body)
{
    SDL_Surface *s;

    if (!v || !v->canvas)
        return;

    fill(v->canvas, 0, 0, LAUNCHER_VIEW_W, LAUNCHER_VIEW_H, kBackground);
    draw_header(v, NULL, NULL);
    draw_footer(v, kMessageHint);

    if (heading && heading[0]) {
        s = render_fitted(v->font, heading, LAUNCHER_VIEW_W - 2 * MARGIN,
                          kTextBright);
        if (s) {
            blit_clipped(v->canvas, s, MARGIN + 24, LIST_Y0 + 24, MARGIN,
                         LIST_Y0, LAUNCHER_VIEW_W - 2 * MARGIN, LIST_H);
            SDL_FreeSurface(s);
        }
    }
    draw_body(v, v->font_small, body, LIST_Y0 + 24 + LAUNCHER_VIEW_ROW_H,
              kTextDim);
}

static void preflight_line(LauncherView *v, const char *text, int y, int h,
                           SDL_Color colour)
{
    SDL_Surface *s = render_fitted(v->font_small, text,
                                  LAUNCHER_VIEW_W - MARGIN * 2, colour);
    draw_cell(v, s, MARGIN, y, LAUNCHER_VIEW_W - MARGIN * 2, h);
    SDL_FreeSurface(s);
}

void launcher_view_compose_preflight(LauncherView *v,
                                     const PreflightSummary *summary, unsigned page)
{
    static const char *const warnings[PREFLIGHT_WARNING_COUNT] = {
        "Windows features may fail. Try a version made for mkxp-z.",
        "Background tasks may stall. Try playing; return here if it hangs.",
        "Online or external tools may fail. Play offline if possible.",
        "Save recovery is untested. Back up saves and keep a spare slot.",
        "Screen changes may fail. Keep the game's default screen size.",
        "MIDI music may be silent. Try a copy with Ogg or WAV music.",
        "Some sounds may be silent. Try a copy with Ogg or WAV audio.",
        "Large maps may draw incorrectly. Check for missing map tiles.",
        "Large images may run out of memory. Try lower-resolution assets.",
        "Extra scripts may fail. Try a version without optional add-ons.",
        "Some files could not be checked, so other problems may exist."
    };
    static const char *const rules[PREFLIGHT_WARNING_COUNT] = {
        "native", "threads", "process", "save", "resize",
        "midi", "audio", "tileset", "image", "dynamic", "incomplete"
    };
    unsigned i, row = 0;
    char line[160];

    if (!v || !v->canvas)
        return;
    page %= preflight_pages(summary);
    fill(v->canvas, 0, 0, LAUNCHER_VIEW_W, LAUNCHER_VIEW_H, kBackground);
    draw_header(v, page ? "Game preflight - Details" : "Before you play",
                "Not tested on this build");
    draw_footer(v, page ? "Cross/Circle: Continue   Square: Back   Left/Right: Warnings / Details" :
                         "Cross/Circle: Continue   Square: Back   Left/Right: Details");
    if (!summary) {
        preflight_line(v, "No readable game check. You can still try playing.", LIST_Y0, 44, kTextDim);
        return;
    }
    if (!page) {
        preflight_line(v, "These are possible problems. You can still try playing.", LIST_Y0, 44, kTextDim);
        for (i = 0; i < PREFLIGHT_WARNING_COUNT; ++i) {
            int y;
            if (!summary->counts[i]) continue;
            y = LIST_Y0 + 44 + (int)row++ * 32;
            fill(v->canvas, MARGIN, y, LAUNCHER_VIEW_W - MARGIN * 2, 1, kRule);
            preflight_line(v, warnings[i], y, 32, kTextBright);
        }
        if (!row) preflight_line(v, "No warnings found. Save often while trying this game.",
                                 LIST_Y0 + 44, 44, kTextBright);
    } else {
        snprintf(line, sizeof(line), "%s | Host scan %s | Reference counts do not prove a feature runs.",
                 summary->engine, summary->complete ? "complete" : "incomplete");
        preflight_line(v, line, LIST_Y0, 44, kTextDim);
        for (i = 0; i < PREFLIGHT_WARNING_COUNT; i += 2) {
            snprintf(line, sizeof(line), "%s: %u%s%s", rules[i], summary->counts[i],
                     i + 1 < PREFLIGHT_WARNING_COUNT ? "    |    " : "",
                     i + 1 < PREFLIGHT_WARNING_COUNT ? rules[i + 1] : "");
            if (i + 1 < PREFLIGHT_WARNING_COUNT)
                snprintf(line + strlen(line), sizeof(line) - strlen(line), ": %u", summary->counts[i + 1]);
            preflight_line(v, line, LIST_Y0 + 44 + (int)row++ * 28, 28, kTextBright);
        }
        snprintf(line, sizeof(line), "Report %.12s    |    Inputs %.12s", summary->report_hash, summary->input_hash);
        preflight_line(v, line, 304, 32, kTextDim);
        snprintf(line, sizeof(line), "Scanner %.12s    |    Rules %.12s", summary->scanner_hash, summary->rulebook_hash);
        preflight_line(v, line, 336, 32, kTextDim);
        preflight_line(v, "The hashes identify the scan behind this page. Game files may have changed since.",
                       400, 44, kTextDim);
    }
}
