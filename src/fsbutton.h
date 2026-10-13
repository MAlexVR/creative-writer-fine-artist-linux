#ifndef FSBUTTON_H
#define FSBUTTON_H
/* The in-window fullscreen button (experiment, removable: delete this unit and its few calls in present_sdl.c).
 * Pure functions only: geometry of the top-right corner button, when it is visible (mouse moved recently or near the
 * corner), whether a click belongs to it (then it is not forwarded to the guest) and its pixel shape. Times are ms. */
#include <stdbool.h>
#include <stdint.h>

#define FSB_SIZE 28
#define FSB_MARGIN 6
#define FSB_HIDE_MS 2000
#define FSB_FADE_MS 400
#define FSB_NEAR 90            /* the mouse within this many pixels of the corner keeps it visible */

typedef struct { int x, y, w, h; } fsb_rect;
typedef struct {
    bool disabled;             /* --no-fs-button / CWFA_NO_FS_BUTTON */
    bool seen;                 /* any mouse movement yet */
    bool near_corner;
    uint32_t moved_ms;
} fsb_state;

void fsb_init(fsb_state *s);
fsb_rect fsb_rect_for(int win_w, int win_h);
bool fsb_hit(fsb_rect r, int x, int y);
void fsb_mouse_moved(fsb_state *s, int x, int y, int win_w, int win_h, uint32_t now_ms);
bool fsb_visible(const fsb_state *s, uint32_t now_ms);
int fsb_alpha(const fsb_state *s, uint32_t now_ms);      /* 0..255 */
/* True when the click is on the visible button (the caller toggles fullscreen and drops the event). */
bool fsb_click(const fsb_state *s, int x, int y, int win_w, int win_h, uint32_t now_ms);
/* FSB_SIZE x FSB_SIZE bytes: 0 background, 1 border, 2 glyph (anything not 7 is written; the rest is left as it was). */
void fsb_draw_mask(uint8_t *px, bool fullscreen);

#endif
