#include "fnt.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ne.h"

static unsigned rd16(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }

/* Parses one FNT; takes ownership of blob on success. */
static bool strike_init(fnt_strike *s, uint8_t *blob, size_t len) {
    unsigned ver, tab, nchars, i, face;
    memset(s, 0, sizeof *s);
    if (len < 118) return false;
    ver = rd16(blob);
    if (ver != 0x200 && ver != 0x300) return false;
    s->points = (int)rd16(blob + 68);
    s->ascent = (int)rd16(blob + 74);
    s->intlead = (int)rd16(blob + 76);
    s->extlead = (int)rd16(blob + 78);
    s->italic = blob[80] != 0;
    s->underline = blob[81] != 0;
    s->strikeout = blob[82] != 0;
    s->weight = (int)rd16(blob + 83);
    s->height = (int)rd16(blob + 88);
    s->pitch_family = blob[90];
    s->avgw = (int)rd16(blob + 91);
    s->maxw = (int)rd16(blob + 93);
    s->first = blob[95];
    s->last = blob[96];
    s->dflt = blob[97];
    s->brk = blob[98];
    face = rd32(blob + 105);
    if (s->height <= 0 || s->last < s->first) return false;
    if (face && face < len) {
        size_t k = 0;
        while (face + k < len && blob[face + k] && k < sizeof s->face - 1) {
            s->face[k] = (char)blob[face + k];
            k++;
        }
        s->face[k] = 0;
    }
    tab = ver == 0x300 ? 148 : 118;
    nchars = (unsigned)(s->last - s->first) + 2;       /* the range plus the default glyph */
    {
        unsigned esz = ver == 0x300 ? 6 : 4;
        if ((size_t)tab + (size_t)nchars * esz > len) return false;
        for (i = 0; i < nchars; i++) {
            const uint8_t *e = blob + tab + i * esz;
            s->width[i] = (uint16_t)rd16(e);
            s->offset[i] = ver == 0x300 ? rd32(e + 2) : rd16(e + 2);
            if (s->width[i] && (size_t)s->offset[i] + (size_t)((s->width[i] + 7) / 8) * (size_t)s->height > len) return false;
        }
    }
    s->blob = blob;
    s->blob_len = len;
    return true;
}

static bool add_strike(fnt_font *f, const uint8_t *data, size_t len) {
    fnt_strike *n;
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) return false;
    memcpy(copy, data, len);
    n = realloc(f->s, (f->n + 1) * sizeof *n);
    if (!n) {
        free(copy);
        return false;
    }
    f->s = n;
    if (!strike_init(&f->s[f->n], copy, len)) {
        free(copy);
        return false;
    }
    f->n++;
    return true;
}

bool fnt_load_buffer(const void *buf, size_t len, fnt_font *out) {
    const uint8_t *b = buf;
    memset(out, 0, sizeof *out);
    if (len >= 2 && b[0] == 'M' && b[1] == 'Z') {
        ne_image ne;
        size_t t, j;
        if (ne_open_buffer(&ne, buf, len) != NE_OK) return false;
        for (t = 0; t < ne.restype_count; t++) {
            const ne_restype *rt = &ne.restypes[t];
            if (rt->name[0] || rt->id != 8) continue;                  /* RT_FONT */
            for (j = 0; j < rt->count; j++) {
                const ne_resource *r = &rt->res[j];
                if ((size_t)r->offset + r->length > len || !add_strike(out, b + r->offset, r->length)) {
                    ne_free(&ne);
                    fnt_free(out);
                    return false;
                }
            }
        }
        ne_free(&ne);
    } else if (!add_strike(out, b, len)) {
        return false;
    }
    if (out->n == 0) return false;
    return true;
}

bool fnt_load_file(const char *path, fnt_font *out) {
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    long n;
    bool ok;
    memset(out, 0, sizeof *out);
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0 || (n = ftell(fp)) <= 0 || fseek(fp, 0, SEEK_SET) != 0 || n > (32L << 20)) {
        fclose(fp);
        return false;
    }
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    ok = fnt_load_buffer(buf, (size_t)n, out);
    free(buf);
    return ok;
}

void fnt_free(fnt_font *f) {
    size_t i;
    for (i = 0; i < f->n; i++) free(f->s[i].blob);
    free(f->s);
    memset(f, 0, sizeof *f);
}

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

const fnt_strike *fnt_pick(const fnt_font *f, const char *face, int cell_height, int em_height) {
    const fnt_strike *best = NULL;
    int bestd = 0;
    size_t i;
    for (i = 0; i < f->n; i++) {
        const fnt_strike *s = &f->s[i];
        int want = em_height > 0 ? em_height : cell_height, have = em_height > 0 ? s->height - s->intlead : s->height, d;
        if (!ieq(s->face, face)) continue;
        d = have > want ? have - want : want - have;
        if (!best || d < bestd || (d == bestd && s->height < best->height)) {
            best = s;
            bestd = d;
        }
    }
    return best;
}

static int index_of(const fnt_strike *s, unsigned char ch) {
    return ch >= s->first && ch <= s->last ? ch - s->first : s->last - s->first + 1;
}

int fnt_width(const fnt_strike *s, unsigned char ch) { return s->width[index_of(s, ch)]; }

int fnt_pixel(const fnt_strike *s, unsigned char ch, int x, int y) {
    int i = index_of(s, ch);
    const uint8_t *col;
    if (x < 0 || y < 0 || y >= s->height || x >= s->width[i]) return 0;
    col = s->blob + s->offset[i] + (size_t)(x / 8) * (size_t)s->height;
    return (col[y] >> (7 - (x & 7))) & 1;
}
