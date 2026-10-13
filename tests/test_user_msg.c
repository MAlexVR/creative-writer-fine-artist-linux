#define _POSIX_C_SOURCE 200809L
/* USER HLE, message side: the message loop through real guest code, posted and
 * hardware messages, timers, focus/capture/cursor/keyboard state, the paint
 * API, rectangle helpers, wsprintf, character helpers, MessageBox and stubs. */
#include "ufixture.h"

#define U_CODE_E 0x900   /* WM_PAINT: BeginPaint/EndPaint on a PAINTSTRUCT on its stack */
#define U_CODE_LOOP 0xA00 /* while (GetMessage(&msg, 0, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); } HLT */
#define MSG_OFF 0x60     /* the loop's MSG buffer, 18 bytes in the auto data segment */
#define PAINT_LOG 0x80   /* where the WM_PAINT procedure stores BeginPaint's result */

static void put_far(uint8_t *b, unsigned at, loader_thunk *t) {
    b[at] = (uint8_t)t->off; b[at + 1] = (uint8_t)(t->off >> 8);
    b[at + 2] = (uint8_t)t->sel; b[at + 3] = (uint8_t)(t->sel >> 8);
}

static void install_msg_code(void) {
    uint8_t e[] = {
        0x55, 0x89, 0xE5, 0x83, 0xEC, 0x20,
        0x81, 0x7E, 0x0C, 0x0F, 0x00, 0x75, 0x23,
        0xFF, 0x76, 0x0E, 0x16, 0x8D, 0x46, 0xE0, 0x50, 0x9A, 0, 0, 0, 0,
        0xA3, PAINT_LOG, 0x00,
        0xFF, 0x76, 0x0E, 0x16, 0x8D, 0x46, 0xE0, 0x50, 0x9A, 0, 0, 0, 0,
        0x31, 0xC0, 0x31, 0xD2, 0xEB, 0x05,
        0xB8, 0x07, 0x00, 0x31, 0xD2,
        0x89, 0xEC, 0x5D, 0xCA, 0x0A, 0x00};
    uint8_t l[] = {
        0x1E, 0x6A, MSG_OFF, 0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00, 0x9A, 0, 0, 0, 0,
        0x09, 0xC0, 0x74, 0x12,
        0x1E, 0x6A, MSG_OFF, 0x9A, 0, 0, 0, 0,
        0x1E, 0x6A, MSG_OFF, 0x9A, 0, 0, 0, 0,
        0xEB, 0xDC, 0xF4};
    put_far(e, 22, fx_thunk(&uf, "USER", 39));
    put_far(e, 38, fx_thunk(&uf, "USER", 40));
    put_far(l, 10, fx_thunk(&uf, "USER", 108));
    put_far(l, 22, fx_thunk(&uf, "USER", 113));
    put_far(l, 30, fx_thunk(&uf, "USER", 114));
    u_put(U_CODE_E, e, sizeof e);
    u_put(U_CODE_LOOP, l, sizeof l);
}

static void boot(void) {
    uboot();
    install_msg_code();
}

/* Run the guest message loop until it halts. */
static loader_stop run_loop(void) {
    cpu_t *c = &uf.p->cpu;
    c->seg[CPU_CS] = u_cs();
    c->eip = U_CODE_LOOP;
    return loader_run(uf.p, 2000000);
}

static void test_post_get_peek(void) {
    uint16_t a, m, ds;
    printf("test_usermsg_post_get_peek\n");
    boot();
    ds = u_ds();
    u_register("A", u_far(U_CODE_A), 0, 0, 0, 1, 1);
    a = u_create("A", "a", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, 1);
    m = useg(0x40);
    uclock = 1000 + 77;
    CHECK_EQ(UCALL(110, a, WM_USER + 1, 0x1234, LONG(0xAABBCCDDu)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(UCALL(110, 0x7777, WM_USER, 0, LONG(0)), LOADER_STOP_HLT);   /* bad window */
    CHECK_EQ(fx_ax(&uf), 0);
    /* PeekMessage(&msg, 0, 0, 0, PM_NOREMOVE) then PM_REMOVE */
    CHECK_EQ(UCALL(109, FARP(m, 0), 0, 0, 0, 0), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(urd16(m, 0), a);
    CHECK_EQ(urd16(m, 2), WM_USER + 1);
    CHECK_EQ(urd16(m, 4), 0x1234);
    CHECK_EQ(urd32(m, 6), 0xAABBCCDDu);
    CHECK_EQ(urd32(m, 10), 77);                                   /* time stamp */
    CHECK_EQ(uf.sp_after, uf.sp_before);
    CHECK_EQ(UCALL(109, FARP(m, 0), 0, 0, 0, 1), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(UCALL(109, FARP(m, 0), 0, 0, 0, 1), LOADER_STOP_HLT);   /* queue is empty now */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(upump_calls, 1);                                    /* an empty PeekMessage runs the host pump */
    /* GetMessage with a window and range filter */
    UCALL(110, a, WM_USER + 2, 2, LONG(0));
    UCALL(110, a, WM_USER + 3, 3, LONG(0));
    CHECK_EQ(UCALL(108, FARP(m, 0), a, WM_USER + 3, WM_USER + 3), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(urd16(m, 2), WM_USER + 3);
    CHECK_EQ(UCALL(108, FARP(m, 0), 0, 0, 0), LOADER_STOP_HLT);
    CHECK_EQ(urd16(m, 2), WM_USER + 2);
    /* GetMessageTime / GetMessagePos describe the last message retrieved */
    CHECK_EQ(UCALL(120), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), 77);
    CHECK_EQ(UCALL(119), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), (uint32_t)((480 / 2) << 16 | (640 / 2)));   /* the cursor starts in the middle */
    /* DispatchMessage calls the window procedure and returns its result */
    uwr16(m, 0, a); uwr16(m, 2, WM_USER + 9); uwr16(m, 4, 5); uwr32(m, 6, 0x100);
    ulog_reset();
    CHECK_EQ(UCALL(114, FARP(m, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), 0x101);
    CHECK_EQ(ulog_n(), 1);
    CHECK_EQ(ulog_w(0, ULOG_WP), 5);
    CHECK_EQ(urd16(ds, 0x30), ds);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    /* a message with no window dispatches to nothing */
    uwr16(m, 0, 0);
    ulog_reset();
    CHECK_EQ(UCALL(114, FARP(m, 0)), LOADER_STOP_HLT);
    CHECK_EQ(ulog_n(), 0);
    udown();
}

static uint16_t g_first;

static void post_to_first_window(bool wait) {
    wm_t *w = user_wm(uu);
    (void)wait;
    if (upump_calls == 2) wm_post(w, g_first, WM_USER + 40, 4, 0);
}

static void test_idle_pump(void) {
    uint16_t a, m;
    printf("test_usermsg_idle_pump\n");
    boot();
    u_register("A", u_far(U_CODE_A), 0, 0, 0, 1, 1);
    a = u_create("A", "a", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, 1);
    g_first = a;
    m = useg(0x40);
    /* an empty GetMessage waits in the host pump, which supplies the message */
    upump_script = post_to_first_window;
    CHECK_EQ(UCALL(108, FARP(m, 0), 0, 0, 0), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(urd16(m, 0), a);
    CHECK_EQ(urd16(m, 2), WM_USER + 40);
    CHECK_EQ(upump_calls, 2);
    /* WaitMessage returns as soon as something is queued */
    upump_calls = 0;
    CHECK_EQ(UCALL(112), LOADER_STOP_HLT);
    CHECK_EQ(upump_calls, 2);
    UCALL(110, a, WM_USER, 0, LONG(0));
    upump_calls = 0;
    CHECK_EQ(UCALL(112), LOADER_STOP_HLT);                         /* already pending: no wait */
    CHECK_EQ(upump_calls, 0);
    udown();
    /* the pump asking to stop ends the guest run */
    boot();
    m = useg(0x40);
    upump_stop = true;
    CHECK_EQ(UCALL(108, FARP(m, 0), 0, 0, 0), LOADER_STOP_REQUESTED);
    udown();
    boot();
    m = useg(0x40);
    upump_stop = true;
    CHECK_EQ(UCALL(112), LOADER_STOP_REQUESTED);
    udown();
    /* no pump at all: an empty GetMessage can never return, so the run stops instead of hanging */
    u_nopump = true;
    boot();
    u_nopump = false;
    m = useg(0x40);
    CHECK_EQ(UCALL(108, FARP(m, 0), 0, 0, 0), LOADER_STOP_REQUESTED);
    udown();
}

static void test_quit(void) {
    uint16_t m;
    printf("test_usermsg_quit\n");
    boot();
    m = useg(0x40);
    CHECK_EQ(UCALL(6, 42), LOADER_STOP_HLT);                    /* PostQuitMessage(42) */
    UCALL(110, 0, WM_USER, 0, LONG(0));                          /* a posted message comes first */
    CHECK_EQ(UCALL(108, FARP(m, 0), 0, 0, 0), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(UCALL(108, FARP(m, 0), 0, 0, 0), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 0);                                    /* WM_QUIT: GetMessage returns FALSE */
    CHECK_EQ(urd16(m, 2), WM_QUIT);
    CHECK_EQ(urd16(m, 4), 42);
    CHECK_EQ(UCALL(116, 0, WM_QUIT, 7, LONG(0)), LOADER_STOP_HLT);   /* PostAppMessage(WM_QUIT) is the same thing */
    CHECK_EQ(UCALL(109, FARP(m, 0), 0, 0, 0, 1), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(urd16(m, 2), WM_QUIT);
    CHECK_EQ(urd16(m, 4), 7);
    udown();
}

static void test_guest_message_loop(void) {
    uint16_t a, b;
    printf("test_usermsg_guest_message_loop\n");
    boot();
    u_register("A", u_far(U_CODE_A), 0, 0, 0, 1, 1);
    a = u_create("A", "a", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, 1);
    b = u_create("A", "b", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, 1);
    ulog_reset();
    UCALL(110, a, WM_USER + 1, 1, LONG(0));
    UCALL(110, b, WM_USER + 2, 2, LONG(0));
    UCALL(110, a, WM_USER + 3, 3, LONG(0));
    UCALL(6, 0);
    CHECK_EQ(run_loop(), LOADER_STOP_HLT);                      /* the loop ended at WM_QUIT */
    CHECK_EQ(ulog_n(), 3);
    CHECK(ulog_w(0, ULOG_HWND) == a && ulog_w(0, ULOG_MSG) == WM_USER + 1);
    CHECK(ulog_w(1, ULOG_HWND) == b && ulog_w(1, ULOG_MSG) == WM_USER + 2);
    CHECK(ulog_w(2, ULOG_HWND) == a && ulog_w(2, ULOG_MSG) == WM_USER + 3);
    udown();
}

static void script_type_a(bool wait) {
    wm_t *w = user_wm(uu);
    wm_input e;
    (void)wait;
    memset(&e, 0, sizeof e);
    if (upump_calls == 1) {
        e.kind = WM_IN_KEY_DOWN; e.vk = 'A'; e.scan = 0x1E;
        wm_input_event(w, &e);
        e.kind = WM_IN_KEY_UP;
        wm_input_event(w, &e);
    } else if (upump_calls == 2) {
        wm_post_quit(w, 0);
    }
}

static void test_keyboard_through_loop(void) {
    uint16_t a;
    printf("test_usermsg_keyboard_translate\n");
    boot();
    u_register("A", u_far(U_CODE_A), 0, 0, 0, 1, 1);
    a = u_create("A", "a", WS_OVERLAPPED | WS_VISIBLE, 0, 0, 50, 50, 0, 0, 1);
    wm_validate(user_wm(uu), a, NULL);
    ulog_reset();
    upump_script = script_type_a;
    CHECK_EQ(run_loop(), LOADER_STOP_HLT);
    /* WM_KEYDOWN 'A', then TranslateMessage's WM_CHAR 'a', then WM_KEYUP */
    {
        int kd = ulog_find(WM_KEYDOWN, a, 0), ch = ulog_find(WM_CHAR, a, 0), ku = ulog_find(WM_KEYUP, a, 0);
        CHECK(kd >= 0 && ch > kd && ku > ch);
        if (kd >= 0) CHECK_EQ(ulog_w((unsigned)kd, ULOG_WP), 'A');
        if (ch >= 0) CHECK_EQ(ulog_w((unsigned)ch, ULOG_WP), 'a');
    }
    udown();
}

static void script_timer_ticks(bool wait) {
    (void)wait;
    uclock += 10;
    if (upump_calls == 3) wm_post_quit(user_wm(uu), 0);
}

static void test_timers_api(void) {
    uint16_t a, id;
    printf("test_usermsg_timers\n");
    boot();
    u_register("A", u_far(U_CODE_A), 0, 0, 0, 1, 1);
    a = u_create("A", "a", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, 1);
    CHECK_EQ(UCALL(10, a, 7, 10, FARP(0, 0)), LOADER_STOP_HLT);   /* SetTimer(hwnd, id, elapse, proc) */
    CHECK_EQ(fx_ax(&uf), 7);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    ulog_reset();
    upump_script = script_timer_ticks;
    CHECK_EQ(run_loop(), LOADER_STOP_HLT);
    CHECK(ulog_n() >= 2);
    CHECK(ulog_w(0, ULOG_HWND) == a && ulog_w(0, ULOG_MSG) == WM_TIMER && ulog_w(0, ULOG_WP) == 7);
    CHECK_EQ(ulog_lp(0), 0);                                       /* no TIMERPROC */
    CHECK_EQ(UCALL(12, a, 7), LOADER_STOP_HLT);                    /* KillTimer */
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(UCALL(12, a, 7), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 0);
    /* a TIMERPROC gets (hwnd, WM_TIMER, id, tick) with DS set */
    upump_calls = 0;
    CHECK_EQ(UCALL(10, 0, 0, 10, FARP(u_cs(), U_CODE_A)), LOADER_STOP_HLT);
    id = fx_ax(&uf);
    CHECK(id != 0);
    ulog_reset();
    uwr16(u_ds(), 0x30, 0);
    CHECK_EQ(run_loop(), LOADER_STOP_HLT);
    CHECK(ulog_n() >= 2);
    CHECK(ulog_w(0, ULOG_HWND) == 0 && ulog_w(0, ULOG_MSG) == WM_TIMER && ulog_w(0, ULOG_WP) == id);
    CHECK(ulog_lp(0) >= 10);                                       /* dwTime: milliseconds since start */
    CHECK_EQ(urd16(u_ds(), 0x30), u_ds());
    UCALL(12, 0, id);
    udown();
}

static void test_focus_capture_state(void) {
    uint16_t a, b;
    uint16_t pt;
    wm_input e;
    printf("test_usermsg_focus_capture_cursor\n");
    boot();
    u_register("W", 0, 0, 0, 0, 1, 1);
    a = u_create("W", "a", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 100, 100, 0, 0, 1);
    b = u_create("W", "b", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 0, 100, 100, 0, 0, 1);
    UCALL(23); CHECK_EQ(fx_ax(&uf), b);                           /* GetFocus */
    UCALL(22, a); CHECK_EQ(fx_ax(&uf), b);                        /* SetFocus returns the previous focus */
    UCALL(23); CHECK_EQ(fx_ax(&uf), a);
    UCALL(60); CHECK_EQ(fx_ax(&uf), a);                           /* the focus window's top level is active */
    UCALL(59, b); CHECK_EQ(fx_ax(&uf), a);                        /* SetActiveWindow returns the previous */
    UCALL(60); CHECK_EQ(fx_ax(&uf), b);
    UCALL(236); CHECK_EQ(fx_ax(&uf), 0);                          /* GetCapture */
    UCALL(18, a); CHECK_EQ(fx_ax(&uf), 0);                        /* SetCapture returns the previous capture */
    UCALL(236); CHECK_EQ(fx_ax(&uf), a);
    UCALL(18, b); CHECK_EQ(fx_ax(&uf), a);
    CHECK_EQ(UCALL(19), LOADER_STOP_HLT);                         /* ReleaseCapture */
    UCALL(236); CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    /* cursor */
    pt = useg(0x10);
    memset(&e, 0, sizeof e);
    e.kind = WM_IN_MOVE; e.x = 321; e.y = 123;
    wm_input_event(user_wm(uu), &e);
    CHECK_EQ(UCALL(17, FARP(pt, 0)), LOADER_STOP_HLT);            /* GetCursorPos */
    CHECK(urd16(pt, 0) == 321 && urd16(pt, 2) == 123);
    CHECK_EQ(UCALL(70, 5, 6), LOADER_STOP_HLT);                   /* SetCursorPos */
    UCALL(17, FARP(pt, 0));
    CHECK(urd16(pt, 0) == 5 && urd16(pt, 2) == 6);
    UCALL(69, 0x1234); CHECK_EQ(fx_ax(&uf), 0);                   /* SetCursor returns the previous cursor */
    UCALL(69, 0x4321); CHECK_EQ(fx_ax(&uf), 0x1234);
    UCALL(71, 1); CHECK_EQ(fx_ax(&uf), 1);                        /* ShowCursor: display counter */
    UCALL(71, 1); CHECK_EQ(fx_ax(&uf), 2);
    UCALL(71, 0); CHECK_EQ(fx_ax(&uf), 1);
    UCALL(71, 0); UCALL(71, 0); CHECK_EQ(fx_ax(&uf), (uint16_t)-1);
    UCALL(169); CHECK_EQ(fx_ax(&uf), 500);                        /* GetCaretBlinkTime */
    udown();
}

static void test_key_state_api(void) {
    wm_input e;
    printf("test_usermsg_key_state\n");
    boot();
    memset(&e, 0, sizeof e);
    UCALL(106, 'A'); CHECK_EQ(fx_ax(&uf) & 0x8000, 0);
    e.kind = WM_IN_KEY_DOWN; e.vk = 'A';
    wm_input_event(user_wm(uu), &e);
    UCALL(106, 'A'); CHECK_EQ(fx_ax(&uf) & 0x8000, 0x8000);
    UCALL(249, 'A'); CHECK_EQ(fx_ax(&uf) & 0x8000, 0x8000);       /* GetAsyncKeyState */
    e.kind = WM_IN_KEY_UP;
    wm_input_event(user_wm(uu), &e);
    UCALL(249, 'A'); CHECK_EQ(fx_ax(&uf) & 0x8000, 0);
    e.kind = WM_IN_KEY_DOWN; e.vk = 0x14;                         /* caps lock toggles */
    wm_input_event(user_wm(uu), &e);
    e.kind = WM_IN_KEY_UP;
    wm_input_event(user_wm(uu), &e);
    UCALL(106, 0x14); CHECK_EQ(fx_ax(&uf) & 1, 1);
    udown();
}

static void test_register_window_message(void) {
    uint16_t s1, s2;
    uint16_t id1, id2;
    printf("test_usermsg_register_window_message\n");
    boot();
    s1 = ustr("commdlg_FindReplace");
    s2 = ustr("other message");
    UCALL(118, FARP(s1, 0)); id1 = fx_ax(&uf);
    CHECK(id1 >= 0xC000);
    UCALL(118, FARP(s1, 0)); CHECK_EQ(fx_ax(&uf), id1);          /* same string, same id */
    UCALL(118, FARP(s2, 0)); id2 = fx_ax(&uf);
    CHECK(id2 >= 0xC000 && id2 != id1);
    udown();
}

static void test_paint_api(void) {
    uint16_t a, ps, rc, hdc;
    printf("test_usermsg_paint_api\n");
    boot();
    u_register("W", 0, 0, 0, 0, 1, COLOR_WINDOW + 1);
    a = u_create("W", "a", WS_OVERLAPPED | WS_BORDER | WS_VISIBLE, 100, 100, 200, 150, 0, 0, 1);
    ps = useg(0x40);
    rc = useg(0x10);
    /* everything is invalid after ShowWindow */
    CHECK_EQ(UCALL(190, a, FARP(rc, 0), 0), LOADER_STOP_HLT);    /* GetUpdateRect */
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK(urd16(rc, 0) == 0 && urd16(rc, 2) == 0 && urd16(rc, 4) == 198 && urd16(rc, 6) == 148);
    CHECK_EQ(UCALL(127, a, FARP(0, 0)), LOADER_STOP_HLT);        /* ValidateRect(NULL) validates everything */
    UCALL(190, a, FARP(rc, 0), 0); CHECK_EQ(fx_ax(&uf), 0);
    uwr16(rc, 0, 10); uwr16(rc, 2, 20); uwr16(rc, 4, 60); uwr16(rc, 6, 50);
    CHECK_EQ(UCALL(125, a, FARP(rc, 0), 1), LOADER_STOP_HLT);    /* InvalidateRect(rect, erase) */
    uwr16(rc, 0, 0); uwr16(rc, 2, 0); uwr16(rc, 4, 0); uwr16(rc, 6, 0);
    UCALL(190, a, FARP(rc, 0), 0); CHECK_EQ(fx_ax(&uf), 1);
    CHECK(urd16(rc, 0) == 10 && urd16(rc, 2) == 20 && urd16(rc, 4) == 60 && urd16(rc, 6) == 50);
    /* BeginPaint fills the PAINTSTRUCT; the default procedure erased with the class brush (COLOR_WINDOW) */
    fb_fill_rect(user_fb(uu), &(fb_rect){0, 0, 640, 480}, FB_RGB(255, 0, 0));
    CHECK_EQ(UCALL(39, a, FARP(ps, 0)), LOADER_STOP_HLT);
    hdc = fx_ax(&uf);
    CHECK(hdc != 0);
    CHECK_EQ(urd16(ps, 0), hdc);
    CHECK_EQ(urd16(ps, 2), 0);                                    /* fErase: the background was erased */
    CHECK(urd16(ps, 4) == 10 && urd16(ps, 6) == 20 && urd16(ps, 8) == 60 && urd16(ps, 10) == 50);
    {
        int ox, oy;
        wm_client_origin(user_wm(uu), a, &ox, &oy);
        CHECK_EQ(fb_colorref_at(user_fb(uu), ox + 10, oy + 20), wm_sys_color(COLOR_WINDOW));
        CHECK_EQ(fb_colorref_at(user_fb(uu), ox + 59, oy + 49), wm_sys_color(COLOR_WINDOW));
        CHECK_EQ(fb_colorref_at(user_fb(uu), ox + 60, oy + 20), FB_RGB(255, 0, 0));
    }
    UCALL(190, a, FARP(rc, 0), 0); CHECK_EQ(fx_ax(&uf), 0);       /* validated by BeginPaint */
    CHECK_EQ(UCALL(40, a, FARP(ps, 0)), LOADER_STOP_HLT);         /* EndPaint releases the DC */
    CHECK(wm_dc_get(user_wm(uu), hdc) == NULL);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    /* GetDC / ReleaseDC */
    CHECK_EQ(UCALL(66, a), LOADER_STOP_HLT);
    hdc = fx_ax(&uf);
    CHECK(hdc != 0 && wm_dc_get(user_wm(uu), hdc) != NULL);
    CHECK_EQ(UCALL(68, a, hdc), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK(wm_dc_get(user_wm(uu), hdc) == NULL);
    UCALL(68, a, hdc); CHECK_EQ(fx_ax(&uf), 0);                   /* not twice */
    UCALL(66, 0x7777); CHECK_EQ(fx_ax(&uf), 0);
    udown();
}

static void script_quit(bool wait) {
    (void)wait;
    if (upump_calls == 1) wm_post_quit(user_wm(uu), 0);
}

static void test_update_window_guest_paint(void) {
    uint16_t a, rc;
    printf("test_usermsg_update_window_guest_paint\n");
    boot();
    u_register("P", u_far(U_CODE_E), 0, 0, 0, 1, 1);
    a = u_create("P", "paint", WS_OVERLAPPED | WS_VISIBLE, 0, 0, 100, 100, 0, 0, 1);
    rc = useg(0x10);
    uwr16(u_ds(), PAINT_LOG, 0);
    CHECK_EQ(UCALL(124, a), LOADER_STOP_HLT);                     /* UpdateWindow: WM_PAINT straight to the procedure */
    CHECK(urd16(u_ds(), PAINT_LOG) != 0);                              /* BeginPaint ran in the guest, on its stack PAINTSTRUCT */
    UCALL(190, a, FARP(rc, 0), 0); CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(wm_dc_get(user_wm(uu), urd16(u_ds(), PAINT_LOG)) == NULL, 1);   /* EndPaint ran too */
    CHECK_EQ(uf.sp_after, uf.sp_before);
    /* through the message loop: WM_PAINT is generated, dispatched and validates the window */
    uwr16(u_ds(), PAINT_LOG, 0);
    UCALL(125, a, FARP(0, 0), 1);
    upump_calls = 0;
    upump_script = script_quit;                                   /* WM_QUIT outranks WM_PAINT, so quit from the idle pump */
    CHECK_EQ(run_loop(), LOADER_STOP_HLT);
    CHECK(urd16(u_ds(), PAINT_LOG) != 0);
    UCALL(190, a, FARP(rc, 0), 0); CHECK_EQ(fx_ax(&uf), 0);
    udown();
}

static void test_rect_functions(void) {
    uint16_t r, s, d;
    printf("test_usermsg_rects\n");
    boot();
    r = useg(0x10); s = useg(0x10); d = useg(0x10);
    CHECK_EQ(UCALL(72, FARP(r, 0), 10, 20, 110, 70), LOADER_STOP_HLT);   /* SetRect */
    CHECK(urd16(r, 0) == 10 && urd16(r, 2) == 20 && urd16(r, 4) == 110 && urd16(r, 6) == 70);
    UCALL(75, FARP(r, 0)); CHECK_EQ(fx_ax(&uf), 0);                       /* IsRectEmpty */
    UCALL(73, FARP(r, 0));                                                /* SetRectEmpty */
    CHECK(urd16(r, 0) == 0 && urd16(r, 2) == 0 && urd16(r, 4) == 0 && urd16(r, 6) == 0);
    UCALL(75, FARP(r, 0)); CHECK_EQ(fx_ax(&uf), 1);
    UCALL(72, FARP(r, 0), 10, 20, 110, 70);
    UCALL(74, FARP(s, 0), FARP(r, 0));                                    /* CopyRect(dst, src) */
    CHECK(urd16(s, 0) == 10 && urd16(s, 6) == 70);
    UCALL(244, FARP(s, 0), FARP(r, 0)); CHECK_EQ(fx_ax(&uf), 1);          /* EqualRect */
    uwr16(s, 0, 11);
    UCALL(244, FARP(s, 0), FARP(r, 0)); CHECK_EQ(fx_ax(&uf), 0);
    /* PtInRect(&r, POINT): the point is passed by value, x in the low word; right/bottom are exclusive */
    UCALL(76, FARP(r, 0), LONG(20 << 16 | 10)); CHECK_EQ(fx_ax(&uf), 1);
    UCALL(76, FARP(r, 0), LONG(20 << 16 | 110)); CHECK_EQ(fx_ax(&uf), 0);
    UCALL(76, FARP(r, 0), LONG(70 << 16 | 50)); CHECK_EQ(fx_ax(&uf), 0);
    UCALL(76, FARP(r, 0), LONG(69 << 16 | 109)); CHECK_EQ(fx_ax(&uf), 1);
    UCALL(77, FARP(r, 0), 5, (uint16_t)-5);                               /* OffsetRect */
    CHECK(urd16(r, 0) == 15 && urd16(r, 2) == 15 && urd16(r, 4) == 115 && urd16(r, 6) == 65);
    UCALL(78, FARP(r, 0), 5, 10);                                         /* InflateRect */
    CHECK(urd16(r, 0) == 10 && urd16(r, 2) == 5 && urd16(r, 4) == 120 && urd16(r, 6) == 75);
    UCALL(78, FARP(r, 0), (uint16_t)-5, (uint16_t)-10);
    CHECK(urd16(r, 0) == 15 && urd16(r, 2) == 15 && urd16(r, 4) == 115 && urd16(r, 6) == 65);
    /* IntersectRect(dst, a, b) */
    UCALL(72, FARP(s, 0), 50, 0, 200, 40);
    CHECK_EQ(UCALL(79, FARP(d, 0), FARP(r, 0), FARP(s, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK(urd16(d, 0) == 50 && urd16(d, 2) == 15 && urd16(d, 4) == 115 && urd16(d, 6) == 40);
    UCALL(72, FARP(s, 0), 500, 500, 600, 600);
    UCALL(79, FARP(d, 0), FARP(r, 0), FARP(s, 0)); CHECK_EQ(fx_ax(&uf), 0);
    CHECK(urd16(d, 0) == 0 && urd16(d, 2) == 0 && urd16(d, 4) == 0 && urd16(d, 6) == 0);
    /* UnionRect(dst, a, b): empty rectangles are ignored */
    CHECK_EQ(UCALL(80, FARP(d, 0), FARP(r, 0), FARP(s, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK(urd16(d, 0) == 15 && urd16(d, 2) == 15 && urd16(d, 4) == 600 && urd16(d, 6) == 600);
    UCALL(72, FARP(s, 0), 7, 7, 7, 50);                                   /* empty */
    UCALL(80, FARP(d, 0), FARP(r, 0), FARP(s, 0));
    CHECK(urd16(d, 0) == 15 && urd16(d, 4) == 115);
    UCALL(72, FARP(r, 0), 0, 0, 0, 0);
    UCALL(80, FARP(d, 0), FARP(r, 0), FARP(s, 0)); CHECK_EQ(fx_ax(&uf), 0);   /* both empty */
    CHECK_EQ(uf.sp_after, uf.sp_before);
    udown();
}

/* wsprintf is cdecl: arguments are pushed right to left and the caller pops them. */
static void test_wsprintf(void) {
    uint16_t out, fmt, str;
    printf("test_usermsg_wsprintf\n");
    boot();
    out = useg(0x400);
    str = ustr("far");
    fmt = ustr("[%d|%u|%x|%X|%c|%s|%%]");
    CHECK_EQ(UCALL(420, FARP(str, 0), 'z', 0xBEEF, 0xBEEF, 65535, (uint16_t)-5, FARP(fmt, 0), FARP(out, 0)), LOADER_STOP_HLT);
    CHECK(!strcmp((const char *)umem(out, 0, 1), "[-5|65535|beef|BEEF|z|far|%]"));
    CHECK_EQ(fx_ax(&uf), strlen("[-5|65535|beef|BEEF|z|far|%]"));
    CHECK_EQ(uf.sp_before - uf.sp_after, 22);                    /* 11 words pushed, none popped: the caller cleans up */
    fmt = ustr("%ld/%lu/%lx");
    UCALL(420, LONG(0xDEADBEEFu), LONG(4000000000u), LONG((uint32_t)-70000), FARP(fmt, 0), FARP(out, 0));
    CHECK(!strcmp((const char *)umem(out, 0, 1), "-70000/4000000000/deadbeef"));
    fmt = ustr("[%5d][%-5d][%05d][%3s][%-3s]");
    str = ustr("a");
    UCALL(420, FARP(str, 0), FARP(str, 0), 12, 12, 12, FARP(fmt, 0), FARP(out, 0));
    CHECK(!strcmp((const char *)umem(out, 0, 1), "[   12][12   ][00012][  a][a  ]"));
    fmt = ustr("%s");                                              /* output is capped at 1023 characters */
    {
        uint16_t big = useg(0x1000);
        memset(umem(big, 0, 0x1000), 'x', 0xFFF);
        umem(big, 0, 0x1000)[0xFFF] = 0;
        UCALL(420, FARP(big, 0), FARP(fmt, 0), FARP(out, 0));
        CHECK_EQ(fx_ax(&uf), 1023);
        CHECK_EQ(strlen((const char *)umem(out, 0, 1)), 1023);
    }
    udown();
}

static void test_char_and_string_helpers(void) {
    uint16_t s, t, buf;
    printf("test_usermsg_char_helpers\n");
    boot();
    UCALL(435, 'A'); CHECK_EQ(fx_ax(&uf), 1);                    /* IsCharUpper */
    UCALL(435, 'a'); CHECK_EQ(fx_ax(&uf), 0);
    UCALL(435, 0xC9); CHECK_EQ(fx_ax(&uf), 1);                   /* E acute */
    UCALL(436, 'a'); CHECK_EQ(fx_ax(&uf), 1);
    UCALL(436, '5'); CHECK_EQ(fx_ax(&uf), 0);
    UCALL(433, 'x'); CHECK_EQ(fx_ax(&uf), 1);                    /* IsCharAlpha */
    UCALL(433, '9'); CHECK_EQ(fx_ax(&uf), 0);
    UCALL(434, '9'); CHECK_EQ(fx_ax(&uf), 1);                    /* IsCharAlphanumeric */
    UCALL(434, '-'); CHECK_EQ(fx_ax(&uf), 0);
    buf = ustr("MiXed 12 \xE9\xC9");
    CHECK_EQ(UCALL(437, FARP(buf, 0), 12), LOADER_STOP_HLT);     /* AnsiUpperBuff(buf, n) returns n */
    CHECK_EQ(fx_ax(&uf), 12);
    CHECK(!strcmp((const char *)umem(buf, 0, 1), "MIXED 12 \xC9\xC9"));
    CHECK_EQ(UCALL(438, FARP(buf, 0), 5), LOADER_STOP_HLT);
    CHECK(!strncmp((const char *)umem(buf, 0, 1), "mixed", 5));
    /* from the shared USER string helpers */
    s = ustr("abc"); t = ustr("ABD");
    UCALL(430, FARP(s, 0), FARP(t, 0)); CHECK(fx_ax(&uf) != 0);   /* lstrcmp is case sensitive */
    UCALL(471, FARP(s, 0), FARP(t, 0)); CHECK_EQ((int16_t)fx_ax(&uf), -1);
    UCALL(432, FARP(s, 0)); CHECK_EQ(fx_dxax(&uf), (uint32_t)s << 16);   /* AnsiLower returns the pointer */
    UCALL(431, FARP(s, 0));
    CHECK(!strcmp((const char *)umem(s, 0, 1), "ABC"));
    UCALL(472, FARP(s, 1)); CHECK_EQ(fx_dxax(&uf), (uint32_t)s << 16 | 2);   /* AnsiNext */
    UCALL(473, FARP(s, 0), FARP(s, 2)); CHECK_EQ(fx_dxax(&uf), (uint32_t)s << 16 | 1);   /* AnsiPrev */
    udown();
}

/* MessageBox is a real modal dialog now: Enter answers with the default button (the dialog module's own tests
 * cover layout, keys and the mouse), and the call is logged. */
static unsigned mb_keys;
static void press_enter(bool wait) {
    wm_input in;
    (void)wait;
    memset(&in, 0, sizeof in);
    in.kind = mb_keys++ % 2 ? WM_IN_KEY_UP : WM_IN_KEY_DOWN;
    in.vk = 0x0D;
    wm_input_event(user_wm(uu), &in);
}

static void test_message_box(void) {
    FILE *log;
    char *mem = NULL;
    size_t sz = 0;
    uint16_t t, c;
    user_config cfg;
    static const struct { unsigned type, id; } kinds[] = {{0, 1}, {1, 1}, {4, 6}, {3, 6}, {5, 4}, {2, 3}};
    unsigned i;
    printf("test_usermsg_message_box\n");
    log = open_memstream(&mem, &sz);
    memset(&cfg, 0, sizeof cfg);
    cfg.log = log;
    uboot_cfg(&cfg);
    t = ustr("Disk full");
    c = ustr("Creative Writer");
    for (i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        mb_keys = 0;
        upump_script = press_enter;
        CHECK_EQ(UCALL(1, 0, FARP(t, 0), FARP(c, 0), (uint16_t)kinds[i].type), LOADER_STOP_HLT);
        CHECK_EQ(fx_ax(&uf), kinds[i].id);                                       /* the default button's id */
        CHECK_EQ(uf.sp_after, uf.sp_before);
    }
    upump_script = NULL;
    CHECK_EQ(UCALL(104, 0), LOADER_STOP_HLT);                                 /* MessageBeep */
    CHECK_EQ(fx_ax(&uf), 1);
    fflush(log);
    CHECK(mem && strstr(mem, "MessageBox") && strstr(mem, "Disk full") && strstr(mem, "Creative Writer"));
    udown();
    fclose(log);
    free(mem);
}

/* Every USER import has a real handler now (USER.483 is SystemParametersInfo): no logged stub is left, so a call to
 * 483 with an action it does not know answers FALSE and logs nothing. */
static void test_stubs_are_logged(void) {
    printf("test_usermsg_stubs\n");
    boot();
    CHECK_EQ(user_stub_count(uu), 0);
    CHECK_EQ(UCALL(483, 999, 0, 0, 0, 0), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);                           /* the declared argument bytes are popped */
    CHECK_EQ(user_stub_count(uu), 0);
    /* the loaders return real handles */
    CHECK_EQ(UCALL(173, 0, FARP(0, 32512)), LOADER_STOP_HLT);      /* LoadCursor(NULL, IDC_ARROW) */
    CHECK(fx_ax(&uf) != 0);
    CHECK_EQ(UCALL(243), LOADER_STOP_HLT);                         /* GetDialogBaseUnits: system font 8x16 */
    CHECK_EQ(fx_dxax(&uf), (uint32_t)(16 << 16 | 8));
    CHECK_EQ(UCALL(284, 0), LOADER_STOP_HLT);                      /* GetFreeSystemResources */
    CHECK(fx_ax(&uf) > 50);
    udown();
}

int main(void) {
    test_post_get_peek();
    test_idle_pump();
    test_quit();
    test_guest_message_loop();
    test_keyboard_through_loop();
    test_timers_api();
    test_focus_capture_state();
    test_key_state_api();
    test_register_window_message();
    test_paint_api();
    test_update_window_guest_paint();
    test_rect_functions();
    test_wsprintf();
    test_char_and_string_helpers();
    test_message_box();
    test_stubs_are_logged();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
