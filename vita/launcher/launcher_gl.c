// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * launcher_gl.c — see launcher_gl.h.
 *
 * One texture, one VBO, one program: the smallest GL setup that reaches a
 * current GLES2 context on the device.
 */
#include "launcher_gl.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(__vita__) && defined(MKXPZ_VITAGL_BACKEND)
/* vitaGL backend: vitaGL.h is the GLES2 header surface and omits
 * this core constant. */
#include <vitaGL.h>
#ifndef GL_DITHER
#define GL_DITHER 0x0BD0
#endif
#else
#include <GLES2/gl2.h>
#endif

/* The shader pair: position + uv in, one sampler out. */
static const char *const kVS =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *const kFS =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "void main() { gl_FragColor = texture2D(uTex, vUV); }\n";

/*
 * pos.xy, uv.xy — one triangle strip covering the clip cube. v is flipped
 * relative to a GL-oriented quad because the source is a top-down CPU canvas
 * (row 0 is the top of the screen), not a GL-oriented render target.
 */
static const GLfloat kQuad[16] = {
    -1.f, -1.f, 0.f, 1.f,
     1.f, -1.f, 1.f, 1.f,
    -1.f,  1.f, 0.f, 0.f,
     1.f,  1.f, 1.f, 0.f,
};

static void trace_fmt(LauncherTraceFn trace, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void trace_fmt(LauncherTraceFn trace, const char *fmt, ...)
{
    char line[256];
    va_list ap;

    if (!trace)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = '\0';
    trace(line);
}

/* Bound error draining even if a broken context never returns GL_NO_ERROR. */
static GLenum take_errors(void)
{
    GLenum first = GL_NO_ERROR;
    int i;

    for (i = 0; i < 8; i++) {
        GLenum error = glGetError();
        if (error == GL_NO_ERROR)
            break;
        if (first == GL_NO_ERROR)
            first = error;
    }
    return first;
}

static int checked(LauncherTraceFn trace, const char *operation)
{
    GLenum error = take_errors();

    if (error == GL_NO_ERROR)
        return 1;
    trace_fmt(trace, "launcher: %s failed GL error=0x%x", operation,
              (unsigned int)error);
    return 0;
}

static GLuint compile_shader(GLenum type, const char *src, const char *tag,
                             LauncherTraceFn trace)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    if (!sh) {
        trace_fmt(trace, "launcher: glCreateShader(%s) returned 0", tag);
        return 0;
    }
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[256];
        GLsizei len = 0;

        info[0] = '\0';
        glGetShaderInfoLog(sh, (GLsizei)sizeof(info), &len, info);
        info[sizeof(info) - 1] = '\0';
        trace_fmt(trace, "launcher: shader %s COMPILE FAILED: %s", tag, info);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* The program is one code-heap allocation and must exist before the texture:
 * a shader that cannot allocate its code heap fails the draw silently, and we
 * would rather find that out before the texture holds any memory. */
static int build_program(LauncherGL *gl, LauncherTraceFn trace)
{
    GLuint vs, fs;
    GLint ok = 0;

    vs = compile_shader(GL_VERTEX_SHADER, kVS, "vs", trace);
    fs = compile_shader(GL_FRAGMENT_SHADER, kFS, "fs", trace);
    if (!vs || !fs) {
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        return 0;
    }

    gl->program = (unsigned int)glCreateProgram();
    if (!gl->program) {
        trace_fmt(trace, "launcher: glCreateProgram returned 0");
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }
    gl->programs++;

    glAttachShader((GLuint)gl->program, vs);
    glAttachShader((GLuint)gl->program, fs);
    glLinkProgram((GLuint)gl->program);
    glGetProgramiv((GLuint)gl->program, GL_LINK_STATUS, &ok);

    /* Detached-and-deleted the moment the link is done: the shader objects
     * are the compiler's, the code heap segment is the program's. */
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!ok) {
        char info[256];
        GLsizei len = 0;

        info[0] = '\0';
        glGetProgramInfoLog((GLuint)gl->program, (GLsizei)sizeof(info), &len,
                            info);
        info[sizeof(info) - 1] = '\0';
        trace_fmt(trace, "launcher: program LINK FAILED: %s", info);
        return 0;
    }

    gl->a_pos = (int)glGetAttribLocation((GLuint)gl->program, "aPos");
    gl->a_uv = (int)glGetAttribLocation((GLuint)gl->program, "aUV");
    gl->u_tex = (int)glGetUniformLocation((GLuint)gl->program, "uTex");
    if (gl->a_pos < 0 || gl->a_uv < 0 || gl->u_tex < 0) {
        trace_fmt(trace, "launcher: missing program locations aPos=%d aUV=%d "
                         "uTex=%d", gl->a_pos, gl->a_uv, gl->u_tex);
        return 0;
    }
    return 1;
}

const char *launcher_gl_budget_line(const LauncherGL *gl, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return buf;
    if (!gl) {
        buf[0] = '\0';
        return buf;
    }
    /* Plain %u, never the size_t length modifier: Vita newlib's printf
     * ignores that one and prints the literal letters instead. */
    snprintf(buf, cap,
             "launcher: gpu objects textures=%u vbos=%u programs=%u fbos=0",
             gl->textures, gl->vbos, gl->programs);
    buf[cap - 1] = '\0';
    return buf;
}

int launcher_gl_init(LauncherGL *gl, int width, int height,
                     LauncherTraceFn trace)
{
    char line[160];
    GLuint name = 0;

    if (!gl || width <= 0 || height <= 0)
        return 0;

    memset(gl, 0, sizeof(*gl));
    gl->a_pos = -1;
    gl->a_uv = -1;
    gl->u_tex = -1;
    gl->width = width;
    gl->height = height;

    (void)take_errors(); /* Errors left by the caller are not setup failures. */
    if (!build_program(gl, trace) || !checked(trace, "program setup")) {
        launcher_gl_shutdown(gl, trace);
        return 0;
    }

    glGenBuffers(1, &name);
    if (!name) {
        trace_fmt(trace, "launcher: glGenBuffers returned 0");
        launcher_gl_shutdown(gl, trace);
        return 0;
    }
    gl->vbo = (unsigned int)name;
    gl->vbos++;
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)gl->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(kQuad), kQuad,
                 GL_STATIC_DRAW);
    if (!checked(trace, "VBO storage")) {
        launcher_gl_shutdown(gl, trace);
        return 0;
    }

    name = 0;
    glGenTextures(1, &name);
    if (!name) {
        trace_fmt(trace, "launcher: glGenTextures returned 0");
        launcher_gl_shutdown(gl, trace);
        return 0;
    }
    gl->texture = (unsigned int)name;
    gl->textures++;
    glBindTexture(GL_TEXTURE_2D, (GLuint)gl->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* Tightly packed rows; SDL's 32-bit surfaces are 4-byte aligned anyway,
     * but the default of 4 is a promise this module does not want to make. */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    /* Allocate the level now, so a pool too small for it fails at init. */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if (!checked(trace, "texture storage")) {
        launcher_gl_shutdown(gl, trace);
        return 0;
    }
    gl->uploads++;

    /* The launcher draws one opaque quad over the whole screen: everything
     * below is state the driver would otherwise have to carry per draw. */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    if (!checked(trace, "draw state")) {
        launcher_gl_shutdown(gl, trace);
        return 0;
    }

    gl->ready = 1;
    if (trace)
        trace(launcher_gl_budget_line(gl, line, sizeof(line)));
    return 1;
}

void launcher_gl_upload(LauncherGL *gl, const void *pixels)
{
    if (!gl || !gl->ready || !pixels)
        return;

    gl->pending_pixels = pixels;
}

static int upload_pending(LauncherGL *gl)
{
    (void)take_errors();
    glBindTexture(GL_TEXTURE_2D, (GLuint)gl->texture);
    /* Re-specify the whole level. The texture is sampled every frame, so
     * vitaGL would copy the old level before a glTexSubImage2D; a new level
     * is one copy, and the old one is freed once the GPU is done with it
     * A failure leaves the previous level intact. */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)gl->width,
                 (GLsizei)gl->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 gl->pending_pixels);
    if (!checked(NULL, "texture upload"))
        return 0;
    gl->uploads++;
    gl->pending_pixels = NULL;
    gl->has_pixels = 1;
    return 1;
}

void launcher_gl_draw(LauncherGL *gl)
{
    if (!gl || !gl->ready)
        return;

    glViewport(0, 0, (GLsizei)gl->width, (GLsizei)gl->height);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Keep the CPU canvas dirty on failure; never sample undefined storage.
     * A draw makes at most one attempt, even after several view changes. */
    if (gl->pending_pixels && !upload_pending(gl))
        return;
    if (!gl->has_pixels)
        return;

    glUseProgram((GLuint)gl->program);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)gl->vbo);
    glEnableVertexAttribArray((GLuint)gl->a_pos);
    glEnableVertexAttribArray((GLuint)gl->a_uv);
    glVertexAttribPointer((GLuint)gl->a_pos, 2, GL_FLOAT, GL_FALSE, 16,
                          (const GLvoid *)0);
    glVertexAttribPointer((GLuint)gl->a_uv, 2, GL_FLOAT, GL_FALSE, 16,
                          (const GLvoid *)8);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)gl->texture);
    glUniform1i(gl->u_tex, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl->draws++;
}

void launcher_gl_shutdown(LauncherGL *gl, LauncherTraceFn trace)
{
    GLuint name;

    if (!gl)
        return;

    if (gl->texture || gl->vbo || gl->program) {
        unsigned char pixel[4];

        /* A returned swap can still be in flight. The default-framebuffer
         * 1x1 read waits without a full-size temp. */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    }
    if (gl->texture) {
        name = (GLuint)gl->texture;
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &name);
        gl->texture = 0;
    }
    if (gl->vbo) {
        name = (GLuint)gl->vbo;
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteBuffers(1, &name);
        gl->vbo = 0;
    }
    if (gl->program) {
        glUseProgram(0);
        glDeleteProgram((GLuint)gl->program);
        gl->program = 0;
    }
    gl->ready = 0;
    gl->pending_pixels = NULL;
    gl->has_pixels = 0;
    trace_fmt(trace, "launcher: gpu objects released textures=0 vbos=0 "
                     "programs=0 fbos=0");
}
