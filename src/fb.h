#ifndef FB_H
#define FB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Software framebuffer behind the Win16 screen DC.
 *
 * The default is 640x480 8-bit palettized, like Windows 3.1 on a 256-colour
 * VGA/SVGA driver: the 20 static system colours sit at palette entries 0-9 and
 * 246-255, the other 236 entries are free for the palette manager. A
 * 32-bit mode (0x00RRGGBB per pixel) exists for later use. Colours handed in
 * by the API are COLORREFs (0x00BBGGRR); in 8-bit mode they map to the nearest
 * palette entry, exact matches first. */

typedef struct { int left, top, right, bottom; } fb_rect;

typedef struct {
    int w, h, bpp;       /* bpp is 8 or 32 */
    uint8_t *p8;         /* bpp 8: palette indices, w*h */
    uint32_t *p32;       /* bpp 32: 0x00RRGGBB */
    uint32_t pal[256];   /* 0x00RRGGBB */
    unsigned long dirty; /* incremented by every drawing call */
} fb_t;

#define FB_RGB(r, g, b) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16)

bool fb_init(fb_t *f, int w, int h, int bpp);
void fb_destroy(fb_t *f);
void fb_default_palette(fb_t *f);

/* The pixel value (palette index or 0x00RRGGBB) a COLORREF draws as. */
uint32_t fb_pixel_of(const fb_t *f, uint32_t colorref);
/* COLORREF of the pixel; 0 outside the buffer. */
uint32_t fb_colorref_at(const fb_t *f, int x, int y);

void fb_put(fb_t *f, int x, int y, uint32_t colorref);
/* Fill clipped to the buffer; returns the number of pixels written. */
size_t fb_fill_rect(fb_t *f, const fb_rect *r, uint32_t colorref);
/* Converts to 0xAARRGGBB (alpha 255), w*h entries. */
void fb_to_argb(const fb_t *f, uint32_t *out);

/* Writes the screen as a binary PPM (P6) with the current palette. */
bool fb_save_ppm(const fb_t *f, const char *path);

/* Rectangle helpers shared with the window manager. */
bool fb_rect_empty(const fb_rect *r);
bool fb_rect_intersect(fb_rect *out, const fb_rect *a, const fb_rect *b);
void fb_rect_union(fb_rect *out, const fb_rect *a, const fb_rect *b);

#endif
