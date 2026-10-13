/* Selector/LDT model and guest memory. See mem.h. */
#include <stdlib.h>
#include <string.h>

#include "mem.h"

struct mem_block {
    uint8_t *data;
    uint32_t size;
    unsigned refs;
};

#define GROUP_MEMBER 0xFFFFu /* desc.group value of a non-head huge selector */

static uint8_t access_for(mem_kind k) {
    return k == MEM_CODE ? MEM_ACC_CODE : k == MEM_RODATA ? MEM_ACC_RODATA : MEM_ACC_DATA;
}

/* Descriptor of a selector (RPL ignored), NULL for GDT selectors, the unused
 * index 0 and free entries. */
static mem_desc *desc_of(const mem_t *m, uint16_t sel) {
    unsigned idx = sel >> 3;
    if (!(sel & 4) || idx == 0 || idx >= MEM_LDT_ENTRIES) return NULL;
    return m->ldt[idx].blk ? (mem_desc *)&m->ldt[idx] : NULL;
}

static uint16_t sel_of(unsigned idx) { return (uint16_t)((idx << 3) | 7); }

void mem_init(mem_t *m) { memset(m, 0, sizeof *m); }

static void block_release(mem_block *b) {
    if (b && --b->refs == 0) {
        free(b->data);
        free(b);
    }
}

void mem_destroy(mem_t *m) {
    unsigned i;
    for (i = 0; i < MEM_LDT_ENTRIES; i++) block_release(m->ldt[i].blk);
    memset(m, 0, sizeof *m);
}

static mem_block *block_new(uint32_t size) {
    mem_block *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->data = calloc(size ? size : 1, 1);
    if (!b->data) {
        free(b);
        return NULL;
    }
    b->size = size;
    b->refs = 0;
    return b;
}

/* First index of a run of n free entries, or 0. */
static unsigned find_run(const mem_t *m, unsigned n) {
    unsigned i, run = 0;
    for (i = 1; i < MEM_LDT_ENTRIES; i++) {
        run = m->ldt[i].blk ? 0 : run + 1;
        if (run == n) return i - n + 1;
    }
    return 0;
}

static void fill(mem_desc *d, mem_block *b, uint32_t base, uint32_t limit, uint8_t access) {
    d->blk = b;
    b->refs++;
    d->base = base;
    d->limit = limit;
    d->access = access;
    d->present = true;
    d->group = 0;
}

uint16_t mem_alloc(mem_t *m, uint32_t size, mem_kind kind) {
    unsigned idx;
    mem_block *b;
    if (size == 0 || size > MEM_SEG_MAX) return 0;
    idx = find_run(m, 1);
    if (!idx || !(b = block_new(size))) return 0;
    fill(&m->ldt[idx], b, 0, size - 1, access_for(kind));
    return sel_of(idx);
}

uint16_t mem_alloc_huge(mem_t *m, uint32_t size, mem_kind kind) {
    unsigned n, idx, i;
    mem_block *b;
    if (size == 0 || size > MEM_HUGE_MAX) return 0;
    if (size <= MEM_SEG_MAX) return mem_alloc(m, size, kind);
    n = (size + MEM_SEG_MAX - 1) / MEM_SEG_MAX;
    /* selectors are MEM_AHINCR apart, i.e. consecutive LDT entries */
    idx = find_run(m, n);
    if (!idx || !(b = block_new(size))) return 0;
    for (i = 0; i < n; i++) {
        uint32_t base = i * MEM_SEG_MAX, left = size - base;
        /* the limit runs to the end of the block: 386 code (RLE/DIB decoders) addresses past 64K with 32-bit offsets */
        fill(&m->ldt[idx + i], b, base, left - 1, access_for(kind));
        m->ldt[idx + i].group = i ? GROUP_MEMBER : (uint16_t)n;
    }
    return sel_of(idx);
}

uint16_t mem_huge_sel(uint16_t first, unsigned i) { return (uint16_t)(first + i * MEM_AHINCR); }

bool mem_free(mem_t *m, uint16_t sel) {
    mem_desc *d = desc_of(m, sel);
    unsigned idx = sel >> 3, n = 1, i;
    if (!d || d->group == GROUP_MEMBER) return false;
    if (d->group) n = d->group;
    for (i = 0; i < n; i++) {
        block_release(m->ldt[idx + i].blk);
        memset(&m->ldt[idx + i], 0, sizeof m->ldt[0]);
    }
    return true;
}

bool mem_resize(mem_t *m, uint16_t sel, uint32_t size) {
    mem_desc *d = desc_of(m, sel);
    mem_block *b;
    uint8_t *p;
    if (!d || d->group || size == 0 || size > MEM_SEG_MAX) return false;
    b = d->blk;
    if (b->refs != 1) return false;
    if (size < b->size) memset(b->data + size, 0, b->size - size);
    p = realloc(b->data, size);
    if (!p) return false;
    if (size > b->size) memset(p + b->size, 0, size - b->size);
    b->data = p;
    b->size = size;
    d->limit = size - 1;
    return true;
}

uint16_t mem_alias(mem_t *m, uint16_t sel, mem_kind kind) {
    mem_desc *d = desc_of(m, sel);
    unsigned idx;
    if (!d) return 0;
    idx = find_run(m, 1);
    if (!idx) return 0;
    fill(&m->ldt[idx], d->blk, d->base, d->limit, access_for(kind));
    return sel_of(idx);
}

bool mem_valid(const mem_t *m, uint16_t sel) {
    const mem_desc *d = desc_of(m, sel);
    return d && d->present;
}

uint32_t mem_size(const mem_t *m, uint16_t sel) {
    const mem_desc *d = desc_of(m, sel);
    return d && d->present ? d->limit + 1 : 0;
}

bool mem_set_present(mem_t *m, uint16_t sel, bool present) {
    mem_desc *d = desc_of(m, sel);
    if (!d) return false;
    d->present = present;
    d->access = present ? (uint8_t)(d->access | 0x80) : (uint8_t)(d->access & 0x7F);
    return true;
}

bool mem_info(const mem_t *m, uint16_t sel, cpu_selinfo *out) {
    const mem_desc *d = desc_of(m, sel);
    if (!d) return false;
    out->limit = d->limit;
    out->access = d->access;
    return true;
}

/* Range check without overflow: off + len - 1 <= limit, len >= 1. */
static uint8_t *span(const mem_t *m, uint16_t sel, uint32_t off, uint32_t len, bool write,
                     cpu_status *st) {
    const mem_desc *d = desc_of(m, sel);
    if (!d || !d->present || (write && (d->access & 0xA) != 0x2)) {
        *st = CPU_E_SEL;
        return NULL;
    }
    if (len == 0) len = 1;
    if (off > d->limit || len - 1 > d->limit - off) {
        *st = CPU_E_LIMIT;
        return NULL;
    }
    *st = CPU_OK;
    return d->blk->data + d->base + off;
}

void *mem_ptr(mem_t *m, uint16_t sel, uint32_t off, uint32_t len) {
    cpu_status st;
    return span(m, sel, off, len, false, &st);
}

/* ---- CPU callbacks ---------------------------------------------------- */

static cpu_status rd(void *ctx, uint16_t sel, uint32_t off, unsigned n, uint32_t *v) {
    cpu_status st;
    const uint8_t *p = span(ctx, sel, off, n, false, &st);
    unsigned i;
    if (!p) return st;
    *v = 0;
    for (i = 0; i < n; i++) *v |= (uint32_t)p[i] << (8 * i);
    return CPU_OK;
}

static cpu_status wr(void *ctx, uint16_t sel, uint32_t off, unsigned n, uint32_t v) {
    cpu_status st;
    uint8_t *p = span(ctx, sel, off, n, true, &st);
    unsigned i;
    if (!p) return st;
    for (i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
    return CPU_OK;
}

static cpu_status cb_r8(void *c, uint16_t s, uint32_t o, uint8_t *v) {
    uint32_t t = 0;
    cpu_status st = rd(c, s, o, 1, &t);
    if (st == CPU_OK) *v = (uint8_t)t;
    return st;
}
static cpu_status cb_r16(void *c, uint16_t s, uint32_t o, uint16_t *v) {
    uint32_t t = 0;
    cpu_status st = rd(c, s, o, 2, &t);
    if (st == CPU_OK) *v = (uint16_t)t;
    return st;
}
static cpu_status cb_r32(void *c, uint16_t s, uint32_t o, uint32_t *v) { return rd(c, s, o, 4, v); }
static cpu_status cb_w8(void *c, uint16_t s, uint32_t o, uint8_t v) { return wr(c, s, o, 1, v); }
static cpu_status cb_w16(void *c, uint16_t s, uint32_t o, uint16_t v) { return wr(c, s, o, 2, v); }
static cpu_status cb_w32(void *c, uint16_t s, uint32_t o, uint32_t v) { return wr(c, s, o, 4, v); }
static bool cb_valid(void *c, uint16_t s) { return mem_valid(c, s); }
static bool cb_info(void *c, uint16_t s, cpu_selinfo *o) { return mem_info(c, s, o); }

void mem_cpu_iface(mem_t *m, cpu_mem *out) {
    memset(out, 0, sizeof *out);
    out->ctx = m;
    out->read8 = cb_r8;
    out->write8 = cb_w8;
    out->read16 = cb_r16;
    out->write16 = cb_w16;
    out->read32 = cb_r32;
    out->write32 = cb_w32;
    out->sel_valid = cb_valid;
    out->sel_info = cb_info;
}
