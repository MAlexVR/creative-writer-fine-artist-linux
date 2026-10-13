#include "kfixture.h"

#define R_FIND 60
#define R_LOAD 61
#define R_LOCK 62
#define R_FREE 63
#define R_ACCESS 64
#define R_SIZEOF 65
#define R_ALLOC 66

#define RT_BITMAP 2
#define RT_STRING 6

static uint16_t hinst(void) { return kf.p->auto_ds; }

/* the program image: bytes 0x80.. hold the payload of the resources */
static void boot_with_resources(void) {
    kboot();
    memcpy(kf.file + 0x80, "BM-bitmap-bytes!", 16);
    memcpy(kf.file + 0xA0, "string-table", 12);
    fx_add_resource(&kf, RT_BITMAP, NULL, 1, NULL, 0x80, 16);
    fx_add_resource(&kf, RT_BITMAP, NULL, 0, "LOGO", 0xA0, 12);
    fx_add_resource(&kf, 0, "MYDATA", 7, NULL, 0xA0, 6);
    fx_add_resource(&kf, RT_STRING, NULL, 3, NULL, 0xA0, 12);
}

static uint16_t find(uint16_t inst, uint16_t nsel, uint16_t noff, uint16_t tsel, uint16_t toff) {
    FX_CALL(&kf, "KERNEL", R_FIND, inst, FARP(nsel, noff), FARP(tsel, toff));
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return fx_ax(&kf);
}

static void test_find_resource(void) {
    uint16_t a, b, s;
    printf("test_find_resource\n");
    boot_with_resources();
    a = find(hinst(), 0, 1, 0, RT_BITMAP);                     /* MAKEINTRESOURCE ids */
    CHECK(a != 0);
    CHECK_EQ(find(hinst(), 0, 1, 0, RT_BITMAP), a);            /* the same handle each time */
    s = kstr("logo");
    b = find(hinst(), s, 0, 0, RT_BITMAP);                     /* names ignore case */
    CHECK(b != 0 && b != a);
    s = kstr("#1");
    CHECK_EQ(find(hinst(), s, 0, 0, RT_BITMAP), a);            /* "#n" is the integer n */
    s = kstr("MyData");
    b = kstr("mydata");
    CHECK(find(hinst(), 0, 7, b, 0) != 0);                     /* a string resource type */
    CHECK_EQ(find(hinst(), s, 0, 0, RT_BITMAP), 0);            /* wrong type/name pairing */
    CHECK_EQ(find(hinst(), 0, 99, 0, RT_BITMAP), 0);
    CHECK_EQ(find(hinst(), 0, 1, 0, 14), 0);                   /* no such type */
    CHECK_EQ(find(0x1234, 0, 1, 0, RT_BITMAP), 0);             /* not the program's module */
    kdown();
}

/* With the HLE trace on, FindResource says which type and name it was asked for and whether it found it. */
static void test_find_resource_trace(void) {
    FILE *log = tmpfile();
    char buf[512];
    size_t n;
    printf("test_find_resource_trace\n");
    boot_with_resources();
    kh.log = log;
    kh.trace = true;
    find(hinst(), 0, 1, 0, RT_BITMAP);
    find(hinst(), kstr("logo"), 0, 0, RT_BITMAP);
    find(hinst(), 0, 99, 0, RT_BITMAP);
    kh.log = NULL;
    kh.trace = false;
    rewind(log);
    n = fread(buf, 1, sizeof buf - 1, log);
    buf[n] = 0;
    CHECK(strstr(buf, "resource: FindResource type #2 name #1 -> ok") != NULL);
    CHECK(strstr(buf, "name \"logo\" -> ok") != NULL);
    CHECK(strstr(buf, "name #99 -> not found") != NULL);
    fclose(log);
    kdown();
}

static void test_load_lock_free(void) {
    uint16_t r, h, sel;
    printf("test_load_lock_free\n");
    boot_with_resources();
    r = find(hinst(), 0, 1, 0, RT_BITMAP);
    FX_CALL(&kf, "KERNEL", R_SIZEOF, hinst(), r);
    CHECK_EQ(fx_dxax(&kf), 16);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    FX_CALL(&kf, "KERNEL", R_LOAD, hinst(), r);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    h = fx_ax(&kf);
    CHECK(h != 0 && mem_valid(&kf.p->mem, h));
    FX_CALL(&kf, "KERNEL", R_LOCK, h);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    sel = (uint16_t)(fx_dxax(&kf) >> 16);
    CHECK_EQ(fx_ax(&kf), 0);
    CHECK_EQ(sel, h);
    CHECK(kmem(sel, 0, 16) && !memcmp(kmem(sel, 0, 16), "BM-bitmap-bytes!", 16));
    /* loading again shares the block and counts a reference */
    FX_CALL(&kf, "KERNEL", R_LOAD, hinst(), r);
    CHECK_EQ(fx_ax(&kf), h);
    FX_CALL(&kf, "KERNEL", R_FREE, h);
    CHECK_EQ(fx_ax(&kf), 0);
    CHECK(mem_valid(&kf.p->mem, h));                           /* one reference left */
    FX_CALL(&kf, "KERNEL", R_FREE, h);
    CHECK_EQ(fx_ax(&kf), 0);
    CHECK(!mem_valid(&kf.p->mem, h));
    FX_CALL(&kf, "KERNEL", R_FREE, h);                         /* already gone: failure returns the handle */
    CHECK_EQ(fx_ax(&kf), h);
    /* invalid arguments */
    FX_CALL(&kf, "KERNEL", R_LOAD, hinst(), 0);
    CHECK_EQ(fx_ax(&kf), 0);
    FX_CALL(&kf, "KERNEL", R_LOAD, hinst(), 0x7000);
    CHECK_EQ(fx_ax(&kf), 0);
    FX_CALL(&kf, "KERNEL", R_LOCK, 0x1234);
    CHECK_EQ(fx_dxax(&kf), 0);
    kdown();
}

static void test_alloc_resource(void) {
    uint16_t r, h;
    printf("test_alloc_resource\n");
    boot_with_resources();
    r = find(hinst(), 0, 1, 0, RT_BITMAP);
    FX_CALL(&kf, "KERNEL", R_ALLOC, hinst(), r, LONG(0x300));
    CHECK_EQ(kf.sp_after, kf.sp_before);
    h = fx_ax(&kf);
    CHECK(h != 0 && mem_size(&kf.p->mem, h) == 0x300);
    FX_CALL(&kf, "KERNEL", R_ALLOC, hinst(), r, LONG(0));      /* 0: the resource's own size */
    h = fx_ax(&kf);
    CHECK(h != 0 && mem_size(&kf.p->mem, h) == 16);
    kdown();
}

int main(void) {
    test_find_resource_trace();
    test_find_resource();
    test_load_lock_free();
    test_alloc_resource();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
