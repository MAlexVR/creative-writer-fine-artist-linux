/* The program's own icon as a host icon: first icon group of the resource table decoded to ARGB (AND mask = transparent),
 * and a dependency-free PNG encoder checked down to the chunk CRCs and the stored-deflate payload. */
#include "appicon.h"
#include "install_es.h"
#include "res.h"
#include "rfixture.h"

static void bih(rb_t *b, int w, int h, int bpp, int ncol) {
    rb_u32(b, 40); rb_u32(b, (uint32_t)w); rb_u32(b, (uint32_t)h);
    rb_u16(b, 1); rb_u16(b, (unsigned)bpp);
    rb_u32(b, 0); rb_u32(b, 0); rb_u32(b, 0); rb_u32(b, 0); rb_u32(b, (uint32_t)ncol); rb_u32(b, 0);
}
static void quad(rb_t *b, int r, int g, int bl) { rb_u8(b, bl); rb_u8(b, g); rb_u8(b, r); rb_u8(b, 0); }

/* a 32x32 8-bpp icon: right half red, left half transparent; one group "APP" naming it */
static void add_icon(fx_t *f) {
    rb_t b, g;
    int y, x;
    b.n = 0;
    bih(&b, 32, 64, 8, 2);
    quad(&b, 0, 0, 0); quad(&b, 255, 0, 0);
    for (y = 0; y < 32; y++) for (x = 0; x < 32; x++) rb_u8(&b, x < 16 ? 0 : 1);
    for (y = 0; y < 32; y++) { rb_u8(&b, 0xFF); rb_u8(&b, 0xFF); rb_u8(&b, 0); rb_u8(&b, 0); }
    rf_add(f, RT_ICON, NULL, 11, NULL, b.b, b.n);
    g.n = 0;
    rb_u16(&g, 0); rb_u16(&g, 1); rb_u16(&g, 1);
    rb_u8(&g, 32); rb_u8(&g, 32); rb_u8(&g, 0); rb_u8(&g, 0); rb_u16(&g, 1); rb_u16(&g, 8); rb_u32(&g, (uint32_t)b.n); rb_u16(&g, 11);
    rf_add(f, RT_GROUP_ICON, NULL, 0, "APP", g.b, g.n);
}

static void test_from_ne(void) {
    fx_t f;
    appicon ic;
    printf("test_appicon_from_ne\n");
    fx_init(&f, 0x200, 0x100);
    CHECK(!appicon_from_ne(&f.ne, 32, &ic));                       /* no icon group: nothing */
    add_icon(&f);
    CHECK(appicon_from_ne(&f.ne, 32, &ic));
    CHECK(ic.w == 32 && ic.h == 32 && ic.argb != NULL);
    if (ic.argb) {
        CHECK_EQ(ic.argb[0], 0x00000000u);                          /* AND mask set: transparent */
        CHECK_EQ(ic.argb[15], 0x00000000u);
        CHECK_EQ(ic.argb[16], 0xFFFF0000u);                         /* opaque red */
        CHECK_EQ(ic.argb[31 * 32 + 31], 0xFFFF0000u);
    }
    appicon_free(&ic);
    CHECK(ic.argb == NULL);
}


/* a 32x32 icon image of one colour at 8 bpp (id given), or a 1 bpp black one when bpp == 1 */
static void add_flat_icon(fx_t *f, unsigned id, int bpp, int r, int g, int bl) {
    rb_t b;
    int y, x;
    b.n = 0;
    bih(&b, 32, 64, bpp, bpp == 1 ? 2 : 1);
    if (bpp == 1) { quad(&b, 0, 0, 0); quad(&b, 255, 255, 255); } else quad(&b, r, g, bl);
    for (y = 0; y < 32; y++) {
        if (bpp == 1) { rb_u8(&b, 0); rb_u8(&b, 0); rb_u8(&b, 0); rb_u8(&b, 0); }
        else for (x = 0; x < 32; x++) rb_u8(&b, 0);
    }
    for (y = 0; y < 32; y++) { rb_u8(&b, 0); rb_u8(&b, 0); rb_u8(&b, 0); rb_u8(&b, 0); }       /* AND mask: all opaque */
    rf_add(f, RT_ICON, NULL, id, NULL, b.b, b.n);
}

static void add_group(fx_t *f, const char *name, unsigned gid, const unsigned *ids, const int *bpps, int n) {
    rb_t g;
    int i;
    g.n = 0;
    rb_u16(&g, 0); rb_u16(&g, 1); rb_u16(&g, (unsigned)n);
    for (i = 0; i < n; i++) {
        rb_u8(&g, 32); rb_u8(&g, 32); rb_u8(&g, bpps[i] == 1 ? 2 : 0); rb_u8(&g, 0);
        rb_u16(&g, 1); rb_u16(&g, (unsigned)bpps[i]); rb_u32(&g, 0x100); rb_u16(&g, ids[i]);
    }
    rf_add(f, RT_GROUP_ICON, NULL, gid, name, g.b, g.n);
}

/* Which icon a program shows, the way Windows 3.1 picks it: the first group of the resource table (named or numbered, whatever
 * its id), and inside it the entry with the most colours the screen can show, the first one when two are alike. The Fine Artist
 * program carries sixteen groups (SPLAT1.., BOTH1..) and its own icon is the first. */
static void test_selection(void) {
    fx_t f;
    appicon ic;
    static const unsigned ids1[] = {11, 12, 13}, ids2[] = {14};
    static const int bpp1[] = {1, 8, 8}, bpp2[] = {8};
    printf("test_appicon_selection\n");
    fx_init(&f, 0x200, 0x100);
    add_flat_icon(&f, 11, 1, 0, 0, 0);
    add_flat_icon(&f, 12, 8, 0, 255, 0);       /* green: the colour entry that comes first */
    add_flat_icon(&f, 13, 8, 0, 0, 255);       /* blue: same size and depth, later */
    add_flat_icon(&f, 14, 8, 255, 0, 0);       /* red: belongs to the second group */
    add_group(&f, "SPLAT1", 0, ids1, bpp1, 3);
    add_group(&f, "BOTH1", 0, ids2, bpp2, 1);
    CHECK(appicon_from_ne(&f.ne, 32, &ic));
    CHECK_EQ(ic.argb[0], 0xFF00FF00u);          /* first group; colour beats mono; first of two equal entries */
    appicon_free(&ic);
}

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Launcher icons come in several sizes: whole-number nearest-neighbour scaling keeps the pixel art crisp. */
static void test_scale(void) {
    uint32_t px[2 * 2] = {0xFF111111u, 0x00000000u, 0xFF222222u, 0xFF333333u};
    appicon src = {2, 2, px}, big;
    int x, y;
    printf("test_appicon_scale\n");
    CHECK(appicon_scale(&src, 3, &big));
    CHECK(big.w == 6 && big.h == 6 && big.argb != NULL);
    if (big.argb) {
        for (y = 0; y < 6; y++)
            for (x = 0; x < 6; x++) CHECK_EQ(big.argb[y * 6 + x], px[(y / 3) * 2 + x / 3]);
    }
    appicon_free(&big);
    CHECK(appicon_scale(&src, 1, &big) && big.w == 2 && big.argb != NULL && big.argb != px);   /* a copy, never the same pixels */
    appicon_free(&big);
    CHECK(!appicon_scale(&src, 0, &big) && big.argb == NULL);
    CHECK(!appicon_scale(&src, 9999, &big) && big.argb == NULL);                               /* absurd size refused */
}

static void test_png(void) {
    uint32_t px[4 * 2] = {0xFF112233u, 0x80445566u, 0x00000000u, 0xFFFFFFFFu, 0xFF000000u, 0xFF0000FFu, 0x7F00FF00u, 0xFFFF0000u};
    appicon ic = {4, 2, px};
    uint8_t *png = NULL, *idat = NULL, raw[2 * (1 + 4 * 4)];
    size_t len = 0, pos = 8, nidat = 0, rawn = 0, k;
    bool saw_ihdr = false, saw_iend = false;
    int x, y;
    printf("test_appicon_png\n");
    CHECK(appicon_png(&ic, &png, &len) && png && len > 40);
    if (!png) return;
    CHECK(!memcmp(png, "\x89PNG\r\n\x1a\n", 8));
    idat = malloc(len);
    while (pos + 12 <= len) {
        uint32_t n = be32(png + pos);
        const uint8_t *type = png + pos + 4, *data = png + pos + 8;
        if (pos + 12 + n > len) break;
        CHECK_EQ(be32(data + n), crc32_of(type, 4 + n));            /* every chunk's CRC */
        if (!memcmp(type, "IHDR", 4)) {
            saw_ihdr = true;
            CHECK(n == 13 && be32(data) == 4 && be32(data + 4) == 2 && data[8] == 8 && data[9] == 6);   /* 4x2, 8 bit RGBA */
        } else if (!memcmp(type, "IDAT", 4)) {
            memcpy(idat + nidat, data, n);
            nidat += n;
        } else if (!memcmp(type, "IEND", 4)) {
            saw_iend = true;
        }
        pos += 12 + n;
    }
    CHECK(saw_ihdr && saw_iend);
    /* zlib stream of stored blocks: 2 header bytes, blocks (final flag, LEN, NLEN, data), Adler-32 */
    CHECK(nidat > 6 && (idat[0] & 0x0F) == 8 && ((idat[0] << 8) | idat[1]) % 31 == 0);
    k = 2;
    for (;;) {
        unsigned fin = idat[k] & 1, n = idat[k + 1] | idat[k + 2] << 8, nn = idat[k + 3] | idat[k + 4] << 8;
        CHECK_EQ((idat[k] >> 1) & 3, 0);                            /* stored */
        CHECK_EQ(n, (~nn) & 0xFFFF);
        memcpy(raw + rawn, idat + k + 5, n);
        rawn += n;
        k += 5 + n;
        if (fin) break;
    }
    CHECK_EQ(rawn, sizeof raw);
    for (y = 0; y < 2; y++) {
        CHECK_EQ(raw[y * 17], 0);                                   /* filter none */
        for (x = 0; x < 4; x++) {
            uint32_t v = px[y * 4 + x];
            const uint8_t *q = raw + y * 17 + 1 + x * 4;
            CHECK(q[0] == ((v >> 16) & 255) && q[1] == ((v >> 8) & 255) && q[2] == (v & 255) && q[3] == (v >> 24));
        }
    }
    {   /* Adler-32 of the raw bytes */
        uint32_t a = 1, b = 0;
        for (k = 0; k < rawn; k++) {
            a = (a + raw[k]) % 65521;
            b = (b + a) % 65521;
        }
        CHECK_EQ(be32(idat + nidat - 4), (b << 16) | a);
    }
    free(idat);
    free(png);
    CHECK(!appicon_png(&(appicon){0, 0, NULL}, &png, &len));
}

/* What the icon shows, counted from its pixels: the program's colour (Creative Writer is a green waste-paper basket with a pencil,
 * Fine Artist a magenta/pink-red one with a brush) and the CD drawn at its bottom left in the English CD edition (silver pixels
 * 0xC0C0C0 in that quadrant; the Spanish editions carry the same artwork without the disc). */
typedef struct { int green, magenta, disc; } icon_look;

static icon_look look_at(const appicon *ic) {
    icon_look l = {0, 0, 0};
    int x, y;
    for (y = 0; y < ic->h; y++)
        for (x = 0; x < ic->w; x++) {
            uint32_t v = ic->argb[y * ic->w + x];
            int r = (int)(v >> 16 & 0xFF), g = (int)(v >> 8 & 0xFF), b = (int)(v & 0xFF);
            if ((v >> 24) != 0xFF) continue;
            if (g > r + 80 && g > b + 80) l.green++;
            if (r > g + 80 && b > g + 80) l.magenta++;
            if (x < ic->w / 2 && y >= ic->h / 2 && r == 0xC0 && g == 0xC0 && b == 0xC0) l.disc++;
        }
    return l;
}

/* The real programs, when given: the first icon group of the resource table, as Windows shows it. want_green: Creative Writer;
 * with_disc: the English CD edition. */
static void check_real(const char *path, const char *label, bool want_green, bool with_disc) {
    ne_image ne;
    appicon ic;
    icon_look l;
    printf("test_appicon_real_%s\n", label);
    CHECK_EQ(ne_open_file(&ne, path), NE_OK);
    CHECK(appicon_from_ne(&ne, 32, &ic));
    CHECK(ic.w == 32 && ic.h == 32);
    {
        int i, opaque = 0;
        for (i = 0; i < ic.w * ic.h; i++) opaque += (ic.argb[i] >> 24) == 0xFF;
        CHECK(opaque > 100);                                        /* a drawn icon, not an empty one */
    }
    l = look_at(&ic);
    if (want_green) CHECK(l.green > 100 && l.green > 3 * l.magenta);      /* Creative Writer: green */
    else CHECK(l.magenta > 100 && l.magenta > 3 * l.green);               /* Fine Artist: magenta / pink-red */
    if (with_disc) CHECK(l.disc >= 20);
    else CHECK(l.disc <= 3);
    appicon_free(&ic);
    ne_free(&ne);
}

static void test_real(const char *var, const char *label, bool want_green) {
    const char *p = getenv(var);
    if (!p || !*p) {
        printf("test_appicon_real_%s\n  skipped (%s not set)\n", label, var);
        return;
    }
    check_real(p, label, want_green, true);                         /* the English CD edition draws a CD on both */
}

/* the Spanish editions, staged from their media into a scratch drive C: */
static void test_real_es(const char *var, const char *exe_name, const char *label, bool want_green) {
    const char *media = getenv(var);
    char c[256], exe[300];
    install_report rep;
    if (!media || !*media) {
        printf("test_appicon_real_%s\n  skipped (%s not set)\n", label, var);
        return;
    }
    snprintf(c, sizeof c, "build/appicon_%s", label);
    CHECK_EQ(system("rm -rf build/appicon_es_writer build/appicon_es_artist"), 0);
    CHECK(install_stage_es(media, c, NULL, &rep));
    snprintf(exe, sizeof exe, "%s/MSKIDS/%s", c, exe_name);
    check_real(exe, label, want_green, false);                      /* no CD on them */
}

int main(void) {
    test_from_ne();
    test_scale();
    test_selection();
    test_png();
    test_real("CWFA_WRITER_EXE", "writer", true);
    test_real("CWFA_ARTIST_EXE", "artist", false);
    test_real_es("CWFA_ES_CD", "WRITER.EXE", "es_writer", true);
    test_real_es("CWFA_ES_FA", "ARTIST.EXE", "es_artist", false);
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
