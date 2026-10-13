#define _POSIX_C_SOURCE 200809L
/* TOOLHELP TaskFirst / TaskNext: the one task the runtime runs, so a
 * single-instance check finds no other copy. */
#include "dllfix.h"
#include "hostdir.h"
#include "kfixture.h"
#include "toolhelp.h"

#define TASKENTRY_SIZE 39

static toolhelp_t *th;

static void boot(void) {
    kernel_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.exe_path = "C:\\CWFA\\WRITER.EXE";
    kboot_ex(&cfg, 0, 0x800, 0x1000);
    th = toolhelp_create(&kh, kk);
    CHECK(th != NULL);
    if (th) CHECK(toolhelp_attach(th, kf.p));
}

static void down(void) {
    toolhelp_destroy(th);
    th = NULL;
    kdown();
}

static uint16_t rd16(uint16_t s, uint32_t off) { return (uint16_t)(kmem(s, off, 2)[0] | kmem(s, off, 2)[1] << 8); }
static uint32_t rd32(uint16_t s, uint32_t off) { return rd16(s, off) | (uint32_t)rd16(s, off + 2) << 16; }

static void test_ordinals(void) {
    const hle_entry *e;
    printf("test_toolhelp_ordinals\n");
    boot();
    e = hle_find(&kh, "TOOLHELP", 63);
    CHECK(e && e->fn && !strcmp(e->name, "TaskFirst") && e->arg_bytes == 4);
    e = hle_find(&kh, "TOOLHELP", 64);
    CHECK(e && e->fn && !strcmp(e->name, "TaskNext") && e->arg_bytes == 4);
    down();
}

static void test_task_first_next(void) {
    uint16_t s, task;
    printf("test_toolhelp_task_first_next\n");
    boot();
    s = kseg(0x100);
    /* the handle GetCurrentTask returns */
    CHECK_EQ(fx_call_words(&kf, "KERNEL", 36, (uint16_t[]){0}, 0), LOADER_STOP_HLT);
    task = fx_ax(&kf);
    CHECK(task != 0);
    /* dwSize must say it is a TASKENTRY */
    memset(kmem(s, 0, 0x100), 0, 0x100);
    CHECK_EQ(FX_CALL(&kf, "TOOLHELP", 63, FARP(s, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 0);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    ((uint8_t *)kmem(s, 0, 4))[0] = TASKENTRY_SIZE;
    CHECK_EQ(FX_CALL(&kf, "TOOLHELP", 63, FARP(s, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 1);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(rd32(s, 0), TASKENTRY_SIZE);
    CHECK_EQ(rd16(s, 4), task);                              /* hTask: the running task */
    CHECK_EQ(rd16(s, 8), kf.p->auto_ds);                     /* hInst */
    CHECK_EQ(rd16(s, 10), kf.p->auto_ds);                    /* hModule: the same value GetModuleHandle gives */
    CHECK_EQ(rd16(s, 12), kf.p->cpu.seg[CPU_SS]);            /* wSS */
    CHECK(rd16(s, 16) >= rd16(s, 20));                       /* wStackTop above wStackBottom */
    CHECK(strcmp((char *)kmem(s, 26, 9), "WRITER") == 0);    /* szModule */
    CHECK_EQ(rd16(s, 37), 0);                                /* hNext: none */
    /* TaskNext: the list ends after the one task, and keeps ending */
    CHECK_EQ(FX_CALL(&kf, "TOOLHELP", 64, FARP(s, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 0);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(FX_CALL(&kf, "TOOLHELP", 64, FARP(s, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 0);
    /* TaskNext before TaskFirst, or with a wrong size: FALSE */
    memset(kmem(s, 0, 0x100), 0, 0x100);
    CHECK_EQ(FX_CALL(&kf, "TOOLHELP", 64, FARP(s, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 0);
    /* a second TaskFirst starts the walk again */
    ((uint8_t *)kmem(s, 0, 4))[0] = TASKENTRY_SIZE;
    FX_CALL(&kf, "TOOLHELP", 63, FARP(s, 0));
    CHECK_EQ(fx_ax(&kf), 1);
    /* a pointer that is not valid memory faults like any API call */
    CHECK(FX_CALL(&kf, "TOOLHELP", 63, FARP(0x7777, 0)) != LOADER_STOP_HLT);
    down();
}

/* The single-instance check as a program writes it: walk the task list and compare each entry's module name
 * and handle with its own. Here the host plays the loop. */
static void test_single_instance_walk(void) {
    uint16_t s, self_inst;
    int others = 0, walked = 0;
    printf("test_toolhelp_single_instance\n");
    boot();
    s = kseg(0x100);
    self_inst = kf.p->auto_ds;
    ((uint8_t *)kmem(s, 0, 4))[0] = TASKENTRY_SIZE;
    FX_CALL(&kf, "TOOLHELP", 63, FARP(s, 0));
    while (fx_ax(&kf) && walked < 10) {
        walked++;
        if (rd16(s, 8) != self_inst && strcmp((char *)kmem(s, 26, 9), "WRITER") == 0) others++;
        FX_CALL(&kf, "TOOLHELP", 64, FARP(s, 0));
    }
    CHECK_EQ(walked, 1);
    CHECK_EQ(others, 0);                                     /* no other instance */
    down();
}

/* A program with one library loaded from a file: the module list KIDSWTCH walks to find the MSKids programs. */
static void test_module_walk(void) {
    kernel_config cfg;
    loader_config lc;
    nb_t t;
    static uint8_t img[NB_MAX];
    char *root = hd_make();
    uint16_t s, lib;
    size_t n;
    printf("test_toolhelp_module_walk\n");
    hd_mkdir(root, "WINDOWS");
    hd_mkdir(root, "WINDOWS/SYSTEM");
    mydll_nb(&t);
    n = nb_build(img, &t);
    hd_write(root, "WINDOWS/SYSTEM/MYDLL.DLL", img, n);
    memset(&cfg, 0, sizeof cfg);
    memset(&lc, 0, sizeof lc);
    cfg.host_root['C' - 'A'] = root;
    cfg.exe_path = "C:\\CWFA\\WRITER.EXE";
    hle_init(&kh);
    kh.stub_unknown = true;
    kk = kernel_create(&cfg, &kh, 0);
    lc.resolve = hle_resolve;
    lc.resolve_ctx = &kh;
    lc.open_module = kernel_open_module;
    lc.open_ctx = kk;
    fx_init(&kf, 0x800, 0x1000);
    CHECK(fx_load(&kf, &lc) == LOADER_OK);
    CHECK(kernel_attach(kk, kf.p));
    th = toolhelp_create(&kh, kk);
    CHECK(th != NULL);
    if (th) CHECK(toolhelp_attach(th, kf.p));
    s = kseg(0x200);
    CHECK_EQ(FX_CALL(&kf, "KERNEL", 95, FARP(kstr("MYDLL"), 0)), LOADER_STOP_HLT);
    lib = fx_ax(&kf);
    CHECK(lib >= 32);
    {   /* ModuleFirst: dwSize is checked like TaskFirst's */
        const hle_entry *e = hle_find(&kh, "TOOLHELP", 59);
        CHECK(e && e->fn && !strcmp(e->name, "ModuleFirst") && e->arg_bytes == 4);
        e = hle_find(&kh, "TOOLHELP", 60);
        CHECK(e && e->fn && !strcmp(e->name, "ModuleNext") && e->arg_bytes == 4);
    }
    memset(kmem(s, 0, 0x200), 0, 0x200);
    CHECK_EQ(FX_CALL(&kf, "TOOLHELP", 59, FARP(s, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 0);
    ((uint8_t *)kmem(s, 0, 4))[0] = 276 & 0xFF;
    ((uint8_t *)kmem(s, 0, 4))[1] = 276 >> 8;
    CHECK_EQ(FX_CALL(&kf, "TOOLHELP", 59, FARP(s, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 1);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK(!strcmp((char *)kmem(s, 4, 10), "WRITER"));              /* szModule */
    CHECK_EQ(rd16(s, 14), kf.p->auto_ds);                         /* hModule */
    CHECK_EQ(rd16(s, 16), 1);                                     /* wcUsage */
    CHECK(!strcmp((char *)kmem(s, 18, 20), "C:\\CWFA\\WRITER.EXE")); /* szExePath */
    CHECK_EQ(rd16(s, 274), lib);                                  /* wNext: the library */
    CHECK_EQ(FX_CALL(&kf, "TOOLHELP", 60, FARP(s, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 1);
    CHECK(!strcmp((char *)kmem(s, 4, 10), "MYDLL"));
    CHECK_EQ(rd16(s, 14), lib);
    CHECK_EQ(rd16(s, 16), 1);
    CHECK(!strncmp((char *)kmem(s, 18, 30), "C:\\WINDOWS\\SYSTEM\\MYDLL.DLL", 26));
    CHECK_EQ(rd16(s, 274), 0);
    CHECK_EQ(FX_CALL(&kf, "TOOLHELP", 60, FARP(s, 0)), LOADER_STOP_HLT);   /* end of the list */
    CHECK_EQ(fx_ax(&kf), 0);
    {   /* TaskFindHandle(entry, task): the one task, nothing else */
        uint16_t task;
        const hle_entry *e = hle_find(&kh, "TOOLHELP", 65);
        CHECK(e && e->fn && !strcmp(e->name, "TaskFindHandle") && e->arg_bytes == 6);
        CHECK_EQ(fx_call_words(&kf, "KERNEL", 36, (uint16_t[]){0}, 0), LOADER_STOP_HLT);
        task = fx_ax(&kf);
        memset(kmem(s, 0, 0x100), 0, 0x100);
        ((uint8_t *)kmem(s, 0, 4))[0] = TASKENTRY_SIZE;
        CHECK_EQ(FX_CALL(&kf, "TOOLHELP", 65, FARP(s, 0), task), LOADER_STOP_HLT);
        CHECK_EQ(fx_ax(&kf), 1);
        CHECK_EQ(kf.sp_after, kf.sp_before);
        CHECK_EQ(rd16(s, 4), task);
        CHECK(!strcmp((char *)kmem(s, 26, 9), "WRITER"));
        CHECK_EQ(FX_CALL(&kf, "TOOLHELP", 65, FARP(s, 0), task + 8), LOADER_STOP_HLT);
        CHECK_EQ(fx_ax(&kf), 0);
        /* KIDSWTCH's TASKENTRY is 40 bytes (word-aligned szModule tail): the same call, the tail one byte later */
        memset(kmem(s, 0, 0x100), 0, 0x100);
        ((uint8_t *)kmem(s, 0, 4))[0] = 40;
        CHECK_EQ(FX_CALL(&kf, "TOOLHELP", 65, FARP(s, 0), task), LOADER_STOP_HLT);
        CHECK_EQ(fx_ax(&kf), 1);
        CHECK_EQ(rd16(s, 10), kf.p->auto_ds);                     /* hModule */
        CHECK(!strcmp((char *)kmem(s, 26, 9), "WRITER"));
        CHECK_EQ(rd16(s, 36), 0x100);                             /* wPSPOffset */
        CHECK_EQ(rd16(s, 38), 0);                                 /* hNext */
        ((uint8_t *)kmem(s, 0, 4))[0] = 38;                       /* neither layout */
        CHECK_EQ(FX_CALL(&kf, "TOOLHELP", 65, FARP(s, 0), task), LOADER_STOP_HLT);
        CHECK_EQ(fx_ax(&kf), 0);
    }
    down();
    hd_remove(root);
}

int main(void) {
    test_module_walk();
    test_ordinals();
    test_task_first_next();
    test_single_instance_walk();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
