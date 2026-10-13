#define _XOPEN_SOURCE 700
/* scripts/package.sh builds one self-extracting executable; it carries no game data, refuses to run without the
 * CD, and (with the CD directory in CWFA_CD_ROOT, and the ISO in CWFA_ISO) runs the program headless from an empty
 * cache and an empty home. Plain asserts; the CD-dependent runs skip when the variables are unset. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fixture.h"

#define OUT "build/pkgtest"
#define ENVP "XDG_CACHE_HOME=" OUT "/cache XDG_DATA_HOME=" OUT "/data "

static int run(const char *cmd, char *out, size_t cap) {
    char full[4096];
    FILE *p;
    size_t n = 0;
    int c, status;
    snprintf(full, sizeof full, "%s 2>&1", cmd);
    p = popen(full, "r");
    if (!p) return -1;
    while ((c = fgetc(p)) != EOF)
        if (n + 1 < cap) out[n++] = (char)c;
    out[n] = 0;
    status = pclose(p);
    return status == -1 ? -1 : (status & 0x7F) ? 128 + (status & 0x7F) : (status >> 8) & 0xFF;
}

static char g_run[256];

static void test_build(void) {
    static char out[1 << 16];
    struct stat st;
    char cmd[1024];
    printf("test_package_build\n");
    CHECK_EQ(system("rm -rf " OUT), 0);
    CHECK_EQ(run("sh scripts/package.sh " OUT, out, sizeof out), 0);
    CHECK(strstr(out, "package:") != NULL);
    snprintf(g_run, sizeof g_run, OUT "/cwfa-portable-%s.run", "x86_64");
    {
        FILE *p = popen("uname -m", "r");
        char arch[32] = "";
        if (p && fgets(arch, sizeof arch, p)) arch[strcspn(arch, "\n")] = 0;
        if (p) pclose(p);
        snprintf(g_run, sizeof g_run, OUT "/cwfa-portable-%s.run", arch);
    }
    CHECK_EQ(stat(g_run, &st), 0);
    CHECK(st.st_mode & S_IXUSR);
    CHECK(st.st_size > 100000);                                    /* the binary and the SDL2 libraries */
    CHECK(st.st_size < 20 * 1024 * 1024);                          /* and no game data */
    /* the payload lists no game file */
    snprintf(cmd, sizeof cmd, "n=$(grep -an '^__PAYLOAD_BELOW__' %s | head -1 | cut -d: -f1); "
                              "tail -n +$((n + 1)) %s | tar -tJ", g_run, g_run);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(strstr(out, "bin/cwfa") != NULL);
    CHECK(strstr(out, "cwfa-launch") != NULL);
    CHECK(strstr(out, "lib/libSDL2") != NULL);
    CHECK(!strstr(out, ".EXE") && !strstr(out, ".EX_") && !strstr(out, ".PIC") && !strstr(out, ".ISO"));
}

static void test_usage(void) {
    static char out[1 << 16];
    char cmd[1024];
    printf("test_package_usage\n");
    snprintf(cmd, sizeof cmd, ENVP "%s", g_run);
    CHECK_EQ(run(cmd, out, sizeof out), 2);
    CHECK(strstr(out, "usage:"));
    snprintf(cmd, sizeof cmd, ENVP "env -u CWFA_CD %s writer", g_run);
    CHECK_EQ(run(cmd, out, sizeof out), 2);
    CHECK(strstr(out, "not part of this package"));                /* the CD must come from the user */
    snprintf(cmd, sizeof cmd, ENVP "%s writer --cd /nonexistent/cd", g_run);
    CHECK_EQ(run(cmd, out, sizeof out), 1);
    CHECK(strstr(out, "neither a directory nor a file"));
    snprintf(cmd, sizeof cmd, ENVP "%s artist --cd " OUT, g_run);  /* a directory that is not the CD */
    CHECK_EQ(run(cmd, out, sizeof out), 1);
    CHECK(strstr(out, "not found"));
}

static long file_size(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void smoke(const char *label, const char *prog, const char *cd) {
    static char out[1 << 16];
    char cmd[2048], shot[128];
    printf("test_package_smoke_%s\n", label);
    snprintf(shot, sizeof shot, OUT "/%s.ppm", label);
    snprintf(cmd, sizeof cmd,
             ENVP "CWFA_HOME=" OUT "/home-%s %s %s --cd '%s' --headless --frames 30 --max-steps 6000000000 --screenshot %s",
             label, g_run, prog, cd, shot);
    CHECK_EQ(run(cmd, out, sizeof out), 0);                        /* the frame limit: no fault, nothing unimplemented */
    CHECK(!strstr(out, "NOT IMPLEMENTED"));
    CHECK_EQ(file_size(shot), 640L * 480 * 3 + 15);                /* the login screen was drawn */
    /* the program's installed drive C: lives under the home the user chose, not under the package */
    snprintf(cmd, sizeof cmd, "test -d " OUT "/home-%s/%s/MSKIDS", label, prog);
    CHECK_EQ(system(cmd), 0);
}

/* Where the data lives (R2): by default ./cwfa-data in the directory the program is run from (not under $HOME and not
 * under the package); --data-dir DIR chooses another; CWFA_HOME keeps its per-program meaning; --install-dir and
 * --c-root still pass through; and the launcher says where. */
static void data_dir(const char *cd) {
    static char out[1 << 16];
    char cmd[8192], abs[1024], cdabs[1400], pre[4096];
    printf("test_package_data_dir\n");
    if (!getcwd(abs, sizeof abs)) return;
    snprintf(cdabs, sizeof cdabs, "%s%s%s", cd[0] == '/' ? "" : abs, cd[0] == '/' ? "" : "/", cd);
    CHECK_EQ(system("rm -rf " OUT "/work && mkdir -p " OUT "/work"), 0);
    snprintf(pre, sizeof pre, "cd " OUT "/work && env -u CWFA_HOME XDG_CACHE_HOME='%s/" OUT "/cache' XDG_DATA_HOME='%s/" OUT "/data' "
             "HOME='%s/" OUT "/fakehome' ", abs, abs, abs);
    /* 1: no option, no variable: ./cwfa-data of the current directory, shared by both programs */
    snprintf(cmd, sizeof cmd, "%s'%s/%s' writer --cd '%s' --headless --frames 20 --max-steps 3000000000", pre, abs, g_run, cdabs);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(strstr(out, "your data") && strstr(out, "/work/cwfa-data"));
    CHECK_EQ(system("test -d " OUT "/work/cwfa-data/MSKIDS/USERS"), 0);
    CHECK_EQ(system("test -f " OUT "/work/cwfa-data/WINDOWS/WIN.INI"), 0);
    CHECK(system("test -e " OUT "/fakehome/.local/share/cwfa/writer/MSKIDS") != 0);   /* nothing under $HOME */
    /* 2: --data-dir, relative to the current directory */
    snprintf(cmd, sizeof cmd, "%s'%s/%s' artist --cd '%s' --data-dir mydata --headless --frames 20 --max-steps 3000000000",
             pre, abs, g_run, cdabs);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(strstr(out, "your data") && strstr(out, "/work/mydata"));
    CHECK_EQ(system("test -d " OUT "/work/mydata/MSKIDS/ARTIST"), 0);
    /* 3: --install-dir passes through and wins over the default */
    snprintf(cmd, sizeof cmd, "%s'%s/%s' writer --cd '%s' --install-dir inst2 --headless --frames 20 --max-steps 3000000000",
             pre, abs, g_run, cdabs);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("test -d " OUT "/work/inst2/MSKIDS"), 0);
    CHECK(!strstr(out, "/work/cwfa-data/work"));
}

/* Menu entries: --install-desktop writes cwfa-<program>.desktop and a PNG icon per program under XDG_DATA_HOME, the Exec
 * line starts the package with an absolute CD path and a data directory, and --remove-desktop deletes them again. */
static void desktop(const char *cd) {
    static char out[1 << 16];
    char cmd[8192], abs[1024], pre[4096];
    printf("test_package_desktop\n");
    if (!getcwd(abs, sizeof abs)) return;
    CHECK_EQ(system("rm -rf " OUT "/dt && mkdir -p " OUT "/dt"), 0);
    snprintf(pre, sizeof pre, "env XDG_CACHE_HOME='%s/" OUT "/cache' XDG_DATA_HOME='%s/" OUT "/dt' ", abs, abs);
    snprintf(cmd, sizeof cmd, "%s'%s/%s' --install-desktop --cd '%s'", pre, abs, g_run, cd);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(strstr(out, "installed: writer artist"));
    CHECK_EQ(system("test -f " OUT "/dt/applications/cwfa-writer.desktop && test -f " OUT "/dt/applications/cwfa-artist.desktop"), 0);
    CHECK_EQ(system("head -c 8 " OUT "/dt/icons/hicolor/32x32/apps/cwfa-writer.png | grep -q PNG"), 0);
    CHECK_EQ(system("grep -q '^StartupWMClass=cwfa-writer$' " OUT "/dt/applications/cwfa-writer.desktop"), 0);
    CHECK_EQ(system("grep -q '^Exec=\"/' " OUT "/dt/applications/cwfa-writer.desktop"), 0);       /* absolute launcher */
    CHECK_EQ(system("grep -q -- '--cd \"/' " OUT "/dt/applications/cwfa-artist.desktop"), 0);      /* absolute CD path */
    snprintf(cmd, sizeof cmd, "%s'%s/%s' --remove-desktop", pre, abs, g_run);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(system("test -e " OUT "/dt/applications/cwfa-writer.desktop") != 0);
    CHECK(system("test -e " OUT "/dt/icons/hicolor/32x32/apps/cwfa-writer.png") != 0);
}

/* Without a manual step: starting a program installs (or refreshes) its own menu entry and icons, quietly, only for that
 * program, only when something changed, and never when CWFA_NO_DESKTOP_INTEGRATION=1. A headless run has no window to
 * label, so it skips this unless CWFA_DESKTOP_INTEGRATION=1 asks for it (tests). */
static void integration(const char *cd) {
    static char out[1 << 16];
    char cmd[8192], abs[1024], pre[4096];
    const char *sizes[] = {"32", "64", "128", "256"};
    int i;
    printf("test_package_desktop_auto\n");
    if (!getcwd(abs, sizeof abs)) return;
    CHECK_EQ(system("rm -rf " OUT "/it && mkdir -p " OUT "/it"), 0);
    snprintf(pre, sizeof pre, "env XDG_CACHE_HOME='%s/" OUT "/cache' XDG_DATA_HOME='%s/" OUT "/it' CWFA_DESKTOP_INTEGRATION=1 ", abs, abs);
    snprintf(cmd, sizeof cmd, "%s'%s/%s' writer --cd '%s' --data-dir " OUT "/it/home --headless --frames 2 --max-steps 6000000000", pre, abs, g_run, cd);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("test -f " OUT "/it/applications/cwfa-writer.desktop"), 0);
    CHECK(system("test -e " OUT "/it/applications/cwfa-artist.desktop") != 0);                     /* only the program started */
    for (i = 0; i < 4; i++) {
        char chk[512];
        snprintf(chk, sizeof chk, "test -f " OUT "/it/icons/hicolor/%sx%s/apps/cwfa-writer.png && file " OUT "/it/icons/hicolor/%sx%s/apps/cwfa-writer.png | grep -q '%s x %s'", sizes[i], sizes[i], sizes[i], sizes[i], sizes[i], sizes[i]);
        CHECK_EQ(system(chk), 0);
    }
    CHECK_EQ(system("grep -q '^StartupWMClass=cwfa-writer$' " OUT "/it/applications/cwfa-writer.desktop"), 0);
    CHECK_EQ(system("grep -q -- '--data-dir \"/.*it/home\"' " OUT "/it/applications/cwfa-writer.desktop"), 0);   /* the data it ran with */
    CHECK_EQ(system("if command -v desktop-file-validate >/dev/null; then desktop-file-validate " OUT "/it/applications/cwfa-writer.desktop; fi"), 0);
    CHECK(!strstr(out, "install-desktop") && !strstr(out, "installed:"));                          /* quiet */
    /* a second start rewrites nothing */
    CHECK_EQ(system("touch -d '2001-01-01' " OUT "/it/applications/cwfa-writer.desktop " OUT "/it/icons/hicolor/64x64/apps/cwfa-writer.png"), 0);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("test \"$(stat -c %Y " OUT "/it/applications/cwfa-writer.desktop)\" = \"$(date -d 2001-01-01 +%s)\""), 0);
    CHECK_EQ(system("test \"$(stat -c %Y " OUT "/it/icons/hicolor/64x64/apps/cwfa-writer.png)\" = \"$(date -d 2001-01-01 +%s)\""), 0);
    /* a moved data directory is refreshed */
    snprintf(cmd, sizeof cmd, "%s'%s/%s' writer --cd '%s' --data-dir " OUT "/it/home2 --headless --frames 2 --max-steps 6000000000", pre, abs, g_run, cd);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("grep -q -- 'it/home2\"' " OUT "/it/applications/cwfa-writer.desktop"), 0);
    /* the opt-out */
    CHECK_EQ(system("rm -rf " OUT "/it2 && mkdir -p " OUT "/it2"), 0);
    snprintf(pre, sizeof pre, "env XDG_CACHE_HOME='%s/" OUT "/cache' XDG_DATA_HOME='%s/" OUT "/it2' CWFA_DESKTOP_INTEGRATION=1 CWFA_NO_DESKTOP_INTEGRATION=1 ", abs, abs);
    snprintf(cmd, sizeof cmd, "%s'%s/%s' writer --cd '%s' --data-dir " OUT "/it2/home --headless --frames 2 --max-steps 6000000000", pre, abs, g_run, cd);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(system("test -e " OUT "/it2/applications") != 0);
    /* scripts/play.sh does the same from a checkout (it needs reference/WRITER.EXE and reference/cd) */
    if (access("reference/WRITER.EXE", R_OK) == 0 && access("reference/cd", R_OK) == 0) {
        CHECK_EQ(system("rm -rf " OUT "/it3 && mkdir -p " OUT "/it3"), 0);
        snprintf(pre, sizeof pre, "env XDG_DATA_HOME='%s/" OUT "/it3' CWFA_DESKTOP_INTEGRATION=1 ", abs);
        snprintf(cmd, sizeof cmd, "%sscripts/play.sh writer --data-dir " OUT "/it3/pdata --headless --frames 2 --max-steps 6000000000", pre);
        CHECK_EQ(run(cmd, out, sizeof out), 0);
        CHECK_EQ(system("test -f " OUT "/it3/applications/cwfa-writer.desktop && test -s " OUT "/it3/icons/hicolor/256x256/apps/cwfa-writer.png"), 0);
        CHECK_EQ(system("grep -q -- 'play.sh\" writer --data-dir \".*it3/pdata\"' " OUT "/it3/applications/cwfa-writer.desktop"), 0);
        CHECK(!strstr(out, "install-desktop"));
    }
    /* a headless run alone does nothing */
    snprintf(pre, sizeof pre, "env XDG_CACHE_HOME='%s/" OUT "/cache' XDG_DATA_HOME='%s/" OUT "/it2' ", abs, abs);
    snprintf(cmd, sizeof cmd, "%s'%s/%s' writer --cd '%s' --data-dir " OUT "/it2/home --headless --frames 2 --max-steps 6000000000", pre, abs, g_run, cd);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(system("test -e " OUT "/it2/applications") != 0);
}

/* Installing for the user: --install puts the runtime, the launcher link, the menu entries and the icons under the user's
 * home and remembers the CD; the installed launcher starts a program without --cd, keeping drive C: in the user's data
 * directory; --uninstall removes what was written and keeps the documents. */
static void installer(const char *cd) {
    static char out[1 << 16];
    char cmd[12000], abs[1024], pre[5200];
    printf("test_package_install\n");
    if (!getcwd(abs, sizeof abs)) return;
    CHECK_EQ(system("rm -rf " OUT "/ins && mkdir -p " OUT "/ins/home " OUT "/ins/data " OUT "/ins/config"), 0);
    snprintf(pre, sizeof pre, "env XDG_CACHE_HOME='%s/" OUT "/cache' XDG_DATA_HOME='%s/" OUT "/ins/data' XDG_CONFIG_HOME='%s/" OUT "/ins/config' "
             "HOME='%s/" OUT "/ins/home' CWFA_NO_DIALOG=1 ", abs, abs, abs, abs);
    snprintf(cmd, sizeof cmd, "%s'%s/%s' --install --cd '%s'", pre, abs, g_run, cd);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("test -x " OUT "/ins/data/cwfa/app/bin/cwfa && test -d " OUT "/ins/data/cwfa/app/lib && test -L " OUT "/ins/home/.local/bin/cwfa-launch"), 0);
    CHECK_EQ(system("test -f " OUT "/ins/data/applications/cwfa-writer.desktop && test -f " OUT "/ins/data/applications/cwfa-artist.desktop"), 0);
    CHECK(system("test -e " OUT "/ins/data/applications/cwfa-es-writer.desktop") != 0);              /* no Spanish media given */
    CHECK_EQ(system("grep -q -- ' writer --user-data$' " OUT "/ins/data/applications/cwfa-writer.desktop"), 0);
    CHECK_EQ(system("head -c 8 " OUT "/ins/data/icons/hicolor/256x256/apps/cwfa-artist.png | grep -q PNG"), 0);   /* the program's own pixels */
    CHECK_EQ(system("grep -q '^cd=' " OUT "/ins/config/cwfa/media.conf"), 0);
    /* the installed launcher: no --cd, the data in the user's directory */
    snprintf(cmd, sizeof cmd, "%s" OUT "/ins/home/.local/bin/cwfa-launch writer --user-data --headless --frames 20 --max-steps 3000000000", pre);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(strstr(out, "ins/data/cwfa/data"));
    CHECK_EQ(system("test -d " OUT "/ins/data/cwfa/data/MSKIDS/USERS && test -f " OUT "/ins/data/cwfa/data/WINDOWS/WIN.INI"), 0);
    /* uninstall keeps the documents and removes the rest */
    snprintf(cmd, sizeof cmd, "%s" OUT "/ins/home/.local/bin/cwfa-launch --uninstall", pre);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(system("test -e " OUT "/ins/data/cwfa/app") != 0);
    CHECK(system("ls " OUT "/ins/data/applications/cwfa-* >/dev/null 2>&1") != 0);
    CHECK(system("ls " OUT "/ins/data/icons/hicolor/*/apps/cwfa-* >/dev/null 2>&1") != 0);
    CHECK(system("test -e " OUT "/ins/home/.local/bin/cwfa-launch") != 0);
    CHECK_EQ(system("test -f " OUT "/ins/data/cwfa/data/WINDOWS/WIN.INI"), 0);
}

int main(void) {
    const char *cd = getenv("CWFA_CD_ROOT"), *iso = getenv("CWFA_ISO");
    test_build();
    test_usage();
    if (cd && *cd) {
        smoke("writer_dir", "writer", cd);
        smoke("artist_dir", "artist", cd);
        data_dir(cd);
        desktop(cd);
        integration(cd);
        installer(cd);
    } else {
        printf("test_package_smoke\n  SKIP (CWFA_CD_ROOT not set)\n");
    }
    if (iso && *iso) smoke("writer_iso", "writer", iso);
    else printf("test_package_smoke_iso\n  SKIP (CWFA_ISO not set)\n");
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
