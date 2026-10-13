/* The non-client area: caption bar, frame, system box, minimize/maximize buttons, menu bar, WM_NCCALCSIZE,
 * WM_NCHITTEST and WM_SYSCOMMAND, checked on pixels of the framebuffer. */
#include "gfixture.h"

#define U_CREATEMENU 151
#define U_APPENDMENU 411

enum { HTCLIENT_ = 1, HTCAPTION_ = 2, HTSYSMENU_ = 3, HTMENU_ = 5, HTREDUCE_ = 8, HTZOOM_ = 9, HTBORDER_ = 18 };

static uint32_t px(int x, int y) { return fb_colorref_at(user_fb(uu), x, y); }

static uint16_t add_item(uint16_t m, unsigned flags, unsigned id, const char *text) {
    uint16_t s = ustr(text);
    return UA(U_APPENDMENU, m, (uint16_t)flags, (uint16_t)id, FARP(s, 0));
}

static int count_color(int l, int t, int r, int b, uint32_t col) {
    int x, y, n = 0;
    for (y = t; y < b; y++)
        for (x = l; x < r; x++) n += px(x, y) == col;
    return n;
}

#define WX 50
#define WY 60
#define WW 300
#define WH 200
#define F 5    /* sizing frame (4) plus the window border (1) */

static uint16_t make_window(uint16_t bar, uint32_t extra) {
    return u_create("NC", "Hello", WS_OVERLAPPEDWINDOW | extra, WX, WY, WW, WH, 0, bar, u_ds());
}

static uint16_t nc_hit(uint16_t hw, int x, int y) {
    return UA(107, hw, 0x84, 0, LONG((uint32_t)(uint16_t)y << 16 | (uint16_t)x));
}

static void test_caption_and_bar(void) {
    uint16_t bar, hw, hw2;
    printf("test_nc_paint\n");
    gboot();
    u_register("NC", 0, 0, 0, 0, u_ds(), 6);
    bar = UA(U_CREATEMENU);
    add_item(bar, 0, 100, "One");
    add_item(bar, 0, 101, "Two");
    g_clear(77);
    hw = make_window(bar, WS_VISIBLE);
    CHECK(hw != 0);
    g_clear(77);
    UCALL(111, hw, 0x85, 1, LONG(0));                                      /* WM_NCPAINT */
    CHECK_EQ(px(WX, WY), wm_sys_color(COLOR_WINDOWFRAME));                 /* outer line */
    CHECK_EQ(px(WX + 1, WY + 40), wm_sys_color(20));                       /* white highlight inside the frame */
    CHECK_EQ(px(WX + WW - 2, WY + 40), wm_sys_color(16));                  /* dark shadow */
    CHECK_EQ(px(WX + F + 60, WY + F + 3), wm_sys_color(COLOR_ACTIVECAPTION));
    CHECK_EQ(px(WX + F + 3, WY + F + 7), wm_sys_color(COLOR_WINDOWFRAME)); /* system box: dash outline */
    CHECK_EQ(px(WX + F + 8, WY + F + 8), wm_sys_color(COLOR_WINDOW));      /* ... and its white inside */
    CHECK_EQ(px(WX + F + 60, WY + F + 18), wm_sys_color(COLOR_WINDOWFRAME)); /* line under the caption */
    CHECK(count_color(WX + F + 20, WY + F, WX + WW - F - 40, WY + F + 18, wm_sys_color(9)) > 20);   /* title text */
    CHECK_EQ(wm_sys_color(4), 0xFFFFFFu);                                  /* Windows 3.1 menus are white */
    CHECK_EQ(px(WX + F + 250, WY + F + 19 + 5), wm_sys_color(4));          /* menu bar background */
    CHECK_EQ(px(WX + F + 250, WY + F + 19 + 19), wm_sys_color(COLOR_WINDOWFRAME));   /* line under the bar */
    CHECK(count_color(WX + F + 2, WY + F + 19, WX + F + 80, WY + F + 19 + 19, wm_sys_color(7)) > 15);   /* "One" */
    CHECK(count_color(WX + WW - F - 36, WY + F, WX + WW - F, WY + F + 18, wm_sys_color(COLOR_WINDOWFRAME)) > 10);   /* arrows */
    /* the client area is not touched */
    CHECK_EQ(g_px(WX + F + 20, WY + F + 19 + 20 + 20), 77);
    CHECK_EQ(g_px(WX + WW - F - 1 - 1, WY + WH - F - 3), 77);
    /* an inactive window: white caption, black text */
    hw2 = u_create("NC", "Other", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 400, 300, 200, 100, 0, 0, u_ds());
    CHECK(hw2 != 0);
    UCALL(111, hw, 0x85, 1, LONG(0));
    CHECK_EQ(px(WX + F + 60, WY + F + 3), wm_sys_color(3));
    CHECK(count_color(WX + F + 20, WY + F, WX + WW - F - 40, WY + F + 18, wm_sys_color(19)) > 20);
    gdown();
}

static void test_hit_test(void) {
    uint16_t bar, hw;
    printf("test_nc_hit_test\n");
    gboot();
    u_register("NC", 0, 0, 0, 0, u_ds(), 6);
    bar = UA(U_CREATEMENU);
    add_item(bar, 0, 100, "One");
    hw = make_window(bar, 0);
    CHECK_EQ(nc_hit(hw, WX + F + 60, WY + F + 5), HTCAPTION_);
    CHECK_EQ(nc_hit(hw, WX + F + 5, WY + F + 5), HTSYSMENU_);
    CHECK_EQ(nc_hit(hw, WX + F + 100, WY + F + 19 + 5), HTMENU_);
    CHECK_EQ(nc_hit(hw, WX + WW - F - 9, WY + F + 5), HTZOOM_);
    CHECK_EQ(nc_hit(hw, WX + WW - F - 27, WY + F + 5), HTREDUCE_);
    CHECK_EQ(nc_hit(hw, WX + F + 10, WY + F + 19 + 20 + 10), HTCLIENT_);
    CHECK_EQ(nc_hit(hw, WX + 1, WY + 100), HTBORDER_);
    CHECK_EQ(nc_hit(hw, WX + 150, WY + WH - 1), HTBORDER_);
    CHECK_EQ(nc_hit(hw, WX - 5, WY + 10), 0);
    gdown();
}

static void test_nccalcsize(void) {
    uint16_t bar, hw, rs;
    printf("test_nc_calcsize\n");
    gboot();
    u_register("NC", 0, 0, 0, 0, u_ds(), 6);
    bar = UA(U_CREATEMENU);
    add_item(bar, 0, 100, "One");
    hw = make_window(bar, 0);
    rs = g_seg(16);
    uwr16(rs, 0, 0); uwr16(rs, 2, 0); uwr16(rs, 4, WW); uwr16(rs, 6, WH);
    UCALL(111, hw, 0x83, 0, LONG((uint32_t)rs << 16));                       /* WM_NCCALCSIZE, wParam 0 */
    CHECK_EQ(urd16(rs, 0), F);
    CHECK_EQ(urd16(rs, 2), F + 19 + 20);
    CHECK_EQ(urd16(rs, 4), WW - F);
    CHECK_EQ(urd16(rs, 6), WH - F);
    /* sent during creation, between WM_NCCREATE and WM_CREATE, with the window rectangle */
    {
        uint32_t wp = u_far(U_CODE_A);
        uint16_t cs = useg(0x100), ns = ustr("REC");
        int a, b, c;
        uwr16(cs, 0, 0); uwr32(cs, 2, wp); uwr16(cs, 6, 0); uwr16(cs, 8, 0); uwr16(cs, 10, u_ds());
        uwr16(cs, 12, 0); uwr16(cs, 14, 0); uwr16(cs, 16, 6); uwr32(cs, 18, 0); uwr32(cs, 22, (uint32_t)ns << 16);
        UCALL(57, FARP(cs, 0));
        ulog_reset();
        CHECK(u_create("REC", "r", WS_OVERLAPPEDWINDOW, 10, 20, 200, 100, 0, 0, u_ds()) != 0);
        a = ulog_find(0x81, 0, 0);
        b = ulog_find(0x83, 0, 0);
        c = ulog_find(0x01, 0, 0);
        CHECK(a >= 0 && b > a && c > b);
        CHECK_EQ(ulog_w((unsigned)b, ULOG_WP), 0);
    }
    gdown();
}

static void test_sys_commands(void) {
    uint16_t hw;
    printf("test_nc_syscommands\n");
    gboot();
    u_register("NC", 0, 0, 0, 0, u_ds(), 6);
    hw = make_window(0, WS_VISIBLE);
    UA(107, hw, 0x112, 0xF030, LONG(0));                                      /* SC_MAXIMIZE */
    CHECK(wm_get(user_wm(uu), hw)->style & WS_MAXIMIZE);
    UA(107, hw, 0x112, 0xF120, LONG(0));                                      /* SC_RESTORE */
    CHECK(!(wm_get(user_wm(uu), hw)->style & (WS_MAXIMIZE | WS_MINIMIZE)));
    UA(107, hw, 0x112, 0xF020, LONG(0));                                      /* SC_MINIMIZE */
    CHECK(wm_get(user_wm(uu), hw)->style & WS_MINIMIZE);
    UA(107, hw, 0x112, 0xF120, LONG(0));
    /* the buttons of the caption do the same through WM_NCLBUTTONDOWN */
    UCALL(111, hw, 0xA1, HTZOOM_, LONG((uint32_t)(WY + F + 5) << 16 | (uint16_t)(WX + WW - F - 9)));
    CHECK(wm_get(user_wm(uu), hw)->style & WS_MAXIMIZE);
    UCALL(111, hw, 0xA1, HTZOOM_, LONG(5 << 16 | 600));                        /* the restore button */
    CHECK(!(wm_get(user_wm(uu), hw)->style & WS_MAXIMIZE));
    UCALL(111, hw, 0xA1, HTREDUCE_, LONG(0));
    CHECK(wm_get(user_wm(uu), hw)->style & WS_MINIMIZE);
    UA(107, hw, 0x112, 0xF120, LONG(0));
    UA(107, hw, 0x112, 0xF060, LONG(0));                                      /* SC_CLOSE */
    CHECK(!wm_is_window(user_wm(uu), hw));
    gdown();
}

int main(void) {
    test_caption_and_bar();
    test_hit_test();
    test_nccalcsize();
    test_sys_commands();
    printf("test_ui_nc: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
