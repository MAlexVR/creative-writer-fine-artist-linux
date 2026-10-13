#include "fsbutton.h"

#include <string.h>

void fsb_init(fsb_state *s) { memset(s, 0, sizeof *s); }

fsb_rect fsb_rect_for(int win_w, int win_h) {
    fsb_rect r;
    (void)win_h;
    r.w = r.h = FSB_SIZE;
    r.x = win_w - FSB_SIZE - FSB_MARGIN;
    r.y = FSB_MARGIN;
    return r;
}

bool fsb_hit(fsb_rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

void fsb_mouse_moved(fsb_state *s, int x, int y, int win_w, int win_h, uint32_t now_ms) {
    s->seen = true;
    s->moved_ms = now_ms;
    s->near_corner = x >= win_w - FSB_NEAR && y <= FSB_NEAR && win_h > 0;
}

bool fsb_visible(const fsb_state *s, uint32_t now_ms) {
    if (s->disabled || !s->seen) return false;
    return s->near_corner || (uint32_t)(now_ms - s->moved_ms) < FSB_HIDE_MS;
}

int fsb_alpha(const fsb_state *s, uint32_t now_ms) {
    uint32_t age;
    if (!fsb_visible(s, now_ms)) return 0;
    if (s->near_corner) return 255;
    age = now_ms - s->moved_ms;
    if (age + FSB_FADE_MS <= FSB_HIDE_MS) return 255;
    return (int)(255 * (FSB_HIDE_MS - age) / FSB_FADE_MS);
}

bool fsb_click(const fsb_state *s, int x, int y, int win_w, int win_h, uint32_t now_ms) {
    return fsb_visible(s, now_ms) && fsb_hit(fsb_rect_for(win_w, win_h), x, y);
}

void fsb_draw_mask(uint8_t *px, bool fullscreen) {
    int x, y;
    for (y = 0; y < FSB_SIZE; y++)
        for (x = 0; x < FSB_SIZE; x++) {
            bool border = x == 0 || y == 0 || x == FSB_SIZE - 1 || y == FSB_SIZE - 1;
            int gx = x - 7, gy = y - 7, g = FSB_SIZE - 14;      /* glyph box 14x14 */
            bool glyph = false;
            if (gx >= 0 && gy >= 0 && gx < g && gy < g) {
                int ex = gx < 4 || gx >= g - 4, ey = gy < 4 || gy >= g - 4;   /* corner brackets */
                if (!fullscreen) glyph = (gx < 2 || gx >= g - 2 || gy < 2 || gy >= g - 2) && ex && ey;
                else glyph = gx >= 3 && gx < g - 3 && gy >= 3 && gy < g - 3 && (gx < 5 || gx >= g - 5 || gy < 5 || gy >= g - 5);   /* a framed box */
            }
            if (glyph) px[y * FSB_SIZE + x] = 2;
            else if (border) px[y * FSB_SIZE + x] = 1;
            else px[y * FSB_SIZE + x] = 0;
        }
}
