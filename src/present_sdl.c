/* SDL2 presenter. See present_sdl.h. */
#include "present_sdl.h"
#include "fsbutton.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

struct present {
    fb_t *fb;
    wm_t *wm;
    present_config cfg;
    SDL_Window *win;
    SDL_Surface *src;        /* wraps the framebuffer memory */
    SDL_Palette *pal;
    unsigned long frames, uploads, last_dirty;
    uint32_t last_upload;
    bool uploaded, closed, owns_video;
    bool fullscreen, swallow_alt_up;
    int win_w, win_h;        /* window size in event coordinates */
    fsb_state fsb;           /* the in-window fullscreen button */
    bool fsb_swallow_up;     /* the button press was taken: its release must not reach the guest either */
    bool fsb_was_shown;
};

static const char *default_title = "Creative Writer / Fine Artist (win16rt)";

unsigned present_vk_from_sdl(int k) {
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (unsigned)(k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (unsigned)(k - SDLK_0);
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (unsigned)(k - SDLK_F1);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return 0x61 + (unsigned)(k - SDLK_KP_1);
    switch (k) {
    case SDLK_KP_0: return 0x60;
    case SDLK_KP_MULTIPLY: return 0x6A;
    case SDLK_KP_PLUS: return 0x6B;
    case SDLK_KP_MINUS: return 0x6D;
    case SDLK_KP_PERIOD: return 0x6E;
    case SDLK_KP_DIVIDE: return 0x6F;
    case SDLK_KP_ENTER: case SDLK_RETURN: return 0x0D;
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB: return 0x09;
    case SDLK_ESCAPE: return 0x1B;
    case SDLK_SPACE: return 0x20;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
    case SDLK_LALT: case SDLK_RALT: return 0x12;
    case SDLK_CAPSLOCK: return 0x14;
    case SDLK_PAGEUP: return 0x21;
    case SDLK_PAGEDOWN: return 0x22;
    case SDLK_END: return 0x23;
    case SDLK_HOME: return 0x24;
    case SDLK_LEFT: return 0x25;
    case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27;
    case SDLK_DOWN: return 0x28;
    case SDLK_INSERT: return 0x2D;
    case SDLK_DELETE: return 0x2E;
    case SDLK_NUMLOCKCLEAR: return 0x90;
    case SDLK_SCROLLLOCK: return 0x91;
    case SDLK_SEMICOLON: return 0xBA;
    case SDLK_EQUALS: return 0xBB;
    case SDLK_COMMA: return 0xBC;
    case SDLK_MINUS: return 0xBD;
    case SDLK_PERIOD: return 0xBE;
    case SDLK_SLASH: return 0xBF;
    case SDLK_BACKQUOTE: return 0xC0;
    case SDLK_LEFTBRACKET: return 0xDB;
    case SDLK_BACKSLASH: return 0xDC;
    case SDLK_RIGHTBRACKET: return 0xDD;
    case SDLK_QUOTE: return 0xDE;
    default: return 0;
    }
}

present_rect present_viewport(int win_w, int win_h, int fb_w, int fb_h, bool integer_only) {
    present_rect r = {0, 0, 0, 0};
    long w, h;
    if (win_w <= 0 || win_h <= 0 || fb_w <= 0 || fb_h <= 0) return r;
    if (integer_only && win_w >= fb_w && win_h >= fb_h) {
        int k = win_w / fb_w < win_h / fb_h ? win_w / fb_w : win_h / fb_h;
        w = (long)fb_w * k;
        h = (long)fb_h * k;
    } else if ((long)win_w * fb_h <= (long)win_h * fb_w) {           /* width-limited */
        w = win_w;
        h = ((long)win_w * fb_h + fb_w / 2) / fb_w;
    } else {                                                          /* height-limited */
        h = win_h;
        w = ((long)win_h * fb_w + fb_h / 2) / fb_h;
    }
    if (w > win_w) w = win_w;
    if (h > win_h) h = win_h;
    r.w = (int)w;
    r.h = (int)h;
    r.x = (win_w - r.w) / 2;
    r.y = (win_h - r.h) / 2;
    return r;
}

bool present_map_point(present_rect vp, int fb_w, int fb_h, int wx, int wy, int *gx, int *gy) {
    bool inside = wx >= vp.x && wy >= vp.y && wx < vp.x + vp.w && wy < vp.y + vp.h;
    long x, y;
    if (vp.w <= 0 || vp.h <= 0) {
        *gx = *gy = 0;
        return false;
    }
    x = (long)(wx - vp.x) * fb_w / vp.w;
    y = (long)(wy - vp.y) * fb_h / vp.h;
    if (wx < vp.x) x = 0;
    if (wy < vp.y) y = 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= fb_w) x = fb_w - 1;
    if (y >= fb_h) y = fb_h - 1;
    *gx = (int)x;
    *gy = (int)y;
    return inside;
}

static present_rect viewport_of(const present_t *p) {
    return present_viewport(p->win_w, p->win_h, p->fb->w, p->fb->h, p->cfg.integer_scale);
}

static void send_input(present_t *p, wm_in_kind kind, int x, int y, int button, unsigned vk, unsigned scan, bool rep) {
    wm_input e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    if (kind == WM_IN_KEY_DOWN || kind == WM_IN_KEY_UP || kind == WM_IN_QUIT) {
        e.x = x;
        e.y = y;
    } else {
        present_map_point(viewport_of(p), p->fb->w, p->fb->h, x, y, &e.x, &e.y);
    }
    e.button = button;
    e.vk = vk;
    e.scan = scan;
    e.repeat = rep;
    wm_input_event(p->wm, &e);
}

static int button_index(int sdl_button) {
    return sdl_button == SDL_BUTTON_RIGHT ? 1 : sdl_button == SDL_BUTTON_MIDDLE ? 2 : 0;
}

static void sync_size(present_t *p) {
    SDL_GetWindowSize(p->win, &p->win_w, &p->win_h);
    p->last_dirty = (unsigned long)-1;
}

bool present_set_fullscreen(present_t *p, bool on) {
    if (!p || !p->win) return false;
    if (SDL_SetWindowFullscreen(p->win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) return false;
    p->fullscreen = on;
    sync_size(p);
    return true;
}

bool present_is_fullscreen(const present_t *p) { return p && p->fullscreen; }

void present_window_size(const present_t *p, int *w, int *h) {
    *w = p->win_w;
    *h = p->win_h;
}

void present_resize(present_t *p, int w, int h) {
    if (!p || !p->win) return;
    SDL_SetWindowSize(p->win, w, h);
    sync_size(p);
}

static void handle_event(present_t *p, const SDL_Event *ev) {
    switch (ev->type) {
    case SDL_MOUSEMOTION:
        fsb_mouse_moved(&p->fsb, ev->motion.x, ev->motion.y, p->win_w, p->win_h, SDL_GetTicks());
        send_input(p, WM_IN_MOVE, ev->motion.x, ev->motion.y, 0, 0, 0, false);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT &&
            fsb_click(&p->fsb, ev->button.x, ev->button.y, p->win_w, p->win_h, SDL_GetTicks())) {
            present_set_fullscreen(p, !p->fullscreen);       /* the button: the guest never sees this click */
            p->fsb_swallow_up = true;
            break;
        }
        if (ev->type == SDL_MOUSEBUTTONUP && p->fsb_swallow_up) {
            p->fsb_swallow_up = false;
            break;
        }
        send_input(p, ev->type == SDL_MOUSEBUTTONDOWN ? WM_IN_BUTTON_DOWN : WM_IN_BUTTON_UP, ev->button.x,
                   ev->button.y, button_index(ev->button.button), 0, 0, false);
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        SDL_Keycode sym = ev->key.keysym.sym;
        bool down = ev->type == SDL_KEYDOWN;
        if (sym == SDLK_F11 || ((sym == SDLK_RETURN || sym == SDLK_KP_ENTER) && (ev->key.keysym.mod & KMOD_ALT))) {
            if (down && !ev->key.repeat) {                      /* the host's own keys: never seen by the program */
                present_set_fullscreen(p, !p->fullscreen);
                if (sym != SDLK_F11) p->swallow_alt_up = true;  /* the Alt release must not open a menu in the guest */
            }
            break;
        }
        if (!down && (sym == SDLK_LALT || sym == SDLK_RALT) && p->swallow_alt_up) {
            p->swallow_alt_up = false;
            send_input(p, WM_IN_KEY_UP, 0, 0, 0, 0x12, (unsigned)ev->key.keysym.scancode & 0xFF, false);
            break;
        }
        {
        unsigned vk = present_vk_from_sdl((int)ev->key.keysym.sym);
        if (vk)   /* the SDL scancode stands in for the PC scan code in lParam bits 16-23 */
            send_input(p, ev->type == SDL_KEYDOWN ? WM_IN_KEY_DOWN : WM_IN_KEY_UP, 0, 0, 0, vk,
                       (unsigned)ev->key.keysym.scancode & 0xFF, ev->key.repeat != 0);
        }
        break;
    }
    case SDL_WINDOWEVENT:
        if (ev->window.event == SDL_WINDOWEVENT_CLOSE) send_input(p, WM_IN_QUIT, 0, 0, 0, 0, 0, false);
        else if (ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || ev->window.event == SDL_WINDOWEVENT_RESIZED ||
                 ev->window.event == SDL_WINDOWEVENT_MAXIMIZED || ev->window.event == SDL_WINDOWEVENT_RESTORED) sync_size(p);
        else if (ev->window.event == SDL_WINDOWEVENT_EXPOSED) p->last_dirty = (unsigned long)-1;
        break;
    case SDL_QUIT:
        send_input(p, WM_IN_QUIT, 0, 0, 0, 0, 0, false);
        break;
    default:
        break;
    }
}

/* The corner button, blended over the picture (or the black bar). Removable with src/fsbutton.[ch]. */
static void draw_fsb(present_t *p, SDL_Surface *dst) {
    uint32_t now = SDL_GetTicks();
    int alpha = fsb_alpha(&p->fsb, now), x, y;
    fsb_rect r;
    uint8_t mask[FSB_SIZE * FSB_SIZE];
    p->fsb_was_shown = alpha > 0;
    if (alpha <= 0 || dst->format->BytesPerPixel != 4) return;
    r = fsb_rect_for(dst->w, dst->h);          /* surface pixels (equals window units except on high-DPI) */
    fsb_draw_mask(mask, p->fullscreen);
    if (SDL_MUSTLOCK(dst) && SDL_LockSurface(dst) != 0) return;
    for (y = 0; y < r.h; y++)
        for (x = 0; x < r.w; x++) {
            static const int col[3][3] = {{20, 20, 20}, {255, 255, 255}, {255, 255, 255}};
            uint8_t k = mask[y * FSB_SIZE + x], dr, dg, db;
            int a = k == 0 ? alpha * 55 / 100 : alpha, px = r.x + x, py = r.y + y;
            uint32_t *q;
            if (px < 0 || py < 0 || px >= dst->w || py >= dst->h) continue;
            q = (uint32_t *)((uint8_t *)dst->pixels + (size_t)py * (size_t)dst->pitch) + px;
            SDL_GetRGB(*q, dst->format, &dr, &dg, &db);
            dr = (uint8_t)((dr * (255 - a) + col[k][0] * a) / 255);
            dg = (uint8_t)((dg * (255 - a) + col[k][1] * a) / 255);
            db = (uint8_t)((db * (255 - a) + col[k][2] * a) / 255);
            *q = SDL_MapRGB(dst->format, dr, dg, db);
        }
    if (SDL_MUSTLOCK(dst)) SDL_UnlockSurface(dst);
}

void present_flush(present_t *p) {
    SDL_Surface *dst;
    unsigned i;
    if (!p || !p->win) return;
    if (p->fb->bpp == 8) {
        SDL_Color c[256];
        for (i = 0; i < 256; i++) {
            c[i].r = (Uint8)(p->fb->pal[i] >> 16);
            c[i].g = (Uint8)(p->fb->pal[i] >> 8);
            c[i].b = (Uint8)p->fb->pal[i];
            c[i].a = 255;
        }
        SDL_SetPaletteColors(p->pal, c, 0, 256);
    }
    if (!(dst = SDL_GetWindowSurface(p->win))) return;
    {
        present_rect v = viewport_of(p);
        SDL_Rect to;
        /* the surface may be in pixels while events are in window units (high-DPI): same ratio on both axes */
        to.x = v.x * dst->w / (p->win_w > 0 ? p->win_w : dst->w);
        to.y = v.y * dst->h / (p->win_h > 0 ? p->win_h : dst->h);
        to.w = v.w * dst->w / (p->win_w > 0 ? p->win_w : dst->w);
        to.h = v.h * dst->h / (p->win_h > 0 ? p->win_h : dst->h);
        if (to.w != dst->w || to.h != dst->h) SDL_FillRect(dst, NULL, SDL_MapRGB(dst->format, 0, 0, 0));
        if (to.w == p->fb->w && to.h == p->fb->h) SDL_BlitSurface(p->src, NULL, dst, &to);
        else SDL_BlitScaled(p->src, NULL, dst, &to);
    }
    draw_fsb(p, dst);
    SDL_UpdateWindowSurface(p->win);
    p->last_dirty = p->fb->dirty;
    p->last_upload = SDL_GetTicks();
    p->uploads++;
    p->uploaded = true;
}

bool present_pump(void *ctx, bool wait) {
    present_t *p = ctx;
    SDL_Event ev;
    bool last;
    p->frames++;
    if (wait) {
        if (SDL_WaitEventTimeout(&ev, 4)) handle_event(p, &ev);
    }
    while (SDL_PollEvent(&ev)) handle_event(p, &ev);
    last = p->cfg.max_frames && p->frames >= p->cfg.max_frames;
    if (p->fsb_was_shown || fsb_visible(&p->fsb, SDL_GetTicks())) p->last_dirty = (unsigned long)-1;   /* animate the button */
    if (p->fb->dirty != p->last_dirty &&
        (last || !p->uploaded || SDL_GetTicks() - p->last_upload >= p->cfg.min_present_ms))
        present_flush(p);
    return !last;
}

void present_refresh(void *ctx) {
    present_t *p = ctx;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) handle_event(p, &ev);
    if (p->fb->dirty != p->last_dirty && (!p->uploaded || SDL_GetTicks() - p->last_upload >= p->cfg.min_present_ms))
        present_flush(p);
}

present_t *present_create(const present_config *cfg, fb_t *fb, wm_t *wm) {
    present_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    if (cfg) p->cfg = *cfg;
    if (!p->cfg.title) p->cfg.title = default_title;
    if (p->cfg.scale < 1) p->cfg.scale = 1;
    if (!p->cfg.min_present_ms) p->cfg.min_present_ms = 16;
    p->fb = fb;
    p->wm = wm;
    fsb_init(&p->fsb);
    p->fsb.disabled = p->cfg.no_fs_button;
    p->last_dirty = (unsigned long)-1;
    if (p->cfg.headless) SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        free(p);
        return NULL;
    }
    p->owns_video = true;
    if (p->cfg.app_id && *p->cfg.app_id) {                 /* the shell matches the window to its launcher by this name */
        SDL_SetHint("SDL_VIDEO_X11_WMCLASS", p->cfg.app_id);
        SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS", p->cfg.app_id);
        SDL_SetHint("SDL_APP_ID", p->cfg.app_id);
    }
    p->win = SDL_CreateWindow(p->cfg.title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, fb->w * p->cfg.scale,
                              fb->h * p->cfg.scale,
                              (p->cfg.fixed_size ? 0 : SDL_WINDOW_RESIZABLE) |
                                  (p->cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (p->win) {
        p->fullscreen = p->cfg.fullscreen;
        sync_size(p);
        if (p->cfg.icon_argb && p->cfg.icon_w > 0 && p->cfg.icon_h > 0) {
            SDL_Surface *ic = SDL_CreateRGBSurfaceWithFormatFrom((void *)p->cfg.icon_argb, p->cfg.icon_w, p->cfg.icon_h, 32,
                                                                 p->cfg.icon_w * 4, SDL_PIXELFORMAT_ARGB8888);
            if (ic) {
                SDL_SetWindowIcon(p->win, ic);                 /* SDL copies the pixels */
                SDL_FreeSurface(ic);
            }
        }
    }
    if (fb->bpp == 8) {
        p->src = SDL_CreateRGBSurfaceWithFormatFrom(fb->p8, fb->w, fb->h, 8, fb->w, SDL_PIXELFORMAT_INDEX8);
        p->pal = SDL_AllocPalette(256);
        if (p->src && p->pal) SDL_SetSurfacePalette(p->src, p->pal);
    } else {
        p->src = SDL_CreateRGBSurfaceWithFormatFrom(fb->p32, fb->w, fb->h, 32, fb->w * 4, SDL_PIXELFORMAT_RGB888);
    }
    if (!p->win || !p->src || (fb->bpp == 8 && !p->pal)) {
        present_destroy(p);
        return NULL;
    }
    return p;
}

void present_destroy(present_t *p) {
    if (!p) return;
    if (p->src) SDL_FreeSurface(p->src);
    if (p->pal) SDL_FreePalette(p->pal);
    if (p->win) SDL_DestroyWindow(p->win);
    if (p->owns_video) SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    free(p);
}

unsigned long present_frames(const present_t *p) { return p->frames; }
unsigned long present_uploads(const present_t *p) { return p->uploads; }
const char *present_driver(const present_t *p) { (void)p; return SDL_GetCurrentVideoDriver(); }

uint32_t present_pixel(const present_t *p, int x, int y) {
    SDL_Surface *s = p->win ? SDL_GetWindowSurface(p->win) : NULL;
    uint8_t r, g, b;
    uint32_t v;
    if (!s || !p->uploaded || x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    if (s->format->BytesPerPixel != 4) return 0;
    memcpy(&v, (const uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch + (size_t)x * 4, 4);
    SDL_GetRGB(v, s->format, &r, &g, &b);
    return (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}

bool present_inject_motion(present_t *p, int x, int y) {
    SDL_Event e;
    (void)p;
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEMOTION;
    e.motion.x = x;
    e.motion.y = y;
    return SDL_PushEvent(&e) >= 0;
}

bool present_inject_button(present_t *p, int button, bool down, int x, int y) {
    SDL_Event e;
    (void)p;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.button = (Uint8)(button == 1 ? SDL_BUTTON_RIGHT : button == 2 ? SDL_BUTTON_MIDDLE : SDL_BUTTON_LEFT);
    e.button.x = x;
    e.button.y = y;
    return SDL_PushEvent(&e) >= 0;
}

bool present_inject_key(present_t *p, int sdl_keycode, bool down) { return present_inject_key_mod(p, sdl_keycode, down, 0); }

bool present_inject_key_mod(present_t *p, int sdl_keycode, bool down, unsigned sdl_kmod) {
    SDL_Event e;
    (void)p;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.keysym.mod = (Uint16)sdl_kmod;
    e.key.keysym.sym = sdl_keycode;
    e.key.keysym.scancode = SDL_GetScancodeFromKey((SDL_Keycode)sdl_keycode);
    return SDL_PushEvent(&e) >= 0;
}

bool present_inject_quit(present_t *p) {
    SDL_Event e;
    (void)p;
    memset(&e, 0, sizeof e);
    e.type = SDL_QUIT;
    return SDL_PushEvent(&e) >= 0;
}
