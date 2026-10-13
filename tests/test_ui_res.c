/* Resource driven USER, resource side: LoadString, KEYBOARD translation, and later
 * bitmaps, icons, cursors and accelerators, through the HLE entry points. */
#include "gfixture.h"
#include "res.h"
#include "rfixture.h"
#include "kernel.h"
#include "ui.h"

#define KR(ord, ...) g_call("KEYBOARD", (ord), (const uint16_t[]){0, ##__VA_ARGS__} + 1, \
                            sizeof((const uint16_t[]){0, ##__VA_ARGS__}) / sizeof(uint16_t) - 1)

static void string_bundle(void) {
    rb_t b;
    int i;
    b.n = 0;
    rb_u8(&b, 5); rb_str(&b, "Hello"); b.n--;
    rb_u8(&b, 0);
    rb_u8(&b, 6); rb_str(&b, "World!"); b.n--;
    for (i = 3; i < 16; i++) rb_u8(&b, 0);
    rf_add(&uf, RT_STRING, NULL, 2, NULL, b.b, b.n);
}

static void test_loadstring(void) {
    uint16_t buf, inst;
    char *s;
    printf("test_ui_loadstring\n");
    gboot();
    buf = g_seg(0x100);
    string_bundle();
    inst = u_ds();
    s = (char *)umem(buf, 0, 0x100);
    memset(s, 'x', 0x100);
    CHECK_EQ(UA(176, inst, 16, FARP(buf, 0), 0x100), 5);
    CHECK(!strcmp(s, "Hello"));
    CHECK_EQ(UA(176, inst, 18, FARP(buf, 0), 0x100), 6);
    CHECK(!strcmp(s, "World!"));
    CHECK_EQ(UA(176, inst, 18, FARP(buf, 0), 4), 3);          /* room for 3 characters and the NUL */
    CHECK(!strcmp(s, "Wor"));
    CHECK_EQ(UA(176, inst, 17, FARP(buf, 0), 0x100), 0);      /* an empty slot */
    CHECK_EQ(s[0], 0);
    CHECK_EQ(UA(176, inst, 99, FARP(buf, 0), 0x100), 0);      /* no such string */
    CHECK_EQ(UA(176, inst, 16, FARP(buf, 0), 0), 0);          /* no room */
    CHECK_EQ(UA(176, 0x7777, 16, FARP(buf, 0), 0x100), 0);    /* another module */
    gdown();
}

static void test_keyboard(void) {
    uint16_t a, b;
    char *s, *d;
    printf("test_ui_keyboard\n");
    gboot();
    a = g_seg(0x100);
    b = g_seg(0x100);
    s = (char *)umem(a, 0, 0x100);
    d = (char *)umem(b, 0, 0x100);
    strcpy(s, "Caf\xE9 \xF1 ok");
    CHECK(g_lo(KR(5, FARP(a, 0), FARP(b, 0))) != 0);          /* AnsiToOem */
    CHECK(!memcmp(d, "Caf\x82 \xA4 ok", 9));
    CHECK_EQ(d[9], 0);
    strcpy(s, "\x82\x81 \xA4");
    KR(6, FARP(a, 0), FARP(b, 0));                            /* OemToAnsi */
    CHECK(!strcmp(d, "\xE9\xFC \xF1"));
    strcpy(s, "plain \xE9");                                  /* in place */
    KR(5, FARP(a, 0), FARP(a, 0));
    CHECK(!strcmp(s, "plain \x82"));
    strcpy(s, "\xE9\xF1x!");
    memset(d, 0, 8);
    KR(137, FARP(a, 0), FARP(b, 0), 3);                       /* AnsiToOemBuff: three bytes */
    CHECK(!memcmp(d, "\x82\xA4x", 3) && d[3] == 0);
    KR(138, FARP(b, 0), FARP(a, 0), 3);                       /* OemToAnsiBuff */
    CHECK(!memcmp(s, "\xE9\xF1x!", 4));
    CHECK_EQ(g_lo(KR(135)), 437);                             /* GetKBCodePage */
    gdown();
}

/* ---- bitmaps, icons and cursors ---------------------------------------------------------------- */

static void bih(rb_t *b, int w, int h, int bpp, int ncol) {
    rb_u32(b, 40); rb_u32(b, (uint32_t)w); rb_u32(b, (uint32_t)h);
    rb_u16(b, 1); rb_u16(b, (unsigned)bpp);
    rb_u32(b, 0); rb_u32(b, 0); rb_u32(b, 0); rb_u32(b, 0); rb_u32(b, (uint32_t)ncol); rb_u32(b, 0);
}
static void quad(rb_t *b, int r, int g, int bl) { rb_u8(b, bl); rb_u8(b, g); rb_u8(b, r); rb_u8(b, 0); }

static uint32_t mem_pixel(uint16_t hdc, int x, int y) { return GR(83, hdc, (uint16_t)x, (uint16_t)y); }

static void test_loadbitmap(void) {
    rb_t b;
    uint16_t nm, hbm, hdc, obj, hbm2;
    printf("test_ui_loadbitmap\n");
    gboot();
    b.n = 0;
    bih(&b, 4, 2, 8, 4);
    quad(&b, 0, 0, 0); quad(&b, 255, 0, 0); quad(&b, 0, 255, 0); quad(&b, 0, 0, 255);
    rb_u8(&b, 0); rb_u8(&b, 3); rb_u8(&b, 2); rb_u8(&b, 1);    /* bottom row */
    rb_u8(&b, 1); rb_u8(&b, 2); rb_u8(&b, 3); rb_u8(&b, 0);    /* top row */
    rf_add(&uf, RT_BITMAP, NULL, 0, "LOGO", b.b, b.n);
    rf_add(&uf, RT_BITMAP, NULL, 5, NULL, b.b, b.n);
    nm = ustr("logo");
    hbm = UA(175, u_ds(), nm, 0);                               /* LoadBitmap(hInst, "logo") */
    CHECK(hbm != 0);
    obj = g_seg(32);
    CHECK_EQ(GA(82, hbm, 14, FARP(obj, 0)), 14);                 /* GetObject */
    CHECK_EQ(urd16(obj, 2), 4);
    CHECK_EQ(urd16(obj, 4), 2);
    hdc = GA(52, 0);
    GA(45, hdc, hbm);
    CHECK_EQ(mem_pixel(hdc, 0, 0), RGB(255, 0, 0));
    CHECK_EQ(mem_pixel(hdc, 1, 0), RGB(0, 255, 0));
    CHECK_EQ(mem_pixel(hdc, 2, 0), RGB(0, 0, 255));
    CHECK_EQ(mem_pixel(hdc, 3, 0), RGB(0, 0, 0));
    CHECK_EQ(mem_pixel(hdc, 1, 1), RGB(0, 0, 255));
    CHECK_EQ(mem_pixel(hdc, 3, 1), RGB(255, 0, 0));
    hbm2 = UA(175, u_ds(), 0, 5);                                /* by MAKEINTRESOURCE(5) */
    CHECK(hbm2 != 0 && hbm2 != hbm);                            /* a new bitmap per load */
    CHECK_EQ(GA(69, hbm2), 1);                                   /* DeleteObject */
    CHECK_EQ(UA(175, u_ds(), 0, 77), 0);                         /* no such bitmap */
    nm = ustr("nothing");
    CHECK_EQ(UA(175, u_ds(), nm, 0), 0);
    CHECK_EQ(UA(175, 0x7777, 0, 5), 0);                          /* another module */
    /* 1 bpp bitmaps stay monochrome */
    b.n = 0;
    bih(&b, 8, 1, 1, 2);
    quad(&b, 0, 0, 0); quad(&b, 255, 255, 255);
    rb_u8(&b, 0x80); rb_u8(&b, 0); rb_u16(&b, 0);
    rf_add(&uf, RT_BITMAP, NULL, 6, NULL, b.b, b.n);
    hbm2 = UA(175, u_ds(), 0, 6);
    CHECK(hbm2 != 0);
    CHECK_EQ(GA(82, hbm2, 14, FARP(obj, 0)), 14);
    CHECK_EQ(umem(obj, 9, 1)[0], 1);                             /* bmBitsPixel */
    gdown();
}

#define IDI_APPLICATION 32512
#define IDC_ARROW 32512
#define IDC_IBEAM 32513

static void icon_resources(void) {
    rb_t b, g;
    int y, x;
    b.n = 0;
    bih(&b, 32, 64, 8, 2);
    quad(&b, 0, 0, 0); quad(&b, 255, 0, 0);
    for (y = 0; y < 32; y++) for (x = 0; x < 32; x++) rb_u8(&b, x < 16 ? 0 : 1);    /* XOR: right half red */
    for (y = 0; y < 32; y++) { rb_u8(&b, 0xFF); rb_u8(&b, 0xFF); rb_u8(&b, 0); rb_u8(&b, 0); }  /* AND: left half transparent */
    rf_add(&uf, RT_ICON, NULL, 11, NULL, b.b, b.n);
    g.n = 0;
    rb_u16(&g, 0); rb_u16(&g, 1); rb_u16(&g, 1);
    rb_u8(&g, 32); rb_u8(&g, 32); rb_u8(&g, 0); rb_u8(&g, 0); rb_u16(&g, 1); rb_u16(&g, 8); rb_u32(&g, (uint32_t)b.n); rb_u16(&g, 11);
    rf_add(&uf, RT_GROUP_ICON, NULL, 0, "APP", g.b, g.n);
    /* a cursor: 8x8, 1 bpp, hotspot (3,4) */
    b.n = 0;
    rb_u16(&b, 3); rb_u16(&b, 4);
    bih(&b, 8, 16, 1, 2);
    quad(&b, 0, 0, 0); quad(&b, 255, 255, 255);
    for (y = 0; y < 8; y++) { rb_u8(&b, 0x0F); rb_u8(&b, 0); rb_u16(&b, 0); }
    for (y = 0; y < 8; y++) { rb_u8(&b, 0xFF); rb_u8(&b, 0); rb_u16(&b, 0); }
    rf_add(&uf, RT_CURSOR, NULL, 21, NULL, b.b, b.n);
    g.n = 0;
    rb_u16(&g, 0); rb_u16(&g, 2); rb_u16(&g, 1);
    rb_u16(&g, 8); rb_u16(&g, 16); rb_u16(&g, 1); rb_u16(&g, 1); rb_u32(&g, (uint32_t)b.n - 4); rb_u16(&g, 21);
    rf_add(&uf, RT_GROUP_CURSOR, NULL, 0, "HAND", g.b, g.n);
}

static void test_icons(void) {
    uint16_t nm, h, h2, dc;
    int differs = 0, x, y;
    printf("test_ui_icons\n");
    gboot();
    icon_resources();
    nm = ustr("App");
    h = UA(174, u_ds(), nm, 0);                                  /* LoadIcon */
    CHECK(h != 0);
    h2 = UA(174, u_ds(), nm, 0);
    CHECK_EQ(h2, h);                                             /* cached per resource */
    dc = g_screen_dc();
    g_clear(7);
    CHECK_EQ(UA(84, dc, 10, 20, h), 1);                          /* DrawIcon */
    CHECK_EQ(g_px(10 + 20, 20 + 5), 249);                        /* opaque: red */
    CHECK_EQ(g_px(10 + 2, 20 + 5), 7);                           /* transparent: untouched */
    CHECK_EQ(g_px(10 + 31, 20 + 31), 249);
    CHECK_EQ(g_px(10 + 32, 20 + 5), 7);
    /* DrawIcon takes logical coordinates: the program's offscreen DCs have a viewport origin (the KIDSWTCH switcher
     * draws the other program's icon at 57,16 of such a DC and it used to land at the wrong place, so no icon showed) */
    g_clear(7);
    GA(13, dc, 30, 40);                                          /* SetViewportOrg */
    CHECK_EQ(UA(84, dc, 10, 20, h), 1);
    CHECK_EQ(g_px(30 + 10 + 20, 40 + 20 + 5), 249);              /* drawn at the mapped place */
    CHECK_EQ(g_px(10 + 20, 20 + 5), 7);                          /* and not at the raw coordinates */
    GA(13, dc, 0, 0);
    CHECK_EQ(UA(457, h), 1);                                     /* DestroyIcon */
    g_clear(7);
    UA(84, dc, 10, 20, h);
    CHECK_EQ(g_px(10 + 20, 20 + 5), 7);                          /* a dead handle draws nothing */
    nm = ustr("nothing");
    CHECK_EQ(UA(174, u_ds(), nm, 0), 0);
    h = UA(174, 0, 0, IDI_APPLICATION);                          /* system icon */
    CHECK(h != 0);
    g_clear(7);
    UA(84, dc, 100, 100, h);
    for (y = 100; y < 132; y++) for (x = 100; x < 132; x++) differs += g_px(x, y) != 7;
    CHECK(differs > 50);
    CHECK_EQ(UA(174, 0, 0, 5555), 0);
    UA(68, 0, dc);
    gdown();
}

static void test_cursors(void) {
    uint16_t nm, h, ibeam, old;
    int w, hh, hx, hy;
    printf("test_ui_cursors\n");
    gboot();
    icon_resources();
    nm = ustr("hand");
    h = UA(173, u_ds(), nm, 0);                                  /* LoadCursor */
    CHECK(h != 0);
    CHECK_EQ(UA(173, u_ds(), nm, 0), h);
    CHECK(ui_cursor_info(user_ui(uu), h, &w, &hh, &hx, &hy));
    CHECK(w == 8 && hh == 8 && hx == 3 && hy == 4);
    old = UA(247);                                               /* GetCursor */
    CHECK_EQ(UA(69, h), old);                                    /* SetCursor returns the previous one */
    CHECK_EQ(UA(247), h);
    ibeam = UA(173, 0, 0, IDC_IBEAM);
    CHECK(ibeam != 0 && ibeam != h);
    CHECK(ui_cursor_info(user_ui(uu), ibeam, &w, &hh, &hx, &hy));
    CHECK(w == 32 && hh == 32);
    CHECK(UA(173, 0, 0, IDC_ARROW) != 0 && UA(173, 0, 0, IDC_ARROW) != ibeam);
    CHECK_EQ(UA(458, h), 1);                                     /* DestroyCursor */
    CHECK(!ui_cursor_info(user_ui(uu), h, &w, &hh, &hx, &hy));
    CHECK_EQ(UA(173, 0, 0, 7777), 0);
    gdown();
}

/* A Win16 HCURSOR is a global memory handle: programs GlobalLock it and read the CURSORSHAPE (hot spot, size,
 * planes/bits per pixel, then the AND mask and the XOR mask). Fine Artist locks the cursor of its stamp tools to
 * make a bitmap from the mask; with an opaque handle GlobalLock returned 0 and it reported "trouble finding space
 * in memory or on your hard disk". */
#define KRN(ord, ...) g_call("KERNEL", (ord), (const uint16_t[]){0, ##__VA_ARGS__} + 1, \
                             sizeof((const uint16_t[]){0, ##__VA_ARGS__}) / sizeof(uint16_t) - 1)

static void test_cursor_shape(void) {
    kernel_config kc;
    kernel_t *kk;
    uint16_t nm, h, sel;
    uint32_t p;
    const uint8_t *s;
    int w, hh, hx, hy;
    printf("test_ui_cursor_shape\n");
    gboot();
    memset(&kc, 0, sizeof kc);
    kk = kernel_create(&kc, &uh, 0);
    CHECK(kk != NULL);
    if (kk) CHECK(kernel_attach(kk, uf.p));
    icon_resources();
    nm = ustr("hand");
    h = UA(173, u_ds(), nm, 0);                                  /* LoadCursor */
    CHECK(h != 0);
    p = KRN(18, h);                                              /* GlobalLock(hCursor) */
    sel = (uint16_t)(p >> 16);
    CHECK(sel != 0);
    s = sel ? (const uint8_t *)umem(sel, 0, 12 + 16) : NULL;
    if (s) {
        CHECK_EQ(s[0] | s[1] << 8, 3);                           /* xHotSpot */
        CHECK_EQ(s[2] | s[3] << 8, 4);                           /* yHotSpot */
        CHECK_EQ(s[4] | s[5] << 8, 8);                           /* cx */
        CHECK_EQ(s[6] | s[7] << 8, 8);                           /* cy */
        CHECK_EQ(s[8] | s[9] << 8, 1);                           /* cbWidth */
        CHECK_EQ(s[10], 1);                                      /* Planes */
        CHECK_EQ(s[11], 1);                                      /* BitsPixel */
        CHECK_EQ(s[12], 0xFF);                                   /* AND mask, first row (1 = transparent) */
        CHECK_EQ(s[12 + 7], 0xFF);
        CHECK_EQ(s[12 + 8], 0x0F);                               /* XOR mask, first row */
        CHECK_EQ(s[12 + 15], 0x0F);
    }
    CHECK_EQ(UA(173, u_ds(), nm, 0), h);                         /* still cached: one handle */
    CHECK(ui_cursor_info(user_ui(uu), h, &w, &hh, &hx, &hy));    /* and still a cursor for SetCursor */
    CHECK(w == 8 && hh == 8 && hx == 3 && hy == 4);
    CHECK_EQ(UA(69, h) != h, 1);
    CHECK_EQ(UA(247), h);
    h = UA(173, 0, 0, IDC_IBEAM);                                /* the system cursors are locked the same way */
    p = KRN(18, h);
    CHECK((uint16_t)(p >> 16) != 0);
    CHECK_EQ(UA(458, h), 1);
    kernel_destroy(kk);
    gdown();
}

/* ---- accelerators ---------------------------------------------------------------------------------- */

#define WM_KEYDOWN_ 0x100
#define WM_CHAR_ 0x102
#define WM_SYSKEYDOWN_ 0x104
#define WM_INITMENU_ 0x116
#define WM_INITMENUPOPUP_ 0x117

static void key(wm_in_kind k, unsigned vk) {
    wm_input in;
    memset(&in, 0, sizeof in);
    in.kind = k;
    in.vk = vk;
    wm_input_event(user_wm(uu), &in);
}

/* TranslateAccelerator(hwnd, haccel, &msg) with a MSG built in guest memory */
static uint16_t translate(uint16_t hwnd, uint16_t acc, uint16_t msg, uint16_t wp) {
    uint16_t m = g_seg(32);
    uwr16(m, 0, hwnd);
    uwr16(m, 2, msg);
    uwr16(m, 4, wp);
    uwr32(m, 6, 0x00010001u);
    return UA(178, hwnd, acc, FARP(m, 0));
}

static void accel_blob(rb_t *b) {
    b->n = 0;
    rb_u8(b, AF_VIRTKEY | AF_CONTROL); rb_u16(b, 'N'); rb_u16(b, 100);
    rb_u8(b, AF_VIRTKEY); rb_u16(b, 0x70); rb_u16(b, 101);
    rb_u8(b, 0); rb_u16(b, 'x'); rb_u16(b, 102);
    rb_u8(b, AF_VIRTKEY | AF_ALT | AF_LAST); rb_u16(b, 0x7A); rb_u16(b, 103);
}

static void test_accelerators(void) {
    rb_t b;
    uint16_t acc, nm, hw, bar, pop;
    int i;
    printf("test_ui_accelerators\n");
    gboot();
    accel_blob(&b);
    rf_add(&uf, RT_ACCELERATOR, NULL, 0, "ACC", b.b, b.n);
    nm = ustr("acc");
    acc = UA(177, u_ds(), nm, 0);                                  /* LoadAccelerators */
    CHECK(acc != 0);
    CHECK_EQ(UA(177, u_ds(), nm, 0), acc);                         /* the same table, the same handle */
    CHECK_EQ(UA(177, u_ds(), ustr("none"), 0), 0);
    CHECK_EQ(UA(177, 0x7777, nm, 0), 0);
    u_register("ACCW", u_far(U_CODE_A), 0, 0, 0, u_ds(), 6);
    hw = u_create("ACCW", "a", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0, 0, u_ds());
    CHECK(hw != 0);
    ulog_reset();
    /* Ctrl+N */
    CHECK_EQ(translate(hw, acc, WM_KEYDOWN_, 'N'), 0);              /* Ctrl not down */
    CHECK_EQ(ulog_n(), 0);
    key(WM_IN_KEY_DOWN, 0x11);
    CHECK_EQ(translate(hw, acc, WM_KEYDOWN_, 'N'), 1);
    CHECK_EQ(ulog_n(), 1);
    CHECK_EQ(ulog_w(0, ULOG_MSG), WM_COMMAND);
    CHECK_EQ(ulog_w(0, ULOG_WP), 100);
    CHECK_EQ(ulog_lp(0), 0x00010000u);                             /* HIWORD 1: from an accelerator */
    key(WM_IN_KEY_DOWN, 0x10);
    CHECK_EQ(translate(hw, acc, WM_KEYDOWN_, 'N'), 0);              /* Shift too: the modifiers must match exactly */
    key(WM_IN_KEY_UP, 0x10);
    key(WM_IN_KEY_UP, 0x11);
    CHECK_EQ(translate(hw, acc, WM_KEYDOWN_, 0x70), 1);             /* F1 */
    CHECK_EQ(ulog_w(1, ULOG_WP), 101);
    CHECK_EQ(translate(hw, acc, WM_CHAR_, 'x'), 1);                 /* an ASCII entry matches WM_CHAR */
    CHECK_EQ(ulog_w(2, ULOG_WP), 102);
    CHECK_EQ(translate(hw, acc, WM_CHAR_, 'X'), 0);
    CHECK_EQ(translate(hw, acc, WM_KEYDOWN_, 'x'), 0);              /* ... and nothing else */
    key(WM_IN_KEY_DOWN, 0x12);
    CHECK_EQ(translate(hw, acc, WM_SYSKEYDOWN_, 0x7A), 1);          /* Alt+F11 */
    CHECK_EQ(ulog_w(3, ULOG_WP), 103);
    key(WM_IN_KEY_UP, 0x12);
    CHECK_EQ(translate(hw, acc, WM_SYSKEYDOWN_, 0x7A), 0);
    CHECK_EQ(translate(hw, acc, 0x200, 0x70), 0);                   /* mouse messages are not keys */
    CHECK_EQ(translate(hw, 0x7777, WM_KEYDOWN_, 0x70), 0);          /* a bad table */
    CHECK_EQ(translate(0x7777, acc, WM_KEYDOWN_, 0x70), 0);         /* a bad window */
    /* a window with a menu: the owner gets WM_INITMENU and WM_INITMENUPOPUP before the command, and a
     * disabled item swallows the key without a command */
    bar = UA(151);
    pop = UA(415);
    {
        uint16_t t = ustr("&New");
        UA(411, pop, 0, 100, FARP(t, 0));
        t = ustr("File");
        UA(411, bar, 0x10, pop, FARP(t, 0));
    }
    hw = u_create("ACCW", "m", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0, bar, u_ds());
    ulog_reset();
    key(WM_IN_KEY_DOWN, 0x11);
    CHECK_EQ(translate(hw, acc, WM_KEYDOWN_, 'N'), 1);
    CHECK_EQ(ulog_n(), 3);
    CHECK_EQ(ulog_w(0, ULOG_MSG), WM_INITMENU_);
    CHECK_EQ(ulog_w(0, ULOG_WP), bar);
    CHECK_EQ(ulog_w(1, ULOG_MSG), WM_INITMENUPOPUP_);
    CHECK_EQ(ulog_w(1, ULOG_WP), pop);
    CHECK_EQ(ulog_w(2, ULOG_MSG), WM_COMMAND);
    UA(155, pop, 100, 1);                                            /* gray the item */
    ulog_reset();
    CHECK_EQ(translate(hw, acc, WM_KEYDOWN_, 'N'), 1);               /* still an accelerator ... */
    for (i = 0; i < (int)ulog_n(); i++) CHECK(ulog_w((unsigned)i, ULOG_MSG) != WM_COMMAND);   /* ... but no command */
    key(WM_IN_KEY_UP, 0x11);
    gdown();
}

int main(void) {
    test_accelerators();
    test_loadstring();
    test_keyboard();
    test_loadbitmap();
    test_icons();
    test_cursors();
    test_cursor_shape();
    printf("test_ui_res: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
