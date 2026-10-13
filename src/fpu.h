#ifndef FPU_H
#define FPU_H

#include <stdbool.h>
#include <stdint.h>

/* x87 (387-class) floating point unit for the 16-bit CPU core.
 *
 * The register stack holds long double: 80-bit extended with a 64-bit
 * mantissa on x86-64 Linux, so values are bit exact there. On hosts with a
 * narrower long double results are correct to that host precision only.
 * The unit never delivers #MF: all exceptions behave as masked, the sticky
 * flags are still set in the status word. The CPU decodes ModRM and moves the
 * memory operand bytes; this module only sees (escape 0-7, ModRM, bytes). */

#define FPU_SW_IE 0x0001u
#define FPU_SW_DE 0x0002u
#define FPU_SW_ZE 0x0004u
#define FPU_SW_OE 0x0008u
#define FPU_SW_UE 0x0010u
#define FPU_SW_PE 0x0020u
#define FPU_SW_SF 0x0040u
#define FPU_SW_ES 0x0080u
#define FPU_SW_C0 0x0100u
#define FPU_SW_C1 0x0200u
#define FPU_SW_C2 0x0400u
#define FPU_SW_C3 0x4000u
#define FPU_SW_CC (FPU_SW_C0 | FPU_SW_C1 | FPU_SW_C2 | FPU_SW_C3)
#define FPU_SW_TOP_SHIFT 11
#define FPU_CW_DEFAULT 0x037Fu

typedef struct {
    long double st[8];  /* physical registers; ST(i) = st[(TOP + i) & 7] */
    uint16_t cw, sw;    /* control word; status word with TOP in bits 11-13 */
    uint8_t empty;      /* bit n set: physical register n is empty (tag 11) */
} fpu_t;

#define FPU_MAX_OPERAND 94 /* FSAVE / FRSTOR */

/* Shape of one instruction: how many bytes it reads from / writes to its
 * memory operand (0 for register forms), and whether it returns AX. */
typedef struct {
    bool valid;
    uint8_t rd, wr;
    bool ax;
} fpu_op;

void fpu_init(fpu_t *f);                      /* FINIT */
/* escape is 0-7 for D8-DF. False for encodings the 387 does not define. */
bool fpu_decode(unsigned escape, uint8_t modrm, fpu_op *op);
/* in/out point at the memory operand (rd/wr bytes), either may be NULL when
 * the size is 0. *ax is written by FSTSW AX only. */
void fpu_exec(fpu_t *f, unsigned escape, uint8_t modrm, const uint8_t *in, uint8_t *out,
              uint16_t *ax);

/* Inspection helpers. */
unsigned fpu_top(const fpu_t *f);
long double fpu_st(const fpu_t *f, unsigned i);   /* ST(i), no exception side effects */
unsigned fpu_depth(const fpu_t *f);               /* non-empty registers */
uint16_t fpu_tag_word(const fpu_t *f);            /* 2 bits per physical register */

#endif
