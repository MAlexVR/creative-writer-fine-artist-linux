/* Staging of an installed C: drive from the CD. See install.h. */
#define _XOPEN_SOURCE 700
#include "ini.h"
#include "install.h"
#include "install_es.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_CAP 2400

static void upper(char *s) {
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/* The entry of dir whose name equals `name` ignoring case (the CD is upper case, copies need not be). */
static bool ci_lookup(const char *dir, const char *name, char *out, size_t cap) {
    DIR *d = opendir(dir);
    struct dirent *e;
    bool found = false;
    if (!d) return false;
    while (!found && (e = readdir(d)))
        if (!strcasecmp(e->d_name, name)) {
            snprintf(out, cap, "%s/%s", dir, e->d_name);
            found = true;
        }
    closedir(d);
    return found;
}

static bool exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

static bool is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool make_dir(install_report *rep, const char *path) {
    if (is_directory(path)) return true;
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        snprintf(rep->err, sizeof rep->err, "cannot create %.150s: %s", path, strerror(errno));
        return false;
    }
    rep->dirs++;
    return true;
}

/* Creates every directory of the slash-separated, already upper-case path rel under root. */
static bool make_path(install_report *rep, const char *root, const char *rel) {
    char p[PATH_CAP];
    size_t n = (size_t)snprintf(p, sizeof p, "%s", root), i;
    if (!make_dir(rep, p)) return false;
    for (i = 0; rel[i];) {
        size_t k = strcspn(rel + i, "/");
        if (k) {
            n += (size_t)snprintf(p + n, sizeof p - n, "/%.*s", (int)k, rel + i);
            if (n >= sizeof p - 1 || !make_dir(rep, p)) return false;
        }
        i += k + (rel[i + k] ? 1 : 0);
    }
    return true;
}

static void link_file(install_report *rep, const char *target, const char *dst) {
    if (exists(dst)) {
        rep->skipped++;
        return;
    }
    if (symlink(target, dst) == 0) rep->linked++;
    else snprintf(rep->err, sizeof rep->err, "cannot link %.150s: %s", dst, strerror(errno));
}

/* Mirrors srcdir under dstdir: real directories, linked files. rel is the upper-case path below
 * C:\MSKIDS; the user directories are created but not filled. */
static bool mirror(install_report *rep, const char *srcdir, const char *dstdir, const char *rel) {
    DIR *d = opendir(srcdir);
    struct dirent *e;
    bool ok = true;
    if (!d) return true;
    while (ok && (e = readdir(d))) {
        char src[PATH_CAP], dst[PATH_CAP], name[256], nrel[PATH_CAP];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(name, sizeof name, "%s", e->d_name);
        upper(name);
        snprintf(src, sizeof src, "%s/%s", srcdir, e->d_name);
        snprintf(dst, sizeof dst, "%s/%s", dstdir, name);
        snprintf(nrel, sizeof nrel, "%s%s%s", rel, rel[0] ? "/" : "", name);
        if (is_directory(src)) {
            ok = make_dir(rep, dst);
            if (ok && strcmp(nrel, "SHARED/USERS")) ok = mirror(rep, src, dst, nrel);
        } else {
            link_file(rep, src, dst);
        }
    }
    closedir(d);
    return ok;
}

/* Splits a SETUP.INF row into trimmed fields; a leading "Name"= is dropped. Returns the field count. */
static unsigned split_row(char *line, char *f[], unsigned max) {
    unsigned n = 0;
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        char *q = strchr(p + 1, '"');
        if (q) {
            q++;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') p = q + 1;
        }
    }
    for (;;) {
        char *c = strchr(p, ','), *end;
        if (c) *c = 0;
        while (*p == ' ' || *p == '\t') p++;
        end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = 0;
        if (n < max) f[n++] = p;
        if (!c) break;
        p = c + 1;
    }
    return n;
}

static bool stage_inf(install_report *rep, const char *setup_dir, const char *inf_path, const char *c_root) {
    FILE *fp = fopen(inf_path, "r");
    char line[1024], section[64] = "";
    if (!fp) return true;
    rep->had_inf = true;
    while (fgets(line, sizeof line, fp)) {
        char *f[16], *p = line;
        const char *base;
        unsigned n;
        size_t len;
        char rel[1100], src[PATH_CAP], dst[PATH_CAP], dir[1100], *slash;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '\n' || *p == '\r' || !*p) continue;
        if (*p == '[') {
            char *e = strchr(p, ']');
            snprintf(section, sizeof section, "%.*s", e ? (int)(e - p - 1) : 0, p + 1);
            continue;
        }
        if (!strcasecmp(section, "Files")) base = "MSKIDS";
        else if (!strcasecmp(section, "WinSysStuff")) base = "WINDOWS/SYSTEM";
        else continue;
        n = split_row(p, f, 16);
        if (n < 12 || !f[1][0] || !f[11][0]) continue;       /* column 1 = source, column 11 = destination */
        len = strlen(f[1]);
        if (f[1][len - 1] == '_') continue;                   /* compressed: the EXEs */
        snprintf(rel, sizeof rel, "%s/%s", base, f[11]);
        for (p = rel; *p; p++) if (*p == '\\') *p = '/';
        upper(rel);
        if (!ci_lookup(setup_dir, f[1], src, sizeof src)) {
            rep->missing++;
            continue;
        }
        snprintf(dst, sizeof dst, "%.1000s/%s", c_root, rel);
        snprintf(dir, sizeof dir, "%s", rel);
        if ((slash = strrchr(dir, '/'))) *slash = 0;
        if (!make_path(rep, c_root, dir)) {
            fclose(fp);
            return false;
        }
        link_file(rep, src, dst);
    }
    fclose(fp);
    return true;
}

/* The virtual PDF printer, as Windows' printer setup would record it: [windows] device= (the default printer), the
 * [devices] and [PrinterPorts] lists. Whatever the user already chose is kept. */
static void register_printer(const char *c_root) {
    char win[PATH_CAP], v[160];
    snprintf(win, sizeof win, "%.1000s/WINDOWS/WIN.INI", c_root);
    if (!ini_lookup(win, "windows", "device", v, sizeof v)) ini_write(win, "windows", "device", "PDF Printer,CWFAPRN,FILE:");
    if (!ini_lookup(win, "devices", "PDF Printer", v, sizeof v)) ini_write(win, "devices", "PDF Printer", "CWFAPRN,FILE:");
    if (!ini_lookup(win, "PrinterPorts", "PDF Printer", v, sizeof v))
        ini_write(win, "PrinterPorts", "PDF Printer", "CWFAPRN,FILE:,15,45");
}

bool install_stage(const char *cd_root, const char *c_root, const char *exe_host, install_report *rep) {
    char cd[PATH_MAX], setup[PATH_CAP], inf[PATH_CAP], mskids[PATH_CAP], dst[PATH_CAP];
    if (install_es_detect(cd_root)) {                                                         /* Spanish KWAJ media */
        bool ok = install_stage_es(cd_root, c_root, exe_host, rep);
        if (ok) register_printer(c_root);
        return ok;
    }
    memset(rep, 0, sizeof *rep);
    if (!realpath(cd_root, cd) || !is_directory(cd)) {
        snprintf(rep->err, sizeof rep->err, "cannot use the CD directory %.150s", cd_root);
        return false;
    }
    if (!make_dir(rep, c_root) || !make_path(rep, c_root, "WINDOWS/SYSTEM") ||
        !make_path(rep, c_root, "MSKIDS/USERS") || !make_path(rep, c_root, "MSKIDS/SHARED/USERS"))
        return false;
    if (ci_lookup(cd, "MSKIDS", mskids, sizeof mskids)) {
        snprintf(dst, sizeof dst, "%.1000s/MSKIDS", c_root);
        if (!mirror(rep, mskids, dst, "")) return false;
    }
    if (ci_lookup(cd, "SETUP", setup, sizeof setup) && ci_lookup(setup, "SETUP.INF", inf, sizeof inf) &&
        !stage_inf(rep, setup, inf, c_root))
        return false;
    if (exe_host) {
        char name[256], abs[PATH_MAX];
        const char *b = strrchr(exe_host, '/');
        snprintf(name, sizeof name, "%s", b ? b + 1 : exe_host);
        upper(name);
        snprintf(dst, sizeof dst, "%.1000s/MSKIDS/%s", c_root, name);
        if (realpath(exe_host, abs)) link_file(rep, abs, dst);
    }
    register_printer(c_root);
    return rep->err[0] == 0;
}
