/* Scripted input: parsing, the events delivered to the window manager, pacing. */
#include "inject.h"
#include "wmfixture.h"

static unsigned drain_count(uint16_t msg, uint16_t *first_wp) {
    wm_msg m;
    unsigned n = 0;
    while (wm_next_message(&wm, &m, 0, 0x100, 0x209, true))        /* keys and mouse only: paints would never end */
        if (m.message == msg) {
            if (n == 0 && first_wp) *first_wp = m.wparam;
            n++;
        }
    return n;
}

static void test_parse(void) {
    char err[80];
    inject_t *in;
    printf("test_inject_parse\n");
    in = inject_parse("wait:2; key:RETURN; key:ctrl+n; type:Hi 1; click:10,20; shot:/tmp/x.ppm; quit", err, sizeof err);
    CHECK(in != NULL);
    CHECK_EQ(inject_steps_left(in), 10);               /* "Hi 1" is four keys */
    inject_free(in);
    CHECK(inject_parse("", err, sizeof err) != NULL);
    CHECK(inject_parse("key:NOSUCH", err, sizeof err) == NULL);
    CHECK(strstr(err, "NOSUCH") != NULL);
    CHECK(inject_parse("bogus:1", err, sizeof err) == NULL);
    CHECK(strstr(err, "bogus"));
    CHECK(inject_parse("click:abc", err, sizeof err) == NULL);
    CHECK(inject_parse("click:10", err, sizeof err) == NULL);
    CHECK(inject_parse("key:hyper+x", err, sizeof err) == NULL);
    CHECK(inject_parse("type:\x01", err, sizeof err) == NULL);
    CHECK(inject_parse("key:F13", err, sizeof err) == NULL);
}

static void test_keys(void) {
    inject_t *in;
    char err[80];
    uint16_t wp = 0;
    printf("test_inject_keys\n");
    wboot();
    wdef_through = true;
    CHECK(wclass("T", 0) != 0);
    wm_set_focus(&wm, wmake("T", WS_POPUP | WS_VISIBLE, 0, 0, 100, 100, 0));
    drain_count(0xFFFF, NULL);
    in = inject_parse("key:RETURN; key:ctrl+n; keydown:A; keyup:A; type:ab", err, sizeof err);
    CHECK(in != NULL);
    CHECK(inject_run(in, &wm, &wfb));                  /* one step per call */
    CHECK_EQ(drain_count(WM_KEYDOWN, &wp), 1);
    CHECK_EQ(wp, 0x0D);
    CHECK(inject_run(in, &wm, &wfb));                  /* Ctrl+N: Control down, N down, N up, Control up */
    {
        wm_msg m;
        unsigned seq[8], n = 0;
        while (wm_next_message(&wm, &m, 0, 0x100, 0x209, true) && n < 8) seq[n++] = (m.message == WM_KEYDOWN ? 0x100 : 0x200) | m.wparam;
        CHECK_EQ(n, 4);
        CHECK(seq[0] == (0x100 | 0x11) && seq[1] == (0x100 | 'N') && seq[2] == (0x200 | 'N') && seq[3] == (0x200 | 0x11));
    }
    CHECK(inject_run(in, &wm, &wfb));                  /* keydown:A */
    CHECK_EQ(wm_key_state(&wm, 'A') & 0x8000, 0x8000);
    CHECK(inject_run(in, &wm, &wfb));                  /* keyup:A */
    CHECK_EQ(wm_key_state(&wm, 'A') & 0x8000, 0);
    drain_count(0xFFFF, NULL);
    CHECK(inject_run(in, &wm, &wfb));
    CHECK(inject_run(in, &wm, &wfb));                  /* the two typed letters, one per call */
    CHECK(!inject_run(in, &wm, &wfb));                 /* nothing left */
    CHECK(inject_done(in));
    inject_free(in);
    wdown();
}

static void test_mouse_and_pacing(void) {
    inject_t *in;
    char err[80];
    wm_msg m;
    unsigned downs = 0, ups = 0, dbl = 0;
    printf("test_inject_mouse\n");
    wboot();
    wdef_through = true;
    {
        wm_class_def d;
        memset(&d, 0, sizeof d);
        d.name = "T";
        d.hinstance = 0x1111;
        d.style = CS_DBLCLKS;
        wm_register_class(&wm, &d);
    }
    CHECK(wmake("T", WS_POPUP | WS_VISIBLE, 0, 0, 100, 100, 0) != 0);
    drain_count(0xFFFF, NULL);
    in = inject_parse("wait:2; click:10,20; dblclick:30,40; rclick:5,5; shot:build/inject_test.ppm", err, sizeof err);
    CHECK(in != NULL);
    CHECK(inject_run(in, &wm, &wfb));                  /* wait:2 uses two frames: this call and the next */
    CHECK(inject_run(in, &wm, &wfb));
    CHECK(!wm_next_message(&wm, &m, 0, 0x100, 0x209, false));
    CHECK(inject_run(in, &wm, &wfb));                  /* click */
    while (wm_next_message(&wm, &m, 0, 0x100, 0x209, true)) {
        if (m.message == WM_LBUTTONDOWN) { downs++; CHECK_EQ((int16_t)m.lparam, 10); CHECK_EQ((int16_t)(m.lparam >> 16), 20); }
        if (m.message == WM_LBUTTONUP) ups++;
    }
    CHECK(downs == 1 && ups == 1);
    wclock += 50;
    CHECK(inject_run(in, &wm, &wfb));                  /* dblclick: down up, then down as WM_LBUTTONDBLCLK, up */
    downs = ups = 0;
    while (wm_next_message(&wm, &m, 0, 0x100, 0x209, true)) {
        downs += m.message == WM_LBUTTONDOWN;
        dbl += m.message == WM_LBUTTONDBLCLK;
        ups += m.message == WM_LBUTTONUP;
    }
    CHECK(downs == 1 && dbl == 1 && ups == 2);
    CHECK(inject_run(in, &wm, &wfb));                  /* rclick */
    {
        unsigned r = 0;
        while (wm_next_message(&wm, &m, 0, 0x100, 0x209, true)) r += m.message == WM_RBUTTONDOWN || m.message == WM_RBUTTONUP;
        CHECK_EQ(r, 2);
    }
    remove("build/inject_test.ppm");
    CHECK(!inject_run(in, &wm, &wfb));                 /* the shot runs without using a frame, then the script is over */
    {
        FILE *f = fopen("build/inject_test.ppm", "rb");
        CHECK(f != NULL);
        if (f) fclose(f);
    }
    inject_free(in);
    in = inject_parse("quit", err, sizeof err);
    CHECK(inject_run(in, &wm, &wfb));
    CHECK(wm_next_message(&wm, &m, 0, WM_SYSCOMMAND, WM_SYSCOMMAND, true) && m.wparam == 0xF060);   /* the close box */
    inject_free(in);
    wdown();
}

/* Spanish text: type: takes UTF-8 and delivers the Windows ANSI (code page 1252) character, the way a Spanish
 * keyboard's dead-key and N-tilde keys end up as WM_CHAR. */
static void test_accents(void) {
    inject_t *in;
    char err[80];
    wm_msg m;
    unsigned want[] = {0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xF1, 0xBF, 0xA1, 0xD1, 0xC1, 'a'}, got[16], n = 0, i;
    printf("test_inject_accents\n");
    wboot();
    wdef_through = true;
    CHECK(wclass("T", 0) != 0);
    wm_set_focus(&wm, wmake("T", WS_POPUP | WS_VISIBLE, 0, 0, 100, 100, 0));
    drain_count(0xFFFF, NULL);
    in = inject_parse("type:\xc3\xa1\xc3\xa9\xc3\xad\xc3\xb3\xc3\xba\xc3\xb1\xc2\xbf\xc2\xa1\xc3\x91\xc3\x81" "a", err, sizeof err);
    CHECK(in != NULL);
    CHECK_EQ(inject_steps_left(in), 11);
    while (inject_run(in, &wm, &wfb)) {
        while (wm_next_message(&wm, &m, 0, 0x100, 0x209, true)) {
            if (m.message == WM_CHAR && n < 16) got[n++] = m.wparam;
            if (m.message == WM_KEYDOWN) {
                m.ch = 0;                                  /* the program sees a MSG copy: no ch field */
                wm_translate(&wm, &m);
            }
        }
    }
    CHECK_EQ(n, 11);
    for (i = 0; i < n && i < 11; i++) CHECK_EQ(got[i], want[i]);
    inject_free(in);
    CHECK(inject_parse("type:\xe2\x82\xac", err, sizeof err) == NULL);   /* the euro sign is not reachable */
    wdown();
}

/* waitms:N waits for N milliseconds of the window manager's clock (the program's time), however many frames pass:
 * speech, animations and timers of the programs run on that clock, not on frames. */
static void test_wait_ms(void) {
    inject_t *in;
    char err[200];
    printf("test_inject_wait_ms\n");
    wboot();
    in = inject_parse("waitms:500; wait:1; quit", err, sizeof err);
    CHECK(in != NULL);
    if (!in) { wdown(); return; }
    CHECK(inject_run(in, &wm, &wfb));                  /* starts the wait at clock 1000 */
    wclock += 499;
    CHECK(inject_run(in, &wm, &wfb));                  /* 499 ms: still waiting, however many frames */
    CHECK(inject_run(in, &wm, &wfb));
    CHECK_EQ(inject_steps_left(in), 2);                /* wait:1 and quit are still to come */
    wclock += 1;
    CHECK(inject_run(in, &wm, &wfb));                  /* 500 ms: the next step (wait:1) runs */
    CHECK_EQ(inject_steps_left(in), 1);
    CHECK(inject_run(in, &wm, &wfb));                  /* quit */
    CHECK_EQ(inject_steps_left(in), 0);
    CHECK(!inject_run(in, &wm, &wfb));
    inject_free(in);
    in = inject_parse("wait:1; stop; wait:5", err, sizeof err);   /* stop: ends the run (the host checks inject_stopped) */
    CHECK(in != NULL);
    if (in) {
        CHECK(!inject_stopped(in));
        inject_run(in, &wm, &wfb);
        CHECK(!inject_stopped(in));
        inject_run(in, &wm, &wfb);
        CHECK(inject_stopped(in));
        inject_free(in);
    }
    in = inject_parse("waitms:abc", err, sizeof err);  /* a number is required */
    CHECK(in == NULL);
    wdown();
}

int main(void) {
    test_parse();
    test_accents();
    test_keys();
    test_mouse_and_pacing();
    test_wait_ms();
    printf("test_inject: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
