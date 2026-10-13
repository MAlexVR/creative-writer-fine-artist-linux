/* TOOLHELP HLE. See toolhelp.h. */
#include "toolhelp.h"

#include <stdlib.h>
#include <string.h>

#define TASKENTRY_SIZE 39u      /* packed; KIDSWTCH's copy of the struct is 40: wPSPOffset and hNext word-aligned */
#define TASKENTRY_SIZE_ALIGNED 40u
#define MODULEENTRY_SIZE 276u

struct toolhelp {
    kernel_t *k;
    loader_proc *p;
};

/* At most max bytes of s, the rest of the (already zeroed) field stays NUL. */
static void put_str(uint8_t *dst, const char *s, size_t max) {
    size_t n = strlen(s);
    memcpy(dst, s, n < max ? n : max);
}

static void st16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static uint32_t entry_size(const uint8_t *e) {
    return (uint32_t)(e[0] | e[1] << 8 | e[2] << 16 | (uint32_t)e[3] << 24);
}

/* TASKENTRY: dwSize, hTask, hTaskParent, hInst, hModule, wSS, wSP, wStackTop, wStackMinimum, wStackBottom, wcEvents,
 * hQueue, szModule[9], wPSPOffset, hNext. */
static bool task_size_ok(const uint8_t *e) {
    uint32_t n = entry_size(e);
    return n == TASKENTRY_SIZE || n == TASKENTRY_SIZE_ALIGNED;
}

static void fill_task(toolhelp_t *t, uint8_t *e) {
    loader_proc *p = t->p;
    uint32_t size = entry_size(e);
    uint16_t top = p->sp_default, bottom = (uint16_t)(top - p->stack_size);
    memset(e + 4, 0, size - 4);
    st16(e + 4, kernel_task(t->k));             /* hTask */
    st16(e + 8, p->auto_ds);                    /* hInst */
    st16(e + 10, p->auto_ds);                   /* hModule */
    st16(e + 12, p->cpu.seg[CPU_SS]);
    st16(e + 14, (uint16_t)p->cpu.r[CPU_ESP]);
    st16(e + 16, top);
    st16(e + 18, bottom);
    st16(e + 20, bottom);
    put_str(e + 26, kernel_module_name(t->k), 8);
    st16(e + (size == TASKENTRY_SIZE ? 35 : 36), 0x100); /* the PSP follows the 256 byte task header */
}

static cpu_status task_first(hle_call *c) {
    toolhelp_t *t = c->ctx;
    hle_args a;
    uint16_t sel, off;
    uint8_t *e;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    if (a.st) return a.st;
    if (!(e = hle_ptr(c, sel, off, TASKENTRY_SIZE))) return CPU_E_SEL;
    if (!task_size_ok(e) || !hle_ptr(c, sel, off, entry_size(e))) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    fill_task(t, e);
    hle_ret16(c, 1);
    return CPU_OK;
}

/* TaskFindHandle(lpte, hTask): the entry of one task; the runtime has one. */
static cpu_status task_find_handle(hle_call *c) {
    toolhelp_t *t = c->ctx;
    hle_args a;
    uint16_t sel, off, task;
    uint8_t *e;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    task = hle_u16(&a);
    if (a.st) return a.st;
    if (!(e = hle_ptr(c, sel, off, TASKENTRY_SIZE))) return CPU_E_SEL;
    if (!task_size_ok(e) || !hle_ptr(c, sel, off, entry_size(e)) || task != kernel_task(t->k)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    fill_task(t, e);
    hle_ret16(c, 1);
    return CPU_OK;
}

/* MODULEENTRY: dwSize, szModule[10], hModule, wcUsage, szExePath[256], wNext. The modules are the program and the
 * libraries loaded from files, in load order; the built-in system modules are not listed. */
static void fill_module(toolhelp_t *t, size_t i, uint8_t *e) {
    const loader_module *m = t->p->mods[i];
    const char *path = i == 0 ? kernel_exe_path(t->k) : m->path;
    memset(e + 4, 0, MODULEENTRY_SIZE - 4);
    put_str(e + 4, i == 0 ? kernel_module_name(t->k) : m->name, 9);
    st16(e + 14, m->handle);
    st16(e + 16, (uint16_t)m->usage);
    put_str(e + 18, path, 255);
    st16(e + 274, i + 1 < t->p->nmods ? t->p->mods[i + 1]->handle : 0);
}

static cpu_status module_walk(hle_call *c, bool next) {
    toolhelp_t *t = c->ctx;
    hle_args a;
    uint16_t sel, off;
    uint8_t *e;
    size_t i = 0;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    if (a.st) return a.st;
    if (!(e = hle_ptr(c, sel, off, MODULEENTRY_SIZE))) return CPU_E_SEL;
    hle_ret16(c, 0);
    if (entry_size(e) != MODULEENTRY_SIZE) return CPU_OK;
    if (next) {
        uint16_t h = (uint16_t)(e[14] | e[15] << 8);
        for (i = 0; i < t->p->nmods && t->p->mods[i]->handle != h; i++) {}
        if (++i >= t->p->nmods) return CPU_OK;      /* unknown module, or the last one */
    }
    fill_module(t, i, e);
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status module_first(hle_call *c) { return module_walk(c, false); }
static cpu_status module_next(hle_call *c) { return module_walk(c, true); }

static cpu_status task_next(hle_call *c) {
    hle_args a;
    uint16_t sel, off;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    if (a.st) return a.st;
    if (!hle_ptr(c, sel, off, TASKENTRY_SIZE)) return CPU_E_SEL;
    hle_ret16(c, 0);                            /* the one task is the whole list */
    return CPU_OK;
}

toolhelp_t *toolhelp_create(hle_t *h, kernel_t *k) {
    static const hle_def defs[] = {
        {"TOOLHELP", 59, "ModuleFirst", 4, module_first},
        {"TOOLHELP", 60, "ModuleNext", 4, module_next},
        {"TOOLHELP", 63, "TaskFirst", 4, task_first},
        {"TOOLHELP", 65, "TaskFindHandle", 6, task_find_handle},
        {"TOOLHELP", 64, "TaskNext", 4, task_next},
    };
    toolhelp_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->k = k;
    if (!hle_register_table(h, defs, sizeof defs / sizeof defs[0], t)) {
        free(t);
        return NULL;
    }
    return t;
}

void toolhelp_destroy(toolhelp_t *t) { free(t); }

bool toolhelp_attach(toolhelp_t *t, loader_proc *p) {
    t->p = p;
    return true;
}
