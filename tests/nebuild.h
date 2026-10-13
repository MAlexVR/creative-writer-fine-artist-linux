#ifndef NEBUILD_H
#define NEBUILD_H
/* Builds a complete NE file (programs and DLLs) in memory for the module tests.
 * Segments, relocation records, exports and imports are given as data; the
 * image goes through the real parser (ne_open_buffer), so the loader sees what a
 * file on disk would give it. Entries are fixed and one per ordinal. */
#include <stdint.h>
#include <string.h>

#include "ne.h"

#define NB_MAX 0x10000
#define NB_SEGS 6
#define NB_REF_NAMES 8

typedef struct { uint8_t src, flags; uint16_t off, a, b; } nb_rel; /* flags: target type | 4 when additive */
typedef struct {
    const uint8_t *data;
    unsigned len;
    uint16_t flags;
    unsigned alloc;
    const nb_rel *rel;
    unsigned nrel;
} nb_seg;
typedef struct { uint8_t seg; uint16_t off; } nb_ent; /* ordinal = index + 1, seg 0 = unused */
typedef struct { const char *name; uint16_t ord; } nb_name;

#define NB_F_INT 0
#define NB_F_ORD 1
#define NB_F_NAME 2
#define NB_F_ADD 4

typedef struct { uint16_t id; const uint8_t *data; unsigned len; } nb_res; /* len <= 0x400 */

typedef struct {
    const char *module;
    uint16_t flags, autods, heap, stack, cs, ip, ss, sp;
    nb_seg segs[NB_SEGS];
    unsigned nsegs;
    const char *modrefs[NB_REF_NAMES]; /* 1-based in relocations */
    unsigned nmodrefs;
    const char *impnames[NB_REF_NAMES]; /* by-name imports; offsets from nb_impname_off */
    unsigned nimpnames;
    nb_ent ents[16];
    unsigned nents;
    nb_name names[8], nonres[8];
    unsigned nnames, nnonres;
    uint16_t restype;      /* integer resource type of res[] */
    nb_res res[4];
    unsigned nres;
} nb_t;

static inline void nb_p8(uint8_t *b, size_t at, unsigned v) { b[at] = (uint8_t)v; }
static inline void nb_p16(uint8_t *b, size_t at, unsigned v) { nb_p8(b, at, v); nb_p8(b, at + 1, v >> 8); }

static inline size_t nb_name_put(uint8_t *b, size_t at, const char *s, unsigned ord) {
    size_t l = strlen(s);
    nb_p8(b, at, (unsigned)l);
    memcpy(&b[at + 1], s, l);
    nb_p16(b, at + 1 + l, ord);
    return at + 1 + l + 2;
}

/* Offset of by-name import i inside the imported-names table (byte 0 is a pad). */
static inline unsigned nb_impname_off(const nb_t *t, unsigned i) {
    unsigned off = 1, k;
    for (k = 0; k < t->nmodrefs; k++) off += 1 + (unsigned)strlen(t->modrefs[k]);
    for (k = 0; k < i; k++) off += 1 + (unsigned)strlen(t->impnames[k]);
    return off;
}

/* Returns the file length. */
static inline size_t nb_build(uint8_t *b, const nb_t *t) {
    const size_t h = 0x40;
    size_t p, pos = 0x1000;
    unsigned i, modref_off, impname_off, entry_off, nonres_off = 0xF00;
    memset(b, 0, NB_MAX);
    memcpy(b, "MZ", 2);
    nb_p16(b, 0x3c, (unsigned)h);
    memcpy(&b[h], "NE", 2);
    nb_p8(b, h + 2, 5); nb_p8(b, h + 3, 10);
    nb_p16(b, h + 0x0c, t->flags);
    nb_p16(b, h + 0x0e, t->autods);
    nb_p16(b, h + 0x10, t->heap); nb_p16(b, h + 0x12, t->stack);
    nb_p16(b, h + 0x14, t->ip); nb_p16(b, h + 0x16, t->cs);
    nb_p16(b, h + 0x18, t->sp); nb_p16(b, h + 0x1a, t->ss);
    nb_p16(b, h + 0x1c, t->nsegs);
    nb_p16(b, h + 0x1e, t->nmodrefs);
    nb_p16(b, h + 0x22, 0x40);
    nb_p16(b, h + 0x24, 0x40 + 8 * t->nsegs);
    nb_p16(b, h + 0x32, 4);
    nb_p8(b, h + 0x36, 2);
    p = h + 0x40 + 8 * t->nsegs;
    if (t->nres) { /* one resource type; data at file offset 0x4000 + 0x400 * i */
        nb_p16(b, p, 4);
        nb_p16(b, p + 2, 0x8000u | t->restype);
        nb_p16(b, p + 4, t->nres);
        p += 10;
        for (i = 0; i < t->nres; i++, p += 12) {
            nb_p16(b, p, (0x4000 + 0x400 * i) >> 4);
            nb_p16(b, p + 2, (t->res[i].len + 15) >> 4);
            nb_p16(b, p + 4, 0x30);
            nb_p16(b, p + 6, 0x8000u | t->res[i].id);
            memcpy(&b[0x4000 + 0x400 * i], t->res[i].data, t->res[i].len);
        }
        p += 2;
    }
    nb_p16(b, h + 0x26, (unsigned)(p - h));
    p = nb_name_put(b, p, t->module, 0);
    for (i = 0; i < t->nnames; i++) p = nb_name_put(b, p, t->names[i].name, t->names[i].ord);
    nb_p8(b, p++, 0);
    modref_off = (unsigned)(p - h);
    {
        unsigned off = 1;
        for (i = 0; i < t->nmodrefs; i++) {
            nb_p16(b, p, off);
            p += 2;
            off += 1 + (unsigned)strlen(t->modrefs[i]);
        }
    }
    impname_off = (unsigned)(p - h);
    nb_p8(b, p++, 0);
    for (i = 0; i < t->nmodrefs; i++) p = nb_name_put(b, p, t->modrefs[i], 0) - 2;
    for (i = 0; i < t->nimpnames; i++) p = nb_name_put(b, p, t->impnames[i], 0) - 2;
    nb_p8(b, p++, 0);
    entry_off = (unsigned)(p - h);
    for (i = 0; i < t->nents; i++) {
        if (t->ents[i].seg == 0) {
            nb_p8(b, p, 1); nb_p8(b, p + 1, 0);
            p += 2;
        } else {
            nb_p8(b, p, 1); nb_p8(b, p + 1, t->ents[i].seg); nb_p8(b, p + 2, 1);
            nb_p16(b, p + 3, t->ents[i].off);
            p += 5;
        }
    }
    nb_p8(b, p++, 0);
    nb_p16(b, h + 0x04, entry_off); nb_p16(b, h + 0x06, (unsigned)(p - h - entry_off));
    nb_p16(b, h + 0x28, modref_off); nb_p16(b, h + 0x2a, impname_off);
    p = nonres_off;
    p = nb_name_put(b, p, "Description", 0);
    for (i = 0; i < t->nnonres; i++) p = nb_name_put(b, p, t->nonres[i].name, t->nonres[i].ord);
    nb_p8(b, p++, 0);
    nb_p16(b, h + 0x2c, nonres_off);
    nb_p16(b, h + 0x20, (unsigned)(p - nonres_off));
    for (i = 0; i < t->nsegs; i++) {
        const nb_seg *s = &t->segs[i];
        size_t e = h + 0x40 + 8 * i;
        uint16_t flags = (uint16_t)(s->flags | (s->nrel ? NE_SEG_RELOCINFO : 0));
        pos = (pos + 15) & ~(size_t)15;
        if (s->data || s->len) {
            nb_p16(b, e, (unsigned)(pos >> 4)); nb_p16(b, e + 2, s->len);
            if (s->data) memcpy(&b[pos], s->data, s->len);
        }
        nb_p16(b, e + 4, flags); nb_p16(b, e + 6, s->alloc);
        pos += s->len;
        if (s->nrel) {
            unsigned k;
            nb_p16(b, pos, s->nrel); pos += 2;
            for (k = 0; k < s->nrel; k++) {
                nb_p8(b, pos, s->rel[k].src); nb_p8(b, pos + 1, s->rel[k].flags);
                nb_p16(b, pos + 2, s->rel[k].off); nb_p16(b, pos + 4, s->rel[k].a);
                nb_p16(b, pos + 6, s->rel[k].b);
                pos += 8;
            }
        }
    }
    if (t->nres && pos < 0x4000 + 0x400 * t->nres) pos = 0x4000 + 0x400 * t->nres;
    return pos;
}

#endif
