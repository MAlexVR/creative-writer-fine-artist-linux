/* String tables, the KEYBOARD translation functions and (below) bitmaps, icons, cursors and
 * accelerator tables loaded from the program's NE resources. */
#include <stdlib.h>
#include <string.h>

#include "ui_priv.h"

/* ---- LoadString ---------------------------------------------------------------------- */

static cpu_status u_loadstring(hle_call *c) {
    UBEGIN;
    uint16_t inst = hle_u16(&a), id = hle_u16(&a), sel, off;
    int cch;
    char tmp[512];
    int n;
    uint8_t *d;
    hle_far(&a, &sel, &off);
    cch = (int16_t)hle_u16(&a);
    UCHECK;
    hle_ret16(c, 0);
    if (cch <= 0 || inst != u->p->auto_ds) return CPU_OK;
    n = res_string(u->p->ne, id, tmp, sizeof tmp);
    if (!(d = hle_ptr(c, sel, off, 1))) return CPU_E_SEL;
    if (n < 0) n = 0;
    if (n > cch - 1) n = cch - 1;
    if (!(d = hle_ptr(c, sel, off, (uint32_t)n + 1))) return CPU_E_SEL;
    memcpy(d, tmp, (size_t)n);
    d[n] = 0;
    hle_ret16(c, (uint16_t)n);
    return CPU_OK;
}


/* ---- images: bitmaps, icons and cursors ------------------------------------------------------ */

#define RGBC(r, g, b) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16)
#define IDC_FIRST 32512
#define SYS_ICON_SIZE 32

/* The name or integer id behind a (segment, offset) resource name argument. */
typedef struct { bool by_name; unsigned id; char name[64]; } resname;

static bool resname_arg(hle_call *c, uint16_t sel, uint16_t off, resname *r) {
    memset(r, 0, sizeof *r);
    if (sel == 0) {
        r->id = off;
        return true;
    }
    if (!hle_read_cstr(c, sel, off, r->name, sizeof r->name)) return false;
    if (r->name[0] == '#') {
        r->id = (unsigned)atoi(r->name + 1);
        r->name[0] = 0;
    } else {
        r->by_name = true;
    }
    return true;
}

static uint32_t name_key(const resname *r) {
    uint32_t h = 5381;
    const char *p;
    if (!r->by_name) return r->id;
    for (p = r->name; *p; p++) h = h * 33 + (uint32_t)(unsigned char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    return h | 0x80000000u;
}

ui_image *ui_image_get(ui_t *ui, uint16_t h) {
    unsigned i;
    for (i = 0; i < UI_MAX_IMAGES; i++)
        if (ui->img[i].used && ui->img[i].handle == h) return &ui->img[i];
    return NULL;
}

static ui_image *image_slot(ui_t *ui, uint16_t *h) {
    unsigned i;
    for (i = 0; i < UI_MAX_IMAGES; i++)
        if (!ui->img[i].used) {
            memset(&ui->img[i], 0, sizeof ui->img[i]);
            ui->img[i].used = true;
            ui->img[i].handle = *h = (uint16_t)(UI_IMAGE_BASE + i);
            return &ui->img[i];
        }
    return NULL;
}

static void image_free(ui_t *ui, ui_image *im) {
    kernel_t *k = im->gh ? ui_kernel(ui->u) : NULL;
    if (k) kernel_global_free(k, im->gh);
    free(im->cref);
    free(im->mask);
    memset(im, 0, sizeof *im);
}

/* A Win16 HCURSOR is a global memory handle to a CURSORSHAPE (xHotSpot, yHotSpot, cx, cy, cbWidth words, Planes and
 * BitsPixel bytes, then the AND mask and the XOR mask as 1 bpp rows padded to a byte): programs GlobalLock it and read
 * the mask (Fine Artist builds a bitmap from it). The block becomes the handle the program holds. XOR bit: a pixel
 * the cursor draws white. Without a KERNEL the opaque slot handle stays. */
static void image_shape(ui_t *ui, ui_image *im, uint16_t *h) {
    kernel_t *k = ui_kernel(ui->u);
    unsigned cb = (unsigned)(im->w + 7) / 8, plane = cb * (unsigned)im->h, x, y;
    uint32_t len = 12 + 2 * plane;
    uint16_t g;
    uint8_t *p;
    if (!im->cursor || !k || !(g = kernel_global_alloc(k, len, 0x0002))) return;
    if (!(p = mem_ptr(&ui->u->p->mem, g, 0, len))) {
        kernel_global_free(k, g);
        return;
    }
    memset(p, 0, len);
    p[0] = (uint8_t)im->hot_x; p[1] = (uint8_t)(im->hot_x >> 8);
    p[2] = (uint8_t)im->hot_y; p[3] = (uint8_t)(im->hot_y >> 8);
    p[4] = (uint8_t)im->w; p[5] = (uint8_t)(im->w >> 8);
    p[6] = (uint8_t)im->h; p[7] = (uint8_t)(im->h >> 8);
    p[8] = (uint8_t)cb; p[9] = (uint8_t)(cb >> 8);
    p[10] = 1;
    p[11] = 1;
    for (y = 0; y < (unsigned)im->h; y++)
        for (x = 0; x < (unsigned)im->w; x++) {
            uint32_t c = im->cref[y * (unsigned)im->w + x];
            unsigned lum = ((c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF)) / 3;
            if (!im->mask || im->mask[y * (unsigned)im->w + x]) p[12 + y * cb + x / 8] |= (uint8_t)(0x80 >> (x & 7));
            if (lum >= 128) p[12 + plane + y * cb + x / 8] |= (uint8_t)(0x80 >> (x & 7));
        }
    im->gh = im->handle = g;
    *h = g;
}

bool ui_cursor_info(ui_t *ui, uint16_t h, int *w, int *hh, int *hx, int *hy) {
    ui_image *im = ui_image_get(ui, h);
    if (!im || !im->cursor) return false;
    *w = im->w;
    *hh = im->h;
    *hx = im->hot_x;
    *hy = im->hot_y;
    return true;
}

/* ---- the built-in cursors and icons: drawn with integer geometry and tiny glyph tables ---- */

typedef struct { uint32_t *c; uint8_t *m; } canvas;

static void cv_set(canvas *v, int x, int y, uint32_t col) {
    if (x < 0 || y < 0 || x >= SYS_ICON_SIZE || y >= SYS_ICON_SIZE) return;
    v->c[y * SYS_ICON_SIZE + x] = col;
    v->m[y * SYS_ICON_SIZE + x] = 0;
}

static void cv_art(canvas *v, const char *const *rows, int n, int x0, int y0) {
    int y, x;
    for (y = 0; y < n; y++)
        for (x = 0; rows[y][x]; x++) {
            if (rows[y][x] == 'X') cv_set(v, x0 + x, y0 + y, RGBC(0, 0, 0));
            else if (rows[y][x] == '.') cv_set(v, x0 + x, y0 + y, RGBC(255, 255, 255));
        }
}

static const char *const art_arrow[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ", "X.....X     ",
    "X......X    ", "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ",
    "X    X..X   ", "     X..X   ", "      X..X  ", "      X..X  ", "       XX   "};
static const char *const art_wait[] = {
    "XXXXXXXXX", " X.....X ", "  X...X  ", "   X.X   ", "    X    ", "   X.X   ", "  X...X  ", " X.....X ", "XXXXXXXXX"};
static const char *const art_ibeam[] = {
    " XX XX ", "   X   ", "   X   ", "   X   ", "   X   ", "   X   ", "   X   ", "   X   ",
    "   X   ", "   X   ", "   X   ", "   X   ", "   X   ", "   X   ", "   X   ", " XX XX "};

static bool make_system_cursor(ui_image *im, unsigned id) {
    canvas v;
    int i, n = SYS_ICON_SIZE * SYS_ICON_SIZE;
    im->w = im->h = SYS_ICON_SIZE;
    im->cref = calloc((size_t)n, sizeof *im->cref);
    im->mask = malloc((size_t)n);
    if (!im->cref || !im->mask) return false;
    memset(im->mask, 1, (size_t)n);
    v.c = im->cref;
    v.m = im->mask;
    switch (id) {
    case 32513:
        cv_art(&v, art_ibeam, 16, 12, 8);
        im->hot_x = 15;
        im->hot_y = 16;
        break;
    case 32514:
        cv_art(&v, art_wait, 9, 11, 11);
        im->hot_x = 15;
        im->hot_y = 15;
        break;
    case 32512:
    case 32516:
    case 32641:
        cv_art(&v, art_arrow, 19, 0, 0);
        break;
    default:   /* crosshair and the sizing cursors */
        for (i = 4; i < 28; i++) {
            if (i < 14 || i > 17) {
                cv_set(&v, i, 16, RGBC(0, 0, 0));
                cv_set(&v, 16, i, RGBC(0, 0, 0));
            }
        }
        im->hot_x = im->hot_y = 16;
        break;
    }
    return true;
}

static const char *const glyph_q[7] = {"01110", "10001", "00001", "00110", "00100", "00000", "00100"};
static const char *const glyph_bang[7] = {"00100", "00100", "00100", "00100", "00100", "00000", "00100"};
static const char *const glyph_i[7] = {"00100", "00000", "01100", "00100", "00100", "00100", "01110"};

static void cv_glyph(canvas *v, const char *const *g, int x0, int y0, int scale, uint32_t col) {
    int r, c, dx, dy;
    for (r = 0; r < 7; r++)
        for (c = 0; c < 5; c++)
            if (g[r][c] == '1')
                for (dy = 0; dy < scale; dy++)
                    for (dx = 0; dx < scale; dx++) cv_set(v, x0 + c * scale + dx, y0 + r * scale + dy, col);
}

static bool make_system_icon(ui_image *im, unsigned id) {
    canvas v;
    int x, y, n = SYS_ICON_SIZE * SYS_ICON_SIZE;
    im->w = im->h = SYS_ICON_SIZE;
    im->cref = calloc((size_t)n, sizeof *im->cref);
    im->mask = malloc((size_t)n);
    if (!im->cref || !im->mask) return false;
    memset(im->mask, 1, (size_t)n);
    v.c = im->cref;
    v.m = im->mask;
    for (y = 0; y < SYS_ICON_SIZE; y++)
        for (x = 0; x < SYS_ICON_SIZE; x++) {
            int dx = 2 * x - 31, dy = 2 * y - 31, d2 = dx * dx + dy * dy;   /* doubled coordinates around the centre */
            switch (id) {
            case 32513:                                   /* IDI_HAND: red disc, white cross */
                if (d2 <= 29 * 29) cv_set(&v, x, y, d2 > 26 * 26 ? RGBC(128, 0, 0) : RGBC(255, 0, 0));
                if (d2 <= 26 * 26 && abs(x - y) <= 1 && x >= 9 && x <= 22) cv_set(&v, x, y, RGBC(255, 255, 255));
                if (d2 <= 26 * 26 && abs(x + y - 31) <= 1 && x >= 9 && x <= 22) cv_set(&v, x, y, RGBC(255, 255, 255));
                break;
            case 32514:                                   /* IDI_QUESTION: white bubble, blue ? */
                if (d2 <= 29 * 29) cv_set(&v, x, y, d2 > 26 * 26 ? RGBC(0, 0, 0) : RGBC(255, 255, 255));
                break;
            case 32515:                                   /* IDI_EXCLAMATION: yellow triangle */
                if (y >= 2 && y <= 29 && abs(2 * x - 31) <= (y - 1) + 1) {
                    bool edge = y >= 28 || abs(2 * x - 31) >= (y - 1) - 1;
                    cv_set(&v, x, y, edge ? RGBC(0, 0, 0) : RGBC(255, 255, 0));
                }
                break;
            case 32516:                                   /* IDI_ASTERISK: white disc, blue i */
                if (d2 <= 29 * 29) cv_set(&v, x, y, d2 > 26 * 26 ? RGBC(0, 0, 128) : RGBC(255, 255, 255));
                break;
            default:                                      /* IDI_APPLICATION: a small window */
                if (x >= 3 && x <= 28 && y >= 5 && y <= 26) {
                    bool edge = x == 3 || x == 28 || y == 5 || y == 26;
                    cv_set(&v, x, y, edge ? RGBC(0, 0, 0) : y <= 9 ? RGBC(0, 0, 128) : RGBC(255, 255, 255));
                }
                break;
            }
        }
    if (id == 32514) cv_glyph(&v, glyph_q, 8, 6, 3, RGBC(0, 0, 255));
    if (id == 32515) cv_glyph(&v, glyph_bang, 9, 8, 3, RGBC(0, 0, 0));
    if (id == 32516) cv_glyph(&v, glyph_i, 9, 6, 3, RGBC(0, 0, 128));
    return true;
}

static ui_image *system_image(ui_t *ui, bool cursor, unsigned id, uint16_t *h) {
    unsigned i;
    ui_image *im;
    if (cursor ? (id < IDC_FIRST || (id > 32516 && (id < 32640 || id > 32645))) : (id < IDC_FIRST || id > 32516)) return NULL;
    for (i = 0; i < UI_MAX_IMAGES; i++) {
        ui_image *c = &ui->img[i];
        if (c->used && c->system && c->cursor == cursor && c->key == id) {
            *h = c->handle;
            return c;
        }
    }
    if (!(im = image_slot(ui, h))) return NULL;
    im->system = true;
    im->cursor = cursor;
    im->key = id;
    if (!(cursor ? make_system_cursor(im, id) : make_system_icon(im, id))) {
        image_free(ui, im);
        return NULL;
    }
    image_shape(ui, im, h);
    return im;
}

/* ---- loading from the program's resources ---------------------------------------------------- */

static ui_image *find_cached(ui_t *ui, bool cursor, uint16_t hinst, uint32_t key, uint16_t *h) {
    unsigned i;
    for (i = 0; i < UI_MAX_IMAGES; i++) {
        ui_image *c = &ui->img[i];
        if (c->used && !c->system && c->cursor == cursor && c->hinst == hinst && c->key == key) {
            *h = c->handle;
            return c;
        }
    }
    return NULL;
}

static uint16_t load_image_core(user_t *u, bool cursor, uint16_t inst, const resname *rnp) {
    uint16_t h = 0;
    resname rn = *rnp;
    res_err e;
    const uint8_t *grp, *dat;
    size_t gl, dl;
    res_group_pick pk;
    res_image img;
    ui_image *im;
    int hx = 0, hy = 0;
    if (inst == 0) return system_image(u->ui, cursor, rn.id, &h) ? h : 0;
    if (inst != u->p->auto_ds) return 0;
    if (find_cached(u->ui, cursor, inst, name_key(&rn), &h)) return h;
    if (res_find(u->p->ne, cursor ? RT_GROUP_CURSOR : RT_GROUP_ICON, rn.by_name, rn.id, rn.name, &grp, &gl) != RES_OK) return 0;
    if (res_group_pick_best(grp, gl, cursor, 32, 32, &pk) != RES_OK) return 0;
    if (res_find(u->p->ne, cursor ? RT_CURSOR : RT_ICON, false, pk.id, NULL, &dat, &dl) != RES_OK) return 0;
    e = cursor ? res_cursor_decode(dat, dl, &img, &hx, &hy) : res_icon_decode(dat, dl, &img);
    if (e != RES_OK) return 0;
    if (!(im = image_slot(u->ui, &h))) {
        res_image_free(&img);
        return 0;
    }
    im->cursor = cursor;
    im->hinst = inst;
    im->key = name_key(&rn);
    im->w = img.w;
    im->h = img.h;
    im->hot_x = hx;
    im->hot_y = hy;
    im->cref = img.cref;
    im->mask = img.mask;
    image_shape(u->ui, im, &h);
    user_log(u, "%s %s -> %04x (%dx%d)", cursor ? "LoadCursor" : "LoadIcon", rn.by_name ? rn.name : "#id", h, im->w, im->h);
    return h;
}

static uint16_t load_image(hle_call *c, bool cursor) {
    UBEGIN;
    uint16_t inst = hle_u16(&a), sel, off;
    resname rn;
    hle_far(&a, &sel, &off);
    if (a.st || !resname_arg(c, sel, off, &rn)) return 0;
    return load_image_core(u, cursor, inst, &rn);
}

/* An icon named by a window text: "#n" is the integer id n. */
uint16_t ui_icon_from_text(user_t *u, uint16_t hinst, const char *text) {
    resname rn;
    memset(&rn, 0, sizeof rn);
    if (text[0] == '#') {
        rn.id = (unsigned)atoi(text + 1);
        if (rn.id >= 32512 && rn.id <= 32516) hinst = 0;       /* IDI_*: the system's icons */
    } else {
        rn.by_name = true;
        strncpy(rn.name, text, sizeof rn.name - 1);
    }
    return load_image_core(u, false, hinst, &rn);
}

static cpu_status u_loadicon(hle_call *c) {
    uint16_t h = load_image(c, false);
    hle_ret16(c, h);
    return ((user_t *)c->ctx)->abort;
}

static cpu_status u_loadcursor(hle_call *c) {
    uint16_t h = load_image(c, true);
    hle_ret16(c, h);
    return ((user_t *)c->ctx)->abort;
}

static cpu_status u_loadbitmap(hle_call *c) {
    UBEGIN;
    uint16_t inst = hle_u16(&a), sel, off;
    resname rn;
    const uint8_t *d;
    size_t dl;
    res_image img;
    uint16_t hb;
    hle_far(&a, &sel, &off);
    UCHECK;
    hle_ret16(c, 0);
    if (!resname_arg(c, sel, off, &rn) || inst != u->p->auto_ds || !u->has_gfx) return CPU_OK;
    if (res_find(u->p->ne, RT_BITMAP, rn.by_name, rn.id, rn.name, &d, &dl) != RES_OK) return CPU_OK;
    if (res_dib_decode(d, dl, false, &img) != RES_OK) return CPU_OK;
    hb = u->gfx.bitmap_create(u->gfx.ctx, img.w, img.h, img.bpp == 1, img.cref);
    res_image_free(&img);
    hle_ret16(c, hb);
    return CPU_OK;
}

/* ---- drawing, destroying, the current cursor ------------------------------------------------- */

void ui_draw_image(user_t *u, uint16_t hdc, int x, int y, const ui_image *im) {
    int r, cx;
    if (!u->has_gfx) return;
    for (r = 0; r < im->h; r++) {
        for (cx = 0; cx < im->w;) {
            size_t k = (size_t)r * (size_t)im->w + (size_t)cx;
            uint32_t col = im->cref[k];
            bool opaque = im->mask ? im->mask[k] == 0 : true;
            bool invert = !opaque && col != 0;
            int e = cx + 1;
            fb_rect q;
            if (!opaque && !invert) {
                cx++;
                continue;
            }
            while (e < im->w) {
                size_t k2 = (size_t)r * (size_t)im->w + (size_t)e;
                bool o2 = im->mask ? im->mask[k2] == 0 : true;
                bool i2 = !o2 && im->cref[k2] != 0;
                if (opaque ? !(o2 && im->cref[k2] == col) : !i2) break;
                e++;
            }
            q.left = x + cx;
            q.right = x + e;
            q.top = y + r;
            q.bottom = y + r + 1;
            if (opaque) u->gfx.fill(u->gfx.ctx, hdc, &q, col);
            else u->gfx.invert(u->gfx.ctx, hdc, &q);
            cx = e;
        }
    }
}

static cpu_status u_drawicon(hle_call *c) {
    UBEGIN;
    uint16_t hdc = hle_u16(&a);
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a);
    uint16_t hi = hle_u16(&a);
    ui_image *im;
    UCHECK;
    im = ui_image_get(u->ui, hi);
    if (u->has_gfx && u->gfx.to_device) u->gfx.to_device(u->gfx.ctx, hdc, &x, &y);   /* DrawIcon takes logical coordinates */
    if (im && !im->cursor) ui_draw_image(u, hdc, x, y, im);
    hle_ret16(c, im != NULL && !im->cursor);
    return CPU_OK;
}

static cpu_status destroy_image(hle_call *c, bool cursor) {
    UBEGIN;
    uint16_t h = hle_u16(&a);
    ui_image *im;
    UCHECK;
    im = ui_image_get(u->ui, h);
    if (im && im->cursor == cursor) {
        if (!im->system) image_free(u->ui, im);
        hle_ret16(c, 1);
    } else {
        hle_ret16(c, 0);
    }
    return CPU_OK;
}

static cpu_status u_destroyicon(hle_call *c) { return destroy_image(c, false); }
static cpu_status u_destroycursor(hle_call *c) { return destroy_image(c, true); }

static cpu_status u_getcursor(hle_call *c) {
    user_t *u = c->ctx;
    hle_ret16(c, u->wm.cursor);
    return CPU_OK;
}

/* ---- accelerators ------------------------------------------------------------------------------------- */

static cpu_status u_loadaccelerators(hle_call *c) {
    UBEGIN;
    uint16_t inst = hle_u16(&a), sel, off;
    resname rn;
    const uint8_t *d;
    size_t dl, n = 0, i;
    res_accel *e;
    uint32_t key;
    hle_far(&a, &sel, &off);
    UCHECK;
    hle_ret16(c, 0);
    if (inst != u->p->auto_ds || !resname_arg(c, sel, off, &rn)) return CPU_OK;
    key = name_key(&rn);
    for (i = 0; i < UI_MAX_ACCEL; i++)
        if (u->ui->acc[i].used && u->ui->acc[i].hinst == inst && u->ui->acc[i].key == key) {
            hle_ret16(c, (uint16_t)(UI_ACCEL_BASE + i));
            return CPU_OK;
        }
    if (res_find(u->p->ne, RT_ACCELERATOR, rn.by_name, rn.id, rn.name, &d, &dl) != RES_OK) return CPU_OK;
    if (!(e = calloc(dl / 5 + 1, sizeof *e))) return CPU_OK;
    if (res_accel_parse(d, dl, e, dl / 5 + 1, &n) != RES_OK) {
        free(e);
        return CPU_OK;
    }
    for (i = 0; i < UI_MAX_ACCEL; i++)
        if (!u->ui->acc[i].used) {
            u->ui->acc[i].used = true;
            u->ui->acc[i].e = e;
            u->ui->acc[i].n = n;
            u->ui->acc[i].hinst = inst;
            u->ui->acc[i].key = key;
            hle_ret16(c, (uint16_t)(UI_ACCEL_BASE + i));
            return CPU_OK;
        }
    free(e);
    return CPU_OK;
}

/* Sends WM_INITMENU and the WM_INITMENUPOPUPs on the way from the bar to the popup that holds the item. */
static bool init_path(user_t *u, uint16_t hwnd, uint16_t menu, uint16_t target, int depth) {
    ui_menu *m = ui_menu_get(u->ui, menu);
    int i;
    if (!m || depth > 8) return false;
    for (i = 0; i < m->n; i++) {
        if (!(m->it[i].flags & MF_POPUP)) continue;
        if (m->it[i].id == target || init_path(u, hwnd, m->it[i].id, target, depth + 1)) {
            wm_send(&u->wm, hwnd, WM_INITMENUPOPUP, m->it[i].id, (uint16_t)i);
            return true;
        }
    }
    return false;
}

static cpu_status u_translateaccelerator(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a), hacc = hle_u16(&a), sel, off;
    const uint8_t *p;
    uint16_t msg, wp;
    unsigned ai = (unsigned)hacc - UI_ACCEL_BASE;
    ui_accel *ac;
    size_t i;
    hle_far(&a, &sel, &off);
    UCHECK;
    hle_ret16(c, 0);
    if (!(p = hle_ptr(c, sel, off, 18))) return CPU_E_SEL;
    msg = ld16(p + 2);
    wp = ld16(p + 4);
    if (hacc < UI_ACCEL_BASE || ai >= UI_MAX_ACCEL || !u->ui->acc[ai].used || !wm_is_window(&u->wm, hwnd)) return CPU_OK;
    ac = &u->ui->acc[ai];
    {
        bool shift = (wm_key_state(&u->wm, VK_SHIFT) & 0x8000) != 0, ctrl = (wm_key_state(&u->wm, VK_CONTROL) & 0x8000) != 0,
             alt = (wm_key_state(&u->wm, VK_MENU) & 0x8000) != 0;
        bool is_key = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, is_char = msg == WM_CHAR || msg == WM_SYSCHAR;
        for (i = 0; i < ac->n; i++) {
            const res_accel *e = &ac->e[i];
            wm_window *x;
            ui_menu *bar;
            uint16_t mh;
            int idx;
            bool match;
            if (e->flags & AF_VIRTKEY)
                match = is_key && wp == e->key && shift == ((e->flags & AF_SHIFT) != 0) && ctrl == ((e->flags & AF_CONTROL) != 0) &&
                        alt == ((e->flags & AF_ALT) != 0);
            else
                match = is_char && wp == e->key && (msg == WM_SYSCHAR) == ((e->flags & AF_ALT) != 0);
            if (!match) continue;
            hle_ret16(c, 1);
            if (!wm_is_enabled(&u->wm, hwnd)) return CPU_OK;
            if (e->cmd >= 0xF000) {
                wm_send(&u->wm, hwnd, WM_SYSCOMMAND, e->cmd, 0);
                return u->abort;
            }
            x = wm_get(&u->wm, hwnd);
            bar = x && !(x->style & WS_CHILD) ? ui_menu_get(u->ui, x->id) : NULL;
            if (bar) {
                wm_send(&u->wm, hwnd, WM_INITMENU, x->id, 0);
                if (u->abort) return u->abort;
                if (ui_menu_find(u->ui, x->id, e->cmd, 0, &mh, &idx)) {
                    const ui_item *it;
                    if (mh != x->id) init_path(u, hwnd, x->id, mh, 0);
                    if (u->abort) return u->abort;
                    if (!wm_is_window(&u->wm, hwnd)) return CPU_OK;
                    it = &ui_menu_get(u->ui, mh)->it[idx];
                    if (it->flags & (MF_GRAYED | MF_DISABLED)) return CPU_OK;     /* swallowed: the item is off */
                }
            }
            wm_send(&u->wm, hwnd, WM_COMMAND, e->cmd, 1u << 16);
            return u->abort;
        }
    }
    return CPU_OK;
}

/* SHELL.34 ExtractIcon(hInst, lpszExeFileName, nIconIndex): the icon of another program's file (KIDSWTCH draws the other
 * MSKids programs in its task switcher). The nIconIndex-th icon group of the NE file; 0xFFFF asks for the count. 0: no
 * such icon, 1: not a Windows executable. */
static cpu_status shell_extracticon(hle_call *c) {
    UBEGIN;
    uint16_t inst = hle_u16(&a), sel, off, idx;
    char path[260], host[512];
    ne_image ne;
    const ne_restype *rt = NULL;
    size_t t;
    uint16_t h = 0;
    kernel_t *k;
    (void)inst;
    hle_far(&a, &sel, &off);
    idx = hle_u16(&a);
    if (a.st) return a.st;
    k = ui_kernel(u);
    if (!hle_read_cstr(c, sel, off, path, sizeof path) || !k || !kernel_host_path(k, path, host, sizeof host)) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (ne_open_file(&ne, host) != NE_OK) {
        hle_ret16(c, 1);
        return CPU_OK;
    }
    for (t = 0; t < ne.restype_count; t++)
        if (!ne.restypes[t].name[0] && ne.restypes[t].id == RT_GROUP_ICON) rt = &ne.restypes[t];
    if (rt && idx == 0xFFFF) {
        h = rt->count;
    } else if (rt && idx < rt->count) {
        uint32_t key = 0x811C9DC5u;
        const char *p;
        res_group_pick pk;
        res_image img;
        const uint8_t *dat;
        size_t dl;
        ui_image *im;
        for (p = host; *p; p++) key = (key ^ (unsigned char)*p) * 16777619u;
        key = (key ^ idx) * 16777619u;
        if (find_cached(u->ui, false, 0xFFFE, key, &h)) {
            ne_free(&ne);
            hle_ret16(c, h);
            return CPU_OK;
        }
        h = 0;
        if (res_group_pick_best(ne.data + rt->res[idx].offset, rt->res[idx].length, false, 32, 32, &pk) == RES_OK &&
            res_find(&ne, RT_ICON, false, pk.id, NULL, &dat, &dl) == RES_OK && res_icon_decode(dat, dl, &img) == RES_OK) {
            if ((im = image_slot(u->ui, &h))) {
                im->hinst = 0xFFFE;
                im->key = key;
                im->w = img.w;
                im->h = img.h;
                im->cref = img.cref;
                im->mask = img.mask;
                image_shape(u->ui, im, &h);
                user_log(u, "ExtractIcon %s #%u -> %04x (%dx%d)", path, idx, h, im->w, im->h);
            } else {
                res_image_free(&img);
            }
        }
    }
    ne_free(&ne);
    hle_ret16(c, h);
    return CPU_OK;
}

const hle_def ui_res_defs[] = {
    {"SHELL", 34, "ExtractIcon", 8, shell_extracticon},            /* UNV: by the Win 3.1 export list */
    {USER_MODULE, 84, "DrawIcon", 8, u_drawicon},                  /* UNV */
    {USER_MODULE, 173, "LoadCursor", 6, u_loadcursor},
    {USER_MODULE, 174, "LoadIcon", 6, u_loadicon},
    {USER_MODULE, 175, "LoadBitmap", 6, u_loadbitmap},             /* UNV */
    {USER_MODULE, 176, "LoadString", 10, u_loadstring},
    {USER_MODULE, 177, "LoadAccelerators", 6, u_loadaccelerators},
    {USER_MODULE, 178, "TranslateAccelerator", 8, u_translateaccelerator},
    {USER_MODULE, 247, "GetCursor", 0, u_getcursor},               /* UNV */
    {USER_MODULE, 457, "DestroyIcon", 2, u_destroyicon},
    {USER_MODULE, 458, "DestroyCursor", 2, u_destroycursor},
};
const size_t ui_res_ndefs = sizeof ui_res_defs / sizeof ui_res_defs[0];

/* ---- KEYBOARD: AnsiToOem, OemToAnsi (code page 437 <-> Windows Latin-1) -------------------- */

/* Latin-1 equivalent of OEM 0x80..0xFF; 0 where there is none (the byte is kept). */
static const uint8_t oem_to_latin[128] = {
    0xC7, 0xFC, 0xE9, 0xE2, 0xE4, 0xE0, 0xE5, 0xE7, 0xEA, 0xEB, 0xE8, 0xEF, 0xEE, 0xEC, 0xC4, 0xC5,
    0xC9, 0xE6, 0xC6, 0xF4, 0xF6, 0xF2, 0xFB, 0xF9, 0xFF, 0xD6, 0xDC, 0xA2, 0xA3, 0xA5, 0x00, 0x83,
    0xE1, 0xED, 0xF3, 0xFA, 0xF1, 0xD1, 0xAA, 0xBA, 0xBF, 0x00, 0xAC, 0xBD, 0xBC, 0xA1, 0xAB, 0xBB,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xDF, 0x00, 0x00, 0x00, 0x00, 0xB5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xB1, 0x00, 0x00, 0x00, 0x00, 0xF7, 0x00, 0xB0, 0xB7, 0xB7, 0x00, 0x6E, 0xB2, 0x00, 0xA0};

static uint8_t ansi_to_oem_byte(uint8_t ch) {
    unsigned i;
    if (ch < 0x80) return ch;
    for (i = 0; i < 128; i++)
        if (oem_to_latin[i] == ch) return (uint8_t)(0x80 + i);
    return ch;
}

static uint8_t oem_to_ansi_byte(uint8_t ch) {
    return ch >= 0x80 && oem_to_latin[ch - 0x80] ? oem_to_latin[ch - 0x80] : ch;
}

static cpu_status translate(hle_call *c, bool to_oem, bool counted) {
    UBEGIN;
    uint16_t ssel, soff, dsel, doff, n = 0;
    const uint8_t *src;
    uint8_t *dst;
    size_t len, i;
    char *str;
    hle_far(&a, &ssel, &soff);
    hle_far(&a, &dsel, &doff);
    if (counted) n = hle_u16(&a);
    UCHECK;
    if (counted) {
        len = n ? n : 0x10000;
        if (!(src = hle_ptr(c, ssel, soff, (uint32_t)len))) return CPU_E_SEL;
    } else {
        if (!kernel_guest_str(c, ssel, soff, &str, &len)) return CPU_E_SEL;
        src = (const uint8_t *)str;
        len++;                          /* the terminator comes along */
    }
    if (!(dst = hle_ptr(c, dsel, doff, (uint32_t)len))) return CPU_E_SEL;
    for (i = 0; i < len; i++) {
        uint8_t ch = src[i];   /* read first: source and destination may be the same string */
        dst[i] = to_oem ? ansi_to_oem_byte(ch) : oem_to_ansi_byte(ch);
    }
    hle_ret32(c, 1);
    return CPU_OK;
}

static cpu_status k_ansitooem(hle_call *c) { return translate(c, true, false); }
static cpu_status k_oemtoansi(hle_call *c) { return translate(c, false, false); }
static cpu_status k_ansitooembuff(hle_call *c) { return translate(c, true, true); }
static cpu_status k_oemtoansibuff(hle_call *c) { return translate(c, false, true); }
static cpu_status k_getkbcodepage(hle_call *c) { hle_ret16(c, 437); return CPU_OK; }

const hle_def ui_kbd_defs[] = {
    {"KEYBOARD", 5, "AnsiToOem", 8, k_ansitooem},
    {"KEYBOARD", 6, "OemToAnsi", 8, k_oemtoansi},
    {"KEYBOARD", 135, "GetKBCodePage", 0, k_getkbcodepage},         /* WRITER imports it; name UNVERIFIED */
    {"KEYBOARD", 137, "AnsiToOemBuff", 10, k_ansitooembuff},        /* UNV */
    {"KEYBOARD", 138, "OemToAnsiBuff", 10, k_oemtoansibuff},        /* UNV */
};
const size_t ui_kbd_ndefs = sizeof ui_kbd_defs / sizeof ui_kbd_defs[0];

