/* The clipboard (host-side storage), carets, InvalidateRgn / ValidateRgn / GetWindowDC, and COMMDLG.
 *
 * Clipboard. Formats and their data live on the host. SetClipboardData copies the bytes of the program's global
 * block (its size comes from KERNEL's table); GetClipboardData hands out one global block per format, allocated
 * through KERNEL, until the clipboard is emptied. CF_BITMAP and CF_PALETTE keep the GDI handle.
 *
 * COMMDLG is not a loaded DLL: the imports WRITER and ARTIST use (GetSaveFileName, PrintDlg,
 * CommDlgExtendedError) are answered as a cancelled dialog without an error. The real dialogs are not provided. */
#include <stdlib.h>
#include <string.h>

#include "ui_priv.h"

#define CF_TEXT 1
#define CF_BITMAP 2
#define CF_METAFILEPICT 3
#define CF_PALETTE 9
#define WM_DESTROYCLIPBOARD 0x0307
#define WM_RENDERFORMAT 0x0305
#define WM_DRAWCLIPBOARD 0x0308

kernel_t *ui_kernel(user_t *u) {
    const hle_entry *e = hle_find(u->hle, "KERNEL", 15);     /* GlobalAlloc's context is the kernel */
    return e ? e->ctx : NULL;
}

static ui_clip_fmt *clip_find(ui_t *ui, unsigned fmt) {
    int i;
    for (i = 0; i < ui->clip.n; i++)
        if (ui->clip.f[i].fmt == fmt) return &ui->clip.f[i];
    return NULL;
}

static void clip_free_fmt(user_t *u, ui_clip_fmt *f) {
    kernel_t *k = ui_kernel(u);
    free(f->data);
    if (f->hmem && k) kernel_global_free(k, f->hmem);
    memset(f, 0, sizeof *f);
}

static void clip_empty(user_t *u) {
    int i;
    for (i = 0; i < u->ui->clip.n; i++) clip_free_fmt(u, &u->ui->clip.f[i]);
    u->ui->clip.n = 0;
}

static ui_clip_fmt *clip_add(user_t *u, unsigned fmt) {
    ui_clip *c = &u->ui->clip;
    ui_clip_fmt *f = clip_find(u->ui, fmt);
    if (f) {
        clip_free_fmt(u, f);
    } else {
        if (c->n >= UI_CLIP_MAX) return NULL;
        f = &c->f[c->n++];
    }
    memset(f, 0, sizeof *f);
    f->fmt = fmt;
    return f;
}

/* Text for the EDIT control, without a program in the middle. */
void ui_clip_set_text(user_t *u, const char *s, int n) {
    ui_clip_fmt *f;
    clip_empty(u);
    if (!(f = clip_add(u, CF_TEXT))) return;
    f->data = malloc((size_t)n + 1);
    if (!f->data) {
        u->ui->clip.n = 0;
        return;
    }
    memcpy(f->data, s, (size_t)n);
    f->data[n] = 0;
    f->len = (size_t)n + 1;
}

char *ui_clip_get_text(user_t *u) {
    ui_clip_fmt *f = clip_find(u->ui, CF_TEXT);
    if (!f || !f->data) return NULL;
    return ui_strdup((const char *)f->data);
}

static cpu_status u_openclipboard(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a);
    UCHECK;
    if (u->ui->clip.open_by && u->ui->clip.open_by != hwnd) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    u->ui->clip.open_by = hwnd ? hwnd : 0xFFFF;
    u->ui->clip.changed = false;
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status u_closeclipboard(hle_call *c) {
    user_t *u = c->ctx;
    ui_clip *cl = &u->ui->clip;
    bool ok = cl->open_by != 0;
    cl->open_by = 0;
    if (ok && cl->changed && cl->viewer && wm_is_window(&u->wm, cl->viewer)) wm_send(&u->wm, cl->viewer, WM_DRAWCLIPBOARD, 0, 0);
    cl->changed = false;
    hle_ret16(c, ok);
    return u->abort;
}

static cpu_status u_emptyclipboard(hle_call *c) {
    user_t *u = c->ctx;
    ui_clip *cl = &u->ui->clip;
    uint16_t old = cl->owner;
    if (!cl->open_by) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (old && wm_is_window(&u->wm, old)) wm_send(&u->wm, old, WM_DESTROYCLIPBOARD, 0, 0);
    clip_empty(u);
    cl->owner = cl->open_by == 0xFFFF ? 0 : cl->open_by;
    cl->changed = true;
    hle_ret16(c, 1);
    return u->abort;
}

static cpu_status u_setclipboarddata(hle_call *c) {
    UBEGIN;
    uint16_t fmt = hle_u16(&a), h = hle_u16(&a);
    ui_clip_fmt *f;
    kernel_t *k = ui_kernel(u);
    UCHECK;
    if (!u->ui->clip.open_by) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (!(f = clip_add(u, fmt))) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    f->program_handle = h;
    f->delayed = h == 0;
    if (h && fmt != CF_BITMAP && fmt != CF_PALETTE && fmt != CF_METAFILEPICT) {
        uint32_t size = k && (h >> 3) < MEM_MAX_SEL_ ? k->gb[h >> 3].size : 0;
        const uint8_t *p;
        if (!size) size = mem_size(&u->p->mem, h);
        if (!(p = mem_ptr(&u->p->mem, h, 0, size ? size : 1)) || !(f->data = malloc(size ? size : 1))) {
            f->program_handle = 0;
            hle_ret16(c, 0);
            return CPU_OK;
        }
        memcpy(f->data, p, size);
        f->len = size;
    }
    u->ui->clip.changed = true;
    hle_ret16(c, h);
    return CPU_OK;
}

static cpu_status u_getclipboarddata(hle_call *c) {
    UBEGIN;
    uint16_t fmt = hle_u16(&a);
    ui_clip_fmt *f;
    kernel_t *k = ui_kernel(u);
    UCHECK;
    hle_ret16(c, 0);
    if (!u->ui->clip.open_by || !(f = clip_find(u->ui, fmt))) return CPU_OK;
    if (f->delayed && u->ui->clip.owner && wm_is_window(&u->wm, u->ui->clip.owner)) {
        f->delayed = false;                                  /* the owner renders it now, through SetClipboardData */
        wm_send(&u->wm, u->ui->clip.owner, WM_RENDERFORMAT, fmt, 0);
        if (u->abort) return u->abort;
        f = clip_find(u->ui, fmt);
        if (!f) return CPU_OK;
    }
    if (fmt == CF_BITMAP || fmt == CF_PALETTE || fmt == CF_METAFILEPICT) {
        hle_ret16(c, f->program_handle);
        return CPU_OK;
    }
    if (!f->data || !k) return CPU_OK;
    if (!f->hmem) {
        uint8_t *p;
        f->hmem = kernel_global_alloc(k, (uint32_t)f->len, 0x0002);
        if (!f->hmem || !(p = mem_ptr(&u->p->mem, f->hmem, 0, (uint32_t)f->len ? (uint32_t)f->len : 1))) return CPU_OK;
        memcpy(p, f->data, f->len);
    }
    hle_ret16(c, f->hmem);
    return CPU_OK;
}

static cpu_status u_isclipboardformatavailable(hle_call *c) {
    UBEGIN;
    uint16_t fmt = hle_u16(&a);
    UCHECK;
    hle_ret16(c, clip_find(u->ui, fmt) != NULL);
    return CPU_OK;
}

static cpu_status u_countclipboardformats(hle_call *c) {
    user_t *u = c->ctx;
    hle_ret16(c, (uint16_t)u->ui->clip.n);
    return CPU_OK;
}

static cpu_status u_enumclipboardformats(hle_call *c) {
    UBEGIN;
    uint16_t fmt = hle_u16(&a);
    int i;
    UCHECK;
    hle_ret16(c, 0);
    if (!u->ui->clip.open_by) return CPU_OK;
    if (fmt == 0) {
        if (u->ui->clip.n) hle_ret16(c, (uint16_t)u->ui->clip.f[0].fmt);
        return CPU_OK;
    }
    for (i = 0; i + 1 < u->ui->clip.n; i++)
        if (u->ui->clip.f[i].fmt == fmt) {
            hle_ret16(c, (uint16_t)u->ui->clip.f[i + 1].fmt);
            break;
        }
    return CPU_OK;
}

static cpu_status u_registerclipboardformat(hle_call *c) {
    UBEGIN;
    uint16_t sel, off;
    char name[64];
    int i;
    hle_far(&a, &sel, &off);
    UCHECK;
    hle_ret16(c, 0);
    if (!sel || !hle_read_cstr(c, sel, off, name, sizeof name)) return CPU_OK;
    for (i = 0; i < u->ui->clip.nreg; i++)
        if (!strcmp(u->ui->clip.reg[i], name)) break;
    if (i == u->ui->clip.nreg) {
        if (i == UI_CLIP_REG) return CPU_OK;
        strcpy(u->ui->clip.reg[i], name);
        u->ui->clip.nreg++;
    }
    hle_ret16(c, (uint16_t)(0xC000 + i));
    return CPU_OK;
}

static cpu_status u_getclipboardformatname(hle_call *c) {
    UBEGIN;
    uint16_t fmt = hle_u16(&a), sel, off, max;
    unsigned i = (unsigned)fmt - 0xC000u;
    uint8_t *d;
    hle_far(&a, &sel, &off);
    max = hle_u16(&a);
    UCHECK;
    hle_ret16(c, 0);
    if (fmt >= 0xC000 && i < (unsigned)u->ui->clip.nreg && max > 0) {
        size_t n = strlen(u->ui->clip.reg[i]);
        if (n > (size_t)max - 1) n = (size_t)max - 1;
        if (!(d = hle_ptr(c, sel, off, (uint32_t)n + 1))) return CPU_E_SEL;
        memcpy(d, u->ui->clip.reg[i], n);
        d[n] = 0;
        hle_ret16(c, (uint16_t)n);
    }
    return CPU_OK;
}

static cpu_status u_getclipboardowner(hle_call *c) {
    user_t *u = c->ctx;
    hle_ret16(c, u->ui->clip.owner);
    return CPU_OK;
}

static cpu_status u_getopenclipboardwindow(hle_call *c) {
    user_t *u = c->ctx;
    hle_ret16(c, u->ui->clip.open_by == 0xFFFF ? 0 : u->ui->clip.open_by);
    return CPU_OK;
}

static cpu_status u_setclipboardviewer(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a);
    UCHECK;
    hle_ret16(c, u->ui->clip.viewer);
    u->ui->clip.viewer = h;
    return CPU_OK;
}

static cpu_status u_getclipboardviewer(hle_call *c) {
    user_t *u = c->ctx;
    hle_ret16(c, u->ui->clip.viewer);
    return CPU_OK;
}

static cpu_status u_changeclipboardchain(hle_call *c) {
    UBEGIN;
    uint16_t h = hle_u16(&a), next = hle_u16(&a);
    UCHECK;
    if (u->ui->clip.viewer == h) u->ui->clip.viewer = next;
    hle_ret16(c, 1);
    return CPU_OK;
}

/* ---- carets ---------------------------------------------------------------------------------------------- */

static void caret_draw(user_t *u) {
    ui_caret *cr = &u->ui->caret;
    uint16_t dc;
    fb_rect r;
    if (!cr->hwnd || !u->has_gfx || !wm_is_visible(&u->wm, cr->hwnd) || !(dc = wm_get_dc(&u->wm, cr->hwnd))) return;
    r = (fb_rect){cr->x, cr->y, cr->x + cr->w, cr->y + cr->h};
    u->gfx.invert(u->gfx.ctx, dc, &r);
    wm_release_dc(&u->wm, cr->hwnd, dc);
    cr->drawn = !cr->drawn;
}

static void caret_hide_visual(user_t *u) {
    if (u->ui->caret.drawn) caret_draw(u);
}

static void caret_show_visual(user_t *u) {
    ui_caret *cr = &u->ui->caret;
    if (cr->hwnd && cr->hide == 0 && !cr->drawn) caret_draw(u);
}

static cpu_status u_createcaret(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a), hbm = hle_u16(&a);
    int w = (int16_t)hle_u16(&a), h = (int16_t)hle_u16(&a);
    UCHECK;
    caret_hide_visual(u);
    u->ui->caret.hwnd = wm_is_window(&u->wm, hwnd) ? hwnd : 0;
    u->ui->caret.w = w > 0 ? w : 1;
    u->ui->caret.h = h > 0 ? h : 1;
    u->ui->caret.x = u->ui->caret.y = 0;
    u->ui->caret.hide = 1;                                      /* created hidden */
    (void)hbm;
    hle_ret16(c, u->ui->caret.hwnd != 0);
    return CPU_OK;
}

static cpu_status u_destroycaret(hle_call *c) {
    user_t *u = c->ctx;
    caret_hide_visual(u);
    hle_ret16(c, u->ui->caret.hwnd != 0);
    memset(&u->ui->caret, 0, sizeof u->ui->caret);
    return CPU_OK;
}

static cpu_status u_setcaretpos(hle_call *c) {
    UBEGIN;
    int x = (int16_t)hle_u16(&a), y = (int16_t)hle_u16(&a);
    UCHECK;
    if (!u->ui->caret.hwnd) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    caret_hide_visual(u);
    u->ui->caret.x = x;
    u->ui->caret.y = y;
    caret_show_visual(u);
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status u_getcaretpos(hle_call *c) {
    UBEGIN;
    uint16_t sel, off;
    uint8_t *p;
    hle_far(&a, &sel, &off);
    UCHECK;
    if (!(p = hle_ptr(c, sel, off, 4))) return CPU_E_SEL;
    st16(p, (uint16_t)u->ui->caret.x);
    st16(p + 2, (uint16_t)u->ui->caret.y);
    return CPU_OK;
}

static cpu_status u_hidecaret(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a);
    UCHECK;
    if (u->ui->caret.hwnd && (hwnd == 0 || hwnd == u->ui->caret.hwnd)) {
        caret_hide_visual(u);
        u->ui->caret.hide++;
    }
    return CPU_OK;
}

static cpu_status u_showcaret(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a);
    UCHECK;
    if (u->ui->caret.hwnd && (hwnd == 0 || hwnd == u->ui->caret.hwnd) && u->ui->caret.hide > 0) {
        u->ui->caret.hide--;
        caret_show_visual(u);
    }
    return CPU_OK;
}

static cpu_status u_setcaretblinktime(hle_call *c) {
    UBEGIN;
    u->ui->caret.blink = hle_u16(&a);
    UCHECK;
    hle_ret16(c, 1);
    return CPU_OK;
}

bool ui_caret_state(ui_t *ui, uint16_t *hwnd, int *x, int *y, int *w, int *h, bool *visible) {
    if (!ui->caret.hwnd) return false;
    *hwnd = ui->caret.hwnd;
    *x = ui->caret.x;
    *y = ui->caret.y;
    *w = ui->caret.w;
    *h = ui->caret.h;
    *visible = ui->caret.hide == 0;
    return true;
}

/* ---- regions, window DCs ---------------------------------------------------------------------------------- */

static cpu_status region_update(hle_call *c, bool invalidate) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a), hrgn = hle_u16(&a), erase = invalidate ? hle_u16(&a) : 0;
    fb_rect r, cl;
    UCHECK;
    hle_ret16(c, 0);
    if (!wm_is_window(&u->wm, hwnd)) return CPU_OK;
    wm_client_rect(&u->wm, hwnd, &cl);
    r = cl;
    if (hrgn && u->has_gfx && u->gfx.region_box) {
        if (!u->gfx.region_box(u->gfx.ctx, hrgn, &r)) return CPU_OK;
    }
    if (invalidate) wm_invalidate(&u->wm, hwnd, &r, erase != 0);
    else wm_validate(&u->wm, hwnd, &r);
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status u_invalidatergn(hle_call *c) { return region_update(c, true); }
static cpu_status u_validatergn(hle_call *c) { return region_update(c, false); }

static cpu_status u_getwindowdc(hle_call *c) {
    UBEGIN;
    uint16_t hwnd = hle_u16(&a);
    UCHECK;
    hle_ret16(c, hwnd ? wm_get_window_dc(&u->wm, hwnd) : wm_get_dc(&u->wm, 0));
    return CPU_OK;
}

/* ---- COMMDLG ------------------------------------------------------------------------------------------------- */

static cpu_status commdlg_cancel(hle_call *c) {
    UBEGIN;
    user_log(u, "COMMDLG.%u %s: not implemented, answered as a cancelled dialog", c->entry->ordinal, c->entry->name);
    hle_ret16(c, 0);
    return CPU_OK;
}

static cpu_status commdlg_error(hle_call *c) {
    hle_ret32(c, 0);                 /* CDERR: no error, the user cancelled */
    return CPU_OK;
}

#define U USER_MODULE
const hle_def ui_misc_defs[] = {
    {U, 67, "GetWindowDC", 2, u_getwindowdc},                           /* UNV */
    {U, 126, "InvalidateRgn", 6, u_invalidatergn},                      /* UNV */
    {U, 128, "ValidateRgn", 4, u_validatergn},                          /* UNV */
    {U, 137, "OpenClipboard", 2, u_openclipboard},
    {U, 138, "CloseClipboard", 0, u_closeclipboard},
    {U, 139, "EmptyClipboard", 0, u_emptyclipboard},
    {U, 140, "GetClipboardOwner", 0, u_getclipboardowner},
    {U, 141, "SetClipboardData", 4, u_setclipboarddata},
    {U, 142, "GetClipboardData", 2, u_getclipboarddata},
    {U, 143, "CountClipboardFormats", 0, u_countclipboardformats},      /* UNV */
    {U, 144, "EnumClipboardFormats", 2, u_enumclipboardformats},
    {U, 145, "RegisterClipboardFormat", 4, u_registerclipboardformat},
    {U, 146, "GetClipboardFormatName", 8, u_getclipboardformatname},    /* UNV */
    {U, 147, "SetClipboardViewer", 2, u_setclipboardviewer},            /* UNV */
    {U, 148, "GetClipboardViewer", 0, u_getclipboardviewer},            /* UNV */
    {U, 149, "ChangeClipboardChain", 4, u_changeclipboardchain},        /* UNV */
    {U, 163, "CreateCaret", 8, u_createcaret},                          /* UNV */
    {U, 164, "DestroyCaret", 0, u_destroycaret},                        /* UNV */
    {U, 165, "SetCaretPos", 4, u_setcaretpos},                          /* UNV */
    {U, 166, "HideCaret", 2, u_hidecaret},                              /* UNV */
    {U, 167, "ShowCaret", 2, u_showcaret},                              /* UNV */
    {U, 168, "SetCaretBlinkTime", 2, u_setcaretblinktime},              /* UNV */
    {U, 183, "GetCaretPos", 4, u_getcaretpos},                          /* UNV */
    {U, 193, "IsClipboardFormatAvailable", 2, u_isclipboardformatavailable},   /* UNV */
    {U, 248, "GetOpenClipboardWindow", 0, u_getopenclipboardwindow},    /* UNV */
};
#undef U
const size_t ui_misc_ndefs = sizeof ui_misc_defs / sizeof ui_misc_defs[0];

const hle_def ui_commdlg_defs[] = {
    {"COMMDLG", 1, "GetOpenFileName", 4, commdlg_cancel},               /* UNV */
    {"COMMDLG", 2, "GetSaveFileName", 4, commdlg_cancel},               /* name from the import table */
    {"COMMDLG", 26, "CommDlgExtendedError", 0, commdlg_error},
};
const size_t ui_commdlg_ndefs = sizeof ui_commdlg_defs / sizeof ui_commdlg_defs[0];
