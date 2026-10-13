/* TrueType text in GDI: faces without a raster font (Arial, MS Sans Serif, Times New Roman, Courier New) are drawn from
 * metric-compatible free faces when the configuration has a font set, 1 bit per pixel; faces from the CD's own .TTF files
 * (AddFontResource) are used under their family name; without a font set the built-in 5x7 glyphs remain. */
#include <stdlib.h>
#include <unistd.h>

#include "gfixture.h"
#include "ttf.h"

enum {
    O_TEXTOUT = 33, O_SELECTOBJECT = 45, O_CREATEFONT = 56, O_GETTEXTEXTENT = 91, O_GETTEXTMETRICS = 93,
    O_ADDFONTRESOURCE = 119, O_GETCHARWIDTH = 350, O_DELETEOBJECT = 69
};

static bool host_path(void *ctx, const char *dos, char *host, size_t cap) {
    (void)ctx;
    if (strcmp(dos, "D:\\SETUP\\LSANS.TTF")) return false;
    snprintf(host, cap, "third_party/fonts/LiberationSerif-Regular.ttf");
    return true;
}

static uint16_t put_str(const char *s) {
    uint16_t seg = g_seg(0x100);
    strcpy((char *)umem(seg, 0, 1), s);
    return seg;
}

static uint16_t mkfont(int height, int weight, int italic, const char *face) {
    uint16_t f = put_str(face);
    return GA(O_CREATEFONT, (uint16_t)height, 0, 0, 0, (uint16_t)weight, (uint16_t)italic, 0, 0, 0, 0, 0, 0, 0, FARP(f, 0));
}

static int extent_w(uint16_t dc, const char *s) {
    return (int)(GR(O_GETTEXTEXTENT, dc, FARP(put_str(s), 0), (uint16_t)strlen(s)) & 0xFFFF);
}

static int ink_in(int x0, int y0, int x1, int y1, int bg) {
    int x, y, n = 0;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) n += g_px(x, y) != bg;
    return n;
}

static void test_without_fonts(void) {
    uint16_t dc, s;
    printf("test_gdi_ttf_none\n");
    gboot();
    dc = g_screen_dc();
    s = g_seg(0x100);
    GA(O_SELECTOBJECT, dc, mkfont(-12, 400, 0, "Arial"));
    GA(O_GETTEXTMETRICS, dc, FARP(s, 0));
    CHECK_EQ(urd16(s, 0), 15);                              /* the 5x7 fallback: cell = em + (em + 4) / 5 */
    CHECK_EQ((int)umem(s, 23, 1)[0] & 4, 0);                            /* not TrueType */
    gdown();
}

static void test_faces(ttf_set *set) {
    gdi_config cfg;
    uint16_t dc, s, f;
    ttf_inst *want;
    const ttf_face *sans = ttf_set_pick(set, "Arial", 400, 0x22, NULL);
    int w_iW, w_mono_i, w_mono_W, w_arial, w_bold, w_italic;
    printf("test_gdi_ttf_faces\n");
    memset(&cfg, 0, sizeof cfg);
    cfg.fonts = set;
    cfg.host_path = host_path;
    gboot_cfg(&cfg);
    dc = g_screen_dc();
    s = g_seg(0x100);
    want = ttf_set_inst(set, sans, 0, 12);
    f = mkfont(-12, 400, 0, "Arial");
    CHECK(f != 0);
    GA(O_SELECTOBJECT, dc, f);
    CHECK_EQ(GA(O_GETTEXTMETRICS, dc, FARP(s, 0)), 1);
    CHECK_EQ(urd16(s, 0), ttf_inst_metrics(want)->height);            /* tmHeight */
    CHECK_EQ(urd16(s, 2), ttf_inst_metrics(want)->ascent);
    CHECK_EQ(urd16(s, 4), ttf_inst_metrics(want)->descent);
    CHECK_EQ(urd16(s, 6), ttf_inst_metrics(want)->intlead);
    CHECK_EQ(urd16(s, 10), ttf_inst_metrics(want)->avgw);
    CHECK_EQ((int)umem(s, 23, 1)[0] & 4, 4);                                      /* TMPF_TRUETYPE */
    CHECK_EQ(urd16(s, 25), 0);                                        /* no overhang */
    w_iW = extent_w(dc, "iW");
    CHECK_EQ(w_iW, ttf_inst_advance(want, 'i') + ttf_inst_advance(want, 'W'));
    CHECK_EQ((int)(GR(O_GETTEXTEXTENT, dc, FARP(put_str("iW"), 0), 2) >> 16), ttf_inst_metrics(want)->height);
    GA(O_GETCHARWIDTH, dc, 'W', 'i', FARP(s, 0x40));
    CHECK_EQ(urd16(s, 0x40), ttf_inst_advance(want, 'W'));
    CHECK_EQ(urd16(s, 0x40 + 2 * ('i' - 'W')), ttf_inst_advance(want, 'i'));
    /* proportional vs fixed */
    w_arial = extent_w(dc, "iiii");
    GA(O_SELECTOBJECT, dc, mkfont(-12, 400, 0, "Courier New"));
    w_mono_i = extent_w(dc, "i");
    w_mono_W = extent_w(dc, "W");
    CHECK_EQ(w_mono_i, w_mono_W);
    CHECK(w_mono_i * 4 > w_arial);
    /* bold is its own face, wider; italic keeps the advance */
    GA(O_SELECTOBJECT, dc, mkfont(-12, 700, 0, "Arial"));
    w_bold = extent_w(dc, "Bold text");
    GA(O_SELECTOBJECT, dc, mkfont(-12, 400, 0, "Arial"));
    CHECK(w_bold > extent_w(dc, "Bold text"));
    GA(O_SELECTOBJECT, dc, mkfont(-12, 400, 1, "Arial"));
    w_italic = extent_w(dc, "Bold text");
    GA(O_SELECTOBJECT, dc, mkfont(-12, 400, 0, "Arial"));
    CHECK_EQ(w_italic, extent_w(dc, "Bold text"));
    /* drawing: black ink on a white cell, thresholded: only the two colours appear */
    g_clear(7);
    GA(O_SELECTOBJECT, dc, mkfont(-16, 400, 0, "Times New Roman"));
    GA(O_TEXTOUT, dc, 10, 20, FARP(put_str("Hello"), 0), 5);
    {
        int x, y, other = 0, black = 0, cell = g_px(10, 20);        /* the opaque cell colour */
        for (y = 20; y < 40; y++)
            for (x = 10; x < 50; x++) {
                int p = g_px(x, y);
                if (p == 0) black++;
                else if (p != cell && p != 7) other++;
            }
        CHECK(black > 40);
        CHECK_EQ(other, 0);                                        /* no grey ramp: 1 bit only */
        CHECK(ink_in(120, 18, 200, 50, 7) == 0);
    }
    /* the face sits on its baseline: ink above and below the text cell origin row does not leak */
    CHECK(ink_in(0, 0, 640, 19, 7) == 0);
    gdown();
}

static void test_added_ttf(ttf_set *set) {
    gdi_config cfg;
    uint16_t dc, s;
    int w_added, w_arial;
    printf("test_gdi_ttf_added\n");
    memset(&cfg, 0, sizeof cfg);
    cfg.fonts = set;
    cfg.host_path = host_path;
    gboot_cfg(&cfg);
    dc = g_screen_dc();
    s = g_seg(0x100);
    CHECK_EQ(GA(O_ADDFONTRESOURCE, FARP(put_str("D:\\SETUP\\LSANS.TTF"), 0)), 1);
    GA(O_SELECTOBJECT, dc, mkfont(-16, 400, 0, "Lucida Sans"));
    w_added = extent_w(dc, "Hello world");
    GA(O_SELECTOBJECT, dc, mkfont(-16, 400, 0, "Arial"));
    w_arial = extent_w(dc, "Hello world");
    CHECK(w_added != w_arial);                                        /* the added file is Liberation Serif here */
    GA(O_SELECTOBJECT, dc, mkfont(-16, 400, 0, "Times New Roman"));
    CHECK_EQ(w_added, extent_w(dc, "Hello world"));
    (void)s;
    gdown();
}

int main(void) {
    ttf_set *set = ttf_set_create();
    CHECK_EQ(ttf_set_add_dir(set, "third_party/fonts"), 6);
    test_without_fonts();
    test_faces(set);
    test_added_ttf(set);
    ttf_set_free(set);
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
