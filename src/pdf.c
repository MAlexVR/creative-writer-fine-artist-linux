#include "pdf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double x, y, size; char *text; size_t n; } ptext;

typedef struct {
    double w, h;
    int iw, ih;
    uint8_t *enc;            /* RunLengthDecode image data */
    size_t enc_len;
    uint8_t pal[256][3];
    ptext *texts;
    size_t ntexts, captexts;
} ppage;

struct pdf {
    ppage *pages;
    size_t n, cap;
};

/* ---- RunLengthDecode --------------------------------------------------------------------------------------- */

size_t pdf_rle_encode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    size_t i = 0, o = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 128 && src[i + run] == src[i]) run++;
        if (run >= 2) {                                  /* a repeat: 257 - count, then the byte */
            if (o + 2 > cap) return 0;
            dst[o++] = (uint8_t)(257 - run);
            dst[o++] = src[i];
            i += run;
        } else {                                         /* literals up to the next run of 2+ (or 128 bytes) */
            size_t lit = 1;
            while (i + lit < n && lit < 128 && !(i + lit + 1 < n && src[i + lit] == src[i + lit + 1])) lit++;
            if (o + 1 + lit > cap) return 0;
            dst[o++] = (uint8_t)(lit - 1);
            memcpy(dst + o, src + i, lit);
            o += lit;
            i += lit;
        }
    }
    if (o + 1 > cap) return 0;
    dst[o++] = 128;
    return o;
}

/* ---- document ---------------------------------------------------------------------------------------------- */

pdf_t *pdf_create(void) { return calloc(1, sizeof(pdf_t)); }

void pdf_free(pdf_t *p) {
    size_t i, k;
    if (!p) return;
    for (i = 0; i < p->n; i++) {
        for (k = 0; k < p->pages[i].ntexts; k++) free(p->pages[i].texts[k].text);
        free(p->pages[i].texts);
        free(p->pages[i].enc);
    }
    free(p->pages);
    free(p);
}

size_t pdf_page_count(const pdf_t *p) { return p->n; }

bool pdf_add_page(pdf_t *p, double w_pt, double h_pt, int iw, int ih, const uint8_t *idx, const uint8_t pal[256][3]) {
    ppage *pg;
    size_t n, cap;
    if (!p || !idx || !pal || w_pt <= 0 || h_pt <= 0 || iw <= 0 || ih <= 0 || (long long)iw * ih > (1LL << 28)) return false;
    if (p->n == p->cap) {
        size_t nc = p->cap ? p->cap * 2 : 4;
        ppage *np = realloc(p->pages, nc * sizeof *np);
        if (!np) return false;
        p->pages = np;
        p->cap = nc;
    }
    n = (size_t)iw * (size_t)ih;
    cap = n + n / 128 + 16;                              /* the worst case: all literals */
    pg = &p->pages[p->n];
    memset(pg, 0, sizeof *pg);
    pg->enc = malloc(cap);
    if (!pg->enc) return false;
    pg->enc_len = pdf_rle_encode(idx, n, pg->enc, cap);
    if (!pg->enc_len) {
        free(pg->enc);
        return false;
    }
    pg->enc = realloc(pg->enc, pg->enc_len);             /* shrinking cannot fail in practice; the old block stays valid */
    pg->w = w_pt;
    pg->h = h_pt;
    pg->iw = iw;
    pg->ih = ih;
    memcpy(pg->pal, pal, sizeof pg->pal);
    p->n++;
    return true;
}

bool pdf_page_text(pdf_t *p, double x_pt, double y_pt, double size_pt, const char *text, size_t n) {
    ppage *pg;
    ptext *t;
    if (!p || !p->n || !text || !n) return p && p->n;
    pg = &p->pages[p->n - 1];
    if (pg->ntexts == pg->captexts) {
        size_t nc = pg->captexts ? pg->captexts * 2 : 16;
        ptext *nt = realloc(pg->texts, nc * sizeof *nt);
        if (!nt) return false;
        pg->texts = nt;
        pg->captexts = nc;
    }
    t = &pg->texts[pg->ntexts];
    t->text = malloc(n + 1);
    if (!t->text) return false;
    memcpy(t->text, text, n);
    t->text[n] = 0;
    t->n = n;
    t->x = x_pt;
    t->y = y_pt;
    t->size = size_pt;
    pg->ntexts++;
    return true;
}

/* ---- serialisation ----------------------------------------------------------------------------------------- */

typedef struct { uint8_t *b; size_t n, cap; bool bad; } wbuf;

static void put(wbuf *w, const void *d, size_t n) {
    if (w->bad) return;
    if (w->n + n > w->cap) {
        size_t nc = w->cap ? w->cap : 4096;
        uint8_t *nb;
        while (nc < w->n + n) nc *= 2;
        nb = realloc(w->b, nc);
        if (!nb) {
            w->bad = true;
            return;
        }
        w->b = nb;
        w->cap = nc;
    }
    memcpy(w->b + w->n, d, n);
    w->n += n;
}

static void putf(wbuf *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void putf(wbuf *w, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) put(w, tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1));
}

static void put_string(wbuf *w, const char *s, size_t n) {
    size_t i;
    put(w, "(", 1);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '(' || c == ')' || c == '\\') {
            char e[2] = {'\\', (char)c};
            put(w, e, 2);
        } else if (c < 32 || c >= 127) putf(w, "\\%03o", c);
        else put(w, &c, 1);
    }
    put(w, ")", 1);
}

bool pdf_save(const pdf_t *p, const char *path) {
    wbuf w = {0};
    size_t i, k, nobj = 3 + 3 * p->n, *off = calloc(nobj + 1, sizeof *off);
    FILE *f;
    bool ok;
    if (!off) return false;
    putf(&w, "%%PDF-1.4\n%%\xE2\xE3\xCF\xD3\n");
    off[1] = w.n;
    putf(&w, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    off[2] = w.n;
    putf(&w, "2 0 obj\n<< /Type /Pages /Count %zu /Kids [", p->n);
    for (i = 0; i < p->n; i++) putf(&w, "%zu 0 R ", 4 + 3 * i);
    putf(&w, "] >>\nendobj\n");
    off[3] = w.n;
    putf(&w, "3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>\nendobj\n");
    for (i = 0; i < p->n; i++) {
        const ppage *pg = &p->pages[i];
        size_t pid = 4 + 3 * i, cid = pid + 1, iid = pid + 2;
        wbuf c = {0};
        putf(&c, "q %.2f 0 0 %.2f 0 0 cm /Im0 Do Q\n", pg->w, pg->h);
        if (pg->ntexts) putf(&c, "BT 3 Tr\n");
        for (k = 0; k < pg->ntexts; k++) {
            const ptext *t = &pg->texts[k];
            putf(&c, "/F1 %.2f Tf 1 0 0 1 %.2f %.2f Tm ", t->size, t->x, pg->h - t->y);
            put_string(&c, t->text, t->n);
            putf(&c, " Tj\n");
        }
        if (pg->ntexts) putf(&c, "ET\n");
        off[pid] = w.n;
        putf(&w, "%zu 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Resources << /XObject << /Im0 %zu 0 R >> "
                 "/Font << /F1 3 0 R >> >> /Contents %zu 0 R >>\nendobj\n", pid, pg->w, pg->h, iid, cid);
        off[cid] = w.n;
        putf(&w, "%zu 0 obj\n<< /Length %zu >>\nstream\n", cid, c.n);
        put(&w, c.b, c.n);
        putf(&w, "endstream\nendobj\n");
        free(c.b);
        off[iid] = w.n;
        putf(&w, "%zu 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d /BitsPerComponent 8 "
                 "/ColorSpace [/Indexed /DeviceRGB 255 <", iid, pg->iw, pg->ih);
        for (k = 0; k < 256; k++) putf(&w, "%02x%02x%02x", pg->pal[k][0], pg->pal[k][1], pg->pal[k][2]);
        putf(&w, ">] /Filter /RunLengthDecode /Length %zu >>\nstream\n", pg->enc_len);
        put(&w, pg->enc, pg->enc_len);
        putf(&w, "\nendstream\nendobj\n");
    }
    {
        size_t xref = w.n;
        putf(&w, "xref\n0 %zu\n0000000000 65535 f \n", nobj + 1);
        for (i = 1; i <= nobj; i++) putf(&w, "%010zu 00000 n \n", off[i]);
        putf(&w, "trailer\n<< /Size %zu /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n", nobj + 1, xref);
    }
    free(off);
    if (w.bad) {
        free(w.b);
        return false;
    }
    f = fopen(path, "wb");
    ok = f && fwrite(w.b, 1, w.n, f) == w.n;
    if (f && fclose(f) != 0) ok = false;
    free(w.b);
    return ok;
}
