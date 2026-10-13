#ifndef WM_H
#define WM_H

#include <stdbool.h>
#include <stdint.h>

#include "fb.h"

/* Win16 window manager: classes, the window tree and its Z-order, per-thread
 * message queue, timers, input state, paint state and device contexts. Pure
 * host code: the guest is reached through wm_ops only (user.c implements it by
 * calling guest window procedures through the CPU).
 *
 * Model and simplifications:
 *  - Window rectangles are kept in parent-client coordinates (screen
 *    coordinates for top-level windows); the non-client area is derived from
 *    the style (see wm_nc_insets) and WM_NCCALCSIZE is not sent.
 *  - The update region is the bounding rectangle of everything invalidated;
 *    ValidateRect only shrinks it when the validated rectangle covers it fully
 *    or trims a full-width/height edge.
 *  - Drawing goes straight to the framebuffer: the clip is the window's client
 *    area (or the paint rectangle) and never accounts for overlapping
 *    siblings. Exposing a region repaints the windows that intersect it.
 *  - Message priority: posted, input, WM_QUIT, WM_PAINT, WM_TIMER. */

#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUERYENDSESSION 0x0011
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCPAINT 0x0085
#define WM_NCACTIVATE 0x0086
#define WM_GETDLGCODE 0x0087
#define WM_NCMOUSEMOVE 0x00A0
#define WM_NCLBUTTONDOWN 0x00A1
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MBUTTONDBLCLK 0x0209
#define WM_USER 0x0400

#define WS_OVERLAPPED 0x00000000u
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_MINIMIZE 0x20000000u
#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u
#define WS_CLIPSIBLINGS 0x04000000u
#define WS_CLIPCHILDREN 0x02000000u
#define WS_MAXIMIZE 0x01000000u
#define WS_CAPTION 0x00C00000u
#define WS_BORDER 0x00800000u
#define WS_DLGFRAME 0x00400000u
#define WS_VSCROLL 0x00200000u
#define WS_HSCROLL 0x00100000u
#define WS_SYSMENU 0x00080000u
#define WS_THICKFRAME 0x00040000u
#define WS_OVERLAPPEDWINDOW 0x00CF0000u

#define CS_DBLCLKS 0x0008
#define CS_OWNDC 0x0020
#define CS_GLOBALCLASS 0x4000

#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA 8
#define SW_RESTORE 9

#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2

#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOREDRAW 0x0008
#define SWP_NOACTIVATE 0x0010
#define SWP_SHOWWINDOW 0x0040
#define SWP_HIDEWINDOW 0x0080

#define HTERROR (-2)
#define HTNOWHERE 0
#define HTCLIENT 1
#define HTCAPTION 2
#define HTSYSMENU 3
#define HTGROWBOX 4
#define HTMENU 5
#define HTHSCROLL 6
#define HTVSCROLL 7
#define HTREDUCE 8
#define HTZOOM 9
#define HTBORDER 18

#define GW_HWNDFIRST 0
#define GW_HWNDLAST 1
#define GW_HWNDNEXT 2
#define GW_HWNDPREV 3
#define GW_OWNER 4
#define GW_CHILD 5

#define COLOR_SCROLLBAR 0
#define COLOR_BACKGROUND 1
#define COLOR_ACTIVECAPTION 2
#define COLOR_WINDOW 5
#define COLOR_WINDOWFRAME 6
#define COLOR_BTNFACE 15
#define COLOR_NCOLORS 21

#define WM_CW_USEDEFAULT ((int16_t)0x8000)
#define WM_MAX_WINDOWS 1024
#define WM_MAX_CLASSES 64
#define WM_MAX_TIMERS 32
#define WM_MAX_DCS 32
#define WM_QUEUE_SIZE 512

typedef struct {
    uint16_t hwnd;
    uint16_t message;
    uint16_t wparam;
    uint32_t lparam;
    uint32_t time;
    int16_t pt_x, pt_y;
    uint8_t keys;   /* keyboard messages: Shift 1, Ctrl 2, Alt 4, CapsLock 8 as they were when queued */
    uint8_t ch;     /* WM_KEYDOWN/SYSKEYDOWN: the ANSI character TranslateMessage produces when not 0 (accents) */
} wm_msg;

typedef struct wm wm_t;

typedef struct {
    bool used;
    uint16_t atom;
    char name[64];
    uint16_t style;
    uint32_t wndproc;
    int16_t cls_extra, wnd_extra;
    uint16_t hinstance, hicon, hcursor, hbrbackground;
    uint32_t menu_name;
    bool builtin;
    uint8_t *extra;
} wm_class;

typedef struct {
    const char *name;
    uint16_t style;
    uint32_t wndproc;
    int16_t cls_extra, wnd_extra;
    uint16_t hinstance, hicon, hcursor, hbrbackground;
    uint32_t menu_name;
    bool builtin;
} wm_class_def;

typedef struct wm_create_params {
    const char *class_name;   /* or atom != 0 */
    uint16_t atom;
    const char *text;
    uint32_t style, exstyle;
    int16_t x, y, cx, cy;
    uint16_t parent, id_or_menu, hinstance;
    uint32_t param;           /* lpCreateParams */
    uint32_t class_name_far, text_far; /* guest pointers for the CREATESTRUCT */
} wm_create_params;

typedef struct {
    void *ctx;
    /* Run the window procedure at far address proc (sel << 16 | off). Also used
     * for TIMERPROCs, which take the same (HWND, WORD, WORD, DWORD) frame. */
    uint32_t (*call_wndproc)(void *ctx, uint32_t proc, uint16_t hwnd, uint16_t msg, uint16_t wparam,
                             uint32_t lparam);
    uint32_t (*now_ms)(void *ctx);
    /* Default window procedure for windows without one (and DefWindowProc);
     * NULL: wm_defproc. user.c adds the messages that carry guest pointers. */
    uint32_t (*defproc)(void *ctx, uint16_t hwnd, uint16_t msg, uint16_t wparam, uint32_t lparam);
    /* CREATESTRUCT for WM_NCCREATE/WM_CREATE: returns its far pointer (lParam);
     * leave is called when creation is over. NULL: lParam 0. */
    uint32_t (*createstruct_enter)(void *ctx, const struct wm_create_params *p);
    void (*createstruct_leave)(void *ctx);
    /* A top-level window created without a menu: the menu of its class (menu_name is the class's far pointer), or 0. */
    uint16_t (*class_menu)(void *ctx, uint16_t hinstance, uint32_t menu_name);
    /* Last call for a window about to be freed (after WM_NCDESTROY). */
    void (*destroyed)(void *ctx, uint16_t hwnd);
    /* WM_NCCALCSIZE (wParam 0) for a window rectangle in parent coordinates. On return *rc is the client rectangle
     * the window procedure (or the default) produced; true: use it, false: keep the style-derived client area. */
    bool (*nccalcsize)(void *ctx, uint16_t hwnd, fb_rect *rc);
} wm_ops;

typedef struct wm_window {
    uint16_t hwnd;
    uint16_t parent, owner;
    uint16_t first_child, prev, next; /* siblings: first_child is topmost, next is below */
    wm_class *cls;
    uint32_t style, exstyle, wndproc;
    uint16_t hinstance, id;
    fb_rect rect, restore_rect;
    bool has_client;             /* the window procedure's WM_NCCALCSIZE answer differs from the style's */
    int ci_l, ci_t, ci_r, ci_b;  /* ... and these are the client insets it gave */
    char *text;
    uint8_t *extra;
    int nextra;
    fb_rect update;
    bool has_update, erase, nc_dirty;
    bool destroying;
    uint32_t userdata;
    void *ui_data;             /* owned by the UI layer, freed through ops.destroyed */
} wm_window;

typedef struct {
    bool used;
    uint16_t hwnd, id;
    uint32_t interval, due, proc;
} wm_timer;

typedef struct {
    bool used;
    uint16_t hdc, hwnd;
    int org_x, org_y;  /* screen position of the DC origin */
    fb_rect clip;      /* screen coordinates */
    bool own;          /* the private DC of a CS_OWNDC window: ReleaseDC keeps it */
    unsigned serial;   /* unique per allocation: lets GDI tell a recycled handle from the old one */
} wm_dc;

typedef struct {
    uint16_t hdc;
    bool erase;
    fb_rect rc;        /* client coordinates */
} wm_paint;

typedef enum { WM_IN_MOVE, WM_IN_BUTTON_DOWN, WM_IN_BUTTON_UP, WM_IN_KEY_DOWN, WM_IN_KEY_UP, WM_IN_QUIT } wm_in_kind;

typedef struct {
    wm_in_kind kind;
    int x, y;          /* screen position for mouse events */
    int button;        /* 0 left, 1 right, 2 middle */
    unsigned vk;       /* Win16 virtual-key code */
    unsigned scan;
    unsigned ch;       /* key down: ANSI character to produce instead of the one derived from the VK (0 = derive) */
    bool repeat;
} wm_input;

typedef struct {
    wm_msg q[WM_QUEUE_SIZE];
    unsigned head, count;
} wm_queue;

struct wm {
    fb_t *fb;
    wm_ops ops;
    wm_class classes[WM_MAX_CLASSES];
    wm_window *win[WM_MAX_WINDOWS];
    unsigned next_slot;
    wm_timer timers[WM_MAX_TIMERS];
    wm_dc dcs[WM_MAX_DCS];
    uint16_t next_dc_h;
    unsigned dc_serial;
    wm_queue posted, input;
    bool quit_pending;
    int quit_code;
    uint16_t desktop, focus, active, capture;
    int cursor_x, cursor_y;
    int cursor_show;
    uint16_t cursor;
    uint8_t key_down[256], key_toggle[256];
    bool keys_valid;   /* the last retrieved message was a keyboard one: GetKeyState reads its modifiers */
    uint8_t keys_at;
    uint8_t next_ch;   /* ANSI character for the next queued key-down message (see wm_input.ch) */
    uint8_t last_ch;   /* ch of the key-down message retrieved last: the guest MSG that TranslateMessage sees has no such field */
    uint16_t last_hwnd, last_vk;
    unsigned buttons;
    wm_msg last_msg;
    uint32_t last_click_time;
    int last_click_x, last_click_y, last_click_button;
    uint16_t next_timer_id;
    uint16_t next_atom;
    unsigned cascade;
    uint32_t now_cache;
};

void wm_init(wm_t *w, fb_t *fb, const wm_ops *ops);
void wm_destroy(wm_t *w);
uint32_t wm_now(wm_t *w);

/* classes */
uint16_t wm_register_class(wm_t *w, const wm_class_def *d);   /* atom, 0 on failure */
bool wm_unregister_class(wm_t *w, const char *name, uint16_t hinstance);
wm_class *wm_find_class(wm_t *w, const char *name, uint16_t hinstance);
wm_class *wm_class_by_atom(wm_t *w, uint16_t atom);

/* windows */
wm_window *wm_get(wm_t *w, uint16_t hwnd);
bool wm_is_window(wm_t *w, uint16_t hwnd);
uint16_t wm_create_window(wm_t *w, const wm_create_params *p);
bool wm_destroy_window(wm_t *w, uint16_t hwnd);
bool wm_is_visible(wm_t *w, uint16_t hwnd);
bool wm_is_child(wm_t *w, uint16_t parent, uint16_t hwnd);
uint16_t wm_get_parent(wm_t *w, uint16_t hwnd);
uint16_t wm_set_parent(wm_t *w, uint16_t hwnd, uint16_t parent);
uint16_t wm_get_window(wm_t *w, uint16_t hwnd, unsigned cmd);
uint16_t wm_top_window(wm_t *w, uint16_t parent);
bool wm_enable_window(wm_t *w, uint16_t hwnd, bool enable); /* returns the previous disabled state */
bool wm_is_enabled(wm_t *w, uint16_t hwnd);
bool wm_show_window(wm_t *w, uint16_t hwnd, int cmd);       /* previous visibility */
bool wm_move_window(wm_t *w, uint16_t hwnd, int x, int y, int cx, int cy, bool repaint);
bool wm_set_window_pos(wm_t *w, uint16_t hwnd, uint16_t after, int x, int y, int cx, int cy, unsigned flags);
bool wm_bring_to_top(wm_t *w, uint16_t hwnd);
void wm_set_text(wm_t *w, uint16_t hwnd, const char *s);
const char *wm_get_text(wm_t *w, uint16_t hwnd);

/* geometry */
void wm_nc_insets(uint32_t style, uint32_t exstyle, bool menu, int *l, int *t, int *r, int *b);
bool wm_window_rect(wm_t *w, uint16_t hwnd, fb_rect *screen);
bool wm_insets(wm_t *w, uint16_t hwnd, int *l, int *t, int *r, int *b);   /* non-client widths */
/* Style or menu changed: the client area moved. Sends WM_SIZE when its size changed and schedules the repaint. */
void wm_frame_changed(wm_t *w, uint16_t hwnd);
bool wm_client_rect(wm_t *w, uint16_t hwnd, fb_rect *rc);   /* (0,0)-(cx,cy) */
bool wm_client_origin(wm_t *w, uint16_t hwnd, int *x, int *y); /* screen */
bool wm_client_to_screen(wm_t *w, uint16_t hwnd, int *x, int *y);
bool wm_screen_to_client(wm_t *w, uint16_t hwnd, int *x, int *y);
uint16_t wm_window_from_point(wm_t *w, int x, int y);

/* window / class words, byte offsets or GWW/GWL/GCW/GCL indices. */
bool wm_get_window_long(wm_t *w, uint16_t hwnd, int idx, uint32_t *v, bool is_word);
bool wm_set_window_long(wm_t *w, uint16_t hwnd, int idx, uint32_t v, bool is_word, uint32_t *old);
bool wm_get_class_long(wm_t *w, uint16_t hwnd, int idx, uint32_t *v, bool is_word);
bool wm_set_class_long(wm_t *w, uint16_t hwnd, int idx, uint32_t v, bool is_word, uint32_t *old);

/* focus, activation, capture */
uint16_t wm_set_focus(wm_t *w, uint16_t hwnd);
uint16_t wm_set_active(wm_t *w, uint16_t hwnd);
uint16_t wm_set_capture(wm_t *w, uint16_t hwnd);
void wm_release_capture(wm_t *w);

/* painting */
void wm_invalidate(wm_t *w, uint16_t hwnd, const fb_rect *rc, bool erase);
void wm_validate(wm_t *w, uint16_t hwnd, const fb_rect *rc);
bool wm_get_update_rect(wm_t *w, uint16_t hwnd, fb_rect *rc, bool erase);
void wm_update_window(wm_t *w, uint16_t hwnd);
uint16_t wm_begin_paint(wm_t *w, uint16_t hwnd, wm_paint *ps);
void wm_end_paint(wm_t *w, uint16_t hwnd, const wm_paint *ps);
uint32_t wm_sys_color(int index);

/* device contexts */
uint16_t wm_get_dc(wm_t *w, uint16_t hwnd);       /* hwnd 0: whole screen */
uint16_t wm_get_window_dc(wm_t *w, uint16_t hwnd); /* origin at the window's top-left corner, clip: the whole window */
bool wm_release_dc(wm_t *w, uint16_t hwnd, uint16_t hdc);
wm_dc *wm_dc_get(wm_t *w, uint16_t hdc);
void wm_dc_fill(wm_t *w, uint16_t hdc, const fb_rect *rc, uint32_t colorref); /* rc in DC coordinates */

/* messages */
uint32_t wm_send(wm_t *w, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp);
uint32_t wm_defproc(wm_t *w, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp);
bool wm_post(wm_t *w, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp);
void wm_post_quit(wm_t *w, int code);
/* Next message for the filter (hwnd 0, min == max == 0: any). Non-blocking. */
bool wm_next_message(wm_t *w, wm_msg *out, uint16_t hwnd, uint16_t min, uint16_t max, bool remove);
uint32_t wm_dispatch(wm_t *w, const wm_msg *m);
bool wm_translate(wm_t *w, const wm_msg *m);
bool wm_have_message(wm_t *w);

/* timers */
uint16_t wm_set_timer(wm_t *w, uint16_t hwnd, uint16_t id, uint32_t interval, uint32_t proc);
bool wm_kill_timer(wm_t *w, uint16_t hwnd, uint16_t id);

/* input */
void wm_input_event(wm_t *w, const wm_input *ev);
int wm_key_state(wm_t *w, unsigned vk);        /* GetKeyState semantics */
int wm_async_key_state(wm_t *w, unsigned vk);

#endif
