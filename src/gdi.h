#ifndef GDI_H
#define GDI_H

#include <stdbool.h>
#include <stdio.h>

#include "hle.h"
#include "ttf.h"
#include "user.h"

/* Win16 GDI.EXE on the HLE dispatcher over the window manager's device
 * contexts and the 8-bit framebuffer.
 *
 * Model. Every GDI object (pen, brush, font, bitmap, palette, region, memory
 * DC) is a slot in one table; its handle is 0x1000 + 2 * slot, so it never
 * collides with the window manager's screen DCs (0x0800-0x0BFF). Screen DCs
 * keep their window and clip in wm.h; their GDI state (selected objects,
 * colours, modes, mapping, clip region) is created on first use and dropped
 * when the window manager recycles the handle. Memory DCs draw into the
 * bitmap selected into them. Bitmaps are device dependent: 1 byte per pixel,
 * a palette index of the screen (8 bpp) or 0/1 (monochrome).
 *
 * Every drawing call goes through the DC's clip: the window manager's visible
 * or paint rectangle intersected with the DC clip region (a rectangle list,
 * see gdi_rgn.c). The framebuffer must be 8 bpp.
 *
 * The USER functions that draw (FillRect, FrameRect, InvertRect, DrawFocusRect,
 * DrawText, ScrollDC, SelectPalette, RealizePalette) are registered here. */

typedef struct gdi gdi_t;

typedef struct {
    FILE *log; /* logged stubs and unsupported requests; NULL: silent */
    /* DOS path -> host path, for AddFontResource of raster fonts (.FON/.FNT); NULL: fonts are only registered by name */
    bool (*host_path)(void *ctx, const char *dos, char *host, size_t cap);
    void *host_ctx;
    /* TrueType faces for the names no raster font covers (Arial, MS Sans Serif, ...); borrowed, NULL: built-in 5x7 glyphs */
    ttf_set *fonts;
    /* where the virtual "PDF Printer" writes each finished document; NULL: $HOME (or the current directory) */
    const char *print_dir;
} gdi_config;

#define GDI_STUB_MAX 128

typedef struct {
    const char *module;
    uint16_t ordinal;
    const char *name;
    const char *task; /* who implements it */
    uint16_t caller_cs, caller_ip;
} gdi_stub_rec;

/* Registers GDI (and the drawing half of USER) in h; u supplies the window
 * manager, the framebuffer and guest callbacks. NULL on failure. */
gdi_t *gdi_create(const gdi_config *cfg, hle_t *h, user_t *u);
void gdi_destroy(gdi_t *g);
bool gdi_attach(gdi_t *g, loader_proc *p);

/* PDF files written by the virtual printer so far (full path of the i-th). */
size_t gdi_print_count(const gdi_t *g);
const char *gdi_print_path(const gdi_t *g, size_t i);

/* Live GDI objects (every type, DCs included) and memory device contexts among them. */
void gdi_stats(const gdi_t *g, unsigned *objects, unsigned *dcs);

size_t gdi_stub_count(const gdi_t *g);
const gdi_stub_rec *gdi_stub_at(const gdi_t *g, size_t i);

#endif
