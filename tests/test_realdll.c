/* The real libraries on the CD (skipped unless CWFA_CD_ROOT is set): KIDSWTCH.DLL and the two .SND resource
 * libraries load through LoadLibrary, run their entry points on the interpreter, export what nedump lists,
 * and every import of KIDSWTCH has a handler. */
#define _POSIX_C_SOURCE 200809L
#include "fixture.h"
#include "hostdir.h"
#include "kernel.h"
#include "toolhelp.h"
#include "user.h"
#include "ver.h"

static hle_t h;
static kernel_t *k;
static user_t *u;
static toolhelp_t *th;
static ver_t *ver;
static fx_t f;
static char *g_c;

static bool boot(const char *cd) {
    kernel_config cfg;
    user_config uc;
    loader_config lc;
    memset(&cfg, 0, sizeof cfg);
    memset(&uc, 0, sizeof uc);
    memset(&lc, 0, sizeof lc);
    g_c = hd_make();
    hd_mkdir(g_c, "WINDOWS");
    cfg.host_root['C' - 'A'] = g_c;
    cfg.host_root['D' - 'A'] = cd;
    cfg.exe_path = "C:\\MSKIDS\\WRITER.EXE";
    cfg.module_path = "D:\\SETUP";
    hle_init(&h);
    h.stub_unknown = true;
    k = kernel_create(&cfg, &h, 0);
    u = user_create(&uc, &h);
    th = k ? toolhelp_create(&h, k) : NULL;
    ver = k ? ver_create(&h, k) : NULL;
    CHECK(k && u && th && ver);
    lc.resolve = hle_resolve;
    lc.resolve_ctx = &h;
    lc.open_module = kernel_open_module;
    lc.open_ctx = k;
    fx_init(&f, 0x800, 0x1000);
    if (!k || fx_load(&f, &lc) != LOADER_OK) return false;
    CHECK(kernel_attach(k, f.p) && user_attach(u, f.p) && toolhelp_attach(th, f.p));
    return true;
}

static void down(void) {
    fx_free(&f);
    ver_destroy(ver);
    toolhelp_destroy(th);
    user_destroy(u);
    kernel_destroy(k);
    hle_destroy(&h);
    hd_remove(g_c);
}

static uint16_t seg_of(uint32_t size) { return mem_alloc(&f.p->mem, size, MEM_DATA); }
static uint16_t str_seg(const char *s) {
    uint16_t sel = seg_of(0x200);
    strcpy((char *)mem_ptr(&f.p->mem, sel, 0, 1), s);
    return sel;
}

static uint16_t word_at(uint16_t sel, unsigned off) {
    const uint8_t *d = mem_ptr(&f.p->mem, sel, off, 2);
    return d ? (uint16_t)(d[0] | d[1] << 8) : 0;
}

/* FindResource(hm, name or id, "SND") */
static uint16_t find_snd(uint16_t hm, const ne_resource *r) {
    uint16_t type = str_seg("SND");
    if (r->name[0]) CHECK_EQ(FX_CALL(&f, "KERNEL", 60, hm, FARP(str_seg(r->name), 0), FARP(type, 0)), LOADER_STOP_HLT);
    else CHECK_EQ(FX_CALL(&f, "KERNEL", 60, hm, FARP(0, r->id), FARP(type, 0)), LOADER_STOP_HLT);
    return fx_ax(&f);
}

static uint16_t load_library(const char *name) {
    CHECK_EQ(FX_CALL(&f, "KERNEL", 95, FARP(str_seg(name), 0)), LOADER_STOP_HLT);
    return fx_ax(&f);
}

static void test_kidswtch(void) {
    static const char *const names[] = {
        "FCONNECTSWITCHER", "DISCONNECTSWITCHER", "FCONNECTKIDSAPP", "DISCONNECTKIDSAPP", "FGETFSPLOGIN",
        "FSETFSPLOGIN", "FADDFSPAPP", "FADDFSPUSER", "FREADAPPLIST", "FREADUSERLIST", "DISPOSELIST", "CFSP",
        "FGETFSP", "FSWITCHTOFSPAPP", "FDRAWFSPAPPICON", "GETFSPSTZNAME", "SETFSPSTZNAME", "CLEARFSPPATH", "FFSPEQUAL"};
    uint16_t hm, sel, off;
    loader_module *m;
    ne_import *imp = NULL;
    size_t n = 0, i, missing = 0, by_module[6] = {0};
    unsigned unimpl = 0;
    static const char *const mods[] = {"TOOLHELP", "VER", "SHELL", "KERNEL", "USER", "other"};
    char name[NE_NAME_MAX + 1];
    printf("test_realdll_kidswtch\n");
    if (!boot(getenv("CWFA_CD_ROOT"))) {
        down();
        return;
    }
    hm = load_library("D:\\SETUP\\KIDSWTCH.DLL");
    CHECK(hm >= 32);
    m = loader_module_by_handle(f.p, hm);
    CHECK(m != NULL);
    if (!m) {
        down();
        return;
    }
    CHECK(!strcmp(m->name, "KIDSWTCH"));
    CHECK(m->inited);                                         /* the entry point (LIBENTRY: LocalInit, LibMain) ran */
    CHECK(m->is_dll);
    CHECK_EQ(m->usage, 1);
    CHECK_EQ(m->heap_size, 0x400);
    CHECK_EQ(m->ne->entry_count, 20);
    CHECK(word_at(m->auto_ds, 6) != 0);                       /* pLocalHeap: LocalInit ran */
    for (i = 0; i < sizeof names / sizeof *names; i++) {
        g_checks++;
        if (!loader_module_proc(m, names[i], 0, &sel, &off)) {
            g_fails++;
            printf("  FAIL no export %s\n", names[i]);
        }
    }
    CHECK(loader_module_proc(m, "___EXPORTEDSTUB", 0, &sel, &off));   /* ARTIST imports this one by name */
    CHECK(loader_module_proc(m, "fconnectswitcher", 0, &sel, &off));  /* any case */
    CHECK(loader_module_proc(m, NULL, 1, &sel, &off));                /* FCONNECTSWITCHER */
    CHECK_EQ(off, 0x0010);
    CHECK_EQ(sel, m->seg_sel[0]);
    CHECK(loader_module_proc(m, NULL, 5, &sel, &off));                /* FGETFSPLOGIN: where WRITER's run once stopped */
    CHECK_EQ(off, 0x01ea);
    CHECK(!loader_module_proc(m, NULL, 21, &sel, &off));
    /* its imports: every one has a handler or is listed */
    CHECK_EQ(ne_collect_imports(m->ne, &imp, &n), NE_OK);
    for (i = 0; i < n; i++) {
        const hle_entry *e;
        size_t mi;
        if (ne_modref_name(m->ne, imp[i].modref, name) != NE_OK) continue;
        for (mi = 0; mi < 5 && strcmp(mods[mi], name); mi++) {}
        by_module[mi]++;
        e = hle_find(&h, name, imp[i].ordinal);
        if (!e || (!e->fn && !e->is_const)) {
            missing++;
            printf("    %s.%u has no handler\n", name, imp[i].ordinal);
            if (!strcmp(name, "KERNEL") && imp[i].ordinal == 166) unimpl |= 1;
            if (!strcmp(name, "USER") && imp[i].ordinal == 224) unimpl |= 2;
            if (!strcmp(name, "SHELL") && imp[i].ordinal == 34) unimpl |= 4;
        }
    }
    printf("  imports: %zu (TOOLHELP %zu, VER %zu, SHELL %zu, KERNEL %zu, USER %zu), %zu without a handler\n", n,
           by_module[0], by_module[1], by_module[2], by_module[3], by_module[4], missing);
    CHECK_EQ(n, 49);
    CHECK_EQ(by_module[5], 0);
    CHECK_EQ(by_module[0], 5);
    CHECK_EQ(by_module[1], 3);
    CHECK_EQ(by_module[2], 1);
    CHECK_EQ(by_module[3], 27);
    CHECK_EQ(by_module[4], 13);
    /* Every import has a handler: KERNEL.166 is WinExec (the call site passes the command line and SW_SHOW and
     * compares the result with 0x20) and USER.224 GetWindowTask (compared with the TASKENTRY task); SHELL.34 is
     * ExtractIcon. */
    CHECK_EQ(missing, 0);
    CHECK_EQ(unimpl, 0);
    ne_free_imports(imp);
    CHECK_EQ(FX_CALL(&f, "KERNEL", 96, hm), LOADER_STOP_HLT);
    CHECK_EQ(f.p->nmods, 1);
    down();
}

/* One SND library: loads, its entry point ran LocalInit, its resources read back as the file holds them. */
static void snd(const char *file, unsigned snd_count) {
    char path[64];
    uint16_t hm, hr, hg;
    loader_module *m;
    const ne_restype *t = NULL, *range = NULL;
    const ne_resource *r;
    size_t i;
    uint32_t p;
    printf("test_realdll_%s\n", file);
    if (!boot(getenv("CWFA_CD_ROOT"))) {
        down();
        return;
    }
    snprintf(path, sizeof path, "D:\\SETUP\\%s", file);
    hm = load_library(path);
    CHECK(hm >= 32);
    m = loader_module_by_handle(f.p, hm);
    CHECK(m != NULL);
    if (!m) {
        down();
        return;
    }
    CHECK(!strcmp(m->name, "SOUNDDLL"));                      /* both files carry this module name */
    CHECK(!strncmp(m->file, file, 6));
    CHECK_EQ(m->ne->entry_count, 0);                          /* nothing exported: resources only */
    CHECK_EQ(m->heap_size, 0x1000);
    CHECK(m->inited);
    CHECK(word_at(m->auto_ds, 6) != 0);
    {   /* the entry point's LocalInit made a heap in the library's own data segment */
        uint16_t save = f.p->cpu.seg[CPU_DS];
        f.p->cpu.seg[CPU_DS] = m->auto_ds;
        CHECK_EQ(FX_CALL(&f, "KERNEL", 5, 0, 100), LOADER_STOP_HLT);        /* LocalAlloc(LMEM_FIXED, 100) */
        f.p->cpu.seg[CPU_DS] = save;
        CHECK(fx_ax(&f) >= m->heap_start && fx_ax(&f) < m->heap_start + m->heap_size);
    }
    for (i = 0; i < m->ne->restype_count; i++) {
        if (!strcmp(m->ne->restypes[i].name, "SND")) t = &m->ne->restypes[i];
        if (!strcmp(m->ne->restypes[i].name, "SNDRANGE")) range = &m->ne->restypes[i];
    }
    CHECK(t && range);
    if (t && range) {
        CHECK_EQ(t->count, snd_count);
        CHECK_EQ(range->count, 1);
        r = &t->res[0];
        hr = find_snd(hm, r);
        CHECK(hr != 0);
        CHECK_EQ(FX_CALL(&f, "KERNEL", 65, hm, hr), LOADER_STOP_HLT);        /* SizeofResource */
        CHECK_EQ(fx_dxax(&f), r->length);
        CHECK_EQ(FX_CALL(&f, "KERNEL", 61, hm, hr), LOADER_STOP_HLT);        /* LoadResource */
        hg = fx_ax(&f);
        CHECK(hg != 0);
        CHECK_EQ(FX_CALL(&f, "KERNEL", 62, hg), LOADER_STOP_HLT);            /* LockResource */
        p = fx_dxax(&f);
        CHECK(p != 0);
        CHECK(!memcmp(mem_ptr(&f.p->mem, (uint16_t)(p >> 16), (uint16_t)p, 16), m->ne->data + r->offset, 16));
        CHECK(r->length > 1000);                                              /* sound data, not a stub */
        r = &t->res[t->count - 1];                                            /* the last one too */
        CHECK(find_snd(hm, r) != 0);
    }
    CHECK_EQ(FX_CALL(&f, "KERNEL", 96, hm), LOADER_STOP_HLT);
    CHECK_EQ(f.p->nmods, 1);
    down();
}

int main(void) {
    const char *cd = getenv("CWFA_CD_ROOT");
    if (!cd || !*cd) {
        printf("test_realdll\n  SKIP (CWFA_CD_ROOT not set)\n");
        printf("%d checks, %d failures\n", g_checks, g_fails);
        return 0;
    }
    test_kidswtch();
    snd("WRITER.SND", 127);
    snd("ARTIST.SND", 137);
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
