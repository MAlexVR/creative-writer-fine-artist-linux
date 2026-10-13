#ifndef FIXTURE_H
#define FIXTURE_H
/* Shared test fixture: a hand-built NE image (no file parsing) loaded into a
 * loader_proc, and a helper that runs one far call into an import thunk from
 * guest code, the way a Win16 program would (pushed Pascal arguments). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader.h"

static int g_checks, g_fails;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_fails++;                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define CHECK_EQ(got, want)                                                  \
    do {                                                                     \
        unsigned long long g_ = (unsigned long long)(got);                   \
        unsigned long long w_ = (unsigned long long)(want);                  \
        g_checks++;                                                          \
        if (g_ != w_) {                                                      \
            g_fails++;                                                       \
            printf("  FAIL %s:%d: %s = 0x%llx, want 0x%llx\n", __FILE__,     \
                   __LINE__, #got, g_, w_);                                  \
        }                                                                    \
    } while (0)

#define FX_CODE_SIZE 0x1000
#define FX_CALL_IP 0x100

typedef struct {
    ne_image ne;
    loader_proc *p;
    uint8_t *file;
    loader_stop stop;
    uint16_t sp_before, sp_after; /* SP around the last call (callee cleans the args) */
} fx_t;

/* seg 1 = code, seg 2 = auto data (static 0x200 + stack + heap) */
static inline void fx_init(fx_t *f, uint16_t stack, uint16_t heap) {
    memset(f, 0, sizeof *f);
    f->file = calloc(1, 0x100);
    f->ne.data = f->file;
    f->ne.size = 0x100;
    f->ne.seg_count = 2;
    f->ne.segs = calloc(2, sizeof *f->ne.segs);
    f->ne.segs[0].alloc_size = FX_CODE_SIZE;
    f->ne.segs[1].alloc_size = 0x200;
    f->ne.segs[1].flags = NE_SEG_DATA;
    f->ne.cs = 1;
    f->ne.ss = 2;
    f->ne.auto_data_seg = 2;
    f->ne.stack_size = stack;
    f->ne.heap_size = heap;
}

static inline loader_err fx_load(fx_t *f, const loader_config *cfg) {
    loader_err e = loader_create(&f->ne, cfg, &f->p);
    if (e != LOADER_OK) printf("  loader_create failed: %s\n", loader_strerror(e));
    return e;
}

static inline void fx_free(fx_t *f) {
    loader_destroy(f->p);
    free(f->ne.segs);
    for (size_t i = 0; i < f->ne.restype_count; i++) free(f->ne.restypes[i].res);
    free(f->ne.restypes);
    free(f->ne.resident_names);
    free(f->ne.entries);
    free(f->file);
    memset(f, 0, sizeof *f);
}

/* Adds a resource to the hand-built image (type/name by id when the string is NULL). */
static inline void fx_add_resource(fx_t *f, unsigned type_id, const char *type_name,
                                   unsigned id, const char *name, uint32_t off, uint32_t len) {
    ne_restype *t = NULL;
    ne_resource *r;
    size_t i;
    for (i = 0; i < f->ne.restype_count; i++) {
        ne_restype *c = &f->ne.restypes[i];
        if (type_name ? !strcmp(c->name, type_name) : (!c->name[0] && c->id == type_id)) t = c;
    }
    if (!t) {
        f->ne.restypes = realloc(f->ne.restypes, (f->ne.restype_count + 1) * sizeof *f->ne.restypes);
        t = &f->ne.restypes[f->ne.restype_count++];
        memset(t, 0, sizeof *t);
        if (type_name) strcpy(t->name, type_name);
        else t->id = (uint16_t)type_id;
    }
    t->res = realloc(t->res, (t->count + 1u) * sizeof *t->res);
    r = &t->res[t->count++];
    memset(r, 0, sizeof *r);
    if (name) strcpy(r->name, name);
    else r->id = (uint16_t)id;
    r->offset = off;
    r->length = len;
    r->flags = NE_RES_MOVEABLE | NE_RES_PURE;
}

static inline loader_thunk *fx_thunk(fx_t *f, const char *module, unsigned ordinal) {
    loader_import imp;
    loader_thunk *t = NULL;
    memset(&imp, 0, sizeof imp);
    strcpy(imp.module, module);
    imp.ordinal = (uint16_t)ordinal;
    loader_thunk_for(f->p, &imp, &t);
    return t;
}

/* Guest code: push each word, far call the thunk, HLT. Runs it to the HLT. */
static inline loader_stop fx_call_words(fx_t *f, const char *module, unsigned ordinal,
                                 const uint16_t *w, unsigned n) {
    loader_thunk *t = fx_thunk(f, module, ordinal);
    uint8_t *c = mem_ptr(&f->p->mem, f->p->seg_sel[0], FX_CALL_IP, 3 * n + 6);
    cpu_t *cpu = &f->p->cpu;
    unsigned i, k = 0;
    if (!t || !c) return LOADER_STOP_FAULT;
    for (i = 0; i < n; i++) {
        c[k++] = 0x68;
        c[k++] = (uint8_t)w[i];
        c[k++] = (uint8_t)(w[i] >> 8);
    }
    c[k++] = 0x9A;
    c[k++] = (uint8_t)t->off; c[k++] = (uint8_t)(t->off >> 8);
    c[k++] = (uint8_t)t->sel; c[k++] = (uint8_t)(t->sel >> 8);
    c[k++] = 0xF4;
    cpu->seg[CPU_CS] = f->p->seg_sel[0];
    cpu->eip = FX_CALL_IP;
    f->sp_before = (uint16_t)cpu->r[CPU_ESP];
    f->stop = loader_run(f->p, 1000);
    f->sp_after = (uint16_t)cpu->r[CPU_ESP];
    return f->stop;
}

/* Variadic convenience: words are pushed left to right (Pascal order). A far
 * pointer is FARP(sel, off) and a DWORD is LONG(v): high word first. */
#define FX_CALL(f, mod, ord, ...)                                              \
    fx_call_words((f), (mod), (ord), (const uint16_t[]){0, ##__VA_ARGS__} + 1, \
                  sizeof((const uint16_t[]){0, ##__VA_ARGS__}) / sizeof(uint16_t) - 1)
#define FARP(sel, off) (sel), (off)
#define LONG(v) (uint16_t)((uint32_t)(v) >> 16), (uint16_t)(v)

static inline uint32_t fx_dxax(const fx_t *f) {
    return (uint32_t)(f->p->cpu.r[CPU_EDX] & 0xFFFF) << 16 | (f->p->cpu.r[CPU_EAX] & 0xFFFF);
}
static inline uint16_t fx_ax(const fx_t *f) { return (uint16_t)f->p->cpu.r[CPU_EAX]; }

#endif
