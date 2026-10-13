/* VER HLE. See ver.h.
 *
 * A 16-bit version resource is a tree of nodes: wLength (the whole node,
 * children included), wValueLength (bytes), the key (ANSI, NUL terminated),
 * padding to a DWORD relative to the block, the value, padding, then the
 * children. The block handed out by GetFileVersionInfo is the resource as
 * stored, so VerQueryValue walks the guest's copy and returns pointers into it. */
#include "ver.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define RT_VERSION 16

struct ver {
    kernel_t *k;
};

static unsigned rd16(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }
static unsigned align4(unsigned v) { return (v + 3) & ~3u; }

/* The RT_VERSION resource of the NE file at a DOS path, copied to *data (free it); 0 when there is none. */
static uint32_t read_version(ver_t *v, hle_call *c, uint16_t sel, uint16_t off, uint8_t **data) {
    char dos[260], host[1024];
    ne_image ne;
    const ne_resource *r = NULL;
    uint32_t len = 0;
    *data = NULL;
    if (!hle_read_cstr(c, sel, off, dos, sizeof dos) || !kernel_host_path(v->k, dos, host, sizeof host)) return 0;
    if (ne_open_file(&ne, host) != NE_OK) return 0;
    if (ne_find_resource(&ne, (ne_resid){RT_VERSION, NULL}, (ne_resid){1, NULL}, &r) != NE_OK) {
        size_t i;
        for (i = 0; i < ne.restype_count && !r; i++)
            if (!ne.restypes[i].name[0] && ne.restypes[i].id == RT_VERSION && ne.restypes[i].count)
                r = &ne.restypes[i].res[0];
    }
    if (r && r->length && (*data = malloc(r->length))) {
        memcpy(*data, ne.data + r->offset, r->length);
        len = r->length;
    }
    ne_free(&ne);
    return len;
}

static cpu_status v_info_size(hle_call *c) {
    ver_t *v = c->ctx;
    hle_args a;
    uint16_t ps, po, hs, ho;
    uint8_t *data, *h;
    uint32_t len;
    hle_args_init(&a, c);
    hle_far(&a, &ps, &po);
    hle_far(&a, &hs, &ho);
    if (a.st) return a.st;
    len = read_version(v, c, ps, po, &data);
    free(data);
    if (hs && (h = hle_ptr(c, hs, ho, 4))) memset(h, 0, 4);   /* the handle is not needed */
    hle_ret32(c, len);
    return CPU_OK;
}

static cpu_status v_info(hle_call *c) {
    ver_t *v = c->ctx;
    hle_args a;
    uint16_t ps, po, ds, dof;
    uint32_t handle, cb, len, n;
    uint8_t *data, *dst;
    hle_args_init(&a, c);
    hle_far(&a, &ps, &po);
    handle = hle_u32(&a);
    cb = hle_u32(&a);
    hle_far(&a, &ds, &dof);
    (void)handle;
    if (a.st) return a.st;
    len = read_version(v, c, ps, po, &data);
    if (!len) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    n = cb < len ? cb : len;
    if (!(dst = hle_ptr(c, ds, dof, n ? n : 1))) {
        free(data);
        return CPU_E_SEL;
    }
    memcpy(dst, data, n);
    free(data);
    hle_ret16(c, 1);
    return CPU_OK;
}

typedef struct {
    unsigned at, end;       /* the node inside the block */
    unsigned val, vlen;     /* its value */
    char key[64];
} vnode;

static bool node_at(const uint8_t *b, unsigned total, unsigned at, vnode *n) {
    unsigned klen = 0;
    if (at + 4 > total) return false;
    n->at = at;
    n->end = at + rd16(b + at);
    n->vlen = rd16(b + at + 2);
    if (n->end > total || n->end < at + 4) return false;
    while (at + 4 + klen < n->end && b[at + 4 + klen]) klen++;
    if (at + 4 + klen >= n->end) return false;
    snprintf(n->key, sizeof n->key, "%.*s", (int)klen, (const char *)b + at + 4);
    n->val = align4(at + 4 + klen + 1);
    return n->val <= n->end;
}

/* The node reached from the root by the backslash-separated keys of sub, ignoring case. */
static bool find_node(const uint8_t *b, unsigned total, const char *sub, vnode *out) {
    vnode cur;
    if (!node_at(b, total, 0, &cur)) return false;
    while (*sub) {
        char key[64];
        size_t k = strcspn(sub, "\\");
        unsigned at;
        vnode child;
        bool found = false;
        if (k == 0) {
            sub++;
            continue;
        }
        snprintf(key, sizeof key, "%.*s", (int)(k < sizeof key - 1 ? k : sizeof key - 1), sub);
        sub += k;
        for (at = align4(cur.val + cur.vlen); at + 4 <= cur.end && node_at(b, total, at, &child);
             at = align4(child.end)) {
            if (!strcasecmp(child.key, key)) {
                found = true;
                break;
            }
            if (child.end <= at) break;
        }
        if (!found) return false;
        cur = child;
    }
    *out = cur;
    return true;
}

static cpu_status v_query(hle_call *c) {
    hle_args a;
    uint16_t bs, bo, ss, so, ps, po, ls, lo;
    char sub[260];
    const uint8_t *b;
    uint8_t *pp, *lp;
    vnode n;
    unsigned total;
    bool ok = false;
    hle_args_init(&a, c);
    hle_far(&a, &bs, &bo);
    hle_far(&a, &ss, &so);
    hle_far(&a, &ps, &po);
    hle_far(&a, &ls, &lo);
    if (a.st) return a.st;
    pp = hle_ptr(c, ps, po, 4);
    lp = hle_ptr(c, ls, lo, 2);
    if (!pp || !lp || !hle_read_cstr(c, ss, so, sub, sizeof sub)) return CPU_E_SEL;
    memset(pp, 0, 4);
    memset(lp, 0, 2);
    if ((b = hle_ptr(c, bs, bo, 4))) {
        total = rd16(b);
        if (total >= 4 && (b = hle_ptr(c, bs, bo, total)) && find_node(b, total, sub, &n)) {
            unsigned off = bo + n.val;
            pp[0] = (uint8_t)off; pp[1] = (uint8_t)(off >> 8);
            pp[2] = (uint8_t)bs; pp[3] = (uint8_t)(bs >> 8);
            lp[0] = (uint8_t)n.vlen; lp[1] = (uint8_t)(n.vlen >> 8);
            ok = true;
        }
    }
    hle_ret16(c, ok);
    return CPU_OK;
}

ver_t *ver_create(hle_t *h, kernel_t *k) {
    static const hle_def defs[] = {
        {"VER", 6, "GetFileVersionInfoSize", 8, v_info_size},
        {"VER", 7, "GetFileVersionInfo", 16, v_info},
        {"VER", 11, "VerQueryValue", 16, v_query},
    };
    ver_t *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->k = k;
    if (!hle_register_table(h, defs, sizeof defs / sizeof defs[0], v)) {
        free(v);
        return NULL;
    }
    return v;
}

void ver_destroy(ver_t *v) { free(v); }
