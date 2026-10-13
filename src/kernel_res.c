/* KERNEL resource functions over the NE resource table of the program. */
#include <stdlib.h>
#include <string.h>

#include "kernel_priv.h"

#define GMEM_MOVEABLE_DISCARDABLE 0x0102

/* The string behind a name/type far pointer: an integer id (segment 0), a
 * "#n" decimal id, or a name. buf must outlive the returned id. */
static bool resid_arg(hle_call *c, uint16_t sel, uint16_t off, char *buf, size_t cap, ne_resid *out) {
    if (sel == 0) {
        out->id = off;
        out->name = NULL;
        return true;
    }
    if (!hle_read_cstr(c, sel, off, buf, cap)) return false;
    if (buf[0] == '#') {
        out->id = (uint16_t)atoi(buf + 1);
        out->name = NULL;
    } else {
        out->id = 0;
        out->name = buf;
    }
    return true;
}

/* "resource: FindResource type #2 name "logo" -> ok": integer ids as #n, names quoted. */
static void trace_find(kernel_t *k, uint16_t ns, uint16_t no, uint16_t ts, uint16_t to, const char *nb, const char *tb, bool ok) {
    char t[160], n[160];
    if (ts == 0) snprintf(t, sizeof t, "#%u", to);
    else snprintf(t, sizeof t, tb[0] == '#' ? "%s" : "\"%s\"", tb);
    if (ns == 0) snprintf(n, sizeof n, "#%u", no);
    else snprintf(n, sizeof n, nb[0] == '#' ? "%s" : "\"%s\"", nb);
    fprintf(k->hle->log, "resource: FindResource type %s name %s -> %s\n", t, n, ok ? "ok" : "not found");
}

static kres *res_of(kernel_t *k, uint16_t inst, uint16_t hrsrc) {
    if (hrsrc == 0 || hrsrc > k->nres || k->res[hrsrc - 1].inst != inst || !k->res[hrsrc - 1].ne) return NULL;
    return &k->res[hrsrc - 1];
}

cpu_status k_findresource(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t inst, ns, no, ts, to;
    char nb[128], tb[128];
    ne_resid name, type;
    const ne_resource *r;
    const loader_module *lm;
    unsigned i;
    hle_args_init(&a, c);
    inst = hle_u16(&a);
    hle_far(&a, &ns, &no);
    hle_far(&a, &ts, &to);
    if (a.st) return a.st;
    hle_ret16(c, 0);
    if (!(lm = loader_module_by_handle(k->p, inst)) || !resid_arg(c, ns, no, nb, sizeof nb, &name) ||
        !resid_arg(c, ts, to, tb, sizeof tb, &type) || ne_find_resource(lm->ne, type, name, &r) != NE_OK) {
        if (k->hle && k->hle->trace && k->hle->log) trace_find(k, ns, no, ts, to, nb, tb, false);
        return CPU_OK;
    }
    if (k->hle && k->hle->trace && k->hle->log) trace_find(k, ns, no, ts, to, nb, tb, true);
    for (i = 0; i < k->nres; i++)
        if (k->res[i].r == r && k->res[i].ne) {
            hle_ret16(c, (uint16_t)(i + 1));
            return CPU_OK;
        }
    if (k->nres == k->rescap) {
        unsigned cap = k->rescap ? k->rescap * 2 : 32;
        kres *v = realloc(k->res, cap * sizeof *v);
        if (!v) return CPU_OK;
        k->res = v;
        k->rescap = cap;
    }
    memset(&k->res[k->nres], 0, sizeof k->res[0]);
    k->res[k->nres].r = r;
    k->res[k->nres].ne = lm->ne;
    k->res[k->nres].inst = inst;
    hle_ret16(c, (uint16_t)++k->nres);
    return CPU_OK;
}

cpu_status k_loadresource(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t inst, hr;
    kres *r;
    hle_args_init(&a, c);
    inst = hle_u16(&a);
    hr = hle_u16(&a);
    if (a.st) return a.st;
    hle_ret16(c, 0);
    if (!(r = res_of(k, inst, hr))) return CPU_OK;
    if (r->refs == 0) {
        uint8_t *dst;
        r->h = kernel_global_alloc(k, r->r->length, GMEM_MOVEABLE_DISCARDABLE);
        if (!r->h) return CPU_OK;
        dst = mem_ptr(&k->p->mem, r->h, 0, 1);
        if (dst && r->r->length) memcpy(dst, r->ne->data + r->r->offset, r->r->length);
    }
    r->refs++;
    hle_ret16(c, r->h);
    return CPU_OK;
}

cpu_status k_freeresource(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t h;
    unsigned i;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    if (a.st) return a.st;
    hle_ret16(c, h);
    for (i = 0; i < k->nres; i++) {
        kres *r = &k->res[i];
        if (r->refs && r->h == h) {
            if (--r->refs == 0) {
                kernel_global_free(k, h);
                r->h = 0;
            }
            hle_ret16(c, 0);
            break;
        }
    }
    return CPU_OK;
}

cpu_status k_sizeofresource(hle_call *c) {
    hle_args a;
    uint16_t inst, hr;
    kres *r;
    hle_args_init(&a, c);
    inst = hle_u16(&a);
    hr = hle_u16(&a);
    if (a.st) return a.st;
    r = res_of(c->ctx, inst, hr);
    hle_ret32(c, r ? r->r->length : 0);
    return CPU_OK;
}

cpu_status k_allocresource(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t inst, hr;
    uint32_t size;
    kres *r;
    hle_args_init(&a, c);
    inst = hle_u16(&a);
    hr = hle_u16(&a);
    size = hle_u32(&a);
    if (a.st) return a.st;
    r = res_of(k, inst, hr);
    hle_ret16(c, r ? kernel_global_alloc(k, size ? size : r->r->length, 0x0002) : 0);
    return CPU_OK;
}

void kernel_res_drop(kernel_t *k, uint16_t inst) {
    unsigned i;
    for (i = 0; i < k->nres; i++)
        if (k->res[i].inst == inst && k->res[i].ne) {
            if (k->res[i].refs) kernel_global_free(k, k->res[i].h);
            k->res[i].refs = 0;
            k->res[i].h = 0;
            k->res[i].ne = NULL;
        }
}

void kernel_res_destroy(kernel_t *k) {
    free(k->res);
    k->res = NULL;
    k->nres = k->rescap = 0;
}

const ne_resource *kernel_res_get(kernel_t *k, uint16_t inst, uint16_t hrsrc) {
    kres *r = res_of(k, inst, hrsrc);
    return r ? r->r : NULL;
}
