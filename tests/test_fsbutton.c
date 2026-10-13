/* fsbutton: pure geometry, visibility timing and pixel shape of the in-window fullscreen button. */
#include "fixture.h"
#include "fsbutton.h"

static void test_rect(void) {
    fsb_rect r;
    printf("test_fsbutton_rect\n");
    r = fsb_rect_for(1280, 960);
    CHECK_EQ(r.w, FSB_SIZE);
    CHECK_EQ(r.x + r.w + FSB_MARGIN, 1280);       /* top-right corner */
    CHECK_EQ(r.y, FSB_MARGIN);
    r = fsb_rect_for(1920, 1080);
    CHECK_EQ(r.x + r.w + FSB_MARGIN, 1920);
    CHECK(fsb_hit(r, r.x, r.y));
    CHECK(fsb_hit(r, r.x + r.w - 1, r.y + r.h - 1));
    CHECK(!fsb_hit(r, r.x - 1, r.y));
    CHECK(!fsb_hit(r, r.x, r.y + r.h));
    CHECK(!fsb_hit(r, 0, 0));
}

static void test_visibility(void) {
    fsb_state s;
    fsb_rect r = fsb_rect_for(1280, 960);
    printf("test_fsbutton_visibility\n");
    fsb_init(&s);
    CHECK(!fsb_visible(&s, 0));                                  /* hidden until the mouse moves */
    fsb_mouse_moved(&s, 100, 500, 1280, 960, 1000);              /* away from the corner */
    CHECK(fsb_visible(&s, 1000));                                /* recent movement shows it */
    CHECK(fsb_visible(&s, 1000 + FSB_HIDE_MS - 1));
    CHECK(!fsb_visible(&s, 1000 + FSB_HIDE_MS));                 /* hides after ~2 s */
    fsb_mouse_moved(&s, r.x + 3, r.y + 3, 1280, 960, 5000);      /* near the corner: stays while it is there */
    CHECK(fsb_visible(&s, 5000 + 10 * FSB_HIDE_MS));
    fsb_mouse_moved(&s, 100, 500, 1280, 960, 20000);
    CHECK(fsb_visible(&s, 20000));
    CHECK(!fsb_visible(&s, 20000 + FSB_HIDE_MS));
    /* alpha fades out in the last 400 ms */
    CHECK_EQ(fsb_alpha(&s, 20000), 255);
    CHECK(fsb_alpha(&s, 20000 + FSB_HIDE_MS - 100) < 255 && fsb_alpha(&s, 20000 + FSB_HIDE_MS - 100) > 0);
    CHECK_EQ(fsb_alpha(&s, 20000 + FSB_HIDE_MS), 0);
}

static void test_click(void) {
    fsb_state s;
    fsb_rect r = fsb_rect_for(1280, 960);
    printf("test_fsbutton_click\n");
    fsb_init(&s);
    CHECK(!fsb_click(&s, r.x + 2, r.y + 2, 1280, 960, 0));       /* invisible: not a button, the guest gets the click */
    fsb_mouse_moved(&s, r.x + 2, r.y + 2, 1280, 960, 10);
    CHECK(fsb_click(&s, r.x + 2, r.y + 2, 1280, 960, 20));       /* visible and inside: consumed */
    CHECK(!fsb_click(&s, 300, 300, 1280, 960, 20));              /* elsewhere: forwarded */
    s.disabled = true;
    CHECK(!fsb_click(&s, r.x + 2, r.y + 2, 1280, 960, 20));
    CHECK(!fsb_visible(&s, 20));
}

static void test_draw(void) {
    uint8_t px[FSB_SIZE * FSB_SIZE];
    unsigned i, lit = 0;
    printf("test_fsbutton_draw\n");
    memset(px, 7, sizeof px);
    fsb_draw_mask(px, false);                                    /* "enter fullscreen" icon */
    for (i = 0; i < sizeof px; i++) if (px[i] != 7) lit++;
    CHECK(lit > 20);
    { uint8_t q[FSB_SIZE * FSB_SIZE]; unsigned n = 0;
      memset(q, 7, sizeof q);
      fsb_draw_mask(q, true);                                    /* "restore" icon differs */
      CHECK(memcmp(px, q, sizeof px) != 0);
      for (i = 0; i < sizeof q; i++) if (q[i] != 7) n++;
      CHECK(n > 20); }
}

int main(void) {
    test_rect();
    test_visibility();
    test_click();
    test_draw();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
