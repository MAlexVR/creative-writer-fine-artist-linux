/* The GDI import surface of WRITER and ARTIST: every ordinal has a handler
 * under the name of the import table, the logged stubs say who owns them, and the
 * ARTIST-only ordinals are present. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "ne.h"

#include "gfixture.h"
#include "win16_imports.h"

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

/* WRITER.EXE's 116 GDI imports: ordinal (from the NE relocations) and the name the program's import table gives it
 * (tests/win16_imports.h, same order). */
static const struct { unsigned ord; const char *name; } verified[] = {
    {1, "SETBKCOLOR"}, {2, "SETBKMODE"}, {3, "SETMAPMODE"}, {4, "SETROP2"}, {6, "SETPOLYFILLMODE"},
    {7, "SETSTRETCHBLTMODE"}, {9, "SETTEXTCOLOR"}, {11, "SETWINDOWORG"}, {12, "SETWINDOWEXT"},
    {13, "SETVIEWPORTORG"}, {14, "SETVIEWPORTEXT"}, {19, "LINETO"}, {20, "MOVETO"}, {22, "INTERSECTCLIPRECT"},
    {24, "ELLIPSE"}, {25, "FLOODFILL"}, {27, "RECTANGLE"}, {28, "ROUNDRECT"}, {29, "PATBLT"}, {30, "SAVEDC"},
    {33, "TEXTOUT"}, {34, "BITBLT"}, {35, "STRETCHBLT"}, {36, "POLYGON"}, {37, "POLYLINE"}, {38, "ESCAPE"},
    {39, "RESTOREDC"}, {40, "FILLRGN"}, {41, "FRAMERGN"}, {42, "INVERTRGN"}, {43, "PAINTRGN"},
    {44, "SELECTCLIPRGN"}, {45, "SELECTOBJECT"}, {47, "COMBINERGN"}, {48, "CREATEBITMAP"},
    {49, "CREATEBITMAPINDIRECT"}, {50, "CREATEBRUSHINDIRECT"}, {51, "CREATECOMPATIBLEBITMAP"},
    {52, "CREATECOMPATIBLEDC"}, {53, "CREATEDC"}, {56, "CREATEFONT"}, {57, "CREATEFONTINDIRECT"},
    {58, "CREATEHATCHBRUSH"}, {60, "CREATEPATTERNBRUSH"}, {61, "CREATEPEN"}, {62, "CREATEPENINDIRECT"},
    {63, "CREATEPOLYGONRGN"}, {64, "CREATERECTRGN"}, {65, "CREATERECTRGNINDIRECT"}, {66, "CREATESOLIDBRUSH"},
    {67, "DPTOLP"}, {68, "DELETEDC"}, {69, "DELETEOBJECT"}, {74, "GETBITMAPBITS"}, {75, "GETBKCOLOR"},
    {77, "GETCLIPBOX"}, {78, "GETCURRENTPOSITION"}, {80, "GETDEVICECAPS"}, {81, "GETMAPMODE"},
    {82, "GETOBJECT"}, {83, "GETPIXEL"}, {87, "GETSTOCKOBJECT"}, {90, "GETTEXTCOLOR"}, {91, "GETTEXTEXTENT"},
    {93, "GETTEXTMETRICS"}, {99, "LPTODP"}, {101, "OFFSETRGN"}, {119, "ADDFONTRESOURCE"},
    {123, "PLAYMETAFILE"}, {124, "GETMETAFILE"}, {125, "CREATEMETAFILE"}, {126, "CLOSEMETAFILE"},
    {127, "DELETEMETAFILE"}, {136, "REMOVEFONTRESOURCE"}, {148, "SETBRUSHORG"}, {150, "UNREALIZEOBJECT"},
    {153, "CREATEIC"}, {154, "GETNEARESTCOLOR"}, {156, "CREATEDISCARDABLEBITMAP"}, {159, "GETMETAFILEBITS"},
    {160, "SETMETAFILEBITS"}, {175, "ENUMMETAFILE"}, {176, "PLAYMETAFILERECORD"},
    {308, "GETOUTLINETEXTMETRICS"}, {309, "GETGLYPHOUTLINE"}, {313, "GETRASTERIZERCAPS"},
    {330, "ENUMFONTFAMILIES"}, {345, "GETTEXTALIGN"}, {346, "SETTEXTALIGN"}, {350, "GETCHARWIDTH"},
    {351, "EXTTEXTOUT"}, {360, "CREATEPALETTE"}, {363, "GETPALETTEENTRIES"}, {364, "SETPALETTEENTRIES"},
    {370, "GETNEARESTPALETTEINDEX"}, {372, "EXTFLOODFILL"}, {376, "RESETDC"}, {377, "STARTDOC"},
    {378, "ENDDOC"}, {381, "SETABORTPROC"}, {382, "ABORTDOC"}, {439, "STRETCHDIBITS"}, {440, "SETDIBITS"},
    {441, "GETDIBITS"}, {442, "CREATEDIBITMAP"}, {445, "CREATEDIBPATTERNBRUSH"}, {450, "POLYPOLYGON"},
    {462, "ISGDIOBJECT"}, {470, "GETCURRENTPOSITIONEX"}, {471, "GETTEXTEXTENTPOINT"},
    {472, "GETVIEWPORTEXTEX"}, {473, "GETVIEWPORTORGEX"}, {474, "GETWINDOWEXTEX"}, {475, "GETWINDOWORGEX"},
    {479, "SETVIEWPORTEXTEX"}, {480, "SETVIEWPORTORGEX"},
};

static void test_ordinal_table(void) {
    size_t i, j, n = sizeof verified / sizeof verified[0], tsv = 0, found = 0;
    printf("test_gdi_ordinal_table\n");
    gboot();
    CHECK_EQ(n, 116);
    for (i = 0; i < n; i++) {
        const hle_entry *e = hle_find(&uh, "GDI", (uint16_t)verified[i].ord);
        CHECK(e != NULL && e->fn != NULL);
        if (e) CHECK(ieq(e->name, verified[i].name));
        if (!e) printf("    missing GDI.%u %s\n", verified[i].ord, verified[i].name);
        else if (!ieq(e->name, verified[i].name)) printf("    GDI.%u is %s, the import table says %s\n", verified[i].ord, e->name, verified[i].name);
    }
    for (i = 0; i < WIN16_IMPORT_COUNT; i++) {
        if (strcmp(win16_imports[i].module, "GDI")) continue;
        tsv++;
        for (j = 0; j < n; j++)
            if (ieq(win16_imports[i].name, verified[j].name)) { found++; break; }
    }
    CHECK_EQ(tsv, 116);
    CHECK_EQ(found, 116);
    /* ARTIST imports 10 more: named from the export list and the arguments at their call sites */
    {
        static const struct { unsigned ord; const char *name; unsigned args; } artist[] = {
            {55, "CreateEllipticRgnIndirect", 4}, {76, "GetBkMode", 2}, {84, "GetPolyFillMode", 2}, {85, "GetRelAbs", 2},
            {88, "GetStretchBltMode", 2}, {95, "GetViewportOrg", 2}, {106, "SetRectRgn", 10}, {128, "MulDiv", 6},
            {379, "StartPage", 2}, {380, "EndPage", 2}};
        for (i = 0; i < sizeof artist / sizeof artist[0]; i++) {
            const hle_entry *e = hle_find(&uh, "GDI", (uint16_t)artist[i].ord);
            CHECK(e != NULL && e->fn != NULL);
            if (e) {
                CHECK(ieq(e->name, artist[i].name));
                CHECK_EQ(e->arg_bytes, artist[i].args);
            }
        }
    }
    /* the USER half: every function that draws */
    {
        static const struct { unsigned ord; const char *name; } user[] = {{81, "FillRect"}, {82, "InvertRect"}, {83, "FrameRect"}, {85, "DrawText"},
                                                                         {221, "ScrollDC"}, {282, "SelectPalette"}, {283, "RealizePalette"},
                                                                         {466, "DrawFocusRect"}};
        for (i = 0; i < sizeof user / sizeof user[0]; i++) {
            const hle_entry *e = hle_find(&uh, "USER", (uint16_t)user[i].ord);
            CHECK(e != NULL && e->fn != NULL);
            if (e) CHECK(ieq(e->name, user[i].name));
        }
    }
    gdown();
}

static void test_logged_stubs(void) {
    static const struct { unsigned ord; unsigned words; const char *name; } stubs[] = {
        {124, 2, "GetMetaFile"}};
    size_t i;
    printf("test_gdi_logged_stubs\n");
    gboot();
    CHECK_EQ(gdi_stub_count(gg), 0);
    for (i = 0; i < sizeof stubs / sizeof stubs[0]; i++) {
        uint16_t w[8] = {0};
        const hle_entry *e = hle_find(&uh, "GDI", (uint16_t)stubs[i].ord);
        CHECK(e != NULL);
        if (!e) continue;
        CHECK_EQ(e->arg_bytes, stubs[i].words * 2u);
        CHECK_EQ(g_call("GDI", stubs[i].ord, w, stubs[i].words) & 0xFFFF, 0);
        {
            const gdi_stub_rec *r = gdi_stub_at(gg, i);
            CHECK(r != NULL);
            if (r) {
                CHECK_EQ(r->ordinal, stubs[i].ord);
                CHECK(ieq(r->name, stubs[i].name));
                CHECK(r->task != NULL && r->task[0] != 0);
                CHECK(r->caller_cs == u_cs());
            }
        }
    }
    CHECK_EQ(gdi_stub_count(gg), sizeof stubs / sizeof stubs[0]);
    /* printing belongs to gdi_print.c: on the display DC every escape is refused and the document calls fail */
    {
        uint16_t s = g_seg(0x20);
        uwr16(s, 0, 4);
        CHECK_EQ(GA(38, g_screen_dc(), 8, 2, FARP(s, 0), FARP(0, 0)), 0);
        CHECK_EQ(GA(377, g_screen_dc(), FARP(s, 0)), 0xFFFF);
        CHECK_EQ(GA(378, g_screen_dc()), 0xFFFF);
    }
    /* GetRelAbs / SetRelAbs: always absolute */
    CHECK_EQ(GA(85, g_screen_dc()), 1);
    CHECK_EQ(GA(5, g_screen_dc(), 2), 1);
    CHECK_EQ(GA(85, g_screen_dc()), 1);
    gdown();
}

/* Every GDI import of the real programs has a handler; the union over both is counted, and the logged
 * stubs (printing, Escape, GetMetaFile) are the ones with an owning task. */
static void test_real_imports(void) {
    static const char *const envs[2] = {"CWFA_WRITER_EXE", "CWFA_ARTIST_EXE"};
    static const unsigned stub_ords[] = {124};
    unsigned seen[512], nseen = 0, implemented = 0, stubbed = 0, e_i;
    printf("test_gdi_real_imports\n");
    gboot();
    for (e_i = 0; e_i < 2; e_i++) {
        const char *path = getenv(envs[e_i]);
        ne_image ne;
        ne_import *imp = NULL;
        size_t n = 0, i;
        unsigned total = 0, missing = 0;
        char name[NE_NAME_MAX + 1];
        if (!path || !*path) {
            printf("  SKIP (%s not set)\n", envs[e_i]);
            continue;
        }
        CHECK_EQ(ne_open_file(&ne, path), NE_OK);
        CHECK_EQ(ne_collect_imports(&ne, &imp, &n), NE_OK);
        for (i = 0; i < n; i++) {
            const hle_entry *e;
            unsigned k, j;
            if (ne_modref_name(&ne, imp[i].modref, name) != NE_OK || strcmp(name, "GDI") || imp[i].by_name) continue;
            total++;
            e = hle_find(&uh, "GDI", imp[i].ordinal);
            if (!e || !e->fn) {
                missing++;
                printf("    GDI.%u has no handler\n", imp[i].ordinal);
            }
            for (k = 0; k < nseen && seen[k] != imp[i].ordinal; k++) {}
            if (k == nseen && nseen < 512) {
                seen[nseen++] = imp[i].ordinal;
                for (j = 0; j < sizeof stub_ords / sizeof stub_ords[0]; j++)
                    if (stub_ords[j] == imp[i].ordinal) break;
                if (e && e->fn) {
                    if (j < sizeof stub_ords / sizeof stub_ords[0]) stubbed++;
                    else implemented++;
                }
            }
        }
        printf("  %s: %u GDI imports, %u without a handler\n", envs[e_i], total, missing);
        CHECK_EQ(missing, 0);
        CHECK_EQ(total, e_i == 0 ? 116u : 109u);
        ne_free_imports(imp);
        ne_free(&ne);
    }
    if (nseen) {
        printf("  union %u ordinals: %u implemented, %u logged stubs\n", nseen, implemented, stubbed);
        CHECK_EQ(nseen, 126u);
        CHECK_EQ(stubbed, 1u);
        CHECK_EQ(implemented, 125u);
    }
    gdown();
}

int main(void) {
    test_ordinal_table();
    test_real_imports();
    test_logged_stubs();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
