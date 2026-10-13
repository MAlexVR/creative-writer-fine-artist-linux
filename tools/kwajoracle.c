/* Dev-only oracle (NOT part of the runtime or `make test`): expands KWAJ files with libmspack and with our own
 * decoder (src/kwaj.c) and compares the bytes.
 *   cc -Isrc tools/kwajoracle.c src/kwaj.c -lmspack -o build/kwajoracle
 *   build/kwajoracle FILE...        prints OK/DIFF/ERR per file; exit status 1 if any differs
 * With -o DIR it also keeps libmspack's output as DIR/<basename>.out. */
#include <mspack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kwaj.h"

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

int main(int argc, char **argv) {
    struct mskwaj_decompressor *d = mspack_create_kwaj_decompressor(NULL);
    const char *outdir = NULL;
    int i, bad = 0;
    if (!d) return 2;
    if (argc > 2 && !strcmp(argv[1], "-o")) {
        outdir = argv[2];
        argc -= 2;
        argv += 2;
    }
    for (i = 1; i < argc; i++) {
        char tmp[1024];
        size_t in_len, a_len = 0, b_len = 0;
        uint8_t *in = slurp(argv[i], &in_len), *a = NULL, *b;
        const char *base = strrchr(argv[i], '/');
        int err;
        kwaj_err ours;
        base = base ? base + 1 : argv[i];
        snprintf(tmp, sizeof tmp, "%s/%s.out", outdir ? outdir : "/tmp", base);
        if (!in) {
            printf("ERR  %s: unreadable\n", argv[i]);
            bad++;
            continue;
        }
        err = d->decompress(d, argv[i], tmp);
        ours = kwaj_expand(in, in_len, &a, &a_len);
        b = err ? NULL : slurp(tmp, &b_len);
        if (err || ours != KWAJ_OK)
            printf("ERR  %s: libmspack %d, ours %s\n", base, err, kwaj_strerror(ours));
        else if (a_len == b_len && b && !memcmp(a, b, a_len))
            printf("OK   %s (%zu bytes)\n", base, a_len);
        else {
            printf("DIFF %s: ours %zu bytes, libmspack %zu\n", base, a_len, b_len);
            bad++;
        }
        if (!outdir) remove(tmp);
        free(in);
        free(a);
        free(b);
    }
    mspack_destroy_kwaj_decompressor(d);
    return bad != 0;
}
