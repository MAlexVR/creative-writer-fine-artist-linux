/* WM_PAINT with real GDI: BeginPaint's DC is clipped to the update area, the
 * class background brush erases through WM_ERASEBKGND, and a guest window
 * procedure draws through GDI calls. */
#include "gfixture.h"

enum { O_RECTANGLE = 27, O_CREATESOLIDBRUSH = 66, U_UPDATEWINDOW = 124 };

#define CODE_PAINT 0xA00

static uint8_t pix_of(uint32_t cref) { return (uint8_t)fb_pixel_of(g_fb(), cref); }

/* window procedure: on WM_PAINT BeginPaint, Rectangle(hdc, 0, 0, 200, 200), EndPaint; the PAINTSTRUCT is kept at DS:0x200
 * and the HDC at DS:0x1F0; everything else returns 1 */
static void install_paint_proc(void) {
    loader_thunk *bp = fx_thunk(&uf, "USER", 39), *ep = fx_thunk(&uf, "USER", 40), *rc = fx_thunk(&uf, "GDI", 27);
    uint8_t code[] = {
        0x55, 0x89, 0xE5,                               /* push bp; mov bp,sp */
        0x81, 0x7E, 0x0C, 0x0F, 0x00,                   /* cmp word [bp+12],0Fh */
        0x75, 49,                                       /* jne other */
        0xFF, 0x76, 0x0E, 0x1E, 0x68, 0x00, 0x02,       /* push hwnd; push ds; push 200h */
        0x9A, 0, 0, 0, 0,                               /* call BeginPaint */
        0xA3, 0xF0, 0x01,                               /* mov [1F0h],ax */
        0x50, 0x6A, 0x00, 0x6A, 0x00,                   /* push hdc; push 0; push 0 */
        0x68, 0xC8, 0x00, 0x68, 0xC8, 0x00,             /* push 200; push 200 */
        0x9A, 0, 0, 0, 0,                               /* call Rectangle */
        0xFF, 0x76, 0x0E, 0x1E, 0x68, 0x00, 0x02,       /* push hwnd; push ds; push 200h */
        0x9A, 0, 0, 0, 0,                               /* call EndPaint */
        0x31, 0xC0, 0x31, 0xD2, 0xEB, 0x05,             /* xor ax,ax; xor dx,dx; jmp done */
        0xB8, 0x01, 0x00, 0x31, 0xD2,                   /* other: mov ax,1; xor dx,dx (WM_NCCREATE must succeed) */
        0x5D, 0xCA, 0x0A, 0x00};                        /* done: pop bp; retf 10 */
    code[18] = (uint8_t)bp->off; code[19] = (uint8_t)(bp->off >> 8); code[20] = (uint8_t)bp->sel; code[21] = (uint8_t)(bp->sel >> 8);
    code[37] = (uint8_t)rc->off; code[38] = (uint8_t)(rc->off >> 8); code[39] = (uint8_t)rc->sel; code[40] = (uint8_t)(rc->sel >> 8);
    code[49] = (uint8_t)ep->off; code[50] = (uint8_t)(ep->off >> 8); code[51] = (uint8_t)ep->sel; code[52] = (uint8_t)(ep->sel >> 8);
    u_put(CODE_PAINT, code, sizeof code);
}

static uint16_t make_class_window(const char *cls, uint32_t wndproc, uint16_t brush, int x, int y, int cx, int cy) {
    u_register(cls, wndproc, 0, 0, 0, 1, brush);
    return u_create(cls, "p", WS_POPUP | WS_VISIBLE, x, y, cx, cy, 0, 0, 1);
}

static void invalidate(uint16_t hwnd, int l, int t, int r, int b, bool erase) {
    uint16_t rs = g_seg(0x20);
    uwr16(rs, 0, (uint16_t)l); uwr16(rs, 2, (uint16_t)t); uwr16(rs, 4, (uint16_t)r); uwr16(rs, 6, (uint16_t)b);
    CHECK_EQ(UCALL(125, hwnd, FARP(rs, 0), erase), LOADER_STOP_HLT);
}

static void test_default_erase(void) {
    uint16_t hwnd, br;
    int x, y, bad;
    printf("test_paint_default_erase\n");
    gboot();
    /* a solid class brush */
    br = GA(O_CREATESOLIDBRUSH, LONG(0x0000FF));
    g_clear(0x03);
    hwnd = make_class_window("PE1", 0, br, 100, 100, 200, 150);
    CHECK(hwnd != 0);
    UCALL(U_UPDATEWINDOW, hwnd);                         /* the first paint covers the whole client area */
    CHECK_EQ(g_px(150, 150), pix_of(0x0000FF));
    g_clear(0x03);
    invalidate(hwnd, 10, 20, 60, 70, true);
    CHECK_EQ(UCALL(U_UPDATEWINDOW, hwnd), LOADER_STOP_HLT);
    bad = 0;
    for (y = 0; y < 480; y++)
        for (x = 0; x < 640; x++) {
            bool in = x >= 110 && x < 160 && y >= 120 && y < 170;
            uint8_t want = in ? pix_of(0x0000FF) : 0x03;
            if (g_px(x, y) != want) bad++;
        }
    CHECK_EQ(bad, 0);                                     /* erased exactly the update rectangle */
    /* no erase requested: nothing is painted */
    g_clear(0x03);
    invalidate(hwnd, 10, 20, 60, 70, false);
    UCALL(U_UPDATEWINDOW, hwnd);
    CHECK_EQ(g_px(120, 130), 0x03);
    /* a system colour index in the class: COLOR_WINDOW + 1 is white, COLOR_BACKGROUND + 1 grey */
    u_register("PE2", 0, 0, 0, 0, 1, 6);
    {
        uint16_t w2 = u_create("PE2", "p", WS_POPUP | WS_VISIBLE, 300, 200, 100, 100, 0, 0, 1);
        UCALL(U_UPDATEWINDOW, w2);
        g_clear(0x03);
        invalidate(w2, 0, 0, 100, 100, true);
        UCALL(U_UPDATEWINDOW, w2);
        CHECK_EQ(g_px(350, 250), pix_of(0xFFFFFF));
        CHECK_EQ(g_px(299, 250), 0x03);
        CHECK_EQ(g_px(400, 250), 0x03);
    }
    /* a class with no background brush leaves the area alone */
    {
        uint16_t w3 = make_class_window("PE3", 0, 0, 450, 200, 100, 100);
        UCALL(U_UPDATEWINDOW, w3);
        g_clear(0x03);
        invalidate(w3, 0, 0, 100, 100, true);
        UCALL(U_UPDATEWINDOW, w3);
        CHECK_EQ(g_px(460, 210), 0x03);
    }
    gdown();
}

static void test_guest_paint_clipped(void) {
    uint16_t hwnd;
    int x, y, bad;
    printf("test_paint_guest_wndproc\n");
    gboot();
    install_paint_proc();
    hwnd = make_class_window("PG1", u_far(CODE_PAINT), 0, 100, 100, 200, 150);
    CHECK(hwnd != 0);
    UCALL(U_UPDATEWINDOW, hwnd);                          /* flush the initial full-window paint */
    g_clear(0x03);
    /* only the update rectangle may receive pixels, whatever the procedure draws */
    invalidate(hwnd, 20, 30, 80, 90, false);
    CHECK_EQ(UCALL(U_UPDATEWINDOW, hwnd), LOADER_STOP_HLT);
    bad = 0;
    for (y = 0; y < 480; y++)
        for (x = 0; x < 640; x++) {
            bool in = x >= 120 && x < 180 && y >= 130 && y < 190;
            if (!in && g_px(x, y) != 0x03) bad++;
        }
    CHECK_EQ(bad, 0);
    CHECK_EQ(g_px(150, 160), 0xFF);                       /* the stock white brush fills the interior */
    CHECK_EQ(g_px(120, 130), 0xFF);
    CHECK(g_px(100, 100) == 0x03);                        /* the rectangle's own border at client (0,0) is outside the update area */
    /* the PAINTSTRUCT the procedure got: hdc, fErase, rcPaint */
    CHECK(urd16(u_ds(), 0x200) != 0);
    CHECK_EQ(urd16(u_ds(), 0x200 + 4), 20);
    CHECK_EQ(urd16(u_ds(), 0x200 + 6), 30);
    CHECK_EQ(urd16(u_ds(), 0x200 + 8), 80);
    CHECK_EQ(urd16(u_ds(), 0x200 + 10), 90);
    /* the update rectangle is validated by BeginPaint: a second UpdateWindow does nothing */
    g_clear(0x03);
    UCALL(U_UPDATEWINDOW, hwnd);
    CHECK_EQ(g_px(150, 160), 0x03);
    /* a full invalidation draws the whole rectangle including its border */
    invalidate(hwnd, 0, 0, 200, 150, false);
    UCALL(U_UPDATEWINDOW, hwnd);
    CHECK_EQ(g_px(100, 100), 0x00);
    CHECK_EQ(g_px(299, 249), 0x00);                       /* the right border; the rectangle's bottom edge lies below the window */
    CHECK_EQ(g_px(299, 250), 0x03);
    CHECK_EQ(g_px(200, 200), 0xFF);
    CHECK_EQ(g_px(200, 250), 0x03);                       /* below the client area */
    gdown();
}

int main(void) {
    test_default_erase();
    test_guest_paint_clipped();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
