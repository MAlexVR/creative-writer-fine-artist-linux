/* HLE dispatcher. See hle.h. */
#include "hle.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    return *a == *b;
}

void hle_init(hle_t *h) { memset(h, 0, sizeof *h); }

void hle_destroy(hle_t *h) {
    size_t i;
    for (i = 0; i < h->count; i++) free(h->entries[i]);
    free(h->entries);
    free(h->rec);
    memset(h, 0, sizeof *h);
}

const hle_entry *hle_find(const hle_t *h, const char *module, uint16_t ordinal) {
    size_t i;
    for (i = 0; i < h->count; i++)
        if (h->entries[i]->ordinal == ordinal && ieq(h->entries[i]->module, module))
            return h->entries[i];
    return NULL;
}

const hle_entry *hle_find_name(const hle_t *h, const char *module, const char *name) {
    size_t i;
    for (i = 0; i < h->count; i++)
        if (h->entries[i]->name[0] && ieq(h->entries[i]->module, module) &&
            ieq(h->entries[i]->name, name))
            return h->entries[i];
    return NULL;
}

static hle_entry *add_entry(hle_t *h, const char *module, uint16_t ordinal, const char *name) {
    hle_entry *e;
    if (hle_find(h, module, ordinal) || strlen(module) > NE_NAME_MAX ||
        (name && strlen(name) > NE_NAME_MAX))
        return NULL;
    if (h->count == h->cap) {
        size_t cap = h->cap ? h->cap * 2 : 64;
        hle_entry **v = realloc(h->entries, cap * sizeof *v);
        if (!v) return NULL;
        h->entries = v;
        h->cap = cap;
    }
    e = calloc(1, sizeof *e);
    if (!e) return NULL;
    strcpy(e->module, module);
    if (name) strcpy(e->name, name);
    e->ordinal = ordinal;
    e->hle = h;
    h->entries[h->count++] = e;
    return e;
}

bool hle_register(hle_t *h, const hle_def *def, void *ctx) {
    hle_entry *e = add_entry(h, def->module, def->ordinal, def->name);
    if (!e) return false;
    e->arg_bytes = def->arg_bytes;
    e->fn = def->fn;
    e->ctx = ctx;
    return true;
}

bool hle_register_table(hle_t *h, const hle_def *defs, size_t n, void *ctx) {
    size_t i;
    for (i = 0; i < n; i++)
        if (!hle_register(h, &defs[i], ctx)) return false;
    return true;
}

bool hle_register_const(hle_t *h, const char *module, uint16_t ordinal, const char *name,
                        uint16_t value, uint32_t seg_bytes) {
    hle_entry *e = add_entry(h, module, ordinal, name);
    if (!e) return false;
    e->is_const = true;
    e->const_off = value;
    e->const_seg_bytes = seg_bytes;
    return true;
}

size_t hle_recent(const hle_t *h, hle_record *out, size_t max) {
    size_t n = h->ncalls < HLE_RING ? (size_t)h->ncalls : HLE_RING, i;
    if (n > max) n = max;
    for (i = 0; i < n; i++) out[i] = h->ring[(h->ncalls - n + i) % HLE_RING];
    return n;
}

/* ---- call path -------------------------------------------------------- */

static void note(hle_t *h, const hle_entry *e, const loader_import *imp, bool implemented,
                 uint16_t cs, uint16_t ip) {
    hle_record r;
    memset(&r, 0, sizeof r);
    r.entry = e;
    snprintf(r.module, sizeof r.module, "%s", imp->module);
    r.by_name = imp->by_name;
    r.ordinal = imp->by_name ? 0 : imp->ordinal;
    snprintf(r.name, sizeof r.name, "%s", imp->by_name ? imp->name : e ? e->name : "");
    r.implemented = implemented;
    r.caller_cs = cs;
    r.caller_ip = ip;
    h->ring[h->ncalls % HLE_RING] = r;
    h->ncalls++;
    if (!implemented) {
        h->last_unimpl = r;
        h->unimpl_calls++;
    }
    if (h->record && h->nrec < HLE_RECORD_MAX) {
        if (h->nrec == h->caprec) {
            size_t cap = h->caprec ? h->caprec * 2 : 256;
            hle_record *v = realloc(h->rec, cap * sizeof *v);
            if (!v) goto log;
            h->rec = v;
            h->caprec = cap;
        }
        h->rec[h->nrec++] = r;
    }
log:
    if (h->log && (h->trace || !implemented)) {
        const char *tag = implemented ? "" : "unimplemented ";
        if (r.by_name)
            fprintf(h->log, "hle: %s%s.%s called from %04x:%04x\n", tag, r.module, r.name, cs, ip);
        else
            fprintf(h->log, "hle: %s%s.%u%s%s called from %04x:%04x\n", tag, r.module, r.ordinal,
                    r.name[0] ? " " : "", r.name, cs, ip);
    }
}

static void caller_of(cpu_t *c, uint16_t *cs, uint16_t *ip) {
    uint16_t sp = (uint16_t)c->r[CPU_ESP];
    *ip = 0;
    *cs = 0;
    cpu_read16(c, c->seg[CPU_SS], sp, ip);
    cpu_read16(c, c->seg[CPU_SS], (uint16_t)(sp + 2), cs);
}

static cpu_status dispatch(loader_proc *p, const loader_import *imp, void *ctx) {
    hle_entry *e = ctx;
    hle_call call;
    call.hle = e->hle;
    call.proc = p;
    call.cpu = &p->cpu;
    call.entry = e;
    call.ctx = e->ctx;
    caller_of(&p->cpu, &call.caller_cs, &call.caller_ip);
    e->calls++;
    note(e->hle, e, imp, true, call.caller_cs, call.caller_ip);
    if (e->hle->log && e->hle->trace && e->hle->trace_args) {
        uint16_t sp = (uint16_t)p->cpu.r[CPU_ESP], i, v;
        fprintf(e->hle->log, "hle:   args(%u):", e->arg_bytes);
        for (i = 0; i + 1 < e->arg_bytes; i += 2) {      /* the return address is 4 bytes; the first argument is deepest */
            v = 0;
            cpu_read16(&p->cpu, p->cpu.seg[CPU_SS], (uint16_t)(sp + 4 + e->arg_bytes - 2 - i), &v);
            fprintf(e->hle->log, " %04x", v);
        }
        fprintf(e->hle->log, "\n");
    }
    if (e->hle->tap) e->hle->tap(e->hle->tap_ctx, &call);
    {
        cpu_status st = e->fn(&call);
        if (e->hle->log && e->hle->trace)    /* what the call answered, DX:AX */
            fprintf(e->hle->log, "hle:    = %04x:%04x\n", (unsigned)(p->cpu.r[CPU_EDX] & 0xFFFF),
                    (unsigned)(p->cpu.r[CPU_EAX] & 0xFFFF));
        return st;
    }
}

static cpu_status unimpl(loader_proc *p, const loader_import *imp, void *ctx) {
    hle_t *h = ctx;
    uint16_t cs, ip;
    caller_of(&p->cpu, &cs, &ip);
    note(h, imp->by_name ? hle_find_name(h, imp->module, imp->name) : hle_find(h, imp->module, imp->ordinal),
         imp, false, cs, ip);
    return loader_request_stop(p);
}

bool hle_resolve(void *ctx, const loader_import *imp, loader_api *out) {
    hle_t *h = ctx;
    const hle_entry *e = imp->by_name ? hle_find_name(h, imp->module, imp->name)
                                      : hle_find(h, imp->module, imp->ordinal);
    memset(out, 0, sizeof *out);
    if (e && e->is_const) {
        out->is_const = true;
        out->const_off = e->const_off;
        out->const_seg_bytes = e->const_seg_bytes;
        return true;
    }
    if (e && e->fn) {
        out->fn = dispatch;
        out->arg_bytes = e->arg_bytes;
        out->ctx = (void *)e;
        return true;
    }
    if (!h->stub_unknown) return false;
    out->fn = unimpl;
    out->ctx = h;
    return true;
}

/* ---- handler helpers --------------------------------------------------- */

void hle_args_init(hle_args *a, hle_call *call) {
    a->call = call;
    a->left = call->entry->arg_bytes;
    a->st = CPU_OK;
}

static uint32_t arg_at(hle_args *a, unsigned size) {
    uint32_t v = 0;
    cpu_t *c = a->call->cpu;
    uint16_t sp = (uint16_t)c->r[CPU_ESP];
    cpu_status s;
    if (a->left < size) {
        if (!a->st) a->st = CPU_E_UD;
        return 0;
    }
    a->left -= size;
    if (size == 2) {
        uint16_t w = 0;
        s = cpu_read16(c, c->seg[CPU_SS], (uint16_t)(sp + 4 + a->left), &w);
        v = w;
    } else {
        s = cpu_read32(c, c->seg[CPU_SS], (uint16_t)(sp + 4 + a->left), &v);
    }
    if (s != CPU_OK && !a->st) a->st = s;
    return v;
}

uint16_t hle_u16(hle_args *a) { return (uint16_t)arg_at(a, 2); }
uint32_t hle_u32(hle_args *a) { return arg_at(a, 4); }

void hle_far(hle_args *a, uint16_t *sel, uint16_t *off) {
    uint32_t v = arg_at(a, 4);
    *off = (uint16_t)v;
    *sel = (uint16_t)(v >> 16);
}

cpu_status hle_invoke(hle_t *h, loader_proc *p, const char *module, uint16_t ordinal, const uint16_t *words,
                      unsigned nwords, uint32_t *dxax) {
    const hle_entry *e = hle_find(h, module, ordinal);
    cpu_t *c = &p->cpu;
    uint32_t esp = c->r[CPU_ESP];
    uint16_t sp = (uint16_t)esp, ss = c->seg[CPU_SS];
    unsigned i;
    cpu_status st = CPU_OK;
    hle_call call;
    if (!e || !e->fn || e->arg_bytes != nwords * 2u) return CPU_E_UD;
    for (i = 0; i < nwords && st == CPU_OK; i++) {
        sp = (uint16_t)(sp - 2);
        st = cpu_write16(c, ss, sp, words[i]);
    }
    for (i = 0; i < 2 && st == CPU_OK; i++) {          /* the far return address the handler would see */
        sp = (uint16_t)(sp - 2);
        st = cpu_write16(c, ss, sp, 0);
    }
    if (st != CPU_OK) {
        c->r[CPU_ESP] = esp;
        return st;
    }
    c->r[CPU_ESP] = (esp & 0xFFFF0000u) | sp;
    call.hle = h;
    call.proc = p;
    call.cpu = c;
    call.entry = e;
    call.ctx = e->ctx;
    call.caller_cs = call.caller_ip = 0;
    st = e->fn(&call);
    c->r[CPU_ESP] = esp;
    if (dxax) *dxax = (c->r[CPU_EDX] & 0xFFFF) << 16 | (c->r[CPU_EAX] & 0xFFFF);
    return st;
}

void hle_ret16(hle_call *call, uint16_t v) {
    call->cpu->r[CPU_EAX] = (call->cpu->r[CPU_EAX] & 0xFFFF0000u) | v;
}

void hle_ret32(hle_call *call, uint32_t v) {
    hle_ret16(call, (uint16_t)v);
    call->cpu->r[CPU_EDX] = (call->cpu->r[CPU_EDX] & 0xFFFF0000u) | (v >> 16);
}

void *hle_ptr(hle_call *call, uint16_t sel, uint32_t off, uint32_t len) {
    return mem_ptr(&call->proc->mem, sel, off, len);
}

bool hle_read_cstr(hle_call *call, uint16_t sel, uint16_t off, char *out, size_t cap) {
    size_t i = 0;
    out[0] = 0;
    if (!cap) return false;
    for (;;) {
        const uint8_t *p = mem_ptr(&call->proc->mem, sel, (uint32_t)off + i, 1);
        if (!p) return false;
        if (*p == 0) break;
        if (i + 1 < cap) out[i] = (char)*p;
        i++;
        if (off + i > 0xFFFF) break;
    }
    out[i < cap ? i : cap - 1] = 0;
    return true;
}

void hle_huge_add(uint16_t *sel, uint32_t *off, uint32_t delta) {
    uint32_t t = *off + delta;
    *sel = mem_huge_sel(*sel, t >> 16);
    *off = t & 0xFFFF;
}
