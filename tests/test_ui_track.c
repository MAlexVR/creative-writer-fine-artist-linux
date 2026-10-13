/* Menu tracking: the bar and its popups driven by injected mouse and keyboard events, the system menu,
 * TrackPopupMenu, the order of WM_INITMENU / WM_INITMENUPOPUP / WM_MENUSELECT / WM_COMMAND, and the
 * rendered popups with the background restored afterwards. */
#include "gfixture.h"

enum { U_CREATEMENU = 151, U_APPENDMENU = 411, U_CREATEPOPUPMENU = 415, U_TRACKPOPUPMENU = 416, U_GETSYSTEMMENU = 156 };
#define WM_INITMENU_ 0x116
#define WM_INITMENUPOPUP_ 0x117
#define WM_MENUSELECT_ 0x11F
#define MF_GRAYED 1
#define MF_POPUP 0x10
#define MF_HILITE 0x80
#define VK_RETURN 0x0D
#define VK_ESCAPE 0x1B
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_F10 0x79

#define WX 50
#define WY 60
#define F 5
#define BAR_BOTTOM (WY + F + 19 + 19)   /* where the popups hang */
#define ROW 18

static uint16_t add_item(uint16_t m, unsigned flags, unsigned id, const char *text) {
    uint16_t s = text ? ustr(text) : 0;
    return UA(U_APPENDMENU, m, (uint16_t)flags, (uint16_t)id, FARP(s, 0));
}

static uint32_t px(int x, int y) { return fb_colorref_at(user_fb(uu), x, y); }

/* ---- scripted input ---------------------------------------------------------------------------- */

typedef struct { wm_in_kind kind; int x, y; unsigned vk; } step;
static const step *script;
static unsigned nscript, script_pos;
static void (*script_hook)(unsigned i);   /* runs before step i */
static unsigned script_extra_idle;

static void run_script(bool wait) {
    wm_input in;
    (void)wait;
    if (script_hook) script_hook(script_pos);
    if (script_pos >= nscript) {
        if (++script_extra_idle > 50) upump_stop = true;      /* a tracker that never ends: stop the run */
        return;
    }
    memset(&in, 0, sizeof in);
    in.kind = script[script_pos].kind;
    in.x = script[script_pos].x;
    in.y = script[script_pos].y;
    in.vk = script[script_pos].vk;
    script_pos++;
    wm_input_event(user_wm(uu), &in);
}

static void set_script(const step *s, unsigned n) {
    script = s;
    nscript = n;
    script_pos = 0;
    script_hook = NULL;
    script_extra_idle = 0;
    upump_script = run_script;
}

/* ---- the window ------------------------------------------------------------------------------- */

static uint16_t bar, filem, editm, hw;

static void make_window(void) {
    uint32_t wp = u_far(U_CODE_A);
    uint16_t cs = useg(0x100), ns = ustr("TRK");
    uwr16(cs, 0, 0); uwr32(cs, 2, wp); uwr16(cs, 6, 0); uwr16(cs, 8, 0); uwr16(cs, 10, u_ds());
    uwr16(cs, 12, 0); uwr16(cs, 14, 0); uwr16(cs, 16, 6); uwr32(cs, 18, 0); uwr32(cs, 22, (uint32_t)ns << 16);
    UCALL(57, FARP(cs, 0));
    bar = UA(U_CREATEMENU);
    filem = UA(U_CREATEPOPUPMENU);
    editm = UA(U_CREATEPOPUPMENU);
    add_item(filem, 0, 100, "&New\tCtrl+N");
    add_item(filem, MF_GRAYED, 101, "&Open");
    add_item(filem, 0x800, 0, NULL);
    add_item(filem, 0, 102, "E&xit");
    add_item(editm, 0, 200, "Cu&t");
    add_item(bar, MF_POPUP, filem, "&File");
    add_item(bar, MF_POPUP, editm, "&Edit");
    hw = u_create("TRK", "Track", WS_OVERLAPPEDWINDOW | WS_VISIBLE, WX, WY, 400, 300, 0, bar, u_ds());
    UA(124, hw);
    ulog_reset();
}

/* the messages the window procedure saw, newest last, as "msg:wparam:lparam-low" */
static const char *seen(void) {
    static char buf[512];
    unsigned i, n = ulog_n();
    buf[0] = 0;
    for (i = 0; i < n; i++) {
        char t[48];
        uint16_t m = ulog_w(i, ULOG_MSG);
        if (m == WM_INITMENU_ || m == WM_INITMENUPOPUP_ || m == WM_MENUSELECT_ || m == WM_COMMAND || m == WM_SYSCOMMAND) {
            snprintf(t, sizeof t, "%x:%x:%x/%x;", m, ulog_w(i, ULOG_WP), ulog_w(i, ULOG_LPLO), ulog_w(i, ULOG_LPHI));
            strcat(buf, t);
        }
    }
    return buf;
}

static bool next_posted(wm_msg *m) { return wm_next_message(user_wm(uu), m, 0, WM_COMMAND, WM_SYSCOMMAND, true); }

static void test_keyboard_bar(void) {
    static const step keys[] = {
        {WM_IN_KEY_DOWN, 0, 0, VK_DOWN},      /* opens File, selects New */
        {WM_IN_KEY_DOWN, 0, 0, VK_DOWN},      /* the grayed Open can be highlighted */
        {WM_IN_KEY_DOWN, 0, 0, VK_DOWN},      /* skips the separator: Exit */
        {WM_IN_KEY_DOWN, 0, 0, VK_RETURN},
    };
    wm_msg m;
    char want[256];
    printf("test_track_keyboard\n");
    gboot();
    make_window();
    set_script(keys, 4);
    UCALL(107, hw, WM_SYSCOMMAND, 0xF100, LONG(0));            /* SC_KEYMENU: the bar becomes active */
    CHECK_EQ(script_pos, 4);
    snprintf(want, sizeof want, "%x:%x:%x/%x;", WM_INITMENU_, bar, 0, 0);
    CHECK(strstr(seen(), want) == seen());                     /* WM_INITMENU comes first */
    snprintf(want, sizeof want, "%x:%x:%x/%x;", WM_INITMENUPOPUP_, filem, 0, 0);
    CHECK(strstr(seen(), want));                               /* ... then the File popup is about to open */
    snprintf(want, sizeof want, "%x:%x:%x/%x;", WM_MENUSELECT_, 100, 0, filem);
    CHECK(strstr(seen(), want));                               /* New (flags 0) in the File popup */
    snprintf(want, sizeof want, "%x:%x:%x/%x;", WM_MENUSELECT_, 101, MF_GRAYED, filem);
    CHECK(strstr(seen(), want));
    snprintf(want, sizeof want, "%x:%x:%x/%x;", WM_MENUSELECT_, 102, 0, filem);
    CHECK(strstr(seen(), want));
    CHECK(strstr(seen(), "11f:0:ffff/0;"));                    /* the menu closed */
    CHECK(next_posted(&m) && m.message == WM_COMMAND && m.wparam == 102);
    CHECK(!next_posted(&m));
    CHECK_EQ(user_wm(uu)->capture, 0);                         /* capture is given back */
    gdown();
}

static void test_keyboard_navigation(void) {
    static const step keys[] = {
        {WM_IN_KEY_DOWN, 0, 0, VK_DOWN},      /* File, New */
        {WM_IN_KEY_DOWN, 0, 0, VK_RIGHT},     /* the bar's next item: Edit opens, Cut */
        {WM_IN_KEY_DOWN, 0, 0, VK_LEFT},      /* back to File */
        {WM_IN_KEY_DOWN, 0, 0, VK_ESCAPE},    /* closes the popup, stays in the bar */
        {WM_IN_KEY_DOWN, 0, 0, VK_ESCAPE},    /* leaves the menu */
    };
    wm_msg m;
    printf("test_track_navigation\n");
    gboot();
    make_window();
    set_script(keys, 5);
    UCALL(107, hw, WM_SYSCOMMAND, 0xF100, LONG(0));
    CHECK_EQ(script_pos, 5);
    {
        char want[64];
        snprintf(want, sizeof want, "%x:%x:", WM_INITMENUPOPUP_, editm);
        CHECK(strstr(seen(), want));
    }
    CHECK(strstr(seen(), "11f:c8:0/"));                        /* Cut (id 200) was highlighted */
    CHECK(!next_posted(&m));                                    /* nothing chosen */
    CHECK(strstr(seen(), "11f:0:ffff/0;"));
    gdown();
}

static void test_mnemonics(void) {
    static const step keys[] = {{WM_IN_KEY_DOWN, 0, 0, 'T'}};     /* the popup: Cu&t */
    wm_msg m;
    printf("test_track_mnemonics\n");
    gboot();
    make_window();
    set_script(keys, 1);
    UCALL(107, hw, WM_SYSCOMMAND, 0xF100, LONG('e'));          /* Alt+E opens Edit; T chooses Cut */
    CHECK(next_posted(&m) && m.message == WM_COMMAND && m.wparam == 200);
    gdown();
}

static const char *saved_msg;

static void test_mouse(void) {
    /* File item of the bar starts at bar.left = F; its popup hangs at BAR_BOTTOM */
    static step mouse[8];
    wm_msg m;
    unsigned i;
    int px_x = WX + F, py_y = BAR_BOTTOM;
    printf("test_track_mouse\n");
    gboot();
    make_window();
    g_clear(77);
    /* move over the New row, release on it */
    mouse[0] = (step){WM_IN_MOVE, px_x + 20, py_y + 2 + 9, 0};
    mouse[1] = (step){WM_IN_BUTTON_UP, px_x + 20, py_y + 2 + 9, 0};
    set_script(mouse, 2);
    UCALL(107, hw, WM_SYSCOMMAND, 0xF095, LONG((uint32_t)(WY + F + 19 + 5) << 16 | (uint16_t)(WX + F + 10)));   /* WM_NCLBUTTONDOWN, HTMENU, on File */
    CHECK_EQ(script_pos, 2);
    CHECK(next_posted(&m) && m.message == WM_COMMAND && m.wparam == 100);
    /* the popup is gone again: the pixels under it are back */
    CHECK_EQ(g_px(px_x + 20, py_y + 2 + 9), 77);
    /* pressing outside cancels */
    make_window();
    ulog_reset();
    mouse[0] = (step){WM_IN_BUTTON_DOWN, 700 % 640, 400, 0};
    set_script(mouse, 1);
    UCALL(107, hw, WM_SYSCOMMAND, 0xF095, LONG((uint32_t)(WY + F + 19 + 5) << 16 | (uint16_t)(WX + F + 10)));
    CHECK(!next_posted(&m));
    CHECK(strstr(seen(), "11f:0:ffff/0;"));
    /* the grayed item does not choose */
    mouse[0] = (step){WM_IN_MOVE, px_x + 20, py_y + 2 + ROW + 9, 0};
    mouse[1] = (step){WM_IN_BUTTON_UP, px_x + 20, py_y + 2 + ROW + 9, 0};
    mouse[2] = (step){WM_IN_BUTTON_DOWN, 600, 400, 0};
    set_script(mouse, 3);
    UCALL(107, hw, WM_SYSCOMMAND, 0xF095, LONG((uint32_t)(WY + F + 19 + 5) << 16 | (uint16_t)(WX + F + 10)));
    CHECK(!next_posted(&m));
    (void)i;
    (void)saved_msg;
    gdown();
}

static bool popup_ok;
static int popup_text;
#define SEP 9
static void check_popup_pixels(unsigned i) {
    int x, y;
    if (i != 1) return;                                          /* idle after the first DOWN was processed */
    popup_ok = true;
    if (px(WX + F, BAR_BOTTOM) != wm_sys_color(6)) popup_ok = false;                       /* black border */
    if (px(WX + F + 3, BAR_BOTTOM + 2 + ROW * 2 + SEP + 3) != wm_sys_color(4)) popup_ok = false;   /* white row of Exit */
    if (px(WX + F + 3, BAR_BOTTOM + 2 + 3) != wm_sys_color(13)) popup_ok = false;          /* New is highlighted */
    if (px(WX + F + 3, BAR_BOTTOM + 2 + ROW + 3) != wm_sys_color(4)) popup_ok = false;
    for (y = BAR_BOTTOM + 2; y < BAR_BOTTOM + 2 + ROW; y++)
        for (x = WX + F + 20; x < WX + F + 120; x++) popup_text += px(x, y) == wm_sys_color(14);   /* white text on navy */
    for (y = BAR_BOTTOM + 2 + ROW; y < BAR_BOTTOM + 2 + 2 * ROW; y++)
        for (x = WX + F + 20; x < WX + F + 120; x++) popup_text += px(x, y) == wm_sys_color(17) ? 1000 : 0;   /* gray text of Open */
    /* the separator is a line */
    if (px(WX + F + 30, BAR_BOTTOM + 2 + ROW * 2 + SEP / 2) == wm_sys_color(4)) popup_ok = false;
}

static void test_popup_rendering(void) {
    static const step keys[] = {
        {WM_IN_KEY_DOWN, 0, 0, VK_DOWN},
        {WM_IN_KEY_DOWN, 0, 0, VK_ESCAPE},
        {WM_IN_KEY_DOWN, 0, 0, VK_ESCAPE},
    };
    printf("test_track_rendering\n");
    gboot();
    make_window();
    g_clear(77);
    popup_ok = false;
    popup_text = 0;
    set_script(keys, 3);
    script_hook = check_popup_pixels;
    UCALL(107, hw, WM_SYSCOMMAND, 0xF100, LONG(0));
    CHECK(popup_ok);
    CHECK(popup_text % 1000 > 10);                               /* "New" text pixels */
    CHECK(popup_text / 1000 > 10);                               /* "Open" in the gray text colour */
    CHECK_EQ(g_px(WX + F + 5, BAR_BOTTOM + 20), 77);             /* restored */
    CHECK_EQ(g_px(WX + F + 5, BAR_BOTTOM), 0);                   /* the line under the bar is back (the popup covered it) */
    CHECK_EQ(g_px(WX + F + 5, BAR_BOTTOM + 1), 77);
    gdown();
}

static void test_trackpopupmenu(void) {
    static const step keys[] = {{WM_IN_KEY_DOWN, 0, 0, VK_DOWN}, {WM_IN_KEY_DOWN, 0, 0, VK_RETURN}};
    wm_msg m;
    uint16_t pop;
    printf("test_track_popupmenu\n");
    gboot();
    make_window();
    pop = UA(U_CREATEPOPUPMENU);
    add_item(pop, 0, 300, "One");
    add_item(pop, 0, 301, "Two");
    set_script(keys, 2);
    /* TrackPopupMenu(hMenu, flags, x, y, reserved, hwnd, lprc) */
    CHECK_EQ(UA(U_TRACKPOPUPMENU, pop, 0, 200, 200, 0, hw, 0, 0), 1);
    CHECK(next_posted(&m) && m.message == WM_COMMAND && m.wparam == 300);
    CHECK(strstr(seen(), "117:") != NULL);                      /* WM_INITMENUPOPUP */
    CHECK_EQ(script_pos, 2);
    /* a click outside returns without a command */
    {
        static const step out[] = {{WM_IN_BUTTON_DOWN, 600, 450, 0}};
        set_script(out, 1);
        CHECK_EQ(UA(U_TRACKPOPUPMENU, pop, 0, 200, 200, 0, hw, 0, 0), 1);
        CHECK(!next_posted(&m));
    }
    gdown();
}

static void test_system_menu(void) {
    static const step keys[] = {
        {WM_IN_KEY_DOWN, 0, 0, VK_DOWN},      /* Restore is grayed but can be highlighted */
        {WM_IN_KEY_DOWN, 0, 0, VK_DOWN},      /* Move */
        {WM_IN_KEY_DOWN, 0, 0, VK_DOWN},      /* Size */
        {WM_IN_KEY_DOWN, 0, 0, VK_DOWN},      /* Minimize */
        {WM_IN_KEY_DOWN, 0, 0, VK_RETURN},
    };
    wm_msg m;
    printf("test_track_sysmenu\n");
    gboot();
    make_window();
    set_script(keys, 5);
    UCALL(107, hw, WM_SYSCOMMAND, 0xF100, LONG(' '));          /* Alt+Space */
    CHECK(next_posted(&m) && m.message == WM_SYSCOMMAND && (m.wparam & 0xFFF0) == 0xF020);   /* SC_MINIMIZE */
    gdown();
}

static void test_alt_and_f10(void) {
    printf("test_track_f10\n");
    gboot();
    make_window();
    /* the recording window procedure sees WM_SYSCOMMAND but does not forward it, so no menu opens */
    UCALL(107, hw, 0x104, VK_F10, LONG(0));                       /* F10 down: DefWindowProc sends SC_KEYMENU */
    CHECK(strstr(seen(), "112:f100:0/0;"));
    ulog_reset();
    UCALL(107, hw, 0x104, 0x12, LONG(0x20000000));                /* Alt alone: down, up */
    UCALL(107, hw, 0x105, 0x12, LONG(0xE0000000u));
    CHECK(strstr(seen(), "112:f100:0/0;"));
    ulog_reset();
    UCALL(107, hw, 0x104, 0x12, LONG(0x20000000));                /* Alt with another key: no menu */
    UCALL(107, hw, 0x104, 'X', LONG(0x20000000));
    UCALL(107, hw, 0x105, 0x12, LONG(0xE0000000u));
    CHECK(!strstr(seen(), "112:f100"));
    ulog_reset();
    UCALL(107, hw, 0x106, ' ', LONG(0x20000000));                 /* WM_SYSCHAR Alt+Space: the system menu */
    CHECK(strstr(seen(), "112:f100:20/0;"));
    ulog_reset();
    UCALL(107, hw, 0x106, 'f', LONG(0x20000000));                 /* WM_SYSCHAR Alt+F: the File item */
    CHECK(strstr(seen(), "112:f100:66/0;"));
    gdown();
}

int main(void) {
    test_keyboard_bar();
    test_keyboard_navigation();
    test_mnemonics();
    test_mouse();
    test_popup_rendering();
    test_trackpopupmenu();
    test_system_menu();
    test_alt_and_f10();
    printf("test_ui_track: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
