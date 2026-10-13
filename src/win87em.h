#ifndef WIN87EM_H
#define WIN87EM_H

#include <stdint.h>

#include "hle.h"

/* WIN87EM.DLL, the Windows math emulator, as seen by the Microsoft C runtime of
 * the two programs: they import only ordinal 1, __FPMATH, and call it with the
 * command in BX and no stack arguments, expecting CF clear on success. The
 * CPU always provides a working x87 (fpu.h), so every command succeeds:
 *   BX = 0   initialise: FINIT, CF clear
 *   BX = 3   DX:AX is a far pointer the runtime hands over (its floating point
 *            exception callback); recorded in the state, never called
 *   others   accepted, CF clear
 * Exceptions stay masked, so the callback is never needed. The command meanings
 * are inferred from the call sites in WRITER 5:0147 / 5:015a and are UNVERIFIED
 * against the real DLL. */

typedef struct {
    uint32_t calls;
    uint16_t last_cmd;
    uint16_t trap_sel, trap_off; /* last command 3 pointer */
} win87em_t;

/* Registers WIN87EM.1 (__FPMATH) in the HLE table; NULL on failure. */
win87em_t *win87em_create(hle_t *h);
void win87em_destroy(win87em_t *w);

#endif
