/* USER HLE, window side: ordinal table, metrics, classes, window creation with
 * guest window procedures run through the CPU, geometry, words, predicates,
 * SendMessage re-entrancy and callback faults. */
#include "gdi.h"
#include "ufixture.h"
#include "win16_imports.h"

static const struct { unsigned ord; const char *name; } verified[] = {
    {1, "MessageBox"}, {5, "InitApp"}, {10, "SetTimer"}, {12, "KillTimer"}, {13, "GetTickCount"},
    {17, "GetCursorPos"}, {18, "SetCapture"}, {19, "ReleaseCapture"}, {22, "SetFocus"}, {23, "GetFocus"},
    {28, "ClientToScreen"}, {29, "ScreenToClient"}, {31, "IsIconic"}, {32, "GetWindowRect"},
    {36, "GetWindowText"}, {37, "SetWindowText"}, {38, "GetWindowTextLength"}, {39, "BeginPaint"},
    {40, "EndPaint"}, {41, "CreateWindow"}, {42, "ShowWindow"}, {46, "GetParent"}, {47, "IsWindow"},
    {53, "DestroyWindow"}, {57, "RegisterClass"}, {66, "GetDC"}, {68, "ReleaseDC"}, {69, "SetCursor"},
    {71, "ShowCursor"}, {72, "SetRect"}, {75, "IsRectEmpty"}, {77, "OffsetRect"}, {80, "UnionRect"},
    {81, "FillRect"}, {82, "InvertRect"}, {87, "DialogBox"}, {88, "EndDialog"}, {91, "GetDlgItem"},
    {92, "SetDlgItemText"}, {102, "AdjustWindowRect"}, {104, "MessageBeep"}, {106, "GetKeyState"},
    {107, "DefWindowProc"}, {108, "GetMessage"}, {109, "PeekMessage"}, {110, "PostMessage"},
    {111, "SendMessage"}, {113, "TranslateMessage"}, {114, "DispatchMessage"}, {116, "PostAppMessage"},
    {122, "CallWindowProc"}, {124, "UpdateWindow"}, {125, "InvalidateRect"}, {127, "ValidateRect"},
    {129, "GetClassWord"}, {133, "GetWindowWord"}, {134, "SetWindowWord"}, {135, "GetWindowLong"},
    {136, "SetWindowLong"}, {137, "OpenClipboard"}, {138, "CloseClipboard"}, {139, "EmptyClipboard"},
    {140, "GetClipboardOwner"}, {141, "SetClipboardData"}, {142, "GetClipboardData"},
    {144, "EnumClipboardFormats"}, {145, "RegisterClipboardFormat"}, {155, "EnableMenuItem"},
    {156, "GetSystemMenu"}, {169, "GetCaretBlinkTime"}, {173, "LoadCursor"}, {174, "LoadIcon"},
    {176, "LoadString"}, {177, "LoadAccelerators"}, {178, "TranslateAccelerator"}, {179, "GetSystemMetrics"},
    {190, "GetUpdateRect"}, {221, "ScrollDC"}, {232, "SetWindowPos"}, {236, "GetCapture"},
    {243, "GetDialogBaseUnits"}, {249, "GetAsyncKeyState"}, {282, "SelectPalette"}, {283, "RealizePalette"},
    {404, "GetClassInfo"}, {413, "DeleteMenu"}, {414, "ModifyMenu"}, {420, "_wsprintf"}, {430, "lstrcmp"},
    {431, "AnsiUpper"}, {435, "IsCharUpper"}, {436, "IsCharLower"}, {437, "AnsiUpperBuff"},
    {438, "AnsiLowerBuff"}, {457, "DestroyIcon"}, {458, "DestroyCursor"}, {471, "lstrcmpi"},
};

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20) && *a != *b) return 0;
    return *a == *b;
}

/* The 97 USER ordinals WRITER.EXE imports pair with the 97 names of the program's import
 * table (tests/win16_imports.h): same set. That cannot detect two names swapped
 * inside the set; ordering constraints from the Win 3.1 export list and the
 * arguments seen at call sites back it up. */
static void test_ordinal_table(void) {
    size_t i, j, n = sizeof verified / sizeof verified[0];
    size_t tsv = 0, found = 0;
    gdi_t *gdi;
    printf("test_ordinal_table\n");
    uboot();
    gdi = gdi_create(&(gdi_config){0}, &uh, uu);     /* the drawing half of USER (FillRect...) is registered by GDI */
    CHECK(gdi != NULL);
    CHECK_EQ(n, 97);
    for (i = 0; i < n; i++) {
        const hle_entry *e = hle_find(&uh, "USER", (uint16_t)verified[i].ord);
        CHECK(e != NULL && e->fn != NULL);
        if (e) CHECK(ieq(e->name, verified[i].name));
        if (!e) printf("    missing USER.%u %s\n", verified[i].ord, verified[i].name);
    }
    for (i = 0; i < WIN16_IMPORT_COUNT; i++) {
        if (strcmp(win16_imports[i].module, "USER")) continue;
        tsv++;
        for (j = 0; j < n; j++)
            if (ieq(win16_imports[i].name, verified[j].name)) { found++; break; }
    }
    CHECK_EQ(tsv, 97);
    CHECK_EQ(found, 97);
    gdi_destroy(gdi);
    udown();
}

static void test_init_metrics(void) {
    static const struct { unsigned idx, v; } m[] = {
        {0, 640}, {1, 480}, {2, 16}, {3, 16}, {4, 20}, {5, 1}, {6, 1}, {7, 4}, {8, 4}, {11, 32}, {12, 32},
        {13, 32}, {14, 32}, {15, 19}, {16, 640}, {19, 1}, {20, 16}, {21, 16}, {22, 0}, {23, 0}, {32, 4},
        {33, 4}, {36, 4}, {37, 4}, {43, 3}, {99, 0}};
    size_t i;
    printf("test_user_init_metrics\n");
    uboot();
    CHECK_EQ(UCALL(5, 0x1234), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    for (i = 0; i < sizeof m / sizeof m[0]; i++) {
        CHECK_EQ(UCALL(179, (uint16_t)m[i].idx), LOADER_STOP_HLT);
        CHECK_EQ(fx_ax(&uf), m[i].v);
    }
    CHECK_EQ(UCALL(179, 17), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 480 - 20);                      /* SM_CYFULLSCREEN: without the caption */
    CHECK_EQ(UCALL(13), LOADER_STOP_HLT);                /* GetTickCount follows the injected clock */
    CHECK_EQ(fx_dxax(&uf), 0);                           /* milliseconds since start-up */
    uclock = 1000 + 42;
    CHECK_EQ(UCALL(13), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), 42);
    CHECK_EQ(user_now(uu), 42);
    CHECK_EQ(UCALL(15), LOADER_STOP_HLT);                /* GetCurrentTime is the same clock */
    CHECK_EQ(fx_dxax(&uf), 42);
    udown();
}

/* A program that waits on the clock without ever calling PeekMessage (Creative Writer's splash screen is a
 * timeGetTime delay loop) must still get its screen shown: GetTickCount lets the host refresh the display, at most
 * once per 16 ms of program time. Before this the splash was drawn but only presented when the loop ended. */
static void test_clock_refresh(void) {
    int i;
    printf("test_user_clock_refresh\n");
    uboot();
    for (i = 0; i < 20; i++) {
        uclock += 5;                                     /* 100 ms in 5 ms steps */
        CHECK_EQ(UCALL(13), LOADER_STOP_HLT);
    }
    CHECK(urefresh_calls >= 5 && urefresh_calls <= 7);
    CHECK_EQ(upump_calls, 0);                            /* the message pump (and the frame counting) is not involved */
    udown();
}

static void test_register_class(void) {
    uint16_t atom, cs, ns, out;
    printf("test_user_register_class\n");
    uboot();
    atom = u_register("MyApp", u_far(U_CODE_A), 0x0008, 4, 8, 0x77, 6);
    CHECK(atom != 0);
    CHECK_EQ(u_register("MyApp", u_far(U_CODE_A), 0, 0, 0, 0x77, 6), 0);   /* duplicate name + instance */
    CHECK(u_register("MyApp", u_far(U_CODE_A), 0, 0, 0, 0x78, 6) != 0);    /* another instance */
    CHECK_EQ(uf.sp_after, uf.sp_before);
    /* GetClassInfo(hInst, name, &wndclass) */
    ns = ustr("myapp");
    out = useg(0x40);
    CHECK_EQ(UCALL(404, 0x77, FARP(ns, 0), FARP(out, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(urd16(out, 0), 0x0008);
    CHECK_EQ(urd32(out, 2), u_far(U_CODE_A));
    CHECK_EQ(urd16(out, 6), 4);
    CHECK_EQ(urd16(out, 8), 8);
    CHECK_EQ(urd16(out, 10), 0x77);
    CHECK_EQ(urd16(out, 12), 0x0301);
    CHECK_EQ(urd16(out, 14), 0x0302);
    CHECK_EQ(urd16(out, 16), 6);
    ns = ustr("nothing");
    CHECK_EQ(UCALL(404, 0x77, FARP(ns, 0), FARP(out, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 0);
    /* a window of the class: class words */
    {
        uint16_t h = u_create("MyApp", "x", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, 0x77);
        CHECK(h != 0);
        CHECK_EQ(UCALL(129, h, 0xFFF6), LOADER_STOP_HLT);        /* GCW_HBRBACKGROUND (-10) */
        CHECK_EQ(fx_ax(&uf), 6);
        CHECK_EQ(UCALL(129, h, 0xFFEE), LOADER_STOP_HLT);        /* GCW_CBWNDEXTRA (-18) */
        CHECK_EQ(fx_ax(&uf), 8);
        CHECK_EQ(UCALL(129, h, 0xFFE6), LOADER_STOP_HLT);        /* GCW_STYLE (-26) */
        CHECK_EQ(fx_ax(&uf), 8);
        CHECK_EQ(UCALL(130, h, 0xFFF2, 9), LOADER_STOP_HLT);     /* SetClassWord(GCW_HICON = -14) */
        CHECK_EQ(fx_ax(&uf), 0x0301);
        CHECK_EQ(UCALL(131, h, 0xFFE8), LOADER_STOP_HLT);        /* GetClassLong(GCL_WNDPROC = -24) */
        CHECK_EQ(fx_dxax(&uf), u_far(U_CODE_A));
        CHECK_EQ(UCALL(132, h, 0, LONG(0xABCD1234u)), LOADER_STOP_HLT);   /* SetClassLong(extra bytes) */
        CHECK_EQ(UCALL(131, h, 0), LOADER_STOP_HLT);
        CHECK_EQ(fx_dxax(&uf), 0xABCD1234u);
        /* a class in use cannot be unregistered */
        cs = ustr("MyApp");
        CHECK_EQ(UCALL(403, FARP(cs, 0), 0x77), LOADER_STOP_HLT);
        CHECK_EQ(fx_ax(&uf), 0);
        UCALL(53, h);
        CHECK_EQ(UCALL(403, FARP(cs, 0), 0x77), LOADER_STOP_HLT);
        CHECK_EQ(fx_ax(&uf), 1);
        CHECK_EQ(u_register("MyApp", 0, 0, 0, 0, 0x77, 6) != 0, 1);   /* free again */
    }
    udown();
}

static void test_create_window_guest(void) {
    uint16_t h, ds, cs;
    uint32_t lp;
    printf("test_user_create_window_guest\n");
    uboot();
    ds = u_ds();
    u_register("GApp", u_far(U_CODE_A), 0, 0, 4, 0x55, COLOR_WINDOW + 1);
    ulog_reset();
    uf.p->cpu.r[CPU_ESI] = 0x1357;
    uf.p->cpu.r[CPU_EDI] = 0x2468;
    h = u_create("GApp", "Hello", WS_OVERLAPPEDWINDOW, 10, 20, 300, 200, 0, 0, 0x55);
    CHECK(h != 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);                 /* Pascal cleanup intact */
    CHECK_EQ(uf.p->cpu.r[CPU_ESI] & 0xFFFF, 0x1357);     /* the callback clobbered SI/DI; the API call must not */
    CHECK_EQ(uf.p->cpu.r[CPU_EDI] & 0xFFFF, 0x2468);
    CHECK_EQ(uf.p->cpu.seg[CPU_DS], ds);
    CHECK_EQ(ulog_n(), 5);                               /* NCCREATE, NCCALCSIZE, CREATE, SIZE, MOVE */
    CHECK_EQ(ulog_w(0, ULOG_MSG), WM_NCCREATE);
    CHECK_EQ(ulog_w(1, ULOG_MSG), WM_NCCALCSIZE);
    CHECK_EQ(ulog_w(2, ULOG_MSG), WM_CREATE);
    CHECK_EQ(ulog_w(3, ULOG_MSG), WM_SIZE);
    CHECK_EQ(ulog_w(4, ULOG_MSG), WM_MOVE);
    CHECK_EQ(ulog_w(0, ULOG_HWND), h);
    /* the window procedure ran with DS = AX = the program's auto data segment */
    CHECK_EQ(urd16(ds, 0x30), ds);
    CHECK_EQ(urd16(ds, 0x32), ds);
    /* lParam is a CREATESTRUCT in guest memory */
    lp = ulog_lp(0);
    CHECK(lp != 0);
    cs = (uint16_t)(lp >> 16);
    CHECK_EQ(urd32(cs, (uint16_t)lp + 0), 0);            /* lpCreateParams */
    CHECK_EQ(urd16(cs, (uint16_t)lp + 4), 0x55);         /* hInstance */
    CHECK_EQ(urd16(cs, (uint16_t)lp + 10), 200);         /* cy */
    CHECK_EQ(urd16(cs, (uint16_t)lp + 12), 300);         /* cx */
    CHECK_EQ(urd16(cs, (uint16_t)lp + 14), 20);          /* y */
    CHECK_EQ(urd16(cs, (uint16_t)lp + 16), 10);          /* x */
    CHECK_EQ(urd32(cs, (uint16_t)lp + 18), WS_OVERLAPPEDWINDOW);
    {
        uint32_t name = urd32(cs, (uint16_t)lp + 22), cls = urd32(cs, (uint16_t)lp + 26);
        CHECK(!strcmp((const char *)umem((uint16_t)(name >> 16), (uint16_t)name, 1), "Hello"));
        CHECK(!strcmp((const char *)umem((uint16_t)(cls >> 16), (uint16_t)cls, 1), "GApp"));
    }
    /* SIZE gets the client size, MOVE the client origin */
    CHECK_EQ(ulog_lp(3) & 0xFFFF, 300 - 10);
    CHECK_EQ(ulog_lp(4), (uint32_t)(20 + 24) << 16 | (10 + 5));   /* MOVE: client origin */
    CHECK_EQ(ulog_lp(3) >> 16, 200 - 24 - 5);
    udown();
}

static void test_create_failures_api(void) {
    printf("test_user_create_failures\n");
    uboot();
    CHECK_EQ(u_create("NoSuchClass", "x", WS_OVERLAPPED, 0, 0, 10, 10, 0, 0, 1), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);                 /* arguments still popped on failure */
    u_register("C", u_far(U_CODE_A), 0, 0, 0, 1, 1);
    /* CreateWindow of a child with an invalid parent */
    CHECK_EQ(u_create("C", "x", WS_CHILD, 0, 0, 10, 10, 0x7777, 1, 1), 0);
    /* builtin classes are global */
    CHECK(u_create("BUTTON", "OK", WS_CHILD, 0, 0, 10, 10, u_create("C", "p", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, 1), 5, 1) != 0);
    udown();
}

static void test_create_window_ex(void) {
    uint16_t h, cs, ts;
    printf("test_user_create_window_ex\n");
    uboot();
    cs = ustr("C");
    ts = ustr("ex");
    u_register("C", 0, 0, 0, 0, 1, 1);
    CHECK_EQ(UCALL(452, LONG(0x00000001u), FARP(cs, 0), FARP(ts, 0), LONG(WS_OVERLAPPED), 1, 2, 30, 40,
                   0, 0, 1, FARP(0, 0)), LOADER_STOP_HLT);
    h = fx_ax(&uf);
    CHECK(h != 0);
    CHECK_EQ(wm_get(user_wm(uu), h)->exstyle, 1);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    udown();
}

static void test_window_text(void) {
    uint16_t h, buf, s;
    printf("test_user_window_text\n");
    uboot();
    buf = useg(0x100);
    u_register("D", 0, 0, 0, 0, 1, 1);
    u_register("G", u_far(U_CODE_A), 0, 0, 0, 1, 1);
    /* default procedure: the text is held by the window manager */
    h = u_create("D", "Title", WS_OVERLAPPED, 0, 0, 10, 10, 0, 0, 1);
    CHECK_EQ(UCALL(38, h), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 5);
    CHECK_EQ(UCALL(36, h, FARP(buf, 0), 100), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 5);
    CHECK(!strcmp((const char *)umem(buf, 0, 1), "Title"));
    CHECK_EQ(UCALL(36, h, FARP(buf, 0), 4), LOADER_STOP_HLT);        /* truncated, NUL terminated */
    CHECK_EQ(fx_ax(&uf), 3);
    CHECK(!strcmp((const char *)umem(buf, 0, 1), "Tit"));
    s = ustr("Renamed");
    CHECK_EQ(UCALL(37, h, FARP(s, 0)), LOADER_STOP_HLT);
    CHECK_EQ(UCALL(38, h), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 7);
    CHECK_EQ(UCALL(36, h, FARP(buf, 0), 100), LOADER_STOP_HLT);
    CHECK(!strcmp((const char *)umem(buf, 0, 1), "Renamed"));
    CHECK_EQ(UCALL(36, 0x7777, FARP(buf, 0), 100), LOADER_STOP_HLT); /* bad window */
    CHECK_EQ(fx_ax(&uf), 0);
    /* with a guest procedure SetWindowText really sends WM_SETTEXT (the recorder does not call
     * DefWindowProc, so the text stays) */
    h = u_create("G", "keep", WS_OVERLAPPED, 0, 0, 10, 10, 0, 0, 1);
    ulog_reset();
    UCALL(37, h, FARP(s, 0));
    CHECK_EQ(ulog_n(), 1);
    CHECK_EQ(ulog_w(0, ULOG_MSG), 0x000C);
    CHECK_EQ(ulog_lp(0), (uint32_t)s << 16);
    UCALL(38, h);
    CHECK_EQ(ulog_w(1, ULOG_MSG), 0x000E);
    udown();
}

static void test_geometry_api(void) {
    uint16_t h, ch, r, pt;
    printf("test_user_geometry\n");
    uboot();
    r = useg(0x20);
    pt = useg(0x10);
    u_register("D", 0, 0, 0, 0, 1, 1);
    h = u_create("D", "w", WS_OVERLAPPED | WS_CAPTION, 100, 50, 300, 200, 0, 0, 1);
    CHECK_EQ(UCALL(32, h, FARP(r, 0)), LOADER_STOP_HLT);
    CHECK(urd16(r, 0) == 100 && urd16(r, 2) == 50 && urd16(r, 4) == 400 && urd16(r, 6) == 250);
    CHECK_EQ(UCALL(33, h, FARP(r, 0)), LOADER_STOP_HLT);
    CHECK(urd16(r, 0) == 0 && urd16(r, 2) == 0 && urd16(r, 4) == 298 && urd16(r, 6) == 179);
    ch = u_create("D", "c", WS_CHILD, 10, 20, 100, 50, h, 3, 1);
    CHECK_EQ(UCALL(32, ch, FARP(r, 0)), LOADER_STOP_HLT);              /* screen coordinates */
    CHECK(urd16(r, 0) == 111 && urd16(r, 2) == 90 && urd16(r, 4) == 211 && urd16(r, 6) == 140);
    uwr16(pt, 0, 5); uwr16(pt, 2, 6);
    CHECK_EQ(UCALL(28, ch, FARP(pt, 0)), LOADER_STOP_HLT);
    CHECK(urd16(pt, 0) == 116 && urd16(pt, 2) == 96);
    CHECK_EQ(UCALL(29, ch, FARP(pt, 0)), LOADER_STOP_HLT);
    CHECK(urd16(pt, 0) == 5 && urd16(pt, 2) == 6);
    uwr16(pt, 0, 0); uwr16(pt, 2, 0);
    UCALL(29, ch, FARP(pt, 0));
    CHECK(urd16(pt, 0) == (uint16_t)-111 && urd16(pt, 2) == (uint16_t)-90);
    /* MoveWindow(h, x, y, cx, cy, repaint) */
    CHECK_EQ(UCALL(56, h, 5, 6, 200, 100, 1), LOADER_STOP_HLT);
    UCALL(32, h, FARP(r, 0));
    CHECK(urd16(r, 0) == 5 && urd16(r, 2) == 6 && urd16(r, 4) == 205 && urd16(r, 6) == 106);
    /* SetWindowPos(h, after, x, y, cx, cy, flags): move only */
    CHECK_EQ(UCALL(232, h, 0, 40, 41, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE), LOADER_STOP_HLT);
    UCALL(32, h, FARP(r, 0));
    CHECK(urd16(r, 0) == 40 && urd16(r, 2) == 41 && urd16(r, 4) == 240 && urd16(r, 6) == 141);
    CHECK_EQ(UCALL(32, 0x7777, FARP(r, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    udown();
}

static void test_words_longs_api(void) {
    uint16_t h;
    printf("test_user_window_words\n");
    uboot();
    u_register("W", 0, 0, 0, 8, 0x66, 1);
    h = u_create("W", "w", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, 0x66);
    CHECK_EQ(UCALL(136, h, 0, LONG(0x12345678u)), LOADER_STOP_HLT);   /* SetWindowLong, extra bytes */
    CHECK_EQ(fx_dxax(&uf), 0);
    CHECK_EQ(UCALL(135, h, 0), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), 0x12345678u);
    CHECK_EQ(UCALL(133, h, 2), LOADER_STOP_HLT);                      /* GetWindowWord */
    CHECK_EQ(fx_ax(&uf), 0x1234);
    CHECK_EQ(UCALL(134, h, 6, 0xBEEF), LOADER_STOP_HLT);
    CHECK_EQ(UCALL(133, h, 6), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 0xBEEF);
    CHECK_EQ(UCALL(133, h, 100), LOADER_STOP_HLT);                    /* out of range */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(UCALL(133, h, 0xFFFA), LOADER_STOP_HLT);                 /* GWW_HINSTANCE (-6) */
    CHECK_EQ(fx_ax(&uf), 0x66);
    CHECK_EQ(UCALL(135, h, 0xFFF0), LOADER_STOP_HLT);                 /* GWL_STYLE (-16) */
    CHECK_EQ(fx_dxax(&uf), WS_OVERLAPPED);
    CHECK_EQ(UCALL(134, h, 0xFFF4, 99), LOADER_STOP_HLT);             /* GWW_ID (-12) */
    CHECK_EQ(UCALL(133, h, 0xFFF4), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 99);
    /* GWL_WNDPROC: the default procedure is reported as a callable address */
    CHECK_EQ(UCALL(135, h, 0xFFFC), LOADER_STOP_HLT);
    CHECK(fx_dxax(&uf) != 0);
    CHECK_EQ(UCALL(136, h, 0xFFFC, LONG(u_far(U_CODE_A))), LOADER_STOP_HLT);   /* subclass */
    CHECK_EQ(UCALL(135, h, 0xFFFC), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), u_far(U_CODE_A));
    udown();
}

static void test_window_predicates(void) {
    uint16_t a, b, c, d;
    printf("test_user_window_predicates\n");
    uboot();
    u_register("W", 0, 0, 0, 0, 1, 1);
    a = u_create("W", "a", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0, 0, 1);
    b = u_create("W", "b", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, a, 1, 1);
    c = u_create("W", "c", WS_POPUP, 0, 0, 10, 10, a, 0, 1);
    UCALL(47, a); CHECK_EQ(fx_ax(&uf), 1);
    UCALL(47, 0x7777); CHECK_EQ(fx_ax(&uf), 0);
    UCALL(47, 0); CHECK_EQ(fx_ax(&uf), 0);
    UCALL(49, a); CHECK_EQ(fx_ax(&uf), 0);                  /* IsWindowVisible */
    UCALL(42, a, SW_SHOWNORMAL);
    UCALL(49, a); CHECK_EQ(fx_ax(&uf), 1);
    UCALL(49, b); CHECK_EQ(fx_ax(&uf), 1);
    UCALL(31, a); CHECK_EQ(fx_ax(&uf), 0);                  /* IsIconic */
    UCALL(42, a, SW_SHOWMINIMIZED);
    UCALL(31, a); CHECK_EQ(fx_ax(&uf), 1);
    UCALL(42, a, SW_SHOWMAXIMIZED);
    UCALL(31, a); CHECK_EQ(fx_ax(&uf), 0);
    UCALL(272, a); CHECK_EQ(fx_ax(&uf), 1);                 /* IsZoomed */
    UCALL(42, a, SW_RESTORE);
    UCALL(272, a); CHECK_EQ(fx_ax(&uf), 0);
    UCALL(46, b); CHECK_EQ(fx_ax(&uf), a);                  /* GetParent */
    UCALL(46, a); CHECK_EQ(fx_ax(&uf), 0);
    UCALL(46, c); CHECK_EQ(fx_ax(&uf), a);                  /* owner of a popup */
    UCALL(48, a, b); CHECK_EQ(fx_ax(&uf), 1);               /* IsChild(parent, child) */
    UCALL(48, b, a); CHECK_EQ(fx_ax(&uf), 0);
    UCALL(262, a, GW_CHILD); CHECK_EQ(fx_ax(&uf), b);       /* GetWindow */
    UCALL(262, c, GW_OWNER); CHECK_EQ(fx_ax(&uf), a);
    UCALL(286); d = fx_ax(&uf);                             /* GetDesktopWindow */
    CHECK(d != 0 && d == wm_get(user_wm(uu), a)->parent);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    UCALL(224, a); CHECK(fx_ax(&uf) != 0);                  /* GetWindowTask: the one task owns every window */
    CHECK_EQ(uf.sp_after, uf.sp_before);
    UCALL(224, 0x7777); CHECK_EQ(fx_ax(&uf), 0);            /* not a window */
    UCALL(34, a, 0); CHECK_EQ(fx_ax(&uf), 0);               /* EnableWindow(FALSE): was enabled */
    UCALL(35, a); CHECK_EQ(fx_ax(&uf), 0);
    UCALL(34, a, 1); CHECK_EQ(fx_ax(&uf), 1);               /* was disabled */
    UCALL(35, a); CHECK_EQ(fx_ax(&uf), 1);
    UCALL(233, b, 0);                                       /* SetParent(b, desktop) */
    CHECK_EQ(fx_ax(&uf), a);
    CHECK_EQ(wm_get(user_wm(uu), b)->parent, d);
    CHECK_EQ(UCALL(45, c), LOADER_STOP_HLT);                /* BringWindowToTop */
    CHECK_EQ(UCALL(105, a, 1), LOADER_STOP_HLT);            /* FlashWindow: no effect, returns FALSE */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(UCALL(53, a), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 1);
    UCALL(47, a); CHECK_EQ(fx_ax(&uf), 0);
    UCALL(53, a); CHECK_EQ(fx_ax(&uf), 0);
    udown();
}

static void test_adjust_window_rect(void) {
    uint16_t r;
    printf("test_user_adjust_window_rect\n");
    uboot();
    r = useg(0x10);
    uwr16(r, 0, 0); uwr16(r, 2, 0); uwr16(r, 4, 100); uwr16(r, 6, 80);
    CHECK_EQ(UCALL(102, FARP(r, 0), LONG(WS_OVERLAPPEDWINDOW), 0), LOADER_STOP_HLT);
    CHECK(urd16(r, 0) == (uint16_t)-5 && urd16(r, 2) == (uint16_t)-24 && urd16(r, 4) == 105 && urd16(r, 6) == 85);
    uwr16(r, 0, 0); uwr16(r, 2, 0); uwr16(r, 4, 100); uwr16(r, 6, 80);
    CHECK_EQ(UCALL(102, FARP(r, 0), LONG(WS_OVERLAPPEDWINDOW), 1), LOADER_STOP_HLT);   /* with a menu bar */
    CHECK(urd16(r, 2) == (uint16_t)-44);
    uwr16(r, 0, 0); uwr16(r, 2, 0); uwr16(r, 4, 100); uwr16(r, 6, 80);
    CHECK_EQ(UCALL(102, FARP(r, 0), LONG(WS_CHILD), 0), LOADER_STOP_HLT);
    CHECK(urd16(r, 0) == 0 && urd16(r, 6) == 80);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    udown();
}

static void test_send_message_reentrancy(void) {
    uint16_t a, b, ds;
    printf("test_user_send_message_reentrancy\n");
    uboot();
    ds = u_ds();
    u_register("A", u_far(U_CODE_A), 0, 0, 0, 1, 1);
    u_register("B", u_far(U_CODE_B), 0, 0, 0, 1, 1);
    a = u_create("A", "a", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, 1);
    b = u_create("B", "b", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, 1);
    CHECK(a && b);
    uwr16(ds, 0x50, a);                                     /* B forwards to A */
    ulog_reset();
    uf.p->cpu.r[CPU_ESI] = 0x1111;
    uf.p->cpu.r[CPU_EBP] = 0x3333;
    CHECK_EQ(UCALL(111, b, WM_USER, 7, LONG(0x00010000u)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), 2);                                /* A returned lParam + 1 = 1, B added 1 */
    CHECK_EQ(fx_dxax(&uf), 2);
    CHECK_EQ(ulog_n(), 1);                                  /* only A records */
    CHECK_EQ(ulog_w(0, ULOG_HWND), a);
    CHECK_EQ(ulog_w(0, ULOG_MSG), WM_USER + 1);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    CHECK_EQ(uf.p->cpu.r[CPU_ESI] & 0xFFFF, 0x1111);
    CHECK_EQ(uf.p->cpu.r[CPU_EBP] & 0xFFFF, 0x3333);
    /* other messages take B's other branch */
    CHECK_EQ(UCALL(111, b, WM_USER + 5, 0, LONG(0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), 7);
    /* the result is a full LONG */
    ulog_reset();
    CHECK_EQ(UCALL(111, a, WM_USER, 1, LONG(0x0001FFFFu)), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), 0x00020000u);
    CHECK_EQ(ulog_w(0, ULOG_WP), 1);
    /* sending to a bad window returns 0 without calling anything */
    ulog_reset();
    CHECK_EQ(UCALL(111, 0x7777, WM_USER, 0, LONG(0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), 0);
    CHECK_EQ(ulog_n(), 0);
    udown();
}

static void test_def_and_call_window_proc(void) {
    uint16_t h, d, ds;
    printf("test_user_defwindowproc_callwindowproc\n");
    uboot();
    ds = u_ds();
    u_register("W", 0, 0, 0, 0, 1, COLOR_WINDOW + 1);
    h = u_create("W", "t", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, 1);
    CHECK_EQ(UCALL(107, h, WM_NCCREATE, 0, LONG(0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), 1);
    CHECK_EQ(UCALL(107, h, WM_ERASEBKGND, 0, LONG(0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), 0);
    CHECK_EQ(UCALL(107, h, WM_NCHITTEST, 0, LONG(100 << 16 | 100)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&uf), HTNOWHERE);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    /* CallWindowProc(proc, hwnd, msg, wp, lp): proc 0 = default procedure; a guest proc is called */
    CHECK_EQ(UCALL(122, LONG(0), h, WM_NCCREATE, 0, LONG(0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), 1);
    ulog_reset();
    CHECK_EQ(UCALL(122, LONG(u_far(U_CODE_A)), h, WM_USER, 3, LONG(0x100)), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), 0x101);
    CHECK_EQ(ulog_n(), 1);
    CHECK_EQ(urd16(ds, 0x30), ds);                          /* DS fixed up for CallWindowProc too */
    /* subclassing: D forwards what it receives to the original procedure at DS:[54h] */
    u_register("S", u_far(U_CODE_D), 0, 0, 0, 1, 1);
    uwr32(ds, 0x54, u_far(U_CODE_A));
    d = u_create("S", "s", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, 1);
    ulog_reset();
    CHECK_EQ(UCALL(111, d, WM_USER + 2, 9, LONG(0x40)), LOADER_STOP_HLT);
    CHECK_EQ(fx_dxax(&uf), 0x41);
    CHECK_EQ(ulog_n(), 1);
    CHECK_EQ(ulog_w(0, ULOG_HWND), d);
    CHECK_EQ(ulog_w(0, ULOG_WP), 9);
    udown();
}

static void test_callback_fault_and_stop(void) {
    uint16_t h;
    printf("test_user_callback_fault\n");
    uboot();
    u_register("F", u_far(U_CODE_C), 0, 0, 0, 1, 1);
    u_register("N", 0, 0, 0, 0, 1, 1);
    h = u_create("N", "n", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, 1);
    /* switch the window to the faulting procedure and send it a message */
    CHECK_EQ(UCALL(136, h, 0xFFFC, LONG(u_far(U_CODE_C))), LOADER_STOP_HLT);
    CHECK_EQ(UCALL(111, h, WM_USER, 0, LONG(0)), LOADER_STOP_FAULT);   /* not papered over */
    CHECK_EQ(uf.p->fault, CPU_E_UD);
    CHECK_EQ(uf.p->cpu.seg[CPU_CS], u_cs());                 /* the CPU stays at the faulting instruction */
    CHECK_EQ(uf.p->cpu.eip, U_CODE_C);
    udown();
    /* an unimplemented API called from inside a callback stops the whole run */
    uboot();
    {
        loader_thunk *t = fx_thunk(&uf, "USER", 9999);      /* not registered: logging stub that stops */
        uint8_t p[] = {0x55, 0x89, 0xE5, 0x9A, 0, 0, 0, 0, 0x5D, 0xCA, 0x0A, 0x00};
        p[4] = (uint8_t)t->off; p[5] = (uint8_t)(t->off >> 8); p[6] = (uint8_t)t->sel; p[7] = (uint8_t)(t->sel >> 8);
        u_put(0x800, p, sizeof p);
        u_register("X", u_far(0x800), 0, 0, 0, 1, 1);
        h = u_create("X", "x", WS_OVERLAPPED, 0, 0, 50, 50, 0, 0, 1);
        CHECK_EQ(h, 0);                                      /* creation stops inside WM_NCCREATE */
        CHECK_EQ(uf.stop, LOADER_STOP_REQUESTED);
        CHECK_EQ(uh.unimpl_calls, 1);
    }
    udown();
}

/* user_config.log_messages: every message handed to a guest window procedure is logged (cwfa --trace-msgs). */
static void test_message_log(void) {
    user_config cfg;
    FILE *f = tmpfile();
    char line[256];
    bool created = false, sized = false;
    printf("test_user_message_log\n");
    memset(&cfg, 0, sizeof cfg);
    cfg.log = f;
    cfg.log_messages = true;
    uboot_cfg(&cfg);
    u_register("GApp", u_far(U_CODE_A), 0, 0, 4, 0x55, COLOR_WINDOW + 1);
    CHECK(u_create("GApp", "Hello", WS_OVERLAPPEDWINDOW, 10, 20, 300, 200, 0, 0, 0x55) != 0);
    rewind(f);
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "msg: hwnd=") && strstr(line, "msg=0001")) created = true;     /* WM_CREATE */
        if (strstr(line, "msg: hwnd=") && strstr(line, "msg=0005")) sized = true;       /* WM_SIZE */
    }
    CHECK(created && sized);
    udown();
    fclose(f);
    uboot();                                                  /* without the flag nothing is logged */
    udown();
}

/* USER.483 is SystemParametersInfo(uAction, uParam, lpvParam, fuWinIni): Fine Artist reads the screen saver flag
 * (SPI_GETSCREENSAVEACTIVE = 16, 0x10) into a word and clears it (SPI_SETSCREENSAVEACTIVE = 17, 0x11, uParam 0). */
static void test_system_parameters_info(void) {
    uint16_t buf;
    printf("test_system_parameters_info\n");
    uboot();
    buf = useg(0x40);
    memset(umem(buf, 0, 16), 0xEE, 16);
    UCALL(483, 16, 0, buf, 0, 0); CHECK_EQ(fx_ax(&uf), 1);       /* GETSCREENSAVEACTIVE: on by default */
    CHECK_EQ(uf.sp_after, uf.sp_before);
    CHECK_EQ(umem(buf, 0, 2)[0] | umem(buf, 0, 2)[1] << 8, 1);
    UCALL(483, 17, 0, 0, 0, 0); CHECK_EQ(fx_ax(&uf), 1);          /* SETSCREENSAVEACTIVE(FALSE) */
    UCALL(483, 16, 0, buf, 0, 0); CHECK_EQ(umem(buf, 0, 2)[0], 0);
    UCALL(483, 17, 1, 0, 0, 0);
    UCALL(483, 16, 0, buf, 0, 0); CHECK_EQ(umem(buf, 0, 2)[0], 1);
    UCALL(483, 1, 0, buf, 0, 0); CHECK_EQ(umem(buf, 0, 2)[0], 1); /* GETBEEP */
    UCALL(483, 10, 0, buf, 0, 0); CHECK_EQ(umem(buf, 0, 2)[0], 31); /* GETKEYBOARDSPEED */
    memset(umem(buf, 0, 16), 0xEE, 16);
    UCALL(483, 14, 0, buf, 0, 0); CHECK_EQ(umem(buf, 0, 2)[0] | umem(buf, 0, 2)[1], 0);   /* GETSCREENSAVETIMEOUT: 0 */
    memset(umem(buf, 0, 16), 0xEE, 16);
    UCALL(483, 18, 0, buf, 0, 0); CHECK_EQ(umem(buf, 0, 2)[0] | umem(buf, 0, 2)[1], 0);   /* GETGRIDGRANULARITY: 0 */
    UCALL(483, 5, 0, buf, 0, 0); CHECK_EQ(umem(buf, 0, 2)[0], 1);                         /* GETBORDER */
    UCALL(483, 3, 0, buf, 0, 0); CHECK_EQ(fx_ax(&uf), 1);                                  /* GETMOUSE: threshold 0, 0, accel 1 */
    CHECK(umem(buf, 0, 6)[0] == 0 && umem(buf, 0, 6)[1] == 0 && umem(buf, 0, 6)[2] == 0 && umem(buf, 0, 6)[3] == 0);
    CHECK(umem(buf, 0, 6)[4] == 1 && umem(buf, 0, 6)[5] == 0);
    UCALL(483, 4, 0, 0, 0, 0); CHECK_EQ(fx_ax(&uf), 1);           /* SET*: accepted without effect, no pointer read */
    UCALL(483, 11, 5, 0, 0, 0); CHECK_EQ(fx_ax(&uf), 1);
    UCALL(483, 15, 5, 0, 0, 0); CHECK_EQ(fx_ax(&uf), 1);
    UCALL(483, 19, 5, 0, 0, 0); CHECK_EQ(fx_ax(&uf), 1);
    UCALL(483, 999, 0, buf, 0, 0); CHECK_EQ(fx_ax(&uf), 0);       /* unknown action: FALSE */
    udown();
}

int main(void) {
    test_ordinal_table();
    test_init_metrics();
    test_register_class();
    test_create_window_guest();
    test_create_failures_api();
    test_create_window_ex();
    test_window_text();
    test_geometry_api();
    test_words_longs_api();
    test_clock_refresh();
    test_window_predicates();
    test_system_parameters_info();
    test_adjust_window_rect();
    test_send_message_reentrancy();
    test_def_and_call_window_proc();
    test_callback_fault_and_stop();
    test_message_log();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
