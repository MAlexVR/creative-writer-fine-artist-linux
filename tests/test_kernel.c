#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <strings.h>
#include "kfixture.h"
#include "win16_imports.h"

#define K_FATALEXIT 1
#define K_GETVERSION 3
#define K_GETCURRENTTASK 36
#define K_WAITEVENT 30
#define K_YIELD 29
#define K_INITTASK 91
#define K_SETERRORMODE 107
#define K_OUTPUTDEBUGSTRING 115
#define K_GETDOSENVIRONMENT 131
#define K_GETWINFLAGS 132
#define K_GETDRIVETYPE 136
#define K_FATALAPPEXIT 137
#define K_MAKEPROCINSTANCE 51
#define K_FREEPROCINSTANCE 52
#define K_DEFINEHANDLETABLE 94

/* Imported ordinal -> name, as in the import table of WRITER.EXE (tests/win16_imports.h). The
 * ordinal side comes from the NE import table of WRITER.EXE (nedump). */
static const struct { unsigned ord; const char *name; } verified[] = {
    {1, "FatalExit"}, {3, "GetVersion"}, {5, "LocalAlloc"}, {7, "LocalFree"}, {8, "LocalLock"},
    {9, "LocalUnlock"}, {11, "LocalHandle"}, {15, "GlobalAlloc"}, {16, "GlobalReAlloc"},
    {17, "GlobalFree"}, {18, "GlobalLock"}, {19, "GlobalUnlock"}, {20, "GlobalSize"},
    {21, "GlobalHandle"}, {30, "WaitEvent"}, {36, "GetCurrentTask"}, {47, "GetModuleHandle"},
    {49, "GetModuleFileName"}, {50, "GetProcAddress"}, {51, "MakeProcInstance"},
    {52, "FreeProcInstance"}, {55, "Catch"}, {56, "Throw"}, {57, "GetProfileInt"},
    {58, "GetProfileString"}, {59, "WriteProfileString"}, {60, "FindResource"},
    {61, "LoadResource"}, {62, "LockResource"}, {63, "FreeResource"}, {74, "OpenFile"},
    {81, "_lclose"}, {82, "_lread"}, {83, "_lcreat"}, {84, "_llseek"}, {85, "_lopen"},
    {88, "lstrcpy"}, {89, "lstrcat"}, {90, "lstrlen"}, {91, "InitTask"}, {95, "LoadLibrary"},
    {96, "FreeLibrary"}, {97, "GetTempFileName"}, {102, "Dos3Call"}, {107, "SetErrorMode"},
    {113, "__AHSHIFT"}, {114, "__AHINCR"}, {115, "OutputDebugString"}, {131, "GetDOSEnvironment"},
    {132, "GetWinFlags"}, {136, "GetDriveType"}, {137, "FatalAppExit"}, {169, "GetFreeSpace"},
    {178, "__WINFLAGS"}, {195, "__C000H"}, {349, "_hread"},
};

static void test_ordinal_table(void) {
    size_t i, j, tsv = 0, found = 0;
    printf("test_ordinal_table\n");
    kboot();
    CHECK_EQ(sizeof verified / sizeof verified[0], 56); /* KERNEL imports of WRITER.EXE */
    for (i = 0; i < sizeof verified / sizeof verified[0]; i++) {
        const hle_entry *e = hle_find(&kh, "KERNEL", (uint16_t)verified[i].ord);
        CHECK(e != NULL);
        if (e) CHECK(!strcasecmp(e->name, verified[i].name));
        if (!e) printf("    missing KERNEL.%u %s\n", verified[i].ord, verified[i].name);
    }
    for (i = 0; i < WIN16_IMPORT_COUNT; i++) {          /* the names of WRITER's import table are the same set */
        if (strcmp(win16_imports[i].module, "KERNEL")) continue;
        tsv++;
        for (j = 0; j < sizeof verified / sizeof verified[0]; j++)
            if (!strcasecmp(win16_imports[i].name, verified[j].name)) { found++; break; }
    }
    CHECK_EQ(tsv, 56);
    CHECK_EQ(found, 56);
    kdown();
}

static void test_version_and_flags(void) {
    printf("test_version_and_flags\n");
    kboot();
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_GETVERSION), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&kf), 0x00060A03u); /* Windows 3.10 in AL.AH, DOS 6.00 in DL.DH */
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_GETWINFLAGS), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf) & 0x0021, 0x0021);  /* WF_PMODE | WF_ENHANCED */
    CHECK_EQ(fx_ax(&kf) & 0x0010, 0);       /* not WF_STANDARD */
    CHECK_EQ(fx_ax(&kf) & 0x0800, 0x0800);  /* WF_PAGING */
    CHECK_EQ(fx_ax(&kf) & 0x0400, 0x0400);  /* WF_80x87: the CPU core provides an x87 */
    kdown();
}

static void test_constant_exports(void) {
    loader_import imp;
    loader_thunk *t;
    printf("test_constant_exports\n");
    kboot();
    memset(&imp, 0, sizeof imp);
    strcpy(imp.module, "KERNEL");
    imp.ordinal = 113;
    CHECK_EQ(loader_thunk_for(kf.p, &imp, &t), LOADER_OK);
    CHECK(t->api.is_const && t->off == 3); /* __AHSHIFT */
    imp.ordinal = 114;
    CHECK_EQ(loader_thunk_for(kf.p, &imp, &t), LOADER_OK);
    CHECK(t->api.is_const && t->off == MEM_AHINCR); /* __AHINCR matches the memory model */
    imp.ordinal = 178;
    CHECK_EQ(loader_thunk_for(kf.p, &imp, &t), LOADER_OK);
    CHECK(t->api.is_const && t->off == 0x0C29); /* __WINFLAGS equals GetWinFlags */
    imp.ordinal = 195;
    CHECK_EQ(loader_thunk_for(kf.p, &imp, &t), LOADER_OK);
    CHECK(t->api.is_const && mem_valid(&kf.p->mem, t->sel)); /* __C000H */
    kdown();
}

static void test_inittask(void) {
    cpu_t *c;
    uint8_t *ds;
    printf("test_inittask\n");
    kboot();
    c = &kf.p->cpu;
    c->r[CPU_EAX] = 0xDEAD; c->r[CPU_EBX] = 0; c->r[CPU_ECX] = 0; c->r[CPU_EDX] = 0;
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_INITTASK), LOADER_STOP_HLT);
    CHECK(fx_ax(&kf) != 0);                      /* success */
    CHECK_EQ(c->r[CPU_EBX] & 0xFFFF, 0x81);      /* ES:BX = command line in the PSP */
    CHECK_EQ(c->seg[CPU_ES], kf.p->psp_sel);
    CHECK_EQ(c->r[CPU_ECX] & 0xFFFF, kf.p->stack_size);
    CHECK_EQ(c->r[CPU_EDX] & 0xFFFF, 1);         /* SW_SHOWNORMAL */
    CHECK_EQ(c->r[CPU_EDI] & 0xFFFF, kf.p->auto_ds);
    CHECK_EQ(c->r[CPU_ESI] & 0xFFFF, 0);         /* no previous instance */
    /* instance data header the C runtime reads: pLocalHeap, stack top/min/bottom */
    ds = kmem(kf.p->auto_ds, 0, 0x10);
    CHECK(ds != NULL);
    if (ds) {
        unsigned top = ds[0x0A] | ds[0x0B] << 8, min = ds[0x0C] | ds[0x0D] << 8,
                 bottom = ds[0x0E] | ds[0x0F] << 8, heap = ds[0x06] | ds[0x07] << 8;
        CHECK_EQ(heap, kf.p->heap_start);
        CHECK_EQ(top, kf.p->sp_default);
        CHECK_EQ(min, top);
        CHECK_EQ(bottom, top - kf.p->stack_size);
    }
    kdown();
}

static void test_task_misc(void) {
    uint32_t task;
    printf("test_task_misc\n");
    kboot();
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_WAITEVENT, 0), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 0);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_YIELD), LOADER_STOP_HLT);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_GETCURRENTTASK), LOADER_STOP_HLT);
    task = fx_ax(&kf);
    CHECK(task != 0 && mem_valid(&kf.p->mem, (uint16_t)task));
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_GETCURRENTTASK), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), task);
    /* SetErrorMode returns the previous mode */
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_SETERRORMODE, 0x8001), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 0);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_SETERRORMODE, 0), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 0x8001);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    /* MakeProcInstance returns the procedure itself, FreeProcInstance is a no-op */
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_MAKEPROCINSTANCE, FARP(0x1234, 0x5678), 0x0042), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&kf), 0x12345678u);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_FREEPROCINSTANCE, FARP(0x1234, 0x5678)), LOADER_STOP_HLT);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_DEFINEHANDLETABLE, 0x1234), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 1);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    kdown();
}

static void test_dos_environment(void) {
    uint16_t sel;
    const char *e;
    size_t n = 0;
    printf("test_dos_environment\n");
    kboot();
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_GETDOSENVIRONMENT), LOADER_STOP_HLT);
    sel = (uint16_t)(fx_dxax(&kf) >> 16);
    CHECK_EQ(fx_ax(&kf), 0);
    CHECK(sel != 0 && mem_valid(&kf.p->mem, sel));
    if (!kmem(sel, 0, 1)) { kdown(); return; }
    e = kcstr(sel, 0);
    while (e[n]) n += strlen(e + n) + 1;           /* walk the NAME=value strings */
    CHECK(n > 0);
    CHECK_EQ(e[n], 0);                              /* block ends with an empty string */
    CHECK_EQ(kmem(sel, n + 1, 2)[0], 1);            /* then a word count of 1 */
    CHECK(!strcmp(kcstr(sel, n + 3), "C:\\CWFA\\APP.EXE")); /* then the program path */
    kdown();
}

static void test_psp_environment(void) {
    const uint8_t *psp;
    uint16_t env;
    printf("test_psp_environment\n");
    kboot();
    /* the C runtime finds the environment through PSP:2C, as under Windows */
    psp = kmem(kf.p->psp_sel, 0x2C, 2);
    env = psp ? (uint16_t)(psp[0] | psp[1] << 8) : 0;
    CHECK(env != 0 && mem_valid(&kf.p->mem, env));
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_GETDOSENVIRONMENT), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&kf) >> 16, env);              /* the same block GetDOSEnvironment hands out */
    kdown();
}

static void test_drive_type(void) {
    kernel_config cfg;
    printf("test_drive_type\n");
    memset(&cfg, 0, sizeof cfg);
    cfg.host_root['C' - 'A'] = "/tmp";
    cfg.host_root['D' - 'A'] = "/tmp";
    kboot_ex(&cfg, 0, 0x800, 0x1000);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_GETDRIVETYPE, 2), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 3); /* DRIVE_FIXED */
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_GETDRIVETYPE, 3), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 4); /* the CD shows up as DRIVE_REMOTE, as under Windows 3.1 */
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_GETDRIVETYPE, 4), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 0); /* no such drive */
    CHECK_EQ(kf.sp_after, kf.sp_before);
    kdown();
}

static void test_debug_and_fatal(void) {
    kernel_config cfg;
    char buf[256];
    size_t n;
    uint16_t msg;
    printf("test_debug_and_fatal\n");
    memset(&cfg, 0, sizeof cfg);
    cfg.debug = tmpfile();
    kboot_ex(&cfg, 0, 0x800, 0x1000);
    msg = kstr("trace line");
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_OUTPUTDEBUGSTRING, FARP(msg, 0)), LOADER_STOP_HLT);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    msg = kstr("Out of memory");
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_FATALAPPEXIT, 0, FARP(msg, 0)), LOADER_STOP_REQUESTED);
    CHECK_EQ(kf.p->stop, LOADER_STOP_REQUESTED);
    kf.p->stop = LOADER_STOP_NONE;
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_FATALEXIT, 0x0123), LOADER_STOP_REQUESTED);
    rewind(cfg.debug);
    n = fread(buf, 1, sizeof buf - 1, cfg.debug);
    buf[n] = 0;
    CHECK(strstr(buf, "trace line") != NULL);
    CHECK(strstr(buf, "FatalAppExit") != NULL && strstr(buf, "Out of memory") != NULL);
    CHECK(strstr(buf, "FatalExit") != NULL && strstr(buf, "0123") != NULL);
    kdown();
    fclose(cfg.debug);
}


/* ---- modules ------------------------------------------------------------ */

#define K_GETMODULEHANDLE 47
#define K_GETMODULEFILENAME 49
#define K_GETPROCADDRESS 50
#define K_LOADLIBRARY 95
#define K_FREELIBRARY 96

static kernel_config writer_cfg(void) {
    kernel_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.exe_path = "C:\\CWFA\\WRITER.EXE";
    return cfg;
}

static uint16_t call_str(unsigned ord, const char *s) {
    uint16_t sel = kstr(s);
    FX_CALL(&kf, "KERNEL", ord, FARP(sel, 0));
    return fx_ax(&kf);
}

static void test_module_handles(void) {
    kernel_config cfg = writer_cfg();
    uint16_t kernel, user, comm;
    printf("test_module_handles\n");
    kboot_ex(&cfg, 0, 0x800, 0x1000);
    CHECK_EQ(call_str(K_GETMODULEHANDLE, "WRITER"), kf.p->auto_ds);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(call_str(K_GETMODULEHANDLE, "writer.exe"), kf.p->auto_ds); /* case and extension ignored */
    kernel = call_str(K_GETMODULEHANDLE, "KERNEL");
    user = call_str(K_GETMODULEHANDLE, "USER");
    CHECK(kernel != 0 && user != 0 && kernel != user && kernel != kf.p->auto_ds);
    CHECK_EQ(call_str(K_GETMODULEHANDLE, "kernel"), kernel);
    CHECK_EQ(call_str(K_GETMODULEHANDLE, "NOSUCH"), 0);
    CHECK_EQ(call_str(K_GETMODULEHANDLE, "KIDSWTCH"), 0); /* a library that was never loaded: test_kernel_modules loads real ones */
    /* LoadLibrary: system modules by name or path, otherwise a Windows error code < 32 */
    comm = call_str(K_LOADLIBRARY, "C:\\WINDOWS\\SYSTEM\\commdlg.dll");
    CHECK(comm >= 32);
    CHECK_EQ(call_str(K_GETMODULEHANDLE, "COMMDLG"), comm);
    CHECK_EQ(call_str(K_LOADLIBRARY, "COMMDLG.DLL"), comm);
    CHECK_EQ(call_str(K_LOADLIBRARY, "NOSUCH.DLL"), 2);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_FREELIBRARY, comm), LOADER_STOP_HLT);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    kdown();
}

static void test_module_usage(void) {
    kernel_config cfg = writer_cfg();
    uint16_t comm;
    printf("test_module_usage\n");
    kboot_ex(&cfg, 0, 0x800, 0x1000);
    FX_CALL(&kf, "KERNEL", 48, kf.p->auto_ds);
    CHECK_EQ(fx_ax(&kf), 1);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    comm = call_str(K_GETMODULEHANDLE, "COMMDLG");
    FX_CALL(&kf, "KERNEL", 48, comm);
    CHECK_EQ(fx_ax(&kf), 1);                                   /* system modules are resident */
    call_str(K_LOADLIBRARY, "COMMDLG.DLL");
    FX_CALL(&kf, "KERNEL", 48, comm);
    CHECK_EQ(fx_ax(&kf), 2);
    FX_CALL(&kf, "KERNEL", K_FREELIBRARY, comm);
    FX_CALL(&kf, "KERNEL", 48, comm);
    CHECK_EQ(fx_ax(&kf), 1);
    FX_CALL(&kf, "KERNEL", K_FREELIBRARY, comm);               /* never below the resident reference */
    FX_CALL(&kf, "KERNEL", 48, comm);
    CHECK_EQ(fx_ax(&kf), 1);
    FX_CALL(&kf, "KERNEL", 48, 0x4321);
    CHECK_EQ(fx_ax(&kf), 0);
    kdown();
}

static void test_module_file_name(void) {
    kernel_config cfg = writer_cfg();
    uint16_t buf, user;
    printf("test_module_file_name\n");
    kboot_ex(&cfg, 0, 0x800, 0x1000);
    buf = kseg(0x100);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_GETMODULEFILENAME, kf.p->auto_ds, FARP(buf, 0), 0x40), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 18);
    CHECK(!strcmp(kcstr(buf, 0), "C:\\CWFA\\WRITER.EXE"));
    CHECK_EQ(kf.sp_after, kf.sp_before);
    FX_CALL(&kf, "KERNEL", K_GETMODULEFILENAME, kf.p->auto_ds, FARP(buf, 0), 5);
    CHECK_EQ(fx_ax(&kf), 4); /* truncated, NUL terminated */
    CHECK(!strcmp(kcstr(buf, 0), "C:\\C"));
    user = call_str(K_GETMODULEHANDLE, "USER");
    FX_CALL(&kf, "KERNEL", K_GETMODULEFILENAME, user, FARP(buf, 0), 0x40);
    CHECK(!strcmp(kcstr(buf, 0), "C:\\WINDOWS\\SYSTEM\\USER.EXE"));
    FX_CALL(&kf, "KERNEL", K_GETMODULEFILENAME, 0x1234, FARP(buf, 0), 0x40);
    CHECK_EQ(fx_ax(&kf), 0); /* not a module */
    kdown();
}

static uint32_t thunk_addr(unsigned ord) {
    loader_thunk *t = fx_thunk(&kf, "KERNEL", ord);
    return (uint32_t)t->sel << 16 | t->off;
}

static void test_get_proc_address(void) {
    kernel_config cfg = writer_cfg();
    uint16_t kernel, name, sel;
    ne_name *rn;
    ne_entry *en;
    printf("test_get_proc_address\n");
    kboot_ex(&cfg, 0, 0x800, 0x1000);
    kernel = call_str(K_GETMODULEHANDLE, "KERNEL");
    name = kstr("globalalloc"); /* names are case-insensitive */
    FX_CALL(&kf, "KERNEL", K_GETPROCADDRESS, kernel, FARP(name, 0));
    CHECK_EQ(fx_dxax(&kf), thunk_addr(15));
    CHECK_EQ(kf.sp_after, kf.sp_before);
    FX_CALL(&kf, "KERNEL", K_GETPROCADDRESS, kernel, FARP(0, 18)); /* by ordinal */
    CHECK_EQ(fx_dxax(&kf), thunk_addr(18));
    name = kstr("NoSuchFunction");
    FX_CALL(&kf, "KERNEL", K_GETPROCADDRESS, kernel, FARP(name, 0));
    CHECK_EQ(fx_dxax(&kf), 0);
    FX_CALL(&kf, "KERNEL", K_GETPROCADDRESS, 0x4321, FARP(name, 0)); /* bad module */
    CHECK_EQ(fx_dxax(&kf), 0);
    /* the program's own exports */
    kf.ne.resident_names = calloc(2, sizeof *kf.ne.resident_names);
    kf.ne.resident_count = 2;
    rn = kf.ne.resident_names;
    strcpy(rn[0].text, "WRITER");
    strcpy(rn[1].text, "MAINWNDPROC");
    rn[1].ordinal = 1;
    kf.ne.entries = en = calloc(1, sizeof *kf.ne.entries);
    kf.ne.entry_count = 1;
    en->kind = NE_ENTRY_FIXED; en->segment = 1; en->offset = 0x50;
    name = kstr("MainWndProc");
    FX_CALL(&kf, "KERNEL", K_GETPROCADDRESS, kf.p->auto_ds, FARP(name, 0));
    sel = kf.p->seg_sel[0];
    CHECK_EQ(fx_dxax(&kf), (uint32_t)sel << 16 | 0x50);
    FX_CALL(&kf, "KERNEL", K_GETPROCADDRESS, kf.p->auto_ds, FARP(0, 1));
    CHECK_EQ(fx_dxax(&kf), (uint32_t)sel << 16 | 0x50);
    kdown();
}

/* ---- strings and memory helpers ------------------------------------------ */

#define K_LSTRCPY 88
#define K_LSTRCAT 89
#define K_LSTRLEN 90
#define K_HMEMCPY 348
#define U_INITAPP 5
#define U_GETTICKCOUNT 13
#define U_GETCURRENTTIME 15
#define U_LSTRCMP 430
#define U_ANSIUPPER 431
#define U_ANSILOWER 432
#define U_LSTRCMPI 471
#define U_ANSINEXT 472
#define U_ANSIPREV 473

static void test_lstr(void) {
    uint16_t dst, src;
    printf("test_lstr\n");
    kboot();
    dst = kseg(0x100);
    src = kstr("Hello");
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_LSTRCPY, FARP(dst, 0x10), FARP(src, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&kf), (uint32_t)dst << 16 | 0x10); /* returns the destination */
    CHECK(!strcmp(kcstr(dst, 0x10), "Hello"));
    CHECK_EQ(kf.sp_after, kf.sp_before);
    src = kstr(", world");
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_LSTRCAT, FARP(dst, 0x10), FARP(src, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&kf), (uint32_t)dst << 16 | 0x10);
    CHECK(!strcmp(kcstr(dst, 0x10), "Hello, world"));
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_LSTRLEN, FARP(dst, 0x10)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 12);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    /* overlapping copy inside one buffer behaves like memmove */
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_LSTRCPY, FARP(dst, 0x12), FARP(dst, 0x10)), LOADER_STOP_HLT);
    CHECK(!strcmp(kcstr(dst, 0x12), "Hello, world"));
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_LSTRLEN, FARP(0x1234, 0)), LOADER_STOP_FAULT); /* bad pointer faults */
    kdown();
}

static void test_hmemcpy_huge(void) {
    uint16_t a, b;
    uint32_t n = 0x25000, i;
    uint8_t *pa;
    printf("test_hmemcpy_huge\n");
    kboot();
    a = mem_alloc_huge(&kf.p->mem, 0x30000, MEM_DATA);
    b = mem_alloc_huge(&kf.p->mem, 0x30000, MEM_DATA);
    for (i = 0; i < n; i += 0x1000) {
        pa = kmem(mem_huge_sel(a, i >> 16), i & 0xFFFF, 1);
        if (pa) *pa = (uint8_t)(i >> 12 ^ 0x5A);
    }
    /* start 0x100 below the 64K boundary so the copy crosses it */
    CHECK_EQ(FX_CALL(&kf, "KERNEL", K_HMEMCPY, FARP(b, 0xFF00), FARP(a, 0xFF00), LONG(n - 0xFF00)), LOADER_STOP_HLT);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    for (i = 0x10000; i < n; i += 0x1000) {
        const uint8_t *pb = kmem(mem_huge_sel(b, i >> 16), i & 0xFFFF, 1);
        pa = kmem(mem_huge_sel(a, i >> 16), i & 0xFFFF, 1);
        CHECK(pa && pb && *pa == *pb && *pa != 0);
    }
    kdown();
}

static void test_user_string_helpers(void) {
    uint16_t a, b, buf;
    kernel_config cfg;
    printf("test_user_string_helpers\n");
    memset(&cfg, 0, sizeof cfg);
    kboot_ex(&cfg, 0, 0x800, 0x1000);
    CHECK(hle_find(&kh, "USER", U_LSTRCMP) == NULL); /* USER is not registered unless asked for */
    kdown();
    kboot_ex(&cfg, KERNEL_REG_USER_HELPERS, 0x800, 0x1000);
    a = kstr("Apple");
    b = kstr("apple");
    FX_CALL(&kf, "USER", U_LSTRCMP, FARP(a, 0), FARP(b, 0));
    CHECK((int16_t)fx_ax(&kf) < 0); /* 'A' < 'a' */
    CHECK_EQ(kf.sp_after, kf.sp_before);
    FX_CALL(&kf, "USER", U_LSTRCMPI, FARP(a, 0), FARP(b, 0));
    CHECK_EQ(fx_ax(&kf), 0);
    FX_CALL(&kf, "USER", U_LSTRCMP, FARP(b, 0), FARP(a, 0));
    CHECK((int16_t)fx_ax(&kf) > 0);
    buf = kstr("abc\xe9z");
    FX_CALL(&kf, "USER", U_ANSIUPPER, FARP(buf, 0));
    CHECK_EQ(fx_dxax(&kf), (uint32_t)buf << 16);
    CHECK(!strncmp(kcstr(buf, 0), "ABC", 3));
    FX_CALL(&kf, "USER", U_ANSILOWER, FARP(buf, 0));
    CHECK(!strncmp(kcstr(buf, 0), "abc", 3));
    FX_CALL(&kf, "USER", U_ANSIUPPER, FARP(0, 'q')); /* a single character in the low word */
    CHECK_EQ(fx_ax(&kf), 'Q');
    FX_CALL(&kf, "USER", U_ANSINEXT, FARP(buf, 1));
    CHECK_EQ(fx_dxax(&kf), (uint32_t)buf << 16 | 2);
    FX_CALL(&kf, "USER", U_ANSIPREV, FARP(buf, 0), FARP(buf, 3));
    CHECK_EQ(fx_dxax(&kf), (uint32_t)buf << 16 | 2);
    FX_CALL(&kf, "USER", U_ANSIPREV, FARP(buf, 0), FARP(buf, 0)); /* never before the start */
    CHECK_EQ(fx_dxax(&kf), (uint32_t)buf << 16);
    b = kstr("");
    FX_CALL(&kf, "USER", U_ANSINEXT, FARP(b, 0)); /* never past the terminator */
    CHECK_EQ(fx_dxax(&kf), (uint32_t)b << 16);
    CHECK_EQ(FX_CALL(&kf, "USER", U_INITAPP, kf.p->auto_ds), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 1);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    kdown();
}

static uint32_t g_now;
static uint32_t fake_now(void *ctx) { (void)ctx; return g_now; }

static void test_time(void) {
    kernel_config cfg;
    uint32_t t0;
    printf("test_time\n");
    memset(&cfg, 0, sizeof cfg);
    cfg.now_ms = fake_now;
    kboot_ex(&cfg, KERNEL_REG_USER_HELPERS, 0x800, 0x1000);
    g_now = 0x12345678u;
    FX_CALL(&kf, "USER", U_GETTICKCOUNT);
    CHECK_EQ(fx_dxax(&kf), 0x12345678u);
    g_now = 99;
    FX_CALL(&kf, "USER", U_GETCURRENTTIME);
    CHECK_EQ(fx_dxax(&kf), 99);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    kdown();
    kboot_ex(NULL, KERNEL_REG_USER_HELPERS, 0x800, 0x1000); /* default clock is monotonic */
    FX_CALL(&kf, "USER", U_GETTICKCOUNT);
    t0 = fx_dxax(&kf);
    FX_CALL(&kf, "USER", U_GETTICKCOUNT);
    CHECK(fx_dxax(&kf) - t0 < 1000);
    kdown();
}

/* ---- Catch / Throw -------------------------------------------------------- */

#define K_CATCH 55
#define K_THROW 56

static size_t put(uint8_t *c, size_t k, const uint8_t *b, size_t n) {
    memcpy(c + k, b, n);
    return k + n;
}

static size_t put_push(uint8_t *c, size_t k, unsigned v) {
    const uint8_t b[3] = {0x68, (uint8_t)v, (uint8_t)(v >> 8)};
    return put(c, k, b, 3);
}

static size_t put_call(uint8_t *c, size_t k, unsigned ord) {
    loader_thunk *t = fx_thunk(&kf, "KERNEL", ord);
    const uint8_t b[5] = {0x9A, (uint8_t)t->off, (uint8_t)(t->off >> 8), (uint8_t)t->sel,
                          (uint8_t)(t->sel >> 8)};
    return put(c, k, b, 5);
}

static void test_catch_throw(void) {
    uint8_t *c;
    uint16_t buf;
    size_t k = 0;
    static const uint8_t cmp0[] = {0x3D, 0x00, 0x00};        /* cmp ax, 0 */
    static const uint8_t jne17[] = {0x75, 0x17};             /* jne +23 (to the HLT) */
    static const uint8_t clobber[] = {0xBE, 0x99, 0x99,      /* mov si, 0x9999 */
                                      0xBF, 0x88, 0x88};     /* mov di, 0x8888 */
    static const uint8_t after_throw[] = {0xB8, 0x66, 0x66}; /* mov ax, 0x6666 (unreachable) */
    static const uint8_t hlt[] = {0xF4};
    cpu_t *cpu;
    printf("test_catch_throw\n");
    kboot();
    cpu = &kf.p->cpu;
    buf = kseg(0x20);
    c = kmem(kf.p->seg_sel[0], FX_CALL_IP, 0x80);
    k = put_push(c, k, buf);
    k = put_push(c, k, 0);
    k = put_call(c, k, K_CATCH);
    k = put(c, k, cmp0, sizeof cmp0);
    k = put(c, k, jne17, sizeof jne17);
    k = put(c, k, clobber, sizeof clobber);
    k = put_push(c, k, buf);
    k = put_push(c, k, 0);
    k = put_push(c, k, 7);
    k = put_call(c, k, K_THROW);
    k = put(c, k, after_throw, sizeof after_throw);
    k = put(c, k, hlt, sizeof hlt);
    cpu->seg[CPU_CS] = kf.p->seg_sel[0];
    cpu->eip = FX_CALL_IP;
    cpu->r[CPU_ESI] = 0x1111;
    cpu->r[CPU_EDI] = 0x2222;
    cpu->r[CPU_EBP] = 0x3333;
    kf.sp_before = (uint16_t)cpu->r[CPU_ESP];
    CHECK_EQ(loader_run(kf.p, 1000), LOADER_STOP_HLT);
    CHECK_EQ(cpu->r[CPU_EAX] & 0xFFFF, 7);          /* Catch "returns" the thrown value again */
    CHECK_EQ(cpu->r[CPU_ESI] & 0xFFFF, 0x1111);     /* registers are those of the Catch call */
    CHECK_EQ(cpu->r[CPU_EDI] & 0xFFFF, 0x2222);
    CHECK_EQ(cpu->r[CPU_EBP] & 0xFFFF, 0x3333);
    CHECK_EQ(cpu->r[CPU_ESP] & 0xFFFF, kf.sp_before); /* stack as after the first Catch return */
    CHECK_EQ(kf.p->api_calls, 2);
    kdown();
}

int main(void) {
    test_ordinal_table();
    test_version_and_flags();
    test_constant_exports();
    test_inittask();
    test_task_misc();
    test_dos_environment();
    test_psp_environment();
    test_drive_type();
    test_debug_and_fatal();
    test_module_handles();
    test_module_usage();
    test_module_file_name();
    test_get_proc_address();
    test_lstr();
    test_hmemcpy_huge();
    test_user_string_helpers();
    test_time();
    test_catch_throw();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
