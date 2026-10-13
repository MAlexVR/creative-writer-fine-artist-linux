#ifndef HOSTDIR_H
#define HOSTDIR_H
/* Temporary host directories for tests that touch the file system. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>

static inline char *hd_make(void) {
    const char *base = getenv("TMPDIR");
    static char path[512];
    snprintf(path, sizeof path, "%s/cwfa_test_XXXXXX", base && *base ? base : "/tmp");
    return mkdtemp(path) ? path : NULL;
}

/* only ever removes directories created by hd_make */
static inline void hd_remove(const char *dir) {
    char cmd[600];
    if (!strstr(dir, "cwfa_test_")) return;
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "cleanup of %s failed\n", dir);
}

static inline void hd_mkdir(const char *root, const char *rel) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    mkdir(p, 0755);
}

static inline void hd_write(const char *root, const char *rel, const void *data, size_t n) {
    char p[1024];
    FILE *f;
    snprintf(p, sizeof p, "%s/%s", root, rel);
    f = fopen(p, "wb");
    if (f) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}

static inline size_t hd_read(const char *root, const char *rel, char *buf, size_t cap) {
    char p[1024];
    FILE *f;
    size_t n = 0;
    snprintf(p, sizeof p, "%s/%s", root, rel);
    f = fopen(p, "rb");
    if (f) {
        n = fread(buf, 1, cap - 1, f);
        fclose(f);
    }
    buf[n] = 0;
    return n;
}

static inline bool hd_exists(const char *root, const char *rel) {
    char p[1024];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", root, rel);
    return stat(p, &st) == 0;
}
#endif
