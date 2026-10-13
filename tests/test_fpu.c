/* x87 FPU tests: hand-assembled instruction bytes run through the CPU core
 * (flat test machine), checking registers, status/control words and memory. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"

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

/* long double comparison, exact (NaN equals NaN) */
#define CHECK_LD(got, want)                                                  \
    do {                                                                     \
        long double g_ = (got), w_ = (want);                                 \
        g_checks++;                                                          \
        if (!(g_ == w_ || (isnan(g_) && isnan(w_)))) {                       \
            g_fails++;                                                       \
            printf("  FAIL %s:%d: %s = %.21Lg, want %.21Lg\n", __FILE__,     \
                   __LINE__, #got, g_, w_);                                  \
        }                                                                    \
    } while (0)

/* close to within a relative tolerance (transcendentals) */
#define CHECK_NEAR(got, want, tol)                                           \
    do {                                                                     \
        long double g_ = (got), w_ = (want);                                 \
        g_checks++;                                                          \
        if (!(fabsl(g_ - w_) <= (tol) * (fabsl(w_) > 1 ? fabsl(w_) : 1))) {  \
            g_fails++;                                                       \
            printf("  FAIL %s:%d: %s = %.21Lg, want %.21Lg\n", __FILE__,     \
                   __LINE__, #got, g_, w_);                                  \
        }                                                                    \
    } while (0)

/* ---- flat machine ------------------------------------------------------- */

#define MEMSZ 0x100000u
#define S_CS 0x10
#define S_DS 0x18
#define S_ES 0x20
#define S_SS 0x28
#define S_SMALL 0x30 /* limit 0x0F */
#define CSB 0x00000u
#define DSB 0x10000u
#define ESB 0x20000u
#define SSB 0x30000u
#define SMB 0x40000u
#define CODE_OFF 0x100u

static uint8_t g_mem[MEMSZ];
static cpu_t g_cpu;

static bool seg_info(uint16_t sel, uint32_t *base, uint32_t *limit) {
    switch (sel) {
    case S_CS: *base = CSB; break;
    case S_DS: *base = DSB; break;
    case S_ES: *base = ESB; break;
    case S_SS: *base = SSB; break;
    case S_SMALL: *base = SMB; *limit = 0x0F; return true;
    default: return false;
    }
    *limit = 0xFFFF;
    return true;
}
static cpu_status t_r8(void *x, uint16_t sel, uint32_t off, uint8_t *v) {
    uint32_t b, l;
    (void)x;
    if (!seg_info(sel, &b, &l)) return CPU_E_SEL;
    if (off > l) return CPU_E_LIMIT;
    *v = g_mem[b + off];
    return CPU_OK;
}
static cpu_status t_w8(void *x, uint16_t sel, uint32_t off, uint8_t v) {
    uint32_t b, l;
    (void)x;
    if (!seg_info(sel, &b, &l)) return CPU_E_SEL;
    if (off > l) return CPU_E_LIMIT;
    g_mem[b + off] = v;
    return CPU_OK;
}
static bool t_valid(void *x, uint16_t sel) {
    uint32_t b, l;
    (void)x;
    return seg_info(sel, &b, &l);
}

static void reset(void) {
    cpu_mem m;
    memset(g_mem, 0, sizeof g_mem);
    memset(&m, 0, sizeof m);
    m.read8 = t_r8; m.write8 = t_w8; m.sel_valid = t_valid;
    cpu_init(&g_cpu, &m);
    g_cpu.seg[CPU_CS] = S_CS; g_cpu.seg[CPU_DS] = S_DS; g_cpu.seg[CPU_ES] = S_ES;
    g_cpu.seg[CPU_SS] = S_SS;
    g_cpu.eip = CODE_OFF;
    g_cpu.r[CPU_ESP] = 0x8000;
}

static cpu_status run_bytes(const uint8_t *b, size_t n) {
    memcpy(g_mem + CSB + CODE_OFF, b, n);
    g_mem[CSB + CODE_OFF + n] = 0xF4; /* HLT */
    g_cpu.eip = CODE_OFF;
    return cpu_run(&g_cpu, 100000);
}
#define RUN(...) run_bytes((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))
#define RUN_OK(...) CHECK_EQ(RUN(__VA_ARGS__), CPU_HALT)

/* data at DS:0x200 (slot A) and DS:0x220 (slot B), absolute [disp16] operands */
#define A_OFF 0x200u
#define B_OFF 0x220u
#define MA(reg) (uint8_t)((reg) << 3 | 6), 0x00, 0x02
#define MB(reg) (uint8_t)((reg) << 3 | 6), 0x20, 0x02
#define PTR(off) (g_mem + DSB + (off))

static void put_f(uint32_t off, float v) { memcpy(PTR(off), &v, 4); }
static void put_d(uint32_t off, double v) { memcpy(PTR(off), &v, 8); }
static float get_f(uint32_t off) { float v; memcpy(&v, PTR(off), 4); return v; }
static double get_d(uint32_t off) { double v; memcpy(&v, PTR(off), 8); return v; }
static void put_i16(uint32_t off, int16_t v) { memcpy(PTR(off), &v, 2); }
static void put_i32(uint32_t off, int32_t v) { memcpy(PTR(off), &v, 4); }
static void put_i64(uint32_t off, int64_t v) { memcpy(PTR(off), &v, 8); }
static int16_t get_i16(uint32_t off) { int16_t v; memcpy(&v, PTR(off), 2); return v; }
static int32_t get_i32(uint32_t off) { int32_t v; memcpy(&v, PTR(off), 4); return v; }
static int64_t get_i64(uint32_t off) { int64_t v; memcpy(&v, PTR(off), 8); return v; }
static void put_m80(uint32_t off, const char *hex) { /* hex digits, most significant first */
    unsigned i;
    for (i = 0; i < 10; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        PTR(off)[9 - i] = (uint8_t)v;
    }
}
static bool m80_is(uint32_t off, const char *hex) {
    unsigned i;
    for (i = 0; i < 10; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        if (PTR(off)[9 - i] != v) return false;
    }
    return true;
}

#define FPU (g_cpu.fpu)
#define ST(i) fpu_st(&FPU, (i))
#define SW (FPU.sw)
#define CC (FPU.sw & FPU_SW_CC)
#define TOP fpu_top(&FPU)

static void set_cw(uint16_t cw) {
    put_i16(0x240, (int16_t)cw);
    RUN_OK(0xD9, 0x2E, 0x40, 0x02); /* fldcw [0x240] */
}

/* ---- group 1: data movement, constants, control -------------------------- */

static void test_fld_fst(void) {
    printf("test_fld_fst\n");
    reset();
    put_f(A_OFF, 1.5f);
    RUN_OK(0xD9, MA(0));                          /* fld dword [A] */
    CHECK_LD(ST(0), 1.5L); CHECK_EQ(fpu_depth(&FPU), 1); CHECK_EQ(TOP, 7);
    RUN_OK(0xD9, MB(3));                          /* fstp dword [B] */
    CHECK_EQ(get_f(B_OFF), 1.5f); CHECK_EQ(fpu_depth(&FPU), 0); CHECK_EQ(TOP, 0);
    put_d(A_OFF, -2.25);
    RUN_OK(0xDD, MA(0));                          /* fld qword [A] */
    CHECK_LD(ST(0), -2.25L);
    RUN_OK(0xDD, MB(2));                          /* fst qword [B] (no pop) */
    CHECK_EQ(get_d(B_OFF), -2.25); CHECK_EQ(fpu_depth(&FPU), 1);
    RUN_OK(0xDD, MB(3));                          /* fstp qword [B] */
    CHECK_EQ(fpu_depth(&FPU), 0);
    /* extended: round trip is bit exact, including denormals and infinity */
    put_m80(A_OFF, "3FFFC000000000000000");       /* 1.5 */
    RUN_OK(0xDB, MA(5), 0xDB, MB(7));             /* fld tbyte [A]; fstp tbyte [B] */
    CHECK(m80_is(B_OFF, "3FFFC000000000000000"));
    put_m80(A_OFF, "400DD49A784BCD1B8AFE");
    RUN_OK(0xDB, MA(5), 0xDB, MB(7));
    CHECK(m80_is(B_OFF, "400DD49A784BCD1B8AFE"));
    put_m80(A_OFF, "00000000000000000001");       /* smallest denormal */
    RUN_OK(0xDB, MA(5), 0xDB, MB(7));
    CHECK(m80_is(B_OFF, "00000000000000000001"));
    put_m80(A_OFF, "7FFF8000000000000000");       /* +inf */
    RUN_OK(0xDB, MA(5)); CHECK(isinf(ST(0))); CHECK(ST(0) > 0);
    RUN_OK(0xDB, MB(7)); CHECK(m80_is(B_OFF, "7FFF8000000000000000"));
    put_m80(A_OFF, "BFFF8000000000000000");       /* -1.0 */
    RUN_OK(0xDB, MA(5)); CHECK_LD(ST(0), -1.0L);
    RUN_OK(0xDB, MB(7));
    /* FLD ST(i) and FST ST(i)/FSTP ST(i) */
    RUN_OK(0xD9, 0xE8, 0xD9, 0xEE);               /* fld1; fldz */
    RUN_OK(0xD9, 0xC1);                           /* fld st(1) */
    CHECK_LD(ST(0), 1.0L); CHECK_EQ(fpu_depth(&FPU), 3);
    RUN_OK(0xDD, 0xD2);                           /* fst st(2) */
    CHECK_LD(ST(2), 1.0L); CHECK_EQ(fpu_depth(&FPU), 3);
    RUN_OK(0xDD, 0xDA);                           /* fstp st(2): st(2)=st(0), pop */
    CHECK_EQ(fpu_depth(&FPU), 2); CHECK_LD(ST(0), 0.0L); CHECK_LD(ST(1), 1.0L);
    /* narrowing store: out of float range gives infinity and OE; fractional gives PE-free value */
    reset();
    put_d(A_OFF, 1e300);
    RUN_OK(0xDD, MA(0), 0xD9, MB(3));
    CHECK(isinf(get_f(B_OFF)));
    put_d(A_OFF, 0.1);
    RUN_OK(0xDD, MA(0), 0xD9, MB(3));
    CHECK_EQ(get_f(B_OFF), 0.1f);
}

static void test_fild_fist(void) {
    printf("test_fild_fist\n");
    reset();
    put_i16(A_OFF, -1234);
    RUN_OK(0xDF, MA(0));                          /* fild word */
    CHECK_LD(ST(0), -1234.0L);
    put_i32(A_OFF, 123456789);
    RUN_OK(0xDB, MA(0));                          /* fild dword */
    CHECK_LD(ST(0), 123456789.0L);
    put_i64(A_OFF, 0x7FFFFFFFFFFFFFFFll);
    RUN_OK(0xDF, MA(5));                          /* fild qword */
    CHECK_LD(ST(0), 9223372036854775807.0L);      /* exact in a 64-bit mantissa */
    CHECK(ST(0) == (long double)0x7FFFFFFFFFFFFFFFll);
    put_i64(A_OFF, (int64_t)0x8000000000000000ull);
    RUN_OK(0xDF, MA(5)); CHECK_LD(ST(0), -9223372036854775808.0L);
    /* FIST rounds per RC: nearest-even, down, up, truncate */
    {
        static const struct { uint16_t rc; double in; int32_t out; } t[] = {
            {0, 2.5, 2}, {0, 3.5, 4}, {0, -2.5, -2}, {0, 2.6, 3}, {0, -0.4, 0},
            {1, 2.9, 2}, {1, -2.1, -3}, {1, -2.0, -2},
            {2, 2.1, 3}, {2, -2.9, -2}, {2, 2.0, 2},
            {3, 2.9, 2}, {3, -2.9, -2},
        };
        size_t i;
        for (i = 0; i < sizeof t / sizeof t[0]; i++) {
            reset();
            set_cw((uint16_t)(FPU_CW_DEFAULT | t[i].rc << 10));
            put_d(A_OFF, t[i].in);
            RUN_OK(0xDD, MA(0), 0xDB, MB(3));     /* fld qword; fistp dword */
            CHECK_EQ((long long)get_i32(B_OFF), (long long)t[i].out);
            CHECK_EQ(fpu_depth(&FPU), 0);
            if (t[i].in == (double)(int)t[i].in) CHECK_EQ(SW & FPU_SW_PE, 0);
            else CHECK_EQ(SW & FPU_SW_PE, FPU_SW_PE);
        }
    }
    reset();
    put_d(A_OFF, 7.5);
    RUN_OK(0xDD, MA(0), 0xDF, MB(2));             /* fld; fist word (no pop) */
    CHECK_EQ(get_i16(B_OFF), 8); CHECK_EQ(fpu_depth(&FPU), 1);
    RUN_OK(0xDB, MB(2)); CHECK_EQ(get_i32(B_OFF), 8);
    RUN_OK(0xDF, MB(7)); CHECK_EQ(get_i64(B_OFF), 8); CHECK_EQ(fpu_depth(&FPU), 0);
    /* out of range / NaN: integer indefinite and IE */
    reset();
    put_d(A_OFF, 40000.0);
    RUN_OK(0xDD, MA(0), 0xDF, MB(3));
    CHECK_EQ(get_i16(B_OFF), -32768); CHECK_EQ(SW & FPU_SW_IE, FPU_SW_IE);
    reset();
    put_d(A_OFF, 3e9);
    RUN_OK(0xDD, MA(0), 0xDB, MB(3));
    CHECK_EQ((uint32_t)get_i32(B_OFF), 0x80000000u); CHECK_EQ(SW & FPU_SW_IE, FPU_SW_IE);
    reset();
    put_d(A_OFF, -1e19);
    RUN_OK(0xDD, MA(0), 0xDF, MB(7));
    CHECK_EQ((uint64_t)get_i64(B_OFF), 0x8000000000000000ull); CHECK_EQ(SW & FPU_SW_IE, FPU_SW_IE);
    reset();
    put_d(A_OFF, 32767.0);
    RUN_OK(0xDD, MA(0), 0xDF, MB(3)); CHECK_EQ(get_i16(B_OFF), 32767); CHECK_EQ(SW & FPU_SW_IE, 0);
    put_d(A_OFF, -32768.0);
    RUN_OK(0xDD, MA(0), 0xDF, MB(3)); CHECK_EQ(get_i16(B_OFF), -32768); CHECK_EQ(SW & FPU_SW_IE, 0);
    put_d(A_OFF, -32768.4);                       /* rounds to -32768: in range */
    RUN_OK(0xDD, MA(0), 0xDF, MB(3)); CHECK_EQ(get_i16(B_OFF), -32768); CHECK_EQ(SW & FPU_SW_IE, 0);
    reset();
    put_m80(A_OFF, "7FFFC000000000000000");       /* NaN */
    RUN_OK(0xDB, MA(5), 0xDF, MB(3));
    CHECK_EQ(get_i16(B_OFF), -32768); CHECK_EQ(SW & FPU_SW_IE, FPU_SW_IE);
}

static void test_bcd(void) {
    printf("test_bcd\n");
    reset();
    put_d(A_OFF, 123456789012345678.0);           /* not exact in a double: use an integer */
    put_i64(A_OFF, 123456789012345678ll);
    RUN_OK(0xDF, MA(5), 0xDF, MB(6));             /* fild qword; fbstp [B] */
    CHECK_EQ(PTR(B_OFF)[0], 0x78); CHECK_EQ(PTR(B_OFF)[1], 0x56); CHECK_EQ(PTR(B_OFF)[2], 0x34);
    CHECK_EQ(PTR(B_OFF)[3], 0x12); CHECK_EQ(PTR(B_OFF)[4], 0x90); CHECK_EQ(PTR(B_OFF)[5], 0x78);
    CHECK_EQ(PTR(B_OFF)[6], 0x56); CHECK_EQ(PTR(B_OFF)[7], 0x34); CHECK_EQ(PTR(B_OFF)[8], 0x12);
    CHECK_EQ(PTR(B_OFF)[9], 0x00); CHECK_EQ(fpu_depth(&FPU), 0);
    RUN_OK(0xDF, MB(4));                          /* fbld [B] */
    CHECK(ST(0) == 123456789012345678.0L);
    RUN_OK(0xDF, MA(7));                          /* fistp qword [A] */
    CHECK_EQ(get_i64(A_OFF), 123456789012345678ll);
    reset();
    put_d(A_OFF, -42.0);
    RUN_OK(0xDD, MA(0), 0xDF, MB(6));
    CHECK_EQ(PTR(B_OFF)[0], 0x42); CHECK_EQ(PTR(B_OFF)[1], 0); CHECK_EQ(PTR(B_OFF)[9], 0x80);
    RUN_OK(0xDF, MB(4)); CHECK_LD(ST(0), -42.0L);
    /* rounding per RC: 2.5 nearest-even -> 2, truncate -> 2, up -> 3 */
    reset();
    put_d(A_OFF, 2.5);
    RUN_OK(0xDD, MA(0), 0xDF, MB(6)); CHECK_EQ(PTR(B_OFF)[0], 0x02);
    set_cw(FPU_CW_DEFAULT | 2 << 10);
    RUN_OK(0xDD, MA(0), 0xDF, MB(6)); CHECK_EQ(PTR(B_OFF)[0], 0x03);
    /* too big: BCD indefinite, IE */
    reset();
    put_d(A_OFF, 1e18);
    RUN_OK(0xDD, MA(0), 0xDF, MB(6));
    CHECK_EQ(PTR(B_OFF)[7], 0xC0); CHECK_EQ(PTR(B_OFF)[8], 0xFF); CHECK_EQ(PTR(B_OFF)[9], 0xFF);
    CHECK_EQ(PTR(B_OFF)[0], 0); CHECK_EQ(SW & FPU_SW_IE, FPU_SW_IE);
    /* -0 keeps its sign through FBSTP */
    reset();
    put_d(A_OFF, -0.0);
    RUN_OK(0xDD, MA(0), 0xDF, MB(6)); CHECK_EQ(PTR(B_OFF)[9], 0x80);
    put_d(A_OFF, 999999999999999999.0);
    RUN_OK(0xDD, MA(0), 0xDF, MB(6));             /* 1e18 as a double: out of range */
    CHECK_EQ(PTR(B_OFF)[9], 0xFF);
}

static void test_constants(void) {
    static const struct { uint8_t op; const char *hex; } t[] = {
        {0xE8, "3FFF8000000000000000"},   /* fld1 */
        {0xE9, "4000D49A784BCD1B8AFE"},   /* fldl2t */
        {0xEA, "3FFFB8AA3B295C17F0BC"},   /* fldl2e (nearest) */
        {0xEB, "4000C90FDAA22168C235"},   /* fldpi */
        {0xEC, "3FFD9A209A84FBCFF799"},   /* fldlg2 */
        {0xED, "3FFEB17217F7D1CF79AC"},   /* fldln2 */
        {0xEE, "00000000000000000000"},   /* fldz */
    };
    size_t i;
    printf("test_constants\n");
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        reset();
        RUN_OK(0xD9, t[i].op, 0xDB, MB(7));
        CHECK(m80_is(B_OFF, t[i].hex));
        CHECK_EQ(fpu_depth(&FPU), 0);
    }
    reset();
    RUN_OK(0xD9, 0xEE);
    CHECK_EQ(fpu_tag_word(&FPU), 0x7FFF);        /* top=7 holds zero: tag 01 at bits 14-15 */
}

static void test_stack_ops(void) {
    printf("test_stack_ops\n");
    reset();
    RUN_OK(0xD9, 0xE8, 0xD9, 0xEE);               /* fld1; fldz: st0=0 st1=1 */
    RUN_OK(0xD9, 0xC9);                           /* fxch st(1) */
    CHECK_LD(ST(0), 1.0L); CHECK_LD(ST(1), 0.0L);
    RUN_OK(0xD9, 0xCA);                           /* fxch st(2): st(2) is empty -> indefinite */
    CHECK(isnan(ST(0))); CHECK_EQ(SW & FPU_SW_IE, FPU_SW_IE); CHECK_EQ(SW & FPU_SW_SF, FPU_SW_SF);
    reset();
    RUN_OK(0xD9, 0xE8, 0xD9, 0xEE, 0xDD, 0xC0);   /* ffree st(0) */
    CHECK_EQ(fpu_depth(&FPU), 1); CHECK_EQ(TOP, 6);
    RUN_OK(0xD9, 0xF7);                           /* fincstp */
    CHECK_EQ(TOP, 7);
    RUN_OK(0xD9, 0xF6, 0xD9, 0xF6);               /* fdecstp x2 */
    CHECK_EQ(TOP, 5);
    RUN_OK(0xD9, 0xD0);                           /* fnop */
    CHECK_EQ(TOP, 5);
    RUN_OK(0x9B, 0x90);                           /* wait */
    /* overflow: 9th push with masked exceptions loads the real indefinite, C1 = 1 */
    reset();
    {
        int i;
        for (i = 0; i < 8; i++) RUN_OK(0xD9, 0xE8);
        CHECK_EQ(fpu_depth(&FPU), 8); CHECK_EQ(SW & FPU_SW_IE, 0);
        RUN_OK(0xD9, 0xEE);                       /* fldz: no free register */
        CHECK(isnan(ST(0))); CHECK(signbit(ST(0)));
        CHECK_EQ(SW & (FPU_SW_IE | FPU_SW_SF | FPU_SW_C1), FPU_SW_IE | FPU_SW_SF | FPU_SW_C1);
        CHECK_EQ(fpu_depth(&FPU), 8);
    }
    /* underflow: popping an empty stack gives indefinite, C1 = 0 */
    reset();
    put_d(A_OFF, 5.0);
    RUN_OK(0xD9, MB(3));                          /* fstp dword with an empty stack */
    CHECK_EQ(SW & (FPU_SW_IE | FPU_SW_SF | FPU_SW_C1), FPU_SW_IE | FPU_SW_SF);
    CHECK(isnan(get_f(B_OFF)));
    CHECK_EQ(fpu_depth(&FPU), 0);
    reset();
    RUN_OK(0xD9, 0xE8, 0xD9, 0xE8, 0xD9, 0xE8);
    CHECK_EQ(TOP, 5);
    RUN_OK(0xDD, 0xD8, 0xDD, 0xD8, 0xDD, 0xD8);   /* three fstp st(0) */
    CHECK_EQ(TOP, 0); CHECK_EQ(SW & FPU_SW_IE, 0);
}

static void test_control(void) {
    printf("test_control\n");
    reset();
    CHECK_EQ(FPU.cw, 0x037F); CHECK_EQ(SW, 0); CHECK_EQ(fpu_tag_word(&FPU), 0xFFFF);
    RUN_OK(0xD9, 0xE8, 0xD9, 0xE8);
    CHECK_EQ(fpu_tag_word(&FPU), 0x0FFF);         /* phys 6,7 valid (tag 00), the rest empty */
    CHECK_EQ(SW >> 11 & 7, 6);
    RUN_OK(0xDB, 0xE3);                           /* fninit */
    CHECK_EQ(FPU.cw, 0x037F); CHECK_EQ(SW, 0); CHECK_EQ(fpu_depth(&FPU), 0);
    set_cw(0x0C7F);
    CHECK_EQ(FPU.cw, 0x0C7F);
    RUN_OK(0xD9, MB(7));                          /* fnstcw [B] */
    CHECK_EQ(get_i16(B_OFF), 0x0C7F);
    RUN_OK(0xD9, 0xE8, 0xD9, 0xE8);
    RUN_OK(0xDD, MB(7));                          /* fnstsw [B] */
    CHECK_EQ(get_i16(B_OFF) & 0xFFFF, 6 << 11);
    g_cpu.r[CPU_EAX] = 0xABCD0000;
    RUN_OK(0xDF, 0xE0);                           /* fnstsw ax */
    CHECK_EQ(g_cpu.r[CPU_EAX], 0xABCD0000u | (6 << 11));
    RUN_OK(0x9B, 0xDF, 0xE0);                     /* wait; fnstsw ax */
    CHECK_EQ(g_cpu.r[CPU_EAX] & 0xFFFF, 6 << 11);
    /* FCLEX clears exception flags and keeps TOP and condition codes */
    FPU.sw |= FPU_SW_IE | FPU_SW_ZE | FPU_SW_SF | FPU_SW_ES | FPU_SW_C3;
    RUN_OK(0xDB, 0xE2);
    CHECK_EQ(SW, (6u << 11) | FPU_SW_C3);
    RUN_OK(0xDB, 0xE0, 0xDB, 0xE1, 0xDB, 0xE4);   /* feni, fdisi, fsetpm: no-ops */
    CHECK_EQ(SW, (6u << 11) | FPU_SW_C3);
}

static void test_env_save(void) {
    printf("test_env_save\n");
    reset();
    RUN_OK(0xD9, 0xE8, 0xD9, 0xEB, 0xD9, 0xEE);   /* fld1; fldpi; fldz */
    set_cw(0x0F7F);
    RUN_OK(0xD9, MA(6));                          /* fnstenv [A] */
    CHECK_EQ(get_i16(A_OFF + 0) & 0xFFFF, 0x0F7F);
    CHECK_EQ(get_i16(A_OFF + 2) & 0xFFFF, 5u << 11);
    /* top=5 holds zero (tag 01), 6 holds pi (00), 7 holds 1.0 (00) */
    CHECK_EQ(get_i16(A_OFF + 4) & 0xFFFF, 0x07FF);
    CHECK_EQ(FPU.cw & 0x3F, 0x3F);                /* fnstenv masks all exceptions */
    RUN_OK(0xDB, 0xE3);
    CHECK_EQ(fpu_depth(&FPU), 0);
    put_i16(A_OFF + 2, (int16_t)(3u << 11 | FPU_SW_C0));
    put_i16(A_OFF + 4, (int16_t)0xFFFF);
    put_i16(A_OFF + 0, 0x0B7F);
    RUN_OK(0xD9, MA(4));                          /* fldenv [A] */
    CHECK_EQ(FPU.cw, 0x0B7F); CHECK_EQ(TOP, 3); CHECK_EQ(SW & FPU_SW_C0, FPU_SW_C0);
    CHECK_EQ(fpu_depth(&FPU), 0);
    /* fnsave / frstor round trip */
    reset();
    put_d(A_OFF, 3.25);
    RUN_OK(0xD9, 0xE8, 0xDD, MA(0), 0xD9, 0xEB);  /* fld1; fld qword 3.25; fldpi */
    set_cw(0x0F7F);
    FPU.sw |= FPU_SW_C2;
    {
        uint16_t sw_before = FPU.sw;
        RUN_OK(0xDD, MB(6));                      /* fnsave [B] */
        CHECK_EQ(FPU.cw, 0x037F); CHECK_EQ(SW, 0); CHECK_EQ(fpu_depth(&FPU), 0);
        CHECK_EQ(get_i16(B_OFF) & 0xFFFF, 0x0F7F);
        CHECK_EQ(get_i16(B_OFF + 2) & 0xFFFF, sw_before);
        CHECK(m80_is(B_OFF + 14, "4000C90FDAA22168C235"));       /* st(0) = pi */
        CHECK(m80_is(B_OFF + 24, "4000D000000000000000"));  /* st(1) = 3.25 */
        RUN_OK(0xDD, MB(4));                      /* frstor [B] */
        CHECK_EQ(FPU.cw, 0x0F7F); CHECK_EQ(SW, sw_before); CHECK_EQ(fpu_depth(&FPU), 3);
        CHECK(ST(0) == 3.14159265358979323846264338327950288L); CHECK_LD(ST(1), 3.25L);
        CHECK_LD(ST(2), 1.0L);
    }
}

static void test_addressing_and_faults(void) {
    cpu_insn in;
    cpu_mem m;
    printf("test_addressing_and_faults\n");
    reset();
    memcpy(g_mem + ESB, &(float){2.5f}, 4);
    RUN_OK(0x26, 0xD9, 0x06, 0x00, 0x00);         /* fld dword es:[0] */
    CHECK_LD(ST(0), 2.5L);
    put_f(A_OFF, 7.0f);
    RUN_OK(0x67, 0xD9, 0x05, 0x00, 0x02, 0x00, 0x00);   /* fld dword [disp32] */
    CHECK_LD(ST(0), 7.0L);
    g_cpu.r[CPU_EBX] = A_OFF - 4;
    RUN_OK(0xD9, 0x47, 0x04);                     /* fld dword [bx+4] */
    CHECK_LD(ST(0), 7.0L);
    memcpy(g_mem + SSB + 0x10, &(double){-8.5}, 8);
    g_cpu.r[CPU_EBP] = 0x10;
    RUN_OK(0xDD, 0x46, 0x00);                     /* fld qword [bp+0]: SS by default */
    CHECK_LD(ST(0), -8.5L);
    RUN_OK(0xD9, 0xE8, 0xD9, 0xEE);
    CHECK_EQ(fpu_depth(&FPU), 6);
    /* operand beyond the segment limit: fault, EIP rewinds, FPU state untouched */
    reset();
    g_cpu.seg[CPU_DS] = S_SMALL;
    RUN_OK(0xD9, 0xE8);
    CHECK_EQ(RUN(0xD9, 0x06, 0x0E, 0x00), CPU_E_LIMIT);   /* fld dword [0x0E]: straddles */
    CHECK_EQ(g_cpu.eip, CODE_OFF); CHECK_EQ(fpu_depth(&FPU), 1);
    CHECK_EQ(RUN(0xD9, 0x1E, 0x0E, 0x00), CPU_E_LIMIT);   /* fstp dword [0x0E] */
    CHECK_EQ(fpu_depth(&FPU), 1); CHECK_LD(ST(0), 1.0L); CHECK_EQ(TOP, 7);
    RUN_OK(0xD9, 0x1E, 0x0C, 0x00);               /* fstp dword [0x0C]: fits */
    CHECK_EQ(fpu_depth(&FPU), 0);
    /* undefined encodings */
    reset();
    CHECK_EQ(RUN(0xD9, 0x0E, 0x00, 0x02), CPU_E_UD);      /* d9 /1 memory */
    CHECK_EQ(g_cpu.eip, CODE_OFF);
    CHECK_EQ(RUN(0xD9, 0xD1), CPU_E_UD);
    CHECK_EQ(RUN(0xDA, 0xC1), CPU_E_UD);                  /* fcmovb: not a 387 */
    CHECK_EQ(RUN(0xDB, 0xF1), CPU_E_UD);                  /* fcomi: not a 387 */
    /* length decoder */
    memset(&m, 0, sizeof m);
    m.read8 = t_r8; m.write8 = t_w8; m.sel_valid = t_valid;
#define LEN(n, ...) do { memset(g_mem + CSB + CODE_OFF, 0, 32);                  \
        memcpy(g_mem + CSB + CODE_OFF, (const uint8_t[]){__VA_ARGS__}, n);       \
        CHECK_EQ(cpu_decode(&m, S_CS, CODE_OFF, &in), CPU_OK);                   \
        CHECK_EQ(in.length, n); CHECK(in.fpu); CHECK(in.supported); } while (0)
    LEN(2, 0xD9, 0xE8);
    LEN(4, 0xD9, 0x06, 0x00, 0x02);
    LEN(3, 0xD9, 0x47, 0x04);
    LEN(4, 0xD9, 0x86, 0x00, 0x02);
    LEN(4, 0x26, 0xD9, 0x46, 0x04);
    LEN(7, 0x67, 0xD9, 0x05, 0x00, 0x02, 0x00, 0x00);
    LEN(3, 0xDD, 0x44, 0x24);
#undef LEN
}


/* ---- group 2: arithmetic and comparison ---------------------------------- */

static void ld_d(double v) { /* fld qword v */
    put_d(0x260, v);
    RUN_OK(0xDD, 0x06, 0x60, 0x02);
}

/* The four condition codes C3 C2 C1 C0 as a nibble. */
static unsigned cc_bits(void) {
    return (SW & FPU_SW_C3 ? 8u : 0u) | (SW & FPU_SW_C2 ? 4u : 0u) | (SW & FPU_SW_C1 ? 2u : 0u) |
           (SW & FPU_SW_C0 ? 1u : 0u);
}

static void test_arith_basic(void) {
    printf("test_arith_basic\n");
    reset();
    RUN_OK(0xD9, 0xE8, 0xD9, 0xE8, 0xDE, 0xC1);   /* fld1; fld1; faddp st(1),st */
    CHECK_LD(ST(0), 2.0L); CHECK_EQ(fpu_depth(&FPU), 1);
    {   /* memory operand, ST0 op m32: st0 = 10, operand 4 */
        static const struct { uint8_t reg; long double want; } t[] = {
            {0, 14.0L}, {1, 40.0L}, {4, 6.0L}, {5, -6.0L}, {6, 2.5L}, {7, 0.4L}};
        size_t k;
        for (k = 0; k < 6; k++) {
            reset(); ld_d(10.0);
            put_f(A_OFF, 4.0f);
            RUN_OK(0xD8, MA(t[k].reg));
            CHECK_LD(ST(0), t[k].want); CHECK_EQ(fpu_depth(&FPU), 1);
            reset(); ld_d(10.0);
            put_d(A_OFF, 4.0);
            RUN_OK(0xDC, MA(t[k].reg));                /* same with a qword operand */
            CHECK_LD(ST(0), t[k].want);
            reset(); ld_d(10.0);
            put_i32(A_OFF, 4);
            RUN_OK(0xDA, MA(t[k].reg));                /* fiadd/fimul/... dword */
            CHECK_LD(ST(0), t[k].want);
            reset(); ld_d(10.0);
            put_i16(A_OFF, 4);
            RUN_OK(0xDE, MA(t[k].reg));                /* word integer forms */
            CHECK_LD(ST(0), t[k].want);
        }
    }
    {   /* register forms: st0 = 10, st1 = 3 */
        static const struct { uint8_t d8, dc, de; long double r0, r1, rp; } t[] = {
            /* d8: st0 result; dc: st1 result; de: st0 after the pop */
            {0xC1, 0xC1, 0xC1, 13.0L, 13.0L, 13.0L},
            {0xC9, 0xC9, 0xC9, 30.0L, 30.0L, 30.0L},
            {0xE1, 0xE1, 0xE1, 7.0L, 7.0L, 7.0L},       /* dc/de e1: st1 = st0 - st1 */
            {0xE9, 0xE9, 0xE9, -7.0L, -7.0L, -7.0L},    /* st(0)=3-10 / st1 = 3-10 */
            {0xF1, 0xF1, 0xF1, 10.0L / 3.0L, 10.0L / 3.0L, 10.0L / 3.0L},
            {0xF9, 0xF9, 0xF9, 0.3L, 0.3L, 0.3L},
        };
        size_t k;
        for (k = 0; k < 6; k++) {
            long double want_d8 = t[k].r0;
            /* d8 e9 is fsubr st0,st1 = st1 - st0 = -7; d8 e1 fsub = 7; d8 f1 = 10/3; d8 f9 = 3/10 */
            reset(); ld_d(3.0); ld_d(10.0);
            RUN_OK(0xD8, t[k].d8);
            CHECK_LD(ST(0), want_d8); CHECK_LD(ST(1), 3.0L);
            reset(); ld_d(3.0); ld_d(10.0);
            RUN_OK(0xDC, t[k].dc);
            CHECK_LD(ST(0), 10.0L); CHECK_LD(ST(1), t[k].r1);
            reset(); ld_d(3.0); ld_d(10.0);
            RUN_OK(0xDE, t[k].de);
            CHECK_EQ(fpu_depth(&FPU), 1); CHECK_LD(ST(0), t[k].rp);
        }
    }
    /* st(i) other than 1, and st0 op st0 */
    reset(); ld_d(1.0); ld_d(2.0); ld_d(4.0);
    RUN_OK(0xD8, 0xC2);                           /* fadd st0, st2 */
    CHECK_LD(ST(0), 5.0L);
    RUN_OK(0xD8, 0xC8);                           /* fmul st0, st0 */
    CHECK_LD(ST(0), 25.0L);
    RUN_OK(0xDC, 0xC2);                           /* fadd st2, st0 */
    CHECK_LD(ST(2), 26.0L);
    RUN_OK(0xDE, 0xC2);                           /* faddp st2, st0 */
    CHECK_EQ(fpu_depth(&FPU), 2); CHECK_LD(ST(1), 51.0L); CHECK_LD(ST(0), 2.0L);
}

static void test_arith_special(void) {
    printf("test_arith_special\n");
    reset();
    ld_d(0.0); ld_d(1.0);
    RUN_OK(0xD8, 0xF1);                           /* fdiv st0, st1: 1/0 */
    CHECK(isinf(ST(0))); CHECK(ST(0) > 0); CHECK_EQ(SW & (FPU_SW_ZE | FPU_SW_IE), FPU_SW_ZE);
    reset();
    ld_d(0.0); ld_d(-1.0);
    RUN_OK(0xD8, 0xF1); CHECK(isinf(ST(0))); CHECK(ST(0) < 0);
    reset();
    ld_d(0.0); ld_d(0.0);
    RUN_OK(0xD8, 0xF1);                           /* 0/0 */
    CHECK(isnan(ST(0))); CHECK(signbit(ST(0))); CHECK_EQ(SW & FPU_SW_IE, FPU_SW_IE);
    reset();
    put_m80(A_OFF, "7FFF8000000000000000");
    RUN_OK(0xDB, MA(5), 0xDB, MA(5));
    RUN_OK(0xDE, 0xE9);                           /* inf - inf */
    CHECK(isnan(ST(0))); CHECK_EQ(SW & FPU_SW_IE, FPU_SW_IE);
    reset();
    put_m80(A_OFF, "7FFE8000000000000000");       /* huge finite */
    RUN_OK(0xDB, MA(5), 0xDB, MA(5));
    RUN_OK(0xDE, 0xC9);                           /* fmulp: overflows to infinity */
    CHECK(isinf(ST(0))); CHECK_EQ(SW & FPU_SW_OE, FPU_SW_OE);
    reset();
    ld_d(-0.0); ld_d(0.0);
    RUN_OK(0xDE, 0xC1); CHECK_LD(ST(0), 0.0L); CHECK(!signbit(ST(0)));
    /* NaN operands propagate quietly */
    reset();
    put_m80(A_OFF, "7FFFC000000000000000");
    RUN_OK(0xDB, MA(5)); ld_d(1.0);
    RUN_OK(0xDE, 0xC1); CHECK(isnan(ST(0)));
    /* empty operand: stack fault gives the indefinite */
    reset();
    ld_d(1.0);
    RUN_OK(0xD8, 0xC2);                           /* fadd st0, st2 (empty) */
    CHECK(isnan(ST(0))); CHECK_EQ(SW & (FPU_SW_IE | FPU_SW_SF), FPU_SW_IE | FPU_SW_SF);
    /* precision control */
    reset();
    set_cw(0x007F);                               /* 24-bit */
    RUN_OK(0xD9, 0xE8); ld_d(3.0); RUN_OK(0xDE, 0xF9);   /* st1 = st1 / st0 = 1/3, pop */
    CHECK(ST(0) == (long double)(float)(1.0L / 3.0L));
    reset();
    set_cw(0x027F);                               /* 53-bit */
    RUN_OK(0xD9, 0xE8); ld_d(3.0); RUN_OK(0xDE, 0xF9);
    CHECK(ST(0) == (long double)(double)(1.0L / 3.0L));
    reset();
    RUN_OK(0xD9, 0xE8); ld_d(3.0); RUN_OK(0xDE, 0xF9);
    CHECK(ST(0) == 1.0L / 3.0L);                  /* 64-bit mantissa */
    /* rounding control: down < exact < up, one ulp apart */
    {
        long double down, up;
        reset(); set_cw(0x077F);
        RUN_OK(0xD9, 0xE8); ld_d(3.0); RUN_OK(0xDE, 0xF9);
        down = ST(0);
        reset(); set_cw(0x0B7F);
        RUN_OK(0xD9, 0xE8); ld_d(3.0); RUN_OK(0xDE, 0xF9);
        up = ST(0);
        CHECK(down < up); CHECK(nextafterl(down, 1.0L) == up);
        reset(); set_cw(0x0F7F);
        RUN_OK(0xD9, 0xE8); ld_d(3.0); RUN_OK(0xDE, 0xF9);
        CHECK(ST(0) == down);                     /* truncation of a positive value */
    }
}

static void fcom_flags(uint16_t *ax_out) { /* fnstsw ax */
    RUN_OK(0xDF, 0xE0);
    *ax_out = (uint16_t)g_cpu.r[CPU_EAX];
}

static void test_compare(void) {
    uint16_t ax;
    printf("test_compare\n");
    /* st0 = a, operand b -> condition codes */
    {
        static const struct { double a, b; unsigned cc; } t[] = {
            {2.0, 1.0, 0}, {1.0, 2.0, 1}, {3.0, 3.0, 8}, {-1.0, 1.0, 1}, {0.0, -0.0, 8}};
        size_t k;
        for (k = 0; k < 5; k++) {
            reset(); ld_d(t[k].a);
            put_f(A_OFF, (float)t[k].b);
            RUN_OK(0xD8, MA(2));                  /* fcom dword */
            CHECK_EQ(cc_bits(), t[k].cc); CHECK_EQ(fpu_depth(&FPU), 1);
            RUN_OK(0xD8, MA(3));                  /* fcomp dword */
            CHECK_EQ(cc_bits(), t[k].cc); CHECK_EQ(fpu_depth(&FPU), 0);
            reset(); ld_d(t[k].a);
            put_d(A_OFF, t[k].b);
            RUN_OK(0xDC, MA(2)); CHECK_EQ(cc_bits(), t[k].cc);
            RUN_OK(0xDC, MA(3)); CHECK_EQ(fpu_depth(&FPU), 0);
            reset(); ld_d(t[k].a);
            put_i32(A_OFF, (int32_t)t[k].b);
            RUN_OK(0xDA, MA(2)); CHECK_EQ(cc_bits(), t[k].cc);
            RUN_OK(0xDA, MA(3)); CHECK_EQ(fpu_depth(&FPU), 0);
            reset(); ld_d(t[k].a);
            put_i16(A_OFF, (int16_t)t[k].b);
            RUN_OK(0xDE, MA(2)); CHECK_EQ(cc_bits(), t[k].cc);
            RUN_OK(0xDE, MA(3)); CHECK_EQ(fpu_depth(&FPU), 0);
            reset(); ld_d(t[k].b); ld_d(t[k].a);  /* st0 = a, st1 = b */
            RUN_OK(0xD8, 0xD1); CHECK_EQ(cc_bits(), t[k].cc); CHECK_EQ(fpu_depth(&FPU), 2);
            RUN_OK(0xD8, 0xD9); CHECK_EQ(cc_bits(), t[k].cc); CHECK_EQ(fpu_depth(&FPU), 1);
            reset(); ld_d(t[k].b); ld_d(t[k].a);
            RUN_OK(0xDE, 0xD9); CHECK_EQ(cc_bits(), t[k].cc); CHECK_EQ(fpu_depth(&FPU), 0);
            reset(); ld_d(t[k].b); ld_d(t[k].a);
            RUN_OK(0xDD, 0xE1); CHECK_EQ(cc_bits(), t[k].cc); CHECK_EQ(fpu_depth(&FPU), 2);
            RUN_OK(0xDD, 0xE9); CHECK_EQ(cc_bits(), t[k].cc); CHECK_EQ(fpu_depth(&FPU), 1);
            reset(); ld_d(t[k].b); ld_d(t[k].a);
            RUN_OK(0xDA, 0xE9); CHECK_EQ(cc_bits(), t[k].cc); CHECK_EQ(fpu_depth(&FPU), 0);
        }
    }
    /* FSTSW AX; SAHF mapping: C0 -> CF, C2 -> PF, C3 -> ZF */
    reset(); ld_d(2.0); ld_d(1.0);               /* st0 = 1 < st1 = 2 */
    RUN_OK(0xD8, 0xD1, 0x9B, 0xDF, 0xE0, 0x9E);
    CHECK_EQ(g_cpu.eflags & (CPU_CF | CPU_PF | CPU_ZF), CPU_CF);
    reset(); ld_d(2.0); ld_d(2.0);
    RUN_OK(0xD8, 0xD1, 0xDF, 0xE0, 0x9E);
    CHECK_EQ(g_cpu.eflags & (CPU_CF | CPU_PF | CPU_ZF), CPU_ZF);
    reset(); ld_d(1.0); ld_d(2.0);
    RUN_OK(0xD8, 0xD1, 0xDF, 0xE0, 0x9E);
    CHECK_EQ(g_cpu.eflags & (CPU_CF | CPU_PF | CPU_ZF), 0);
    /* unordered: NaN. FCOM raises IE, FUCOM does not */
    reset();
    put_m80(A_OFF, "7FFFC000000000000000");
    RUN_OK(0xDB, MA(5)); ld_d(1.0);
    RUN_OK(0xD8, 0xD1); CHECK_EQ(cc_bits(), 8u | 4u | 1u); CHECK_EQ(SW & FPU_SW_IE, FPU_SW_IE);
    RUN_OK(0xDB, 0xE2);
    RUN_OK(0xDD, 0xE1); CHECK_EQ(cc_bits(), 8u | 4u | 1u); CHECK_EQ(SW & FPU_SW_IE, 0);
    RUN_OK(0xDF, 0xE0, 0x9E);
    CHECK_EQ(g_cpu.eflags & (CPU_CF | CPU_PF | CPU_ZF), CPU_CF | CPU_PF | CPU_ZF);
    /* compare with an empty register: indefinite, unordered, IE | SF */
    reset(); ld_d(1.0);
    RUN_OK(0xD8, 0xD2);
    CHECK_EQ(cc_bits() & 0xD, 0xD); CHECK_EQ(SW & (FPU_SW_IE | FPU_SW_SF), FPU_SW_IE | FPU_SW_SF);
    /* FTST */
    reset(); ld_d(0.0);
    RUN_OK(0xD9, 0xE4); CHECK_EQ(cc_bits(), 8u);
    reset(); ld_d(-3.0);
    RUN_OK(0xD9, 0xE4); CHECK_EQ(cc_bits(), 1u);
    reset(); ld_d(3.0);
    RUN_OK(0xD9, 0xE4); CHECK_EQ(cc_bits(), 0u); CHECK_EQ(fpu_depth(&FPU), 1);
    fcom_flags(&ax);
    CHECK_EQ(ax & 0x4700, 0);
}

static void test_fxam(void) {
    static const struct { const char *hex; unsigned cc; } t[] = {
        {"3FFF8000000000000000", 4u},            /* +1.0: C2 */
        {"BFFF8000000000000000", 4u | 2u},       /* -1.0: C2, C1 = sign */
        {"00000000000000000000", 8u},            /* +0: C3 */
        {"80000000000000000000", 8u | 2u},       /* -0 */
        {"7FFF8000000000000000", 4u | 1u},       /* +inf: C2 C0 */
        {"FFFF8000000000000000", 4u | 2u | 1u},  /* -inf */
        {"7FFFC000000000000000", 1u},            /* NaN: C0 */
        {"00000000000000000001", 8u | 4u},       /* denormal: C3 C2 */
    };
    size_t k;
    printf("test_fxam\n");
    for (k = 0; k < sizeof t / sizeof t[0]; k++) {
        reset();
        put_m80(A_OFF, t[k].hex);
        RUN_OK(0xDB, MA(5), 0xD9, 0xE5);
        CHECK_EQ(cc_bits(), t[k].cc);
        CHECK_EQ(fpu_depth(&FPU), 1);
    }
    reset();
    RUN_OK(0xD9, 0xE5);                           /* empty: C3 C0 */
    CHECK_EQ(cc_bits(), 8u | 1u); CHECK_EQ(SW & FPU_SW_IE, 0);
}


/* ---- group 3: unary, remainder, scaling, transcendental ------------------ */

static void ld_m80(const char *hex) {
    put_m80(0x270, hex);
    RUN_OK(0xDB, 0x2E, 0x70, 0x02); /* fld tbyte [0x270] */
}

static void test_unary(void) {
    printf("test_unary\n");
    reset(); ld_d(2.5);
    RUN_OK(0xD9, 0xE0); CHECK_LD(ST(0), -2.5L);                /* fchs */
    RUN_OK(0xD9, 0xE0); CHECK_LD(ST(0), 2.5L);
    RUN_OK(0xD9, 0xE1); CHECK_LD(ST(0), 2.5L);                 /* fabs */
    reset(); ld_d(-0.0);
    RUN_OK(0xD9, 0xE1); CHECK(!signbit(ST(0)));
    RUN_OK(0xD9, 0xE0); CHECK(signbit(ST(0))); CHECK_LD(ST(0), 0.0L);
    reset(); ld_m80("FFFF8000000000000000");                    /* -inf */
    RUN_OK(0xD9, 0xE1); CHECK(isinf(ST(0)) && ST(0) > 0);
    reset(); RUN_OK(0xD9, 0xE0);                               /* fchs on an empty stack */
    CHECK(isnan(ST(0))); CHECK_EQ(SW & (FPU_SW_IE | FPU_SW_SF), FPU_SW_IE | FPU_SW_SF);
    /* fsqrt */
    reset(); ld_d(4.0);
    RUN_OK(0xD9, 0xFA); CHECK_LD(ST(0), 2.0L);
    reset(); ld_d(2.0);
    RUN_OK(0xD9, 0xFA); CHECK(ST(0) == sqrtl(2.0L));
    reset(); ld_d(-1.0);
    RUN_OK(0xD9, 0xFA); CHECK(isnan(ST(0))); CHECK_EQ(SW & FPU_SW_IE, FPU_SW_IE);
    reset(); ld_d(-0.0);
    RUN_OK(0xD9, 0xFA); CHECK(signbit(ST(0))); CHECK_EQ(SW & FPU_SW_IE, 0);
    /* frndint follows RC */
    {
        static const struct { uint16_t rc; double in; double out; } t[] = {
            {0, 2.5, 2}, {0, 3.5, 4}, {0, -2.5, -2}, {0, 2.6, 3}, {1, -2.5, -3}, {1, 2.9, 2},
            {2, 2.1, 3}, {2, -2.9, -2}, {3, -2.9, -2}, {3, 2.9, 2}};
        size_t k;
        for (k = 0; k < sizeof t / sizeof t[0]; k++) {
            reset(); set_cw((uint16_t)(FPU_CW_DEFAULT | t[k].rc << 10));
            ld_d(t[k].in);
            RUN_OK(0xD9, 0xFC);
            CHECK_LD(ST(0), (long double)t[k].out); CHECK_EQ(fpu_depth(&FPU), 1);
        }
    }
}

static void test_scale_extract(void) {
    printf("test_scale_extract\n");
    reset(); ld_d(4.0); ld_d(3.0);                             /* st0 = 3, st1 = 4 */
    RUN_OK(0xD9, 0xFD);                                        /* fscale: 3 * 2^4 */
    CHECK_LD(ST(0), 48.0L); CHECK_LD(ST(1), 4.0L); CHECK_EQ(fpu_depth(&FPU), 2);
    reset(); ld_d(-1.5); ld_d(3.0);
    RUN_OK(0xD9, 0xFD); CHECK_LD(ST(0), 1.5L);                 /* scale by trunc(-1.5) = -1 */
    reset(); ld_d(100000.0); ld_d(1.0);
    RUN_OK(0xD9, 0xFD); CHECK(isinf(ST(0)));                   /* overflow */
    reset(); ld_d(12.0);
    RUN_OK(0xD9, 0xF4);                                        /* fxtract */
    CHECK_EQ(fpu_depth(&FPU), 2); CHECK_LD(ST(0), 1.5L); CHECK_LD(ST(1), 3.0L);
    reset(); ld_d(-0.15625);                                   /* -1.25 * 2^-3 */
    RUN_OK(0xD9, 0xF4); CHECK_LD(ST(0), -1.25L); CHECK_LD(ST(1), -3.0L);
    reset(); ld_d(0.0);
    RUN_OK(0xD9, 0xF4);
    CHECK_LD(ST(0), 0.0L); CHECK(isinf(ST(1)) && ST(1) < 0); CHECK_EQ(SW & FPU_SW_ZE, FPU_SW_ZE);
    reset(); ld_m80("00000000000000000001");                    /* smallest denormal: 2^-16445 */
    RUN_OK(0xD9, 0xF4); CHECK_LD(ST(0), 1.0L); CHECK_LD(ST(1), -16445.0L);
}

static void test_prem(void) {
    printf("test_prem\n");
    {   /* FPREM: truncated quotient; Q0 -> C1, Q1 -> C3, Q2 -> C0 */
        static const struct { double x, y, r; } t[] = {
            {10, 3, 1}, {-17, 5, -2}, {100, 3, 1}, {7, 2, 1}, {1, 3, 1}, {32, 3, 2}, {40, 3, 1}};
        size_t k;
        for (k = 0; k < sizeof t / sizeof t[0]; k++) {
            unsigned q = (unsigned)(long long)(fabs(t[k].x) / t[k].y) & 7u;
            unsigned want = (q & 1u ? 2u : 0u) | (q & 2u ? 8u : 0u) | (q & 4u ? 1u : 0u);
            reset(); ld_d(t[k].y); ld_d(t[k].x);
            RUN_OK(0xD9, 0xF8);
            CHECK_LD(ST(0), (long double)t[k].r); CHECK_LD(ST(1), (long double)t[k].y);
            CHECK_EQ(cc_bits(), want);                         /* C2 = 0: complete */
            CHECK_EQ(fpu_depth(&FPU), 2);
        }
    }
    {   /* huge exponent difference: partial remainder, C2 = 1 until finished */
        unsigned rounds = 0;
        reset(); ld_d(3.0); ld_m80("40638000000000000000");   /* st0 = 2^100 */
        RUN_OK(0xD9, 0xF8);
        CHECK_EQ(SW & FPU_SW_C2, FPU_SW_C2);
        while ((SW & FPU_SW_C2) && rounds++ < 10) RUN_OK(0xD9, 0xF8);
        CHECK(rounds < 10); CHECK_EQ(SW & FPU_SW_C2, 0);
        CHECK_LD(ST(0), fmodl(ldexpl(1.0L, 100), 3.0L));
    }
    /* FPREM1: IEEE (nearest) quotient */
    {
        static const struct { double x, y, r; unsigned q; } t[] = {
            {10, 3, 1, 3}, {11, 3, -1, 4}, {8, 3, -1, 3}, {5, 2, 1, 2}, {7, 2, -1, 4}, {-11, 3, 1, 4}};
        size_t k;
        for (k = 0; k < sizeof t / sizeof t[0]; k++) {
            unsigned want = (t[k].q & 1u ? 2u : 0u) | (t[k].q & 2u ? 8u : 0u) | (t[k].q & 4u ? 1u : 0u);
            reset(); ld_d(t[k].y); ld_d(t[k].x);
            RUN_OK(0xD9, 0xF5);
            CHECK_LD(ST(0), (long double)t[k].r); CHECK_EQ(cc_bits(), want);
        }
    }
    /* invalid: divisor zero, dividend infinite */
    reset(); ld_d(0.0); ld_d(5.0);
    RUN_OK(0xD9, 0xF8); CHECK(isnan(ST(0))); CHECK_EQ(SW & FPU_SW_IE, FPU_SW_IE);
    reset(); ld_d(5.0); ld_m80("7FFF8000000000000000");
    RUN_OK(0xD9, 0xF5); CHECK(isnan(ST(0))); CHECK_EQ(SW & FPU_SW_IE, FPU_SW_IE);
    reset(); ld_m80("7FFF8000000000000000"); ld_d(5.0);        /* x finite, y infinite: x unchanged */
    RUN_OK(0xD9, 0xF8); CHECK_LD(ST(0), 5.0L); CHECK_EQ(SW & FPU_SW_C2, 0);
}

static void test_transcendental(void) {
    const long double pi = 3.14159265358979323846264338327950288L;
    printf("test_transcendental\n");
    /* F2XM1 */
    reset(); ld_d(1.0);
    RUN_OK(0xD9, 0xF0); CHECK_NEAR(ST(0), 1.0L, 1e-18L);
    reset(); ld_d(0.5);
    RUN_OK(0xD9, 0xF0); CHECK_NEAR(ST(0), 0.41421356237309504880168872420969807L, 1e-18L);
    reset(); ld_d(-1.0);
    RUN_OK(0xD9, 0xF0); CHECK_NEAR(ST(0), -0.5L, 1e-18L);
    reset(); ld_d(0.0);
    RUN_OK(0xD9, 0xF0); CHECK_LD(ST(0), 0.0L);
    /* FYL2X: st1 * log2(st0), pop */
    reset(); ld_d(3.0); ld_d(8.0);
    RUN_OK(0xD9, 0xF1); CHECK_NEAR(ST(0), 9.0L, 1e-18L); CHECK_EQ(fpu_depth(&FPU), 1);
    reset(); ld_d(5.0); ld_d(1.0);
    RUN_OK(0xD9, 0xF1); CHECK_LD(ST(0), 0.0L);
    reset(); ld_d(1.0); ld_d(0.0);
    RUN_OK(0xD9, 0xF1); CHECK(isinf(ST(0)) && ST(0) < 0); CHECK_EQ(SW & FPU_SW_ZE, FPU_SW_ZE);
    reset(); ld_d(1.0); ld_d(-2.0);
    RUN_OK(0xD9, 0xF1); CHECK(isnan(ST(0))); CHECK_EQ(SW & FPU_SW_IE, FPU_SW_IE);
    /* FYL2XP1: st1 * log2(st0 + 1) */
    reset(); ld_d(5.0); ld_d(1.0);
    RUN_OK(0xD9, 0xF9); CHECK_NEAR(ST(0), 5.0L, 1e-18L); CHECK_EQ(fpu_depth(&FPU), 1);
    reset(); ld_d(5.0); ld_d(0.0);
    RUN_OK(0xD9, 0xF9); CHECK_LD(ST(0), 0.0L);
    /* FPATAN: atan2(st1, st0), pop */
    reset(); ld_d(1.0); ld_d(1.0);
    RUN_OK(0xD9, 0xF3); CHECK_NEAR(ST(0), pi / 4, 1e-18L); CHECK_EQ(fpu_depth(&FPU), 1);
    reset(); ld_d(1.0); ld_d(-1.0);
    RUN_OK(0xD9, 0xF3); CHECK_NEAR(ST(0), 3 * pi / 4, 1e-18L);
    reset(); ld_d(0.0); ld_d(-1.0);
    RUN_OK(0xD9, 0xF3); CHECK_NEAR(ST(0), pi, 1e-18L);
    reset(); ld_d(-1.0); ld_d(0.0);
    RUN_OK(0xD9, 0xF3); CHECK_NEAR(ST(0), -pi / 2, 1e-18L);
    /* FSIN / FCOS / FSINCOS / FPTAN on a double-representable angle */
    reset(); ld_d(0.5);
    RUN_OK(0xD9, 0xFE); CHECK_NEAR(ST(0), 0.479425538604203L, 1e-15L); CHECK_EQ(SW & FPU_SW_C2, 0);
    reset(); ld_d(0.5);
    RUN_OK(0xD9, 0xFF); CHECK_NEAR(ST(0), 0.8775825618903728L, 1e-15L);
    reset(); ld_d(0.5);
    RUN_OK(0xD9, 0xFB);
    CHECK_EQ(fpu_depth(&FPU), 2); CHECK_NEAR(ST(0), 0.8775825618903728L, 1e-15L);
    CHECK_NEAR(ST(1), 0.479425538604203L, 1e-15L);
    reset(); ld_d(0.5);
    RUN_OK(0xD9, 0xF2);
    CHECK_EQ(fpu_depth(&FPU), 2); CHECK_LD(ST(0), 1.0L); CHECK_NEAR(ST(1), 0.5463024898437905L, 1e-15L);
    CHECK_EQ(SW & FPU_SW_C2, 0);
    reset(); ld_d(0.0);
    RUN_OK(0xD9, 0xFE); CHECK_LD(ST(0), 0.0L);
    reset(); ld_d(0.0);
    RUN_OK(0xD9, 0xFF); CHECK_LD(ST(0), 1.0L);
    /* out of range operand (|x| >= 2^63): C2 set, operand unchanged */
    reset(); ld_m80("403E8000000000000000");                   /* 2^63 */
    RUN_OK(0xD9, 0xFE);
    CHECK_EQ(SW & FPU_SW_C2, FPU_SW_C2); CHECK_LD(ST(0), ldexpl(1.0L, 63)); CHECK_EQ(fpu_depth(&FPU), 1);
    reset(); ld_m80("403E8000000000000000");
    RUN_OK(0xD9, 0xF2);
    CHECK_EQ(SW & FPU_SW_C2, FPU_SW_C2); CHECK_EQ(fpu_depth(&FPU), 1);
    reset(); ld_m80("7FFF8000000000000000");                   /* infinity: invalid */
    RUN_OK(0xD9, 0xFE); CHECK(isnan(ST(0))); CHECK_EQ(SW & FPU_SW_IE, FPU_SW_IE);
}


/* ---- group 4: WIN87EM interrupt forms and linker-emitted sequences -------- */

static void test_int_forms(void) {
    cpu_insn in;
    cpu_mem m;
    printf("test_int_forms\n");
    reset();
    RUN_OK(0xCD, 0x35, 0xE8);                      /* int 35h = esc D9: fld1 */
    CHECK_EQ(fpu_depth(&FPU), 1); CHECK_LD(ST(0), 1.0L);
    put_f(A_OFF, 2.5f);
    RUN_OK(0xCD, 0x35, MA(0));                     /* fld dword [A] */
    CHECK_LD(ST(0), 2.5L);
    reset();
    CHECK_EQ(RUN(0xCD, 0x36, 0xC1), CPU_E_UD);     /* fcmovb is not a 387 */
    CHECK_EQ(g_cpu.eip, CODE_OFF);
    reset();
    RUN_OK(0xCD, 0x34, 0xC0 | 0, 0x90);            /* int 34h = esc D8: fadd st0,st0 on empty */
    reset();
    put_d(A_OFF, 6.25);
    RUN_OK(0xCD, 0x39, MA(0));                     /* int 39h = esc DD: fld qword */
    CHECK_LD(ST(0), 6.25L);
    RUN_OK(0xCD, 0x3B, 0xE0);                      /* int 3bh = esc DF: fnstsw ax */
    CHECK_EQ(g_cpu.r[CPU_EAX] & 0xFFFF, 7u << 11);
    RUN_OK(0xCD, 0x39, 0xD8);                      /* fstp st(0) */
    CHECK_EQ(fpu_depth(&FPU), 0);
    /* segment override forms: 3Ch = ES:, 3Eh = CS:, then the escape byte and ModRM */
    reset();
    memcpy(g_mem + ESB, &(float){4.5f}, 4);
    RUN_OK(0xCD, 0x3C, 0xD9, 0x06, 0x00, 0x00);
    CHECK_LD(ST(0), 4.5L);
    memcpy(g_mem + CSB + 0x200, &(float){3.5f}, 4);
    RUN_OK(0xCD, 0x3E, 0xD9, 0x06, 0x00, 0x02);
    CHECK_LD(ST(0), 3.5L); CHECK_EQ(fpu_depth(&FPU), 2);
    CHECK_EQ(RUN(0xCD, 0x3C, 0x90), CPU_E_UD);     /* override not followed by an escape */
    /* 3Dh = FWAIT; 3Fh = overlay manager: not an FPU call */
    reset();
    RUN_OK(0xD9, 0xE8, 0xCD, 0x3D);
    CHECK_EQ(fpu_depth(&FPU), 1);
    CHECK_EQ(RUN(0xCD, 0x3F), CPU_E_FPU); CHECK_EQ(g_cpu.eip, CODE_OFF);
    /* length decoder */
    memset(&m, 0, sizeof m);
    m.read8 = t_r8; m.write8 = t_w8; m.sel_valid = t_valid;
#define LEN(n, sup, ...) do { memset(g_mem + CSB + CODE_OFF, 0, 32);              \
        memcpy(g_mem + CSB + CODE_OFF, (const uint8_t[]){__VA_ARGS__}, n);       \
        CHECK_EQ(cpu_decode(&m, S_CS, CODE_OFF, &in), CPU_OK);                   \
        CHECK_EQ(in.length, n); CHECK(in.fpu); CHECK_EQ(in.supported, sup); } while (0)
    LEN(3, true, 0xCD, 0x35, 0xE8);
    LEN(5, true, 0xCD, 0x35, 0x06, 0x00, 0x02);
    LEN(4, true, 0xCD, 0x3B, 0x46, 0x04);
    LEN(6, true, 0xCD, 0x3C, 0xD9, 0x06, 0x00, 0x00);
    LEN(6, true, 0xCD, 0x3E, 0xD9, 0x06, 0x00, 0x00);
    LEN(2, true, 0xCD, 0x3D);
    LEN(2, false, 0xCD, 0x3F);
    LEN(3, false, 0xCD, 0x3C, 0x90);
    LEN(8, true, 0x67, 0xCD, 0x35, 0x05, 0x00, 0x02, 0x00, 0x00);
#undef LEN
    memset(g_mem + CSB + CODE_OFF, 0, 8);
    g_mem[CSB + CODE_OFF] = 0xCD; g_mem[CSB + CODE_OFF + 1] = 0x21;
    CHECK_EQ(cpu_decode(&m, S_CS, CODE_OFF, &in), CPU_OK);
    CHECK_EQ(in.length, 2); CHECK(!in.fpu); CHECK(in.supported);
}

/* The truncating-conversion helper the Microsoft C runtime emits (WRITER 5:1f14):
 * WAIT / FNSTCW / FLDCW / FISTP qword, byte for byte as linked. */
static void test_linked_sequences(void) {
    static const uint8_t seq[] = {
        0x9B, 0xD9, 0x7E, 0xFC,        /* wait; fnstcw [bp-4] */
        0x90, 0x9B,                    /* nop; wait */
        0x8B, 0x46, 0xFC,              /* mov ax,[bp-4] */
        0x80, 0xCC, 0x0C,              /* or ah,0Ch   (RC = truncate) */
        0x89, 0x46, 0xFA,              /* mov [bp-6],ax */
        0x9B, 0xD9, 0x6E, 0xFA,        /* wait; fldcw [bp-6] */
        0x9B, 0xDF, 0x7E, 0xF2,        /* wait; fistp qword [bp-0eh] */
        0x9B, 0xD9, 0x6E, 0xFC,        /* wait; fldcw [bp-4] */
    };
    printf("test_linked_sequences\n");
    reset();
    g_cpu.r[CPU_EBP] = 0x1000;
    ld_d(-7.9);
    CHECK_EQ(RUN(0x9B, 0x90), CPU_HALT);
    CHECK_EQ(run_bytes(seq, sizeof seq), CPU_HALT);
    {
        int64_t v;
        memcpy(&v, g_mem + SSB + 0x1000 - 0x0E, 8);
        CHECK_EQ((uint64_t)v, (uint64_t)-7);
    }
    CHECK_EQ(FPU.cw, 0x037F); CHECK_EQ(fpu_depth(&FPU), 0);
    reset();
    g_cpu.r[CPU_EBP] = 0x1000;
    ld_d(7.9);
    CHECK_EQ(run_bytes(seq, sizeof seq), CPU_HALT);
    {
        int64_t v;
        memcpy(&v, g_mem + SSB + 0x1000 - 0x0E, 8);
        CHECK_EQ(v, 7);
    }
}

int main(void) {
    test_fld_fst();
    test_fild_fist();
    test_bcd();
    test_constants();
    test_stack_ops();
    test_control();
    test_env_save();
    test_addressing_and_faults();
    test_arith_basic();
    test_arith_special();
    test_compare();
    test_fxam();
    test_unary();
    test_scale_extract();
    test_prem();
    test_transcendental();
    test_int_forms();
    test_linked_sequences();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
