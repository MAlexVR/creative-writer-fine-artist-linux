#define _POSIX_C_SOURCE 200809L
/* SDL2 presenter on SDL's dummy video driver: no display is needed. The tests
 * queue real SDL events through the presenter's hooks and read the Win16
 * messages that come out of the window manager. */
#include "present_sdl.h"
#include "wmfixture.h"

static present_t *pr;

static void boot(const present_config *in) {
    present_config cfg;
    wboot();
    wdef_through = true;
    memset(&cfg, 0, sizeof cfg);
    if (in) cfg = *in;
    cfg.headless = true;
    pr = present_create(&cfg, &wfb, &wm);
    CHECK(pr != NULL);
}

static void down(void) {
    present_destroy(pr);
    pr = NULL;
    wdown();
}

static bool next(wm_msg *m, uint16_t min, uint16_t max) { return wm_next_message(&wm, m, 0, min, max, true); }

static void test_headless_driver_and_upload(void) {
    fb_rect r = {10, 20, 30, 40};
    printf("test_present_upload\n");
    boot(NULL);
    if (!pr) return;
    CHECK(!strcmp(present_driver(pr), "dummy"));
    CHECK_EQ(present_uploads(pr), 0);
    CHECK_EQ(present_pixel(pr, 15, 25), 0);                       /* nothing uploaded yet */
    fb_fill_rect(&wfb, &r, FB_RGB(255, 0, 0));
    CHECK(present_pump(pr, false));
    CHECK_EQ(present_frames(pr), 1);
    CHECK_EQ(present_uploads(pr), 1);
    CHECK_EQ(present_pixel(pr, 15, 25), 0xFF0000);
    CHECK_EQ(present_pixel(pr, 5, 25), wfb.pal[wfb.p8[25 * 640 + 5]]);   /* the palette converts the rest */
    CHECK_EQ(present_pixel(pr, 640, 0), 0);
    /* a palette change shows up on the next upload */
    wfb.pal[wfb.p8[25 * 640 + 15]] = 0x123456;
    wfb.dirty++;
    present_flush(pr);
    CHECK_EQ(present_pixel(pr, 15, 25), 0x123456);
    down();
}

static void test_pacing_and_frame_limit(void) {
    present_config cfg;
    fb_rect r = {0, 0, 4, 4};
    printf("test_present_pacing\n");
    memset(&cfg, 0, sizeof cfg);
    cfg.min_present_ms = 100000;
    cfg.max_frames = 4;
    boot(&cfg);
    if (!pr) return;
    present_pump(pr, false);
    CHECK_EQ(present_uploads(pr), 1);                              /* the first frame is always shown */
    present_pump(pr, false);
    CHECK_EQ(present_uploads(pr), 1);                              /* unchanged: nothing to upload */
    fb_fill_rect(&wfb, &r, FB_RGB(0, 255, 0));
    CHECK(present_pump(pr, false));
    CHECK_EQ(present_uploads(pr), 1);                              /* changed, but inside the pacing window */
    CHECK(!present_pump(pr, false));                               /* frame 4 of 4: stop ... */
    CHECK_EQ(present_uploads(pr), 2);                              /* ... and show the final picture */
    CHECK_EQ(present_pixel(pr, 1, 1), 0x00FF00);
    down();
}

static void test_mouse_events(void) {
    present_config cfg;
    uint16_t a;
    wm_msg m;
    printf("test_present_mouse\n");
    memset(&cfg, 0, sizeof cfg);
    cfg.scale = 2;
    boot(&cfg);
    if (!pr) return;
    wclass("W", PROC_A);
    a = wmake("W", WS_POPUP | WS_VISIBLE, 50, 60, 100, 100, 0);
    wdrain();
    CHECK(present_inject_motion(pr, 120, 160));                    /* window pixels: (60, 80) on the 2x screen */
    CHECK(present_pump(pr, false));
    CHECK(next(&m, WM_MOUSEMOVE, WM_MOUSEMOVE));
    CHECK_EQ(m.hwnd, a);
    CHECK_EQ(m.lparam, (uint32_t)(20 << 16 | 10));                 /* client (10, 20) */
    CHECK_EQ(wm.cursor_x, 60);
    CHECK_EQ(wm.cursor_y, 80);
    present_inject_button(pr, 0, true, 120, 160);
    present_inject_button(pr, 0, false, 120, 160);
    present_inject_button(pr, 1, true, 120, 160);
    present_inject_button(pr, 2, true, 120, 160);
    present_pump(pr, false);
    CHECK(next(&m, WM_LBUTTONDOWN, WM_LBUTTONDOWN));
    CHECK_EQ(m.wparam & 1, 1);                                     /* MK_LBUTTON */
    CHECK(next(&m, WM_LBUTTONUP, WM_LBUTTONUP));
    CHECK(next(&m, WM_RBUTTONDOWN, WM_RBUTTONDOWN));
    CHECK(next(&m, WM_MBUTTONDOWN, WM_MBUTTONDOWN));
    down();
}

static void test_keyboard_events(void) {
    uint16_t a;
    wm_msg m;
    printf("test_present_keyboard\n");
    boot(NULL);
    if (!pr) return;
    wclass("W", PROC_A);
    a = wmake("W", WS_POPUP | WS_VISIBLE, 0, 0, 100, 100, 0);
    wm_set_focus(&wm, a);
    wdrain();
    present_inject_key(pr, 'a', true);
    present_inject_key(pr, 'a', false);
    present_inject_key(pr, 0x4000003A, true);                      /* F1 */
    present_inject_key(pr, 0x40000050, true);                      /* left arrow */
    present_inject_key(pr, 0x40000048, true);                      /* Pause: no Win16 key, ignored */
    present_pump(pr, false);
    CHECK(next(&m, WM_KEYDOWN, WM_KEYDOWN));
    CHECK(m.hwnd == a && m.wparam == 'A');
    CHECK(next(&m, WM_KEYUP, WM_KEYUP));
    CHECK_EQ(m.wparam, 'A');
    CHECK(next(&m, WM_KEYDOWN, WM_KEYDOWN));
    CHECK_EQ(m.wparam, 0x70);
    CHECK(next(&m, WM_KEYDOWN, WM_KEYDOWN));
    CHECK_EQ(m.wparam, 0x25);
    CHECK(!next(&m, WM_KEYDOWN, WM_KEYDOWN));
    CHECK_EQ(wm_key_state(&wm, 0x25) & 0x8000, 0x8000);
    down();
}

static void test_key_table(void) {
    printf("test_present_key_table\n");
    CHECK_EQ(present_vk_from_sdl('z'), 'Z');
    CHECK_EQ(present_vk_from_sdl('7'), '7');
    CHECK_EQ(present_vk_from_sdl('\r'), 0x0D);
    CHECK_EQ(present_vk_from_sdl(' '), 0x20);
    CHECK_EQ(present_vk_from_sdl(';'), 0xBA);
    CHECK_EQ(present_vk_from_sdl('['), 0xDB);
    CHECK_EQ(present_vk_from_sdl(0x400000E1), 0x10);               /* left shift */
    CHECK_EQ(present_vk_from_sdl(0x40000059), 0x61);               /* keypad 1 */
    CHECK_EQ(present_vk_from_sdl(0x40000049), 0x2D);               /* insert */
    CHECK_EQ(present_vk_from_sdl(0x7F), 0x2E);                     /* delete */
    CHECK_EQ(present_vk_from_sdl('\\'), 0xDC);
    CHECK_EQ(present_vk_from_sdl(0x1234567), 0);
}

static void test_quit(void) {
    uint16_t a;
    wm_msg m;
    printf("test_present_quit\n");
    boot(NULL);
    if (!pr) return;
    present_inject_quit(pr);
    present_pump(pr, false);
    CHECK(next(&m, WM_QUIT, WM_QUIT));                             /* no window to ask: the loop ends */
    wclass("W", PROC_A);
    a = wmake("W", WS_POPUP | WS_VISIBLE, 0, 0, 100, 100, 0);
    wm_set_active(&wm, a);
    wdrain();
    present_inject_quit(pr);
    present_pump(pr, false);
    CHECK(next(&m, WM_SYSCOMMAND, WM_SYSCOMMAND) && m.wparam == 0xF060);   /* SC_CLOSE: the active window decides */
    CHECK_EQ(m.hwnd, a);
    down();
}


static void test_viewport_math(void) {
    present_rect v;
    int gx, gy;
    printf("test_present_viewport\n");
    v = present_viewport(640, 480, 640, 480, false);                /* exact fit */
    CHECK(v.x == 0 && v.y == 0 && v.w == 640 && v.h == 480);
    v = present_viewport(1920, 1080, 640, 480, false);              /* wide screen: pillarbox, height-limited */
    CHECK(v.h == 1080 && v.w == 1440 && v.x == 240 && v.y == 0);
    v = present_viewport(1920, 1080, 640, 480, true);               /* integer: 2x */
    CHECK(v.w == 1280 && v.h == 960 && v.x == 320 && v.y == 60);
    v = present_viewport(800, 1200, 640, 480, false);               /* tall window: letterbox, width-limited */
    CHECK(v.w == 800 && v.h == 600 && v.x == 0 && v.y == 300);
    v = present_viewport(500, 300, 640, 480, true);                 /* smaller than 1x: integer mode falls back to fit */
    CHECK(v.h == 300 && v.w == 400 && v.x == 50);
    v = present_viewport(0, 0, 640, 480, false);                    /* degenerate window */
    CHECK(v.w == 0 && v.h == 0);
    v = present_viewport(1920, 1080, 640, 480, false);
    CHECK(present_map_point(v, 640, 480, 240, 0, &gx, &gy) && gx == 0 && gy == 0);
    CHECK(present_map_point(v, 640, 480, 240 + 1439, 1079, &gx, &gy) && gx == 639 && gy == 479);
    CHECK(present_map_point(v, 640, 480, 960, 540, &gx, &gy) && gx == 320 && gy == 240);
    CHECK(!present_map_point(v, 640, 480, 10, 540, &gx, &gy));      /* in the black bar: clamped to the edge, not inside */
    CHECK(gx == 0 && gy == 240);
    CHECK(!present_map_point(v, 640, 480, 1900, 2000, &gx, &gy));
    CHECK(gx == 639 && gy == 479);
}

static void test_fullscreen_and_resize(void) {
    present_config cfg;
    uint16_t a;
    wm_msg m;
    int w, h;
    printf("test_present_fullscreen\n");
    memset(&cfg, 0, sizeof cfg);
    boot(&cfg);
    if (!pr) return;
    wclass("W", PROC_A);
    a = wmake("W", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, 0);
    wm_set_focus(&wm, a);
    wdrain();
    CHECK(!present_is_fullscreen(pr));
    present_inject_key(pr, 0x40000044, true);                       /* F11 */
    present_inject_key(pr, 0x40000044, false);
    present_pump(pr, false);
    CHECK(present_is_fullscreen(pr));
    CHECK(!next(&m, WM_KEYDOWN, WM_KEYUP));                         /* not forwarded to the guest */
    CHECK(!next(&m, 0, 0xFFFF) || m.message != WM_KEYDOWN);
    present_inject_key(pr, 0x40000044, true);
    present_pump(pr, false);
    CHECK(!present_is_fullscreen(pr));
    /* Alt+Enter toggles as well; Enter and the Alt release are swallowed, Alt itself is a plain key */
    present_inject_key_mod(pr, '\r', true, 0x100);                  /* KMOD_LALT */
    present_pump(pr, false);
    CHECK(present_is_fullscreen(pr));
    present_set_fullscreen(pr, false);
    CHECK(!present_is_fullscreen(pr));
    /* resize: the viewport follows and the mouse maps back to 640x480 through the letterbox */
    present_resize(pr, 1280, 600);                                  /* 800x600 picture, 240 px bars at both sides */
    present_pump(pr, false);
    present_window_size(pr, &w, &h);
    CHECK(w == 1280 && h == 600);
    present_inject_motion(pr, 240 + 400, 300);                      /* centre of the picture */
    present_pump(pr, false);
    CHECK_EQ(wm.cursor_x, 320);
    CHECK_EQ(wm.cursor_y, 240);
    present_inject_motion(pr, 10, 300);                             /* in the left bar: clamped to x = 0 */
    present_pump(pr, false);
    CHECK_EQ(wm.cursor_x, 0);
    CHECK_EQ(wm.cursor_y, 240);
    /* pixels: the picture fills the viewport, the bars are black */
    {
        fb_rect r = {0, 0, 640, 480};
        fb_fill_rect(&wfb, &r, FB_RGB(255, 0, 0));
        present_flush(pr);
        CHECK_EQ(present_pixel(pr, 10, 300), 0);
        CHECK_EQ(present_pixel(pr, 640, 300), 0xFF0000);
        CHECK_EQ(present_pixel(pr, 1270, 300), 0);
    }
    down();
}

/* The corner button: a click on it toggles fullscreen and never reaches the guest; elsewhere nothing changes. */
static void test_fs_button(void) {
    present_config cfg;
    wm_msg m;
    int ww, wh;
    printf("test_present_fs_button\n");
    memset(&cfg, 0, sizeof cfg);
    cfg.scale = 2;
    boot(&cfg);
    if (!pr) return;
    wclass("W", PROC_A);
    wmake("W", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, 0);
    wdrain();
    present_window_size(pr, &ww, &wh);
    CHECK(!present_is_fullscreen(pr));
    present_inject_button(pr, 0, true, ww - 20, 20);               /* invisible button: a normal click */
    present_inject_button(pr, 0, false, ww - 20, 20);
    present_pump(pr, false);
    CHECK(!present_is_fullscreen(pr));
    CHECK(next(&m, WM_LBUTTONDOWN, WM_LBUTTONDOWN));
    CHECK(next(&m, WM_LBUTTONUP, WM_LBUTTONUP));
    wdrain();
    present_inject_motion(pr, ww - 20, 20);                        /* now it shows */
    present_pump(pr, false);
    CHECK(next(&m, WM_MOUSEMOVE, WM_MOUSEMOVE));
    present_inject_button(pr, 0, true, ww - 20, 20);
    present_inject_button(pr, 0, false, ww - 20, 20);
    present_pump(pr, false);
    CHECK(present_is_fullscreen(pr));                              /* toggled (the dummy driver allows it) */
    CHECK(!next(&m, WM_LBUTTONDOWN, WM_LBUTTONUP));                /* press and release both swallowed */
    down();
    memset(&cfg, 0, sizeof cfg);
    cfg.scale = 2;
    cfg.no_fs_button = true;
    boot(&cfg);
    if (!pr) return;
    wclass("W", PROC_A);
    wmake("W", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, 0);
    wdrain();
    present_window_size(pr, &ww, &wh);
    present_inject_motion(pr, ww - 20, 20);
    present_inject_button(pr, 0, true, ww - 20, 20);
    present_pump(pr, false);
    CHECK(!present_is_fullscreen(pr));                             /* disabled: the guest gets the click */
    CHECK(next(&m, WM_LBUTTONDOWN, WM_LBUTTONDOWN));
    down();
}

int main(void) {
    test_headless_driver_and_upload();
    test_pacing_and_frame_limit();
    test_mouse_events();
    test_keyboard_events();
    test_key_table();
    test_quit();
    test_viewport_math();
    test_fullscreen_and_resize();
    test_fs_button();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
