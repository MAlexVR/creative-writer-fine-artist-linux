/* Multi-module loader: NE libraries built in memory, loaded through the open
 * callback, imports resolved against other modules' exports, entry points, the
 * shared auto data segment, reference counts and unloading. */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>

#include "fixture.h"
#include "dllfix.h"

typedef struct {
    const char *key;
    uint8_t *buf;
    size_t len;
    int err; /* non-zero: the open callback fails with this Windows error code */
} store_ent;

static store_ent g_store[8];
static unsigned g_nstore, g_opens;

static void store_add(const char *key, const nb_t *t) {
    uint8_t *b = malloc(NB_MAX);
    g_store[g_nstore].key = key;
    g_store[g_nstore].len = nb_build(b, t);
    g_store[g_nstore].buf = b;
    g_store[g_nstore].err = 0;
    g_nstore++;
}

static void store_clear(void) {
    unsigned i;
    for (i = 0; i < g_nstore; i++) free(g_store[i].buf);
    g_nstore = g_opens = 0;
}

static int open_cb(void *ctx, const char *spec, ne_image *out, char path[LOADER_PATH_MAX]) {
    const char *b = spec, *q;
    char key[32], ext[8] = "DLL";
    size_t n = 0, i;
    (void)ctx;
    g_opens++;
    for (q = spec; *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
    for (; b[n] && b[n] != '.' && n < sizeof key - 1; n++) key[n] = (char)toupper((unsigned char)b[n]);
    key[n] = 0;
    if (b[n] == '.') {
        for (i = 0; i < 3 && b[n + 1 + i]; i++) ext[i] = (char)toupper((unsigned char)b[n + 1 + i]);
        ext[i] = 0;
    }
    for (i = 0; i < g_nstore; i++) {
        if (strcmp(g_store[i].key, key)) continue;
        if (g_store[i].err) return g_store[i].err;
        if (ne_open_buffer(out, g_store[i].buf, g_store[i].len) != NE_OK) return 11;
        snprintf(path, LOADER_PATH_MAX, "C:\\LIB\\%s.%s", key, ext);
        return 0;
    }
    return 2;
}

/* ---- images ---------------------------------------------------------------- */

/* A program: calls MYDLL.1(7, 5) and MYDLL."getd", stores the results at DS:20 and DS:22, HLT. */
static const uint8_t prog_code[] = {
    0x68, 0x07, 0x00, 0x68, 0x05, 0x00, 0x9A, 0xFF, 0xFF, 0x00, 0x00, 0xA3, 0x20, 0x00,
    0x9A, 0xFF, 0xFF, 0x00, 0x00, 0xA3, 0x22, 0x00, 0xF4,
};
static const uint8_t halt_code[] = {0xF4};

static void prog_nb(nb_t *t, const uint8_t *code, unsigned len, const nb_rel *rel, unsigned nrel,
                    const char *ref) {
    memset(t, 0, sizeof *t);
    t->module = "PROG";
    t->flags = 0x0002;
    t->autods = 2;
    t->stack = 0x200;
    t->cs = 1;
    t->ss = 2;
    t->segs[0] = (nb_seg){code, len, SEG_CODE, len, rel, nrel};
    t->segs[1] = (nb_seg){NULL, 0, SEG_DATA, 0x100, NULL, 0};
    t->nsegs = 2;
    if (ref) {
        t->modrefs[0] = ref;
        t->nmodrefs = 1;
    }
}

typedef struct {
    ne_image ne;
    loader_proc *p;
} prog_t;

static bool prog_load(prog_t *g, const nb_t *t, loader_err *err) {
    static uint8_t buf[NB_MAX];
    loader_config lc;
    size_t n = nb_build(buf, t);
    memset(g, 0, sizeof *g);
    if (ne_open_buffer(&g->ne, buf, n) != NE_OK) return false;
    memset(&lc, 0, sizeof lc);
    lc.open_module = open_cb;
    *err = loader_create(&g->ne, &lc, &g->p);
    return *err == LOADER_OK;
}

static void prog_free(prog_t *g) {
    loader_destroy(g->p);
    ne_free(&g->ne);
}

static uint16_t ds_word(loader_proc *p, uint16_t sel, unsigned off) {
    const uint8_t *d = mem_ptr(&p->mem, sel, off, 2);
    return d ? (uint16_t)(d[0] | d[1] << 8) : 0xDEAD;
}

/* ---- tests ----------------------------------------------------------------- */

static void test_import_from_dll(void) {
    nb_t dll, prog;
    prog_t g;
    loader_err e;
    loader_module *m;
    uint16_t dsel;
    nb_rel rel[2];
    printf("test_modules_import_from_dll\n");
    store_clear();
    mydll_nb(&dll);
    store_add("MYDLL", &dll);
    memset(rel, 0, sizeof rel);
    rel[0] = (nb_rel){NE_SRC_FARPTR, NB_F_ORD, 0x07, 1, 1};
    prog_nb(&prog, prog_code, sizeof prog_code, rel, 1, "MYDLL");
    prog.impnames[0] = "getd";
    prog.nimpnames = 1;
    rel[1] = (nb_rel){NE_SRC_FARPTR, NB_F_NAME, 0x0F, 1, (uint16_t)nb_impname_off(&prog, 0)};
    prog.segs[0].nrel = 2;
    CHECK(prog_load(&g, &prog, &e));
    if (!g.p) return;
    CHECK_EQ(g.p->nmods, 2);
    m = g.p->nmods > 1 ? g.p->mods[1] : NULL;
    CHECK(m != NULL);
    if (!m) return;
    CHECK(!strcmp(m->name, "MYDLL"));
    CHECK(!strcmp(m->key, "MYDLL"));
    CHECK(!strcmp(m->path, "C:\\LIB\\MYDLL.DLL"));
    CHECK(m->is_dll);
    CHECK_EQ(m->usage, 1);
    CHECK_EQ(m->handle, m->auto_ds);
    CHECK(m->auto_ds != 0 && m->auto_ds != g.p->auto_ds);
    CHECK_EQ(m->heap_size, 0x100);
    CHECK_EQ(g.p->mods[0]->usage, 1);
    CHECK(g.p->mods[0]->ne == &g.ne);
    CHECK_EQ(loader_run(g.p, 10000), LOADER_STOP_HLT);
    dsel = m->auto_ds;
    CHECK_EQ(m->inited, true);
    CHECK_EQ(ds_word(g.p, dsel, 0x10), 0x0100);            /* the entry point ran with DS = auto data */
    CHECK_EQ(ds_word(g.p, dsel, 0x12), m->handle);          /* DI = hInstance */
    CHECK_EQ(ds_word(g.p, dsel, 0x14), 0x100);              /* CX = heap size */
    CHECK_EQ(ds_word(g.p, g.p->auto_ds, 0x20), 7 + 5 + 0x100); /* ADD2 used its own DS (patched prolog) */
    CHECK_EQ(ds_word(g.p, g.p->auto_ds, 0x22), 0x0100);    /* by-name import, case-insensitive, 2nd prolog pattern */
    CHECK_EQ(ds_word(g.p, g.p->auto_ds, 0x10), 0);          /* the program's data is untouched */
    CHECK_EQ(g.p->unresolved, 0);
    CHECK_EQ(g.p->nthunks, 0);                              /* no thunk: the call goes to real code */
    CHECK_EQ(g_opens, 1);                                   /* one open for the one module reference */
    prog_free(&g);
}

static void test_module_queries(void) {
    nb_t dll, prog;
    prog_t g;
    loader_err e;
    loader_module *m;
    uint16_t sel, off;
    printf("test_modules_queries\n");
    store_clear();
    mydll_nb(&dll);
    store_add("MYDLL", &dll);
    prog_nb(&prog, halt_code, sizeof halt_code, NULL, 0, NULL);
    CHECK(prog_load(&g, &prog, &e));
    if (!g.p) return;
    CHECK_EQ(g.p->nmods, 1);
    CHECK(loader_module_by_name(g.p, "prog") == g.p->mods[0]);
    CHECK(loader_module_by_handle(g.p, g.p->auto_ds) == g.p->mods[0]);
    CHECK(loader_module_by_ds(g.p, g.p->auto_ds) == g.p->mods[0]);
    CHECK(loader_load_library(g.p, "mydll", &m, &(unsigned){0}, &(cpu_status){CPU_OK}) == LOADER_OK);
    CHECK(m != NULL);
    if (!m) return;
    CHECK(loader_module_by_name(g.p, "MyDll") == m);        /* module name, any case */
    CHECK(loader_module_by_name(g.p, "mydll") == m);
    CHECK(loader_module_by_name(g.p, "OTHER") == NULL);
    CHECK(loader_module_by_handle(g.p, m->handle) == m);
    CHECK(loader_module_by_ds(g.p, m->auto_ds) == m);
    CHECK(loader_module_by_handle(g.p, 0x1234) == NULL);
    CHECK(loader_module_proc(m, NULL, 1, &sel, &off));      /* ordinal */
    CHECK_EQ(sel, m->seg_sel[0]);
    CHECK_EQ(off, 0);
    CHECK(loader_module_proc(m, "add2", 0, &sel, &off));     /* resident name, case-insensitive */
    CHECK_EQ(off, 0);
    CHECK(loader_module_proc(m, "GETD", 0, &sel, &off));
    CHECK_EQ(off, 0x20);
    CHECK(loader_module_proc(m, "nonres", 0, &sel, &off));   /* non-resident name */
    CHECK_EQ(off, 0x20);
    CHECK(!loader_module_proc(m, "MyDll", 0, &sel, &off));   /* the module name is not an export */
    CHECK(!loader_module_proc(m, "NOSUCH", 0, &sel, &off));
    CHECK(!loader_module_proc(m, NULL, 9, &sel, &off));      /* no such entry */
    CHECK(!loader_module_proc(m, NULL, 0, &sel, &off));
    CHECK(loader_module_proc(g.p->mods[0], NULL, 1, &sel, &off) == false); /* the program exports nothing */
    prog_free(&g);
}

/* LIBB.1 returns its data word 0x10 (set by its entry point); LIBA.1 calls it and adds one, and its own
 * entry point calls LIBB.1 too, so LIBB must have been initialized first. */
static const uint8_t libb_code[0x40] = {
    0x8C, 0xD8, 0x90, 0x45, 0x55, 0x8B, 0xEC, 0x1E, 0x8E, 0xD8, 0xA1, 0x10, 0x00, 0x8D, 0x66, 0xFE, 0x1F,
    0x5D, 0x4D, 0xCB,
    [0x30] = 0xC7, 0x06, 0x10, 0x00, 0x33, 0x33, 0xB8, 0x01, 0x00, 0xCB,
};
static const uint8_t liba_code[0x40] = {
    0x8C, 0xD8, 0x90, 0x45, 0x55, 0x8B, 0xEC, 0x1E, 0x8E, 0xD8, 0x9A, 0xFF, 0xFF, 0x00, 0x00, 0x40,
    0x8D, 0x66, 0xFE, 0x1F, 0x5D, 0x4D, 0xCB,
    [0x30] = 0x9A, 0xFF, 0xFF, 0x00, 0x00, 0xA3, 0x10, 0x00, 0xB8, 0x01, 0x00, 0xCB,
};

static void chain_images(void) {
    static const nb_rel liba_rel[] = {{NE_SRC_FARPTR, NB_F_ORD, 0x0B, 1, 1}, {NE_SRC_FARPTR, NB_F_ORD, 0x31, 1, 1}};
    nb_t t;
    memset(&t, 0, sizeof t);
    t.module = "LIBB";
    t.flags = 0x8001;
    t.autods = 2;
    t.heap = 0x10;
    t.cs = 1;
    t.ip = 0x30;
    t.segs[0] = (nb_seg){libb_code, sizeof libb_code, SEG_CODE, sizeof libb_code, NULL, 0};
    t.segs[1] = (nb_seg){NULL, 0, SEG_DATA, 0x40, NULL, 0};
    t.nsegs = 2;
    t.ents[0] = (nb_ent){1, 0};
    t.nents = 1;
    store_add("LIBB", &t);
    memset(&t, 0, sizeof t);
    t.module = "LIBA";
    t.flags = 0x8001;
    t.autods = 2;
    t.cs = 1;
    t.ip = 0x30;
    t.segs[0] = (nb_seg){liba_code, sizeof liba_code, SEG_CODE, sizeof liba_code, liba_rel, 2};
    t.segs[1] = (nb_seg){NULL, 0, SEG_DATA, 0x40, NULL, 0};
    t.nsegs = 2;
    t.modrefs[0] = "LIBB";
    t.nmodrefs = 1;
    t.ents[0] = (nb_ent){1, 0};
    t.nents = 1;
    store_add("LIBA", &t);
}

static void test_dependency_chain(void) {
    static const uint8_t code[] = {0x9A, 0xFF, 0xFF, 0x00, 0x00, 0xA3, 0x20, 0x00, 0xF4};
    static const nb_rel prel[] = {{NE_SRC_FARPTR, NB_F_ORD, 0x01, 1, 1}};
    nb_t prog;
    prog_t g;
    loader_err e;
    loader_module *la, *lb;
    printf("test_modules_dependency_chain\n");
    store_clear();
    chain_images();
    prog_nb(&prog, code, sizeof code, prel, 1, "LIBA");
    CHECK(prog_load(&g, &prog, &e));
    if (!g.p) return;
    CHECK_EQ(g.p->nmods, 3);                                 /* program, LIBA, and LIBB loaded for it */
    la = loader_module_by_name(g.p, "LIBA");
    lb = loader_module_by_name(g.p, "LIBB");
    CHECK(la && lb);
    if (!la || !lb) return;
    CHECK_EQ(lb->usage, 1);                                  /* held by LIBA */
    CHECK_EQ(la->usage, 1);                                  /* held by the program */
    CHECK_EQ(la->ndeps, 1);
    CHECK(la->deps[0] == lb);
    CHECK_EQ(g.p->mods[0]->ndeps, 1);
    CHECK_EQ(loader_run(g.p, 10000), LOADER_STOP_HLT);
    CHECK_EQ(ds_word(g.p, lb->auto_ds, 0x10), 0x3333);
    CHECK_EQ(ds_word(g.p, la->auto_ds, 0x10), 0x3333);       /* LIBA's entry saw LIBB initialized first */
    CHECK_EQ(ds_word(g.p, g.p->auto_ds, 0x20), 0x3334);
    prog_free(&g);
}

static void test_load_and_free_library(void) {
    nb_t prog;
    prog_t g;
    loader_err e;
    loader_module *a, *b, *c;
    unsigned werr = 99;
    cpu_status fault = CPU_E_UD;
    uint16_t asel, bsel, hand;
    printf("test_modules_load_free_library\n");
    store_clear();
    chain_images();
    prog_nb(&prog, halt_code, sizeof halt_code, NULL, 0, NULL);
    CHECK(prog_load(&g, &prog, &e));
    if (!g.p) return;
    CHECK(loader_load_library(g.p, "C:\\LIB\\liba.dll", &a, &werr, &fault) == LOADER_OK);
    CHECK_EQ(fault, CPU_OK);
    CHECK(a != NULL);
    if (!a) return;
    CHECK_EQ(g.p->nmods, 3);
    CHECK_EQ(a->usage, 1);
    CHECK(a->inited);
    CHECK_EQ(ds_word(g.p, a->auto_ds, 0x10), 0x3333);        /* initialized in dependency order from the host */
    b = loader_module_by_name(g.p, "LIBB");
    CHECK(b != NULL);
    if (!b) return;
    CHECK_EQ(b->usage, 1);
    asel = a->auto_ds;
    bsel = b->auto_ds;
    hand = a->handle;
    CHECK(loader_load_library(g.p, "LIBA", &c, &werr, &fault) == LOADER_OK); /* already loaded: usage + 1 */
    CHECK(c == a);
    CHECK_EQ(a->usage, 2);
    CHECK_EQ(g.p->nmods, 3);
    CHECK(!loader_free_library(g.p, a));                      /* 2 -> 1: stays */
    CHECK_EQ(a->usage, 1);
    CHECK(mem_valid(&g.p->mem, asel));
    CHECK(loader_free_library(g.p, a));                       /* 1 -> 0: unloaded with its dependency */
    CHECK_EQ(g.p->nmods, 1);
    CHECK(!mem_valid(&g.p->mem, asel));
    CHECK(!mem_valid(&g.p->mem, bsel));
    CHECK(loader_module_by_handle(g.p, hand) == NULL);
    CHECK(loader_load_library(g.p, "LIBA", &c, &werr, &fault) == LOADER_OK); /* and can be loaded again */
    CHECK_EQ(g.p->nmods, 3);
    CHECK(!loader_free_library(g.p, g.p->mods[0]));           /* the program is never freed */
    CHECK_EQ(g.p->nmods, 3);
    prog_free(&g);
}

static void test_load_errors(void) {
    static const uint8_t bad[] = {0xB8, 0x00, 0x00, 0xCB};   /* entry point returns 0 */
    static const uint8_t crash[] = {0x0F, 0x0B};             /* UD2 */
    nb_t prog, t;
    prog_t g;
    loader_err e;
    loader_module *m = NULL;
    unsigned werr = 0;
    cpu_status fault = CPU_OK;
    printf("test_modules_load_errors\n");
    store_clear();
    memset(&t, 0, sizeof t);
    t.module = "BADINIT";
    t.flags = 0x8001;
    t.autods = 2;
    t.cs = 1;
    t.segs[0] = (nb_seg){bad, sizeof bad, SEG_CODE, sizeof bad, NULL, 0};
    t.segs[1] = (nb_seg){NULL, 0, SEG_DATA, 0x20, NULL, 0};
    t.nsegs = 2;
    store_add("BADINIT", &t);
    t.module = "CRASHY";
    t.segs[0] = (nb_seg){crash, sizeof crash, SEG_CODE, sizeof crash, NULL, 0};
    store_add("CRASHY", &t);
    store_add("BROKEN", &t);
    g_store[g_nstore - 1].err = 11;
    prog_nb(&prog, halt_code, sizeof halt_code, NULL, 0, NULL);
    CHECK(prog_load(&g, &prog, &e));
    if (!g.p) return;
    CHECK(loader_load_library(g.p, "NOSUCH", &m, &werr, &fault) == LOADER_ERR_NOTFOUND);
    CHECK_EQ(werr, 2);
    CHECK(m == NULL);
    CHECK(loader_load_library(g.p, "BROKEN", &m, &werr, &fault) == LOADER_ERR_FORMAT);
    CHECK_EQ(werr, 11);
    CHECK(loader_load_library(g.p, "BADINIT", &m, &werr, &fault) == LOADER_ERR_INIT);
    CHECK_EQ(werr, 0);
    CHECK_EQ(fault, CPU_OK);
    CHECK_EQ(g.p->nmods, 1);                                 /* a failed entry point leaves nothing loaded */
    CHECK(loader_load_library(g.p, "CRASHY", &m, &werr, &fault) == LOADER_ERR_INIT);
    CHECK(fault != CPU_OK);                                  /* guest fault: the caller must stop the run */
    prog_free(&g);
}

static void test_call_far_nested(void) {
    nb_t prog, dll;
    prog_t g;
    loader_err e;
    loader_module *m;
    uint32_t ret = 0;
    uint16_t args[2] = {30, 4};
    uint32_t esp, eip;
    uint16_t cs;
    printf("test_modules_call_far\n");
    store_clear();
    mydll_nb(&dll);
    store_add("MYDLL", &dll);
    prog_nb(&prog, halt_code, sizeof halt_code, NULL, 0, NULL);
    CHECK(prog_load(&g, &prog, &e));
    if (!g.p) return;
    CHECK(loader_load_library(g.p, "MYDLL", &m, &(unsigned){0}, &(cpu_status){CPU_OK}) == LOADER_OK);
    if (!m) return;
    esp = g.p->cpu.r[CPU_ESP];
    eip = g.p->cpu.eip;
    cs = g.p->cpu.seg[CPU_CS];
    g.p->cpu.r[CPU_EBX] = 0x1234;
    CHECK_EQ(loader_call_far(g.p, m->seg_sel[0], 0, args, 2, NULL, &ret), CPU_OK);
    CHECK_EQ(ret & 0xFFFF, 30 + 4 + 0x100);
    CHECK_EQ(g.p->cpu.r[CPU_ESP], esp);                      /* registers and stack as before the call */
    CHECK_EQ(g.p->cpu.eip, eip);
    CHECK_EQ(g.p->cpu.seg[CPU_CS], cs);
    CHECK_EQ(g.p->cpu.r[CPU_EBX], 0x1234);
    CHECK_EQ(g.p->depth, 0);
    prog_free(&g);
}

static void test_resource_only_library(void) {
    nb_t prog, t;
    prog_t g;
    loader_err e;
    loader_module *m = NULL;
    unsigned werr = 0;
    cpu_status fault = CPU_OK;
    uint16_t h;
    loader_module *c2 = NULL;
    printf("test_modules_resource_only\n");
    store_clear();
    memset(&t, 0, sizeof t);
    t.module = "SOUNDDLL";
    t.flags = 0x8001;
    store_add("WRITER", &t);                                 /* two files, one module name */
    store_add("ARTIST", &t);
    prog_nb(&prog, halt_code, sizeof halt_code, NULL, 0, NULL);
    CHECK(prog_load(&g, &prog, &e));
    if (!g.p) return;
    CHECK(loader_load_library(g.p, "WRITER.SND", &m, &werr, &fault) == LOADER_OK);
    CHECK(m != NULL);
    if (!m) return;
    CHECK(!strcmp(m->name, "SOUNDDLL"));
    CHECK(!strcmp(m->key, "WRITER"));
    CHECK(!strcmp(m->file, "WRITER.SND"));
    CHECK(!strcmp(m->path, "C:\\LIB\\WRITER.SND"));
    CHECK_EQ(m->auto_ds, 0);
    CHECK(m->handle != 0 && mem_valid(&g.p->mem, m->handle)); /* a private handle selector */
    CHECK(m->own_handle);
    h = m->handle;
    CHECK(loader_load_library(g.p, "writer.snd", &c2, &werr, &fault) == LOADER_OK && c2 == m); /* same file */
    CHECK_EQ(m->usage, 2);
    CHECK(!loader_free_library(g.p, m));
    CHECK(loader_module_by_handle(g.p, h) == m);
    CHECK(loader_load_library(g.p, "ARTIST.SND", &m, &werr, &fault) == LOADER_OK); /* a second file, same module name */
    CHECK(m != NULL && m->handle != h);
    CHECK_EQ(g.p->nmods, 3);
    CHECK(loader_free_library(g.p, loader_module_by_handle(g.p, h)));
    CHECK(!mem_valid(&g.p->mem, h));
    prog_free(&g);
}

/* A reference cycle (A imports B, B imports A) loads; both resolve against the other's segments. */
static void test_cycle(void) {
    static const uint8_t cd[0x40] = {0x9A, 0xFF, 0xFF, 0x00, 0x00, 0xCB, [0x10] = 0xB8, 0x01, 0x00, 0xCB};
    static const nb_rel ra[] = {{NE_SRC_FARPTR, NB_F_ORD, 0x01, 1, 1}};
    nb_t t;
    nb_t prog;
    prog_t g;
    loader_err e;
    printf("test_modules_cycle\n");
    store_clear();
    memset(&t, 0, sizeof t);
    t.module = "CYCA";
    t.flags = 0x8001;
    t.segs[0] = (nb_seg){cd, sizeof cd, SEG_CODE, sizeof cd, ra, 1};
    t.nsegs = 1;
    t.modrefs[0] = "CYCB";
    t.nmodrefs = 1;
    t.ents[0] = (nb_ent){1, 0};
    t.nents = 1;
    store_add("CYCA", &t);
    t.module = "CYCB";
    t.modrefs[0] = "CYCA";
    store_add("CYCB", &t);
    prog_nb(&prog, halt_code, sizeof halt_code, NULL, 0, NULL);
    CHECK(prog_load(&g, &prog, &e));
    if (!g.p) return;
    /* the program imports nothing from CYCA, so only the explicit load pulls the cycle in */
    {
        loader_module *m = NULL;
        CHECK(loader_load_library(g.p, "CYCA", &m, &(unsigned){0}, &(cpu_status){CPU_OK}) == LOADER_OK);
        CHECK_EQ(g.p->nmods, 3);
        CHECK(m && m->ndeps == 1);
    }
    prog_free(&g);
}

static cpu_status api_77(loader_proc *p, const loader_import *imp, void *ctx) {
    (void)imp;
    (void)ctx;
    p->cpu.r[CPU_EAX] = 0x77;
    return CPU_OK;
}

static bool test_resolve(void *ctx, const loader_import *imp, loader_api *out) {
    (void)ctx;
    if (strcmp(imp->module, "KERNEL")) return false;
    memset(out, 0, sizeof *out);
    out->fn = api_77;
    return true;
}

/* Imports no loaded or loadable module satisfies go to the host resolver; real exports win over it. */
static void test_host_resolver_fallback(void) {
    static const uint8_t code[] = {0x9A, 0xFF, 0xFF, 0x00, 0x00, 0xA3, 0x20, 0x00, 0x9A, 0xFF, 0xFF, 0x00, 0x00,
                                   0xA3, 0x22, 0x00, 0xF4};
    static const nb_rel rel[] = {{NE_SRC_FARPTR, NB_F_ORD, 0x01, 1, 5}, {NE_SRC_FARPTR, NB_F_ORD, 0x09, 2, 99}};
    nb_t prog;
    ne_image ne;
    static uint8_t buf[NB_MAX];
    loader_config lc;
    loader_proc *p = NULL;
    printf("test_modules_host_resolver_fallback\n");
    store_clear();
    prog_nb(&prog, code, sizeof code, rel, 2, "KERNEL");
    prog.modrefs[1] = "MISSING";
    prog.nmodrefs = 2;
    CHECK(ne_open_buffer(&ne, buf, nb_build(buf, &prog)) == NE_OK);
    memset(&lc, 0, sizeof lc);
    lc.open_module = open_cb;
    lc.resolve = test_resolve;
    CHECK_EQ(loader_create(&ne, &lc, &p), LOADER_OK);
    if (p) {
        CHECK_EQ(p->nmods, 1);                               /* KERNEL and MISSING are no files */
        CHECK_EQ(p->nthunks, 2);
        CHECK_EQ(p->unresolved, 1);                          /* MISSING.99 has no taker */
        CHECK_EQ(g_opens, 2);                                /* one open per module reference, not per relocation */
        CHECK_EQ(loader_run(p, 1000), LOADER_STOP_UNRESOLVED);
        loader_destroy(p);
    }
    ne_free(&ne);
}

int main(void) {
    test_import_from_dll();
    test_module_queries();
    test_dependency_chain();
    test_load_and_free_library();
    test_load_errors();
    test_call_far_nested();
    test_resource_only_library();
    test_cycle();
    test_host_resolver_fallback();
    store_clear();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
