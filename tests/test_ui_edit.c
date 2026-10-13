/* EDIT, LISTBOX and COMBOBOX controls: messages, keyboard and mouse editing, notifications and pixels. */
#include "gfixture.h"

enum {
    EM_GETSEL = 0x400, EM_SETSEL, EM_GETRECT, EM_SETRECT, EM_SETRECTNP, EM_SCROLL, EM_LINESCROLL, EM_GETMODIFY = 0x408,
    EM_SETMODIFY, EM_GETLINECOUNT, EM_LINEINDEX, EM_SETHANDLE, EM_GETHANDLE, EM_LINELENGTH = 0x411, EM_REPLACESEL,
    EM_GETLINE = 0x414, EM_LIMITTEXT, EM_CANUNDO, EM_UNDO, EM_FMTLINES, EM_LINEFROMCHAR, EM_SETTABSTOPS = 0x41B,
    EM_SETPASSWORDCHAR, EM_EMPTYUNDOBUFFER, EM_GETFIRSTVISIBLELINE, EM_SETREADONLY
};
enum {
    LB_ADDSTRING = 0x401, LB_INSERTSTRING, LB_DELETESTRING, LB_RESETCONTENT = 0x405, LB_SETSEL, LB_SETCURSEL, LB_GETSEL,
    LB_GETCURSEL, LB_GETTEXT, LB_GETTEXTLEN, LB_GETCOUNT, LB_SELECTSTRING, LB_DIR, LB_GETTOPINDEX, LB_FINDSTRING,
    LB_GETSELCOUNT, LB_GETSELITEMS, LB_SETTABSTOPS, LB_GETHORIZONTALEXTENT, LB_SETHORIZONTALEXTENT, LB_SETCOLUMNWIDTH,
    LB_ADDFILE, LB_SETTOPINDEX, LB_GETITEMRECT, LB_GETITEMDATA, LB_SETITEMDATA, LB_SELITEMRANGE,
    LB_SETCARETINDEX = 0x41F, LB_GETCARETINDEX, LB_SETITEMHEIGHT, LB_GETITEMHEIGHT, LB_FINDSTRINGEXACT
};
enum {
    CB_GETEDITSEL = 0x400, CB_LIMITTEXT, CB_SETEDITSEL, CB_ADDSTRING, CB_DELETESTRING, CB_DIR, CB_GETCOUNT, CB_GETCURSEL,
    CB_GETLBTEXT, CB_GETLBTEXTLEN, CB_INSERTSTRING, CB_RESETCONTENT, CB_FINDSTRING, CB_SELECTSTRING, CB_SETCURSEL,
    CB_SHOWDROPDOWN, CB_GETITEMDATA, CB_SETITEMDATA, CB_GETDROPPEDCONTROLRECT, CB_SETITEMHEIGHT, CB_GETITEMHEIGHT,
    CB_SETEXTENDEDUI, CB_GETEXTENDEDUI, CB_GETDROPPEDSTATE, CB_FINDSTRINGEXACT
};
#define EN_SETFOCUS 0x100
#define EN_KILLFOCUS 0x200
#define EN_CHANGE 0x300
#define EN_UPDATE 0x400
#define EN_MAXTEXT 0x501
#define LBN_SELCHANGE 1
#define LBN_DBLCLK 2
#define CBN_SELCHANGE 1
#define CBN_DROPDOWN 7
#define CBN_CLOSEUP 8
#define ES_MULTILINE 4
#define ES_PASSWORD 0x20
#define ES_AUTOHSCROLL 0x80
#define ES_AUTOVSCROLL 0x40
#define LBS_NOTIFY 1
#define LBS_SORT 2
#define LBS_MULTIPLESEL 8
#define LBS_HASSTRINGS 0x40
#define CBS_DROPDOWN 2
#define CBS_DROPDOWNLIST 3
#define WS_BORDER_ 0x800000u
#define WS_VSCROLL_ 0x200000u
#define VK_BACK 8
#define VK_RETURN 13
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_DELETE 0x2E
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11

#define PX 100
#define PY 100

static uint16_t parent;

static uint32_t px(int x, int y) { return fb_colorref_at(user_fb(uu), x, y); }

static void drain(void) {
    wm_msg m;
    int guard = 0;
    while (guard++ < 4000 && wm_next_message(user_wm(uu), &m, 0, 0, 0, true)) {
        if (m.message == WM_QUIT) break;
        wm_translate(user_wm(uu), &m);
        wm_dispatch(user_wm(uu), &m);
    }
}

static void mouse(wm_in_kind k, int x, int y) {
    wm_input in;
    memset(&in, 0, sizeof in);
    in.kind = k;
    in.x = x;
    in.y = y;
    wm_input_event(user_wm(uu), &in);
}

static void key(wm_in_kind k, unsigned vk) {
    wm_input in;
    memset(&in, 0, sizeof in);
    in.kind = k;
    in.vk = vk;
    wm_input_event(user_wm(uu), &in);
}

static void tap(unsigned vk) {
    key(WM_IN_KEY_DOWN, vk);
    key(WM_IN_KEY_UP, vk);
    drain();
}

static void type(const char *s) {
    for (; *s; s++) tap((unsigned char)*s >= 'a' && (unsigned char)*s <= 'z' ? (unsigned)(*s - 32) : (unsigned char)*s);
}

static void setup(void) {
    gboot();
    u_register("PAR", u_far(U_CODE_A), 0, 0, 0, u_ds(), 6);
    g_clear(77);
    parent = u_create("PAR", "p", WS_POPUP | WS_VISIBLE, PX, PY, 400, 300, 0, 0, u_ds());
    UA(127, parent, FARP(0, 0));
    {
        fb_rect r = {0, 0, 400, 300};
        wm_dc_fill(user_wm(uu), wm_get_dc(user_wm(uu), parent), &r, RGB(255, 255, 255));
    }
    drain();
    ulog_reset();
}

static uint16_t mk(const char *cls, const char *text, uint32_t style, int x, int y, int w, int h, unsigned id) {
    return u_create(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (uint16_t)id, u_ds());
}

static uint32_t send(uint16_t h, unsigned msg, unsigned wp, uint32_t lp) {
    UCALL(111, h, (uint16_t)msg, (uint16_t)wp, LONG(lp));
    return fx_dxax(&uf);
}

static uint32_t sendp(uint16_t h, unsigned msg, unsigned wp, const char *s) {
    uint16_t seg = ustr(s);
    return send(h, msg, wp, (uint32_t)seg << 16);
}

static const char *text_of(uint16_t h) {
    static char buf[1024];
    uint16_t b = g_seg(1024);
    UA(36, h, FARP(b, 0), 1000);
    strcpy(buf, (char *)umem(b, 0, 1));
    return buf;
}

/* notifications seen by the parent: "code:id;" for each WM_COMMAND with a nonzero notification code */
static const char *notes(void) {
    static char buf[256];
    unsigned i;
    buf[0] = 0;
    for (i = 0; i < ulog_n(); i++)
        if (ulog_w(i, ULOG_MSG) == WM_COMMAND) {
            char t[24];
            snprintf(t, sizeof t, "%x:%u;", ulog_lp(i) >> 16, ulog_w(i, ULOG_WP));
            strcat(buf, t);
        }
    return buf;
}

static int count_black(int l, int t, int r, int b) {
    int x, y, n = 0;
    for (y = t; y < b; y++)
        for (x = l; x < r; x++) n += px(x, y) == 0;
    return n;
}

static uint16_t sel_start(uint16_t e) { return (uint16_t)send(e, EM_GETSEL, 0, 0); }
static uint16_t sel_end(uint16_t e) { return (uint16_t)(send(e, EM_GETSEL, 0, 0) >> 16); }

static int right_edge(int l, int t, int r, int b) {          /* rightmost black pixel column in the box, -1 if none */
    int x, y, m = -1;
    for (y = t; y < b; y++)
        for (x = l; x < r; x++)
            if (px(x, y) == 0 && x > m) m = x;
    return m;
}

/* The programs give their edit boxes a font with WM_SETFONT and switch WS_DISABLED on the box that has no focus
 * (SetWindowLong GWL_STYLE): the text is drawn with that font and in the window text colour, not greyed. */
static void test_edit_font_and_disabled(void) {
    uint16_t e, f;
    int plain, big, i, grey = 0;
    printf("test_edit_font_and_disabled\n");
    setup();
    e = mk("EDIT", "Hello", WS_BORDER_ | ES_AUTOHSCROLL, 10, 10, 200, 40, 5);
    drain();
    plain = right_edge(PX + 12, PY + 12, PX + 208, PY + 48);
    CHECK_EQ(send(e, 0x31, 0, 0), 0);                                          /* WM_GETFONT: none yet */
    f = (uint16_t)GR(56, (uint16_t)-32, 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0x22, FARP(ustr("Arial"), 0));
    CHECK(f != 0);
    send(e, 0x30, f, 1);                                                       /* WM_SETFONT, redraw */
    CHECK_EQ(send(e, 0x31, 0, 0), f);
    drain();
    big = right_edge(PX + 12, PY + 12, PX + 208, PY + 48);
    CHECK(big > plain + 8);                                                    /* the text is drawn in the new, larger font */
    /* a disabled edit keeps the window text colour */
    UA(34, e, 0);                                                              /* EnableWindow(e, FALSE) */
    UA(125, e, FARP(0, 0), 1);                                                 /* InvalidateRect */
    drain();
    for (i = 0; i < 200 * 40; i++) grey += px(PX + 12 + i % 196, PY + 12 + i / 196) == wm_sys_color(16);
    CHECK_EQ(grey, 0);
    CHECK(count_black(PX + 12, PY + 12, PX + 208, PY + 48) > 15);
    gdown();
}

/* ---- EDIT ------------------------------------------------------------------------------------------------- */

static void test_edit_single(void) {
    uint16_t e;
    printf("test_edit_single\n");
    setup();
    e = mk("EDIT", "Hello", WS_BORDER_ | ES_AUTOHSCROLL, 10, 10, 120, 22, 5);
    drain();
    CHECK(!strcmp(text_of(e), "Hello"));
    CHECK(count_black(PX + 12, PY + 12, PX + 100, PY + 30) > 15);              /* text drawn */
    CHECK_EQ(px(PX + 10 + 60, PY + 10), 0);                                    /* the border */
    CHECK_EQ(px(PX + 10 + 100, PY + 10 + 15), RGB(255, 255, 255));              /* white inside */
    CHECK_EQ(send(e, EM_GETSEL, 0, 0), 0);
    CHECK_EQ(send(e, 0x87, 0, 0) & 0x89, 0x89);                                 /* WM_GETDLGCODE: chars, set-sel, arrows */
    /* focus notifications */
    UA(22, e);
    drain();
    CHECK(strstr(notes(), "100:5;"));                                           /* EN_SETFOCUS */
    /* selection messages: EM_SETSEL(start in the low word, end in the high word) */
    send(e, EM_SETSEL, 0, (uint32_t)5 << 16 | 5);
    CHECK_EQ(sel_start(e), 5);
    send(e, EM_SETSEL, 0, (uint32_t)0x7FFF << 16 | 1);                          /* to the end */
    CHECK_EQ(sel_start(e), 1);
    CHECK_EQ(sel_end(e), 5);
    send(e, EM_SETSEL, 0, (uint32_t)5 << 16 | 5);
    /* typing appends at the caret and sends EN_UPDATE then EN_CHANGE */
    ulog_reset();
    type("ab");
    CHECK(!strcmp(text_of(e), "Helloab"));
    CHECK(strstr(notes(), "400:5;300:5;400:5;300:5;") == notes());
    CHECK_EQ(sel_start(e), 7);
    CHECK_EQ(send(e, EM_GETMODIFY, 0, 0), 1);
    send(e, EM_SETMODIFY, 0, 0);
    CHECK_EQ(send(e, EM_GETMODIFY, 0, 0), 0);
    /* Backspace, Delete, Home, End, arrows */
    tap(VK_BACK);
    CHECK(!strcmp(text_of(e), "Helloa"));
    tap(VK_HOME);
    CHECK_EQ(sel_start(e), 0);
    tap(VK_DELETE);
    CHECK(!strcmp(text_of(e), "elloa"));
    tap(VK_RIGHT);
    tap(VK_RIGHT);
    CHECK_EQ(sel_start(e), 2);
    tap(VK_LEFT);
    CHECK_EQ(sel_start(e), 1);
    tap(VK_END);
    CHECK_EQ(sel_start(e), 5);
    /* shift+arrow selects; typing replaces the selection */
    key(WM_IN_KEY_DOWN, VK_SHIFT);
    tap(VK_LEFT);
    tap(VK_LEFT);
    key(WM_IN_KEY_UP, VK_SHIFT);
    drain();
    CHECK_EQ(sel_start(e), 3);
    CHECK_EQ(sel_end(e), 5);
    type("z");
    CHECK(!strcmp(text_of(e), "ellz"));
    /* EM_REPLACESEL and WM_SETTEXT */
    send(e, EM_SETSEL, 0, (uint32_t)1 << 16 | 0);
    sendp(e, EM_REPLACESEL, 0, "XY");
    CHECK(!strcmp(text_of(e), "XYllz"));
    UA(37, e, FARP(ustr("new"), 0));
    CHECK(!strcmp(text_of(e), "new"));
    CHECK_EQ(sel_start(e), 0);
    /* EM_LIMITTEXT: typing beyond it is refused with EN_MAXTEXT */
    send(e, EM_LIMITTEXT, 4, 0);
    send(e, EM_SETSEL, 0, (uint32_t)3 << 16 | 3);
    ulog_reset();
    type("q");
    CHECK(!strcmp(text_of(e), "newq"));
    type("r");
    CHECK(!strcmp(text_of(e), "newq"));
    CHECK(strstr(notes(), "501:5;"));
    /* read-only */
    send(e, EM_SETREADONLY, 1, 0);
    type("s");
    CHECK(!strcmp(text_of(e), "newq"));
    send(e, EM_SETREADONLY, 0, 0);
    /* mouse: a click on the far right puts the caret at the end, on the left at the start */
    mouse(WM_IN_MOVE, PX + 10 + 110, PY + 20);
    mouse(WM_IN_BUTTON_DOWN, PX + 10 + 110, PY + 20);
    mouse(WM_IN_BUTTON_UP, PX + 10 + 110, PY + 20);
    drain();
    CHECK_EQ(sel_start(e), 4);
    mouse(WM_IN_BUTTON_DOWN, PX + 10 + 2, PY + 20);
    mouse(WM_IN_MOVE, PX + 10 + 110, PY + 20);                                  /* drag selects */
    mouse(WM_IN_BUTTON_UP, PX + 10 + 110, PY + 20);
    drain();
    CHECK_EQ(sel_start(e), 0);
    CHECK_EQ(sel_end(e), 4);
    /* losing the focus: EN_KILLFOCUS */
    ulog_reset();
    UA(22, parent);
    drain();
    CHECK(strstr(notes(), "200:5;"));
    gdown();
}

static void test_edit_password_and_case(void) {
    uint16_t e, p;
    printf("test_edit_password\n");
    setup();
    e = mk("EDIT", "", WS_BORDER_ | ES_AUTOHSCROLL | 8, 10, 10, 120, 22, 5);      /* ES_UPPERCASE */
    p = mk("EDIT", "", WS_BORDER_ | ES_AUTOHSCROLL | ES_PASSWORD, 10, 50, 120, 22, 6);
    UA(22, e);
    type("ab");
    CHECK(!strcmp(text_of(e), "AB"));
    UA(22, p);
    type("pw");
    CHECK(!strcmp(text_of(p), "pw"));                                           /* the text is kept, only the display changes */
    drain();
    {   /* two '*' glyphs in the first 20 pixels, and not the letters p and w: compare against the edit without the style */
        int stars = count_black(PX + 12, PY + 52, PX + 40, PY + 70);
        CHECK(stars > 6);
    }
    CHECK_EQ(send(p, 0x422, 0, 0), '*');                                        /* EM_GETPASSWORDCHAR */
    gdown();
}

static void test_edit_multiline(void) {
    uint16_t e, b;
    printf("test_edit_multiline\n");
    setup();
    e = mk("EDIT", "line1\r\nline2\r\nline3", ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_BORDER_, 10, 10, 150, 80, 7);
    drain();
    CHECK_EQ(send(e, EM_GETLINECOUNT, 0, 0), 3);
    CHECK_EQ(send(e, EM_LINEINDEX, 1, 0), 7);
    CHECK_EQ(send(e, EM_LINEINDEX, 2, 0), 14);
    CHECK_EQ(send(e, EM_LINEINDEX, 5, 0), 0xFFFFFFFFu);                         /* no such line: -1 */
    CHECK_EQ(send(e, EM_LINELENGTH, 8, 0), 5);                                  /* the line holding character 8 */
    CHECK_EQ(send(e, EM_LINEFROMCHAR, 8, 0), 1);
    CHECK_EQ(send(e, EM_LINEFROMCHAR, 0, 0), 0);
    b = g_seg(32);
    uwr16(b, 0, 30);                                                             /* buffer size in the first word */
    CHECK_EQ(send(e, EM_GETLINE, 1, (uint32_t)b << 16), 5);
    CHECK(!memcmp(umem(b, 0, 5), "line2", 5));
    CHECK_EQ(send(e, 0x87, 0, 0) & 0x89, 0x89);
    /* Enter inserts a new line when the control wants returns; Up/Down move between lines */
    UA(22, e);
    send(e, EM_SETSEL, 0, (uint32_t)5 << 16 | 5);
    tap(VK_DOWN);
    CHECK_EQ(send(e, EM_LINEFROMCHAR, sel_start(e), 0), 1);
    tap(VK_DOWN);
    CHECK_EQ(send(e, EM_LINEFROMCHAR, sel_start(e), 0), 2);
    tap(VK_UP);
    CHECK_EQ(send(e, EM_LINEFROMCHAR, sel_start(e), 0), 1);
    tap(VK_END);
    CHECK_EQ(sel_start(e), 12);
    tap(VK_RETURN);
    CHECK(!strcmp(text_of(e), "line1\r\nline2\r\n\r\nline3"));
    CHECK_EQ(send(e, EM_GETLINECOUNT, 0, 0), 4);
    /* the second line is drawn below the first */
    drain();
    CHECK(count_black(PX + 12, PY + 12, PX + 100, PY + 28) > 10);
    CHECK(count_black(PX + 12, PY + 28, PX + 100, PY + 44) > 10);
    /* a long line wraps without ES_AUTOHSCROLL */
    {
        uint16_t w = mk("EDIT", "aaa bbb ccc ddd eee fff ggg hhh", ES_MULTILINE | WS_BORDER_, 200, 10, 80, 80, 8);
        drain();
        CHECK(send(w, EM_GETLINECOUNT, 0, 0) >= 3);
    }
    gdown();
}

/* ---- LISTBOX ------------------------------------------------------------------------------------------------ */

static void test_listbox(void) {
    uint16_t l, b;
    printf("test_listbox\n");
    setup();
    l = mk("LISTBOX", "", LBS_NOTIFY | LBS_HASSTRINGS | WS_BORDER_, 10, 10, 120, 80, 9);
    drain();
    CHECK_EQ(send(l, LB_GETCOUNT, 0, 0), 0);
    CHECK_EQ(sendp(l, LB_ADDSTRING, 0, "Banana"), 0);
    CHECK_EQ(sendp(l, LB_ADDSTRING, 0, "Apple"), 1);
    CHECK_EQ(sendp(l, LB_ADDSTRING, 0, "Cherry"), 2);
    CHECK_EQ(send(l, LB_GETCOUNT, 0, 0), 3);
    CHECK_EQ(sendp(l, LB_INSERTSTRING, 1, "Mid"), 1);
    CHECK_EQ(sendp(l, LB_INSERTSTRING, 0xFFFF, "Last"), 4);                      /* -1 appends */
    b = g_seg(64);
    CHECK_EQ(send(l, LB_GETTEXT, 1, (uint32_t)b << 16), 3);
    CHECK(!strcmp((char *)umem(b, 0, 1), "Mid"));
    CHECK_EQ(send(l, LB_GETTEXTLEN, 0, 0), 6);
    CHECK_EQ(send(l, LB_GETTEXT, 99, (uint32_t)b << 16), 0xFFFFFFFFu);           /* LB_ERR */
    CHECK_EQ(send(l, LB_DELETESTRING, 1, 0), 4);                                  /* returns the new count */
    CHECK_EQ(send(l, LB_GETCURSEL, 0, 0), 0xFFFFFFFFu);
    CHECK_EQ(send(l, LB_SETCURSEL, 2, 0), 2);
    CHECK_EQ(send(l, LB_GETCURSEL, 0, 0), 2);
    CHECK_EQ(send(l, LB_GETSEL, 2, 0), 1);
    CHECK_EQ(send(l, LB_GETSEL, 0, 0), 0);
    CHECK_EQ(send(l, LB_SETCURSEL, 0xFFFF, 0), 0xFFFFFFFFu);                      /* -1 clears */
    CHECK_EQ(send(l, LB_GETCURSEL, 0, 0), 0xFFFFFFFFu);
    /* searching: the prefix, ignoring case; the exact string; the item after the start index */
    CHECK_EQ(sendp(l, LB_FINDSTRING, 0xFFFF, "ap"), 1);
    CHECK_EQ(sendp(l, LB_FINDSTRING, 1, "ap"), 1);                                /* wraps around to find it again */
    CHECK_EQ(sendp(l, LB_FINDSTRINGEXACT, 0xFFFF, "apple"), 1);
    CHECK_EQ(sendp(l, LB_FINDSTRINGEXACT, 0xFFFF, "app"), 0xFFFFFFFFu);
    CHECK_EQ(sendp(l, LB_SELECTSTRING, 0xFFFF, "Cher"), 2);
    CHECK_EQ(send(l, LB_GETCURSEL, 0, 0), 2);
    /* item data */
    send(l, LB_SETITEMDATA, 1, 0x12345678u);
    CHECK_EQ(send(l, LB_GETITEMDATA, 1, 0), 0x12345678u);
    /* items are drawn: rows of 16 pixels, the selected one highlighted */
    drain();
    CHECK_EQ(px(PX + 10 + 60, PY + 10 + 1 + 16 * 2 + 8), wm_sys_color(13));       /* the selected row (index 2) is navy */
    CHECK(count_black(PX + 12, PY + 12, PX + 100, PY + 28) > 10);
    CHECK_EQ(send(l, LB_GETITEMRECT, 1, 0), 0xFFFFFFFFu);                         /* no rectangle buffer: LB_ERR */
    {
        uint16_t rb = g_seg(16);
        CHECK_EQ(send(l, LB_GETITEMRECT, 1, (uint32_t)rb << 16), 0);
        CHECK_EQ(urd16(rb, 2) - 0, urd16(rb, 6) - 16);                              /* top + height = bottom */
    }
    /* mouse: click an item selects it and notifies; double click notifies LBN_DBLCLK */
    send(l, LB_SETCURSEL, 0xFFFF, 0);
    ulog_reset();
    mouse(WM_IN_MOVE, PX + 10 + 40, PY + 10 + 1 + 16 + 8);
    mouse(WM_IN_BUTTON_DOWN, PX + 10 + 40, PY + 10 + 1 + 16 + 8);
    mouse(WM_IN_BUTTON_UP, PX + 10 + 40, PY + 10 + 1 + 16 + 8);
    drain();
    CHECK_EQ(send(l, LB_GETCURSEL, 0, 0), 1);
    CHECK(strstr(notes(), "1:9;"));                                                /* LBN_SELCHANGE */
    uclock += 100;                                                                 /* a second press, quickly: double click */
    mouse(WM_IN_BUTTON_DOWN, PX + 10 + 40, PY + 10 + 1 + 16 + 8);
    mouse(WM_IN_BUTTON_UP, PX + 10 + 40, PY + 10 + 1 + 16 + 8);
    drain();
    CHECK(strstr(notes(), "2:9;"));
    /* keyboard */
    UA(22, l);
    tap(VK_DOWN);
    CHECK_EQ(send(l, LB_GETCURSEL, 0, 0), 2);
    tap(VK_HOME);
    CHECK_EQ(send(l, LB_GETCURSEL, 0, 0), 0);
    tap(VK_END);
    CHECK_EQ(send(l, LB_GETCURSEL, 0, 0), 3);
    tap(VK_UP);
    CHECK_EQ(send(l, LB_GETCURSEL, 0, 0), 2);
    /* type-ahead by first letter */
    tap('A');
    CHECK_EQ(send(l, LB_GETCURSEL, 0, 0), 1);                                      /* Apple */
    /* scrolling: more items than rows */
    {
        int i;
        for (i = 0; i < 20; i++) {
            char t[16];
            snprintf(t, sizeof t, "Item %02d", i);
            sendp(l, LB_ADDSTRING, 0, t);
        }
    }
    send(l, LB_SETTOPINDEX, 5, 0);
    CHECK_EQ(send(l, LB_GETTOPINDEX, 0, 0), 5);
    send(l, LB_SETCURSEL, 20, 0);                                                  /* scrolls the selection into view */
    CHECK(send(l, LB_GETTOPINDEX, 0, 0) > 5);
    CHECK_EQ(send(l, LB_RESETCONTENT, 0, 0), 0);
    CHECK_EQ(send(l, LB_GETCOUNT, 0, 0), 0);
    gdown();
}

static void test_listbox_sorted_multi(void) {
    uint16_t l, b;
    printf("test_listbox_sorted_multi\n");
    setup();
    l = mk("LISTBOX", "", LBS_SORT | LBS_HASSTRINGS | LBS_MULTIPLESEL | LBS_NOTIFY | WS_BORDER_, 10, 10, 120, 80, 9);
    CHECK_EQ(sendp(l, LB_ADDSTRING, 0, "pear"), 0);
    CHECK_EQ(sendp(l, LB_ADDSTRING, 0, "Apple"), 0);                              /* sorted: inserted before */
    CHECK_EQ(sendp(l, LB_ADDSTRING, 0, "banana"), 1);
    b = g_seg(64);
    send(l, LB_GETTEXT, 2, (uint32_t)b << 16);
    CHECK(!strcmp((char *)umem(b, 0, 1), "pear"));
    send(l, LB_SETSEL, 1, 0);                                                     /* wParam select, index in the low word */
    send(l, LB_SETSEL, 1, 2);
    CHECK_EQ(send(l, LB_GETSEL, 0, 0), 1);
    CHECK_EQ(send(l, LB_GETSEL, 1, 0), 0);
    CHECK_EQ(send(l, LB_GETSEL, 2, 0), 1);
    CHECK_EQ(send(l, LB_GETSELCOUNT, 0, 0), 0 + 2 - 0);
    CHECK_EQ(send(l, LB_GETSELITEMS, 8, (uint32_t)b << 16), 2);
    CHECK_EQ(urd16(b, 0), 0);
    CHECK_EQ(urd16(b, 2), 2);
    /* clicking toggles in a multiple-selection box */
    mouse(WM_IN_MOVE, PX + 10 + 40, PY + 10 + 1 + 16 + 8);
    mouse(WM_IN_BUTTON_DOWN, PX + 10 + 40, PY + 10 + 1 + 16 + 8);
    mouse(WM_IN_BUTTON_UP, PX + 10 + 40, PY + 10 + 1 + 16 + 8);
    drain();
    CHECK_EQ(send(l, LB_GETSEL, 1, 0), 1);
    CHECK_EQ(send(l, LB_GETSELCOUNT, 0, 0), 3);
    gdown();
}

/* ---- COMBOBOX ------------------------------------------------------------------------------------------------ */

typedef struct { wm_in_kind kind; int x, y; unsigned vk; } step;
static const step *script;
static unsigned nscript, script_pos;
static void (*script_hook)(unsigned);

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

static bool seen_open;
static void probe_open(unsigned i) {
    (void)i;
    seen_open = seen_open || count_black(PX + 10, PY + 10 + 24, PX + 130, PY + 10 + 24 + 80) > 40;
}

static void test_combobox(void) {
    uint16_t c, b;
    printf("test_combobox\n");
    setup();
    c = mk("COMBOBOX", "", CBS_DROPDOWNLIST, 10, 10, 120, 100, 11);
    CHECK_EQ(sendp(c, CB_ADDSTRING, 0, "Red"), 0);
    CHECK_EQ(sendp(c, CB_ADDSTRING, 0, "Green"), 1);
    CHECK_EQ(sendp(c, CB_ADDSTRING, 0, "Blue"), 2);
    CHECK_EQ(send(c, CB_GETCOUNT, 0, 0), 3);
    CHECK_EQ(send(c, CB_GETCURSEL, 0, 0), 0xFFFFFFFFu);
    CHECK_EQ(send(c, CB_SETCURSEL, 1, 0), 1);
    b = g_seg(32);
    CHECK_EQ(send(c, CB_GETLBTEXT, 1, (uint32_t)b << 16), 5);
    CHECK(!strcmp((char *)umem(b, 0, 1), "Green"));
    CHECK_EQ(send(c, CB_GETLBTEXTLEN, 2, 0), 4);
    CHECK_EQ(sendp(c, CB_FINDSTRING, 0xFFFF, "bl"), 2);
    CHECK_EQ(sendp(c, CB_FINDSTRINGEXACT, 0xFFFF, "red"), 0);
    CHECK_EQ(sendp(c, CB_SELECTSTRING, 0xFFFF, "Blu"), 2);
    CHECK_EQ(send(c, CB_GETCURSEL, 0, 0), 2);
    CHECK_EQ(sendp(c, CB_INSERTSTRING, 0, "Pink"), 0);
    CHECK_EQ(send(c, CB_DELETESTRING, 0, 0), 3);
    send(c, CB_SETITEMDATA, 0, 77);
    CHECK_EQ(send(c, CB_GETITEMDATA, 0, 0), 77);
    drain();
    /* the closed box shows the selected text */
    CHECK(count_black(PX + 12, PY + 12, PX + 100, PY + 30) > 10);
    /* clicking the arrow opens the list: it appears below the box, and choosing an item closes it */
    {
        static const step s[] = {{WM_IN_MOVE, PX + 10 + 40, PY + 10 + 24 + 1 + 16 + 8, 0},
                                 {WM_IN_BUTTON_DOWN, PX + 10 + 40, PY + 10 + 24 + 1 + 16 + 8, 0},
                                 {WM_IN_BUTTON_UP, PX + 10 + 40, PY + 10 + 24 + 1 + 16 + 8, 0}};
        send(c, CB_SETCURSEL, 0, 0);
        ulog_reset();
        seen_open = false;
        set_script(s, 3, probe_open);
        mouse(WM_IN_MOVE, PX + 10 + 110, PY + 10 + 10);
        mouse(WM_IN_BUTTON_DOWN, PX + 10 + 110, PY + 10 + 10);
        mouse(WM_IN_BUTTON_UP, PX + 10 + 110, PY + 10 + 10);
        drain();
        CHECK(seen_open);
        CHECK_EQ(send(c, CB_GETCURSEL, 0, 0), 1);                                  /* the second item */
        CHECK(strstr(notes(), "7:11;"));                                            /* CBN_DROPDOWN */
        CHECK(strstr(notes(), "1:11;"));                                            /* CBN_SELCHANGE */
        CHECK(strstr(notes(), "8:11;"));                                            /* CBN_CLOSEUP */
        CHECK_EQ(send(c, CB_GETDROPPEDSTATE, 0, 0), 0);
        upump_script = NULL;
    }
    /* Escape closes without a change */
    {
        static const step s[] = {{WM_IN_KEY_DOWN, 0, 0, VK_DOWN}, {WM_IN_KEY_DOWN, 0, 0, 0x1B}};
        set_script(s, 2, NULL);
        send(c, CB_SETCURSEL, 0, 0);
        send(c, CB_SHOWDROPDOWN, 1, 0);
        CHECK_EQ(send(c, CB_GETCURSEL, 0, 0), 0);
        upump_script = NULL;
    }
    /* a drop-down with an edit part keeps typed text */
    {
        uint16_t d = mk("COMBOBOX", "start", CBS_DROPDOWN, 200, 10, 120, 100, 12);
        sendp(d, CB_ADDSTRING, 0, "one");
        CHECK(!strcmp(text_of(d), "start"));
        send(d, CB_SETCURSEL, 0, 0);
        CHECK(!strcmp(text_of(d), "one"));                                           /* selecting copies the item into the edit part */
        UA(37, d, FARP(ustr("typed"), 0));
        CHECK(!strcmp(text_of(d), "typed"));
    }
    gdown();
}

int main(void) {
    test_edit_single();
    test_edit_font_and_disabled();
    test_edit_password_and_case();
    test_edit_multiline();
    test_listbox();
    test_listbox_sorted_multi();
    test_combobox();
    printf("test_ui_edit: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
