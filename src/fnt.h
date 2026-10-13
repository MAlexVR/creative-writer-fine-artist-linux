#ifndef FNT_H
#define FNT_H
/* Raster fonts: the FNT format of Windows 2.x/3.x (dfVersion 0x200 and 0x300), alone or as the RT_FONT resources of a
 * .FON file (a resource-only NE module). Each FNT is one "strike": one face at one size. Glyph bitmaps are stored in
 * byte-wide column groups: for a glyph w pixels wide there are ceil(w / 8) groups of `height` bytes each. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char face[32];
    int points, height, ascent, intlead, extlead, weight, avgw, maxw, first, last, dflt, brk;
    bool italic, underline, strikeout;
    uint8_t pitch_family;
    uint8_t *blob;                 /* owned copy of the FNT resource */
    size_t blob_len;
    uint16_t width[257];           /* per character first..last, then the default glyph at last + 1 (index = ch - first) */
    uint32_t offset[257];
} fnt_strike;

typedef struct {
    fnt_strike *s;
    size_t n;
} fnt_font;

/* A .FON (NE) or a bare FNT. false: not a font, truncated or an unsupported version. */
bool fnt_load_buffer(const void *buf, size_t len, fnt_font *out);
bool fnt_load_file(const char *path, fnt_font *out);
void fnt_free(fnt_font *f);

/* The strike of the face (case-insensitive) whose size is closest: em_height > 0 compares height - intlead (a negative
 * LOGFONT height), otherwise cell_height compares the cell height; ties go to the smaller strike. NULL: no such face. */
const fnt_strike *fnt_pick(const fnt_font *f, const char *face, int cell_height, int em_height);

/* Advance width of a character (characters outside first..last use the default glyph). */
int fnt_width(const fnt_strike *s, unsigned char ch);
/* Pixel (x, y) of a glyph: 0 outside the glyph. */
int fnt_pixel(const fnt_strike *s, unsigned char ch, int x, int y);

#endif
