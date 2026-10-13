#ifndef UI_PRIV_H
#define UI_PRIV_H
/* Internal to the ui_*.c files. */
#include "res.h"
#include "ui.h"
#include "user_priv.h"

#include <stdlib.h>
#include <string.h>

static inline char *ui_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

#define UI_MAX_IMAGES 256
#define UI_IMAGE_BASE 0x6800
#define UI_MAX_ACCEL 32
#define UI_ACCEL_BASE 0x7000

typedef struct {
    bool used, cursor, system;
    int w, h, hot_x, hot_y;
    uint32_t *cref;        /* w * h COLORREFs */
    uint8_t *mask;         /* 1 = AND bit set */
    uint16_t hinst;
    uint16_t handle;       /* what the program holds: the CURSORSHAPE global block of a cursor, else UI_IMAGE_BASE + slot */
    uint16_t gh;           /* the global block behind a cursor handle (0: none) */
    uint32_t key;          /* resource id, or a hash of the name */
    char name[24];
} ui_image;

typedef struct {
    bool used;
    res_accel *e;
    size_t n;
    uint16_t hinst;
    uint32_t key;
} ui_accel;

/* ---- menus ---------------------------------------------------------------------------- */

#define MF_GRAYED 0x0001
#define MF_DISABLED 0x0002
#define MF_BITMAP 0x0004
#define MF_CHECKED 0x0008
#define MF_POPUP 0x0010
#define MF_MENUBARBREAK 0x0020
#define MF_MENUBREAK 0x0040
#define MF_HILITE 0x0080
#define MF_OWNERDRAW 0x0100
#define MF_BYPOSITION 0x0400
#define MF_SEPARATOR 0x0800
#define MF_SYSMENU 0x2000
#define MF_HELP 0x4000
#define MF_MOUSESELECT 0x8000
#define MF_STATEMASK (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_MENUBARBREAK | MF_MENUBREAK | MF_HILITE | MF_SEPARATOR | MF_HELP)

#define UI_MENU_BASE 0x6000
#define UI_MAX_MENUS 512

typedef struct {
    uint16_t flags;   /* MF_* */
    uint16_t id;      /* command, or the submenu handle of a popup */
    char *text;       /* NULL for separators, bitmaps and owner-draw items */
    uint32_t data;    /* bitmap handle or owner-draw data */
    int x, w;         /* layout in the menu bar or popup, set by the last layout pass */
} ui_item;

typedef struct {
    bool used;
    ui_item *it;
    int n, cap;
} ui_menu;

/* ---- scroll bars, controls and dialogs ------------------------------------------------------------ */

#define WM_HSCROLL 0x0114
#define WM_VSCROLL 0x0115
#define WM_CTLCOLOR 0x0019
#define SB_HORZ 0
#define SB_VERT 1
#define SB_CTL 2
#define SB_BOTH 3
#define SB_LINEUP 0
#define SB_LINEDOWN 1
#define SB_PAGEUP 2
#define SB_PAGEDOWN 3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK 5
#define SB_TOP 6
#define SB_BOTTOM 7
#define SB_ENDSCROLL 8
#define SB_BTN 16        /* arrow button and thumb size */

typedef struct { int min, max, pos; bool dis_lo, dis_hi; } ui_sb;
typedef struct { int start, len; } tline;

typedef struct {
    char **s;
    uint32_t *data;
    uint8_t *sel;
    int n, cap;
    int cur, top, caret;
} ui_list;

typedef enum { CK_NONE = 0, CK_BUTTON, CK_STATIC, CK_EDIT, CK_LISTBOX, CK_COMBO, CK_SCROLL } ck_kind;

typedef struct ui_ctl {
    ck_kind kind;
    int check;                    /* button: 0, 1, 2 (indeterminate) */
    bool pressed, inside;         /* mouse interaction */
    int sel_a, sel_b;             /* edit: anchor and caret; combo edit part too */
    int maxlen, hscroll, vscroll_line, want_x, pwchar;
    bool modified, readonly, dragging;
    ui_list list;                 /* listbox, combobox */
    bool dropped;
    ui_sb sb;                     /* scroll bar control */
    int item_h;
} ui_ctl;

/* Per-window UI state, hung off wm_window.ui_data. */
typedef struct ui_win {
    uint16_t sysmenu;
    ui_ctl *ctl;
    struct ui_dlg *dlg;
    ui_sb sb[2];                  /* window scroll bars: SB_HORZ, SB_VERT */
    int cell;                     /* font cell height of a dialog and its controls, 0: the system font */
    uint16_t font;                /* HFONT given with WM_SETFONT (0: none): wins over cell for this control */
    int base_x, base_y;           /* dialog base units */
} ui_win;

#define WM_INITMENU 0x116
#define WM_INITMENUPOPUP 0x117
#define WM_MENUSELECT 0x11F
#define WM_MENUCHAR 0x120
#define WM_ENTERIDLE 0x121
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_DELETE 0x2E
#define VK_F10 0x79
#define WM_SYSKEYDOWN_ 0x0104
#define WM_SYSKEYUP_ 0x0105
#define WM_SYSCHAR_ 0x0106

#define DLGC_WANTARROWS 0x0001
#define DLGC_WANTTAB 0x0002
#define DLGC_WANTALLKEYS 0x0004
#define DLGC_HASSETSEL 0x0008
#define DLGC_DEFPUSHBUTTON 0x0010
#define DLGC_UNDEFPUSHBUTTON 0x0020
#define DLGC_RADIOBUTTON 0x0040
#define DLGC_WANTCHARS 0x0080
#define DLGC_STATIC 0x0100
#define DLGC_BUTTON 0x2000
#define MEM_MAX_SEL_ 8192

#define WS_GROUP_ 0x00020000u
#define WS_TABSTOP_ 0x00010000u

#define SC_SIZE 0xF000
#define SC_MOVE 0xF010
#define SC_MINIMIZE 0xF020
#define SC_MAXIMIZE 0xF030
#define SC_NEXTWINDOW 0xF040
#define SC_PREVWINDOW 0xF050
#define SC_CLOSE 0xF060
#define SC_MOUSEMENU 0xF090
#define SC_KEYMENU 0xF100
#define SC_RESTORE 0xF120

#define WS_MINIMIZEBOX_ 0x00020000u
#define WS_MAXIMIZEBOX_ 0x00010000u

/* Non-client geometry of a top-level window, in window coordinates (origin: its top-left corner). */
typedef struct {
    int w, h, f;                 /* size and frame width */
    bool has_cap, has_sys, has_min, has_max, has_bar;
    fb_rect cap, sys, minb, maxb, bar;
} nc_geom;

/* clipboard and caret state */
#define UI_CLIP_MAX 16
#define UI_CLIP_REG 32
typedef struct {
    unsigned fmt;
    uint8_t *data;
    size_t len;
    uint16_t program_handle;      /* what the program gave SetClipboardData (GDI handles are kept as they are) */
    uint16_t hmem;                /* the global block handed out by GetClipboardData */
    bool delayed;
} ui_clip_fmt;

typedef struct {
    ui_clip_fmt f[UI_CLIP_MAX];
    int n;
    uint16_t open_by, owner, viewer;
    bool changed;
    char reg[UI_CLIP_REG][64];
    int nreg;
} ui_clip;

typedef struct {
    uint16_t hwnd;
    int x, y, w, h, hide, blink;
    bool drawn;
} ui_caret;

struct ui {
    user_t *u;
    ui_clip clip;
    ui_caret caret;
    ui_menu menu[UI_MAX_MENUS];
    uint16_t trk_hwnd;           /* window whose menu bar is being tracked, 0: none */
    int trk_item;                /* highlighted bar item */
    bool alt_only;               /* Alt went down and nothing else did since */
    ui_image img[UI_MAX_IMAGES];
    ui_accel acc[UI_MAX_ACCEL];
};

ui_image *ui_image_get(ui_t *ui, uint16_t h);
kernel_t *ui_kernel(user_t *u);   /* the KERNEL behind GlobalAlloc, NULL without one */
uint16_t ui_icon_from_text(user_t *u, uint16_t hinst, const char *text);
void ui_draw_image(user_t *u, uint16_t hdc, int x, int y, const ui_image *im);

ui_win *ui_win_get(ui_t *ui, uint16_t hwnd, bool create);
ui_menu *ui_menu_get(ui_t *ui, uint16_t h);
uint16_t ui_menu_new(ui_t *ui);
void ui_menu_destroy(ui_t *ui, uint16_t h);
uint16_t ui_menu_load(ui_t *ui, const uint8_t *blob, size_t len);
uint16_t ui_class_menu(user_t *u, uint16_t hinst, uint32_t menu_name);
void ui_window_destroyed(user_t *u, uint16_t hwnd);
bool ui_menu_find(ui_t *ui, uint16_t h, unsigned item, unsigned flags, uint16_t *mh, int *idx);
void ui_sysmenu_refresh(ui_t *ui, uint16_t hwnd, uint16_t h);
uint16_t ui_sysmenu_get(ui_t *ui, uint16_t hwnd, bool revert);

bool ui_nc_geom(user_t *u, uint16_t hwnd, nc_geom *g);
void ui_ncpaint(user_t *u, uint16_t hwnd);
int ui_nchittest(user_t *u, uint16_t hwnd, int sx, int sy);
/* The bar item under a point given in window coordinates, -1 if none. */
int ui_menubar_item_at(user_t *u, uint16_t hwnd, int wx, int wy);
void ui_menubar_layout(user_t *u, uint16_t hmenu, const nc_geom *g);
/* menu item text split into label (without '&'), the index of the underlined character (-1) and the text after a tab */
void ui_item_label(const ui_item *it, char *label, size_t cap, int *ul, char *accel, size_t acap);

bool ui_nc_defproc(user_t *u, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp, uint32_t *ret);
void ui_menu_text_at(user_t *u, uint16_t dc, int x, int y, const char *label, int ul, uint32_t fg);

void ui_track_window_menu(user_t *u, uint16_t hwnd, bool sys, int start, bool popup_open, bool mouse);
bool ui_track_popup(user_t *u, uint16_t hmenu, unsigned flags, int x, int y, uint16_t hwnd);
int ui_menubar_mnemonic(user_t *u, uint16_t hwnd, unsigned ch);

/* drawing helpers (ui.c) */
void ui_fill(user_t *u, uint16_t dc, int l, int t, int r, int b, uint32_t c);
void ui_outline(user_t *u, uint16_t dc, int l, int t, int r, int b, uint32_t c);
void ui_dotted(user_t *u, uint16_t dc, int l, int t, int r, int b);
/* A cell value with this bit set carries a GDI font handle in its low 16 bits (WM_SETFONT) instead of a cell height. */
#define UI_CELL_FONT 0x10000
int ui_cell_of(user_t *u, uint16_t hwnd);
int ui_tw(user_t *u, int cell, const char *s, int n);
int ui_th(user_t *u, int cell);
void ui_text(user_t *u, uint16_t dc, int cell, int x, int y, const char *s, int n, uint32_t fg, uint32_t bg);
/* sends WM_COMMAND(id, hwnd | code << 16) to the parent of a control */
void ui_notify_parent(user_t *u, uint16_t hwnd, unsigned code);
/* colours of a control: the defaults for its type, then what the parent's WM_CTLCOLOR says */
void ui_ctlcolors(user_t *u, uint16_t hwnd, uint16_t hdc, int type, uint32_t *fg, uint32_t *bg);

/* scroll bars (ui_scroll.c) */
typedef struct sbtrack {
    user_t *u;
    uint16_t notify, ctl;
    bool vert;
    ui_sb *sb;
    fb_rect rect;                 /* screen rectangle of the bar */
    void (*redraw)(void *ctx, int pressed);
    void *rctx;
} sbtrack;
void ui_sb_draw(user_t *u, uint16_t dc, const fb_rect *r, bool vert, const ui_sb *s, int pressed);
void ui_sb_track(sbtrack *t, int sx, int sy);
ui_sb *ui_sb_of(user_t *u, uint16_t hwnd, int bar);
bool ui_nc_sb_rect(user_t *u, uint16_t hwnd, bool vert, fb_rect *r);
void ui_nc_scrollbars(user_t *u, uint16_t dc, uint16_t hwnd);
void ui_nc_sb_press(user_t *u, uint16_t hwnd, bool vert, uint32_t lp);
extern const hle_def ui_scroll_defs[];
extern const size_t ui_scroll_ndefs;

int ui_wrap_text(user_t *u, int cell, const char *s, int width, bool allow_wrap, tline *out, int max);
void ui_ctl_free(ui_ctl *c);
bool ui_is_dialog_message(user_t *u, uint16_t dlg, const wm_msg *m);
void ui_dlg_btn_focus(user_t *u, uint16_t btn, bool gained, uint16_t other);
extern const hle_def ui_dlg_defs[];
extern const size_t ui_dlg_ndefs;
void ui_text_changed(user_t *u, uint16_t hwnd);
bool ui_dlg_defproc(user_t *u, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp, uint32_t *ret);
bool ui_edit_proc(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, uint16_t msg, uint16_t wp, uint32_t lp, uint32_t *ret);
bool ui_list_proc(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c, uint16_t msg, uint16_t wp, uint32_t lp, uint32_t *ret);
bool ui_ctl_defproc(user_t *u, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp, uint32_t *ret);

void ui_clip_set_text(user_t *u, const char *s, int n);
char *ui_clip_get_text(user_t *u);    /* malloc'd, NULL when there is no text */
void ui_edit_text_set(user_t *u, uint16_t hwnd, wm_window *x, ui_ctl *c);
extern const hle_def ui_misc_defs[];
extern const size_t ui_misc_ndefs;
extern const hle_def ui_commdlg_defs[];
extern const size_t ui_commdlg_ndefs;

/* tables registered by ui_create */
extern const hle_def ui_res_defs[];
extern const size_t ui_res_ndefs;
extern const hle_def ui_track_defs[];
extern const size_t ui_track_ndefs;
extern const hle_def ui_menu_defs[];
extern const size_t ui_menu_ndefs;
extern const hle_def ui_kbd_defs[];
extern const size_t ui_kbd_ndefs;

#endif
