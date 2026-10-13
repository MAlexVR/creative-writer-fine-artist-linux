/* Window manager message layer: queue ordering and priorities, timers, quit,
 * dispatch, SendMessage re-entrancy, input routing, TranslateMessage and the
 * default window procedure. */
#include "wmfixture.h"

#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_F1 0x70
#define VK_F10 0x79

static uint16_t one_window(void) {
    wclass("W", PROC_A);
    return wmake("W", WS_OVERLAPPEDWINDOW, 0, 0, 200, 150, 0);
}

static void test_post_get(void) {
    uint16_t a, b;
    wm_msg m;
    printf("test_wmq_post_get\n");
    wboot();
    a = one_window();
    b = wmake("W", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0);
    CHECK(!wm_have_message(&wm));
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));
    wclock = 5000;
    CHECK(wm_post(&wm, a, WM_USER + 1, 11, 0x11111111u));
    CHECK(wm_post(&wm, b, WM_USER + 2, 22, 0x22222222u));
    CHECK(wm_post(&wm, a, WM_USER + 3, 33, 0x33333333u));
    CHECK(wm_have_message(&wm));
    CHECK(!wm_post(&wm, 0x7777, WM_USER, 0, 0));          /* invalid window */
    /* FIFO order; peek does not remove */
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, false));
    CHECK(m.hwnd == a && m.message == WM_USER + 1 && m.wparam == 11 && m.lparam == 0x11111111u);
    CHECK_EQ(m.time, 5000);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK_EQ(m.message, WM_USER + 1);
    /* window filter takes the first message of that window */
    CHECK(wm_next_message(&wm, &m, a, 0, 0, true));
    CHECK(m.hwnd == a && m.message == WM_USER + 3);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.hwnd == b);
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));
    /* message range filter */
    wm_post(&wm, a, WM_USER + 5, 0, 0);
    wm_post(&wm, a, WM_USER + 6, 0, 0);
    wm_post(&wm, a, WM_USER + 7, 0, 0);
    CHECK(wm_next_message(&wm, &m, 0, WM_USER + 6, WM_USER + 6, true));
    CHECK_EQ(m.message, WM_USER + 6);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK_EQ(m.message, WM_USER + 5);
    CHECK(!wm_next_message(&wm, &m, b, 0, 0, true));       /* nothing for b */
    CHECK(wm_next_message(&wm, &m, a, WM_USER + 7, 0xFFFF, true));
    /* last retrieved message is remembered (GetMessageTime / GetMessagePos) */
    CHECK_EQ(wm.last_msg.message, WM_USER + 7);
    /* the queue is bounded */
    {
        unsigned i, ok = 0;
        for (i = 0; i < WM_QUEUE_SIZE + 10; i++) ok += wm_post(&wm, a, WM_USER, 0, 0);
        CHECK_EQ(ok, WM_QUEUE_SIZE);
    }
    wdown();
}

static void test_priorities(void) {
    uint16_t h;
    wm_msg m;
    wm_input ev;
    printf("test_wmq_priorities\n");
    wboot();
    h = one_window();
    wm_show_window(&wm, h, SW_SHOWNA);                      /* needs painting */
    wm_validate(&wm, h, NULL);
    wm_invalidate(&wm, h, NULL, false);                     /* WM_PAINT pending */
    wm_set_timer(&wm, h, 7, 100, 0);
    wclock += 500;                                          /* the timer is due */
    wm_post_quit(&wm, 3);
    memset(&ev, 0, sizeof ev);
    ev.kind = WM_IN_KEY_DOWN;
    ev.vk = 'A';
    wm_set_focus(&wm, h);
    wm_input_event(&wm, &ev);                               /* input queue */
    wm_post(&wm, h, WM_USER, 0, 0);                         /* posted queue */
    /* posted, input, quit, paint, timer */
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true)); CHECK_EQ(m.message, WM_USER);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true)); CHECK_EQ(m.message, WM_KEYDOWN);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true)); CHECK_EQ(m.message, WM_QUIT); CHECK_EQ(m.wparam, 3);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true)); CHECK_EQ(m.message, WM_PAINT); CHECK_EQ(m.hwnd, h);
    wm_validate(&wm, h, NULL);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true)); CHECK_EQ(m.message, WM_TIMER);
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));
    /* WM_QUIT is delivered once */
    CHECK(!wm_have_message(&wm));
    wdown();
}

static void test_paint_messages(void) {
    uint16_t a, b;
    wm_msg m;
    fb_rect r = {0, 0, 5, 5};
    printf("test_wmq_paint\n");
    wboot();
    a = one_window();
    b = wmake("W", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0);
    /* hidden windows are not painted even when invalidated */
    wm_invalidate(&wm, a, &r, true);
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));
    wm_show_window(&wm, a, SW_SHOWNA);
    wm_show_window(&wm, b, SW_SHOWNA);
    wm_validate(&wm, b, NULL);
    wclear();
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, false));        /* peek without removing */
    CHECK(m.message == WM_PAINT && m.hwnd == a);
    CHECK_EQ(wcount(WM_NCPAINT), 0);                        /* peeking sends nothing */
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(wm_get(&wm, a)->has_update);                      /* retrieving does not validate */
    CHECK_EQ(wcount(WM_NCPAINT), 1);                        /* the frame is painted first */
    CHECK_EQ(wlog[wfind(WM_NCPAINT, a, 0)].wp, 1);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true) && m.message == WM_PAINT);   /* still pending */
    CHECK(!wm_next_message(&wm, &m, b, 0, 0, true));        /* filter: b has nothing */
    CHECK(!wm_next_message(&wm, &m, 0, WM_USER, WM_USER + 4, true));   /* range excludes WM_PAINT */
    wm_validate(&wm, a, NULL);
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));
    /* UpdateWindow sends WM_PAINT straight to the procedure */
    wm_invalidate(&wm, b, &r, false);
    wclear();
    wm_update_window(&wm, b);
    CHECK_EQ(wcount(WM_PAINT), 1);
    CHECK_EQ(wlog[wfind(WM_PAINT, b, 0)].lp, 0);
    wm_validate(&wm, b, NULL);
    wclear();
    wm_update_window(&wm, b);                               /* nothing to paint now */
    CHECK_EQ(wcount(WM_PAINT), 0);
    /* UpdateWindow(parent) also paints the children with an update region (Creative Writer invalidates its splash
     * child and updates the main window, then loads for seconds without a message loop) */
    {
        uint16_t kid = wmake("W", WS_CHILD | WS_VISIBLE, 10, 10, 50, 50, b);
        wm_show_window(&wm, kid, SW_SHOWNA);
        wm_validate(&wm, b, NULL);
        wm_invalidate(&wm, kid, &r, false);
        wclear();
        wm_update_window(&wm, b);
        CHECK_EQ(wcount(WM_PAINT), 1);
        CHECK(wfind(WM_PAINT, kid, 0) >= 0);
        wm_validate(&wm, kid, NULL);                        /* (the test procedure does not call BeginPaint) */
        wclear();
        wm_update_window(&wm, b);
        CHECK_EQ(wcount(WM_PAINT), 0);
    }
    wdown();
}

static void test_timers(void) {
    uint16_t h, t1, t2;
    wm_msg m;
    printf("test_wmq_timers\n");
    wboot();
    h = one_window();
    t1 = wm_set_timer(&wm, h, 5, 100, 0);
    CHECK_EQ(t1, 5);
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));        /* not due yet */
    wclock += 99;
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));
    wclock += 1;
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, false));        /* peek: due, stays due */
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.hwnd == h && m.message == WM_TIMER && m.wparam == 5 && m.lparam == 0);
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));        /* re-armed for +100 */
    wclock += 1000;                                         /* long delay: one message, no backlog */
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));
    /* same id replaces */
    CHECK_EQ(wm_set_timer(&wm, h, 5, 50, 0), 5);
    wclock += 50;
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(wm_kill_timer(&wm, h, 5));
    CHECK(!wm_kill_timer(&wm, h, 5));
    wclock += 1000;
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));
    /* hwnd 0 timers get an id and carry the TIMERPROC */
    t1 = wm_set_timer(&wm, 0, 0, 10, 0xAAAA0001u);
    t2 = wm_set_timer(&wm, 0, 0, 10, 0xBBBB0002u);
    CHECK(t1 != 0 && t2 != 0 && t1 != t2);
    wclock += 10;
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.hwnd == 0 && m.message == WM_TIMER && m.wparam == t1 && m.lparam == 0xAAAA0001u);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.wparam == t2);
    /* with a filter only matching timers */
    wm_set_timer(&wm, h, 9, 10, 0);
    wclock += 10;
    CHECK(wm_next_message(&wm, &m, h, 0, 0, true));
    CHECK_EQ(m.wparam, 9);
    CHECK(wm_kill_timer(&wm, 0, t1));
    CHECK(wm_kill_timer(&wm, 0, t2));
    /* the table is bounded and a window's timers die with it */
    {
        unsigned i, ok = 0;
        for (i = 1; i <= WM_MAX_TIMERS + 4; i++) ok += wm_set_timer(&wm, h, (uint16_t)(100 + i), 10, 0) != 0;
        CHECK_EQ(ok, WM_MAX_TIMERS - 1);                    /* timer 9 still occupies a slot */
    }
    wm_destroy_window(&wm, h);
    wclock += 100;
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));
    wdown();
}

static void test_dispatch(void) {
    uint16_t h;
    wm_msg m;
    printf("test_wmq_dispatch\n");
    wboot();
    h = one_window();
    wclear();
    m.hwnd = h; m.message = WM_USER + 9; m.wparam = 4; m.lparam = 0x01020304u;
    wresult[WM_USER + 9] = 0x55;
    CHECK_EQ(wm_dispatch(&wm, &m), 0x55);
    CHECK(wlog[0].proc == PROC_A && wlog[0].hwnd == h && wlog[0].msg == WM_USER + 9 && wlog[0].lp == 0x01020304u);
    /* a message without a window is dropped */
    wclear();
    m.hwnd = 0;
    wm_dispatch(&wm, &m);
    m.hwnd = 0x7777;
    wm_dispatch(&wm, &m);
    CHECK_EQ(wnlog, 0);
    /* WM_TIMER with a TIMERPROC calls it instead of the window procedure */
    m.hwnd = h; m.message = WM_TIMER; m.wparam = 3; m.lparam = 0xCAFE0010u;
    wclock = 12345;
    wm_dispatch(&wm, &m);
    CHECK(wlog[0].proc == 0xCAFE0010u && wlog[0].hwnd == h && wlog[0].msg == WM_TIMER && wlog[0].wp == 3);
    CHECK_EQ(wlog[0].lp, 12345);                           /* dwTime */
    m.hwnd = 0;                                            /* thread timer: no window needed */
    wclear();
    wm_dispatch(&wm, &m);
    CHECK_EQ(wnlog, 1);
    wdown();
}

static uint32_t reenter_hook(wm_t *w, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp) {
    (void)wp; (void)lp;
    if (msg == WM_USER) {
        wm_post(w, hwnd, WM_USER + 2, 0, 0);               /* posted: must not run inside */
        return wm_send(w, (uint16_t)(hwnd + 1), WM_USER + 1, 0, 0) + 1;
    }
    return 100;
}

static void test_send_reentrancy(void) {
    uint16_t a, b;
    wm_msg m;
    uint32_t r;
    printf("test_wmq_send_reentrancy\n");
    wboot();
    a = one_window();
    b = wmake("W", WS_OVERLAPPEDWINDOW, 0, 0, 10, 10, 0);
    CHECK_EQ(b, a + 1);
    wclear();
    wresult[WM_USER + 1] = 100;
    whook = reenter_hook;
    r = wm_send(&wm, a, WM_USER, 0, 0);
    CHECK_EQ(wnlog, 2);                                    /* a then b, nested */
    CHECK(wlog[0].hwnd == a && wlog[1].hwnd == b);
    CHECK_EQ(r, 100 + 1);                                  /* b's result flows back through a */
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));        /* the post waits in the queue */
    CHECK_EQ(m.message, WM_USER + 2);
    /* SendMessage to a destroyed window returns 0 */
    wm_destroy_window(&wm, b);
    CHECK_EQ(wm_send(&wm, b, WM_USER, 0, 0), 0);
    wdown();
}

static void test_destroy_drops_messages(void) {
    uint16_t a, b;
    wm_msg m;
    printf("test_wmq_destroy_drops\n");
    wboot();
    a = one_window();
    b = wmake("W", WS_OVERLAPPEDWINDOW, 0, 0, 10, 10, 0);
    wm_post(&wm, a, WM_USER, 0, 0);
    wm_post(&wm, b, WM_USER + 1, 0, 0);
    wm_post(&wm, a, WM_USER + 2, 0, 0);
    wm_destroy_window(&wm, a);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.hwnd == b && m.message == WM_USER + 1);
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));
    /* a message already retrieved for a destroyed window is dropped by dispatch */
    wclear();
    m.hwnd = a; m.message = WM_USER; m.wparam = 0; m.lparam = 0;
    wm_dispatch(&wm, &m);
    CHECK_EQ(wnlog, 0);
    wdown();
}

static void test_broadcast(void) {
    uint16_t a, b, c;
    wm_msg m;
    unsigned n = 0;
    printf("test_wmq_broadcast\n");
    wboot();
    a = one_window();
    b = wmake("W", WS_OVERLAPPEDWINDOW, 0, 0, 10, 10, 0);
    c = wmake("W", WS_CHILD, 0, 0, 10, 10, a);
    CHECK(wm_post(&wm, 0xFFFF, WM_USER, 1, 2));
    while (wm_next_message(&wm, &m, 0, 0, 0, true)) {
        n++;
        CHECK(m.hwnd == a || m.hwnd == b);                 /* top-level windows only */
        CHECK(m.hwnd != c);
    }
    CHECK_EQ(n, 2);
    wdown();
}

/* ---- input ---------------------------------------------------------------- */

static uint16_t input_window(void) {
    uint16_t h;
    wclass("W", PROC_A);
    h = wmake("W", WS_OVERLAPPED | WS_BORDER | WS_CAPTION | WS_VISIBLE, 100, 100, 200, 150, 0);
    wm_validate(&wm, h, NULL);
    wclear();
    return h;
}

static wm_input mouse(wm_in_kind k, int x, int y, int button) {
    wm_input e;
    memset(&e, 0, sizeof e);
    e.kind = k; e.x = x; e.y = y; e.button = button;
    return e;
}

static void test_mouse(void) {
    uint16_t h, ch;
    wm_msg m;
    wm_input e;
    printf("test_wmq_mouse\n");
    wboot();
    h = input_window();                                    /* client origin (101,120), 198x129 */
    /* the fake procedure answers WM_NCHITTEST with HTCLIENT */
    wresult[WM_NCHITTEST] = HTCLIENT;
    e = mouse(WM_IN_MOVE, 150, 150, 0);
    wm_input_event(&wm, &e);
    CHECK_EQ(wm.cursor_x, 150);
    CHECK(wfind(WM_NCHITTEST, h, 0) >= 0);
    CHECK_EQ(wlog[wfind(WM_NCHITTEST, h, 0)].lp, (uint32_t)(150 << 16 | 150));   /* screen coordinates */
    CHECK(wfind(WM_SETCURSOR, h, 0) >= 0);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.hwnd == h && m.message == WM_MOUSEMOVE && m.wparam == 0);
    CHECK_EQ(m.lparam, (uint32_t)((150 - 120) << 16 | (150 - 101)));             /* client coordinates */
    CHECK_EQ(m.pt_x, 150);
    /* consecutive moves coalesce */
    e = mouse(WM_IN_MOVE, 151, 150, 0); wm_input_event(&wm, &e);
    e = mouse(WM_IN_MOVE, 152, 150, 0); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK_EQ(m.lparam & 0xFFFF, 152 - 101);
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));
    /* buttons: flags in wParam, state tracked */
    e = mouse(WM_IN_BUTTON_DOWN, 150, 150, 0); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.message == WM_LBUTTONDOWN && m.wparam == 1);
    CHECK(wm_key_state(&wm, 1) < 0);                       /* VK_LBUTTON is down */
    e = mouse(WM_IN_MOVE, 160, 150, 0); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true) && m.wparam == 1);   /* MK_LBUTTON while dragging */
    e = mouse(WM_IN_BUTTON_UP, 160, 150, 0); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.message == WM_LBUTTONUP && m.wparam == 0);
    e = mouse(WM_IN_BUTTON_DOWN, 150, 150, 1); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true) && m.message == WM_RBUTTONDOWN && m.wparam == 2);
    e = mouse(WM_IN_BUTTON_UP, 150, 150, 1); wm_input_event(&wm, &e);
    wm_next_message(&wm, &m, 0, 0, 0, true);
    e = mouse(WM_IN_BUTTON_DOWN, 150, 150, 2); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true) && m.message == WM_MBUTTONDOWN && m.wparam == 0x10);
    e = mouse(WM_IN_BUTTON_UP, 150, 150, 2); wm_input_event(&wm, &e);
    wm_next_message(&wm, &m, 0, 0, 0, true);
    /* shift and control show in wParam */
    { wm_input k; memset(&k, 0, sizeof k); k.kind = WM_IN_KEY_DOWN; k.vk = VK_SHIFT; wm_input_event(&wm, &k);
      k.vk = VK_CONTROL; wm_input_event(&wm, &k);
      while (wm_next_message(&wm, &m, 0, WM_KEYDOWN, WM_KEYDOWN, true)) {} }
    e = mouse(WM_IN_BUTTON_DOWN, 150, 150, 0); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, WM_LBUTTONDOWN, WM_LBUTTONDOWN, true));
    CHECK_EQ(m.wparam, 1 | 4 | 8);
    e = mouse(WM_IN_BUTTON_UP, 150, 150, 0); wm_input_event(&wm, &e);
    { wm_input k; memset(&k, 0, sizeof k); k.kind = WM_IN_KEY_UP; k.vk = VK_SHIFT; wm_input_event(&wm, &k);
      k.vk = VK_CONTROL; wm_input_event(&wm, &k); }
    wdrain();
    /* capture sends everything to the capturing window, in its client coordinates */
    ch = wmake("W", WS_CHILD | WS_VISIBLE, 10, 10, 50, 50, h);
    wm_validate(&wm, ch, NULL);
    wm_set_capture(&wm, ch);
    wclear();
    e = mouse(WM_IN_MOVE, 500, 400, 0); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.hwnd == ch && m.message == WM_MOUSEMOVE);
    CHECK_EQ((int16_t)(m.lparam & 0xFFFF), 500 - 111);
    CHECK_EQ((int16_t)(m.lparam >> 16), 400 - 130);
    CHECK_EQ(wcount(WM_NCHITTEST), 0);
    wm_release_capture(&wm);
    /* the deepest child under the pointer is the target */
    e = mouse(WM_IN_MOVE, 120, 140, 0); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true) && m.hwnd == ch);
    /* the desktop gets nothing */
    wclear();
    e = mouse(WM_IN_MOVE, 5, 5, 0); wm_input_event(&wm, &e);
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));
    wdown();
}

static void test_mouse_nonclient_and_activation(void) {
    uint16_t a, b;
    wm_msg m;
    wm_input e;
    printf("test_wmq_mouse_nonclient\n");
    wboot();
    wclass("W", PROC_A);
    wdef_through = true;                                   /* real WM_NCHITTEST from the default procedure */
    a = wmake("W", WS_OVERLAPPED | WS_BORDER | WS_CAPTION | WS_VISIBLE, 0, 0, 200, 150, 0);
    b = wmake("W", WS_OVERLAPPED | WS_BORDER | WS_CAPTION | WS_VISIBLE, 300, 0, 200, 150, 0);
    CHECK_EQ(wm.active, b);
    wdrain();
    e = mouse(WM_IN_BUTTON_DOWN, 50, 5, 0);                /* caption of a */
    wm_input_event(&wm, &e);
    CHECK_EQ(wm.active, a);                                /* clicking activates */
    CHECK(wm_next_message(&wm, &m, a, 0, 0, true));
    CHECK(m.message == WM_NCLBUTTONDOWN && m.wparam == HTCAPTION);
    CHECK_EQ(m.lparam, (uint32_t)(5 << 16 | 50));          /* screen coordinates */
    e = mouse(WM_IN_BUTTON_UP, 50, 5, 0); wm_input_event(&wm, &e);
    e = mouse(WM_IN_MOVE, 0, 70, 0); wm_input_event(&wm, &e);     /* left border */
    wm_next_message(&wm, &m, a, WM_NCMOUSEMOVE, WM_NCMOUSEMOVE, true);
    CHECK_EQ(m.message, WM_NCMOUSEMOVE);
    CHECK_EQ(m.wparam, HTBORDER);
    /* disabled windows get no mouse input */
    wdrain();
    wm_enable_window(&wm, a, false);
    e = mouse(WM_IN_BUTTON_DOWN, 50, 70, 0); wm_input_event(&wm, &e);
    CHECK(!wm_next_message(&wm, &m, a, 0, 0, true));
    wdown();
}

static void test_double_click(void) {
    uint16_t h;
    wm_msg m;
    wm_input e;
    printf("test_wmq_double_click\n");
    wboot();
    wclass("W", PROC_A);
    {
        wm_class_def d;
        memset(&d, 0, sizeof d);
        d.name = "DC"; d.wndproc = PROC_A; d.style = CS_DBLCLKS; d.hinstance = 0x1111;
        wm_register_class(&wm, &d);
    }
    wresult[WM_NCHITTEST] = HTCLIENT;
    h = wmake("DC", WS_POPUP | WS_VISIBLE, 0, 0, 200, 150, 0);
    wm_validate(&wm, h, NULL);
    wdrain();
    e = mouse(WM_IN_BUTTON_DOWN, 10, 10, 0); wm_input_event(&wm, &e);
    e = mouse(WM_IN_BUTTON_UP, 10, 10, 0); wm_input_event(&wm, &e);
    wclock += 200;
    e = mouse(WM_IN_BUTTON_DOWN, 11, 11, 0); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, WM_LBUTTONDOWN, WM_LBUTTONDOWN, true));
    CHECK(wm_next_message(&wm, &m, 0, WM_LBUTTONUP, WM_LBUTTONUP, true));
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK_EQ(m.message, WM_LBUTTONDBLCLK);
    e = mouse(WM_IN_BUTTON_UP, 11, 11, 0); wm_input_event(&wm, &e);
    wclock += 200;                                         /* a third click is a plain one again */
    wdrain();
    e = mouse(WM_IN_BUTTON_DOWN, 11, 11, 0); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK_EQ(m.message, WM_LBUTTONDOWN);
    e = mouse(WM_IN_BUTTON_UP, 11, 11, 0); wm_input_event(&wm, &e);
    wclock += 1000;                                        /* too slow */
    e = mouse(WM_IN_BUTTON_DOWN, 11, 11, 0); wm_input_event(&wm, &e);
    e = mouse(WM_IN_BUTTON_UP, 11, 11, 0); wm_input_event(&wm, &e);
    wclock += 1000;
    e = mouse(WM_IN_BUTTON_DOWN, 11, 11, 0); wm_input_event(&wm, &e);
    {
        unsigned dbl = 0;
        while (wm_next_message(&wm, &m, 0, 0, 0, true)) dbl += m.message == WM_LBUTTONDBLCLK;
        CHECK_EQ(dbl, 0);
    }
    /* a class without CS_DBLCLKS never sees one */
    wdown();
}

static wm_input key(wm_in_kind k, unsigned vk) {
    wm_input e;
    memset(&e, 0, sizeof e);
    e.kind = k; e.vk = vk; e.scan = vk & 0x7F;
    return e;
}

static void test_keyboard(void) {
    uint16_t a, b;
    wm_msg m;
    wm_input e;
    printf("test_wmq_keyboard\n");
    wboot();
    a = one_window();
    wm_show_window(&wm, a, SW_SHOWNORMAL);
    b = wmake("W", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, a);
    wdrain();
    e = key(WM_IN_KEY_DOWN, 'A');
    wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.hwnd == a && m.message == WM_KEYDOWN && m.wparam == 'A');
    CHECK_EQ(m.lparam, (uint32_t)(1 | ('A' & 0x7F) << 16));
    CHECK(wm_key_state(&wm, 'A') < 0);
    CHECK(wm_async_key_state(&wm, 'A') < 0);
    e.repeat = true;
    wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.lparam & 0x40000000u);                         /* previous state bit */
    e = key(WM_IN_KEY_UP, 'A');
    wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.message == WM_KEYUP && (m.lparam & 0xC0000000u) == 0xC0000000u);
    CHECK_EQ(wm_key_state(&wm, 'A'), 0);
    /* keys go to the focus window */
    wm_set_focus(&wm, b);
    e = key(WM_IN_KEY_DOWN, 'B'); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true) && m.hwnd == b);
    e = key(WM_IN_KEY_UP, 'B'); wm_input_event(&wm, &e);
    wm_next_message(&wm, &m, 0, 0, 0, true);
    /* with Alt held, or F10: WM_SYSKEYDOWN with the context bit */
    e = key(WM_IN_KEY_DOWN, VK_MENU); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true) && m.message == WM_SYSKEYDOWN);
    e = key(WM_IN_KEY_DOWN, 'X'); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.message == WM_SYSKEYDOWN && (m.lparam & 0x20000000u));
    e = key(WM_IN_KEY_UP, 'X'); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true) && m.message == WM_SYSKEYUP);
    e = key(WM_IN_KEY_UP, VK_MENU); wm_input_event(&wm, &e);
    wm_next_message(&wm, &m, 0, 0, 0, true);
    e = key(WM_IN_KEY_DOWN, VK_F10); wm_input_event(&wm, &e);
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true) && m.message == WM_SYSKEYDOWN);
    /* toggle keys */
    CHECK_EQ(wm_key_state(&wm, VK_CAPITAL) & 1, 0);
    e = key(WM_IN_KEY_DOWN, VK_CAPITAL); wm_input_event(&wm, &e);
    e = key(WM_IN_KEY_UP, VK_CAPITAL); wm_input_event(&wm, &e);
    CHECK_EQ(wm_key_state(&wm, VK_CAPITAL) & 1, 1);
    CHECK(wm_key_state(&wm, VK_CAPITAL) >= 0);             /* toggled but not held */
    /* no focus and no active window: dropped */
    wdrain();
    wm_destroy_window(&wm, a);
    e = key(WM_IN_KEY_DOWN, 'Q'); wm_input_event(&wm, &e);
    CHECK(!wm_next_message(&wm, &m, 0, 0, 0, true));
    wdown();
}

static void test_quit_event(void) {
    uint16_t a;
    wm_msg m;
    wm_input e;
    printf("test_wmq_quit_event\n");
    wboot();
    a = one_window();
    wm_show_window(&wm, a, SW_SHOWNORMAL);
    wdrain();
    memset(&e, 0, sizeof e);
    e.kind = WM_IN_QUIT;
    wm_input_event(&wm, &e);                               /* closing the host window: Close from the system menu */
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.hwnd == a && m.message == WM_SYSCOMMAND && m.wparam == 0xF060);   /* SC_CLOSE: DefWindowProc turns it into WM_CLOSE */
    wm_destroy_window(&wm, a);
    wm_input_event(&wm, &e);                               /* nothing to close: quit the loop */
    CHECK(wm_next_message(&wm, &m, 0, 0, 0, true));
    CHECK(m.message == WM_QUIT);
    wdown();
}

static void test_translate(void) {
    uint16_t a;
    wm_msg m, out;
    struct { unsigned vk; bool shift, ctrl, caps; unsigned want; } t[] = {
        {'A', false, false, false, 'a'}, {'A', true, false, false, 'A'}, {'A', false, false, true, 'A'},
        {'A', true, false, true, 'a'}, {'A', false, true, false, 1}, {'Z', false, true, false, 26},
        {'1', false, false, false, '1'}, {'1', true, false, false, '!'}, {'0', true, false, false, ')'},
        {0x20, false, false, false, ' '}, {VK_RETURN, false, false, false, '\r'},
        {VK_BACK, false, false, false, 8}, {VK_TAB, false, false, false, 9},
        {VK_ESCAPE, false, false, false, 27}, {0xBC, false, false, false, ','},
        {0xBC, true, false, false, '<'}, {0xBF, true, false, false, '?'}, {0xDE, true, false, false, '"'},
        {0x61, false, false, false, '1'}, {0x6A, false, false, false, '*'},
    };
    size_t i;
    printf("test_wmq_translate\n");
    wboot();
    a = one_window();
    wm_show_window(&wm, a, SW_SHOWNORMAL);
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        memset(wm.key_down, 0, sizeof wm.key_down);
        memset(wm.key_toggle, 0, sizeof wm.key_toggle);
        wm.keys_valid = false;                              /* the table sets the live state directly */
        if (t[i].shift) wm.key_down[VK_SHIFT] = 1;
        if (t[i].ctrl) wm.key_down[VK_CONTROL] = 1;
        if (t[i].caps) wm.key_toggle[VK_CAPITAL] = 1;
        m.hwnd = a; m.message = WM_KEYDOWN; m.wparam = (uint16_t)t[i].vk; m.lparam = 0x00020001u + (uint32_t)i;
        wdrain();
        CHECK(wm_translate(&wm, &m));
        CHECK(wm_next_message(&wm, &out, 0, 0, 0, true));
        CHECK(out.hwnd == a && out.message == WM_CHAR);
        CHECK_EQ(out.wparam, t[i].want);
        CHECK_EQ(out.lparam, m.lparam);
    }
    memset(wm.key_down, 0, sizeof wm.key_down);
    memset(wm.key_toggle, 0, sizeof wm.key_toggle);
    wdrain();
    m.wparam = VK_F1;                                       /* no character */
    CHECK(!wm_translate(&wm, &m));
    CHECK(!wm_next_message(&wm, &out, 0, 0, 0, true));
    m.message = WM_KEYUP; m.wparam = 'A';                   /* only key-downs translate */
    CHECK(!wm_translate(&wm, &m));
    m.message = WM_SYSKEYDOWN; m.wparam = 'X';              /* Alt+X: WM_SYSCHAR */
    CHECK(wm_translate(&wm, &m));
    CHECK(wm_next_message(&wm, &out, 0, 0, 0, true) && out.message == WM_SYSCHAR && out.wparam == 'x');
    m.message = WM_USER;
    CHECK(!wm_translate(&wm, &m));
    wdown();
}

static void test_defproc(void) {
    uint16_t a, c;
    wm_paint ps;
    uint32_t r;
    fb_rect inv = {0, 0, 10, 10};
    int ox, oy;
    printf("test_wmq_defproc\n");
    wboot();
    wclass("W", 0);
    wm_class_def d;
    memset(&d, 0, sizeof d);
    d.name = "NOBRUSH"; d.hinstance = 0x1111;
    wm_register_class(&wm, &d);
    a = wmake("W", WS_OVERLAPPED | WS_BORDER | WS_VISIBLE, 50, 50, 100, 100, 0);   /* brush COLOR_WINDOW + 1 */
    CHECK_EQ(wm_defproc(&wm, a, WM_NCCREATE, 0, 0), 1);
    CHECK_EQ(wm_defproc(&wm, a, WM_CREATE, 0, 0), 0);
    CHECK_EQ(wm_defproc(&wm, a, WM_NCACTIVATE, 1, 0), 1);
    /* WM_PAINT validates; WM_ERASEBKGND fills with the class brush colour */
    wm_validate(&wm, a, NULL);
    wm_invalidate(&wm, a, &inv, true);
    wm_client_origin(&wm, a, &ox, &oy);
    fb_fill_rect(&wfb, &(fb_rect){0, 0, 640, 480}, FB_RGB(255, 0, 0));
    wm_defproc(&wm, a, WM_PAINT, 0, 0);
    CHECK(!wm_get_update_rect(&wm, a, NULL, false));
    CHECK_EQ(fb_colorref_at(&wfb, ox, oy), wm_sys_color(COLOR_WINDOW));
    CHECK_EQ(fb_colorref_at(&wfb, ox + 9, oy + 9), wm_sys_color(COLOR_WINDOW));
    CHECK_EQ(fb_colorref_at(&wfb, ox + 10, oy), FB_RGB(255, 0, 0));
    /* background brush 0: not erased */
    c = wmake("NOBRUSH", WS_POPUP | WS_VISIBLE, 300, 300, 50, 50, 0);
    ps.hdc = wm_begin_paint(&wm, c, &ps);
    CHECK_EQ(wm_defproc(&wm, c, WM_ERASEBKGND, ps.hdc, 0), 0);
    wm_end_paint(&wm, c, &ps);
    /* hit testing */
    CHECK_EQ((int16_t)wm_defproc(&wm, a, WM_NCHITTEST, 0, (uint32_t)(60 << 16 | 60)), HTCLIENT);
    CHECK_EQ((int16_t)wm_defproc(&wm, a, WM_NCHITTEST, 0, (uint32_t)(5 << 16 | 5)), HTNOWHERE);
    CHECK_EQ((int16_t)wm_defproc(&wm, a, WM_NCHITTEST, 0, (uint32_t)(50 << 16 | 50)), HTBORDER);
    /* WM_ACTIVATE gives the window the focus */
    wm_set_focus(&wm, c);
    wm_defproc(&wm, a, WM_ACTIVATE, 1, 0);
    CHECK_EQ(wm.focus, a);
    /* SC_CLOSE becomes WM_CLOSE, which destroys the window */
    r = wm_defproc(&wm, a, WM_SYSCOMMAND, 0xF060, 0);
    CHECK_EQ(r, 0);
    CHECK(!wm_is_window(&wm, a));
    CHECK_EQ(wm_defproc(&wm, 0x7777, WM_PAINT, 0, 0), 0);
    wdown();
}

/* GetKeyState/TranslateMessage see the keyboard as it was when the message was retrieved, not the state after
 * every queued event: Shift down, A down, A up, Shift up queued in one go still types a capital A. */
static void test_key_state_follows_the_message(void) {
    wm_input e;
    wm_msg m, out;
    uint16_t a;
    static const struct { wm_in_kind kind; unsigned vk; } seq[] = {
        {WM_IN_KEY_DOWN, VK_SHIFT}, {WM_IN_KEY_DOWN, 'A'}, {WM_IN_KEY_UP, 'A'}, {WM_IN_KEY_UP, VK_SHIFT},
        {WM_IN_KEY_DOWN, 'B'}, {WM_IN_KEY_UP, 'B'}};
    size_t i;
    printf("test_wmq_key_state_follows_the_message\n");
    wboot();
    a = one_window();
    wm_show_window(&wm, a, SW_SHOWNORMAL);
    wm_set_focus(&wm, a);
    wdrain();
    for (i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        memset(&e, 0, sizeof e);
        e.kind = seq[i].kind;
        e.vk = seq[i].vk;
        wm_input_event(&wm, &e);
    }
    CHECK_EQ(wm_key_state(&wm, VK_SHIFT) & 0x8000, 0);       /* live state: everything is released */
    CHECK(wm_next_message(&wm, &m, 0, 0x100, 0x109, true));  /* Shift down */
    CHECK(wm_next_message(&wm, &m, 0, 0x100, 0x109, true));  /* A down */
    CHECK_EQ(m.wparam, 'A');
    CHECK_EQ(wm_key_state(&wm, VK_SHIFT) & 0x8000, 0x8000);
    CHECK(wm_translate(&wm, &m));
    CHECK(wm_next_message(&wm, &out, 0, WM_CHAR, WM_CHAR, true));
    CHECK_EQ(out.wparam, 'A');
    CHECK(wm_next_message(&wm, &m, 0, 0x100, 0x109, true));  /* A up */
    CHECK(wm_next_message(&wm, &m, 0, 0x100, 0x109, true));  /* Shift up */
    CHECK(wm_next_message(&wm, &m, 0, 0x100, 0x109, true));  /* B down: Shift is up again */
    CHECK_EQ(m.wparam, 'B');
    CHECK_EQ(wm_key_state(&wm, VK_SHIFT) & 0x8000, 0);
    CHECK(wm_translate(&wm, &m));
    CHECK(wm_next_message(&wm, &out, 0, WM_CHAR, WM_CHAR, true));
    CHECK_EQ(out.wparam, 'b');
    wdown();
}

int main(void) {
    test_post_get();
    test_priorities();
    test_paint_messages();
    test_timers();
    test_dispatch();
    test_send_reentrancy();
    test_destroy_drops_messages();
    test_broadcast();
    test_mouse();
    test_mouse_nonclient_and_activation();
    test_double_click();
    test_keyboard();
    test_quit_event();
    test_translate();
    test_key_state_follows_the_message();
    test_defproc();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
