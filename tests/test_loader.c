#define _POSIX_C_SOURCE 200809L
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

/* ---- synthetic NE builder ---------------------------------------------- */

typedef struct { uint8_t src, flags; uint16_t off, a, b; } trel;
typedef struct {
    const uint8_t *data;
    unsigned len;
    uint16_t flags;
    unsigned alloc;
    const trel *rel;
    unsigned nrel;
} tseg;
typedef struct { bool moveable; uint8_t seg; uint16_t off; } tent;
typedef struct {
    uint16_t cs, ip, ss, sp, autods, heap, stack;
    const tseg *segs;
    unsigned nsegs;
    const tent *ents;
    unsigned nents;
} tne;

#define IMG_MAX 0x4000
#define NE_AT 0x40
#define R_NONE 0 /* relocation flag bits: target type, additive = 4 */
#define F_INT 0
#define F_ORD 1
#define F_NAME 2
#define F_OS 3
#define F_ADD 4

static void p8(uint8_t *b, size_t at, unsigned v) { b[at] = (uint8_t)v; }
static void p16(uint8_t *b, size_t at, unsigned v) { p8(b, at, v); p8(b, at + 1, v >> 8); }

static size_t put_name(uint8_t *b, size_t at, const char *s, unsigned ord) {
    size_t l = strlen(s);
    p8(b, at, (unsigned)l);
    memcpy(&b[at + 1], s, l);
    p16(b, at + 1 + l, ord);
    return at + 1 + l + 2;
}

/* Modules: 1 = KERNEL, 2 = USER; imported name "WinFunc" at offset 13. */
static size_t build_ne(uint8_t *b, const tne *t) {
    size_t h = NE_AT, p, pos = 0x1000, end;
    unsigned i, modref_off, impname_off, entry_off;
    memset(b, 0, IMG_MAX);
    memcpy(b, "MZ", 2);
    p16(b, 0x3c, NE_AT);
    memcpy(&b[h], "NE", 2);
    p8(b, h + 2, 5); p8(b, h + 3, 10);
    p16(b, h + 0x0c, 0x030a);
    p16(b, h + 0x0e, t->autods);
    p16(b, h + 0x10, t->heap); p16(b, h + 0x12, t->stack);
    p16(b, h + 0x14, t->ip); p16(b, h + 0x16, t->cs);
    p16(b, h + 0x18, t->sp); p16(b, h + 0x1a, t->ss);
    p16(b, h + 0x1c, t->nsegs);
    p16(b, h + 0x1e, 2);
    p16(b, h + 0x22, 0x40);
    p16(b, h + 0x24, 0x40 + 8 * t->nsegs);
    p16(b, h + 0x26, 0x40 + 8 * t->nsegs);
    p16(b, h + 0x32, 4);
    p8(b, h + 0x36, 2);
    p = h + 0x40 + 8 * t->nsegs;
    p = put_name(b, p, "TEST", 0);
    p8(b, p++, 0);
    modref_off = (unsigned)(p - h);
    p16(b, p, 1); p16(b, p + 2, 8); p += 4;
    impname_off = (unsigned)(p - h);
    p8(b, p++, 0);
    p = put_name(b, p, "KERNEL", 0) - 2;
    p = put_name(b, p, "USER", 0) - 2;
    p = put_name(b, p, "WinFunc", 0) - 2;
    p8(b, p++, 0);
    entry_off = (unsigned)(p - h);
    for (i = 0; i < t->nents; i++) {
        p8(b, p, 1);
        if (t->ents[i].moveable) {
            p8(b, p + 1, 0xff); p8(b, p + 2, 1); p16(b, p + 3, 0x3fcd);
            p8(b, p + 5, t->ents[i].seg); p16(b, p + 6, t->ents[i].off);
            p += 8;
        } else {
            p8(b, p + 1, t->ents[i].seg); p8(b, p + 2, 1); p16(b, p + 3, t->ents[i].off);
            p += 5;
        }
    }
    p8(b, p++, 0);
    p16(b, h + 0x04, entry_off); p16(b, h + 0x06, (unsigned)(p - h - entry_off));
    p16(b, h + 0x28, modref_off); p16(b, h + 0x2a, impname_off);
    p = 0xF00;
    p = put_name(b, p, "Test", 0);
    p8(b, p++, 0);
    b[h + 0x2c] = 0x00; p16(b, h + 0x2c, 0xF00);
    p16(b, h + 0x20, (unsigned)(p - 0xF00));
    /* segments */
    for (i = 0; i < t->nsegs; i++) {
        const tseg *s = &t->segs[i];
        size_t e = h + 0x40 + 8 * i;
        uint16_t flags = s->flags | (s->nrel ? NE_SEG_RELOCINFO : 0);
        pos = (pos + 15) & ~(size_t)15;
        if (s->data || s->len) {
            p16(b, e, (unsigned)(pos >> 4)); p16(b, e + 2, s->len);
            if (s->data) memcpy(&b[pos], s->data, s->len);
        }
        p16(b, e + 4, flags); p16(b, e + 6, s->alloc);
        pos += s->len;
        if (s->nrel) {
            unsigned k;
            p16(b, pos, s->nrel); pos += 2;
            for (k = 0; k < s->nrel; k++) {
                p8(b, pos, s->rel[k].src); p8(b, pos + 1, s->rel[k].flags);
                p16(b, pos + 2, s->rel[k].off); p16(b, pos + 4, s->rel[k].a);
                p16(b, pos + 6, s->rel[k].b);
                pos += 8;
            }
        }
    }
    end = pos;
    return end;
}

/* ---- fixture program ---------------------------------------------------
 * seg1 code, seg2 auto data (alloc 0x100), seg3 data (chain tests).
 * entry 1: fixed 1:0, entry 2: moveable 1:0x60, entry 3: fixed 2:0x10.      */

#define CODE_SZ 0x70
static uint8_t g_code[CODE_SZ];
static uint8_t g_d2[0x40];
static uint8_t g_d3[0x40];

static const trel g_rel1[] = {
    /* far call KERNEL.7 at 0x06 -> 0x27 (end) */
    {NE_SRC_FARPTR, F_ORD, 0x06, 1, 7},
    /* far call internal 1:0x60 at 0x0e */
    {NE_SRC_FARPTR, F_INT, 0x0e, 1, 0x60},
    /* mov bx, 5 + offset(1:0x60), additive, site 0x13 */
    {NE_SRC_OFFSET, F_INT | F_ADD, 0x13, 1, 0x60},
    /* mov ax, selector(seg3), site 0x16 */
    {NE_SRC_SELECTOR, F_INT, 0x16, 3, 0},
    /* far call USER.3 at 0x30 */
    {NE_SRC_FARPTR, F_ORD, 0x30, 2, 3},
    /* far call USER."WinFunc" at 0x38 */
    {NE_SRC_FARPTR, F_NAME, 0x38, 2, 13},
};

static const trel g_rel3[] = {
    {NE_SRC_OFFSET, F_INT, 0x04, 1, 0x60},               /* chain 04 -> 10 -> 1c */
    {NE_SRC_FARPTR, F_INT, 0x20, NE_SEG_MOVEABLE_REF, 2},/* entry 2 = 1:0x60 */
    {NE_SRC_LOBYTE, F_INT, 0x08, 2, 0x1234},
    {NE_SRC_SELECTOR, F_INT | F_ADD, 0x0a, 2, 0},
    {NE_SRC_OFFSET, F_INT | F_ADD, 0x0c, 1, 0x60},
    {NE_SRC_FARPTR, F_INT | F_ADD, 0x28, 1, 0x60},
    {NE_SRC_OFFSET, F_OS | F_ADD, 0x2c, 4, 0},
    {NE_SRC_OFFSET, F_ORD, 0x30, 1, 12},                 /* KERNEL.12, chain 30 -> 34 */
    {NE_SRC_OFFSET, F_INT, 0x38, 3, 0x5678},             /* fixed segment/offset */
};

static const tent g_ents[] = {{false, 1, 0}, {true, 1, 0x60}, {false, 2, 0x10}};

static void build_fixture_parts(void) {
    static const uint8_t code[] = {
        0x68, 0x34, 0x12,                   /* 00 push 0x1234 */
        0x6A, 0x02,                         /* 03 push 2 */
        0x9A, 0xFF, 0xFF, 0x00, 0x00,       /* 05 call far KERNEL.7 (chain to 0x27) */
        0xA3, 0x10, 0x00,                   /* 0a mov [0x10],ax */
        0x9A, 0xFF, 0xFF, 0x00, 0x00,       /* 0d call far 1:0x60 */
        0xBB, 0x05, 0x00,                   /* 12 mov bx, 5 (+ offset) */
        0xB8, 0xFF, 0xFF,                   /* 15 mov ax, selector seg3 */
        0x8E, 0xC0,                         /* 18 mov es,ax */
        0x26, 0xC7, 0x06, 0x00, 0x00, 0xEF, 0xBE, /* 1a mov word es:[0],0xBEEF */
        0x6A, 0x07,                         /* 21 push 7 */
        0x6A, 0x08,                         /* 23 push 8 */
        0x9A, 0xFF, 0xFF, 0x00, 0x00,       /* 25 call far KERNEL.7 (chain end) */
        0xA3, 0x12, 0x00,                   /* 2a mov [0x12],ax */
        0x90, 0x90,                         /* 2d */
        0x9A, 0xFF, 0xFF, 0x00, 0x00,       /* 2f call far USER.3 */
        0xA3, 0x14, 0x00,                   /* 34 mov [0x14],ax */
        0x9A, 0xFF, 0xFF, 0x00, 0x00,       /* 37 call far USER.WinFunc */
        0x89, 0x16, 0x16, 0x00,             /* 3c mov [0x16],dx */
        0xF4,                               /* 40 hlt */
    };
    memset(g_code, 0x90, sizeof g_code);
    memcpy(g_code, code, sizeof code);
    /* the chain through both KERNEL.7 call sites: 0x06 -> 0x26 */
    g_code[0x06] = 0x26; g_code[0x07] = 0x00;
    memset(g_code + 0x60, 0, 16);
    g_code[0x60] = 0xFF; g_code[0x61] = 0x06; g_code[0x62] = 0x18; g_code[0x63] = 0x00; /* inc [0x18] */
    g_code[0x64] = 0xCB;                                                               /* retf */
    memset(g_d2, 0xAA, sizeof g_d2);
    memset(g_d2 + 0x10, 0, 0x10);
    memset(g_d3, 0, sizeof g_d3);
    g_d3[0x04] = 0x10; g_d3[0x10] = 0x1C; g_d3[0x1C] = 0xFF; g_d3[0x1D] = 0xFF;
    g_d3[0x08] = 0xFF; g_d3[0x09] = 0xFF;
    g_d3[0x0A] = 0x11; g_d3[0x0B] = 0x11;
    g_d3[0x0C] = 0x10;
    g_d3[0x20] = 0xFF; g_d3[0x21] = 0xFF;
    g_d3[0x28] = 0x05;
    g_d3[0x30] = 0x34; g_d3[0x34] = 0xFF; g_d3[0x35] = 0xFF;
    g_d3[0x38] = 0xFF; g_d3[0x39] = 0xFF;
}

static const tseg g_segs[] = {
    {g_code, sizeof g_code, 0x0050, sizeof g_code, g_rel1, sizeof g_rel1 / sizeof g_rel1[0]},
    {g_d2, sizeof g_d2, NE_SEG_DATA, 0x100, NULL, 0},
    {g_d3, sizeof g_d3, NE_SEG_DATA, 0x40, g_rel3, sizeof g_rel3 / sizeof g_rel3[0]},
};

static tne fixture_tne(void) {
    tne t;
    memset(&t, 0, sizeof t);
    t.cs = 1; t.ip = 0; t.ss = 2; t.sp = 0; t.autods = 2; t.heap = 0x20; t.stack = 0x200;
    t.segs = g_segs; t.nsegs = 3; t.ents = g_ents; t.nents = 3;
    return t;
}

static uint8_t g_img[IMG_MAX];
static ne_image g_ne;
static loader_proc *g_p;

static void unload(void) {
    loader_destroy(g_p); g_p = NULL;
    ne_free(&g_ne); memset(&g_ne, 0, sizeof g_ne);
}

static loader_err load_tne(const tne *t, const loader_config *cfg) {
    size_t n = build_ne(g_img, t);
    unload();
    if (ne_open_buffer(&g_ne, g_img, n) != NE_OK) { printf("  fixture rejected by ne parser\n"); g_fails++; return LOADER_ERR_ARG; }
    return loader_create(&g_ne, cfg, &g_p);
}

/* ---- API handlers ----------------------------------------------------- */

typedef struct { char mod[NE_NAME_MAX + 1]; unsigned ord; char name[NE_NAME_MAX + 1]; } call_rec;
static call_rec g_log[32];
static unsigned g_nlog;

static void log_call(const loader_import *imp) {
    call_rec *r = &g_log[g_nlog++ % 32];
    snprintf(r->mod, sizeof r->mod, "%s", imp->module);
    r->ord = imp->by_name ? 0 : imp->ordinal;
    snprintf(r->name, sizeof r->name, "%s", imp->by_name ? imp->name : "");
}

static cpu_status api_add(loader_proc *p, const loader_import *imp, void *ctx) {
    uint16_t a, b;
    (void)ctx;
    log_call(imp);
    if (loader_arg16(&p->cpu, 0, &a) || loader_arg16(&p->cpu, 2, &b)) return CPU_E_SEL;
    p->cpu.r[CPU_EAX] = (uint16_t)(a + b);
    return CPU_OK;
}
static cpu_status api_const(loader_proc *p, const loader_import *imp, void *ctx) {
    log_call(imp);
    p->cpu.r[CPU_EAX] = *(unsigned *)ctx;
    return CPU_OK;
}
static cpu_status api_dx(loader_proc *p, const loader_import *imp, void *ctx) {
    (void)ctx;
    log_call(imp);
    p->cpu.r[CPU_EDX] = 0x77;
    return CPU_OK;
}

static unsigned g_const_val = 0x4242;
#define ORD_CONST_OFF 113 /* absolute value 3, like __AHSHIFT */
#define ORD_CONST_SEG 195 /* selector over a fresh 0x100 byte segment, like __C000H */

static bool resolve_fixture(void *ctx, const loader_import *imp, loader_api *out) {
    (void)ctx;
    memset(out, 0, sizeof *out);
    if (!strcmp(imp->module, "KERNEL") && !imp->by_name && imp->ordinal == 7) {
        out->fn = api_add; out->arg_bytes = 4;
    } else if (!strcmp(imp->module, "USER") && !imp->by_name && imp->ordinal == 3) {
        out->fn = api_const; out->ctx = &g_const_val;
    } else if (!strcmp(imp->module, "USER") && imp->by_name && !strcmp(imp->name, "WinFunc")) {
        out->fn = api_dx;
    } else if (!strcmp(imp->module, "KERNEL") && !imp->by_name && imp->ordinal == ORD_CONST_OFF) {
        out->is_const = true; out->const_off = 3;
    } else if (!strcmp(imp->module, "KERNEL") && !imp->by_name && imp->ordinal == ORD_CONST_SEG) {
        out->is_const = true; out->const_seg_bytes = 0x100;
    } else {
        return false;
    }
    return true;
}

static uint16_t rd16(uint16_t sel, uint32_t off) {
    const uint8_t *p = mem_ptr(&g_p->mem, sel, off, 2);
    return p ? (uint16_t)(p[0] | p[1] << 8) : 0xDEAD;
}
static uint8_t rd8(uint16_t sel, uint32_t off) {
    const uint8_t *p = mem_ptr(&g_p->mem, sel, off, 1);
    return p ? *p : 0xDE;
}

static const loader_config cfg_fix = {.resolve = resolve_fixture};

/* ---- tests ------------------------------------------------------------ */

static void test_segments(void) {
    tne t = fixture_tne();
    unsigned i;
    cpu_selinfo si;
    printf("test_segments\n");
    CHECK_EQ(load_tne(&t, &cfg_fix), LOADER_OK);
    if (!g_p) return;
    for (i = 0; i < 3; i++) CHECK(g_p->seg_sel[i] != 0 && mem_valid(&g_p->mem, g_p->seg_sel[i]));
    CHECK(g_p->seg_sel[0] != g_p->seg_sel[1]);
    CHECK(mem_info(&g_p->mem, g_p->seg_sel[0], &si)); CHECK_EQ(si.access, MEM_ACC_CODE);
    CHECK(mem_info(&g_p->mem, g_p->seg_sel[2], &si)); CHECK_EQ(si.access, MEM_ACC_DATA);
    CHECK_EQ(mem_size(&g_p->mem, g_p->seg_sel[0]), CODE_SZ);
    /* auto data: alloc 0x100 + stack 0x200 + heap 0x20 */
    CHECK_EQ(g_p->auto_ds, g_p->seg_sel[1]);
    CHECK_EQ(mem_size(&g_p->mem, g_p->auto_ds), 0x100 + 0x200 + 0x20);
    CHECK_EQ(g_p->stack_size, 0x200); CHECK_EQ(g_p->heap_start, 0x300); CHECK_EQ(g_p->heap_size, 0x20);
    CHECK_EQ(rd8(g_p->auto_ds, 0x3F), 0xAA); CHECK_EQ(rd8(g_p->auto_ds, 0x40), 0); /* bss zeroed */
    CHECK_EQ(rd8(g_p->seg_sel[0], 0x60), 0xFF); /* code bytes copied */
    CHECK_EQ(rd8(g_p->seg_sel[2], 0x3F), 0);
}

static void test_big_segments(void) {
    /* stored alloc 0 means 64K; auto data is capped at 64K including stack and heap */
    tseg segs[3];
    tne t = fixture_tne();
    printf("test_big_segments\n");
    memcpy(segs, g_segs, sizeof segs);
    segs[1].alloc = 0xE000; segs[2].alloc = 0;
    t.segs = segs; t.heap = 0x2000; t.stack = 0x1000;
    CHECK_EQ(load_tne(&t, &cfg_fix), LOADER_OK);
    if (!g_p) return;
    CHECK_EQ(mem_size(&g_p->mem, g_p->seg_sel[2]), 0x10000);
    CHECK_EQ(mem_size(&g_p->mem, g_p->auto_ds), 0x10000);
    CHECK_EQ(g_p->stack_size, 0x1000);
    CHECK_EQ(g_p->heap_start, 0xF000); /* heap clipped to what is left */
    CHECK_EQ(g_p->heap_size, 0x1000);
    CHECK_EQ(g_p->cpu.r[CPU_ESP] & 0xFFFF, 0xF000);
}

static void test_relocations(void) {
    tne t = fixture_tne();
    uint16_t c1, d2, d3;
    loader_import in;
    loader_thunk *k12;
    printf("test_relocations\n");
    CHECK_EQ(load_tne(&t, &cfg_fix), LOADER_OK);
    if (!g_p) return;
    c1 = g_p->seg_sel[0]; d2 = g_p->seg_sel[1]; d3 = g_p->seg_sel[2];
    /* chain 04 -> 10 -> 1c patched with the same offset */
    CHECK_EQ(rd16(d3, 0x04), 0x60); CHECK_EQ(rd16(d3, 0x10), 0x60); CHECK_EQ(rd16(d3, 0x1C), 0x60);
    CHECK_EQ(rd16(d3, 0x06), 0); /* neighbours untouched */
    /* far pointer through a moveable entry: offset then selector */
    CHECK_EQ(rd16(d3, 0x20), 0x60); CHECK_EQ(rd16(d3, 0x22), c1);
    /* lobyte replaces only the low byte; the chain terminator's high byte stays */
    CHECK_EQ(rd8(d3, 0x08), 0x34); CHECK_EQ(rd8(d3, 0x09), 0xFF);
    /* additive: selector replaced, offsets added to the stored value */
    CHECK_EQ(rd16(d3, 0x0A), d2);
    CHECK_EQ(rd16(d3, 0x0C), 0x10 + 0x60);
    CHECK_EQ(rd16(d3, 0x28), 0x05 + 0x60); CHECK_EQ(rd16(d3, 0x2A), c1);
    /* internal reference to a fixed segment/offset */
    CHECK_EQ(rd16(d3, 0x38), 0x5678);
    /* OS fixup recorded, bytes untouched */
    CHECK_EQ(g_p->nosfixups, 1);
    CHECK(g_p->nosfixups == 1 && g_p->osfixups[0].seg == 2 && g_p->osfixups[0].offset == 0x2c &&
          g_p->osfixups[0].type == 4);
    CHECK_EQ(rd16(d3, 0x2C), 0);
    /* imported ordinal offset fixups share a thunk: KERNEL.12 */
    memset(&in, 0, sizeof in);
    strcpy(in.module, "KERNEL"); in.ordinal = 12;
    k12 = loader_find_thunk(g_p, &in);
    CHECK(k12 != NULL);
    if (k12) {
        CHECK_EQ(rd16(d3, 0x30), k12->off); CHECK_EQ(rd16(d3, 0x34), k12->off);
        CHECK(!k12->resolved);
    }
    /* code: far calls hold thunk / internal addresses, selector fixup patched */
    CHECK_EQ(rd16(c1, 0x0E), 0x60); CHECK_EQ(rd16(c1, 0x10), c1);
    CHECK_EQ(rd16(c1, 0x13), 5 + 0x60);
    CHECK_EQ(rd16(c1, 0x16), d3);
    CHECK_EQ(rd16(c1, 0x06), rd16(c1, 0x26)); /* both KERNEL.7 sites identical */
    CHECK_EQ(rd16(c1, 0x08), g_p->thunk_sel); CHECK_EQ(rd16(c1, 0x28), g_p->thunk_sel);
    CHECK_EQ(g_p->reloc_records, 6 + 9);
    CHECK_EQ(g_p->reloc_sites, 7 + 11); /* patched sites; the OS fixup is only recorded */
    CHECK_EQ(g_p->unresolved, 1); /* only KERNEL.12 */
    CHECK_EQ(g_p->nthunks, 4);    /* KERNEL.7, USER.3, USER.WinFunc, KERNEL.12 */
}

static void test_run_to_completion(void) {
    tne t = fixture_tne();
    uint16_t ds, sp0;
    printf("test_run_to_completion\n");
    g_nlog = 0;
    CHECK_EQ(load_tne(&t, &cfg_fix), LOADER_OK);
    if (!g_p) return;
    ds = g_p->auto_ds;
    sp0 = (uint16_t)g_p->cpu.r[CPU_ESP];
    CHECK_EQ(loader_run(g_p, 1000), LOADER_STOP_HLT);
    CHECK_EQ(g_p->stop, LOADER_STOP_HLT);
    CHECK_EQ(rd16(ds, 0x10), 0x1234 + 2);       /* KERNEL.7 result */
    CHECK_EQ(rd16(ds, 0x12), 7 + 8);            /* second call, same thunk */
    CHECK_EQ(rd16(ds, 0x14), 0x4242);           /* USER.3, no arguments */
    CHECK_EQ(rd16(ds, 0x16), 0x77);             /* USER.WinFunc by name */
    CHECK_EQ(rd16(ds, 0x18), 1);                /* internal far call executed */
    CHECK_EQ(g_p->cpu.r[CPU_EBX] & 0xFFFF, 0x65);
    CHECK_EQ(rd16(g_p->seg_sel[2], 0), 0xBEEF); /* es = selector fixup */
    CHECK_EQ((uint16_t)g_p->cpu.r[CPU_ESP], sp0); /* pascal cleanup balanced */
    CHECK_EQ(g_p->api_calls, 4);
    CHECK_EQ(g_nlog, 4);
    CHECK(!strcmp(g_log[0].mod, "KERNEL") && g_log[0].ord == 7);
    CHECK(!strcmp(g_log[2].mod, "USER") && g_log[2].ord == 3);
    CHECK(!strcmp(g_log[3].name, "WinFunc"));
}

static void test_initial_state(void) {
    tne t = fixture_tne();
    loader_config cfg = cfg_fix;
    const uint8_t *psp;
    cpu_t *c;
    printf("test_initial_state\n");
    cfg.cmdline = "hello /x";
    CHECK_EQ(load_tne(&t, &cfg), LOADER_OK);
    if (!g_p) return;
    c = &g_p->cpu;
    CHECK_EQ(c->seg[CPU_CS], g_p->seg_sel[0]); CHECK_EQ(c->eip, 0);
    CHECK_EQ(c->seg[CPU_SS], g_p->auto_ds); CHECK_EQ(c->seg[CPU_DS], g_p->auto_ds);
    CHECK_EQ(c->r[CPU_ESP] & 0xFFFF, 0x300); /* sp 0 in the header: top of data + stack */
    CHECK_EQ(c->r[CPU_EAX] & 0xFFFF, 1);
    CHECK_EQ(c->r[CPU_ECX] & 0xFFFF, 0x20);    /* heap size */
    CHECK_EQ(c->r[CPU_EBX] & 0xFFFF, 0x200);   /* stack size */
    CHECK_EQ(c->r[CPU_EDI] & 0xFFFF, g_p->auto_ds); /* hInstance */
    CHECK_EQ(c->r[CPU_ESI] & 0xFFFF, 0);       /* hPrevInstance */
    CHECK_EQ(c->seg[CPU_ES], g_p->psp_sel);
    psp = mem_ptr(&g_p->mem, g_p->psp_sel, 0, 0x100);
    CHECK(psp != NULL);
    if (psp) {
        CHECK(psp[0] == 0xCD && psp[1] == 0x20);
        CHECK_EQ(psp[0x80], 8); CHECK(!memcmp(psp + 0x81, "hello /x", 8));
        CHECK_EQ(psp[0x89], 0);               /* the tail is the NUL-terminated lpCmdLine, not a DOS CR-terminated one */
    }
    /* explicit SP in the header wins; a missing command line is an empty tail */
    t.sp = 0x1F0;
    CHECK_EQ(load_tne(&t, &cfg_fix), LOADER_OK);
    if (g_p) {
        CHECK_EQ(g_p->cpu.r[CPU_ESP] & 0xFFFF, 0x1F0);
        psp = mem_ptr(&g_p->mem, g_p->psp_sel, 0, 0x100);
        CHECK(psp && psp[0x80] == 0 && psp[0x81] == 0);   /* WinMain's lpCmdLine is the empty string */
    }
}

static unsigned g_stop_calls;
static cpu_status api_stop(loader_proc *p, const loader_import *imp, void *ctx) {
    (void)imp; (void)ctx;
    g_stop_calls++;
    return loader_request_stop(p);
}
static bool resolve_stop(void *ctx, const loader_import *imp, loader_api *out) {
    (void)ctx; (void)imp;
    out->fn = api_stop; out->arg_bytes = 4; out->ctx = NULL;
    return true;
}

static cpu_status api_redirect(loader_proc *p, const loader_import *imp, void *ctx) {
    (void)imp;
    p->cpu.seg[CPU_CS] = p->seg_sel[0]; /* handler transfers control itself */
    p->cpu.eip = *(uint32_t *)ctx;
    return CPU_OK;
}
static uint32_t g_redirect_to = 0x40;
static bool resolve_redirect(void *ctx, const loader_import *imp, loader_api *out) {
    (void)ctx; (void)imp;
    out->fn = api_redirect; out->arg_bytes = 4; out->ctx = &g_redirect_to;
    return true;
}

static void test_run_stops(void) {
    tne t = fixture_tne();
    loader_config cfg;
    uint16_t sp;
    printf("test_run_stops\n");
    /* unresolved imports stop the run when called */
    cfg = cfg_fix; cfg.resolve = NULL;
    CHECK_EQ(load_tne(&t, &cfg), LOADER_OK);
    if (g_p) {
        CHECK_EQ(g_p->unresolved, 4);
        CHECK_EQ(loader_run(g_p, 1000), LOADER_STOP_UNRESOLVED);
        CHECK(g_p->last_call && !strcmp(g_p->last_call->imp.module, "KERNEL") && g_p->last_call->imp.ordinal == 7);
    }
    /* handler stop request: CS:IP stays on the thunk, call is not retired */
    cfg.resolve = resolve_stop; g_stop_calls = 0;
    CHECK_EQ(load_tne(&t, &cfg), LOADER_OK);
    if (g_p) {
        sp = (uint16_t)g_p->cpu.r[CPU_ESP];
        CHECK_EQ(loader_run(g_p, 1000), LOADER_STOP_REQUESTED);
        CHECK_EQ(g_stop_calls, 1);
        CHECK_EQ(g_p->cpu.seg[CPU_CS], g_p->thunk_sel);
        CHECK_EQ((uint16_t)g_p->cpu.r[CPU_ESP], (uint16_t)(sp - 4 - 2 - 2)); /* 2 args + far return */
        CHECK(g_p->last_call != NULL);
    }
    /* budget */
    CHECK_EQ(load_tne(&t, &cfg_fix), LOADER_OK);
    if (g_p) {
        CHECK_EQ(loader_run(g_p, 3), LOADER_STOP_BUDGET);
        CHECK_EQ(loader_run(g_p, 1000), LOADER_STOP_HLT); /* resumable */
    }
    /* a handler that moves CS:IP itself suppresses the automatic return */
    cfg.resolve = resolve_redirect;
    CHECK_EQ(load_tne(&t, &cfg), LOADER_OK);
    if (g_p) {
        CHECK_EQ(loader_run(g_p, 1000), LOADER_STOP_HLT);
        CHECK_EQ(g_p->cpu.eip, 0x41); /* ran the hlt at 1:0x40 */
        CHECK_EQ(g_p->api_calls, 1);
    }
}

static void test_run_fault(void) {
    tne t = fixture_tne();
    printf("test_run_fault\n");
    build_fixture_parts();
    g_code[0] = 0x0F; g_code[1] = 0xFF; /* undefined opcode */
    CHECK_EQ(load_tne(&t, &cfg_fix), LOADER_OK);
    if (g_p) {
        CHECK_EQ(loader_run(g_p, 100), LOADER_STOP_FAULT);
        CHECK_EQ(g_p->fault, CPU_E_UD);
        CHECK_EQ(g_p->cpu.eip, 0); CHECK_EQ(g_p->cpu.seg[CPU_CS], g_p->seg_sel[0]);
    }
    /* guest write into its own code segment faults, it never reaches the host */
    build_fixture_parts();
    memcpy(g_code, (uint8_t[]){0x2E, 0xC6, 0x06, 0x00, 0x00, 0x55}, 6); /* mov byte cs:[0],0x55 */
    CHECK_EQ(load_tne(&t, &cfg_fix), LOADER_OK);
    if (g_p) {
        CHECK_EQ(loader_run(g_p, 100), LOADER_STOP_FAULT);
        CHECK_EQ(g_p->fault, CPU_E_SEL);
    }
    build_fixture_parts();
}

/* The NE parser rejects some malformed records itself; the loader must also
 * survive them when they reach it, so the record is corrupted after parsing. */
static loader_err load_with_reloc(const ne_reloc *bad) {
    tseg segs[3];
    trel benign = {NE_SRC_OFFSET, F_INT, 0x04, 1, 0};
    tne t = fixture_tne();
    size_t n;
    loader_err e;
    memcpy(segs, g_segs, sizeof segs);
    segs[2].rel = &benign; segs[2].nrel = 1;
    t.segs = segs;
    n = build_ne(g_img, &t);
    unload();
    if (ne_open_buffer(&g_ne, g_img, n) != NE_OK) { g_fails++; return LOADER_ERR_ARG; }
    g_ne.segs[2].relocs[0] = *bad;
    e = loader_create(&g_ne, &cfg_fix, &g_p);
    return e;
}

static void test_bad_relocs(void) {
    ne_reloc r = {NE_SRC_OFFSET, NE_TGT_INTERNAL, 0, 0x04, 9, 0};
    tne t = fixture_tne();
    printf("test_bad_relocs\n");
    CHECK_EQ(load_with_reloc(&r), LOADER_ERR_BAD_RELOC);        /* no such segment */
    CHECK(g_p == NULL);
    r.a = NE_SEG_MOVEABLE_REF; r.b = 9;
    CHECK_EQ(load_with_reloc(&r), LOADER_ERR_BAD_RELOC);        /* no such entry ordinal */
    r.a = NE_SEG_MOVEABLE_REF; r.b = 4;
    CHECK_EQ(load_with_reloc(&r), LOADER_ERR_BAD_RELOC);        /* entry 4 is past the table */
    r.a = 1; r.b = 0; r.additive = 1; r.src_offset = 0x3E; r.src_type = NE_SRC_FARPTR;
    CHECK_EQ(load_with_reloc(&r), LOADER_ERR_BAD_RELOC);        /* dword site past the segment */
    r.src_offset = 0x04; r.src_type = 1;
    CHECK_EQ(load_with_reloc(&r), LOADER_ERR_BAD_RELOC);        /* unsupported source type */
    r.src_type = NE_SRC_OFFSET; r.additive = 0; r.src_offset = 0x04;
    g_d3[0x04] = 0x10; g_d3[0x10] = 0x04; g_d3[0x11] = 0;        /* chain 04 -> 10 -> 04 */
    {
        trel loop = {NE_SRC_OFFSET, F_INT, 0x04, 1, 0};
        tseg segs[3];
        size_t n;
        memcpy(segs, g_segs, sizeof segs);
        segs[2].rel = &loop; segs[2].nrel = 1;
        t.segs = segs;
        n = build_ne(g_img, &t);
        unload();
        CHECK_EQ(ne_open_buffer(&g_ne, g_img, n), NE_OK);
        CHECK_EQ(loader_create(&g_ne, &cfg_fix, &g_p), LOADER_ERR_BAD_RELOC);
    }
    build_fixture_parts();
    /* no stack / entry state */
    t = fixture_tne(); t.ss = 0; t.autods = 0;
    CHECK_EQ(load_tne(&t, &cfg_fix), LOADER_ERR_SEGMENT);
    t = fixture_tne(); t.cs = 9;
    CHECK_EQ(load_tne(&t, &cfg_fix), LOADER_ERR_SEGMENT);
    CHECK_EQ(loader_create(NULL, NULL, &g_p), LOADER_ERR_ARG);
}

static void test_thunk_api(void) {
    tne t = fixture_tne();
    loader_import in;
    loader_thunk *a, *b, *c;
    unsigned i;
    printf("test_thunk_api\n");
    CHECK_EQ(load_tne(&t, &cfg_fix), LOADER_OK);
    if (!g_p) return;
    memset(&in, 0, sizeof in);
    strcpy(in.module, "KERNEL"); in.ordinal = 7;
    CHECK_EQ(loader_thunk_for(g_p, &in, &a), LOADER_OK);
    CHECK(a == loader_find_thunk(g_p, &in) && a->resolved && a->api.arg_bytes == 4);
    CHECK_EQ(loader_thunk_for(g_p, &in, &b), LOADER_OK);
    CHECK(a == b);
    in.ordinal = 8;
    CHECK_EQ(loader_thunk_for(g_p, &in, &c), LOADER_OK);
    CHECK(c != a && !c->resolved && c->off != a->off && c->sel == a->sel);
    /* an import by name never matches the same text as an ordinal */
    in.by_name = true; strcpy(in.name, "X");
    CHECK(loader_find_thunk(g_p, &in) == NULL);
    /* thunk bytes are HLT, and the hook table copes with many thunks */
    CHECK_EQ(rd8(a->sel, a->off), 0xF4);
    in.by_name = false;
    for (i = 100; i < 600; i++) { in.ordinal = (uint16_t)i; CHECK_EQ(loader_thunk_for(g_p, &in, &c), LOADER_OK); }
    CHECK_EQ(g_p->nthunks, 5 + 500);
    /* the CPU hook table bounds the thunk count; a failed add leaves nothing behind */
    for (i = 1000; i < 4000; i++) {
        in.ordinal = (uint16_t)i;
        if (loader_thunk_for(g_p, &in, &c) != LOADER_OK) break;
    }
    CHECK(i < 4000);
    CHECK_EQ(loader_thunk_for(g_p, &in, &c), LOADER_ERR_THUNKS);
    CHECK(loader_find_thunk(g_p, &in) == NULL);
    CHECK_EQ(g_p->nthunks, CPU_HOOK_SLOTS / 2 + 1); /* the loader's own return trap uses one hook; + the two constants of the loop above need none */
}

static void test_const_imports(void) {
    static const trel rel[] = {
        {NE_SRC_OFFSET, F_ORD, 0x00, 1, ORD_CONST_OFF},
        {NE_SRC_LOBYTE, F_ORD | F_ADD, 0x04, 1, ORD_CONST_OFF},
        {NE_SRC_SELECTOR, F_ORD, 0x08, 1, ORD_CONST_SEG},
        {NE_SRC_FARPTR, F_ORD, 0x0c, 1, ORD_CONST_SEG},
    };
    static uint8_t d3[0x20];
    tseg segs[3];
    tne t = fixture_tne();
    loader_import in;
    loader_thunk *c;
    uint16_t sel;
    printf("test_const_imports\n");
    memset(d3, 0, sizeof d3);
    d3[0] = 0xFF; d3[1] = 0xFF; d3[4] = 0x10; d3[8] = 0xFF; d3[9] = 0xFF; d3[0x0c] = 0xFF; d3[0x0d] = 0xFF;
    memcpy(segs, g_segs, sizeof segs);
    segs[2].data = d3; segs[2].len = sizeof d3; segs[2].alloc = sizeof d3;
    segs[2].rel = rel; segs[2].nrel = sizeof rel / sizeof rel[0];
    t.segs = segs;
    CHECK_EQ(load_tne(&t, &cfg_fix), LOADER_OK);
    if (!g_p) return;
    CHECK_EQ(rd16(g_p->seg_sel[2], 0x00), 3);       /* offset: the value, not a thunk address */
    CHECK_EQ(rd8(g_p->seg_sel[2], 0x04), 0x13);     /* additive lobyte */
    sel = rd16(g_p->seg_sel[2], 0x08);
    CHECK(sel != 0 && mem_valid(&g_p->mem, sel) && mem_size(&g_p->mem, sel) == 0x100);
    CHECK_EQ(rd16(g_p->seg_sel[2], 0x0c), 0);       /* far pointer: offset 0, same selector */
    CHECK_EQ(rd16(g_p->seg_sel[2], 0x0e), sel);
    memset(&in, 0, sizeof in);
    strcpy(in.module, "KERNEL"); in.ordinal = ORD_CONST_OFF;
    CHECK_EQ(loader_thunk_for(g_p, &in, &c), LOADER_OK);
    CHECK(c->resolved && c->api.is_const && c->off == 3 && c->sel == 0);
    CHECK_EQ(g_p->unresolved, 0);
    /* a constant is not code: nothing is hooked for it */
    in.ordinal = ORD_CONST_SEG;
    CHECK_EQ(loader_thunk_for(g_p, &in, &c), LOADER_OK);
    CHECK(c->sel == sel && c->off == 0);
}

/* ---- real binaries (skipped unless CWFA_*_EXE is set) --------------------- */

static unsigned g_stub_calls;
static char g_first_call[2 * NE_NAME_MAX + 32];

static cpu_status api_stub(loader_proc *p, const loader_import *imp, void *ctx) {
    (void)ctx;
    if (!g_stub_calls++) {
        if (imp->by_name) snprintf(g_first_call, sizeof g_first_call, "%s.%s", imp->module, imp->name);
        else snprintf(g_first_call, sizeof g_first_call, "%s.%u", imp->module, imp->ordinal);
    }
    return loader_request_stop(p);
}
/* Minimal INITTASK (KERNEL.91): enough to see what the program does next. */
static bool g_inittask;
static cpu_status api_inittask(loader_proc *p, const loader_import *imp, void *ctx) {
    cpu_t *c = &p->cpu;
    (void)imp; (void)ctx;
    c->r[CPU_EAX] = 1; c->r[CPU_ECX] = p->stack_size; c->r[CPU_EDX] = 1; /* SW_SHOWNORMAL */
    c->seg[CPU_ES] = p->psp_sel; c->r[CPU_EBX] = 0x81;
    c->r[CPU_EDI] = p->auto_ds; c->r[CPU_ESI] = 0;
    return CPU_OK;
}
static bool resolve_stub(void *ctx, const loader_import *imp, loader_api *out) {
    (void)ctx;
    out->fn = api_stub; out->arg_bytes = 0; out->ctx = NULL;
    if (g_inittask && !strcmp(imp->module, "KERNEL") && !imp->by_name && imp->ordinal == 91)
        out->fn = api_inittask;
    return true;
}

/* Independent re-derivation of every relocation site from the NE tables. */
static size_t verify_all_sites(const ne_image *ne, loader_proc *p, size_t *bad) {
    size_t verified = 0;
    unsigned i, j;
    *bad = 0;
    for (i = 0; i < ne->seg_count; i++) {
        const ne_segment *s = &ne->segs[i];
        for (j = 0; j < s->reloc_count; j++) {
            const ne_reloc *r = &s->relocs[j];
            uint16_t tsel = 0, toff = 0, *sites;
            size_t n = 0, k;
            if (r->target_type == NE_TGT_OSFIXUP) continue;
            if (r->target_type == NE_TGT_INTERNAL) {
                unsigned seg = r->a;
                toff = r->b;
                if (r->a == NE_SEG_MOVEABLE_REF) {
                    const ne_entry *e;
                    if (ne_entry_for_ordinal(ne, r->b, &e) != NE_OK) { (*bad)++; continue; }
                    seg = e->segment; toff = e->offset;
                }
                tsel = p->seg_sel[seg - 1];
            } else {
                loader_import imp;
                loader_thunk *t;
                memset(&imp, 0, sizeof imp);
                ne_modref_name(ne, r->a, imp.module);
                if (r->target_type == NE_TGT_IMPORT_NAME) { imp.by_name = true; ne_imported_name(ne, r->b, imp.name); }
                else imp.ordinal = r->b;
                t = loader_find_thunk(p, &imp);
                if (!t) { (*bad)++; continue; }
                tsel = t->sel; toff = t->off;
            }
            ne_reloc_sites(ne, i, r, NULL, 0, &n);
            sites = malloc(n * sizeof *sites);
            ne_reloc_sites(ne, i, r, sites, n, &n);
            for (k = 0; k < n; k++) {
                unsigned size = r->src_type == NE_SRC_LOBYTE ? 1 : r->src_type == NE_SRC_FARPTR ? 4 : 2;
                const uint8_t *m = mem_ptr(&p->mem, p->seg_sel[i], sites[k], size);
                uint16_t orig = 0, want;
                bool ok;
                if (!m) { (*bad)++; continue; }
                if (r->additive && s->file_offset && (uint32_t)sites[k] + size <= s->file_length) {
                    const uint8_t *f = ne->data + s->file_offset + sites[k];
                    orig = (uint16_t)(f[0] | (size > 1 ? f[1] << 8 : 0));
                }
                want = r->additive ? (uint16_t)(orig + toff) : toff;
                switch (r->src_type) {
                case NE_SRC_LOBYTE: ok = m[0] == (uint8_t)want; break;
                case NE_SRC_SELECTOR: ok = (uint16_t)(m[0] | m[1] << 8) == tsel; break;
                case NE_SRC_OFFSET: ok = (uint16_t)(m[0] | m[1] << 8) == want; break;
                default: ok = (uint16_t)(m[0] | m[1] << 8) == want && (uint16_t)(m[2] | m[3] << 8) == tsel;
                }
                if (!ok) (*bad)++;
                verified++;
            }
            free(sites);
        }
    }
    return verified;
}

static void real_binary(const char *label, const char *env, size_t want_records, size_t want_imports) {
    const char *path = getenv(env);
    ne_image ne;
    loader_config cfg = {.resolve = resolve_stub, .cmdline = ""};
    size_t records = 0, os = 0, bad = 0, verified;
    unsigned i, j;
    loader_stop st;
    cpu_t *c;
    printf("test_real_%s\n", label);
    if (!path || !*path) {
        printf("  SKIP (%s not set)\n", env);
        return;
    }
    if (ne_open_file(&ne, path) != NE_OK) { printf("  cannot open %s\n", path); g_fails++; return; }
    for (i = 0; i < ne.seg_count; i++)
        for (j = 0; j < ne.segs[i].reloc_count; j++) {
            records++;
            if (ne.segs[i].relocs[j].target_type == NE_TGT_OSFIXUP) os++;
        }
    CHECK_EQ(loader_create(&ne, &cfg, &g_p), LOADER_OK);
    if (!g_p) { ne_free(&ne); return; }
    if (want_records) CHECK_EQ(records, want_records);
    CHECK_EQ(g_p->reloc_records, records);
    CHECK_EQ(g_p->unresolved, 0);
    CHECK_EQ(g_p->nthunks, want_imports);
    CHECK_EQ(g_p->nosfixups, os); /* every OS fixup here is additive: one site each */
    verified = verify_all_sites(&ne, g_p, &bad);
    CHECK_EQ(bad, 0);
    CHECK_EQ(verified, g_p->reloc_sites);
    printf("  %s: %u segments, %zu relocation records (%zu OS fixups), %zu sites patched, %zu imports\n",
           label, ne.seg_count, records, os, g_p->reloc_sites, g_p->nthunks);
    c = &g_p->cpu;
    printf("  entry %04x:%04x ss:sp %04x:%04x ds %04x psp %04x\n", c->seg[CPU_CS], c->eip,
           c->seg[CPU_SS], (unsigned)c->r[CPU_ESP], c->seg[CPU_DS], c->seg[CPU_ES]);
    g_stub_calls = 0; g_first_call[0] = 0;
    st = loader_run(g_p, 50000000ull);
    printf("  run: %s", loader_strstop(st));
    if (st == LOADER_STOP_FAULT) printf(" (%s)", cpu_strstatus(g_p->fault));
    printf(" at %04x:%04x, %u stub call(s), first API: %s\n", c->seg[CPU_CS], c->eip, g_stub_calls,
           g_first_call[0] ? g_first_call : "none");
    {
        uint8_t *op = mem_ptr(&g_p->mem, c->seg[CPU_CS], c->eip, 1);
        unsigned k;
        printf("  bytes at CS:IP:");
        for (k = 0; k < 8; k++) {
            op = mem_ptr(&g_p->mem, c->seg[CPU_CS], c->eip + k, 1);
            if (op) printf(" %02x", *op);
        }
        printf("\n");
    }
    CHECK_EQ(st, LOADER_STOP_REQUESTED);
    CHECK_EQ(g_stub_calls, 1);
    CHECK(!strcmp(g_first_call, "KERNEL.91")); /* INITTASK */
    /* characterization: with INITTASK answered, where does the program go next? */
    unload();
    g_inittask = true;
    CHECK_EQ(loader_create(&ne, &cfg, &g_p), LOADER_OK);
    if (g_p) {
        c = &g_p->cpu;
        g_stub_calls = 0; g_first_call[0] = 0;
        st = loader_run(g_p, 50000000ull);
        printf("  after INITTASK: %s", loader_strstop(st));
        if (st == LOADER_STOP_FAULT) printf(" (%s)", cpu_strstatus(g_p->fault));
        printf(" at %04x:%04x, %llu API call(s), last import %s.%u\n", c->seg[CPU_CS], c->eip,
               (unsigned long long)g_p->api_calls,
               g_p->last_call ? g_p->last_call->imp.module : "-",
               g_p->last_call ? g_p->last_call->imp.ordinal : 0);
    }
    g_inittask = false;
    unload();
}

int main(void) {
    build_fixture_parts();
    test_segments();
    test_big_segments();
    test_relocations();
    test_run_to_completion();
    test_initial_state();
    test_run_stops();
    test_run_fault();
    test_bad_relocs();
    test_thunk_api();
    test_const_imports();
    real_binary("writer", "CWFA_WRITER_EXE", 3150, 318);
    real_binary("artist", "CWFA_ARTIST_EXE", 0, 311);
    unload();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
