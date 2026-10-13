/* KERNEL module services over real library files: LoadLibrary and the module
 * search path, reference counts, GetModuleHandle/FileName/Usage, GetProcAddress
 * and a far call through the returned pointer, LocalInit in a library, and the
 * resources of a library instance. The libraries are written to a temporary C:
 * and D: as files, so the whole path (dospath, ne_open_file, loader) runs. */
#define _POSIX_C_SOURCE 200809L
#include "dllfix.h"
#include "fixture.h"
#include "hostdir.h"
#include "kernel.h"

static hle_t kh;
static kernel_t *kk;
static fx_t kf;
static char *g_root;
static char g_cd[600];

static void boot(const char *module_path) {
    kernel_config cfg;
    loader_config lc;
    static char cdroot[600];
    memset(&cfg, 0, sizeof cfg);
    memset(&lc, 0, sizeof lc);
    g_root = hd_make();
    snprintf(cdroot, sizeof cdroot, "%s/cd", g_root);
    snprintf(g_cd, sizeof g_cd, "%s", cdroot);
    hd_mkdir(g_root, "WINDOWS");
    hd_mkdir(g_root, "WINDOWS/SYSTEM");
    hd_mkdir(g_root, "PROG");
    hd_mkdir(g_root, "cd");
    hd_mkdir(g_root, "cd/SETUP");
    cfg.host_root['C' - 'A'] = g_root;
    cfg.host_root['D' - 'A'] = cdroot;
    cfg.exe_path = "C:\\PROG\\TEST.EXE";
    cfg.module_path = module_path;
    cfg.debug = getenv("KDBG") ? stderr : NULL;
    hle_init(&kh);
    kh.stub_unknown = true;
    kk = kernel_create(&cfg, &kh, 0);
    lc.resolve = hle_resolve;
    lc.resolve_ctx = &kh;
    lc.open_module = kernel_open_module;
    lc.open_ctx = kk;
    fx_init(&kf, 0x800, 0x1000);
    CHECK(kk != NULL);
    if (fx_load(&kf, &lc) != LOADER_OK) return;
    CHECK(kernel_attach(kk, kf.p));
}

static void down(void) {
    fx_free(&kf);
    kernel_destroy(kk);
    hle_destroy(&kh);
    kk = NULL;
    hd_remove(g_root);
}

static uint16_t seg_of(uint32_t size) { return mem_alloc(&kf.p->mem, size, MEM_DATA); }
static uint8_t *mem_at(uint16_t sel, uint32_t off, uint32_t len) { return mem_ptr(&kf.p->mem, sel, off, len); }

static uint16_t str_seg(const char *s) {
    uint16_t sel = seg_of(0x400);
    strcpy((char *)mem_at(sel, 0, 1), s);
    return sel;
}

static void put_image(const char *rel, const nb_t *t) {
    static uint8_t buf[NB_MAX];
    size_t n = nb_build(buf, t);
    hd_write(g_root, rel, buf, n);
}

static uint16_t load_library(const char *name) {
    uint16_t s = str_seg(name);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 95, FARP(s, 0)), LOADER_STOP_HLT);
    return fx_ax(&kf);
}

static void free_library(uint16_t h) {
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 96, h), LOADER_STOP_HLT);
}

static uint16_t module_handle(const char *name) {
    uint16_t s = str_seg(name);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 47, FARP(s, 0)), LOADER_STOP_HLT);
    return fx_ax(&kf);
}

static uint16_t module_usage(uint16_t h) {
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 48, h), LOADER_STOP_HLT);
    return fx_ax(&kf);
}

static const char *module_file(uint16_t h) {
    static char out[160];
    uint16_t s = seg_of(0x100);
    uint16_t n;
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 49, h, FARP(s, 0), 0x100), LOADER_STOP_HLT);
    n = fx_ax(&kf);
    snprintf(out, sizeof out, "%s", (const char *)mem_at(s, 0, 1));
    return n ? out : "";
}

static uint32_t proc_address(uint16_t h, const char *name, uint16_t ordinal) {
    uint16_t s = name ? str_seg(name) : 0;
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 50, h, FARP(s, name ? 0 : ordinal)), LOADER_STOP_HLT);
    return fx_dxax(&kf);
}

/* Pushes a and b, far-calls fp from guest code and returns AX. */
static uint16_t call_far2(uint32_t fp, uint16_t a, uint16_t b) {
    uint8_t *c = mem_at(kf.p->seg_sel[0], FX_CALL_IP, 16);
    cpu_t *cpu = &kf.p->cpu;
    unsigned k = 0;
    c[k++] = 0x68; c[k++] = (uint8_t)a; c[k++] = (uint8_t)(a >> 8);
    c[k++] = 0x68; c[k++] = (uint8_t)b; c[k++] = (uint8_t)(b >> 8);
    c[k++] = 0x9A; c[k++] = (uint8_t)fp; c[k++] = (uint8_t)(fp >> 8);
    c[k++] = (uint8_t)(fp >> 16); c[k++] = (uint8_t)(fp >> 24);
    c[k++] = 0xF4;
    cpu->seg[CPU_CS] = kf.p->seg_sel[0];
    cpu->eip = FX_CALL_IP;
    CHECK_EQ(loader_run(kf.p, 1000), LOADER_STOP_HLT);
    return fx_ax(&kf);
}

static void named_mydll(nb_t *t, const char *module) {
    mydll_nb(t);
    t->module = module;
}

static void test_load_use_and_free(void) {
    nb_t t;
    uint16_t h, h2;
    loader_module *m;
    uint32_t add2, getd;
    printf("test_kernel_modules_load\n");
    boot(NULL);
    mydll_nb(&t);
    put_image("WINDOWS/SYSTEM/mydll.dll", &t);               /* lower-case host file name */
    h = load_library("MYDLL");
    CHECK(h >= 32);
    m = loader_module_by_handle(kf.p, h);
    CHECK(m != NULL);
    if (!m) { down(); return; }
    CHECK_EQ(h, m->auto_ds);
    CHECK_EQ(kf.p->nmods, 2);
    CHECK(!strcmp(m->path, "C:\\WINDOWS\\SYSTEM\\MYDLL.DLL"));
    CHECK_EQ(mem_at(m->auto_ds, 0x10, 2)[1], 0x01);          /* the entry point ran */
    CHECK_EQ(module_usage(h), 1);
    CHECK_EQ(load_library("mydll.dll"), h);                  /* same module: usage + 1 */
    CHECK_EQ(module_usage(h), 2);
    CHECK_EQ(module_handle("mydll"), h);
    CHECK_EQ(module_handle("MyDll.Dll"), h);
    CHECK_EQ(module_handle("OTHER"), 0);
    CHECK(!strcmp(module_file(h), "C:\\WINDOWS\\SYSTEM\\MYDLL.DLL"));
    add2 = proc_address(h, "add2", 0);
    getd = proc_address(h, NULL, 2);
    CHECK_EQ(add2, (uint32_t)m->seg_sel[0] << 16);
    CHECK_EQ(getd, (uint32_t)m->seg_sel[0] << 16 | 0x20);
    CHECK_EQ(proc_address(h, "NOSUCH", 0), 0);
    CHECK_EQ(proc_address(h, NULL, 99), 0);
    CHECK_EQ(call_far2(add2, 7, 5), 7 + 5 + 0x100);          /* a guest far call into the library's code */
    CHECK_EQ(call_far2(getd, 0, 0), 0x100);
    free_library(h);
    CHECK_EQ(module_usage(h), 1);
    CHECK_EQ(module_handle("MYDLL"), h);
    free_library(h);
    CHECK_EQ(module_usage(h), 0);
    CHECK_EQ(module_handle("MYDLL"), 0);
    CHECK_EQ(kf.p->nmods, 1);
    free_library(h);                                         /* freeing a dead handle is harmless */
    h2 = load_library("MYDLL");
    CHECK(h2 >= 32);
    down();
}

static void rm(const char *rel) {
    char p[700];
    snprintf(p, sizeof p, "%s/%s", g_root, rel);
    remove(p);
}

static void test_search_order(void) {
    nb_t t;
    uint16_t h;
    printf("test_kernel_modules_search_order\n");
    boot(NULL);
    named_mydll(&t, "FROMPROG");
    put_image("PROG/lib.dll", &t);
    named_mydll(&t, "FROMCUR");
    put_image("LIB.DLL", &t);                                /* the current directory is C:\ */
    named_mydll(&t, "FROMSYS");
    put_image("WINDOWS/SYSTEM/LIB.DLL", &t);
    named_mydll(&t, "FROMWIN");
    put_image("WINDOWS/LIB.DLL", &t);
    h = load_library("LIB");
    CHECK(h >= 32);
    CHECK(!strcmp(module_file(h), "C:\\PROG\\LIB.DLL"));      /* the program's directory first */
    CHECK_EQ(module_handle("FROMPROG"), h);                  /* the module name from the header */
    free_library(h);
    rm("PROG/lib.dll");
    h = load_library("LIB.DLL");
    CHECK(!strcmp(module_file(h), "C:\\LIB.DLL"));
    free_library(h);
    rm("LIB.DLL");
    h = load_library("lib");
    CHECK(!strcmp(module_file(h), "C:\\WINDOWS\\SYSTEM\\LIB.DLL"));
    free_library(h);
    rm("WINDOWS/SYSTEM/LIB.DLL");
    h = load_library("LIB");
    CHECK(!strcmp(module_file(h), "C:\\WINDOWS\\LIB.DLL"));
    free_library(h);
    down();
}

static void test_explicit_paths_and_drives(void) {
    nb_t t;
    uint16_t h, h2;
    printf("test_kernel_modules_explicit_paths\n");
    boot("C:\\NOWHERE");                                     /* a module path that finds nothing */
    mydll_nb(&t);
    put_image("cd/SETUP/mydll.dll", &t);
    put_image("cd/SETUP/writer.snd", &t);
    CHECK_EQ(load_library("MYDLL"), 2);                      /* not on the module path */
    h = load_library("D:\\SETUP\\MYDLL");                    /* no extension: .DLL */
    CHECK(h >= 32);
    CHECK(!strcmp(module_file(h), "D:\\SETUP\\MYDLL.DLL"));
    h2 = load_library("d:\\setup\\Writer.Snd");              /* the extension as given, any case */
    CHECK(h2 >= 32 && h2 != h);
    CHECK(!strcmp(module_file(h2), "D:\\SETUP\\WRITER.SND"));
    CHECK_EQ(load_library("D:\\SETUP\\WRITER.SND"), h2);     /* the same file */
    CHECK_EQ(module_usage(h2), 2);
    CHECK_EQ(load_library("D:\\NODIR\\X.DLL"), 3);           /* path not found */
    CHECK_EQ(load_library("E:\\X.DLL"), 3);                  /* unmapped drive */
    down();
}

static void test_errors_and_builtins(void) {
    static const uint8_t crash[] = {0x0F, 0x0B};
    nb_t t;
    uint16_t h;
    printf("test_kernel_modules_errors\n");
    boot(NULL);
    hd_write(g_root, "WINDOWS/SYSTEM/TEXT.DLL", "this is not a module", 20);
    CHECK_EQ(load_library("NOSUCH"), 2);
    CHECK_EQ(load_library("TEXT"), 11);                      /* bad format */
    mydll_nb(&t);
    t.segs[0] = (nb_seg){crash, sizeof crash, SEG_CODE, sizeof crash, NULL, 0};
    t.ip = 0;
    put_image("WINDOWS/SYSTEM/CRASH.DLL", &t);
    {   /* an entry point that faults stops the run at the fault; no module is left behind */
        uint16_t s = str_seg("CRASH");
        CHECK_EQ(FX_CALL(&kf, "KERNEL", 95, FARP(s, 0)), LOADER_STOP_FAULT);
        CHECK_EQ(kf.p->nmods, 1);
    }
    h = load_library("KERNEL");                              /* system modules stay with the HLE */
    CHECK(h >= 32);
    CHECK(loader_module_by_handle(kf.p, h) == NULL);
    CHECK_EQ(kf.p->nmods, 1);
    CHECK_EQ(module_handle("KERNEL"), h);
    down();
}

/* LIBENTRY-style entry: LocalInit(DS, 0, CX) with the heap the loader appended to the data segment;
 * ALLOC then calls LocalAlloc on the library's own heap (its prolog loads DS). */
static void test_library_heap(void) {
    static const uint8_t code[0x60] = {
        0x1E, 0x6A, 0x00, 0x51, 0x9A, 0xFF, 0xFF, 0x00, 0x00, 0xCB,               /* push ds; push 0; push cx; call LocalInit; retf */
        [0x20] = 0x8C, 0xD8, 0x90, 0x45, 0x55, 0x8B, 0xEC, 0x1E, 0x8E, 0xD8,
        0x6A, 0x00, 0x6A, 0x10, 0x9A, 0xFF, 0xFF, 0x00, 0x00,                       /* LocalAlloc(LMEM_FIXED, 16) */
        0x8D, 0x66, 0xFE, 0x1F, 0x5D, 0x4D, 0xCB,
    };
    static const nb_rel rel[] = {{NE_SRC_FARPTR, NB_F_ORD, 0x05, 1, 4}, {NE_SRC_FARPTR, NB_F_ORD, 0x2F, 1, 5}};
    nb_t t;
    uint16_t h;
    uint32_t alloc;
    loader_module *m;
    uint16_t a, b;
    printf("test_kernel_modules_library_heap\n");
    boot(NULL);
    memset(&t, 0, sizeof t);
    t.module = "HEAPDLL";
    t.flags = 0x8001;
    t.autods = 2;
    t.heap = 0x400;
    t.cs = 1;
    t.segs[0] = (nb_seg){code, sizeof code, SEG_CODE, sizeof code, rel, 2};
    t.segs[1] = (nb_seg){NULL, 0, SEG_DATA, 0x40, NULL, 0};
    t.nsegs = 2;
    t.modrefs[0] = "KERNEL";
    t.nmodrefs = 1;
    t.ents[0] = (nb_ent){1, 0x20};
    t.nents = 1;
    put_image("WINDOWS/SYSTEM/HEAPDLL.DLL", &t);
    h = load_library("HEAPDLL");
    CHECK(h >= 32);
    m = loader_module_by_handle(kf.p, h);
    if (!m) { down(); return; }
    CHECK_EQ(m->heap_start, 0x40);
    CHECK_EQ(m->heap_size, 0x400);
    alloc = proc_address(h, NULL, 1);
    a = call_far2(alloc, 0, 0);
    b = call_far2(alloc, 0, 0);
    CHECK(a >= 0x40 && a + 16 <= 0x40 + 0x400);              /* inside the library's heap, not its static data */
    CHECK(b >= 0x40 && b != a && b + 16 <= 0x40 + 0x400);
    CHECK_EQ(mem_size(&kf.p->mem, m->auto_ds), 0x40 + 0x400);
    free_library(h);
    h = load_library("HEAPDLL");                             /* the dropped heap does not shadow the new one */
    CHECK(h >= 32);
    a = call_far2(proc_address(h, NULL, 1), 0, 0);
    CHECK(a >= 0x40 && a + 16 <= 0x40 + 0x400);
    free_library(h);
    down();
}

static void test_library_resources(void) {
    static const uint8_t hello[] = "HELLO";
    static const uint8_t other[] = "SECOND RESOURCE";
    nb_t t;
    uint16_t h, s, hr, hg, hf;
    uint32_t p;
    const char *txt;
    printf("test_kernel_modules_resources\n");
    boot(NULL);
    memset(&t, 0, sizeof t);
    t.module = "SOUNDDLL";
    t.flags = 0x8000;
    t.restype = 0x0A;
    t.res[0] = (nb_res){7, hello, 5};
    t.res[1] = (nb_res){9, other, 15};
    t.nres = 2;
    put_image("PROG/WRITER.SND", &t);
    h = load_library("WRITER.SND");
    CHECK(h >= 32);
    CHECK(loader_module_by_handle(kf.p, h) != NULL);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 60, h, FARP(0, 9), FARP(0, 0x0A)), LOADER_STOP_HLT);   /* FindResource */
    hr = fx_ax(&kf);
    CHECK(hr != 0);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 65, h, hr), LOADER_STOP_HLT);                          /* SizeofResource */
    CHECK_EQ(fx_dxax(&kf), 16);                             /* the table stores whole 16-byte units */
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 61, h, hr), LOADER_STOP_HLT);                          /* LoadResource */
    hg = fx_ax(&kf);
    CHECK(hg != 0);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 62, hg), LOADER_STOP_HLT);                             /* LockResource */
    p = fx_dxax(&kf);
    txt = (const char *)mem_at((uint16_t)(p >> 16), (uint16_t)p, 15);
    CHECK(txt && !memcmp(txt, "SECOND RESOURCE", 15));
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 60, h, FARP(0, 7), FARP(0, 0x0B)), LOADER_STOP_HLT);   /* wrong type */
    CHECK_EQ(fx_ax(&kf), 0);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 60, h, FARP(0, 7), FARP(0, 0x0A)), LOADER_STOP_HLT);
    hr = fx_ax(&kf);
    CHECK(hr != 0);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 64, h, hr), LOADER_STOP_HLT);                          /* AccessResource */
    hf = fx_ax(&kf);
    CHECK(hf != 0xFFFF);
    s = seg_of(0x40);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 82, hf, FARP(s, 0), 5), LOADER_STOP_HLT);              /* _lread */
    CHECK_EQ(fx_ax(&kf), 5);
    CHECK(!memcmp(mem_at(s, 0, 5), "HELLO", 5));
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 65, kf.p->auto_ds, hr), LOADER_STOP_HLT);               /* the program has none */
    CHECK_EQ(fx_dxax(&kf), 0);
    down();
}

int main(void) {
    test_load_use_and_free();
    test_search_order();
    test_explicit_paths_and_drives();
    test_errors_and_builtins();
    test_library_heap();
    test_library_resources();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
