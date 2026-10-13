#ifndef CPU_H
#define CPU_H

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fpu.h"

/* 16-bit protected-mode x86 interpreter (286/386 subset used by Win3.x apps).
 * Code and stack segments are always 16-bit (IP/SP wrap at 64K); 0x66/0x67
 * prefixes select 32-bit operands/addresses. Memory is reached only through
 * the cpu_mem callbacks, which own selector -> linear translation. */

typedef enum {
    CPU_OK = 0,
    CPU_HALT,     /* HLT executed, or a hook asked to stop */
    CPU_BUDGET,   /* cpu_run exhausted its instruction budget */
    CPU_E_UD,     /* invalid / unsupported opcode */
    CPU_E_DE,     /* divide error */
    CPU_E_GP,     /* bad selector on a segment load, or other protection fault */
    CPU_E_SEL,    /* bad selector on a memory access (from the host callbacks) */
    CPU_E_LIMIT,  /* offset beyond the segment limit (from the host callbacks) */
    CPU_E_BOUND,  /* BOUND range exceeded */
    CPU_E_FPU,    /* INT 3Fh (overlay manager): not an FPU call, not implemented */
    CPU_E_INT     /* software interrupt with no host handler */
} cpu_status;

#define CPU_CF 0x0001u
#define CPU_PF 0x0004u
#define CPU_AF 0x0010u
#define CPU_ZF 0x0040u
#define CPU_SF 0x0080u
#define CPU_TF 0x0100u
#define CPU_IF 0x0200u
#define CPU_DF 0x0400u
#define CPU_OF 0x0800u

enum { CPU_EAX, CPU_ECX, CPU_EDX, CPU_EBX, CPU_ESP, CPU_EBP, CPU_ESI, CPU_EDI };
enum { CPU_ES, CPU_CS, CPU_SS, CPU_DS, CPU_FS, CPU_GS };

/* Descriptor facts for VERR/VERW/LAR/LSL: the x86 access-rights byte (P, DPL,
 * S, type: bit 3 code, bit 1 readable (code) / writable (data)) and the limit. */
typedef struct {
    uint32_t limit;
    uint8_t access;
} cpu_selinfo;

/* read8/write8 are required; the wider callbacks are optional fast paths.
 * Callbacks return CPU_OK, CPU_E_SEL or CPU_E_LIMIT and must never touch
 * memory outside what the selector grants. sel_valid is used by segment loads
 * (a null selector is always accepted for DS/ES/FS/GS); NULL accepts all.
 * sel_info fills a selector's limit and access rights, false for a null or
 * unallocated selector; NULL means every sel_valid selector is a 64K writable
 * data segment with DPL 3. Code always runs at CPL 3, so descriptors with
 * DPL < 3 or without the S bit are inaccessible to VERR/VERW. */
typedef struct cpu_mem {
    void *ctx;
    cpu_status (*read8)(void *ctx, uint16_t sel, uint32_t off, uint8_t *v);
    cpu_status (*write8)(void *ctx, uint16_t sel, uint32_t off, uint8_t v);
    cpu_status (*read16)(void *ctx, uint16_t sel, uint32_t off, uint16_t *v);
    cpu_status (*write16)(void *ctx, uint16_t sel, uint32_t off, uint16_t v);
    cpu_status (*read32)(void *ctx, uint16_t sel, uint32_t off, uint32_t *v);
    cpu_status (*write32)(void *ctx, uint16_t sel, uint32_t off, uint32_t v);
    bool (*sel_valid)(void *ctx, uint16_t sel);
    bool (*sel_info)(void *ctx, uint16_t sel, cpu_selinfo *out);
} cpu_mem;

struct cpu;
typedef cpu_status (*cpu_hook_fn)(struct cpu *c, void *ctx);
typedef cpu_status (*cpu_int_fn)(struct cpu *c, uint8_t vec, void *ctx);

#define CPU_HOOK_SLOTS 4096 /* power of two; at most CPU_HOOK_SLOTS/2 hooks */

typedef struct {
    uint16_t sel;
    uint32_t off;
    cpu_hook_fn fn;
    void *ctx;
    bool used;
} cpu_hook;

typedef struct cpu {
    uint32_t r[8];
    uint32_t eip;
    uint32_t eflags;
    uint16_t seg[6];
    cpu_mem mem;

    fpu_t fpu;

    cpu_hook hooks[CPU_HOOK_SLOTS];
    unsigned nhooks;
    cpu_int_fn int_fn;
    void *int_ctx;
    cpu_hook_fn step_fn;      /* called before every instruction (tracing), may be NULL */
    void *step_ctx;

    /* per-instruction decode state (internal) */
    jmp_buf jb;
    uint32_t start_eip, start_esp;
    uint16_t start_cs;
    bool op32, ad32;
    int seg_ovr; /* -1 none */
    uint8_t rep; /* 0, 0xF2 or 0xF3 */
    cpu_status ret; /* status of the instruction being executed */
    uint8_t mod, reg, rm;
    int ea_seg;
    uint32_t ea_off;
} cpu_t;

void cpu_init(cpu_t *c, const cpu_mem *mem);

/* Execute one instruction (or one iteration of a REP string op). Returns
 * CPU_OK to keep going. On a fault, CS:EIP and ESP are restored to the faulting
 * instruction; other registers/memory may be partly updated. */
cpu_status cpu_step(cpu_t *c);
/* Run until a non-OK status, or CPU_BUDGET after max_steps steps. */
cpu_status cpu_run(cpu_t *c, uint64_t max_steps);

/* Hook invoked instead of executing the instruction at sel:off. If it returns
 * CPU_OK and moved CS:EIP, execution resumes there; if it left them alone the
 * instruction is executed normally. Returns false when the table is full. */
bool cpu_add_hook(cpu_t *c, uint16_t sel, uint32_t off, cpu_hook_fn fn, void *ctx);
void cpu_set_int_handler(cpu_t *c, cpu_int_fn fn, void *ctx);
/* fn is called with the CPU before each instruction is executed (its status is ignored). NULL removes it. */
void cpu_set_step_hook(cpu_t *c, cpu_hook_fn fn, void *ctx);

/* Helpers for hooks (HLE thunks): memory access and stack ops on SS:SP. */
cpu_status cpu_read8(cpu_t *c, uint16_t sel, uint32_t off, uint8_t *v);
cpu_status cpu_read16(cpu_t *c, uint16_t sel, uint32_t off, uint16_t *v);
cpu_status cpu_read32(cpu_t *c, uint16_t sel, uint32_t off, uint32_t *v);
cpu_status cpu_write8(cpu_t *c, uint16_t sel, uint32_t off, uint8_t v);
cpu_status cpu_write16(cpu_t *c, uint16_t sel, uint32_t off, uint16_t v);
cpu_status cpu_write32(cpu_t *c, uint16_t sel, uint32_t off, uint32_t v);
cpu_status cpu_push16(cpu_t *c, uint16_t v);
cpu_status cpu_pop16(cpu_t *c, uint16_t *v);
/* Far RET popping IP, CS and then nbytes of arguments (Pascal callee cleanup). */
cpu_status cpu_retf(cpu_t *c, uint16_t nbytes);

/* Instruction length decoder (no execution). */
typedef struct {
    unsigned length;   /* total bytes including prefixes */
    unsigned opcode;   /* 0xNN, or 0x0FNN for two-byte opcodes */
    bool supported;    /* the interpreter implements it */
    bool fpu;          /* x87 escape or WIN87EM INT 34h-3Fh */
} cpu_insn;

/* Decode the instruction at cs:ip. Returns CPU_OK, or the memory status when
 * the bytes cannot be fetched or the instruction is over 15 bytes (CPU_E_UD). */
cpu_status cpu_decode(const cpu_mem *m, uint16_t cs, uint32_t ip, cpu_insn *out);

const char *cpu_strstatus(cpu_status s);

#endif
