#ifndef MEM_H
#define MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

/* Win16 guest memory: a local descriptor table of selector-addressed segments.
 *
 * Selectors are (index << 3) | 4 | 3 (LDT, RPL 3). Index 0 is never handed out,
 * so selector 0x0007 is always invalid. Segments of up to 64K are backed by a
 * host block; aliases (code <-> data) and the selectors of a huge block share
 * one block. Every guest access goes through a descriptor limit check, so the
 * guest can never reach host memory outside what its selectors grant. */

#define MEM_LDT_ENTRIES 8192
#define MEM_AHINCR 8              /* selector spacing of a huge block's segments */
#define MEM_SEG_MAX 0x10000u
#define MEM_HUGE_MAX (0x4000000u) /* 64 MB cap for one huge block */

/* x86 access-rights bytes: present, DPL 3, S=1, then type */
#define MEM_ACC_DATA 0xF2 /* read/write data */
#define MEM_ACC_RODATA 0xF0
#define MEM_ACC_CODE 0xFA /* execute/read code */

typedef enum { MEM_DATA = 0, MEM_RODATA, MEM_CODE } mem_kind;

typedef struct mem_block mem_block;

typedef struct {
    mem_block *blk; /* NULL when the entry is free */
    uint32_t base;  /* offset of this segment inside the block */
    uint32_t limit; /* last valid offset */
    uint8_t access;
    bool present;
    uint16_t group; /* huge block head: number of selectors; 0 otherwise */
} mem_desc;

typedef struct mem {
    mem_desc ldt[MEM_LDT_ENTRIES];
} mem_t;

void mem_init(mem_t *m);
void mem_destroy(mem_t *m);

/* New zero-filled segment of 1..64K bytes. Returns the selector, 0 on failure. */
uint16_t mem_alloc(mem_t *m, uint32_t size, mem_kind kind);
/* Block over 64K: ceil(size/64K) selectors at MEM_AHINCR spacing, the first is
 * returned. Sizes up to 64K degenerate to mem_alloc. 0 on failure. */
uint16_t mem_alloc_huge(mem_t *m, uint32_t size, mem_kind kind);
/* Frees one selector (the whole group when sel heads a huge block). */
bool mem_free(mem_t *m, uint16_t sel);
/* Grow or shrink a plain segment; new bytes are zero. Refused for aliased or
 * huge segments. Host pointers from mem_ptr are invalidated. */
bool mem_resize(mem_t *m, uint16_t sel, uint32_t size);
/* New selector over the same memory as sel with a different type (AllocCStoDSAlias). */
uint16_t mem_alias(mem_t *m, uint16_t sel, mem_kind kind);

bool mem_valid(const mem_t *m, uint16_t sel);        /* allocated and present */
uint32_t mem_size(const mem_t *m, uint16_t sel);     /* limit + 1, 0 if invalid */
bool mem_set_present(mem_t *m, uint16_t sel, bool present);
bool mem_info(const mem_t *m, uint16_t sel, cpu_selinfo *out);
uint16_t mem_huge_sel(uint16_t first, unsigned i);

/* Host pointer to sel:off for len bytes, NULL unless the range lies inside the
 * segment. Host-side (HLE/loader) access: ignores the read-only attribute. */
void *mem_ptr(mem_t *m, uint16_t sel, uint32_t off, uint32_t len);

/* The callbacks the CPU core runs on (ctx = the mem_t). */
void mem_cpu_iface(mem_t *m, cpu_mem *out);

#endif
