/* Metafiles (Win16 METAHEADER + records). A metafile handle is the global handle of its bits, so SetMetaFileBits
 * and GetMetaFileBits hand the same block back. Playing replays each record through the GDI handlers themselves
 * (hle_invoke with the arguments pushed on the guest stack), so a record draws exactly what the API call would. */
#include "gdi_priv.h"
#include "kernel_priv.h"

#define META_EOF 0x0000
#define META_REALIZEPALETTE 0x0035
#define META_SETPALENTRIES 0x0037
#define META_SAVEDC 0x001E
#define META_DIBCREATEPATTERNBRUSH 0x0142
#define META_DELETEOBJECT 0x01F0
#define META_CREATEPALETTE 0x00F7
#define META_SELECTOBJECT 0x012D
#define META_SELECTPALETTE 0x0234
#define META_CREATEPENINDIRECT 0x02FA
#define META_CREATEFONTINDIRECT 0x02FB
#define META_CREATEBRUSHINDIRECT 0x02FC
#define META_TEXTOUT 0x0521
#define META_EXTTEXTOUT 0x0A32
#define META_POLYGON 0x0324
#define META_POLYLINE 0x0325
#define META_POLYPOLYGON 0x0538
#define META_DIBBITBLT 0x0940
#define META_DIBSTRETCHBLT 0x0B41
#define META_STRETCHDIB 0x0F43
#define META_SETDIBTODEV 0x0D33

#define MAX_HANDLES 4096

/* Records that are one GDI call with only numeric arguments: the record holds the arguments after the DC, last
 * argument first, which is the order they sit on the stack. */
typedef struct { uint16_t func, ordinal; uint8_t np; } simple_rec;
static const simple_rec simple[] = {
    {0x0201, 1, 2},  /* SetBkColor */
    {0x0102, 2, 1},  /* SetBkMode */
    {0x0103, 3, 1},  /* SetMapMode */
    {0x0104, 4, 1},  /* SetROP2 */
    {0x0105, 5, 1},  /* SetRelAbs */
    {0x0106, 6, 1},  /* SetPolyFillMode */
    {0x0107, 7, 1},  /* SetStretchBltMode */
    {0x0108, 8, 1},  /* SetTextCharacterExtra */
    {0x0209, 9, 2},  /* SetTextColor */
    {0x020A, 10, 2}, /* SetTextJustification */
    {0x020B, 11, 2}, /* SetWindowOrg */
    {0x020C, 12, 2}, /* SetWindowExt */
    {0x020D, 13, 2}, /* SetViewportOrg */
    {0x020E, 14, 2}, /* SetViewportExt */
    {0x020F, 15, 2}, /* OffsetWindowOrg */
    {0x0400, 16, 4}, /* ScaleWindowExt */
    {0x0211, 17, 2}, /* OffsetViewportOrg */
    {0x0412, 18, 4}, /* ScaleViewportExt */
    {0x0213, 19, 2}, /* LineTo */
    {0x0214, 20, 2}, /* MoveTo */
    {0x0415, 21, 4}, /* ExcludeClipRect */
    {0x0416, 22, 4}, /* IntersectClipRect */
    {0x0817, 23, 8}, /* Arc */
    {0x0418, 24, 4}, /* Ellipse */
    {0x0419, 25, 4}, /* FloodFill */
    {0x081A, 26, 8}, /* Pie */
    {0x041B, 27, 4}, /* Rectangle */
    {0x061C, 28, 6}, /* RoundRect */
    {0x061D, 29, 6}, /* PatBlt */
    {0x001E, 30, 0}, /* SaveDC */
    {0x041F, 31, 4}, /* SetPixel */
    {0x0220, 32, 2}, /* OffsetClipRgn */
    {0x0127, 39, 1}, /* RestoreDC */
    {0x012E, 346, 1}, /* SetTextAlign */
    {0x0830, 348, 8}, /* Chord */
    {0x0548, 372, 5}, /* ExtFloodFill */
};
#define NSIMPLE (sizeof simple / sizeof simple[0])

/* The Ex forms record like the plain ones (the last argument is a POINT/SIZE pointer for the old value). */
static const struct { uint16_t func, ordinal; } exrec[] = {
    {0x020D, 480}, {0x020E, 479}, {0x020B, 482}, {0x020C, 481}, {0x0211, 476}, {0x020F, 477}};
#define NEXREC (sizeof exrec / sizeof exrec[0])

static const simple_rec *simple_by_func(uint16_t f) {
    size_t i;
    for (i = 0; i < NSIMPLE; i++)
        if (simple[i].func == f) return &simple[i];
    return NULL;
}

/* ---- bits ------------------------------------------------------------------------------------- */

static kernel_t *kernel_of(gdi_t *g) {
    const hle_entry *e = hle_find(g->hle, "KERNEL", 15);
    return e ? e->ctx : NULL;
}

/* The bits of a metafile block: the pointer and the byte count the header declares, or NULL if it is not one. */
static const uint8_t *meta_bits(gdi_t *g, uint16_t h, uint32_t *len) {
    uint32_t have = mem_size(&g->p->mem, h), words;
    const uint8_t *p;
    if (have < 18 || !(p = mem_ptr(&g->p->mem, h, 0, 18))) return NULL;
    if ((gld16(p) != 1 && gld16(p) != 2) || gld16(p + 2) != 9) return NULL;
    if (gld16(p + 4) != 0x0300 && gld16(p + 4) != 0x0100) return NULL;
    words = gld32(p + 6);
    if (words < 9 || words * 2 > have) return NULL;
    if (len) *len = words * 2;
    return mem_ptr(&g->p->mem, h, 0, words * 2);
}

static uint16_t block_alloc(gdi_t *g, const uint8_t *data, uint32_t len) {
    kernel_t *k = kernel_of(g);
    uint16_t h = k ? kernel_global_alloc(k, len, 0x0002) : mem_alloc(&g->p->mem, len, MEM_DATA);
    uint8_t *p = h ? mem_ptr(&g->p->mem, h, 0, len) : NULL;
    if (!p) return 0;
    memcpy(p, data, len);
    return h;
}


static void block_free(gdi_t *g, uint16_t h) {
    kernel_t *k = kernel_of(g);
    if (k) kernel_global_free(k, h);
    else mem_free(&g->p->mem, h);
}

/* ---- playing ---------------------------------------------------------------------------------- */

typedef struct {
    gdi_t *g;
    uint16_t hdc;
    uint16_t *ht;          /* handle table: objects the records created, by their index */
    unsigned nht;
    uint16_t tmp;          /* guest block for the pointer arguments of the call in progress */
    uint32_t tmpsize;
    cpu_status st;
} gplay;

static bool inv(gplay *p, const char *mod, uint16_t ord, const uint16_t *w, unsigned n, uint32_t *r) {
    cpu_status st = hle_invoke(p->g->hle, p->g->p, mod, ord, w, n, r);
    if (st != CPU_OK) p->st = st;
    return st == CPU_OK;
}

/* A zeroed guest block of at least n bytes for call arguments. */
static uint8_t *tmp_block(gplay *p, uint32_t n) {
    if (n < 64) n = 64;
    if (p->tmp && p->tmpsize < n) {
        mem_free(&p->g->p->mem, p->tmp);
        p->tmp = 0;
    }
    if (!p->tmp) {
        p->tmp = mem_alloc(&p->g->p->mem, n, MEM_DATA);
        p->tmpsize = n;
    }
    return p->tmp ? mem_ptr(&p->g->p->mem, p->tmp, 0, n) : NULL;
}

static int ht_add(gplay *p, uint16_t h) {
    unsigned i;
    if (!h) return -1;
    for (i = 0; i < p->nht; i++)
        if (!p->ht[i]) {
            p->ht[i] = h;
            return (int)i;
        }
    {
        uint16_t del[1] = {h};
        inv(p, GDI_MODULE, 69, del, 1, NULL);
    }
    return -1;
}

/* Where a DIB in a record puts its bits: the header and colour table first. */
static uint32_t dib_bits_at(const uint8_t *d, uint32_t avail, unsigned usage) {
    uint32_t size, ncol = 0, ent = 4, clr;
    unsigned bpp;
    if (avail < 12) return 0;
    size = gld32(d);
    if (size == 12) {
        bpp = gld16(d + 10);
        ent = 3;
    } else if (size >= 40 && avail >= 40) {
        bpp = gld16(d + 14);
        clr = gld32(d + 32);
        if (bpp <= 8) ncol = clr ? clr : 1u << bpp;
        if (ncol > 256) ncol = 256;
    } else {
        return 0;
    }
    if (size == 12 && bpp <= 8) ncol = 1u << bpp;
    return size + ncol * (usage == 1 ? 2u : ent);
}

/* Copies the DIB that ends a record into the scratch block: the far pointers of its header and its bits. */
static bool dib_in_tmp(gplay *p, const uint8_t *dib, uint32_t n, unsigned usage, uint16_t bmi[2], uint16_t bits[2]) {
    uint32_t at = dib_bits_at(dib, n, usage);
    uint8_t *t;
    if (!at || at > n || !(t = tmp_block(p, n + 4))) return false;
    memcpy(t, dib, n);
    bmi[0] = p->tmp;
    bmi[1] = 0;
    bits[0] = mem_huge_sel(p->tmp, at >> 16);
    bits[1] = (uint16_t)at;
    return true;
}

/* Plays one record; p->ht and the DC are the state. False stops the whole metafile (a fault or the end). */
static bool play_record(gplay *p, const uint8_t *rec, uint32_t words) {
    uint16_t func = gld16(rec + 4);
    unsigned np = words - 3;
    const uint8_t *prm = rec + 6;
    uint16_t w[40];
    uint32_t r;
    unsigned i;
    const simple_rec *s;
    if (func == META_EOF) return false;
    if ((s = simple_by_func(func)) != NULL) {
        if (np < s->np || s->np > 12) return true;
        w[0] = p->hdc;
        for (i = 0; i < s->np; i++) w[1 + i] = gld16(prm + 2 * (s->np - 1 - i));
        inv(p, GDI_MODULE, s->ordinal, w, 1u + s->np, &r);
        return p->st == CPU_OK;
    }
    switch (func) {
    case META_SELECTOBJECT: {
        unsigned idx = np ? gld16(prm) : MAX_HANDLES;
        if (idx < p->nht && p->ht[idx]) {
            w[0] = p->hdc;
            w[1] = p->ht[idx];
            inv(p, GDI_MODULE, 45, w, 2, &r);
        }
        break;
    }
    case META_DELETEOBJECT: {
        unsigned idx = np ? gld16(prm) : MAX_HANDLES;
        if (idx < p->nht && p->ht[idx]) {
            w[0] = p->ht[idx];
            inv(p, GDI_MODULE, 69, w, 1, &r);
            p->ht[idx] = 0;
        }
        break;
    }
    case META_CREATEPENINDIRECT:
    case META_CREATEBRUSHINDIRECT:
    case META_CREATEFONTINDIRECT:
    case META_CREATEPALETTE: {
        uint32_t bytes = np * 2u;
        uint8_t *t = tmp_block(p, bytes + 16);
        uint16_t ord = func == META_CREATEPENINDIRECT ? 62 : func == META_CREATEBRUSHINDIRECT ? 50
                       : func == META_CREATEFONTINDIRECT ? 57 : 360;
        if (!t || (func == META_CREATEPENINDIRECT && bytes < 10) || (func == META_CREATEBRUSHINDIRECT && bytes < 8) ||
            (func == META_CREATEFONTINDIRECT && bytes < 50))
            break;
        memcpy(t, prm, bytes);
        w[0] = p->tmp;                                        /* a far pointer is pushed selector first, then offset */
        w[1] = 0;
        inv(p, GDI_MODULE, ord, w, 2, &r);
        ht_add(p, (uint16_t)r);
        break;
    }
    case META_TEXTOUT: {          /* count, string (padded to a word), y, x */
        unsigned n = np ? gld16(prm) : 0, padded = (n + 1u) & ~1u;
        uint8_t *t;
        if (np < 1 + padded / 2 + 2) break;
        if (!(t = tmp_block(p, n + 2))) break;
        memcpy(t, prm + 2, n);
        t[n] = 0;
        w[0] = p->hdc;
        w[1] = gld16(prm + 2 + padded + 2);                      /* x */
        w[2] = gld16(prm + 2 + padded);                          /* y */
        w[3] = p->tmp;
        w[4] = 0;
        w[5] = (uint16_t)n;
        inv(p, GDI_MODULE, 33, w, 6, &r);
        break;
    }
    case META_EXTTEXTOUT: {       /* y, x, count, options, [rect], string (padded), dx... */
        unsigned n, opt, at = 4, padded, bytes;
        uint8_t *t;
        if (np < 4) break;
        n = gld16(prm + 4);
        opt = gld16(prm + 6);
        at = 8;
        if (opt & 6) at += 8;                                    /* ETO_OPAQUE | ETO_CLIPPED carry a rectangle */
        padded = (n + 1u) & ~1u;
        if (at + padded > np * 2u) break;
        bytes = np * 2u - at - padded;                           /* what is left is the dx array */
        if (!(t = tmp_block(p, 16 + n + 2 + bytes))) break;
        if (opt & 6) memcpy(t, prm + 8, 8);                      /* the rectangle at tmp:0 */
        memcpy(t + 16, prm + at, n);
        t[16 + n] = 0;
        t[16 + n + 1] = 0;
        if (bytes >= n * 2u) memcpy(t + 16 + n + 2, prm + at + padded, n * 2u);
        w[0] = p->hdc;
        w[1] = gld16(prm + 2);                                   /* x */
        w[2] = gld16(prm);                                       /* y */
        w[3] = (uint16_t)opt;
        w[4] = (opt & 6) ? p->tmp : 0;                           /* lprect */
        w[5] = 0;
        w[6] = p->tmp;                                           /* lpString */
        w[7] = 16;
        w[8] = (uint16_t)n;
        w[9] = bytes >= n * 2u && n ? p->tmp : 0;                /* lpDx */
        w[10] = bytes >= n * 2u && n ? (uint16_t)(16 + n + 2) : 0;
        inv(p, GDI_MODULE, 351, w, 11, &r);
        break;
    }
    case META_POLYGON:
    case META_POLYLINE: {         /* count, then x, y pairs */
        unsigned n = np ? gld16(prm) : 0;
        uint8_t *t;
        if (np < 1 + n * 2 || n < 1 || !(t = tmp_block(p, n * 4u))) break;
        memcpy(t, prm + 2, n * 4u);
        w[0] = p->hdc;
        w[1] = p->tmp;
        w[2] = 0;
        w[3] = (uint16_t)n;
        inv(p, GDI_MODULE, func == META_POLYGON ? 36 : 37, w, 4, &r);
        break;
    }
    case META_POLYPOLYGON: {      /* polygon count, point counts, then all the points */
        unsigned npoly = np ? gld16(prm) : 0, i2, total = 0;
        uint8_t *t;
        if (np < 1 + npoly || npoly < 1) break;
        for (i2 = 0; i2 < npoly; i2++) total += gld16(prm + 2 + 2 * i2);
        if (np < 1 + npoly + total * 2 || !(t = tmp_block(p, npoly * 2u + total * 4u + 4))) break;
        memcpy(t, prm + 2, npoly * 2u);                          /* counts at tmp:0 */
        memcpy(t + npoly * 2u, prm + 2 + npoly * 2u, total * 4u); /* points follow */
        w[0] = p->hdc;
        w[1] = p->tmp;
        w[2] = (uint16_t)(npoly * 2u);                           /* lppt */
        w[3] = p->tmp;
        w[4] = 0;                                                /* lpcounts */
        w[5] = (uint16_t)npoly;
        inv(p, GDI_MODULE, 450, w, 6, &r);
        break;
    }
    case META_STRETCHDIB:         /* rop (lo, hi), usage, cySrc, cxSrc, ySrc, xSrc, cyDest, cxDest, yDest, xDest, DIB */
    case META_DIBSTRETCHBLT:      /* rop (lo, hi), cySrc, cxSrc, ySrc, xSrc, cyDest, cxDest, yDest, xDest, DIB */
    case META_DIBBITBLT: {        /* rop (lo, hi), ySrc, xSrc, cy, cx, yDest, xDest, DIB */
        unsigned usage = func == META_STRETCHDIB ? gld16(prm + 4) : 0;
        unsigned k = func == META_STRETCHDIB ? 11 : func == META_DIBSTRETCHBLT ? 10 : 8;
        unsigned o = func == META_STRETCHDIB ? 3 : 2;           /* where the geometry starts */
        uint16_t bmi[2], bits[2];
        int xd, yd, cxd, cyd, xs, ys, cxs, cys;
        if (np < k + 6 || !dib_in_tmp(p, prm + k * 2, (np - k) * 2u, usage, bmi, bits)) break;
        if (func == META_DIBBITBLT) {
            ys = (int16_t)gld16(prm + 2 * o);
            xs = (int16_t)gld16(prm + 2 * o + 2);
            cyd = cys = (int16_t)gld16(prm + 2 * o + 4);
            cxd = cxs = (int16_t)gld16(prm + 2 * o + 6);
            yd = (int16_t)gld16(prm + 2 * o + 8);
            xd = (int16_t)gld16(prm + 2 * o + 10);
        } else {
            cys = (int16_t)gld16(prm + 2 * o);
            cxs = (int16_t)gld16(prm + 2 * o + 2);
            ys = (int16_t)gld16(prm + 2 * o + 4);
            xs = (int16_t)gld16(prm + 2 * o + 6);
            cyd = (int16_t)gld16(prm + 2 * o + 8);
            cxd = (int16_t)gld16(prm + 2 * o + 10);
            yd = (int16_t)gld16(prm + 2 * o + 12);
            xd = (int16_t)gld16(prm + 2 * o + 14);
        }
        w[0] = p->hdc;
        w[1] = (uint16_t)xd; w[2] = (uint16_t)yd; w[3] = (uint16_t)cxd; w[4] = (uint16_t)cyd;
        w[5] = (uint16_t)xs; w[6] = (uint16_t)ys; w[7] = (uint16_t)cxs; w[8] = (uint16_t)cys;
        w[9] = bits[0]; w[10] = bits[1];
        w[11] = bmi[0]; w[12] = bmi[1];
        w[13] = (uint16_t)usage;
        w[14] = gld16(prm + 2);                                  /* rop high word first, then low */
        w[15] = gld16(prm);
        inv(p, GDI_MODULE, 439, w, 16, &r);
        break;
    }
    case META_SETDIBTODEV: {      /* usage, numscans, startscan, ySrc, xSrc, cy, cx, yDest, xDest, DIB */
        uint16_t bmi[2], bits[2];
        if (np < 9 + 6 || !dib_in_tmp(p, prm + 18, (np - 9) * 2u, gld16(prm), bmi, bits)) break;
        w[0] = p->hdc;
        w[1] = gld16(prm + 16);                                  /* xDest */
        w[2] = gld16(prm + 14);                                  /* yDest */
        w[3] = gld16(prm + 12);                                  /* cx */
        w[4] = gld16(prm + 10);                                  /* cy */
        w[5] = gld16(prm + 8);                                   /* xSrc */
        w[6] = gld16(prm + 6);                                   /* ySrc */
        w[7] = gld16(prm + 4);                                   /* startscan */
        w[8] = gld16(prm + 2);                                   /* numscans */
        w[9] = bits[0]; w[10] = bits[1];
        w[11] = bmi[0]; w[12] = bmi[1];
        w[13] = gld16(prm);                                      /* usage */
        inv(p, GDI_MODULE, 443, w, 14, &r);
        break;
    }
    default:
        break;
    }
    return p->st == CPU_OK;
}

static void play(gplay *p, const uint8_t *bits, uint32_t len) {
    uint32_t off = gld16(bits + 2) * 2u;
    uint16_t w[2];
    w[0] = p->hdc;
    inv(p, GDI_MODULE, 30, w, 1, NULL);                       /* SaveDC: the records must not leak into the DC */
    while (off + 6 <= len && p->st == CPU_OK) {
        uint32_t words = gld32(bits + off);
        if (words < 3 || off + words * 2u > len) break;
        if (!play_record(p, bits + off, words)) break;
        off += words * 2u;
    }
    w[1] = (uint16_t)-1;
    inv(p, GDI_MODULE, 39, w, 2, NULL);                       /* RestoreDC(-1) */
}

static void play_free(gplay *p) {
    unsigned i;
    for (i = 0; i < p->nht; i++)
        if (p->ht[i]) {
            uint16_t w[1] = {p->ht[i]};
            inv(p, GDI_MODULE, 69, w, 1, NULL);
        }
    if (p->tmp) mem_free(&p->g->p->mem, p->tmp);
    free(p->ht);
}

/* ---- recording -------------------------------------------------------------------------------- */

#define META_OBJS 256

/* A metafile in the making: the records so far and the GDI objects they selected, by index. The calls the program
 * makes on its DC are noted by the hle tap before the handlers run them on the memory DC behind it. */
typedef struct gmeta {
    uint16_t hdc;
    uint8_t *buf;
    uint32_t len, cap, maxrec;
    struct { uint16_t h; uint8_t def[50]; uint8_t ndef; uint8_t kind; } obj[META_OBJS];
    unsigned nobj;
} gmeta;

static bool mgrow(gmeta *m, uint32_t more) {
    if (m->len + more > m->cap) {
        uint32_t nc = m->cap ? m->cap * 2 : 1024;
        uint8_t *n;
        while (nc < m->len + more) nc *= 2;
        n = realloc(m->buf, nc);
        if (!n) return false;
        m->buf = n;
        m->cap = nc;
    }
    return true;
}

/* One record: size (words), function, the parameter bytes (padded to a word). */
static bool mrec(gmeta *m, uint16_t func, const uint8_t *prm, uint32_t nbytes) {
    uint32_t padded = (nbytes + 1u) & ~1u, words = 3 + padded / 2;
    uint8_t *d;
    if (!mgrow(m, words * 2u)) return false;
    d = m->buf + m->len;
    gst32(d, words);
    gst16(d + 4, func);
    if (nbytes) memcpy(d + 6, prm, nbytes);
    if (padded > nbytes) d[6 + nbytes] = 0;
    m->len += words * 2u;
    if (words > m->maxrec) m->maxrec = words;
    return true;
}

static gmeta *meta_for(gdi_t *g, uint16_t hdc) {
    int i;
    for (i = 0; i < g->nmetas; i++)
        if (g->metas[i]->hdc == hdc) return g->metas[i];
    return NULL;
}

static uint16_t sword(hle_call *c, unsigned i) {            /* the i-th argument word from the top of the stack */
    uint16_t v = 0;
    cpu_read16(c->cpu, c->cpu->seg[CPU_SS], (uint16_t)((uint16_t)c->cpu->r[CPU_ESP] + 4 + 2 * i), &v);
    return v;
}

/* The pointer a far argument at stack words i (offset), i+1 (selector) designates, for len bytes. */
static const uint8_t *sptr(hle_call *c, unsigned i, uint32_t len) {
    uint16_t off = sword(c, i), sel = sword(c, i + 1);
    return sel || off ? hle_ptr(c, sel, off, len ? len : 1) : NULL;
}

/* Registers the object in the metafile (emitting its create record when it is new or changed) and returns its index. */
static int meta_object(gdi_t *g, gmeta *m, uint16_t h) {
    gobj *o = gobj_get(g, h);
    uint8_t def[64];
    unsigned nd = 0, i;
    uint16_t func;
    if (!o) return -1;
    switch (o->type) {
    case GO_PEN:
        gst16(def, (uint16_t)o->u.pen.style);
        gst16(def + 2, (uint16_t)o->u.pen.width);
        gst16(def + 4, 0);
        gst32(def + 6, o->u.pen.color);
        nd = 10;
        func = META_CREATEPENINDIRECT;
        break;
    case GO_BRUSH:
        if (o->u.brush.style != BS_SOLID && o->u.brush.style != BS_NULL && o->u.brush.style != BS_HATCHED) return -2;
        gst16(def, (uint16_t)o->u.brush.style);
        gst32(def + 2, o->u.brush.color);
        gst16(def + 6, (uint16_t)o->u.brush.hatch);
        nd = 8;
        func = META_CREATEBRUSHINDIRECT;
        break;
    case GO_FONT:
        memcpy(def, o->u.font.raw, 50);
        nd = 50;
        func = META_CREATEFONTINDIRECT;
        break;
    default:
        return -2;
    }
    for (i = 0; i < m->nobj; i++)
        if (m->obj[i].h == h && m->obj[i].kind == o->type && m->obj[i].ndef == nd && !memcmp(m->obj[i].def, def, nd))
            return (int)i;
    if (m->nobj >= META_OBJS) return -2;
    if (!mrec(m, func, def, nd)) return -2;
    m->obj[m->nobj].h = h;
    m->obj[m->nobj].kind = o->type;
    m->obj[m->nobj].ndef = (uint8_t)nd;
    memcpy(m->obj[m->nobj].def, def, nd);
    return (int)m->nobj++;
}

static void unrecorded(gdi_t *g, hle_call *c) {
    if (g->cfg.log)
        fprintf(g->cfg.log, "gdi: metafile: %s not recorded (called from %04x:%04x)\n", c->entry->name, c->caller_cs, c->caller_ip);
}

/* The calls the program makes with a metafile DC as first argument, as records. */
void gdi_meta_tap(void *ctx, hle_call *c) {
    gdi_t *g = ctx;
    const hle_entry *e = c->entry;
    unsigned nw = e->arg_bytes / 2, i;
    gmeta *m;
    uint8_t prm[256];
    if (!g->nmetas || nw == 0 || strcmp(e->module, GDI_MODULE) != 0) return;
    if (!(m = meta_for(g, sword(c, nw - 1)))) return;
    for (i = 0; i < NSIMPLE; i++)
        if (simple[i].ordinal == e->ordinal) {
            unsigned np = simple[i].np, k;
            for (k = 0; k < np; k++) gst16(prm + 2 * k, sword(c, k));
            mrec(m, simple[i].func, prm, np * 2u);
            return;
        }
    for (i = 0; i < NEXREC; i++)
        if (exrec[i].ordinal == e->ordinal) {      /* the Ex forms: the same record, without the trailing POINT pointer */
            unsigned k;
            for (k = 0; k < 2; k++) gst16(prm + 2 * k, sword(c, 2 + k));
            mrec(m, exrec[i].func, prm, 4);
            return;
        }
    switch (e->ordinal) {
    case 45: {                                      /* SelectObject: the object's create record, then the select */
        int idx = meta_object(g, m, sword(c, 0));
        if (idx >= 0) {
            gst16(prm, (uint16_t)idx);
            mrec(m, META_SELECTOBJECT, prm, 2);
        } else {
            unrecorded(g, c);
        }
        return;
    }
    case 33: {                                      /* TextOut(hdc, x, y, str far, count) */
        unsigned n = sword(c, 0);
        const uint8_t *s = n ? sptr(c, 1, n) : NULL;
        uint8_t *rec;
        uint32_t padded = (n + 1u) & ~1u;
        if (n && !s) return;
        rec = malloc(2 + padded + 4);
        if (!rec) return;
        gst16(rec, (uint16_t)n);
        memset(rec + 2, 0, padded);
        if (n) memcpy(rec + 2, s, n);
        gst16(rec + 2 + padded, sword(c, 3));       /* y */
        gst16(rec + 2 + padded + 2, sword(c, 4));   /* x */
        mrec(m, META_TEXTOUT, rec, 2 + padded + 4);
        free(rec);
        return;
    }
    case 351: {                                     /* ExtTextOut(hdc, x, y, opts, rect far, str far, count, dx far) */
        unsigned n = sword(c, 2), opt = sword(c, 7);
        const uint8_t *s = n ? sptr(c, 3, n) : NULL, *rc = sptr(c, 5, 8), *dx = n ? sptr(c, 0, n * 2u) : NULL;
        uint32_t padded = (n + 1u) & ~1u, total;
        uint8_t *rec;
        bool hasrect = (opt & 6) && rc;
        if (n && !s) return;
        total = 8 + (hasrect ? 8u : 0u) + padded + (dx ? n * 2u : 0u);
        rec = calloc(1, total);
        if (!rec) return;
        gst16(rec, sword(c, 8));                    /* y */
        gst16(rec + 2, sword(c, 9));                /* x */
        gst16(rec + 4, (uint16_t)n);
        gst16(rec + 6, (uint16_t)opt);
        if (hasrect) memcpy(rec + 8, rc, 8);
        if (n) memcpy(rec + 8 + (hasrect ? 8 : 0), s, n);
        if (dx) memcpy(rec + 8 + (hasrect ? 8 : 0) + padded, dx, n * 2u);
        mrec(m, META_EXTTEXTOUT, rec, total);
        free(rec);
        return;
    }
    case 36:
    case 37: {                                      /* Polygon / Polyline(hdc, points far, count) */
        unsigned n = sword(c, 0);
        const uint8_t *pts = n ? sptr(c, 1, n * 4u) : NULL;
        uint8_t *rec;
        if (!pts) return;
        rec = malloc(2 + n * 4u);
        if (!rec) return;
        gst16(rec, (uint16_t)n);
        memcpy(rec + 2, pts, n * 4u);
        mrec(m, e->ordinal == 36 ? META_POLYGON : META_POLYLINE, rec, 2 + n * 4u);
        free(rec);
        return;
    }
    case 450: {                                     /* PolyPolygon(hdc, points far, counts far, n) */
        unsigned n = sword(c, 0), k, total = 0;
        const uint8_t *counts = n ? sptr(c, 1, n * 2u) : NULL, *pts;
        uint8_t *rec;
        if (!counts) return;
        for (k = 0; k < n; k++) total += gld16(counts + 2 * k);
        pts = sptr(c, 3, total * 4u);
        if (!pts || !(rec = malloc(2 + n * 2u + total * 4u))) return;
        gst16(rec, (uint16_t)n);
        memcpy(rec + 2, counts, n * 2u);
        memcpy(rec + 2 + n * 2u, pts, total * 4u);
        mrec(m, META_POLYPOLYGON, rec, 2 + n * 2u + total * 4u);
        free(rec);
        return;
    }
    case 439:                                       /* StretchDIBits(hdc, xd, yd, cxd, cyd, xs, ys, cxs, cys, bits, bmi, usage, rop) */
    case 443: {                                     /* SetDIBitsToDevice(hdc, x, y, cx, cy, xs, ys, start, nscans, bits, bmi, usage) */
        bool stretch = e->ordinal == 439;
        unsigned o = stretch ? 2 : 0;               /* first stack word of the far pointers */
        uint16_t usage = sword(c, o);
        const uint8_t *bmi = sptr(c, o + 1, 40), *bits;
        uint32_t at, imgbytes, head, k;
        uint8_t *rec;
        int h;
        if (!bmi) return;
        at = dib_bits_at(bmi, 40, usage);
        if (!at) return;
        if (!(bmi = sptr(c, o + 1, at))) return;
        h = (int32_t)gld32(bmi + 8);
        if (h < 0) h = -h;
        if (gld32(bmi + 16) != 0) imgbytes = gld32(bmi + 20);                   /* compressed: the stream's size */
        else imgbytes = (((uint32_t)(int32_t)gld32(bmi + 4) * gld16(bmi + 14) + 31) / 32 * 4) * (uint32_t)h;
        if (!stretch) {
            unsigned ns = sword(c, 5);
            if (gld32(bmi + 16) == 0 && h > 0) imgbytes = imgbytes / (uint32_t)h * (ns < (unsigned)h ? ns : (unsigned)h);
        }
        if (!imgbytes || !(bits = sptr(c, o + 3, imgbytes))) return;
        head = stretch ? 22u : 18u;
        rec = malloc(head + at + imgbytes);
        if (!rec) return;
        if (stretch) {
            for (k = 0; k < 11; k++) gst16(rec + 2 * k, sword(c, k < 2 ? k : k == 2 ? 2 : k + 4));
        } else {
            for (k = 0; k < 9; k++) gst16(rec + 2 * k, sword(c, k == 0 ? 0 : k + 4));
        }
        memcpy(rec + head, bmi, at);
        memcpy(rec + head + at, bits, imgbytes);
        mrec(m, stretch ? META_STRETCHDIB : META_SETDIBTODEV, rec, head + at + imgbytes);
        free(rec);
        return;
    }
    default:
        break;
    }
    unrecorded(g, c);
}

static cpu_status g_createmetafile(hle_call *c) {
    GBEGIN;
    uint16_t sel, off, hdc;
    gmeta *m;
    hle_far(&a, &sel, &off);
    GCHECK;
    if (g->nmetas >= GMETA_MAX || !(m = calloc(1, sizeof *m)) || !(hdc = gdi_new_memdc(g))) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    m->hdc = hdc;
    g->metas[g->nmetas++] = m;
    hle_ret16(c, hdc);
    return CPU_OK;
}

static cpu_status g_closemetafile(hle_call *c) {
    GBEGIN;
    uint16_t hdc = hle_u16(&a), h;
    gmeta *m = meta_for(g, hdc);
    uint8_t head[18];
    uint8_t *all;
    int i;
    GCHECK;
    if (!m) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    mrec(m, META_EOF, NULL, 0);
    gst16(head, 1);
    gst16(head + 2, 9);
    gst16(head + 4, 0x0300);
    gst32(head + 6, (m->len + 18) / 2);
    gst16(head + 10, (uint16_t)m->nobj);
    gst32(head + 12, m->maxrec);
    gst16(head + 16, 0);
    all = malloc(m->len + 18);
    h = 0;
    if (all) {
        memcpy(all, head, 18);
        memcpy(all + 18, m->buf, m->len);
        h = block_alloc(g, all, m->len + 18);
        free(all);
    }
    for (i = 0; i < g->nmetas; i++)
        if (g->metas[i] == m) {
            memmove(&g->metas[i], &g->metas[i + 1], (size_t)(g->nmetas - i - 1) * sizeof g->metas[0]);
            g->nmetas--;
            break;
        }
    free(m->buf);
    free(m);
    {
        uint16_t w[1] = {hdc};
        cpu_status st = hle_invoke(g->hle, g->p, GDI_MODULE, 68, w, 1, NULL);   /* DeleteDC */
        if (st != CPU_OK) return st;
    }
    hle_ret16(c, h);
    return CPU_OK;
}

void gdi_meta_free_all(gdi_t *g) {
    int i;
    for (i = 0; i < g->nmetas; i++) {
        free(g->metas[i]->buf);
        free(g->metas[i]);
    }
    g->nmetas = 0;
}

/* ---- handlers --------------------------------------------------------------------------------- */

static cpu_status g_setmetafilebits(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    GCHECK;
    hle_ret16(c, meta_bits(g, h, NULL) ? h : 0);
    return CPU_OK;
}

static cpu_status g_getmetafilebits(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    GCHECK;
    hle_ret16(c, meta_bits(g, h, NULL) ? h : 0);
    return CPU_OK;
}

static cpu_status g_deletemetafile(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    GCHECK;
    if (!meta_bits(g, h, NULL)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    block_free(g, h);
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status g_playmetafile(hle_call *c) {
    GBEGIN;
    uint16_t hdc = hle_u16(&a), h = hle_u16(&a);
    uint32_t len;
    const uint8_t *bits;
    gplay p;
    GCHECK;
    if (!gdc_get(g, hdc) || !(bits = meta_bits(g, h, &len))) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    memset(&p, 0, sizeof p);
    p.g = g;
    p.hdc = hdc;
    p.nht = gld16(bits + 10) + 16u;
    if (p.nht > MAX_HANDLES) p.nht = MAX_HANDLES;
    p.ht = calloc(p.nht, sizeof *p.ht);
    if (!p.ht) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    play(&p, bits, len);
    play_free(&p);
    hle_ret16(c, p.st == CPU_OK);
    return p.st;
}

/* PlayMetaFileRecord(hdc, lpHandleTable, lpMetaRecord, nHandles): one record against the caller's handle table. */
static cpu_status g_playmetafilerecord(hle_call *c) {
    GBEGIN;
    uint16_t hdc = hle_u16(&a), tsel, toff, rsel, roff, n;
    const uint8_t *rec;
    uint8_t *tab;
    uint32_t words;
    gplay p;
    unsigned i;
    hle_far(&a, &tsel, &toff);
    hle_far(&a, &rsel, &roff);
    n = hle_u16(&a);
    GCHECK;
    rec = hle_ptr(c, rsel, roff, 6);
    tab = n ? hle_ptr(c, tsel, toff, n * 2u) : NULL;
    if (!gdc_get(g, hdc) || !rec || (n && !tab)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    words = gld32(rec);
    if (words < 3 || !(rec = hle_ptr(c, rsel, roff, words * 2u))) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    memset(&p, 0, sizeof p);
    p.g = g;
    p.hdc = hdc;
    p.nht = n > MAX_HANDLES ? MAX_HANDLES : n;
    p.ht = calloc(p.nht ? p.nht : 1, sizeof *p.ht);
    if (!p.ht) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    for (i = 0; i < p.nht; i++) p.ht[i] = gld16(tab + 2 * i);
    play_record(&p, rec, words);
    if ((tab = n ? hle_ptr(c, tsel, toff, n * 2u) : NULL))
        for (i = 0; i < p.nht; i++) gst16(tab + 2 * i, p.ht[i]);
    if (p.tmp) mem_free(&g->p->mem, p.tmp);
    free(p.ht);
    hle_ret16(c, p.st == CPU_OK);
    return p.st;
}

/* EnumMetaFile(hdc, hmf, lpEnumFunc, lpData): int CALLBACK f(hdc, lpHandleTable, lpMetaRecord, nObjects, lpData). */
static cpu_status g_enummetafile(hle_call *c) {
    GBEGIN;
    uint16_t hdc = hle_u16(&a), h = hle_u16(&a);
    uint32_t proc = hle_u32(&a), data = hle_u32(&a), len, off, ret = 1;
    const uint8_t *bits;
    uint16_t tbl, nobj;
    GCHECK;
    if (!gdc_get(g, hdc) || !(bits = meta_bits(g, h, &len))) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    nobj = gld16(bits + 10);
    tbl = mem_alloc(&g->p->mem, (uint32_t)(nobj ? nobj : 1) * 2u, MEM_DATA);
    if (!tbl) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    off = gld16(bits + 2) * 2u;
    while (off + 6 <= len && ret & 0xFFFF) {
        uint32_t words = gld32(bits + off);
        uint16_t args[8];
        if (words < 3 || off + words * 2u > len) break;
        args[0] = hdc;
        args[1] = tbl;
        args[2] = 0;
        args[3] = h;
        args[4] = (uint16_t)off;
        args[5] = nobj;
        args[6] = (uint16_t)(data >> 16);
        args[7] = (uint16_t)data;
        if (!user_call_guest(g->user, proc, args, 8, &ret)) {
            mem_free(&g->p->mem, tbl);
            return user_abort(g->user);
        }
        off += words * 2u;
        if (!(bits = meta_bits(g, h, &len))) break;              /* the callback may have deleted it */
    }
    mem_free(&g->p->mem, tbl);
    hle_ret16(c, 1);
    return CPU_OK;
}

#define G GDI_MODULE
const hle_def gdi_meta_defs[] = {
    {G, 123, "PlayMetaFile", 4, g_playmetafile},
    {G, 125, "CreateMetaFile", 4, g_createmetafile},
    {G, 126, "CloseMetaFile", 2, g_closemetafile},
    {G, 127, "DeleteMetaFile", 2, g_deletemetafile},
    {G, 159, "GetMetaFileBits", 2, g_getmetafilebits},
    {G, 160, "SetMetaFileBits", 2, g_setmetafilebits},
    {G, 175, "EnumMetaFile", 12, g_enummetafile},
    {G, 176, "PlayMetaFileRecord", 12, g_playmetafilerecord},
};
#undef G
const size_t gdi_meta_ndefs = sizeof gdi_meta_defs / sizeof gdi_meta_defs[0];
