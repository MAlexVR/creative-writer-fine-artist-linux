/* Metafiles: SetMetaFileBits/GetMetaFileBits/DeleteMetaFile, playing records on a DC, recording them through a
 * metafile DC. A metafile handle is the global handle of its bits (here a plain selector: the fixture has no KERNEL). */
#include "gfixture.h"

#define META_SETBKMODE 0x0102
#define META_SETMAPMODE 0x0103
#define META_SETWINDOWORG 0x020B
#define META_SETWINDOWEXT 0x020C
#define META_RECTANGLE 0x041B
#define META_SELECTOBJECT 0x012D
#define META_DELETEOBJECT 0x01F0
#define META_CREATEBRUSHINDIRECT 0x02FC
#define META_CREATEPENINDIRECT 0x02FA
#define META_EOF 0x0000
#define META_MOVETO 0x0214
#define META_LINETO 0x0213
#define META_POLYGON 0x0324
#define META_POLYLINE 0x0325
#define META_TEXTOUT 0x0521
#define META_SETTEXTCOLOR 0x0209
#define META_SAVEDC 0x001E
#define META_STRETCHDIB 0x0F43
#define META_DIBSTRETCHBLT 0x0B41
#define META_DIBBITBLT 0x0940
#define META_SETDIBTODEV 0x0D33

typedef struct {
    uint8_t b[8192];
    size_t n;
    unsigned nobj, maxrec;
} mf_t;

static void mf_w16(mf_t *m, uint16_t v) { m->b[m->n++] = (uint8_t)v; m->b[m->n++] = (uint8_t)(v >> 8); }
static void mf_w32(mf_t *m, uint32_t v) { mf_w16(m, (uint16_t)v); mf_w16(m, (uint16_t)(v >> 16)); }

static void mf_begin(mf_t *m) {
    memset(m, 0, sizeof *m);
    mf_w16(m, 1);        /* memory metafile */
    mf_w16(m, 9);        /* header size in words */
    mf_w16(m, 0x300);
    mf_w32(m, 0);        /* total size in words: patched by mf_end */
    mf_w16(m, 0);        /* objects: patched */
    mf_w32(m, 0);        /* largest record: patched */
    mf_w16(m, 0);
}

static void mf_rec(mf_t *m, uint16_t func, const uint16_t *p, unsigned np) {
    unsigned i, size = 3 + np;
    mf_w32(m, size);
    mf_w16(m, func);
    for (i = 0; i < np; i++) mf_w16(m, p[i]);
    if (size > m->maxrec) m->maxrec = size;
}
#define MF(m, f, ...) mf_rec((m), (f), (const uint16_t[]){0, ##__VA_ARGS__} + 1, sizeof((const uint16_t[]){0, ##__VA_ARGS__}) / sizeof(uint16_t) - 1)

static void mf_end(mf_t *m, unsigned nobj) {
    MF(m, META_EOF);
    m->b[6] = (uint8_t)(m->n / 2); m->b[7] = (uint8_t)(m->n / 2 >> 8); m->b[8] = (uint8_t)(m->n / 2 >> 16); m->b[9] = (uint8_t)(m->n / 2 >> 24);
    m->b[10] = (uint8_t)nobj; m->b[11] = (uint8_t)(nobj >> 8);
    m->b[12] = (uint8_t)m->maxrec; m->b[13] = (uint8_t)(m->maxrec >> 8);
}

/* The bits in a fresh guest block, as a program's GlobalAlloc block would hold them. */
static uint16_t mf_block(const mf_t *m) {
    uint16_t sel = g_seg((uint32_t)m->n);
    memcpy(umem(sel, 0, (uint32_t)m->n), m->b, m->n);
    return sel;
}

/* A solid-colour brush drawn rectangle at the given place directly through the API: the reference pixels. */
static void direct_rect(uint16_t dc, uint32_t color, int l, int t, int r, int b) {
    uint16_t br = GA(66, (uint16_t)(color >> 16), (uint16_t)color);   /* a DWORD goes high word first */
    GA(45, dc, br);
    GA(27, dc, (uint16_t)l, (uint16_t)t, (uint16_t)r, (uint16_t)b);
}

static void test_bits_and_delete(void) {
    mf_t m;
    uint16_t blk, h;
    printf("test_meta_bits\n");
    gboot();
    mf_begin(&m);
    MF(&m, META_SETBKMODE, 1);
    mf_end(&m, 0);
    blk = mf_block(&m);
    h = GA(160, blk);                            /* SetMetaFileBits */
    CHECK(h != 0);
    CHECK_EQ(GA(159, h), h);                     /* GetMetaFileBits gives the block back */
    CHECK_EQ(GA(127, h), 1);                     /* DeleteMetaFile */
    CHECK_EQ(mem_size(&uf.p->mem, blk), 0);      /* ... frees it */
    m.b[0] = 7;                                  /* not a metafile type */
    blk = mf_block(&m);
    CHECK_EQ(GA(160, blk), 0);
    gdown();
}

static void test_play_rectangle(void) {
    mf_t m;
    uint16_t dc, h, blk;
    uint8_t want;
    printf("test_meta_play_rectangle\n");
    gboot();
    dc = g_screen_dc();
    direct_rect(dc, RGB(255, 0, 0), 100, 100, 120, 120);
    want = g_px(110, 110);
    mf_begin(&m);
    MF(&m, META_CREATEBRUSHINDIRECT, 0, 0x00FF, 0x0000, 0);   /* style, color lo, hi, hatch */
    MF(&m, META_SELECTOBJECT, 0);
    MF(&m, META_RECTANGLE, 30, 30, 10, 10);                   /* bottom, right, top, left */
    MF(&m, META_SELECTOBJECT, 0);
    MF(&m, META_DELETEOBJECT, 0);
    mf_end(&m, 1);
    h = GA(160, mf_block(&m));
    CHECK(h != 0);
    CHECK_EQ(GA(123, dc, h), 1);                 /* PlayMetaFile */
    CHECK_EQ(g_px(20, 20), want);                /* the brush filled it */
    CHECK(g_px(5, 5) != want);
    (void)blk;
    gdown();
}

/* A pen record, a line drawn with it, and the DC afterwards as it was before the metafile ran. */
static void test_play_pen_and_state(void) {
    mf_t m;
    uint16_t dc, h;
    uint8_t line;
    printf("test_meta_play_pen_and_state\n");
    gboot();
    dc = g_screen_dc();
    /* reference: the same line with the same pen, straight through the API */
    {
        uint16_t pen = GA(61, 0, 1, 0x0000, 0x00FF);                           /* style, width, color (hi, lo) */
        GA(45, dc, pen);
        GA(20, dc, 100, 50);                                                   /* MoveTo */
        GA(19, dc, 140, 50);                                                   /* LineTo */
        line = g_px(120, 50);
        GA(45, dc, GA(87, 7));                                                 /* back to the stock black pen */
    }
    mf_begin(&m);
    MF(&m, META_CREATEPENINDIRECT, 0, 0, 0, 0x00FF, 0x0000);    /* style, width x, width y, color lo, hi */
    MF(&m, META_SELECTOBJECT, 0);
    MF(&m, META_MOVETO, 90, 10);                                  /* y, x */
    MF(&m, META_LINETO, 90, 50);
    mf_end(&m, 1);
    h = GA(160, mf_block(&m));
    CHECK_EQ(GA(123, dc, h), 1);
    CHECK_EQ(g_px(30, 90), line);                                 /* the red line the record drew */
    CHECK(g_px(60, 90) != line);
    GA(19, dc, 200, 200);                                         /* the DC's own pen is back: black, not red */
    CHECK(g_px(150, 200) != line);
    gdown();
}

static void test_play_text_and_poly(void) {
    mf_t m;
    uint16_t dc, h, ref;
    uint8_t want_text[64], got_text[64];
    int i;
    printf("test_meta_play_text_and_poly\n");
    gboot();
    dc = g_screen_dc();
    ref = g_seg(16);
    memcpy(umem(ref, 0, 8), "Hello", 6);
    GA(33, dc, 20, 20, ref, 0, 5);                               /* TextOut(hdc, x, y, str far, count) */
    for (i = 0; i < 64; i++) want_text[i] = g_px(20 + i % 32, 20 + i / 32);
    g_clear(0xFF);                                              /* white: black ink is index 0 */
    mf_begin(&m);
    {   /* TEXTOUT: count, string padded to a word, y, x */
        const uint16_t p[] = {5, 'H' | 'e' << 8, 'l' | 'l' << 8, 'o', 20, 20};
        mf_rec(&m, META_TEXTOUT, p, 6);
    }
    {   /* a triangle: count, then x, y pairs */
        const uint16_t p[] = {3, 100, 100, 140, 100, 120, 140};
        mf_rec(&m, META_POLYGON, p, 7);
    }
    {
        const uint16_t p[] = {2, 200, 100, 240, 140};
        mf_rec(&m, META_POLYLINE, p, 5);
    }
    mf_end(&m, 0);
    h = GA(160, mf_block(&m));
    CHECK_EQ(GA(123, dc, h), 1);
    for (i = 0; i < 64; i++) got_text[i] = g_px(20 + i % 32, 20 + i / 32);
    CHECK(!memcmp(want_text, got_text, 64));
    {
        uint8_t blank[64];
        memset(blank, 0xFF, sizeof blank);
        CHECK(memcmp(got_text, blank, 64) != 0);                 /* text pixels were drawn at all */
    }
    CHECK(g_px(120, 100) != 0xFF);                               /* the triangle outline */
    CHECK(g_px(220, 120) != 0xFF);                               /* the polyline's diagonal */
    gdown();
}

/* A 4x4 8-bit DIB with four colours: the header, the colour table and the rows (bottom row first). */
static size_t make_dib(uint8_t *d) {
    static const uint8_t rows[16] = {0, 1, 2, 3, 1, 2, 3, 0, 2, 3, 0, 1, 3, 0, 1, 2};
    static const uint8_t cols[4][4] = {{0, 0, 255, 0}, {0, 255, 0, 0}, {255, 0, 0, 0}, {0, 255, 255, 0}};   /* B G R x */
    memset(d, 0, 40 + 16 + 16);
    d[0] = 40; d[4] = 4; d[8] = 4; d[12] = 1; d[14] = 8; d[32] = 4;   /* size, w, h, planes, bpp, colours used */
    memcpy(d + 40, cols, 16);
    memcpy(d + 56, rows, 16);
    return 72;
}

/* Plays a one-record metafile (the parameters, then the DIB) and returns the 16x16 pixels at (20,20). */
static void play_dib_record(uint16_t func, const uint16_t *head, unsigned nhead, const uint8_t *dib, size_t dibn, uint8_t *out) {
    mf_t m;
    uint16_t p[64 + 64], h, dc = g_screen_dc();
    int i;
    memcpy(p, head, nhead * 2u);
    memcpy(p + nhead, dib, dibn);
    g_clear(0xFF);
    mf_begin(&m);
    mf_rec(&m, func, p, nhead + (unsigned)dibn / 2);
    mf_end(&m, 0);
    h = GA(160, mf_block(&m));
    CHECK_EQ(GA(123, dc, h), 1);
    for (i = 0; i < 256; i++) out[i] = g_px(20 + i % 16, 20 + i / 16);
}

static void test_play_dibs(void) {
    uint16_t dc, ref;
    uint8_t want[256], got[256], blank[256];
    uint8_t dib[72];
    printf("test_meta_play_dibs\n");
    gboot();
    dc = g_screen_dc();
    make_dib(dib);
    memset(blank, 0xFF, sizeof blank);
    ref = g_seg(128);
    memcpy(umem(ref, 0, 72), dib, 72);
    /* StretchDIBits(hdc, xd, yd, cxd, cyd, xs, ys, cxs, cys, bits far, bmi far, usage, rop dword) */
    GA(439, dc, 20, 20, 16, 16, 0, 0, 4, 4, ref, 56, ref, 0, 0, 0x00CC, 0x0020);
    {
        int i;
        for (i = 0; i < 256; i++) want[i] = g_px(20 + i % 16, 20 + i / 16);
    }
    CHECK(memcmp(want, blank, 256) != 0);
    {   /* STRETCHDIB: rop (lo, hi), usage, cySrc, cxSrc, ySrc, xSrc, cyDest, cxDest, yDest, xDest, then the DIB */
        const uint16_t head[11] = {0x0020, 0x00CC, 0, 4, 4, 0, 0, 16, 16, 20, 20};
        play_dib_record(META_STRETCHDIB, head, 11, dib, 72, got);
        CHECK(!memcmp(want, got, 256));
    }
    {   /* DIBSTRETCHBLT: rop, cySrc, cxSrc, ySrc, xSrc, cyDest, cxDest, yDest, xDest, then the DIB */
        const uint16_t head[10] = {0x0020, 0x00CC, 4, 4, 0, 0, 16, 16, 20, 20};
        play_dib_record(META_DIBSTRETCHBLT, head, 10, dib, 72, got);
        CHECK(!memcmp(want, got, 256));
    }
    {   /* DIBBITBLT: rop, ySrc, xSrc, cy, cx, yDest, xDest, then the DIB: 1:1 */
        const uint16_t head[8] = {0x0020, 0x00CC, 0, 0, 4, 4, 20, 20};
        play_dib_record(META_DIBBITBLT, head, 8, dib, 72, got);
        CHECK(memcmp(got, blank, 256) != 0);
        CHECK_EQ(got[0], want[0]);                              /* the 4x4 picture in the corner, unstretched */
        CHECK_EQ(got[3], want[15]);
        CHECK_EQ(got[16 * 3 + 3], want[16 * 15 + 15]);
        CHECK_EQ(got[16 * 5], 0xFF);
    }
    {   /* SETDIBTODEV: usage, numscans, startscan, ySrc, xSrc, cy, cx, yDest, xDest, then the DIB */
        const uint16_t head[9] = {0, 4, 0, 0, 0, 4, 4, 20, 20};
        play_dib_record(META_SETDIBTODEV, head, 9, dib, 72, got);
        CHECK_EQ(got[0], want[0]);
        CHECK_EQ(got[16 * 3 + 3], want[16 * 15 + 15]);
    }
    gdown();
}

/* PlayMetaFileRecord plays one record against a handle table the caller keeps; EnumMetaFile hands every record,
 * the EOF included, to a callback. */
static void test_record_by_record(void) {
    mf_t m;
    uint16_t dc, h, ds, tbl, rec, ref, cb_far_off;
    uint8_t want;
    printf("test_meta_records\n");
    gboot();
    dc = g_screen_dc();
    ds = u_ds();
    direct_rect(dc, RGB(255, 0, 0), 100, 100, 120, 120);
    want = g_px(110, 110);
    g_clear(0xFF);
    tbl = g_seg(64);
    rec = g_seg(64);
    /* a CREATEBRUSHINDIRECT record: it fills table slot 0; then SELECTOBJECT 0 and RECTANGLE */
    {
        const uint16_t p[] = {0, 0x00FF, 0x0000, 0};
        uint8_t *r = umem(rec, 0, 14);
        r[0] = 7; r[1] = 0; r[2] = 0; r[3] = 0;                    /* size: 3 + 4 words */
        r[4] = 0xFC; r[5] = 0x02;
        memcpy(r + 6, p, 8);
        CHECK_EQ(GA(176, dc, tbl, 0, rec, 0, 4), 1);              /* PlayMetaFileRecord(hdc, ht far, rec far, n) */
        CHECK(urd16(tbl, 0) != 0);                                 /* the brush handle landed in slot 0 */
    }
    {
        uint8_t *r = umem(rec, 0, 10);
        r[0] = 4; r[1] = 0; r[2] = 0; r[3] = 0;
        r[4] = 0x2D; r[5] = 0x01; r[6] = 0; r[7] = 0;
        CHECK_EQ(GA(176, dc, tbl, 0, rec, 0, 4), 1);              /* SELECTOBJECT 0 */
    }
    {
        uint8_t *r = umem(rec, 0, 14);
        r[0] = 7; r[1] = 0; r[2] = 0; r[3] = 0;
        r[4] = 0x1B; r[5] = 0x04;
        r[6] = 30; r[7] = 0; r[8] = 30; r[9] = 0; r[10] = 10; r[11] = 0; r[12] = 10; r[13] = 0;
        CHECK_EQ(GA(176, dc, tbl, 0, rec, 0, 4), 1);              /* RECTANGLE */
    }
    CHECK_EQ(g_px(20, 20), want);
    /* EnumMetaFile */
    ref = g_seg(16);
    (void)ref;
    cb_far_off = U_CODE_ENUM;
    mf_begin(&m);
    MF(&m, META_SETBKMODE, 1);
    MF(&m, META_SETBKMODE, 2);
    mf_end(&m, 0);
    h = GA(160, mf_block(&m));
    uwr16(ds, 0x60, 0);
    CHECK_EQ(GA(175, dc, h, u_cs(), cb_far_off, 0x1234, 0x5678), 1);   /* EnumMetaFile(hdc, hmf, proc far, data dword) */
    CHECK_EQ(urd16(ds, 0x60), 3);                                  /* two records and the EOF */
    CHECK_EQ(urd16(ds, 0x62), 0x5678);                             /* the client data came through */
    gdown();
}

/* ---- recording ---------------------------------------------------------------------------------- */

static uint16_t hdr16(const uint8_t *b, unsigned off) { return (uint16_t)(b[off] | b[off + 1] << 8); }
static uint32_t hdr32(const uint8_t *b, unsigned off) { return hdr16(b, off) | (uint32_t)hdr16(b, off + 2) << 16; }

/* The bits of a closed metafile, or NULL. */
static const uint8_t *meta_of(uint16_t hmf, uint32_t *bytes) {
    uint16_t bits = GA(159, hmf);
    const uint8_t *b;
    if (!bits) return NULL;
    b = umem(bits, 0, 18);
    if (b && bytes) *bytes = hdr32(b, 6) * 2u;
    return b ? umem(bits, 0, hdr32(b, 6) * 2u) : NULL;
}

static void test_record_shapes(void) {
    uint16_t mdc, hmf, br, dc;
    const uint8_t *b;
    uint32_t n;
    printf("test_meta_record_shapes\n");
    gboot();
    dc = g_screen_dc();
    mdc = GA(125, 0, 0);                                           /* CreateMetaFile(NULL) */
    CHECK(mdc != 0);
    CHECK(mdc != dc);
    GR(11, mdc, 0, 0);                                             /* SetWindowOrg(hdc, x, y) */
    GR(12, mdc, 100, 80);                                          /* SetWindowExt */
    br = GA(66, 0x0000, 0x00FF);                                   /* a red brush */
    GA(45, mdc, br);                                               /* SelectObject: a create record, then the select */
    GA(27, mdc, 10, 20, 30, 40);                                   /* Rectangle(hdc, l, t, r, b) */
    hmf = GA(126, mdc);                                            /* CloseMetaFile */
    CHECK(hmf != 0);
    b = meta_of(hmf, &n);
    CHECK(b != NULL);
    if (b) {
        unsigned at = 18, i = 0;
        static const struct { uint16_t func; unsigned size; } want[] = {
            {0x020B, 5}, {0x020C, 5}, {0x02FC, 7}, {0x012D, 4}, {0x041B, 7}, {0x0000, 3}};
        CHECK_EQ(hdr16(b, 0), 1);                                  /* memory metafile */
        CHECK_EQ(hdr16(b, 2), 9);
        CHECK_EQ(hdr16(b, 4), 0x300);
        CHECK_EQ(hdr16(b, 10), 1);                                 /* one object */
        CHECK_EQ(hdr32(b, 12), 7);                                 /* the longest record */
        for (i = 0; i < sizeof want / sizeof want[0] && at + 6 <= n; i++) {
            CHECK_EQ(hdr32(b, at), want[i].size);
            CHECK_EQ(hdr16(b, at + 4), want[i].func);
            at += want[i].size * 2;
        }
        CHECK_EQ(at, n);                                           /* nothing else */
        CHECK_EQ(hdr16(b, 18 + 6), 0);                             /* SetWindowOrg: y, x */
        CHECK_EQ(hdr16(b, 18 + 10 + 6), 80);                       /* SetWindowExt: y = 80 */
        CHECK_EQ(hdr16(b, 18 + 10 + 8), 100);                      /* x = 100 */
        CHECK_EQ(hdr16(b, 18 + 20 + 8), 0x00FF);                   /* the brush: style, color lo (word 1) */
        CHECK_EQ(hdr16(b, 18 + 34 + 6), 0);                        /* SELECTOBJECT 0 */
        CHECK_EQ(hdr16(b, 18 + 42 + 6), 40);                       /* Rectangle: bottom, right, top, left */
        CHECK_EQ(hdr16(b, 18 + 42 + 8), 30);
        CHECK_EQ(hdr16(b, 18 + 42 + 10), 20);
        CHECK_EQ(hdr16(b, 18 + 42 + 12), 10);
    }
    CHECK_EQ(GA(126, mdc), 0);                                     /* it is closed: the DC is gone */
    gdown();
}

/* What a program draws on a metafile DC and plays back equals what it draws directly: text, a polygon, a pen and a
 * brush, a stretched DIB. */
static void draw_scene(uint16_t dc, uint16_t str, uint16_t pts, uint16_t dibseg) {
    uint16_t pen = GA(61, 0, 1, 0x00FF, 0x0000);                  /* CreatePen(style, width, color): hi, lo */
    uint16_t br = GA(66, 0x00FF, 0x0000);
    GA(45, dc, pen);
    GA(45, dc, br);
    GA(33, dc, 20, 20, str, 0, 5);                                /* TextOut "Hello" */
    GA(36, dc, pts, 0, 3);                                        /* Polygon */
    GA(439, dc, 120, 120, 16, 16, 0, 0, 4, 4, dibseg, 56, dibseg, 0, 0, 0x00CC, 0x0020);
    GA(27, dc, 200, 200, 230, 220);
}

static void test_record_roundtrip(void) {
    uint16_t dc, mdc, hmf, str, pts, dibseg;
    uint8_t dib[72];
    static uint8_t want[480 * 640], got[480 * 640];
    printf("test_meta_record_roundtrip\n");
    gboot();
    dc = g_screen_dc();
    make_dib(dib);
    str = g_seg(16);
    memcpy(umem(str, 0, 6), "Hello", 6);
    pts = g_seg(32);
    {
        const uint16_t tri[6] = {60, 60, 100, 60, 80, 100};
        memcpy(umem(pts, 0, 12), tri, 12);
    }
    dibseg = g_seg(128);
    memcpy(umem(dibseg, 0, 72), dib, 72);
    g_clear(0xFF);
    draw_scene(dc, str, pts, dibseg);
    memcpy(want, g_fb()->p8, sizeof want);
    mdc = GA(125, 0, 0);
    draw_scene(mdc, str, pts, dibseg);
    hmf = GA(126, mdc);
    CHECK(hmf != 0);
    g_clear(0xFF);
    CHECK_EQ(GA(123, dc, hmf), 1);
    memcpy(got, g_fb()->p8, sizeof got);
    CHECK(!memcmp(want, got, sizeof got));
    {
        uint32_t n;
        const uint8_t *b = meta_of(hmf, &n);
        unsigned at = 18, kinds[16] = {0}, nk = 0;
        while (b && at + 6 <= n && nk < 16) {
            kinds[nk++] = hdr16(b, at + 4);
            at += hdr32(b, at) * 2;
        }
        CHECK(nk >= 8);
        CHECK_EQ(kinds[0], 0x02FA);                               /* the pen's create record first */
        CHECK_EQ(kinds[nk - 1], 0x0000);
    }
    gdown();
}

int main(void) {
    test_bits_and_delete();
    test_play_rectangle();
    test_play_pen_and_state();
    test_play_text_and_poly();
    test_play_dibs();
    test_record_by_record();
    test_record_shapes();
    test_record_roundtrip();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
