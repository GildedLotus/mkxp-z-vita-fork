#include "fluid-fun.h"

#include <string.h>
#include <SDL_loadso.h>
#include <SDL_platform.h>

#include "debugwriter.h"

#if __LINUX__ || __ANDROID__
#define FLUID_LIB "libfluidsynth.so.3"
#elif MKXPZ_BUILD_XCODE
#define FLUID_LIB "@rpath/libfluidsynth.dylib"
#elif __APPLE__
#define FLUID_LIB "libfluidsynth.3.dylib"
#elif __WIN32__
#define FLUID_LIB "fluidsynth.dll"
#elif defined(__vita__) || defined(__psp2__)
/* No fluidsynth on Vita; load fails, MIDI silent. */
#define FLUID_LIB "libfluidsynth.so.3"
#else
#error "platform not recognized"
#endif

#ifdef MKXPZ_TSF
#include "tsf-vita.h"
#endif

struct FluidFunctions fluid;
#if !defined(SHARED_FLUID) && !defined(MKXPZ_TSF) && !(defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && !defined(__psp2__)
static void *so;
#endif

void initFluidFunctions()
{
#if defined(MKXPZ_TSF)
#define FLUID_FUN(name, type) fluid.name = midi_##name;
#define FLUID_FUN2(name, type, real_name) fluid.name = midi_##name;
#elif (defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) || defined(__psp2__)
	Debug() << "MIDI disabled: TinySoundFont headers were absent at build time";
	memset(&fluid, 0, sizeof(fluid));
	return;
#define FLUID_FUN(name, type)
#define FLUID_FUN2(name, type, real_name)
#elif defined(SHARED_FLUID)

#define FLUID_FUN(name, type) \
	fluid.name = fluid_##name;

#define FLUID_FUN2(name, type, real_name) \
	fluid.name = real_name;

#else
	so = SDL_LoadObject(FLUID_LIB);

	if (!so)
		goto fail;

#define FLUID_FUN(name, type) \
	fluid.name = (type) SDL_LoadFunction(so, "fluid_" #name); \
	if (!fluid.name) \
		goto fail;

#define FLUID_FUN2(name, type, real_name) \
	fluid.name = (type) SDL_LoadFunction(so, #real_name); \
	if (!fluid.name) \
		goto fail;
#endif

FLUID_FUNCS
FLUID_FUNCS2

	return;

#if !defined(SHARED_FLUID) && !defined(MKXPZ_TSF) && !(defined(__vita__) || defined(MKXPZ_HOST_PORT_LOGIC)) && !defined(__psp2__)
fail:
	Debug() << "Failed to load " FLUID_LIB ". Midi playback is disabled.";

	memset(&fluid, 0, sizeof(fluid));
	SDL_UnloadObject(so);
	so = 0;
#endif
}
