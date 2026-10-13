/* The clipboard, carets, InvalidateRgn / ValidateRgn / GetWindowDC and the COMMDLG answers. */
#include "gfixture.h"
#include "kernel.h"

#define KR(ord, ...) g_call("KERNEL", (ord), (const uint16_t[]){0, ##__VA_ARGS__} + 1, \
                            sizeof((const uint16_t[]){0, ##__VA_ARGS__}) / sizeof(uint16_t) - 1)
#define CR(ord, ...) g_call("COMMDLG", (ord), (const uint16_t[]){0, ##__VA_ARGS__} + 1, \
                            sizeof((const uint16_t[]){0, ##__VA_ARGS__}) / sizeof(uint16_t) - 1)

enum {
    U_OPEN = 137, U_CLOSE = 138, U_EMPTY = 139, U_GETOWNER = 140, U_SETDATA = 141, U_GETDATA = 142, U_COUNT = 143,
    U_ENUM = 144, U_REGISTER = 145, U_FORMATNAME = 146, U_SETVIEWER = 147, U_GETVIEWER = 148, U_CHANGECHAIN = 149,
    U_ISAVAIL = 193, U_CREATECARET = 163, U_DESTROYCARET = 164, U_SETCARETPOS = 165, U_HIDECARET = 166,
    U_SHOWCARET = 167, U_GETCARETPOS = 183, U_INVALIDATERGN = 126, U_VALIDATERGN = 128, U_GETWINDOWDC = 67
};
#define CF_TEXT 1
#define CF_BITMAP 2
#define WM_DESTROYCLIPBOARD 0x307
#define WM_RENDERFORMAT 0x305
#define WM_DRAWCLIPBOARD 0x308

static kernel_t *kk;

static void boot(void) {
    kernel_config kc;
    gboot();
    memset(&kc, 0, sizeof kc);
    kk = kernel_create(&kc, &uh, 0);
    CHECK(kk != NULL);
    if (kk) CHECK(kernel_attach(kk, uf.p));
}

static void down(void) {
    kernel_destroy(kk);
    kk = NULL;
    gdown();
}

static uint16_t gmem(const char *text) {
    uint16_t h = (uint16_t)KR(15, 0x2002, LONG(strlen(text) + 1)), sel;
    uint32_t p;
    CHECK(h != 0);
    p = KR(18, h);                                  /* GlobalLock */
    sel = (uint16_t)(p >> 16);
    strcpy((char *)umem(sel, 0, 1), text);
    return h;
}

static const char *gtext(uint16_t h) {
    uint32_t p = KR(18, h);
    return (const char *)umem((uint16_t)(p >> 16), (uint16_t)p, 1);
}

static void test_clipboard(void) {
    uint16_t a, b, h, h2, buf;
    int i;
    printf("test_misc_clipboard\n");
    boot();
    u_register("CBW", u_far(U_CODE_A), 0, 0, 0, u_ds(), 6);
    a = u_create("CBW", "a", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0, 0, u_ds());
    b = u_create("CBW", "b", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0, 0, u_ds());
    CHECK_EQ(UA(U_ISAVAIL, CF_TEXT), 0);
    CHECK_EQ(UA(U_GETDATA, CF_TEXT), 0);                        /* not open */
    CHECK_EQ(UA(U_EMPTY), 0);
    CHECK_EQ(UA(U_OPEN, a), 1);
    CHECK_EQ(UA(U_OPEN, b), 0);                                 /* open by another window */
    CHECK_EQ(UA(U_EMPTY), 1);
    CHECK_EQ(UA(U_GETOWNER), a);
    h = gmem("Hi there");
    CHECK_EQ(UA(U_SETDATA, CF_TEXT, h), h);
    CHECK_EQ(UA(U_ISAVAIL, CF_TEXT), 1);
    CHECK_EQ(UA(U_ISAVAIL, CF_BITMAP), 0);
    CHECK_EQ(UA(U_COUNT), 1);
    CHECK_EQ(UA(U_ENUM, 0), CF_TEXT);
    CHECK_EQ(UA(U_ENUM, CF_TEXT), 0);
    h2 = UA(U_GETDATA, CF_TEXT);
    CHECK(h2 != 0);
    CHECK(!strcmp(gtext(h2), "Hi there"));
    CHECK_EQ(UA(U_GETDATA, CF_TEXT), h2);                       /* the same block until the clipboard is emptied */
    CHECK_EQ(UA(U_CLOSE), 1);
    CHECK_EQ(UA(U_CLOSE), 0);
    /* the data survives the program freeing its block */
    KR(17, h);                                                   /* GlobalFree */
    UA(U_OPEN, b);
    h2 = UA(U_GETDATA, CF_TEXT);
    CHECK(h2 != 0 && !strcmp(gtext(h2), "Hi there"));
    /* emptying tells the previous owner */
    ulog_reset();
    CHECK_EQ(UA(U_EMPTY), 1);
    CHECK_EQ(UA(U_GETOWNER), b);
    i = ulog_find(WM_DESTROYCLIPBOARD, a, 0);
    CHECK(i >= 0);
    CHECK_EQ(UA(U_ISAVAIL, CF_TEXT), 0);
    CHECK_EQ(UA(U_COUNT), 0);
    /* registered formats */
    buf = g_seg(64);
    CHECK_EQ(UA(U_REGISTER, FARP(ustr("MyFormat"), 0)), 0xC000);
    CHECK_EQ(UA(U_REGISTER, FARP(ustr("Other"), 0)), 0xC001);
    CHECK_EQ(UA(U_REGISTER, FARP(ustr("MyFormat"), 0)), 0xC000);
    CHECK_EQ(UA(U_FORMATNAME, 0xC001, FARP(buf, 0), 32), 5);
    CHECK(!strcmp((char *)umem(buf, 0, 1), "Other"));
    CHECK_EQ(UA(U_FORMATNAME, CF_TEXT, FARP(buf, 0), 32), 0);
    /* private formats and bitmaps */
    h = gmem("blob");
    UA(U_SETDATA, 0xC000, h);
    CHECK_EQ(UA(U_ISAVAIL, 0xC000), 1);
    UA(U_SETDATA, CF_BITMAP, 0x1234);                            /* GDI handles are kept as given */
    CHECK_EQ(UA(U_GETDATA, CF_BITMAP), 0x1234);
    CHECK_EQ(UA(U_COUNT), 2);
    CHECK_EQ(UA(U_ENUM, 0xC000), CF_BITMAP);
    /* a viewer is told about changes when the clipboard closes */
    CHECK_EQ(UA(U_SETVIEWER, a), 0);
    CHECK_EQ(UA(U_GETVIEWER), a);
    ulog_reset();
    UA(U_CLOSE);
    CHECK(ulog_find(WM_DRAWCLIPBOARD, a, 0) >= 0);
    CHECK_EQ(UA(U_CHANGECHAIN, a, 0), 1);
    CHECK_EQ(UA(U_GETVIEWER), 0);
    /* delayed rendering: the owner is asked when somebody wants the data */
    UA(U_OPEN, a);
    UA(U_EMPTY);
    UA(U_SETDATA, CF_TEXT, 0);
    ulog_reset();
    UA(U_GETDATA, CF_TEXT);
    CHECK(ulog_find(WM_RENDERFORMAT, a, 0) >= 0);
    UA(U_CLOSE);
    down();
}

static void test_edit_clipboard(void) {
    uint16_t par, e1, e2, h;
    int guard;
    printf("test_misc_edit_clipboard\n");
    boot();
    u_register("PAR", 0, 0, 0, 0, u_ds(), 6);
    par = u_create("PAR", "p", WS_POPUP | WS_VISIBLE, 100, 100, 300, 200, 0, 0, u_ds());
    e1 = u_create("EDIT", "", WS_CHILD | WS_VISIBLE | 0x80, 10, 10, 100, 22, par, 1, u_ds());
    e2 = u_create("EDIT", "", WS_CHILD | WS_VISIBLE | 0x80, 10, 50, 100, 22, par, 2, u_ds());
    {
        wm_msg m;
        UA(22, e1);
        UCALL(111, e1, 0xC, 0, LONG((uint32_t)ustr("abc") << 16));              /* WM_SETTEXT */
        UCALL(111, e1, 0x401, 0, LONG((uint32_t)0x7FFF << 16 | 0));              /* EM_SETSEL all */
        {
            wm_input in;
            memset(&in, 0, sizeof in);
            in.kind = WM_IN_KEY_DOWN;
            in.vk = 0x11;
            wm_input_event(user_wm(uu), &in);
            in.vk = 'C';
            wm_input_event(user_wm(uu), &in);
            in.kind = WM_IN_KEY_UP;
            wm_input_event(user_wm(uu), &in);
        }
        for (guard = 0; guard < 100 && wm_next_message(user_wm(uu), &m, 0, 0, 0, true); guard++) {
            wm_translate(user_wm(uu), &m);
            wm_dispatch(user_wm(uu), &m);
        }
    }
    UA(U_OPEN, par);
    h = UA(U_GETDATA, CF_TEXT);
    CHECK(h != 0 && !strcmp(gtext(h), "abc"));                    /* Ctrl+C copied the selection */
    UA(U_CLOSE);
    /* Ctrl+V pastes it into the other control at its caret */
    UA(22, e2);
    {
        wm_msg m;
        wm_input in;
        memset(&in, 0, sizeof in);
        in.kind = WM_IN_KEY_DOWN;
        in.vk = 'V';
        wm_input_event(user_wm(uu), &in);
        for (guard = 0; guard < 100 && wm_next_message(user_wm(uu), &m, 0, 0, 0, true); guard++) {
            wm_translate(user_wm(uu), &m);
            wm_dispatch(user_wm(uu), &m);
        }
    }
    {
        uint16_t b = g_seg(32);
        UA(36, e2, FARP(b, 0), 30);
        CHECK(!strcmp((char *)umem(b, 0, 1), "abc"));
    }
    down();
}

static void test_caret(void) {
    uint16_t w, buf;
    int ox, oy;
    printf("test_misc_caret\n");
    boot();
    u_register("CAR", 0, 0, 0, 0, u_ds(), 6);
    w = u_create("CAR", "c", WS_POPUP | WS_VISIBLE, 50, 50, 200, 100, 0, 0, u_ds());
    g_clear(255);
    wm_client_origin(user_wm(uu), w, &ox, &oy);
    CHECK_EQ(UA(U_CREATECARET, w, 0, 2, 16), 1);
    CHECK_EQ(g_px(ox + 10, oy + 20), 255);                         /* created hidden */
    UA(U_SETCARETPOS, 10, 20);
    CHECK_EQ(g_px(ox + 10, oy + 20), 255);
    UA(U_SHOWCARET, w);
    CHECK_EQ(g_px(ox + 10, oy + 20), 0);                           /* the caret inverts the pixels it covers */
    CHECK_EQ(g_px(ox + 11, oy + 35), 0);
    CHECK_EQ(g_px(ox + 12, oy + 20), 255);
    CHECK_EQ(g_px(ox + 10, oy + 36), 255);
    UA(U_SETCARETPOS, 30, 40);
    CHECK_EQ(g_px(ox + 10, oy + 20), 255);                         /* the old place is restored */
    CHECK_EQ(g_px(ox + 30, oy + 40), 0);
    buf = g_seg(8);
    UA(U_GETCARETPOS, FARP(buf, 0));
    CHECK_EQ(urd16(buf, 0), 30);
    CHECK_EQ(urd16(buf, 2), 40);
    UA(U_HIDECARET, w);
    CHECK_EQ(g_px(ox + 30, oy + 40), 255);
    UA(U_HIDECARET, w);                                            /* hide counts nest */
    UA(U_SHOWCARET, w);
    CHECK_EQ(g_px(ox + 30, oy + 40), 255);
    UA(U_SHOWCARET, w);
    CHECK_EQ(g_px(ox + 30, oy + 40), 0);
    CHECK_EQ(UA(U_DESTROYCARET), 1);
    CHECK_EQ(g_px(ox + 30, oy + 40), 255);
    CHECK_EQ(UA(U_DESTROYCARET), 0);
    CHECK_EQ(UA(U_CREATECARET, 0x7777, 0, 2, 16), 0);
    CHECK_EQ(UA(U_SETCARETPOS, 1, 1), 0);
    down();
}

static void test_regions_and_window_dc(void) {
    uint16_t w, rgn, dc, rs;
    fb_rect r;
    printf("test_misc_regions\n");
    boot();
    u_register("RGW", 0, 0, 0, 0, u_ds(), 6);
    w = u_create("RGW", "r", WS_POPUP | WS_VISIBLE, 50, 50, 200, 100, 0, 0, u_ds());
    UA(127, w, FARP(0, 0));                                        /* nothing pending */
    rs = g_seg(16);
    CHECK_EQ(UA(190, w, FARP(rs, 0), 0), 0);                      /* GetUpdateRect */
    rgn = GA(64, 10, 20, 60, 50);                                  /* CreateRectRgn */
    CHECK_EQ(UA(U_INVALIDATERGN, w, rgn, 1), 1);
    CHECK_EQ(UA(190, w, FARP(rs, 0), 0), 1);
    CHECK_EQ(urd16(rs, 0), 10);
    CHECK_EQ(urd16(rs, 2), 20);
    CHECK_EQ(urd16(rs, 4), 60);
    CHECK_EQ(urd16(rs, 6), 50);
    CHECK_EQ(UA(U_VALIDATERGN, w, rgn), 1);
    CHECK_EQ(UA(190, w, FARP(rs, 0), 0), 0);
    CHECK_EQ(UA(U_INVALIDATERGN, w, 0, 1), 1);                    /* no region: the whole client area */
    CHECK_EQ(UA(190, w, FARP(rs, 0), 0), 1);
    CHECK_EQ(urd16(rs, 4), 200);
    CHECK_EQ(UA(U_INVALIDATERGN, 0x7777, rgn, 1), 0);
    /* GetWindowDC: its origin is the window's corner */
    dc = UA(U_GETWINDOWDC, w);
    CHECK(dc != 0);
    {
        wm_dc *d = wm_dc_get(user_wm(uu), dc);
        wm_window_rect(user_wm(uu), w, &r);
        CHECK(d && d->org_x == r.left && d->org_y == r.top);
    }
    CHECK_EQ(UA(68, w, dc), 1);
    down();
}

static void test_commdlg(void) {
    uint16_t ofn = 0;
    printf("test_misc_commdlg\n");
    boot();
    ofn = g_seg(0x100);
    CHECK_EQ(CR(2, FARP(ofn, 0)), 0);                               /* GetSaveFileName: cancelled */
    CHECK_EQ(CR(1, FARP(ofn, 0)), 0);                               /* GetOpenFileName */
    CHECK_EQ(CR(20, FARP(ofn, 0)), 0);                              /* PrintDlg */
    CHECK_EQ(CR(26), 0);                                            /* CommDlgExtendedError: no error */
    down();
}

int main(void) {
    test_clipboard();
    test_edit_clipboard();
    test_caret();
    test_regions_and_window_dc();
    test_commdlg();
    printf("test_ui_misc: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
