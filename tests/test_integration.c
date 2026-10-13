/* Characterization: run the real programs from their entry point with the
 * KERNEL HLE installed and every other import bound to the logging stub that
 * stops the run. Skipped unless CWFA_WRITER_EXE / CWFA_ARTIST_EXE are set;
 * CWFA_CD_ROOT optionally maps the CD to drive D:. */
#include "hostdir.h"
#include "fixture.h"
#include "kernel.h"
#include "user.h"
#include "win87em.h"

#define SEQ_MAX 160

typedef struct {
    char module[16];
    unsigned ordinal;
    unsigned seg;       /* NE segment number of the call site (1-based), 0 if not a program segment */
    uint16_t off;       /* call site offset: the far call's return address */
    bool implemented;
} seq_rec;

typedef struct {
    seq_rec seq[SEQ_MAX];
    loader_stop stop;
    cpu_status fault;
    size_t ncalls;       /* API calls executed, the stopping one included */
    size_t kernel_calls; /* of those, KERNEL calls that were implemented */
    bool stopped_on_unimpl;
    char stop_module[NE_NAME_MAX + 1];
    unsigned stop_ordinal;
    char stop_name[NE_NAME_MAX + 1];
    uint16_t stop_cs, stop_ip;       /* where the guest stopped */
    uint16_t caller_cs, caller_ip;   /* the call site of the stopping API call */
    uint64_t esc_hits[4];            /* executions of the C runtime's four x87 escapes (__ftol) */
} result;

/* The Microsoft C runtime's float-to-long helper (__ftol) holds the four x87
 * escapes WRITER 5:1f14, 1f23, 1f27, 1f2b; ARTIST has the same code at segment 15, d3fa ... */
typedef struct { unsigned seg; uint16_t esc[4]; uint16_t entry; } ftol_site;
static const ftol_site writer_ftol = {5, {0x1f14, 0x1f23, 0x1f27, 0x1f2b}, 0x1f02};
static const ftol_site artist_ftol = {15, {0xd3fa, 0xd409, 0xd40d, 0xd411}, 0xd3e8};

static cpu_status halt_hook(cpu_t *c, void *ctx) {
    (void)c; (void)ctx;
    return CPU_HALT;
}

static cpu_status count_hook(cpu_t *c, void *ctx) {
    (void)c;
    ++*(uint64_t *)ctx;
    return CPU_OK;
}

static unsigned seg_of(const loader_proc *p, uint16_t sel) {
    unsigned i;
    for (i = 0; i < p->ne->seg_count; i++)
        if (p->seg_sel[i] == sel) return i + 1;
    return 0;
}

static void collect_sequence(const hle_t *h, const loader_proc *p, result *out) {
    size_t i;
    for (i = 0; i < h->nrec && i < SEQ_MAX; i++) {
        const hle_record *r = &h->rec[i];
        seq_rec *q = &out->seq[i];
        snprintf(q->module, sizeof q->module, "%.15s", r->module);
        q->ordinal = r->ordinal;
        q->seg = seg_of(p, r->caller_cs);
        q->off = r->caller_ip;
        q->implemented = r->implemented;
        printf("    %3zu %s.%u%s%s%s  from segment %u:%04x\n", i + 1, r->module, r->ordinal,
               r->name[0] ? " " : "", r->name, r->implemented ? "" : "  <== UNIMPLEMENTED/STOP",
               q->seg, r->caller_ip);
    }
}

/* Every KERNEL import of the program must have a handler (or be a constant). */
static void check_kernel_coverage(const char *label, const char *env) {
    const char *path = getenv(env);
    ne_image ne;
    hle_t h;
    kernel_t *k;
    ne_import *imp = NULL;
    size_t n = 0, i, total = 0, missing = 0;
    char name[NE_NAME_MAX + 1];
    printf("test_kernel_coverage_%s\n", label);
    if (!path || !*path) {
        printf("  SKIP (%s not set)\n", env);
        return;
    }
    CHECK_EQ(ne_open_file(&ne, path), NE_OK);
    hle_init(&h);
    k = kernel_create(NULL, &h, 0);
    CHECK(k != NULL);
    CHECK_EQ(ne_collect_imports(&ne, &imp, &n), NE_OK);
    for (i = 0; i < n; i++) {
        const hle_entry *e;
        if (ne_modref_name(&ne, imp[i].modref, name) != NE_OK || strcmp(name, "KERNEL") || imp[i].by_name)
            continue;
        total++;
        e = hle_find(&h, "KERNEL", imp[i].ordinal);
        if (!e || (!e->fn && !e->is_const)) {
            missing++;
            printf("    not implemented: KERNEL.%u %s\n", imp[i].ordinal, e ? e->name : "?");
        }
    }
    printf("  %zu KERNEL imports, %zu without a handler\n", total, missing);
    CHECK_EQ(missing, 0);
    ne_free_imports(imp);
    kernel_destroy(k);
    hle_destroy(&h);
    ne_free(&ne);
}

static void run_program(const char *label, const char *env, unsigned flags, bool fpu, const ftol_site *ft,
                        result *out) {
    const char *path = getenv(env), *cd = getenv("CWFA_CD_ROOT");
    ne_image ne;
    hle_t h;
    kernel_t *k;
    win87em_t *w87 = NULL;
    kernel_config cfg;
    loader_config lc;
    loader_proc *p;
    char *tmp;
    char host[512];
    printf("test_integration_%s\n", label);
    memset(out, 0, sizeof *out);
    if (!path || !*path) {
        printf("  SKIP (%s not set)\n", env);
        return;
    }
    CHECK_EQ(ne_open_file(&ne, path), NE_OK);
    tmp = hd_make();
    snprintf(host, sizeof host, "%s", tmp);
    memset(&cfg, 0, sizeof cfg);
    cfg.host_root['C' - 'A'] = host;
    if (cd && *cd) cfg.host_root['D' - 'A'] = cd;
    cfg.exe_path = "C:\\CWFA\\PROGRAM.EXE";
    cfg.debug = stderr;
    hle_init(&h);
    h.stub_unknown = true;
    h.record = true;
    k = kernel_create(&cfg, &h, flags);
    if (fpu) {
        w87 = win87em_create(&h);
        CHECK(w87 != NULL);
    }
    CHECK(k != NULL);
    memset(&lc, 0, sizeof lc);
    lc.resolve = hle_resolve;
    lc.resolve_ctx = &h;
    lc.cmdline = "";
    CHECK_EQ(loader_create(&ne, &lc, &p), LOADER_OK);
    if (p && k) {
        cpu_t *c = &p->cpu;
        CHECK(kernel_attach(k, p));
        {
            unsigned e;
            for (e = 0; e < 4 && ft; e++)
                CHECK(cpu_add_hook(c, p->seg_sel[ft->seg - 1], ft->esc[e], count_hook, &out->esc_hits[e]));
        }
        out->stop = loader_run(p, 400000000ull);
        out->fault = p->fault;
        out->stop_cs = c->seg[CPU_CS];
        out->stop_ip = (uint16_t)c->eip;
        out->ncalls = h.nrec;
        {
            size_t i;
            for (i = 0; i < h.nrec; i++)
                if (h.rec[i].implemented && !strcmp(h.rec[i].module, "KERNEL")) out->kernel_calls++;
        }
        if (h.nrec && !h.rec[h.nrec - 1].implemented) {
            const hle_record *r = &h.rec[h.nrec - 1];
            out->stopped_on_unimpl = true;
            snprintf(out->stop_module, sizeof out->stop_module, "%s", r->module);
            out->stop_ordinal = r->ordinal;
            snprintf(out->stop_name, sizeof out->stop_name, "%s", r->name);
            out->caller_cs = r->caller_cs;
            out->caller_ip = r->caller_ip;
        }
        printf("  %zu API calls (%zu implemented KERNEL), run: %s", out->ncalls, out->kernel_calls,
               loader_strstop(out->stop));
        if (out->stop == LOADER_STOP_FAULT) printf(" (%s)", cpu_strstatus(out->fault));
        printf(" at %04x:%04x\n", out->stop_cs, out->stop_ip);
        collect_sequence(&h, p, out);
        if (ft)
            printf("  x87 escapes of the C runtime's float-to-long helper executed: %llu %llu %llu %llu\n",
                   (unsigned long long)out->esc_hits[0], (unsigned long long)out->esc_hits[1],
                   (unsigned long long)out->esc_hits[2], (unsigned long long)out->esc_hits[3]);
    }
    loader_destroy(p);
    kernel_destroy(k);
    win87em_destroy(w87);
    hle_destroy(&h);
    ne_free(&ne);
    hd_remove(host);
}

/* Calls the program's own float-to-long helper (the code with the four x87
 * escapes) as the C runtime would: the value on the FPU stack, the far return
 * address on the guest stack. It must truncate toward zero and restore the
 * control word. */
static void run_ftol(const char *label, const char *env, const ftol_site *ft) {
    static const struct { long double in; int32_t out; } t[] = {
        {-123.9L, -123}, {123.9L, 123}, {0.5L, 0}, {-0.5L, 0}, {2147483647.0L, 2147483647},
        {-2147483648.0L, INT32_MIN}, {4294967297.5L, 1}};
    const char *path = getenv(env);
    ne_image ne;
    hle_t h;
    kernel_t *k;
    kernel_config cfg;
    loader_config lc;
    loader_proc *p;
    char *tmp;
    char host[512];
    printf("test_ftol_%s\n", label);
    if (!path || !*path) {
        printf("  SKIP (%s not set)\n", env);
        return;
    }
    CHECK_EQ(ne_open_file(&ne, path), NE_OK);
    tmp = hd_make();
    snprintf(host, sizeof host, "%s", tmp);
    memset(&cfg, 0, sizeof cfg);
    cfg.host_root['C' - 'A'] = host;
    hle_init(&h);
    h.stub_unknown = true;
    k = kernel_create(&cfg, &h, KERNEL_REG_USER_HELPERS);
    memset(&lc, 0, sizeof lc);
    lc.resolve = hle_resolve;
    lc.resolve_ctx = &h;
    lc.cmdline = "";
    CHECK_EQ(loader_create(&ne, &lc, &p), LOADER_OK);
    if (p && k && kernel_attach(k, p)) {
        cpu_t *c = &p->cpu;
        uint16_t cs = p->seg_sel[ft->seg - 1];
        size_t i;
        uint64_t hits[4] = {0, 0, 0, 0};
        CHECK(cpu_add_hook(c, cs, 0, halt_hook, NULL));
        for (i = 0; i < 4; i++) CHECK(cpu_add_hook(c, cs, ft->esc[i], count_hook, &hits[i]));
        for (i = 0; i < sizeof t / sizeof t[0]; i++) {
            uint32_t sp0 = c->r[CPU_ESP];
            fpu_init(&c->fpu);
            c->fpu.sw = 7u << FPU_SW_TOP_SHIFT;
            c->fpu.empty = 0x7F;
            c->fpu.st[7] = t[i].in;
            c->seg[CPU_CS] = cs;
            c->eip = ft->entry;
            CHECK_EQ(cpu_push16(c, cs), CPU_OK);
            CHECK_EQ(cpu_push16(c, 0), CPU_OK);
            CHECK_EQ(cpu_run(c, 1000), CPU_HALT);
            CHECK_EQ((uint32_t)((c->r[CPU_EDX] & 0xFFFF) << 16 | (c->r[CPU_EAX] & 0xFFFF)), (uint32_t)t[i].out);
            CHECK_EQ(c->fpu.cw, FPU_CW_DEFAULT);   /* control word restored */
            CHECK_EQ(fpu_depth(&c->fpu), 0);       /* FISTP popped the value */
            CHECK_EQ(c->r[CPU_ESP], sp0);          /* far return consumed */
        }
        for (i = 0; i < 4; i++) CHECK_EQ(hits[i], sizeof t / sizeof t[0]);   /* every escape ran each call */
    }
    loader_destroy(p);
    kernel_destroy(k);
    hle_destroy(&h);
    ne_free(&ne);
    hd_remove(host);
}

/* Every operating-system fixup site (the linker's FIARQQ-style floating point
 * fixups) keeps the bytes the linker wrote: WAIT + escape (type 5), with a CS: or
 * ES: override in between (types 3 and 4), or NOP + WAIT (type 6). The CPU must
 * decode each as a defined x87 instruction. */
static void check_osfixups(const char *label, const char *env) {
    const char *path = getenv(env);
    ne_image ne;
    hle_t h;
    loader_config lc;
    loader_proc *p;
    size_t i, by_type[8] = {0}, esc = 0;
    printf("test_osfixups_%s\n", label);
    if (!path || !*path) {
        printf("  SKIP (%s not set)\n", env);
        return;
    }
    CHECK_EQ(ne_open_file(&ne, path), NE_OK);
    hle_init(&h);
    memset(&lc, 0, sizeof lc);
    lc.resolve = hle_resolve;
    lc.resolve_ctx = &h;
    lc.cmdline = "";
    CHECK_EQ(loader_create(&ne, &lc, &p), LOADER_OK);
    for (i = 0; p && i < p->nosfixups; i++) {
        const loader_osfixup *f = &p->osfixups[i];
        uint16_t sel = p->seg_sel[f->seg];
        const uint8_t *b = mem_ptr(&p->mem, sel, f->offset, 4);
        cpu_insn in;
        unsigned k = 1;
        if (!b || f->type >= 8) { CHECK(0); continue; }
        by_type[f->type]++;
        if (f->type == 6) { CHECK_EQ(b[0], 0x90); CHECK_EQ(b[1], 0x9B); continue; }
        CHECK_EQ(b[0], 0x9B);
        if (f->type == 3 || f->type == 4) { CHECK_EQ(b[1], f->type == 3 ? 0x2E : 0x26); k = 2; }
        CHECK(b[k] >= 0xD8 && b[k] <= 0xDF);
        {
            fpu_op op;
            CHECK(fpu_decode(b[k] - 0xD8u, b[k + 1], &op));
            esc++;
        }
        CHECK_EQ(cpu_decode(&p->cpu.mem, sel, (uint32_t)f->offset + 1, &in), CPU_OK);
        CHECK(in.fpu && in.supported);
    }
    if (p)
        printf("  %zu OS fixups: types 3:%zu 4:%zu 5:%zu 6:%zu, %zu x87 escapes all defined\n", p->nosfixups,
               by_type[3], by_type[4], by_type[5], by_type[6], esc);
    loader_destroy(p);
    hle_destroy(&h);
    ne_free(&ne);
}


/* Third characterization: USER is real (user.c). Imports of other modules still
 * stop the run, except the stand-ins listed in `standin` (module, ordinal,
 * argument bytes), which return 0: they let the run go on to the next stop. */
typedef struct { const char *module; uint16_t ordinal; uint16_t arg_bytes; const char *name; } standin_def;

static cpu_status standin_fn(hle_call *c) {
    hle_ret32(c, 0);
    return CPU_OK;
}

static void run_user(const char *label, const char *env, const standin_def *si, size_t nsi, result *out) {
    const char *path = getenv(env), *cd = getenv("CWFA_CD_ROOT");
    ne_image ne;
    hle_t h;
    kernel_t *k;
    user_t *u;
    win87em_t *w87;
    kernel_config cfg;
    user_config uc;
    loader_config lc;
    loader_proc *p;
    char *tmp;
    char host[512];
    size_t i;
    printf("test_user_run_%s\n", label);
    memset(out, 0, sizeof *out);
    if (!path || !*path) {
        printf("  SKIP (%s not set)\n", env);
        return;
    }
    CHECK_EQ(ne_open_file(&ne, path), NE_OK);
    tmp = hd_make();
    snprintf(host, sizeof host, "%s", tmp);
    memset(&cfg, 0, sizeof cfg);
    cfg.host_root['C' - 'A'] = host;
    if (cd && *cd) cfg.host_root['D' - 'A'] = cd;
    cfg.exe_path = "C:\\CWFA\\PROGRAM.EXE";
    memset(&uc, 0, sizeof uc);
    uc.log = stdout;
    hle_init(&h);
    h.stub_unknown = true;
    h.record = true;
    k = kernel_create(&cfg, &h, 0);
    w87 = win87em_create(&h);
    u = user_create(&uc, &h);
    for (i = 0; i < nsi; i++) {
        hle_def d = {si[i].module, si[i].ordinal, si[i].name, si[i].arg_bytes, standin_fn};
        CHECK(hle_register(&h, &d, NULL));
    }
    CHECK(k && w87 && u);
    memset(&lc, 0, sizeof lc);
    lc.resolve = hle_resolve;
    lc.resolve_ctx = &h;
    lc.cmdline = "";
    CHECK_EQ(loader_create(&ne, &lc, &p), LOADER_OK);
    if (p && k && u) {
        CHECK(kernel_attach(k, p));
        CHECK(user_attach(u, p));
        out->stop = loader_run(p, 400000000ull);
        out->fault = p->fault;
        out->stop_cs = p->cpu.seg[CPU_CS];
        out->stop_ip = (uint16_t)p->cpu.eip;
        out->ncalls = h.nrec;
        if (h.nrec && !h.rec[h.nrec - 1].implemented) {
            const hle_record *r = &h.rec[h.nrec - 1];
            out->stopped_on_unimpl = true;
            snprintf(out->stop_module, sizeof out->stop_module, "%s", r->module);
            out->stop_ordinal = r->ordinal;
            snprintf(out->stop_name, sizeof out->stop_name, "%s", r->name);
        }
        printf("  %zu API calls, run: %s", out->ncalls, loader_strstop(out->stop));
        if (out->stop == LOADER_STOP_FAULT) printf(" (%s)", cpu_strstatus(out->fault));
        printf(" at %04x:%04x\n", out->stop_cs, out->stop_ip);
        collect_sequence(&h, p, out);
        printf("  USER stubs called: %zu\n", user_stub_count(u));
        for (i = 0; i < user_stub_count(u); i++) {
            const user_stub_rec *s = user_stub_at(u, i);
            printf("    USER.%u %s (%s) from %04x:%04x\n", s->ordinal, s->name, s->task, s->caller_cs, s->caller_ip);
        }
    }
    loader_destroy(p);
    user_destroy(u);
    win87em_destroy(w87);
    kernel_destroy(k);
    hle_destroy(&h);
    ne_free(&ne);
    hd_remove(host);
}

static const standin_def standins[] = {
    {"TOOLHELP", 63, 4, "TaskFirst"}, {"TOOLHELP", 64, 4, "TaskNext"}, {"GDI", 87, 2, "GetStockObject"}, {"GDI", 80, 4, "GetDeviceCaps"}};

typedef struct {
    const char *module;
    unsigned ordinal;
    unsigned seg;
    uint16_t off;
} want_rec;

static void expect(const result *r, const want_rec *w, size_t n, const char *stop_module,
                   unsigned stop_ordinal) {
    size_t i;
    CHECK_EQ(r->stop, LOADER_STOP_REQUESTED);
    CHECK(r->stopped_on_unimpl && !strcmp(r->stop_module, stop_module) && r->stop_ordinal == stop_ordinal);
    CHECK_EQ(r->ncalls, n);
    for (i = 0; i < n && i < r->ncalls; i++) {
        CHECK(!strcmp(r->seq[i].module, w[i].module) && r->seq[i].ordinal == w[i].ordinal);
        CHECK_EQ(r->seq[i].seg, w[i].seg);
        CHECK_EQ(r->seq[i].off, w[i].off);
        CHECK_EQ(r->seq[i].implemented, i + 1 < n);
    }
}

static const want_rec w2[] = {
    {"KERNEL", 91, 5, 0x0022}, {"KERNEL", 3, 5, 0x004d},
    {"KERNEL", 102, 5, 0x0060}, {"KERNEL", 30, 5, 0x007d},
    {"USER", 5, 5, 0x0086}, {"KERNEL", 102, 5, 0x0103},
    {"KERNEL", 102, 5, 0x0125}, {"WIN87EM", 1, 5, 0x0147}};
static const want_rec a2[] = {
    {"KERNEL", 91, 15, 0xd200}, {"KERNEL", 3, 15, 0xd22b},
    {"KERNEL", 102, 15, 0xd23e}, {"KERNEL", 30, 15, 0xd25b},
    {"USER", 5, 15, 0xd264}, {"KERNEL", 102, 15, 0xdb91},
    {"KERNEL", 102, 15, 0xdbb3}, {"WIN87EM", 1, 15, 0xdbd5}};
static const want_rec w3[] = {
    {"KERNEL", 91, 5, 0x0022}, {"KERNEL", 3, 5, 0x004d},
    {"KERNEL", 102, 5, 0x0060}, {"KERNEL", 30, 5, 0x007d},
    {"USER", 5, 5, 0x0086}, {"KERNEL", 102, 5, 0x0103},
    {"KERNEL", 102, 5, 0x0125}, {"WIN87EM", 1, 5, 0x0147},
    {"WIN87EM", 1, 5, 0x015a}, {"KERNEL", 5, 145, 0x113b},
    {"KERNEL", 49, 5, 0x02f5}, {"KERNEL", 131, 5, 0x045e},
    {"KERNEL", 5, 145, 0x113b}, {"KERNEL", 5, 145, 0x113b},
    {"KERNEL", 132, 127, 0x007e}, {"KERNEL", 132, 127, 0x008f},
    {"USER", 179, 127, 0x00a1}};
static const want_rec a3[] = {
    {"KERNEL", 91, 15, 0xd200}, {"KERNEL", 3, 15, 0xd22b},
    {"KERNEL", 102, 15, 0xd23e}, {"KERNEL", 30, 15, 0xd25b},
    {"USER", 5, 15, 0xd264}, {"KERNEL", 102, 15, 0xdb91},
    {"KERNEL", 102, 15, 0xdbb3}, {"WIN87EM", 1, 15, 0xdbd5},
    {"WIN87EM", 1, 15, 0xdbe8}, {"KERNEL", 23, 15, 0xf488},
    {"KERNEL", 5, 15, 0xf494}, {"KERNEL", 24, 15, 0xf4a0},
    {"KERNEL", 49, 15, 0xde3d}, {"KERNEL", 131, 15, 0xdd94},
    {"KERNEL", 23, 15, 0xf488}, {"KERNEL", 5, 15, 0xf494},
    {"KERNEL", 24, 15, 0xf4a0}, {"KERNEL", 23, 15, 0xf488},
    {"KERNEL", 5, 15, 0xf494}, {"KERNEL", 24, 15, 0xf4a0},
    {"KERNEL", 132, 3, 0x30d1}, {"KERNEL", 132, 3, 0x30e1},
    {"USER", 179, 3, 0x30f3}};

/* USER real, TOOLHELP.TaskFirst/TaskNext and GDI.GetStockObject/GetDeviceCaps answered with 0 by
 * stand-ins: both programs register their classes, create the 640x480 main window
 * (a visible WS_OVERLAPPEDWINDOW at 0,0), run window procedures that call DefWindowProc, take a
 * DC and then reach MMSYSTEM (WRITER) / GDI.313 (ARTIST). GDI.80 is GetDeviceCaps and GDI.87
 * GetStockObject (Win 3.1 export list, UNVERIFIED). Without the stand-ins the run stops at
 * TOOLHELP.63 (TaskFirst) at call 22 / 28. */
static const want_rec wu1[] = {
    {"KERNEL", 91, 5, 0x0022}, {"KERNEL", 3, 5, 0x004d}, {"KERNEL", 102, 5, 0x0060}, {"KERNEL", 30, 5, 0x007d},
    {"USER", 5, 5, 0x0086}, {"KERNEL", 102, 5, 0x0103}, {"KERNEL", 102, 5, 0x0125}, {"WIN87EM", 1, 5, 0x0147},
    {"WIN87EM", 1, 5, 0x015a}, {"KERNEL", 5, 145, 0x113b}, {"KERNEL", 49, 5, 0x02f5}, {"KERNEL", 131, 5, 0x045e},
    {"KERNEL", 5, 145, 0x113b}, {"KERNEL", 5, 145, 0x113b}, {"KERNEL", 132, 127, 0x007e},
    {"KERNEL", 132, 127, 0x008f}, {"USER", 179, 127, 0x00a1}, {"USER", 179, 127, 0x00b0},
    {"USER", 179, 127, 0x00bf}, {"KERNEL", 107, 127, 0x00d8}, {"KERNEL", 47, 127, 0x0272},
    {"TOOLHELP", 63, 127, 0x0299}, {"KERNEL", 15, 120, 0x0015}, {"KERNEL", 18, 119, 0x0250},
    {"KERNEL", 20, 119, 0x0264}, {"KERNEL", 55, 127, 0x00f8}, {"USER", 174, 128, 0x002b},
    {"USER", 173, 128, 0x003a}, {"GDI", 87, 128, 0x0044}, {"USER", 57, 128, 0x0067}, {"USER", 173, 128, 0x00a0},
    {"GDI", 87, 128, 0x00aa}, {"USER", 57, 128, 0x00c9}, {"KERNEL", 3, 128, 0x0106}, {"USER", 179, 128, 0x0137},
    {"USER", 179, 128, 0x013f}, {"USER", 41, 128, 0x0150}, {"USER", 107, 124, 0x05c0}, {"USER", 31, 124, 0x0127},
    {"USER", 179, 124, 0x0152}, {"USER", 179, 124, 0x015c}, {"USER", 107, 124, 0x05c0},
    {"USER", 107, 124, 0x05c0}, {"USER", 66, 16, 0x06cc}, {"GDI", 80, 16, 0x06de}, {"USER", 68, 16, 0x0710},
    {"USER", 66, 122, 0x00c7}, {"GDI", 80, 122, 0x00d8}, {"GDI", 80, 122, 0x00e2}, {"USER", 68, 122, 0x00ee},
    {"KERNEL", 60, 122, 0x006b}, {"KERNEL", 61, 122, 0x007c}, {"KERNEL", 62, 122, 0x0089},
    {"GDI", 360, 120, 0x012f}};
static const want_rec au1[] = {
    {"KERNEL", 91, 15, 0xd200}, {"KERNEL", 3, 15, 0xd22b}, {"KERNEL", 102, 15, 0xd23e},
    {"KERNEL", 30, 15, 0xd25b}, {"USER", 5, 15, 0xd264}, {"KERNEL", 102, 15, 0xdb91}, {"KERNEL", 102, 15, 0xdbb3},
    {"WIN87EM", 1, 15, 0xdbd5}, {"WIN87EM", 1, 15, 0xdbe8}, {"KERNEL", 23, 15, 0xf488}, {"KERNEL", 5, 15, 0xf494},
    {"KERNEL", 24, 15, 0xf4a0}, {"KERNEL", 49, 15, 0xde3d}, {"KERNEL", 131, 15, 0xdd94},
    {"KERNEL", 23, 15, 0xf488}, {"KERNEL", 5, 15, 0xf494}, {"KERNEL", 24, 15, 0xf4a0}, {"KERNEL", 23, 15, 0xf488},
    {"KERNEL", 5, 15, 0xf494}, {"KERNEL", 24, 15, 0xf4a0}, {"KERNEL", 132, 3, 0x30d1}, {"KERNEL", 132, 3, 0x30e1},
    {"USER", 179, 3, 0x30f3}, {"USER", 179, 3, 0x3102}, {"USER", 179, 3, 0x3111}, {"KERNEL", 107, 3, 0x312a},
    {"KERNEL", 47, 3, 0x3310}, {"TOOLHELP", 63, 3, 0x3337}, {"KERNEL", 15, 1, 0xf94f}, {"KERNEL", 18, 1, 0xf95e},
    {"KERNEL", 20, 1, 0xf972}, {"KERNEL", 55, 3, 0x314a}, {"USER", 174, 2, 0x2958}, {"USER", 173, 2, 0x2965},
    {"GDI", 87, 2, 0x296f}, {"USER", 57, 2, 0x2992}, {"GDI", 87, 2, 0x29c8}, {"USER", 57, 2, 0x29e7},
    {"USER", 173, 2, 0x2a20}, {"GDI", 87, 2, 0x2a2a}, {"USER", 57, 2, 0x2a47}, {"KERNEL", 3, 2, 0x2a68},
    {"USER", 179, 2, 0x2a99}, {"USER", 179, 2, 0x2aa1}, {"USER", 41, 2, 0x2ab0}, {"USER", 107, 3, 0x7bc4},
    {"USER", 31, 3, 0x76e1}, {"USER", 179, 3, 0x770c}, {"USER", 179, 3, 0x7716}, {"USER", 156, 3, 0x7744},
    {"USER", 413, 3, 0x7755}, {"USER", 413, 3, 0x7760}, {"USER", 413, 3, 0x776b}, {"USER", 413, 3, 0x7776},
    {"USER", 107, 3, 0x7bc4}, {"USER", 107, 3, 0x7bc4}, {"USER", 66, 14, 0x6acf}, {"GDI", 80, 14, 0x6ae1},
    {"USER", 68, 14, 0x6b14}, {"USER", 66, 3, 0x2de3}, {"GDI", 80, 3, 0x2df4}, {"GDI", 80, 3, 0x2dfe},
    {"USER", 68, 3, 0x2e0a}, {"KERNEL", 60, 3, 0x2d87}, {"KERNEL", 61, 3, 0x2d98}, {"KERNEL", 62, 3, 0x2da5},
    {"GDI", 360, 2, 0x00ab}};

int main(void) {
    result w, a;
    check_kernel_coverage("writer", "CWFA_WRITER_EXE");
    check_kernel_coverage("artist", "CWFA_ARTIST_EXE");
    /* WRITER: entry 5:001a. First pass: every non-KERNEL import stops the run. */
    static const want_rec w1[] = {{"KERNEL", 91, 5, 0x22}, {"KERNEL", 3, 5, 0x4d}, {"KERNEL", 102, 5, 0x60},
                                  {"KERNEL", 30, 5, 0x7d}, {"USER", 5, 5, 0x86}};
    /* ARTIST: its start-up code sits in segment 15. */
    static const want_rec a1[] = {{"KERNEL", 91, 15, 0xd200}, {"KERNEL", 3, 15, 0xd22b}, {"KERNEL", 102, 15, 0xd23e},
                                  {"KERNEL", 30, 15, 0xd25b}, {"USER", 5, 15, 0xd264}};
    run_program("writer", "CWFA_WRITER_EXE", 0, false, NULL, &w);
    if (w.ncalls) expect(&w, w1, sizeof w1 / sizeof w1[0], "USER", 5);
    run_program("artist", "CWFA_ARTIST_EXE", 0, false, NULL, &a);
    if (a.ncalls) expect(&a, a1, sizeof a1 / sizeof a1[0], "USER", 5);
    /* Second pass: USER.InitApp answered (KERNEL_REG_USER_HELPERS), the next stop is the emulator. */
    run_program("writer_initapp", "CWFA_WRITER_EXE", KERNEL_REG_USER_HELPERS, false, NULL, &w);
    if (w.ncalls) expect(&w, w2, sizeof w2 / sizeof w2[0], "WIN87EM", 1);
    run_program("artist_initapp", "CWFA_ARTIST_EXE", KERNEL_REG_USER_HELPERS, false, NULL, &a);
    if (a.ncalls) expect(&a, a2, sizeof a2 / sizeof a2[0], "WIN87EM", 1);
    /* Third pass: with the real WIN87EM HLE (win87em.c) and the x87 hooks counting. */
    run_program("writer_win87em", "CWFA_WRITER_EXE", KERNEL_REG_USER_HELPERS, true, &writer_ftol, &w);
    if (w.ncalls) expect(&w, w3, sizeof w3 / sizeof w3[0], "USER", 179);
    run_program("artist_win87em", "CWFA_ARTIST_EXE", KERNEL_REG_USER_HELPERS, true, &artist_ftol, &a);
    if (a.ncalls) expect(&a, a3, sizeof a3 / sizeof a3[0], "USER", 179);
    run_user("writer", "CWFA_WRITER_EXE", NULL, 0, &w);
    if (w.ncalls) expect(&w, wu1, 22, "TOOLHELP", 63);
    run_user("artist", "CWFA_ARTIST_EXE", NULL, 0, &a);
    if (a.ncalls) expect(&a, au1, 28, "TOOLHELP", 63);
    run_user("writer_standins", "CWFA_WRITER_EXE", standins, sizeof standins / sizeof standins[0], &w);
    if (w.ncalls) expect(&w, wu1, sizeof wu1 / sizeof wu1[0], "GDI", 360);
    run_user("artist_standins", "CWFA_ARTIST_EXE", standins, sizeof standins / sizeof standins[0], &a);
    if (a.ncalls) expect(&a, au1, sizeof au1 / sizeof au1[0], "GDI", 360);
    check_osfixups("writer", "CWFA_WRITER_EXE");
    check_osfixups("artist", "CWFA_ARTIST_EXE");
    run_ftol("writer", "CWFA_WRITER_EXE", &writer_ftol);
    run_ftol("artist", "CWFA_ARTIST_EXE", &artist_ftol);
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
