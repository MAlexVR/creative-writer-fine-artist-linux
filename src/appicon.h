#ifndef APPICON_H
#define APPICON_H
/* The icon of a Win16 program as a host window/launcher icon: its first icon group (RT_GROUP_ICON), the entry closest to the
 * wanted size, as 0xAARRGGBB with the icon's AND mask as transparency; and a PNG encoder (stored deflate, no dependency) so
 * the same pixels can be exported for a .desktop launcher. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ne.h"

typedef struct {
    int w, h;
    uint32_t *argb;      /* w * h, top row first; owned: appicon_free */
} appicon;

bool appicon_from_ne(const ne_image *ne, int want, appicon *out);
void appicon_free(appicon *ic);
/* A copy scaled up by a whole factor (1..16), nearest neighbour so the pixel art stays sharp; owned: appicon_free. */
bool appicon_scale(const appicon *src, int factor, appicon *out);
/* PNG file bytes (RGBA, 8 bits); *png is malloc'ed, the caller frees it. */
bool appicon_png(const appicon *ic, uint8_t **png, size_t *len);

#endif
