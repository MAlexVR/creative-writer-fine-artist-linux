/* Staging of an installed C: drive from the Spanish KWAJ-compressed setup media (src/install_es.h). Synthetic
 * media always; with CWFA_ES_CD (CD layout) and/or CWFA_ES_FA (directory of DISCOn) the real disks too. */
#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>

#include "fixture.h"
#include "hostdir.h"
#include "ini.h"
#include "install.h"
#include "install_es.h"

static char g_root[400], g_src[500], g_c[500];

/* a stored (method 0) KWAJ container around data */
static size_t kwaj0(uint8_t *out, const void *data, size_t n) {
    static const uint8_t h[18] = {'K', 'W', 'A', 'J', 0x88, 0xF0, 0x27, 0xD1, 0, 0, 18, 0, 1, 0};
    memcpy(out, h, 18);
    out[14] = (uint8_t)n;
    out[15] = (uint8_t)(n >> 8);
    out[16] = (uint8_t)(n >> 16);
    out[17] = (uint8_t)(n >> 24);
    memcpy(out + 18, data, n);
    return 18 + n;
}

static void put_kwaj(const char *dir, const char *name, const char *data) {
    uint8_t buf[4096];
    char rel[300];
    size_t n = kwaj0(buf, data, strlen(data));
    snprintf(rel, sizeof rel, "%s/%s", dir, name);
    hd_write(g_root, rel, buf, n);
}

static const char script[] =
    "[Source Media Descriptions]\n"
    "    \"1\",\"Disk 1\",\"instalar.exe\",\"..\\disco1\"\n"
    "[Files]\n"
    "    1,writer.exe,,,,1994-09-01,,,ALWAYS,,,writer.exe,,,,1,,,,,VITAL\n"
    "    2,big.pi_,,,,1994-09-01,,,ALWAYS,,,writer\\big.pic,,,,1,,,,,VITAL\n"
    "    3,big2.pi_,writer\\big.pic,,,1994-09-01,,,OLDER,,,,,,,1,,,,,VITAL\n"
    "    4,f_caba\xf1" "a.max,,,,1994-09-01,,,OLDER,,,shared\\users\\max\\f_caba\xf1" "a.max,,,,1,,,,,\n"
    "    4,nodest.dat,,,,1994-09-01,,,OLDER,,,,,,,1,,,,,\n"
    "    4,absent.dat,,,,1994-09-01,,,OLDER,,,writer\\absent.dat,,,,1,,,,,\n"
    "[Fonts]\n"
    "\"Stencil\" = 1, stencil.ttf,,,, 1994-09-01,,, NEVER,,,,,,, 48724,,,,,\n"
    "[Shared Fonts]\n"
    "\"Stencil\" = 1, stencil.ttf,,,,1994-09-01,,, NEVER,,,shared\\stuff\\stencil.ttf,,,, 48724,,,,,\n"
    "[Net Admin]\n"
    "\"App\"=1, _setup.exe,,,,1994-09-01,,,ALWAYS,,,maxsetup.exe,,,,521728,,,,,\n";

static void make_media(bool floppies) {
    const char *d1 = floppies ? "src/DISCO1" : "src", *d2 = floppies ? "src/disco2" : "src";
    char *root = hd_make();
    snprintf(g_root, sizeof g_root, "%s", root);
    snprintf(g_src, sizeof g_src, "%s/src", g_root);
    snprintf(g_c, sizeof g_c, "%s/c", g_root);
    hd_mkdir(g_root, "src");
    if (floppies) {
        hd_mkdir(g_root, d1);
        hd_mkdir(g_root, d2);
    }
    {
        char rel[300];
        snprintf(rel, sizeof rel, "%s/INSTALAR.LST", d1);
        hd_write(g_root, rel, "[Params]\n", 9);
        snprintf(rel, sizeof rel, "%s/_SETUP.IN_", d1);
        if (floppies) put_kwaj(d1, "_SETUP.IN_", script);       /* compressed on the floppies */
        else hd_write(g_root, rel, script, strlen(script));     /* plain on the CD */
    }
    put_kwaj(d1, "WRITER.EX_", "MZ-program");
    put_kwaj(d1, "STENCIL.TT_", "fontdata");
    put_kwaj(d2, "BIG.PI_", "AAAA");
    put_kwaj(d2, "BIG2.PI_", "BBBB");
    put_kwaj(d2, "F_CABA\xa5" "A.MA_", "ninia");              /* DOS name: the OEM capital N-tilde */
    put_kwaj(d2, "NODEST.DA_", "x");
}

static bool file_is(const char *rel, const char *want) {
    char buf[300];
    size_t n = hd_read(g_c, rel, buf, sizeof buf);
    return n == strlen(want) && !memcmp(buf, want, n);
}

static void check_synthetic(bool floppies) {
    install_report rep;
    CHECK(install_es_detect(g_src));
    CHECK(install_stage_es(g_src, g_c, NULL, &rep));
    CHECK(file_is("MSKIDS/WRITER.EXE", "MZ-program"));
    CHECK(file_is("MSKIDS/WRITER/BIG.PIC", "AAAABBBB"));         /* append-to rows concatenate in order */
    CHECK(file_is("MSKIDS/SHARED/STUFF/STENCIL.TTF", "fontdata"));
    CHECK(file_is("MSKIDS/SHARED/USERS/MAX/F_CABA\xa5" "A.MAX", "ninia"));   /* ANSI n-tilde in the script, OEM on disk */
    CHECK(!hd_exists(g_c, "MSKIDS/NODEST.DAT"));
    CHECK(hd_exists(g_c, "MSKIDS/USERS") && hd_exists(g_c, "WINDOWS/SYSTEM"));
    CHECK_EQ(rep.missing, 1);                                    /* absent.dat */
    CHECK(rep.had_inf);
    CHECK_EQ(rep.linked, 4);
    /* second run changes nothing and does not append twice */
    CHECK(install_stage_es(g_src, g_c, NULL, &rep));
    CHECK(file_is("MSKIDS/WRITER/BIG.PIC", "AAAABBBB"));
    CHECK(rep.skipped >= 4);
    (void)floppies;
}

/* The Spanish programs ask WIN.INI for the default printer exactly like the English ones; without the virtual PDF
 * printer registered, Creative Writer's print wizard ends in "Hay algun problema con tu impresora". */
static void check_printer_registered(void) {
    install_report rep;
    char p[600], v[80];
    CHECK(install_stage(g_src, g_c, NULL, &rep));
    snprintf(p, sizeof p, "%s/WINDOWS/WIN.INI", g_c);
    CHECK(ini_lookup(p, "windows", "device", v, sizeof v) && !strcmp(v, "PDF Printer,CWFAPRN,FILE:"));
    CHECK(ini_lookup(p, "devices", "PDF Printer", v, sizeof v) && !strcmp(v, "CWFAPRN,FILE:"));
    CHECK(ini_lookup(p, "PrinterPorts", "PDF Printer", v, sizeof v) && !strcmp(v, "CWFAPRN,FILE:,15,45"));
    CHECK(ini_write(p, "windows", "device", "Other,OTHERDRV,LPT1:"));                     /* a user's own choice stays */
    CHECK(install_stage(g_src, g_c, NULL, &rep));
    CHECK(ini_lookup(p, "windows", "device", v, sizeof v) && !strcmp(v, "Other,OTHERDRV,LPT1:"));
}

static void test_synthetic(void) {
    make_media(false);
    check_synthetic(false);
    check_printer_registered();
    hd_remove(g_root);
    make_media(true);
    CHECK(install_es_detect(g_src));
    check_synthetic(true);
    hd_remove(g_root);
}

static void test_detect_negative(void) {
    char *root = hd_make();
    snprintf(g_root, sizeof g_root, "%s", root);
    CHECK(!install_es_detect(g_root));
    CHECK(!install_es_detect("/nonexistent/path/xyz"));
    hd_mkdir(g_root, "SETUP");
    hd_write(g_root, "SETUP/SETUP.INF", "[Files]\n", 8);        /* the English CD is not Spanish media */
    CHECK(!install_es_detect(g_root));
    hd_remove(g_root);
}

static void test_errors(void) {
    install_report rep;
    char *root = hd_make();
    snprintf(g_root, sizeof g_root, "%s", root);
    CHECK(!install_stage_es("/nonexistent/path/xyz", "/tmp/cwfa_never", NULL, &rep));
    CHECK(rep.err[0]);
    hd_write(g_root, "INSTALAR.LST", "x", 1);                    /* media without a setup script */
    snprintf(g_c, sizeof g_c, "%s/c", g_root);
    CHECK(!install_stage_es(g_root, g_c, NULL, &rep));
    CHECK(strstr(rep.err, "script") != NULL);
    hd_remove(g_root);
}

static bool in_tree(const char *rel) {
    char p[1300];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", g_c, rel);
    return stat(p, &st) == 0 && st.st_size > 0;
}

static void test_real(const char *env, const char *const *want, int nwant) {
    const char *src = getenv(env);
    install_report rep;
    int i;
    if (!src || !*src) {
        printf("  skip real Spanish media (%s unset)\n", env);
        return;
    }
    {
        char *root = hd_make();
        snprintf(g_root, sizeof g_root, "%s", root);
        snprintf(g_c, sizeof g_c, "%s/c", g_root);
    }
    CHECK(install_es_detect(src));
    CHECK(install_stage_es(src, g_c, NULL, &rep));
    if (rep.err[0]) printf("  err: %s\n", rep.err);
    CHECK_EQ(rep.missing, 0);
    for (i = 0; i < nwant; i++) {
        if (!in_tree(want[i])) printf("  missing: %s\n", want[i]);
        CHECK(in_tree(want[i]));
    }
    hd_remove(g_root);
}

int main(void) {
    static const char *const cw[] = {"MSKIDS/WRITER.EXE", "MSKIDS/WRITER/WRITER.PIC", "MSKIDS/WRITER/WRITER.SND",
                                     "MSKIDS/WRITER/SPELL.DCT", "MSKIDS/WRITER/THES.DCT", "MSKIDS/SHARED/STUFF/PABLO.FON",
                                     "MSKIDS/SHARED/STUFF/IMPACT.TTF", "MSKIDS/SHARED/STUFF/MAX100.ART",
                                     "MSKIDS/SHARED/USERS/MAX/FELIZ1.MAX"};
    static const char *const fa[] = {"MSKIDS/ARTIST.EXE", "MSKIDS/ARTIST/ARTIST.PIC", "MSKIDS/ARTIST/ARTIST.SND",
                                     "MSKIDS/SHARED/STUFF/MAG100.ART", "MSKIDS/SHARED/USERS/MAGGIE/TITULO.MAG",
                                     "MSKIDS/SHARED/STUFF/LSANS.TTF"};
    test_detect_negative();
    test_synthetic();
    test_errors();
    test_real("CWFA_ES_CD", cw, 9);
    test_real("CWFA_ES_FA", fa, 6);
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
