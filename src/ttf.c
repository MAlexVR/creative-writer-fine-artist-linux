/* TrueType faces for GDI text: stb_truetype (third_party/stb, public domain / MIT) rasterises, we threshold to 1 bit. */
#define _POSIX_C_SOURCE 200809L
#include "ttf.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "../third_party/stb/stb_truetype.h"
#pragma GCC diagnostic pop

#if defined(__has_include)
#if __has_include(<ft2build.h>)
#include <ft2build.h>
#include FT_FREETYPE_H
#define TTF_HAVE_FT 1
#endif
#endif

/* A coverage value at or above this is ink. Win 3.1 hinted outlines put stems on whole pixels; unhinted ones thresholded
 * at 50% break thin strokes at 8..12 px, so the cut sits well below half (chosen by eye over Arial/Times/Courier at 10..14 px). */
#define INK_THRESHOLD 72
#define INK_FAINT 44          /* coverage that still counts as a feature (see threshold_keep_features) */

struct ttf_face {
    uint8_t *data;
    size_t len;
    stbtt_fontinfo info;
    char family[64];
    int asc, desc, gap, upm;
};

typedef struct {
    int state;               /* 0 not rendered, 1 rendered, 2 no glyph */
    int adv;
    ttf_glyph g;
    uint8_t *bits;
} gcache;

struct ttf_inst {
    const ttf_face *face;
    int cell, em;            /* the request */
    float scale;
    int ppem;
    bool useft;
    void *ft;                /* FT_Face of this size when the set renders through libfreetype */
    ttf_metrics m;
    gcache c[256];
    struct ttf_inst *next;
};

enum { R_SANS, R_SANS_B, R_SERIF, R_SERIF_B, R_MONO, R_MONO_B, R_COUNT };

typedef struct {
    char name[48];
    ttf_face *face;
    bool owner;
} added;

struct ttf_set {
    char path[R_COUNT][512];
    ttf_face *face[R_COUNT];
    bool tried[R_COUNT];
    added *add;
    int nadd, capadd;
    ttf_inst *insts;
    bool use_ft;
};

static const char *const aliases[R_COUNT][4] = {
    {"LiberationSans-Regular.ttf", "Arimo-Regular.ttf", "DejaVuSans.ttf", NULL},
    {"LiberationSans-Bold.ttf", "Arimo-Bold.ttf", "DejaVuSans-Bold.ttf", NULL},
    {"LiberationSerif-Regular.ttf", "Tinos-Regular.ttf", "DejaVuSerif.ttf", NULL},
    {"LiberationSerif-Bold.ttf", "Tinos-Bold.ttf", "DejaVuSerif-Bold.ttf", NULL},
    {"LiberationMono-Regular.ttf", "Cousine-Regular.ttf", "DejaVuSansMono.ttf", NULL},
    {"LiberationMono-Bold.ttf", "Cousine-Bold.ttf", "DejaVuSansMono-Bold.ttf", NULL}};

/* ---- faces ---------------------------------------------------------------------------------------------- */

static void read_family(ttf_face *f) {
    int len = 0, i, n = 0;
    const char *s = stbtt_GetFontNameString(&f->info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                                            STBTT_MS_LANG_ENGLISH, 1);
    f->family[0] = 0;
    if (s && len > 1) {
        for (i = 0; i + 1 < len && n < (int)sizeof f->family - 1; i += 2) f->family[n++] = s[i + 1];
    } else if ((s = stbtt_GetFontNameString(&f->info, &len, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN, STBTT_MAC_LANG_ENGLISH, 1)) != NULL) {
        for (i = 0; i < len && n < (int)sizeof f->family - 1; i++) f->family[n++] = s[i];
    }
    f->family[n] = 0;
}

ttf_face *ttf_face_from_buffer(const void *data, size_t len) {
    ttf_face *f;
    if (!data || len < 16) return NULL;
    f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->data = malloc(len);
    if (!f->data) {
        free(f);
        return NULL;
    }
    memcpy(f->data, data, len);
    f->len = len;
    if (!stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
        free(f->data);
        free(f);
        return NULL;
    }
    stbtt_GetFontVMetrics(&f->info, &f->asc, &f->desc, &f->gap);
    f->upm = (int)(1.0f / stbtt_ScaleForMappingEmToPixels(&f->info, 1.0f) + 0.5f);
    if (f->upm <= 0 || f->asc - f->desc <= 0) {
        free(f->data);
        free(f);
        return NULL;
    }
    read_family(f);
    return f;
}

ttf_face *ttf_face_from_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    long n;
    uint8_t *buf;
    ttf_face *f;
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) || (n = ftell(fp)) <= 0 || n > 32L * 1024 * 1024 || fseek(fp, 0, SEEK_SET)) {
        fclose(fp);
        return NULL;
    }
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    f = ttf_face_from_buffer(buf, (size_t)n);
    free(buf);
    return f;
}

void ttf_face_free(ttf_face *f) {
    if (!f) return;
    free(f->data);
    free(f);
}

const char *ttf_face_family(const ttf_face *f) { return f->family; }

unsigned ttf_unicode(unsigned char ch) {
    static const unsigned hi[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                    0x2039, 0x0152, 0x8D,   0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
    return ch >= 0x80 && ch < 0xA0 ? hi[ch - 0x80] : ch;
}

/* ---- sets ----------------------------------------------------------------------------------------------- */


/* ---- hinted glyphs through libfreetype (dlopen) --------------------------------------------------------- */

#ifdef TTF_HAVE_FT
static struct {
    bool tried, ok;
    void *lib;
    FT_Library ft;
    FT_Error (*init)(FT_Library *);
    void (*done_lib)(FT_Library);
    FT_Error (*new_face)(FT_Library, const FT_Byte *, FT_Long, FT_Long, FT_Face *);
    FT_Error (*done_face)(FT_Face);
    FT_Error (*set_px)(FT_Face, FT_UInt, FT_UInt);
    FT_UInt (*char_index)(FT_Face, FT_ULong);
    FT_Error (*load)(FT_Face, FT_UInt, FT_Int32);
} ftl;

static bool ft_ready(void) {
    if (ftl.tried) return ftl.ok;
    ftl.tried = true;
    if (getenv("CWFA_NO_FREETYPE")) return false;
    ftl.lib = dlopen("libfreetype.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!ftl.lib) return false;
    *(void **)&ftl.init = dlsym(ftl.lib, "FT_Init_FreeType");
    *(void **)&ftl.done_lib = dlsym(ftl.lib, "FT_Done_FreeType");
    *(void **)&ftl.new_face = dlsym(ftl.lib, "FT_New_Memory_Face");
    *(void **)&ftl.done_face = dlsym(ftl.lib, "FT_Done_Face");
    *(void **)&ftl.set_px = dlsym(ftl.lib, "FT_Set_Pixel_Sizes");
    *(void **)&ftl.char_index = dlsym(ftl.lib, "FT_Get_Char_Index");
    *(void **)&ftl.load = dlsym(ftl.lib, "FT_Load_Glyph");
    ftl.ok = ftl.init && ftl.done_lib && ftl.new_face && ftl.done_face && ftl.set_px && ftl.char_index && ftl.load &&
             ftl.init(&ftl.ft) == 0;
    if (!ftl.ok) {
        dlclose(ftl.lib);
        ftl.lib = NULL;
    }
    return ftl.ok;
}

/* The face of an instance at its pixel size (NULL when libfreetype cannot open it). */
static FT_Face ft_face_of(ttf_inst *i, const ttf_face *f, size_t len) {
    FT_Face fc = NULL;
    if (i->ft) return i->ft;
    if (ftl.new_face(ftl.ft, f->data, (FT_Long)len, 0, &fc) != 0) return NULL;
    if (ftl.set_px(fc, 0, (FT_UInt)i->ppem) != 0) {
        ftl.done_face(fc);
        return NULL;
    }
    i->ft = fc;
    return fc;
}

/* Glyph for a code point: the hinted 1-bit bitmap (FT_LOAD_TARGET_MONO runs the font's TrueType instructions) and its
 * hinted advance. False when the face has no such glyph. */
static bool ft_raster_cp(ttf_inst *i, unsigned cp, gcache *c, bool *has_glyph) {
    FT_Face fc = ft_face_of(i, i->face, i->face->len);
    FT_GlyphSlot sl;
    FT_UInt gi;
    int x, y, w, h;
    uint8_t *bits;
    *has_glyph = false;
    if (!fc) return false;
    gi = ftl.char_index(fc, cp);
    if (!gi) return false;
    *has_glyph = true;
    if (ftl.load(fc, gi, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO) != 0) return false;
    sl = fc->glyph;
    c->adv = (int)(sl->advance.x >> 6);
    w = (int)sl->bitmap.width;
    h = (int)sl->bitmap.rows;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || sl->bitmap.pixel_mode != FT_PIXEL_MODE_MONO) return true;   /* blank */
    bits = calloc((size_t)w * (size_t)h, 1);
    if (!bits) return false;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) bits[y * w + x] = (uint8_t)((sl->bitmap.buffer[y * sl->bitmap.pitch + (x >> 3)] >> (7 - (x & 7))) & 1);
    c->bits = bits;
    c->g.w = w;
    c->g.h = h;
    c->g.xoff = sl->bitmap_left;
    c->g.yoff = -sl->bitmap_top;
    c->g.bits = bits;
    return true;
}

static void ft_free_inst(ttf_inst *i) {
    if (i->ft) ftl.done_face((FT_Face)i->ft);
    i->ft = NULL;
}
#endif

bool ttf_set_use_freetype(ttf_set *s, bool on) {
    if (!s) return false;
#ifdef TTF_HAVE_FT
    s->use_ft = on && ft_ready();
    return s->use_ft;
#else
    (void)on;
    s->use_ft = false;
    return false;
#endif
}

ttf_set *ttf_set_create(void) { return calloc(1, sizeof(ttf_set)); }

void ttf_set_free(ttf_set *s) {
    int i, k;
    ttf_inst *n;
    if (!s) return;
    while (s->insts) {
        n = s->insts->next;
        for (k = 0; k < 256; k++) free(s->insts->c[k].bits);
#ifdef TTF_HAVE_FT
        ft_free_inst(s->insts);
#endif
        free(s->insts);
        s->insts = n;
    }
    for (i = 0; i < R_COUNT; i++) ttf_face_free(s->face[i]);
    for (i = 0; i < s->nadd; i++)
        if (s->add[i].owner) ttf_face_free(s->add[i].face);
    free(s->add);
    free(s);
}

static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

int ttf_set_add_dir(ttf_set *s, const char *dir) {
    int r, a, found = 0;
    char p[512];
    if (!s || !dir) return 0;
    for (r = 0; r < R_COUNT; r++) {
        if (s->path[r][0]) {
            found++;
            continue;
        }
        for (a = 0; aliases[r][a]; a++) {
            snprintf(p, sizeof p, "%s/%s", dir, aliases[r][a]);
            if (is_file(p)) {
                snprintf(s->path[r], sizeof s->path[r], "%s", p);
                found++;
                break;
            }
        }
    }
    return found;
}

static bool find_in_tree(const char *root, const char *name, int depth, char *out, size_t cap) {
    DIR *d = opendir(root);
    struct dirent *e;
    char p[1024];
    if (!d) return false;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", root, e->d_name);
        if (!strcmp(e->d_name, name) && is_file(p)) {
            closedir(d);
            if (strlen(p) >= cap) return false;
            memcpy(out, p, strlen(p) + 1);
            return true;
        }
        if (depth > 0 && find_in_tree(p, name, depth - 1, out, cap)) {
            closedir(d);
            return true;
        }
    }
    closedir(d);
    return false;
}

int ttf_set_add_system(ttf_set *s) {
    char roots[4][512];
    const char *home = getenv("HOME");
    int nroots = 0, r, a, i, have = 0;
    if (!s) return 0;
    snprintf(roots[nroots++], sizeof roots[0], "/usr/share/fonts");
    snprintf(roots[nroots++], sizeof roots[0], "/usr/local/share/fonts");
    if (home && *home) {
        snprintf(roots[nroots++], sizeof roots[0], "%s/.local/share/fonts", home);
        snprintf(roots[nroots++], sizeof roots[0], "%s/.fonts", home);
    }
    for (r = 0; r < R_COUNT; r++) {
        for (a = 0; aliases[r][a] && !s->path[r][0]; a++)       /* alias order is the preference order */
            for (i = 0; i < nroots && !s->path[r][0]; i++)
                if (find_in_tree(roots[i], aliases[r][a], 3, s->path[r], sizeof s->path[r])) break;
        if (s->path[r][0]) have++;
    }
    return have;
}

static bool add_named(ttf_set *s, const char *family, ttf_face *f, bool owner) {
    if (!s || !f || !family) return false;
    if (s->nadd == s->capadd) {
        int nc = s->capadd ? s->capadd * 2 : 8;
        added *na = realloc(s->add, (size_t)nc * sizeof *na);
        if (!na) return false;
        s->add = na;
        s->capadd = nc;
    }
    snprintf(s->add[s->nadd].name, sizeof s->add[0].name, "%s", family);
    s->add[s->nadd].face = f;
    s->add[s->nadd].owner = owner;
    s->nadd++;
    return true;
}

bool ttf_set_add_face(ttf_set *s, const char *family, ttf_face *f) { return add_named(s, family, f, true); }
bool ttf_set_alias(ttf_set *s, const char *family, const ttf_face *f) { return add_named(s, family, (ttf_face *)f, false); }

int ttf_set_add_installed(ttf_set *s, const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        char p[1024];
        ttf_face *f;
        if (len < 5 || strcasecmp(e->d_name + len - 4, ".ttf") != 0) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        f = ttf_face_from_file(p);
        if (!f) continue;
        if (f->family[0] && add_named(s, f->family, f, true)) n++;
        else ttf_face_free(f);
    }
    closedir(d);
    return n;
}

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

static bool has(const char *name, const char *what) {
    size_t n = strlen(what), i, j;
    for (i = 0; name[i]; i++) {
        for (j = 0; j < n && name[i + j]; j++)
            if (tolower((unsigned char)name[i + j]) != tolower((unsigned char)what[j])) break;
        if (j == n) return true;
    }
    return false;
}

static const ttf_face *role_face(ttf_set *s, int role) {
    if (!s->face[role] && !s->tried[role] && s->path[role][0]) {
        s->tried[role] = true;
        s->face[role] = ttf_face_from_file(s->path[role]);
    }
    return s->face[role];
}

static bool is_serif_name(const char *name) {
    if (has(name, "sans")) return false;
    return has(name, "times") || has(name, "serif") || has(name, "tms") || has(name, "roman") || has(name, "garamond") ||
           has(name, "book");
}

const ttf_face *ttf_set_pick(ttf_set *s, const char *name, int weight, int pitch_family, bool *synth_bold) {
    int i, base;
    bool bold = weight >= 600;
    const ttf_face *f;
    if (synth_bold) *synth_bold = false;
    if (!s) return NULL;
    for (i = 0; i < s->nadd; i++)
        if (ieq(s->add[i].name, name)) {
            if (synth_bold) *synth_bold = bold;
            return s->add[i].face;
        }
    if (has(name, "courier") || has(name, "terminal") || has(name, "fixedsys") || has(name, "mono") || has(name, "console"))
        base = R_MONO;
    else if (is_serif_name(name))
        base = R_SERIF;
    else if (has(name, "arial") || has(name, "helv") || has(name, "sans") || has(name, "system") || has(name, "tahoma") ||
             has(name, "verdana"))
        base = R_SANS;
    else if ((pitch_family & 3) == 1)
        base = R_MONO;
    else if ((pitch_family & 0xF0) == 0x10)
        base = R_SERIF;
    else if ((pitch_family & 0xF0) == 0x30)
        base = R_MONO;
    else
        base = R_SANS;
    f = bold ? role_face(s, base + 1) : NULL;
    if (f) return f;
    f = role_face(s, base);
    if (f && bold && synth_bold) *synth_bold = true;
    return f;
}

/* ---- instances ------------------------------------------------------------------------------------------ */

static int iround(double v) { return (int)floor(v + 0.5); }

static int raw_advance(const ttf_inst *i, unsigned cp) {
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&i->face->info, (int)cp, &adv, &lsb);
    return iround(adv * (double)i->scale);
}

static void compute_metrics(ttf_inst *i) {
    const ttf_face *f = i->face;
    int ppem, ch, a, w;
    double asc, desc;
    if (i->em > 0) ppem = i->em;
    else {
        ppem = iround((double)i->cell * f->upm / (f->asc - f->desc));
        if (ppem < 1) ppem = 1;
    }
    i->ppem = ppem;
    i->scale = (float)ppem / (float)f->upm;
    asc = ceil(f->asc * (double)i->scale - 0.02);
    desc = ceil(-f->desc * (double)i->scale - 0.02);
    i->m.ascent = (int)asc;
    i->m.descent = (int)desc;
    if (i->em <= 0) {                       /* a cell height: that is the height, the descent takes the difference */
        i->m.ascent = iround(f->asc * (double)i->scale);
        if (i->m.ascent > i->cell) i->m.ascent = i->cell;
        i->m.descent = i->cell - i->m.ascent;
    }
    i->m.height = i->m.ascent + i->m.descent;
    i->m.intlead = i->m.height - ppem;
    if (i->m.intlead < 0) i->m.intlead = 0;
    i->m.extlead = iround(f->gap * (double)i->scale);
    w = 0;
    a = 0;
    for (ch = 'a'; ch <= 'z'; ch++) {
        w += raw_advance(i, (unsigned)ch);
        a++;
    }
    i->m.avgw = (w + a / 2) / a;
    if (i->m.avgw < 1) i->m.avgw = 1;
    i->m.maxw = i->m.avgw;
    for (ch = 32; ch < 256; ch++) {
        if (ch == 0x7F || (ch >= 0x80 && ch < 0xA0 && ch != 0x80 && ch != 0x85 && ch != 0x91 && ch != 0x92 && ch != 0x93 &&
                           ch != 0x94 && ch != 0x95 && ch != 0x96 && ch != 0x97 && ch != 0x99)) continue;
        a = raw_advance(i, ttf_unicode((unsigned char)ch));
        if (a > i->m.maxw) i->m.maxw = a;
    }
}

ttf_inst *ttf_set_inst(ttf_set *s, const ttf_face *f, int cell_height, int em_height) {
    ttf_inst *i;
    if (!s || !f) return NULL;
    if (em_height < 0) em_height = -em_height;
    if (em_height > 0) cell_height = 0;
    if (em_height <= 0 && cell_height <= 0) cell_height = 16;
    if (em_height > 400 || cell_height > 400) return NULL;
    for (i = s->insts; i; i = i->next)
        if (i->face == f && i->cell == cell_height && i->em == em_height) return i;
    i = calloc(1, sizeof *i);
    if (!i) return NULL;
    i->face = f;
    i->useft = s->use_ft;
    i->cell = cell_height;
    i->em = em_height;
    compute_metrics(i);
    i->next = s->insts;
    s->insts = i;
    return i;
}

const ttf_metrics *ttf_inst_metrics(const ttf_inst *i) { return &i->m; }

/* Threshold the coverage map to ink. A small feature (the dots of a diaeresis or of an i, a period, a thin serif) is
 * spread over pixels that are each a little below the threshold, so at 8..14 px it would vanish; Win 3.1's hinted
 * outlines never lost them. A pixel with at least INK_FAINT coverage that is a horizontal maximum of its row (not
 * less than either neighbour) is ink as well: stems, which are strong in the middle and weak at the edges, are not
 * widened, a faint dot is kept. */
static void threshold_keep_features(uint8_t *cov, int w, int h) {
    int r, c;
    for (r = 0; r < h; r++) {
        uint8_t *row = cov + (size_t)r * (size_t)w;
        uint8_t prev = 0;                                /* the coverage to the left, before this row was overwritten */
        for (c = 0; c < w; c++) {
            uint8_t v = row[c], next = c + 1 < w ? row[c + 1] : 0;
            row[c] = v >= INK_THRESHOLD || (v >= INK_FAINT && v >= prev && v >= next) ? 1 : 0;
            prev = v;
        }
    }
}

static bool raster_cp(ttf_inst *i, unsigned cp, gcache *c);

/* At 13 px and below the two dots of a diaeresis come out of an unhinted rasteriser as a smear that touches the letter
 * (or as nothing). A hinted face puts two 1 px dots one clear row above the letter: build the glyph that way from the
 * plain letter (the dotless i for i with a diaeresis). */
static bool compose_diaeresis(ttf_inst *i, gcache *c, unsigned char ch) {
    static const struct { unsigned char ch; unsigned base; } map[] = {
        {0xE4, 'a'}, {0xEB, 'e'}, {0xEF, 0x131 /* dotless i */}, {0xF6, 'o'}, {0xFC, 'u'}, {0xFF, 'y'},
        {0xC4, 'A'}, {0xCB, 'E'}, {0xCF, 'I'}, {0xD6, 'O'}, {0xDC, 'U'}};
    unsigned k;
    gcache base, *b = &base;
    bool ok = false;
    int x, y, xmin = 1 << 20, xmax = -1, top = -1, n, lx, rx, dot_y, nh, nw, x0, x1;
    uint8_t *bits;
    if ((int)(i->scale * (float)i->face->upm + 0.5f) > 13) return false;
    for (k = 0; k < sizeof map / sizeof map[0] && map[k].ch != ch; k++) {}
    if (k == sizeof map / sizeof map[0]) return false;
    memset(&base, 0, sizeof base);
    if (!stbtt_FindGlyphIndex(&i->face->info, (int)map[k].base) || !raster_cp(i, map[k].base, &base)) return false;
    for (y = 0; y < b->g.h; y++)
        for (x = 0; x < b->g.w; x++)
            if (b->bits[y * b->g.w + x]) {
                if (top < 0) top = y;
                if (x < xmin) xmin = x;
                if (x > xmax) xmax = x;
            }
    if (top < 0) goto done;
    n = xmax - xmin + 1;
    lx = xmin + n / 4;
    rx = xmax - n / 4;
    if (rx < lx + 2) {                                     /* a narrow letter (i): the dots flank its stem */
        lx = (xmin + xmax) / 2 - 1;
        rx = lx + 2;
    }
    x0 = lx < 0 ? lx : 0;                                  /* the bitmap grows sideways for dots outside the letter */
    x1 = rx >= b->g.w ? rx + 1 : b->g.w;
    nw = x1 - x0;
    dot_y = b->g.yoff + top - 2;                           /* one empty row between the dots and the letter */
    nh = b->g.yoff + b->g.h - dot_y;
    if (!(bits = calloc((size_t)nw * (size_t)nh, 1))) goto done;
    for (y = 0; y < b->g.h; y++)
        memcpy(bits + (size_t)(b->g.yoff - dot_y + y) * (size_t)nw + (size_t)(-x0), b->bits + (size_t)y * (size_t)b->g.w, (size_t)b->g.w);
    bits[lx - x0] = 1;
    bits[rx - x0] = 1;
    free(c->bits);
    c->bits = bits;
    c->g.w = nw;
    c->g.h = nh;
    c->g.xoff = b->g.xoff + x0;
    c->g.yoff = dot_y;
    c->g.bits = bits;
    ok = true;
done:
    free(base.bits);
    return ok;
}

/* The thresholded bitmap of a code point into c (c->bits owned by the caller). False when it has no ink to draw. */
static bool raster_cp(ttf_inst *i, unsigned cp, gcache *c) {
    int x0, y0, x1, y1, w, h;
    uint8_t *tmp;
    stbtt_GetCodepointBitmapBox(&i->face->info, (int)cp, i->scale, i->scale, &x0, &y0, &x1, &y1);
    w = x1 - x0;
    h = y1 - y0;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
    tmp = calloc((size_t)w * (size_t)h, 1);
    if (!tmp) return false;
    stbtt_MakeCodepointBitmap(&i->face->info, tmp, w, h, w, i->scale, i->scale, (int)cp);
    threshold_keep_features(tmp, w, h);
    c->bits = tmp;
    c->g.w = w;
    c->g.h = h;
    c->g.xoff = x0;
    c->g.yoff = y0;
    c->g.bits = tmp;
    return true;
}

static gcache *render(ttf_inst *i, unsigned char ch) {
    gcache *c = &i->c[ch];
    unsigned cp;
    if (c->state) return c;
    cp = ch < 32 ? 32 : ttf_unicode(ch);
    if (ch >= 0x80 && ch < 0xA0 && cp == ch) cp = 32;                 /* undefined in Windows-1252 */
    if (!stbtt_FindGlyphIndex(&i->face->info, (int)cp)) {
        c->state = 2;
        c->adv = raw_advance(i, 32);
        return c;
    }
    c->state = 1;
    c->adv = raw_advance(i, cp);
#ifdef TTF_HAVE_FT
    if (i->useft) {
        bool has;
        if (ft_raster_cp(i, cp, c, &has)) return c;                   /* hinted: accents and dots come out of the font */
        if (has) {                                                    /* known but not renderable: keep the stb glyph */
            free(c->bits);
            c->bits = NULL;
            memset(&c->g, 0, sizeof c->g);
            c->adv = raw_advance(i, cp);
        }
    }
#endif
    if (raster_cp(i, cp, c)) compose_diaeresis(i, c, ch);
    return c;
}

int ttf_inst_advance(ttf_inst *i, unsigned char ch) { return render(i, ch)->adv; }

bool ttf_inst_glyph(ttf_inst *i, unsigned char ch, ttf_glyph *g) {
    gcache *c = render(i, ch);
    if (c->state != 1) return false;
    if (c->bits) *g = c->g;
    else memset(g, 0, sizeof *g);
    return true;
}

