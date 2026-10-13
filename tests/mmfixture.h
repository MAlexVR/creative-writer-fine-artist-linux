#ifndef MMFIXTURE_H
#define MMFIXTURE_H
/* Fixture for the MMSYSTEM tests: USER on a manual clock plus MMSYSTEM over a null-sink audio engine, and a
 * guest procedure that records its sixteen argument bytes (timer and wave callbacks). */
#include "audio.h"
#include "mmsystem.h"
#include "ufixture.h"

static mmsystem_t *mm;
static audio_t *maudio;

#define M_REC 0x800   /* records 8 argument words at DS:0x100 + 16 * n and counts at DS:0x40; retf 16 */

static inline void m_install_recorder(void) {
    static const uint8_t code[] = {
        0x55, 0x89, 0xE5,
        0xA1, 0x40, 0x00, 0xB9, 0x10, 0x00, 0xF7, 0xE1, 0x05, 0x00, 0x01, 0x89, 0xC3,   /* bx = 0x100 + 16 * count */
        0x8B, 0x46, 0x06, 0x89, 0x07, 0x8B, 0x46, 0x08, 0x89, 0x47, 0x02,
        0x8B, 0x46, 0x0A, 0x89, 0x47, 0x04, 0x8B, 0x46, 0x0C, 0x89, 0x47, 0x06,
        0x8B, 0x46, 0x0E, 0x89, 0x47, 0x08, 0x8B, 0x46, 0x10, 0x89, 0x47, 0x0A,
        0x8B, 0x46, 0x12, 0x89, 0x47, 0x0C, 0x8B, 0x46, 0x14, 0x89, 0x47, 0x0E,
        0xFF, 0x06, 0x40, 0x00,
        0x31, 0xC0, 0x31, 0xD2, 0x5D, 0xCA, 0x10, 0x00};
    u_put(M_REC, code, sizeof code);
}

/* Recorded call i: arguments as the callee sees them, w[0] = last pushed word. For a (WORD id, WORD msg, DWORD user,
 * DWORD dw1, DWORD dw2) callback: id = w[7], msg = w[6], user = w[4]|w[5]<<16, dw1 = w[2]|w[3]<<16, dw2 = w[0]|w[1]<<16. */
static inline uint16_t mrec_w(unsigned i, unsigned w) { return urd16(u_ds(), 0x100 + 16 * i + 2 * w); }
static inline uint16_t mrec_id(unsigned i) { return mrec_w(i, 7); }
static inline uint16_t mrec_msg(unsigned i) { return mrec_w(i, 6); }
static inline uint32_t mrec_user(unsigned i) { return mrec_w(i, 4) | (uint32_t)mrec_w(i, 5) << 16; }
static inline uint32_t mrec_dw1(unsigned i) { return mrec_w(i, 2) | (uint32_t)mrec_w(i, 3) << 16; }
static inline uint32_t mrec_dw2(unsigned i) { return mrec_w(i, 0) | (uint32_t)mrec_w(i, 1) << 16; }

static inline void mmboot_k(bool with_audio, kernel_t *k) {
    mmsystem_config mc;
    uboot();
    memset(&mc, 0, sizeof mc);
    if (with_audio) {
        maudio = audio_create(&(audio_config){.rate = 44100});
        mc.audio = maudio;
    }
    mm = mmsystem_create(&mc, &uh, uu, k);
    CHECK(mm != NULL);
    if (mm) CHECK(mmsystem_attach(mm, uf.p));
    m_install_recorder();
}

static inline void mmboot(bool with_audio) { mmboot_k(with_audio, NULL); }

static inline void mmdown(void) {
    mmsystem_destroy(mm);
    mm = NULL;
    audio_destroy(maudio);
    maudio = NULL;
    udown();
}

#define MCALL(ord, ...) fx_call_words(&uf, "MMSYSTEM", (ord), (const uint16_t[]){0, ##__VA_ARGS__} + 1, \
                                      sizeof((const uint16_t[]){0, ##__VA_ARGS__}) / sizeof(uint16_t) - 1)

#endif
