/* Dialogs: templates in dialog units, DialogBox / CreateDialog and their variants, the message order
 * (WM_INITDIALOG ...), the dialog item API, IsDialogMessage keyboard handling through a modal loop driven by
 * injected input, DS_SETFONT metrics and MessageBox. */
#include "gfixture.h"
#include "res.h"
#include "rfixture.h"

enum {
    U_MESSAGEBOX = 1, U_DIALOGBOX = 87, U_ENDDIALOG = 88, U_CREATEDIALOG = 89, U_ISDIALOGMESSAGE = 90, U_GETDLGITEM = 91,
    U_SETDLGITEMTEXT = 92, U_GETDLGITEMTEXT = 93, U_SETDLGITEMINT = 94, U_GETDLGITEMINT = 95, U_CHECKRADIOBUTTON = 96,
    U_CHECKDLGBUTTON = 97, U_ISDLGBUTTONCHECKED = 98, U_SENDDLGITEMMESSAGE = 101, U_MAPDIALOGRECT = 103,
    U_DIALOGBOXINDIRECT = 218, U_CREATEDIALOGINDIRECT = 219, U_NEXTGROUP = 227, U_NEXTTAB = 228,
    U_DIALOGBOXPARAM = 239, U_CREATEDIALOGPARAM = 241, U_GETDLGCTRLID = 277, U_DEFDLGPROC = 308
};
#define U_CODE_E 0x800     /* logs like U_CODE_A, returns FALSE, except TRUE for WM_INITDIALOG */
#define U_CODE_F 0x900     /* on WM_COMMAND from IDOK/IDCANCEL calls EndDialog(hDlg, wParam) and returns TRUE; FALSE otherwise */
#define WM_INITDIALOG_ 0x110
#define BM_GETCHECK 0x400

#define DS_MODALFRAME 0x80u
#define DS_SETFONT 0x40u
#define WS_CHILD_VIS 0x50000000u
#define VK_RETURN 0x0D
#define VK_ESCAPE 0x1B
#define VK_TAB 0x09
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SHIFT 0x10
#define VK_MENU 0x12

static uint32_t px(int x, int y) { return fb_colorref_at(user_fb(uu), x, y); }

/* ---- guest dialog procedures --------------------------------------------------------------------- */

static void install_dlgprocs(void) {
    static const uint8_t logging_prefix[] = {
        0x55, 0x89, 0xE5,
        0xA3, 0x32, 0x00, 0x8C, 0xD8, 0xA3, 0x30, 0x00,
        0xA1, 0x40, 0x00, 0xB9, 0x0A, 0x00, 0xF7, 0xE1, 0x89, 0xC3,
        0x8B, 0x46, 0x06, 0x89, 0x87, 0x00, 0x01,
        0x8B, 0x46, 0x08, 0x89, 0x87, 0x02, 0x01,
        0x8B, 0x46, 0x0A, 0x89, 0x87, 0x04, 0x01,
        0x8B, 0x46, 0x0C, 0x89, 0x87, 0x06, 0x01,
        0x8B, 0x46, 0x0E, 0x89, 0x87, 0x08, 0x01,
        0xFF, 0x06, 0x40, 0x00};
    static const uint8_t e_tail[] = {
        0x31, 0xC0, 0x31, 0xD2,                    /* xor ax,ax; xor dx,dx */
        0x81, 0x7E, 0x0C, 0x10, 0x01,              /* cmp word [bp+12],110h */
        0x75, 0x03, 0xB8, 0x01, 0x00,              /* jne +3; mov ax,1 */
        0x5D, 0xCA, 0x0A, 0x00};
    uint8_t e[sizeof logging_prefix + sizeof e_tail];
    uint8_t f[] = {
        0x55, 0x89, 0xE5,
        0x81, 0x7E, 0x0C, 0x11, 0x01,             /*  3: cmp word [bp+12],111h */
        0x75, 0x16,                               /*  8: jne other */
        0x83, 0x7E, 0x0A, 0x03,                   /* 10: cmp word [bp+10],3 */
        0x73, 0x10,                               /* 14: jae other: only IDOK, IDCANCEL end the dialog */
        0xFF, 0x76, 0x0E, 0xFF, 0x76, 0x0A,       /* 16: push hDlg; push wParam */
        0x9A, 0, 0, 0, 0,                         /* 22: call far EndDialog */
        0xB8, 0x01, 0x00,                         /* 27: mov ax,1 */
        0xEB, 0x02,                               /* 30: jmp done */
        0x31, 0xC0,                               /* 32: other: xor ax,ax */
        0x31, 0xD2, 0x5D, 0xCA, 0x0A, 0x00};      /* 34: done: xor dx,dx; pop bp; retf 10 */
    loader_thunk *t = fx_thunk(&uf, "USER", U_ENDDIALOG);
    memcpy(e, logging_prefix, sizeof logging_prefix);
    memcpy(e + sizeof logging_prefix, e_tail, sizeof e_tail);
    u_put(U_CODE_E, e, sizeof e);
    f[23] = (uint8_t)t->off; f[24] = (uint8_t)(t->off >> 8);
    f[25] = (uint8_t)t->sel; f[26] = (uint8_t)(t->sel >> 8);
    u_put(U_CODE_F, f, sizeof f);
}

/* ---- scripted input ---------------------------------------------------------------------------------- */

typedef struct { wm_in_kind kind; int x, y; unsigned vk; } step;
static const step *script;
static unsigned nscript, script_pos;
static void (*script_hook)(unsigned i);

static void run_script(bool wait) {
    wm_input in;
    (void)wait;
    if (script_hook) script_hook(script_pos);
    if (script_pos >= nscript) return;
    memset(&in, 0, sizeof in);
    in.kind = script[script_pos].kind;
    in.x = script[script_pos].x;
    in.y = script[script_pos].y;
    in.vk = script[script_pos].vk;
    script_pos++;
    wm_input_event(user_wm(uu), &in);
}

static void set_script(const step *s, unsigned n, void (*hook)(unsigned)) {
    script = s;
    nscript = n;
    script_pos = 0;
    script_hook = hook;
    upump_script = run_script;
}

#define KEY(vk) {WM_IN_KEY_DOWN, 0, 0, (vk)}, {WM_IN_KEY_UP, 0, 0, (vk)}

/* ---- templates ------------------------------------------------------------------------------------------ */

static void dlg_head(rb_t *b, uint32_t style, unsigned n, int x, int y, int cx, int cy, const char *caption, bool font) {
    b->n = 0;
    rb_u32(b, style);
    rb_u8(b, n);
    rb_u16(b, (unsigned)x); rb_u16(b, (unsigned)y); rb_u16(b, (unsigned)cx); rb_u16(b, (unsigned)cy);
    rb_u8(b, 0);          /* no menu */
    rb_u8(b, 0);          /* default class */
    rb_str(b, caption);
    if (font) { rb_u16(b, 8); rb_str(b, "MS Sans Serif"); }
}

static void dlg_item(rb_t *b, int x, int y, int cx, int cy, unsigned id, uint32_t style, unsigned cls, const char *text) {
    rb_u16(b, (unsigned)x); rb_u16(b, (unsigned)y); rb_u16(b, (unsigned)cx); rb_u16(b, (unsigned)cy);
    rb_u16(b, id);
    rb_u32(b, style);
    rb_u8(b, cls);
    rb_str(b, text);
    rb_u8(b, 0);
}

/* controls: static, check box, two radio buttons, default OK, Cancel; 150 x 80 dialog units */
static void main_template(rb_t *b) {
    dlg_head(b, 0x90C800C0u, 6, 10, 20, 150, 80, "Test", true);
    dlg_item(b, 4, 4, 40, 8, 0xFFFF, WS_CHILD_VIS | 0x20000, 0x82, "Name:");
    dlg_item(b, 4, 16, 60, 10, 10, WS_CHILD_VIS | 0x10000 | 3, 0x80, "&Check");
    dlg_item(b, 4, 30, 40, 10, 20, WS_CHILD_VIS | 0x10000 | 0x20000 | 9, 0x80, "R&A");
    dlg_item(b, 4, 42, 40, 10, 21, WS_CHILD_VIS | 9, 0x80, "RB");
    dlg_item(b, 50, 60, 40, 14, 1, WS_CHILD_VIS | 0x10000 | 1, 0x80, "&OK");
    dlg_item(b, 96, 60, 40, 14, 2, WS_CHILD_VIS | 0x10000, 0x80, "Cancel");
}

static void add_main(const char *name) {
    rb_t b;
    main_template(&b);
    rf_add(&uf, RT_DIALOG, NULL, 0, name, b.b, b.n);
}

static uint16_t item(uint16_t dlg, unsigned id) { return UA(U_GETDLGITEM, dlg, (uint16_t)id); }
static unsigned ctrl_id(uint16_t h) { return UA(U_GETDLGCTRLID, h); }

/* Hooks run inside a modal loop, in the middle of an API call: they may not call into the guest. */
static uint16_t host_item(uint16_t dlg, unsigned id) {
    wm_window *d = wm_get(user_wm(uu), dlg);
    uint16_t k;
    for (k = d ? d->first_child : 0; k; k = wm_get(user_wm(uu), k)->next)
        if (wm_get(user_wm(uu), k)->id == id) return k;
    return 0;
}

static uint16_t host_dialog(void) {
    wm_window *d = wm_get(user_wm(uu), user_wm(uu)->desktop);
    uint16_t k, dlg = 0;
    for (k = d->first_child; k; k = wm_get(user_wm(uu), k)->next)
        if (wm_get(user_wm(uu), k)->cls && !strcmp(wm_get(user_wm(uu), k)->cls->name, "#32770")) dlg = k;
    return dlg;
}

static void boot(void) {
    gboot();
    install_dlgprocs();
    add_main("MAIN");
    ulog_reset();
}

static int count_msgs(uint16_t msg) {
    int n = 0;
    unsigned i;
    for (i = 0; i < ulog_n(); i++) n += ulog_w(i, ULOG_MSG) == msg;
    return n;
}

/* ---- tests ----------------------------------------------------------------------------------------------- */

static void test_create_dialog(void) {
    uint16_t dlg, nm, ok, cb, s;
    fb_rect cl, wr, r;
    int i, ox, oy;
    printf("test_dlg_create\n");
    boot();
    nm = ustr("main");
    dlg = UA(U_CREATEDIALOGPARAM, u_ds(), FARP(nm, 0), 0, FARP(u_cs(), U_CODE_E), LONG(0x00BB00AAu));
    CHECK(dlg != 0);
    CHECK(wm_is_window(user_wm(uu), dlg));
    CHECK(wm_is_visible(user_wm(uu), dlg));                    /* the template says WS_VISIBLE */
    /* the dialog procedure's first message: WM_INITDIALOG(hFirstTabStop, lParam) */
    CHECK(ulog_n() >= 1);
    CHECK_EQ(ulog_w(0, ULOG_MSG), WM_INITDIALOG_);
    CHECK_EQ(ulog_w(0, ULOG_WP), item(dlg, 10));                /* the first WS_TABSTOP control: the check box */
    CHECK_EQ(ulog_lp(0), 0x00BB00AAu);
    CHECK_EQ(ulog_w(0, ULOG_HWND), dlg);
    /* the dialog procedure saw the dialog being shown afterwards */
    i = ulog_find(0x18, dlg, 0);
    CHECK(i > 0);
    /* controls exist with their ids, in template order (GW_CHILD is the first) */
    ok = item(dlg, 1);
    cb = item(dlg, 2);
    s = item(dlg, 10);
    CHECK(ok && cb && s && item(dlg, 20) && item(dlg, 21));
    CHECK_EQ(item(dlg, 999), 0);
    CHECK_EQ(UA(262, dlg, 5), item(dlg, 0xFFFF));              /* GetWindow(GW_CHILD) is the first control of the template */
    CHECK_EQ(ctrl_id(ok), 1);
    CHECK_EQ(ctrl_id(cb), 2);
    CHECK_EQ(UA(46, ok), dlg);                                 /* GetParent */
    /* the focus went to the first tab stop because the dialog procedure returned TRUE */
    CHECK_EQ(user_wm(uu)->focus, item(dlg, 10));
    /* geometry: 8 point MS Sans Serif gives base units 6 x 13; the client area is 150x80 dialog units */
    wm_client_rect(user_wm(uu), dlg, &cl);
    CHECK_EQ(cl.right, 150 * 6 / 4);
    CHECK_EQ(cl.bottom, 80 * 13 / 8);
    wm_window_rect(user_wm(uu), ok, &wr);
    wm_client_origin(user_wm(uu), dlg, &ox, &oy);
    CHECK_EQ(wr.left, ox + 50 * 6 / 4);
    CHECK_EQ(wr.top, oy + 60 * 13 / 8);
    CHECK_EQ(wr.right - wr.left, 40 * 6 / 4);
    CHECK_EQ(wr.bottom - wr.top, 14 * 13 / 8);
    /* the dialog window: positioned at (10, 20) dialog units from the screen (no owner) */
    wm_window_rect(user_wm(uu), dlg, &r);
    CHECK_EQ(r.left, 10 * 6 / 4);
    CHECK_EQ(r.top, 20 * 13 / 8);
    /* MapDialogRect */
    {
        uint16_t rs = g_seg(16);
        uwr16(rs, 0, 4); uwr16(rs, 2, 8); uwr16(rs, 4, 40); uwr16(rs, 6, 16);
        UA(U_MAPDIALOGRECT, dlg, FARP(rs, 0));
        CHECK_EQ(urd16(rs, 0), 4 * 6 / 4);
        CHECK_EQ(urd16(rs, 2), 8 * 13 / 8);
        CHECK_EQ(urd16(rs, 4), 40 * 6 / 4);
        CHECK_EQ(urd16(rs, 6), 16 * 13 / 8);
    }
    /* GetDialogBaseUnits is the system font's, whatever the dialog uses */
    UCALL(243);
    CHECK_EQ(fx_dxax(&uf), (uint32_t)(16 << 16 | 8));
    UA(53, dlg);
    CHECK(!wm_is_window(user_wm(uu), dlg));
    CHECK(!wm_is_window(user_wm(uu), ok));                     /* the controls go with it */
    /* missing templates */
    CHECK_EQ(UA(U_CREATEDIALOG, u_ds(), FARP(ustr("nothing"), 0), 0, FARP(0, 0)), 0);
    gdown();
}

static void test_item_api(void) {
    uint16_t dlg, buf, ok, chk;
    printf("test_dlg_items\n");
    boot();
    dlg = UA(U_CREATEDIALOG, u_ds(), FARP(ustr("MAIN"), 0), 0, FARP(0, 0));    /* no dialog procedure at all */
    CHECK(dlg != 0);
    ok = item(dlg, 1);
    buf = g_seg(0x100);
    CHECK_EQ(UA(U_GETDLGITEMTEXT, dlg, 1, FARP(buf, 0), 0x40), 3);
    CHECK(!strcmp((char *)umem(buf, 0, 1), "&OK"));
    UA(U_SETDLGITEMTEXT, dlg, 1, FARP(ustr("Fine"), 0));
    CHECK_EQ(UA(U_GETDLGITEMTEXT, dlg, 1, FARP(buf, 0), 0x40), 4);
    CHECK(!strcmp((char *)umem(buf, 0, 1), "Fine"));
    CHECK_EQ(UA(U_GETDLGITEMTEXT, dlg, 999, FARP(buf, 0), 0x40), 0);
    /* integers */
    UA(U_SETDLGITEMINT, dlg, 1, 1234, 0);
    UA(U_GETDLGITEMTEXT, dlg, 1, FARP(buf, 0), 0x40);
    CHECK(!strcmp((char *)umem(buf, 0, 1), "1234"));
    UA(U_SETDLGITEMINT, dlg, 1, (uint16_t)-5, 1);
    UA(U_GETDLGITEMTEXT, dlg, 1, FARP(buf, 0), 0x40);
    CHECK(!strcmp((char *)umem(buf, 0, 1), "-5"));
    UA(U_SETDLGITEMTEXT, dlg, 1, FARP(ustr("  77"), 0));
    uwr16(buf, 0x80, 0xFFFF);
    CHECK_EQ(UA(U_GETDLGITEMINT, dlg, 1, FARP(buf, 0x80), 0), 77);
    CHECK_EQ(urd16(buf, 0x80), 1);                                  /* translated */
    UA(U_SETDLGITEMTEXT, dlg, 1, FARP(ustr("12x"), 0));
    CHECK_EQ(UA(U_GETDLGITEMINT, dlg, 1, FARP(buf, 0x80), 0), 0);
    CHECK_EQ(urd16(buf, 0x80), 0);                                  /* not a number */
    UA(U_SETDLGITEMTEXT, dlg, 1, FARP(ustr("-9"), 0));
    CHECK_EQ(UA(U_GETDLGITEMINT, dlg, 1, FARP(buf, 0x80), 1), (uint16_t)-9);
    UA(U_SETDLGITEMTEXT, dlg, 1, FARP(ustr("70000"), 0));
    CHECK_EQ(UA(U_GETDLGITEMINT, dlg, 1, FARP(buf, 0x80), 0), 0);   /* overflow: not translated */
    CHECK_EQ(urd16(buf, 0x80), 0);
    /* buttons */
    chk = item(dlg, 10);
    CHECK_EQ(UA(U_ISDLGBUTTONCHECKED, dlg, 10), 0);
    UA(U_CHECKDLGBUTTON, dlg, 10, 1);
    CHECK_EQ(UA(U_ISDLGBUTTONCHECKED, dlg, 10), 1);
    UCALL(111, chk, BM_GETCHECK, 0, LONG(0));
    CHECK_EQ(fx_ax(&uf), 1);
    UA(U_CHECKRADIOBUTTON, dlg, 20, 21, 21);                         /* CheckRadioButton(first, last, check) */
    CHECK_EQ(UA(U_ISDLGBUTTONCHECKED, dlg, 21), 1);
    CHECK_EQ(UA(U_ISDLGBUTTONCHECKED, dlg, 20), 0);
    UA(U_CHECKRADIOBUTTON, dlg, 20, 21, 20);
    CHECK_EQ(UA(U_ISDLGBUTTONCHECKED, dlg, 20), 1);
    CHECK_EQ(UA(U_ISDLGBUTTONCHECKED, dlg, 21), 0);
    /* SendDlgItemMessage */
    UCALL(U_SENDDLGITEMMESSAGE, dlg, 10, 0x401, 0, LONG(0));        /* BM_SETCHECK 0 on the check box */
    CHECK_EQ(UA(U_ISDLGBUTTONCHECKED, dlg, 10), 0);
    (void)ok;
    UA(53, dlg);
    gdown();
}

static void test_dlgproc_messages(void) {
    uint16_t dlg;
    int n0;
    printf("test_dlg_messages\n");
    boot();
    dlg = UA(U_CREATEDIALOG, u_ds(), FARP(ustr("MAIN"), 0), 0, FARP(u_cs(), U_CODE_E));
    CHECK(dlg != 0);
    /* WM_CLOSE reaches the dialog procedure, which does nothing; the default sends WM_COMMAND(IDCANCEL) */
    ulog_reset();
    UCALL(111, dlg, 0x10, 0, LONG(0));
    CHECK_EQ(ulog_w(0, ULOG_MSG), 0x10);
    CHECK_EQ(ulog_w(1, ULOG_MSG), WM_COMMAND);
    CHECK_EQ(ulog_w(1, ULOG_WP), 2);
    CHECK_EQ(ulog_lp(1) & 0xFFFF, item(dlg, 2));
    /* a disabled Cancel button: nothing is sent */
    UA(34, item(dlg, 2), 0);
    ulog_reset();
    UCALL(111, dlg, 0x10, 0, LONG(0));
    n0 = count_msgs(WM_COMMAND);
    CHECK_EQ(n0, 0);
    /* the dialog's DWL_MSGRESULT / DWL_USER words are usable: DWLP offsets 0, 4, 8 */
    UA(136, dlg, 8, LONG(0x12345678u));
    UA(135, dlg, 8);
    CHECK_EQ(fx_dxax(&uf), 0x12345678u);
    UA(135, dlg, 4);
    CHECK_EQ(fx_dxax(&uf), u_far(U_CODE_E));                         /* DWL_DLGPROC */
    /* DefDlgProc is callable */
    UCALL(U_DEFDLGPROC, dlg, 0x37, 0, LONG(0));
    UA(53, dlg);
    gdown();
}

static void test_paint(void) {
    uint16_t dlg;
    int x, y, ink = 0, gray = 0;
    printf("test_dlg_paint\n");
    boot();
    g_clear(77);
    dlg = UA(U_CREATEDIALOG, u_ds(), FARP(ustr("MAIN"), 0), 0, FARP(0, 0));
    UA(124, dlg);                                                   /* UpdateWindow */
    {
        wm_msg m;
        int guard = 0;
        while (guard++ < 200 && wm_next_message(user_wm(uu), &m, 0, 0, 0, true)) wm_dispatch(user_wm(uu), &m);
    }
    {
        int cx, cy;
        wm_client_origin(user_wm(uu), dlg, &cx, &cy);
        CHECK_EQ(px(cx + 2, cy + 2), wm_sys_color(15));              /* the dialog background */
        /* "Name:" in the static at (4,4): black ink on grey */
        for (y = cy + 4 * 13 / 8; y < cy + 4 * 13 / 8 + 13; y++)
            for (x = cx + 6; x < cx + 6 + 40; x++) {
                ink += px(x, y) == 0;
                gray += px(x, y) == wm_sys_color(15);
            }
        CHECK(ink > 8);
        CHECK(gray > 100);                                           /* the static's background is the dialog's */
        /* the default push button has its double border */
        CHECK_EQ(px(cx + 50 * 6 / 4 + 20, cy + 60 * 13 / 8 + 1), 0);
        /* the radio and check boxes are drawn */
        CHECK_EQ(px(cx + 4 * 6 / 4 + 6, cy + 16 * 13 / 8 + 6), wm_sys_color(5));
    }
    {   /* the caption bar shows the title */
        fb_rect wr;
        int white = 0;
        wm_window_rect(user_wm(uu), dlg, &wr);
        for (y = wr.top + 4; y < wr.top + 22; y++)
            for (x = wr.left + 4; x < wr.right - 4; x++) white += px(x, y) == wm_sys_color(9);
        CHECK(white > 10);
    }
    UA(53, dlg);
    gdown();
}

/* ---- modal dialogs with injected input -------------------------------------------------------------------------- */

static unsigned seen_focus[16];
static unsigned nseen;
static void record_focus(unsigned i) {
    (void)i;
    if (nseen < 16) {
        uint16_t f = user_wm(uu)->focus;
        seen_focus[nseen++] = f && wm_is_window(user_wm(uu), f) ? wm_get(user_wm(uu), f)->id : 0;
    }
}

static uint16_t run_modal(const step *s, unsigned n, void (*hook)(unsigned), uint16_t parentw) {
    uint16_t nm = ustr("MAIN");
    set_script(s, n, hook);
    nseen = 0;
    return UA(U_DIALOGBOX, u_ds(), FARP(nm, 0), parentw, FARP(u_cs(), U_CODE_F));
}

static void test_modal_keys(void) {
    printf("test_dlg_modal_keys\n");
    {   /* Enter on the default button (OK) */
        static const step s[] = {KEY(VK_RETURN)};
        boot();
        CHECK_EQ(run_modal(s, 2, record_focus, 0), 1);
        CHECK_EQ(seen_focus[0], 10);                               /* initial focus: the first tab stop */
        gdown();
    }
    {   /* Escape cancels */
        static const step s[] = {KEY(VK_ESCAPE)};
        boot();
        CHECK_EQ(run_modal(s, 2, NULL, 0), 2);
        gdown();
    }
    {   /* Tab walks the tab stops: check box, radio group (one stop), OK, Cancel, around */
        static const step s[] = {KEY(VK_TAB), KEY(VK_TAB), KEY(VK_TAB), KEY(VK_TAB), KEY(VK_RETURN)};
        boot();
        run_modal(s, 10, record_focus, 0);
        CHECK(nseen >= 8);
        CHECK_EQ(seen_focus[0], 10);                                /* the hook runs before each step: the focus after k steps */
        CHECK_EQ(seen_focus[1], 20);                                /* after one Tab: the radio group's first button */
        CHECK_EQ(seen_focus[3], 1);                                 /* OK */
        CHECK_EQ(seen_focus[5], 2);                                 /* Cancel */
        CHECK_EQ(seen_focus[7], 10);                                /* and around */
        gdown();
    }
    {   /* Shift+Tab goes backwards; Enter on the focused (non-default) Cancel clicks it */
        static const step s[] = {{WM_IN_KEY_DOWN, 0, 0, VK_SHIFT}, KEY(VK_TAB), {WM_IN_KEY_UP, 0, 0, VK_SHIFT}, KEY(VK_RETURN)};
        boot();
        CHECK_EQ(run_modal(s, 6, record_focus, 0), 2);              /* from the check box backwards: Cancel */
        gdown();
    }
    {   /* arrow keys inside the radio group; the arrow selects the radio button it lands on */
        static const step s[] = {KEY(VK_TAB), KEY(VK_DOWN), KEY(VK_TAB), KEY(VK_RETURN)};
        boot();
        run_modal(s, 8, record_focus, 0);
        CHECK_EQ(seen_focus[1], 20);
        CHECK_EQ(seen_focus[3], 21);
        CHECK_EQ(seen_focus[5], 1);
        gdown();
    }
    {   /* the space bar toggles the focused check box */
        static const step s[] = {KEY(' '), KEY(VK_RETURN)};
        boot();
        run_modal(s, 4, NULL, 0);
        gdown();
    }
    {   /* Alt+C: the mnemonic of the check box (the static label does not count) */
        static const step s[] = {{WM_IN_KEY_DOWN, 0, 0, VK_MENU}, {WM_IN_KEY_DOWN, 0, 0, 'O'}, {WM_IN_KEY_UP, 0, 0, 'O'},
                                 {WM_IN_KEY_UP, 0, 0, VK_MENU}};
        boot();
        CHECK_EQ(run_modal(s, 4, NULL, 0), 1);                      /* Alt+O clicks &OK */
        gdown();
    }
}

static unsigned check_after;
static void probe_check(unsigned i) {
    uint16_t dlg = host_dialog();
    if (dlg && i == 2) check_after = wm_send(user_wm(uu), host_item(dlg, 10), BM_GETCHECK, 0, 0);
}

static void test_modal_toggle(void) {
    static const step s[] = {KEY(' '), KEY(VK_RETURN)};
    printf("test_dlg_modal_toggle\n");
    boot();
    check_after = 99;
    run_modal(s, 4, probe_check, 0);
    CHECK_EQ(check_after, 1);                                        /* space toggled the check box */
    gdown();
}

static void click_item(unsigned i) {
    uint16_t dlg = host_dialog(), b;
    fb_rect r;
    wm_input in;
    if (i != 0) return;
    b = dlg ? host_item(dlg, 2) : 0;
    if (!b) return;
    wm_window_rect(user_wm(uu), b, &r);
    memset(&in, 0, sizeof in);
    in.kind = WM_IN_MOVE;
    in.x = (r.left + r.right) / 2;
    in.y = (r.top + r.bottom) / 2;
    wm_input_event(user_wm(uu), &in);
    in.kind = WM_IN_BUTTON_DOWN;
    wm_input_event(user_wm(uu), &in);
    in.kind = WM_IN_BUTTON_UP;
    wm_input_event(user_wm(uu), &in);
}

static void test_modal_mouse(void) {
    static const step none[] = {{WM_IN_MOVE, 0, 0, 0}};
    uint16_t owner;
    printf("test_dlg_modal_mouse\n");
    boot();
    u_register("OWN", 0, 0, 0, 0, u_ds(), 6);
    owner = u_create("OWN", "owner", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 480, 0, 0, u_ds());
    set_script(none, 1, click_item);
    CHECK_EQ(UA(U_DIALOGBOX, u_ds(), FARP(ustr("MAIN"), 0), owner, FARP(u_cs(), U_CODE_F)), 2);   /* clicked Cancel */
    CHECK(wm_is_enabled(user_wm(uu), owner));                        /* the owner is enabled again ... */
    CHECK_EQ(user_wm(uu)->active, owner);                            /* ... and active */
    gdown();
}

static bool owner_was_disabled;
static void probe_owner(unsigned i) {
    wm_window *d = wm_get(user_wm(uu), user_wm(uu)->desktop);
    uint16_t k;
    if (i != 0) return;
    for (k = d->first_child; k; k = wm_get(user_wm(uu), k)->next)
        if (wm_get(user_wm(uu), k)->cls && !strcmp(wm_get(user_wm(uu), k)->cls->name, "OWN"))
            owner_was_disabled = !wm_is_enabled(user_wm(uu), k);
}

static void test_modal_owner(void) {
    static const step s[] = {KEY(VK_RETURN)};
    uint16_t owner;
    printf("test_dlg_modal_owner\n");
    boot();
    u_register("OWN", 0, 0, 0, 0, u_ds(), 6);
    owner = u_create("OWN", "owner", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 480, 0, 0, u_ds());
    owner_was_disabled = false;
    run_modal(s, 2, probe_owner, owner);
    CHECK(owner_was_disabled);
    gdown();
}

static void test_dialog_variants(void) {
    static const step s[] = {KEY(VK_RETURN)};
    rb_t b;
    uint16_t g;
    printf("test_dlg_variants\n");
    boot();
    set_script(s, 2, NULL);
    CHECK_EQ(UA(U_DIALOGBOXPARAM, u_ds(), FARP(ustr("MAIN"), 0), 0, FARP(u_cs(), U_CODE_F), LONG(0x1234)), 1);
    /* DialogBoxIndirect: the template in a global block */
    main_template(&b);
    g = g_seg(0x400);
    memcpy(umem(g, 0, b.n), b.b, b.n);
    set_script(s, 2, NULL);
    CHECK_EQ(UA(U_DIALOGBOXINDIRECT, u_ds(), g, 0, FARP(u_cs(), U_CODE_F)), 1);
    {
        uint16_t d = UA(U_CREATEDIALOGINDIRECT, u_ds(), FARP(g, 0), 0, FARP(0, 0));
        CHECK(d != 0 && item(d, 1) != 0);
        UA(53, d);
    }
    /* a template that does not exist: DialogBox returns -1 */
    set_script(s, 2, NULL);
    CHECK_EQ(UA(U_DIALOGBOX, u_ds(), FARP(ustr("NOPE"), 0), 0, FARP(u_cs(), U_CODE_F)), 0xFFFF);
    gdown();
}

static void test_quit_in_modal_loop(void) {
    printf("test_dlg_quit\n");
    boot();
    upump_stop = false;
    script = NULL;
    nscript = 0;
    upump_script = NULL;
    wm_post_quit(user_wm(uu), 7);
    UA(U_DIALOGBOX, u_ds(), FARP(ustr("MAIN"), 0), 0, FARP(u_cs(), U_CODE_F));
    CHECK(user_wm(uu)->quit_pending);                                /* WM_QUIT is put back for the program's loop */
    gdown();
}

/* ---- MessageBox ------------------------------------------------------------------------------------------------ */

static int mb_buttons;
static char mb_caption[64], mb_text[256];
static int mb_icon_ink;
static void probe_mb(unsigned i) {
    wm_window *d = wm_get(user_wm(uu), user_wm(uu)->desktop);
    uint16_t k, c;
    if (i != 0) return;
    mb_buttons = 0;
    for (k = d->first_child; k; k = wm_get(user_wm(uu), k)->next) {
        wm_window *x = wm_get(user_wm(uu), k);
        if (!x->cls || strcmp(x->cls->name, "#32770")) continue;
        strncpy(mb_caption, wm_get_text(user_wm(uu), k), sizeof mb_caption - 1);
        for (c = x->first_child; c; c = wm_get(user_wm(uu), c)->next) {
            wm_window *cx = wm_get(user_wm(uu), c);
            if (cx->cls && !strcmp(cx->cls->name, "BUTTON")) mb_buttons = mb_buttons * 10 + (cx->id % 10);
            if (cx->cls && !strcmp(cx->cls->name, "STATIC") && ((cx->style & 0xF) == 0)) strncpy(mb_text, wm_get_text(user_wm(uu), c), sizeof mb_text - 1);
        }
        {
            fb_rect wr;
            int x0, y0, n = 0, xx, yy;
            wm_window_rect(user_wm(uu), k, &wr);
            x0 = wr.left;
            y0 = wr.top;
            for (yy = y0 + 26; yy < y0 + 70; yy++)
                for (xx = x0 + 20; xx < x0 + 56; xx++) n += px(xx, yy) == RGB(255, 0, 0);
            mb_icon_ink = n;
        }
    }
}

static uint16_t message_box(uint16_t parentw, const char *text, const char *caption, unsigned type) {
    return UA(U_MESSAGEBOX, parentw, FARP(ustr(text), 0), FARP(caption ? ustr(caption) : 0, 0), (uint16_t)type);
}

static void test_message_box(void) {
    printf("test_dlg_messagebox\n");
    {
        static const step s[] = {KEY(VK_RETURN)};
        boot();
        set_script(s, 2, probe_mb);
        mb_caption[0] = mb_text[0] = 0;
        CHECK_EQ(message_box(0, "Disk full", "Creative Writer", 0x10), 1);          /* MB_OK | MB_ICONHAND */
        CHECK(!strcmp(mb_caption, "Creative Writer"));
        CHECK(!strcmp(mb_text, "Disk full"));
        CHECK_EQ(mb_buttons, 1);                                                      /* one button: IDOK */
        CHECK(mb_icon_ink > 20);                                                      /* the stop sign is red */
        gdown();
    }
    {   /* OKCANCEL: Escape cancels, Enter takes the default (OK) */
        static const step esc[] = {KEY(VK_ESCAPE)};
        boot();
        set_script(esc, 2, probe_mb);
        CHECK_EQ(message_box(0, "Sure?", NULL, 0x01 | 0x20), 2);
        CHECK(!strcmp(mb_caption, "Error"));                                          /* no caption: "Error" */
        CHECK_EQ(mb_buttons, 12);
        gdown();
    }
    {
        static const step ent[] = {KEY(VK_RETURN)};
        boot();
        set_script(ent, 2, NULL);
        CHECK_EQ(message_box(0, "Sure?", "t", 0x01), 1);
        gdown();
    }
    {   /* YESNO: the default is Yes, Escape does nothing (there is no Cancel), the second default is No */
        static const step s1[] = {KEY(VK_ESCAPE), KEY(VK_RETURN)};
        static const step s2[] = {KEY(VK_RETURN)};
        boot();
        set_script(s1, 4, probe_mb);
        CHECK_EQ(message_box(0, "q", "t", 0x04), 6);                                   /* IDYES */
        CHECK_EQ(mb_buttons, 67);
        set_script(s2, 2, NULL);
        CHECK_EQ(message_box(0, "q", "t", 0x04 | 0x100), 7);                           /* MB_DEFBUTTON2: IDNO */
        gdown();
    }
    {   /* YESNOCANCEL, ABORTRETRYIGNORE, RETRYCANCEL */
        static const step ent[] = {KEY(VK_RETURN)};
        static const step tab[] = {KEY(VK_TAB), KEY(VK_RETURN)};
        static const step tab2[] = {KEY(VK_TAB), KEY(VK_TAB), KEY(VK_RETURN)};
        boot();
        set_script(ent, 2, probe_mb);
        CHECK_EQ(message_box(0, "q", "t", 0x03), 6);
        CHECK_EQ(mb_buttons, 672);
        set_script(tab2, 6, NULL);
        CHECK_EQ(message_box(0, "q", "t", 0x03), 2);                                   /* Yes, No, Cancel: two Tabs */
        set_script(ent, 2, probe_mb);
        CHECK_EQ(message_box(0, "q", "t", 0x02), 3);                                   /* IDABORT */
        CHECK_EQ(mb_buttons, 345);
        set_script(tab, 4, NULL);
        CHECK_EQ(message_box(0, "q", "t", 0x05), 2);                                   /* RETRYCANCEL: Tab to Cancel */
        gdown();
    }
    {   /* a long text is wrapped, the dialog stays on the screen */
        static const step ent[] = {KEY(VK_RETURN)};
        boot();
        set_script(ent, 2, NULL);
        message_box(0, "A very long message that goes on and on and on and on and on and on and on and on and on "
                       "and on and on and on and on and on and on and on and on and on and on", "t", 0);
        gdown();
    }
    {   /* the dialog goes away: the desktop is repainted where it was */
        static const step ent[] = {KEY(VK_RETURN)};
        boot();
        set_script(ent, 2, NULL);
        message_box(0, "x", "t", 0);
        CHECK_EQ(wm_window_from_point(user_wm(uu), 320, 240), user_wm(uu)->desktop);
        gdown();
    }
    {   /* without any input the loop ends when the host pump says stop */
        boot();
        script = NULL;
        nscript = 0;
        upump_script = NULL;
        upump_stop = true;
        CHECK_EQ(UCALL(U_MESSAGEBOX, 0, FARP(ustr("x"), 0), FARP(ustr("c"), 0), 0), LOADER_STOP_REQUESTED);
        gdown();
    }
}

int main(void) {
    test_create_dialog();
    test_item_api();
    test_dlgproc_messages();
    test_paint();
    test_modal_keys();
    test_modal_toggle();
    test_modal_mouse();
    test_modal_owner();
    test_dialog_variants();
    test_quit_in_modal_loop();
    test_message_box();
    printf("test_ui_dlg: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
