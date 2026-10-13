/* WIN87EM HLE. See win87em.h. */
#include <stdlib.h>

#include "win87em.h"

static cpu_status fpmath(hle_call *c) {
    win87em_t *w = c->ctx;
    cpu_t *cpu = c->cpu;
    uint16_t cmd = (uint16_t)cpu->r[CPU_EBX];
    w->calls++;
    w->last_cmd = cmd;
    if (cmd == 0) fpu_init(&cpu->fpu);
    else if (cmd == 3) {
        w->trap_off = (uint16_t)cpu->r[CPU_EAX];
        w->trap_sel = (uint16_t)cpu->r[CPU_EDX];
    }
    cpu->eflags &= ~CPU_CF;
    return CPU_OK;
}

win87em_t *win87em_create(hle_t *h) {
    static const hle_def def = {"WIN87EM", 1, "__FPMATH", 0, fpmath};
    win87em_t *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    if (!hle_register(h, &def, w)) {
        free(w);
        return NULL;
    }
    return w;
}

void win87em_destroy(win87em_t *w) { free(w); }
