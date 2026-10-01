// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * launcher_gl.h — the launcher's entire GPU footprint.
 *
 * One texture, one VBO, one program, no FBO, no per-frame readback. The
 * contract dates from an earlier GL driver, which took a firmware sync
 * object per texture, VBO and render surface from a small process-wide pool.
 * It is kept on vitaGL because the launcher runs in the SAME eboot as the
 * player and hands the process to it with sceAppMgrLoadExec, so every object
 * the launcher holds at that moment is a question mark over the game that
 * follows. Three objects is the whole bill:
 *
 *     texture 1  +  VBO 1  +  program (code-heap segments)  =  0 surfaces
 *
 * The shader pair, the 4-vertex strip and the "upload a whole level, draw one
 * quad" loop are the setup that was measured on hardware.
 *
 * Rules this module keeps, each measured rather than assumed:
 *
 *  - Whole-level uploads only, as glTexImage2D. The canvas texture is
 *    sampled every frame, and vitaGL copies such a level before any
 *    glTexSubImage2D. The launcher composes its 960x544 canvas on the CPU
 *    and uploads all of it, or nothing.
 *  - Upload only when the view is dirty. The list changes on a keypress, not
 *    on a frame, so a still screen costs one draw and a swap.
 *  - No FBO and no per-frame readback. The launcher has one full-screen
 *    quad to draw and no reason to hold a render surface across the
 *    hand-over.
 *
 * C99 + <GLES2/gl2.h> only. No SDL, no Vita headers, no allocation, so
 * this file compiles on the host against any gl2.h.
 */
#ifndef VITA_LAUNCHER_GL_H
#define VITA_LAUNCHER_GL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One-line diagnostic sink. Every launcher file takes one of these rather
 * than calling vita_glue_trace directly: the glue is a Vita object, and a
 * host-compilable build links none of it. On hardware the front end passes
 * vita_glue_trace itself, so the lines land in the same log as the boot
 * breadcrumbs, in order. NULL means "say nothing".
 *
 * printf is never an option here — see the long comment in vita_glue.h: on
 * this newlib no printf line has ever reached a Vita log.
 */
#ifndef VITA_LAUNCHER_TRACE_FN_DEFINED
#define VITA_LAUNCHER_TRACE_FN_DEFINED
typedef void (*LauncherTraceFn)(const char *msg);
#endif

/*
 * Caller-owned state. Zero it (or just let it be static) before init; a
 * failed init leaves it safe to pass to launcher_gl_shutdown.
 */
typedef struct LauncherGL {
    unsigned int program;
    unsigned int vbo;
    unsigned int texture;
    int a_pos;
    int a_uv;
    int u_tex;
    int width;
    int height;
    int ready;              /* 1 once the whole set exists */
    const void *pending_pixels; /* borrowed CPU canvas, retained for retry */
    int has_pixels;         /* 1 after the first successful canvas upload */

    /* Budget bookkeeping, mirrored into the one log line below so a hardware
     * log can be read without a debugger. */
    unsigned int textures;  /* glGenTextures calls that succeeded */
    unsigned int vbos;
    unsigned int programs;
    unsigned int uploads;   /* successful allocation + whole-level uploads */
    unsigned int draws;
} LauncherGL;

/*
 * Build the whole set on the current GL context: compile + link the program,
 * upload the static 4-vertex strip, allocate the width x height GL_RGBA
 * level (CLAMP_TO_EDGE, GL_LINEAR, no mipmaps).
 *
 * Emits exactly one line on success:
 *
 *     launcher: gpu objects textures=1 vbos=1 programs=1 fbos=0
 *
 * Returns 1 on success, 0 on failure (the reason is traced). On failure the
 * partial set is released, so the caller may go straight to shutdown.
 */
int launcher_gl_init(LauncherGL *gl, int width, int height,
                     LauncherTraceFn trace);

/*
 * Queue the whole texture level from `pixels` (width*height RGBA8888
 * bytes, tightly packed — the canvas SDL_Surface's own pixels).
 *
 * Call it only when the canvas actually changed. The next draw attempts the
 * upload, retaining it for later draws on failure. Multiple calls coalesce
 * to the latest canvas. Keep the borrowed pixels valid until upload succeeds,
 * another canvas replaces them, or shutdown (the front end owns a persistent
 * canvas). There is deliberately no sub-rectangle entry point.
 */
void launcher_gl_upload(LauncherGL *gl, const void *pixels);

/* Clear, attempt one pending upload, and draw the quad over the viewport.
 * Clear only until pixels are uploaded successfully, including on retry.
 * No swap: the window belongs to the caller (SDL_GL_SwapWindow on the Vita). */
void launcher_gl_draw(LauncherGL *gl);

/* Drain once with a 1x1 default-framebuffer read, then delete the texture,
 * VBO and program. Idempotent; safe on a zeroed or half-initialised struct.
 * A live set requires its context to remain current through the drain. */
void launcher_gl_shutdown(LauncherGL *gl, LauncherTraceFn trace);

/*
 * Format the budget line into `buf` ("launcher: gpu objects textures=%u
 * vbos=%u programs=%u fbos=0"). Exposed so the caller can log the same line
 * again just before handing the process over, when the interesting question
 * is what is still live. Returns `buf`.
 */
const char *launcher_gl_budget_line(const LauncherGL *gl, char *buf,
                                    size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* VITA_LAUNCHER_GL_H */
