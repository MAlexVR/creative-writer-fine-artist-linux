#ifndef VER_H
#define VER_H

#include <stdbool.h>

#include "hle.h"
#include "kernel.h"

/* Win16 VER.DLL (the version-stamping library) over the RT_VERSION resource of
 * an NE file: GetFileVersionInfoSize (6), GetFileVersionInfo (7) and
 * VerQueryValue (11), which is what KIDSWTCH uses to read an MSKids program's
 * product name. The real VER.DLL on the CD is not loaded: it needs LZEXPAND and
 * the selector-manipulation half of KERNEL, none of which a start-up path needs. */

typedef struct ver ver_t;

ver_t *ver_create(hle_t *h, kernel_t *k);
void ver_destroy(ver_t *v);

#endif
