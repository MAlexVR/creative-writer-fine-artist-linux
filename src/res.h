#ifndef RES_H
#define RES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ne.h"

/* Decoders for the Win16 resource formats USER needs: string tables, menu and
 * dialog templates, accelerator tables, and the DIB-based bitmap, icon and
 * cursor resources. Pure functions over byte buffers: no guest memory, no
 * window manager. Every parser bounds-checks its input and returns RES_BAD for
 * a truncated or inconsistent blob. */

#define RT_CURSOR 1
#define RT_BITMAP 2
#define RT_ICON 3
#define RT_MENU 4
#define RT_DIALOG 5
#define RT_STRING 6
#define RT_ACCELERATOR 9
#define RT_GROUP_CURSOR 12
#define RT_GROUP_ICON 14

typedef enum { RES_OK = 0, RES_BAD, RES_NOTFOUND, RES_NOMEM } res_err;

/* ---- string tables ------------------------------------------------------- */

/* Text of string id in the program's RT_STRING bundles (16 strings per bundle,
 * bundle number id / 16 + 1). Returns its length (possibly 0) or -1 when the
 * string does not exist. The text is truncated to cap - 1 bytes. */
int res_string(const ne_image *ne, unsigned id, char *out, size_t cap);
/* One bundle blob: the idx-th string (0..15) as pointer + length, false if absent. */
bool res_bundle_string(const uint8_t *blob, size_t len, unsigned idx, const uint8_t **s, size_t *n);

/* ---- menus ----------------------------------------------------------------- */

#define MFR_GRAYED 0x0001
#define MFR_INACTIVE 0x0002
#define MFR_BITMAP 0x0004
#define MFR_CHECKED 0x0008
#define MFR_POPUP 0x0010
#define MFR_MENUBARBREAK 0x0020
#define MFR_MENUBREAK 0x0040
#define MFR_END 0x0080
#define MFR_HELP 0x4000

/* Called for every item in file order. A popup item is reported (is_popup) before its
 * children, which have depth + 1; id is 0 for popups. Return false to abort. */
typedef bool (*res_menu_fn)(void *ctx, int depth, uint16_t flags, uint16_t id, const char *text, bool is_popup);
res_err res_menu_walk(const uint8_t *blob, size_t len, res_menu_fn fn, void *ctx);
/* Counts: all items, popups, and the deepest nesting. */
res_err res_menu_count(const uint8_t *blob, size_t len, unsigned *items, unsigned *popups, unsigned *depth);

/* ---- dialogs ----------------------------------------------------------------- */

#define RES_DLG_MAXCTL 256

typedef struct {
    int16_t x, y, cx, cy;       /* dialog units */
    uint16_t id;
    uint32_t style;
    char cls[24];               /* "BUTTON" ... or a custom class name */
    char text[96];              /* caption; when text_is_id, text is empty */
    uint16_t text_id;           /* the 0xFF id form (a STATIC icon name) */
    bool text_is_id;
    uint8_t ncd;                /* creation data bytes */
    uint8_t cdata[16];
} res_dlg_item;

typedef struct {
    uint32_t style;
    unsigned nitems;
    int16_t x, y, cx, cy;
    char menu[40];
    uint16_t menu_id;
    bool menu_is_id;
    char cls[24];               /* empty: the default dialog class */
    char caption[96];
    uint16_t pointsize;         /* DS_SETFONT */
    char face[32];
    res_dlg_item *items;        /* nitems entries, owned: res_dlg_free */
} res_dlg;

res_err res_dlg_parse(const uint8_t *blob, size_t len, res_dlg *out);
void res_dlg_free(res_dlg *d);

/* ---- accelerators ------------------------------------------------------------- */

#define AF_VIRTKEY 0x01
#define AF_NOINVERT 0x02
#define AF_SHIFT 0x04
#define AF_CONTROL 0x08
#define AF_ALT 0x10
#define AF_LAST 0x80

typedef struct { uint8_t flags; uint16_t key, cmd; } res_accel;
/* Parses up to cap entries; *n receives the number found. */
res_err res_accel_parse(const uint8_t *blob, size_t len, res_accel *out, size_t cap, size_t *n);

/* ---- DIBs, icons, cursors -------------------------------------------------------- */

typedef struct {
    int w, h;                   /* visible size (the AND mask is not counted in h) */
    int bpp;
    uint32_t *cref;             /* w * h COLORREFs, top row first */
    uint8_t *mask;              /* w * h, 1 = AND mask bit set; NULL for a plain bitmap */
} res_image;

/* Decodes a BITMAPINFOHEADER / BITMAPCOREHEADER DIB (1, 4, 8, 24 bpp, BI_RGB). With
 * has_mask the height covers an XOR bitmap and a 1-bpp AND mask below it. */
res_err res_dib_decode(const uint8_t *dib, size_t len, bool has_mask, res_image *out);
void res_image_free(res_image *im);

/* Group directory entry chosen for display: the RT_ICON / RT_CURSOR id and its size. */
typedef struct { uint16_t id; int w, h, bpp; } res_group_pick;
/* Picks the entry of a group resource closest to want_w x want_h, preferring up to 8 bpp. */
res_err res_group_pick_best(const uint8_t *blob, size_t len, bool cursor, int want_w, int want_h,
                            res_group_pick *out);
/* Number of entries of a group directory (-1 when malformed). */
int res_group_count(const uint8_t *blob, size_t len);
/* RT_ICON / RT_CURSOR blob -> image (cursors: *hot_x, *hot_y from the first two words). */
res_err res_icon_decode(const uint8_t *blob, size_t len, res_image *out);
res_err res_cursor_decode(const uint8_t *blob, size_t len, res_image *out, int *hot_x, int *hot_y);

/* ---- the program's resource table -------------------------------------------------- */

/* Finds a resource by integer id or name and returns its bytes inside the image. */
res_err res_find(const ne_image *ne, unsigned type, bool by_name, unsigned id, const char *name,
                 const uint8_t **data, size_t *len);

#endif
