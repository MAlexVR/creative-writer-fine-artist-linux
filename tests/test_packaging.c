#define _XOPEN_SOURCE 700
/* The files that make the RPM show up in an application store and remove cleanly: the spec, the desktop entries, the AppStream
 * metadata and the icons shipped with the package (our own artwork, not the programs' icons, which come from the user's media
 * at the first start). Static checks run wherever the validators exist; CWFA_TEST_RPM=1 also builds the RPM from the committed
 * tree and inspects its contents without installing anything. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fixture.h"

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

static int have(const char *tool) {
    char cmd[256];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", tool);
    return system(cmd) == 0;
}

static void test_files(void) {
    static const char *files[] = {
        "packaging/rpm/cwfa.spec",
        "packaging/desktop/cwfa-writer.desktop",
        "packaging/desktop/cwfa-artist.desktop",
        "packaging/desktop/cwfa-es-writer.desktop",
        "packaging/desktop/cwfa-es-artist.desktop",
        "packaging/appstream/io.github.malexvr.cwfa.writer.metainfo.xml",
        "packaging/appstream/io.github.malexvr.cwfa.artist.metainfo.xml",
        "packaging/appstream/io.github.malexvr.cwfa.writer_es.metainfo.xml",
        "packaging/appstream/io.github.malexvr.cwfa.artist_es.metainfo.xml",
        "packaging/icons/cwfa-writer.svg",
        "packaging/icons/cwfa-artist.svg",
        "packaging/icons/cwfa-es-writer.svg",
        "packaging/icons/cwfa-es-artist.svg",
        "scripts/build-rpm.sh",
    };
    size_t i;
    printf("test_packaging_files\n");
    for (i = 0; i < sizeof files / sizeof *files; i++) {
        char chk[300];
        snprintf(chk, sizeof chk, "test -s %s", files[i]);
        if (system(chk) != 0) printf("  missing %s\n", files[i]);
        CHECK_EQ(system(chk), 0);
    }
}

static void test_desktop(void) {
    static char out[1 << 14];
    const char *ids[] = {"writer", "artist"};
    int i;
    printf("test_packaging_desktop\n");
    for (i = 0; i < 2; i++) {
        char cmd[512];
        /* starts the installed launcher with the shared data directory; the window carries the matching app id */
        snprintf(cmd, sizeof cmd, "grep -q '^Exec=cwfa-launch %s --user-data$' packaging/desktop/cwfa-%s.desktop", ids[i], ids[i]);
        CHECK_EQ(system(cmd), 0);
        snprintf(cmd, sizeof cmd, "grep -q '^StartupWMClass=cwfa-%s$' packaging/desktop/cwfa-%s.desktop && grep -q '^Icon=cwfa-%s$' packaging/desktop/cwfa-%s.desktop",
                 ids[i], ids[i], ids[i], ids[i]);
        CHECK_EQ(system(cmd), 0);
        if (have("desktop-file-validate")) {
            snprintf(cmd, sizeof cmd, "desktop-file-validate packaging/desktop/cwfa-%s.desktop", ids[i]);
            CHECK_EQ(run(cmd, out, sizeof out), 0);
        }
    }
}

/* the Spanish edition's launchers: the plain names, the Spanish programs, the window and icon names the launcher writes at first start */
static void test_desktop_spanish(void) {
    static char out[1 << 14];
    const char *ids[] = {"writer", "artist"}, *names[] = {"Creative Writer", "Fine Artist"};
    int i;
    printf("test_packaging_desktop_spanish\n");
    for (i = 0; i < 2; i++) {
        char cmd[640];
        snprintf(cmd, sizeof cmd, "grep -qx 'Name=%s' packaging/desktop/cwfa-es-%s.desktop", names[i], ids[i]);
        CHECK_EQ(system(cmd), 0);
        snprintf(cmd, sizeof cmd, "! grep -x 'Name=.*Espa.*' packaging/desktop/cwfa-es-%s.desktop", ids[i]);
        CHECK_EQ(system(cmd), 0);
        snprintf(cmd, sizeof cmd, "grep -qx 'Exec=cwfa-launch es-%s --user-data' packaging/desktop/cwfa-es-%s.desktop", ids[i], ids[i]);
        CHECK_EQ(system(cmd), 0);
        snprintf(cmd, sizeof cmd, "grep -qx 'StartupWMClass=cwfa-es-%s' packaging/desktop/cwfa-es-%s.desktop && grep -qx 'Icon=cwfa-es-%s' packaging/desktop/cwfa-es-%s.desktop",
                 ids[i], ids[i], ids[i], ids[i]);
        CHECK_EQ(system(cmd), 0);
        snprintf(cmd, sizeof cmd, "grep -q '^Comment\\[es\\]=' packaging/desktop/cwfa-es-%s.desktop", ids[i]);
        CHECK_EQ(system(cmd), 0);
        if (have("desktop-file-validate")) {
            snprintf(cmd, sizeof cmd, "desktop-file-validate packaging/desktop/cwfa-es-%s.desktop", ids[i]);
            CHECK_EQ(run(cmd, out, sizeof out), 0);
        }
    }
}

static void test_appstream(void) {
    static char out[1 << 14];
    const char *ids[] = {"writer", "artist"};
    int i;
    printf("test_packaging_appstream\n");
    for (i = 0; i < 2; i++) {
        char cmd[640];
        snprintf(cmd, sizeof cmd, "grep -q '<launchable type=\"desktop-id\">cwfa-%s.desktop</launchable>' packaging/appstream/io.github.malexvr.cwfa.%s.metainfo.xml", ids[i], ids[i]);
        CHECK_EQ(system(cmd), 0);
        snprintf(cmd, sizeof cmd, "grep -q '<project_license>MIT</project_license>' packaging/appstream/io.github.malexvr.cwfa.%s.metainfo.xml", ids[i]);
        CHECK_EQ(system(cmd), 0);
        if (have("appstreamcli")) {
            snprintf(cmd, sizeof cmd, "appstreamcli validate --no-net packaging/appstream/io.github.malexvr.cwfa.%s.metainfo.xml", ids[i]);
            CHECK_EQ(run(cmd, out, sizeof out), 0);
            if (strstr(out, "Validation failed")) printf("%s\n", out);
        }
    }
    for (i = 0; i < 2; i++) {                                   /* the Spanish editions: their own components, plain names, Spanish text */
        char cmd[700];
        snprintf(cmd, sizeof cmd, "grep -q '<launchable type=\"desktop-id\">cwfa-es-%s.desktop</launchable>' packaging/appstream/io.github.malexvr.cwfa.%s_es.metainfo.xml", ids[i], ids[i]);
        CHECK_EQ(system(cmd), 0);
        snprintf(cmd, sizeof cmd, "grep -q '<icon type=\"stock\">cwfa-es-%s</icon>' packaging/appstream/io.github.malexvr.cwfa.%s_es.metainfo.xml", ids[i], ids[i]);
        CHECK_EQ(system(cmd), 0);
        snprintf(cmd, sizeof cmd, "grep -q '<lang>es</lang>' packaging/appstream/io.github.malexvr.cwfa.%s_es.metainfo.xml && grep -qi 'Spanish' packaging/appstream/io.github.malexvr.cwfa.%s_es.metainfo.xml",
                 ids[i], ids[i]);
        CHECK_EQ(system(cmd), 0);
        snprintf(cmd, sizeof cmd, "grep -q '<project_license>MIT</project_license>' packaging/appstream/io.github.malexvr.cwfa.%s_es.metainfo.xml", ids[i]);
        CHECK_EQ(system(cmd), 0);
        if (have("appstreamcli")) {
            snprintf(cmd, sizeof cmd, "appstreamcli validate --no-net packaging/appstream/io.github.malexvr.cwfa.%s_es.metainfo.xml", ids[i]);
            CHECK_EQ(run(cmd, out, sizeof out), 0);
            if (strstr(out, "Validation failed")) printf("%s\n", out);
        }
    }
}

static void test_icons_spec(void) {
    static char out[1 << 14];
    printf("test_packaging_icons_spec\n");
    CHECK_EQ(system("python3 -c \"import xml.dom.minidom as m; [m.parse(f) for f in ('packaging/icons/cwfa-writer.svg','packaging/icons/cwfa-artist.svg')]\""), 0);
    CHECK_EQ(system("! grep -qi 'microsoft' packaging/icons/cwfa-writer.svg packaging/icons/cwfa-artist.svg"), 0);          /* our own art */
    CHECK_EQ(system("grep -q '^Name:[[:space:]]*cwfa$' packaging/rpm/cwfa.spec && grep -q '^License:[[:space:]]*MIT' packaging/rpm/cwfa.spec"), 0);
    CHECK_EQ(system("grep -q 'liberation-sans-fonts' packaging/rpm/cwfa.spec"), 0);                                           /* system fonts, none bundled */
    CHECK_EQ(system("grep -q '%{_bindir}/cwfa-launch' packaging/rpm/cwfa.spec && grep -q 'metainfo' packaging/rpm/cwfa.spec"), 0);
    CHECK_EQ(system("! grep -q '\\.ISO\\|\\.EXE\\|\\.EX_' packaging/rpm/cwfa.spec"), 0);                                       /* no game files */
    if (have("rpmspec")) {
        CHECK_EQ(run("rpmspec -q --qf '%{name}-%{version}\\n' packaging/rpm/cwfa.spec", out, sizeof out), 0);
        CHECK(strstr(out, "cwfa-") != NULL);
    }
    /* three packages: the runtime alone, and one package of launchers per edition; the runtime requires neither */
    CHECK_EQ(system("grep -q '^%package english' packaging/rpm/cwfa.spec && grep -q '^%package spanish' packaging/rpm/cwfa.spec"), 0);
    CHECK_EQ(system("grep -q '^%files english' packaging/rpm/cwfa.spec && grep -q '^%files spanish' packaging/rpm/cwfa.spec"), 0);
    CHECK_EQ(system("sed -n '/^%package english/,$p' packaging/rpm/cwfa.spec | grep -q '^Requires:[[:space:]]*cwfa = %{version}-%{release}'"), 0);
    CHECK_EQ(system("! sed -n '1,/^%package english/p' packaging/rpm/cwfa.spec | grep -q '^Requires:.*cwfa-\\(english\\|spanish\\)'"), 0);
    CHECK_EQ(system("grep -q '^Release:[[:space:]]*2%{?dist}' packaging/rpm/cwfa.spec"), 0);                           /* upgrades 0.1.0-1 */
    CHECK_EQ(system("sh -n scripts/build-rpm.sh"), 0);
}

/* the built package: contents, dependencies, validators on the extracted files; nothing is installed */
static void test_rpm(void) {
    static char out[1 << 16];
    printf("test_packaging_rpm\n");
    if (!getenv("CWFA_TEST_RPM") || !have("rpmbuild")) {
        printf("  skipped (CWFA_TEST_RPM=1 and rpmbuild needed)\n");
        return;
    }
    CHECK_EQ(run("sh scripts/build-rpm.sh >build/rpm-build.log 2>&1; tail -n 3 build/rpm-build.log", out, sizeof out), 0);
    CHECK_EQ(system("ls build/rpm/RPMS/*/cwfa-[0-9]*.rpm >/dev/null 2>&1"), 0);
    CHECK_EQ(run("rpm -qlp build/rpm/RPMS/*/cwfa-[0-9]*.rpm", out, sizeof out), 0);
    CHECK(strstr(out, "/usr/bin/cwfa-launch") != NULL);
    CHECK(strstr(out, "/usr/libexec/cwfa/bin/cwfa") != NULL);
    CHECK(!strstr(out, "/usr/share/applications/") && !strstr(out, "/usr/share/metainfo/") && !strstr(out, "/usr/share/icons/")); /* the runtime alone lists no launcher */
    CHECK(!strstr(out, ".EXE") && !strstr(out, ".ISO") && !strstr(out, ".ttf"));                  /* no game files, no bundled fonts */
    CHECK_EQ(run("rpm -qlp build/rpm/RPMS/*/cwfa-english-[0-9]*.rpm", out, sizeof out), 0);
    CHECK(strstr(out, "/usr/share/applications/cwfa-writer.desktop") != NULL);
    CHECK(strstr(out, "/usr/share/applications/cwfa-artist.desktop") != NULL);
    CHECK(strstr(out, "/usr/share/metainfo/io.github.malexvr.cwfa.writer.metainfo.xml") != NULL);
    CHECK(strstr(out, "/usr/share/icons/hicolor/scalable/apps/cwfa-writer.svg") != NULL);
    CHECK(!strstr(out, "cwfa-es-") && !strstr(out, "_es.metainfo"));
    CHECK_EQ(run("rpm -qlp build/rpm/RPMS/*/cwfa-spanish-[0-9]*.rpm", out, sizeof out), 0);
    CHECK(strstr(out, "/usr/share/applications/cwfa-es-writer.desktop") != NULL);
    CHECK(strstr(out, "/usr/share/applications/cwfa-es-artist.desktop") != NULL);
    CHECK(strstr(out, "/usr/share/metainfo/io.github.malexvr.cwfa.writer_es.metainfo.xml") != NULL);
    CHECK(strstr(out, "/usr/share/metainfo/io.github.malexvr.cwfa.artist_es.metainfo.xml") != NULL);
    CHECK(strstr(out, "/usr/share/icons/hicolor/scalable/apps/cwfa-es-writer.svg") != NULL);
    CHECK(!strstr(out, "applications/cwfa-writer.desktop") && !strstr(out, "applications/cwfa-artist.desktop"));
    CHECK_EQ(run("rpm -qp --requires build/rpm/RPMS/*/cwfa-spanish-[0-9]*.rpm build/rpm/RPMS/*/cwfa-english-[0-9]*.rpm", out, sizeof out), 0);
    CHECK(strstr(out, "cwfa = 0.1.0-2") != NULL);                                                  /* each edition needs the runtime */
    CHECK_EQ(run("rpm -qp --requires build/rpm/RPMS/*/cwfa-[0-9]*.rpm", out, sizeof out), 0);
    CHECK(!strstr(out, "cwfa-english") && !strstr(out, "cwfa-spanish"));                             /* the runtime needs neither */
    CHECK_EQ(run("rpm -qp --requires build/rpm/RPMS/*/cwfa-[0-9]*.rpm", out, sizeof out), 0);
    CHECK(strstr(out, "liberation-sans-fonts") != NULL);
    CHECK(strstr(out, "libSDL2") != NULL);
    /* extract into a scratch root and check what would be installed */
    CHECK_EQ(system("rm -rf build/rpmroot && mkdir -p build/rpmroot && cd build/rpmroot && for r in ../rpm/RPMS/*/cwfa-[0-9]*.rpm ../rpm/RPMS/*/cwfa-english-[0-9]*.rpm ../rpm/RPMS/*/cwfa-spanish-[0-9]*.rpm; do rpm2cpio $r | cpio -idm --quiet; done"), 0);
    if (have("desktop-file-validate"))
        CHECK_EQ(run("desktop-file-validate build/rpmroot/usr/share/applications/cwfa-*.desktop", out, sizeof out), 0);
    if (have("appstreamcli"))
        CHECK_EQ(run("appstreamcli validate --no-net build/rpmroot/usr/share/metainfo/*.metainfo.xml", out, sizeof out), 0);
    /* the launcher resolves its link into /usr/libexec/cwfa and works from that layout */
    CHECK_EQ(system("test \"$(readlink build/rpmroot/usr/bin/cwfa-launch)\" = ../libexec/cwfa/cwfa-launch"), 0);
    CHECK_EQ(run("build/rpmroot/usr/bin/cwfa-launch --help", out, sizeof out), 2);
    CHECK(strstr(out, "usage:") != NULL);
    CHECK_EQ(run("build/rpmroot/usr/bin/cwfa-launch --uninstall", out, sizeof out), 2);                  /* a package: the package manager removes it */
    CHECK(strstr(out, "package manager") != NULL);
}

int main(void) {
    test_files();
    test_desktop();
    test_desktop_spanish();
    test_appstream();
    test_icons_spec();
    test_rpm();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
