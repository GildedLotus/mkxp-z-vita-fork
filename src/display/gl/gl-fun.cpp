/*
 ** gl-fun.cpp
 **
 ** This file is part of mkxp.
 **
 ** Copyright (C) 2014 - 2021 Amaryllis Kulla <ancurio@mapleshrine.eu>
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

#include "gl-fun.h"

#include "boost-hash.h"
#include "exception.h"

#include <SDL_video.h>
#include <string>
#include <cstdio>

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "vita_glue.h"
#include <stdint.h>
#endif

GLFunctions gl;

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
/* Crash-safe one-liner. vita_glue_trace already fflushes. */
static void glTrace(const char *msg)
{
	vita_glue_trace(msg);
}

static void glTracePtr(const char *tag, const void *p)
{
	char tb[160];
	snprintf(tb, sizeof(tb), "%s %p", tag, p);
	vita_glue_trace(tb);
}

#else
#define glTrace(msg) do { } while (0)
#define glTracePtr(tag, p) do { (void)(tag); (void)(p); } while (0)
#endif

typedef const GLubyte* (APIENTRYP _PFNGLGETSTRINGIPROC) (GLenum, GLuint);

static void parseExtensionsCore(_PFNGLGETINTEGERVPROC GetIntegerv, BoostSet<std::string> &out)
{
    _PFNGLGETSTRINGIPROC GetStringi =
    (_PFNGLGETSTRINGIPROC) SDL_GL_GetProcAddress("glGetStringi");
    
    GLint extCount = 0;
    GetIntegerv(GL_NUM_EXTENSIONS, &extCount);
    
    for (GLint i = 0; i < extCount; ++i)
        out.insert((const char*) GetStringi(GL_EXTENSIONS, i));
}

static void parseExtensionsCompat(_PFNGLGETSTRINGPROC GetString, BoostSet<std::string> &out)
{
    const char *ext = (const char*) GetString(GL_EXTENSIONS);
    
    if (!ext)
        return;
    
    char buffer[0x100];
    size_t bufferI;
    
    while (*ext)
    {
        bufferI = 0;
        while (*ext && *ext != ' ')
            buffer[bufferI++] = *ext++;
        
        buffer[bufferI] = '\0';
        
        out.insert(buffer);
        
        if (*ext == ' ')
            ++ext;
    }
}

#define GL_FUN(name, type) \
gl.name = (type) SDL_GL_GetProcAddress("gl" #name EXT_SUFFIX);

#define EXC(msg) \
Exception(Exception::MKXPError, "%s", msg)

void initGLFunctions()
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	glTrace("trace: initGLFunctions enter");
#endif
#define EXT_SUFFIX ""
    GL_20_FUN;

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* vitaGL resolves every GLES2 entry point through SDL_GL_GetProcAddress;
	 * no linked fallback is installed, so a lookup failure is never masked. */
	glTracePtr("trace: GetString proc", (const void *)gl.GetString);
	glTrace("trace: initGLFunctions GL_20_FUN loaded + fallbacks");
	{
		int nullCount = 0;
#undef GL_FUN
#define GL_FUN(name, type) if (!gl.name) ++nullCount; (void)0;
		GL_20_FUN;
#undef GL_FUN
#define GL_FUN(name, type) \
gl.name = (type) SDL_GL_GetProcAddress("gl" #name EXT_SUFFIX);
		char tb[160];
		snprintf(tb, sizeof(tb),
		         "trace: initGLFunctions residual-null GL_20_FUN=%d", nullCount);
		vita_glue_trace(tb);
	}
#endif

    /* Determine GL version */
    const char *ver = 0;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	if (!gl.GetString)
		throw EXC("gl.GetString is NULL after GetProcAddress + linked fallback");
	ver = (const char*) gl.GetString(GL_VERSION);
	{
		char tb[160];
		snprintf(tb, sizeof(tb), "trace: glGetString(GL_VERSION)=%s",
		         ver ? ver : "(null)");
		vita_glue_trace(tb);
	}
	if (!ver)
		throw EXC("glGetString(GL_VERSION) returned NULL");
#else
    ver = (const char*) gl.GetString(GL_VERSION);
#endif
    
    const char glesPrefix[] = "OpenGL ES ";
    const size_t glesPrefixN = sizeof(glesPrefix)-1;
    
    bool gles = false;
    
    if (!strncmp(ver, glesPrefix, glesPrefixN))
    {
        gles = true;
        gl.glsles = true;
        
        ver += glesPrefixN;
    }
    
    /* Assume single digit */
    int glMajor = *ver - '0';
    
    if (glMajor < 2)
#ifndef GLES2_HEADER
        throw Exception(Exception::MKXPError,
                  "A graphics card that supports OpenGL 2.0 or later is required.\n\n"
                  "Driver information:\n"
                  "Vendor: %s\n"
                  "Renderer: %s\n"
                  "Version: %s\n"
                  "GLSL Version: %s\n",
                  gl.GetString(GL_VENDOR), gl.GetString(GL_RENDERER), gl.GetString(GL_VERSION),
                  gl.GetString(GL_SHADING_LANGUAGE_VERSION));
#else
        // on macOS, we're actually using either desktop GL or Metal due to ANGLE, but every Mac that supports Sierra
        // (officially or otherwise) should support ANGLE, so this should never be seen. Probably, anyway. Don't @ me
        throw EXC("A graphics card that supports OpenGL ES 2.0 or later is required.");
#endif
    
    if (gles)
    {
        GL_ES_FUN;
    }


#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	{
		char tb[160];
		snprintf(tb, sizeof(tb),
		         "trace: initGLFunctions version parsed gles=%d major=%d",
		         (int)gles, glMajor);
		vita_glue_trace(tb);
	}
#endif

    BoostSet<std::string> ext;

    if (glMajor >= 3)
        parseExtensionsCore(gl.GetIntegerv, ext);
    else
        parseExtensionsCompat(gl.GetString, ext);

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	{
		unsigned n = 0;
		for (auto it = ext.cbegin(); it != ext.cend(); ++it)
			++n;
		char tb[160];
		snprintf(tb, sizeof(tb), "trace: initGLFunctions ext count=%u", n);
		vita_glue_trace(tb);
	}
#endif
    
#define HAVE_EXT(_ext) ext.contains("GL_" #_ext)
    
    /* FBO entrypoints */
    if (glMajor >= 3 || HAVE_EXT(ARB_framebuffer_object))
    {
#undef EXT_SUFFIX
#define EXT_SUFFIX ""
        GL_FBO_FUN;
        GL_FBO_BLIT_FUN;
    }
    else if (gles && glMajor == 2)
    {
        GL_FBO_FUN;
    }
    else if (HAVE_EXT(EXT_framebuffer_object))
    {
#undef EXT_SUFFIX
#define EXT_SUFFIX "EXT"
        GL_FBO_FUN;
        
        if (HAVE_EXT(EXT_framebuffer_blit))
        {
            GL_FBO_BLIT_FUN;
        }
    }
    else
    {
        throw EXC("No FBO support available");
    }

    /* VAO entrypoints */
    if (HAVE_EXT(ARB_vertex_array_object) || glMajor >= 3)
    {
#undef EXT_SUFFIX
#define EXT_SUFFIX ""
        GL_VAO_FUN;
    }
    else if (HAVE_EXT(APPLE_vertex_array_object))
    {
#undef EXT_SUFFIX
#define EXT_SUFFIX "APPLE"
        GL_VAO_FUN;
    }
    else if (HAVE_EXT(OES_vertex_array_object))
    {
#undef EXT_SUFFIX
#define EXT_SUFFIX "OES"
        GL_VAO_FUN;
    }

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* Native VAOs stay off: the software VAO path rebinds VBO/IBO/attribs
	 * every draw and allocates no GL object per Quad. mkxp-z builds one VAO
	 * per Sprite, Window and Plane, and each native VAO would take a GPU
	 * program/object slot, so an allocation could fail at an arbitrary point
	 * mid-game. Force HAVE_NATIVE_VAO false. */
	gl.GenVertexArrays = 0;
	gl.DeleteVertexArrays = 0;
	gl.BindVertexArray = 0;
	glTrace("trace: Vita forced software VAO (native VAO disabled)");
#endif
    
    /* Debug callback entrypoints */
    if (HAVE_EXT(KHR_debug))
    {
#undef EXT_SUFFIX
#define EXT_SUFFIX ""
        GL_DEBUG_KHR_FUN;
    }
    else if (HAVE_EXT(ARB_debug_output))
    {
#undef EXT_SUFFIX
#define EXT_SUFFIX "ARB"
        GL_DEBUG_KHR_FUN;
    }
    
    if (HAVE_EXT(GREMEDY_string_marker))
    {
#undef EXT_SUFFIX
#define EXT_SUFFIX "GREMEDY"
        GL_GREMEMDY_FUN;
    }
    
    /* Misc caps */
    if (!gles || glMajor >= 3 || HAVE_EXT(EXT_unpack_subimage))
        gl.unpack_subimage = true;

    if (!gles || glMajor >= 3 || HAVE_EXT(OES_texture_npot))
        gl.npot_repeat = true;

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	{
		char tb[256];
		snprintf(tb, sizeof(tb),
		         "trace: initGLFunctions leave FBO=%p Clear=%p CreateShader=%p "
		         "GenBuffers=%p BindBuffer=%p BufferData=%p "
		         "DrawElements=%p TexImage2D=%p UseProgram=%p",
		         (const void *)gl.GenFramebuffers, (const void *)gl.Clear,
		         (const void *)gl.CreateShader, (const void *)gl.GenBuffers,
		         (const void *)gl.BindBuffer, (const void *)gl.BufferData,
		         (const void *)gl.DrawElements, (const void *)gl.TexImage2D,
		         (const void *)gl.UseProgram);
		vita_glue_trace(tb);
	}
	/* Diagnostics: caps + extension list (NPOT / VAO / unpack). */
	{
		GLint maxTex = 0;
		if (gl.GetIntegerv)
			gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
		char tb[160];
		snprintf(tb, sizeof(tb), "trace: GL_MAX_TEXTURE_SIZE=%d npot_repeat=%d unpack_subimage=%d",
		         (int)maxTex, (int)gl.npot_repeat, (int)gl.unpack_subimage);
		vita_glue_trace(tb);
	}
	{
		char line[200];
		int pos = 0;
		unsigned n = 0;
		for (auto it = ext.cbegin(); it != ext.cend(); ++it) {
			const std::string &e = *it;
			if (pos == 0) {
				pos = snprintf(line, sizeof(line), "trace: GLext %u:", n);
			}
			int need = (int)e.size() + 1;
			if (pos + need >= (int)sizeof(line) - 1) {
				vita_glue_trace(line);
				pos = snprintf(line, sizeof(line), "trace: GLext %u:", n);
			}
			pos += snprintf(line + pos, sizeof(line) - pos, " %s", e.c_str());
			++n;
		}
		if (pos > 0)
			vita_glue_trace(line);
		snprintf(line, sizeof(line),
		         "trace: ext OES_texture_npot=%d OES_vertex_array_object=%d EXT_unpack_subimage=%d",
		         (int)HAVE_EXT(OES_texture_npot),
		         (int)HAVE_EXT(OES_vertex_array_object),
		         (int)HAVE_EXT(EXT_unpack_subimage));
		vita_glue_trace(line);
	}
#endif
}
