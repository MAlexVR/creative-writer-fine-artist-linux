#ifndef TTF_H
#define TTF_H
/* TrueType text without anti-aliasing: faces are rasterised with the public-domain/MIT stb_truetype and thresholded to 1 bit,
 * which is how Windows 3.1 drew TrueType on an 8-bit palette display (no ClearType, no grey ramps).
 *
 * A ttf_set is a lookup of the faces a program may ask for. Win16 programs name Windows' own faces (Arial, MS Sans Serif,
 * Helv, Times New Roman, MS Serif, Courier, ...) that are not on the CD; the set answers them with metric-compatible
 * free faces (Liberation Sans/Serif/Mono, SIL OFL) found in a directory (the portable package ships them) or in the usual
 * system font directories. A set can also hold faces the program adds itself (the CD's own .TTF files). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ttf_face ttf_face;
typedef struct ttf_set ttf_set;

/* A face at one size: metrics are in pixels, glyphs are cached 1-bit bitmaps. Owned by the set. */
typedef struct ttf_inst ttf_inst;

typedef struct {
    int height, ascent, descent, intlead, extlead, avgw, maxw;   /* Windows TEXTMETRIC meanings */
} ttf_metrics;

typedef struct {
    int w, h;              /* bitmap size; 0 for blank glyphs */
    int xoff, yoff;        /* top-left of the bitmap from the pen position on the baseline (yoff is negative above it) */
    const uint8_t *bits;   /* w * h bytes, 0 or 1 */
} ttf_glyph;

/* Faces. data is copied. NULL: not a TrueType font. */
ttf_face *ttf_face_from_buffer(const void *data, size_t len);
ttf_face *ttf_face_from_file(const char *path);
void ttf_face_free(ttf_face *f);
/* family name from the name table ("" when absent) */
const char *ttf_face_family(const ttf_face *f);

/* Windows-1252 byte to Unicode */
unsigned ttf_unicode(unsigned char ch);

/* ---- sets --------------------------------------------------------------------------------------------- */
ttf_set *ttf_set_create(void);
void ttf_set_free(ttf_set *s);

/* Registers the Liberation files LiberationSans/Serif/Mono-Regular/Bold.ttf found in dir (loaded on first use).
 * Returns how many of the six were found. */
int ttf_set_add_dir(ttf_set *s, const char *dir);
/* The usual system font directories (fontconfig layouts under /usr/share/fonts and ~/.fonts, searched recursively one
 * level for the Liberation names, then DejaVu Sans/Serif/Mono as a last resort). Faces already registered are kept. */
int ttf_set_add_system(ttf_set *s);
/* Every *.TTF in dir (any case), registered under the family name of its name table: the faces a setup installs. Returns
 * how many loaded. */
int ttf_set_add_installed(ttf_set *s, const char *dir);
/* A face under a family name the program uses (the CD's Lucida Sans ...); the set owns it. */
bool ttf_set_add_face(ttf_set *s, const char *family, ttf_face *f);

/* The face for a request: an added family of that name first; otherwise the generic mapping from the name (Arial, Helv,
 * MS Sans Serif, System -> sans; Times, MS Serif, Tms Rmn -> serif; Courier, Terminal, Fixedsys -> mono; unknown names
 * from the pitch/family byte, defaulting to sans). *synth_bold: the face has no bold style, smear it. NULL: nothing
 * available (the caller falls back to its built-in glyphs). */
/* Another name for a face already in the set (not owned twice). */
bool ttf_set_alias(ttf_set *s, const char *family, const ttf_face *f);

const ttf_face *ttf_set_pick(ttf_set *s, const char *name, int weight, int pitch_family, bool *synth_bold);

/* Instance at a size: em_height > 0 gives an em of that many pixels (negative LOGFONT height), otherwise the cell
 * (ascent + descent) is cell_height. */
ttf_inst *ttf_set_inst(ttf_set *s, const ttf_face *f, int cell_height, int em_height);
const ttf_metrics *ttf_inst_metrics(const ttf_inst *i);
int ttf_inst_advance(ttf_inst *i, unsigned char ch);
/* Hinted 1-bit glyphs through the system's libfreetype (loaded at run time, so nothing links against it and a machine
 * without it keeps the stb rendering): the TrueType instructions of the Liberation faces put stems on whole pixels, which is
 * what Windows 3.1 showed at 9..12 px and what thresholding unhinted outlines cannot do (m and n merge into a blob). Applies
 * to instances created afterwards. False when libfreetype cannot be used (or CWFA_NO_FREETYPE is set). */
bool ttf_set_use_freetype(ttf_set *s, bool on);

/* false: the face has no glyph for the character. A blank glyph (space) is true with w == 0. */
bool ttf_inst_glyph(ttf_inst *i, unsigned char ch, ttf_glyph *g);

#endif
