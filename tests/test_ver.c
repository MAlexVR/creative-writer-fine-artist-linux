/* VER: GetFileVersionInfoSize, GetFileVersionInfo and VerQueryValue over the RT_VERSION resource of an NE file. */
#define _POSIX_C_SOURCE 200809L
#include "dllfix.h"
#include "hostdir.h"
#include "kfixture.h"
#include "ver.h"

static ver_t *vv;
static char *g_root;

typedef struct { uint8_t b[0x400]; unsigned n; } blk;

static void align4(blk *w) { while (w->n & 3) w->b[w->n++] = 0; }

/* A version node: wLength (set by end_node), wValueLength, key, padding; the value follows. */
static unsigned begin_node(blk *w, const char *key, unsigned vlen) {
    unsigned start;
    align4(w);
    start = w->n;
    nb_p16(w->b, w->n + 2, vlen);
    w->n += 4;
    memcpy(&w->b[w->n], key, strlen(key) + 1);
    w->n += (unsigned)strlen(key) + 1;
    align4(w);
    return start;
}

static void end_node(blk *w, unsigned start) { nb_p16(w->b, start, w->n - start); }

static unsigned version_block(blk *w) {
    unsigned root, sfi, st, pn, vfi, tr;
    memset(w, 0, sizeof *w);
    root = begin_node(w, "VS_VERSION_INFO", 52);
    nb_p16(w->b, w->n, 0x04BD); nb_p16(w->b, w->n + 2, 0xFEEF);     /* dwSignature 0xFEEF04BD */
    nb_p16(w->b, w->n + 6, 1);                                       /* dwStrucVersion 0x00010000 */
    nb_p16(w->b, w->n + 8, 1); nb_p16(w->b, w->n + 10, 1);           /* dwFileVersionMS 1.1 */
    nb_p16(w->b, w->n + 12, 18);                                     /* dwFileVersionLS 0.18 */
    w->n += 52;
    sfi = begin_node(w, "StringFileInfo", 0);
    st = begin_node(w, "040904E4", 0);
    pn = begin_node(w, "ProductName", 16);
    memcpy(&w->b[w->n], "Creative Writer", 16);
    w->n += 16;
    end_node(w, pn);
    end_node(w, st);
    end_node(w, sfi);
    vfi = begin_node(w, "VarFileInfo", 0);
    tr = begin_node(w, "Translation", 4);
    w->b[w->n++] = 0x09; w->b[w->n++] = 0x04; w->b[w->n++] = 0xE4; w->b[w->n++] = 0x04;
    end_node(w, tr);
    end_node(w, vfi);
    end_node(w, root);
    return w->n;
}

static void put_exe(const char *rel, bool with_version) {
    static uint8_t img[NB_MAX];
    nb_t t;
    static blk w;
    unsigned n;
    memset(&t, 0, sizeof t);
    mydll_nb(&t);
    t.flags = 0x0002;
    if (with_version) {
        n = version_block(&w);
        t.restype = 16;
        t.res[0] = (nb_res){1, w.b, n};
        t.nres = 1;
    }
    hd_write(g_root, rel, img, nb_build(img, &t));
}

static uint16_t rd16(uint16_t s, uint32_t off) { return (uint16_t)(kmem(s, off, 2)[0] | kmem(s, off, 2)[1] << 8); }
static uint32_t rd32(uint16_t s, uint32_t off) { return rd16(s, off) | (uint32_t)rd16(s, off + 2) << 16; }

static void boot(void) {
    kernel_config cfg;
    memset(&cfg, 0, sizeof cfg);
    g_root = hd_make();
    hd_mkdir(g_root, "CWFA");
    cfg.host_root['C' - 'A'] = g_root;
    cfg.exe_path = "C:\\CWFA\\WRITER.EXE";
    kboot_ex(&cfg, 0, 0x800, 0x1000);
    vv = ver_create(&kh, kk);
    CHECK(vv != NULL);
}

static void down(void) {
    ver_destroy(vv);
    vv = NULL;
    kdown();
    hd_remove(g_root);
}

/* GetFileVersionInfoSize(path, &handle) */
static uint32_t info_size(const char *path, uint16_t hs, uint32_t hoff) {
    CHECK_EQ(FX_CALL(&kf, "VER", 6, FARP(kstr(path), 0), FARP(hs, hoff)), LOADER_STOP_HLT);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return fx_dxax(&kf);
}

static void test_ordinals(void) {
    static const struct { unsigned ord; const char *name; unsigned args; } want[] = {
        {6, "GetFileVersionInfoSize", 8}, {7, "GetFileVersionInfo", 16}, {11, "VerQueryValue", 16}};
    size_t i;
    printf("test_ver_ordinals\n");
    boot();
    for (i = 0; i < sizeof want / sizeof *want; i++) {
        const hle_entry *e = hle_find(&kh, "VER", (uint16_t)want[i].ord);
        CHECK(e && e->fn && !strcmp(e->name, want[i].name) && e->arg_bytes == want[i].args);
    }
    down();
}

static void test_info_size_and_data(void) {
    uint16_t hs, buf;
    uint32_t size;
    blk w;
    unsigned real;
    printf("test_ver_info\n");
    boot();
    real = version_block(&w);
    put_exe("CWFA/WRITER.EXE", true);
    put_exe("CWFA/PLAIN.EXE", false);
    hd_write(g_root, "CWFA/TEXT.TXT", "not a module", 12);
    hs = kseg(0x20);
    memset(kmem(hs, 0, 4), 0xEE, 4);
    size = info_size("C:\\CWFA\\writer.exe", hs, 0);          /* DOS path, any case */
    CHECK_EQ(size, (real + 15) & ~15u);                       /* the resource, as the NE table stores it */
    CHECK_EQ(rd32(hs, 0), 0);                                 /* the handle is unused */
    CHECK_EQ(info_size("C:\\CWFA\\PLAIN.EXE", hs, 0), 0);     /* no version resource */
    CHECK_EQ(info_size("C:\\CWFA\\TEXT.TXT", hs, 0), 0);      /* not an NE file */
    CHECK_EQ(info_size("C:\\CWFA\\NOSUCH.EXE", hs, 0), 0);
    buf = kseg(0x400);
    memset(kmem(buf, 0, 0x400), 0x55, 0x400);
    CHECK_EQ(FX_CALL(&kf, "VER", 7, FARP(kstr("C:\\CWFA\\WRITER.EXE"), 0), LONG(0), LONG(size), FARP(buf, 0)), LOADER_STOP_HLT);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(fx_ax(&kf), 1);
    CHECK(!memcmp(kmem(buf, 0, real), w.b, real));            /* the block as stored */
    CHECK_EQ(kmem(buf, size, 1)[0], 0x55);                    /* nothing written past the size */
    CHECK_EQ(FX_CALL(&kf, "VER", 7, FARP(kstr("C:\\CWFA\\PLAIN.EXE"), 0), LONG(0), LONG(size), FARP(buf, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 0);
    CHECK_EQ(FX_CALL(&kf, "VER", 7, FARP(kstr("C:\\CWFA\\WRITER.EXE"), 0), LONG(0), LONG(16), FARP(buf, 0)), LOADER_STOP_HLT);
    CHECK_EQ(fx_ax(&kf), 1);                                  /* a short buffer gets the head of the block */
    down();
}

/* VerQueryValue(block, subblock, &ptr, &len) */
static uint16_t query(uint16_t buf, const char *sub, uint16_t out) {
    CHECK_EQ(FX_CALL(&kf, "VER", 11, FARP(buf, 0), FARP(kstr(sub), 0), FARP(out, 0), FARP(out, 8)), LOADER_STOP_HLT);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return fx_ax(&kf);
}

static void test_query_value(void) {
    uint16_t buf, out, psel, plen;
    uint32_t size;
    printf("test_ver_query\n");
    boot();
    put_exe("CWFA/WRITER.EXE", true);
    out = kseg(0x20);
    size = info_size("C:\\CWFA\\WRITER.EXE", out, 0);
    buf = kseg(0x400);
    CHECK_EQ(FX_CALL(&kf, "VER", 7, FARP(kstr("C:\\CWFA\\WRITER.EXE"), 0), LONG(0), LONG(size), FARP(buf, 0)), LOADER_STOP_HLT);
    /* the root value: VS_FIXEDFILEINFO */
    memset(kmem(out, 0, 0x20), 0, 0x20);
    CHECK_EQ(query(buf, "\\", out), 1);
    psel = rd16(out, 2);
    plen = rd16(out, 8);
    CHECK_EQ(psel, buf);                                      /* points into the caller's block */
    CHECK_EQ(plen, 52);
    CHECK_EQ(rd32(psel, rd16(out, 0)), 0xFEEF04BDu);
    CHECK_EQ(rd32(psel, rd16(out, 0) + 8), 0x00010001u);
    /* translation table and a string value */
    CHECK_EQ(query(buf, "\\VarFileInfo\\Translation", out), 1);
    CHECK_EQ(rd16(out, 8), 4);
    CHECK_EQ(rd16(rd16(out, 2), rd16(out, 0)), 0x0409);
    CHECK_EQ(rd16(rd16(out, 2), rd16(out, 0) + 2), 0x04E4);
    CHECK_EQ(query(buf, "\\StringFileInfo\\040904E4\\ProductName", out), 1);
    CHECK_EQ(rd16(out, 8), 16);                               /* the stored length, NUL included */
    CHECK(!strcmp((const char *)kmem(rd16(out, 2), rd16(out, 0), 16), "Creative Writer"));
    CHECK_EQ(query(buf, "\\stringfileinfo\\040904e4\\productname", out), 1);   /* keys compare ignoring case */
    /* unknown keys: FALSE, and the outputs are cleared */
    memset(kmem(out, 0, 0x20), 0xAA, 0x20);
    CHECK_EQ(query(buf, "\\StringFileInfo\\040904E4\\Comments", out), 0);
    CHECK_EQ(rd32(out, 0), 0);
    CHECK_EQ(rd16(out, 8), 0);
    CHECK_EQ(query(buf, "\\StringFileInfo\\0407\\ProductName", out), 0);
    CHECK_EQ(query(buf, "", out), 1);                         /* an empty sub-block is the root value as well */
    down();
}

int main(void) {
    test_ordinals();
    test_info_size_and_data();
    test_query_value();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
