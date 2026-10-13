#ifndef KERNEL_PRIV_H
#define KERNEL_PRIV_H
/* Internal to the kernel_*.c files. */
#include "dospath.h"
#include "kernel.h"

#define KERNEL_MODULE "KERNEL"
#define KERNEL_MAX_MODS 32

typedef struct {
    char name[16]; /* upper case, no extension */
    uint16_t handle;
    int usage;
} kmodule;

typedef struct {
    uint8_t used, lock;
    uint16_t flags;   /* GMEM_DISCARDABLE and friends */
    uint32_t size;    /* requested size; the selector holds at least 1 byte */
} gblock;

/* One block of a local heap, host side. kind 2 is the 4-byte handle entry
 * (word pointer, byte flags, byte lock count) of a movable block (kind 1). */
typedef struct {
    uint32_t off, size;
    uint16_t req;
    uint16_t hnd;     /* kind 1: offset of its handle entry */
    uint8_t used, kind, lock, flags;
} lblock;

typedef struct {
    uint16_t sel;
    uint32_t start, end;
    lblock *b;
    unsigned n, cap;
} lheap;

#define KERNEL_MAX_HEAPS 32
#define KERNEL_MAX_FILES 40
#define KERNEL_GLOBAL_LIMIT 0x04000000u /* bytes GlobalAlloc may hand out in total */

typedef struct {
    const ne_resource *r;
    const ne_image *ne;  /* the module's image; NULL once that library is unloaded */
    uint16_t inst;       /* the module's handle */
    uint16_t h;     /* global handle while loaded */
    unsigned refs;  /* LoadResource calls not yet matched by FreeResource */
} kres;

struct kernel {
    hle_t *hle;
    loader_proc *p;
    kernel_config cfg;
    char exe_path[128], windows_dir[128], cur_dir[128], module_name[16];
    char module_path[512]; /* ';'-separated DOS directories searched by LoadLibrary */

    uint16_t task_sel, env_sel;
    uint16_t errmode;
    kmodule mods[KERNEL_MAX_MODS];
    unsigned nmods;

    uint32_t t0_ms;

    gblock *gb;              /* indexed by selector >> 3 */
    uint32_t global_used;
    lheap heaps[KERNEL_MAX_HEAPS];
    unsigned nheaps;
    kres *res;               /* HRSRC = index + 1 */
    unsigned nres, rescap;

    dospath_t dp;
    FILE *files[KERNEL_MAX_FILES]; /* DOS handle = index + 5 */
    unsigned tmpseq;
    uint16_t dta_sel, dta_off;     /* DOS disk transfer area; 0: the PSP's default at task:0x180 */
    struct kfind {                 /* FindFirst state, referenced from the DTA's reserved bytes */
        char host[DP_PATH_MAX], pattern[16];
        unsigned attr, next;
        bool used;
    } finds[8];
    uint16_t dos_error;       /* the last DOS error code, for AH=59h */
    unsigned find_seq;
};

/* handlers implemented outside kernel.c, referenced by its export table */
cpu_status k_lstrcpy(hle_call *c);
cpu_status k_lstrcat(hle_call *c);
cpu_status k_lstrlen(hle_call *c);
cpu_status k_hmemcpy(hle_call *c);
cpu_status k_globalalloc(hle_call *c);
cpu_status k_globalrealloc(hle_call *c);
cpu_status k_globalfree(hle_call *c);
cpu_status k_globallock(hle_call *c);
cpu_status k_globalunlock(hle_call *c);
cpu_status k_globalsize(hle_call *c);
cpu_status k_globalhandle(hle_call *c);
cpu_status k_globalflags(hle_call *c);
cpu_status k_globalcompact(hle_call *c);
cpu_status k_getfreespace(hle_call *c);
cpu_status k_globallru(hle_call *c);
cpu_status k_locksegment(hle_call *c);
cpu_status k_unlocksegment(hle_call *c);
cpu_status k_localinit(hle_call *c);
cpu_status k_localalloc(hle_call *c);
cpu_status k_localrealloc(hle_call *c);
cpu_status k_localfree(hle_call *c);
cpu_status k_locallock(hle_call *c);
cpu_status k_localunlock(hle_call *c);
cpu_status k_localsize(hle_call *c);
cpu_status k_localhandle(hle_call *c);
cpu_status k_localflags(hle_call *c);
cpu_status k_localcompact(hle_call *c);
bool kernel_heap_attach(kernel_t *k);  /* the auto data segment's local heap, global tables */
void kernel_heap_destroy(kernel_t *k);
void kernel_heap_drop(kernel_t *k, uint16_t sel); /* forget the local heap of an unloaded segment */
/* Allocate/free global memory outside the guest API (resources). 0 on failure. */
uint16_t kernel_global_alloc(kernel_t *k, uint32_t size, uint16_t flags);
bool kernel_global_free(kernel_t *k, uint16_t h);

cpu_status k_findresource(hle_call *c);
cpu_status k_loadresource(hle_call *c);
cpu_status k_freeresource(hle_call *c);
cpu_status k_sizeofresource(hle_call *c);
cpu_status k_allocresource(hle_call *c);
void kernel_res_destroy(kernel_t *k);
void kernel_res_drop(kernel_t *k, uint16_t inst); /* a library was unloaded: its resources go with it */
/* Host file behind a module's resource data (AccessResource), NULL if there is none. */
FILE *kernel_module_file(kernel_t *k, uint16_t inst);

cpu_status k_lopen(hle_call *c);
cpu_status k_lcreat(hle_call *c);
cpu_status k_lclose(hle_call *c);
cpu_status k_lread(hle_call *c);
cpu_status k_lwrite(hle_call *c);
cpu_status k_hread(hle_call *c);
cpu_status k_hwrite(hle_call *c);
cpu_status k_llseek(hle_call *c);
cpu_status k_openfile(hle_call *c);
cpu_status k_dos3call(hle_call *c);
cpu_status k_gettempfilename(hle_call *c);
cpu_status k_getprofileint(hle_call *c);
cpu_status k_getprofilestring(hle_call *c);
cpu_status k_writeprofilestring(hle_call *c);
cpu_status k_getprivateprofileint(hle_call *c);
cpu_status k_getprivateprofilestring(hle_call *c);
cpu_status k_writeprivateprofilestring(hle_call *c);
cpu_status k_getwindowsdirectory(hle_call *c);
cpu_status k_getsystemdirectory(hle_call *c);
cpu_status k_accessresource(hle_call *c);
void kernel_file_destroy(kernel_t *k);
const ne_resource *kernel_res_get(kernel_t *k, uint16_t inst, uint16_t hrsrc);

/* USER helpers, registered under KERNEL_REG_USER_HELPERS */
unsigned kernel_ansi_upper(unsigned ch);   /* Windows ANSI (Latin-1) case mapping */
unsigned kernel_ansi_lower(unsigned ch);
extern const hle_def kernel_user_defs[];
extern const size_t kernel_user_ndefs;

/* NUL-terminated string in guest memory: host pointer and length, false when
 * the selector is bad or no terminator lies inside the segment. */
bool kernel_guest_str(hle_call *c, uint16_t sel, uint16_t off, char **s, size_t *len);
/* Copy n bytes between huge pointers, a 64K piece at a time. */
bool kernel_huge_copy(loader_proc *p, uint16_t dsel, uint32_t doff, uint16_t ssel, uint32_t soff,
                      uint32_t n);
uint32_t kernel_now_ms(kernel_t *k);

#endif
