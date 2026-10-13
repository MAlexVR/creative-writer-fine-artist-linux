/* Staging from the Spanish KWAJ-compressed setup media. See install_es.h. */
#define _XOPEN_SOURCE 700
#include "install_es.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kwaj.h"

#define PATH_CAP 2400
#define MAX_DISKS 16
#define MAX_CREATED 4096

typedef struct {
    char dir[MAX_DISKS][PATH_CAP];
    unsigned n;
} disks;

/* Names are compared ignoring ASCII case; a byte >= 0x80 matches any byte >= 0x80, because the script spells
 * "n with tilde" in the ANSI code page and the media in the OEM one. */
static bool name_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if ((x >= 0x80) != (y >= 0x80)) return false;
        if (x < 0x80 && tolower(x) != tolower(y)) return false;
    }
    return *a == *b;
}

static bool lookup_in(const char *dir, const char *name, char *out, size_t cap) {
    DIR *d = opendir(dir);
    struct dirent *e;
    bool found = false;
    if (!d) return false;
    while (!found && (e = readdir(d)))
        if (name_eq(e->d_name, name)) {
            snprintf(out, cap, "%s/%s", dir, e->d_name);
            found = true;
        }
    closedir(d);
    return found;
}

static bool is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_disk_name(const char *n) {
    const char *p = n;
    if (!strncasecmp(n, "disco", 5)) p = n + 5;
    else if (!strncasecmp(n, "d", 1)) p = n + 1;
    else return false;
    if (!*p) return false;
    for (; *p; p++)
        if (!isdigit((unsigned char)*p)) return false;
    return true;
}

static int cmp_str(const void *a, const void *b) {
    return strcasecmp((const char *)a, (const char *)b);
}

static bool find_disks(const char *src, disks *ds) {
    char probe[PATH_CAP];
    DIR *d;
    struct dirent *e;
    char names[MAX_DISKS][256];
    unsigned n = 0, i;
    ds->n = 0;
    if (!is_directory(src)) return false;
    if (lookup_in(src, "INSTALAR.LST", probe, sizeof probe)) {
        snprintf(ds->dir[0], PATH_CAP, "%s", src);
        ds->n = 1;
        return true;
    }
    d = opendir(src);
    if (!d) return false;
    while ((e = readdir(d)) && n < MAX_DISKS) {
        char sub[PATH_CAP];
        if (!is_disk_name(e->d_name)) continue;
        snprintf(sub, sizeof sub, "%.1500s/%.255s", src, e->d_name);
        if (is_directory(sub)) snprintf(names[n++], sizeof names[0], "%s", e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof names[0], cmp_str);
    for (i = 0; i < n; i++) snprintf(ds->dir[ds->n++], PATH_CAP, "%.1500s/%.255s", src, names[i]);
    /* a disk set must carry the installer's list on some disk */
    for (i = 0; i < ds->n; i++)
        if (lookup_in(ds->dir[i], "INSTALAR.LST", probe, sizeof probe)) return true;
    ds->n = 0;
    return false;
}

bool install_es_detect(const char *dir) {
    disks *ds = malloc(sizeof *ds);
    bool ok;
    if (!ds) return false;
    ok = find_disks(dir, ds);
    free(ds);
    return ok;
}

/* ---- files ---------------------------------------------------------------------------------------- */

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *b = NULL;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        b = malloc((size_t)n + 1);
        if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
            free(b);
            b = NULL;
        } else if (b) {
            *len = (size_t)n;
        }
    }
    fclose(f);
    return b;
}

/* The file's real contents: expanded when it is KWAJ/SZDD, as is otherwise. */
static uint8_t *load_expanded(const char *path, size_t *len, kwaj_err *err) {
    size_t n = 0, out_len = 0;
    uint8_t *raw = slurp(path, &n), *out = NULL;
    *err = KWAJ_OK;
    if (!raw) {
        *err = KWAJ_TRUNCATED;
        return NULL;
    }
    if (!kwaj_is_compressed(raw, n)) {
        *len = n;
        return raw;
    }
    *err = kwaj_expand(raw, n, &out, &out_len);
    free(raw);
    *len = out_len;
    return out;
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

/* ANSI (script) to the upper-case OEM (code page 850) spelling the DOS file system shows. */
static unsigned char to_oem_upper(unsigned char c) {
    static const struct { unsigned char ansi, oem; } map[] = {
        {0xC1, 0xB5}, {0xC9, 0x90}, {0xCD, 0xD6}, {0xD3, 0xE0}, {0xDA, 0xE9}, {0xD1, 0xA5}, {0xDC, 0x9A}, {0xC7, 0x80}};
    size_t i;
    if (c < 0x80) return (unsigned char)toupper(c);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) c = (unsigned char)(c - 0x20);   /* lower-case Latin-1 letters */
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].ansi == c) return map[i].oem;
    return c;
}

static void dest_name(char *s) {
    for (; *s; s++) *s = *s == '\\' ? '/' : (char)to_oem_upper((unsigned char)*s);
}

/* ---- script --------------------------------------------------------------------------------------- */

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

/* "writer.exe" -> "writer.ex_" (the last character of the name becomes an underscore) */
static void compressed_name(const char *name, char *out, size_t cap) {
    size_t n = strlen(name);
    snprintf(out, cap, "%s", name);
    if (n && n + 1 < cap) out[n - 1] = '_';
}

static bool find_source(const disks *ds, const char *name, char *out, size_t cap) {
    char comp[300];
    unsigned i;
    compressed_name(name, comp, sizeof comp);
    for (i = 0; i < ds->n; i++)
        if (lookup_in(ds->dir[i], comp, out, cap) || lookup_in(ds->dir[i], name, out, cap)) return true;
    return false;
}

typedef struct {
    char (*paths)[PATH_CAP];
    unsigned n;
} created_set;

static bool was_created(const created_set *c, const char *path) {
    unsigned i;
    for (i = 0; i < c->n; i++)
        if (!strcmp(c->paths[i], path)) return true;
    return false;
}

static bool write_file(const char *path, const uint8_t *data, size_t n, bool append, install_report *rep) {
    FILE *f = fopen(path, append ? "ab" : "wb");
    bool ok = f && (n == 0 || fwrite(data, 1, n, f) == n);
    if (f && fclose(f) != 0) ok = false;
    if (!ok) snprintf(rep->err, sizeof rep->err, "cannot write %.150s: %s", path, strerror(errno));
    return ok;
}

static bool install_row(const disks *ds, const char *c_root, char *f[], unsigned nf, created_set *made, install_report *rep) {
    char src[PATH_CAP], rel[PATH_CAP], dst[PATH_CAP], dir[PATH_CAP], *slash;
    const char *append = nf > 2 ? f[2] : "", *dest = nf > 11 ? f[11] : "";
    bool is_append = *append != 0;
    uint8_t *data;
    size_t len = 0;
    kwaj_err kerr;
    bool ok;

    if (!f[1][0] || (!*dest && !is_append)) return true;       /* removal rows and the like */
    snprintf(rel, sizeof rel, "MSKIDS/%s", is_append ? append : dest);
    dest_name(rel + 7);
    snprintf(dst, sizeof dst, "%.1000s/%.1300s", c_root, rel);
    if (is_append ? !was_created(made, dst) : access(dst, F_OK) == 0) {
        if (!is_append) rep->skipped++;
        return true;
    }
    if (!find_source(ds, f[1], src, sizeof src)) {
        rep->missing++;
        return true;
    }
    data = load_expanded(src, &len, &kerr);
    if (!data) {
        snprintf(rep->err, sizeof rep->err, "cannot expand %.120s: %s", src, kwaj_strerror(kerr));
        return false;
    }
    if (kerr != KWAJ_OK) {
        free(data);
        return false;
    }
    snprintf(dir, sizeof dir, "%.1500s", rel);
    if ((slash = strrchr(dir, '/'))) *slash = 0;
    ok = make_path(rep, c_root, dir) && write_file(dst, data, len, is_append, rep);
    free(data);
    if (!ok) return false;
    if (!is_append) {
        if (made->n < MAX_CREATED) snprintf(made->paths[made->n++], PATH_CAP, "%s", dst);
        rep->linked++;
    }
    return true;
}

bool install_stage_es(const char *src, const char *c_root, const char *exe_host, install_report *rep) {
    disks *ds = malloc(sizeof *ds);
    created_set made = {malloc(sizeof(char[PATH_CAP]) * MAX_CREATED), 0};
    char script[PATH_CAP], section[64] = "", line[1024];
    uint8_t *text = NULL;
    size_t tlen = 0, pos = 0;
    kwaj_err kerr;
    bool ok = false;
    memset(rep, 0, sizeof *rep);
    if (!ds || !made.paths) {
        snprintf(rep->err, sizeof rep->err, "out of memory");
        goto done;
    }
    if (!find_disks(src, ds)) {
        snprintf(rep->err, sizeof rep->err, "%.150s is not Spanish setup media (no INSTALAR.LST)", src);
        goto done;
    }
    {
        unsigned i;
        bool found = false;
        for (i = 0; i < ds->n && !found; i++) found = lookup_in(ds->dir[i], "_SETUP.IN_", script, sizeof script);
        if (!found) {
            snprintf(rep->err, sizeof rep->err, "no setup script (_SETUP.IN_) on the media");
            goto done;
        }
    }
    text = load_expanded(script, &tlen, &kerr);
    if (!text) {
        snprintf(rep->err, sizeof rep->err, "cannot read the setup script: %s", kwaj_strerror(kerr));
        goto done;
    }
    rep->had_inf = true;
    if (!make_dir(rep, c_root) || !make_path(rep, c_root, "WINDOWS/SYSTEM") || !make_path(rep, c_root, "MSKIDS/USERS") ||
        !make_path(rep, c_root, "MSKIDS/SHARED/USERS"))
        goto done;
    ok = true;
    while (ok && pos < tlen) {
        char *f[16], *p = line;
        size_t k = 0;
        unsigned nf;
        while (pos < tlen && text[pos] != '\n' && k + 1 < sizeof line) line[k++] = (char)text[pos++];
        while (pos < tlen && text[pos] != '\n') pos++;           /* over-long line: ignore the rest */
        pos++;
        line[k] = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '\n' || *p == '\r' || !*p) continue;
        if (*p == '[') {
            char *e = strchr(p, ']');
            snprintf(section, sizeof section, "%.*s", e ? (int)(e - p - 1) : 0, p + 1);
            continue;
        }
        if (strcasecmp(section, "Files") && strcasecmp(section, "Shared Fonts")) continue;
        nf = split_row(p, f, 16);
        if (nf < 2) continue;
        ok = install_row(ds, c_root, f, nf, &made, rep);
    }
    if (ok && exe_host) {
        /* the program is installed from the media; only link a host file when none was */
        char name[256], dst[PATH_CAP], abs[PATH_MAX];
        const char *b = strrchr(exe_host, '/');
        snprintf(name, sizeof name, "%s", b ? b + 1 : exe_host);
        for (b = name; *b; b++) name[b - name] = (char)toupper((unsigned char)*b);
        snprintf(dst, sizeof dst, "%.1000s/MSKIDS/%s", c_root, name);
        if (access(dst, F_OK) != 0 && realpath(exe_host, abs) && symlink(abs, dst) == 0) rep->linked++;
    }
    ok = ok && rep->err[0] == 0;
done:
    free(text);
    free(made.paths);
    free(ds);
    return ok;
}
