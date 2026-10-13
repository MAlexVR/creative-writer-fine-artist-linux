#ifndef HLE_H
#define HLE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "loader.h"

/* High-level emulation of the Win16 API: a table keyed by (module, ordinal)
 * with the declared Pascal argument byte count of each function. hle_resolve
 * is a loader_resolve_fn: handlers run as loader thunk handlers, the thunk
 * performs the RETF n. Imports without a handler bind to a logging stub that
 * stops the run (stub_unknown), reporting the caller's CS:IP. */

typedef struct hle hle_t;
typedef struct hle_entry hle_entry;

typedef struct {
    hle_t *hle;
    loader_proc *proc;
    cpu_t *cpu;
    const hle_entry *entry;
    void *ctx;                      /* the pointer given at registration */
    uint16_t caller_cs, caller_ip;  /* return address of the far call */
} hle_call;

typedef cpu_status (*hle_fn)(hle_call *call);

typedef struct {
    const char *module;
    uint16_t ordinal;
    const char *name;     /* may be NULL */
    uint16_t arg_bytes;
    hle_fn fn;            /* NULL: name only, the call is logged as unimplemented */
} hle_def;

struct hle_entry {
    char module[NE_NAME_MAX + 1];
    char name[NE_NAME_MAX + 1];
    uint16_t ordinal;
    uint16_t arg_bytes;
    hle_fn fn;
    void *ctx;
    bool is_const;
    uint16_t const_off;
    uint32_t const_seg_bytes;
    uint64_t calls;
    hle_t *hle;
};

typedef struct {
    const hle_entry *entry; /* NULL for an import the table does not know */
    char module[NE_NAME_MAX + 1];
    char name[NE_NAME_MAX + 1]; /* by-name import, else the known name or "" */
    uint16_t ordinal;
    bool by_name;
    bool implemented;
    uint16_t caller_cs, caller_ip;
} hle_record;

#define HLE_RECORD_MAX 8192
#define HLE_RING 256      /* the most recent calls are always kept, see hle_recent */

struct hle {
    hle_entry **entries;
    size_t count, cap;
    bool stub_unknown;   /* bind unregistered imports to the logging stub */
    FILE *log;           /* NULL: silent. Receives unimplemented calls (and the trace) */
    bool trace;          /* also log every call */
    bool trace_args;     /* with trace: also log the argument words of each call (first pushed first) */
    bool record;         /* keep the call sequence in rec[] (bounded) */
    hle_record *rec;
    size_t nrec, caprec;
    hle_record last_unimpl;
    uint64_t unimpl_calls;
    uint64_t ncalls;           /* every call from the program, for progress detection */
    hle_record ring[HLE_RING]; /* circular, newest at (ncalls - 1) % HLE_RING */
    /* Called with every call from the program before its handler runs (not for hle_invoke): lets a module watch
     * what the program does (metafile recording). */
    void (*tap)(void *ctx, hle_call *call);
    void *tap_ctx;
};

/* The last calls (at most max, at most HLE_RING), oldest first; returns how many were stored. */
size_t hle_recent(const hle_t *h, hle_record *out, size_t max);

void hle_init(hle_t *h);
void hle_destroy(hle_t *h);

/* False when (module, ordinal) is already registered or memory is exhausted. */
bool hle_register(hle_t *h, const hle_def *def, void *ctx);
bool hle_register_table(hle_t *h, const hle_def *defs, size_t n, void *ctx);
/* Constant import: an absolute value (seg_bytes == 0) or a selector over a fresh
 * zeroed data segment of seg_bytes bytes (see loader_api). */
bool hle_register_const(hle_t *h, const char *module, uint16_t ordinal, const char *name,
                        uint16_t value, uint32_t seg_bytes);

const hle_entry *hle_find(const hle_t *h, const char *module, uint16_t ordinal);
/* Case-insensitive name lookup within a module. */
const hle_entry *hle_find_name(const hle_t *h, const char *module, const char *name);

/* loader_resolve_fn: ctx is the hle_t. */
bool hle_resolve(void *ctx, const loader_import *imp, loader_api *out);

/* Pascal arguments are read in declaration order (leftmost first). */
typedef struct {
    hle_call *call;
    unsigned left;    /* argument bytes not consumed yet */
    cpu_status st;    /* first memory fault, sticky */
} hle_args;

void hle_args_init(hle_args *a, hle_call *call);
uint16_t hle_u16(hle_args *a);
uint32_t hle_u32(hle_args *a);                          /* LONG / DWORD */
void hle_far(hle_args *a, uint16_t *sel, uint16_t *off); /* far pointer */

/* Runs a registered handler the way a guest call does: the argument words (declaration order) are pushed on the
 * guest stack under a dummy far return address and the stack is restored afterwards. *dxax gets the result. */
cpu_status hle_invoke(hle_t *h, loader_proc *p, const char *module, uint16_t ordinal, const uint16_t *words,
                      unsigned nwords, uint32_t *dxax);

void hle_ret16(hle_call *call, uint16_t v);              /* AX */
void hle_ret32(hle_call *call, uint32_t v);              /* DX:AX */

/* Guest memory access for handlers. Strings are truncated to cap-1 bytes. */
void *hle_ptr(hle_call *call, uint16_t sel, uint32_t off, uint32_t len);
bool hle_read_cstr(hle_call *call, uint16_t sel, uint16_t off, char *out, size_t cap);
/* sel:off + delta over a huge block (selector steps of AHINCR per 64K). */
void hle_huge_add(uint16_t *sel, uint32_t *off, uint32_t delta);

#endif
