/* Window manager core: classes, window tree and Z-order, geometry, words,
 * show/enable, focus/capture/activation, paint region and device contexts. */
#include "wmfixture.h"

static void test_classes(void) {
    wm_class_def d;
    uint16_t a1, a2;
    wm_class *c;
    printf("test_wm_classes\n");
    wboot();
    memset(&d, 0, sizeof d);
    d.name = "MyClass";
    d.wndproc = PROC_A;
    d.hinstance = 7;
    d.cls_extra = 6;
    d.wnd_extra = 4;
    d.hbrbackground = 3;
    a1 = wm_register_class(&wm, &d);
    CHECK(a1 != 0);
    CHECK(wm_register_class(&wm, &d) == 0);          /* same name and instance */
    d.hinstance = 8;
    a2 = wm_register_class(&wm, &d);
    CHECK(a2 != 0 && a2 != a1);                       /* another instance may reuse the name */
    c = wm_find_class(&wm, "myclass", 7);             /* case-insensitive */
    CHECK(c != NULL && c->atom == a1 && c->wndproc == PROC_A && c->wnd_extra == 4);
    CHECK(wm_class_by_atom(&wm, a1) == c);
    CHECK(wm_find_class(&wm, "nothing", 7) == NULL);
    CHECK(wm_find_class(&wm, "MyClass", 99) == NULL); /* neither global nor that instance */
    CHECK(wm_find_class(&wm, "BUTTON", 99) != NULL);  /* built-in classes are global */
    CHECK(wm_unregister_class(&wm, "MyClass", 7));
    CHECK(wm_find_class(&wm, "MyClass", 7) == NULL);
    CHECK(!wm_unregister_class(&wm, "MyClass", 7));
    wdown();
}

static void test_create_destroy(void) {
    uint16_t h, hc;
    wm_window *win;
    printf("test_wm_create_destroy\n");
    wboot();
    wclass("TOP", PROC_A);
    CHECK(wm_is_window(&wm, wm.desktop));
    CHECK(!wm_is_window(&wm, 0));
    CHECK(!wm_is_window(&wm, 0x7777));
    CHECK_EQ(wmake("NOSUCH", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0), 0);
    CHECK_EQ(wnlog, 0);
    wresult[WM_CREATE] = 0;
    h = wmake("TOP", WS_OVERLAPPEDWINDOW, 10, 20, 300, 200, 0);
    CHECK(h != 0 && wm_is_window(&wm, h));
    /* NCCREATE, CREATE, then SIZE and MOVE (visible or not), in that order */
    CHECK(wlog[0].msg == WM_NCCREATE && wlog[0].hwnd == h && wlog[0].lp == 0xABCD0000u);
    CHECK(wlog[1].msg == WM_CREATE && wlog[1].lp == 0xABCD0000u);
    CHECK(wlog[2].msg == WM_SIZE && wlog[3].msg == WM_MOVE);
    CHECK_EQ(wcs_enter, 1);
    CHECK_EQ(wcs_leave, 1);
    CHECK(wlog[0].proc == PROC_A);
    win = wm_get(&wm, h);
    CHECK(win->cls == wm_find_class(&wm, "TOP", 0x1111));
    CHECK(!strcmp(wm_get_text(&wm, h), "title"));
    CHECK_EQ(wm_get_parent(&wm, h), 0);
    CHECK_EQ(wm_get_window(&wm, h, GW_OWNER), 0);
    /* child */
    wclear();
    hc = wmake("TOP", WS_CHILD | WS_VISIBLE, 5, 6, 50, 40, h);
    CHECK(hc != 0);
    CHECK_EQ(wm_get_parent(&wm, hc), h);
    CHECK(wm_is_child(&wm, h, hc));
    CHECK(!wm_is_child(&wm, hc, h));
    CHECK_EQ(wm_get_window(&wm, h, GW_CHILD), hc);
    /* destroy: parent WM_DESTROY, child DESTROY then NCDESTROY, parent NCDESTROY last */
    wclear();
    CHECK(wm_destroy_window(&wm, h));
    CHECK(!wm_is_window(&wm, h) && !wm_is_window(&wm, hc));
    CHECK(wlog[0].msg == WM_DESTROY && wlog[0].hwnd == h);
    CHECK(wfind(WM_DESTROY, hc, 0) > 0);
    CHECK(wfind(WM_NCDESTROY, hc, 0) < wfind(WM_NCDESTROY, h, 0));
    CHECK(wfind(WM_DESTROY, hc, 0) < wfind(WM_NCDESTROY, hc, 0));
    CHECK(!wm_destroy_window(&wm, h));
    CHECK(!wm_destroy_window(&wm, wm.desktop));
    wdown();
}

static void test_create_failures(void) {
    uint16_t h;
    printf("test_wm_create_failures\n");
    wboot();
    wclass("TOP", PROC_A);
    wresult[WM_NCCREATE] = 0;                          /* NCCREATE returns FALSE */
    CHECK_EQ(wmake("TOP", WS_OVERLAPPED, 0, 0, 10, 10, 0), 0);
    CHECK_EQ(wcount(WM_CREATE), 0);
    CHECK_EQ(wcount(WM_NCDESTROY), 1);
    wresult[WM_NCCREATE] = 1;
    wclear();
    wresult[WM_CREATE] = (uint32_t)-1;                 /* CREATE returns -1 */
    CHECK_EQ(wmake("TOP", WS_OVERLAPPED, 0, 0, 10, 10, 0), 0);
    CHECK_EQ(wcount(WM_DESTROY), 1);
    CHECK_EQ(wcount(WM_NCDESTROY), 1);
    CHECK_EQ(wcs_enter, wcs_leave);
    wresult[WM_CREATE] = 0;
    /* a class without a procedure uses the default one */
    wclass("PLAIN", 0);
    wclear();
    h = wmake("PLAIN", WS_OVERLAPPED, 0, 0, 10, 10, 0);
    CHECK(h != 0);
    CHECK_EQ(wnlog, 0);                                /* never reached the guest */
    wdown();
}

static void test_defaults_and_atom(void) {
    wm_create_params p;
    uint16_t h, atom;
    fb_rect r;
    printf("test_wm_default_position_atom\n");
    wboot();
    atom = wclass("TOP", PROC_A);
    p = wparams(NULL, WS_OVERLAPPEDWINDOW | WS_VISIBLE, WM_CW_USEDEFAULT, WM_CW_USEDEFAULT,
                WM_CW_USEDEFAULT, WM_CW_USEDEFAULT, 0);
    p.atom = atom;                                     /* class by atom */
    h = wm_create_window(&wm, &p);
    CHECK(h != 0);
    wm_window_rect(&wm, h, &r);
    CHECK(r.right - r.left > 100 && r.right <= 640);
    CHECK(r.bottom - r.top > 100 && r.bottom <= 480);
    CHECK_EQ(r.left, 0);
    wdown();
}

static void test_geometry(void) {
    uint16_t top, ch, gch;
    fb_rect r;
    int x, y, l, t, rr, b;
    printf("test_wm_geometry\n");
    wboot();
    wclass("W", PROC_A);
    top = wmake("W", WS_OVERLAPPED | WS_BORDER | WS_CAPTION, 100, 50, 300, 200, 0);
    wm_window_rect(&wm, top, &r);
    CHECK(r.left == 100 && r.top == 50 && r.right == 400 && r.bottom == 250);
    wm_nc_insets(WS_CAPTION, 0, false, &l, &t, &rr, &b);
    CHECK(l == 1 && rr == 1 && b == 1 && t == 20);   /* border + (caption - border) */
    wm_nc_insets(WS_OVERLAPPEDWINDOW, 0, true, &l, &t, &rr, &b);
    CHECK(l == 5 && t == 5 + 19 + 20 && b == 5);
    wm_nc_insets(WS_CHILD, 0, false, &l, &t, &rr, &b);
    CHECK(l == 0 && t == 0 && rr == 0 && b == 0);
    wm_nc_insets(WS_POPUP | WS_DLGFRAME, 0, false, &l, &t, &rr, &b);
    CHECK(l == 4 && t == 4);
    wm_client_rect(&wm, top, &r);
    CHECK(r.left == 0 && r.top == 0 && r.right == 298 && r.bottom == 200 - 20 - 1);
    wm_client_origin(&wm, top, &x, &y);
    CHECK(x == 101 && y == 70);
    ch = wmake("W", WS_CHILD, 10, 20, 100, 50, top);
    wm_client_origin(&wm, ch, &x, &y);
    CHECK(x == 111 && y == 90);
    wm_window_rect(&wm, ch, &r);                       /* screen coordinates */
    CHECK(r.left == 111 && r.top == 90 && r.right == 211 && r.bottom == 140);
    gch = wmake("W", WS_CHILD, 1, 2, 10, 10, ch);
    wm_client_origin(&wm, gch, &x, &y);
    CHECK(x == 112 && y == 92);
    x = 0; y = 0;
    CHECK(wm_client_to_screen(&wm, gch, &x, &y));
    CHECK(x == 112 && y == 92);
    CHECK(wm_screen_to_client(&wm, gch, &x, &y));
    CHECK(x == 0 && y == 0);
    x = 500; y = 400;
    wm_screen_to_client(&wm, ch, &x, &y);
    CHECK(x == 500 - 111 && y == 400 - 90);
    CHECK(!wm_client_to_screen(&wm, 0x7777, &x, &y));
    wdown();
}

/* A window procedure that answers WM_NCCALCSIZE with a client rectangle (the programs' full-screen windows give
 * the whole window back) changes the client area; windows whose answer is ignored keep the style-derived one. */
static bool calc_whole(void *ctx, uint16_t hwnd, fb_rect *rc) { (void)ctx; (void)hwnd; (void)rc; return true; }
static bool calc_ignored(void *ctx, uint16_t hwnd, fb_rect *rc) { (void)ctx; (void)hwnd; (void)rc; return false; }
static bool calc_inset(void *ctx, uint16_t hwnd, fb_rect *rc) {
    (void)ctx; (void)hwnd;
    rc->left += 7; rc->top += 9; rc->right -= 3; rc->bottom -= 4;
    return true;
}

static void test_client_from_nccalcsize(void) {
    uint16_t top, ch;
    fb_rect r;
    int x, y;
    printf("test_wm_client_from_nccalcsize\n");
    wboot();
    wclass("W", PROC_A);
    wm.ops.nccalcsize = calc_whole;
    top = wmake("W", WS_OVERLAPPED | WS_BORDER | WS_CAPTION, 100, 50, 300, 200, 0);
    wm_client_rect(&wm, top, &r);
    CHECK(r.left == 0 && r.top == 0 && r.right == 300 && r.bottom == 200);
    wm_client_origin(&wm, top, &x, &y);
    CHECK(x == 100 && y == 50);
    ch = wmake("W", WS_CHILD, 10, 20, 100, 50, top);
    wm_client_origin(&wm, ch, &x, &y);
    CHECK(x == 110 && y == 70);
    wm_move_window(&wm, top, 120, 60, 300, 200, false);   /* the client area follows the window */
    wm_client_origin(&wm, top, &x, &y);
    CHECK(x == 120 && y == 60);
    wm.ops.nccalcsize = calc_inset;
    wm_move_window(&wm, top, 120, 60, 310, 210, false);
    wm_client_rect(&wm, top, &r);
    CHECK(r.right == 310 - 7 - 3 && r.bottom == 210 - 9 - 4);
    wm_client_origin(&wm, top, &x, &y);
    CHECK(x == 127 && y == 69);
    {   /* the frame cannot be drawn over a client area that covers it: the window DC has nothing left to draw on */
        fb_rect box = {0, 0, 50, 10};
        wm.ops.nccalcsize = calc_whole;
        wm_move_window(&wm, top, 120, 60, 320, 210, false);
        wm_show_window(&wm, top, SW_SHOWNORMAL);
        wdrain();
        wm_dc_fill(&wm, wm_get_window_dc(&wm, top), &box, 0x0000FF);
        CHECK(fb_colorref_at(&wfb, 130, 65) != 0x0000FF);
        wm_move_window(&wm, top, 120, 60, 321, 210, false);
    }
    wm.ops.nccalcsize = calc_ignored;
    wm_move_window(&wm, top, 120, 60, 320, 210, false);
    wm_client_rect(&wm, top, &r);
    CHECK(r.right == 318 && r.bottom == 210 - 20 - 1);       /* border and caption again */
    wdown();
}

static void test_zorder_and_tree(void) {
    uint16_t a, b, c, d;
    printf("test_wm_zorder_tree\n");
    wboot();
    wclass("W", PROC_A);
    a = wmake("W", WS_POPUP, 0, 0, 50, 50, 0);
    b = wmake("W", WS_POPUP, 0, 0, 50, 50, 0);
    c = wmake("W", WS_POPUP, 0, 0, 50, 50, 0);
    /* a newly created window goes on top */
    CHECK_EQ(wm_top_window(&wm, wm.desktop), c);
    CHECK_EQ(wm_get_window(&wm, c, GW_HWNDNEXT), b);
    CHECK_EQ(wm_get_window(&wm, b, GW_HWNDNEXT), a);
    CHECK_EQ(wm_get_window(&wm, a, GW_HWNDNEXT), 0);
    CHECK_EQ(wm_get_window(&wm, a, GW_HWNDPREV), b);
    CHECK_EQ(wm_get_window(&wm, b, GW_HWNDFIRST), c);
    CHECK_EQ(wm_get_window(&wm, b, GW_HWNDLAST), a);
    CHECK(wm_bring_to_top(&wm, a));
    CHECK_EQ(wm_top_window(&wm, wm.desktop), a);
    CHECK_EQ(wm_get_window(&wm, a, GW_HWNDNEXT), c);
    /* SetWindowPos z-order: bottom, after a sibling, top */
    wm_set_window_pos(&wm, a, 1, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    CHECK_EQ(wm_get_window(&wm, a, GW_HWNDLAST), a);
    CHECK_EQ(wm_top_window(&wm, wm.desktop), c);
    wm_set_window_pos(&wm, a, c, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    CHECK_EQ(wm_get_window(&wm, c, GW_HWNDNEXT), a);
    wm_set_window_pos(&wm, b, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    CHECK_EQ(wm_top_window(&wm, wm.desktop), b);
    /* removing a middle window keeps the chain intact */
    d = wm_get_window(&wm, b, GW_HWNDNEXT);
    CHECK_EQ(d, c);
    wm_destroy_window(&wm, c);
    CHECK_EQ(wm_get_window(&wm, b, GW_HWNDNEXT), a);
    CHECK_EQ(wm_get_window(&wm, a, GW_HWNDPREV), b);
    /* hit testing finds the topmost window */
    wm_show_window(&wm, a, SW_SHOWNA);
    wm_show_window(&wm, b, SW_SHOWNA);
    CHECK_EQ(wm_window_from_point(&wm, 10, 10), b);
    CHECK_EQ(wm_window_from_point(&wm, 100, 100), wm.desktop);
    wm_bring_to_top(&wm, a);
    CHECK_EQ(wm_window_from_point(&wm, 10, 10), a);
    wdown();
}

static void test_owner_and_parent(void) {
    uint16_t top, pop, ch, other;
    printf("test_wm_owner_parent\n");
    wboot();
    wclass("W", PROC_A);
    top = wmake("W", WS_OVERLAPPEDWINDOW, 0, 0, 200, 200, 0);
    pop = wmake("W", WS_POPUP, 0, 0, 50, 50, top);     /* owned popup */
    CHECK_EQ(wm_get_window(&wm, pop, GW_OWNER), top);
    CHECK_EQ(wm_get_parent(&wm, pop), top);            /* GetParent returns the owner */
    CHECK_EQ(wm_get_window(&wm, top, GW_CHILD), 0);    /* it is a desktop child, not top's */
    ch = wmake("W", WS_CHILD, 0, 0, 20, 20, top);
    other = wmake("W", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0);
    CHECK_EQ(wm_set_parent(&wm, ch, other), top);
    CHECK_EQ(wm_get_parent(&wm, ch), other);
    CHECK_EQ(wm_get_window(&wm, top, GW_CHILD), 0);
    CHECK_EQ(wm_get_window(&wm, other, GW_CHILD), ch);
    /* destroying the owner destroys the owned popup */
    wm_destroy_window(&wm, top);
    CHECK(!wm_is_window(&wm, pop));
    CHECK(wm_is_window(&wm, ch) && wm_is_window(&wm, other));
    wdown();
}

static void test_words_and_longs(void) {
    uint16_t h;
    uint32_t v, old;
    wm_class_def d;
    printf("test_wm_words_longs\n");
    wboot();
    memset(&d, 0, sizeof d);
    d.name = "X";
    d.wndproc = PROC_A;
    d.hinstance = 5;
    d.wnd_extra = 8;
    d.cls_extra = 4;
    d.style = 0x0008;
    d.hbrbackground = 4;
    d.hcursor = 0x20;
    d.hicon = 0x30;
    wm_register_class(&wm, &d);
    wm_create_params p = wparams("X", WS_OVERLAPPEDWINDOW, 0, 0, 40, 40, 0);
    p.hinstance = 5;
    p.id_or_menu = 0;
    h = wm_create_window(&wm, &p);
    CHECK(h != 0);
    /* extra bytes */
    CHECK(wm_set_window_long(&wm, h, 0, 0x11223344u, false, &old));
    CHECK_EQ(old, 0);
    CHECK(wm_get_window_long(&wm, h, 0, &v, false));
    CHECK_EQ(v, 0x11223344u);
    CHECK(wm_get_window_long(&wm, h, 0, &v, true));
    CHECK_EQ(v, 0x3344u);
    CHECK(wm_get_window_long(&wm, h, 2, &v, true));
    CHECK_EQ(v, 0x1122u);
    CHECK(wm_set_window_long(&wm, h, 6, 0xBEEF, true, &old));
    CHECK(!wm_set_window_long(&wm, h, 7, 0xBEEF, true, &old));   /* past the extra bytes */
    CHECK(!wm_get_window_long(&wm, h, 8, &v, true));
    CHECK(!wm_get_window_long(&wm, h, 6, &v, false));            /* a long needs 4 bytes */
    /* named indices */
    CHECK(wm_get_window_long(&wm, h, -4, &v, false));            /* GWL_WNDPROC */
    CHECK_EQ(v, PROC_A);
    CHECK(wm_set_window_long(&wm, h, -4, PROC_B, false, &old));
    CHECK_EQ(old, PROC_A);
    CHECK_EQ(wm_get(&wm, h)->wndproc, PROC_B);
    CHECK(wm_get_window_long(&wm, h, -6, &v, true));             /* GWW_HINSTANCE */
    CHECK_EQ(v, 5);
    CHECK(wm_get_window_long(&wm, h, -16, &v, false));           /* GWL_STYLE */
    CHECK_EQ(v, WS_OVERLAPPEDWINDOW);
    CHECK(wm_set_window_long(&wm, h, -12, 77, true, &old));      /* GWW_ID */
    CHECK_EQ(wm_get(&wm, h)->id, 77);
    CHECK(!wm_get_window_long(&wm, 0x7777, 0, &v, true));
    /* class words */
    CHECK(wm_get_class_long(&wm, h, -10, &v, true));             /* GCW_HBRBACKGROUND */
    CHECK_EQ(v, 4);
    CHECK(wm_get_class_long(&wm, h, -12, &v, true));             /* GCW_HCURSOR */
    CHECK_EQ(v, 0x20);
    CHECK(wm_get_class_long(&wm, h, -14, &v, true));             /* GCW_HICON */
    CHECK_EQ(v, 0x30);
    CHECK(wm_get_class_long(&wm, h, -18, &v, true));             /* GCW_CBWNDEXTRA */
    CHECK_EQ(v, 8);
    CHECK(wm_get_class_long(&wm, h, -24, &v, false));            /* GCL_WNDPROC */
    CHECK_EQ(v, PROC_A);                                          /* the class keeps its own procedure */
    CHECK(wm_get_class_long(&wm, h, -26, &v, true));             /* GCW_STYLE */
    CHECK_EQ(v, 8);
    CHECK(wm_set_class_long(&wm, h, 0, 0xCAFEF00Du, false, &old));
    CHECK_EQ(old, 0);
    CHECK(wm_get_class_long(&wm, h, 2, &v, true));
    CHECK_EQ(v, 0xCAFE);
    CHECK(!wm_set_class_long(&wm, h, 4, 1, true, &old));
    CHECK(wm_set_class_long(&wm, h, -10, 9, true, &old));
    CHECK_EQ(old, 4);
    CHECK(wm_get_class_long(&wm, h, -10, &v, true));
    CHECK_EQ(v, 9);
    wdown();
}

static void test_text(void) {
    uint16_t h;
    printf("test_wm_text\n");
    wboot();
    wclass("W", PROC_A);
    h = wmake("W", WS_OVERLAPPED, 0, 0, 40, 40, 0);
    wm_set_text(&wm, h, "Creative Writer");
    CHECK(!strcmp(wm_get_text(&wm, h), "Creative Writer"));
    wm_set_text(&wm, h, NULL);
    CHECK(!strcmp(wm_get_text(&wm, h), ""));
    CHECK(wm_get_text(&wm, 0x7777) == NULL);
    wdown();
}

static void test_show_enable(void) {
    uint16_t top, ch;
    fb_rect u;
    printf("test_wm_show_enable\n");
    wboot();
    wclass("W", PROC_A);
    top = wmake("W", WS_OVERLAPPEDWINDOW, 10, 10, 200, 150, 0);
    CHECK(!wm_is_visible(&wm, top));
    wclear();
    CHECK(!wm_show_window(&wm, top, SW_SHOWNORMAL));    /* was hidden */
    CHECK(wm_is_visible(&wm, top));
    CHECK(wm_get(&wm, top)->style & WS_VISIBLE);
    CHECK_EQ(wlog[wfind(WM_SHOWWINDOW, top, 0)].wp, 1);
    CHECK(wm_get(&wm, top)->has_update && wm_get(&wm, top)->erase);   /* shown: needs painting */
    CHECK(wm_get_update_rect(&wm, top, &u, false));
    wm_client_rect(&wm, top, &u);
    CHECK(wm_show_window(&wm, top, SW_SHOW));           /* already visible: no new message */
    CHECK_EQ(wcount(WM_SHOWWINDOW), 1);
    /* activation: showing a top-level window activates and focuses it */
    CHECK_EQ(wm.active, top);
    CHECK_EQ(wm.focus, top);
    CHECK(wfind(WM_ACTIVATE, top, 0) >= 0);
    ch = wmake("W", WS_CHILD, 0, 0, 20, 20, top);
    CHECK(!wm_is_visible(&wm, ch));
    wm_show_window(&wm, ch, SW_SHOW);
    CHECK(wm_is_visible(&wm, ch));
    wm_show_window(&wm, top, SW_HIDE);                  /* a visible child of a hidden parent */
    CHECK(!wm_is_visible(&wm, ch));
    CHECK(wm_get(&wm, ch)->style & WS_VISIBLE);
    CHECK(wfind(WM_SHOWWINDOW, top, 2) >= 0);
    CHECK_EQ(wm.active, 0);                             /* nothing else to activate */
    CHECK_EQ(wm.focus, 0);
    /* enable / disable */
    wclear();
    CHECK(!wm_enable_window(&wm, top, false));          /* was enabled: returns old disabled state */
    CHECK(!wm_is_enabled(&wm, top));
    CHECK_EQ(wlog[wfind(WM_ENABLE, top, 0)].wp, 0);
    CHECK(wm_enable_window(&wm, top, false));
    CHECK_EQ(wcount(WM_ENABLE), 1);                     /* no change, no message */
    CHECK(wm_enable_window(&wm, top, true));
    CHECK(wm_is_enabled(&wm, top));
    CHECK_EQ(wlog[wfind(WM_ENABLE, top, 1)].wp, 1);
    wdown();
}

static void test_show_cmds(void) {
    uint16_t top;
    fb_rect r;
    printf("test_wm_show_commands\n");
    wboot();
    wclass("W", PROC_A);
    top = wmake("W", WS_OVERLAPPEDWINDOW, 30, 40, 200, 150, 0);
    wclear();
    wm_show_window(&wm, top, SW_SHOWMAXIMIZED);
    CHECK(wm_get(&wm, top)->style & WS_MAXIMIZE);
    wm_window_rect(&wm, top, &r);
    CHECK(r.left <= 0 && r.top <= 0 && r.right >= 640 && r.bottom >= 480);
    CHECK_EQ(wlog[wfind(WM_SIZE, top, 0)].wp, SIZE_MAXIMIZED);
    wclear();
    wm_show_window(&wm, top, SW_RESTORE);
    CHECK(!(wm_get(&wm, top)->style & WS_MAXIMIZE));
    wm_window_rect(&wm, top, &r);
    CHECK(r.left == 30 && r.top == 40 && r.right == 230 && r.bottom == 190);
    CHECK_EQ(wlog[wfind(WM_SIZE, top, 0)].wp, SIZE_RESTORED);
    wclear();
    wm_show_window(&wm, top, SW_SHOWMINIMIZED);
    CHECK(wm_get(&wm, top)->style & WS_MINIMIZE);
    CHECK_EQ(wlog[wfind(WM_SIZE, top, 0)].wp, SIZE_MINIMIZED);
    wm_show_window(&wm, top, SW_RESTORE);
    CHECK(!(wm_get(&wm, top)->style & WS_MINIMIZE));
    wdown();
}

static void test_move_and_pos(void) {
    uint16_t top;
    fb_rect r;
    printf("test_wm_move_setwindowpos\n");
    wboot();
    wclass("W", PROC_A);
    top = wmake("W", WS_OVERLAPPED | WS_BORDER, 10, 10, 100, 80, 0);
    wm_show_window(&wm, top, SW_SHOWNA);
    wclear();
    CHECK(wm_move_window(&wm, top, 50, 60, 120, 90, true));
    wm_window_rect(&wm, top, &r);
    CHECK(r.left == 50 && r.top == 60 && r.right == 170 && r.bottom == 150);
    CHECK(wcount(WM_MOVE) == 1 && wcount(WM_SIZE) == 1);
    CHECK_EQ(wlog[wfind(WM_SIZE, top, 0)].lp, (uint32_t)(88 << 16 | 118));   /* client size, cy:cx */
    CHECK_EQ(wlog[wfind(WM_MOVE, top, 0)].lp, (uint32_t)(61 << 16 | 51));    /* client origin */
    wclear();
    wm_move_window(&wm, top, 50, 60, 120, 90, true);    /* identical: nothing is sent */
    CHECK_EQ(wnlog, 0);
    wm_set_window_pos(&wm, top, 0, 5, 6, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    wm_window_rect(&wm, top, &r);
    CHECK(r.left == 5 && r.top == 6 && r.right == 125 && r.bottom == 96);
    CHECK_EQ(wcount(WM_SIZE), 0);
    wm_set_window_pos(&wm, top, 0, 0, 0, 40, 30, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    wm_window_rect(&wm, top, &r);
    CHECK(r.left == 5 && r.right == 45 && r.bottom == 36);
    wm_set_window_pos(&wm, top, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_HIDEWINDOW);
    CHECK(!wm_is_visible(&wm, top));
    wm_set_window_pos(&wm, top, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    CHECK(wm_is_visible(&wm, top));
    CHECK(!wm_move_window(&wm, 0x7777, 0, 0, 1, 1, true));
    wdown();
}

/* The application is activated once, when its first top-level window becomes active: WM_ACTIVATEAPP (wParam TRUE) comes
 * before the window's WM_NCACTIVATE/WM_ACTIVATE. Both programs set their 100 ms heartbeat timer (what wakes McZee's
 * speech and animations while the user holds the mouse still) in that message; without it GetMessage never returns on
 * an idle screen. Moving between the program's own windows is not an application switch. */
static void test_activate_app(void) {
    uint16_t a, b;
    int i;
    printf("test_wm_activate_app\n");
    wboot();
    wclass("W", PROC_A);
    a = wmake("W", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0);
    b = wmake("W", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0);
    wclear();
    wm_show_window(&wm, a, SW_SHOWNORMAL);
    CHECK_EQ(wcount(WM_ACTIVATEAPP), 1);
    i = wfind(WM_ACTIVATEAPP, a, 0);
    CHECK(i >= 0);
    CHECK_EQ(wlog[i].wp, 1);                            /* activated */
    CHECK(wfind(WM_NCACTIVATE, a, 0) > i && wfind(WM_ACTIVATE, a, 0) > i);
    wclear();
    wm_show_window(&wm, b, SW_SHOWNORMAL);             /* another window of the same program */
    wm_set_active(&wm, a);
    CHECK_EQ(wcount(WM_ACTIVATEAPP), 0);
    wdown();
}

/* CS_OWNDC: the window has one private DC that GetDC always returns and ReleaseDC never frees (Windows 3.x). Creative
 * Writer's "SplatWindow" (style CS_OWNDC) calls GetDC from its WM_PAINT handler and never ReleaseDC: with a fresh DC per
 * call the 32-entry table filled after 31 repaints, GetDC returned 0 and the story pages stopped drawing (the user's
 * "McZee without a balloon" and the empty sky). */
static void test_own_dc(void) {
    wm_class_def d;
    uint16_t h, d1, other;
    wm_paint ps;
    wm_dc *dc;
    unsigned i;
    fb_rect inv = {5, 5, 25, 15};
    printf("test_wm_own_dc\n");
    wboot();
    memset(&d, 0, sizeof d);
    d.name = "OWN";
    d.wndproc = PROC_A;
    d.hinstance = 0x1111;
    d.style = CS_OWNDC;
    wm_register_class(&wm, &d);
    wclass("W", PROC_A);
    h = wmake("OWN", WS_OVERLAPPED | WS_BORDER | WS_VISIBLE, 20, 30, 100, 80, 0);
    other = wmake("W", WS_OVERLAPPED | WS_BORDER | WS_VISIBLE, 20, 30, 100, 80, 0);
    d1 = wm_get_dc(&wm, h);
    CHECK(d1 != 0);
    for (i = 0; i < 200; i++) CHECK_EQ(wm_get_dc(&wm, h), d1);          /* the same DC every time, nothing leaks */
    CHECK(wm_release_dc(&wm, h, d1));
    CHECK(wm_dc_get(&wm, d1) != NULL);                                   /* ReleaseDC keeps it */
    CHECK_EQ(wm_get_dc(&wm, h), d1);
    /* painting uses it too, clipped to the update region only while painting */
    wm_validate(&wm, h, NULL);
    wm_invalidate(&wm, h, &inv, false);
    CHECK_EQ(wm_begin_paint(&wm, h, &ps), d1);
    dc = wm_dc_get(&wm, d1);
    CHECK(dc != NULL);
    if (dc) { CHECK_EQ(dc->clip.right - dc->clip.left, 20); CHECK_EQ(dc->clip.bottom - dc->clip.top, 10); }
    wm_end_paint(&wm, h, &ps);
    dc = wm_dc_get(&wm, d1);
    CHECK(dc != NULL);
    CHECK_EQ(wm_get_dc(&wm, h), d1);
    dc = wm_dc_get(&wm, d1);
    CHECK(dc && dc->clip.right - dc->clip.left > 20);                    /* the whole client area again */
    /* a class without CS_OWNDC is unchanged: a new DC per call, freed by ReleaseDC */
    { uint16_t a = wm_get_dc(&wm, other), b = wm_get_dc(&wm, other);
      CHECK(a != 0 && b != 0 && a != b);
      wm_release_dc(&wm, other, a);
      CHECK(wm_dc_get(&wm, a) == NULL); wm_release_dc(&wm, other, b); }
    /* destroying the window frees its DC */
    wm_destroy_window(&wm, h);
    CHECK(wm_dc_get(&wm, d1) == NULL);
    wdown();
}

static void test_focus_capture_active(void) {
    uint16_t a, b, c;
    printf("test_wm_focus_capture_active\n");
    wboot();
    wclass("W", PROC_A);
    a = wmake("W", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 100, 100, 0);
    b = wmake("W", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 100, 100, 0);
    wclear();
    CHECK_EQ(wm_set_focus(&wm, a), b);                  /* returns the previous focus */
    CHECK_EQ(wm.focus, a);
    CHECK_EQ(wlog[wfind(WM_KILLFOCUS, b, 0)].wp, a);    /* wParam: window receiving focus */
    CHECK_EQ(wlog[wfind(WM_SETFOCUS, a, 0)].wp, b);     /* wParam: window losing focus */
    CHECK(wfind(WM_KILLFOCUS, b, 0) < wfind(WM_SETFOCUS, a, 0));
    wclear();
    CHECK_EQ(wm_set_focus(&wm, a), a);                  /* no change: no messages */
    CHECK_EQ(wnlog, 0);
    CHECK_EQ(wm_set_focus(&wm, 0), a);                  /* clearing the focus */
    CHECK_EQ(wm.focus, 0);
    CHECK_EQ(wcount(WM_KILLFOCUS), 1);
    CHECK_EQ(wm_set_focus(&wm, 0x7777), 0);             /* invalid handle: ignored */
    /* capture */
    CHECK_EQ(wm_set_capture(&wm, a), 0);
    CHECK_EQ(wm_set_capture(&wm, b), a);
    CHECK_EQ(wm.capture, b);
    wm_release_capture(&wm);
    CHECK_EQ(wm.capture, 0);
    /* activation: WM_ACTIVATE (inactive, then active), previous gets the other window */
    c = wmake("W", WS_OVERLAPPEDWINDOW, 0, 0, 10, 10, 0);
    wm_set_active(&wm, b);
    wclear();
    CHECK_EQ(wm_set_active(&wm, c), b);
    CHECK_EQ(wm.active, c);
    {
        int i = wfind(WM_ACTIVATE, b, 0), j = wfind(WM_ACTIVATE, c, 0);
        CHECK(i >= 0 && j > i);
        CHECK_EQ(wlog[i].wp, 0);                        /* WA_INACTIVE */
        CHECK_EQ(wlog[j].wp, 1);                        /* WA_ACTIVE */
        CHECK_EQ(wlog[i].lp & 0xFFFF, c);               /* LOWORD(lParam): the other window */
    }
    /* destroying the active/focus/capture window clears them */
    wm_set_capture(&wm, c);
    wm_set_focus(&wm, c);
    wm_destroy_window(&wm, c);
    CHECK(wm.focus != c && wm.capture != c && wm.active != c);
    wdown();
}

static void test_paint_region(void) {
    uint16_t h;
    fb_rect rc, u, big = {-10, -10, 1000, 1000};
    printf("test_wm_paint_region\n");
    wboot();
    wclass("W", PROC_A);
    h = wmake("W", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 200, 150, 0);
    wm_client_rect(&wm, h, &rc);
    CHECK(wm_get_update_rect(&wm, h, &u, false));        /* shown: whole client needs painting */
    CHECK(u.left == rc.left && u.right == rc.right && u.bottom == rc.bottom);
    wm_validate(&wm, h, NULL);
    CHECK(!wm_get_update_rect(&wm, h, &u, false));
    CHECK(u.right == 0 && u.bottom == 0);                /* empty on return */
    { fb_rect a = {10, 10, 20, 20}, b = {50, 5, 60, 30};
      wm_invalidate(&wm, h, &a, false);
      wm_invalidate(&wm, h, &b, true);
      CHECK(wm_get_update_rect(&wm, h, &u, false));
      CHECK(u.left == 10 && u.top == 5 && u.right == 60 && u.bottom == 30);   /* bounding box */
      CHECK(wm_get(&wm, h)->erase);
      /* validating a part inside leaves the bounding box alone */
      { fb_rect in = {20, 10, 40, 20}; wm_validate(&wm, h, &in); }
      CHECK(wm_get_update_rect(&wm, h, &u, false));
      CHECK(u.left == 10 && u.right == 60);
      /* a full-height strip on the left trims it */
      { fb_rect strip = {0, 0, 30, 100}; wm_validate(&wm, h, &strip); }
      wm_get_update_rect(&wm, h, &u, false);
      CHECK(u.left == 30 && u.right == 60 && u.top == 5 && u.bottom == 30);
      /* covering all validates */
      { fb_rect all = {0, 0, 100, 100}; wm_validate(&wm, h, &all); }
      CHECK(!wm_get_update_rect(&wm, h, &u, false));
    }
    /* invalidation is clipped to the client area */
    wm_invalidate(&wm, h, &big, false);
    wm_get_update_rect(&wm, h, &u, false);
    CHECK(u.left == rc.left && u.right == rc.right && u.bottom == rc.bottom);
    wm_validate(&wm, h, NULL);
    { fb_rect none = {400, 400, 500, 500};
      wm_invalidate(&wm, h, &none, false);               /* outside the client area: nothing */
      CHECK(!wm_get_update_rect(&wm, h, &u, false)); }
    wdown();
}

static void test_begin_end_paint(void) {
    uint16_t h;
    wm_paint ps;
    wm_dc *dc;
    fb_rect inv = {5, 5, 25, 15}, rc;
    int ox, oy;
    printf("test_wm_begin_end_paint\n");
    wboot();
    wclass("W", PROC_A);
    h = wmake("W", WS_OVERLAPPED | WS_BORDER | WS_VISIBLE, 100, 100, 200, 150, 0);
    wm_validate(&wm, h, NULL);
    wm_invalidate(&wm, h, &inv, true);
    wclear();
    wresult[WM_ERASEBKGND] = 1;                          /* the procedure erased the background */
    ps.hdc = wm_begin_paint(&wm, h, &ps);
    CHECK(ps.hdc != 0);
    CHECK(!ps.erase);
    CHECK(ps.rc.left == 5 && ps.rc.top == 5 && ps.rc.right == 25 && ps.rc.bottom == 15);
    CHECK_EQ(wcount(WM_ERASEBKGND), 1);
    CHECK_EQ(wlog[wfind(WM_ERASEBKGND, h, 0)].wp, ps.hdc);
    CHECK(!wm_get_update_rect(&wm, h, &rc, false));      /* BeginPaint validated it */
    dc = wm_dc_get(&wm, ps.hdc);
    CHECK(dc != NULL);
    wm_client_origin(&wm, h, &ox, &oy);
    CHECK(dc->org_x == ox && dc->org_y == oy);
    CHECK(dc->clip.left == ox + 5 && dc->clip.top == oy + 5 && dc->clip.right == ox + 25 && dc->clip.bottom == oy + 15);
    /* drawing through the DC lands in the framebuffer, clipped to the paint rectangle */
    { fb_rect all = {0, 0, 200, 150}; wm_dc_fill(&wm, ps.hdc, &all, FB_RGB(255, 0, 0)); }
    CHECK_EQ(fb_colorref_at(&wfb, ox + 5, oy + 5), FB_RGB(255, 0, 0));
    CHECK_EQ(fb_colorref_at(&wfb, ox + 24, oy + 14), FB_RGB(255, 0, 0));
    CHECK(fb_colorref_at(&wfb, ox + 4, oy + 5) != FB_RGB(255, 0, 0));
    CHECK(fb_colorref_at(&wfb, ox + 25, oy + 5) != FB_RGB(255, 0, 0));
    wm_end_paint(&wm, h, &ps);
    CHECK(wm_dc_get(&wm, ps.hdc) == NULL);
    /* WM_ERASEBKGND not handled: fErase stays TRUE */
    wm_invalidate(&wm, h, &inv, true);
    wresult[WM_ERASEBKGND] = 0;
    wm_begin_paint(&wm, h, &ps);
    CHECK(ps.erase);
    wm_end_paint(&wm, h, &ps);
    /* no erase flag: no WM_ERASEBKGND */
    wclear();
    wm_invalidate(&wm, h, &inv, false);
    wm_begin_paint(&wm, h, &ps);
    CHECK(!ps.erase);
    CHECK_EQ(wcount(WM_ERASEBKGND), 0);
    wm_end_paint(&wm, h, &ps);
    wdown();
}

static void test_dcs(void) {
    uint16_t h, d1, d2, d3;
    wm_dc *dc;
    int ox, oy;
    fb_rect all = {0, 0, 1000, 1000};
    unsigned i;
    printf("test_wm_dcs\n");
    wboot();
    wclass("W", PROC_A);
    h = wmake("W", WS_OVERLAPPED | WS_BORDER | WS_VISIBLE, 20, 30, 100, 80, 0);
    d1 = wm_get_dc(&wm, h);
    d2 = wm_get_dc(&wm, h);
    CHECK(d1 != 0 && d2 != 0 && d1 != d2);
    dc = wm_dc_get(&wm, d1);
    wm_client_origin(&wm, h, &ox, &oy);
    CHECK(dc->hwnd == h && dc->org_x == ox && dc->org_y == oy);
    CHECK(dc->clip.left == ox && dc->clip.right == ox + 98 && dc->clip.bottom == oy + 78);
    wm_dc_fill(&wm, d1, &all, FB_RGB(0, 255, 0));
    CHECK_EQ(fb_colorref_at(&wfb, ox, oy), FB_RGB(0, 255, 0));
    CHECK_EQ(fb_colorref_at(&wfb, ox + 97, oy + 77), FB_RGB(0, 255, 0));
    CHECK(fb_colorref_at(&wfb, ox + 98, oy) != FB_RGB(0, 255, 0));
    CHECK(fb_colorref_at(&wfb, ox - 1, oy) != FB_RGB(0, 255, 0));
    CHECK(wm_release_dc(&wm, h, d1));
    CHECK(!wm_release_dc(&wm, h, d1));                   /* already released */
    CHECK(wm_release_dc(&wm, h, d2));
    /* the screen DC covers the whole framebuffer */
    d3 = wm_get_dc(&wm, 0);
    dc = wm_dc_get(&wm, d3);
    CHECK(dc->clip.left == 0 && dc->clip.right == 640 && dc->clip.bottom == 480 && dc->org_x == 0);
    wm_release_dc(&wm, 0, d3);
    CHECK(wm_get_dc(&wm, 0x7777) == 0);
    /* table exhaustion fails cleanly */
    for (i = 0; i < WM_MAX_DCS; i++) CHECK(wm_get_dc(&wm, h) != 0);
    CHECK_EQ(wm_get_dc(&wm, h), 0);
    wdown();
}

int main(void) {
    test_classes();
    test_create_destroy();
    test_create_failures();
    test_defaults_and_atom();
    test_geometry();
    test_client_from_nccalcsize();
    test_zorder_and_tree();
    test_owner_and_parent();
    test_words_and_longs();
    test_text();
    test_show_enable();
    test_show_cmds();
    test_move_and_pos();
    test_focus_capture_active();
    test_activate_app();
    test_own_dc();
    test_paint_region();
    test_begin_end_paint();
    test_dcs();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
