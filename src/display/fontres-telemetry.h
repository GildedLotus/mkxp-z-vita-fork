// SPDX-License-Identifier: GPL-3.0-or-later
/* Font-resource attribution telemetry (diagnostic only).
 * Installed at SharedFontState construction iff FONT_RES_MARKER exists.
 * Fixed records; no allocation from log hooks. FT wrapper tracks live/peak
 * and failed sizes. Correlated FT_Open_Face probe on TTF open failure uses
 * a private library so the numeric error is captured before SDL_ttf
 * replaces it with a generic string. */
#ifndef MKXPZ_FONTRES_TELEMETRY_H
#define MKXPZ_FONTRES_TELEMETRY_H
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include <cstdio>
#include <cstring>
#if defined(__has_include) && __has_include(<malloc.h>)
#include <malloc.h>
#elif !defined(FONTRES_MALLINFO_DEFINED)
struct mallinfo { int arena, uordblks, fordblks, keepcost; };
static inline struct mallinfo mallinfo(void) { struct mallinfo m = {}; return m; }
#define FONTRES_MALLINFO_DEFINED
#endif
#if defined(FT_MODULE_H) && !defined(FONTRES_FT_MODULE_STRIPPED)
#include FT_MODULE_H
#elif !defined(FONTRES_FT_DEFINED)
/* Minimal FreeType surface used by the correlated open probe. */
typedef struct FT_MemoryRec_ *FT_Memory;
typedef void *(*FT_Alloc_Func)(FT_Memory, long);
typedef void (*FT_Free_Func)(FT_Memory, void *);
typedef void *(*FT_Realloc_Func)(FT_Memory, long, long, void *);
struct FT_MemoryRec_ { void *user; FT_Alloc_Func alloc; FT_Free_Func free; FT_Realloc_Func realloc; };
typedef struct FT_MemoryRec_ FT_MemoryRec;
typedef struct FT_LibraryRec_ *FT_Library;
typedef struct FT_FaceRec_ *FT_Face;
typedef unsigned char FT_Byte;
typedef long FT_Long;
typedef int FT_Error;
#define FT_OPEN_MEMORY 1
struct FT_Open_Args { int flags; const FT_Byte *memory_base; FT_Long memory_size; };
static FT_Error FT_New_Library(FT_Memory, FT_Library *out) { *out = (FT_Library)1; return 0; }
static void FT_Done_Library(FT_Library) {}
static void FT_Add_Default_Modules(FT_Library) {}
static FT_Error FT_Open_Face(FT_Library, FT_Open_Args *, FT_Long, FT_Face *out) { *out = 0; return 7; }
static void FT_Done_Face(FT_Face) {}
#define FONTRES_FT_DEFINED
#endif
#if !defined(FONTRES_HAS_TRACE) && !defined(VITA_GLUE_H)
static inline void vita_glue_trace(const char *) {}
#endif
#ifndef FONT_RES_MARKER
#if defined(MKXPZ_HOST_PORT_LOGIC) && !defined(__vita__)
#define FONT_RES_MARKER "font-resource.probe"
#else
#define FONT_RES_MARKER "ux0:/data/mkxp-z/font-resource.probe"
#endif
#endif
#ifndef MKXPZ_FONT_RES_MAX
#define MKXPZ_FONT_RES_MAX 22
#endif
enum FontResStage {
	FR_IDLE = 0, FR_BEGUN, FR_SLURPED, FR_TTF, FR_SIZED, FR_POOLED,
	FR_REFUSED, FR_DONE
};
struct FontResHeap { unsigned arena, used, freeB; };
struct FontResRec {
	unsigned index;
	char family[32];
	int size, ppem, outline, hit;
	unsigned copies;
	unsigned long copy_bytes;
	unsigned live_rw, live_files;
	unsigned slurp_reason;
	unsigned long slurp_bytes;
	int read_err, seek_err, close_err;
	int ft_error;
	unsigned long ft_fail_size;
	int stage;
	struct FontResHeap h0, h1, h2, h3;
};
static struct FontResStore {
	int installed, active, stopped;
	unsigned count, dropped, requests;
	unsigned long ft_live, ft_peak, ft_last_fail;
	unsigned ft_fails, ft_realloc_fails;
	struct FontResRec rec[MKXPZ_FONT_RES_MAX];
	struct FontResRec cur;
} g_fontRes;
static void frHeap(struct FontResHeap *h)
{
	const struct mallinfo m = mallinfo();
	h->arena = (unsigned)m.arena;
	h->used = (unsigned)m.uordblks;
	h->freeB = (unsigned)m.fordblks;
}
static void *frAlloc(FT_Memory, long size)
{
	if (size <= 0)
		return 0;
	unsigned char *raw = (unsigned char *)std::malloc((size_t)size + 16);
	if (!raw) {
		g_fontRes.ft_last_fail = (unsigned long)size;
		++g_fontRes.ft_fails;
		return 0;
	}
	*(long *)raw = size;
	g_fontRes.ft_live += (unsigned long)size;
	if (g_fontRes.ft_live > g_fontRes.ft_peak)
		g_fontRes.ft_peak = g_fontRes.ft_live;
	return raw + 16;
}
static void frFree(FT_Memory, void *block)
{
	if (!block)
		return;
	unsigned char *raw = (unsigned char *)block - 16;
	const long size = *(long *)raw;
	if (g_fontRes.ft_live >= (unsigned long)size)
		g_fontRes.ft_live -= (unsigned long)size;
	else
		g_fontRes.ft_live = 0;
	std::free(raw);
}
static void *frRealloc(FT_Memory, long cur_size, long new_size, void *block)
{
	if (!block)
		return frAlloc(0, new_size);
	if (new_size <= 0) {
		frFree(0, block);
		return 0;
	}
	unsigned char *raw = (unsigned char *)block - 16;
	unsigned char *grown = (unsigned char *)std::realloc(raw, (size_t)new_size + 16);
	if (!grown) {
		g_fontRes.ft_last_fail = (unsigned long)new_size;
		++g_fontRes.ft_realloc_fails;
		return 0;
	}
	*(long *)grown = new_size;
	g_fontRes.ft_live = g_fontRes.ft_live - (unsigned long)cur_size
	                    + (unsigned long)new_size;
	if (g_fontRes.ft_live > g_fontRes.ft_peak)
		g_fontRes.ft_peak = g_fontRes.ft_live;
	return grown + 16;
}
static struct FT_MemoryRec_ g_frMem;
static FT_Library g_frLib;
static void frLog(const struct FontResRec *r)
{
	char line[360];
	std::snprintf(line, sizeof(line),
	              "font-res: i=%u fam=%.31s sz=%d ppem=%d ol=%d hit=%d "
	              "copies=%u cbytes=%lu rw=%u files=%u slurp=%u sbytes=%lu "
	              "rerr=%d serr=%d cerr=%d ft=%d ftfail=%lu stage=%d "
	              "h0=%u/%u/%u h1=%u/%u/%u h2=%u/%u/%u h3=%u/%u/%u",
	              r->index, r->family, r->size, r->ppem, r->outline, r->hit,
	              r->copies, r->copy_bytes, r->live_rw, r->live_files,
	              r->slurp_reason, r->slurp_bytes, r->read_err, r->seek_err,
	              r->close_err, r->ft_error, r->ft_fail_size, r->stage,
	              r->h0.arena, r->h0.used, r->h0.freeB,
	              r->h1.arena, r->h1.used, r->h1.freeB,
	              r->h2.arena, r->h2.used, r->h2.freeB,
	              r->h3.arena, r->h3.used, r->h3.freeB);
	vita_glue_trace(line);
}
static void frFinish(int stage, int hit, int ppem)
{
	if (!g_fontRes.active)
		return;
	g_fontRes.cur.stage = stage;
	g_fontRes.cur.hit = hit;
	g_fontRes.cur.ppem = ppem;
	g_fontRes.cur.ft_fail_size = g_fontRes.ft_last_fail;
	if (g_fontRes.count < MKXPZ_FONT_RES_MAX) {
		g_fontRes.rec[g_fontRes.count] = g_fontRes.cur;
		g_fontRes.rec[g_fontRes.count].index = g_fontRes.requests;
		frLog(&g_fontRes.rec[g_fontRes.count]);
		++g_fontRes.count;
	} else {
		++g_fontRes.dropped;
	}
	if (stage == FR_REFUSED)
		g_fontRes.stopped = 1;
	g_fontRes.active = g_fontRes.stopped ? 0 : 1;
}
static void frBegin(const char *family, int size, int outline)
{
	if (!g_fontRes.installed || g_fontRes.stopped)
		return;
	struct FontResHeap h;
	frHeap(&h);
	if (h.freeB < 262144u || (h.arena > 0 && h.arena - h.used < 1048576u)) {
		g_fontRes.stopped = 1;
		g_fontRes.active = 0;
		return;
	}
	++g_fontRes.requests;
	g_fontRes.active = 1;
	g_fontRes.cur = FontResRec();
	std::snprintf(g_fontRes.cur.family, sizeof(g_fontRes.cur.family), "%s",
	              family ? family : "");
	g_fontRes.cur.size = size;
	g_fontRes.cur.outline = outline;
	g_fontRes.cur.ft_error = -1000;
	g_fontRes.cur.stage = FR_BEGUN;
	frHeap(&g_fontRes.cur.h0);
}
static void frAttribute(const void *bytes, size_t nbytes)
{
	if (!g_fontRes.active || !g_frLib)
		return;
	struct FontResRec *r = &g_fontRes.cur;
	if (bytes && nbytes) {
		FT_Face face = 0;
		FT_Open_Args args;
		std::memset(&args, 0, sizeof(args));
		args.flags = FT_OPEN_MEMORY;
		args.memory_base = (const FT_Byte *)bytes;
		args.memory_size = (FT_Long)nbytes;
		r->ft_error = (int)FT_Open_Face(g_frLib, &args, 0, &face);
		if (face)
			FT_Done_Face(face);
	}
	r->ft_fail_size = g_fontRes.ft_last_fail;
}
static void fontResInstall(void)
{
	if (g_fontRes.installed)
		return;
	FILE *marker = std::fopen(FONT_RES_MARKER, "rb");
	if (!marker)
		return;
	std::fclose(marker);
	g_fontRes.installed = 1;
	g_frMem.user = 0;
	g_frMem.alloc = frAlloc;
	g_frMem.free = frFree;
	g_frMem.realloc = frRealloc;
	if (FT_New_Library(&g_frMem, &g_frLib) != 0)
		g_frLib = 0;
	else
		FT_Add_Default_Modules(g_frLib);
	vita_glue_trace("font-res: installed");
}
static void fontResShutdown(void)
{
	if (!g_fontRes.installed)
		return;
	char line[160];
	std::snprintf(line, sizeof(line),
	              "font-res: end records=%u dropped=%u requests=%u stopped=%d "
	              "ft_live=%lu ft_peak=%lu ft_fails=%u ft_realloc_fails=%u "
	              "ft_last_fail=%lu",
	              g_fontRes.count, g_fontRes.dropped, g_fontRes.requests,
	              g_fontRes.stopped, g_fontRes.ft_live, g_fontRes.ft_peak,
	              g_fontRes.ft_fails, g_fontRes.ft_realloc_fails,
	              g_fontRes.ft_last_fail);
	vita_glue_trace(line);
	if (g_frLib) {
		FT_Done_Library(g_frLib);
		g_frLib = 0;
	}
	g_fontRes.installed = 0;
}
#else
static void fontResInstall(void) {}
static void fontResShutdown(void) {}
#endif
#endif
