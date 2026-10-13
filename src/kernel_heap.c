/* KERNEL memory management: the global heap (handle == selector) and local
 * heaps inside data segments. See kernel.h. */
#include <stdlib.h>
#include <string.h>

#include "kernel_priv.h"

#define GMEM_MODIFY 0x0080
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GMEM_FLAG_KEEP 0x0F00 /* DISCARDABLE and the share bits GlobalFlags reports */

/* ---- global heap ------------------------------------------------------- */

static gblock *gb_of(kernel_t *k, uint16_t sel) {
    if (!k->gb || !(sel & 4) || (sel >> 3) >= MEM_LDT_ENTRIES) return NULL;
    return k->gb[sel >> 3].used ? &k->gb[sel >> 3] : NULL;
}

static uint16_t raw_alloc(kernel_t *k, uint32_t size) {
    uint32_t n = size ? size : 1;
    return n > MEM_SEG_MAX ? mem_alloc_huge(&k->p->mem, n, MEM_DATA) : mem_alloc(&k->p->mem, n, MEM_DATA);
}

uint16_t kernel_global_alloc(kernel_t *k, uint32_t size, uint16_t flags) {
    uint16_t sel;
    if (size > MEM_HUGE_MAX || size > KERNEL_GLOBAL_LIMIT - k->global_used) return 0;
    if (!(sel = raw_alloc(k, size))) return 0;
    k->gb[sel >> 3] = (gblock){1, 0, (uint16_t)(flags & GMEM_FLAG_KEEP), size};
    k->global_used += size;
    return sel;
}

bool kernel_global_free(kernel_t *k, uint16_t h) {
    gblock *g = gb_of(k, h);
    if (!g) return false;
    k->global_used -= g->size;
    memset(g, 0, sizeof *g);
    return mem_free(&k->p->mem, h);
}

cpu_status k_globalalloc(hle_call *c) {
    hle_args a;
    uint16_t flags;
    uint32_t size;
    hle_args_init(&a, c);
    flags = hle_u16(&a);
    size = hle_u32(&a);
    if (a.st) return a.st;
    {
        kernel_t *k = c->ctx;
        uint16_t h = kernel_global_alloc(k, size, flags);
        if (k->hle && k->hle->trace && k->hle->log)
            fprintf(k->hle->log, "global: GlobalAlloc %lu bytes flags %04x -> %04x\n", (unsigned long)size, flags, h);
        if (!h && k->cfg.debug)
            fprintf(k->cfg.debug, "GlobalAlloc(flags %04x, %lu bytes) failed, called from %04x:%04x\n", flags,
                    (unsigned long)size, c->caller_cs, c->caller_ip);
        hle_ret16(c, h);
    }
    return CPU_OK;
}

cpu_status k_globalfree(hle_call *c) {
    hle_args a;
    uint16_t h;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    if (a.st) return a.st;
    hle_ret16(c, kernel_global_free(c->ctx, h) ? 0 : h);
    return CPU_OK;
}

static cpu_status globalrealloc(hle_call *c, uint16_t *oh, uint32_t *osize, uint16_t *oflags);

/* Logs what was asked and what came back when the HLE trace is on. */
cpu_status k_globalrealloc(hle_call *c) {
    kernel_t *k = c->ctx;
    uint16_t h = 0, flags = 0;
    uint32_t size = 0;
    cpu_status st = globalrealloc(c, &h, &size, &flags);
    if (k->hle && k->hle->trace && k->hle->log) {
        uint16_t r = (uint16_t)c->cpu->r[CPU_EAX];
        fprintf(k->hle->log, "global: GlobalReAlloc %04x to %lu bytes flags %04x -> %04x%s\n", h, (unsigned long)size, flags, r,
                r ? "" : " (failed)");
    }
    return st;
}

static cpu_status globalrealloc(hle_call *c, uint16_t *oh, uint32_t *osize, uint16_t *oflags) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t h, flags, nh;
    uint32_t size;
    gblock *g;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    size = hle_u32(&a);
    flags = hle_u16(&a);
    *oh = h;
    *osize = size;
    *oflags = flags;
    if (a.st) return a.st;
    hle_ret16(c, 0);
    if (!(g = gb_of(k, h))) return CPU_OK;
    if (flags & GMEM_MODIFY) {
        g->flags = (uint16_t)((flags & GMEM_FLAG_KEEP) | (g->flags & ~GMEM_FLAG_KEEP));
        hle_ret16(c, h);
        return CPU_OK;
    }
    if (size > MEM_HUGE_MAX || size > KERNEL_GLOBAL_LIMIT - (k->global_used - g->size)) return CPU_OK;
    {
        uint32_t old = g->size ? g->size : 1, now = size ? size : 1;
        if (old <= MEM_SEG_MAX && now <= MEM_SEG_MAX) {
            if (!mem_resize(&k->p->mem, h, now)) return CPU_OK;
            nh = h;
        } else {
            /* a huge block cannot be resized in place: new selectors, so a new handle */
            if (!(nh = raw_alloc(k, size))) return CPU_OK;
            kernel_huge_copy(c->proc, nh, 0, h, 0, old < now ? old : now);
            k->gb[nh >> 3] = *g;
            mem_free(&k->p->mem, h);
            memset(g, 0, sizeof *g);
            g = &k->gb[nh >> 3];
        }
    }
    k->global_used = k->global_used - g->size + size;
    g->size = size;
    hle_ret16(c, nh);
    return CPU_OK;
}

cpu_status k_globallock(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t h;
    gblock *g;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    if (a.st) return a.st;
    if ((g = gb_of(k, h))) {
        if (g->lock < 255) g->lock++;
    } else if (!mem_valid(&k->p->mem, h)) {
        hle_ret32(c, 0);
        return CPU_OK;
    }
    hle_ret32(c, (uint32_t)h << 16);
    return CPU_OK;
}

cpu_status k_globalunlock(hle_call *c) {
    hle_args a;
    uint16_t h;
    gblock *g;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    if (a.st) return a.st;
    g = gb_of(c->ctx, h);
    if (g && g->lock) g->lock--;
    hle_ret16(c, g && g->lock ? 1 : 0);
    return CPU_OK;
}

cpu_status k_globalsize(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t h;
    gblock *g;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    if (a.st) return a.st;
    g = gb_of(k, h);
    hle_ret32(c, g ? g->size : mem_size(&k->p->mem, h));
    return CPU_OK;
}

cpu_status k_globalhandle(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t h;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    if (a.st) return a.st;
    hle_ret32(c, mem_valid(&k->p->mem, h) ? (uint32_t)h << 16 | h : 0);
    return CPU_OK;
}

cpu_status k_globalflags(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t h;
    gblock *g;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    if (a.st) return a.st;
    g = gb_of(k, h);
    hle_ret16(c, g ? (uint16_t)(g->flags | g->lock) : mem_valid(&k->p->mem, h) ? 0 : 0x8000);
    return CPU_OK;
}

static uint32_t global_free_bytes(kernel_t *k) { return KERNEL_GLOBAL_LIMIT - k->global_used; }

cpu_status k_globalcompact(hle_call *c) {
    hle_args a;
    hle_args_init(&a, c);
    hle_u32(&a);
    if (a.st) return a.st;
    hle_ret32(c, global_free_bytes(c->ctx));
    return CPU_OK;
}

cpu_status k_getfreespace(hle_call *c) {
    hle_args a;
    hle_args_init(&a, c);
    hle_u16(&a);
    if (a.st) return a.st;
    hle_ret32(c, global_free_bytes(c->ctx));
    return CPU_OK;
}

/* GlobalLRUOldest / GlobalLRUNewest: no discarding here, the handle is its own answer */
cpu_status k_globallru(hle_call *c) {
    hle_args a;
    hle_args_init(&a, c);
    hle_ret16(c, hle_u16(&a));
    return a.st;
}

static uint16_t seg_arg(kernel_t *k, uint16_t seg) { return seg == 0xFFFF ? k->p->auto_ds : seg; }

cpu_status k_locksegment(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t seg;
    gblock *g;
    hle_args_init(&a, c);
    seg = seg_arg(k, hle_u16(&a));
    if (a.st) return a.st;
    if (!mem_valid(&k->p->mem, seg)) seg = 0;
    else if ((g = gb_of(k, seg)) && g->lock < 255) g->lock++;
    hle_ret16(c, seg);
    return CPU_OK;
}

cpu_status k_unlocksegment(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    gblock *g;
    hle_args_init(&a, c);
    g = gb_of(k, seg_arg(k, hle_u16(&a)));
    if (a.st) return a.st;
    if (g && g->lock) g->lock--;
    hle_ret16(c, g ? g->lock : 0);
    return CPU_OK;
}

/* ---- local heaps ----------------------------------------------------------- */

static lheap *heap_of(kernel_t *k, uint16_t sel) {
    unsigned i;
    for (i = 0; i < k->nheaps; i++)
        if (k->heaps[i].sel == sel) return &k->heaps[i];
    return NULL;
}

static bool heap_add(kernel_t *k, uint16_t sel, uint32_t start, uint32_t end) {
    lheap *h;
    uint32_t size = mem_size(&k->p->mem, sel);
    if (end > size) end = size;
    start = (start + 3) & ~3u;
    if (heap_of(k, sel) || k->nheaps == KERNEL_MAX_HEAPS || start >= end) return false;
    h = &k->heaps[k->nheaps];
    h->b = calloc(8, sizeof *h->b);
    if (!h->b) return false;
    h->cap = 8;
    h->n = 1;
    h->sel = sel;
    h->start = start;
    h->end = end & ~3u;
    h->b[0] = (lblock){start, h->end - start, 0, 0, 0, 0, 0, 0};
    k->nheaps++;
    return true;
}

static bool b_insert(lheap *h, unsigned at, lblock nb) {
    if (h->n == h->cap) {
        lblock *v = realloc(h->b, h->cap * 2 * sizeof *v);
        if (!v) return false;
        h->b = v;
        h->cap *= 2;
    }
    memmove(&h->b[at + 1], &h->b[at], (h->n - at) * sizeof *h->b);
    h->b[at] = nb;
    h->n++;
    return true;
}

static void b_remove(lheap *h, unsigned at) {
    memmove(&h->b[at], &h->b[at + 1], (h->n - at - 1) * sizeof *h->b);
    h->n--;
}

/* Index of the block that starts at off (used blocks only), or -1. */
static int b_at(const lheap *h, uint32_t off) {
    unsigned i;
    for (i = 0; i < h->n; i++)
        if (h->b[i].used && h->b[i].off == off) return (int)i;
    return -1;
}

static int b_containing(const lheap *h, uint32_t off) {
    unsigned i;
    for (i = 0; i < h->n; i++)
        if (h->b[i].used && h->b[i].kind != 2 && off >= h->b[i].off && off < h->b[i].off + h->b[i].size)
            return (int)i;
    return -1;
}

static void b_coalesce(lheap *h, unsigned i) {
    if (i + 1 < h->n && !h->b[i + 1].used) {
        h->b[i].size += h->b[i + 1].size;
        b_remove(h, i + 1);
    }
    if (i > 0 && !h->b[i - 1].used) {
        h->b[i - 1].size += h->b[i].size;
        b_remove(h, i);
    }
}

/* Cut `need` bytes (multiple of 4) from the first free block that fits; the
 * offset of the new block is returned in *off. Indexes shift, so callers use b_at. */
static bool b_alloc(lheap *h, uint32_t need, uint8_t kind, uint32_t *off) {
    unsigned i;
    for (i = 0; i < h->n; i++) {
        lblock *f = &h->b[i];
        if (f->used || f->size < need) continue;
        if (f->size > need &&
            !b_insert(h, i + 1, (lblock){f->off + need, f->size - need, 0, 0, 0, 0, 0, 0}))
            return false;
        f = &h->b[i];
        f->size = need;
        f->used = 1;
        f->kind = kind;
        f->req = f->hnd = f->lock = f->flags = 0;
        *off = f->off;
        return true;
    }
    return false;
}

/* A heap that reaches the end of its segment grows by resizing the segment
 * (the host pointers are never kept across calls), up to 64K. */
static bool heap_grow(kernel_t *k, lheap *h, uint32_t need) {
    uint32_t size = mem_size(&k->p->mem, h->sel), nsize, add = need + 8, end = h->end, nend;
    lblock *last;
    if (end != (size & ~3u) || size >= MEM_SEG_MAX) return false; /* heap_add rounds the end down */
    nsize = size + (add < 0x1000 ? 0x1000 : add);
    if (nsize > MEM_SEG_MAX) nsize = MEM_SEG_MAX;
    nend = nsize & ~3u;
    if (!mem_resize(&k->p->mem, h->sel, nsize)) return false;
    last = &h->b[h->n - 1];
    if (!last->used) last->size += nend - end;
    else if (!b_insert(h, h->n, (lblock){end, nend - end, 0, 0, 0, 0, 0, 0})) {
        mem_resize(&k->p->mem, h->sel, size);
        return false;
    }
    h->end = nend;
    return true;
}

static bool b_alloc_grow(kernel_t *k, lheap *h, uint32_t need, uint8_t kind, uint32_t *off) {
    while (!b_alloc(h, need, kind, off))
        if (!heap_grow(k, h, need)) return false;
    return true;
}

static void b_free(lheap *h, uint32_t off) {
    int i = b_at(h, off);
    if (i < 0) return;
    h->b[i].used = 0;
    b_coalesce(h, (unsigned)i);
}

static uint32_t round4(uint32_t n) { return n < 4 ? 4 : (n + 3) & ~3u; }

static void zero_range(kernel_t *k, uint16_t sel, uint32_t off, uint32_t n) {
    uint8_t *p = n ? mem_ptr(&k->p->mem, sel, off, n) : NULL;
    if (p) memset(p, 0, n);
}

static void put16(kernel_t *k, uint16_t sel, uint32_t off, unsigned v) {
    uint8_t *p = mem_ptr(&k->p->mem, sel, off, 2);
    if (p) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
    }
}

/* Mirror a movable block into its guest-visible handle entry. */
static void write_slot(kernel_t *k, const lheap *h, const lblock *d) {
    uint8_t *p = mem_ptr(&k->p->mem, h->sel, d->hnd, 4);
    if (!p) return;
    p[0] = (uint8_t)d->off;
    p[1] = (uint8_t)(d->off >> 8);
    p[2] = d->flags;
    p[3] = d->lock;
}

/* Data block behind a handle or fixed pointer: index or -1. */
static int data_of(const lheap *h, uint16_t v) {
    int i = b_at(h, v);
    unsigned j;
    if (i < 0) return -1;
    if (h->b[i].kind != 2) return i;
    for (j = 0; j < h->n; j++)
        if (h->b[j].used && h->b[j].kind == 1 && h->b[j].hnd == v) return (int)j;
    return -1;
}

static lheap *ds_heap(hle_call *c) { return heap_of(c->ctx, c->cpu->seg[CPU_DS]); }

cpu_status k_localinit(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t seg, start, end;
    hle_args_init(&a, c);
    seg = hle_u16(&a);
    start = hle_u16(&a);
    end = hle_u16(&a);
    if (a.st) return a.st;
    if (seg == 0) seg = c->cpu->seg[CPU_DS];
    {   /* a library's entry point asks for start 0: the heap the loader appended to its data segment */
        const loader_module *lm = loader_module_by_ds(k->p, seg);
        if (start == 0 && lm && lm != k->p->mods[0] && lm->heap_size) {
            start = lm->heap_start;
            end = (uint16_t)(lm->heap_start + lm->heap_size);
        }
    }
    if (heap_add(k, seg, start, end ? (uint32_t)end : mem_size(&k->p->mem, seg))) {
        put16(k, seg, 6, heap_of(k, seg)->start); /* pLocalHeap */
        hle_ret16(c, 1);
    } else {
        hle_ret16(c, 0);
    }
    return CPU_OK;
}

/* Allocation shared by LocalAlloc and the moving case of LocalReAlloc.
 * Returns the data block offset, *slot receives the handle entry (movable). */
static bool local_new(kernel_t *k, lheap *h, uint16_t size, bool movable, uint8_t flags,
                      uint32_t *data, uint32_t *slot) {
    int d;
    *slot = 0;
    if (movable && !b_alloc_grow(k, h, 4, 2, slot)) return false;
    if (!b_alloc_grow(k, h, round4(size), movable ? 1 : 0, data)) {
        if (movable) b_free(h, *slot);
        return false;
    }
    d = b_at(h, *data);
    h->b[d].req = size;
    h->b[d].flags = flags;
    if (movable) {
        h->b[d].hnd = (uint16_t)*slot;
        write_slot(k, h, &h->b[d]);
    }
    return true;
}

cpu_status k_localalloc(hle_call *c) {
    kernel_t *k = c->ctx;
    lheap *h = ds_heap(c);
    hle_args a;
    uint16_t flags, size;
    uint32_t data, slot;
    hle_args_init(&a, c);
    flags = hle_u16(&a);
    size = hle_u16(&a);
    if (a.st) return a.st;
    hle_ret16(c, 0);
    if (!h || !local_new(k, h, size, flags & 2, (uint8_t)(flags >> 8 & 0xF), &data, &slot)) return CPU_OK;
    if (flags & 0x40) zero_range(k, h->sel, data, round4(size));
    hle_ret16(c, (flags & 2) ? (uint16_t)slot : (uint16_t)data);
    return CPU_OK;
}

cpu_status k_localfree(hle_call *c) {
    lheap *h = ds_heap(c);
    hle_args a;
    uint16_t v;
    int d;
    hle_args_init(&a, c);
    v = hle_u16(&a);
    if (a.st) return a.st;
    if (!h || (d = data_of(h, v)) < 0) {
        hle_ret16(c, v);
        return CPU_OK;
    }
    {
        uint32_t doff = h->b[d].off, slot = h->b[d].hnd;
        bool movable = h->b[d].kind == 1;
        b_free(h, doff); /* indexes shift on every free: work with offsets */
        if (movable) b_free(h, slot);
    }
    hle_ret16(c, 0);
    return CPU_OK;
}

cpu_status k_locallock(hle_call *c) {
    lheap *h = ds_heap(c);
    hle_args a;
    uint16_t v;
    int d;
    hle_args_init(&a, c);
    v = hle_u16(&a);
    if (a.st) return a.st;
    if (!h || (d = data_of(h, v)) < 0) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (h->b[d].kind == 1) {
        if (h->b[d].lock < 255) h->b[d].lock++;
        write_slot(c->ctx, h, &h->b[d]);
    }
    hle_ret16(c, (uint16_t)h->b[d].off);
    return CPU_OK;
}

cpu_status k_localunlock(hle_call *c) {
    lheap *h = ds_heap(c);
    hle_args a;
    uint16_t v;
    int d;
    hle_args_init(&a, c);
    v = hle_u16(&a);
    if (a.st) return a.st;
    hle_ret16(c, 0);
    if (!h || (d = data_of(h, v)) < 0 || h->b[d].kind != 1) return CPU_OK;
    if (h->b[d].lock) h->b[d].lock--;
    write_slot(c->ctx, h, &h->b[d]);
    hle_ret16(c, h->b[d].lock ? 1 : 0);
    return CPU_OK;
}

cpu_status k_localsize(hle_call *c) {
    lheap *h = ds_heap(c);
    hle_args a;
    uint16_t v;
    int d;
    hle_args_init(&a, c);
    v = hle_u16(&a);
    if (a.st) return a.st;
    hle_ret16(c, h && (d = data_of(h, v)) >= 0 ? h->b[d].req : 0);
    return CPU_OK;
}

/* LocalHandle takes a pointer (or a handle) and returns the handle. */
cpu_status k_localhandle(hle_call *c) {
    lheap *h = ds_heap(c);
    hle_args a;
    uint16_t v;
    int d;
    hle_args_init(&a, c);
    v = hle_u16(&a);
    if (a.st) return a.st;
    hle_ret16(c, 0);
    if (!h) return CPU_OK;
    if ((d = b_at(h, v)) >= 0 && h->b[d].kind == 2) hle_ret16(c, v);
    else if ((d = b_containing(h, v)) >= 0)
        hle_ret16(c, h->b[d].kind == 1 ? h->b[d].hnd : (uint16_t)h->b[d].off);
    return CPU_OK;
}

cpu_status k_localflags(hle_call *c) {
    lheap *h = ds_heap(c);
    hle_args a;
    uint16_t v;
    int d;
    hle_args_init(&a, c);
    v = hle_u16(&a);
    if (a.st) return a.st;
    hle_ret16(c, h && (d = data_of(h, v)) >= 0 ? (uint16_t)(h->b[d].flags << 8 | h->b[d].lock) : 0);
    return CPU_OK;
}

cpu_status k_localcompact(hle_call *c) {
    lheap *h = ds_heap(c);
    hle_args a;
    unsigned i;
    uint32_t best = 0;
    hle_args_init(&a, c);
    hle_u16(&a);
    if (a.st) return a.st;
    for (i = 0; h && i < h->n; i++)
        if (!h->b[i].used && h->b[i].size > best) best = h->b[i].size;
    hle_ret16(c, (uint16_t)(best > 0xFFFF ? 0xFFFF : best));
    return CPU_OK;
}

cpu_status k_localrealloc(hle_call *c) {
    kernel_t *k = c->ctx;
    lheap *h = ds_heap(c);
    hle_args a;
    uint16_t v, size, flags;
    int d;
    uint32_t need, doff;
    hle_args_init(&a, c);
    v = hle_u16(&a);
    size = hle_u16(&a);
    flags = hle_u16(&a);
    if (a.st) return a.st;
    hle_ret16(c, 0);
    if (!h || (d = data_of(h, v)) < 0) return CPU_OK;
    if (flags & 0x80) { /* LMEM_MODIFY: flags only */
        h->b[d].flags = (uint8_t)(flags >> 8 & 0xF);
        write_slot(k, h, &h->b[d]);
        hle_ret16(c, v);
        return CPU_OK;
    }
    need = round4(size);
    doff = h->b[d].off;
    if (need <= h->b[d].size) { /* shrink (or same) in place, give the tail back */
        uint32_t tail = h->b[d].size - need;
        if (tail && b_insert(h, (unsigned)d + 1, (lblock){doff + need, tail, 0, 0, 0, 0, 0, 0})) {
            h->b[d].size = need;
            b_coalesce(h, (unsigned)d + 1);
        }
    } else if ((unsigned)d + 1 < h->n && !h->b[d + 1].used &&
               h->b[d].size + h->b[d + 1].size >= need) { /* grow into the free neighbour */
        uint32_t extra = need - h->b[d].size, left = h->b[d + 1].size - extra;
        if (left) {
            h->b[d + 1].off += extra;
            h->b[d + 1].size = left;
        } else {
            b_remove(h, (unsigned)d + 1);
        }
        h->b[d].size = need;
    } else { /* has to move: only movable or LMEM_MOVEABLE blocks, and not while locked */
        uint32_t ndata, nslot;
        lblock old = h->b[d];
        uint8_t *src, *dst;
        if (!(old.kind == 1 || (flags & 2)) || old.lock) return CPU_OK;
        if (old.kind == 1) { /* reuse the existing handle entry */
            if (!b_alloc_grow(k, h, need, 1, &ndata)) return CPU_OK;
            nslot = old.hnd;
        } else if (!local_new(k, h, size, false, old.flags, &ndata, &nslot)) {
            return CPU_OK;
        }
        src = mem_ptr(&k->p->mem, h->sel, old.off, old.req < size ? old.req : size);
        dst = mem_ptr(&k->p->mem, h->sel, ndata, 1);
        if (src && dst) memmove(dst, src, old.req < size ? old.req : size);
        d = b_at(h, ndata);
        h->b[d].req = old.req;
        h->b[d].flags = old.flags;
        if (old.kind == 1) {
            h->b[d].hnd = old.hnd;
            write_slot(k, h, &h->b[d]);
        }
        b_free(h, old.off);
        doff = ndata;
    }
    d = b_at(h, doff);
    if ((flags & 0x40) && size > h->b[d].req) zero_range(k, h->sel, doff + h->b[d].req, round4(size) - h->b[d].req);
    h->b[d].req = size;
    if (h->b[d].kind == 1) {
        write_slot(k, h, &h->b[d]);
        hle_ret16(c, h->b[d].hnd);
    } else {
        hle_ret16(c, (uint16_t)doff);
    }
    return CPU_OK;
}

bool kernel_heap_attach(kernel_t *k) {
    loader_proc *p = k->p;
    k->gb = calloc(MEM_LDT_ENTRIES, sizeof *k->gb);
    if (!k->gb) return false;
    if (p->heap_size && heap_add(k, p->auto_ds, p->heap_start, (uint32_t)p->heap_start + p->heap_size))
        return true;
    return p->heap_size == 0;
}

void kernel_heap_drop(kernel_t *k, uint16_t sel) {
    unsigned i;
    for (i = 0; i < k->nheaps; i++)
        if (k->heaps[i].sel == sel) {
            free(k->heaps[i].b);
            memmove(&k->heaps[i], &k->heaps[i + 1], (k->nheaps - i - 1) * sizeof k->heaps[0]);
            k->nheaps--;
            return;
        }
}

void kernel_heap_destroy(kernel_t *k) {
    unsigned i;
    for (i = 0; i < k->nheaps; i++) free(k->heaps[i].b);
    free(k->gb);
    k->gb = NULL;
    k->nheaps = 0;
}
