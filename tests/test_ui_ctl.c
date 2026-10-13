/* Built-in controls: BUTTON (push, default, check, radio, group), STATIC (text, rectangles, icons), the SCROLLBAR
 * control and the scroll bars of windows: pixels, the notification messages and the control messages. */
#include "gfixture.h"
#include "res.h"
#include "rfixture.h"

#define BM_GETCHECK 0x400
#define BM_SETCHECK 0x401
#define BM_GETSTATE 0x402
#define BM_SETSTATE 0x403
#define BM_SETSTYLE 0x404
#define SBM_SETPOS 0x400
#define SBM_GETPOS 0x401
#define SBM_SETRANGE 0x402
#define SBM_GETRANGE 0x403
#define SBM_ENABLE_ARROWS 0x404
#define WM_HSCROLL_ 0x114
#define WM_VSCROLL_ 0x115
#define WM_CTLCOLOR_ 0x19

#define BS_DEFPUSHBUTTON 1
#define BS_CHECKBOX 2
#define BS_AUTOCHECKBOX 3
#define BS_RADIOBUTTON 4
#define BS_AUTO3STATE 6
#define BS_GROUPBOX 7
#define BS_AUTORADIOBUTTON 9
#define SS_CENTER 1
#define SS_RIGHT 2
#define SS_ICON 3
#define SS_BLACKRECT 4
#define SS_GRAYFRAME 8
#define SBS_VERT 1
#define SB_VERT_ 1
#define WS_GROUP 0x20000u
#define WS_TABSTOP 0x10000u

#define PX 100    /* parent client origin on the screen */
#define PY 100

static uint16_t parent;

static uint32_t px(int x, int y) { return fb_colorref_at(user_fb(uu), x, y); }

static void drain(void) {
    wm_msg m;
    int guard = 0;
    while (guard++ < 2000 && wm_next_message(user_wm(uu), &m, 0, 0, 0, true)) {
        if (m.message == WM_QUIT) break;
        wm_translate(user_wm(uu), &m);
        wm_dispatch(user_wm(uu), &m);
    }
}

static void mouse(wm_in_kind k, int x, int y) {
    wm_input in;
    memset(&in, 0, sizeof in);
    in.kind = k;
    in.x = x;
    in.y = y;
    wm_input_event(user_wm(uu), &in);
}

static void key(wm_in_kind k, unsigned vk) {
    wm_input in;
    memset(&in, 0, sizeof in);
    in.kind = k;
    in.vk = vk;
    wm_input_event(user_wm(uu), &in);
}

static void setup(void) {
    gboot();
    u_register("PAR", u_far(U_CODE_A), 0, 0, 0, u_ds(), 6);
    g_clear(77);
    parent = u_create("PAR", "p", WS_POPUP | WS_VISIBLE, PX, PY, 400, 300, 0, 0, u_ds());
    UA(127, parent, FARP(0, 0));                     /* nothing to paint in the parent: keeps WM_PAINT quiet */
    {
        fb_rect r = {0, 0, 400, 300};
        wm_dc_fill(user_wm(uu), wm_get_dc(user_wm(uu), parent), &r, RGB(255, 255, 255));
    }
    drain();
    ulog_reset();
}

static uint16_t mk(const char *cls, const char *text, uint32_t style, int x, int y, int w, int h, unsigned id) {
    return u_create(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (uint16_t)id, u_ds());
}

static int find_msg(uint16_t msg, unsigned from) { return ulog_find(msg, 0, from); }

static int count_black(int l, int t, int r, int b) {
    int x, y, n = 0;
    for (y = t; y < b; y++)
        for (x = l; x < r; x++) n += px(x, y) == 0;
    return n;
}

static void click(int x, int y) {
    mouse(WM_IN_MOVE, x, y);
    mouse(WM_IN_BUTTON_DOWN, x, y);
    drain();
    mouse(WM_IN_BUTTON_UP, x, y);
    drain();
}

static void test_push_button(void) {
    uint16_t b, d;
    int i;
    printf("test_ctl_push_button\n");
    setup();
    b = mk("BUTTON", "OK", 0, 10, 10, 80, 24, 7);
    d = mk("BUTTON", "Go", BS_DEFPUSHBUTTON, 10, 50, 80, 24, 8);
    CHECK(b != 0 && d != 0);
    drain();
    /* look: black rounded border, white highlight inside, grey face, no corner pixels */
    CHECK_EQ(px(PX + 10 + 40, PY + 10), 0);
    CHECK_EQ(px(PX + 10, PY + 10 + 12), 0);
    CHECK_EQ(px(PX + 10, PY + 10), RGB(255, 255, 255));                 /* the rounded corner shows the parent */
    CHECK_EQ(px(PX + 10 + 40, PY + 10 + 1), RGB(255, 255, 255));        /* highlight */
    CHECK_EQ(px(PX + 10 + 40, PY + 10 + 3), wm_sys_color(15));           /* face */
    CHECK_EQ(px(PX + 10 + 40, PY + 10 + 22), wm_sys_color(16));          /* shadow */
    CHECK_EQ(px(PX + 10 + 40, PY + 10 + 23), 0);                         /* bottom border */
    CHECK(count_black(PX + 10 + 20, PY + 10 + 4, PX + 10 + 60, PY + 10 + 20) > 12);   /* the caption */
    CHECK_EQ(px(PX + 10 + 40, PY + 50 + 1), 0);                          /* a default button has a second border */
    /* a click: down shows it pressed, up inside sends WM_COMMAND */
    mouse(WM_IN_MOVE, PX + 50, PY + 22);
    mouse(WM_IN_BUTTON_DOWN, PX + 50, PY + 22);
    drain();
    CHECK_EQ(px(PX + 10 + 40, PY + 10 + 1), wm_sys_color(16));           /* pressed: shadow on top */
    CHECK_EQ(find_msg(WM_COMMAND, 0), -1);
    ulog_reset();
    mouse(WM_IN_BUTTON_UP, PX + 50, PY + 22);
    drain();
    i = find_msg(WM_COMMAND, 0);
    CHECK(i >= 0);
    if (i >= 0) {
        CHECK_EQ(ulog_w((unsigned)i, ULOG_WP), 7);
        CHECK_EQ(ulog_lp((unsigned)i), (uint32_t)b);                     /* BN_CLICKED (0) in the high word */
        CHECK_EQ(ulog_w((unsigned)i, ULOG_HWND), parent);
    }
    CHECK_EQ(px(PX + 10 + 40, PY + 10 + 1), RGB(255, 255, 255));        /* released */
    /* pressed, dragged out and released outside: no command */
    ulog_reset();
    mouse(WM_IN_BUTTON_DOWN, PX + 50, PY + 22);
    drain();
    mouse(WM_IN_MOVE, PX + 300, PY + 200);
    drain();
    mouse(WM_IN_BUTTON_UP, PX + 300, PY + 200);
    drain();
    CHECK_EQ(find_msg(WM_COMMAND, 0), -1);
    /* a disabled button ignores the mouse */
    UA(34, b, 0);                                                       /* EnableWindow(FALSE) */
    click(PX + 50, PY + 22);
    CHECK_EQ(find_msg(WM_COMMAND, 0), -1);
    UA(34, b, 1);
    /* keyboard: focus plus space */
    UA(22, b);                                                          /* SetFocus */
    drain();
    ulog_reset();
    key(WM_IN_KEY_DOWN, 0x20);
    drain();
    CHECK_EQ(find_msg(WM_COMMAND, 0), -1);
    key(WM_IN_KEY_UP, 0x20);
    drain();
    CHECK(find_msg(WM_COMMAND, 0) >= 0);
    gdown();
}

static void test_check_and_radio(void) {
    uint16_t c, r1, r2, t;
    int i;
    printf("test_ctl_check_radio\n");
    setup();
    /* later siblings lie above earlier ones and the tab order follows the Z order, so the template order
     * (checkbox, radio 1, radio 2, tri-state) is created backwards here */
    t = mk("BUTTON", "Tri", BS_AUTO3STATE, 10, 120, 100, 16, 23);
    r2 = mk("BUTTON", "Two", BS_AUTORADIOBUTTON, 10, 100, 100, 16, 22);
    r1 = mk("BUTTON", "One", BS_AUTORADIOBUTTON | WS_GROUP, 10, 80, 100, 16, 21);
    c = mk("BUTTON", "Opt", BS_AUTOCHECKBOX, 10, 50, 100, 16, 20);
    drain();
    CHECK_EQ(px(PX + 10 + 6, PY + 50 + 2 + 6), RGB(255, 255, 255));    /* an empty box is white inside */
    UCALL(111, c, BM_GETCHECK, 0, LONG(0));
    CHECK_EQ(fx_ax(&uf), 0);
    click(PX + 14, PY + 58);
    UCALL(111, c, BM_GETCHECK, 0, LONG(0));
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(px(PX + 10 + 6, PY + 50 + 2 + 6), 0);                     /* the X crosses the middle */
    i = find_msg(WM_COMMAND, 0);
    CHECK(i >= 0 && ulog_w((unsigned)i, ULOG_WP) == 20);
    click(PX + 14, PY + 58);
    UCALL(111, c, BM_GETCHECK, 0, LONG(0));
    CHECK_EQ(fx_ax(&uf), 0);
    UCALL(111, c, BM_SETCHECK, 1, LONG(0));
    UCALL(111, c, BM_GETCHECK, 0, LONG(0));
    CHECK_EQ(fx_ax(&uf), 1);
    /* radio buttons: clicking one clears the others of its group */
    UCALL(111, r1, BM_SETCHECK, 1, LONG(0));
    click(PX + 14, PY + 108);
    UCALL(111, r1, BM_GETCHECK, 0, LONG(0));
    CHECK_EQ(fx_ax(&uf), 0);
    UCALL(111, r2, BM_GETCHECK, 0, LONG(0));
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(px(PX + 10 + 6, PY + 100 + 2 + 6), 0);                    /* the dot */
    CHECK_EQ(px(PX + 10 + 6, PY + 80 + 2 + 6), RGB(255, 255, 255));
    /* three states cycle 0 -> 1 -> 2 -> 0 */
    click(PX + 14, PY + 128);
    UCALL(111, t, BM_GETCHECK, 0, LONG(0));
    CHECK_EQ(fx_ax(&uf), 1);
    click(PX + 14, PY + 128);
    UCALL(111, t, BM_GETCHECK, 0, LONG(0));
    CHECK_EQ(fx_ax(&uf), 2);
    click(PX + 14, PY + 128);
    UCALL(111, t, BM_GETCHECK, 0, LONG(0));
    CHECK_EQ(fx_ax(&uf), 0);
    /* BM_GETSTATE reports the check in the low bits */
    UCALL(111, c, BM_GETSTATE, 0, LONG(0));
    CHECK_EQ(fx_ax(&uf) & 3, 1);
    /* the control sent WM_CTLCOLOR to its parent before painting (CTLCOLOR_BTN = 3) */
    i = find_msg(WM_CTLCOLOR_, 0);
    CHECK(i >= 0 && (ulog_lp((unsigned)i) >> 16) == 3);
    gdown();
}

static void test_group_box(void) {
    printf("test_ctl_groupbox\n");
    setup();
    mk("BUTTON", "Group", BS_GROUPBOX, 10, 10, 120, 60, 30);
    drain();
    /* an etched frame: grey line with a white line inside, interrupted by the title at the top */
    CHECK_EQ(px(PX + 10, PY + 10 + 30), wm_sys_color(16));
    CHECK_EQ(px(PX + 10 + 1, PY + 10 + 30), RGB(255, 255, 255));
    CHECK_EQ(px(PX + 10 + 60, PY + 10 + 58), wm_sys_color(16));
    CHECK_EQ(px(PX + 10 + 60, PY + 10 + 59), RGB(255, 255, 255));
    CHECK(count_black(PX + 10 + 8, PY + 10, PX + 10 + 60, PY + 10 + 16) > 10);       /* the title */
    gdown();
}

static void test_static(void) {
    uint16_t s;
    int x, y, left = 0, right = 0;
    printf("test_ctl_static\n");
    setup();
    mk("STATIC", "Hello", 0, 10, 10, 100, 16, 40);
    drain();
    CHECK(count_black(PX + 10, PY + 10, PX + 110, PY + 26) > 15);
    CHECK_EQ(px(PX + 10 + 99, PY + 10 + 15), RGB(255, 255, 255));        /* a plain window parent: white background */
    /* alignment */
    mk("STATIC", "Hi", SS_RIGHT, 10, 40, 100, 16, 41);
    mk("STATIC", "Hi", SS_CENTER, 10, 70, 100, 16, 42);
    drain();
    for (y = PY + 40; y < PY + 56; y++) for (x = PX + 10; x < PX + 60; x++) left += px(x, y) == 0;
    for (y = PY + 40; y < PY + 56; y++) for (x = PX + 60; x < PX + 110; x++) right += px(x, y) == 0;
    CHECK(left == 0 && right > 5);
    left = right = 0;
    for (y = PY + 70; y < PY + 86; y++) for (x = PX + 10; x < PX + 40; x++) left += px(x, y) == 0;
    for (y = PY + 70; y < PY + 86; y++) for (x = PX + 80; x < PX + 110; x++) right += px(x, y) == 0;
    CHECK(left == 0 && right == 0);
    /* word wrap in a narrow box: two lines of ink, one under the other */
    mk("STATIC", "Hello World", 0, 10, 100, 60, 40, 43);
    drain();
    CHECK(count_black(PX + 10, PY + 100, PX + 70, PY + 116) > 10);
    CHECK(count_black(PX + 10, PY + 116, PX + 70, PY + 132) > 10);
    /* "&" marks a mnemonic and is not drawn; "&&" is a literal ampersand */
    s = mk("STATIC", "&Ab", 0, 10, 150, 100, 16, 44);
    drain();
    {
        char buf[16];
        uint16_t b = g_seg(16);
        UA(36, s, FARP(b, 0), 16);                                       /* GetWindowText keeps the ampersand */
        strcpy(buf, (char *)umem(b, 0, 1));
        CHECK(!strcmp(buf, "&Ab"));
    }
    /* rectangles and frames */
    mk("STATIC", "", SS_BLACKRECT, 10, 170, 30, 10, 45);
    mk("STATIC", "", SS_GRAYFRAME, 50, 170, 30, 10, 46);
    drain();
    CHECK_EQ(count_black(PX + 10, PY + 170, PX + 40, PY + 180), 300);
    CHECK_EQ(px(PX + 50, PY + 175), wm_sys_color(1));
    CHECK_EQ(px(PX + 65, PY + 175), RGB(255, 255, 255));
    gdown();
}

static void test_static_icon(void) {
    rb_t b, g;
    int y, x;
    printf("test_ctl_static_icon\n");
    setup();
    b.n = 0;
    rb_u32(&b, 40); rb_u32(&b, 32); rb_u32(&b, 64); rb_u16(&b, 1); rb_u16(&b, 8);
    rb_u32(&b, 0); rb_u32(&b, 0); rb_u32(&b, 0); rb_u32(&b, 0); rb_u32(&b, 2); rb_u32(&b, 0);
    rb_u8(&b, 0); rb_u8(&b, 0); rb_u8(&b, 0); rb_u8(&b, 0);
    rb_u8(&b, 0); rb_u8(&b, 0); rb_u8(&b, 255); rb_u8(&b, 0);
    for (y = 0; y < 32; y++) for (x = 0; x < 32; x++) rb_u8(&b, x < 16 ? 0 : 1);
    for (y = 0; y < 32; y++) { rb_u8(&b, 0xFF); rb_u8(&b, 0xFF); rb_u8(&b, 0); rb_u8(&b, 0); }
    rf_add(&uf, RT_ICON, NULL, 11, NULL, b.b, b.n);
    g.n = 0;
    rb_u16(&g, 0); rb_u16(&g, 1); rb_u16(&g, 1);
    rb_u8(&g, 32); rb_u8(&g, 32); rb_u8(&g, 0); rb_u8(&g, 0); rb_u16(&g, 1); rb_u16(&g, 8); rb_u32(&g, (uint32_t)b.n); rb_u16(&g, 11);
    rf_add(&uf, RT_GROUP_ICON, NULL, 0, "PIC", g.b, g.n);
    mk("STATIC", "PIC", SS_ICON, 10, 10, 32, 32, 50);
    drain();
    CHECK_EQ(px(PX + 10 + 20, PY + 10 + 5), RGB(255, 0, 0));
    CHECK_EQ(px(PX + 10 + 2, PY + 10 + 5), RGB(255, 255, 255));        /* the transparent half shows the background */
    /* the same icon by resource id: "#11" is not a group id here, so nothing is drawn but nothing breaks */
    mk("STATIC", "#99", SS_ICON, 10, 60, 32, 32, 51);
    drain();
    CHECK_EQ(px(PX + 10 + 20, PY + 60 + 5), RGB(255, 255, 255));
    gdown();
}

static void test_scrollbar_control(void) {
    uint16_t sb;
    int i;
    printf("test_ctl_scrollbar\n");
    setup();
    sb = mk("SCROLLBAR", "", SBS_VERT, 200, 10, 16, 100, 60);
    drain();
    UCALL(111, sb, SBM_SETRANGE, 1, LONG(10 << 16 | 0));              /* min 0, max 10 */
    UCALL(111, sb, SBM_GETRANGE, 0, LONG(0));
    CHECK_EQ(fx_dxax(&uf), (uint32_t)(10 << 16 | 0));
    UCALL(111, sb, SBM_SETPOS, 5, LONG(1));
    UCALL(111, sb, SBM_GETPOS, 0, LONG(0));
    CHECK_EQ(fx_ax(&uf), 5);
    UCALL(111, sb, SBM_SETPOS, 99, LONG(1));                           /* clamped, returns the old position */
    CHECK_EQ(fx_ax(&uf), 5);
    UCALL(111, sb, SBM_GETPOS, 0, LONG(0));
    CHECK_EQ(fx_ax(&uf), 10);
    UCALL(111, sb, SBM_SETPOS, 5, LONG(1));
    drain();
    /* thumb: top = 16 + 5 * (100 - 32 - 16... track 68, thumb 16) / 10 = 16 + 26 */
    CHECK_EQ(px(PX + 201, PY + 10 + 42), RGB(255, 255, 255));          /* the thumb's highlight row */
    CHECK_EQ(px(PX + 201, PY + 10 + 41), wm_sys_color(0));             /* track just above it */
    CHECK_EQ(px(PX + 208, PY + 10 + 8), 0);                            /* the up arrow's glyph */
    /* a press on the lower arrow sends SB_LINEDOWN then SB_ENDSCROLL */
    ulog_reset();
    mouse(WM_IN_MOVE, PX + 208, PY + 10 + 92);
    mouse(WM_IN_BUTTON_DOWN, PX + 208, PY + 10 + 92);
    mouse(WM_IN_BUTTON_UP, PX + 208, PY + 10 + 92);
    drain();
    i = find_msg(WM_VSCROLL_, 0);
    CHECK(i >= 0);
    if (i >= 0) {
        CHECK_EQ(ulog_w((unsigned)i, ULOG_WP), 1);                      /* SB_LINEDOWN */
        CHECK_EQ(ulog_lp((unsigned)i) >> 16, sb);
    }
    i = find_msg(WM_VSCROLL_, (unsigned)(i + 1));
    CHECK(i >= 0 && ulog_w((unsigned)i, ULOG_WP) == 8);                 /* SB_ENDSCROLL */
    /* a page click above the thumb */
    ulog_reset();
    mouse(WM_IN_MOVE, PX + 208, PY + 10 + 30);
    mouse(WM_IN_BUTTON_DOWN, PX + 208, PY + 10 + 30);
    mouse(WM_IN_BUTTON_UP, PX + 208, PY + 10 + 30);
    drain();
    i = find_msg(WM_VSCROLL_, 0);
    CHECK(i >= 0 && ulog_w((unsigned)i, ULOG_WP) == 2);                 /* SB_PAGEUP */
    /* dragging the thumb: SB_THUMBTRACK with positions, SB_THUMBPOSITION on release */
    ulog_reset();
    mouse(WM_IN_MOVE, PX + 208, PY + 10 + 50);
    mouse(WM_IN_BUTTON_DOWN, PX + 208, PY + 10 + 50);
    mouse(WM_IN_MOVE, PX + 208, PY + 10 + 50 + 26);
    mouse(WM_IN_BUTTON_UP, PX + 208, PY + 10 + 50 + 26);
    drain();
    i = find_msg(WM_VSCROLL_, 0);
    CHECK(i >= 0 && ulog_w((unsigned)i, ULOG_WP) == 5);                 /* SB_THUMBTRACK */
    CHECK(i >= 0 && (int16_t)ulog_lp((unsigned)i) >= 9);                /* about 26 pixels = 4 steps of 10/52 more... */
    i = find_msg(WM_VSCROLL_, (unsigned)(i + 1));
    CHECK(i >= 0);
    gdown();
}

static void test_window_scrollbars(void) {
    uint16_t w, buf;
    fb_rect cl0, cl1;
    int i;
    printf("test_ctl_window_scrollbars\n");
    gboot();
    u_register("SBW", u_far(U_CODE_A), 0, 0, 0, u_ds(), 6);
    w = u_create("SBW", "s", WS_POPUP | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL, 50, 50, 200, 150, 0, 0, u_ds());
    CHECK(w != 0);
    buf = g_seg(16);
    wm_client_rect(user_wm(uu), w, &cl0);
    CHECK_EQ(cl0.right, 200 - 16);                                      /* both bars take 16 pixels */
    CHECK_EQ(cl0.bottom, 150 - 16);
    CHECK_EQ(UA(65, w, SB_VERT_, FARP(buf, 0), FARP(buf, 2)), 1);       /* GetScrollRange */
    CHECK(urd16(buf, 0) == 0 && urd16(buf, 2) == 0);
    CHECK_EQ(UA(64, w, SB_VERT_, 0, 100, 1), 1);                        /* SetScrollRange */
    CHECK_EQ(UA(62, w, SB_VERT_, 40, 1), 0);                            /* SetScrollPos returns the old position */
    CHECK_EQ(UA(62, w, SB_VERT_, 60, 1), 40);
    CHECK_EQ(UA(63, w, SB_VERT_), 60);
    CHECK_EQ(UA(62, w, SB_VERT_, 500, 1), 60);
    CHECK_EQ(UA(63, w, SB_VERT_), 100);                                 /* clamped */
    CHECK_EQ(UA(65, w, SB_VERT_, FARP(buf, 0), FARP(buf, 2)), 1);
    CHECK(urd16(buf, 0) == 0 && urd16(buf, 2) == 100);
    UA(62, w, SB_VERT_, 0, 1);
    UCALL(107, w, 0x85, 1, LONG(0));                                    /* DefWindowProc(WM_NCPAINT) */
    /* the vertical bar at x = 50+200-16..: its up arrow glyph (black) and a thumb at the top */
    CHECK_EQ(px(50 + 200 - 8, 50 + 8 - 1 + 1), 0);
    /* hit testing */
    CHECK_EQ(UA(107, w, 0x84, 0, LONG((uint32_t)(50 + 70) << 16 | (50 + 200 - 8))), 7);    /* HTVSCROLL */
    CHECK_EQ(UA(107, w, 0x84, 0, LONG((uint32_t)(50 + 150 - 8) << 16 | (50 + 60))), 6);    /* HTHSCROLL */
    /* hiding the vertical bar gives its 16 pixels back */
    CHECK_EQ(UA(265, w, SB_VERT_, 0), 1);
    wm_client_rect(user_wm(uu), w, &cl1);
    CHECK_EQ(cl1.right, 200);
    CHECK_EQ(cl1.bottom, 150 - 16);
    CHECK_EQ(UA(265, w, SB_VERT_, 1), 1);
    wm_client_rect(user_wm(uu), w, &cl1);
    CHECK_EQ(cl1.right, 200 - 16);
    /* EnableScrollBar: both arrows */
    CHECK_EQ(UA(482, w, SB_VERT_, 3), 1);
    UCALL(107, w, 0x85, 1, LONG(0));
    CHECK_EQ(px(50 + 200 - 8, 50 + 8), wm_sys_color(16));                /* the glyph turned grey */
    CHECK_EQ(UA(482, w, SB_VERT_, 0), 1);
    /* a click on the down arrow of the vertical bar: WM_VSCROLL(SB_LINEDOWN) to the window itself */
    ulog_reset();
    mouse(WM_IN_BUTTON_DOWN, 50 + 200 - 8, 50 + 150 - 16 - 8);
    mouse(WM_IN_BUTTON_UP, 50 + 200 - 8, 50 + 150 - 16 - 8);
    UCALL(107, w, 0xA1, 7, LONG((uint32_t)(50 + 150 - 16 - 8) << 16 | (50 + 200 - 8)));    /* WM_NCLBUTTONDOWN, HTVSCROLL */
    i = find_msg(WM_VSCROLL_, 0);
    CHECK(i >= 0 && ulog_w((unsigned)i, ULOG_WP) == 1 && ulog_lp((unsigned)i) >> 16 == 0);
    gdown();
}

int main(void) {
    test_push_button();
    test_check_and_radio();
    test_group_box();
    test_static();
    test_static_icon();
    test_scrollbar_control();
    test_window_scrollbars();
    printf("test_ui_ctl: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
