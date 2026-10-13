#define _POSIX_C_SOURCE 200809L
#include "dospath.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_COMPS 48
#define COMP_MAX 128

static int idx_of(char drive) {
    drive = (char)toupper((unsigned char)drive);
    return drive >= 'A' && drive <= 'Z' ? drive - 'A' : -1;
}

void dp_init(dospath_t *dp) {
    int i;
    memset(dp, 0, sizeof *dp);
    dp->cur_drive = 'C';
    for (i = 0; i < 26; i++) strcpy(dp->cur_dir[i], "\\");
}

void dp_free(dospath_t *dp) {
    int i;
    for (i = 0; i < 26; i++) free(dp->root[i]);
    memset(dp->root, 0, sizeof dp->root);
}

bool dp_map(dospath_t *dp, char drive, const char *host_dir) {
    int i = idx_of(drive);
    char *copy;
    if (i < 0 || !host_dir) return false;
    copy = strdup(host_dir);
    if (!copy) return false;
    free(dp->root[i]);
    dp->root[i] = copy;
    return true;
}

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    return *a == *b;
}

/* Splits a DOS path tail into components, applying "." and "..". */
static bool split(const char *s, char comps[][COMP_MAX], int *n) {
    while (*s) {
        char c[COMP_MAX];
        size_t len = 0;
        while (*s == '\\' || *s == '/') s++;
        if (!*s) break;
        while (*s && *s != '\\' && *s != '/') {
            if (len + 1 >= COMP_MAX || (unsigned char)*s < 0x20 || *s == ':' || *s == '*' || *s == '?')
                return false;
            c[len++] = *s++;
        }
        c[len] = 0;
        if (!strcmp(c, ".")) continue;
        if (!strcmp(c, "..")) {
            if (*n) (*n)--;
            continue;
        }
        if (*n == MAX_COMPS) return false;
        strcpy(comps[(*n)++], c);
    }
    return true;
}

/* Existing entry of dir whose name equals `name` ignoring case. */
static bool find_entry(const char *dir, const char *name, char *found, size_t cap) {
    char p[DP_PATH_MAX];
    struct stat st;
    struct dirent *e;
    DIR *d;
    bool ok = false;
    if (snprintf(p, sizeof p, "%s/%s", dir, name) < (int)sizeof p && stat(p, &st) == 0) {
        if (strlen(name) >= cap) return false;
        strcpy(found, name); /* exact spelling exists */
        return true;
    }
    if (!(d = opendir(dir))) return false;
    while ((e = readdir(d))) {
        if (ieq(e->d_name, name) && strlen(e->d_name) < cap) {
            strcpy(found, e->d_name);
            ok = true;
            break;
        }
    }
    closedir(d);
    return ok;
}

bool dp_resolve(const dospath_t *dp, const char *path, unsigned flags, dp_result *out) {
    char comps[MAX_COMPS][COMP_MAX];
    int n = 0, i, di;
    char drive;
    const char *p = path;
    char host[DP_PATH_MAX];
    size_t dlen;
    if (!dp || !path || !*path || !out) return false;
    memset(out, 0, sizeof *out);
    drive = dp->cur_drive;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        drive = (char)toupper((unsigned char)p[0]);
        p += 2;
    }
    di = idx_of(drive);
    if (di < 0 || !dp->root[di]) return false;
    if (*p != '\\' && *p != '/' && !split(dp->cur_dir[di], comps, &n)) return false;
    if (!split(p, comps, &n)) return false;
    if (snprintf(host, sizeof host, "%s", dp->root[di]) >= (int)sizeof host) return false;
    dlen = (size_t)snprintf(out->dos, sizeof out->dos, "%c:\\", drive);
    out->exists = true;
    out->is_dir = true;
    for (i = 0; i < n; i++) {
        char real[COMP_MAX];
        bool last = i == n - 1;
        struct stat st;
        size_t hl = strlen(host);
        if (!find_entry(host, comps[i], real, sizeof real)) {
            size_t k;
            if (!last && !(flags & DP_CREATE_DIRS)) return false;
            for (k = 0; comps[i][k]; k++) real[k] = (char)toupper((unsigned char)comps[i][k]);
            real[k] = 0;
            out->exists = false;
            out->is_dir = false;
            if (!last) {
                char dir[DP_PATH_MAX];
                snprintf(dir, sizeof dir, "%s/%s", host, real);
                if (mkdir(dir, 0755) != 0) return false;
                out->exists = true;
                out->is_dir = true;
            }
        }
        if (hl + 1 + strlen(real) >= sizeof host) return false;
        snprintf(host + hl, sizeof host - hl, "/%s", real);
        if (out->exists) {
            if (stat(host, &st) != 0) return false;
            out->is_dir = S_ISDIR(st.st_mode);
            if (!last && !out->is_dir) return false;
        }
        {
            size_t k;
            if (dlen + strlen(comps[i]) + 2 >= sizeof out->dos) return false;
            for (k = 0; real[k] && dlen + 1 < sizeof out->dos; k++)
                out->dos[dlen++] = (char)toupper((unsigned char)real[k]);
            if (!last) out->dos[dlen++] = '\\';
            out->dos[dlen] = 0;
        }
    }
    snprintf(out->host, sizeof out->host, "%s", host);
    return true;
}

bool dp_chdir(dospath_t *dp, const char *path) {
    dp_result r;
    int di;
    if (!dp_resolve(dp, path, 0, &r) || !r.exists || !r.is_dir) return false;
    di = idx_of(r.dos[0]);
    snprintf(dp->cur_dir[di], DP_DOS_MAX, "%s", r.dos + 2);
    return true;
}

bool dp_getcwd(const dospath_t *dp, unsigned drive, char *out, size_t cap) {
    int di = drive == 0 ? idx_of(dp->cur_drive) : (int)drive - 1;
    const char *d;
    if (di < 0 || di >= 26 || !dp->root[di]) return false;
    d = dp->cur_dir[di] + 1; /* drop the leading backslash */
    if (strlen(d) >= cap) return false;
    strcpy(out, d);
    return true;
}
