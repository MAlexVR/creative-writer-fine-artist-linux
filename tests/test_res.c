/* Resource decoders: string bundles, menu and dialog templates, accelerator
 * tables, DIB bitmaps, icons and cursors, on in-memory blobs and on the real
 * resource tables of WRITER.EXE and ARTIST.EXE (skipped when not given). */
#include "fb.h"
#include "res.h"
#include "rfixture.h"

static bool env_exe(const char *var, ne_image *ne) {
    const char *p = getenv(var);
    if (!p || !*p) return false;
    return ne_open_file(ne, p) == NE_OK;
}

static void test_string_bundles(void) {
    fx_t f;
    rb_t b;
    char out[64];
    const uint8_t *s;
    size_t n;
    printf("test_res_strings\n");
    fx_init(&f, 0x200, 0x100);
    /* bundle 2 holds ids 16..31: "Hello", "", "World!", then 13 empty */
    b.n = 0;
    rb_u8(&b, 5); rb_str(&b, "Hello"); b.n--;
    rb_u8(&b, 0);
    rb_u8(&b, 6); rb_str(&b, "World!"); b.n--;
    { int i; for (i = 3; i < 16; i++) rb_u8(&b, 0); }
    rf_add(&f, RT_STRING, NULL, 2, NULL, b.b, b.n);
    CHECK_EQ(res_string(&f.ne, 16, out, sizeof out), 5);
    CHECK(!strcmp(out, "Hello"));
    CHECK_EQ(res_string(&f.ne, 17, out, sizeof out), 0);   /* empty */
    CHECK_EQ(res_string(&f.ne, 18, out, sizeof out), 6);
    CHECK(!strcmp(out, "World!"));
    CHECK_EQ(res_string(&f.ne, 18, out, 4), 3);            /* truncated to the buffer */
    CHECK(!strcmp(out, "Wor"));
    CHECK_EQ(res_string(&f.ne, 15, out, sizeof out), -1);  /* no bundle 1 */
    CHECK_EQ(res_string(&f.ne, 300, out, sizeof out), -1);
    CHECK(res_bundle_string(b.b, b.n, 2, &s, &n) && n == 6 && !memcmp(s, "World!", 6));
    CHECK(!res_bundle_string(b.b, b.n, 16, &s, &n));
    CHECK(!res_bundle_string(b.b, 3, 2, &s, &n));          /* truncated bundle */
    fx_free(&f);
}

typedef struct { int n; char log[512]; } mwalk;
static bool menu_cb(void *ctx, int depth, uint16_t flags, uint16_t id, const char *text, bool popup) {
    mwalk *m = ctx;
    char line[96];
    snprintf(line, sizeof line, "%d%s:%x:%u:%s;", depth, popup ? "P" : "I", flags, id, text);
    strcat(m->log, line);
    m->n++;
    return true;
}

static void menu_blob(rb_t *b) {
    b->n = 0;
    rb_u16(b, 0); rb_u16(b, 0);                         /* version, header size */
    rb_u16(b, MFR_POPUP); rb_str(b, "&File");
    rb_u16(b, 0); rb_u16(b, 100); rb_str(b, "&New\tCtrl+N");
    rb_u16(b, 0); rb_u16(b, 0); rb_str(b, "");          /* separator */
    rb_u16(b, MFR_GRAYED | MFR_END); rb_u16(b, 101); rb_str(b, "E&xit");
    rb_u16(b, MFR_POPUP | MFR_END); rb_str(b, "&Help");
    rb_u16(b, MFR_POPUP); rb_str(b, "More");
    rb_u16(b, MFR_CHECKED | MFR_END); rb_u16(b, 7); rb_str(b, "Deep");
    rb_u16(b, MFR_END); rb_u16(b, 102); rb_str(b, "&About");
}

static void test_menu_walk(void) {
    rb_t b;
    mwalk m = {0, ""};
    unsigned items, popups, depth;
    printf("test_res_menu\n");
    menu_blob(&b);
    CHECK_EQ(res_menu_walk(b.b, b.n, menu_cb, &m), RES_OK);
    CHECK_EQ(m.n, 8);
    CHECK(!strcmp(m.log, "0P:10:0:&File;1I:0:100:&New\tCtrl+N;1I:0:0:;1I:81:101:E&xit;"
                         "0P:90:0:&Help;1P:10:0:More;2I:88:7:Deep;1I:80:102:&About;"));
    CHECK_EQ(res_menu_count(b.b, b.n, &items, &popups, &depth), RES_OK);
    CHECK_EQ(items, 8);
    CHECK_EQ(popups, 3);
    CHECK_EQ(depth, 3);
    CHECK_EQ(res_menu_walk(b.b, b.n - 3, menu_cb, &m), RES_BAD);   /* truncated */
    CHECK_EQ(res_menu_walk(b.b, 2, menu_cb, &m), RES_BAD);
}

static void test_dialog_parse(void) {
    rb_t b;
    res_dlg d;
    printf("test_res_dialog\n");
    b.n = 0;
    rb_u32(&b, 0x80C80040u | 0x00040000);   /* DS_SETFONT | WS_POPUP | WS_CAPTION ... */
    rb_u8(&b, 4);                           /* four controls */
    rb_u16(&b, 10); rb_u16(&b, 20); rb_u16(&b, 180); rb_u16(&b, 90);
    rb_u8(&b, 0xFF); rb_u16(&b, 55);        /* menu by id */
    rb_str(&b, "MYCLASS");
    rb_str(&b, "Open File");
    rb_u16(&b, 8); rb_str(&b, "MS Sans Serif");
    /* push button, predefined class code 0x80, text "OK" */
    rb_u16(&b, 5); rb_u16(&b, 70); rb_u16(&b, 40); rb_u16(&b, 14); rb_u16(&b, 1); rb_u32(&b, 0x50010001u);
    rb_u8(&b, 0x80); rb_str(&b, "OK"); rb_u8(&b, 0);
    /* static icon: class 0x82, text by id */
    rb_u16(&b, 5); rb_u16(&b, 5); rb_u16(&b, 18); rb_u16(&b, 20); rb_u16(&b, 0xFFFF); rb_u32(&b, 0x50000003u);
    rb_u8(&b, 0x82); rb_u8(&b, 0xFF); rb_u16(&b, 321); rb_u8(&b, 0);
    /* edit with creation data */
    rb_u16(&b, 30); rb_u16(&b, 5); rb_u16(&b, 100); rb_u16(&b, 12); rb_u16(&b, 200); rb_u32(&b, 0x50810080u);
    rb_u8(&b, 0x81); rb_str(&b, ""); rb_u8(&b, 3); rb_u8(&b, 1); rb_u8(&b, 2); rb_u8(&b, 3);
    /* custom class by name */
    rb_u16(&b, 1); rb_u16(&b, 2); rb_u16(&b, 3); rb_u16(&b, 4); rb_u16(&b, 9); rb_u32(&b, 0x40000000u);
    rb_str(&b, "MyCtl"); rb_str(&b, "x"); rb_u8(&b, 0);
    CHECK_EQ(res_dlg_parse(b.b, b.n, &d), RES_OK);
    CHECK_EQ(d.nitems, 4);
    if (d.nitems != 4) return;
    CHECK_EQ(d.style, 0x80C80040u | 0x00040000);
    CHECK(d.x == 10 && d.y == 20 && d.cx == 180 && d.cy == 90);
    CHECK(d.menu_is_id && d.menu_id == 55);
    CHECK(!strcmp(d.cls, "MYCLASS"));
    CHECK(!strcmp(d.caption, "Open File"));
    CHECK_EQ(d.pointsize, 8);
    CHECK(!strcmp(d.face, "MS Sans Serif"));
    CHECK(!strcmp(d.items[0].cls, "BUTTON") && !strcmp(d.items[0].text, "OK") && d.items[0].id == 1);
    CHECK(d.items[0].x == 5 && d.items[0].y == 70 && d.items[0].cx == 40 && d.items[0].cy == 14);
    CHECK_EQ(d.items[0].style, 0x50010001u);
    CHECK(!strcmp(d.items[1].cls, "STATIC") && d.items[1].text_is_id && d.items[1].text_id == 321);
    CHECK(!strcmp(d.items[2].cls, "EDIT") && d.items[2].ncd == 3 && d.items[2].cdata[2] == 3);
    CHECK(!strcmp(d.items[3].cls, "MyCtl") && !strcmp(d.items[3].text, "x") && d.items[3].id == 9);
    res_dlg_free(&d);
    CHECK_EQ(res_dlg_parse(b.b, b.n - 4, &d), RES_BAD);     /* truncated in the last control */
    /* no font, no class, no menu */
    b.n = 0;
    rb_u32(&b, 0x80000000u); rb_u8(&b, 0);
    rb_u16(&b, 0); rb_u16(&b, 0); rb_u16(&b, 50); rb_u16(&b, 40);
    rb_u8(&b, 0); rb_u8(&b, 0); rb_str(&b, "T");
    CHECK_EQ(res_dlg_parse(b.b, b.n, &d), RES_OK);
    CHECK(!d.menu_is_id && d.menu[0] == 0 && d.cls[0] == 0 && !strcmp(d.caption, "T") && d.pointsize == 0);
    res_dlg_free(&d);
}

static void test_accel_parse(void) {
    rb_t b;
    res_accel a[8];
    size_t n = 0;
    printf("test_res_accel\n");
    b.n = 0;
    rb_u8(&b, AF_VIRTKEY | AF_CONTROL); rb_u16(&b, 'N'); rb_u16(&b, 100);
    rb_u8(&b, 0); rb_u16(&b, 'x'); rb_u16(&b, 101);
    rb_u8(&b, AF_VIRTKEY | AF_ALT | AF_SHIFT | AF_LAST); rb_u16(&b, 0x70); rb_u16(&b, 102);
    CHECK_EQ(res_accel_parse(b.b, b.n, a, 8, &n), RES_OK);
    CHECK_EQ(n, 3);
    CHECK(a[0].flags == (AF_VIRTKEY | AF_CONTROL) && a[0].key == 'N' && a[0].cmd == 100);
    CHECK(a[1].flags == 0 && a[1].key == 'x' && a[1].cmd == 101);
    CHECK(a[2].flags == (AF_VIRTKEY | AF_ALT | AF_SHIFT | AF_LAST) && a[2].key == 0x70 && a[2].cmd == 102);
    CHECK_EQ(res_accel_parse(b.b, b.n - 1, a, 8, &n), RES_BAD);   /* truncated */
    CHECK_EQ(res_accel_parse(b.b, b.n, a, 2, &n), RES_BAD);       /* more entries than room */
}

static void bih(rb_t *b, int w, int h, int bpp, int ncol) {
    rb_u32(b, 40); rb_u32(b, (uint32_t)w); rb_u32(b, (uint32_t)h);
    rb_u16(b, 1); rb_u16(b, (unsigned)bpp);
    rb_u32(b, 0); rb_u32(b, 0); rb_u32(b, 0); rb_u32(b, 0); rb_u32(b, (uint32_t)ncol); rb_u32(b, 0);
}
static void quad(rb_t *b, int r, int g, int bl) { rb_u8(b, bl); rb_u8(b, g); rb_u8(b, r); rb_u8(b, 0); }

static bool dib_ok(const rb_t *b, res_image *im) {
    res_err e = res_dib_decode(b->b, b->n, false, im);
    CHECK_EQ(e, RES_OK);
    return e == RES_OK;
}

static void test_dib_decode(void) {
    rb_t b;
    res_image im;
    printf("test_res_dib\n");
    /* 24 bpp 2x2, bottom-up: bottom row first. Row stride 8. */
    b.n = 0;
    bih(&b, 2, 2, 24, 0);
    rb_u8(&b, 0); rb_u8(&b, 0); rb_u8(&b, 255);  rb_u8(&b, 0); rb_u8(&b, 255); rb_u8(&b, 0);  rb_u16(&b, 0);  /* bottom: red, green */
    rb_u8(&b, 255); rb_u8(&b, 0); rb_u8(&b, 0);  rb_u8(&b, 255); rb_u8(&b, 255); rb_u8(&b, 255); rb_u16(&b, 0); /* top: blue, white */
    if (!dib_ok(&b, &im)) return;
    CHECK(im.w == 2 && im.h == 2 && im.bpp == 24 && im.mask == NULL);
    CHECK_EQ(im.cref[0], FB_RGB(0, 0, 255));
    CHECK_EQ(im.cref[1], FB_RGB(255, 255, 255));
    CHECK_EQ(im.cref[2], FB_RGB(255, 0, 0));
    CHECK_EQ(im.cref[3], FB_RGB(0, 255, 0));
    res_image_free(&im);
    /* 8 bpp 3x2 with a 4-entry table, stride 4 */
    b.n = 0;
    bih(&b, 3, 2, 8, 4);
    quad(&b, 0, 0, 0); quad(&b, 255, 0, 0); quad(&b, 0, 255, 0); quad(&b, 0, 0, 255);
    rb_u8(&b, 3); rb_u8(&b, 2); rb_u8(&b, 1); rb_u8(&b, 0);   /* bottom row */
    rb_u8(&b, 0); rb_u8(&b, 1); rb_u8(&b, 2); rb_u8(&b, 0);   /* top row */
    if (!dib_ok(&b, &im)) return;
    CHECK_EQ(im.cref[0], FB_RGB(0, 0, 0));
    CHECK_EQ(im.cref[2], FB_RGB(0, 255, 0));
    CHECK_EQ(im.cref[3], FB_RGB(0, 0, 255));
    CHECK_EQ(im.cref[5], FB_RGB(255, 0, 0));
    res_image_free(&im);
    /* 4 bpp 4x1: high nibble first; 1 bpp 8x1 */
    b.n = 0;
    bih(&b, 4, 1, 4, 3);
    quad(&b, 1, 2, 3); quad(&b, 4, 5, 6); quad(&b, 7, 8, 9);
    rb_u8(&b, 0x01); rb_u8(&b, 0x20); rb_u16(&b, 0);
    if (!dib_ok(&b, &im)) return;
    CHECK_EQ(im.cref[0], FB_RGB(1, 2, 3));
    CHECK_EQ(im.cref[1], FB_RGB(4, 5, 6));
    CHECK_EQ(im.cref[2], FB_RGB(7, 8, 9));
    CHECK_EQ(im.cref[3], FB_RGB(1, 2, 3));
    res_image_free(&im);
    b.n = 0;
    bih(&b, 8, 1, 1, 2);
    quad(&b, 0, 0, 0); quad(&b, 255, 255, 255);
    rb_u8(&b, 0xA5); rb_u8(&b, 0); rb_u16(&b, 0);
    if (!dib_ok(&b, &im)) return;
    CHECK_EQ(im.cref[0], FB_RGB(255, 255, 255));
    CHECK_EQ(im.cref[1], FB_RGB(0, 0, 0));
    CHECK_EQ(im.cref[7], FB_RGB(255, 255, 255));
    res_image_free(&im);
    /* old BITMAPCOREHEADER, 8 bpp 2x1 with RGBTRIPLE table of 256 */
    b.n = 0;
    rb_u32(&b, 12); rb_u16(&b, 2); rb_u16(&b, 1); rb_u16(&b, 1); rb_u16(&b, 8);
    { int i; for (i = 0; i < 256; i++) { rb_u8(&b, (unsigned)(255 - i)); rb_u8(&b, (unsigned)i); rb_u8(&b, 0); } }
    rb_u8(&b, 2); rb_u8(&b, 9); rb_u16(&b, 0);
    if (!dib_ok(&b, &im)) return;
    CHECK_EQ(im.cref[0], FB_RGB(0, 2, 253));
    CHECK_EQ(im.cref[1], FB_RGB(0, 9, 246));
    res_image_free(&im);
    /* truncated bits */
    b.n = 0;
    bih(&b, 8, 8, 8, 2);
    quad(&b, 0, 0, 0); quad(&b, 255, 255, 255);
    CHECK_EQ(res_dib_decode(b.b, b.n, false, &im), RES_BAD);
    CHECK_EQ(res_dib_decode(b.b, 10, false, &im), RES_BAD);
}

static void test_icon_cursor(void) {
    rb_t b, g;
    res_image im;
    res_group_pick pk;
    int hx = -1, hy = -1, y;
    printf("test_res_icon_cursor\n");
    /* 8x8 4bpp icon: height 16 = XOR + AND. XOR: all colour 1 (red); AND mask: left half transparent */
    b.n = 0;
    bih(&b, 8, 16, 4, 2);
    quad(&b, 0, 0, 0); quad(&b, 255, 0, 0);
    for (y = 0; y < 8; y++) { rb_u8(&b, 0x11); rb_u8(&b, 0x11); rb_u8(&b, 0x11); rb_u8(&b, 0x11); }
    for (y = 0; y < 8; y++) { rb_u8(&b, 0xF0); rb_u8(&b, 0); rb_u16(&b, 0); }   /* AND rows are 4-byte padded */
    if (res_icon_decode(b.b, b.n, &im) != RES_OK) { CHECK(!"icon decode"); return; }
    CHECK(im.w == 8 && im.h == 8 && im.mask != NULL);
    CHECK_EQ(im.cref[0], FB_RGB(255, 0, 0));
    CHECK_EQ(im.mask[0], 1);                 /* bit set: transparent */
    CHECK_EQ(im.mask[3], 1);
    CHECK_EQ(im.mask[4], 0);
    CHECK_EQ(im.mask[8 * 7 + 7], 0);
    res_image_free(&im);
    /* cursor: hotspot words precede the DIB; 1 bpp 8x8 (XOR black/white + AND) */
    g.n = 0;
    rb_u16(&g, 3); rb_u16(&g, 4);
    bih(&g, 8, 16, 1, 2);
    quad(&g, 0, 0, 0); quad(&g, 255, 255, 255);
    for (y = 0; y < 8; y++) { rb_u8(&g, 0x0F); rb_u8(&g, 0); rb_u16(&g, 0); }   /* XOR */
    for (y = 0; y < 8; y++) { rb_u8(&g, 0xFF); rb_u8(&g, 0); rb_u16(&g, 0); }   /* AND */
    if (res_cursor_decode(g.b, g.n, &im, &hx, &hy) != RES_OK) { CHECK(!"cursor decode"); return; }
    CHECK(hx == 3 && hy == 4 && im.w == 8 && im.h == 8);
    CHECK_EQ(im.cref[0], FB_RGB(0, 0, 0));
    CHECK_EQ(im.cref[7], FB_RGB(255, 255, 255));
    CHECK_EQ(im.mask[0], 1);
    res_image_free(&im);
    /* group directory: two icons, 16x16 4bpp (id 11) and 32x32 8bpp (id 12), one 32x32 1bpp (id 13) */
    g.n = 0;
    rb_u16(&g, 0); rb_u16(&g, 1); rb_u16(&g, 3);
    rb_u8(&g, 16); rb_u8(&g, 16); rb_u8(&g, 16); rb_u8(&g, 0); rb_u16(&g, 1); rb_u16(&g, 4); rb_u32(&g, 300); rb_u16(&g, 11);
    rb_u8(&g, 32); rb_u8(&g, 32); rb_u8(&g, 0); rb_u8(&g, 0); rb_u16(&g, 1); rb_u16(&g, 8); rb_u32(&g, 1000); rb_u16(&g, 12);
    rb_u8(&g, 32); rb_u8(&g, 32); rb_u8(&g, 2); rb_u8(&g, 0); rb_u16(&g, 1); rb_u16(&g, 1); rb_u32(&g, 200); rb_u16(&g, 13);
    CHECK_EQ(res_group_count(g.b, g.n), 3);
    CHECK_EQ(res_group_pick_best(g.b, g.n, false, 32, 32, &pk), RES_OK);
    CHECK_EQ(pk.id, 12);
    CHECK_EQ(res_group_pick_best(g.b, g.n, false, 16, 16, &pk), RES_OK);
    CHECK_EQ(pk.id, 11);
    CHECK_EQ(res_group_count(g.b, 7), -1);
    /* cursor group: WORD width/height, planes, bpp, DWORD size, WORD id */
    g.n = 0;
    rb_u16(&g, 0); rb_u16(&g, 2); rb_u16(&g, 1);
    rb_u16(&g, 32); rb_u16(&g, 64); rb_u16(&g, 1); rb_u16(&g, 1); rb_u32(&g, 300); rb_u16(&g, 21);
    CHECK_EQ(res_group_pick_best(g.b, g.n, true, 32, 32, &pk), RES_OK);
    CHECK(pk.id == 21 && pk.w == 32 && pk.h == 32);   /* the stored height is XOR + AND */
}

/* The real resource tables */
static void real_exe(const char *var, const char *label) {
    ne_image ne;
    size_t t, r;
    unsigned counts[16] = {0}, bad = 0, icons_ok = 0, cursors_ok = 0, menu_items = 0, dlg_ctls = 0, strs = 0;
    printf("test_res_real_%s\n", label);
    if (!env_exe(var, &ne)) {
        printf("  skipped (%s not set)\n", var);
        return;
    }
    for (t = 0; t < ne.restype_count; t++) {
        const ne_restype *ty = &ne.restypes[t];
        if (ty->name[0]) continue;
        for (r = 0; r < ty->count; r++) {
            const ne_resource *rs = &ty->res[r];
            const uint8_t *d = ne.data + rs->offset;
            size_t n = rs->length;
            if (ty->id < 16) counts[ty->id]++;
            switch (ty->id) {
            case RT_STRING: {
                unsigned i;
                for (i = 0; i < 16; i++) {
                    const uint8_t *s;
                    size_t sl;
                    if (!res_bundle_string(d, n, i, &s, &sl)) { bad++; printf("  bad string bundle %u\n", rs->id); break; }
                    strs += sl != 0;
                }
                break;
            }
            case RT_MENU: {
                unsigned it, po, de;
                if (res_menu_count(d, n, &it, &po, &de) != RES_OK) { bad++; printf("  bad menu %u '%s'\n", rs->id, rs->name); }
                else menu_items += it;
                break;
            }
            case RT_DIALOG: {
                res_dlg dl;
                if (res_dlg_parse(d, n, &dl) != RES_OK) { bad++; printf("  bad dialog %u '%s'\n", rs->id, rs->name); }
                else { dlg_ctls += dl.nitems; res_dlg_free(&dl); }
                break;
            }
            case RT_ACCELERATOR: {
                res_accel a[256];
                size_t na;
                if (res_accel_parse(d, n, a, 256, &na) != RES_OK) { bad++; printf("  bad accelerator %u\n", rs->id); }
                break;
            }
            case RT_BITMAP: {
                res_image im;
                if (res_dib_decode(d, n, false, &im) != RES_OK) { bad++; printf("  bad bitmap %u '%s'\n", rs->id, rs->name); }
                else res_image_free(&im);
                break;
            }
            case RT_GROUP_ICON:
            case RT_GROUP_CURSOR: {
                res_group_pick pk;
                const uint8_t *id;
                size_t il;
                res_image im;
                bool cur = ty->id == RT_GROUP_CURSOR;
                int hx, hy;
                res_err e = res_group_pick_best(d, n, cur, 32, 32, &pk);
                if (e == RES_OK) e = res_find(&ne, cur ? RT_CURSOR : RT_ICON, false, pk.id, NULL, &id, &il);
                if (e == RES_OK) e = cur ? res_cursor_decode(id, il, &im, &hx, &hy) : res_icon_decode(id, il, &im);
                if (e != RES_OK) { bad++; printf("  bad %s group %u '%s' (%d)\n", cur ? "cursor" : "icon", rs->id, rs->name, e); }
                else { res_image_free(&im); if (cur) cursors_ok++; else icons_ok++; }
                break;
            }
            default: break;
            }
        }
    }
    printf("  %s: menus %u (%u items), dialogs %u (%u controls), accelerators %u, string bundles %u (%u strings), "
           "bitmaps %u, icon groups %u (%u decoded), cursor groups %u (%u decoded), bad %u\n",
           label, counts[RT_MENU], menu_items, counts[RT_DIALOG], dlg_ctls, counts[RT_ACCELERATOR], counts[RT_STRING],
           strs, counts[RT_BITMAP], counts[RT_GROUP_ICON], icons_ok, counts[RT_GROUP_CURSOR], cursors_ok, bad);
    CHECK_EQ(bad, 0);
    CHECK_EQ(icons_ok, counts[RT_GROUP_ICON]);
    CHECK_EQ(cursors_ok, counts[RT_GROUP_CURSOR]);
    ne_free(&ne);
}

int main(void) {
    test_string_bundles();
    test_menu_walk();
    test_dialog_parse();
    test_accel_parse();
    test_dib_decode();
    test_icon_cursor();
    real_exe("CWFA_WRITER_EXE", "WRITER");
    real_exe("CWFA_ARTIST_EXE", "ARTIST");
    printf("test_res: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
