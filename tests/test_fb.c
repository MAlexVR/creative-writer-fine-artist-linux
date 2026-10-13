/* Software framebuffer: palette, fills, conversion. */
#include <string.h>

#include "fb.h"
#include "fixture.h"

static void test_init(void) {
    fb_t f;
    printf("test_fb_init\n");
    CHECK(fb_init(&f, 640, 480, 8));
    CHECK_EQ(f.w, 640);
    CHECK_EQ(f.h, 480);
    CHECK(f.p8 != NULL && f.p32 == NULL);
    CHECK_EQ(f.p8[0], 0);
    fb_destroy(&f);
    CHECK(fb_init(&f, 16, 8, 32));
    CHECK(f.p32 != NULL && f.p8 == NULL);
    fb_destroy(&f);
    CHECK(!fb_init(&f, 16, 8, 16));
    CHECK(!fb_init(&f, 0, 8, 8));
}

static void test_palette(void) {
    fb_t f;
    printf("test_fb_palette\n");
    fb_init(&f, 4, 4, 8);
    CHECK_EQ(f.pal[0], 0x000000);
    CHECK_EQ(f.pal[255], 0xFFFFFF);
    CHECK_EQ(f.pal[7], 0xC0C0C0);          /* light gray */
    CHECK_EQ(f.pal[248], 0x808080);        /* dark gray */
    CHECK_EQ(f.pal[249], 0xFF0000);        /* red */
    CHECK_EQ(f.pal[252], 0x0000FF);        /* blue */
    CHECK_EQ(f.pal[1], 0x800000);
    /* exact colours map to their static entry; COLORREF is BGR */
    CHECK_EQ(fb_pixel_of(&f, FB_RGB(255, 255, 255)), 255);
    CHECK_EQ(fb_pixel_of(&f, FB_RGB(0, 0, 0)), 0);
    CHECK_EQ(fb_pixel_of(&f, FB_RGB(255, 0, 0)), 249);
    CHECK_EQ(fb_pixel_of(&f, FB_RGB(0, 0, 255)), 252);
    /* near colours pick the closest entry */
    CHECK_EQ(fb_pixel_of(&f, FB_RGB(250, 250, 250)), 255);
    CHECK_EQ(fb_pixel_of(&f, FB_RGB(3, 2, 1)), 0);
    CHECK_EQ(fb_pixel_of(&f, FB_RGB(190, 190, 190)), 7);
    fb_destroy(&f);
}

static void test_fill(void) {
    fb_t f;
    fb_rect r = {2, 1, 5, 3};
    int x, y, n = 0;
    printf("test_fb_fill\n");
    fb_init(&f, 8, 4, 8);
    CHECK_EQ(fb_fill_rect(&f, &r, FB_RGB(255, 0, 0)), 6);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 8; x++) n += f.p8[y * 8 + x] == 249;
    CHECK_EQ(n, 6);
    CHECK_EQ(f.p8[1 * 8 + 2], 249);
    CHECK_EQ(f.p8[3 * 8 + 2], 0);
    CHECK_EQ(fb_colorref_at(&f, 2, 1), FB_RGB(255, 0, 0));
    CHECK_EQ(fb_colorref_at(&f, 0, 0), 0);
    CHECK_EQ(fb_colorref_at(&f, 99, 0), 0);
    {   /* clipped to the buffer */
        fb_rect big = {-5, -5, 100, 100};
        CHECK_EQ(fb_fill_rect(&f, &big, FB_RGB(255, 255, 255)), 32);
        fb_rect none = {4, 4, 2, 2};
        CHECK_EQ(fb_fill_rect(&f, &none, 0), 0);
    }
    fb_put(&f, 1, 1, FB_RGB(0, 0, 0));
    CHECK_EQ(f.p8[1 * 8 + 1], 0);
    fb_put(&f, -1, 0, 0);          /* ignored */
    CHECK(f.dirty > 0);
    fb_destroy(&f);
}

static void test_argb(void) {
    fb_t f;
    uint32_t out[4];
    fb_rect r = {1, 0, 2, 1};
    printf("test_fb_argb\n");
    fb_init(&f, 2, 2, 8);
    fb_fill_rect(&f, &r, FB_RGB(0, 0, 255));
    fb_to_argb(&f, out);
    CHECK_EQ(out[0], 0xFF000000u);
    CHECK_EQ(out[1], 0xFF0000FFu);
    fb_destroy(&f);
    fb_init(&f, 2, 2, 32);
    fb_put(&f, 0, 1, FB_RGB(1, 2, 3));
    fb_to_argb(&f, out);
    CHECK_EQ(out[2], 0xFF010203u);
    CHECK_EQ(fb_colorref_at(&f, 0, 1), FB_RGB(1, 2, 3));
    CHECK_EQ(fb_pixel_of(&f, FB_RGB(1, 2, 3)), 0x010203u);
    fb_destroy(&f);
}

static void test_rects(void) {
    fb_rect a = {0, 0, 10, 10}, b = {5, 5, 20, 20}, c = {30, 30, 40, 40}, o;
    printf("test_fb_rects\n");
    CHECK(fb_rect_intersect(&o, &a, &b));
    CHECK(o.left == 5 && o.top == 5 && o.right == 10 && o.bottom == 10);
    CHECK(!fb_rect_intersect(&o, &a, &c));
    CHECK(fb_rect_empty(&o));
    fb_rect_union(&o, &a, &c);
    CHECK(o.left == 0 && o.top == 0 && o.right == 40 && o.bottom == 40);
    {
        fb_rect e = {3, 3, 3, 9};
        fb_rect_union(&o, &e, &b);          /* an empty rect is the identity */
        CHECK(o.left == 5 && o.right == 20);
        CHECK(fb_rect_empty(&e));
    }
}

static void test_ppm(void) {
    fb_t f;
    FILE *fp;
    unsigned char buf[64];
    size_t n;
    const char *path = "build/test_fb.ppm";
    printf("test_fb_ppm\n");
    fb_init(&f, 3, 2, 8);
    f.p8[0] = 249;                         /* red */
    f.p8[1] = 252;                         /* blue */
    f.p8[2] = 255;                         /* white */
    f.p8[5] = 7;                           /* light gray */
    CHECK(fb_save_ppm(&f, path));
    fp = fopen(path, "rb");
    CHECK(fp != NULL);
    n = fp ? fread(buf, 1, sizeof buf, fp) : 0;
    if (fp) fclose(fp);
    CHECK_EQ(n, 11 + 18);                  /* "P6\n3 2\n255\n" + 6 pixels */
    CHECK(n >= 11 && memcmp(buf, "P6\n3 2\n255\n", 11) == 0);
    CHECK(n >= 29 && buf[11] == 255 && buf[12] == 0 && buf[13] == 0);      /* red */
    CHECK(n >= 29 && buf[14] == 0 && buf[15] == 0 && buf[16] == 255);      /* blue */
    CHECK(n >= 29 && buf[17] == 255 && buf[18] == 255 && buf[19] == 255);  /* white */
    CHECK(n >= 29 && buf[20] == 0 && buf[21] == 0 && buf[22] == 0);        /* index 0 is black */
    CHECK(n >= 29 && buf[26] == 0xC0 && buf[27] == 0xC0 && buf[28] == 0xC0);
    f.pal[249] = 0x010203;                   /* a palette change shows in the file */
    CHECK(fb_save_ppm(&f, path));
    fp = fopen(path, "rb");
    n = fp ? fread(buf, 1, sizeof buf, fp) : 0;
    if (fp) fclose(fp);
    CHECK(n >= 14 && buf[11] == 1 && buf[12] == 2 && buf[13] == 3);
    CHECK(!fb_save_ppm(&f, "build/no/such/dir/x.ppm"));
    fb_destroy(&f);
    fb_init(&f, 2, 1, 32);
    f.p32[0] = 0x112233;
    f.p32[1] = 0xAABBCC;
    CHECK(fb_save_ppm(&f, path));
    fp = fopen(path, "rb");
    n = fp ? fread(buf, 1, sizeof buf, fp) : 0;
    if (fp) fclose(fp);
    CHECK_EQ(n, 11 + 6);
    CHECK(n >= 17 && buf[11] == 0x11 && buf[12] == 0x22 && buf[13] == 0x33 && buf[14] == 0xAA);
    fb_destroy(&f);
}

int main(void) {
    test_init();
    test_ppm();
    test_palette();
    test_fill();
    test_argb();
    test_rects();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
