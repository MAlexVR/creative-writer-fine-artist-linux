#ifndef TOOLHELP_H
#define TOOLHELP_H

#include <stdbool.h>

#include "hle.h"
#include "kernel.h"

/* Win16 TOOLHELP.DLL, the part WRITER and ARTIST import: TaskFirst (ordinal 63)
 * and TaskNext (64), used for the single-instance check. The runtime runs exactly
 * one task, so TaskFirst reports it and TaskNext reports the end of the list: a
 * program that looks for another copy of itself finds none. */

typedef struct toolhelp toolhelp_t;

toolhelp_t *toolhelp_create(hle_t *h, kernel_t *k);
void toolhelp_destroy(toolhelp_t *t);
bool toolhelp_attach(toolhelp_t *t, loader_proc *p);

#endif
