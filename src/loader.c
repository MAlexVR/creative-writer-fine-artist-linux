/* NE module -> Win16 process image. See loader.h. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader.h"

#define THUNK_SLOT 8
#define THUNK_MAX (MEM_SEG_MAX / THUNK_SLOT - 1)

static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr16le(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

const char *loader_strerror(loader_err e) {
    switch (e) {
    case LOADER_OK: return "ok";
    case LOADER_ERR_ARG: return "bad argument";
    case LOADER_ERR_NOMEM: return "out of memory";
    case LOADER_ERR_SEGMENT: return "segment allocation or entry state invalid";
    case LOADER_ERR_BAD_RELOC: return "malformed relocation";
    case LOADER_ERR_THUNKS: return "thunk segment or hook table full";
    case LOADER_ERR_RESOLVE: return "invalid resolver registration";
    case LOADER_ERR_NOTFOUND: return "module file not found";
    case LOADER_ERR_FORMAT: return "module file is not a valid NE module";
    case LOADER_ERR_INIT: return "library entry point failed";
    }
    return "unknown";
}

const char *loader_strstop(loader_stop s) {
    switch (s) {
    case LOADER_STOP_NONE: return "none";
    case LOADER_STOP_HLT: return "hlt";
    case LOADER_STOP_REQUESTED: return "stop requested";
    case LOADER_STOP_UNRESOLVED: return "unresolved import called";
    case LOADER_STOP_BUDGET: return "budget exhausted";
    case LOADER_STOP_FAULT: return "fault";
    case LOADER_STOP_INIT: return "library initialization failed";
    }
    return "unknown";
}

/* ---- thunks ------------------------------------------------------------ */

cpu_status loader_arg16(cpu_t *c, unsigned ofs, uint16_t *v) {
    uint32_t sp = (c->r[CPU_ESP] + 4 + ofs) & 0xFFFF;
    return cpu_read16(c, c->seg[CPU_SS], sp, v);
}

cpu_status loader_request_stop(loader_proc *p) {
    p->stop = LOADER_STOP_REQUESTED;
    return CPU_HALT;
}

static cpu_status unresolved_handler(loader_proc *p, const loader_import *imp, void *ctx) {
    (void)imp;
    (void)ctx;
    p->stop = LOADER_STOP_UNRESOLVED;
    return CPU_HALT;
}

static cpu_status thunk_hook(cpu_t *c, void *ctx) {
    loader_thunk *t = ctx;
    loader_proc *p = t->proc;
    uint16_t cs = c->seg[CPU_CS];
    uint32_t ip = c->eip;
    cpu_status s;
    p->last_call = t;
    p->api_calls++;
    s = t->api.fn(p, &t->imp, t->api.ctx);
    if (s != CPU_OK) return s;
    if (c->seg[CPU_CS] != cs || c->eip != ip) return CPU_OK; /* handler redirected */
    return cpu_retf(c, t->api.arg_bytes);
}

static bool same_import(const loader_import *a, const loader_import *b) {
    if (a->by_name != b->by_name || strcmp(a->module, b->module) != 0) return false;
    return a->by_name ? strcmp(a->name, b->name) == 0 : a->ordinal == b->ordinal;
}

loader_thunk *loader_find_thunk(const loader_proc *p, const loader_import *imp) {
    size_t i;
    for (i = 0; i < p->nthunks; i++)
        if (same_import(&p->thunks[i]->imp, imp)) return p->thunks[i];
    return NULL;
}

loader_err loader_thunk_for(loader_proc *p, const loader_import *imp, loader_thunk **out) {
    loader_thunk *t = loader_find_thunk(p, imp);
    if (t) {
        *out = t;
        return LOADER_OK;
    }
    if (p->nthunks >= THUNK_MAX) return LOADER_ERR_THUNKS;
    t = calloc(1, sizeof *t);
    if (!t) return LOADER_ERR_NOMEM;
    t->imp = *imp;
    t->proc = p;
    if (p->cfg.resolve && p->cfg.resolve(p->cfg.resolve_ctx, imp, &t->api)) {
        if (!t->api.fn && !t->api.is_const) {
            free(t);
            return LOADER_ERR_RESOLVE;
        }
        t->resolved = true;
    } else {
        memset(&t->api, 0, sizeof t->api);
        t->api.fn = unresolved_handler;
    }
    if (t->api.is_const) {
        t->sel = t->api.const_sel;
        t->off = t->api.const_off;
        if (t->api.const_seg_bytes) {
            uint32_t n = t->api.const_seg_bytes;
            t->sel = n > MEM_SEG_MAX ? 0 : mem_alloc(&p->mem, n, MEM_DATA);
            if (!t->sel) {
                free(t);
                return LOADER_ERR_NOMEM;
            }
        }
    } else {
        t->sel = p->thunk_sel;
        t->off = (uint16_t)((p->nthunks + 1) * THUNK_SLOT);
    }
    if (p->nthunks == p->thunk_cap) {
        size_t cap = p->thunk_cap ? p->thunk_cap * 2 : 64;
        loader_thunk **v = realloc(p->thunks, cap * sizeof *v);
        if (!v) {
            free(t);
            return LOADER_ERR_NOMEM;
        }
        p->thunks = v;
        p->thunk_cap = cap;
    }
    if (!t->api.is_const && !cpu_add_hook(&p->cpu, t->sel, t->off, thunk_hook, t)) {
        if (t->api.const_seg_bytes) mem_free(&p->mem, t->sel);
        free(t);
        return LOADER_ERR_THUNKS;
    }
    p->thunks[p->nthunks++] = t;
    if (!t->resolved) p->unresolved++;
    *out = t;
    return LOADER_OK;
}

/* ---- modules ------------------------------------------------------------- */

static int ci_cmp(const char *a, const char *b) {
    for (; *a && toupper((unsigned char)*a) == toupper((unsigned char)*b); a++, b++) {}
    return toupper((unsigned char)*a) - toupper((unsigned char)*b);
}

/* "C:\DIR\Name.ext" -> "NAME" (key) and "NAME.EXT" (file); a spec without an extension names a .DLL. */
static void split_spec(const char *spec, char key[16], char file[16]) {
    const char *b = spec, *q, *dot;
    size_t n = 0, e = 0;
    for (q = spec; *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
    dot = strchr(b, '.');
    for (; b[n] && b[n] != '.' && n < 15; n++) key[n] = (char)toupper((unsigned char)b[n]);
    key[n] = 0;
    memcpy(file, key, n);
    if (dot && dot[1]) {
        file[n++] = '.';
        for (; dot[1 + e] && n < 15; e++) file[n++] = (char)toupper((unsigned char)dot[1 + e]);
    } else if (!dot && n <= 11) {
        memcpy(file + n, ".DLL", 5);
        n += 4;
    }
    file[n] = 0;
}

loader_module *loader_module_by_name(const loader_proc *p, const char *name) {
    size_t i;
    for (i = 0; i < p->nmods; i++)
        if (!ci_cmp(p->mods[i]->name, name) || !ci_cmp(p->mods[i]->key, name)) return p->mods[i];
    return NULL;
}

loader_module *loader_module_by_handle(const loader_proc *p, uint16_t handle) {
    size_t i;
    if (!handle) return NULL;
    for (i = 0; i < p->nmods; i++)
        if (p->mods[i]->handle == handle) return p->mods[i];
    return NULL;
}

loader_module *loader_module_by_ds(const loader_proc *p, uint16_t ds) {
    size_t i;
    if (!ds) return NULL;
    for (i = 0; i < p->nmods; i++)
        if (p->mods[i]->auto_ds == ds) return p->mods[i];
    return NULL;
}

bool loader_module_proc(const loader_module *m, const char *name, uint16_t ordinal, uint16_t *sel,
                        uint16_t *off) {
    const ne_image *ne = m->ne;
    const ne_entry *en;
    size_t i;
    if (name) {
        ordinal = 0;
        for (i = 0; i < ne->resident_count && !ordinal; i++)
            if (ne->resident_names[i].ordinal && !ci_cmp(ne->resident_names[i].text, name))
                ordinal = ne->resident_names[i].ordinal;
        for (i = 0; i < ne->nonres_count && !ordinal; i++)
            if (ne->nonres_names[i].ordinal && !ci_cmp(ne->nonres_names[i].text, name))
                ordinal = ne->nonres_names[i].ordinal;
    }
    if (ne_entry_for_ordinal(ne, ordinal, &en) != NE_OK || en->segment == 0 || en->segment > ne->seg_count)
        return false;
    *sel = m->seg_sel[en->segment - 1];
    *off = en->offset;
    return true;
}

static void module_destroy(loader_proc *p, loader_module *m) {
    unsigned i;
    if (m->own_seg_sel && m->seg_sel) {
        for (i = 0; i < m->ne->seg_count; i++)
            if (m->seg_sel[i]) mem_free(&p->mem, m->seg_sel[i]);
        free(m->seg_sel);
    }
    if (m->own_handle && m->handle) mem_free(&p->mem, m->handle);
    if (m->owned) {
        ne_free(m->owned);
        free(m->owned);
    }
    free(m->deps);
    free(m->ref);
    free(m->ref_state);
    free(m);
}

static bool mods_push(loader_proc *p, loader_module *m) {
    if (p->nmods == p->mod_cap) {
        size_t cap = p->mod_cap ? p->mod_cap * 2 : 8;
        loader_module **v = realloc(p->mods, cap * sizeof *v);
        if (!v) return false;
        p->mods = v;
        p->mod_cap = cap;
    }
    p->mods[p->nmods++] = m;
    return true;
}

static void mods_remove(loader_proc *p, loader_module *m) {
    size_t i;
    for (i = 0; i < p->nmods; i++)
        if (p->mods[i] == m) {
            memmove(&p->mods[i], &p->mods[i + 1], (p->nmods - i - 1) * sizeof *p->mods);
            p->nmods--;
            return;
        }
}

/* The module's name from the header (upper case), the file name if it has none. */
static loader_module *module_new(const ne_image *ne, ne_image *owned, const char *path) {
    loader_module *m = calloc(1, sizeof *m);
    size_t i;
    if (!m) return NULL;
    m->ne = ne;
    m->owned = owned;
    snprintf(m->path, sizeof m->path, "%s", path ? path : "");
    split_spec(m->path, m->key, m->file);
    if (ne->resident_count && ne->resident_names[0].text[0])
        snprintf(m->name, sizeof m->name, "%s", ne->resident_names[0].text);
    else
        snprintf(m->name, sizeof m->name, "%s", m->key);
    for (i = 0; m->name[i]; i++) m->name[i] = (char)toupper((unsigned char)m->name[i]);
    m->is_dll = (ne->flags & 0x8000) != 0;
    m->ref = calloc((size_t)ne->modref_count + 1, sizeof *m->ref);
    m->ref_state = calloc((size_t)ne->modref_count + 1, 1);
    if (!m->ref || !m->ref_state) {
        free(m->ref);
        free(m->ref_state);
        free(m);
        return NULL;
    }
    return m;
}

/* Records that m uses dep; a module loaded just for m already carries that one use. */
static bool add_dep(loader_module *m, loader_module *dep, bool fresh) {
    loader_module **v = realloc(m->deps, (m->ndeps + 1) * sizeof *v);
    if (!v) return false;
    m->deps = v;
    m->deps[m->ndeps++] = dep;
    if (!fresh) dep->usage++;
    return true;
}

static loader_err module_load(loader_proc *p, ne_image *own, const char *path, loader_module **out);

/* The loaded module behind a module reference of m, loading its file on first use. */
static loader_module *import_module(loader_proc *p, loader_module *m, unsigned idx, const char *name) {
    loader_module *dep = NULL;
    bool fresh = false;
    size_t i;
    if (idx > m->ne->modref_count) return NULL;
    if (m->ref_state[idx]) return m->ref[idx];
    for (i = 1; i < p->nmods && !dep; i++)
        if (p->mods[i] != m && (!ci_cmp(p->mods[i]->name, name) || !ci_cmp(p->mods[i]->key, name)))
            dep = p->mods[i];
    if (!dep && p->cfg.open_module) {
        ne_image tmp;
        char path[LOADER_PATH_MAX];
        memset(&tmp, 0, sizeof tmp);
        path[0] = 0;
        if (p->cfg.open_module(p->cfg.open_ctx, name, &tmp, path) == 0) {
            ne_image *own = malloc(sizeof *own);
            if (own) {
                *own = tmp;
                if (module_load(p, own, path, &dep) != LOADER_OK) dep = NULL;
                fresh = dep != NULL;
            } else {
                ne_free(&tmp);
            }
        }
    }
    if (dep && !add_dep(m, dep, fresh)) dep = NULL;
    m->ref[idx] = dep;
    m->ref_state[idx] = dep ? 1 : 2;
    return dep;
}

/* ---- relocations ------------------------------------------------------- */

static loader_err record_osfixup(loader_proc *p, unsigned seg, uint16_t site, uint16_t type) {
    loader_osfixup *v = realloc(p->osfixups, (p->nosfixups + 1) * sizeof *v);
    if (!v) return LOADER_ERR_NOMEM;
    p->osfixups = v;
    v[p->nosfixups].seg = (uint16_t)seg;
    v[p->nosfixups].offset = site;
    v[p->nosfixups].type = type;
    p->nosfixups++;
    return LOADER_OK;
}

/* Patch one site. Additive: the stored value is added to (selectors replace). */
static loader_err patch_site(loader_proc *p, uint16_t sel, uint16_t site, const ne_reloc *r,
                             uint16_t tsel, uint16_t toff) {
    unsigned size = r->src_type == NE_SRC_LOBYTE ? 1 : r->src_type == NE_SRC_FARPTR ? 4 : 2;
    uint8_t *d = mem_ptr(&p->mem, sel, site, size);
    if (!d) return LOADER_ERR_BAD_RELOC;
    switch (r->src_type) {
    case NE_SRC_LOBYTE: d[0] = (uint8_t)(r->additive ? d[0] + (toff & 0xFF) : toff); break;
    case NE_SRC_SELECTOR: wr16le(d, tsel); break;
    case NE_SRC_OFFSET: wr16le(d, r->additive ? rd16le(d) + toff : toff); break;
    default: /* NE_SRC_FARPTR */
        wr16le(d, r->additive ? rd16le(d) + toff : toff);
        wr16le(d + 2, tsel);
        break;
    }
    return LOADER_OK;
}

static loader_err resolve_target(loader_proc *p, loader_module *m, const ne_reloc *r, uint16_t *tsel,
                                 uint16_t *toff) {
    const ne_image *ne = m->ne;
    unsigned seg;
    if (r->target_type == NE_TGT_INTERNAL) {
        if (r->a == NE_SEG_MOVEABLE_REF) {
            const ne_entry *e;
            if (ne_entry_for_ordinal(ne, r->b, &e) != NE_OK || e->kind == NE_ENTRY_UNUSED)
                return LOADER_ERR_BAD_RELOC;
            seg = e->segment;
            *toff = e->offset;
        } else {
            seg = r->a;
            *toff = r->b;
        }
        if (seg == 0 || seg > ne->seg_count) return LOADER_ERR_BAD_RELOC;
        *tsel = m->seg_sel[seg - 1];
        return LOADER_OK;
    } else {
        loader_import imp;
        loader_thunk *t;
        loader_module *dep;
        loader_err e;
        memset(&imp, 0, sizeof imp);
        if (ne_modref_name(ne, r->a, imp.module) != NE_OK) return LOADER_ERR_BAD_RELOC;
        if (r->target_type == NE_TGT_IMPORT_NAME) {
            imp.by_name = true;
            if (ne_imported_name(ne, r->b, imp.name) != NE_OK) return LOADER_ERR_BAD_RELOC;
        } else {
            imp.ordinal = r->b;
        }
        /* real code of a loaded module first, the host resolver only for what no module exports */
        dep = import_module(p, m, r->a, imp.module);
        if (dep && loader_module_proc(dep, imp.by_name ? imp.name : NULL, imp.ordinal, tsel, toff))
            return LOADER_OK;
        if ((e = loader_thunk_for(p, &imp, &t)) != LOADER_OK) return e;
        *tsel = t->sel;
        *toff = t->off;
        return LOADER_OK;
    }
}

static loader_err apply_reloc(loader_proc *p, loader_module *m, unsigned seg, const ne_reloc *r) {
    uint16_t tsel = 0, toff = 0, *sites;
    size_t n = 0, i;
    loader_err e = LOADER_OK;
    if (r->src_type != NE_SRC_LOBYTE && r->src_type != NE_SRC_SELECTOR &&
        r->src_type != NE_SRC_FARPTR && r->src_type != NE_SRC_OFFSET)
        return LOADER_ERR_BAD_RELOC;
    if (r->target_type != NE_TGT_OSFIXUP && (e = resolve_target(p, m, r, &tsel, &toff)) != LOADER_OK)
        return e;
    if (ne_reloc_sites(m->ne, seg, r, NULL, 0, &n) != NE_OK) return LOADER_ERR_BAD_RELOC;
    sites = malloc(n * sizeof *sites);
    if (!sites) return LOADER_ERR_NOMEM;
    if (ne_reloc_sites(m->ne, seg, r, sites, n, &n) != NE_OK) {
        free(sites);
        return LOADER_ERR_BAD_RELOC;
    }
    for (i = 0; i < n && e == LOADER_OK; i++) {
        if (r->target_type == NE_TGT_OSFIXUP) {
            e = record_osfixup(p, seg, sites[i], r->a);
        } else if ((e = patch_site(p, m->seg_sel[seg], sites[i], r, tsel, toff)) == LOADER_OK) {
            p->reloc_sites++;
        }
    }
    free(sites);
    return e;
}

static loader_err apply_relocs(loader_proc *p, loader_module *m) {
    unsigned i, j;
    loader_err e;
    for (i = 0; i < m->ne->seg_count; i++)
        for (j = 0; j < m->ne->segs[i].reloc_count; j++) {
            p->reloc_records++;
            if ((e = apply_reloc(p, m, i, &m->ne->segs[i].relocs[j])) != LOADER_OK) return e;
        }
    return LOADER_OK;
}

/* ---- loading ------------------------------------------------------------ */

static loader_err alloc_segments(loader_proc *p, loader_module *m) {
    const ne_image *ne = m->ne;
    unsigned i;
    for (i = 0; i < ne->seg_count; i++) {
        const ne_segment *s = &ne->segs[i];
        uint32_t size = s->alloc_size;
        mem_kind kind = !(s->flags & NE_SEG_DATA) ? MEM_CODE :
                        (s->flags & NE_SEG_READONLY) ? MEM_RODATA : MEM_DATA;
        uint8_t *d;
        if (s->file_offset && s->file_length > size) size = s->file_length;
        if (size > MEM_SEG_MAX) size = MEM_SEG_MAX;
        if (ne->auto_data_seg == i + 1) {
            /* [static data][stack][local heap], clipped to 64K, heap first */
            uint32_t stack = ne->ss == ne->auto_data_seg ? ne->stack_size : 0;
            if (stack > MEM_SEG_MAX - size) stack = MEM_SEG_MAX - size;
            m->stack_size = (uint16_t)stack;
            m->heap_start = (uint16_t)(size + stack); /* 64K wraps to 0 only with heap_size 0 */
            m->heap_size = ne->heap_size > MEM_SEG_MAX - size - stack
                               ? (uint16_t)(MEM_SEG_MAX - size - stack) : ne->heap_size;
            m->sp_default = (uint16_t)((size + stack) & 0xFFFF);
            size += stack + m->heap_size;
        }
        if (!(m->seg_sel[i] = mem_alloc(&p->mem, size, kind))) return LOADER_ERR_SEGMENT;
        if (s->file_offset && s->file_length) {
            d = mem_ptr(&p->mem, m->seg_sel[i], 0, s->file_length);
            if (!d || (uint64_t)s->file_offset + s->file_length > ne->size) return LOADER_ERR_SEGMENT;
            memcpy(d, ne->data + s->file_offset, s->file_length);
        }
    }
    return LOADER_OK;
}

/* A library's exported far procedures start with "mov ax,ds; nop" or "push ds;
 * pop ax; nop" and save AX as their DS: the Windows loader patches the first
 * to "mov ax, <auto data selector>" so a call from any program uses the
 * library's own data. */
static void patch_prologs(loader_proc *p, loader_module *m) {
    const ne_image *ne = m->ne;
    size_t i;
    if (!m->is_dll || !m->auto_ds) return;
    for (i = 0; i < ne->entry_count; i++) {
        const ne_entry *e = &ne->entries[i];
        uint8_t *d;
        if (e->kind == NE_ENTRY_UNUSED || e->segment == 0 || e->segment > ne->seg_count ||
            (ne->segs[e->segment - 1].flags & NE_SEG_DATA))
            continue;
        d = mem_ptr(&p->mem, m->seg_sel[e->segment - 1], e->offset, 3);
        if (!d || d[2] != 0x90) continue;
        if ((d[0] == 0x8C && d[1] == 0xD8) || (d[0] == 0x1E && d[1] == 0x58)) {
            d[0] = 0xB8;
            wr16le(d + 1, m->auto_ds);
        }
    }
}

static loader_err module_handle(loader_proc *p, loader_module *m) {
    m->auto_ds = m->ne->auto_data_seg ? m->seg_sel[m->ne->auto_data_seg - 1] : 0;
    if (m->auto_ds) {
        m->handle = m->auto_ds;
    } else {
        if (!(m->handle = mem_alloc(&p->mem, 16, MEM_DATA))) return LOADER_ERR_NOMEM;
        m->own_handle = true;
    }
    return LOADER_OK;
}

static void module_release(loader_proc *p, loader_module *m);

/* Loads a library image (ownership of own passes to the loader on success and on failure). */
static loader_err module_load(loader_proc *p, ne_image *own, const char *path, loader_module **out) {
    loader_module *m = module_new(own, own, path);
    loader_err e;
    if (!m) {
        ne_free(own);
        free(own);
        return LOADER_ERR_NOMEM;
    }
    m->seg_sel = calloc(own->seg_count ? own->seg_count : 1, sizeof *m->seg_sel);
    m->own_seg_sel = true;
    m->usage = 1;
    if (!m->seg_sel) {
        module_destroy(p, m);
        return LOADER_ERR_NOMEM;
    }
    if ((e = alloc_segments(p, m)) != LOADER_OK || (e = module_handle(p, m)) != LOADER_OK) {
        module_destroy(p, m);
        return e;
    }
    if (!mods_push(p, m)) {
        module_destroy(p, m);
        return LOADER_ERR_NOMEM;
    }
    if ((e = apply_relocs(p, m)) != LOADER_OK) {
        module_release(p, m);
        return e;
    }
    patch_prologs(p, m);
    *out = m;
    return LOADER_OK;
}

/* Drops a module that is being unloaded: its hold on the dependencies, then its memory. */
static void module_release(loader_proc *p, loader_module *m) {
    size_t i;
    if (m->handle && p->cfg.unload_module) p->cfg.unload_module(p->cfg.unload_ctx, m);
    mods_remove(p, m);
    for (i = 0; i < m->ndeps; i++) loader_free_library(p, m->deps[i]);
    module_destroy(p, m);
}

bool loader_free_library(loader_proc *p, loader_module *m) {
    if (!m || m == p->mods[0] || m->usage == 0) return false;
    if (--m->usage > 0) return false;
    module_release(p, m);
    return true;
}

static loader_err make_psp(loader_proc *p) {
    uint8_t *d;
    size_t n = p->cfg.cmdline ? strlen(p->cfg.cmdline) : 0;
    if (n > 127) n = 127;
    if (!(p->psp_sel = mem_alloc(&p->mem, 0x100, MEM_DATA))) return LOADER_ERR_SEGMENT;
    d = mem_ptr(&p->mem, p->psp_sel, 0, 0x100);
    d[0] = 0xCD; d[1] = 0x20; /* INT 20h */
    d[0x80] = (uint8_t)n;
    if (n) memcpy(d + 0x81, p->cfg.cmdline, n);
    d[0x81 + n] = 0; /* InitTask hands the tail out as lpCmdLine: an ordinary NUL-terminated string */
    return LOADER_OK;
}

static loader_err init_state(loader_proc *p) {
    const ne_image *ne = p->ne;
    cpu_t *c = &p->cpu;
    cpu_mem m;
    uint16_t ss, sp;
    if (ne->cs == 0 || ne->cs > ne->seg_count || ne->ss == 0 || ne->ss > ne->seg_count ||
        ne->auto_data_seg > ne->seg_count)
        return LOADER_ERR_SEGMENT;
    mem_cpu_iface(&p->mem, &m);
    cpu_init(c, &m);
    p->auto_ds = ne->auto_data_seg ? p->seg_sel[ne->auto_data_seg - 1] : p->seg_sel[ne->ss - 1];
    ss = p->seg_sel[ne->ss - 1];
    /* SP 0 in the header: the loader places the stack at the top of the stack area */
    sp = ne->sp ? ne->sp : ne->ss == ne->auto_data_seg ? p->sp_default
                                                       : (uint16_t)(mem_size(&p->mem, ss) & 0xFFFF);
    c->seg[CPU_CS] = p->seg_sel[ne->cs - 1];
    c->eip = ne->ip;
    c->seg[CPU_SS] = ss;
    c->seg[CPU_DS] = p->auto_ds;
    c->seg[CPU_ES] = p->psp_sel;
    c->r[CPU_ESP] = sp;
    c->r[CPU_EAX] = 1;
    c->r[CPU_EBX] = p->stack_size;
    c->r[CPU_ECX] = p->heap_size;
    c->r[CPU_EDI] = p->auto_ds; /* hInstance */
    c->r[CPU_ESI] = 0;          /* hPrevInstance */
    return LOADER_OK;
}

void loader_destroy(loader_proc *p) {
    size_t i;
    if (!p) return;
    for (i = 0; i < p->nthunks; i++) free(p->thunks[i]);
    free(p->thunks);
    free(p->osfixups);
    for (i = p->nmods; i-- > 0;) {
        loader_module *m = p->mods[i];
        if (i == 0) { /* the program: its segment table is p->seg_sel, its image is the caller's */
            free(m->deps);
            free(m->ref);
            free(m->ref_state);
            free(m);
        } else {
            m->own_seg_sel = false; /* mem_destroy releases every selector */
            m->own_handle = false;
            free(m->seg_sel);
            m->seg_sel = NULL;
            module_destroy(p, m);
        }
    }
    free(p->mods);
    free(p->seg_sel);
    mem_destroy(&p->mem);
    free(p);
}

static cpu_status trap_hook(cpu_t *c, void *ctx) {
    loader_proc *p = ctx;
    (void)c;
    p->trap_depth = p->depth;
    return CPU_HALT;
}

loader_err loader_create(const ne_image *ne, const loader_config *cfg, loader_proc **out) {
    loader_proc *p;
    loader_module *m0;
    loader_err e;
    if (!out) return LOADER_ERR_ARG;
    *out = NULL;
    if (!ne || !ne->data) return LOADER_ERR_ARG;
    p = calloc(1, sizeof *p);
    if (!p) return LOADER_ERR_NOMEM;
    mem_init(&p->mem);
    p->ne = ne;
    if (cfg) p->cfg = *cfg;
    p->seg_sel = calloc(ne->seg_count ? ne->seg_count : 1, sizeof *p->seg_sel);
    if (!p->seg_sel) {
        e = LOADER_ERR_NOMEM;
        goto fail;
    }
    if (!(m0 = module_new(ne, NULL, "")) || !mods_push(p, m0)) {
        free(m0);
        e = LOADER_ERR_NOMEM;
        goto fail;
    }
    m0->seg_sel = p->seg_sel;
    m0->usage = 1;
    m0->inited = true;
    if ((e = alloc_segments(p, m0)) != LOADER_OK) goto fail;
    p->stack_size = m0->stack_size;
    p->heap_start = m0->heap_start;
    p->heap_size = m0->heap_size;
    p->sp_default = m0->sp_default;
    if (!(p->thunk_sel = mem_alloc(&p->mem, MEM_SEG_MAX, MEM_CODE))) {
        e = LOADER_ERR_SEGMENT;
        goto fail;
    }
    memset(mem_ptr(&p->mem, p->thunk_sel, 0, MEM_SEG_MAX), 0xF4, MEM_SEG_MAX); /* HLT backstop */
    if ((e = make_psp(p)) != LOADER_OK || (e = init_state(p)) != LOADER_OK) goto fail;
    m0->auto_ds = m0->handle = p->auto_ds;
    if (!cpu_add_hook(&p->cpu, p->thunk_sel, p->trap_off, trap_hook, p)) {
        e = LOADER_ERR_THUNKS;
        goto fail;
    }
    if ((e = apply_relocs(p, m0)) != LOADER_OK) goto fail;
    *out = p;
    return LOADER_OK;
fail:
    loader_destroy(p);
    return e;
}

/* ---- calling guest code ---------------------------------------------------- */

cpu_status loader_call_far(loader_proc *p, uint16_t cs, uint16_t ip, const uint16_t *args, unsigned nargs,
                           const loader_callregs *regs, uint32_t *ret) {
    cpu_t *c = &p->cpu;
    uint32_t r[8], eip, eflags;
    uint16_t seg[6];
    unsigned i, mine;
    cpu_status s;
    if (ret) *ret = 0;
    if (p->depth >= 8) return CPU_E_GP;
    memcpy(r, c->r, sizeof r);
    memcpy(seg, c->seg, sizeof seg);
    eip = c->eip;
    eflags = c->eflags;
    for (i = 0; i < nargs; i++)
        if ((s = cpu_push16(c, args[i])) != CPU_OK) return s;
    if ((s = cpu_push16(c, p->thunk_sel)) != CPU_OK || (s = cpu_push16(c, p->trap_off)) != CPU_OK) return s;
    c->seg[CPU_CS] = cs;
    c->eip = ip;
    if (regs) {
        c->r[CPU_EAX] = regs->ax;
        c->r[CPU_ECX] = regs->cx;
        c->r[CPU_EDI] = regs->di;
        c->r[CPU_ESI] = regs->si;
        c->seg[CPU_DS] = regs->ds;
        c->seg[CPU_ES] = regs->es;
    }
    c->eflags &= ~CPU_DF;
    mine = ++p->depth;
    p->trap_depth = 0;
    s = cpu_run(c, 0x7FFFFFFFFFFFFFFFull);
    p->depth--;
    if (s != CPU_HALT || p->trap_depth != mine) return s == CPU_OK || s == CPU_BUDGET ? CPU_E_UD : s;
    if (ret) *ret = (c->r[CPU_EDX] & 0xFFFF) << 16 | (c->r[CPU_EAX] & 0xFFFF);
    memcpy(c->r, r, sizeof r);
    memcpy(c->seg, seg, sizeof seg);
    c->eip = eip;
    c->eflags = eflags;
    return CPU_OK;
}

/* ---- library entry points and LoadLibrary ------------------------------------ */

/* Runs the entry point of m after those of its dependencies. *failed: the entry returned 0. */
static cpu_status init_module(loader_proc *p, loader_module *m, bool *failed) {
    size_t i;
    cpu_status s;
    if (m->inited) return CPU_OK;
    m->inited = true;
    for (i = 0; i < m->ndeps; i++)
        if ((s = init_module(p, m->deps[i], failed)) != CPU_OK || *failed) return s;
    if (m->ne->cs && m->ne->cs <= m->ne->seg_count) {
        loader_callregs rg;
        uint32_t ret;
        memset(&rg, 0, sizeof rg);
        rg.di = m->handle;
        rg.cx = m->heap_size;
        rg.ds = m->auto_ds;
        rg.es = p->psp_sel;
        rg.si = 0x81;
        s = loader_call_far(p, m->seg_sel[m->ne->cs - 1], m->ne->ip, NULL, 0, &rg, &ret);
        if (s != CPU_OK) return s;
        if ((ret & 0xFFFF) == 0) *failed = true;
    }
    return CPU_OK;
}

static cpu_status init_pending(loader_proc *p, bool *failed) {
    size_t i;
    cpu_status s;
    for (i = 1; i < p->nmods; i++)
        if ((s = init_module(p, p->mods[i], failed)) != CPU_OK || *failed) return s;
    return CPU_OK;
}

loader_err loader_load_library(loader_proc *p, const char *spec, loader_module **out, unsigned *winerr,
                               cpu_status *fault) {
    char key[16], file[16], path[LOADER_PATH_MAX];
    loader_module *m = NULL;
    ne_image tmp, *own;
    size_t i;
    int rc;
    bool failed = false;
    loader_err e;
    *out = NULL;
    *winerr = 0;
    *fault = CPU_OK;
    split_spec(spec, key, file);
    for (i = 1; i < p->nmods && !m; i++)
        if (!strcmp(p->mods[i]->file, file)) m = p->mods[i];
    if (m) {
        m->usage++;
        *out = m;
        return LOADER_OK;
    }
    if (!p->cfg.open_module) {
        *winerr = 2;
        return LOADER_ERR_NOTFOUND;
    }
    memset(&tmp, 0, sizeof tmp);
    path[0] = 0;
    if ((rc = p->cfg.open_module(p->cfg.open_ctx, spec, &tmp, path)) != 0) {
        *winerr = (unsigned)rc;
        return rc == 2 || rc == 3 ? LOADER_ERR_NOTFOUND : LOADER_ERR_FORMAT;
    }
    if (!(own = malloc(sizeof *own))) {
        ne_free(&tmp);
        *winerr = 8;
        return LOADER_ERR_NOMEM;
    }
    *own = tmp;
    if ((e = module_load(p, own, path, &m)) != LOADER_OK) {
        *winerr = e == LOADER_ERR_NOMEM ? 8 : 11;
        return e;
    }
    if ((*fault = init_module(p, m, &failed)) != CPU_OK || failed) {
        loader_free_library(p, m);
        *winerr = 0;
        return LOADER_ERR_INIT;
    }
    *out = m;
    return LOADER_OK;
}

/* ---- running ------------------------------------------------------------- */

loader_stop loader_run(loader_proc *p, uint64_t max_steps) {
    cpu_status s;
    bool failed = false;
    p->stop = LOADER_STOP_NONE;
    p->fault = CPU_OK;
    s = init_pending(p, &failed);
    if (s != CPU_OK || failed) {
        if (s == CPU_HALT) {
            if (p->stop == LOADER_STOP_NONE) p->stop = LOADER_STOP_HLT;
        } else if (s != CPU_OK) {
            p->stop = LOADER_STOP_FAULT;
            p->fault = s;
        } else {
            p->stop = LOADER_STOP_INIT;
        }
        return p->stop;
    }
    s = cpu_run(&p->cpu, max_steps);
    if (s == CPU_HALT) {
        if (p->stop == LOADER_STOP_NONE) p->stop = LOADER_STOP_HLT;
    } else if (s == CPU_BUDGET) {
        p->stop = LOADER_STOP_BUDGET;
    } else {
        p->stop = LOADER_STOP_FAULT;
        p->fault = s;
    }
    return p->stop;
}
