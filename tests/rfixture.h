#ifndef RFIXTURE_H
#define RFIXTURE_H
/* Resources for the hand-built NE image of fixture.h: the payload is appended to the
 * fake file image and the resource table entry points into it. */
#include "fixture.h"

static inline uint32_t rf_append(fx_t *f, const void *p, size_t n) {
    uint32_t off = (uint32_t)f->ne.size;
    f->file = realloc(f->file, f->ne.size + n + 1);
    memcpy(f->file + off, p, n);
    f->ne.data = f->file;
    f->ne.size = off + n;
    return off;
}

/* type by id (tname NULL) and resource by id (name NULL) or name */
static inline void rf_add(fx_t *f, unsigned type, const char *tname, unsigned id, const char *name, const void *p,
                          size_t n) {
    uint32_t off = rf_append(f, p, n);
    fx_add_resource(f, type, tname, id, name, off, (uint32_t)n);
}

/* little endian emitters into a byte buffer */
typedef struct { uint8_t b[4096]; size_t n; } rb_t;
static inline void rb_u8(rb_t *r, unsigned v) { r->b[r->n++] = (uint8_t)v; }
static inline void rb_u16(rb_t *r, unsigned v) { rb_u8(r, v); rb_u8(r, v >> 8); }
static inline void rb_u32(rb_t *r, uint32_t v) { rb_u16(r, v & 0xFFFF); rb_u16(r, v >> 16); }
static inline void rb_str(rb_t *r, const char *s) { while (*s) rb_u8(r, (unsigned char)*s++); rb_u8(r, 0); }

#endif
