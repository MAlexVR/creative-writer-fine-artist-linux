#include "fixture.h"
#include "hle.h"
#include "win87em.h"

static hle_t g_h;
static fx_t g_f;
static win87em_t *g_w;

static void boot(void) {
    loader_config cfg;
    memset(&cfg, 0, sizeof cfg);
    hle_init(&g_h);
    g_h.stub_unknown = true;
    g_w = win87em_create(&g_h);
    CHECK(g_w != NULL);
    cfg.resolve = hle_resolve;
    cfg.resolve_ctx = &g_h;
    fx_init(&g_f, 0x400, 0x100);
    fx_load(&g_f, &cfg);
}

static void down(void) {
    fx_free(&g_f);
    win87em_destroy(g_w);
    hle_destroy(&g_h);
}

static void test_registration(void) {
    const hle_entry *e;
    printf("test_registration\n");
    boot();
    e = hle_find(&g_h, "WIN87EM", 1);
    CHECK(e != NULL);
    if (e) { CHECK(e->fn != NULL); CHECK_EQ(e->arg_bytes, 0); CHECK(!strcmp(e->name, "__FPMATH")); }
    down();
}

static void test_commands(void) {
    cpu_t *c;
    printf("test_commands\n");
    boot();
    c = &g_f.p->cpu;
    /* init: FINIT and CF clear, no stack arguments */
    c->fpu.cw = 0x1234; c->fpu.sw = 0x3800; c->fpu.empty = 0;
    c->eflags |= CPU_CF;
    c->r[CPU_EBX] = 0;
    CHECK_EQ(FX_CALL(&g_f, "WIN87EM", 1), LOADER_STOP_HLT);
    CHECK_EQ(c->eflags & CPU_CF, 0);
    CHECK_EQ(c->fpu.cw, 0x037F); CHECK_EQ(c->fpu.sw, 0); CHECK_EQ(c->fpu.empty, 0xFF);
    CHECK_EQ(g_f.sp_before, g_f.sp_after);
    CHECK_EQ(g_w->calls, 1); CHECK_EQ(g_w->last_cmd, 0);
    /* command 3 records the far pointer in DX:AX */
    c->eflags |= CPU_CF;
    c->r[CPU_EBX] = 3; c->r[CPU_EAX] = 0x5678; c->r[CPU_EDX] = 0x1234;
    CHECK_EQ(FX_CALL(&g_f, "WIN87EM", 1), LOADER_STOP_HLT);
    CHECK_EQ(c->eflags & CPU_CF, 0);
    CHECK_EQ(g_w->trap_sel, 0x1234); CHECK_EQ(g_w->trap_off, 0x5678); CHECK_EQ(g_w->last_cmd, 3);
    /* unknown commands succeed and leave the FPU alone */
    c->fpu.cw = 0x0F7F;
    c->eflags |= CPU_CF;
    c->r[CPU_EBX] = 9;
    CHECK_EQ(FX_CALL(&g_f, "WIN87EM", 1), LOADER_STOP_HLT);
    CHECK_EQ(c->eflags & CPU_CF, 0); CHECK_EQ(c->fpu.cw, 0x0F7F); CHECK_EQ(g_w->calls, 3);
    down();
}

int main(void) {
    test_registration();
    test_commands();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
