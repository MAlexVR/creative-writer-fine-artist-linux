#define _POSIX_C_SOURCE 200809L
#include "ini.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- parsing ------------------------------------------------------------ */

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    return *a == *b;
}

static char *trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

typedef struct {
    char **line; /* without terminators */
    size_t n, cap;
    bool crlf;
} ini_t;

static void ini_free(ini_t *f) {
    size_t i;
    for (i = 0; i < f->n; i++) free(f->line[i]);
    free(f->line);
    memset(f, 0, sizeof *f);
}

static bool push(ini_t *f, const char *s, size_t len) {
    char *c;
    if (f->n == f->cap) {
        size_t cap = f->cap ? f->cap * 2 : 32;
        char **v = realloc(f->line, cap * sizeof *v);
        if (!v) return false;
        f->line = v;
        f->cap = cap;
    }
    if (!(c = malloc(len + 1))) return false;
    memcpy(c, s, len);
    c[len] = 0;
    f->line[f->n++] = c;
    return true;
}

static bool insert_line(ini_t *f, size_t at, const char *s) {
    char *c = strdup(s);
    if (!c || !push(f, "", 0)) {
        free(c);
        return false;
    }
    free(f->line[f->n - 1]);
    memmove(&f->line[at + 1], &f->line[at], (f->n - 1 - at) * sizeof *f->line);
    f->line[at] = c;
    return true;
}

/* A missing file reads as an empty one. */
static bool load(const char *path, ini_t *f) {
    FILE *fp = fopen(path, "rb");
    char *buf, *p, *nl;
    long sz;
    memset(f, 0, sizeof *f);
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }
    buf = malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    buf[sz] = 0;
    for (p = buf; *p;) {
        size_t len;
        nl = strchr(p, '\n');
        len = nl ? (size_t)(nl - p) : strlen(p);
        if (nl && len && p[len - 1] == '\r') {
            f->crlf = true;
            len--;
        }
        if (!push(f, p, len)) break;
        p += len + (nl ? (p[len] == '\r' ? 2 : 1) : 0);
        if (!nl) break;
    }
    free(buf);
    return true;
}

/* "[name]" -> name (trimmed) in out; false if the line is not a section header. */
static bool is_section(const char *line, char *out, size_t cap) {
    const char *s = line, *e;
    size_t n;
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '[' || !(e = strchr(s, ']'))) return false;
    n = (size_t)(e - s - 1);
    if (n >= cap) n = cap - 1;
    memcpy(out, s + 1, n);
    out[n] = 0;
    memmove(out, trim(out), strlen(trim(out)) + 1);
    return true;
}

/* key=value line: splits into key/value (trimmed, in tmp). */
static bool is_pair(const char *line, char *tmp, size_t cap, char **key, char **val) {
    char *eq, *t;
    while (*line == ' ' || *line == '\t') line++;
    if (*line == ';' || *line == '[' || !*line) return false;
    snprintf(tmp, cap, "%s", line);
    if (!(eq = strchr(tmp, '='))) return false;
    *eq = 0;
    t = trim(tmp);
    if (!*t) return false;
    *key = t;
    *val = trim(eq + 1);
    return true;
}

/* [first, end) of the lines of the first section called `name`: first is the header. */
static bool find_section(const ini_t *f, const char *name, size_t from, size_t *first, size_t *end) {
    size_t i;
    char sec[256];
    for (i = from; i < f->n; i++) {
        if (is_section(f->line[i], sec, sizeof sec) && ieq(sec, name)) {
            size_t j;
            for (j = i + 1; j < f->n && !is_section(f->line[j], sec, sizeof sec); j++) {}
            *first = i;
            *end = j;
            return true;
        }
    }
    return false;
}

/* ---- reading ------------------------------------------------------------ */

static size_t copy_out(char *out, size_t size, const char *s, size_t len) {
    if (!size) return 0;
    if (len > size - 1) len = size - 1;
    memcpy(out, s, len);
    out[len] = 0;
    return len;
}

/* Name list for NULL section / NULL key: NUL-separated, extra NUL at the end. */
static size_t list_out(char *out, size_t size, const char *list, size_t len) {
    size_t n;
    if (size < 2) return 0;
    n = len < size - 2 ? len : size - 2;
    memcpy(out, list, n);
    out[n] = out[n + 1] = 0;
    return n;
}

bool ini_lookup(const char *file, const char *section, const char *key, char *value, size_t cap) {
    ini_t f;
    size_t first, end, from = 0, i;
    char tmp[1024], *k, *v;
    bool found = false;
    if (!load(file, &f)) return false;
    while (!found && find_section(&f, section, from, &first, &end)) {
        for (i = first + 1; i < end; i++)
            if (is_pair(f.line[i], tmp, sizeof tmp, &k, &v) && ieq(k, key)) {
                copy_out(value, cap, v, strlen(v));
                found = true;
                break;
            }
        from = end;
    }
    ini_free(&f);
    return found;
}

size_t ini_get_string(const char *file, const char *section, const char *key, const char *def,
                      char *out, size_t size) {
    ini_t f;
    char tmp[1024], sec[256], *k, *v;
    char *list = NULL;
    size_t len = 0, cap = 0, i, first, end, from = 0, n;
    if (!def) def = "";
    if (!size) return 0;
    if (!load(file, &f)) f.n = 0;
    if (!section || !key) { /* enumerations */
        list = malloc(8192);
        cap = 8192;
        if (!list) {
            ini_free(&f);
            return 0;
        }
        if (!section) {
            for (i = 0; i < f.n; i++)
                if (is_section(f.line[i], sec, sizeof sec) && len + strlen(sec) + 1 < cap) {
                    strcpy(list + len, sec);
                    len += strlen(sec) + 1;
                }
        } else {
            while (find_section(&f, section, from, &first, &end)) {
                for (i = first + 1; i < end; i++)
                    if (is_pair(f.line[i], tmp, sizeof tmp, &k, &v) && len + strlen(k) + 1 < cap) {
                        strcpy(list + len, k);
                        len += strlen(k) + 1;
                    }
                from = end;
            }
        }
        n = len ? list_out(out, size, list, len) : copy_out(out, size, def, strlen(def));
        free(list);
        ini_free(&f);
        return n;
    }
    ini_free(&f);
    if (ini_lookup(file, section, key, tmp, sizeof tmp)) return copy_out(out, size, tmp, strlen(tmp));
    return copy_out(out, size, def, strlen(def));
}

/* ---- writing ------------------------------------------------------------ */

static bool save(const char *path, const ini_t *f) {
    FILE *fp = fopen(path, "wb");
    size_t i;
    bool ok = fp != NULL;
    for (i = 0; ok && i < f->n; i++)
        ok = fputs(f->line[i], fp) >= 0 && fputs(f->crlf ? "\r\n" : "\n", fp) >= 0;
    if (fp && fclose(fp) != 0) ok = false;
    return ok;
}

bool ini_write(const char *file, const char *section, const char *key, const char *value) {
    ini_t f;
    size_t first, end, i, from = 0, at;
    char tmp[1024], line[1100], *k, *v;
    bool ok = true;
    if (!load(file, &f)) memset(&f, 0, sizeof f);
    if (!key) { /* delete the whole section */
        while (find_section(&f, section, 0, &first, &end)) {
            size_t n = end - first;
            for (i = first; i < end; i++) free(f.line[i]);
            memmove(&f.line[first], &f.line[end], (f.n - end) * sizeof *f.line);
            f.n -= n;
        }
        ok = save(file, &f);
        ini_free(&f);
        return ok;
    }
    while (find_section(&f, section, from, &first, &end)) {
        for (i = first + 1; i < end; i++) {
            if (!is_pair(f.line[i], tmp, sizeof tmp, &k, &v) || !ieq(k, key)) continue;
            if (!value) { /* delete the key */
                free(f.line[i]);
                memmove(&f.line[i], &f.line[i + 1], (f.n - i - 1) * sizeof *f.line);
                f.n--;
            } else {
                char *nl;
                snprintf(line, sizeof line, "%s=%s", k, value);
                if (!(nl = strdup(line))) ok = false;
                else {
                    free(f.line[i]);
                    f.line[i] = nl;
                }
            }
            if (ok) ok = save(file, &f);
            ini_free(&f);
            return ok;
        }
        from = end;
    }
    if (!value) { /* nothing to delete */
        ini_free(&f);
        return true;
    }
    snprintf(line, sizeof line, "%s=%s", key, value);
    if (find_section(&f, section, 0, &first, &end)) { /* new key at the end of the first section */
        at = end;
        while (at > first + 1 && !*trim(strcpy(tmp, f.line[at - 1]))) at--;
        ok = insert_line(&f, at, line);
    } else {
        if (f.n && *trim(strcpy(tmp, f.line[f.n - 1]))) ok = push(&f, "", 0);
        snprintf(tmp, sizeof tmp, "[%s]", section);
        ok = ok && push(&f, tmp, strlen(tmp)) && push(&f, line, strlen(line));
    }
    if (ok) ok = save(file, &f);
    ini_free(&f);
    return ok;
}
