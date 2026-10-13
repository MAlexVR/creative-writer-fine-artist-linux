#ifndef LOADER_H
#define LOADER_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu.h"
#include "mem.h"
#include "ne.h"

/* Win16 process image: an NE module loaded into guest memory (mem.h) and wired
 * to the CPU core (cpu.h).
 *
 * Every NE segment gets one LDT selector. Imports resolve through a host
 * resolver to a thunk: a far address in a dedicated code segment that is a CPU
 * hook. When the guest far-calls it the registered handler runs on the host and
 * the thunk then performs the Pascal return (RETF n) using the handler's
 * declared argument byte count. Floating point OS fixups (FIARQQ etc.) are
 * recorded in proc->osfixups and the code bytes are left as the linker wrote
 * them (WAIT + x87 escape), which the FPU support executes directly. */

typedef enum {
    LOADER_OK = 0,
    LOADER_ERR_ARG,
    LOADER_ERR_NOMEM,
    LOADER_ERR_SEGMENT,   /* cannot allocate or fill a segment, or no entry/stack segment */
    LOADER_ERR_BAD_RELOC, /* malformed relocation: bad chain, target or site range */
    LOADER_ERR_THUNKS,    /* thunk segment or CPU hook table full */
    LOADER_ERR_RESOLVE,   /* a resolver returned an invalid registration */
    LOADER_ERR_NOTFOUND,  /* LoadLibrary: no such module file (Windows error 2) */
    LOADER_ERR_FORMAT,    /* LoadLibrary: the file is not an NE module (Windows error 11) */
    LOADER_ERR_INIT       /* a library's entry point failed or faulted */
} loader_err;

typedef enum {
    LOADER_STOP_NONE = 0,
    LOADER_STOP_HLT,        /* guest executed HLT */
    LOADER_STOP_REQUESTED,  /* an API handler called loader_request_stop */
    LOADER_STOP_UNRESOLVED, /* guest called an import the resolver rejected */
    LOADER_STOP_BUDGET,     /* instruction budget exhausted */
    LOADER_STOP_FAULT,      /* CPU fault, status in proc->fault */
    LOADER_STOP_INIT        /* a library the program depends on failed to initialize */
} loader_stop;

typedef struct {
    char module[NE_NAME_MAX + 1];
    bool by_name;
    uint16_t ordinal;           /* valid when !by_name */
    char name[NE_NAME_MAX + 1]; /* valid when by_name */
} loader_import;

struct loader_proc;

/* Host side of an API call. Runs with the guest stack as at the far call:
 * SS:SP -> return IP, return CS, then the Pascal arguments (see loader_arg16).
 * Return CPU_OK to let the thunk return to the guest; the handler sets AX/DX.
 * Any other status stops the run and leaves CS:IP on the thunk. A handler that
 * moves CS:IP itself (e.g. to call back into the guest) skips the automatic
 * return. */
typedef cpu_status (*loader_api_fn)(struct loader_proc *p, const loader_import *imp, void *ctx);

/* An import that is a value rather than code (KERNEL __AHINCR, __WINFLAGS,
 * __C000H, ...): relocations get const_off/const_sel instead of a thunk
 * address. const_seg_bytes != 0 makes the loader allocate a zeroed data
 * segment of that size for the selector. fn stays NULL. */
typedef struct {
    loader_api_fn fn;
    uint16_t arg_bytes; /* callee-cleaned Pascal argument bytes */
    void *ctx;
    bool is_const;
    uint16_t const_off, const_sel;
    uint32_t const_seg_bytes;
} loader_api;

/* Fill *out and return true to bind an import; false leaves it unresolved. */
typedef bool (*loader_resolve_fn)(void *ctx, const loader_import *imp, loader_api *out);

typedef struct loader_thunk {
    loader_import imp;
    loader_api api;
    bool resolved;
    uint16_t sel;
    uint16_t off;
    struct loader_proc *proc;
} loader_thunk;

typedef struct {
    uint16_t seg;      /* 0-based NE segment index */
    uint16_t offset;
    uint16_t type;     /* NE OS fixup type: 1 FIARQQ .. 6 FJCRQQ */
} loader_osfixup;

#define LOADER_PATH_MAX 128

struct loader_module;
typedef struct loader_module loader_module;

/* Opens the NE file behind a module name or path, as written in a module
 * reference or given to LoadLibrary. Returns 0 and fills *out (the loader owns
 * it and calls ne_free) and the DOS path of the file, or a Windows error code
 * (2 file not found, 11 bad format). Modules the HLE implements must answer 2. */
typedef int (*loader_open_fn)(void *ctx, const char *spec, ne_image *out, char dos_path[LOADER_PATH_MAX]);

typedef struct {
    loader_resolve_fn resolve; /* NULL: leave every import unresolved */
    void *resolve_ctx;
    const char *cmdline;       /* PSP command tail (lpCmdLine, NUL terminated), up to 127 bytes; NULL = empty */
    loader_open_fn open_module; /* NULL: modules are never loaded from files */
    void *open_ctx;
    /* A library is about to be unloaded: drop whatever refers to its selectors. */
    void (*unload_module)(void *ctx, loader_module *m);
    void *unload_ctx;
} loader_config;

/* A loaded NE module: the program (mods[0]) or a library. The auto data
 * segment of a library is shared by everything that uses it (there is one
 * instance). Imports of a module resolve first against the exports of loaded
 * modules (real code), then against the host resolver. */
struct loader_module {
    const ne_image *ne;
    ne_image *owned;            /* non-NULL when the loader frees the image */
    char name[NE_NAME_MAX + 1]; /* module name from the NE header, upper case */
    char key[16];               /* file base name without extension, upper case */
    char file[16];              /* file name with extension, upper case: what LoadLibrary matches */
    char path[LOADER_PATH_MAX]; /* DOS path of the file, "" when unknown */
    uint16_t handle;            /* HMODULE = HINSTANCE: the auto data selector, else a private one */
    bool own_handle;
    uint16_t *seg_sel;          /* selector of NE segment i+1 */
    bool own_seg_sel;
    uint16_t auto_ds;           /* 0 when the module has no auto data segment */
    uint16_t heap_start, heap_size, stack_size, sp_default;
    unsigned usage;
    bool is_dll, inited;
    loader_module **deps; /* modules whose usage this one holds */
    size_t ndeps;
    loader_module **ref;         /* per module reference (1-based): resolved module or NULL */
    uint8_t *ref_state;          /* 0 unknown, 1 loaded module, 2 not a file module */
};

typedef struct loader_proc {
    mem_t mem;
    cpu_t cpu;
    const ne_image *ne; /* must outlive the process */
    loader_config cfg;

    uint16_t *seg_sel;  /* selector of NE segment i+1 */
    uint16_t auto_ds;   /* hInstance: selector of the auto data segment */
    uint16_t psp_sel;
    uint16_t thunk_sel;
    uint16_t heap_start, heap_size, stack_size; /* local heap and stack inside auto_ds */
    uint16_t sp_default; /* SP when the header says 0 and the stack is in the auto data segment */

    loader_thunk **thunks;
    size_t nthunks, thunk_cap;
    size_t unresolved;  /* thunks without a handler */

    loader_osfixup *osfixups;
    size_t nosfixups;
    size_t reloc_records, reloc_sites; /* relocation records / patched sites */

    loader_module **mods;   /* mods[0] is the program, then libraries in load order */
    size_t nmods, mod_cap;
    uint16_t trap_off;      /* thunk offset where nested guest calls return to */
    unsigned depth, trap_depth;

    loader_stop stop;
    cpu_status fault;
    const loader_thunk *last_call; /* import handled (or reached) last */
    uint64_t api_calls;
} loader_proc;

loader_err loader_create(const ne_image *ne, const loader_config *cfg, loader_proc **out);
void loader_destroy(loader_proc *p);
const char *loader_strerror(loader_err e);

/* Registers a thunk for an import (or returns the existing one) and resolves it
 * through cfg.resolve. The thunk's guest address is thunk->sel:thunk->off. */
loader_err loader_thunk_for(loader_proc *p, const loader_import *imp, loader_thunk **out);
loader_thunk *loader_find_thunk(const loader_proc *p, const loader_import *imp);

/* Run until HLT, fault, a handler stop request or the budget; sets proc->stop. */
loader_stop loader_run(loader_proc *p, uint64_t max_steps);
/* For handlers: stop the run after this call. Returns the status to return. */
cpu_status loader_request_stop(loader_proc *p);
/* Pascal argument at byte offset ofs above the return address (0 = last pushed). */
cpu_status loader_arg16(cpu_t *c, unsigned ofs, uint16_t *v);

const char *loader_strstop(loader_stop s);

/* ---- modules ------------------------------------------------------------- */

/* Case-insensitive match on the module name from the header or the file key. */
loader_module *loader_module_by_name(const loader_proc *p, const char *name);
loader_module *loader_module_by_handle(const loader_proc *p, uint16_t handle);
/* Selector of the module's data segment containing sel, if sel is one of its segments. */
loader_module *loader_module_by_ds(const loader_proc *p, uint16_t ds);
/* Exported entry by name (resident, then non-resident names, case-insensitive)
 * when name != NULL, else by ordinal. */
bool loader_module_proc(const loader_module *m, const char *name, uint16_t ordinal,
                        uint16_t *sel, uint16_t *off);

/* Registers for a nested call of guest code (see loader_call_far). */
typedef struct {
    uint16_t ax, cx, di, si, ds, es;
} loader_callregs;

/* Runs the far procedure cs:ip to its RETF on the current stack (args are the
 * Pascal words, leftmost first; regs may be NULL) and returns DX:AX. Every register is restored on success; after a failure the CPU is left
 * where it stopped and the status (or CPU_HALT with proc->stop set by a stop
 * request) is returned. */
cpu_status loader_call_far(loader_proc *p, uint16_t cs, uint16_t ip, const uint16_t *args, unsigned nargs,
                           const loader_callregs *regs, uint32_t *ret);

/* LoadLibrary: finds the module among the loaded ones (usage + 1) or opens it
 * through cfg.open_module, loads it with its dependencies and runs the entry
 * points that have not run yet. On failure *winerr is the Windows error code;
 * *fault is not CPU_OK when guest code faulted and the run must stop. */
loader_err loader_load_library(loader_proc *p, const char *spec, loader_module **out, unsigned *winerr,
                               cpu_status *fault);
/* FreeLibrary: usage - 1; at 0 the segments are released, with the dependencies.
 * Returns true when the module was unloaded. The program itself is never freed. */
bool loader_free_library(loader_proc *p, loader_module *m);

#endif
