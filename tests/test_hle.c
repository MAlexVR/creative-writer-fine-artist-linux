#include "fixture.h"
#include "hle.h"

static hle_t g_h;
static fx_t g_f;

static void setup(bool stub_unknown) {
    loader_config cfg;
    memset(&cfg, 0, sizeof cfg);
    hle_init(&g_h);
    g_h.stub_unknown = stub_unknown;
    cfg.resolve = hle_resolve;
    cfg.resolve_ctx = &g_h;
    fx_init(&g_f, 0x400, 0x100);
    (void)cfg;
}

static void load(void) {
    loader_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.resolve = hle_resolve;
    cfg.resolve_ctx = &g_h;
    fx_load(&g_f, &cfg);
}

static void teardown(void) {
    fx_free(&g_f);
    hle_destroy(&g_h);
}

/* ---- handlers --------------------------------------------------------- */

static cpu_status h_add(hle_call *c) {
    hle_args a;
    uint16_t x, y;
    hle_args_init(&a, c);
    x = hle_u16(&a);
    y = hle_u16(&a);
    if (a.st) return a.st;
    hle_ret16(c, (uint16_t)(x + y));
    return CPU_OK;
}

static uint16_t g_w, g_sel, g_off;
static uint32_t g_long;
static cpu_status h_mixed(hle_call *c) {
    hle_args a;
    hle_args_init(&a, c);
    g_w = hle_u16(&a);
    hle_far(&a, &g_sel, &g_off);
    g_long = hle_u32(&a);
    if (a.st) return a.st;
    hle_ret32(c, g_long + g_w);
    return CPU_OK;
}

static cpu_status h_overread(hle_call *c) {
    hle_args a;
    hle_args_init(&a, c);
    hle_u16(&a);
    hle_u16(&a); /* only 2 bytes declared */
    return a.st;
}

static int g_ctx_hits;
static cpu_status h_ctx(hle_call *c) {
    (*(int *)c->ctx)++;
    return CPU_OK;
}

static void register_all(void) {
    static const hle_def defs[] = {
        {"KERNEL", 200, "Add", 4, h_add},
        {"KERNEL", 201, "Mixed", 10, h_mixed},
        {"KERNEL", 202, "Overread", 2, h_overread},
        {"KERNEL", 300, "Mystery", 0, NULL},
        {"USER", 5, NULL, 0, h_ctx},
    };
    CHECK(hle_register_table(&g_h, defs, sizeof defs / sizeof defs[0], &g_ctx_hits));
}

/* ---- tests ------------------------------------------------------------ */

static void test_dispatch_and_pascal_cleanup(void) {
    printf("test_dispatch_and_pascal_cleanup\n");
    setup(true);
    register_all();
    load();
    CHECK_EQ(FX_CALL(&g_f, "KERNEL", 200, 7, 5), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&g_f), 12);
    CHECK_EQ(g_f.sp_after, g_f.sp_before); /* RETF 4 consumed the two pushed words */
    CHECK_EQ(FX_CALL(&g_f, "USER", 5), LOADER_STOP_HLT);
    CHECK_EQ(g_ctx_hits, 1); /* per-registration context reaches the handler */
    CHECK_EQ(g_f.sp_after, g_f.sp_before);
    CHECK_EQ(hle_find(&g_h, "kernel", 200)->calls, 1);
    teardown();
}

static void test_args_in_declaration_order(void) {
    printf("test_args_in_declaration_order\n");
    setup(true);
    register_all();
    load();
    CHECK_EQ(FX_CALL(&g_f, "KERNEL", 201, 0x0011, FARP(0x1234, 0x5678), LONG(0xAABBCCDDu)), LOADER_STOP_HLT);
    CHECK_EQ(g_w, 0x11);
    CHECK_EQ(g_sel, 0x1234);
    CHECK_EQ(g_off, 0x5678);
    CHECK_EQ(g_long, 0xAABBCCDDu);
    CHECK_EQ(fx_dxax(&g_f), 0xAABBCCEEu); /* DX:AX return */
    CHECK_EQ(g_f.sp_after, g_f.sp_before);
    /* reading more than declared is reported, not silently garbage */
    CHECK_EQ(FX_CALL(&g_f, "KERNEL", 202, 1, 2), LOADER_STOP_FAULT);
    CHECK_EQ(g_f.p->fault, CPU_E_UD);
    teardown();
}

static void test_unimplemented_logging(void) {
    char buf[256];
    size_t n;
    printf("test_unimplemented_logging\n");
    setup(true);
    g_h.log = tmpfile();
    register_all();
    load();
    CHECK_EQ(FX_CALL(&g_f, "KERNEL", 300, 1, 2, 3), LOADER_STOP_REQUESTED);
    CHECK_EQ(g_h.unimpl_calls, 1);
    CHECK(g_h.last_unimpl.entry != NULL && !strcmp(g_h.last_unimpl.name, "Mystery"));
    CHECK(!strcmp(g_h.last_unimpl.module, "KERNEL") && g_h.last_unimpl.ordinal == 300);
    CHECK_EQ(g_h.last_unimpl.caller_cs, g_f.p->seg_sel[0]);
    CHECK_EQ(g_h.last_unimpl.caller_ip, FX_CALL_IP + 3 * 3 + 5);
    rewind(g_h.log);
    n = fread(buf, 1, sizeof buf - 1, g_h.log);
    buf[n] = 0;
    CHECK(strstr(buf, "unimplemented KERNEL.300 Mystery called from ") != NULL);
    /* an ordinal the table has never heard of */
    CHECK_EQ(FX_CALL(&g_f, "KERNEL", 999), LOADER_STOP_REQUESTED);
    CHECK(g_h.last_unimpl.entry == NULL && g_h.last_unimpl.ordinal == 999);
    CHECK_EQ(g_h.unimpl_calls, 2);
    fclose(g_h.log);
    g_h.log = NULL;
    teardown();
}

static void test_unknown_without_stub_stays_unresolved(void) {
    printf("test_unknown_without_stub_stays_unresolved\n");
    setup(false);
    register_all();
    load();
    CHECK_EQ(FX_CALL(&g_f, "GDI", 1), LOADER_STOP_UNRESOLVED);
    CHECK_EQ(g_f.p->unresolved, 1);
    teardown();
}

static void test_registry(void) {
    hle_def d = {"KERNEL", 200, "Dup", 0, h_add};
    printf("test_registry\n");
    setup(true);
    register_all();
    CHECK(!hle_register(&g_h, &d, NULL)); /* (module, ordinal) is the key */
    d.module = "kernel";
    CHECK(!hle_register(&g_h, &d, NULL)); /* module names compare case-insensitively */
    d.ordinal = 201;
    CHECK(!hle_register(&g_h, &d, NULL));
    CHECK(hle_find(&g_h, "KERNEL", 200) && !strcmp(hle_find(&g_h, "KERNEL", 200)->name, "Add"));
    CHECK(hle_find(&g_h, "USER", 200) == NULL);
    CHECK(hle_find_name(&g_h, "KERNEL", "ADD") == hle_find(&g_h, "KERNEL", 200));
    CHECK(hle_find_name(&g_h, "USER", "Add") == NULL);
    CHECK(hle_find_name(&g_h, "KERNEL", "") == NULL);
    teardown();
}

static void test_import_by_name(void) {
    loader_import imp;
    loader_thunk *t;
    printf("test_import_by_name\n");
    setup(true);
    register_all();
    load();
    memset(&imp, 0, sizeof imp);
    strcpy(imp.module, "KERNEL");
    imp.by_name = true;
    strcpy(imp.name, "add");
    CHECK_EQ(loader_thunk_for(g_f.p, &imp, &t), LOADER_OK);
    CHECK(t->resolved && t->api.arg_bytes == 4);
    strcpy(imp.name, "Nonexistent");
    CHECK_EQ(loader_thunk_for(g_f.p, &imp, &t), LOADER_OK);
    CHECK(t->resolved); /* bound to the logging stub */
    teardown();
}

static void test_constants(void) {
    loader_import imp;
    loader_thunk *t;
    printf("test_constants\n");
    setup(true);
    CHECK(hle_register_const(&g_h, "KERNEL", 114, "__AHINCR", 8, 0));
    CHECK(hle_register_const(&g_h, "KERNEL", 195, "__C000H", 0, 0x100));
    load();
    memset(&imp, 0, sizeof imp);
    strcpy(imp.module, "KERNEL");
    imp.ordinal = 114;
    CHECK_EQ(loader_thunk_for(g_f.p, &imp, &t), LOADER_OK);
    CHECK(t->api.is_const && t->off == 8);
    imp.ordinal = 195;
    CHECK_EQ(loader_thunk_for(g_f.p, &imp, &t), LOADER_OK);
    CHECK(t->api.is_const && mem_size(&g_f.p->mem, t->sel) == 0x100);
    teardown();
}

static void test_record_and_trace(void) {
    char buf[512];
    size_t n;
    printf("test_record_and_trace\n");
    setup(true);
    g_h.record = true;
    g_h.trace = true;
    g_h.log = tmpfile();
    register_all();
    load();
    FX_CALL(&g_f, "KERNEL", 200, 1, 2);
    FX_CALL(&g_f, "KERNEL", 300);
    CHECK_EQ(g_h.nrec, 2);
    CHECK(g_h.rec[0].implemented && !strcmp(g_h.rec[0].name, "Add"));
    CHECK(!g_h.rec[1].implemented && g_h.rec[1].ordinal == 300);
    rewind(g_h.log);
    n = fread(buf, 1, sizeof buf - 1, g_h.log);
    buf[n] = 0;
    CHECK(strstr(buf, "hle: KERNEL.200 Add called from ") != NULL);
    CHECK(strstr(buf, "hle: unimplemented KERNEL.300 Mystery") != NULL);
    fclose(g_h.log);
    g_h.log = NULL;
    teardown();
}

/* --trace-args: with the trace on, each call also logs its argument words in the order they were pushed (the
 * first argument first), so a call site can be read without a disassembler. */
static void test_trace_args(void) {
    char buf[512];
    size_t n;
    printf("test_trace_args\n");
    setup(true);
    g_h.trace = true;
    g_h.trace_args = true;
    g_h.log = tmpfile();
    register_all();
    load();
    FX_CALL(&g_f, "KERNEL", 200, 1, 2);
    rewind(g_h.log);
    n = fread(buf, 1, sizeof buf - 1, g_h.log);
    buf[n] = 0;
    CHECK(strstr(buf, "hle: KERNEL.200 Add called from ") != NULL);
    CHECK(strstr(buf, "hle:   args(4): 0001 0002\n") != NULL);
    fclose(g_h.log);
    g_h.log = NULL;
    teardown();
}

/* The last HLE_RING calls are always kept (even with record off) for diagnostics: what the program did last. */
static void test_recent_ring(void) {
    hle_record r[HLE_RING + 8];
    size_t n;
    int i;
    printf("test_recent_ring\n");
    setup(true);
    register_all();
    load();
    CHECK_EQ(hle_recent(&g_h, r, 4), 0);
    for (i = 0; i < 300; i++) FX_CALL(&g_f, "KERNEL", 200, 1, 2);
    FX_CALL(&g_f, "KERNEL", 300);
    CHECK_EQ(g_h.ncalls, 301);
    n = hle_recent(&g_h, r, HLE_RING + 8);
    CHECK_EQ(n, HLE_RING);
    CHECK(!r[n - 1].implemented && r[n - 1].ordinal == 300);            /* newest last */
    CHECK(r[n - 2].implemented && !strcmp(r[n - 2].name, "Add"));
    n = hle_recent(&g_h, r, 3);
    CHECK_EQ(n, 3);
    CHECK(r[2].ordinal == 300 && r[0].ordinal == 200);
    teardown();
}

static void test_guest_memory_helpers(void) {
    hle_call c;
    char s[8];
    uint16_t sel, off16;
    uint32_t off;
    uint8_t *p;
    printf("test_guest_memory_helpers\n");
    setup(true);
    load();
    memset(&c, 0, sizeof c);
    c.proc = g_f.p;
    c.cpu = &g_f.p->cpu;
    sel = mem_alloc(&g_f.p->mem, 0x20, MEM_DATA);
    p = hle_ptr(&c, sel, 0, 0x20);
    CHECK(p != NULL && hle_ptr(&c, sel, 0x1f, 2) == NULL);
    memcpy(p, "hello\0", 6);
    CHECK(hle_read_cstr(&c, sel, 0, s, sizeof s) && !strcmp(s, "hello"));
    CHECK(hle_read_cstr(&c, sel, 0, s, 3) && !strcmp(s, "he")); /* truncated */
    memset(p, 'x', 0x20); /* no terminator inside the segment: refused */
    CHECK(!hle_read_cstr(&c, sel, 0, s, sizeof s));
    CHECK(!hle_read_cstr(&c, 0x1234, 0, s, sizeof s)); /* bad selector */
    sel = mem_alloc_huge(&g_f.p->mem, 0x30000, MEM_DATA);
    off = 0xFFF0;
    hle_huge_add(&sel, &off, 0x20);
    CHECK_EQ(off, 0x10);
    CHECK_EQ(sel, mem_huge_sel(sel - MEM_AHINCR, 1));
    (void)off16;
    teardown();
}

int main(void) {
    test_dispatch_and_pascal_cleanup();
    test_args_in_declaration_order();
    test_unimplemented_logging();
    test_unknown_without_stub_stays_unresolved();
    test_registry();
    test_import_by_name();
    test_constants();
    test_record_and_trace();
    test_trace_args();
    test_recent_ring();
    test_guest_memory_helpers();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
