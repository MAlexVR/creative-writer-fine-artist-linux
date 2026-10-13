#ifndef KFIXTURE_H
#define KFIXTURE_H
/* Fixture for the KERNEL tests: kernel + hle + a loaded guest process. */
#include "fixture.h"
#include "kernel.h"

static hle_t kh;
static kernel_t *kk;
static fx_t kf;

static inline void kboot_ex(const kernel_config *cfg, unsigned flags, uint16_t stack, uint16_t heap) {
    loader_config lc;
    memset(&lc, 0, sizeof lc);
    hle_init(&kh);
    kh.stub_unknown = true;
    kk = kernel_create(cfg, &kh, flags);
    lc.resolve = hle_resolve;
    lc.resolve_ctx = &kh;
    fx_init(&kf, stack, heap);
    CHECK(kk != NULL);
    if (fx_load(&kf, &lc) != LOADER_OK) return;
    CHECK(kernel_attach(kk, kf.p));
}

static inline void kboot(void) { kboot_ex(NULL, 0, 0x800, 0x1000); }

static inline void kdown(void) {
    fx_free(&kf);
    kernel_destroy(kk);
    hle_destroy(&kh);
    kk = NULL;
}

/* A data segment of `size` bytes in the guest, optionally holding a string. */
static inline uint16_t kseg(uint32_t size) { return mem_alloc(&kf.p->mem, size, MEM_DATA); }
static inline uint8_t *kmem(uint16_t sel, uint32_t off, uint32_t len) {
    return mem_ptr(&kf.p->mem, sel, off, len);
}
static inline uint16_t kstr(const char *s) {
    uint16_t sel = kseg(0x400);
    strcpy((char *)kmem(sel, 0, 1), s);
    return sel;
}
static inline const char *kcstr(uint16_t sel, uint32_t off) { return (const char *)kmem(sel, off, 1); }

#endif
