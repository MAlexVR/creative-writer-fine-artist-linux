/* The virtual PDF printer: CreateDC/CreateIC of "PDF Printer", its device capabilities, the printer escapes and the
 * StartDoc/StartPage/EndPage/EndDoc calls, the printer driver entry points a program loads (ExtDeviceMode,
 * DeviceCapabilities) and the PDF file each document ends up in. */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gfixture.h"
#include "ttf.h"

enum {
    O_ESCAPE = 38, O_TEXTOUT = 33, O_RECTANGLE = 27, O_CREATEDC = 53, O_DELETEDC = 68, O_GETDEVICECAPS = 80,
    O_CREATEIC = 153, O_STARTDOC = 377, O_ENDDOC = 378, O_STARTPAGE = 379, O_ENDPAGE = 380, O_SETABORTPROC = 381,
    O_ABORTDOC = 382, O_SETTEXTCOLOR = 9, O_CREATEFONT = 56, O_SELECTOBJECT = 45
};
enum { E_NEWFRAME = 1, E_ABORTDOC = 2, E_QUERY = 8, E_SETABORT = 9, E_STARTDOC = 10, E_ENDDOC = 11, E_PHYSPAGE = 12, E_OFFSET = 13 };

static char g_dir[64];

static uint16_t put_str(const char *s) {
    uint16_t seg = g_seg(0x200);
    strcpy((char *)umem(seg, 0, 1), s);
    return seg;
}

static uint16_t printer_dc(void) {
    uint16_t d = put_str("CWFAPRN"), n = put_str("PDF Printer"), p = put_str("FILE:");
    return GA(O_CREATEDC, FARP(d, 0), FARP(n, 0), FARP(p, 0), FARP(0, 0));
}

static uint16_t escape(uint16_t dc, int code, int cb, uint16_t in_seg, uint16_t out_seg) {
    return GA(O_ESCAPE, dc, (uint16_t)code, (uint16_t)cb, FARP(in_seg, 0), FARP(out_seg, 0));
}

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    char *b;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)len + 1);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) {
        b[len] = 0;
        *n = (size_t)len;
    }
    return b;
}

static bool has(const char *buf, size_t n, const char *w) {
    size_t i, m = strlen(w);
    for (i = 0; i + m <= n; i++)
        if (!memcmp(buf + i, w, m)) return true;
    return false;
}

static void boot(void) {
    gdi_config cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(g_dir, sizeof g_dir, "build/prn_%d", (int)getpid());
    mkdir(g_dir, 0777);
    cfg.print_dir = g_dir;
    gboot_cfg(&cfg);
}

static void cleanup(void) {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -r %s", g_dir);
    if (system(cmd)) printf("  (cleanup failed)\n");
}

static void test_dc_and_caps(void) {
    uint16_t dc, ic, s;
    printf("test_gdi_print_dc\n");
    boot();
    s = g_seg(0x100);
    CHECK(GA(O_CREATEDC, FARP(put_str("DISPLAY"), 0), FARP(0, 0), FARP(0, 0), FARP(0, 0)) != 0);   /* the screen still works */
    CHECK_EQ(GA(O_CREATEDC, FARP(put_str("HPPCL5"), 0), FARP(put_str("LaserJet"), 0), FARP(put_str("LPT1:"), 0), FARP(0, 0)), 0);
    dc = printer_dc();
    CHECK(dc != 0);
    CHECK_EQ(GA(O_GETDEVICECAPS, dc, 2), 2);                      /* DT_RASPRINTER */
    CHECK_EQ(GA(O_GETDEVICECAPS, dc, 8), 2550);                   /* HORZRES: 8.5 in at 300 dpi */
    CHECK_EQ(GA(O_GETDEVICECAPS, dc, 10), 3300);                  /* VERTRES */
    CHECK_EQ(GA(O_GETDEVICECAPS, dc, 88), 300);                   /* LOGPIXELSX */
    CHECK_EQ(GA(O_GETDEVICECAPS, dc, 90), 300);
    CHECK_EQ(GA(O_GETDEVICECAPS, dc, 4), 216);                    /* HORZSIZE mm */
    CHECK_EQ(GA(O_GETDEVICECAPS, dc, 6), 279);
    CHECK_EQ(GA(O_GETDEVICECAPS, dc, 12), 8);                     /* BITSPIXEL */
    CHECK_EQ(GA(O_GETDEVICECAPS, dc, 38) & 2, 0);                 /* no RC_BANDING: programs print in one piece */
    CHECK(GA(O_GETDEVICECAPS, dc, 38) & 1);                       /* RC_BITBLT */
    CHECK_EQ(GA(O_GETDEVICECAPS, dc, 38) & 0x100, 0);             /* not a palette device */
    /* the information context answers the same without a page */
    ic = GA(O_CREATEIC, FARP(put_str("CWFAPRN"), 0), FARP(put_str("PDF Printer"), 0), FARP(put_str("FILE:"), 0), FARP(0, 0));
    CHECK(ic != 0 && ic != dc);
    CHECK_EQ(GA(O_GETDEVICECAPS, ic, 8), 2550);
    CHECK_EQ(GA(O_DELETEDC, ic), 1);
    /* the display keeps its own */
    CHECK_EQ(GA(O_GETDEVICECAPS, g_screen_dc(), 8), 640);
    /* a printer escape on the screen DC is still refused */
    CHECK_EQ(escape(g_screen_dc(), E_STARTDOC, 4, put_str("x"), 0), 0);
    CHECK_EQ(GA(O_DELETEDC, dc), 1);
    CHECK_EQ(GA(O_GETDEVICECAPS, dc, 8), 0);                      /* gone */
    (void)s;
    gdown();
    cleanup();
}

static void test_escape_document(void) {
    uint16_t dc, s, f;
    size_t n = 0;
    char path[600], *buf;
    printf("test_gdi_print_escape\n");
    boot();
    dc = printer_dc();
    s = g_seg(0x100);
    /* QUERYESCSUPPORT */
    {
        uint16_t codes[] = {E_NEWFRAME, E_ABORTDOC, E_QUERY, E_SETABORT, E_STARTDOC, E_ENDDOC, E_PHYSPAGE, E_OFFSET, 99, 500};
        int want[] = {1, 1, 1, 1, 1, 1, 1, 1, 0, 0}, i;
        for (i = 0; i < 10; i++) {
            uint8_t *q = umem(s, 0, 2);
            q[0] = (uint8_t)codes[i];
            q[1] = (uint8_t)(codes[i] >> 8);
            CHECK_EQ(escape(dc, E_QUERY, 2, s, 0), want[i]);
        }
    }
    CHECK_EQ(escape(dc, E_NEWFRAME, 0, 0, 0), 0xFFFF);            /* before STARTDOC: SP_ERROR */
    CHECK_EQ(escape(dc, E_STARTDOC, 11, put_str("Test doc/1"), 0), 1);
    CHECK_EQ(escape(dc, E_PHYSPAGE, 0, 0, s), 1);
    CHECK_EQ(urd16(s, 0), 2550);
    CHECK_EQ(urd16(s, 2), 3300);
    CHECK_EQ(escape(dc, E_OFFSET, 0, 0, s), 1);
    CHECK_EQ(urd16(s, 0), 0);
    CHECK_EQ(urd16(s, 2), 0);
    CHECK_EQ(escape(dc, E_SETABORT, 4, s, 0), 1);
    f = put_str("Arial");
    GA(O_SELECTOBJECT, dc, GA(O_CREATEFONT, (uint16_t)-60, 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, FARP(f, 0)));
    GA(O_TEXTOUT, dc, 300, 300, FARP(put_str("Hello printer"), 0), 13);
    GA(O_RECTANGLE, dc, 100, 100, 900, 700);
    CHECK_EQ(escape(dc, E_NEWFRAME, 0, 0, 0), 1);
    CHECK_EQ(gdi_print_count(gg), 0);                             /* the file is written at ENDDOC */
    GA(O_TEXTOUT, dc, 300, 500, FARP(put_str("Second page"), 0), 11);
    CHECK_EQ(escape(dc, E_ENDDOC, 0, 0, 0), 1);
    CHECK_EQ(gdi_print_count(gg), 1);
    if (gdi_print_count(gg) == 1) {
        snprintf(path, sizeof path, "%s", gdi_print_path(gg, 0));
        CHECK(!strncmp(path, g_dir, strlen(g_dir)));
        CHECK(strstr(path, "Test_doc_1") != NULL);                /* the document name, made file-name safe */
        buf = slurp(path, &n);
        CHECK(buf != NULL);
        if (buf) {
            CHECK(has(buf, n, "/Count 2"));                       /* NEWFRAME page + the last page */
            CHECK(has(buf, n, "(Hello printer)"));                /* the text layer */
            CHECK(has(buf, n, "(Second page)"));
            CHECK(has(buf, n, "/Width 2550 /Height 3300"));
            free(buf);
        }
    }
    /* a second document is a second file; a document with no drawing makes no page at ENDDOC after NEWFRAME */
    CHECK_EQ(escape(dc, E_STARTDOC, 4, put_str("Two"), 0), 1);
    GA(O_TEXTOUT, dc, 300, 300, FARP(put_str("only"), 0), 4);
    CHECK_EQ(escape(dc, E_NEWFRAME, 0, 0, 0), 1);
    CHECK_EQ(escape(dc, E_ENDDOC, 0, 0, 0), 1);
    CHECK_EQ(gdi_print_count(gg), 2);
    buf = slurp(gdi_print_path(gg, 1), &n);
    CHECK(buf && has(buf, n, "/Count 1"));
    free(buf);
    /* ABORTDOC throws the document away */
    CHECK_EQ(escape(dc, E_STARTDOC, 6, put_str("Gone"), 0), 1);
    GA(O_TEXTOUT, dc, 300, 300, FARP(put_str("x"), 0), 1);
    CHECK_EQ(escape(dc, E_ABORTDOC, 0, 0, 0), 1);
    CHECK_EQ(gdi_print_count(gg), 2);
    GA(O_DELETEDC, dc);
    gdown();
    cleanup();
}

/* Creative Writer and Fine Artist print through banding: Escape NEXTBAND until it returns an empty rectangle, drawing
 * each band in between (their loop is FUN_1050_0964); the page ends with the empty rectangle. BANDINFO is unsupported
 * (they treat that as "graphics and text both"). */
static void test_nextband(void) {
    uint16_t dc, s;
    char path[600];
    char *buf;
    size_t n;
    printf("test_nextband\n");
    boot();
    dc = printer_dc();
    s = g_seg(0x100);
    CHECK_EQ(escape(dc, 3, 0, 0, s), 0xFFFF);                      /* before STARTDOC: SP_ERROR */
    CHECK_EQ(escape(dc, E_STARTDOC, 5, put_str("Band"), 0), 1);
    {
        uint8_t *q = umem(s, 0, 2);
        q[0] = 3; q[1] = 0;
        CHECK_EQ(escape(dc, E_QUERY, 2, s, 0), 1);                 /* NEXTBAND is supported */
        q[0] = 24;
        CHECK_EQ(escape(dc, E_QUERY, 2, s, 0), 0);                 /* BANDINFO is not */
    }
    CHECK_EQ(escape(dc, 3, 0, 0, s), 1);                           /* the first band is the whole page */
    CHECK_EQ(urd16(s, 0), 0);
    CHECK_EQ(urd16(s, 2), 0);
    CHECK_EQ(urd16(s, 4), 2550);
    CHECK_EQ(urd16(s, 6), 3300);
    GA(O_TEXTOUT, dc, 300, 300, FARP(put_str("banded"), 0), 6);
    CHECK_EQ(escape(dc, 3, 0, 0, s), 1);                           /* the next call: an empty rectangle, the page ends */
    CHECK_EQ(urd16(s, 4) <= urd16(s, 0), 1);
    CHECK_EQ(escape(dc, 3, 0, 0, s), 1);                           /* a second page: whole page again */
    CHECK_EQ(urd16(s, 4), 2550);
    GA(O_TEXTOUT, dc, 300, 300, FARP(put_str("page two"), 0), 8);
    CHECK_EQ(escape(dc, 3, 0, 0, s), 1);
    CHECK_EQ(escape(dc, 24, 12, 0, s), 0);                         /* BANDINFO */
    CHECK_EQ(escape(dc, E_ENDDOC, 0, 0, 0), 1);
    CHECK_EQ(gdi_print_count(gg), 1);
    if (gdi_print_count(gg) == 1) {
        snprintf(path, sizeof path, "%s", gdi_print_path(gg, 0));
        buf = slurp(path, &n);
        CHECK(buf && has(buf, n, "/Count 2") && has(buf, n, "(banded)") && has(buf, n, "(page two)"));
        free(buf);
    }
    GA(O_DELETEDC, dc);
    gdown();
    cleanup();
}

static void test_startdoc_api(void) {
    uint16_t dc, s;
    printf("test_gdi_print_startdoc\n");
    boot();
    dc = printer_dc();
    s = g_seg(0x100);
    CHECK_EQ(GA(O_STARTPAGE, dc), 0xFFFF);                        /* no document */
    /* DOCINFO: cbSize, name far, output far */
    {
        uint8_t *d = umem(s, 0, 10);
        uint16_t nm = put_str("API doc");
        d[0] = 10; d[1] = 0; d[2] = 0x20; d[3] = 0;                                    /* offset of the name inside the same segment */
        memcpy((char *)umem(s, 0x20, 16), "API doc", 8);
        d[4] = (uint8_t)s; d[5] = (uint8_t)(s >> 8);
        d[6] = d[7] = d[8] = d[9] = 0;
        (void)nm;
    }
    CHECK(GA(O_STARTDOC, dc, FARP(s, 0)) > 0);
    CHECK(GA(O_STARTPAGE, dc) > 0);
    GA(O_TEXTOUT, dc, 200, 200, FARP(put_str("page one"), 0), 8);
    CHECK(GA(O_ENDPAGE, dc) > 0);
    CHECK(GA(O_STARTPAGE, dc) > 0);
    GA(O_TEXTOUT, dc, 200, 200, FARP(put_str("page two"), 0), 8);
    CHECK(GA(O_ENDPAGE, dc) > 0);
    CHECK(GA(O_ENDDOC, dc) > 0);
    CHECK_EQ(gdi_print_count(gg), 1);
    if (gdi_print_count(gg) == 1) {
        size_t n = 0;
        char *buf = slurp(gdi_print_path(gg, 0), &n);
        CHECK(buf && has(buf, n, "/Count 2") && has(buf, n, "(page two)"));
        CHECK(strstr(gdi_print_path(gg, 0), "API_doc") != NULL);
        free(buf);
    }
    CHECK_EQ(GA(O_SETABORTPROC, dc, FARP(0, 0)), 1);
    GA(O_DELETEDC, dc);
    gdown();
    cleanup();
}

static void test_driver_entry_points(void) {
    uint16_t out;
    uint8_t *m;
    printf("test_gdi_print_driver\n");
    boot();
    out = g_seg(0x200);
    /* ExtDeviceMode(hwnd, hdriver, out, device, port, in, profile, mode): mode 0 asks for the size */
    CHECK_EQ(g_call("CWFAPRN", 90, (const uint16_t[]){0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 13) & 0xFFFF, 68);
    CHECK_EQ(g_call("CWFAPRN", 90, (const uint16_t[]){0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}, 13) & 0xFFFF, 1);  /* DM_COPY with no buffer */
    {
        uint16_t w[] = {0, 0, out, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
        CHECK_EQ(g_call("CWFAPRN", 90, w, 13) & 0xFFFF, 1);
    }
    m = umem(out, 0, 68);
    CHECK(!strcmp((char *)m, "PDF Printer"));
    CHECK_EQ(urd16(out, 36), 68);                                 /* dmSize */
    CHECK_EQ(urd16(out, 44), 1);                                  /* dmOrientation: portrait */
    CHECK_EQ(urd16(out, 46), 1);                                  /* dmPaperSize: Letter */
    CHECK_EQ(urd16(out, 54), 1);                                  /* dmCopies */
    CHECK_EQ(urd16(out, 64), 300);                                /* dmYResolution */
    /* DeviceCapabilities(device, port, what, out, devmode) */
    CHECK_EQ(g_call("CWFAPRN", 91, (const uint16_t[]){0, 0, 0, 0, 18, 0, 0, 0, 0}, 9) & 0xFFFF, 1);   /* DC_COPIES */
    CHECK_EQ(g_call("CWFAPRN", 91, (const uint16_t[]){0, 0, 0, 0, 8, 0, 0, 0, 0}, 9) & 0xFFFF, 68);   /* DC_SIZE */
    CHECK_EQ(g_call("CWFAPRN", 91, (const uint16_t[]){0, 0, 0, 0, 2, 0, 0, 0, 0}, 9) & 0xFFFF, 1);    /* DC_PAPERS count */
    CHECK_EQ(g_call("CWFAPRN", 91, (const uint16_t[]){0, 0, 0, 0, 77, 0, 0, 0, 0}, 9), 0xFFFFFFFFu);  /* unknown */
    gdown();
    cleanup();
}

static void test_printdlg(void) {
    uint16_t pd, dc;
    uint8_t *p;
    printf("test_gdi_print_dlg\n");
    boot();
    pd = g_seg(0x100);
    p = umem(pd, 0, 52);
    memset(p, 0, 52);
    p[0] = 52;
    p[12] = 0x00; p[13] = 0x01;                                   /* Flags: PD_RETURNDC */
    CHECK_EQ(g_call("COMMDLG", 20, (const uint16_t[]){pd, 0}, 2) & 0xFFFF, 1);
    dc = urd16(pd, 10);
    CHECK(dc != 0);
    CHECK_EQ(GA(O_GETDEVICECAPS, dc, 8), 2550);
    CHECK(urd16(pd, 6) != 0);                                     /* hDevMode */
    CHECK(urd16(pd, 8) != 0);                                     /* hDevNames */
    CHECK_EQ(urd16(pd, 24), 1);                                   /* nCopies */
    {
        uint16_t dn = urd16(pd, 8);
        CHECK(!strcmp((char *)umem(dn, urd16(dn, 0), 8), "CWFAPRN"));
        CHECK(!strcmp((char *)umem(dn, urd16(dn, 2), 12), "PDF Printer"));
        CHECK(!strcmp((char *)umem(dn, urd16(dn, 4), 6), "FILE:"));
    }
    GA(O_DELETEDC, dc);
    p = umem(pd, 0, 52);
    p[0] = 8;                                                     /* too small a struct */
    CHECK_EQ(g_call("COMMDLG", 20, (const uint16_t[]){pd, 0}, 2) & 0xFFFF, 0);
    gdown();
    cleanup();
}

/* With CWFA_PRINT_SAMPLE=path the test also leaves a sample page there: the page the virtual printer produces for a
 * program that prints a title, text in TrueType faces and a coloured box (docs/print-sample.pdf). */
static void test_sample(void) {
    const char *out = getenv("CWFA_PRINT_SAMPLE");
    ttf_set *set;
    gdi_config cfg;
    uint16_t dc;
    static const char *const lines[] = {"Creative Writer & Fine Artist", "printed through the virtual PDF Printer",
                                        "Arial, Times New Roman and Courier New come from Liberation faces."};
    if (!out || !*out) return;
    printf("test_gdi_print_sample\n");
    set = ttf_set_create();
    ttf_set_add_dir(set, "third_party/fonts");
    memset(&cfg, 0, sizeof cfg);
    snprintf(g_dir, sizeof g_dir, "build/prn_%d", (int)getpid());
    mkdir(g_dir, 0777);
    cfg.print_dir = g_dir;
    cfg.fonts = set;
    gboot_cfg(&cfg);
    dc = printer_dc();
    CHECK_EQ(escape(dc, E_STARTDOC, 14, put_str("Print sample"), 0), 1);
    {
        const char *faces[] = {"Arial", "Times New Roman", "Courier New"};
        int sizes[] = {-96, -64, -56}, i;
        for (i = 0; i < 3; i++) {
            GA(O_SELECTOBJECT, dc, GA(O_CREATEFONT, (uint16_t)sizes[i], 0, 0, 0, i == 0 ? 700 : 400, 0, 0, 0, 0, 0, 0, 0, 0,
                                      FARP(put_str(faces[i]), 0)));
            GA(O_TEXTOUT, dc, 300, 400 + i * 220, FARP(put_str(lines[i]), 0), (uint16_t)strlen(lines[i]));
        }
    }
    GA(O_SELECTOBJECT, dc, GA(66, (uint16_t)(RGB(255, 200, 0) >> 16), RGB(255, 200, 0) & 0xFFFF));   /* solid brush */
    GA(O_RECTANGLE, dc, 300, 1200, 2250, 1800);
    CHECK_EQ(escape(dc, E_ENDDOC, 0, 0, 0), 1);
    CHECK_EQ(gdi_print_count(gg), 1);
    if (gdi_print_count(gg) == 1) {
        char cmd[1300];
        snprintf(cmd, sizeof cmd, "cp '%s' '%s'", gdi_print_path(gg, 0), out);
        CHECK_EQ(system(cmd), 0);
    }
    GA(O_DELETEDC, dc);
    gdown();
    cleanup();
    ttf_set_free(set);
}

/* With no --print-dir the PDFs go to <documents folder>/Creative Writer: the folder named by XDG_DOCUMENTS_DIR in the
 * user's user-dirs.dirs, else ~/Documents. The folder is created. */
static void print_one_doc(const char *home, const char *xdg_conf, char *path_out, size_t n) {
    gdi_config cfg;
    uint16_t dc;
    memset(&cfg, 0, sizeof cfg);                                  /* print_dir stays NULL */
    setenv("HOME", home, 1);
    if (xdg_conf) setenv("XDG_CONFIG_HOME", xdg_conf, 1); else unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DOCUMENTS_DIR");
    gboot_cfg(&cfg);
    dc = printer_dc();
    CHECK_EQ(escape(dc, E_STARTDOC, 4, put_str("Home"), 0), 1);
    GA(O_TEXTOUT, dc, 300, 300, FARP(put_str("hi"), 0), 2);
    CHECK_EQ(escape(dc, E_ENDDOC, 0, 0, 0), 1);
    CHECK_EQ(gdi_print_count(gg), 1);
    snprintf(path_out, n, "%s", gdi_print_count(gg) == 1 ? gdi_print_path(gg, 0) : "");
    GA(O_DELETEDC, dc);
    gdown();
}

static void test_default_print_dir(void) {
    char home[96], conf[128], path[700], want[200], *old = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    char *oldx = getenv("XDG_CONFIG_HOME") ? strdup(getenv("XDG_CONFIG_HOME")) : NULL;
    FILE *f;
    printf("test_gdi_print_default_dir\n");
    snprintf(home, sizeof home, "build/prnhome_%d", (int)getpid());   /* relative: the tests run from the repository root */
    mkdir("build", 0777);
    mkdir(home, 0777);
    /* 1. a Spanish desktop: user-dirs.dirs names ~/Documentos */
    snprintf(conf, sizeof conf, "%s/.config", home);
    mkdir(conf, 0777);
    snprintf(want, sizeof want, "%s/.config/user-dirs.dirs", home);
    f = fopen(want, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("# comment\nXDG_DESKTOP_DIR=\"$HOME/Escritorio\"\nXDG_DOCUMENTS_DIR=\"$HOME/Documentos\"\n", f);
        fclose(f);
    }
    print_one_doc(home, NULL, path, sizeof path);
    snprintf(want, sizeof want, "%s/Documentos/Creative Writer/cwfa-Home-", home);
    CHECK(!strncmp(path, want, strlen(want)));
    CHECK(access(path, F_OK) == 0);
    /* 2. XDG_CONFIG_HOME moves the file */
    snprintf(conf, sizeof conf, "%s/cfg", home);
    mkdir(conf, 0777);
    snprintf(want, sizeof want, "%s/cfg/user-dirs.dirs", home);
    f = fopen(want, "w");
    if (f) {
        fputs("XDG_DOCUMENTS_DIR=\"$HOME/Mis docs\"\n", f);
        fclose(f);
    }
    print_one_doc(home, conf, path, sizeof path);
    snprintf(want, sizeof want, "%s/Mis docs/Creative Writer/cwfa-Home-", home);
    CHECK(!strncmp(path, want, strlen(want)));
    /* 3. no user-dirs.dirs at all: ~/Documents */
    snprintf(want, sizeof want, "%s/nohome", home);
    mkdir(want, 0777);
    print_one_doc(want, NULL, path, sizeof path);
    snprintf(conf, sizeof conf, "%s/Documents/Creative Writer/cwfa-Home-", want);
    CHECK(!strncmp(path, conf, strlen(conf)));
    if (old) setenv("HOME", old, 1);
    if (oldx) setenv("XDG_CONFIG_HOME", oldx, 1);
    free(old);
    free(oldx);
    snprintf(conf, sizeof conf, "rm -r '%s'", home);
    if (system(conf)) printf("  (cleanup failed)\n");
}

int main(void) {
    test_dc_and_caps();
    test_default_print_dir();
    test_escape_document();
    test_nextband();
    test_startdoc_api();
    test_driver_entry_points();
    test_printdlg();
    test_sample();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
