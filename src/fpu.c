/* x87 FPU. See fpu.h.
 *
 * Model: registers are long double (80-bit extended on x86-64). Exceptions are
 * always masked: invalid operations give the real indefinite (negative quiet
 * NaN), x/0 gives infinity, and the sticky bits in the status word record what
 * happened. The control word is stored as the program wrote it (the exception
 * masks are ignored), rounding control is honoured by integer stores,
 * FRNDINT and arithmetic, precision control by arithmetic (24/53/64 bits).
 *
 * Not implemented (decode as undefined, the CPU raises #UD):
 *   - 80387-and-later only encodings: FCMOVcc, FCOMI/FUCOMI, FISTTP, FXSAVE
 *   - FFREEP and the undocumented aliases (FSTP1, FXCH4, FCOM2, ...)
 *   - operand-size prefixed FSTENV/FLDENV/FSAVE/FRSTOR (32-bit layouts): the
 *     16-bit protected-mode layouts are always used
 *   - the FPU instruction/operand pointers are not tracked (stored as 0)
 *   - signalling NaNs are not distinguished from quiet ones; NaN payloads are
 *     not preserved through memory formats other than the indefinite
 *   - denormal operand/underflow exceptions (DE, UE) are never raised
 *   - FPREM/FPREM1 quotient bits are exact only for quotients below 2^63
 *   - FLDPI/FLDL2T/FLDL2E/FLDLN2/FLDLG2 return the nearest value whatever RC says
 *   - trigonometric results come from the host libm, not the 387 microcode, so
 *     they can differ from a real 387 in the last bits; FSIN/FCOS/FPTAN/FSINCOS
 *     operate on |x| < 2^63 (larger sets C2 and leaves the stack as is)
 */
#include <fenv.h>
#include <math.h>
#include <string.h>

#include "fpu.h"

void fpu_init(fpu_t *f) {
    memset(f, 0, sizeof *f);
    f->cw = FPU_CW_DEFAULT;
    f->empty = 0xFF;
}

unsigned fpu_top(const fpu_t *f) { return (f->sw >> FPU_SW_TOP_SHIFT) & 7u; }
long double fpu_st(const fpu_t *f, unsigned i) { return f->st[(fpu_top(f) + i) & 7u]; }
unsigned fpu_depth(const fpu_t *f) {
    unsigned n = 0, i;
    for (i = 0; i < 8; i++) n += !(f->empty >> i & 1u);
    return n;
}

static unsigned tag_of(const fpu_t *f, unsigned phys) {
    long double v;
    if (f->empty >> phys & 1u) return 3;
    v = f->st[phys];
    if (v == 0) return 1;
    switch (fpclassify(v)) {
    case FP_NORMAL: return 0;
    default: return 2;
    }
}

uint16_t fpu_tag_word(const fpu_t *f) {
    unsigned p, tw = 0;
    for (p = 0; p < 8; p++) tw |= tag_of(f, p) << (2 * p);
    return (uint16_t)tw;
}

/* ---- 80-bit memory format ------------------------------------------------ */

static long double tb_to_ld(const uint8_t *b) {
    uint64_t m = 0;
    unsigned se = (unsigned)(b[8] | b[9] << 8), e = se & 0x7FFF;
    long double r;
    int i;
    for (i = 7; i >= 0; i--) m = m << 8 | b[i];
    if (e == 0x7FFF) r = (m << 1) == 0 ? (long double)INFINITY : nanl("");
    else r = ldexpl((long double)m, (int)(e ? e : 1) - 16383 - 63);
    return se & 0x8000 ? -r : r;
}

static void ld_to_tb(long double x, uint8_t *b) {
    uint64_t m;
    unsigned e;
    int i, ex;
    if (isnan(x)) { e = 0x7FFF; m = 0xC000000000000000ull; }
    else if (isinf(x)) { e = 0x7FFF; m = 0x8000000000000000ull; }
    else if (x == 0) { e = 0; m = 0; }
    else {
        long double a = fabsl(x), fr = frexpl(a, &ex);
        if (ex - 1 + 16383 <= 0) { e = 0; m = (uint64_t)ldexpl(a, 16382 + 63); }
        else { e = (unsigned)(ex - 1 + 16383); m = (uint64_t)ldexpl(fr, 64); }
    }
    for (i = 0; i < 8; i++) { b[i] = (uint8_t)m; m >>= 8; }
    b[8] = (uint8_t)e;
    b[9] = (uint8_t)(e >> 8 | (signbit(x) ? 0x80 : 0));
}

static long double indefinite(void) { return -nanl(""); }

/* ---- stack and exceptions ------------------------------------------------ */

static void set_top(fpu_t *f, unsigned t) {
    f->sw = (uint16_t)((f->sw & ~(7u << FPU_SW_TOP_SHIFT)) | (t & 7u) << FPU_SW_TOP_SHIFT);
}

/* ST(i) as an operand: an empty register is a stack fault giving the indefinite. */
static long double get(fpu_t *f, unsigned i) {
    unsigned p = (fpu_top(f) + i) & 7u;
    if (f->empty >> p & 1u) {
        f->sw = (uint16_t)((f->sw | FPU_SW_IE | FPU_SW_SF) & ~FPU_SW_C1);
        return indefinite();
    }
    return f->st[p];
}

static void put(fpu_t *f, unsigned i, long double v) {
    unsigned p = (fpu_top(f) + i) & 7u;
    f->st[p] = v;
    f->empty &= (uint8_t)~(1u << p);
}

static void push(fpu_t *f, long double v) {
    unsigned t = (fpu_top(f) - 1) & 7u;
    f->sw &= (uint16_t)~FPU_SW_C1;
    if (!(f->empty >> t & 1u)) { /* overflow */
        f->sw |= FPU_SW_IE | FPU_SW_SF | FPU_SW_C1;
        v = indefinite();
    }
    set_top(f, t);
    f->st[t] = v;
    f->empty &= (uint8_t)~(1u << t);
}

static void pop(fpu_t *f) {
    unsigned t = fpu_top(f);
    f->empty |= (uint8_t)(1u << t);
    set_top(f, t + 1);
}

/* ---- rounding ----------------------------------------------------------- */

static unsigned rc_of(const fpu_t *f) { return f->cw >> 10 & 3u; }

static long double round_rc(long double x, unsigned rc) {
    if (!isfinite(x)) return x;
    switch (rc) {
    case 1: return floorl(x);
    case 2: return ceill(x);
    case 3: return truncl(x);
    default: {
        long double r = roundl(x);
        if (fabsl(x - truncl(x)) == 0.5L) r = 2 * roundl(x / 2);
        return r;
    }
    }
}

/* ---- memory conversions ------------------------------------------------- */

static void st_int(fpu_t *f, long double x, unsigned size, uint8_t *out) {
    long double v = round_rc(x, rc_of(f)), lim = ldexpl(1.0L, (int)size * 8 - 1);
    uint64_t u;
    unsigned i;
    if (isnan(v) || v >= lim || v < -lim) {
        f->sw |= FPU_SW_IE;
        u = 1ull << (size * 8 - 1);
    } else {
        if (v != x) f->sw |= FPU_SW_PE;
        u = (uint64_t)(int64_t)v;
    }
    for (i = 0; i < size; i++) out[i] = (uint8_t)(u >> (8 * i));
}

static long double ld_int(const uint8_t *in, unsigned size) {
    uint64_t u = 0;
    unsigned i;
    for (i = 0; i < size; i++) u |= (uint64_t)in[i] << (8 * i);
    if (size == 2) return (int16_t)u;
    if (size == 4) return (int32_t)u;
    return (long double)(int64_t)u;
}

static void st_bcd(fpu_t *f, long double x, uint8_t *out) {
    long double v = round_rc(x, rc_of(f));
    unsigned i;
    uint64_t u;
    if (isnan(v) || fabsl(v) >= 1e18L) {
        f->sw |= FPU_SW_IE;
        memset(out, 0, 7);
        out[7] = 0xC0; out[8] = 0xFF; out[9] = 0xFF;
        return;
    }
    if (v != x) f->sw |= FPU_SW_PE;
    u = (uint64_t)fabsl(v);
    for (i = 0; i < 9; i++) {
        out[i] = (uint8_t)((u % 10) | (u / 10 % 10) << 4);
        u /= 100;
    }
    out[9] = signbit(v) ? 0x80 : 0;
}

static long double ld_bcd(const uint8_t *in) {
    uint64_t u = 0;
    int i;
    for (i = 8; i >= 0; i--) u = u * 100 + (uint64_t)(in[i] >> 4 & 15) * 10 + (in[i] & 15);
    return in[9] & 0x80 ? -(long double)u : (long double)u;
}

static void st_real(fpu_t *f, long double x, unsigned size, uint8_t *out) {
    if (size == 10) { ld_to_tb(x, out); return; }
    if (size == 4) {
        float r = (float)x;
        if (!isnan(x) && (long double)r != x) f->sw |= FPU_SW_PE;
        if (isinf(r) && !isinf(x)) f->sw |= FPU_SW_OE;
        memcpy(out, &r, 4);
    } else {
        double r = (double)x;
        if (!isnan(x) && (long double)r != x) f->sw |= FPU_SW_PE;
        if (isinf(r) && !isinf(x)) f->sw |= FPU_SW_OE;
        memcpy(out, &r, 8);
    }
}

static long double ld_real(const uint8_t *in, unsigned size) {
    if (size == 10) return tb_to_ld(in);
    if (size == 4) { float r; memcpy(&r, in, 4); return r; }
    { double r; memcpy(&r, in, 8); return r; }
}

/* ---- environment (16-bit protected-mode layout) -------------------------- */

#define ENV_SIZE 14
#define SAVE_SIZE 94

static void w16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static unsigned r16(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }

static void st_env(fpu_t *f, uint8_t *out) {
    memset(out, 0, ENV_SIZE);
    w16(out, f->cw);
    w16(out + 2, f->sw);
    w16(out + 4, fpu_tag_word(f));
}

static void ld_env(fpu_t *f, const uint8_t *in) {
    unsigned tw = r16(in + 4), p;
    f->cw = (uint16_t)r16(in);
    f->sw = (uint16_t)r16(in + 2);
    f->empty = 0;
    for (p = 0; p < 8; p++)
        if ((tw >> (2 * p) & 3u) == 3u) f->empty |= (uint8_t)(1u << p);
}

/* ---- decoding ----------------------------------------------------------- */

enum {
    K_BAD, K_FLD_M, K_FST_M, K_FILD_M, K_FIST_M, K_FBLD, K_FBSTP, K_LDENV, K_STENV, K_LDCW,
    K_STCW, K_STSW_M, K_RSTOR, K_SAVE, K_FLD_ST, K_FXCH, K_FST_ST, K_FFREE, K_NOP, K_CONST,
    K_INCSTP, K_DECSTP, K_INIT, K_CLEX, K_STSW_AX,
    K_ARITH_REAL, K_ARITH_INT, K_ARITH_ST, K_COM_REAL, K_COM_INT, K_COM_ST, K_COMPP, K_TST, K_XAM, K_MISC
};

/* n: size, ST index or arithmetic op (the ModRM reg field); a: ST index of an
 * arithmetic/compare operand; dst: arithmetic result goes to ST(a), not ST(0);
 * unord: unordered compare (FUCOM). */
typedef struct { uint8_t kind, n, a, pop, rd, wr, dst, unord; } cls;

static cls mk(unsigned kind, unsigned n, unsigned pop, unsigned rd, unsigned wr) {
    cls c;
    memset(&c, 0, sizeof c);
    c.kind = (uint8_t)kind; c.n = (uint8_t)n; c.pop = (uint8_t)pop; c.rd = (uint8_t)rd; c.wr = (uint8_t)wr;
    return c;
}

/* D8/DA/DC/DE with a memory operand: ST(0) op m32real / m32int / m64real / m16int. */
static cls classify_arith_mem(unsigned esc, unsigned reg) {
    static const uint8_t size[4] = {4, 4, 8, 2};
    unsigned sz = size[esc >> 1];
    bool isint = (esc & 2u) != 0;
    if (reg == 2 || reg == 3) return mk(isint ? K_COM_INT : K_COM_REAL, reg, reg == 3, sz, 0);
    return mk(isint ? K_ARITH_INT : K_ARITH_REAL, reg, 0, sz, 0);
}

static cls classify_mem(unsigned esc, unsigned reg) {
    if ((esc & 1u) == 0 || esc == 2) return classify_arith_mem(esc, reg);
    switch (esc << 3 | reg) {
    case 1 << 3 | 0: return mk(K_FLD_M, 4, 0, 4, 0);
    case 1 << 3 | 2: return mk(K_FST_M, 4, 0, 0, 4);
    case 1 << 3 | 3: return mk(K_FST_M, 4, 1, 0, 4);
    case 1 << 3 | 4: return mk(K_LDENV, 0, 0, ENV_SIZE, 0);
    case 1 << 3 | 5: return mk(K_LDCW, 0, 0, 2, 0);
    case 1 << 3 | 6: return mk(K_STENV, 0, 0, 0, ENV_SIZE);
    case 1 << 3 | 7: return mk(K_STCW, 0, 0, 0, 2);
    case 3 << 3 | 0: return mk(K_FILD_M, 4, 0, 4, 0);
    case 3 << 3 | 2: return mk(K_FIST_M, 4, 0, 0, 4);
    case 3 << 3 | 3: return mk(K_FIST_M, 4, 1, 0, 4);
    case 3 << 3 | 5: return mk(K_FLD_M, 10, 0, 10, 0);
    case 3 << 3 | 7: return mk(K_FST_M, 10, 1, 0, 10);
    case 5 << 3 | 0: return mk(K_FLD_M, 8, 0, 8, 0);
    case 5 << 3 | 2: return mk(K_FST_M, 8, 0, 0, 8);
    case 5 << 3 | 3: return mk(K_FST_M, 8, 1, 0, 8);
    case 5 << 3 | 4: return mk(K_RSTOR, 0, 0, SAVE_SIZE, 0);
    case 5 << 3 | 6: return mk(K_SAVE, 0, 0, 0, SAVE_SIZE);
    case 5 << 3 | 7: return mk(K_STSW_M, 0, 0, 0, 2);
    case 7 << 3 | 0: return mk(K_FILD_M, 2, 0, 2, 0);
    case 7 << 3 | 2: return mk(K_FIST_M, 2, 0, 0, 2);
    case 7 << 3 | 3: return mk(K_FIST_M, 2, 1, 0, 2);
    case 7 << 3 | 4: return mk(K_FBLD, 0, 0, 10, 0);
    case 7 << 3 | 5: return mk(K_FILD_M, 8, 0, 8, 0);
    case 7 << 3 | 6: return mk(K_FBSTP, 0, 1, 0, 10);
    case 7 << 3 | 7: return mk(K_FIST_M, 8, 1, 0, 8);
    default: return mk(K_BAD, 0, 0, 0, 0);
    }
}

static cls classify_reg(unsigned esc, unsigned modrm) {
    unsigned reg = modrm >> 3 & 7, i = modrm & 7;
    cls c;
    switch (esc) {
    case 0:
        if (reg == 2 || reg == 3) { c = mk(K_COM_ST, 0, reg == 3, 0, 0); c.a = (uint8_t)i; return c; }
        c = mk(K_ARITH_ST, reg, 0, 0, 0); c.a = (uint8_t)i;
        return c;
    case 4: case 6:
        if (reg == 2 || reg == 3) {
            if (esc == 6 && modrm == 0xD9) return mk(K_COMPP, 0, 2, 0, 0);
            break;
        }
        c = mk(K_ARITH_ST, reg, esc == 6, 0, 0); c.a = (uint8_t)i; c.dst = 1;
        return c;
    case 1:
        if (reg == 0) return mk(K_FLD_ST, i, 0, 0, 0);
        if (reg == 1) return mk(K_FXCH, i, 0, 0, 0);
        if (modrm == 0xD0) return mk(K_NOP, 0, 0, 0, 0);
        if (modrm == 0xE0 || modrm == 0xE1 || (modrm >= 0xF0 && modrm != 0xF6 && modrm != 0xF7))
            return mk(K_MISC, modrm, 0, 0, 0);
        if (modrm == 0xE4) return mk(K_TST, 0, 0, 0, 0);
        if (modrm == 0xE5) return mk(K_XAM, 0, 0, 0, 0);
        if (modrm >= 0xE8 && modrm <= 0xEE) return mk(K_CONST, modrm - 0xE8, 0, 0, 0);
        if (modrm == 0xF6) return mk(K_DECSTP, 0, 0, 0, 0);
        if (modrm == 0xF7) return mk(K_INCSTP, 0, 0, 0, 0);
        break;
    case 2:
        if (modrm == 0xE9) { c = mk(K_COMPP, 0, 2, 0, 0); c.unord = 1; return c; }
        break;
    case 3:
        if (modrm == 0xE0 || modrm == 0xE1 || modrm == 0xE4) return mk(K_NOP, 0, 0, 0, 0);
        if (modrm == 0xE2) return mk(K_CLEX, 0, 0, 0, 0);
        if (modrm == 0xE3) return mk(K_INIT, 0, 0, 0, 0);
        break;
    case 5:
        if (reg == 0) return mk(K_FFREE, i, 0, 0, 0);
        if (reg == 2) return mk(K_FST_ST, i, 0, 0, 0);
        if (reg == 3) return mk(K_FST_ST, i, 1, 0, 0);
        if (reg == 4 || reg == 5) { c = mk(K_COM_ST, 0, reg == 5, 0, 0); c.a = (uint8_t)i; c.unord = 1; return c; }
        break;
    case 7:
        if (modrm == 0xE0) return mk(K_STSW_AX, 0, 0, 0, 0);
        break;
    default: break;
    }
    return mk(K_BAD, 0, 0, 0, 0);
}

static cls classify(unsigned esc, uint8_t modrm) {
    return modrm >> 6 == 3 ? classify_reg(esc, modrm) : classify_mem(esc, modrm >> 3 & 7);
}

bool fpu_decode(unsigned escape, uint8_t modrm, fpu_op *op) {
    cls c = classify(escape & 7u, modrm);
    op->valid = c.kind != K_BAD;
    op->rd = c.rd;
    op->wr = c.wr;
    op->ax = c.kind == K_STSW_AX;
    return op->valid;
}

/* ---- arithmetic and comparison ------------------------------------------- */

static int host_rc(unsigned rc) {
    return rc == 1 ? FE_DOWNWARD : rc == 2 ? FE_UPWARD : rc == 3 ? FE_TOWARDZERO : FE_TONEAREST;
}

/* x op y for the ModRM reg codes 0 add, 1 mul, 4 x-y, 5 y-x, 6 x/y, 7 y/x,
 * with the control word's rounding and precision control. */
static long double arith(fpu_t *f, unsigned op, long double x, long double y) {
    unsigned rc = rc_of(f), pc = f->cw >> 8 & 3u;
    long double a = x, b = y, res;
    volatile long double va, vb, vr; /* keep the operations between the rounding mode switches */
    int old = fegetround();
    if (op == 5 || op == 7) { a = y; b = x; }
    va = a; vb = b;
    if (rc) fesetround(host_rc(rc));
    switch (op) {
    case 0: vr = va + vb; break;
    case 1: vr = va * vb; break;
    case 4: case 5: vr = va - vb; break;
    default: vr = va / vb; break;
    }
    res = vr;
    if (pc == 0) { volatile float t = (float)res; res = t; }
    else if (pc == 2) { volatile double t = (double)res; res = t; }
    if (rc) fesetround(old);
    if (isnan(res)) {
        if (!isnan(x) && !isnan(y)) { f->sw |= FPU_SW_IE; res = indefinite(); }
    } else if (isinf(res) && isfinite(x) && isfinite(y)) {
        f->sw |= op >= 6 ? FPU_SW_ZE : FPU_SW_OE;
    }
    return res;
}

/* Condition codes of a compare of x with y. FCOM raises IE on a NaN, FUCOM not. */
static void compare(fpu_t *f, long double x, long double y, bool unordered_quiet) {
    f->sw &= (uint16_t)~FPU_SW_CC;
    if (isnan(x) || isnan(y)) {
        f->sw |= FPU_SW_C3 | FPU_SW_C2 | FPU_SW_C0;
        if (!unordered_quiet) f->sw |= FPU_SW_IE;
    } else if (x < y) {
        f->sw |= FPU_SW_C0;
    } else if (x == y) {
        f->sw |= FPU_SW_C3;
    }
}

static void examine(fpu_t *f) {
    unsigned p = fpu_top(f);
    long double v = f->st[p];
    f->sw &= (uint16_t)~FPU_SW_CC;
    if (f->empty >> p & 1u) { f->sw |= FPU_SW_C3 | FPU_SW_C0; return; }
    if (signbit(v)) f->sw |= FPU_SW_C1;
    if (isnan(v)) f->sw |= FPU_SW_C0;
    else if (isinf(v)) f->sw |= FPU_SW_C2 | FPU_SW_C0;
    else if (v == 0) f->sw |= FPU_SW_C3;
    else if (fpclassify(v) == FP_SUBNORMAL) f->sw |= FPU_SW_C3 | FPU_SW_C2;
    else f->sw |= FPU_SW_C2;
}

/* Partial remainder: FPREM (truncating) and FPREM1 (IEEE nearest). The quotient's
 * low three bits go to C1 (Q0), C3 (Q1) and C0 (Q2); C2 means "incomplete, repeat". */
static void prem(fpu_t *f, bool ieee) {
    long double x = get(f, 0), y = get(f, 1), r, ay;
    unsigned q = 0;
    int d;
    f->sw &= (uint16_t)~FPU_SW_CC;
    if (isnan(x) || isnan(y)) { put(f, 0, isnan(x) ? x : y); return; }
    if (isinf(x) || y == 0) { f->sw |= FPU_SW_IE; put(f, 0, indefinite()); return; }
    if (isinf(y) || x == 0) { put(f, 0, x); return; }
    ay = fabsl(y);
    d = ilogbl(x) - ilogbl(y);
    if (d >= 64) { /* reduce by a power-of-two multiple of y and ask for another pass */
        long double y2 = ldexpl(y, d - 32);
        put(f, 0, ieee ? remainderl(x, y2) : fmodl(x, y2));
        f->sw |= FPU_SW_C2;
        return;
    }
    if (ieee) {
        int qi = 0;
        r = remquol(x, y, &qi);
        q = (unsigned)(qi < 0 ? -qi : qi) & 7u;
    } else {
        r = fmodl(x, y);
        q = (unsigned)((uint64_t)((fabsl(x) - fabsl(r)) / ay) & 7u);
    }
    if (q & 1u) f->sw |= FPU_SW_C1;
    if (q & 2u) f->sw |= FPU_SW_C3;
    if (q & 4u) f->sw |= FPU_SW_C0;
    put(f, 0, r);
}

/* |x| below 2^63 is the range the 387 reduces for the trigonometric functions. */
static bool trig_range(fpu_t *f, long double x) {
    f->sw &= (uint16_t)~FPU_SW_C2;
    if (isnan(x)) return false;
    if (isinf(x)) { f->sw |= FPU_SW_IE; return false; }
    if (fabsl(x) >= 0x1p63L) { f->sw |= FPU_SW_C2; return false; }
    return true;
}

static void misc(fpu_t *f, unsigned code) {
    long double x = get(f, 0), y, r;
    switch (code) {
    case 0xE0: put(f, 0, -x); break;                                    /* FCHS */
    case 0xE1: put(f, 0, fabsl(x)); break;                              /* FABS */
    case 0xF0:                                                          /* F2XM1 */
        put(f, 0, fabsl(x) > 0.5L ? exp2l(x) - 1.0L : expm1l(x * 0.693147180559945309417232121458176568L));
        break;
    case 0xF1:                                                          /* FYL2X */
        y = get(f, 1);
        if (isnan(x) || isnan(y)) r = isnan(x) ? x : y;
        else if (x < 0 || (x == 0 && y == 0)) { f->sw |= FPU_SW_IE; r = indefinite(); }
        else if (x == 0) { f->sw |= FPU_SW_ZE; r = y > 0 ? -(long double)INFINITY : (long double)INFINITY; }
        else r = y * log2l(x);
        put(f, 1, r);
        pop(f);
        break;
    case 0xF2:                                                          /* FPTAN */
        if (trig_range(f, x)) { put(f, 0, tanl(x)); push(f, 1.0L); }
        else if (isnan(x) || isinf(x)) put(f, 0, isnan(x) ? x : indefinite());
        break;
    case 0xF3:                                                          /* FPATAN */
        y = get(f, 1);
        put(f, 1, atan2l(y, x));
        pop(f);
        break;
    case 0xF4:                                                          /* FXTRACT */
        if (isnan(x)) { push(f, x); }
        else if (isinf(x)) { put(f, 0, fabsl(x)); push(f, x); }
        else if (x == 0) { f->sw |= FPU_SW_ZE; put(f, 0, -(long double)INFINITY); push(f, x); }
        else {
            int e = ilogbl(x);
            put(f, 0, (long double)e);
            push(f, scalbnl(x, -e));
        }
        break;
    case 0xF5: prem(f, true); break;                                    /* FPREM1 */
    case 0xF8: prem(f, false); break;                                   /* FPREM */
    case 0xF9:                                                          /* FYL2XP1 */
        y = get(f, 1);
        if (isnan(x) || isnan(y)) r = isnan(x) ? x : y;
        else if (x < -1 || (x == -1 && y == 0)) { f->sw |= FPU_SW_IE; r = indefinite(); }
        else if (x == -1) { f->sw |= FPU_SW_ZE; r = y > 0 ? -(long double)INFINITY : (long double)INFINITY; }
        else r = y * (fabsl(x) >= 0.25L ? log2l(1.0L + x) : log1pl(x) / 0.693147180559945309417232121458176568L);
        put(f, 1, r);
        pop(f);
        break;
    case 0xFA:                                                          /* FSQRT */
        if (x < 0) { f->sw |= FPU_SW_IE; put(f, 0, indefinite()); }
        else put(f, 0, sqrtl(x));
        break;
    case 0xFB:                                                          /* FSINCOS */
        if (trig_range(f, x)) { put(f, 0, sinl(x)); push(f, cosl(x)); }
        else if (isnan(x) || isinf(x)) put(f, 0, isnan(x) ? x : indefinite());
        break;
    case 0xFC:                                                          /* FRNDINT */
        r = round_rc(x, rc_of(f));
        if (r != x && !isnan(x)) f->sw |= FPU_SW_PE;
        put(f, 0, r);
        break;
    case 0xFD:                                                          /* FSCALE */
        y = get(f, 1);
        if (isnan(x) || isnan(y)) r = isnan(x) ? x : y;
        else if (isinf(y)) {
            if (y > 0 ? x == 0 : isinf(x)) { f->sw |= FPU_SW_IE; r = indefinite(); }
            else r = y > 0 ? x * y : copysignl(0.0L, x);
        } else {
            r = ldexpl(x, (int)fmaxl(-100000.0L, fminl(100000.0L, truncl(y))));
            if (isinf(r) && isfinite(x)) f->sw |= FPU_SW_OE;
        }
        put(f, 0, r);
        break;
    case 0xFE: case 0xFF:                                               /* FSIN, FCOS */
        if (trig_range(f, x)) put(f, 0, code == 0xFE ? sinl(x) : cosl(x));
        else if (isnan(x) || isinf(x)) put(f, 0, isnan(x) ? x : indefinite());
        break;
    default: break;
    }
}

/* ---- execution ---------------------------------------------------------- */

static long double constant(unsigned n) {
    switch (n) {
    case 0: return 1.0L;
    case 1: return 3.32192809488736234787031942948939018L;  /* log2(10) */
    case 2: return 1.44269504088896340735992468100189214L;  /* log2(e) */
    case 3: return 3.14159265358979323846264338327950288L;  /* pi */
    case 4: return 0.301029995663981195213738894724493027L; /* log10(2) */
    case 5: return 0.693147180559945309417232121458176568L; /* ln(2) */
    default: return 0.0L;
    }
}

void fpu_exec(fpu_t *f, unsigned escape, uint8_t modrm, const uint8_t *in, uint8_t *out,
              uint16_t *ax) {
    cls c = classify(escape & 7u, modrm);
    unsigned i;
    switch (c.kind) {
    case K_FLD_M: push(f, ld_real(in, c.n)); break;
    case K_FILD_M: push(f, ld_int(in, c.n)); break;
    case K_FBLD: push(f, ld_bcd(in)); break;
    case K_FST_M: st_real(f, get(f, 0), c.n, out); if (c.pop) pop(f); break;
    case K_FIST_M: st_int(f, get(f, 0), c.n, out); if (c.pop) pop(f); break;
    case K_FBSTP: st_bcd(f, get(f, 0), out); pop(f); break;
    case K_LDCW: f->cw = (uint16_t)r16(in); break;
    case K_STCW: w16(out, f->cw); break;
    case K_STSW_M: w16(out, f->sw); break;
    case K_STSW_AX: *ax = f->sw; break;
    case K_STENV: st_env(f, out); f->cw |= 0x3F; break;
    case K_LDENV: ld_env(f, in); break;
    case K_SAVE:
        st_env(f, out);
        for (i = 0; i < 8; i++) ld_to_tb(f->st[(fpu_top(f) + i) & 7u], out + ENV_SIZE + 10 * i);
        fpu_init(f);
        break;
    case K_RSTOR:
        ld_env(f, in);
        for (i = 0; i < 8; i++) f->st[(fpu_top(f) + i) & 7u] = tb_to_ld(in + ENV_SIZE + 10 * i);
        break;
    case K_FLD_ST: push(f, get(f, c.n)); break;
    case K_FXCH: {
        long double a = get(f, 0), b = get(f, c.n);
        f->sw &= (uint16_t)~FPU_SW_C1;
        put(f, 0, b);
        put(f, c.n, a);
        break;
    }
    case K_FST_ST: put(f, c.n, get(f, 0)); if (c.pop) pop(f); break;
    case K_FFREE: f->empty |= (uint8_t)(1u << ((fpu_top(f) + c.n) & 7u)); break;
    case K_CONST: push(f, constant(c.n)); break;
    case K_INCSTP: f->sw &= (uint16_t)~FPU_SW_C1; set_top(f, fpu_top(f) + 1); break;
    case K_DECSTP: f->sw &= (uint16_t)~FPU_SW_C1; set_top(f, fpu_top(f) - 1); break;
    case K_ARITH_REAL: put(f, 0, arith(f, c.n, get(f, 0), ld_real(in, c.rd))); break;
    case K_ARITH_INT: put(f, 0, arith(f, c.n, get(f, 0), ld_int(in, c.rd))); break;
    case K_ARITH_ST: {
        long double x = get(f, 0), y = get(f, c.a);
        put(f, c.dst ? c.a : 0, arith(f, c.n, x, y));
        if (c.pop) pop(f);
        break;
    }
    case K_COM_REAL: compare(f, get(f, 0), ld_real(in, c.rd), false); if (c.pop) pop(f); break;
    case K_COM_INT: compare(f, get(f, 0), ld_int(in, c.rd), false); if (c.pop) pop(f); break;
    case K_COM_ST: compare(f, get(f, 0), get(f, c.a), c.unord); if (c.pop) pop(f); break;
    case K_COMPP: compare(f, get(f, 0), get(f, 1), c.unord); pop(f); pop(f); break;
    case K_TST: compare(f, get(f, 0), 0.0L, false); break;
    case K_XAM: examine(f); break;
    case K_MISC: misc(f, c.n); break;
    case K_INIT: fpu_init(f); break;
    case K_CLEX: f->sw &= (uint16_t)~0x80FFu; break;
    default: break;
    }
}
