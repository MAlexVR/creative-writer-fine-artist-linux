#ifndef DLLFIX_H
#define DLLFIX_H
/* Library images shared by the module tests. */
#include "nebuild.h"

#define SEG_CODE 0x0000
#define SEG_DATA NE_SEG_DATA

/* MYDLL: ADD2 (ordinal 1, Pascal 2 words: a + b + data word 0x10), GETD (ordinal 2 and 3: returns
 * data word 0x10) and the entry point, which stores DI and CX in the data segment. */
static const uint8_t mydll_code[0x60] = {
    /* 00 */ 0x8C, 0xD8, 0x90, 0x45, 0x55, 0x8B, 0xEC, 0x1E, 0x8E, 0xD8,  /* mov ax,ds; nop; inc bp; push bp; mov bp,sp; push ds; mov ds,ax */
    /* 0a */ 0x8B, 0x46, 0x08, 0x03, 0x46, 0x06, 0x03, 0x06, 0x10, 0x00,  /* ax = [bp+8] + [bp+6] + [0x10] */
    /* 14 */ 0x8D, 0x66, 0xFE, 0x1F, 0x5D, 0x4D, 0xCA, 0x04, 0x00,
    [0x20] = 0x1E, 0x58, 0x90, 0x45, 0x55, 0x8B, 0xEC, 0x1E, 0x8E, 0xD8,  /* push ds; pop ax; nop; ... */
    /* 2a */ 0xA1, 0x10, 0x00, 0x8D, 0x66, 0xFE, 0x1F, 0x5D, 0x4D, 0xCB,
    [0x40] = 0xC7, 0x06, 0x10, 0x00, 0x00, 0x01,                          /* mov word [0x10],0x0100 */
    /* 46 */ 0x89, 0x3E, 0x12, 0x00, 0x89, 0x0E, 0x14, 0x00,              /* [0x12] = DI, [0x14] = CX */
    /* 4e */ 0xB8, 0x01, 0x00, 0xCB,
};

static inline void mydll_nb(nb_t *t) {
    memset(t, 0, sizeof *t);
    t->module = "MyDll";
    t->flags = 0x8001;
    t->autods = 2;
    t->heap = 0x100;
    t->cs = 1;
    t->ip = 0x40;
    t->segs[0] = (nb_seg){mydll_code, sizeof mydll_code, SEG_CODE, sizeof mydll_code, NULL, 0};
    t->segs[1] = (nb_seg){NULL, 0, SEG_DATA, 0x40, NULL, 0};
    t->nsegs = 2;
    t->ents[0] = (nb_ent){1, 0x00};
    t->ents[1] = (nb_ent){1, 0x20};
    t->ents[2] = (nb_ent){1, 0x20};
    t->nents = 3;
    t->names[0] = (nb_name){"ADD2", 1};
    t->names[1] = (nb_name){"GetD", 2};
    t->nnames = 2;
    t->nonres[0] = (nb_name){"NONRES", 3};
    t->nnonres = 1;
}


#endif
