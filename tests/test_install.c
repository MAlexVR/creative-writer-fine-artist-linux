/* Staging of an installed C: drive from the CD (SETUP.INF + the CD's MSKIDS tree). */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fixture.h"
#include "hostdir.h"
#include "ini.h"
#include "install.h"

static char g_cd[600], g_c[600];

static bool is_link(const char *rel) {
    char p[1300];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", g_c, rel);
    return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
}

static bool is_dir(const char *rel) {
    char p[1300];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", g_c, rel);
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static unsigned count_entries(const char *rel) {
    char p[1300];
    DIR *d;
    unsigned n = 0;
    snprintf(p, sizeof p, "%s/%s", g_c, rel);
    d = opendir(p);
    if (!d) return 9999;
    while (readdir(d)) n++;
    closedir(d);
    return n - 2;
}

static const char inf_text[] =
    "; Setup script\n"
    "[Source Media Descriptions]\n"
    "\t\"1\", \"Disk\", \"\", \"\"\n"
    "[Files]\n"
    "\t1, shared\\users\\max\\banner1.max,,,!COPY,,,,,,REMOVE,,,,,,,,,,\n"   /* a removal row: no destination */
    "\"App\"=1, writer.ex_,,,,1995-09-24,,,ALWAYS,,,writer.exe,,,,1103488,,,,,VITAL\n"
    "\t1, writer.pic,,,,1995-07-19,,,ALWAYS,,,writer\\writer.pic,,,,1930240,,,,,VITAL\n"
    "\t1, writer.snd,,,,1995-07-19,,,ALWAYS,,,writer\\writer.snd,,,,562320,,,,,VITAL\n"
    "\t1, grcache.opt,,,,1995-07-19,,,ALWAYS,,,shared\\stuff\\grcache.opt,,,,1430016,,,,,VITAL\n"
    "\t1, absent.dat,,,,1995-07-19,,,ALWAYS,,,writer\\absent.dat,,,,1,,,,,VITAL\n"
    "[WinSysStuff]\n"
    "\t1, kidswtch.dll,,,,1995-07-19,,,OLDER,,,kidswtch.dll,,,, 17064,,,,1.1.0.0018,VITAL\n"
    "[OLE2]\n"
    "\t1,OLE2.DLL,,,,1994-03-03,,1033,OLDER,,,OLE2.DLL,,,,313344,SYSTEM,,,2.1.100.14,\n";

static void make_cd(void) {
    char *root = hd_make();
    snprintf(g_cd, sizeof g_cd, "%s/cd", root);
    snprintf(g_c, sizeof g_c, "%s/c", root);
    hd_mkdir(root, "cd");
    hd_mkdir(root, "cd/SETUP");
    hd_mkdir(root, "cd/MSKIDS");
    hd_mkdir(root, "cd/MSKIDS/SHARED");
    hd_mkdir(root, "cd/MSKIDS/SHARED/STUFF");
    hd_mkdir(root, "cd/MSKIDS/SHARED/USERS");
    hd_mkdir(root, "cd/MSKIDS/SHARED/USERS/MAG00PIC");
    hd_mkdir(root, "cd/MSKIDS/WRITER");
    hd_write(root, "cd/SETUP/SETUP.INF", inf_text, sizeof inf_text - 1);
    hd_write(root, "cd/SETUP/WRITER.PIC", "pic", 3);
    hd_write(root, "cd/SETUP/writer.snd", "snd", 3);                   /* lower case on the media */
    hd_write(root, "cd/SETUP/GRCACHE.OPT", "opt", 3);
    hd_write(root, "cd/SETUP/KIDSWTCH.DLL", "dll", 3);
    hd_write(root, "cd/SETUP/OLE2.DLL", "ole", 3);
    hd_write(root, "cd/SETUP/WRITER.EX_", "packed", 6);
    hd_write(root, "cd/MSKIDS/SHARED/MSKIDS.INF", "inf", 3);
    hd_write(root, "cd/MSKIDS/SHARED/STUFF/A.DAT", "a", 1);
    hd_write(root, "cd/MSKIDS/SHARED/USERS/MAG00PIC/X.DAT", "x", 1);
    hd_write(root, "cd/MSKIDS/WRITER/CWANIM.PIC", "anim", 4);
    hd_write(root, "WRITER.EXE", "exe", 3);
}

static void test_stage_synthetic(void) {
    install_report rep;
    char exe[720], root[700], buf[16], p[1400];
    printf("test_install_stage\n");
    make_cd();
    snprintf(root, sizeof root, "%.*s", (int)(strlen(g_cd) - 3), g_cd);
    snprintf(exe, sizeof exe, "%s/WRITER.EXE", root);
    CHECK(install_stage(g_cd, g_c, exe, &rep));
    CHECK(rep.had_inf);
    CHECK(is_link("MSKIDS/WRITER/WRITER.PIC"));                        /* [Files], destination from column 11 */
    CHECK(is_link("MSKIDS/WRITER/WRITER.SND"));                        /* the source file name's case does not matter */
    CHECK(is_link("MSKIDS/SHARED/STUFF/GRCACHE.OPT"));
    CHECK(is_link("WINDOWS/SYSTEM/KIDSWTCH.DLL"));                     /* [WinSysStuff] */
    CHECK(is_link("MSKIDS/WRITER.EXE"));                               /* the program */
    CHECK(is_link("MSKIDS/WRITER/CWANIM.PIC"));                        /* the CD's own tree */
    CHECK(is_link("MSKIDS/SHARED/MSKIDS.INF"));
    CHECK(is_link("MSKIDS/SHARED/STUFF/A.DAT"));
    CHECK(is_dir("MSKIDS/SHARED/STUFF") && !is_link("MSKIDS/SHARED/STUFF"));   /* real directories */
    CHECK(!is_link("MSKIDS/WRITER"));
    CHECK(is_dir("MSKIDS/SHARED/USERS") && count_entries("MSKIDS/SHARED/USERS") == 0); /* user data starts empty */
    CHECK(is_dir("MSKIDS/USERS") && count_entries("MSKIDS/USERS") == 0);
    CHECK(is_dir("WINDOWS"));
    CHECK(!is_dir("WINDOWS/SYSTEM/OLE2.DLL") && !is_link("WINDOWS/SYSTEM/OLE2.DLL")); /* other sections stay on the CD */
    CHECK(!is_link("MSKIDS/WRITER.EX_") && !is_link("MSKIDS/WRITER/WRITER.EX_"));      /* compressed: skipped */
    CHECK(!is_link("MSKIDS/WRITER/ABSENT.DAT"));
    CHECK_EQ(rep.missing, 1);                                          /* absent.dat is not on the CD */
    snprintf(p, sizeof p, "%s/MSKIDS/WRITER/WRITER.SND", g_c);
    {
        FILE *f = fopen(p, "rb");                                      /* the link reads the CD file */
        size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        buf[n] = 0;
        if (f) fclose(f);
        CHECK(!strcmp(buf, "snd"));
    }
    /* the virtual PDF printer is registered in WIN.INI so the programs believe a printer exists */
    snprintf(p, sizeof p, "%s/WINDOWS/WIN.INI", g_c);
    {
        char v[80];
        CHECK(ini_lookup(p, "windows", "device", v, sizeof v) && !strcmp(v, "PDF Printer,CWFAPRN,FILE:"));
        CHECK(ini_lookup(p, "devices", "PDF Printer", v, sizeof v) && !strcmp(v, "CWFAPRN,FILE:"));
        CHECK(ini_lookup(p, "PrinterPorts", "PDF Printer", v, sizeof v) && !strcmp(v, "CWFAPRN,FILE:,15,45"));
        CHECK(ini_write(p, "windows", "device", "Other,OTHERDRV,LPT1:"));          /* a user's own choice stays */
        CHECK(install_stage(g_cd, g_c, exe, &rep));
        CHECK(ini_lookup(p, "windows", "device", v, sizeof v) && !strcmp(v, "Other,OTHERDRV,LPT1:"));
    }
    /* idempotent, and what exists is left alone (a user file is not replaced) */
    snprintf(p, sizeof p, "%s/MSKIDS/USERS/ME.DAT", g_c);
    {
        FILE *f = fopen(p, "wb");
        if (f) { fputs("mine", f); fclose(f); }
    }
    CHECK(install_stage(g_cd, g_c, exe, &rep));
    CHECK_EQ(rep.linked, 0);
    CHECK(rep.skipped > 5);
    CHECK_EQ(count_entries("MSKIDS/USERS"), 1);
    hd_remove(strstr(root, "cwfa_test_") ? root : "");
}

static void test_stage_errors(void) {
    install_report rep;
    printf("test_install_errors\n");
    make_cd();
    CHECK(!install_stage("/nonexistent/cd", g_c, NULL, &rep));
    CHECK(strstr(rep.err, "/nonexistent/cd") != NULL);
    {   /* no SETUP.INF: the CD's tree is still mirrored, and the report says there was no INF */
        char cmd[1400];
        snprintf(cmd, sizeof cmd, "rm -f '%s/SETUP/SETUP.INF'", g_cd);
        CHECK_EQ(system(cmd), 0);
        CHECK(install_stage(g_cd, g_c, NULL, &rep));
        CHECK(!rep.had_inf);
        CHECK(is_link("MSKIDS/SHARED/STUFF/A.DAT"));
        CHECK(!is_link("MSKIDS/WRITER/WRITER.SND"));
    }
    {
        char *top = strrchr(g_cd, '/');
        if (top) *top = 0;
        hd_remove(g_cd);
    }
}

/* The real CD (skipped when CWFA_CD_ROOT is unset): everything SETUP.INF names for the two programs and
 * KIDSWTCH.DLL lands where the programs look for it, as a link that resolves. */
static void test_stage_real_cd(void) {
    const char *cd = getenv("CWFA_CD_ROOT");
    install_report rep;
    static const char *const want[] = {
        "MSKIDS/WRITER/WRITER.PIC", "MSKIDS/WRITER/WRITER.SND", "MSKIDS/WRITER/SPELL.DCT", "MSKIDS/WRITER/THES.DCT",
        "MSKIDS/WRITER/PICTIMP.FLT", "MSKIDS/WRITER/CWANIM.PIC", "MSKIDS/ARTIST/ARTIST.PIC", "MSKIDS/ARTIST/ARTIST.SND",
        "MSKIDS/ARTIST/FAANIM.PIC", "MSKIDS/SHARED/STUFF/GRCACHE.OPT", "MSKIDS/SHARED/STUFF/PABLO.FON",
        "MSKIDS/SHARED/MSKIDS.INF", "WINDOWS/SYSTEM/KIDSWTCH.DLL",
    };
    char tmpl[] = "/tmp/cwfa_test_realXXXXXX";
    char *root;
    size_t i;
    printf("test_install_real_cd\n");
    if (!cd || !*cd) {
        printf("  SKIP (CWFA_CD_ROOT not set)\n");
        return;
    }
    root = mkdtemp(tmpl);
    CHECK(root != NULL);
    if (!root) return;
    snprintf(g_c, sizeof g_c, "%s/c", root);
    CHECK(install_stage(cd, g_c, NULL, &rep));
    CHECK(rep.had_inf);
    CHECK_EQ(rep.missing, 0);
    for (i = 0; i < sizeof want / sizeof *want; i++) {
        char p[1400];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", g_c, want[i]);
        g_checks++;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) {   /* stat follows the link: it must resolve */
            g_fails++;
            printf("  FAIL missing or dangling: %s\n", want[i]);
        }
    }
    CHECK(is_dir("MSKIDS/SHARED/USERS") && count_entries("MSKIDS/SHARED/USERS") == 0);
    CHECK(!is_link("MSKIDS/WRITER.EXE"));                              /* no program given */
    hd_remove(root);
}

int main(void) {
    test_stage_synthetic();
    test_stage_errors();
    test_stage_real_cd();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
