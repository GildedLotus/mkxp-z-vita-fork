/*
** shader.cpp
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

#include "shader.h"
#include "config.h"
#include "graphics.h"
#include "sharedstate.h"
#include "glstate.h"
#include "exception.h"
#include "bootprofile.h"

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
#include "vita_glue.h"
#include <SDL_video.h>
#endif

/* The engine program-binary cache is compiled out on vitaGL, which reads its
 * own shipped GXP cache and has no program-binary extension. */
#if defined(__vita__) && !defined(MKXPZ_VITAGL_BACKEND)
#define MKXPZ_PROGRAM_BINARY_CACHE
#include "shader-cache.h"
#endif

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <iostream>

#include "common.h.xxd"
#include "sprite.frag.xxd"
#include "hue.frag.xxd"
#include "trans.frag.xxd"
#include "transSimple.frag.xxd"
#include "bitmapBlit.frag.xxd"
#include "plane.frag.xxd"
#include "gray.frag.xxd"
#include "flatColor.frag.xxd"
#include "simple.frag.xxd"
#include "simpleColor.frag.xxd"
#include "simpleAlpha.frag.xxd"
#include "simpleAlphaUni.frag.xxd"
#include "tilemap.frag.xxd"
#include "flashMap.frag.xxd"
#include "minimal.vert.xxd"
#include "simple.vert.xxd"
#include "simpleColor.vert.xxd"
#include "sprite.vert.xxd"
#include "tilemap.vert.xxd"
#include "blur.frag.xxd"
#include "simpleMatrix.vert.xxd"
#include "blurH.vert.xxd"
#include "blurV.vert.xxd"
#include "tilemapvx.vert.xxd"
#include "movieYuv.frag.xxd"

#define INIT_SHADER(vert, frag, name) \
{ \
	Shader::init(___shader_##vert##_vert, ___shader_##vert##_vert_len, ___shader_##frag##_frag, ___shader_##frag##_frag_len, \
	#vert, #frag, #name); \
}

#define GET_U(name) u_##name = gl.GetUniformLocation(program, #name)

/* A driver that fails the query leaves the length untouched, and one that
 * lies about it must not size an allocation. */
static GLint clampLogLength(GLint length)
{
	const GLint maxLength = 64 * 1024;

	return length < 0 ? 0 : (length > maxLength ? maxLength : length);
}

static void printShaderLog(GLuint shader)
{
	GLint logLength = 0;
	gl.GetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);

	std::string log(clampLogLength(logLength), '\0');
	gl.GetShaderInfoLog(shader, log.size(), 0, &log[0]);

	std::clog << "Shader log:\n" << log;
}

static void printProgramLog(GLuint program)
{
	GLint logLength = 0;
	gl.GetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);

	std::string log(clampLogLength(logLength), '\0');
	gl.GetProgramInfoLog(program, log.size(), 0, &log[0]);

	std::clog << "Program log:\n" << log;
}

Shader::Shader() : initialized(false), finalPresentationVariant(false)
{
	vertShader = gl.CreateShader(GL_VERTEX_SHADER);
	fragShader = gl.CreateShader(GL_FRAGMENT_SHADER);

	program = gl.CreateProgram();
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	/* ShaderSet default-constructs the boot programs; only scream on failure. */
	if (!vertShader || !fragShader || !program) {
		char tb[160];
		snprintf(tb, sizeof(tb),
		         "trace: Shader::Shader FAIL vert=%u frag=%u prog=%u",
		         (unsigned)vertShader, (unsigned)fragShader, (unsigned)program);
		vita_glue_trace(tb);
	}
#endif
}

Shader::~Shader()
{
	gl.DeleteProgram(program);
	gl.DeleteShader(vertShader);
	gl.DeleteShader(fragShader);
}

void Shader::bind()
{
	glState.program.set(program);
}

void Shader::unbind()
{
	gl.ActiveTexture(GL_TEXTURE0);
	glState.program.set(0);
}

static void setupShaderSource(GLuint shader, GLenum type,
                              const unsigned char *body, int bodySize, bool finalPresentation
#ifdef MKXPZ_PROGRAM_BINARY_CACHE
                              , ShaderCache::Key *key = nullptr
#endif
                              )
{
	static const char glesDefine[] = "#define GLSLES\n";
	static const char fragDefine[] = "#define FRAGMENT_SHADER\n";

	const GLchar *shaderSrc[5];
	GLint shaderSrcSize[5];
	size_t i = 0;
	static const char probeDefine[] = "#define FINAL_PRESENTATION_PROBE\n";
	if (finalPresentation) {
		shaderSrc[i] = probeDefine;
		shaderSrcSize[i++] = sizeof(probeDefine)-1;
	}

	if (gl.glsles)
	{
		shaderSrc[i] = glesDefine;
		shaderSrcSize[i] = sizeof(glesDefine)-1;
		++i;
	}

	if (type == GL_FRAGMENT_SHADER)
	{
		shaderSrc[i] = fragDefine;
		shaderSrcSize[i] = sizeof(fragDefine)-1;
		++i;
	}

	shaderSrc[i] = (const GLchar*) ___shader_common_h;
	shaderSrcSize[i] = ___shader_common_h_len;
	++i;

	shaderSrc[i] = (const GLchar*) body;
	shaderSrcSize[i] = bodySize;
	++i;

#ifdef MKXPZ_PROGRAM_BINARY_CACHE
	if (key) {
		key->number(type); key->number(i);
		for (size_t j = 0; j < i; ++j) key->bytes(shaderSrc[j], shaderSrcSize[j]);
	}
#endif
	gl.ShaderSource(shader, i, shaderSrc, shaderSrcSize);
}

void Shader::init(const unsigned char *vert, int vertSize,
                  const unsigned char *frag, int fragSize,
                  const char *vertName, const char *fragName,
                  const char *programName)
{
	if (initialized)
	{
		/* Calling Shader::init() more than once causes a small number of graphics drivers to encounter linking errors.
		 * In particular, the Nintendo Switch homebrew toolchain's Mesa driver has this problem.
		 * So we throw this exception on every platform to reduce the probability of regressions. */
		throw Exception(Exception::MKXPError,
	                    "Attempted to call Shader::init() more than once");
	}

#ifdef MKXPZ_SOFTWARE_BITMAPS
	/* Every program the engine can use is compiled and
	 * linked in ShaderSet's constructor, at boot. A program built after the
	 * boot seal would need a code-heap segment from an exhausted GPU pool
	 * and hard-fail. */
	GPUBudget::creationSite("shader program");
#endif

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	{
		char tb[160];
		snprintf(tb, sizeof(tb), "trace: Shader::init %s", programName);
		vita_glue_trace(tb);
	}
#endif

	BootProfile::begin(BootProfile::Shaders);
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	double compileMS = 0, linkMS = 0, binaryMS = 0;
#endif
	const struct { GLuint index; const char *name; } attributes[] = {
		{Position, "position"}, {TexCoord, "texCoord"}, {Color, "color"}
	};
#ifdef MKXPZ_PROGRAM_BINARY_CACHE
	ShaderCache::Session &cache = ShaderCache::session();
	ShaderCache::Key key = cache.identity();
	for (const auto &attribute : attributes) { key.number(attribute.index); key.text(attribute.name); }
	setupShaderSource(vertShader, GL_VERTEX_SHADER, vert, vertSize, finalPresentationVariant, &key);
	setupShaderSource(fragShader, GL_FRAGMENT_SHADER, frag, fragSize, finalPresentationVariant, &key);
	double binaryStart = vita_glue_frame_profile_now_us();
	const char *cacheResult = cache.load(program, key);
	binaryMS = (vita_glue_frame_profile_now_us() - binaryStart) / 1000.0;
	bool cacheHit = !std::strcmp(cacheResult, "hit");
#else
	bool cacheHit = false;
	const char *cacheResult = "disabled";
	(void)cacheResult;
	setupShaderSource(vertShader, GL_VERTEX_SHADER, vert, vertSize, finalPresentationVariant);
	setupShaderSource(fragShader, GL_FRAGMENT_SHADER, frag, fragSize, finalPresentationVariant);
#endif
	if (!cacheHit) {
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	double compileStart = vita_glue_frame_profile_now_us();
#endif
	GLint success;

	/* Compile vertex shader */
	gl.CompileShader(vertShader);

	gl.GetShaderiv(vertShader, GL_COMPILE_STATUS, &success);

	if (!success)
	{
		printShaderLog(vertShader);
		throw Exception(Exception::MKXPError,
	                    "GLSL: An error occurred while compiling vertex shader '%s' in program '%s'",
	                    vertName, programName);
	}

	/* Compile fragment shader */
	gl.CompileShader(fragShader);

	gl.GetShaderiv(fragShader, GL_COMPILE_STATUS, &success);

	if (!success)
	{
		printShaderLog(fragShader);
		throw Exception(Exception::MKXPError,
	                    "GLSL: An error occurred while compiling fragment shader '%s' in program '%s'",
	                    fragName, programName);
	}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	compileMS = (vita_glue_frame_profile_now_us() - compileStart) / 1000.0;
	double linkStart = vita_glue_frame_profile_now_us();
#endif
	/* Link shader program */
	gl.AttachShader(program, vertShader);
	gl.AttachShader(program, fragShader);

	for (const auto &attribute : attributes)
		gl.BindAttribLocation(program, attribute.index, attribute.name);

	gl.LinkProgram(program);

	gl.GetProgramiv(program, GL_LINK_STATUS, &success);

	if (!success)
	{
		printProgramLog(program);
#ifdef MKXPZ_VITAGL_BACKEND
		throw Exception(Exception::MKXPError,
	                    "GLSL: Cannot build program '%s' (vertex '%s', fragment '%s'): "
	                    "its precompiled shader is missing or stale and no shader compiler is installed",
	                    programName, vertName, fragName);
#endif
		throw Exception(Exception::MKXPError,
	                    "GLSL: An error occurred while linking program '%s' (vertex '%s', fragment '%s')",
	                    programName, vertName, fragName);
	}

#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	linkMS = (vita_glue_frame_profile_now_us() - linkStart) / 1000.0;
#endif
#ifdef MKXPZ_PROGRAM_BINARY_CACHE
	cache.save(program, key);
#endif
	}
	initialized = true;
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	{
		char tb[160];
		snprintf(tb, sizeof(tb), "vita-shader: name=%s compile_ms=%.3f link_ms=%.3f binary_ms=%.3f cache=%s prog=%u",
		         programName, compileMS, linkMS, binaryMS, cacheResult, (unsigned)program);
		vita_glue_trace(tb);
	}
#endif
}

void shaderBootComplete()
{
	BootProfile::end(BootProfile::Shaders);
#ifdef MKXPZ_VITAGL_BACKEND
	vita_glue_vgl_pool_ledger("shaders");
#endif
#ifdef MKXPZ_PROGRAM_BINARY_CACHE
	ShaderCache::session().summary();
#endif
}

void Shader::initFromFile(const char *_vertFile, const char *_fragFile,
                          const char *programName)
{
	std::string vertContents, fragContents;
	readFile(_vertFile, vertContents);
	readFile(_fragFile, fragContents);

	init((const unsigned char*) vertContents.c_str(), vertContents.size(),
	     (const unsigned char*) fragContents.c_str(), fragContents.size(),
	     _vertFile, _fragFile, programName);
}

void Shader::setVec2Uniform(GLint location, const Vec2 &vec)
{
    gl.Uniform2f(location, vec.x, vec.y);
}

void Shader::setVec4Uniform(GLint location, const Vec4 &vec)
{
	gl.Uniform4f(location, vec.x, vec.y, vec.z, vec.w);
}

void Shader::setTexUniform(GLint location, unsigned unitIndex, TEX::ID texture)
{
	GLenum texUnit = GL_TEXTURE0 + unitIndex;

	gl.ActiveTexture(texUnit);
	gl.BindTexture(GL_TEXTURE_2D, texture.gl);
	gl.Uniform1i(location, unitIndex);
	gl.ActiveTexture(GL_TEXTURE0);
}

void ShaderBase::GLProjMat::apply(const Vec2i &value)
{
	/* glOrtho replacement */
	const float a = 2.f / value.x;
	const float b = 2.f / value.y;
	const float c = -2.f;

	GLfloat mat[16] =
	{
		 a,  0,  0,  0,
		 0,  b,  0,  0,
		 0,  0,  c,  0,
		-1, -1, -1,  1
	};

	gl.UniformMatrix4fv(u_mat, 1, GL_FALSE, mat);
}

void ShaderBase::init()
{
	GET_U(texSizeInv);
	GET_U(translation);

	projMat.u_mat = gl.GetUniformLocation(program, "projMat");
}

void ShaderBase::applyViewportProj()
{
	// High-res: scale the matrix if we're rendering to the PingPong framebuffer.
	const IntRect &vp = glState.viewport.get();
	if (shState->config().enableHires && shState->graphics().isPingPongFramebufferActive() && framebufferScalingAllowed()) {
		projMat.set(Vec2i(shState->graphics().width(), shState->graphics().height()));
	}
	else {
		projMat.set(Vec2i(vp.w, vp.h));
	}
}

bool ShaderBase::framebufferScalingAllowed()
{
	return true;
}

void ShaderBase::setTexSize(const Vec2i &value)
{
	gl.Uniform2f(u_texSizeInv, 1.f / value.x, 1.f / value.y);
}

void ShaderBase::setTranslation(const Vec2i &value)
{
	gl.Uniform2f(u_translation, value.x, value.y);
}


FlatColorShader::FlatColorShader()
{
	INIT_SHADER(minimal, flatColor, FlatColorShader);

	ShaderBase::init();

	GET_U(color);
}

void FlatColorShader::setColor(const Vec4 &value)
{
	setVec4Uniform(u_color, value);
}


#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)

// Bounded boot-only selector; the eighth byte rejects any trailing content.
static bool finalPresentationBoot(int &selected, bool &coordinates)
{
	unsigned char bytes[8] = {};
	size_t count = 0;
	FILE *file = fopen("app0:/diagnostics/final-presentation-probe", "rb");
	const bool present = file != nullptr;
	bool readable = true;
	if (file) {
		count = fread(bytes, 1, sizeof(bytes), file);
		readable = !ferror(file);
		fclose(file);
	}
	selected = 0;
	if (readable) {
		if (count == 6 && !memcmp(bytes, "highp\n", 6)) selected = 1;
		if (count == 7 && !memcmp(bytes, "center\n", 7)) selected = 2;
		if (count == 7 && !memcmp(bytes, "affine\n", 7)) selected = 3;
	}
	char hex[17] = {};
	for (size_t i = 0; i < count; ++i)
		snprintf(hex + i * 2, 3, "%02x", bytes[i]);
	GLint range[2] = {}, precision = 0;
	GLenum error = GL_NO_ERROR;
	bool supported = false;
	if (gl.glsles) {
		typedef void (APIENTRYP Query)(GLenum, GLenum, GLint *, GLint *);
		Query query = reinterpret_cast<Query>(SDL_GL_GetProcAddress("glGetShaderPrecisionFormat"));
		if (query) {
			query(GL_FRAGMENT_SHADER, 0x8DF2 /* GL_HIGH_FLOAT */, range, &precision);
			error = gl.GetError();
			supported = error == GL_NO_ERROR && range[0] > 0 && range[1] > 0 && precision > 0;
		}
	}
	coordinates = supported && precision >= 23 && range[0] >= 23 && range[1] >= 23;
	char line[320];
	snprintf(line, sizeof(line),
	         "final-presentation: selector=%s bytes=%u hex=%s readable=%d highp=%s range=%d,%d precision=%d error=0x%x mode=%d coordinates=%d sentinel=ff00fd",
	         !present ? "absent" : selected ? "selected" : "malformed",
	         (unsigned)count, hex, (int)readable, supported ? "supported" : "unsupported",
	         range[0], range[1], precision, (unsigned)error, selected, (int)coordinates);
	vita_glue_trace(line);
	if (selected && (!supported || (selected >= 2 && !coordinates)))
		throw Exception(Exception::MKXPError, "final-presentation: REJECT unsupported fragment highp");
	/* The instrumented variant exists only for an explicitly selected mode.
	 * Hardware highp support alone used to enable it, so every product boot on
	 * the Vita compiled and drew the extra highp varying and its inline
	 * branches. The probe still reports support and
	 * still rejects an unsupported explicit selection above. */
	return selected != 0 && supported;
}
#endif

SimpleShader::SimpleShader()
{
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	finalPresentationVariant = finalPresentationBoot(finalPresentationSelected, finalPresentationCoordinates);
#endif
	INIT_SHADER(simple, simple, SimpleShader);

	ShaderBase::init();

	GET_U(texOffsetX);
	u_finalPresentationHighp = finalPresentationVariant
	    ? gl.GetUniformLocation(program, "finalPresentationHighp") : -1;
	if (finalPresentationVariant && u_finalPresentationHighp < 0)
		throw Exception(Exception::MKXPError, "final-presentation: missing selector uniform");
	if (finalPresentationVariant) {
		u_finalOrigin = gl.GetUniformLocation(program, "finalOrigin");
		u_finalExtent = gl.GetUniformLocation(program, "finalExtent");
		u_finalTexture = gl.GetUniformLocation(program, "finalTexture");
		u_finalInvM = gl.GetUniformLocation(program, "finalInvM");
		u_finalInvT = gl.GetUniformLocation(program, "finalInvT");
		if (u_finalOrigin < 0 || u_finalExtent < 0 || u_finalTexture < 0 ||
		    u_finalInvM < 0 || u_finalInvT < 0)
			throw Exception(Exception::MKXPError, "final-presentation: missing geometry uniform");
	}
}

void SimpleShader::setFinalPresentation(const Presentation &value)
{
	if (u_finalPresentationHighp < 0 || glState.program.get() != program) return;
	if (!value.mode && !finalPresentationValue.mode) return;
	gl.Uniform1i(u_finalPresentationHighp, value.mode);
	gl.Uniform2f(u_finalOrigin, value.origin.x, value.origin.y);
	gl.Uniform2f(u_finalExtent, value.extent.x, value.extent.y);
	gl.Uniform2f(u_finalTexture, value.texture.x, value.texture.y);
	gl.Uniform2f(u_finalInvM, value.extent.x > 0 ? 1.0f/(2.0f*value.extent.x) : 0.0f,
	             value.extent.y > 0 ? 1.0f/(2.0f*value.extent.y) : 0.0f);
	gl.Uniform2f(u_finalInvT, value.texture.x > 0 ? 1.0f/value.texture.x : 0.0f,
	             value.texture.y > 0 ? 1.0f/value.texture.y : 0.0f);
	finalPresentationValue = value;
}

int SimpleShader::finalPresentationFor(bool screen, bool smooth,
                                      const IntRect &src, const IntRect &dst,
                                      const IntRect &viewport, const Vec2i &texture, bool native)
{
	if (!finalPresentationSelected) return 0;
	const char *reason = nullptr;
	if (!finalPresentationVariant) reason = "capability";
	else if (native) reason = "native";
	else if (glState.program.get() != program) reason = "program";
	else if (!screen) reason = "offscreen";
	else if (smooth) reason = "smooth";
	else if (std::abs((long long)src.w) == std::abs((long long)dst.w) &&
	         std::abs((long long)src.h) == std::abs((long long)dst.h)) reason = "one-to-one";
	else if (finalPresentationSelected >= 2) {
		if (!finalPresentationCoordinates) reason = "precision";
		else if (texture.x <= 0 || texture.y <= 0 || texture.x > 4096 || texture.y > 4096)
			reason = "texture-bounds";
		else if (src.x != 0 || src.y != texture.y || src.w != texture.x || src.h != -texture.y)
			reason = "full-source-flip";
		else if (viewport.x != 0 || viewport.y != 0 || viewport.w <= 0 || viewport.h <= 0 ||
		         viewport.w > 960 || viewport.h > 544) reason = "viewport";
		else if (dst.w <= 0 || dst.h <= 0 || dst.w > 960 || dst.h > 960 ||
		         dst.x < 0 || dst.y < 0 || dst.x > viewport.w-dst.w || dst.y > viewport.h-dst.h)
			reason = "destination";
	}
#if defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)
	bool &logged = reason ? finalFallbackLogged : finalEligibleLogged;
	if (!logged) {
		char line[384];
		snprintf(line, sizeof(line),
		         "final-presentation: mode=%d %s=%s src=%d,%d,%d,%d dst=%d,%d,%d,%d viewport=%d,%d,%d,%d texture=%d,%d native=%d screen=%d smooth=%d",
		         finalPresentationSelected, reason ? "fallback" : "eligible", reason ? reason : "selected",
		         src.x,src.y,src.w,src.h,dst.x,dst.y,dst.w,dst.h,
		         viewport.x,viewport.y,viewport.w,viewport.h,texture.x,texture.y,(int)native,(int)screen,(int)smooth);
		vita_glue_trace(line);
		logged = true;
	}
#endif
	return reason ? 0 : finalPresentationSelected;
}

void SimpleShader::setTexOffsetX(int value)
{
	gl.Uniform1f(u_texOffsetX, value);
}


SimpleColorShader::SimpleColorShader()
{
	INIT_SHADER(simpleColor, simpleColor, SimpleColorShader);

	ShaderBase::init();
}


SimpleAlphaShader::SimpleAlphaShader()
{
	INIT_SHADER(simpleColor, simpleAlpha, SimpleAlphaShader);

	ShaderBase::init();
}


SimpleSpriteShader::SimpleSpriteShader()
{
	INIT_SHADER(sprite, simple, SimpleSpriteShader);

	ShaderBase::init();

	GET_U(spriteMat);
}

void SimpleSpriteShader::setSpriteMat(const float value[16])
{
	gl.UniformMatrix4fv(u_spriteMat, 1, GL_FALSE, value);
}

AlphaSpriteShader::AlphaSpriteShader()
{
	INIT_SHADER(sprite, simpleAlphaUni, AlphaSpriteShader);

	ShaderBase::init();

	GET_U(spriteMat);
	GET_U(alpha);
}

void AlphaSpriteShader::setSpriteMat(const float value[16])
{
	gl.UniformMatrix4fv(u_spriteMat, 1, GL_FALSE, value);
}

void AlphaSpriteShader::setAlpha(float value)
{
	gl.Uniform1f(u_alpha, value);
}


TransShader::TransShader()
{
	INIT_SHADER(simple, trans, TransShader);

	ShaderBase::init();

	GET_U(currentScene);
	GET_U(frozenScene);
	GET_U(transMap);
	GET_U(prog);
	GET_U(vague);
}

void TransShader::setCurrentScene(TEX::ID tex)
{
	setTexUniform(u_currentScene, 1, tex);
}

void TransShader::setFrozenScene(TEX::ID tex)
{
	setTexUniform(u_frozenScene, 2, tex);
}

void TransShader::setTransMap(TEX::ID tex)
{
	setTexUniform(u_transMap, 3, tex);
}

void TransShader::setProg(float value)
{
	gl.Uniform1f(u_prog, value);
}

void TransShader::setVague(float value)
{
	gl.Uniform1f(u_vague, value);
}


SimpleTransShader::SimpleTransShader()
{
	INIT_SHADER(simple, transSimple, SimpleTransShader);

	ShaderBase::init();

	GET_U(currentScene);
	GET_U(frozenScene);
	GET_U(prog);
}

void SimpleTransShader::setCurrentScene(TEX::ID tex)
{
	setTexUniform(u_currentScene, 1, tex);
}

void SimpleTransShader::setFrozenScene(TEX::ID tex)
{
	setTexUniform(u_frozenScene, 2, tex);
}

void SimpleTransShader::setProg(float value)
{
	gl.Uniform1f(u_prog, value);
}


SpriteShader::SpriteShader()
{
	INIT_SHADER(sprite, sprite, SpriteShader);

	ShaderBase::init();

	GET_U(spriteMat);
	GET_U(tone);
	GET_U(color);
	GET_U(opacity);
	GET_U(bushY);
	GET_U(bushUnder);
	GET_U(bushSlope);
	GET_U(bushIntercept);
	GET_U(bushOpacity);
    GET_U(pattern);
    GET_U(patternBlendType);
    GET_U(patternTile);
    GET_U(renderPattern);
    GET_U(patternSizeInv);
    GET_U(patternOpacity);
    GET_U(patternScroll);
    GET_U(patternZoom);
    GET_U(invert);
}

void SpriteShader::setSpriteMat(const float value[16])
{
	gl.UniformMatrix4fv(u_spriteMat, 1, GL_FALSE, value);
}

void SpriteShader::setTone(const Vec4 &tone)
{
	setVec4Uniform(u_tone, tone);
}

void SpriteShader::setColor(const Vec4 &color)
{
	setVec4Uniform(u_color, color);
}

void SpriteShader::setOpacity(float value)
{
	gl.Uniform1f(u_opacity, value);
}

void SpriteShader::setBushDepth(bool bushY, bool bushUnder, float bushSlope, float bushIntercept)
{
	gl.Uniform1f(u_bushY, bushY);
	gl.Uniform1f(u_bushUnder, bushUnder);
	gl.Uniform1f(u_bushSlope, bushSlope);
	gl.Uniform1f(u_bushIntercept, bushIntercept);
}

void SpriteShader::setBushOpacity(float value)
{
	gl.Uniform1f(u_bushOpacity, value);
}

void SpriteShader::setPattern(const TEX::ID pattern, const Vec2 &dimensions)
{
    setTexUniform(u_pattern, 1, pattern);
    gl.Uniform2f(u_patternSizeInv, 1.f / dimensions.x, 1.f / dimensions.y);
}

void SpriteShader::setPatternBlendType(int blendType)
{
    gl.Uniform1i(u_patternBlendType, blendType);
}

void SpriteShader::setPatternTile(bool value)
{
    gl.Uniform1i(u_patternTile, value);
}

void SpriteShader::setShouldRenderPattern(bool value)
{
    gl.Uniform1i(u_renderPattern, value);
}

void SpriteShader::setPatternOpacity(float value)
{
    gl.Uniform1f(u_patternOpacity, value);
}

void SpriteShader::setPatternScroll(const Vec2 &scroll)
{
    setVec2Uniform(u_patternScroll, scroll);
}

void SpriteShader::setPatternZoom(const Vec2 &zoom)
{
    setVec2Uniform(u_patternZoom, zoom);
}

void SpriteShader::setInvert(bool value)
{
    gl.Uniform1i(u_invert, value);
}


PlaneShader::PlaneShader()
{
	INIT_SHADER(simple, plane, PlaneShader);

	ShaderBase::init();

	GET_U(tone);
	GET_U(color);
	GET_U(flash);
	GET_U(opacity);
}

void PlaneShader::setTone(const Vec4 &tone)
{
	setVec4Uniform(u_tone, tone);
}

void PlaneShader::setColor(const Vec4 &color)
{
	setVec4Uniform(u_color, color);
}

void PlaneShader::setFlash(const Vec4 &flash)
{
	setVec4Uniform(u_flash, flash);
}

void PlaneShader::setOpacity(float value)
{
	gl.Uniform1f(u_opacity, value);
}


GrayShader::GrayShader()
{
	INIT_SHADER(simple, gray, GrayShader);

	ShaderBase::init();

	GET_U(gray);
}

bool GrayShader::framebufferScalingAllowed()
{
	// This shader is used with input textures that have already had a
	// framebuffer scale applied. So we don't want to double-apply it.
	return false;
}

void GrayShader::setGray(float value)
{
	gl.Uniform1f(u_gray, value);
}


TilemapShader::TilemapShader()
{
	INIT_SHADER(tilemap, tilemap, TilemapShader);

	ShaderBase::init();

	GET_U(tone);
	GET_U(color);
	GET_U(opacity);

	GET_U(aniIndex);
	GET_U(atFrames);
}

void TilemapShader::setTone(const Vec4 &tone)
{
	setVec4Uniform(u_tone, tone);
}

void TilemapShader::setColor(const Vec4 &color)
{
	setVec4Uniform(u_color, color);
}

void TilemapShader::setOpacity(float value)
{
	gl.Uniform1f(u_opacity, value);
}

void TilemapShader::setAniIndex(int value)
{
	gl.Uniform1i(u_aniIndex, value);
}

void TilemapShader::setATFrames(int values[7])
{
	gl.Uniform1iv(u_atFrames, 7, values);
}



FlashMapShader::FlashMapShader()
{
	INIT_SHADER(simpleColor, flashMap, FlashMapShader);

	ShaderBase::init();

	GET_U(alpha);
}

void FlashMapShader::setAlpha(float value)
{
	gl.Uniform1f(u_alpha, value);
}


HueShader::HueShader()
{
	INIT_SHADER(simple, hue, HueShader);

	ShaderBase::init();

	GET_U(hueAdjust);
}

void HueShader::setHueAdjust(float value)
{
	gl.Uniform1f(u_hueAdjust, value);
}


SimpleMatrixShader::SimpleMatrixShader()
{
	INIT_SHADER(simpleMatrix, simpleAlpha, SimpleMatrixShader);

	ShaderBase::init();

	GET_U(matrix);
}

void SimpleMatrixShader::setMatrix(const float value[16])
{
	gl.UniformMatrix4fv(u_matrix, 1, GL_FALSE, value);
}


BlurShader::HPass::HPass()
{
	INIT_SHADER(blurH, blur, BlurShader::HPass);

	ShaderBase::init();
}

BlurShader::VPass::VPass()
{
	INIT_SHADER(blurV, blur, BlurShader::VPass);

	ShaderBase::init();
}


TilemapVXShader::TilemapVXShader()
{
	INIT_SHADER(tilemapvx, simple, TilemapVXShader);

	ShaderBase::init();

	GET_U(aniOffset);
}

void TilemapVXShader::setAniOffset(const Vec2 &value)
{
	gl.Uniform2f(u_aniOffset, value.x, value.y);
}


BltShader::BltShader()
{
	INIT_SHADER(simple, bitmapBlit, BltShader);

	init();
}

void BltShader::init()
{
	ShaderBase::init();

	GET_U(source);
	GET_U(destination);
	GET_U(subRect);
	GET_U(opacity);
}

void BltShader::setSource()
{
	gl.Uniform1i(u_source, 0);
}

void BltShader::setDestination(const TEX::ID value)
{
	setTexUniform(u_destination, 1, value);
}

void BltShader::setSubRect(const FloatRect &value)
{
	gl.Uniform4f(u_subRect, value.x, value.y, value.w, value.h);
}

void BltShader::setOpacity(float value)
{
	gl.Uniform1f(u_opacity, value);
}

MovieYuvShader::MovieYuvShader()
{
	INIT_SHADER(simple, movieYuv, MovieYuvShader);

	ShaderBase::init();

	GET_U(planeInfo);
	GET_U(chromaMax);
}

void MovieYuvShader::setPlanes(const Vec2i &texSize, int chromaRow, int crColumn, const Vec2i &chromaLast)
{
	gl.Uniform4f(u_planeInfo, 1.f / texSize.x, 1.f / texSize.y, chromaRow, crColumn);
	gl.Uniform2f(u_chromaMax, chromaLast.x, chromaLast.y);
}

#ifdef MKXPZ_SOFTWARE_BITMAPS
/* Mirrors ShaderSet's member list exactly, including its #ifdefs; the
 * GPU-bitmap-only members (BlurShader among them) are not built here. */
void shaderSetEnumerate(ShaderSet &set, std::vector<ShaderBase*> &out)
{
	out.clear();

	out.push_back(&set.flatColor);
	out.push_back(&set.simple);
	out.push_back(&set.simpleColor);
	out.push_back(&set.simpleAlpha);
	out.push_back(&set.simpleSprite);
	out.push_back(&set.alphaSprite);
	out.push_back(&set.sprite);
	out.push_back(&set.plane);
	out.push_back(&set.gray);
	out.push_back(&set.tilemap);
	out.push_back(&set.flashMap);
	out.push_back(&set.trans);
	out.push_back(&set.simpleTrans);
	out.push_back(&set.simpleMatrix);
	out.push_back(&set.tilemapVX);
	out.push_back(&set.movieYuv);
}
#endif /* MKXPZ_SOFTWARE_BITMAPS */
