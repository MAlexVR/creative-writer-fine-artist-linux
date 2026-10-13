#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ne.h"

static int g_checks, g_fails;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_fails++;                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define CHECK_ERR(expr, want)                                                \
    do {                                                                     \
        ne_err e_ = (expr);                                                  \
        g_checks++;                                                          \
        if (e_ != (want)) {                                                  \
            g_fails++;                                                       \
            printf("  FAIL %s:%d: %s -> %d, want %d\n", __FILE__, __LINE__,  \
                   #expr, (int)e_, (int)(want));                             \
        }                                                                    \
    } while (0)

/* ---- synthetic fixture ------------------------------------------------ */

#define FX_MAX 0x16000
#define NE_AT 0x40
/* all table offsets below are relative to the NE header */
#define FX_SEGTAB 0x40
#define FX_RESNAMES 0x60
#define FX_FILE_SIZE 0x15000

typedef struct {
    uint8_t b[FX_MAX];
    size_t n;
    uint32_t modref_off, impname_off, entry_off, nonres_off;
} fx_t;

static void p8(fx_t *f, size_t at, unsigned v) { f->b[at] = (uint8_t)v; }
static void p16(fx_t *f, size_t at, unsigned v) { p8(f, at, v); p8(f, at + 1, v >> 8); }
static void p32(fx_t *f, size_t at, uint32_t v) { p16(f, at, v & 0xffff); p16(f, at + 2, v >> 16); }

static size_t put_name(fx_t *f, size_t at, const char *s, unsigned ord) {
    size_t l = strlen(s);
    p8(f, at, (unsigned)l);
    memcpy(&f->b[at + 1], s, l);
    p16(f, at + 1 + l, ord);
    return at + 1 + l + 2;
}

static void fx_build(fx_t *f) {
    size_t p, h = NE_AT;
    memset(f, 0, sizeof *f);
    f->n = FX_FILE_SIZE;
    memcpy(f->b, "MZ", 2);
    p32(f, 0x3c, NE_AT);
    memcpy(&f->b[h], "NE", 2);
    p8(f, h + 2, 5); p8(f, h + 3, 10);              /* linker 5.10 */
    p16(f, h + 0x0c, 0x030a);                        /* flags */
    p16(f, h + 0x0e, 2);                             /* auto data seg */
    p16(f, h + 0x10, 0x400); p16(f, h + 0x12, 0x2800);
    p16(f, h + 0x14, 0x10); p16(f, h + 0x16, 1);     /* IP, CS */
    p16(f, h + 0x18, 0); p16(f, h + 0x1a, 2);        /* SP, SS */
    p16(f, h + 0x1c, 4);                             /* segments */
    p16(f, h + 0x1e, 2);                             /* module refs */
    p16(f, h + 0x22, FX_SEGTAB);
    p16(f, h + 0x24, FX_RESNAMES);                   /* no resources */
    p16(f, h + 0x26, FX_RESNAMES);
    p16(f, h + 0x30, 1);                             /* movable entries */
    p16(f, h + 0x32, 4);                             /* align shift */
    p8(f, h + 0x36, 2);                              /* Windows */

    /* segment table: offset in sectors, length, flags, min alloc */
    p = h + FX_SEGTAB;
    p16(f, p + 0, 0x200); p16(f, p + 2, 0x40); p16(f, p + 4, 0x0150); p16(f, p + 6, 0x40);
    p16(f, p + 8, 0x300); p16(f, p + 10, 0x20); p16(f, p + 12, 0x0001); p16(f, p + 14, 0x100);
    p16(f, p + 16, 0x500); p16(f, p + 18, 0); p16(f, p + 20, 0x0011); p16(f, p + 22, 0);
    p16(f, p + 24, 0); p16(f, p + 26, 0); p16(f, p + 28, 0x0001); p16(f, p + 30, 0);

    p = h + FX_RESNAMES;
    p = put_name(f, p, "TESTMOD", 0);
    p = put_name(f, p, "Foo", 1);
    p = put_name(f, p, "Bar", 2);
    p8(f, p++, 0);

    f->modref_off = (uint32_t)(p - h);
    p16(f, p, 1); p16(f, p + 2, 8); p += 4;
    f->impname_off = (uint32_t)(p - h);
    p8(f, p++, 0);
    p = put_name(f, p, "KERNEL", 0) - 2;   /* offset 1 */
    p = put_name(f, p, "USER", 0) - 2;     /* offset 8 */
    p = put_name(f, p, "WinFunc", 0) - 2;  /* offset 13 */
    p8(f, p++, 0);

    f->entry_off = (uint32_t)(p - h);
    p8(f, p, 2); p8(f, p + 1, 1);                              /* 2 fixed in seg 1 */
    p8(f, p + 2, 1); p16(f, p + 3, 0x10);
    p8(f, p + 5, 1); p16(f, p + 6, 0x20);
    p += 8;
    p8(f, p, 1); p8(f, p + 1, 0xff);                           /* 1 moveable */
    p8(f, p + 2, 3); p16(f, p + 3, 0x3fcd); p8(f, p + 5, 2); p16(f, p + 6, 0x30);
    p += 8;
    p8(f, p, 1); p8(f, p + 1, 0); p += 2;                      /* 1 unused */
    p8(f, p, 1); p8(f, p + 1, 3);                              /* 1 fixed in seg 3 */
    p8(f, p + 2, 0); p16(f, p + 3, 0x40);
    p += 5;
    p8(f, p++, 0);
    p16(f, h + 0x04, f->entry_off);
    p16(f, h + 0x06, (unsigned)(p - h - f->entry_off));
    p16(f, h + 0x28, f->modref_off);
    p16(f, h + 0x2a, f->impname_off);

    f->nonres_off = 0x1000;
    p = f->nonres_off;
    p = put_name(f, p, "Test Description", 0);
    p = put_name(f, p, "Baz", 3);
    p8(f, p++, 0);
    p32(f, h + 0x2c, f->nonres_off);
    p16(f, h + 0x20, (unsigned)(p - f->nonres_off));

    /* segment 1 relocations (file 0x2000, length 0x40) */
    p = 0x2000 + 0x40;
    p16(f, p, 5); p += 2;
    /* 1: far ptr -> internal seg 2 off 0x10, chained from 0x04 */
    p8(f, p, 3); p8(f, p + 1, 0); p16(f, p + 2, 0x04); p8(f, p + 4, 2); p8(f, p + 5, 0); p16(f, p + 6, 0x10); p += 8;
    /* 2: offset -> imported ordinal, modref 1 ordinal 5, chain head 0x20 */
    p8(f, p, 5); p8(f, p + 1, 1); p16(f, p + 2, 0x20); p16(f, p + 4, 1); p16(f, p + 6, 5); p += 8;
    /* 3: selector -> imported name, modref 2 name offset 13, additive */
    p8(f, p, 2); p8(f, p + 1, 2 | 4); p16(f, p + 2, 0x30); p16(f, p + 4, 2); p16(f, p + 6, 13); p += 8;
    /* 4: lobyte -> OS fixup type 1, additive */
    p8(f, p, 0); p8(f, p + 1, 3 | 4); p16(f, p + 2, 0x38); p16(f, p + 4, 1); p16(f, p + 6, 0); p += 8;
    /* 5: far ptr -> internal movable entry ordinal 3, additive */
    p8(f, p, 3); p8(f, p + 1, 0 | 4); p16(f, p + 2, 0x3a); p8(f, p + 4, 0xff); p8(f, p + 5, 0); p16(f, p + 6, 3); p += 8;
    /* chain data inside segment 1 */
    p16(f, 0x2000 + 0x04, 0x10);
    p16(f, 0x2000 + 0x10, 0xffff);
    p16(f, 0x2000 + 0x20, 0xffff);
}

/* ---- step 1: MZ + NE header ------------------------------------------ */

static void test_header(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    printf("test_header\n");
    fx_build(f);
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK(ne.ne_off == NE_AT);
    CHECK(ne.linker_major == 5 && ne.linker_minor == 10);
    CHECK(ne.flags == 0x030a);
    CHECK(ne.auto_data_seg == 2);
    CHECK(ne.heap_size == 0x400 && ne.stack_size == 0x2800);
    CHECK(ne.cs == 1 && ne.ip == 0x10 && ne.ss == 2 && ne.sp == 0);
    CHECK(ne.seg_count == 4 && ne.modref_count == 2);
    CHECK(ne.align_shift == 4 && ne.target_os == 2);
    CHECK(ne.segtab_off == NE_AT + FX_SEGTAB);
    CHECK(ne.resname_off == NE_AT + FX_RESNAMES);
    CHECK(ne.modref_off == NE_AT + f->modref_off);
    CHECK(ne.impname_off == NE_AT + f->impname_off);
    CHECK(ne.entry_off == NE_AT + f->entry_off);
    CHECK(ne.nonres_off == f->nonres_off);
    ne_free(&ne);
    free(f);
}

static void test_align_shift_zero_means_9(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    printf("test_align_shift_zero_means_9\n");
    fx_build(f);
    p16(f, NE_AT + 0x32, 0);
    for (int i = 0; i < 4; i++) p16(f, NE_AT + FX_SEGTAB + i * 8, 0); /* sectors would overshoot at <<9 */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK(ne.align_shift == 9);
    ne_free(&ne);
    free(f);
}

static void test_header_rejects(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    printf("test_header_rejects\n");

    CHECK_ERR(ne_open_buffer(NULL, "x", 1), NE_ERR_ARG);
    CHECK_ERR(ne_open_buffer(&ne, NULL, 10), NE_ERR_ARG);

    fx_build(f);
    CHECK_ERR(ne_open_buffer(&ne, f->b, 0x20), NE_ERR_TRUNCATED);

    fx_build(f);
    f->b[0] = 'Z';
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_MZ);

    fx_build(f);
    p32(f, 0x3c, 0x7fffffff);
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_OFFSET);

    fx_build(f);
    p32(f, 0x3c, 0xfffffff0u); /* offset + header size would wrap 32 bits */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_OFFSET);

    fx_build(f);
    f->b[NE_AT] = 'L'; f->b[NE_AT + 1] = 'E';
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_NE);

    fx_build(f);
    CHECK_ERR(ne_open_buffer(&ne, f->b, NE_AT + 0x20), NE_ERR_TRUNCATED);

    fx_build(f);
    p16(f, NE_AT + 0x22, 0xff00); /* segment table beyond EOF */
    CHECK_ERR(ne_open_buffer(&ne, f->b, 0x1000), NE_ERR_BAD_OFFSET);

    fx_build(f);
    p16(f, NE_AT + 0x28, 0xffff); /* module ref table beyond EOF */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n - 0x14000), NE_ERR_BAD_OFFSET);

    fx_build(f);
    p32(f, NE_AT + 0x2c, 0x7ffffff0); /* non-resident names beyond EOF */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_OFFSET);

    free(f);
}

static void test_strerror(void) {
    int i;
    printf("test_strerror\n");
    for (i = NE_OK; i <= NE_ERR_BAD_CHAIN; i++) {
        const char *s = ne_strerror((ne_err)i);
        CHECK(s != NULL && s[0] != '\0');
    }
    CHECK(ne_strerror((ne_err)999) != NULL);
}

static void test_open_file(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    FILE *fp;
    const char *path = "build/synthetic.exe";
    printf("test_open_file\n");
    CHECK_ERR(ne_open_file(&ne, "build/does-not-exist.exe"), NE_ERR_IO);
    CHECK_ERR(ne_open_file(&ne, NULL), NE_ERR_ARG);
    fx_build(f);
    fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (fp) {
        fwrite(f->b, 1, f->n, fp);
        fclose(fp);
        CHECK_ERR(ne_open_file(&ne, path), NE_OK);
        CHECK(ne.seg_count == 4);
        ne_free(&ne);
        remove(path);
    }
    free(f);
}

/* ---- step 2: segments, names, entry table ---------------------------- */

static void test_segments(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    printf("test_segments\n");
    fx_build(f);
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK(ne.segs != NULL);
    CHECK(ne.segs[0].file_offset == 0x2000 && ne.segs[0].file_length == 0x40);
    CHECK(ne.segs[0].flags == 0x0150 && ne.segs[0].alloc_size == 0x40);
    CHECK((ne.segs[0].flags & NE_SEG_DATA) == 0);
    CHECK(ne.segs[0].flags & NE_SEG_MOVEABLE);
    CHECK(ne.segs[0].flags & NE_SEG_PRELOAD);
    CHECK(ne.segs[0].flags & NE_SEG_RELOCINFO);
    CHECK(ne.segs[1].file_offset == 0x3000 && ne.segs[1].file_length == 0x20);
    CHECK(ne.segs[1].flags & NE_SEG_DATA);
    CHECK(ne.segs[1].alloc_size == 0x100);
    CHECK(ne.segs[2].file_offset == 0x5000 && ne.segs[2].file_length == 0x10000);
    CHECK(ne.segs[2].alloc_size == 0x10000);
    CHECK(ne.segs[3].file_offset == 0 && ne.segs[3].file_length == 0);
    CHECK(ne.segs[3].alloc_size == 0x10000);
    ne_free(&ne);
    free(f);
}

static void test_segments_reject(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    printf("test_segments_reject\n");
    fx_build(f);
    CHECK_ERR(ne_open_buffer(&ne, f->b, 0x14000), NE_ERR_BAD_OFFSET); /* 64K seg cut */
    fx_build(f);
    p16(f, NE_AT + FX_SEGTAB + 8 + 2, 0xffff); /* length runs past EOF? 0x3000+0xffff < size */
    p16(f, NE_AT + FX_SEGTAB + 8, 0xfff0);     /* sector way out */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_OFFSET);
    free(f);
}

static void test_names(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    printf("test_names\n");
    fx_build(f);
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK(ne.resident_count == 3);
    CHECK(strcmp(ne.resident_names[0].text, "TESTMOD") == 0 && ne.resident_names[0].ordinal == 0);
    CHECK(strcmp(ne.resident_names[1].text, "Foo") == 0 && ne.resident_names[1].ordinal == 1);
    CHECK(strcmp(ne.resident_names[2].text, "Bar") == 0 && ne.resident_names[2].ordinal == 2);
    CHECK(ne.nonres_count == 2);
    CHECK(strcmp(ne.nonres_names[0].text, "Test Description") == 0);
    CHECK(strcmp(ne.nonres_names[1].text, "Baz") == 0 && ne.nonres_names[1].ordinal == 3);
    ne_free(&ne);

    fx_build(f);
    p16(f, NE_AT + 0x20, 5); /* table too short for "Test Description" */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_TABLE);
    free(f);
}

static void test_modrefs_and_imported_names(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    char buf[NE_NAME_MAX + 1];
    printf("test_modrefs_and_imported_names\n");
    fx_build(f);
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK_ERR(ne_modref_name(&ne, 1, buf), NE_OK);
    CHECK(strcmp(buf, "KERNEL") == 0);
    CHECK_ERR(ne_modref_name(&ne, 2, buf), NE_OK);
    CHECK(strcmp(buf, "USER") == 0);
    CHECK_ERR(ne_modref_name(&ne, 0, buf), NE_ERR_NOT_FOUND);
    CHECK_ERR(ne_modref_name(&ne, 3, buf), NE_ERR_NOT_FOUND);
    CHECK_ERR(ne_imported_name(&ne, 13, buf), NE_OK);
    CHECK(strcmp(buf, "WinFunc") == 0);
    CHECK_ERR(ne_imported_name(&ne, 0x7000, buf), NE_ERR_BAD_OFFSET);
    CHECK_ERR(ne_imported_name(&ne, 20, buf), NE_ERR_BAD_TABLE); /* length byte overruns table */
    CHECK_ERR(ne_modref_name(NULL, 1, buf), NE_ERR_ARG);
    ne_free(&ne);

    fx_build(f);
    p16(f, NE_AT + f->modref_off, 0xff00); /* modref name offset outside the table */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK_ERR(ne_modref_name(&ne, 1, buf), NE_ERR_BAD_OFFSET);
    ne_free(&ne);
    free(f);
}

static void test_entry_table(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    const ne_entry *e;
    uint16_t ord;
    printf("test_entry_table\n");
    fx_build(f);
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK(ne.entry_count == 5);
    CHECK_ERR(ne_entry_for_ordinal(&ne, 1, &e), NE_OK);
    CHECK(e->kind == NE_ENTRY_FIXED && e->segment == 1 && e->offset == 0x10 && e->flags == 1);
    CHECK_ERR(ne_entry_for_ordinal(&ne, 2, &e), NE_OK);
    CHECK(e->kind == NE_ENTRY_FIXED && e->segment == 1 && e->offset == 0x20);
    CHECK_ERR(ne_entry_for_ordinal(&ne, 3, &e), NE_OK);
    CHECK(e->kind == NE_ENTRY_MOVEABLE && e->segment == 2 && e->offset == 0x30 && e->flags == 3);
    CHECK_ERR(ne_entry_for_ordinal(&ne, 4, &e), NE_ERR_NOT_FOUND); /* unused slot */
    CHECK_ERR(ne_entry_for_ordinal(&ne, 5, &e), NE_OK);
    CHECK(e->segment == 3 && e->offset == 0x40 && e->flags == 0);
    CHECK_ERR(ne_entry_for_ordinal(&ne, 0, &e), NE_ERR_NOT_FOUND);
    CHECK_ERR(ne_entry_for_ordinal(&ne, 6, &e), NE_ERR_NOT_FOUND);

    CHECK_ERR(ne_find_export(&ne, "Foo", &ord), NE_OK);
    CHECK(ord == 1);
    CHECK_ERR(ne_find_export(&ne, "Baz", &ord), NE_OK);
    CHECK(ord == 3);
    CHECK_ERR(ne_find_export(&ne, "Nope", &ord), NE_ERR_NOT_FOUND);
    CHECK_ERR(ne_find_export(&ne, "TESTMOD", &ord), NE_ERR_NOT_FOUND);
    ne_free(&ne);

    fx_build(f);
    f->b[NE_AT + f->entry_off] = 200; /* bundle larger than the table */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_TABLE);

    fx_build(f);
    p16(f, NE_AT + 0x06, 3); /* table cut inside a bundle */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_TABLE);
    free(f);
}

/* ---- step 3: relocations and imports --------------------------------- */

static void test_relocations(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    const ne_reloc *r;
    printf("test_relocations\n");
    fx_build(f);
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK(ne.segs[0].reloc_count == 5);
    CHECK(ne.segs[1].reloc_count == 0 && ne.segs[1].relocs == NULL);
    r = ne.segs[0].relocs;
    CHECK(r[0].src_type == NE_SRC_FARPTR && r[0].target_type == NE_TGT_INTERNAL);
    CHECK(!r[0].additive && r[0].src_offset == 4 && r[0].a == 2 && r[0].b == 0x10);
    CHECK(r[1].src_type == NE_SRC_OFFSET && r[1].target_type == NE_TGT_IMPORT_ORDINAL);
    CHECK(!r[1].additive && r[1].src_offset == 0x20 && r[1].a == 1 && r[1].b == 5);
    CHECK(r[2].src_type == NE_SRC_SELECTOR && r[2].target_type == NE_TGT_IMPORT_NAME);
    CHECK(r[2].additive && r[2].src_offset == 0x30 && r[2].a == 2 && r[2].b == 13);
    CHECK(r[3].src_type == NE_SRC_LOBYTE && r[3].target_type == NE_TGT_OSFIXUP);
    CHECK(r[3].additive && r[3].a == 1);
    CHECK(r[4].target_type == NE_TGT_INTERNAL && r[4].a == NE_SEG_MOVEABLE_REF && r[4].b == 3);
    ne_free(&ne);
    free(f);
}

static void test_relocation_sites(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    uint16_t sites[8];
    size_t n = 0;
    printf("test_relocation_sites\n");
    fx_build(f);
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK_ERR(ne_reloc_sites(&ne, 0, &ne.segs[0].relocs[0], sites, 8, &n), NE_OK);
    CHECK(n == 2 && sites[0] == 4 && sites[1] == 0x10);
    CHECK_ERR(ne_reloc_sites(&ne, 0, &ne.segs[0].relocs[0], NULL, 0, &n), NE_OK);
    CHECK(n == 2);
    CHECK_ERR(ne_reloc_sites(&ne, 0, &ne.segs[0].relocs[1], sites, 8, &n), NE_OK);
    CHECK(n == 1 && sites[0] == 0x20);
    CHECK_ERR(ne_reloc_sites(&ne, 0, &ne.segs[0].relocs[2], sites, 8, &n), NE_OK); /* additive */
    CHECK(n == 1 && sites[0] == 0x30);
    CHECK_ERR(ne_reloc_sites(&ne, 9, &ne.segs[0].relocs[2], sites, 8, &n), NE_ERR_ARG);
    CHECK_ERR(ne_reloc_sites(&ne, 0, NULL, sites, 8, &n), NE_ERR_ARG);
    ne_free(&ne);

    fx_build(f);
    p16(f, 0x2000 + 0x20, 0x20); /* chain points at itself */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK_ERR(ne_reloc_sites(&ne, 0, &ne.segs[0].relocs[1], sites, 8, &n), NE_ERR_BAD_CHAIN);
    ne_free(&ne);

    fx_build(f);
    p16(f, 0x2000 + 0x20, 0x100); /* next link beyond the 0x40-byte segment */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK_ERR(ne_reloc_sites(&ne, 0, &ne.segs[0].relocs[1], sites, 8, &n), NE_ERR_BAD_OFFSET);
    ne_free(&ne);
    free(f);
}

static void test_relocation_rejects(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    const size_t rec = 0x2000 + 0x40 + 2; /* first record of segment 1 */
    printf("test_relocation_rejects\n");
    fx_build(f);
    p16(f, 0x2000 + 0x40, 0x3000); /* count overruns the file */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_OFFSET);

    fx_build(f);
    p8(f, rec + 8, 9); /* record 2: unknown source type */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_TABLE);

    fx_build(f);
    p16(f, rec + 8 + 4, 7); /* record 2: module ref 7 of 2 */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_TABLE);

    fx_build(f);
    p8(f, rec + 4, 9); /* record 1: internal target segment 9 of 4 */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_TABLE);

    fx_build(f);
    p16(f, rec + 16 + 6, 0x7000); /* record 3: imported name offset outside table */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_OFFSET);

    fx_build(f);
    p16(f, NE_AT + 0x32, 20); /* alignment shift that overflows 32-bit offsets */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_TABLE);
    free(f);
}

static void test_collect_imports(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    ne_import *imp = NULL;
    size_t n = 0;
    printf("test_collect_imports\n");
    fx_build(f);
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK_ERR(ne_collect_imports(&ne, &imp, &n), NE_OK);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(imp[0].modref == 1 && !imp[0].by_name && imp[0].ordinal == 5 && imp[0].refs == 1);
        CHECK(imp[1].modref == 2 && imp[1].by_name && strcmp(imp[1].name, "WinFunc") == 0);
    }
    ne_free_imports(imp);
    ne_free(&ne);

    /* record 3 retargeted at KERNEL ordinal 5: one distinct import, two refs */
    fx_build(f);
    {
        const size_t rec3 = 0x2000 + 0x40 + 2 + 16;
        p8(f, rec3 + 1, 1 | 4);
        p16(f, rec3 + 4, 1);
        p16(f, rec3 + 6, 5);
    }
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK_ERR(ne_collect_imports(&ne, &imp, &n), NE_OK);
    CHECK(n == 1 && imp[0].refs == 2);
    ne_free_imports(imp);
    ne_free(&ne);
    CHECK_ERR(ne_collect_imports(NULL, &imp, &n), NE_ERR_ARG);
    free(f);
}

/* ---- nedump CLI -------------------------------------------------------- */

/* ---- resource table --------------------------------------------------- */

/* Minimal NE (no segments) whose resource table sits at NE+0x40:
 * shift 4; type RT_BITMAP(2): id 1 and "HELLO"; type "MYTYPE": id 7. */
#define RS_TAB 0x40
static size_t rs_build(fx_t *f) {
    size_t h = NE_AT, t = h + RS_TAB, p = t, str;
    memset(f, 0, sizeof *f);
    f->n = 0x1000;
    memcpy(f->b, "MZ", 2);
    p32(f, 0x3c, NE_AT);
    memcpy(&f->b[h], "NE", 2);
    p16(f, h + 0x22, RS_TAB);
    p16(f, h + 0x24, RS_TAB);
    p16(f, h + 0x28, 0x100); p16(f, h + 0x2a, 0x100); p16(f, h + 0x04, 0x100);
    p32(f, h + 0x2c, 0x800);
    p16(f, h + 0x32, 4);
    p16(f, p, 4); p += 2;                                  /* resource shift */
    str = 2 + 8 + 24 + 8 + 12 + 2;                         /* first string, from the table start */
    p16(f, p, 0x8002); p16(f, p + 2, 2); p += 8;          /* RT_BITMAP x2 */
    p16(f, p, 0x20); p16(f, p + 2, 3); p16(f, p + 4, 0x30); p16(f, p + 6, 0x8001); p += 12;
    p16(f, p, 0x24); p16(f, p + 2, 1); p16(f, p + 4, 0x10); p16(f, p + 6, str + 7); p += 12;
    p16(f, p, str); p16(f, p + 2, 1); p += 8;              /* "MYTYPE" x1 */
    p16(f, p, 0x30); p16(f, p + 2, 2); p16(f, p + 4, 0x20); p16(f, p + 6, 0x8007); p += 12;
    p16(f, p, 0); p += 2;
    p = put_name(f, p, "MYTYPE", 0) - 2;                   /* str */
    p = put_name(f, p, "HELLO", 0) - 2;                    /* str + 7 */
    p16(f, h + 0x26, (unsigned)(p - h));                   /* resident names follow */
    p8(f, p, 0);
    return p - h;
}

static void test_resources(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    const ne_resource *r = NULL;
    printf("test_resources\n");
    rs_build(f);
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK(ne.restype_count == 2);
    if (ne.restype_count == 2) {
        CHECK(ne.restypes[0].id == 2 && ne.restypes[0].name[0] == 0 && ne.restypes[0].count == 2);
        CHECK(ne.restypes[0].res[0].id == 1 && ne.restypes[0].res[0].name[0] == 0);
        CHECK(ne.restypes[0].res[0].offset == 0x200 && ne.restypes[0].res[0].length == 0x30);
        CHECK(ne.restypes[0].res[0].flags == 0x30);
        CHECK(strcmp(ne.restypes[0].res[1].name, "HELLO") == 0);
        CHECK(ne.restypes[0].res[1].offset == 0x240 && ne.restypes[0].res[1].length == 0x10);
        CHECK(strcmp(ne.restypes[1].name, "MYTYPE") == 0 && ne.restypes[1].count == 1);
        CHECK(ne.restypes[1].res[0].id == 7 && ne.restypes[1].res[0].length == 0x20);
    }
    CHECK_ERR(ne_find_resource(&ne, (ne_resid){2, NULL}, (ne_resid){1, NULL}, &r), NE_OK);
    CHECK(r && r->offset == 0x200);
    CHECK_ERR(ne_find_resource(&ne, (ne_resid){2, NULL}, (ne_resid){0, "hello"}, &r), NE_OK);
    CHECK(r && r->offset == 0x240);
    CHECK_ERR(ne_find_resource(&ne, (ne_resid){0, "mytype"}, (ne_resid){7, NULL}, &r), NE_OK);
    CHECK(r && r->offset == 0x300);
    CHECK_ERR(ne_find_resource(&ne, (ne_resid){2, NULL}, (ne_resid){7, NULL}, &r), NE_ERR_NOT_FOUND);
    CHECK_ERR(ne_find_resource(&ne, (ne_resid){3, NULL}, (ne_resid){1, NULL}, &r), NE_ERR_NOT_FOUND);
    CHECK_ERR(ne_find_resource(&ne, (ne_resid){2, NULL}, (ne_resid){0, "nope"}, &r), NE_ERR_NOT_FOUND);
    CHECK_ERR(ne_find_resource(&ne, (ne_resid){2, NULL}, (ne_resid){1, NULL}, NULL), NE_ERR_ARG);
    ne_free(&ne);

    /* a module without a resource table */
    fx_build(f);
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK(ne.restype_count == 0);
    CHECK_ERR(ne_find_resource(&ne, (ne_resid){2, NULL}, (ne_resid){1, NULL}, &r), NE_ERR_NOT_FOUND);
    ne_free(&ne);
    free(f);
}

static void test_resource_rejects(void) {
    fx_t *f = malloc(sizeof *f);
    ne_image ne;
    printf("test_resource_rejects\n");
    rs_build(f);
    p16(f, NE_AT + RS_TAB + 2 + 2, 200); /* count runs past the table */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_TABLE);
    rs_build(f);
    p16(f, NE_AT + RS_TAB, 20); /* shift too large */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_TABLE);
    rs_build(f);
    p16(f, NE_AT + RS_TAB + 2 + 8, 0xf000); /* first resource beyond EOF */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_OFFSET);
    rs_build(f);
    p16(f, NE_AT + RS_TAB + 2, 0x0fff); /* type name offset outside the table */
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_ERR_BAD_TABLE);
    rs_build(f);
    f->n = 0x310; /* last resource ends at 0x320: clamped to EOF, not rejected */
    p32(f, NE_AT + 0x2c, 0x300);
    CHECK_ERR(ne_open_buffer(&ne, f->b, f->n), NE_OK);
    CHECK(ne.restype_count == 2 && ne.restypes[1].res[0].length == 0x10);
    ne_free(&ne);
    free(f);
}

static void test_nedump_synthetic(void) {
    fx_t *f = malloc(sizeof *f);
    FILE *fp;
    char out[8192];
    size_t len = 0;
    const char *path = "build/synthetic-dump.exe";
    printf("test_nedump_synthetic\n");
    fx_build(f);
    fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (fp) {
        FILE *pp;
        fwrite(f->b, 1, f->n, fp);
        fclose(fp);
        pp = popen("build/nedump build/synthetic-dump.exe 2>&1", "r");
        CHECK(pp != NULL);
        if (pp) {
            len = fread(out, 1, sizeof out - 1, pp);
            CHECK(pclose(pp) == 0);
        }
        out[len] = '\0';
        CHECK(strstr(out, "Module: TESTMOD") != NULL);
        CHECK(strstr(out, "CS:IP = 1:0x0010") != NULL);
        CHECK(strstr(out, "Segments: 4") != NULL);
        CHECK(strstr(out, "Resident names: 3") != NULL);
        CHECK(strstr(out, "Non-resident names: 2") != NULL);
        CHECK(strstr(out, "Entry table: 5 entries") != NULL);
        CHECK(strstr(out, "Module references: 2") != NULL);
        CHECK(strstr(out, "KERNEL.5") != NULL);
        CHECK(strstr(out, "USER.WinFunc") != NULL);
        CHECK(strstr(out, "relocs 5") != NULL);
        remove(path);
    }
    CHECK(system("build/nedump build/does-not-exist.exe >/dev/null 2>&1") != 0);
    free(f);
}

/* ---- real binaries (opt-in via environment) ---------------------------- */

static const char *const WRITER_MODULES[] = {"COMMDLG", "GDI", "KERNEL", "KEYBOARD", "KIDSWTCH",
                                             "MMSYSTEM", "TOOLHELP", "USER", "WIN87EM"};

static size_t count_resources(const ne_image *ne) {
    size_t i, total = 0, j;
    for (i = 0; i < ne->restype_count; i++) {
        for (j = 0; j < ne->restypes[i].count; j++) {
            const ne_resource *r = &ne->restypes[i].res[j];
            CHECK((uint64_t)r->offset + r->length <= ne->size);
            total++;
        }
    }
    return total;
}

static void test_real_writer(void) {
    const char *path = getenv("CWFA_WRITER_EXE");
    ne_image ne;
    ne_import *imp = NULL;
    size_t n = 0, i, total = 0;
    unsigned s, j;
    char name[NE_NAME_MAX + 1];
    int mod_used[9] = {0};
    if (!path || !*path) {
        printf("test_real_writer: SKIP (CWFA_WRITER_EXE not set)\n");
        return;
    }
    printf("test_real_writer\n");
    CHECK_ERR(ne_open_file(&ne, path), NE_OK);
    if (g_fails && !ne.data) return;
    CHECK(ne.cs == 5 && ne.ip == 0x1a);
    CHECK(ne.ss == 0xb0 && ne.sp == 0 && ne.auto_data_seg == 0xb0);
    CHECK(ne.heap_size == 0x400 && ne.stack_size == 0x2800);
    CHECK(ne.win_version == 0x030a);
    CHECK(ne.seg_count == 176);
    CHECK(ne.modref_count == 9);
    for (i = 0; i < 9; i++) {
        CHECK_ERR(ne_modref_name(&ne, (unsigned)i + 1, name), NE_OK);
    }
    CHECK_ERR(ne_collect_imports(&ne, &imp, &n), NE_OK);
    for (i = 0; i < n; i++) {
        size_t k;
        CHECK_ERR(ne_modref_name(&ne, imp[i].modref, name), NE_OK);
        for (k = 0; k < 9; k++)
            if (strcmp(name, WRITER_MODULES[k]) == 0) mod_used[k] = 1;
    }
    for (i = 0; i < 9; i++) CHECK(mod_used[i]);
    for (i = 1; i <= 9; i++) {
        size_t k;
        int found = 0;
        ne_modref_name(&ne, (unsigned)i, name);
        for (k = 0; k < 9; k++) found |= strcmp(name, WRITER_MODULES[k]) == 0;
        CHECK(found);
    }
    CHECK(n == 318);
    CHECK(count_resources(&ne) == 317); /* independently counted from the table */
    ne_free_imports(imp);
    for (s = 0; s < ne.seg_count; s++) {
        for (j = 0; j < ne.segs[s].reloc_count; j++) {
            const ne_reloc *r = &ne.segs[s].relocs[j];
            size_t cnt = 0;
            total++;
            CHECK_ERR(ne_reloc_sites(&ne, s, r, NULL, 0, &cnt), NE_OK);
        }
    }
    CHECK(total > 318);
    for (i = 0; i < ne.entry_count; i++)
        CHECK(ne.entries[i].kind == NE_ENTRY_UNUSED || ne.entries[i].segment <= ne.seg_count);
    printf("  writer: %u segments, %zu relocations, %zu distinct imports, %zu entries\n",
           ne.seg_count, total, n, ne.entry_count);
    ne_free(&ne);
}

static void test_real_artist(void) {
    const char *path = getenv("CWFA_ARTIST_EXE");
    ne_image ne;
    if (!path || !*path) {
        printf("test_real_artist: SKIP (CWFA_ARTIST_EXE not set)\n");
        return;
    }
    printf("test_real_artist\n");
    CHECK_ERR(ne_open_file(&ne, path), NE_OK);
    CHECK(ne.linker_major == 13 && ne.linker_minor == 6);
    CHECK(count_resources(&ne) == 596);
    ne_free(&ne);
}

int main(void) {
    test_header();
    test_align_shift_zero_means_9();
    test_header_rejects();
    test_strerror();
    test_open_file();
    test_segments();
    test_segments_reject();
    test_names();
    test_modrefs_and_imported_names();
    test_entry_table();
    test_relocations();
    test_relocation_sites();
    test_relocation_rejects();
    test_collect_imports();
    test_resources();
    test_resource_rejects();
    test_nedump_synthetic();
    test_real_writer();
    test_real_artist();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
