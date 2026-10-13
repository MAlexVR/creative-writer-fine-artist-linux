#define _POSIX_C_SOURCE 200809L
/* PDF writer: RunLengthDecode encoding, file structure (xref offsets), text layer escaping and, when poppler's tools
 * are installed, pdfinfo / pdftotext reading the result. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pdf.h"

static int g_checks, g_fails;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (a), _b = (b); g_checks++; if (_a != _b) { g_fails++; \
    printf("  FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

/* the PDF RunLengthDecode filter */
static size_t rle_decode(const uint8_t *s, size_t n, uint8_t *d, size_t cap) {
    size_t i = 0, o = 0;
    while (i < n) {
        unsigned len = s[i++];
        if (len == 128) return o;
        if (len < 128) {
            unsigned k;
            for (k = 0; k <= len && i < n && o < cap; k++) d[o++] = s[i++];
        } else {
            unsigned k, cnt = 257 - len;
            if (i >= n) return (size_t)-1;
            for (k = 0; k < cnt && o < cap; k++) d[o++] = s[i];
            i++;
        }
    }
    return (size_t)-1;                       /* no EOD */
}

static void test_rle(void) {
    uint8_t src[1000], enc[3000], dec[1000];
    size_t i, e, d;
    printf("test_pdf_rle\n");
    memset(src, 7, sizeof src);
    e = pdf_rle_encode(src, sizeof src, enc, sizeof enc);
    CHECK(e > 0 && e < 40);                                      /* 1000 equal bytes: 8 runs + EOD */
    d = rle_decode(enc, e, dec, sizeof dec);
    CHECK_EQ(d, 1000);
    CHECK(!memcmp(src, dec, sizeof src));
    for (i = 0; i < sizeof src; i++) src[i] = (uint8_t)((i * 37 + (i / 7)) & 0xFF);   /* mostly literal */
    e = pdf_rle_encode(src, sizeof src, enc, sizeof enc);
    CHECK(e > 0 && e <= sizeof src + sizeof src / 128 + 2);
    d = rle_decode(enc, e, dec, sizeof dec);
    CHECK_EQ(d, 1000);
    CHECK(!memcmp(src, dec, sizeof src));
    for (i = 0; i < sizeof src; i++) src[i] = (uint8_t)((i / 3) % 4 == 0 ? 9 : i);   /* runs of 3 between literals */
    e = pdf_rle_encode(src, sizeof src, enc, sizeof enc);
    CHECK(e > 0);
    d = rle_decode(enc, e, dec, sizeof dec);
    CHECK_EQ(d, 1000);
    CHECK(!memcmp(src, dec, sizeof src));
    CHECK_EQ(pdf_rle_encode(src, sizeof src, enc, 10), 0);       /* does not fit */
    e = pdf_rle_encode(src, 0, enc, sizeof enc);
    CHECK_EQ(e, 1);                                              /* just EOD */
    CHECK_EQ(enc[0], 128);
}

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    char *b;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)len + 1);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) {
        b[len] = 0;
        *n = (size_t)len;
    }
    return b;
}

/* strstr that does not stop at the NUL bytes of the image data */
static const char *memfind(const char *h, size_t n, const char *w) {
    size_t i, m = strlen(w);
    for (i = 0; i + m <= n; i++)
        if (!memcmp(h + i, w, m)) return h + i;
    return NULL;
}

static int tool_exists(const char *name) {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", name);
    return system(cmd) == 0;
}

static void test_document(void) {
    pdf_t *p = pdf_create();
    uint8_t img[8 * 6], pal[256][3];
    char path[64], *buf, *x;
    size_t n, i;
    int count = 0;
    printf("test_pdf_document\n");
    CHECK(p != NULL);
    memset(pal, 0, sizeof pal);
    pal[1][0] = pal[1][1] = pal[1][2] = 255;
    pal[2][0] = 255;
    for (i = 0; i < sizeof img; i++) img[i] = (uint8_t)(i % 3);
    CHECK(!pdf_page_text(p, 10, 10, 12, "no page yet", 11));
    CHECK(pdf_add_page(p, 612, 792, 8, 6, img, (const uint8_t (*)[3])pal));
    CHECK(pdf_page_text(p, 72, 100, 12, "Hello (World) \\ caf\xE9", 20));
    CHECK(pdf_add_page(p, 612, 792, 8, 6, img, (const uint8_t (*)[3])pal));
    CHECK(pdf_page_text(p, 72, 100, 12, "Second page", 11));
    CHECK_EQ(pdf_page_count(p), 2);
    CHECK(!pdf_add_page(p, 0, 792, 8, 6, img, (const uint8_t (*)[3])pal));
    CHECK(!pdf_add_page(p, 612, 792, 0, 6, img, (const uint8_t (*)[3])pal));
    snprintf(path, sizeof path, "build/test_pdf_%d.pdf", (int)getpid());
    CHECK(pdf_save(p, path));
    buf = slurp(path, &n);
    CHECK(buf != NULL);
    if (buf) {
        CHECK(!strncmp(buf, "%PDF-1.", 7));
        CHECK(memfind(buf, n, "%%EOF") != NULL);
        CHECK(memfind(buf, n, "/Count 2") != NULL);
        CHECK(memfind(buf, n, "/RunLengthDecode") != NULL);
        CHECK(memfind(buf, n, "\\(World\\)") != NULL);              /* parentheses escaped in the text string */
        CHECK(memfind(buf, n, "\\351") != NULL);                    /* 0xE9 as an octal escape */
        /* every xref entry points at "N 0 obj" */
        x = (char *)memfind(buf, n, "\nxref\n");
        CHECK(x != NULL);
        if (x) {
            int objs = 0, k;
            long off;
            char *line;
            sscanf(x + 6, "0 %d", &objs);
            CHECK(objs > 5);
            line = strchr(x + 6, '\n') + 1;
            for (k = 0; k < objs; k++, line += 20) {
                char want[32];
                if (k == 0) continue;
                off = strtol(line, NULL, 10);
                snprintf(want, sizeof want, "%d 0 obj", k);
                CHECK(off > 0 && (size_t)off < n && !strncmp(buf + off, want, strlen(want)));
                count++;
            }
            CHECK(count == objs - 1);
        }
        free(buf);
    }
    if (tool_exists("pdfinfo")) {
        char cmd[200], line[200];
        FILE *f;
        int pages = 0;
        snprintf(cmd, sizeof cmd, "pdfinfo %s 2>&1", path);
        f = popen(cmd, "r");
        CHECK(f != NULL);
        while (f && fgets(line, sizeof line, f)) sscanf(line, "Pages: %d", &pages);
        if (f) pclose(f);
        CHECK_EQ(pages, 2);
    } else printf("  SKIP pdfinfo (not installed)\n");
    if (tool_exists("pdftotext")) {
        char cmd[200];
        char *txt;
        snprintf(cmd, sizeof cmd, "pdftotext %s build/test_pdf_%d.txt 2>&1", path, (int)getpid());
        CHECK_EQ(system(cmd), 0);
        snprintf(cmd, sizeof cmd, "build/test_pdf_%d.txt", (int)getpid());
        txt = slurp(cmd, &n);
        CHECK(txt != NULL);
        if (txt) {
            CHECK(strstr(txt, "Hello (World)") != NULL);
            CHECK(strstr(txt, "Second page") != NULL);
            free(txt);
        }
        remove(cmd);
    } else printf("  SKIP pdftotext (not installed)\n");
    remove(path);
    CHECK(!pdf_save(p, "build/no_such_dir/x.pdf"));
    pdf_free(p);
}

int main(void) {
    test_rle();
    test_document();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails != 0;
}
