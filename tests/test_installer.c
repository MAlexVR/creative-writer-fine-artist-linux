#define _XOPEN_SOURCE 700
/* The user-level installer (scripts/install.sh, shipped as cwfa-install) and the launcher it installs (scripts/launch.sh),
 * run against a fake HOME with a stub `cwfa` binary, so no game media and no display are needed. They check that
 * install puts both programs in the application menu with their icons and a launcher that finds the media by itself,
 * that a second install changes nothing, that uninstall removes exactly what install wrote and keeps the user's
 * documents unless --purge --yes is given, and that the English launchers use the plain (disc-free) artwork of the
 * Spanish editions when those are available. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fixture.h"

#define OUT "build/insttest"

static int run(const char *cmd, char *out, size_t cap) {
    char full[8192];
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

static char g_abs[400];
static char g_env[2400];

/* the payload of the portable package, with a stub instead of the real binary */
static void make_payload(void) {
    char cmd[4096];
    CHECK_EQ(system("rm -rf " OUT " && mkdir -p " OUT "/payload/bin " OUT "/payload/fonts " OUT "/cd/SETUP " OUT "/escw " OUT "/esfa/DISCO1"), 0);
    CHECK_EQ(system("cp scripts/launch.sh " OUT "/payload/cwfa-launch && cp scripts/install-desktop.sh " OUT "/payload/cwfa-install-desktop "
                    "&& cp scripts/install.sh " OUT "/payload/cwfa-install && chmod +x " OUT "/payload/cwfa-*"), 0);
    {
        static const char stub[] = "#!/bin/sh\n"
                                   "echo \"$*\" >> \"$STUB_LOG\"\n"
                                   "out=\n"
                                   "while [ $# -gt 0 ]; do case $1 in --export-icon) out=$2; shift 2;; *) shift;; esac; done\n"
                                   "[ -z \"$out\" ] || printf 'PNG-stub\\n' > \"$out\"\n"
                                   "exit 0\n";
        FILE *f = fopen(OUT "/payload/bin/cwfa", "w");
        CHECK(f != NULL);
        if (f) {
            fputs(stub, f);
            fclose(f);
        }
        CHECK_EQ(chmod(OUT "/payload/bin/cwfa", 0755), 0);
    }
    CHECK_EQ(system("echo font > " OUT "/payload/fonts/LiberationSans-Regular.ttf"), 0);
    CHECK_EQ(system("printf 'MZ' > " OUT "/cd/SETUP/WRITER.EX_ && printf 'MZ' > " OUT "/cd/SETUP/ARTIST.EX_"), 0);
    CHECK_EQ(system("touch " OUT "/escw/INSTALAR.LST " OUT "/esfa/DISCO1/INSTALAR.LST"), 0);
    snprintf(cmd, sizeof cmd, "mkdir -p " OUT "/home " OUT "/data " OUT "/config " OUT "/cache");
    CHECK_EQ(system(cmd), 0);
    snprintf(g_env, sizeof g_env,
             "env -u CWFA_HOME -u CWFA_CD HOME='%s/" OUT "/home' XDG_DATA_HOME='%s/" OUT "/data' XDG_CONFIG_HOME='%s/" OUT "/config' "
             "XDG_CACHE_HOME='%s/" OUT "/cache' STUB_LOG='%s/" OUT "/stub.log' CWFA_NO_DIALOG=1 ",
             g_abs, g_abs, g_abs, g_abs, g_abs);
}

static void test_install(void) {
    static char out[1 << 16];
    char cmd[8192];
    printf("test_installer_install\n");
    snprintf(cmd, sizeof cmd, "%s" OUT "/payload/cwfa-install --cd '%s/" OUT "/cd' --es-cd '%s/" OUT "/escw' --es-fa-cd '%s/" OUT "/esfa'",
             g_env, g_abs, g_abs, g_abs);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(strstr(out, "uninstall"));                                             /* says how to undo it */
    /* the program files live under the user's data directory, not in the cache of the .run */
    CHECK_EQ(system("test -x " OUT "/data/cwfa/app/bin/cwfa && test -x " OUT "/data/cwfa/app/cwfa-launch && test -f " OUT "/data/cwfa/app/fonts/LiberationSans-Regular.ttf"), 0);
    CHECK_EQ(system("test \"$(readlink " OUT "/home/.local/bin/cwfa-launch)\" = \"$(cd " OUT "/data/cwfa/app && pwd)/cwfa-launch\""), 0);
    /* menu entries: both English programs, and the Spanish ones because Spanish media were given */
    CHECK_EQ(system("test -f " OUT "/data/applications/cwfa-writer.desktop && test -f " OUT "/data/applications/cwfa-artist.desktop "
                    "&& test -f " OUT "/data/applications/cwfa-es-writer.desktop && test -f " OUT "/data/applications/cwfa-es-artist.desktop"), 0);
    CHECK_EQ(system("grep -q '^Name=Creative Writer$' " OUT "/data/applications/cwfa-writer.desktop && grep -q '^Name=Fine Artist$' " OUT "/data/applications/cwfa-artist.desktop"), 0);
    CHECK_EQ(system("grep -q '^Exec=\".*/cwfa-launch\" writer --user-data$' " OUT "/data/applications/cwfa-writer.desktop"), 0);   /* no media path: it is remembered */
    CHECK_EQ(system("grep -q '^Exec=\".*/cwfa-launch\" es-artist --user-data$' " OUT "/data/applications/cwfa-es-artist.desktop"), 0);
    CHECK_EQ(system("grep -q '^StartupWMClass=cwfa-artist$' " OUT "/data/applications/cwfa-artist.desktop && grep -q '^Icon=cwfa-artist$' " OUT "/data/applications/cwfa-artist.desktop"), 0);
    CHECK_EQ(system("if command -v desktop-file-validate >/dev/null; then for f in " OUT "/data/applications/cwfa-*.desktop; do desktop-file-validate $f || exit 1; done; fi"), 0);
    {
        const char *ids[] = {"writer", "artist", "es-writer", "es-artist"}, *sz[] = {"32", "64", "128", "256"};
        int i, j;
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++) {
                char chk[512];
                snprintf(chk, sizeof chk, "head -c 4 " OUT "/data/icons/hicolor/%sx%s/apps/cwfa-%s.png | grep -q PNG", sz[j], sz[j], ids[i]);
                CHECK_EQ(system(chk), 0);
            }
    }
    /* the remembered media */
    CHECK_EQ(system("grep -q '^cd=.*insttest/cd$' " OUT "/config/cwfa/media.conf && grep -q '^es_cw=.*insttest/escw$' " OUT "/config/cwfa/media.conf "
                    "&& grep -q '^es_fa=.*insttest/esfa$' " OUT "/config/cwfa/media.conf"), 0);
    /* plain artwork: the English launchers take their icon from the Spanish editions (no disc drawn on it) */
    CHECK_EQ(system("grep -q -- '--es-writer .*insttest/escw.* --export-icon' " OUT "/stub.log"), 0);
    CHECK_EQ(system("grep -q -- '--es-artist .*insttest/esfa.* --export-icon' " OUT "/stub.log"), 0);
}

static void test_idempotent(void) {
    static char out[1 << 16];
    char cmd[8192];
    printf("test_installer_idempotent\n");
    CHECK_EQ(system("touch -d 2001-01-01 " OUT "/data/applications/cwfa-writer.desktop " OUT "/data/icons/hicolor/64x64/apps/cwfa-writer.png"), 0);
    snprintf(cmd, sizeof cmd, "%s" OUT "/payload/cwfa-install --cd '%s/" OUT "/cd' --es-cd '%s/" OUT "/escw' --es-fa-cd '%s/" OUT "/esfa'",
             g_env, g_abs, g_abs, g_abs);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("test \"$(stat -c %Y " OUT "/data/applications/cwfa-writer.desktop)\" = \"$(date -d 2001-01-01 +%s)\""), 0);
    CHECK_EQ(system("test \"$(stat -c %Y " OUT "/data/icons/hicolor/64x64/apps/cwfa-writer.png)\" = \"$(date -d 2001-01-01 +%s)\""), 0);
    CHECK_EQ(system("test \"$(grep -c . " OUT "/data/cwfa/install.manifest)\" = \"$(sort -u " OUT "/data/cwfa/install.manifest | grep -c .)\""), 0);   /* no duplicate entries */
}

static void test_original_icons(void) {
    static char out[1 << 16];
    char cmd[8192];
    printf("test_installer_icons_original\n");
    CHECK_EQ(system("rm -rf " OUT "/data2 " OUT "/home2 && mkdir -p " OUT "/home2 && : > " OUT "/stub.log"), 0);
    snprintf(cmd, sizeof cmd, "%s XDG_DATA_HOME='%s/" OUT "/data2' XDG_CONFIG_HOME='%s/" OUT "/config2' HOME='%s/" OUT "/home2' " OUT "/payload/cwfa-install --icons original --cd '%s/" OUT "/cd' --es-cd '%s/" OUT "/escw'",
             g_env, g_abs, g_abs, g_abs, g_abs, g_abs);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("grep -q -- '--exe .*WRITER.EX_ .*--export-icon' " OUT "/stub.log"), 0);       /* the program's own icon */
    CHECK_EQ(system("test -f " OUT "/data2/applications/cwfa-writer.desktop"), 0);
    /* no Spanish media: no Spanish entries, and the English ones still use their own icon */
    CHECK_EQ(system("rm -rf " OUT "/data3 " OUT "/home3 && mkdir -p " OUT "/home3 && : > " OUT "/stub.log"), 0);
    snprintf(cmd, sizeof cmd, "%s XDG_DATA_HOME='%s/" OUT "/data3' XDG_CONFIG_HOME='%s/" OUT "/config3' HOME='%s/" OUT "/home3' " OUT "/payload/cwfa-install --cd '%s/" OUT "/cd'", g_env, g_abs, g_abs, g_abs, g_abs);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(system("test -e " OUT "/data3/applications/cwfa-es-writer.desktop") != 0);
    CHECK_EQ(system("grep -q -- '--exe .*WRITER.EX_ .*--export-icon' " OUT "/stub.log"), 0);
    CHECK(system("grep -q -- '--es-writer' " OUT "/stub.log") != 0);
    /* without any media: the two English entries exist, their icons come with the first start */
    CHECK_EQ(system("rm -rf " OUT "/data4 " OUT "/home4 && mkdir -p " OUT "/home4"), 0);
    snprintf(cmd, sizeof cmd, "%s XDG_DATA_HOME='%s/" OUT "/data4' HOME='%s/" OUT "/home4' XDG_CONFIG_HOME='%s/" OUT "/config4' " OUT "/payload/cwfa-install", g_env, g_abs, g_abs, g_abs);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("test -f " OUT "/data4/applications/cwfa-writer.desktop && test -f " OUT "/data4/applications/cwfa-artist.desktop"), 0);
    CHECK(system("test -e " OUT "/data4/applications/cwfa-es-writer.desktop") != 0);
}

/* a Spanish-only configuration: the Spanish programs are listed as plain "Creative Writer" / "Fine Artist", no English
 * entry is made and the English entries a package ships system-wide are hidden for this user (an override with
 * Hidden=true) only while no English disc is configured */
static void test_spanish_only(void) {
    static char out[1 << 16];
    char cmd[8192], env[7000];
    printf("test_installer_spanish_only\n");
    CHECK_EQ(system("rm -rf " OUT "/data5 " OUT "/home5 " OUT "/config5 " OUT "/sys5 && mkdir -p " OUT "/home5 " OUT "/sys5/applications "
                    OUT "/data5/icons/hicolor/64x64/apps"), 0);
    CHECK_EQ(system("printf '[Desktop Entry]\\nType=Application\\nName=Creative Writer\\nExec=cwfa-launch writer\\n' > " OUT "/sys5/applications/cwfa-writer.desktop && "
                    "printf '[Desktop Entry]\\nType=Application\\nName=Fine Artist\\nExec=cwfa-launch artist\\n' > " OUT "/sys5/applications/cwfa-artist.desktop && "
                    "echo old > " OUT "/data5/icons/hicolor/64x64/apps/cwfa-writer.png && echo old > " OUT "/data5/icons/hicolor/64x64/apps/cwfa-artist.png"), 0);
    snprintf(env, sizeof env, "%s XDG_DATA_HOME='%s/" OUT "/data5' XDG_CONFIG_HOME='%s/" OUT "/config5' HOME='%s/" OUT "/home5' XDG_DATA_DIRS='%s/" OUT "/sys5'",
             g_env, g_abs, g_abs, g_abs, g_abs);
    snprintf(cmd, sizeof cmd, "%s " OUT "/payload/cwfa-launch --configure --es-cd '%s/" OUT "/escw' --es-fa-cd '%s/" OUT "/esfa'", env, g_abs, g_abs);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("grep -qx 'Name=Creative Writer' " OUT "/data5/applications/cwfa-es-writer.desktop"), 0);
    CHECK_EQ(system("grep -qx 'Name=Fine Artist' " OUT "/data5/applications/cwfa-es-artist.desktop"), 0);
    CHECK(system("grep -q 'Espa' " OUT "/data5/applications/cwfa-es-writer.desktop " OUT "/data5/applications/cwfa-es-artist.desktop") != 0);
    CHECK_EQ(system("grep -qx 'StartupWMClass=cwfa-es-writer' " OUT "/data5/applications/cwfa-es-writer.desktop"), 0);
    CHECK_EQ(system("grep -qx 'Hidden=true' " OUT "/data5/applications/cwfa-writer.desktop"), 0);          /* the package's English entries are hidden */
    CHECK_EQ(system("grep -qx 'Hidden=true' " OUT "/data5/applications/cwfa-artist.desktop"), 0);
    CHECK(system("ls " OUT "/data5/icons/hicolor/*/apps/cwfa-writer.png " OUT "/data5/icons/hicolor/*/apps/cwfa-artist.png >/dev/null 2>&1") != 0);   /* stale English icons gone */
    CHECK_EQ(system("test -s " OUT "/data5/icons/hicolor/64x64/apps/cwfa-es-writer.png"), 0);
    /* a second run changes nothing */
    CHECK_EQ(system("touch -d 2001-01-01 " OUT "/data5/applications/cwfa-writer.desktop " OUT "/data5/applications/cwfa-es-writer.desktop"), 0);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("test \"$(stat -c %Y " OUT "/data5/applications/cwfa-writer.desktop)\" = \"$(date -d 2001-01-01 +%s)\""), 0);
    CHECK_EQ(system("test \"$(stat -c %Y " OUT "/data5/applications/cwfa-es-writer.desktop)\" = \"$(date -d 2001-01-01 +%s)\""), 0);
    /* the English disc comes back: the Spanish ones get their suffix again and nothing stays hidden */
    snprintf(cmd, sizeof cmd, "%s " OUT "/payload/cwfa-launch --configure --cd '%s/" OUT "/cd'", env, g_abs);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("grep -qx 'Name=Creative Writer (Español)' " OUT "/data5/applications/cwfa-es-writer.desktop"), 0);
    CHECK_EQ(system("grep -qx 'Name=Fine Artist (Español)' " OUT "/data5/applications/cwfa-es-artist.desktop"), 0);
    CHECK(system("grep -q 'Hidden=true' " OUT "/data5/applications/cwfa-writer.desktop " OUT "/data5/applications/cwfa-artist.desktop") != 0);
    /* Spanish-only again, then --remove takes the overrides away too */
    CHECK_EQ(system("grep -v '^cd=' " OUT "/config5/cwfa/media.conf > " OUT "/config5/cwfa/m.new && mv " OUT "/config5/cwfa/m.new " OUT "/config5/cwfa/media.conf"), 0);
    snprintf(cmd, sizeof cmd, "%s " OUT "/payload/cwfa-launch --configure", env);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("grep -qx 'Hidden=true' " OUT "/data5/applications/cwfa-writer.desktop"), 0);
    snprintf(cmd, sizeof cmd, "%s " OUT "/payload/cwfa-install-desktop --remove --quiet", env);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(system("ls " OUT "/data5/applications/cwfa-* >/dev/null 2>&1") != 0);
    /* no package entry on the system: nothing to hide, so no override file is written */
    CHECK_EQ(system("rm -rf " OUT "/data6 " OUT "/config6 " OUT "/sys6 && mkdir -p " OUT "/sys6/applications " OUT "/data6/applications"), 0);
    /* an override left from the time the English package was installed: with the package gone it is stale and is removed */
    CHECK_EQ(system("printf '[Desktop Entry]\\nType=Application\\nName=Creative Writer\\nHidden=true\\n' > " OUT "/data6/applications/cwfa-writer.desktop && "
                    "printf '[Desktop Entry]\\nType=Application\\nName=Fine Artist\\nHidden=true\\n' > " OUT "/data6/applications/cwfa-artist.desktop"), 0);
    snprintf(env, sizeof env, "%s XDG_DATA_HOME='%s/" OUT "/data6' XDG_CONFIG_HOME='%s/" OUT "/config6' HOME='%s/" OUT "/home5' XDG_DATA_DIRS='%s/" OUT "/sys6'",
             g_env, g_abs, g_abs, g_abs, g_abs);
    snprintf(cmd, sizeof cmd, "%s " OUT "/payload/cwfa-launch --configure --es-cd '%s/" OUT "/escw'", env, g_abs);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("grep -qx 'Name=Creative Writer' " OUT "/data6/applications/cwfa-es-writer.desktop"), 0);
    CHECK(system("test -e " OUT "/data6/applications/cwfa-writer.desktop") != 0);
}

/* the launcher the entries start: media from media.conf, data in the user's data directory, a clear message without media */
static void test_launcher(void) {
    static char out[1 << 16];
    char cmd[8192];
    printf("test_installer_launcher\n");
    CHECK_EQ(system(": > " OUT "/stub.log"), 0);
    snprintf(cmd, sizeof cmd, "%s" OUT "/data/cwfa/app/cwfa-launch writer --user-data --headless", g_env);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("grep -q -- '--exe .*cd/SETUP/WRITER.EX_ --cd .*insttest/cd --install-dir .*insttest/data/cwfa/data ' " OUT "/stub.log"), 0);
    CHECK_EQ(system("test -d " OUT "/data/cwfa/data"), 0);
    CHECK(strstr(out, "insttest/data/cwfa/data"));                                /* it says where the data is */
    CHECK_EQ(system(": > " OUT "/stub.log"), 0);
    snprintf(cmd, sizeof cmd, "%s" OUT "/data/cwfa/app/cwfa-launch es-writer --user-data --headless", g_env);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("grep -q -- '--es-writer .*insttest/escw --install-dir .*insttest/data/cwfa/data-es ' " OUT "/stub.log"), 0);
    CHECK_EQ(system(": > " OUT "/stub.log"), 0);
    snprintf(cmd, sizeof cmd, "%s" OUT "/data/cwfa/app/cwfa-launch es-artist --user-data --headless", g_env);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("grep -q -- '--es-artist .*insttest/esfa --install-dir .*insttest/data/cwfa/data-es ' " OUT "/stub.log"), 0);
    /* an explicit --cd still wins and is remembered */
    CHECK_EQ(system("mkdir -p " OUT "/cd2/SETUP && cp " OUT "/cd/SETUP/* " OUT "/cd2/SETUP/"), 0);
    snprintf(cmd, sizeof cmd, "%s" OUT "/data/cwfa/app/cwfa-launch writer --user-data --cd '%s/" OUT "/cd2' --headless", g_env, g_abs);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("grep -q '^cd=.*insttest/cd2$' " OUT "/config/cwfa/media.conf"), 0);
    /* nothing remembered, no dialog and no terminal: a message that says what to do, exit 2 */
    snprintf(cmd, sizeof cmd, "%s XDG_CONFIG_HOME='%s/" OUT "/empty-config' " OUT "/data/cwfa/app/cwfa-launch artist --user-data --headless < /dev/null", g_env, g_abs);
    CHECK_EQ(run(cmd, out, sizeof out), 2);
    CHECK(strstr(out, "--configure") != NULL);
    /* --configure from a script: media given as options are stored */
    snprintf(cmd, sizeof cmd, "%s XDG_CONFIG_HOME='%s/" OUT "/cfg2' " OUT "/data/cwfa/app/cwfa-launch --configure --cd '%s/" OUT "/cd'", g_env, g_abs, g_abs);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("grep -q '^cd=.*insttest/cd$' " OUT "/cfg2/cwfa/media.conf"), 0);
}

static void snapshot(const char *file) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "(cd " OUT " && find home data config cache -mindepth 1 | sort) > %s", file);
    CHECK_EQ(system(cmd), 0);
}

static void test_uninstall(void) {
    static char out[1 << 16];
    char cmd[8192];
    printf("test_installer_uninstall\n");
    /* a pristine HOME, install, add the user's documents, uninstall */
    CHECK_EQ(system("rm -rf " OUT "/home " OUT "/data " OUT "/config " OUT "/cache && mkdir -p " OUT "/home " OUT "/data " OUT "/config " OUT "/cache"), 0);
    CHECK_EQ(system("mkdir -p " OUT "/data/applications " OUT "/data/icons/hicolor/64x64/apps && echo keep > " OUT "/data/applications/other.desktop && echo x > " OUT "/data/icons/hicolor/64x64/apps/other.png"), 0);
    snapshot(OUT "/before.txt");
    snprintf(cmd, sizeof cmd, "%s" OUT "/payload/cwfa-install --cd '%s/" OUT "/cd' --es-cd '%s/" OUT "/escw'", g_env, g_abs, g_abs);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(system("mkdir -p " OUT "/data/cwfa/data/MSKIDS/USERS/ANA && echo story > " OUT "/data/cwfa/data/MSKIDS/USERS/ANA/KIDS0001.MAX"), 0);
    snprintf(cmd, sizeof cmd, "%s" OUT "/data/cwfa/app/cwfa-install --uninstall", g_env);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(strstr(out, "kept") != NULL && strstr(out, "cwfa/data") != NULL);       /* says where the documents stayed */
    CHECK_EQ(system("test -f " OUT "/data/cwfa/data/MSKIDS/USERS/ANA/KIDS0001.MAX"), 0);
    CHECK(system("test -e " OUT "/data/cwfa/app") != 0);
    CHECK(system("test -e " OUT "/home/.local/bin/cwfa-launch") != 0);
    CHECK(system("ls " OUT "/data/applications/cwfa-* >/dev/null 2>&1") != 0);
    CHECK(system("ls " OUT "/data/icons/hicolor/*/apps/cwfa-* >/dev/null 2>&1") != 0);
    CHECK(system("test -e " OUT "/config/cwfa") != 0);
    CHECK_EQ(system("test -f " OUT "/data/applications/other.desktop && test -f " OUT "/data/icons/hicolor/64x64/apps/other.png"), 0);   /* nothing else touched */
    /* exactly what install wrote is gone: the tree equals the one before plus the documents */
    snapshot(OUT "/after.txt");
    CHECK_EQ(system("diff " OUT "/before.txt " OUT "/after.txt | grep '^<' | wc -l | grep -qx 0"), 0);        /* nothing that was there has vanished */
    CHECK_EQ(system("diff " OUT "/before.txt " OUT "/after.txt | grep '^>' | grep -v 'cwfa/data\\|cwfa$' | wc -l | grep -qx 0"), 0);   /* nothing is left but the documents */
    /* a second uninstall is harmless */
    snprintf(cmd, sizeof cmd, "%s" OUT "/payload/cwfa-install --uninstall", g_env);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    /* --purge asks: without a terminal and without --yes the documents stay */
    snprintf(cmd, sizeof cmd, "%s" OUT "/payload/cwfa-install --uninstall --purge < /dev/null", g_env);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(strstr(out, "--yes") != NULL);
    CHECK_EQ(system("test -f " OUT "/data/cwfa/data/MSKIDS/USERS/ANA/KIDS0001.MAX"), 0);
    snprintf(cmd, sizeof cmd, "%s" OUT "/payload/cwfa-install --uninstall --purge --yes", g_env);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(system("test -e " OUT "/data/cwfa") != 0);
    CHECK_EQ(system("test -f " OUT "/data/applications/other.desktop"), 0);
}

int main(void) {
    if (!getcwd(g_abs, sizeof g_abs)) return 2;
    make_payload();
    test_install();
    test_idempotent();
    test_original_icons();
    test_spanish_only();
    test_launcher();
    test_uninstall();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
