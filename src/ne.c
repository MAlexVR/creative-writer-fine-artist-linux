#include "ne.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MZ_MIN 0x40u
#define NE_HDR_SIZE 0x40u

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }

/* [off, off+len) lies inside a file of `size` bytes; 64-bit math cannot wrap */
static int in_file(const ne_image *ne, uint64_t off, uint64_t len) {
    return off + len <= ne->size;
}

static ne_err parse_header(ne_image *ne) {
    const uint8_t *d = ne->data, *h;
    uint64_t ne_off;

    if (ne->size < MZ_MIN) return NE_ERR_TRUNCATED;
    if (d[0] != 'M' || d[1] != 'Z') return NE_ERR_BAD_MZ;
    ne_off = rd32(d + 0x3c);
    if (ne_off < MZ_MIN) return NE_ERR_BAD_OFFSET;
    if (ne_off + 2 > ne->size) return NE_ERR_BAD_OFFSET;
    h = d + ne_off;
    if (h[0] != 'N' || h[1] != 'E') return NE_ERR_BAD_NE;
    if (ne_off + NE_HDR_SIZE > ne->size) return NE_ERR_TRUNCATED;

    ne->ne_off = (uint32_t)ne_off;
    ne->linker_major = h[2];
    ne->linker_minor = h[3];
    ne->entry_off = (uint32_t)(ne_off + rd16(h + 0x04));
    ne->entry_len = (uint16_t)rd16(h + 0x06);
    ne->flags = (uint16_t)rd16(h + 0x0c);
    ne->auto_data_seg = (uint16_t)rd16(h + 0x0e);
    ne->heap_size = (uint16_t)rd16(h + 0x10);
    ne->stack_size = (uint16_t)rd16(h + 0x12);
    ne->ip = (uint16_t)rd16(h + 0x14);
    ne->cs = (uint16_t)rd16(h + 0x16);
    ne->sp = (uint16_t)rd16(h + 0x18);
    ne->ss = (uint16_t)rd16(h + 0x1a);
    ne->seg_count = (uint16_t)rd16(h + 0x1c);
    ne->modref_count = (uint16_t)rd16(h + 0x1e);
    ne->nonres_len = (uint16_t)rd16(h + 0x20);
    ne->segtab_off = (uint32_t)(ne_off + rd16(h + 0x22));
    ne->resource_off = (uint32_t)(ne_off + rd16(h + 0x24));
    ne->resname_off = (uint32_t)(ne_off + rd16(h + 0x26));
    ne->modref_off = (uint32_t)(ne_off + rd16(h + 0x28));
    ne->impname_off = (uint32_t)(ne_off + rd16(h + 0x2a));
    ne->nonres_off = rd32(h + 0x2c);
    ne->movable_entries = (uint16_t)rd16(h + 0x30);
    ne->win_version = (uint16_t)rd16(h + 0x3e);
    ne->align_shift = (uint8_t)rd16(h + 0x32);
    if (ne->align_shift == 0) ne->align_shift = 9;
    if (ne->align_shift > 15) return NE_ERR_BAD_TABLE;
    ne->resource_segs = (uint16_t)rd16(h + 0x34);
    ne->target_os = h[0x36];

    if (!in_file(ne, ne->segtab_off, (uint64_t)ne->seg_count * 8) ||
        !in_file(ne, ne->modref_off, (uint64_t)ne->modref_count * 2) ||
        !in_file(ne, ne->entry_off, ne->entry_len) ||
        !in_file(ne, ne->nonres_off, ne->nonres_len) ||
        !in_file(ne, ne->resname_off, 1) ||
        !in_file(ne, ne->impname_off, 0) ||
        !in_file(ne, ne->resource_off, 0))
        return NE_ERR_BAD_OFFSET;
    return NE_OK;
}

static ne_err parse_relocs(const ne_image *ne, ne_segment *s) {
    const uint8_t *t;
    uint64_t tab = (uint64_t)s->file_offset + s->file_length;
    unsigned count, i;
    if (!(s->flags & NE_SEG_RELOCINFO) || s->file_offset == 0) return NE_OK;
    if (!in_file(ne, tab, 2)) return NE_ERR_BAD_OFFSET;
    count = rd16(ne->data + tab);
    if (count == 0) return NE_OK;
    if (!in_file(ne, tab + 2, (uint64_t)count * 8)) return NE_ERR_BAD_OFFSET;
    s->relocs = calloc(count, sizeof *s->relocs);
    if (!s->relocs) return NE_ERR_NOMEM;
    t = ne->data + tab + 2;
    for (i = 0; i < count; i++, t += 8) {
        ne_reloc *r = &s->relocs[i];
        char scratch[NE_NAME_MAX + 1];
        r->src_type = t[0] & 0x0f;
        r->target_type = t[1] & 3;
        r->additive = (t[1] & 4) != 0;
        r->src_offset = (uint16_t)rd16(t + 2);
        r->a = (uint16_t)rd16(t + 4);
        r->b = (uint16_t)rd16(t + 6);
        if (r->src_type != NE_SRC_LOBYTE && r->src_type != NE_SRC_SELECTOR &&
            r->src_type != NE_SRC_FARPTR && r->src_type != NE_SRC_OFFSET)
            return NE_ERR_BAD_TABLE;
        switch (r->target_type) {
        case NE_TGT_INTERNAL:
            r->a = t[4];
            r->b = (uint16_t)rd16(t + 6);
            if (r->a != NE_SEG_MOVEABLE_REF && (r->a == 0 || r->a > ne->seg_count))
                return NE_ERR_BAD_TABLE;
            break;
        case NE_TGT_IMPORT_NAME: {
            ne_err err;
            if (r->a == 0 || r->a > ne->modref_count) return NE_ERR_BAD_TABLE;
            err = ne_imported_name(ne, r->b, scratch);
            if (err != NE_OK) return err;
            break;
        }
        case NE_TGT_IMPORT_ORDINAL:
            if (r->a == 0 || r->a > ne->modref_count) return NE_ERR_BAD_TABLE;
            break;
        default:
            break;
        }
        s->reloc_count++;
    }
    return NE_OK;
}

static ne_err parse_segments(ne_image *ne) {
    uint16_t i;
    ne->segs = calloc(ne->seg_count ? ne->seg_count : 1, sizeof *ne->segs);
    if (!ne->segs) return NE_ERR_NOMEM;
    for (i = 0; i < ne->seg_count; i++) {
        const uint8_t *e = ne->data + ne->segtab_off + (size_t)i * 8;
        ne_segment *s = &ne->segs[i];
        unsigned sector = rd16(e), len = rd16(e + 2), minalloc = rd16(e + 6);
        uint64_t off = (uint64_t)sector << ne->align_shift;
        s->flags = (uint16_t)rd16(e + 4);
        s->alloc_size = minalloc ? minalloc : 0x10000;
        if (sector == 0) continue;
        s->file_length = len ? len : 0x10000;
        if (!in_file(ne, off, s->file_length)) return NE_ERR_BAD_OFFSET;
        s->file_offset = (uint32_t)off;
        {
            ne_err err = parse_relocs(ne, s);
            if (err != NE_OK) return err;
        }
    }
    return NE_OK;
}

/* Length-prefixed name table; `limit` bytes available from `off`. When
 * with_ordinal is set each entry is followed by a 16-bit ordinal. */
static ne_err parse_names(const ne_image *ne, uint32_t off, uint64_t limit,
                          ne_name **out, size_t *count) {
    uint64_t p = 0;
    size_t n = 0, cap = 0;
    ne_name *v = NULL;
    *out = NULL;
    *count = 0;
    while (p < limit) {
        unsigned len = ne->data[off + p];
        ne_name *nm;
        if (len == 0) break;
        if (p + 1 + len + 2 > limit) {
            free(v);
            return NE_ERR_BAD_TABLE;
        }
        if (n == cap) {
            ne_name *nv;
            cap = cap ? cap * 2 : 16;
            nv = realloc(v, cap * sizeof *v);
            if (!nv) {
                free(v);
                return NE_ERR_NOMEM;
            }
            v = nv;
        }
        nm = &v[n++];
        nm->len = (uint8_t)len;
        memcpy(nm->text, ne->data + off + p + 1, len);
        nm->text[len] = '\0';
        nm->ordinal = (uint16_t)rd16(ne->data + off + p + 1 + len);
        p += 1 + len + 2;
    }
    *out = v;
    *count = n;
    return NE_OK;
}

static ne_err parse_entries(ne_image *ne) {
    const uint8_t *t = ne->data + ne->entry_off;
    size_t p = 0, len = ne->entry_len, cap = 0;
    while (p < len) {
        unsigned count = t[p], seg, stride, i;
        if (count == 0) break;
        if (p + 2 > len) return NE_ERR_BAD_TABLE;
        seg = t[p + 1];
        stride = seg == 0 ? 0 : seg == 0xff ? 6 : 3;
        p += 2;
        if (p + (size_t)count * stride > len) return NE_ERR_BAD_TABLE;
        if (ne->entry_count + count > cap) {
            ne_entry *nv;
            cap = (ne->entry_count + count) * 2;
            nv = realloc(ne->entries, cap * sizeof *nv);
            if (!nv) return NE_ERR_NOMEM;
            ne->entries = nv;
        }
        for (i = 0; i < count; i++, p += stride) {
            ne_entry *e = &ne->entries[ne->entry_count++];
            memset(e, 0, sizeof *e);
            if (seg == 0) continue;
            e->flags = t[p];
            if (seg == 0xff) {
                e->kind = NE_ENTRY_MOVEABLE;
                e->segment = t[p + 3];
                e->offset = (uint16_t)rd16(t + p + 4);
            } else {
                e->kind = NE_ENTRY_FIXED;
                e->segment = (uint8_t)seg;
                e->offset = (uint16_t)rd16(t + p + 1);
            }
        }
    }
    return NE_OK;
}

/* Id word: high bit set = integer id, else offset of a length-prefixed string
 * measured from the start of the resource table. */
static ne_err read_res_id(const ne_image *ne, uint64_t tab_len, unsigned w, uint16_t *id,
                          char name[NE_NAME_MAX + 1]) {
    const uint8_t *s;
    unsigned len;
    name[0] = 0;
    if (w & 0x8000) {
        *id = (uint16_t)(w & 0x7fff);
        return NE_OK;
    }
    *id = 0;
    if (w >= tab_len) return NE_ERR_BAD_TABLE;
    s = ne->data + ne->resource_off + w;
    len = s[0];
    if (len == 0 || w + 1u + len > tab_len) return NE_ERR_BAD_TABLE;
    memcpy(name, s + 1, len);
    name[len] = 0;
    return NE_OK;
}

static ne_err parse_resources(ne_image *ne) {
    const uint8_t *d = ne->data;
    uint64_t tab_len, p, end;
    unsigned shift;
    size_t cap = 0;
    if (ne->resource_off >= ne->resname_off) return NE_OK;
    tab_len = ne->resname_off - ne->resource_off;
    p = ne->resource_off;
    end = ne->resname_off;
    if (tab_len < 2) return NE_ERR_BAD_TABLE;
    shift = rd16(d + p);
    if (shift > 15) return NE_ERR_BAD_TABLE;
    p += 2;
    for (;;) {
        ne_restype *t;
        unsigned id, count, i;
        ne_err err;
        if (p + 2 > end) return NE_ERR_BAD_TABLE;
        id = rd16(d + p);
        if (id == 0) break;
        if (p + 8 > end) return NE_ERR_BAD_TABLE;
        count = rd16(d + p + 2);
        if (p + 8 + (uint64_t)count * 12 > end) return NE_ERR_BAD_TABLE;
        if (ne->restype_count == cap) {
            ne_restype *v = realloc(ne->restypes, (cap ? cap * 2 : 16) * sizeof *v);
            if (!v) return NE_ERR_NOMEM;
            ne->restypes = v;
            cap = cap ? cap * 2 : 16;
        }
        t = &ne->restypes[ne->restype_count];
        memset(t, 0, sizeof *t);
        ne->restype_count++; /* owned by ne_free from here on */
        if ((err = read_res_id(ne, tab_len, id, &t->id, t->name)) != NE_OK) return err;
        t->res = calloc(count ? count : 1, sizeof *t->res);
        if (!t->res) return NE_ERR_NOMEM;
        p += 8;
        for (i = 0; i < count; i++, p += 12) {
            ne_resource *r = &t->res[i];
            uint64_t off = (uint64_t)rd16(d + p) << shift;
            uint64_t len = (uint64_t)rd16(d + p + 2) << shift;
            if ((err = read_res_id(ne, tab_len, rd16(d + p + 6), &r->id, r->name)) != NE_OK)
                return err;
            if (off >= ne->size && len) return NE_ERR_BAD_OFFSET;
            if (off + len > ne->size) len = ne->size - off; /* padding past EOF */
            r->offset = (uint32_t)off;
            r->length = (uint32_t)len;
            r->flags = (uint16_t)rd16(d + p + 4);
            t->count++;
        }
    }
    return NE_OK;
}

static ne_err parse_tables(ne_image *ne) {
    ne_err err = parse_segments(ne);
    if (err == NE_OK) err = parse_resources(ne);
    if (err == NE_OK)
        err = parse_names(ne, ne->resname_off, ne->size - ne->resname_off,
                          &ne->resident_names, &ne->resident_count);
    if (err == NE_OK)
        err = parse_names(ne, ne->nonres_off, ne->nonres_len,
                          &ne->nonres_names, &ne->nonres_count);
    if (err == NE_OK) err = parse_entries(ne);
    return err;
}

/* length-prefixed string at table-relative `off`, table spans [base, base+size) */
static ne_err read_lpstr(const ne_image *ne, uint32_t base, uint64_t size, uint64_t off,
                         char out[NE_NAME_MAX + 1]) {
    unsigned len;
    if (off >= size) return NE_ERR_BAD_OFFSET;
    len = ne->data[base + off];
    if (off + 1 + len > size) return NE_ERR_BAD_TABLE;
    memcpy(out, ne->data + base + off + 1, len);
    out[len] = '\0';
    return NE_OK;
}

static uint64_t impname_size(const ne_image *ne) {
    uint64_t end = ne->entry_off >= ne->impname_off ? ne->entry_off : ne->size;
    return end - ne->impname_off;
}

ne_err ne_imported_name(const ne_image *ne, unsigned off, char out[NE_NAME_MAX + 1]) {
    if (!ne || !ne->data || !out) return NE_ERR_ARG;
    return read_lpstr(ne, ne->impname_off, impname_size(ne), off, out);
}

ne_err ne_modref_name(const ne_image *ne, unsigned idx, char out[NE_NAME_MAX + 1]) {
    if (!ne || !ne->data || !out) return NE_ERR_ARG;
    if (idx == 0 || idx > ne->modref_count) return NE_ERR_NOT_FOUND;
    return ne_imported_name(ne, rd16(ne->data + ne->modref_off + (size_t)(idx - 1) * 2), out);
}

ne_err ne_entry_for_ordinal(const ne_image *ne, unsigned ordinal, const ne_entry **entry) {
    if (!ne || !entry) return NE_ERR_ARG;
    if (ordinal == 0 || ordinal > ne->entry_count) return NE_ERR_NOT_FOUND;
    if (ne->entries[ordinal - 1].kind == NE_ENTRY_UNUSED) return NE_ERR_NOT_FOUND;
    *entry = &ne->entries[ordinal - 1];
    return NE_OK;
}

static const ne_name *find_name(const ne_name *v, size_t n, const char *name) {
    size_t i;
    for (i = 0; i < n; i++)
        if (v[i].ordinal != 0 && strcmp(v[i].text, name) == 0) return &v[i];
    return NULL;
}

ne_err ne_find_export(const ne_image *ne, const char *name, uint16_t *ordinal) {
    const ne_name *n;
    if (!ne || !name || !ordinal) return NE_ERR_ARG;
    n = find_name(ne->resident_names, ne->resident_count, name);
    if (!n) n = find_name(ne->nonres_names, ne->nonres_count, name);
    if (!n) return NE_ERR_NOT_FOUND;
    *ordinal = n->ordinal;
    return NE_OK;
}

ne_err ne_reloc_sites(const ne_image *ne, unsigned seg, const ne_reloc *r,
                      uint16_t *out, size_t cap, size_t *n) {
    const ne_segment *s;
    const uint8_t *d;
    unsigned off;
    size_t count = 0;
    if (!ne || !ne->data || !r || !n || seg >= ne->seg_count) return NE_ERR_ARG;
    s = &ne->segs[seg];
    if (r->additive) {
        if (out && cap > 0) out[0] = r->src_offset;
        *n = 1;
        return NE_OK;
    }
    if (s->file_offset == 0) return NE_ERR_BAD_OFFSET;
    d = ne->data + s->file_offset;
    off = r->src_offset;
    for (;;) {
        unsigned next;
        if ((uint64_t)off + 2 > s->file_length) return NE_ERR_BAD_OFFSET;
        if (count >= s->file_length) return NE_ERR_BAD_CHAIN; /* revisits an offset */
        if (out && count < cap) out[count] = (uint16_t)off;
        count++;
        next = rd16(d + off);
        if (next == 0xffff) break;
        off = next;
    }
    *n = count;
    return NE_OK;
}

static int import_cmp(const void *pa, const void *pb) {
    const ne_import *a = pa, *b = pb;
    if (a->modref != b->modref) return a->modref < b->modref ? -1 : 1;
    if (a->by_name != b->by_name) return a->by_name ? 1 : -1;
    if (a->by_name) return strcmp(a->name, b->name);
    return a->ordinal == b->ordinal ? 0 : a->ordinal < b->ordinal ? -1 : 1;
}

ne_err ne_collect_imports(const ne_image *ne, ne_import **out, size_t *n) {
    size_t total = 0, k = 0, w = 0, i;
    unsigned s, j;
    ne_import *v;
    if (!ne || !ne->data || !out || !n) return NE_ERR_ARG;
    for (s = 0; s < ne->seg_count; s++) total += ne->segs[s].reloc_count;
    v = calloc(total ? total : 1, sizeof *v);
    if (!v) return NE_ERR_NOMEM;
    for (s = 0; s < ne->seg_count; s++) {
        for (j = 0; j < ne->segs[s].reloc_count; j++) {
            const ne_reloc *r = &ne->segs[s].relocs[j];
            ne_import *im;
            if (r->target_type != NE_TGT_IMPORT_ORDINAL && r->target_type != NE_TGT_IMPORT_NAME)
                continue;
            im = &v[k++];
            im->modref = r->a;
            im->refs = 1;
            if (r->target_type == NE_TGT_IMPORT_ORDINAL) {
                im->ordinal = r->b;
            } else {
                im->by_name = 1;
                if (ne_imported_name(ne, r->b, im->name) != NE_OK) {
                    free(v);
                    return NE_ERR_BAD_TABLE;
                }
            }
        }
    }
    qsort(v, k, sizeof *v, import_cmp);
    for (i = 0; i < k; i++) {
        if (w > 0 && import_cmp(&v[w - 1], &v[i]) == 0) {
            v[w - 1].refs++;
        } else {
            if (w != i) v[w] = v[i];
            w++;
        }
    }
    *out = v;
    *n = w;
    return NE_OK;
}

void ne_free_imports(ne_import *imports) { free(imports); }

ne_err ne_open_buffer(ne_image *ne, const void *buf, size_t len) {
    ne_err err;
    if (!ne || !buf) return NE_ERR_ARG;
    memset(ne, 0, sizeof *ne);
    ne->data = malloc(len ? len : 1);
    if (!ne->data) return NE_ERR_NOMEM;
    memcpy(ne->data, buf, len);
    ne->size = len;
    err = parse_header(ne);
    if (err == NE_OK) err = parse_tables(ne);
    if (err != NE_OK) ne_free(ne);
    return err;
}

ne_err ne_open_file(ne_image *ne, const char *path) {
    FILE *fp;
    long sz;
    void *buf;
    ne_err err;
    if (!ne || !path) return NE_ERR_ARG;
    fp = fopen(path, "rb");
    if (!fp) return NE_ERR_IO;
    if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NE_ERR_IO;
    }
    buf = malloc(sz ? (size_t)sz : 1);
    if (!buf) {
        fclose(fp);
        return NE_ERR_NOMEM;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NE_ERR_IO;
    }
    fclose(fp);
    err = ne_open_buffer(ne, buf, (size_t)sz);
    free(buf);
    return err;
}

void ne_free(ne_image *ne) {
    if (!ne) return;
    if (ne->segs) {
        uint16_t i;
        for (i = 0; i < ne->seg_count; i++) free(ne->segs[i].relocs);
    }
    free(ne->segs);
    if (ne->restypes) {
        size_t i;
        for (i = 0; i < ne->restype_count; i++) free(ne->restypes[i].res);
    }
    free(ne->restypes);
    free(ne->resident_names);
    free(ne->nonres_names);
    free(ne->entries);
    free(ne->data);
    memset(ne, 0, sizeof *ne);
}

const char *ne_strerror(ne_err err) {
    switch (err) {
    case NE_OK: return "ok";
    case NE_ERR_ARG: return "invalid argument";
    case NE_ERR_IO: return "I/O error";
    case NE_ERR_NOMEM: return "out of memory";
    case NE_ERR_TRUNCATED: return "file truncated";
    case NE_ERR_BAD_MZ: return "not an MZ executable";
    case NE_ERR_BAD_NE: return "not a New Executable (NE signature missing)";
    case NE_ERR_BAD_OFFSET: return "offset or size outside the file";
    case NE_ERR_BAD_TABLE: return "malformed table";
    case NE_ERR_NOT_FOUND: return "not found";
    case NE_ERR_BAD_CHAIN: return "malformed relocation chain";
    }
    return "unknown error";
}

static bool res_match(ne_resid want, uint16_t id, const char *name) {
    const char *a, *b;
    if (!want.name) return !name[0] && want.id == id;
    for (a = want.name, b = name; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    return !*a && !*b;
}

ne_err ne_find_resource(const ne_image *ne, ne_resid type, ne_resid name, const ne_resource **out) {
    size_t i;
    unsigned j;
    if (!ne || !out) return NE_ERR_ARG;
    for (i = 0; i < ne->restype_count; i++) {
        const ne_restype *t = &ne->restypes[i];
        if (!res_match(type, t->id, t->name)) continue;
        for (j = 0; j < t->count; j++)
            if (res_match(name, t->res[j].id, t->res[j].name)) {
                *out = &t->res[j];
                return NE_OK;
            }
    }
    return NE_ERR_NOT_FOUND;
}
