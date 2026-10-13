/* diag: the freeze watchdog and the --debug-live dump. */
#include "wmfixture.h"
#include "diag.h"

static void test_watch(void) {
    diag_watch w;
    uint32_t stalled = 0;
    printf("test_diag_watch\n");
    diag_watch_init(&w);
    CHECK(!diag_watch_poll(&w, 10, 1000, 5000, &stalled));      /* first sample only sets the origin */
    CHECK(!diag_watch_poll(&w, 10, 3000, 5000, &stalled));      /* still, but not long enough */
    CHECK(diag_watch_poll(&w, 10, 6000, 5000, &stalled));       /* 5 s without progress */
    CHECK_EQ(stalled, 5000);
    CHECK(!diag_watch_poll(&w, 10, 7000, 5000, &stalled));      /* reported: quiet until another interval has passed */
    CHECK(diag_watch_poll(&w, 10, 11500, 5000, &stalled));
    CHECK(!diag_watch_poll(&w, 11, 12000, 5000, &stalled));     /* progress resets everything */
    CHECK(!diag_watch_poll(&w, 11, 16000, 5000, &stalled));
    CHECK(diag_watch_poll(&w, 11, 17000, 5000, &stalled));
    CHECK(!diag_watch_poll(&w, 11, 17001, 0, &stalled));        /* a zero threshold disables it */
}

static void extra(void *ctx, FILE *f) { fprintf(f, "extra: %s\n", (const char *)ctx); }

static void test_dump(void) {
    hle_t h;
    diag_sources s;
    char buf[8192];
    size_t n;
    FILE *f;
    uint16_t a, b;
    wm_class_def d;
    printf("test_diag_dump\n");
    wboot();
    memset(&d, 0, sizeof d);
    d.name = "Splat";
    d.wndproc = PROC_A;
    d.hinstance = 0x1111;
    d.style = CS_OWNDC;
    wm_register_class(&wm, &d);
    wclass("W", PROC_A);
    a = wmake("Splat", WS_OVERLAPPED | WS_VISIBLE, 0, 0, 100, 100, 0);
    b = wmake("Splat", WS_CHILD | WS_VISIBLE, 5, 5, 20, 20, a);
    (void)b;
    wm_set_timer(&wm, 0, 1, 100, 0);
    wm_post(&wm, a, 0x401, 1, 2);
    wm_get_dc(&wm, a);
    hle_init(&h);
    h.ncalls = 3;
    memset(&s, 0, sizeof s);
    s.hle = &h;
    s.wm = &wm;
    s.now_ms = 4242;
    s.api_calls = 3;
    s.extra = extra;
    s.extra_ctx = "hello";
    f = tmpfile();
    diag_dump(f, &s, "unit test");
    rewind(f);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(strstr(buf, "reason: unit test") != NULL);
    CHECK(strstr(buf, "clock_ms: 4242") != NULL);
    CHECK(strstr(buf, "api_calls: 3") != NULL);
    CHECK(strstr(buf, "class=Splat") != NULL);
    CHECK(strstr(buf, "timers: 1") != NULL);
    CHECK(strstr(buf, "posted_messages: 1") != NULL);
    CHECK(strstr(buf, "dcs_in_use: 1") != NULL);
    CHECK(strstr(buf, "extra: hello") != NULL);
    CHECK(strstr(buf, "recent_api_calls") != NULL);
    hle_destroy(&h);
    wdown();
}

int main(void) {
    test_watch();
    test_dump();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
