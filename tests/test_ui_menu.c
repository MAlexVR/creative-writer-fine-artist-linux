/* Menus: the menu object API, menu resources, window menus and the system menu. */
#include "gfixture.h"
#include "res.h"
#include "rfixture.h"

#define MF_GRAYED 0x0001
#define MF_DISABLED 0x0002
#define MF_BITMAP 0x0004
#define MF_CHECKED 0x0008
#define MF_POPUP 0x0010
#define MF_BYPOSITION 0x0400
#define MF_SEPARATOR 0x0800
#define MF_HELP 0x4000

enum {
    U_CREATEMENU = 151, U_DESTROYMENU = 152, U_CHECKMENUITEM = 154, U_ENABLEMENUITEM = 155, U_GETSYSTEMMENU = 156,
    U_GETMENU = 157, U_SETMENU = 158, U_GETSUBMENU = 159, U_DRAWMENUBAR = 160, U_GETMENUSTRING = 161,
    U_LOADMENU = 150, U_LOADMENUINDIRECT = 220, U_GETMENUSTATE = 250, U_GETMENUITEMCOUNT = 263, U_GETMENUITEMID = 264,
    U_INSERTMENU = 410, U_APPENDMENU = 411, U_REMOVEMENU = 412, U_DELETEMENU = 413, U_MODIFYMENU = 414,
    U_CREATEPOPUPMENU = 415
};

static uint16_t add_item(uint16_t m, unsigned flags, unsigned id, const char *text) {
    uint16_t s = text ? ustr(text) : 0;
    return UA(U_APPENDMENU, m, (uint16_t)flags, (uint16_t)id, FARP(s, 0));
}

static uint16_t menu_count(uint16_t m) { return UA(U_GETMENUITEMCOUNT, m); }

static const char *menu_text(uint16_t m, unsigned id, unsigned flags) {
    static char out[128];
    uint16_t b = g_seg(0x100);
    uint16_t n = UA(U_GETMENUSTRING, m, (uint16_t)id, FARP(b, 0), 0x80, (uint16_t)flags);
    if (n == 0xFFFF || n == 0) {
        out[0] = 0;
        return out;
    }
    strcpy(out, (char *)umem(b, 0, 1));
    return out;
}

static void test_menu_objects(void) {
    uint16_t bar, pop, ext;
    printf("test_menu_objects\n");
    gboot();
    bar = UA(U_CREATEMENU);
    pop = UA(U_CREATEPOPUPMENU);
    CHECK(bar != 0 && pop != 0 && bar != pop);
    CHECK_EQ(add_item(bar, 0, 100, "&New"), 1);
    CHECK_EQ(add_item(bar, MF_POPUP, pop, "Pop"), 1);
    add_item(pop, MF_CHECKED, 200, "Item2");
    add_item(pop, MF_SEPARATOR, 0, NULL);
    add_item(pop, MF_GRAYED, 201, "Item3");
    CHECK_EQ(menu_count(bar), 2);
    CHECK_EQ(menu_count(pop), 3);
    CHECK_EQ(UA(U_GETMENUITEMID, bar, 0), 100);
    CHECK_EQ(UA(U_GETMENUITEMID, bar, 1), 0xFFFF);          /* a popup has no command id */
    CHECK_EQ(UA(U_GETMENUITEMID, pop, 1), 0);               /* a separator */
    CHECK_EQ(UA(U_GETMENUITEMID, pop, 9), 0xFFFF);          /* no such item */
    CHECK_EQ(UA(U_GETSUBMENU, bar, 1), pop);
    CHECK_EQ(UA(U_GETSUBMENU, bar, 0), 0);
    CHECK_EQ(UA(U_GETMENUSTATE, pop, 200, 0), MF_CHECKED);
    CHECK_EQ(UA(U_GETMENUSTATE, pop, 2, MF_BYPOSITION), MF_GRAYED);
    CHECK_EQ(UA(U_GETMENUSTATE, bar, 1, MF_BYPOSITION), (MF_POPUP | 3 << 8));
    CHECK_EQ(UA(U_GETMENUSTATE, pop, 1, MF_BYPOSITION) & MF_SEPARATOR, MF_SEPARATOR);
    CHECK_EQ(UA(U_GETMENUSTATE, bar, 999, 0), 0xFFFF);
    CHECK(!strcmp(menu_text(bar, 100, 0), "&New"));
    CHECK(!strcmp(menu_text(bar, 1, MF_BYPOSITION), "Pop"));
    CHECK_EQ(UA(U_GETMENUSTRING, bar, 100, FARP(g_seg(16), 0), 3, 0), 2);   /* truncated to the buffer */
    /* check and enable return the previous state; command ids are searched in popups too */
    CHECK_EQ(UA(U_CHECKMENUITEM, pop, 201, MF_CHECKED), 0);
    CHECK_EQ(UA(U_GETMENUSTATE, pop, 201, 0), MF_CHECKED | MF_GRAYED);
    CHECK_EQ(UA(U_CHECKMENUITEM, pop, 201, 0), MF_CHECKED);
    CHECK_EQ(UA(U_ENABLEMENUITEM, pop, 201, 0), MF_GRAYED);
    CHECK_EQ(UA(U_GETMENUSTATE, pop, 201, 0), 0);
    CHECK_EQ(UA(U_ENABLEMENUITEM, bar, 200, MF_GRAYED), 0);  /* found inside the popup */
    CHECK_EQ(UA(U_GETMENUSTATE, pop, 200, 0), MF_CHECKED | MF_GRAYED);
    CHECK_EQ(UA(U_ENABLEMENUITEM, bar, 200, MF_DISABLED), MF_GRAYED);
    CHECK_EQ(UA(U_GETMENUSTATE, pop, 200, 0) & 3, MF_DISABLED);
    CHECK_EQ(UA(U_ENABLEMENUITEM, bar, 777, MF_GRAYED), 0xFFFF);
    CHECK_EQ(UA(U_CHECKMENUITEM, bar, 777, MF_CHECKED), 0xFFFF);
    /* insert, modify, remove, delete */
    {
        uint16_t s = ustr("Mid");
        CHECK_EQ(UA(U_INSERTMENU, bar, 1, MF_BYPOSITION, 150, FARP(s, 0)), 1);
        CHECK_EQ(menu_count(bar), 3);
        CHECK_EQ(UA(U_GETMENUITEMID, bar, 1), 150);
        CHECK_EQ(UA(U_GETSUBMENU, bar, 2), pop);
        s = ustr("Mid2");
        CHECK_EQ(UA(U_MODIFYMENU, bar, 150, 0, 151, FARP(s, 0)), 1);
        CHECK_EQ(UA(U_GETMENUITEMID, bar, 1), 151);
        CHECK(!strcmp(menu_text(bar, 151, 0), "Mid2"));
        CHECK_EQ(UA(U_MODIFYMENU, bar, 99, 0, 1, FARP(s, 0)), 0);   /* missing item */
    }
    CHECK_EQ(UA(U_REMOVEMENU, bar, 0, MF_BYPOSITION), 1);
    CHECK_EQ(menu_count(bar), 2);
    CHECK_EQ(UA(U_REMOVEMENU, bar, 99, 0), 0);
    CHECK_EQ(UA(U_REMOVEMENU, bar, 1, MF_BYPOSITION), 1);       /* removes the popup but keeps it alive */
    CHECK_EQ(menu_count(pop), 3);
    CHECK_EQ(add_item(bar, MF_POPUP, pop, "Pop"), 1);
    CHECK_EQ(UA(U_DELETEMENU, bar, 1, MF_BYPOSITION), 1);       /* deletes and destroys it */
    CHECK_EQ(menu_count(pop), 0xFFFF);
    ext = UA(U_CREATEPOPUPMENU);
    add_item(bar, MF_POPUP, ext, "X");
    add_item(ext, 0, 5, "deep");
    CHECK_EQ(UA(U_DESTROYMENU, bar), 1);                         /* destroys the submenus too */
    CHECK_EQ(menu_count(bar), 0xFFFF);
    CHECK_EQ(menu_count(ext), 0xFFFF);
    CHECK_EQ(UA(U_DESTROYMENU, bar), 0);
    gdown();
}

static void menu_blob(rb_t *b) {
    b->n = 0;
    rb_u16(b, 0); rb_u16(b, 0);
    rb_u16(b, 0x10); rb_str(b, "&File");
    rb_u16(b, 0); rb_u16(b, 100); rb_str(b, "&New\tCtrl+N");
    rb_u16(b, 0); rb_u16(b, 0); rb_str(b, "");
    rb_u16(b, 0x81); rb_u16(b, 101); rb_str(b, "E&xit");
    rb_u16(b, 0x90); rb_str(b, "&Help");
    rb_u16(b, 0x88 | 0x4000); rb_u16(b, 102); rb_str(b, "&About");
}

static void test_menu_resources(void) {
    rb_t b;
    uint16_t nm, h, file, tmpl;
    printf("test_menu_resources\n");
    gboot();
    menu_blob(&b);
    rf_add(&uf, RT_MENU, NULL, 0, "MAIN", b.b, b.n);
    nm = ustr("main");
    h = UA(U_LOADMENU, u_ds(), nm, 0);
    CHECK(h != 0);
    CHECK_EQ(menu_count(h), 2);
    file = UA(U_GETSUBMENU, h, 0);
    CHECK(file != 0);
    CHECK_EQ(menu_count(file), 3);
    CHECK(!strcmp(menu_text(file, 100, 0), "&New\tCtrl+N"));
    CHECK_EQ(UA(U_GETMENUITEMID, file, 1), 0);
    CHECK_EQ(UA(U_GETMENUSTATE, file, 101, 0), MF_GRAYED);
    CHECK_EQ(UA(U_GETMENUSTATE, file, 1, MF_BYPOSITION) & MF_SEPARATOR, MF_SEPARATOR);
    CHECK_EQ(UA(U_GETMENUSTATE, UA(U_GETSUBMENU, h, 1), 102, 0), MF_CHECKED | MF_HELP);
    CHECK_EQ(UA(U_LOADMENU, u_ds(), ustr("nothing"), 0), 0);
    CHECK_EQ(UA(U_LOADMENU, 0x7777, nm, 0), 0);
    tmpl = g_seg(0x200);                                         /* LoadMenuIndirect: a template in guest memory */
    memcpy(umem(tmpl, 0, b.n), b.b, b.n);
    CHECK(UA(U_LOADMENUINDIRECT, FARP(tmpl, 0)) != 0);
    CHECK_EQ(UA(U_LOADMENU, u_ds(), 0, 3), 0);                   /* by id: not there */
    gdown();
}

static uint16_t reg_class_menu(const char *name, const char *menu) {
    uint16_t cs = useg(0x100), ns = ustr(name), ms = menu ? ustr(menu) : 0;
    uwr16(cs, 0, 0);
    uwr32(cs, 2, 0);
    uwr16(cs, 6, 0);
    uwr16(cs, 8, 0);
    uwr16(cs, 10, u_ds());
    uwr16(cs, 12, 0);
    uwr16(cs, 14, 0);
    uwr16(cs, 16, 6);
    uwr32(cs, 18, menu ? (uint32_t)ms << 16 : 0);
    uwr32(cs, 22, (uint32_t)ns << 16);
    UCALL(57, FARP(cs, 0));
    return fx_ax(&uf);
}

static void test_window_menus(void) {
    uint16_t bar, hw, hw2, sys, sys2;
    rb_t b;
    fb_rect r0, r1;
    printf("test_window_menus\n");
    gboot();
    reg_class_menu("MW", NULL);
    bar = UA(U_CREATEMENU);
    add_item(bar, 0, 100, "One");
    hw = u_create("MW", "w", WS_OVERLAPPEDWINDOW, 10, 10, 300, 200, 0, bar, u_ds());
    CHECK(hw != 0);
    CHECK_EQ(UA(U_GETMENU, hw), bar);
    wm_client_rect(user_wm(uu), hw, &r0);
    CHECK_EQ(UA(U_SETMENU, hw, 0), 1);
    CHECK_EQ(UA(U_GETMENU, hw), 0);
    wm_client_rect(user_wm(uu), hw, &r1);
    CHECK_EQ(r1.bottom - r1.top, r0.bottom - r0.top + 20);       /* the bar's row goes back to the client */
    CHECK_EQ(UA(U_SETMENU, hw, bar), 1);
    CHECK_EQ(UA(U_GETMENU, hw), bar);
    CHECK_EQ(UA(U_DRAWMENUBAR, hw), 1);
    CHECK_EQ(UA(U_GETMENU, 0x7777), 0);
    /* the system menu */
    sys = UA(U_GETSYSTEMMENU, hw, 0);
    CHECK(sys != 0);
    CHECK_EQ(menu_count(sys), 7);
    CHECK_EQ(UA(U_GETMENUITEMID, sys, 0), 0xF120);               /* Restore */
    CHECK_EQ(UA(U_GETMENUITEMID, sys, 1), 0xF010);               /* Move */
    CHECK_EQ(UA(U_GETMENUITEMID, sys, 2), 0xF000);               /* Size */
    CHECK_EQ(UA(U_GETMENUITEMID, sys, 3), 0xF020);               /* Minimize */
    CHECK_EQ(UA(U_GETMENUITEMID, sys, 4), 0xF030);               /* Maximize */
    CHECK_EQ(UA(U_GETMENUITEMID, sys, 5), 0);                    /* separator */
    CHECK_EQ(UA(U_GETMENUITEMID, sys, 6), 0xF060);               /* Close */
    CHECK_EQ(UA(U_GETMENUSTATE, sys, 0xF120, 0) & 3, MF_GRAYED); /* nothing to restore */
    CHECK_EQ(UA(U_GETSYSTEMMENU, hw, 0), sys);                   /* the same copy each time */
    CHECK_EQ(UA(U_DELETEMENU, sys, 0xF000, 0), 1);               /* ARTIST removes entries like this */
    CHECK_EQ(menu_count(sys), 6);
    sys2 = UA(U_GETSYSTEMMENU, hw, 1);                           /* revert: back to the default */
    CHECK_EQ(sys2, 0);                                           /* nothing is returned when reverting */
    CHECK_EQ(menu_count(UA(U_GETSYSTEMMENU, hw, 0)), 7);
    hw2 = u_create("MW", "nosys", WS_POPUP | WS_CAPTION, 0, 0, 50, 50, 0, 0, u_ds());
    CHECK_EQ(UA(U_GETSYSTEMMENU, hw2, 0), 0);                    /* no WS_SYSMENU: none */
    /* a class menu name loads when no menu is given */
    menu_blob(&b);
    rf_add(&uf, RT_MENU, NULL, 0, "CLSMENU", b.b, b.n);
    reg_class_menu("CM", "CLSMENU");
    hw2 = u_create("CM", "cm", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, 0, 0, u_ds());
    CHECK(hw2 != 0);
    CHECK(UA(U_GETMENU, hw2) != 0);
    CHECK_EQ(menu_count(UA(U_GETMENU, hw2)), 2);
    sys = UA(U_GETMENU, hw2);
    /* a window's menu dies with it */
    CHECK_EQ(UA(53, hw2), 1);
    CHECK_EQ(menu_count(sys), 0xFFFF);
    gdown();
}

int main(void) {
    test_menu_objects();
    test_menu_resources();
    test_window_menus();
    printf("test_ui_menu: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
