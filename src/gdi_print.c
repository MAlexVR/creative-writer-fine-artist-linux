/* The virtual printer: "PDF Printer" (driver CWFAPRN, port FILE:).
 *
 * A printer DC is a memory DC whose bitmap is one Letter page (8.5 x 11 in) at 300 dpi, 8 bits per pixel, drawn by the
 * very same GDI handlers as any memory DC. A page ends (Escape NEWFRAME, EndPage, or ENDDOC with something drawn on it)
 * by being copied into a pdf_t as an indexed image with the screen palette; the text drawn through TextOut/DrawText is
 * also recorded as an invisible text layer so the PDF can be searched. ENDDOC writes the file into the configured
 * directory (gdi_config.print_dir, default <documents folder>/Creative Writer), named after the document.
 *
 * The driver entry points a program loads itself (LoadLibrary("CWFAPRN.DRV") + GetProcAddress) are served as the
 * builtin module CWFAPRN: DeviceMode, ExtDeviceMode and DeviceCapabilities. No real printer driver exists on the CD. */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gdi_priv.h"
#include "kernel_priv.h"
#include "pdf.h"

#define PRN_DRIVER "CWFAPRN"
#define PRN_DEVICE "PDF Printer"
#define PAGE_W_MM 216
#define PAGE_H_MM 279
#define DEVMODE_SIZE 68
#define SP_ERROR 0xFFFF

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

static gprn *prn_of(gdi_t *g, const gdc *dc) {
    if (!dc || dc->prn == 0 || dc->prn > GDI_MAX_PRN) return NULL;
    return g->prn[dc->prn - 1].used ? &g->prn[dc->prn - 1] : NULL;
}

/* ---- device ------------------------------------------------------------------------------------------------ */

static uint8_t white_index(gdi_t *g, uint16_t hdc) {
    gctx x;
    uint8_t w = 255;
    if (gctx_open(g, hdc, &x)) {
        w = gctx_pixel(&x, 0xFFFFFF);
        gctx_close(&x);
    }
    return w;
}

static void clear_page(gdi_t *g, gprn *p) {
    gobj *o = gobj_typed(g, p->hbmp, GO_BITMAP);
    if (o && o->u.bmp.bits) memset(o->u.bmp.bits, white_index(g, p->hdc), (size_t)o->u.bmp.w * (size_t)o->u.bmp.h);
    p->ntxt = 0;
}

static void free_texts(gprn *p) {
    size_t i;
    for (i = 0; i < p->ntxt; i++) free(p->txt[i].s);
    free(p->txt);
    p->txt = NULL;
    p->ntxt = p->captxt = 0;
}

uint16_t gdi_print_create_dc(gdi_t *g, const char *driver, const char *device, bool ic) {
    int i;
    uint16_t hdc, hb = 0;
    gprn *p = NULL;
    gdc *dc;
    gobj *bo = NULL;
    if (!(driver && ieq(driver, PRN_DRIVER)) && !(device && ieq(device, PRN_DEVICE))) return 0;
    for (i = 0; i < GDI_MAX_PRN; i++)
        if (!g->prn[i].used) {
            p = &g->prn[i];
            break;
        }
    if (!p) return 0;
    hdc = gdi_new_memdc(g);
    if (!hdc) return 0;
    dc = gdc_get(g, hdc);
    memset(p, 0, sizeof *p);
    p->w = 8 * 300 + 8 * 300 / 16 * 0 + 150;          /* 2550: 8.5 in */
    p->h = 11 * 300;
    p->dpi = 300;
    p->ic = ic;
    if (!ic) {
        hb = gdi_bitmap_create(g, p->w, p->h, 8, &bo);
        if (!hb) {
            gobj *o = gobj_typed(g, hdc, GO_DC);
            if (o) {
                grgn_free(&o->u.dc->clip);
                free(o->u.dc);
                o->u.dc = NULL;
                o->type = GO_FREE;
            }
            return 0;
        }
        {
            uint16_t prev = dc->bmp;
            gobj *po = gobj_get(g, prev);
            if (po && po->sel > 0) po->sel--;
            dc->bmp = hb;
            bo->sel++;
        }
    }
    p->used = true;
    p->hdc = hdc;
    p->hbmp = hb;
    dc->prn = (uint8_t)(i + 1);
    if (!ic) clear_page(g, p);
    return hdc;
}

void gdi_print_dc_deleted(gdi_t *g, gdc *dc) {
    gprn *p = prn_of(g, dc);
    if (!p) return;
    if (p->pdf) pdf_free(p->pdf);                     /* a document that was never ended is dropped */
    free_texts(p);
    p->pdf = NULL;
    p->used = false;
    if (p->hbmp) {
        gobj *bo = gobj_typed(g, p->hbmp, GO_BITMAP);
        if (bo && bo->sel > 0) bo->sel--;
        dc->bmp = 0;
        if (bo) gobj_release(g, bo);
    }
}

void gdi_print_destroy(gdi_t *g) {
    int i;
    for (i = 0; i < GDI_MAX_PRN; i++) {
        if (g->prn[i].pdf) pdf_free(g->prn[i].pdf);
        free_texts(&g->prn[i]);
        g->prn[i].pdf = NULL;
    }
}

bool gdi_print_device_cap(gdi_t *g, const gdc *dc, int idx, int *val) {
    gprn *p = prn_of(g, dc);
    if (!p) return false;
    switch (idx) {
    case 0: *val = 0x0300; break;
    case 2: *val = 2; break;                           /* DT_RASPRINTER */
    case 4: *val = PAGE_W_MM; break;
    case 6: *val = PAGE_H_MM; break;
    case 8: *val = p->w; break;
    case 10: *val = p->h; break;
    case 12: *val = 8; break;
    case 14: *val = 1; break;
    case 16: *val = -1; break;
    case 18: *val = 16; break;
    case 20: *val = 0; break;
    case 22: *val = 0; break;
    case 24: *val = 256; break;
    case 26: *val = 0; break;
    case 28: *val = 0x00FF; break;
    case 30: *val = 0x007F; break;
    case 32: *val = 0x00FF; break;
    case 34: *val = 0x3C01; break;
    case 36: *val = 1; break;
    case 38: *val = 0x3E99; break;                     /* the display's, without RC_PALETTE (0x100): a printer has no palette */
    case 40: case 42: *val = p->dpi; break;
    case 44: *val = p->dpi * 1414 / 1000; break;
    case 88: case 90: *val = p->dpi; break;
    case 104: *val = 0; break;
    case 106: *val = 0; break;
    case 108: *val = 18; break;
    default: *val = 0; break;
    }
    return true;
}

/* ---- text layer ---------------------------------------------------------------------------------------------- */

void gdi_print_text(gdi_t *g, const gdc *dc, int x, int baseline, int em_px, const uint8_t *s, int n) {
    gprn *p = prn_of(g, dc);
    ptxt *t;
    if (!p || p->ic || n <= 0) return;
    if (p->ntxt == p->captxt) {
        size_t nc = p->captxt ? p->captxt * 2 : 32;
        ptxt *nt = realloc(p->txt, nc * sizeof *nt);
        if (!nt) return;
        p->txt = nt;
        p->captxt = nc;
    }
    t = &p->txt[p->ntxt];
    t->s = malloc((size_t)n);
    if (!t->s) return;
    memcpy(t->s, s, (size_t)n);
    t->n = n;
    t->x = x;
    t->y = baseline;
    t->em = em_px > 0 ? em_px : 1;
    p->ntxt++;
}

/* ---- pages and documents ------------------------------------------------------------------------------------- */

static bool page_blank(gdi_t *g, gprn *p) {
    gobj *o = gobj_typed(g, p->hbmp, GO_BITMAP);
    uint8_t w = white_index(g, p->hdc);
    size_t i, n;
    if (p->ntxt) return false;
    if (!o || !o->u.bmp.bits) return true;
    n = (size_t)o->u.bmp.w * (size_t)o->u.bmp.h;
    for (i = 0; i < n; i++)
        if (o->u.bmp.bits[i] != w) return false;
    return true;
}

static bool emit_page(gdi_t *g, gprn *p) {
    gobj *o = gobj_typed(g, p->hbmp, GO_BITMAP);
    uint8_t pal[256][3];
    size_t i;
    double scale = 72.0 / p->dpi;
    if (!o || !o->u.bmp.bits || !p->pdf) return false;
    for (i = 0; i < 256; i++) {
        uint32_t c = g->fb->pal[i];
        pal[i][0] = (uint8_t)(c >> 16);
        pal[i][1] = (uint8_t)(c >> 8);
        pal[i][2] = (uint8_t)c;
    }
    if (!pdf_add_page(p->pdf, p->w * scale, p->h * scale, p->w, p->h, o->u.bmp.bits, (const uint8_t (*)[3])pal)) return false;
    for (i = 0; i < p->ntxt; i++)
        pdf_page_text(p->pdf, p->txt[i].x * scale, p->txt[i].y * scale, p->txt[i].em * scale, (const char *)p->txt[i].s,
                      (size_t)p->txt[i].n);
    free_texts(p);
    clear_page(g, p);
    return true;
}

static bool start_doc(gprn *p, const char *name) {
    size_t i, k = 0;
    if (p->started || p->ic) return false;
    p->pdf = pdf_create();
    if (!p->pdf) return false;
    for (i = 0; name && name[i] && k < sizeof p->doc - 1; i++) {
        unsigned char c = (unsigned char)name[i];
        p->doc[k++] = isalnum(c) ? (char)c : '_';
    }
    p->doc[k] = 0;
    if (!k) strcpy(p->doc, "document");
    p->started = true;
    return true;
}

/* The folder the PDFs go to when none was chosen: <the user's documents folder>/Creative Writer, where the documents folder is
 * XDG_DOCUMENTS_DIR from $XDG_CONFIG_HOME (else ~/.config)/user-dirs.dirs, else ~/Documents. Parents are created. */
static void default_print_dir(char *out, size_t n) {
    const char *home = getenv("HOME"), *cfgh = getenv("XDG_CONFIG_HOME");
    char file[600], line[700], docs[720] = "";
    FILE *f;
    if (!home || !*home) home = ".";
    if (cfgh && *cfgh) snprintf(file, sizeof file, "%s/user-dirs.dirs", cfgh);
    else snprintf(file, sizeof file, "%s/.config/user-dirs.dirs", home);
    f = fopen(file, "r");
    while (f && fgets(line, sizeof line, f)) {
        char *v, *e;
        if (strncmp(line, "XDG_DOCUMENTS_DIR=", 18)) continue;
        v = line + 18;
        if (*v == '"') v++;
        e = v + strcspn(v, "\"\r\n");
        *e = 0;
        if (!strncmp(v, "$HOME", 5)) snprintf(docs, sizeof docs, "%s%s", home, v + 5);
        else if (*v == '/') snprintf(docs, sizeof docs, "%s", v);
        break;
    }
    if (f) fclose(f);
    if (!*docs || !strcmp(docs, home)) snprintf(docs, sizeof docs, "%s/Documents", home);   /* unset or "no documents folder" */
    mkdir(docs, 0777);
    snprintf(out, n, "%s/Creative Writer", docs);
}

static bool end_doc(gdi_t *g, gprn *p, bool abort_doc) {
    char dir[760], path[960];
    time_t now = time(NULL);
    struct tm tm;
    unsigned n;
    bool ok = true;
    if (!p->started) return false;
    if (!abort_doc && !page_blank(g, p)) emit_page(g, p);
    if (!abort_doc && pdf_page_count(p->pdf) > 0 && g->nprinted < GDI_MAX_PRINTED) {
        if (g->cfg.print_dir && *g->cfg.print_dir) snprintf(dir, sizeof dir, "%s", g->cfg.print_dir);
        else default_print_dir(dir, sizeof dir);
        mkdir(dir, 0777);
        localtime_r(&now, &tm);
        for (n = 0;; n++) {
            char suffix[16] = "";
            if (n) snprintf(suffix, sizeof suffix, "-%u", n + 1);
            snprintf(path, sizeof path, "%s/cwfa-%s-%04d%02d%02d-%02d%02d%02d%s.pdf", dir, p->doc, tm.tm_year + 1900,
                     tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, suffix);
            if (access(path, F_OK) != 0 || n > 999) break;
        }
        ok = pdf_save(p->pdf, path);
        if (ok) {
            snprintf(g->printed[g->nprinted++], sizeof g->printed[0], "%s", path);
            if (g->cfg.log) fprintf(g->cfg.log, "gdi: printed %s\n", path);
            fprintf(stderr, "cwfa: printed -> %s\n", path);          /* so the user sees where the PDF went */
        } else if (g->cfg.log) fprintf(g->cfg.log, "gdi: cannot write %s\n", path);
    }
    pdf_free(p->pdf);
    p->pdf = NULL;
    free_texts(p);
    clear_page(g, p);
    p->started = false;
    p->banding = false;
    return ok;
}

size_t gdi_print_count(const gdi_t *g) { return (size_t)g->nprinted; }
const char *gdi_print_path(const gdi_t *g, size_t i) { return i < (size_t)g->nprinted ? g->printed[i] : NULL; }

/* ---- Escape and the StartDoc family ------------------------------------------------------------------------- */

static bool escape_supported(unsigned code) {
    switch (code) {
    case 1: case 2: case 3: case 6: case 7: case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 17: return true;
    default: return false;
    }
}

/* Escape(hdc, nEscape, cbInput, lpInData, lpOutData) */
static cpu_status g_escape(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), code = hle_u16(&a), cb = hle_u16(&a), isel, ioff, osel, ooff;
    gdc *dc;
    gprn *p;
    const uint8_t *in = NULL;
    char name[64];
    hle_far(&a, &isel, &ioff);
    hle_far(&a, &osel, &ooff);
    GCHECK;
    dc = gdc_get(g, h);
    p = prn_of(g, dc);
    if (!p) {
        gdi_log_stub(c, "Escape on a DC that is not the printer: refused");
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (isel && cb && !(in = hle_ptr(c, isel, ioff, cb))) return CPU_E_SEL;
    if (g->cfg.log) fprintf(g->cfg.log, "gdi: printer Escape %u (cb %u, query %u) from %04x:%04x\n", code, cb, in && cb >= 2 && code == 8 ? gld16(in) : 0, c->caller_cs, c->caller_ip);
    switch (code) {
    case 8:                                            /* QUERYESCSUPPORT */
        hle_ret16(c, in && cb >= 2 && escape_supported(gld16(in)) ? 1 : 0);
        return CPU_OK;
    case 10:                                           /* STARTDOC */
        name[0] = 0;
        if (isel && ioff + 0u < 0x10000u) hle_read_cstr(c, isel, ioff, name, sizeof name);
        hle_ret16(c, start_doc(p, name) ? 1 : SP_ERROR);
        return CPU_OK;
    case 1:                                            /* NEWFRAME */
        hle_ret16(c, p->started && emit_page(g, p) ? 1 : SP_ERROR);
        return CPU_OK;
    case 3: {                                          /* NEXTBAND: one band that is the whole page, then an empty RECT */
        uint8_t *o = osel ? hle_ptr(c, osel, ooff, 8) : NULL;
        if (!p->started || !o) {
            hle_ret16(c, SP_ERROR);
            return CPU_OK;
        }
        if (!p->banding) {
            gst16(o, 0);
            gst16(o + 2, 0);
            gst16(o + 4, (uint16_t)p->w);
            gst16(o + 6, (uint16_t)p->h);
            p->banding = true;
        } else {                                       /* the page is complete: it goes into the document */
            gst16(o, 0);
            gst16(o + 2, 0);
            gst16(o + 4, 0);
            gst16(o + 6, 0);
            p->banding = false;
            if (!emit_page(g, p)) {
                hle_ret16(c, SP_ERROR);
                return CPU_OK;
            }
        }
        hle_ret16(c, 1);
        return CPU_OK;
    }
    case 11:                                           /* ENDDOC */
        hle_ret16(c, end_doc(g, p, false) ? 1 : SP_ERROR);
        return CPU_OK;
    case 2:                                            /* ABORTDOC */
        hle_ret16(c, end_doc(g, p, true) ? 1 : SP_ERROR);
        return CPU_OK;
    case 12: case 13: case 14: {                       /* GETPHYSPAGESIZE, GETPRINTINGOFFSET, GETSCALINGFACTOR */
        uint8_t *o = osel ? hle_ptr(c, osel, ooff, 4) : NULL;
        if (!o) {
            hle_ret16(c, SP_ERROR);
            return CPU_OK;
        }
        gst16(o, code == 12 ? (uint16_t)p->w : 0);
        gst16(o + 2, code == 12 ? (uint16_t)p->h : 0);
        hle_ret16(c, 1);
        return CPU_OK;
    }
    case 6: case 7: case 9: case 17:                   /* FLUSHOUTPUT, DRAFTMODE, SETABORTPROC, SETCOPYCOUNT */
        if (code == 17 && osel) {
            uint8_t *o = hle_ptr(c, osel, ooff, 2);
            if (o) gst16(o, 1);
        }
        hle_ret16(c, 1);
        return CPU_OK;
    default:
        hle_ret16(c, 0);                               /* not supported */
        return CPU_OK;
    }
}

/* StartDoc(hdc, lpdi): DOCINFO is cbSize, lpszDocName, lpszOutput */
static cpu_status g_startdoc(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    gdc *dc;
    gprn *p;
    char name[64] = "";
    hle_far(&a, &sel, &off);
    GCHECK;
    dc = gdc_get(g, h);
    p = prn_of(g, dc);
    if (!p) {
        hle_ret16(c, SP_ERROR);
        return CPU_OK;
    }
    if (sel) {
        const uint8_t *di = hle_ptr(c, sel, off, 10);
        if (di) {
            uint16_t nsel = gld16(di + 4), noff = gld16(di + 2);
            if (nsel) hle_read_cstr(c, nsel, noff, name, sizeof name);
        }
    }
    hle_ret16(c, start_doc(p, name) ? 1 : SP_ERROR);
    return CPU_OK;
}

static cpu_status g_enddoc(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gprn *p;
    GCHECK;
    p = prn_of(g, gdc_get(g, h));
    hle_ret16(c, p && end_doc(g, p, false) ? 1 : SP_ERROR);
    return CPU_OK;
}

static cpu_status g_abortdoc(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gprn *p;
    GCHECK;
    p = prn_of(g, gdc_get(g, h));
    hle_ret16(c, p && end_doc(g, p, true) ? 1 : SP_ERROR);
    return CPU_OK;
}

static cpu_status g_startpage(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gprn *p;
    GCHECK;
    p = prn_of(g, gdc_get(g, h));
    hle_ret16(c, p && p->started ? 1 : SP_ERROR);       /* the page is already blank: EndPage/NEWFRAME clear it */
    return CPU_OK;
}

static cpu_status g_endpage(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    gprn *p;
    GCHECK;
    p = prn_of(g, gdc_get(g, h));
    hle_ret16(c, p && p->started && emit_page(g, p) ? 1 : SP_ERROR);
    return CPU_OK;
}

static cpu_status g_setabortproc(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    hle_far(&a, &sel, &off);
    GCHECK;
    hle_ret16(c, prn_of(g, gdc_get(g, h)) ? 1 : 0);      /* never called: nothing spools */
    return CPU_OK;
}

/* ---- the driver's own entry points (module CWFAPRN) ---------------------------------------------------------- */

/* DEVMODE (Windows 3.1): name[32], specversion, driverversion, size, driverextra (words, 32..38), fields (dword, 40),
 * orientation 44, papersize 46, paperlength 48, paperwidth 50, scale 52, copies 54, defaultsource 56, printquality 58,
 * color 60, duplex 62, yresolution 64, ttoption 66 = 68 bytes. */
static void devmode_fill(uint8_t *d) {
    memset(d, 0, DEVMODE_SIZE);
    strcpy((char *)d, PRN_DEVICE);
    gst16(d + 32, 0x030A);
    gst16(d + 34, 0x0300);
    gst16(d + 36, DEVMODE_SIZE);
    gst16(d + 38, 0);
    gst32(d + 40, 0x0001 | 0x0002 | 0x0100 | 0x0400 | 0x0800);
    gst16(d + 44, 1);                                   /* portrait */
    gst16(d + 46, 1);                                   /* DMPAPER_LETTER */
    gst16(d + 52, 100);                                 /* scale */
    gst16(d + 54, 1);                                   /* copies */
    gst16(d + 56, 15);                                  /* DMBIN_AUTO */
    gst16(d + 58, 300);                                 /* print quality: dpi */
    gst16(d + 60, 2);                                   /* DMCOLOR_COLOR */
    gst16(d + 64, 300);
}

/* ExtDeviceMode(hwnd, hdriver, lpOut, lpDevice, lpPort, lpIn, lpProfile, wMode) */
static cpu_status d_extdevicemode(hle_call *c) {
    GBEGIN;
    uint16_t hwnd = hle_u16(&a), hdrv = hle_u16(&a), osel, ooff, dsel, doff, psel, poff, isel, ioff, fsel, foff, mode;
    (void)hwnd;
    (void)hdrv;
    hle_far(&a, &osel, &ooff);
    hle_far(&a, &dsel, &doff);
    hle_far(&a, &psel, &poff);
    hle_far(&a, &isel, &ioff);
    hle_far(&a, &fsel, &foff);
    mode = hle_u16(&a);
    GCHECK;
    if (mode == 0) {
        hle_ret16(c, DEVMODE_SIZE);                     /* the size the caller must provide */
        return CPU_OK;
    }
    if ((mode & 2) && osel) {                           /* DM_COPY / DM_OUT_BUFFER */
        uint8_t *o = hle_ptr(c, osel, ooff, DEVMODE_SIZE);
        if (!o) return CPU_E_SEL;
        devmode_fill(o);
    }
    hle_ret16(c, 1);                                    /* IDOK */
    return CPU_OK;
}

/* DeviceMode(hwnd, hmodule, lpDevice, lpPort) */
static cpu_status d_devicemode(hle_call *c) {
    GBEGIN;
    uint16_t hwnd = hle_u16(&a), hmod = hle_u16(&a), dsel, doff, psel, poff;
    (void)hwnd;
    (void)hmod;
    hle_far(&a, &dsel, &doff);
    hle_far(&a, &psel, &poff);
    GCHECK;
    hle_ret16(c, 1);
    return CPU_OK;
}

static uint16_t put_pair_list(hle_call *c, uint16_t sel, uint16_t off, const uint16_t *v, unsigned n, unsigned stride) {
    uint8_t *o;
    unsigned i;
    if (!sel) return (uint16_t)n;                       /* the caller only counts */
    if (!(o = hle_ptr(c, sel, off, n * stride))) return 0;
    for (i = 0; i < n; i++) memcpy(o + i * stride, &v[i * stride / 2], stride);
    return (uint16_t)n;
}

/* DeviceCapabilities(lpDevice, lpPort, fwCapability, lpOutput, lpDevMode): DWORD */
static cpu_status d_devicecapabilities(hle_call *c) {
    GBEGIN;
    uint16_t dsel, doff, psel, poff, what, osel, ooff, msel, moff;
    uint32_t r = 0xFFFFFFFFu;
    hle_far(&a, &dsel, &doff);
    hle_far(&a, &psel, &poff);
    what = hle_u16(&a);
    hle_far(&a, &osel, &ooff);
    hle_far(&a, &msel, &moff);
    GCHECK;
    switch (what) {
    case 1: r = 0x0001 | 0x0002 | 0x0100 | 0x0400 | 0x0800; break;                     /* DC_FIELDS */
    case 2: {                                                                          /* DC_PAPERS */
        uint16_t v[1] = {1};
        r = put_pair_list(c, osel, ooff, v, 1, 2);
        break;
    }
    case 3: {                                                                          /* DC_PAPERSIZE: tenths of a mm */
        uint16_t v[2] = {2159, 2794};
        r = put_pair_list(c, osel, ooff, v, 1, 4);
        break;
    }
    case 4: r = (uint32_t)500 | (uint32_t)500 << 16; break;                            /* DC_MINEXTENT */
    case 5: r = (uint32_t)2159 | (uint32_t)2794 << 16; break;                          /* DC_MAXEXTENT */
    case 6: {                                                                          /* DC_BINS */
        uint16_t v[1] = {15};
        r = put_pair_list(c, osel, ooff, v, 1, 2);
        break;
    }
    case 7: r = 0; break;                                                              /* DC_DUPLEX */
    case 8: r = DEVMODE_SIZE; break;                                                   /* DC_SIZE */
    case 9: r = 0; break;                                                              /* DC_EXTRA */
    case 10: r = 0x030A; break;                                                        /* DC_VERSION */
    case 11: r = 0x0300; break;                                                        /* DC_DRIVER */
    case 12: {                                                                         /* DC_BINNAMES: 24-byte slots */
        if (osel) {
            uint8_t *o = hle_ptr(c, osel, ooff, 24);
            if (!o) return CPU_E_SEL;
            memset(o, 0, 24);
            strcpy((char *)o, "Automatic");
        }
        r = 1;
        break;
    }
    case 13: {                                                                         /* DC_ENUMRESOLUTIONS: pairs of LONG */
        if (osel) {
            uint8_t *o = hle_ptr(c, osel, ooff, 8);
            if (!o) return CPU_E_SEL;
            gst32(o, 300);
            gst32(o + 4, 300);
        }
        r = 1;
        break;
    }
    case 14: r = 0; break;                                                             /* DC_FILEDEPENDENCIES */
    case 15: r = 0; break;                                                             /* DC_TRUETYPE */
    case 16: {                                                                         /* DC_PAPERNAMES: 64-byte slots */
        if (osel) {
            uint8_t *o = hle_ptr(c, osel, ooff, 64);
            if (!o) return CPU_E_SEL;
            memset(o, 0, 64);
            strcpy((char *)o, "Letter");
        }
        r = 1;
        break;
    }
    case 17: r = 0; break;                                                             /* DC_ORIENTATION: cannot rotate */
    case 18: r = 1; break;                                                             /* DC_COPIES */
    default: break;
    }
    hle_ret32(c, r);
    return CPU_OK;
}

/* ---- COMMDLG PrintDlg ---------------------------------------------------------------------------------------- */

/* PRINTDLG (Windows 3.1): lStructSize 0, hwndOwner 4, hDevMode 6, hDevNames 8, hDC 10, Flags 12, nFromPage 16, nToPage 18,
 * nMinPage 20, nMaxPage 22, nCopies 24, ... (52 bytes). The virtual printer is the only one: the dialog is answered at
 * once with OK, handing back the DEVMODE and DEVNAMES blocks (and a DC with PD_RETURNDC). */
static uint16_t block_new(gdi_t *g, uint32_t len) {
    const hle_entry *e = hle_find(g->hle, "KERNEL", 15);
    kernel_t *k = e ? e->ctx : NULL;
    return k ? kernel_global_alloc(k, len, 0x0002) : mem_alloc(&g->p->mem, len, MEM_DATA);
}

static cpu_status commdlg_printdlg(hle_call *c) {
    GBEGIN;
    uint16_t sel, off;
    uint8_t *pd;
    uint32_t flags;
    hle_far(&a, &sel, &off);
    GCHECK;
    if (!(pd = hle_ptr(c, sel, off, 52))) return CPU_E_SEL;
    flags = gld32(pd + 12);
    if (gld32(pd) < 52) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (!gld16(pd + 6)) {                                 /* hDevMode */
        uint16_t h = block_new(g, DEVMODE_SIZE);
        uint8_t *d = h ? mem_ptr(&g->p->mem, h, 0, DEVMODE_SIZE) : NULL;
        if (d) {
            devmode_fill(d);
            gst16(pd + 6, h);
        }
    }
    if (!gld16(pd + 8)) {                                 /* hDevNames: DEVNAMES + "CWFAPRN\0PDF Printer\0FILE:\0" */
        static const char strs[] = PRN_DRIVER "\0" PRN_DEVICE "\0" "FILE:";
        uint32_t len = 8 + (uint32_t)sizeof strs + 1;
        uint16_t h = block_new(g, len);
        uint8_t *d = h ? mem_ptr(&g->p->mem, h, 0, len) : NULL;
        if (d) {
            memset(d, 0, len);
            gst16(d, 8);                                  /* driver offset */
            gst16(d + 2, 8 + sizeof PRN_DRIVER);          /* device offset */
            gst16(d + 4, 8 + sizeof PRN_DRIVER + sizeof PRN_DEVICE);   /* output port offset */
            gst16(d + 6, 1);                              /* default printer */
            memcpy(d + 8, strs, sizeof strs);
            gst16(pd + 8, h);
        }
    }
    if (flags & 0x0100) {                                 /* PD_RETURNDC */
        uint16_t dc = gdi_print_create_dc(g, PRN_DRIVER, PRN_DEVICE, false);
        gst16(pd + 10, dc);
        if (!dc) {
            hle_ret16(c, 0);
            return CPU_OK;
        }
    } else if (flags & 0x0200) {                          /* PD_RETURNIC */
        uint16_t dc = gdi_print_create_dc(g, PRN_DRIVER, PRN_DEVICE, true);
        gst16(pd + 10, dc);
    }
    if (gld16(pd + 24) == 0) gst16(pd + 24, 1);           /* nCopies */
    hle_ret16(c, 1);
    return CPU_OK;
}

#define G GDI_MODULE
const hle_def gdi_print_defs[] = {
    {G, 38, "Escape", 14, g_escape},
    {G, 377, "StartDoc", 6, g_startdoc},
    {G, 378, "EndDoc", 2, g_enddoc},
    {G, 379, "StartPage", 2, g_startpage},                       /* ARTIST */
    {G, 380, "EndPage", 2, g_endpage},                           /* ARTIST */
    {G, 381, "SetAbortProc", 6, g_setabortproc},
    {G, 382, "AbortDoc", 2, g_abortdoc},
    {PRN_DRIVER, 13, "DeviceMode", 12, d_devicemode},
    {PRN_DRIVER, 90, "ExtDeviceMode", 26, d_extdevicemode},
    {PRN_DRIVER, 91, "DeviceCapabilities", 18, d_devicecapabilities},
    {"COMMDLG", 20, "PrintDlg", 4, commdlg_printdlg},
};
#undef G
const size_t gdi_print_ndefs = sizeof gdi_print_defs / sizeof gdi_print_defs[0];
