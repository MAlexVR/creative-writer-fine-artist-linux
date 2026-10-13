#include <stdbool.h>

#include "hostdir.h"
#include "dospath.h"
#include "ini.h"

static int g_checks, g_fails;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_fails++;                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

static char root[512];
static dospath_t dp;

static void setup_tree(void) {
    char *r = hd_make();
    snprintf(root, sizeof root, "%s", r);
    hd_mkdir(root, "MSKIDS");
    hd_mkdir(root, "MSKIDS/WRITER");
    hd_mkdir(root, "Setup");
    hd_write(root, "MSKIDS/WRITER.EXE", "MZ", 2);
    hd_write(root, "Setup/Data.Bin", "bin", 3);
    hd_write(root, "AUTORUN.INF", "x", 1);
    dp_init(&dp);
    CHECK(dp_map(&dp, 'D', root));
}

static void teardown(void) {
    dp_free(&dp);
    hd_remove(root);
}

static bool host_is(const dp_result *r, const char *rel) {
    char want[1024];
    snprintf(want, sizeof want, "%s/%s", root, rel);
    return !strcmp(r->host, want);
}

static void test_resolve_case_insensitive(void) {
    dp_result r;
    printf("test_resolve_case_insensitive\n");
    setup_tree();
    CHECK(dp_resolve(&dp, "D:\\MSKIDS\\WRITER.EXE", 0, &r));
    CHECK(r.exists && !r.is_dir && host_is(&r, "MSKIDS/WRITER.EXE"));
    CHECK(!strcmp(r.dos, "D:\\MSKIDS\\WRITER.EXE"));
    CHECK(dp_resolve(&dp, "d:\\mskids\\writer.exe", 0, &r));
    CHECK(r.exists && host_is(&r, "MSKIDS/WRITER.EXE") && !strcmp(r.dos, "D:\\MSKIDS\\WRITER.EXE"));
    CHECK(dp_resolve(&dp, "D:\\SETUP\\DATA.BIN", 0, &r)); /* host file is Setup/Data.Bin */
    CHECK(r.exists && host_is(&r, "Setup/Data.Bin"));
    CHECK(dp_resolve(&dp, "D:/mskids/writer", 0, &r));    /* directory, forward slashes */
    CHECK(r.exists && r.is_dir && host_is(&r, "MSKIDS/WRITER"));
    CHECK(dp_resolve(&dp, "D:\\", 0, &r));
    CHECK(r.exists && r.is_dir && !strcmp(r.host, root));
    CHECK(!strcmp(r.dos, "D:\\"));
    teardown();
}

static void test_missing_components(void) {
    dp_result r;
    printf("test_missing_components\n");
    setup_tree();
    CHECK(dp_resolve(&dp, "D:\\MSKIDS\\new file.txt", 0, &r)); /* last component may not exist */
    CHECK(!r.exists && host_is(&r, "MSKIDS/NEW FILE.TXT") && !strcmp(r.dos, "D:\\MSKIDS\\NEW FILE.TXT"));
    CHECK(!dp_resolve(&dp, "D:\\NOPE\\X.TXT", 0, &r));          /* missing directory */
    CHECK(dp_resolve(&dp, "D:\\NOPE\\X.TXT", DP_CREATE_DIRS, &r));
    CHECK(!r.exists && hd_exists(root, "NOPE") && host_is(&r, "NOPE/X.TXT"));
    CHECK(!dp_resolve(&dp, "E:\\X.TXT", 0, &r));                /* unmapped drive */
    CHECK(!dp_resolve(&dp, "", 0, &r));
    CHECK(!dp_resolve(&dp, NULL, 0, &r));
    teardown();
}

static void test_cwd_and_relative(void) {
    dp_result r;
    char buf[64];
    printf("test_cwd_and_relative\n");
    setup_tree();
    dp.cur_drive = 'D';
    strcpy(dp.cur_dir['D' - 'A'], "\\");
    CHECK(dp_resolve(&dp, "autorun.inf", 0, &r) && r.exists && host_is(&r, "AUTORUN.INF"));
    CHECK(dp_chdir(&dp, "D:\\mskids"));
    CHECK(dp_getcwd(&dp, 0, buf, sizeof buf) && !strcmp(buf, "MSKIDS"));
    CHECK(dp_getcwd(&dp, 4, buf, sizeof buf) && !strcmp(buf, "MSKIDS")); /* drive 4 = D: */
    CHECK(dp_resolve(&dp, "writer.exe", 0, &r) && r.exists && host_is(&r, "MSKIDS/WRITER.EXE"));
    CHECK(dp_resolve(&dp, "WRITER\\..\\WRITER.EXE", 0, &r) && r.exists && host_is(&r, "MSKIDS/WRITER.EXE"));
    CHECK(dp_resolve(&dp, "..\\AUTORUN.INF", 0, &r) && r.exists && host_is(&r, "AUTORUN.INF"));
    CHECK(dp_resolve(&dp, "\\autorun.inf", 0, &r) && r.exists);   /* root-relative on the current drive */
    CHECK(dp_resolve(&dp, "D:writer.exe", 0, &r) && r.exists);    /* drive-relative uses that drive's cwd */
    CHECK(!dp_chdir(&dp, "D:\\NOSUCHDIR"));
    CHECK(!dp_chdir(&dp, "D:\\MSKIDS\\WRITER.EXE"));              /* a file is not a directory */
    CHECK(dp_getcwd(&dp, 0, buf, sizeof buf) && !strcmp(buf, "MSKIDS"));
    CHECK(!dp_getcwd(&dp, 9, buf, sizeof buf));                   /* unmapped drive */
    teardown();
}

static void test_cannot_escape_root(void) {
    dp_result r;
    printf("test_cannot_escape_root\n");
    setup_tree();
    CHECK(dp_resolve(&dp, "D:\\..\\..\\AUTORUN.INF", 0, &r));
    CHECK(r.exists && host_is(&r, "AUTORUN.INF"));                 /* .. stops at the drive root */
    CHECK(!dp_resolve(&dp, "D:\\..\\..\\etc\\passwd", 0, &r) || strncmp(r.host, root, strlen(root)) == 0);
    CHECK(dp_resolve(&dp, "D:\\MSKIDS\\..\\..\\..", 0, &r) && !strcmp(r.host, root));
    teardown();
}

/* ---- ini ---------------------------------------------------------------- */

static void test_ini_read(void) {
    char ini[600], buf[128];
    static const char text[] =
        "; windows settings\n"
        "[windows]\n"
        "Device=HP LaserJet\n"
        "  spaced key  =  spaced value  \n"
        "\n"
        "[Colors]\r\n"
        "Background=255 255 255\r\n"
        "[colors2]\n"
        "Background=other\n"
        "last=no newline at end";
    char *r = hd_make();
    printf("test_ini_read\n");
    snprintf(root, sizeof root, "%s", r);
    hd_write(root, "WIN.INI", text, sizeof text - 1);
    snprintf(ini, sizeof ini, "%s/WIN.INI", root);
    CHECK(ini_lookup(ini, "windows", "device", buf, sizeof buf) && !strcmp(buf, "HP LaserJet"));
    CHECK(ini_lookup(ini, "WINDOWS", "SPACED KEY", buf, sizeof buf) && !strcmp(buf, "spaced value"));
    CHECK(ini_lookup(ini, "colors", "background", buf, sizeof buf) && !strcmp(buf, "255 255 255"));
    CHECK(ini_lookup(ini, "colors2", "background", buf, sizeof buf) && !strcmp(buf, "other"));
    CHECK(ini_lookup(ini, "colors2", "last", buf, sizeof buf) && !strcmp(buf, "no newline at end"));
    CHECK(!ini_lookup(ini, "windows", "background", buf, sizeof buf)); /* other section's key */
    CHECK(!ini_lookup(ini, "nosuch", "device", buf, sizeof buf));
    CHECK(!ini_lookup(ini, "windows", "device;", buf, sizeof buf));
    CHECK(ini_get_string(ini, "windows", "device", "dflt", buf, sizeof buf) == 11 && !strcmp(buf, "HP LaserJet"));
    CHECK(ini_get_string(ini, "windows", "missing", "dflt", buf, sizeof buf) == 4 && !strcmp(buf, "dflt"));
    CHECK(ini_get_string(ini, "windows", "device", "", buf, 5) == 4 && !strcmp(buf, "HP L")); /* truncated */
    CHECK(ini_get_string("/nonexistent/dir/x.ini", "a", "b", "fallback", buf, sizeof buf) == 8);
    /* enumeration: NULL key lists keys, NULL section lists sections */
    CHECK(ini_get_string(ini, "windows", NULL, "", buf, sizeof buf) == 18);
    CHECK(!memcmp(buf, "Device\0spaced key\0\0", 19));
    CHECK(ini_get_string(ini, NULL, NULL, "", buf, sizeof buf) == 23);
    CHECK(!memcmp(buf, "windows\0Colors\0colors2\0\0", 24));
    hd_remove(root);
}

static void test_ini_write(void) {
    char ini[600], buf[512];
    char *r = hd_make();
    printf("test_ini_write\n");
    snprintf(root, sizeof root, "%s", r);
    snprintf(ini, sizeof ini, "%s/PRIVATE.INI", root);
    CHECK(ini_write(ini, "Options", "Sound", "On"));              /* creates the file */
    CHECK(ini_lookup(ini, "options", "sound", buf, sizeof buf) && !strcmp(buf, "On"));
    CHECK(ini_write(ini, "Options", "Music", "Off"));
    CHECK(ini_write(ini, "Other", "Key", "1"));
    CHECK(ini_write(ini, "options", "SOUND", "Off"));             /* replaces, keeps the first spelling */
    CHECK(ini_lookup(ini, "Options", "Sound", buf, sizeof buf) && !strcmp(buf, "Off"));
    CHECK(ini_lookup(ini, "Options", "Music", buf, sizeof buf) && !strcmp(buf, "Off"));
    CHECK(ini_write(ini, "Options", "Late", "x"));                /* added at the end of its section */
    hd_read(root, "PRIVATE.INI", buf, sizeof buf);
    CHECK(strstr(buf, "[Options]\nSound=Off\nMusic=Off\nLate=x\n") != NULL);
    CHECK(ini_write(ini, "Options", "Music", NULL));              /* delete a key */
    CHECK(!ini_lookup(ini, "Options", "Music", buf, sizeof buf));
    CHECK(ini_lookup(ini, "Options", "Late", buf, sizeof buf));
    CHECK(ini_write(ini, "Options", NULL, NULL));                 /* delete a section */
    CHECK(!ini_lookup(ini, "Options", "Sound", buf, sizeof buf));
    CHECK(ini_lookup(ini, "Other", "Key", buf, sizeof buf) && !strcmp(buf, "1"));
    CHECK(!ini_write("/nonexistent/dir/x.ini", "a", "b", "c"));
    hd_remove(root);
}

int main(void) {
    test_resolve_case_insensitive();
    test_missing_components();
    test_cwd_and_relative();
    test_cannot_escape_root();
    test_ini_read();
    test_ini_write();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
