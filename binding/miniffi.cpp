#include "miniffi.h"
#include <assert.h>

mffi_value miniffi_call_intern(MINIFFI_FUNC target, MiniFFIFuncArgs *p, int nparams) {
    assert(nparams <= MINIFFI_MAX_ARGS);
    // Be sure to add more args to the below line if MINIFFI_MAX_ARGS is bumped
    // in the future.
    return target(p->params[0], p->params[1], p->params[2], p->params[3],
                                 p->params[4], p->params[5], p->params[6],
                                 p->params[7], p->params[8], p->params[9],
                                 p->params[10], p->params[11], p->params[12],
                                 p->params[13], p->params[14], p->params[15]);
}
