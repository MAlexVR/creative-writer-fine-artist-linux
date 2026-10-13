/* KERNEL HLE: task, modules and miscellany. See kernel.h.
 *
 * Ordinals. WRITER.EXE imports 56 KERNEL ordinals; each is paired with the name
 * in the program's own import table, which makes the table
 * VERIFIED: the 56 names and the 56 ordinals are the same sets. Ordinals only
 * ARTIST.EXE imports (6 LocalReAlloc, 10 LocalSize, 23 LockSegment,
 * 24 UnlockSegment, 86 _lwrite, 134 GetWindowsDirectory) and ordinals no binary
 * imports (marked "UNV" below) come from the Win 3.1 KERNEL export list and are
 * UNVERIFIED against the binaries. */
#include "kernel_priv.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kernel_api.h"

static void copy_str(char *dst, size_t cap, const char *src, const char *dflt) {
    size_t n;
    if (!src) src = dflt;
    n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* ---- task ------------------------------------------------------------- */

static cpu_status k_inittask(hle_call *c) {
    kernel_t *k = c->ctx;
    cpu_t *cpu = c->cpu;
    hle_ret16(c, 1);
    cpu->r[CPU_EBX] = 0x81; /* ES:BX = command tail in the PSP */
    cpu->seg[CPU_ES] = k->p->psp_sel;
    cpu->r[CPU_ECX] = k->p->stack_size;
    cpu->r[CPU_EDX] = 1; /* SW_SHOWNORMAL */
    cpu->r[CPU_EDI] = k->p->auto_ds;
    cpu->r[CPU_ESI] = 0;
    return CPU_OK;
}

static cpu_status k_getversion(hle_call *c) {
    hle_ret32(c, 0x00060A03u);
    return CPU_OK;
}

static cpu_status k_getwinflags(hle_call *c) {
    hle_ret16(c, KERNEL_WINFLAGS);
    return CPU_OK;
}

static cpu_status k_waitevent(hle_call *c) {
    hle_ret16(c, 0);
    return CPU_OK;
}

static cpu_status k_yield(hle_call *c) {
    (void)c;
    return CPU_OK;
}

static cpu_status k_getcurrenttask(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_ret16(c, k->task_sel);
    return CPU_OK;
}

/* WinExec(lpCmdLine, nCmdShow): the first token of the command line is the program (DOS path), the rest its arguments. */
static cpu_status k_winexec(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t sel, off, show;
    char cmd[260], prog[260], host[512];
    const char *rest;
    size_t n;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    show = hle_u16(&a);
    if (a.st) return a.st;
    if (!hle_read_cstr(c, sel, off, cmd, sizeof cmd) || !k->cfg.winexec) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    rest = cmd;
    while (*rest == ' ') rest++;
    n = strcspn(rest, " \t");
    if (n >= sizeof prog) n = sizeof prog - 1;
    memcpy(prog, rest, n);
    prog[n] = 0;
    rest += n;
    while (*rest == ' ' || *rest == '\t') rest++;
    if (!kernel_host_path(k, prog, host, sizeof host)) {
        hle_ret16(c, 2);                                  /* file not found */
        return CPU_OK;
    }
    hle_ret16(c, (uint16_t)k->cfg.winexec(k->cfg.winexec_ctx, host, prog, rest, show));
    return CPU_OK;
}

static cpu_status k_seterrormode(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t mode;
    hle_args_init(&a, c);
    mode = hle_u16(&a);
    if (a.st) return a.st;
    hle_ret16(c, k->errmode);
    k->errmode = mode;
    return CPU_OK;
}

static cpu_status k_makeprocinstance(hle_call *c) {
    hle_args a;
    uint16_t sel, off;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    hle_u16(&a); /* hInstance */
    if (a.st) return a.st;
    hle_ret32(c, (uint32_t)sel << 16 | off); /* the procedure itself: no DS thunk needed */
    return CPU_OK;
}

static cpu_status k_ret_nothing(hle_call *c) {
    (void)c;
    return CPU_OK;
}

static cpu_status k_ret_true(hle_call *c) {
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status k_getdrivetype(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    unsigned drive;
    hle_args_init(&a, c);
    drive = hle_u16(&a);
    if (a.st) return a.st;
    if (drive >= 26 || !k->cfg.host_root[drive]) hle_ret16(c, 0);
    else if ('A' + (int)drive == (k->cfg.cd_drive ? toupper((unsigned char)k->cfg.cd_drive) : 'D'))
        hle_ret16(c, k->cfg.hide_cd_drive ? 0 : 4); /* DRIVE_REMOTE: Windows 3.1 reports CD-ROMs this way */
    else if (drive < 2) hle_ret16(c, 2);
    else hle_ret16(c, 3);
    return CPU_OK;
}

static int strcasecmp_ascii(const char *a, const char *b) {
    for (; *a && toupper((unsigned char)*a) == toupper((unsigned char)*b); a++, b++) {}
    return toupper((unsigned char)*a) - toupper((unsigned char)*b);
}

/* ---- modules ------------------------------------------------------------ */

/* System modules that exist as built-ins: LoadLibrary succeeds for these only. */
static const char *const builtin_modules[] = {
    "KERNEL", "USER", "GDI", "KEYBOARD", "MOUSE", "DISPLAY", "SOUND", "SYSTEM", "TOOLHELP",
    "WIN87EM", "MMSYSTEM", "COMMDLG", "SHELL", "LZEXPAND", "VER", "DDEML", "OLECLI", "OLESVR",
    "WINOLDAP", "STDDRV", "CWFAPRN",
};

static bool is_builtin(const char *key) {
    size_t j;
    for (j = 0; j < sizeof builtin_modules / sizeof builtin_modules[0]; j++)
        if (!strcmp(builtin_modules[j], key)) return true;
    return false;
}

/* "C:\DIR\Name.dll" -> "NAME" */
static void module_key(const char *path, char out[16]) {
    const char *b = path, *q;
    size_t n = 0;
    for (q = path; *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
    for (; b[n] && b[n] != '.' && n < 15; n++) out[n] = (char)toupper((unsigned char)b[n]);
    out[n] = 0;
}

static kmodule *module_by_name(kernel_t *k, const char *key) {
    unsigned i;
    size_t j;
    for (i = 0; i < k->nmods; i++)
        if (!strcmp(k->mods[i].name, key)) return &k->mods[i];
    for (j = 0; j < sizeof builtin_modules / sizeof builtin_modules[0]; j++) {
        kmodule *m;
        if (strcmp(builtin_modules[j], key) || k->nmods == KERNEL_MAX_MODS) continue;
        m = &k->mods[k->nmods];
        m->handle = mem_alloc(&k->p->mem, 16, MEM_DATA);
        if (!m->handle) return NULL;
        strcpy(m->name, key);
        m->usage = 1; /* system modules are resident */
        k->nmods++;
        return m;
    }
    return NULL;
}

static kmodule *module_by_handle(kernel_t *k, uint16_t h) {
    unsigned i;
    for (i = 0; i < k->nmods; i++)
        if (k->mods[i].handle == h) return &k->mods[i];
    return NULL;
}

/* The library files behind a name: the DLL itself, or the first hit along the module path. */
int kernel_open_module(void *kernel, const char *spec, ne_image *out, char dos_path[LOADER_PATH_MAX]) {
    kernel_t *k = kernel;
    char key[16], name[DP_DOS_MAX], dir[DP_DOS_MAX], cand[DP_DOS_MAX * 2];
    const char *base = spec, *q, *list;
    bool has_path = false, nodir = false, any = false;
    int bad = 0;
    for (q = spec; *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':') {
            base = q + 1;
            has_path = true;
        }
    module_key(spec, key);
    if (!key[0] || is_builtin(key)) return 2;
    snprintf(name, sizeof name, "%s%s", spec, strchr(base, '.') ? "" : ".DLL");
    list = has_path ? "" : k->module_path;
    for (;;) {
        dp_result r;
        size_t n = strcspn(list, ";");
        if (has_path) {
            snprintf(cand, sizeof cand, "%s", name);
        } else if (n) {
            snprintf(dir, sizeof dir, "%.*s", (int)(n < sizeof dir - 1 ? n : sizeof dir - 1), list);
            snprintf(cand, sizeof cand, "%s%s%s", dir, dir[strlen(dir) - 1] == '\\' ? "" : "\\", name);
        } else {
            cand[0] = 0;
        }
        if (cand[0]) {
            if (!dp_resolve(&k->dp, cand, 0, &r)) {
                nodir = true;
            } else if (r.exists && !r.is_dir) {
                ne_err e = ne_open_file(out, r.host);
                any = true;
                if (e == NE_OK) {
                    snprintf(dos_path, LOADER_PATH_MAX, "%s", r.dos);
                    return 0;
                }
                bad = 11;
            }
        }
        if (has_path || !list[n]) break;
        list += n + 1;
    }
    (void)any;
    return bad ? bad : (has_path && nodir) ? 3 : 2;
}

/* A library is being unloaded: nothing may keep pointing at its selectors. */
static void k_unload(void *ctx, loader_module *m) {
    kernel_t *k = ctx;
    if (m->auto_ds) kernel_heap_drop(k, m->auto_ds);
    kernel_res_drop(k, m->handle);
}

/* The loaded library a module name or file name refers to (never the program). */
static loader_module *library_by_name(kernel_t *k, const char *spec) {
    char key[16];
    const char *b = spec, *q;
    size_t i;
    for (q = spec; *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
    module_key(spec, key);
    for (i = 1; i < k->p->nmods; i++) {
        const loader_module *m = k->p->mods[i];
        if (strchr(b, '.') && !strcasecmp_ascii(m->file, b)) return k->p->mods[i];
    }
    for (i = 1; i < k->p->nmods; i++)
        if (!strcmp(k->p->mods[i]->name, key) || !strcmp(k->p->mods[i]->key, key)) return k->p->mods[i];
    return NULL;
}

static cpu_status k_getmodulehandle(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t sel, off;
    char path[128], key[16];
    kmodule *m;
    loader_module *lm;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    if (a.st) return a.st;
    if (sel == 0 || !hle_read_cstr(c, sel, off, path, sizeof path)) { /* NULL: the caller's own module */
        hle_ret16(c, k->p->auto_ds);
        return CPU_OK;
    }
    module_key(path, key);
    if ((lm = library_by_name(k, path))) hle_ret16(c, lm->handle);
    else if (!strcmp(key, k->module_name)) hle_ret16(c, k->p->auto_ds);
    else hle_ret16(c, (m = module_by_name(k, key)) ? m->handle : 0);
    return CPU_OK;
}

static cpu_status k_loadlibrary(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t sel, off;
    char path[128], key[16];
    kmodule *m;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    if (a.st) return a.st;
    if (!hle_read_cstr(c, sel, off, path, sizeof path)) return CPU_E_SEL;
    module_key(path, key);
    if (is_builtin(key)) {
        m = module_by_name(k, key);
        if (m) m->usage++;
        hle_ret16(c, m ? m->handle : 2); /* 2 = file not found (< 32 is an error code) */
    } else {
        loader_module *lm;
        unsigned winerr;
        cpu_status fault;
        loader_err e = loader_load_library(k->p, path, &lm, &winerr, &fault);
        if (fault != CPU_OK) return fault;
        if (e == LOADER_OK && k->cfg.debug) fprintf(k->cfg.debug, "LoadLibrary(%s) = %04X (%s)\n", path, lm->handle, lm->path);
        else if (e != LOADER_OK && k->cfg.debug) fprintf(k->cfg.debug, "LoadLibrary(%s) failed: error %u\n", path, winerr);
        hle_ret16(c, e == LOADER_OK ? lm->handle : (uint16_t)winerr);
    }
    return CPU_OK;
}

static cpu_status k_freelibrary(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t h;
    kmodule *m;
    loader_module *lm;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    if (a.st) return a.st;
    if ((lm = loader_module_by_handle(k->p, h)) && lm != k->p->mods[0]) loader_free_library(k->p, lm);
    else if ((m = module_by_handle(k, h)) && m->usage > 1) m->usage--;
    return CPU_OK;
}

static cpu_status k_getmoduleusage(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t h;
    kmodule *m;
    loader_module *lm;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    if (a.st) return a.st;
    m = module_by_handle(k, h);
    lm = loader_module_by_handle(k->p, h);
    hle_ret16(c, h == k->p->auto_ds ? 1 : lm ? (uint16_t)lm->usage : m ? (uint16_t)m->usage : 0);
    return CPU_OK;
}

static cpu_status k_getmodulefilename(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t h, sel, off, size;
    char path[160];
    kmodule *m;
    loader_module *lm;
    uint8_t *dst;
    size_t n;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    hle_far(&a, &sel, &off);
    size = hle_u16(&a);
    if (a.st) return a.st;
    if (h == k->p->auto_ds) {
        snprintf(path, sizeof path, "%s", k->exe_path);
    } else if ((lm = loader_module_by_handle(k->p, h))) {
        snprintf(path, sizeof path, "%s", lm->path);
    } else if ((m = module_by_handle(k, h))) {
        snprintf(path, sizeof path, "%.100s\\SYSTEM\\%s.%s", k->windows_dir, m->name,
                 !strcmp(m->name, "KERNEL") || !strcmp(m->name, "USER") || !strcmp(m->name, "GDI")
                     ? "EXE" : "DLL");
    } else {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (size == 0) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    n = strlen(path);
    if (n > (size_t)size - 1) n = size - 1;
    if (!(dst = hle_ptr(c, sel, off, (uint32_t)n + 1))) return CPU_E_SEL;
    memcpy(dst, path, n);
    dst[n] = 0;
    hle_ret16(c, (uint16_t)n);
    return CPU_OK;
}

static cpu_status k_getprocaddress(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t h, sel, off, ordinal = 0;
    char name[128];
    const hle_entry *e;
    kmodule *m;
    loader_module *lm;
    loader_import imp;
    loader_thunk *t;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    hle_far(&a, &sel, &off);
    if (a.st) return a.st;
    hle_ret32(c, 0);
    name[0] = 0;
    if (sel == 0) ordinal = off;
    else if (!hle_read_cstr(c, sel, off, name, sizeof name)) return CPU_OK;
    if ((lm = loader_module_by_handle(k->p, h))) { /* the program's or a library's exported entries: real code */
        uint16_t rs, ro;
        if (loader_module_proc(lm, name[0] ? name : NULL, ordinal, &rs, &ro)) hle_ret32(c, (uint32_t)rs << 16 | ro);
        return CPU_OK;
    }
    if (!(m = module_by_handle(k, h))) return CPU_OK;
    e = name[0] ? hle_find_name(k->hle, m->name, name) : hle_find(k->hle, m->name, ordinal);
    if (!e || e->is_const) return CPU_OK;
    memset(&imp, 0, sizeof imp);
    snprintf(imp.module, sizeof imp.module, "%s", m->name);
    imp.ordinal = e->ordinal;
    if (loader_thunk_for(k->p, &imp, &t) != LOADER_OK) return CPU_OK;
    hle_ret32(c, (uint32_t)t->sel << 16 | t->off);
    return CPU_OK;
}

/* ---- Catch / Throw ---------------------------------------------------------
 * CATCHBUF is 9 words: return IP, return CS, SP after the Catch call returns,
 * BP, SI, DI, DS, SS, reserved. */

static cpu_status k_catch(hle_call *c) {
    cpu_t *cpu = c->cpu;
    hle_args a;
    uint16_t sel, off, ip, cs, sp = (uint16_t)cpu->r[CPU_ESP];
    uint16_t *w;
    unsigned i;
    uint8_t *p;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    if (a.st) return a.st;
    ip = c->caller_ip;
    cs = c->caller_cs;
    w = (uint16_t[9]){ip, cs, (uint16_t)(sp + 4 + c->entry->arg_bytes), (uint16_t)cpu->r[CPU_EBP],
                      (uint16_t)cpu->r[CPU_ESI], (uint16_t)cpu->r[CPU_EDI], cpu->seg[CPU_DS],
                      cpu->seg[CPU_SS], 0};
    if (!(p = hle_ptr(c, sel, off, 18))) return CPU_E_SEL;
    for (i = 0; i < 9; i++) {
        p[2 * i] = (uint8_t)w[i];
        p[2 * i + 1] = (uint8_t)(w[i] >> 8);
    }
    hle_ret16(c, 0);
    return CPU_OK;
}

static cpu_status k_throw(hle_call *c) {
    cpu_t *cpu = c->cpu;
    hle_args a;
    uint16_t sel, off, val, w[9];
    const uint8_t *p;
    unsigned i;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    val = hle_u16(&a);
    if (a.st) return a.st;
    if (!(p = hle_ptr(c, sel, off, 18))) return CPU_E_SEL;
    for (i = 0; i < 9; i++) w[i] = (uint16_t)(p[2 * i] | p[2 * i + 1] << 8);
    if (!mem_valid(&c->proc->mem, w[1]) || !mem_valid(&c->proc->mem, w[6]) ||
        !mem_valid(&c->proc->mem, w[7]))
        return CPU_E_GP;
    cpu->eip = w[0];
    cpu->seg[CPU_CS] = w[1];
    cpu->r[CPU_ESP] = w[2];
    cpu->r[CPU_EBP] = w[3];
    cpu->r[CPU_ESI] = w[4];
    cpu->r[CPU_EDI] = w[5];
    cpu->seg[CPU_DS] = w[6];
    cpu->seg[CPU_SS] = w[7];
    cpu->r[CPU_EAX] = val;
    return CPU_OK;
}

/* ---- debug output ------------------------------------------------------ */

static cpu_status k_outputdebugstring(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t sel, off;
    char s[256];
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    if (a.st) return a.st;
    if (hle_read_cstr(c, sel, off, s, sizeof s) && k->cfg.debug) fprintf(k->cfg.debug, "%s", s);
    return CPU_OK;
}

static cpu_status k_fatalexit(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t code;
    hle_args_init(&a, c);
    code = hle_u16(&a);
    if (k->cfg.debug) fprintf(k->cfg.debug, "FatalExit: code %04X\n", code);
    return loader_request_stop(c->proc);
}

static cpu_status k_fatalappexit(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t action, sel, off;
    char s[256];
    hle_args_init(&a, c);
    action = hle_u16(&a);
    hle_far(&a, &sel, &off);
    if (a.st) return a.st;
    if (!hle_read_cstr(c, sel, off, s, sizeof s)) s[0] = 0;
    if (k->cfg.debug) fprintf(k->cfg.debug, "FatalAppExit(%u): %s\n", action, s);
    return loader_request_stop(c->proc);
}

/* ---- DOS environment ---------------------------------------------------- */

/* The environment block: NAME=value strings ended by an empty string, then a
 * word count and the program path. Its selector is also stored at PSP:2C. */
static bool ensure_env(kernel_t *k) {
    static const char vars[] = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\WINDOWS;C:\\\0";
    size_t n = sizeof vars - 1, plen = strlen(k->exe_path);
    uint8_t *e, *psp;
    if (k->env_sel) return true;
    k->env_sel = mem_alloc(&k->p->mem, (uint32_t)(n + 1 + 2 + plen + 1), MEM_DATA);
    e = k->env_sel ? mem_ptr(&k->p->mem, k->env_sel, 0, 1) : NULL;
    if (!e) return false;
    memcpy(e, vars, n);
    e[n + 1] = 1;
    memcpy(e + n + 3, k->exe_path, plen + 1);
    if ((psp = mem_ptr(&k->p->mem, k->p->psp_sel, 0x2C, 2))) {
        psp[0] = (uint8_t)k->env_sel;
        psp[1] = (uint8_t)(k->env_sel >> 8);
    }
    return true;
}

static cpu_status k_getdosenvironment(hle_call *c) {
    kernel_t *k = c->ctx;
    if (!ensure_env(k)) return CPU_E_SEL;
    hle_ret32(c, (uint32_t)k->env_sel << 16);
    return CPU_OK;
}

/* ---- registration -------------------------------------------------------- */

#define K "KERNEL"
static const hle_def kernel_defs[] = {
    {K, 1, "FatalExit", 2, k_fatalexit},
    {K, 3, "GetVersion", 0, k_getversion},
    {K, 4, "LocalInit", 6, k_localinit},               /* UNV */
    {K, 5, "LocalAlloc", 4, k_localalloc},
    {K, 6, "LocalReAlloc", 6, k_localrealloc},            /* ARTIST only */
    {K, 7, "LocalFree", 2, k_localfree},
    {K, 8, "LocalLock", 2, k_locallock},
    {K, 9, "LocalUnlock", 2, k_localunlock},
    {K, 10, "LocalSize", 2, k_localsize},              /* ARTIST only */
    {K, 11, "LocalHandle", 2, k_localhandle},
    {K, 12, "LocalFlags", 2, k_localflags},             /* UNV */
    {K, 13, "LocalCompact", 2, k_localcompact},           /* UNV */
    {K, 15, "GlobalAlloc", 6, k_globalalloc},
    {K, 16, "GlobalReAlloc", 8, k_globalrealloc},
    {K, 17, "GlobalFree", 2, k_globalfree},
    {K, 18, "GlobalLock", 2, k_globallock},
    {K, 19, "GlobalUnlock", 2, k_globalunlock},
    {K, 20, "GlobalSize", 2, k_globalsize},
    {K, 21, "GlobalHandle", 2, k_globalhandle},
    {K, 22, "GlobalFlags", 2, k_globalflags},            /* UNV */
    {K, 23, "LockSegment", 2, k_locksegment},            /* ARTIST only */
    {K, 24, "UnlockSegment", 2, k_unlocksegment},          /* ARTIST only */
    {K, 25, "GlobalCompact", 4, k_globalcompact},          /* UNV */
    {K, 29, "Yield", 0, k_yield},               /* UNV */
    {K, 30, "WaitEvent", 2, k_waitevent},
    {K, 36, "GetCurrentTask", 0, k_getcurrenttask},
    {K, 47, "GetModuleHandle", 4, k_getmodulehandle},
    {K, 166, "WinExec", 6, k_winexec},                       /* KIDSWTCH: FSwitchToFspApp */
    {K, 48, "GetModuleUsage", 2, k_getmoduleusage},         /* UNV */
    {K, 49, "GetModuleFileName", 8, k_getmodulefilename},
    {K, 50, "GetProcAddress", 6, k_getprocaddress},
    {K, 51, "MakeProcInstance", 6, k_makeprocinstance},
    {K, 52, "FreeProcInstance", 4, k_ret_nothing},
    {K, 55, "Catch", 4, k_catch},
    {K, 56, "Throw", 6, k_throw},
    {K, 57, "GetProfileInt", 10, k_getprofileint},
    {K, 58, "GetProfileString", 18, k_getprofilestring},
    {K, 59, "WriteProfileString", 12, k_writeprofilestring},
    {K, 60, "FindResource", 10, k_findresource},
    {K, 61, "LoadResource", 4, k_loadresource},
    {K, 62, "LockResource", 2, k_globallock},
    {K, 63, "FreeResource", 2, k_freeresource},
    {K, 64, "AccessResource", 4, k_accessresource},         /* UNV */
    {K, 65, "SizeofResource", 4, k_sizeofresource},         /* UNV */
    {K, 66, "AllocResource", 8, k_allocresource},          /* UNV */
    {K, 74, "OpenFile", 10, k_openfile},
    {K, 81, "_lclose", 2, k_lclose},
    {K, 82, "_lread", 8, k_lread},
    {K, 83, "_lcreat", 6, k_lcreat},
    {K, 84, "_llseek", 8, k_llseek},
    {K, 85, "_lopen", 6, k_lopen},
    {K, 86, "_lwrite", 8, k_lwrite},                /* ARTIST only */
    {K, 88, "lstrcpy", 8, k_lstrcpy},
    {K, 89, "lstrcat", 8, k_lstrcat},
    {K, 90, "lstrlen", 4, k_lstrlen},
    {K, 91, "InitTask", 0, k_inittask},
    {K, 94, "DefineHandleTable", 2, k_ret_true},/* UNV */
    {K, 95, "LoadLibrary", 4, k_loadlibrary},
    {K, 96, "FreeLibrary", 2, k_freelibrary},
    {K, 97, "GetTempFileName", 12, k_gettempfilename},
    {K, 102, "Dos3Call", 0, k_dos3call},
    {K, 107, "SetErrorMode", 2, k_seterrormode},
    {K, 115, "OutputDebugString", 4, k_outputdebugstring},
    {K, 127, "GetPrivateProfileInt", 14, k_getprivateprofileint},   /* UNV */
    {K, 128, "GetPrivateProfileString", 22, k_getprivateprofilestring},/* UNV */
    {K, 129, "WritePrivateProfileString", 16, k_writeprivateprofilestring}, /* UNV */
    {K, 131, "GetDOSEnvironment", 0, k_getdosenvironment},
    {K, 132, "GetWinFlags", 0, k_getwinflags},
    {K, 134, "GetWindowsDirectory", 6, k_getwindowsdirectory},   /* ARTIST only */
    {K, 135, "GetSystemDirectory", 6, k_getsystemdirectory},    /* UNV */
    {K, 136, "GetDriveType", 2, k_getdrivetype},
    {K, 137, "FatalAppExit", 6, k_fatalappexit},
    {K, 163, "GlobalLRUOldest", 2, k_globallru},       /* UNV */
    {K, 164, "GlobalLRUNewest", 2, k_globallru},       /* UNV */
    {K, 169, "GetFreeSpace", 2, k_getfreespace},
    {K, 348, "hmemcpy", 12, k_hmemcpy},              /* UNV */
    {K, 349, "_hread", 10, k_hread},
    {K, 350, "_hwrite", 10, k_hwrite},              /* UNV */
};
#undef K

kernel_t *kernel_create(const kernel_config *cfg, hle_t *h, unsigned flags) {
    kernel_t *k = calloc(1, sizeof *k);
    char *dot;
    const char *base;
    if (!k) return NULL;
    k->hle = h;
    if (cfg) k->cfg = *cfg;
    copy_str(k->exe_path, sizeof k->exe_path, k->cfg.exe_path, "C:\\CWFA\\APP.EXE");
    copy_str(k->windows_dir, sizeof k->windows_dir, k->cfg.windows_dir, "C:\\WINDOWS");
    copy_str(k->cur_dir, sizeof k->cur_dir, k->cfg.cur_dir, "C:\\");
    base = strrchr(k->exe_path, '\\');
    copy_str(k->module_name, sizeof k->module_name, base ? base + 1 : k->exe_path, "");
    if ((dot = strchr(k->module_name, '.'))) *dot = 0;
    for (dot = k->module_name; *dot; dot++) *dot = (char)toupper((unsigned char)*dot);
    if (k->cfg.module_path) {
        copy_str(k->module_path, sizeof k->module_path, k->cfg.module_path, "");
    } else {
        char dir[128];
        size_t n = base ? (size_t)(base - k->exe_path) : 0;
        snprintf(dir, sizeof dir, "%.*s", (int)n, k->exe_path);
        snprintf(k->module_path, sizeof k->module_path, "%.120s;%.120s;%.120s\\SYSTEM;%.120s",
                 n ? dir : k->cur_dir, k->cur_dir, k->windows_dir, k->windows_dir);
    }
    dp_init(&k->dp);
    for (int i = 0; i < 26; i++)
        if (k->cfg.host_root[i]) dp_map(&k->dp, (char)('A' + i), k->cfg.host_root[i]);
    if (isalpha((unsigned char)k->cur_dir[0]) && k->cur_dir[1] == ':') {
        k->dp.cur_drive = (char)toupper((unsigned char)k->cur_dir[0]);
        dp_chdir(&k->dp, k->cur_dir);
    }
    k->t0_ms = 0;
    k->t0_ms = kernel_now_ms(k);
    if (!hle_register_table(h, kernel_defs, sizeof kernel_defs / sizeof kernel_defs[0], k) ||
        ((flags & KERNEL_REG_USER_HELPERS) && !hle_register_table(h, kernel_user_defs, kernel_user_ndefs, k)) ||
        !hle_register_const(h, "KERNEL", 113, "__AHSHIFT", 3, 0) ||
        !hle_register_const(h, "KERNEL", 114, "__AHINCR", MEM_AHINCR, 0) ||
        !hle_register_const(h, "KERNEL", 178, "__WINFLAGS", KERNEL_WINFLAGS, 0) ||
        !hle_register_const(h, "KERNEL", 195, "__C000H", 0, 0x8000)) {
        free(k);
        return NULL;
    }
    return k;
}

void kernel_destroy(kernel_t *k) {
    if (!k) return;
    kernel_heap_destroy(k);
    kernel_res_destroy(k);
    kernel_file_destroy(k);
    free(k);
}

/* Instance data at the start of the auto data segment: the C runtime reads the
 * local heap and stack limits from here. */
static void write_instance_data(kernel_t *k) {
    loader_proc *p = k->p;
    uint8_t *ds = mem_ptr(&p->mem, p->auto_ds, 0, 0x10);
    uint16_t top = p->sp_default, bottom = (uint16_t)(top - p->stack_size);
    uint16_t heap = p->heap_size ? p->heap_start : 0;
    if (!ds) return;
    ds[6] = (uint8_t)heap; ds[7] = (uint8_t)(heap >> 8);
    ds[0x0A] = (uint8_t)top; ds[0x0B] = (uint8_t)(top >> 8);
    ds[0x0C] = (uint8_t)top; ds[0x0D] = (uint8_t)(top >> 8);
    ds[0x0E] = (uint8_t)bottom; ds[0x0F] = (uint8_t)(bottom >> 8);
}

/* INT 21h from the program itself (C runtime _dos_ calls, int86x): same service as Dos3Call. */
static cpu_status kernel_int(cpu_t *cpu, uint8_t vec, void *ctx) {
    kernel_t *k = ctx;
    hle_call call;
    if (vec != 0x21) return CPU_E_INT;
    memset(&call, 0, sizeof call);
    call.proc = k->p;
    call.cpu = cpu;
    call.ctx = k;
    return k_dos3call(&call);
}

bool kernel_attach(kernel_t *k, loader_proc *p) {
    k->p = p;
    p->cfg.unload_module = k_unload;
    p->cfg.unload_ctx = k;
    cpu_set_int_handler(&p->cpu, kernel_int, k);
    k->task_sel = mem_alloc(&p->mem, 0x100, MEM_DATA);
    if (!k->task_sel) return false;
    write_instance_data(k);
    return ensure_env(k) && kernel_heap_attach(k);
}

uint16_t kernel_task(const kernel_t *k) { return k->task_sel; }
const char *kernel_module_name(const kernel_t *k) { return k->module_name; }
const char *kernel_exe_path(const kernel_t *k) { return k->exe_path; }

bool kernel_host_path(kernel_t *k, const char *dos, char *host, size_t cap) {
    dp_result r;
    if (!dp_resolve(&k->dp, dos, 0, &r) || !r.exists || r.is_dir || strlen(r.host) >= cap) return false;
    strcpy(host, r.host);
    return true;
}
