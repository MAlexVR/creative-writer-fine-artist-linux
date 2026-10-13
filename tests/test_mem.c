#include <stdio.h>
#include <string.h>

#include "mem.h"

static int g_checks, g_fails;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_fails++;                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define CHECK_EQ(got, want)                                                  \
    do {                                                                     \
        unsigned long long g_ = (unsigned long long)(got);                   \
        unsigned long long w_ = (unsigned long long)(want);                  \
        g_checks++;                                                          \
        if (g_ != w_) {                                                      \
            g_fails++;                                                       \
            printf("  FAIL %s:%d: %s = 0x%llx, want 0x%llx\n", __FILE__,     \
                   __LINE__, #got, g_, w_);                                  \
        }                                                                    \
    } while (0)

static mem_t g_mem;
static cpu_mem g_if;

static void fresh(void) {
    mem_destroy(&g_mem);
    mem_init(&g_mem);
    mem_cpu_iface(&g_mem, &g_if);
}

static cpu_status rd8(uint16_t sel, uint32_t off, uint8_t *v) { return g_if.read8(g_if.ctx, sel, off, v); }
static cpu_status wr8(uint16_t sel, uint32_t off, uint8_t v) { return g_if.write8(g_if.ctx, sel, off, v); }

static void test_alloc_basic(void) {
    uint16_t a, b;
    uint8_t v = 0xEE;
    printf("test_alloc_basic\n");
    fresh();
    a = mem_alloc(&g_mem, 0x100, MEM_DATA);
    b = mem_alloc(&g_mem, 0x10, MEM_DATA);
    CHECK(a != 0 && b != 0 && a != b);
    CHECK_EQ(a & 7, 7); CHECK_EQ(b & 7, 7);          /* LDT, RPL 3 */
    CHECK(a >> 3 != 0);                              /* index 0 is never issued */
    CHECK(mem_valid(&g_mem, a));
    CHECK_EQ(mem_size(&g_mem, a), 0x100); CHECK_EQ(mem_size(&g_mem, b), 0x10);
    CHECK_EQ(rd8(a, 0x55, &v), CPU_OK); CHECK_EQ(v, 0);   /* zero-filled */
    CHECK_EQ(wr8(a, 0x55, 0x77), CPU_OK);
    CHECK_EQ(rd8(a, 0x55, &v), CPU_OK); CHECK_EQ(v, 0x77);
    CHECK_EQ(rd8(b, 0x55, &v), CPU_E_LIMIT);              /* b is only 0x10 bytes */
    CHECK(mem_alloc(&g_mem, 0, MEM_DATA) == 0);
    CHECK(mem_alloc(&g_mem, 0x10001, MEM_DATA) == 0);
    a = mem_alloc(&g_mem, 0x10000, MEM_DATA);
    CHECK(a != 0); CHECK_EQ(mem_size(&g_mem, a), 0x10000);
    CHECK_EQ(wr8(a, 0xFFFF, 1), CPU_OK); CHECK_EQ(wr8(a, 0x10000, 1), CPU_E_LIMIT);
}

static void test_limit_faults(void) {
    uint16_t a;
    uint8_t v8;
    uint16_t v16;
    uint32_t v32;
    printf("test_limit_faults\n");
    fresh();
    a = mem_alloc(&g_mem, 8, MEM_DATA);
    CHECK_EQ(g_if.write32(g_if.ctx, a, 4, 0x11223344), CPU_OK);
    CHECK_EQ(g_if.read16(g_if.ctx, a, 4, &v16), CPU_OK); CHECK_EQ(v16, 0x3344);
    CHECK_EQ(g_if.read32(g_if.ctx, a, 4, &v32), CPU_OK); CHECK_EQ(v32, 0x11223344);
    CHECK_EQ(rd8(a, 7, &v8), CPU_OK); CHECK_EQ(v8, 0x11);
    /* a straddling access faults and writes nothing */
    CHECK_EQ(g_if.write32(g_if.ctx, a, 6, 0xAABBCCDD), CPU_E_LIMIT);
    CHECK_EQ(g_if.write16(g_if.ctx, a, 7, 0xAABB), CPU_E_LIMIT);
    CHECK_EQ(rd8(a, 6, &v8), CPU_OK); CHECK_EQ(v8, 0x22);
    CHECK_EQ(g_if.read32(g_if.ctx, a, 5, &v32), CPU_E_LIMIT);
    CHECK_EQ(g_if.read16(g_if.ctx, a, 0xFFFFFFFFu, &v16), CPU_E_LIMIT); /* offset overflow */
    CHECK_EQ(g_if.read32(g_if.ctx, a, 0xFFFFFFFEu, &v32), CPU_E_LIMIT);
    CHECK_EQ(rd8(a, 8, &v8), CPU_E_LIMIT);
    CHECK(mem_ptr(&g_mem, a, 0, 8) != NULL); CHECK(mem_ptr(&g_mem, a, 4, 4) != NULL);
    CHECK(mem_ptr(&g_mem, a, 4, 5) == NULL); CHECK(mem_ptr(&g_mem, a, 8, 1) == NULL);
    CHECK(mem_ptr(&g_mem, a, 0xFFFFFFFFu, 2) == NULL);
}

static void test_invalid_selectors(void) {
    uint16_t a;
    uint8_t v;
    cpu_selinfo si;
    printf("test_invalid_selectors\n");
    fresh();
    a = mem_alloc(&g_mem, 16, MEM_DATA);
    CHECK_EQ(rd8(0, 0, &v), CPU_E_SEL);
    CHECK_EQ(rd8(0x0007, 0, &v), CPU_E_SEL);           /* LDT index 0 */
    CHECK_EQ(rd8((uint16_t)(a & ~4), 0, &v), CPU_E_SEL); /* GDT selector, same index */
    CHECK_EQ(rd8(0xFFFF, 0, &v), CPU_E_SEL);           /* last index, unallocated */
    CHECK_EQ(wr8(0x1237, 0, 1), CPU_E_SEL);
    CHECK(!mem_valid(&g_mem, 0)); CHECK(!mem_valid(&g_mem, 0x0007));
    CHECK(!mem_valid(&g_mem, (uint16_t)(a + 8)));
    CHECK(!g_if.sel_valid(g_if.ctx, 0)); CHECK(g_if.sel_valid(g_if.ctx, a));
    CHECK(!mem_info(&g_mem, 0, &si)); CHECK(mem_ptr(&g_mem, 0, 0, 1) == NULL);
    /* the RPL bits of a valid selector do not matter */
    CHECK_EQ(rd8((uint16_t)(a & ~3), 0, &v), CPU_OK);
    CHECK(!mem_free(&g_mem, 0)); CHECK(!mem_free(&g_mem, (uint16_t)(a + 8)));
}

static void test_free_and_exhaustion(void) {
    uint16_t a, b, first = 0, last = 0;
    unsigned n = 0;
    uint8_t v;
    printf("test_free_and_exhaustion\n");
    fresh();
    a = mem_alloc(&g_mem, 32, MEM_DATA);
    CHECK(mem_free(&g_mem, a));
    CHECK(!mem_valid(&g_mem, a)); CHECK_EQ(rd8(a, 0, &v), CPU_E_SEL);
    CHECK(!mem_free(&g_mem, a)); /* double free */
    b = mem_alloc(&g_mem, 32, MEM_DATA);
    CHECK(b != 0);
    CHECK_EQ(rd8(b, 0, &v), CPU_OK); CHECK_EQ(v, 0); /* recycled slots are zeroed */
    fresh();
    for (;;) {
        uint16_t s = mem_alloc(&g_mem, 1, MEM_DATA);
        if (!s) break;
        if (!first) first = s;
        last = s;
        n++;
    }
    CHECK_EQ(n, MEM_LDT_ENTRIES - 1);
    CHECK(mem_free(&g_mem, last));
    b = mem_alloc(&g_mem, 1, MEM_DATA);
    CHECK_EQ(b, last);
    CHECK(mem_alloc(&g_mem, 1, MEM_DATA) == 0);
    CHECK(mem_free(&g_mem, first));
    CHECK(mem_alloc(&g_mem, 1, MEM_DATA) == first);
}

static void test_resize(void) {
    uint16_t a, al;
    uint8_t v;
    printf("test_resize\n");
    fresh();
    a = mem_alloc(&g_mem, 16, MEM_DATA);
    wr8(a, 3, 0x42);
    CHECK_EQ(rd8(a, 16, &v), CPU_E_LIMIT);
    CHECK(mem_resize(&g_mem, a, 0x400));
    CHECK_EQ(mem_size(&g_mem, a), 0x400);
    CHECK_EQ(rd8(a, 3, &v), CPU_OK); CHECK_EQ(v, 0x42);        /* contents preserved */
    CHECK_EQ(rd8(a, 0x3FF, &v), CPU_OK); CHECK_EQ(v, 0);       /* new bytes zeroed */
    CHECK_EQ(rd8(a, 0x400, &v), CPU_E_LIMIT);
    CHECK(mem_resize(&g_mem, a, 8));
    CHECK_EQ(rd8(a, 8, &v), CPU_E_LIMIT); CHECK_EQ(rd8(a, 3, &v), CPU_OK);
    CHECK(mem_resize(&g_mem, a, 0x400));
    CHECK_EQ(rd8(a, 0x100, &v), CPU_OK); CHECK_EQ(v, 0);       /* regrown bytes are zero again */
    CHECK(mem_resize(&g_mem, a, 0x10000)); CHECK(!mem_resize(&g_mem, a, 0x10001));
    CHECK(!mem_resize(&g_mem, a, 0)); CHECK(!mem_resize(&g_mem, 0, 8));
    al = mem_alias(&g_mem, a, MEM_CODE);
    CHECK(!mem_resize(&g_mem, a, 8));                          /* aliased: refused */
    mem_free(&g_mem, al);
    CHECK(mem_resize(&g_mem, a, 8));                           /* alone again */
}

static void test_alias(void) {
    uint16_t d, c, d2;
    uint8_t v;
    cpu_selinfo si;
    printf("test_alias\n");
    fresh();
    d = mem_alloc(&g_mem, 0x40, MEM_DATA);
    wr8(d, 5, 0x99);
    c = mem_alias(&g_mem, d, MEM_CODE);
    CHECK(c != 0 && c != d);
    CHECK_EQ(mem_size(&g_mem, c), 0x40);
    CHECK_EQ(rd8(c, 5, &v), CPU_OK); CHECK_EQ(v, 0x99);          /* same memory */
    CHECK_EQ(wr8(c, 5, 1), CPU_E_SEL);                           /* code is not writable */
    wr8(d, 5, 0x11);
    CHECK_EQ(rd8(c, 5, &v), CPU_OK); CHECK_EQ(v, 0x11);
    CHECK(mem_info(&g_mem, c, &si)); CHECK_EQ(si.access, MEM_ACC_CODE); CHECK_EQ(si.limit, 0x3F);
    CHECK(mem_info(&g_mem, d, &si)); CHECK_EQ(si.access, MEM_ACC_DATA);
    d2 = mem_alias(&g_mem, c, MEM_DATA);                         /* alias of an alias */
    CHECK_EQ(wr8(d2, 6, 0x33), CPU_OK); CHECK_EQ(rd8(d, 6, &v), CPU_OK); CHECK_EQ(v, 0x33);
    CHECK(mem_free(&g_mem, d));                                  /* backing outlives the original */
    CHECK_EQ(rd8(c, 6, &v), CPU_OK); CHECK_EQ(v, 0x33);
    CHECK(mem_free(&g_mem, c)); CHECK(mem_free(&g_mem, d2));
    CHECK(mem_alias(&g_mem, d, MEM_CODE) == 0);                  /* freed selector */
    CHECK(mem_alias(&g_mem, 0, MEM_CODE) == 0);
}

static void test_kinds_and_present(void) {
    uint16_t ro, code, d;
    cpu_selinfo si;
    uint8_t v;
    printf("test_kinds_and_present\n");
    fresh();
    ro = mem_alloc(&g_mem, 16, MEM_RODATA);
    code = mem_alloc(&g_mem, 16, MEM_CODE);
    d = mem_alloc(&g_mem, 16, MEM_DATA);
    CHECK(mem_info(&g_mem, ro, &si)); CHECK_EQ(si.access, MEM_ACC_RODATA);
    CHECK_EQ(wr8(ro, 0, 1), CPU_E_SEL); CHECK_EQ(rd8(ro, 0, &v), CPU_OK);
    CHECK_EQ(wr8(code, 0, 1), CPU_E_SEL);
    CHECK(mem_ptr(&g_mem, ro, 0, 1) != NULL); /* host access bypasses read-only (loader) */
    CHECK(mem_set_present(&g_mem, d, false));
    CHECK(!mem_valid(&g_mem, d)); CHECK_EQ(rd8(d, 0, &v), CPU_E_SEL);
    CHECK(mem_info(&g_mem, d, &si)); CHECK_EQ(si.access & 0x80, 0); /* LAR still sees it */
    CHECK(mem_ptr(&g_mem, d, 0, 1) == NULL);
    CHECK(mem_set_present(&g_mem, d, true)); CHECK_EQ(rd8(d, 0, &v), CPU_OK);
    CHECK(!mem_set_present(&g_mem, 0, true));
}

static void test_huge(void) {
    uint16_t h, s0, s3, x;
    uint8_t v;
    uint8_t *p;
    cpu_selinfo si;
    unsigned i;
    printf("test_huge\n");
    fresh();
    h = mem_alloc_huge(&g_mem, 0x30000u + 0x100, MEM_DATA);
    CHECK(h != 0);
    for (i = 0; i < 4; i++) {
        CHECK_EQ(mem_huge_sel(h, i), h + i * MEM_AHINCR);
        CHECK(mem_valid(&g_mem, mem_huge_sel(h, i)));
        CHECK(mem_info(&g_mem, mem_huge_sel(h, i), &si));
        CHECK_EQ(si.limit, 0x300FFu + 0 - i * 0x10000u);   /* to the end of the block, not 64K: 386 code uses 32-bit offsets */
    }
    CHECK(!mem_valid(&g_mem, mem_huge_sel(h, 4)));
    s0 = h; s3 = mem_huge_sel(h, 3);
    CHECK_EQ(wr8(s3, 0, 0xAB), CPU_OK);
    CHECK_EQ(wr8(s3, 0xFF, 0xCD), CPU_OK); CHECK_EQ(wr8(s3, 0x100, 1), CPU_E_LIMIT);
    CHECK_EQ(wr8(s0, 0x10000, 1), CPU_OK); CHECK_EQ(wr8(s0, 0x30100, 1), CPU_E_LIMIT);   /* 32-bit offset past 64K */
    p = mem_ptr(&g_mem, s0, 0, 1);
    CHECK(p != NULL && p[0x30000] == 0xAB && p[0x300FF] == 0xCD); /* one contiguous block */
    CHECK_EQ(wr8(mem_huge_sel(h, 1), 0xFFFF, 0x5A), CPU_OK);
    CHECK_EQ(rd8(s0, 0xFFFF, &v), CPU_OK); CHECK_EQ(v, 0);
    CHECK_EQ(p[0x1FFFF], 0x5A);
    CHECK(!mem_resize(&g_mem, h, 8));
    CHECK(mem_free(&g_mem, h));                                   /* frees the whole group */
    for (i = 0; i < 4; i++) CHECK(!mem_valid(&g_mem, mem_huge_sel(h, i)));
    /* exact multiple: the last selector spans exactly 64K */
    h = mem_alloc_huge(&g_mem, 0x20000, MEM_DATA);
    CHECK(mem_info(&g_mem, mem_huge_sel(h, 1), &si)); CHECK_EQ(si.limit, 0xFFFF);
    CHECK(!mem_valid(&g_mem, mem_huge_sel(h, 2)));
    mem_free(&g_mem, h);
    /* small sizes degenerate to a plain segment; absurd ones fail */
    h = mem_alloc_huge(&g_mem, 0x200, MEM_DATA);
    CHECK(h != 0); CHECK_EQ(mem_size(&g_mem, h), 0x200);
    CHECK(mem_alloc_huge(&g_mem, 0, MEM_DATA) == 0);
    CHECK(mem_alloc_huge(&g_mem, MEM_HUGE_MAX + 1, MEM_DATA) == 0);
    /* selectors must be consecutive: a fragmented LDT cannot serve it */
    fresh();
    {
        uint16_t keep[40];
        for (i = 0; i < MEM_LDT_ENTRIES - 1; i++) {
            x = mem_alloc(&g_mem, 1, MEM_DATA);
            if (i < 40) keep[i] = x;
        }
        /* free every other of the first 40 -> no 2-run available */
        for (i = 0; i < 40; i += 2) mem_free(&g_mem, keep[i]);
        CHECK(mem_alloc_huge(&g_mem, 0x20000, MEM_DATA) == 0);
        mem_free(&g_mem, keep[1]);          /* keep[0..2] now a free run of 3 */
        h = mem_alloc_huge(&g_mem, 0x30000, MEM_DATA);
        CHECK_EQ(h, keep[0]);
        CHECK_EQ(mem_huge_sel(h, 2), keep[2]);
    }
}

static void test_wide_access(void) {
    uint16_t a;
    uint16_t v16;
    uint32_t v32;
    printf("test_wide_access\n");
    fresh();
    a = mem_alloc(&g_mem, 0x20, MEM_DATA);
    CHECK_EQ(g_if.write16(g_if.ctx, a, 2, 0xBEEF), CPU_OK);
    CHECK_EQ(g_if.write32(g_if.ctx, a, 8, 0xCAFEBABEu), CPU_OK);
    CHECK_EQ(g_if.read16(g_if.ctx, a, 8, &v16), CPU_OK); CHECK_EQ(v16, 0xBABE);
    CHECK_EQ(g_if.read32(g_if.ctx, a, 2, &v32), CPU_OK); CHECK_EQ(v32, 0xBEEF);
    CHECK_EQ(g_if.read16(g_if.ctx, 0, 0, &v16), CPU_E_SEL);
    CHECK_EQ(g_if.write32(g_if.ctx, 0x1237, 0, 1), CPU_E_SEL);
}

int main(void) {
    mem_init(&g_mem);
    test_alloc_basic();
    test_limit_faults();
    test_invalid_selectors();
    test_free_and_exhaustion();
    test_resize();
    test_alias();
    test_kinds_and_present();
    test_huge();
    test_wide_access();
    mem_destroy(&g_mem);
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
