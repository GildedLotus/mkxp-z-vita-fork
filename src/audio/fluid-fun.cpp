#include "fluid-fun.h"

#include <string.h>

#include "debugwriter.h"

#ifdef MKXPZ_TSF
#include "tsf-vita.h"
#endif

struct FluidFunctions fluid;

void initFluidFunctions()
{
#if defined(MKXPZ_TSF)
#define FLUID_FUN(name, type) fluid.name = midi_##name;
#define FLUID_FUN2(name, type, real_name) fluid.name = midi_##name;
#else
	Debug() << "MIDI disabled: TinySoundFont headers were absent at build time";
	memset(&fluid, 0, sizeof(fluid));
	return;
#define FLUID_FUN(name, type)
#define FLUID_FUN2(name, type, real_name)
#endif

FLUID_FUNCS
FLUID_FUNCS2

#undef FLUID_FUN
#undef FLUID_FUN2
}
