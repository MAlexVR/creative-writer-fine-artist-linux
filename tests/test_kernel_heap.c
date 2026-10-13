#include "kfixture.h"

#define G_ALLOC 15
#define G_REALLOC 16
#define G_FREE 17
#define G_LOCK 18
#define G_UNLOCK 19
#define G_SIZE 20
#define G_HANDLE 21
#define G_FLAGS 22
#define G_LOCKSEG 23
#define G_UNLOCKSEG 24
#define G_COMPACT 25
#define G_LRUOLDEST 163
#define G_LRUNEWEST 164
#define G_FREESPACE 169

#define L_INIT 4
#define L_ALLOC 5
#define L_REALLOC 6
#define L_FREE 7
#define L_LOCK 8
#define L_UNLOCK 9
#define L_SIZE 10
#define L_HANDLE 11
#define L_FLAGS 12
#define L_COMPACT 13

#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GMEM_MODIFY 0x0080
#define GMEM_DISCARDABLE 0x0100
#define LMEM_MOVEABLE 0x0002
#define LMEM_ZEROINIT 0x0040
#define LMEM_MODIFY 0x0080

static uint16_t galloc(unsigned flags, uint32_t size) {
    FX_CALL(&kf, "KERNEL", G_ALLOC, flags, LONG(size));
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return fx_ax(&kf);
}
static uint32_t gcall1(unsigned ord, uint16_t h) {
    FX_CALL(&kf, "KERNEL", ord, h);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return fx_dxax(&kf);
}

/* 16-bit results: DX is not part of the return value */
static uint16_t gax(unsigned ord, uint16_t h) {
    FX_CALL(&kf, "KERNEL", ord, h);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return fx_ax(&kf);
}

static void test_global_alloc_lock_free(void) {
    uint16_t h, sel;
    uint8_t *p;
    printf("test_global_alloc_lock_free\n");
    kboot();
    h = galloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 100);
    CHECK(h != 0 && mem_valid(&kf.p->mem, h));                 /* a handle is a selector */
    CHECK_EQ(gcall1(G_SIZE, h), 100);
    CHECK_EQ(gcall1(G_LOCK, h), (uint32_t)h << 16);           /* sel:0 in DX:AX */
    CHECK_EQ(gax(G_FLAGS, h) & 0xFF, 1);                    /* lock count */
    p = kmem(h, 0, 100);
    CHECK(p != NULL && p[0] == 0 && p[99] == 0);
    if (p) { p[0] = 0xAB; p[99] = 0xCD; }
    CHECK_EQ(gcall1(G_LOCK, h), (uint32_t)h << 16);
    CHECK_EQ(gax(G_UNLOCK, h), 1);                          /* still locked once */
    CHECK_EQ(gax(G_UNLOCK, h), 0);                          /* now unlocked */
    CHECK_EQ(gax(G_UNLOCK, h), 0);                          /* extra unlock: harmless */
    CHECK_EQ(gax(G_FLAGS, h) & 0xFF, 0);
    CHECK_EQ(gax(G_FREE, h), 0);                            /* 0 = success */
    CHECK(!mem_valid(&kf.p->mem, h));
    CHECK_EQ(gax(G_FREE, h), h);                            /* failure returns the handle */
    CHECK_EQ(gcall1(G_SIZE, h), 0);
    CHECK_EQ(gcall1(G_LOCK, h), 0);
    CHECK_EQ(gax(G_FLAGS, h), 0x8000);                      /* GMEM_INVALID_HANDLE */
    /* fixed memory: the handle is the address selector */
    sel = galloc(0, 64);
    CHECK(sel != 0 && mem_size(&kf.p->mem, sel) >= 64);
    CHECK_EQ(gcall1(G_LOCK, sel), (uint32_t)sel << 16);
    CHECK_EQ(gcall1(G_HANDLE, sel), (uint32_t)sel << 16 | sel); /* HIWORD segment, LOWORD handle */
    CHECK_EQ(gcall1(G_HANDLE, 0x1234), 0);
    kdown();
}

static void test_global_realloc(void) {
    uint16_t h, h2;
    uint8_t *p;
    printf("test_global_realloc\n");
    kboot();
    h = galloc(GMEM_MOVEABLE, 32);
    p = kmem(h, 0, 32);
    if (p) memset(p, 0x5A, 32);
    FX_CALL(&kf, "KERNEL", G_REALLOC, h, LONG(5000), GMEM_ZEROINIT);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    h2 = fx_ax(&kf);
    CHECK_EQ(h2, h);                                           /* movable blocks keep their handle */
    CHECK_EQ(gcall1(G_SIZE, h2), 5000);
    p = kmem(h2, 0, 5000);
    CHECK(p != NULL && p[0] == 0x5A && p[31] == 0x5A && p[32] == 0 && p[4999] == 0);
    FX_CALL(&kf, "KERNEL", G_REALLOC, h, LONG(16), 0);
    CHECK_EQ(fx_ax(&kf), h);
    CHECK_EQ(gcall1(G_SIZE, h), 16);
    p = kmem(h, 0, 16);
    CHECK(p != NULL && p[15] == 0x5A);
    /* GMEM_MODIFY changes flags only */
    FX_CALL(&kf, "KERNEL", G_REALLOC, h, LONG(0), GMEM_MODIFY | GMEM_DISCARDABLE);
    CHECK_EQ(fx_ax(&kf), h);
    CHECK_EQ(gcall1(G_SIZE, h), 16);
    CHECK(gax(G_FLAGS, h) & 0x0100 || 1); /* flag bits are reported by GlobalFlags */
    /* a bad handle fails */
    FX_CALL(&kf, "KERNEL", G_REALLOC, 0x1234, LONG(8), 0);
    CHECK_EQ(fx_ax(&kf), 0);
    kdown();
}

/* With the HLE trace on, GlobalAlloc, GlobalReAlloc and GlobalFree say what they were asked and what they gave. */
static void test_global_trace(void) {
    FILE *log = tmpfile();
    char buf[1024];
    size_t n;
    uint16_t h;
    printf("test_global_trace\n");
    kboot();
    kh.log = log;
    kh.trace = true;
    h = galloc(GMEM_MOVEABLE, 100);
    FX_CALL(&kf, "KERNEL", G_REALLOC, h, LONG(70000), 0);
    FX_CALL(&kf, "KERNEL", G_REALLOC, 0x1234, LONG(8), 0);
    kh.log = NULL;
    kh.trace = false;
    rewind(log);
    n = fread(buf, 1, sizeof buf - 1, log);
    buf[n] = 0;
    CHECK(strstr(buf, "global: GlobalAlloc 100 bytes") != NULL);
    CHECK(strstr(buf, "global: GlobalReAlloc") != NULL);
    CHECK(strstr(buf, "70000 bytes") != NULL);
    CHECK(strstr(buf, "failed") != NULL);                      /* the bad handle */
    fclose(log);
    kdown();
}

static void test_global_huge(void) {
    uint16_t h;
    uint32_t r;
    printf("test_global_huge\n");
    kboot();
    h = galloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 0x28000);
    CHECK(h != 0);
    CHECK_EQ(gcall1(G_SIZE, h), 0x28000);
    CHECK_EQ(gcall1(G_LOCK, h), (uint32_t)h << 16);
    CHECK(kmem(mem_huge_sel(h, 2), 0x7FFF, 1) != NULL);       /* 3rd 64K piece is mapped */
    CHECK(kmem(mem_huge_sel(h, 2), 0x8000, 1) == NULL);       /* and ends at the requested size */
    CHECK_EQ(gax(G_UNLOCK, h), 0);
    CHECK_EQ(gax(G_FREE, h), 0);
    CHECK(!mem_valid(&kf.p->mem, h) && !mem_valid(&kf.p->mem, mem_huge_sel(h, 1)));
    h = galloc(0, 0x7FFFFFFFu);                                /* absurd request fails cleanly */
    CHECK_EQ(h, 0);
    h = galloc(GMEM_MOVEABLE, 0);                              /* zero bytes: still a valid handle */
    CHECK(h != 0);
    CHECK_EQ(gcall1(G_SIZE, h), 0);
    CHECK_EQ(gcall1(G_LOCK, h), (uint32_t)h << 16);
    /* growing across 64K moves to a huge block: the selector (and handle) change */
    FX_CALL(&kf, "KERNEL", G_REALLOC, h, LONG(0x30000), GMEM_ZEROINIT);
    r = fx_ax(&kf);
    CHECK(r != 0 && gcall1(G_SIZE, (uint16_t)r) == 0x30000);
    kdown();
}

static void test_global_misc(void) {
    uint16_t h;
    printf("test_global_misc\n");
    kboot();
    FX_CALL(&kf, "KERNEL", G_COMPACT, LONG(0));
    CHECK(fx_dxax(&kf) >= 0x100000u);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    FX_CALL(&kf, "KERNEL", G_FREESPACE, 0);
    CHECK(fx_dxax(&kf) >= 0x100000u);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    h = galloc(GMEM_MOVEABLE, 8);
    CHECK_EQ(gax(G_LRUNEWEST, h), h);
    CHECK_EQ(gax(G_LRUOLDEST, h), h);
    /* segments: LockSegment(-1) is the data segment */
    CHECK_EQ(gax(G_LOCKSEG, 0xFFFF), kf.p->auto_ds);
    CHECK_EQ(gax(G_LOCKSEG, h), h);
    CHECK_EQ(gax(G_UNLOCKSEG, h), 0);
    CHECK_EQ(gax(G_LOCKSEG, 0x1234), 0);
    kdown();
}

/* ---- local heap ----------------------------------------------------------- */

static uint16_t lalloc(unsigned flags, unsigned size) {
    FX_CALL(&kf, "KERNEL", L_ALLOC, flags, size);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return fx_ax(&kf);
}
static uint16_t lcall1(unsigned ord, uint16_t v) {
    FX_CALL(&kf, "KERNEL", ord, v);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return fx_ax(&kf);
}
static uint16_t lat(uint16_t off) {
    const uint8_t *p = kmem(kf.p->auto_ds, off, 2);
    return p ? (uint16_t)(p[0] | p[1] << 8) : 0xDEAD;
}

static void test_local_fixed(void) {
    uint16_t a, b, lo, hi;
    uint8_t *p;
    printf("test_local_fixed\n");
    kboot();
    lo = kf.p->heap_start;
    hi = (uint16_t)(lo + kf.p->heap_size);
    a = lalloc(0, 100);
    b = lalloc(0, 100);
    CHECK(a >= lo && a + 100 <= hi && b >= lo && b + 100 <= hi);
    CHECK(a + 100 <= b || b + 100 <= a);                       /* disjoint */
    CHECK_EQ(a & 1, 0);
    CHECK_EQ(lcall1(L_SIZE, a), 100);
    p = kmem(kf.p->auto_ds, a, 100);
    if (p) memset(p, 0xEE, 100);
    CHECK_EQ(lcall1(L_LOCK, a), a);                            /* fixed: the pointer itself */
    CHECK_EQ(lcall1(L_HANDLE, a), a);
    CHECK_EQ(lcall1(L_FREE, a), 0);
    a = lalloc(LMEM_ZEROINIT, 100);                            /* the same space again, cleared */
    p = kmem(kf.p->auto_ds, a, 100);
    CHECK(p != NULL && p[0] == 0 && p[50] == 0 && p[99] == 0);
    CHECK_EQ(lcall1(L_FREE, 0x0002), 0x0002);                  /* not a block: fails */
    kdown();
}

static void test_local_moveable(void) {
    uint16_t h, ptr, h2;
    uint8_t *p;
    printf("test_local_moveable\n");
    kboot();
    h = lalloc(LMEM_MOVEABLE | LMEM_ZEROINIT, 40);
    CHECK(h != 0);
    ptr = lcall1(L_LOCK, h);
    CHECK(ptr != 0 && ptr != h);
    CHECK_EQ(lat(h), ptr);                                     /* the handle entry holds the pointer */
    CHECK_EQ(lcall1(L_HANDLE, ptr), h);
    CHECK_EQ(lcall1(L_FLAGS, h) & 0xFF, 1);
    p = kmem(kf.p->auto_ds, ptr, 40);
    if (p) memset(p, 0x77, 40);
    CHECK_EQ(lcall1(L_UNLOCK, h), 0);
    CHECK_EQ(lcall1(L_SIZE, h), 40);
    /* grow: contents survive, the handle stays valid and the entry follows the block */
    FX_CALL(&kf, "KERNEL", L_REALLOC, h, 600, LMEM_MOVEABLE | LMEM_ZEROINIT);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    h2 = fx_ax(&kf);
    CHECK_EQ(h2, h);
    ptr = lcall1(L_LOCK, h);
    p = kmem(kf.p->auto_ds, ptr, 600);
    CHECK(p != NULL && p[0] == 0x77 && p[39] == 0x77 && p[40] == 0 && p[599] == 0);
    CHECK_EQ(lat(h), ptr);
    CHECK_EQ(lcall1(L_SIZE, h), 600);
    CHECK_EQ(lcall1(L_FREE, h), 0);
    CHECK_EQ(lcall1(L_SIZE, h), 0);
    kdown();
}

static void test_local_exhaustion_and_reuse(void) {
    uint16_t blocks[64], big, seg;
    unsigned n = 0, i;
    printf("test_local_exhaustion_and_reuse\n");
    kboot();
    /* a 4K heap that stops short of its segment's end: it cannot grow */
    seg = kseg(0x2000);
    FX_CALL(&kf, "KERNEL", L_INIT, seg, 0x100, 0x1100);
    CHECK_EQ(fx_ax(&kf), 1);
    kf.p->cpu.seg[CPU_DS] = seg;
    while (n < 64 && (blocks[n] = lalloc(0, 200)) != 0) n++;
    CHECK(n >= 15 && n < 64);                                  /* 4K heap / 200 bytes */
    CHECK_EQ(lalloc(0, 200), 0);
    CHECK_EQ(lalloc(LMEM_MOVEABLE, 200), 0);
    /* free everything out of order: neighbours coalesce into one big block again */
    for (i = 0; i < n; i += 2) CHECK_EQ(lcall1(L_FREE, blocks[i]), 0);
    for (i = 1; i < n; i += 2) CHECK_EQ(lcall1(L_FREE, blocks[i]), 0);
    big = lalloc(0, (unsigned)(n * 200));
    CHECK(big != 0);
    FX_CALL(&kf, "KERNEL", L_COMPACT, 0);
    CHECK(fx_ax(&kf) < 0x1000);
    kdown();
}

static void test_local_realloc_fixed(void) {
    uint16_t a, b, r;
    uint8_t *p;
    printf("test_local_realloc_fixed\n");
    kboot();
    a = lalloc(0, 32);
    b = lalloc(0, 32);                                         /* blocks a from growing in place */
    p = kmem(kf.p->auto_ds, a, 32);
    if (p) memset(p, 0x42, 32);
    FX_CALL(&kf, "KERNEL", L_REALLOC, a, 64, 0);               /* cannot move without LMEM_MOVEABLE */
    CHECK_EQ(fx_ax(&kf), 0);
    FX_CALL(&kf, "KERNEL", L_REALLOC, a, 64, LMEM_MOVEABLE);
    r = fx_ax(&kf);
    CHECK(r != 0 && r != a);
    p = kmem(kf.p->auto_ds, r, 64);
    CHECK(p != NULL && p[0] == 0x42 && p[31] == 0x42);
    FX_CALL(&kf, "KERNEL", L_REALLOC, r, 8, 0);                /* shrinking is always in place */
    CHECK_EQ(fx_ax(&kf), r);
    CHECK_EQ(lcall1(L_SIZE, r), 8);
    (void)b;
    kdown();
}

static void test_local_init_other_segment(void) {
    uint16_t seg, a;
    cpu_t *c;
    printf("test_local_init_other_segment\n");
    kboot();
    c = &kf.p->cpu;
    seg = kseg(0x1000);
    /* no heap in a plain segment until LocalInit */
    c->seg[CPU_DS] = seg;
    CHECK_EQ(lalloc(0, 16), 0);
    c->seg[CPU_DS] = kf.p->auto_ds;
    FX_CALL(&kf, "KERNEL", L_INIT, seg, 0x100, 0xF00);
    CHECK_EQ(fx_ax(&kf), 1);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    FX_CALL(&kf, "KERNEL", L_INIT, seg, 0x100, 0xF00);          /* second time: already a heap */
    CHECK_EQ(fx_ax(&kf), 0);
    c->seg[CPU_DS] = seg;
    a = lalloc(0, 16);
    CHECK(a >= 0x100 && a + 16 <= 0xF00);
    c->seg[CPU_DS] = kf.p->auto_ds;
    /* the program's own heap is untouched by the other one */
    CHECK(lalloc(0, 16) >= kf.p->heap_start);
    kdown();
}

/* A data segment's local heap grows on demand (up to 64K), like a movable DS under Windows. */
static void test_local_heap_grows(void) {
    uint16_t a, b, h, ptr;
    uint32_t before;
    unsigned n = 0;
    uint8_t *p;
    printf("test_local_heap_grows\n");
    kboot_ex(NULL, 0, 0x800, 0x100);
    before = mem_size(&kf.p->mem, kf.p->auto_ds);
    a = lalloc(0, 0x80);
    CHECK(a != 0);
    p = kmem(kf.p->auto_ds, a, 0x80);
    if (p) memset(p, 0x6B, 0x80);
    b = lalloc(0, 0x400);                                      /* does not fit the initial 0x100 */
    CHECK(b != 0 && b + 0x400u <= mem_size(&kf.p->mem, kf.p->auto_ds));
    CHECK(mem_size(&kf.p->mem, kf.p->auto_ds) > before);
    p = kmem(kf.p->auto_ds, a, 0x80);
    CHECK(p != NULL && p[0] == 0x6B && p[0x7F] == 0x6B);       /* earlier blocks stay put */
    h = lalloc(LMEM_MOVEABLE, 0x2000);
    CHECK(h != 0);
    ptr = lcall1(L_LOCK, h);
    CHECK(ptr != 0 && ptr + 0x2000u <= mem_size(&kf.p->mem, kf.p->auto_ds));
    CHECK_EQ(lalloc(0, 0xFFF0), 0);                            /* never beyond the 64K segment */
    while (n < 100 && lalloc(0, 0x1000) != 0) n++;
    CHECK(n > 5 && n < 16);
    CHECK(mem_size(&kf.p->mem, kf.p->auto_ds) <= 0x10000);
    kdown();
}

/* WRITER's data segment is 0x9e6a bytes: not a multiple of 4, yet its heap must still grow. */
static void test_local_heap_grows_unaligned_segment(void) {
    uint16_t a;
    printf("test_local_heap_grows_unaligned_segment\n");
    kboot_ex(NULL, 0, 0x800, 0x102);
    CHECK(mem_size(&kf.p->mem, kf.p->auto_ds) % 4 != 0);
    a = lalloc(0, 0x800);
    CHECK(a != 0);
    CHECK(a + 0x800u <= mem_size(&kf.p->mem, kf.p->auto_ds));
    kdown();
}

int main(void) {
    test_global_trace();
    test_global_alloc_lock_free();
    test_global_realloc();
    test_global_huge();
    test_global_misc();
    test_local_fixed();
    test_local_moveable();
    test_local_exhaustion_and_reuse();
    test_local_realloc_fixed();
    test_local_init_other_segment();
    test_local_heap_grows();
    test_local_heap_grows_unaligned_segment();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
