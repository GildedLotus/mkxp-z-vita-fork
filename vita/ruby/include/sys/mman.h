// SPDX-License-Identifier: GPL-3.0-or-later
/* Fake <sys/mman.h> for the Vita cross build. Found via -I vita/ruby/include.
 * Real patches should remove the need for this; it exists so unguarded
 * #include <sys/mman.h> sites compile against newlib.
 */
#ifndef _VITA_SYS_MMAN_H
#define _VITA_SYS_MMAN_H
#include "vita_mman.h"
#endif
