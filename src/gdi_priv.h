#ifndef GDI_PRIV_H
#define GDI_PRIV_H
/* Internal to the gdi*.c files. */
#include <stdlib.h>
#include <string.h>

#include "fnt.h"
#include "gdi.h"
#include "wm.h"

#define GDI_MODULE "GDI"
#define USER_MODULE_NAME "USER"

/* ---- regions (gdi_rgn.c): banded rectangle lists ---------------------------------
 * Canonical form: bands sorted by top; inside a band the rectangles share top and
 * bottom, are sorted by left and neither overlap nor touch; consecutive bands that
 * touch and carry identical rectangles are merged. Two regions are equal iff their
 * lists are equal. */
typedef struct {
    fb_rect *r;
    int n, cap;
    fb_rect box; /* bounding box, all zero when empty */
} grgn;

#define RGN_AND 1
#define RGN_OR 2
#define RGN_XOR 3
#define RGN_DIFF 4
#define RGN_COPY 5

#define NULLREGION 1
#define SIMPLEREGION 2
#define COMPLEXREGION 3

void grgn_init(grgn *g);
void grgn_free(grgn *g);
bool grgn_copy(grgn *dst, const grgn *src);
bool grgn_set_rect(grgn *g, int l, int t, int r, int b);
bool grgn_combine(grgn *dst, const grgn *a, const grgn *b, int mode);
bool grgn_intersect_rect(grgn *g, const fb_rect *r);
bool grgn_subtract_rect(grgn *g, const fb_rect *r);
void grgn_offset(grgn *g, int dx, int dy);
bool grgn_equal(const grgn *a, const grgn *b);
bool grgn_has_point(const grgn *g, int x, int y);
bool grgn_overlaps_rect(const grgn *g, const fb_rect *r);
int grgn_kind(const grgn *g);
/* Region of the set bits of a w*h mask placed at (ox, oy). */
bool grgn_from_mask(grgn *g, const uint8_t *mask, int w, int h, int ox, int oy);
bool grgn_ellipse(grgn *g, int l, int t, int r, int b);
bool grgn_roundrect(grgn *g, int l, int t, int r, int b, int ew, int eh);
bool grgn_polygon(grgn *g, const int16_t *xy, int n, bool winding);
bool grgn_polypolygon(grgn *g, const int16_t *xy, const int16_t *counts, int npoly, bool winding);

/* ---- objects -------------------------------------------------------------------- */

typedef enum { GO_FREE = 0, GO_PEN, GO_BRUSH, GO_FONT, GO_BITMAP, GO_PALETTE, GO_REGION, GO_DC } gobj_type;

#define PS_SOLID 0
#define PS_DASH 1
#define PS_DOT 2
#define PS_DASHDOT 3
#define PS_DASHDOTDOT 4
#define PS_NULL 5
#define PS_INSIDEFRAME 6

#define BS_SOLID 0
#define BS_NULL 1
#define BS_HATCHED 2
#define BS_PATTERN 3
#define BS_DIBPATTERN 5

#define GCREF_INDEX 0x01000000u   /* PALETTEINDEX */
#define GCREF_PALRGB 0x02000000u  /* PALETTERGB */
#define GCREF_RAW 0x04000000u     /* internal: a device pixel value (low byte) */
#define GCREF_MONO 0x05000000u    /* internal: monochrome pattern bit (low bit) */

typedef struct { int style, width; uint32_t color; } gpen;

typedef struct {
    int style, hatch;
    uint32_t color;
    uint32_t pat[64]; /* 8x8 pattern, row major, COLORREF or GCREF_RAW/GCREF_MONO */
} gbrush;

typedef struct {
    int w, h, bpp; /* bpp 1 (values 0/1) or 8 (palette indices) */
    uint8_t *bits; /* w*h bytes */
    int dim_x, dim_y;
} gbitmap;

typedef struct {
    int n;
    uint8_t (*e)[4]; /* r, g, b, flags */
    uint8_t *map;    /* system palette index each entry was realized to */
    uint8_t *own;    /* 1 where the entry holds a reference on a dynamic system entry */
    bool realized;
} gpal;

typedef struct {
    uint8_t raw[50];  /* the LOGFONT as the program gave it */
    int height, width; /* resolved cell size in pixels */
    int ascent, descent, intlead, avgw, maxw;
    int weight;
    bool italic, underline, strikeout, fixed, bold;
    int overhang;
    double sx;  /* horizontal glyph unit in pixels */
    int base;
    ttf_inst *tt;          /* a TrueType face at this size: its metrics, advances and 1-bit glyphs replace the built-in set */
    const fnt_strike *bmp; /* a raster face from an added .FON: its glyphs and metrics replace the built-in set */
} gfont;

#define GDI_MAX_OBJECTS 8192
#define GDI_MAX_ADDED 16
#define GDI_HANDLE_BASE 0x1000

struct gdc;
typedef struct gobj {
    uint8_t type;
    bool stock;
    int sel; /* DCs that have it selected */
    union {
        gpen pen;
        gbrush brush;
        gfont font;
        gbitmap bmp;
        gpal pal;
        grgn rgn;
        struct gdc *dc;
    } u;
} gobj;

typedef struct gdc {
    bool used, mem;
    uint16_t h;
    unsigned serial; /* wm_dc serial the state belongs to (screen DCs) */
    uint16_t pen, brush, font, bmp, pal;
    grgn clip;
    bool has_clip;
    uint32_t bk, fg;
    int bkmode, rop2, mapmode, polyfill, stretch, talign, cextra, jextra, jcount;
    int wox, woy, wex, wey, vox, voy, vex, vey;
    uint8_t prn;     /* 1 + index of the virtual printer this DC is (gdi_print.c), 0: not a printer DC */
    int px, py;      /* current position, logical */
    int box, boy;    /* brush origin */
    struct gdc *saved; /* chain of SaveDC states, most recent first */
    int nsaved;
} gdc;

#define OPAQUE 2
#define TRANSPARENT 1

#define MM_TEXT 1
#define MM_LOMETRIC 2
#define MM_HIMETRIC 3
#define MM_LOENGLISH 4
#define MM_HIENGLISH 5
#define MM_TWIPS 6
#define MM_ISOTROPIC 7
#define MM_ANISOTROPIC 8

#define TA_LEFT 0
#define TA_RIGHT 2
#define TA_CENTER 6
#define TA_BOTTOM 8
#define TA_BASELINE 24
#define TA_UPDATECP 1

typedef struct { bool used; uint16_t h; unsigned serial; gdc dc; } gwmdc;

#define GDI_SCRATCH 0x4000

#define GMETA_MAX 8
#define GDI_MAX_PRN 4
#define GDI_MAX_PRINTED 16
struct gmeta;
struct pdf;

/* One virtual printer DC (gdi_print.c): its page bitmap, the document being built and the text drawn on the page. */
typedef struct { uint8_t *s; int n, x, y, em; } ptxt;
typedef struct {
    bool used, ic, started, banding;   /* banding: the whole-page band of the current page was handed out (Escape NEXTBAND) */
    uint16_t hdc, hbmp;
    int w, h, dpi;
    struct pdf *pdf;
    char doc[64];
    ptxt *txt;
    size_t ntxt, captxt;
} gprn;

struct gdi {
    hle_t *hle;
    user_t *user;
    wm_t *wm;
    fb_t *fb;
    loader_proc *p;
    gdi_config cfg;
    gobj **objs;
    int nobjs, objcap;   /* slots in use (high-water mark), allocated */
    uint16_t defbmp;     /* the 1x1 monochrome bitmap every new memory DC holds */
    uint16_t stock[17];  /* GetStockObject handles by index */
    gwmdc wmdc[WM_MAX_DCS];
    gdc cache;           /* scratch DC state for lookups that fail */
    gdi_stub_rec stubs[GDI_STUB_MAX];
    size_t nstubs;
    grgn clipscratch;    /* effective clip of the drawing in progress */
    uint16_t scratch_sel; /* guest memory for callbacks */
    struct { char name[32], path[128]; int refs; int fnt; } added[GDI_MAX_ADDED]; /* AddFontResource; fnt: index in fnts or -1 */
    int nadded;
    fnt_font fnts[GDI_MAX_ADDED * 2];  /* loaded raster fonts; kept until gdi_destroy (fonts made from them point into them) */
    int nfnts;
    unsigned pal_gen, ccache_gen; /* palette changes; generation the colour cache belongs to */
    struct { bool valid; uint32_t key; uint8_t val; } ccache[4096];
    struct { int cell; uint16_t h; } uifont[6]; /* fonts made for dialogs, by cell height */
    int nuifont;
    int sysuse;           /* SYSPAL_STATIC (1) or SYSPAL_NOSTATIC (2) */
    uint8_t sysref[256];  /* palette entries mapped to each dynamic system entry */
    uint8_t syspriv[256]; /* entry allocated with PC_NOCOLLAPSE or PC_RESERVED: never shared */
    struct gmeta *metas[GMETA_MAX]; /* metafiles being recorded (CreateMetaFile .. CloseMetaFile) */
    int nmetas;
    gprn prn[GDI_MAX_PRN];
    char printed[GDI_MAX_PRINTED][960];   /* files the virtual printer wrote */
    int nprinted;
};

/* handler plumbing */
#define GBEGIN gdi_t *g = c->ctx; hle_args a; hle_args_init(&a, c); (void)g
#define GCHECK do { if (a.st) return a.st; } while (0)

static inline uint16_t gld16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t gld32(const uint8_t *p) { return gld16(p) | (uint32_t)gld16(p + 2) << 16; }
static inline void gst16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void gst32(uint8_t *p, uint32_t v) { gst16(p, (uint16_t)v); gst16(p + 2, (uint16_t)(v >> 16)); }

/* object table (gdi.c) */
uint16_t gobj_new(gdi_t *g, gobj_type t, gobj **out);
gobj *gobj_get(gdi_t *g, uint16_t h);
gobj *gobj_typed(gdi_t *g, uint16_t h, gobj_type t);
void gobj_release(gdi_t *g, gobj *o);
gdc *gdc_get(gdi_t *g, uint16_t hdc);
void gdc_select_defaults(gdi_t *g, gdc *dc);
void gdi_log_stub(hle_call *c, const char *task);
uint16_t gdi_brush_pattern(gdi_t *g, uint16_t hbm);
uint16_t gdi_brush_dib(gdi_t *g, hle_call *c, uint16_t hglobal, int usage);
uint16_t gdi_bitmap_create(gdi_t *g, int w, int h, int bpp, gobj **out);
/* the virtual printer (gdi_print.c) */
uint16_t gdi_print_create_dc(gdi_t *g, const char *driver, const char *device, bool ic);
void gdi_print_dc_deleted(gdi_t *g, gdc *dc);
void gdi_print_destroy(gdi_t *g);
bool gdi_print_device_cap(gdi_t *g, const gdc *dc, int idx, int *val);
void gdi_print_text(gdi_t *g, const gdc *dc, int x, int baseline, int em_px, const uint8_t *s, int n);
extern const hle_def gdi_print_defs[];
extern const size_t gdi_print_ndefs;
extern const hle_def gdi_misc_defs[];
extern const size_t gdi_misc_ndefs;
extern const hle_def gdi_pal_defs[];
extern const size_t gdi_pal_ndefs;
extern const hle_def gdi_dib_defs[];
extern const size_t gdi_dib_ndefs;
uint16_t gdi_new_memdc(gdi_t *g);
void gdi_meta_tap(void *ctx, hle_call *call);   /* hle tap: records the calls made on a metafile DC */
void gdi_meta_free_all(gdi_t *g);
extern const hle_def gdi_meta_defs[];
extern const size_t gdi_meta_ndefs;
uint16_t gdi_brush_from_logbrush(gdi_t *g, hle_call *c, uint16_t sel, uint16_t off);
uint16_t gdi_font_stock(gdi_t *g, int idx);
uint16_t gdi_palette_stock(gdi_t *g);
bool gdi_make_default_bitmap(gdi_t *g);
void gdi_font_resolve(gfont *f);
extern const hle_def gdi_font_defs[];
extern const size_t gdi_font_ndefs;
void gdi_ui_install(gdi_t *g); /* gdi_ui.c: hands user_gfx to USER */
extern const hle_def gdi_rgn_defs[];
extern const size_t gdi_rgn_ndefs;

/* mapping (gdi.c) */
void gdc_lp_to_dp(const gdc *dc, int *x, int *y);
void gdc_dp_to_lp(const gdc *dc, int *x, int *y);
int gdc_lx(const gdc *dc, int x);  /* logical x to device x */
int gdc_ly(const gdc *dc, int y);
int gdc_scale_x(const gdc *dc, int dx); /* logical extent to device extent */
int gdc_scale_y(const gdc *dc, int dy);

/* drawing context (gdi_draw.c) */
typedef struct {
    gdi_t *g;
    gdc *dc;
    uint8_t *pix;
    int sw, sh;
    bool mono;
    int ox, oy;          /* surface position of the DC origin */
    grgn clipr;          /* effective clip, surface coordinates (owned) */
    int rop2;
    uint16_t hdc;
} gctx;

/* Where a DC draws: a surface, the DC origin on it and the visible base clip (surface coordinates). */
typedef struct {
    uint8_t *pix;
    int w, h;
    bool mono;
    int ox, oy;
    fb_rect base;
} gsurf;

bool gdc_surface(gdi_t *g, gdc *dc, gsurf *s);
/* False when the DC is unknown or has no drawable surface. gctx_close releases the clip. */
bool gctx_open(gdi_t *g, uint16_t hdc, gctx *x);
void gctx_close(gctx *x);
uint8_t gctx_pixel(const gctx *x, uint32_t colorref);
uint8_t gdi_nearest(gdi_t *g, uint32_t colorref);
void gdi_pal_forget(gdi_t *g, gobj *po);
void gdi_pal_unrealize(gdi_t *g, gobj *po);
int gdi_pal_nearest_entry(const gpal *p, uint32_t rgb_cref);
uint32_t gctx_colorref_of(const gctx *x, uint8_t pixel);

/* 8x8 brush cell resolved to device pixels: values 0-255, or BPAT_CLEAR for "leave alone" */
#define BPAT_CLEAR 0x100
typedef struct {
    int kind;            /* 0 none, 1 solid, 2 pattern */
    uint8_t solid;
    uint16_t pat[64];
} bpat;

void brush_prepare(const gctx *x, const gbrush *b, bpat *bp);
/* Device pixel of the brush at surface position (sx, sy), BPAT_CLEAR when it leaves the pixel alone. */
int bpat_at(const gctx *x, const bpat *bp, int sx, int sy);
uint8_t rop2_apply(int rop2, uint8_t pen, uint8_t dst, uint8_t mask);
/* Fills a surface rectangle (clipped) with the brush; rop2 13 copies. Returns pixels touched. */
void fill_rect_surface(gctx *x, const fb_rect *r, const bpat *bp, int rop2);
void invert_rect_surface(gctx *x, const fb_rect *r);
fb_rect dev_rect_of(const gdc *dc, int l, int t, int r, int b); /* DC device coordinates, normalized */
/* ROP3 (gdi_blt.c): the truth table byte is indexed by P << 2 | S << 1 | D, bit by bit. */
uint8_t rop3_eval(uint8_t rop, uint8_t p, uint8_t s, uint8_t d);
bool rop3_uses_src(uint8_t rop);
bool rop3_uses_pat(uint8_t rop);
bool rop3_uses_dst(uint8_t rop);
/* The pixels a blit reads: device pixels (monochrome 0/1), the rows that hold data, and the source's
 * background colour as a device pixel (colour source into a monochrome destination). */
typedef struct {
    const uint8_t *pix;
    int w, h;
    bool mono;
    uint8_t bkpix;
    int row_lo, row_hi;
} bsrc;
bool blt_core(gctx *d, uint8_t r3, int dl, int dt, int dwa, int dha, const bsrc *s, int sl, int st, int swa, int sha, bool fx,
              bool fy);
extern const hle_def gdi_blt_defs[];
extern const size_t gdi_blt_ndefs;
extern const hle_def gdi_draw_defs[];
extern const size_t gdi_draw_ndefs;

#endif
