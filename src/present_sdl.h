#ifndef PRESENT_SDL_H
#define PRESENT_SDL_H

#include <stdbool.h>
#include <stdint.h>

#include "fb.h"
#include "wm.h"

/* The only module that touches SDL2: shows the framebuffer in a window and
 * turns SDL mouse, keyboard and quit events into Win16 input (wm_input_event).
 *
 * The screen stays what Windows 3.1 drew on a 256-colour driver: the 640x480
 * 8-bit palettized fb is the authoritative surface (palette animation and
 * GDI palette calls land on it). The presenter only converts it with
 * the current palette when it uploads, so it never owns pixels of its own.
 * The window uses SDL's software window surface (no renderer or GL), which
 * also works with SDL_VIDEODRIVER=dummy for tests.
 *
 * present_pump has the user_config.pump signature: it is the host's idle step
 * (events in, pixels out). It returns false when the program must stop: the
 * window was closed with no Win16 window left to answer, or the frame limit was
 * reached. A frame is one call of the pump. */

typedef struct present present_t;

typedef struct {
    const char *title;      /* default "Creative Writer / Fine Artist (win16rt)" */
    int scale;              /* integer window scale, default 1 */
    bool headless;          /* use SDL's dummy video driver: no display needed */
    unsigned long max_frames; /* pump returns false after this many calls; 0: unlimited */
    unsigned min_present_ms;  /* minimum time between uploads, default 16 */
    bool fullscreen;        /* start in desktop fullscreen (SDL_WINDOW_FULLSCREEN_DESKTOP) */
    bool fixed_size;        /* not resizable (the default window can be resized, aspect kept) */
    bool no_fs_button;      /* no in-window fullscreen button (src/fsbutton.[ch]) */
    bool integer_scale;     /* only whole-number picture scales inside the window (black bars); default: fill */
    const uint32_t *icon_argb; /* window icon (0xAARRGGBB, icon_w x icon_h), NULL for none; copied by SDL */
    int icon_w, icon_h;
    const char *app_id;     /* WM_CLASS / Wayland app_id (what a .desktop file's StartupWMClass or name matches) */
} present_config;

/* Pure geometry of the picture inside the window: the largest rectangle of the framebuffer's aspect ratio that fits
 * (centred, the rest is black bars). integer_only rounds the scale down to a whole number when that is at least 1x. */
typedef struct { int x, y, w, h; } present_rect;
present_rect present_viewport(int win_w, int win_h, int fb_w, int fb_h, bool integer_only);
/* Window pixel -> framebuffer pixel through the viewport. A point in the bars is clamped to the nearest edge pixel
 * (so a drag keeps working) and reported by returning false. */
bool present_map_point(present_rect vp, int fb_w, int fb_h, int wx, int wy, int *gx, int *gy);

present_t *present_create(const present_config *cfg, fb_t *fb, wm_t *wm);
void present_destroy(present_t *p);

/* Events and display only: for programs that wait on the clock without a message loop (no frame counting). */
void present_refresh(void *ctx);
bool present_pump(void *ctx, bool wait);

/* Uploads the framebuffer now, whatever the pacing says. */
void present_flush(present_t *p);

unsigned long present_frames(const present_t *p);    /* pump calls so far */
unsigned long present_uploads(const present_t *p);   /* framebuffer uploads so far */
const char *present_driver(const present_t *p);      /* SDL video driver in use */
/* Pixel of the window surface after the last upload, 0x00RRGGBB; 0 outside. */
uint32_t present_pixel(const present_t *p, int x, int y);
/* Win16 virtual-key code of an SDL keycode (SDLK_*), 0 when there is none. */
unsigned present_vk_from_sdl(int sdl_keycode);

/* Desktop fullscreen. F11 and Alt+Enter toggle it at run time; those keys are not forwarded to the guest. */
bool present_set_fullscreen(present_t *p, bool on);
bool present_is_fullscreen(const present_t *p);
void present_window_size(const present_t *p, int *w, int *h);

/* Test hooks: queue a real SDL event, as the OS would deliver it. Mouse
 * positions are window pixels; keys are SDL keycodes; button 0 left, 1 right,
 * 2 middle. */
bool present_inject_motion(present_t *p, int x, int y);
bool present_inject_button(present_t *p, int button, bool down, int x, int y);
bool present_inject_key(present_t *p, int sdl_keycode, bool down);
bool present_inject_key_mod(present_t *p, int sdl_keycode, bool down, unsigned sdl_kmod);
void present_resize(present_t *p, int w, int h);        /* ask for a window size (as dragging the border would) */
bool present_inject_quit(present_t *p);

#endif
