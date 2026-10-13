/* 16-bit protected-mode x86 interpreter. See cpu.h for the host interface.
 *
 * Model: CS/SS are 16-bit segments (IP/SP wrap at 64K); 0x66/0x67 select
 * 32-bit operand/address size. A fault restores CS:EIP and ESP of the faulting
 * instruction; other registers/memory may be partly updated. A REP string op
 * executes one iteration per cpu_step. LOCK is ignored. Where results are
 * architecturally undefined (OF for multi-bit shifts, AF for shifts/logic,
 * flags after MUL/DIV) the values chosen here are only documented, not relied on.
 *
 * Unsupported (decode as CPU_E_UD unless noted):
 *   - x87 escapes D8-DF and WIN87EM INT 34h-3Eh run through fpu.c; INT 3Fh (overlay
 *     manager) raises CPU_E_FPU
 *   - I/O: IN/OUT (E4-E7, EC-EF), INS/OUTS (6C-6F)
 *   - BCD adjust: DAA DAS AAA AAS AAM AAD (27 2F 37 3F D4 D5), SALC (D6)
 *   - ARPL (63), ICEBP (F1), LOCK-only encodings are accepted as prefixes
 *   - 0F-map: system opcodes (LGDT CLTS MOV CRn/DRn, SLDT/STR/LLDT/LTR ...), CMPXCHG,
 *     XADD, BSWAP (486+), CMOVcc, CPUID and everything else not listed in
 *     the implemented set. VERR/VERW (0F 00 /4 /5), LAR and LSL are supported
 *     through cpu_mem.sel_info (CPL is always 3). BOUND (62) raises CPU_E_BOUND on range failure.
 *   - 32-bit stack/code segments (B bit); IRET never changes privilege level.
 */
#include <string.h>

#include "cpu.h"

#define R(i) c->r[i]
#define FAULT(c, s) longjmp((c)->jb, (int)(s))

/* ---- raw memory access (status returning) ----------------------------- */

static cpu_status rawread(cpu_t *c, uint16_t sel, uint32_t off, int size, uint32_t *out) {
    const cpu_mem *m = &c->mem;
    cpu_status s;
    uint32_t v = 0;
    int i;
    if (size == 2 && m->read16) {
        uint16_t t;
        if ((s = m->read16(m->ctx, sel, off, &t)) == CPU_OK) *out = t;
        return s;
    }
    if (size == 4 && m->read32) return m->read32(m->ctx, sel, off, out);
    for (i = 0; i < size; i++) {
        uint8_t b;
        if ((s = m->read8(m->ctx, sel, off + (uint32_t)i, &b)) != CPU_OK) return s;
        v |= (uint32_t)b << (8 * i);
    }
    *out = v;
    return CPU_OK;
}

static cpu_status rawwrite(cpu_t *c, uint16_t sel, uint32_t off, int size, uint32_t v) {
    const cpu_mem *m = &c->mem;
    cpu_status s;
    int i;
    if (size == 2 && m->write16) return m->write16(m->ctx, sel, off, (uint16_t)v);
    if (size == 4 && m->write32) return m->write32(m->ctx, sel, off, v);
    if (size > 1) { /* probe the last byte so a straddling store never half-writes */
        uint8_t probe;
        if ((s = m->read8(m->ctx, sel, off + (uint32_t)size - 1, &probe)) != CPU_OK) return s;
    }
    for (i = 0; i < size; i++)
        if ((s = m->write8(m->ctx, sel, off + (uint32_t)i, (uint8_t)(v >> (8 * i)))) != CPU_OK)
            return s;
    return CPU_OK;
}

static uint32_t mread(cpu_t *c, int sreg, uint32_t off, int size) {
    uint32_t v;
    cpu_status s = rawread(c, c->seg[sreg], off, size, &v);
    if (s != CPU_OK) FAULT(c, s);
    return v;
}

static void mwrite(cpu_t *c, int sreg, uint32_t off, int size, uint32_t v) {
    cpu_status s = rawwrite(c, c->seg[sreg], off, size, v);
    if (s != CPU_OK) FAULT(c, s);
}

/* ---- instruction stream ------------------------------------------------ */

static uint8_t fetch8(cpu_t *c) {
    uint32_t v;
    cpu_status s = rawread(c, c->seg[CPU_CS], c->eip, 1, &v);
    if (s != CPU_OK) FAULT(c, s);
    c->eip++;
    return (uint8_t)v;
}
static uint16_t fetch16(cpu_t *c) {
    uint16_t lo = fetch8(c);
    return (uint16_t)(lo | fetch8(c) << 8);
}
static uint32_t fetch32(cpu_t *c) {
    uint32_t lo = fetch16(c);
    return lo | (uint32_t)fetch16(c) << 16;
}
/* immediate of operand size (1, 2 or 4 bytes) */
static uint32_t fetch_sz(cpu_t *c, int size) {
    return size == 1 ? fetch8(c) : size == 2 ? fetch16(c) : fetch32(c);
}
static int32_t sext(uint32_t v, int size) {
    return size == 1 ? (int8_t)v : size == 2 ? (int16_t)v : (int32_t)v;
}

/* ---- registers --------------------------------------------------------- */

static uint32_t rget(cpu_t *c, int i, int size) {
    if (size == 1) return i < 4 ? R(i) & 0xFF : (R(i - 4) >> 8) & 0xFF;
    return size == 2 ? R(i) & 0xFFFF : R(i);
}
static void rset(cpu_t *c, int i, int size, uint32_t v) {
    if (size == 1) {
        if (i < 4) R(i) = (R(i) & ~0xFFu) | (v & 0xFF);
        else R(i - 4) = (R(i - 4) & ~0xFF00u) | ((v & 0xFF) << 8);
    } else if (size == 2) {
        R(i) = (R(i) & 0xFFFF0000u) | (v & 0xFFFF);
    } else {
        R(i) = v;
    }
}

/* ---- ModRM ------------------------------------------------------------- */

static void modrm(cpu_t *c) {
    uint8_t m = fetch8(c);
    int defseg = CPU_DS;
    uint32_t off = 0;
    c->mod = m >> 6;
    c->reg = (m >> 3) & 7;
    c->rm = m & 7;
    if (c->mod == 3) return;
    if (!c->ad32) {
        uint32_t bx = R(CPU_EBX) & 0xFFFF, bp = R(CPU_EBP) & 0xFFFF;
        uint32_t si = R(CPU_ESI) & 0xFFFF, di = R(CPU_EDI) & 0xFFFF;
        if (c->mod == 0 && c->rm == 6) {
            off = fetch16(c);
        } else {
            switch (c->rm) {
            case 0: off = bx + si; break;
            case 1: off = bx + di; break;
            case 2: off = bp + si; defseg = CPU_SS; break;
            case 3: off = bp + di; defseg = CPU_SS; break;
            case 4: off = si; break;
            case 5: off = di; break;
            case 6: off = bp; defseg = CPU_SS; break;
            default: off = bx; break;
            }
            if (c->mod == 1) off += (uint32_t)(int32_t)(int8_t)fetch8(c);
            else if (c->mod == 2) off += fetch16(c);
        }
        off &= 0xFFFF;
    } else {
        if (c->rm == 4) {
            uint8_t sib = fetch8(c);
            unsigned scale = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
            if (idx != 4) off += R(idx) << scale;
            if (base == 5 && c->mod == 0) {
                off += fetch32(c);
            } else {
                off += R(base);
                if (base == 4 || base == 5) defseg = CPU_SS;
            }
        } else if (c->rm == 5 && c->mod == 0) {
            off = fetch32(c);
        } else {
            off = R(c->rm);
            if (c->rm == 5) defseg = CPU_SS;
        }
        if (c->mod == 1) off += (uint32_t)(int32_t)(int8_t)fetch8(c);
        else if (c->mod == 2) off += fetch32(c);
    }
    c->ea_seg = c->seg_ovr >= 0 ? c->seg_ovr : defseg;
    c->ea_off = off;
}

static uint32_t get_rm(cpu_t *c, int size) {
    if (c->mod == 3) return rget(c, c->rm, size);
    return mread(c, c->ea_seg, c->ea_off, size);
}
static void set_rm(cpu_t *c, int size, uint32_t v) {
    if (c->mod == 3) rset(c, c->rm, size, v);
    else mwrite(c, c->ea_seg, c->ea_off, size, v);
}
static uint32_t get_reg(cpu_t *c, int size) { return rget(c, c->reg, size); }
static void set_reg(cpu_t *c, int size, uint32_t v) { rset(c, c->reg, size, v); }

/* ---- segments and stack ------------------------------------------------ */

static void load_seg(cpu_t *c, int idx, uint16_t sel) {
    if (sel == 0 && idx != CPU_CS && idx != CPU_SS) {
        c->seg[idx] = 0;
        return;
    }
    if (sel == 0 || (c->mem.sel_valid && !c->mem.sel_valid(c->mem.ctx, sel))) FAULT(c, CPU_E_GP);
    c->seg[idx] = sel;
}

static void push(cpu_t *c, int size, uint32_t v) {
    uint32_t sp = ((R(CPU_ESP) & 0xFFFF) - (uint32_t)size) & 0xFFFF;
    mwrite(c, CPU_SS, sp, size, v);
    R(CPU_ESP) = (R(CPU_ESP) & 0xFFFF0000u) | sp;
}
static uint32_t pop(cpu_t *c, int size) {
    uint32_t sp = R(CPU_ESP) & 0xFFFF;
    uint32_t v = mread(c, CPU_SS, sp, size);
    R(CPU_ESP) = (R(CPU_ESP) & 0xFFFF0000u) | ((sp + (uint32_t)size) & 0xFFFF);
    return v;
}

#define FLAGS_DEFINED 0x0FD5u /* CF PF AF ZF SF TF IF DF OF */

/* ---- group 1: data movement ------------------------------------------- */

typedef enum { H_NO = 0, H_OK } hres;

static void load_far(cpu_t *c, int sreg) {
    int osz = c->op32 ? 4 : 2;
    uint32_t off;
    uint16_t sel;
    modrm(c);
    if (c->mod == 3) FAULT(c, CPU_E_UD);
    off = mread(c, c->ea_seg, c->ea_off, osz);
    sel = (uint16_t)mread(c, c->ea_seg, c->ea_off + (uint32_t)osz, 2);
    load_seg(c, sreg, sel);
    set_reg(c, osz, off);
}

static uint32_t incdec(cpu_t *c, uint32_t v, int size, bool dec);
static void call_near(cpu_t *c, uint32_t target);
static void far_indirect(cpu_t *c, bool call);

static hres h_ff(cpu_t *c) {
    int osz = c->op32 ? 4 : 2;
    modrm(c);
    switch (c->reg) {
    case 0: case 1: set_rm(c, osz, incdec(c, get_rm(c, osz), osz, c->reg == 1)); return H_OK;
    case 2: call_near(c, get_rm(c, osz)); return H_OK;
    case 3: far_indirect(c, true); return H_OK;
    case 4: {
        uint32_t t = get_rm(c, osz);
        c->eip = c->op32 ? t : (t & 0xFFFF);
        return H_OK;
    }
    case 5: far_indirect(c, false); return H_OK;
    case 6: push(c, osz, get_rm(c, osz)); return H_OK;
    default: FAULT(c, CPU_E_UD);
    }
    return H_OK;
}

static hres h_move(cpu_t *c, unsigned op) {
    int osz = c->op32 ? 4 : 2;
    switch (op) {
    case 0x88: modrm(c); set_rm(c, 1, get_reg(c, 1)); return H_OK;
    case 0x89: modrm(c); set_rm(c, osz, get_reg(c, osz)); return H_OK;
    case 0x8A: modrm(c); set_reg(c, 1, get_rm(c, 1)); return H_OK;
    case 0x8B: modrm(c); set_reg(c, osz, get_rm(c, osz)); return H_OK;
    case 0x8C: {
        uint32_t v;
        modrm(c);
        if (c->reg > 5) FAULT(c, CPU_E_UD);
        v = c->seg[c->reg];
        if (c->mod == 3) rset(c, c->rm, osz, v);
        else mwrite(c, c->ea_seg, c->ea_off, 2, v);
        return H_OK;
    }
    case 0x8E: {
        uint32_t v;
        modrm(c);
        if (c->reg > 5 || c->reg == CPU_CS) FAULT(c, CPU_E_UD);
        v = get_rm(c, 2);
        load_seg(c, c->reg, (uint16_t)v);
        return H_OK;
    }
    case 0x8D:
        modrm(c);
        if (c->mod == 3) FAULT(c, CPU_E_UD);
        set_reg(c, osz, c->ea_off);
        return H_OK;
    case 0x8F:
        modrm(c);
        if (c->reg != 0) FAULT(c, CPU_E_UD);
        set_rm(c, osz, pop(c, osz));
        return H_OK;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        int sz = (op & 1) ? osz : 1;
        uint32_t off = c->ad32 ? fetch32(c) : fetch16(c);
        int sreg = c->seg_ovr >= 0 ? c->seg_ovr : CPU_DS;
        if (op < 0xA2) rset(c, CPU_EAX, sz, mread(c, sreg, off, sz));
        else mwrite(c, sreg, off, sz, rget(c, CPU_EAX, sz));
        return H_OK;
    }
    case 0xC6: case 0xC7: {
        int sz = op == 0xC6 ? 1 : osz;
        modrm(c);
        if (c->reg != 0) FAULT(c, CPU_E_UD);
        set_rm(c, sz, fetch_sz(c, sz));
        return H_OK;
    }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        rset(c, (int)(op & 7), 1, fetch8(c));
        return H_OK;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        rset(c, (int)(op & 7), osz, fetch_sz(c, osz));
        return H_OK;
    case 0xC4: load_far(c, CPU_ES); return H_OK;
    case 0xC5: load_far(c, CPU_DS); return H_OK;
    case 0x1B2: load_far(c, CPU_SS); return H_OK;
    case 0x1B4: load_far(c, CPU_FS); return H_OK;
    case 0x1B5: load_far(c, CPU_GS); return H_OK;
    case 0x86: case 0x87: {
        int sz = op == 0x86 ? 1 : osz;
        uint32_t a, b;
        modrm(c);
        a = get_rm(c, sz);
        b = get_reg(c, sz);
        set_rm(c, sz, b);
        set_reg(c, sz, a);
        return H_OK;
    }
    case 0x90: return H_OK;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        uint32_t a = rget(c, CPU_EAX, osz), b = rget(c, (int)(op & 7), osz);
        rset(c, CPU_EAX, osz, b);
        rset(c, (int)(op & 7), osz, a);
        return H_OK;
    }
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        push(c, osz, rget(c, (int)(op & 7), osz)); /* PUSH SP pushes the old value */
        return H_OK;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        rset(c, (int)(op & 7), osz, pop(c, osz));
        return H_OK;
    case 0x68: push(c, osz, fetch_sz(c, osz)); return H_OK;
    case 0x6A: push(c, osz, (uint32_t)sext(fetch8(c), 1)); return H_OK;
    case 0x06: push(c, osz, c->seg[CPU_ES]); return H_OK;
    case 0x0E: push(c, osz, c->seg[CPU_CS]); return H_OK;
    case 0x16: push(c, osz, c->seg[CPU_SS]); return H_OK;
    case 0x1E: push(c, osz, c->seg[CPU_DS]); return H_OK;
    case 0x1A0: push(c, osz, c->seg[CPU_FS]); return H_OK;
    case 0x1A8: push(c, osz, c->seg[CPU_GS]); return H_OK;
    case 0x07: load_seg(c, CPU_ES, (uint16_t)pop(c, osz)); return H_OK;
    case 0x17: load_seg(c, CPU_SS, (uint16_t)pop(c, osz)); return H_OK;
    case 0x1F: load_seg(c, CPU_DS, (uint16_t)pop(c, osz)); return H_OK;
    case 0x1A1: load_seg(c, CPU_FS, (uint16_t)pop(c, osz)); return H_OK;
    case 0x1A9: load_seg(c, CPU_GS, (uint16_t)pop(c, osz)); return H_OK;
    case 0x60: {
        uint32_t sp = rget(c, CPU_ESP, osz);
        int i;
        for (i = 0; i < 8; i++) push(c, osz, i == CPU_ESP ? sp : rget(c, i, osz));
        return H_OK;
    }
    case 0x61: {
        int i;
        for (i = 7; i >= 0; i--) {
            uint32_t v = pop(c, osz);
            if (i != CPU_ESP) rset(c, i, osz, v);
        }
        return H_OK;
    }
    case 0x9C: push(c, osz, c->eflags | 2u); return H_OK;
    case 0x9D: {
        uint32_t v = pop(c, osz);
        c->eflags = (c->eflags & ~FLAGS_DEFINED) | (v & FLAGS_DEFINED) | 2u;
        return H_OK;
    }
    case 0xFF: return h_ff(c);
    case 0xF4: return H_OK; /* HLT: exec() reports CPU_HALT */
    default: return H_NO;
    }
}

/* ---- group 2: ALU, shifts, multiply/divide, bit ops -------------------- */

#define FLAGS_ARITH (CPU_CF | CPU_PF | CPU_AF | CPU_ZF | CPU_SF | CPU_OF)

static uint32_t szmask(int size) { return size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu; }

static uint32_t szp_flags(uint32_t res, int size) {
    uint32_t f = 0, x = res & szmask(size);
    uint32_t p = (res & 0xFF) ^ ((res & 0xFF) >> 4);
    if (x == 0) f |= CPU_ZF;
    if (x & (1u << (size * 8 - 1))) f |= CPU_SF;
    if (!((0x6996u >> (p & 0xF)) & 1)) f |= CPU_PF; /* even parity of the low byte */
    return f;
}

static void set_flags(cpu_t *c, uint32_t mask, uint32_t val) {
    c->eflags = (c->eflags & ~mask) | (val & mask) | 2u;
}

/* op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP (CMP returns a). */
static uint32_t alu(cpu_t *c, int op, uint32_t a, uint32_t b, int size) {
    int bits = size * 8;
    uint64_t m = szmask(size), ua = a & m, ub = b & m, r;
    uint32_t sign = 1u << (bits - 1), res, f = 0;
    uint32_t cin = (c->eflags & CPU_CF) ? 1u : 0u;
    switch (op) {
    case 0: case 2:
        r = ua + ub + (op == 2 ? cin : 0);
        res = (uint32_t)(r & m);
        if ((r >> bits) & 1) f |= CPU_CF;
        if ((ua ^ ub ^ res) & 0x10) f |= CPU_AF;
        if (~(ua ^ ub) & (ua ^ res) & sign) f |= CPU_OF;
        break;
    case 3: case 5: case 7:
        r = ua - ub - (op == 3 ? cin : 0);
        res = (uint32_t)(r & m);
        if ((r >> bits) & 1) f |= CPU_CF;
        if ((ua ^ ub ^ res) & 0x10) f |= CPU_AF;
        if ((ua ^ ub) & (ua ^ res) & sign) f |= CPU_OF;
        break;
    case 1: res = (uint32_t)(ua | ub); break;
    case 4: res = (uint32_t)(ua & ub); break;
    default: res = (uint32_t)(ua ^ ub); break;
    }
    set_flags(c, FLAGS_ARITH, f | szp_flags(res, size));
    return op == 7 ? (uint32_t)ua : res;
}

static uint32_t incdec(cpu_t *c, uint32_t v, int size, bool dec) {
    uint32_t cf = c->eflags & CPU_CF, r = alu(c, dec ? 5 : 0, v, 1, size);
    set_flags(c, CPU_CF, cf);
    return r;
}

/* ROL ROR RCL RCR SHL SHR SAL(=SHL) SAR; counts are taken modulo 32. For
 * counts > 1 OF follows the count-1 formula and is architecturally undefined;
 * AF is cleared by shifts. */
static uint32_t shiftop(cpu_t *c, int op, uint32_t v, unsigned cnt, int size) {
    int bits = size * 8;
    uint64_t m = szmask(size), val = v & m, res = val;
    uint32_t cf = (c->eflags & CPU_CF) ? 1u : 0u, of;
    uint64_t top = (uint64_t)1 << (bits - 1);
    unsigned n, i;
    cnt &= 31;
    if (cnt == 0) return v;
    switch (op) {
    case 0:
        n = cnt % (unsigned)bits;
        res = n ? ((val << n) | (val >> (bits - (int)n))) & m : val;
        cf = (uint32_t)(res & 1);
        break;
    case 1:
        n = cnt % (unsigned)bits;
        res = n ? ((val >> n) | (val << (bits - (int)n))) & m : val;
        cf = (uint32_t)((res >> (bits - 1)) & 1);
        break;
    case 2:
        n = cnt % (unsigned)(bits + 1);
        for (i = 0; i < n; i++) {
            uint32_t o = (uint32_t)((res >> (bits - 1)) & 1);
            res = ((res << 1) | cf) & m;
            cf = o;
        }
        break;
    case 3:
        n = cnt % (unsigned)(bits + 1);
        for (i = 0; i < n; i++) {
            uint32_t o = (uint32_t)(res & 1);
            res = (res >> 1) | (cf ? top : 0);
            cf = o;
        }
        break;
    case 4: case 6:
        res = (val << cnt) & m;
        cf = (uint32_t)(((val << cnt) >> bits) & 1);
        break;
    case 5:
        res = val >> cnt;
        cf = (uint32_t)((val >> (cnt - 1)) & 1);
        break;
    default: {
        int64_t sv = (int64_t)sext(v, size);
        res = (uint64_t)(sv >> cnt) & m;
        cf = (uint32_t)(((uint64_t)(sv >> (cnt - 1))) & 1);
        break;
    }
    }
    switch (op) {
    case 0: case 2: case 4: case 6: of = (uint32_t)(((res & top) != 0) ^ cf); break;
    case 1: case 3: of = (uint32_t)(((res & top) != 0) ^ ((res & (top >> 1)) != 0)); break;
    case 5: of = (val & top) != 0; break;
    default: of = 0; break;
    }
    if (op < 4) set_flags(c, CPU_CF | CPU_OF, (cf ? CPU_CF : 0) | (of ? CPU_OF : 0));
    else set_flags(c, FLAGS_ARITH, (cf ? CPU_CF : 0) | (of ? CPU_OF : 0) | szp_flags((uint32_t)res, size));
    return (uint32_t)res;
}

static uint32_t dshift(cpu_t *c, bool left, uint32_t dst, uint32_t src, unsigned cnt, int size) {
    int bits = size * 8;
    uint64_t m = szmask(size), d = dst & m, s = src & m, res;
    uint32_t cf, of;
    cnt &= 31;
    if (cnt == 0) return dst;
    if (left) {
        res = ((d << cnt) | (cnt <= (unsigned)bits ? s >> (bits - (int)cnt) : 0)) & m;
        cf = (uint32_t)(((d << cnt) >> bits) & 1);
    } else {
        res = ((d >> cnt) | (cnt <= (unsigned)bits ? (s << (bits - (int)cnt)) : 0)) & m;
        cf = cnt <= (unsigned)bits ? (uint32_t)((d >> (cnt - 1)) & 1) : 0;
    }
    of = (uint32_t)(((res ^ d) >> (bits - 1)) & 1);
    set_flags(c, FLAGS_ARITH, (cf ? CPU_CF : 0) | (of ? CPU_OF : 0) | szp_flags((uint32_t)res, size));
    return (uint32_t)res;
}

static void store_wide(cpu_t *c, int size, uint64_t prod) {
    if (size == 1) {
        rset(c, CPU_EAX, 2, (uint32_t)prod);
    } else if (size == 2) {
        rset(c, CPU_EAX, 2, (uint32_t)prod);
        rset(c, CPU_EDX, 2, (uint32_t)(prod >> 16));
    } else {
        R(CPU_EAX) = (uint32_t)prod;
        R(CPU_EDX) = (uint32_t)(prod >> 32);
    }
}

static void mul1(cpu_t *c, int size, uint32_t v, bool sgn) {
    int bits = size * 8;
    uint32_t a = rget(c, CPU_EAX, size);
    uint64_t prod;
    bool ovf;
    if (sgn) {
        int64_t p = (int64_t)sext(a, size) * sext(v, size);
        prod = (uint64_t)p;
        ovf = p != (int64_t)sext((uint32_t)p, size);
    } else {
        prod = (uint64_t)a * v;
        ovf = (prod >> bits) != 0;
    }
    store_wide(c, size, prod);
    set_flags(c, CPU_CF | CPU_OF, ovf ? CPU_CF | CPU_OF : 0);
}

static uint32_t imul2(cpu_t *c, int size, uint32_t a, uint32_t b) {
    int64_t p = (int64_t)sext(a, size) * sext(b, size);
    uint32_t r = (uint32_t)p & szmask(size);
    set_flags(c, CPU_CF | CPU_OF, p != (int64_t)sext(r, size) ? CPU_CF | CPU_OF : 0);
    return r;
}

static void div1(cpu_t *c, int size, uint32_t v, bool sgn) {
    int bits = size * 8;
    uint64_t n, m = szmask(size), uq, ur;
    uint32_t d = v & (uint32_t)m;
    if (d == 0) FAULT(c, CPU_E_DE);
    if (size == 1) n = rget(c, CPU_EAX, 2);
    else if (size == 2) n = (uint64_t)rget(c, CPU_EDX, 2) << 16 | rget(c, CPU_EAX, 2);
    else n = (uint64_t)R(CPU_EDX) << 32 | R(CPU_EAX);
    if (sgn) {
        int64_t sn = size == 1 ? (int16_t)n : size == 2 ? (int32_t)n : (int64_t)n;
        int64_t sd = sext(d, size), sq, sr, hi = ((int64_t)1 << (bits - 1)) - 1;
        if (sd == -1 && sn == INT64_MIN) FAULT(c, CPU_E_DE);
        sq = sn / sd;
        sr = sn % sd;
        if (sq > hi || sq < -hi - 1) FAULT(c, CPU_E_DE);
        uq = (uint64_t)sq;
        ur = (uint64_t)sr;
    } else {
        uq = n / d;
        ur = n % d;
        if (uq > m) FAULT(c, CPU_E_DE);
    }
    if (size == 1) {
        rset(c, CPU_EAX, 1, (uint32_t)uq);
        rset(c, 4, 1, (uint32_t)ur); /* AH */
    } else {
        rset(c, CPU_EAX, size, (uint32_t)uq);
        rset(c, CPU_EDX, size, (uint32_t)ur);
    }
}

static void grp3(cpu_t *c, int size) {
    uint32_t v;
    modrm(c);
    v = get_rm(c, size);
    switch (c->reg) {
    case 0: case 1: alu(c, 4, v, fetch_sz(c, size), size); break;
    case 2: set_rm(c, size, ~v); break;
    case 3: set_rm(c, size, alu(c, 5, 0, v, size)); break;
    case 4: mul1(c, size, v, false); break;
    case 5: mul1(c, size, v, true); break;
    case 6: div1(c, size, v, false); break;
    default: div1(c, size, v, true); break;
    }
}

static bool cond_true(const cpu_t *c, unsigned cc) {
    uint32_t f = c->eflags;
    bool cf = f & CPU_CF, zf = f & CPU_ZF, sf = f & CPU_SF, of = f & CPU_OF, pf = f & CPU_PF, r;
    switch (cc >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf || zf; break;
    case 4: r = sf; break;
    case 5: r = pf; break;
    case 6: r = sf != of; break;
    default: r = zf || (sf != of); break;
    }
    return (cc & 1) ? !r : r;
}

/* BT/BTS/BTR/BTC: kind 0..3. A register bit offset may address memory beyond
 * the operand; an immediate offset is taken modulo the operand width. */
static void bit_op(cpu_t *c, int kind, bool imm) {
    int osz = c->op32 ? 4 : 2, bits = osz * 8;
    int32_t boff;
    uint32_t idx, val, off;
    modrm(c);
    if (imm) { /* 0F BA: the operation is in the reg field */
        if (c->reg < 4) FAULT(c, CPU_E_UD);
        kind = c->reg - 4;
    }
    off = c->ea_off;
    boff = imm ? (int32_t)fetch8(c) : sext(get_reg(c, osz), osz);
    idx = (uint32_t)boff & (uint32_t)(bits - 1);
    if (c->mod != 3 && !imm) {
        off += (uint32_t)(((boff - (int32_t)idx) / bits) * osz);
        if (!c->ad32) off &= 0xFFFF;
    }
    val = c->mod == 3 ? rget(c, c->rm, osz) : mread(c, c->ea_seg, off, osz);
    set_flags(c, CPU_CF, (val >> idx) & 1 ? CPU_CF : 0);
    if (kind == 0) return;
    val = kind == 1 ? val | (1u << idx) : kind == 2 ? val & ~(1u << idx) : val ^ (1u << idx);
    if (c->mod == 3) rset(c, c->rm, osz, val);
    else mwrite(c, c->ea_seg, off, osz, val);
}

static void bit_scan(cpu_t *c, bool reverse) {
    int osz = c->op32 ? 4 : 2, i;
    uint32_t v;
    modrm(c);
    v = get_rm(c, osz);
    if (v == 0) {
        set_flags(c, CPU_ZF, CPU_ZF);
        return;
    }
    set_flags(c, CPU_ZF, 0);
    if (reverse) for (i = osz * 8 - 1; !((v >> i) & 1); i--) ;
    else for (i = 0; !((v >> i) & 1); i++) ;
    set_reg(c, osz, (uint32_t)i);
}


/* ---- descriptor queries: VERR VERW LAR LSL (CPL is always 3) ----------- */

static bool desc_info(cpu_t *c, uint16_t sel, cpu_selinfo *out) {
    if (sel == 0) return false;
    if (c->mem.sel_info) {
        if (!c->mem.sel_info(c->mem.ctx, sel, out)) return false;
    } else {
        if (c->mem.sel_valid && !c->mem.sel_valid(c->mem.ctx, sel)) return false;
        out->limit = 0xFFFF;
        out->access = 0xF2;
    }
    return ((out->access >> 5) & 3) >= 3; /* DPL must admit CPL 3 and any RPL */
}

static void set_zf(cpu_t *c, bool on) { c->eflags = on ? c->eflags | CPU_ZF : c->eflags & ~CPU_ZF; }

/* 0F 00 /4 VERR, /5 VERW: ZF=1 when the segment is readable / writable */
static void seg_verify(cpu_t *c) {
    cpu_selinfo di;
    bool ok;
    modrm(c);
    if (c->reg != 4 && c->reg != 5) FAULT(c, CPU_E_UD);
    ok = desc_info(c, (uint16_t)get_rm(c, 2), &di) && (di.access & 0x90) == 0x90;
    if (ok && c->reg == 4) ok = !(di.access & 8) || (di.access & 2); /* code needs the R bit */
    if (ok && c->reg == 5) ok = !(di.access & 8) && (di.access & 2);  /* only writable data */
    set_zf(c, ok);
}

/* 0F 02 LAR / 0F 03 LSL: the destination is only written when ZF becomes 1 */
static void seg_query_op(cpu_t *c, bool lsl, int osz) {
    cpu_selinfo di;
    bool ok;
    modrm(c);
    ok = desc_info(c, (uint16_t)get_rm(c, 2), &di);
    if (ok) set_reg(c, osz, lsl ? di.limit : (uint32_t)di.access << 8);
    set_zf(c, ok);
}
static hres h_alu(cpu_t *c, unsigned op) {
    int osz = c->op32 ? 4 : 2;
    if (op < 0x40 && (op & 7) < 6) {
        int k = (int)(op >> 3), form = (int)(op & 7);
        uint32_t r;
        switch (form) {
        case 0: case 1: {
            int sz = form == 0 ? 1 : osz;
            modrm(c);
            r = alu(c, k, get_rm(c, sz), get_reg(c, sz), sz);
            if (k != 7) set_rm(c, sz, r);
            return H_OK;
        }
        case 2: case 3: {
            int sz = form == 2 ? 1 : osz;
            modrm(c);
            r = alu(c, k, get_reg(c, sz), get_rm(c, sz), sz);
            if (k != 7) set_reg(c, sz, r);
            return H_OK;
        }
        default: {
            int sz = form == 4 ? 1 : osz;
            r = alu(c, k, rget(c, CPU_EAX, sz), fetch_sz(c, sz), sz);
            if (k != 7) rset(c, CPU_EAX, sz, r);
            return H_OK;
        }
        }
    }
    switch (op) {
    case 0x80: case 0x81: case 0x83: {
        int sz = op == 0x80 ? 1 : osz, k;
        uint32_t a, b, r;
        modrm(c);
        k = c->reg;
        a = get_rm(c, sz);
        b = op == 0x81 ? fetch_sz(c, sz) : op == 0x83 ? (uint32_t)sext(fetch8(c), 1) : fetch8(c);
        r = alu(c, k, a, b, sz);
        if (k != 7) set_rm(c, sz, r);
        return H_OK;
    }
    case 0x84: case 0x85: {
        int sz = op == 0x84 ? 1 : osz;
        modrm(c);
        alu(c, 4, get_rm(c, sz), get_reg(c, sz), sz);
        return H_OK;
    }
    case 0xA8: alu(c, 4, rget(c, CPU_EAX, 1), fetch8(c), 1); return H_OK;
    case 0xA9: alu(c, 4, rget(c, CPU_EAX, osz), fetch_sz(c, osz), osz); return H_OK;
    case 0xF6: grp3(c, 1); return H_OK;
    case 0xF7: grp3(c, osz); return H_OK;
    case 0xFE: {
        uint32_t v;
        modrm(c);
        if (c->reg > 1) FAULT(c, CPU_E_UD);
        v = get_rm(c, 1);
        set_rm(c, 1, incdec(c, v, 1, c->reg == 1));
        return H_OK;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        rset(c, (int)(op & 7), osz, incdec(c, rget(c, (int)(op & 7), osz), osz, false));
        return H_OK;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        rset(c, (int)(op & 7), osz, incdec(c, rget(c, (int)(op & 7), osz), osz, true));
        return H_OK;
    case 0x98:
        if (c->op32) R(CPU_EAX) = (uint32_t)sext(R(CPU_EAX), 2);
        else rset(c, CPU_EAX, 2, (uint32_t)sext(R(CPU_EAX), 1));
        return H_OK;
    case 0x99:
        if (c->op32) R(CPU_EDX) = (R(CPU_EAX) & 0x80000000u) ? 0xFFFFFFFFu : 0;
        else rset(c, CPU_EDX, 2, (R(CPU_EAX) & 0x8000u) ? 0xFFFFu : 0);
        return H_OK;
    case 0x69: case 0x6B: {
        uint32_t a, b;
        modrm(c);
        a = get_rm(c, osz);
        b = op == 0x69 ? fetch_sz(c, osz) : (uint32_t)sext(fetch8(c), 1);
        set_reg(c, osz, imul2(c, osz, a, b));
        return H_OK;
    }
    case 0x1AF: {
        uint32_t a, b;
        modrm(c);
        a = get_reg(c, osz);
        b = get_rm(c, osz);
        set_reg(c, osz, imul2(c, osz, a, b));
        return H_OK;
    }
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int sz = (op & 1) ? osz : 1, k;
        uint32_t v;
        unsigned cnt;
        modrm(c);
        k = c->reg;
        v = get_rm(c, sz);
        cnt = op < 0xD0 ? fetch8(c) : op < 0xD2 ? 1u : (unsigned)(R(CPU_ECX) & 0xFF);
        set_rm(c, sz, shiftop(c, k, v, cnt, sz));
        return H_OK;
    }
    case 0x1A4: case 0x1A5: case 0x1AC: case 0x1AD: {
        uint32_t d;
        unsigned cnt;
        modrm(c);
        d = get_rm(c, osz);
        cnt = (op & 1) ? (unsigned)(R(CPU_ECX) & 0xFF) : fetch8(c);
        set_rm(c, osz, dshift(c, op < 0x1A8, d, get_reg(c, osz), cnt, osz));
        return H_OK;
    }
    case 0x1B6: case 0x1B7: case 0x1BE: case 0x1BF: {
        int ssz = (op & 1) ? 2 : 1;
        uint32_t v;
        modrm(c);
        v = get_rm(c, ssz);
        set_reg(c, osz, op >= 0x1BE ? (uint32_t)sext(v, ssz) : v);
        return H_OK;
    }
    case 0x190: case 0x191: case 0x192: case 0x193: case 0x194: case 0x195: case 0x196: case 0x197:
    case 0x198: case 0x199: case 0x19A: case 0x19B: case 0x19C: case 0x19D: case 0x19E: case 0x19F:
        modrm(c);
        set_rm(c, 1, cond_true(c, op & 15) ? 1u : 0u);
        return H_OK;
    case 0x1A3: bit_op(c, 0, false); return H_OK;
    case 0x1AB: bit_op(c, 1, false); return H_OK;
    case 0x1B3: bit_op(c, 2, false); return H_OK;
    case 0x1BB: bit_op(c, 3, false); return H_OK;
    case 0x1BA: bit_op(c, 0, true); return H_OK;
    case 0x100: seg_verify(c); return H_OK;
    case 0x102: case 0x103: seg_query_op(c, op == 0x103, osz); return H_OK;
    case 0x1BC: bit_scan(c, false); return H_OK;
    case 0x1BD: bit_scan(c, true); return H_OK;
    default: return H_NO;
    }
}

/* ---- group 3: control flow, interrupts, strings, flags ----------------- */

static void jump_rel(cpu_t *c, int32_t rel) {
    uint32_t t = c->eip + (uint32_t)rel;
    c->eip = c->op32 ? t : (t & 0xFFFF);
}

static void call_near(cpu_t *c, uint32_t target) {
    int osz = c->op32 ? 4 : 2;
    push(c, osz, c->eip);
    c->eip = c->op32 ? target : (target & 0xFFFF);
}

/* Far transfer: the new CS is validated before any state changes. */
static void far_to(cpu_t *c, uint16_t sel, uint32_t off, bool call) {
    int osz = c->op32 ? 4 : 2;
    if (sel == 0 || (c->mem.sel_valid && !c->mem.sel_valid(c->mem.ctx, sel))) FAULT(c, CPU_E_GP);
    if (call) {
        push(c, osz, c->seg[CPU_CS]);
        push(c, osz, c->eip);
    }
    c->seg[CPU_CS] = sel;
    c->eip = c->op32 ? off : (off & 0xFFFF);
}

static void far_indirect(cpu_t *c, bool call) {
    int osz = c->op32 ? 4 : 2;
    uint32_t off;
    uint16_t sel;
    if (c->mod == 3) FAULT(c, CPU_E_UD);
    off = mread(c, c->ea_seg, c->ea_off, osz);
    sel = (uint16_t)mread(c, c->ea_seg, c->ea_off + (uint32_t)osz, 2);
    far_to(c, sel, off, call);
}

/* Software interrupts never push a frame: the host callback emulates the
 * whole delivery (HLE services, or pushing a frame itself for guest handlers). */
static void do_fpu(cpu_t *c, unsigned esc);

/* WIN87EM emulator calls: INT 34h-3Bh are the escapes D8-DF with the ModRM
 * following; 3Ch / 3Eh are an ES: / CS: override followed by the escape byte;
 * 3Dh is FWAIT; 3Fh (overlay manager) is not an FPU call. The 3Ch-3Eh
 * assignments are UNVERIFIED: no loaded binary contains an INT 3x. */
static void emu_int(cpu_t *c, uint8_t vec) {
    if (vec <= 0x3B) { do_fpu(c, vec - 0x34u); return; }
    if (vec == 0x3D) return;
    if (vec == 0x3F) FAULT(c, CPU_E_FPU);
    {
        uint8_t esc = fetch8(c);
        if (esc < 0xD8 || esc > 0xDF) FAULT(c, CPU_E_UD);
        c->seg_ovr = vec == 0x3C ? CPU_ES : CPU_CS;
        do_fpu(c, esc - 0xD8u);
    }
}

static void do_int(cpu_t *c, uint8_t vec) {
    cpu_status s;
    if (vec >= 0x34 && vec <= 0x3F) { emu_int(c, vec); return; }
    if (!c->int_fn) FAULT(c, CPU_E_INT);
    s = c->int_fn(c, vec, c->int_ctx);
    if (s == CPU_HALT) c->ret = CPU_HALT;
    else if (s != CPU_OK) FAULT(c, s);
}

static void ret_far(cpu_t *c, uint32_t extra) {
    int osz = c->op32 ? 4 : 2;
    uint32_t ip = pop(c, osz);
    uint16_t cs = (uint16_t)pop(c, osz);
    if (cs == 0 || (c->mem.sel_valid && !c->mem.sel_valid(c->mem.ctx, cs))) FAULT(c, CPU_E_GP);
    c->seg[CPU_CS] = cs;
    c->eip = c->op32 ? ip : (ip & 0xFFFF);
    R(CPU_ESP) = (R(CPU_ESP) & 0xFFFF0000u) | (((R(CPU_ESP) & 0xFFFF) + extra) & 0xFFFF);
}

static uint32_t cx_get(const cpu_t *c) { return c->ad32 ? c->r[CPU_ECX] : c->r[CPU_ECX] & 0xFFFF; }
static void cx_set(cpu_t *c, uint32_t v) {
    if (c->ad32) c->r[CPU_ECX] = v;
    else c->r[CPU_ECX] = (c->r[CPU_ECX] & 0xFFFF0000u) | (v & 0xFFFF);
}

static uint32_t idx_get(const cpu_t *c, int reg) {
    return c->ad32 ? c->r[reg] : c->r[reg] & 0xFFFF;
}
static void idx_set(cpu_t *c, int reg, uint32_t v) {
    if (c->ad32) c->r[reg] = v;
    else c->r[reg] = (c->r[reg] & 0xFFFF0000u) | (v & 0xFFFF);
}

/* One iteration of a string instruction; a REP form re-arms by rewinding EIP. */
static void string_op(cpu_t *c, unsigned op) {
    int size = (op & 1) ? (c->op32 ? 4 : 2) : 1;
    int srcseg = c->seg_ovr >= 0 ? c->seg_ovr : CPU_DS;
    bool cmp = (op & ~1u) == 0xA6 || (op & ~1u) == 0xAE;
    uint32_t si = idx_get(c, CPU_ESI), di = idx_get(c, CPU_EDI), cnt = 0;
    uint32_t delta = (c->eflags & CPU_DF) ? (uint32_t)-size : (uint32_t)size;
    if (c->rep) {
        cnt = cx_get(c);
        if (cnt == 0) return;
    }
    switch (op & ~1u) {
    case 0xA4:
        mwrite(c, CPU_ES, di, size, mread(c, srcseg, si, size));
        idx_set(c, CPU_ESI, si + delta);
        idx_set(c, CPU_EDI, di + delta);
        break;
    case 0xA6: {
        uint32_t a = mread(c, srcseg, si, size), b = mread(c, CPU_ES, di, size);
        alu(c, 7, a, b, size);
        idx_set(c, CPU_ESI, si + delta);
        idx_set(c, CPU_EDI, di + delta);
        break;
    }
    case 0xAA:
        mwrite(c, CPU_ES, di, size, rget(c, CPU_EAX, size));
        idx_set(c, CPU_EDI, di + delta);
        break;
    case 0xAC:
        rset(c, CPU_EAX, size, mread(c, srcseg, si, size));
        idx_set(c, CPU_ESI, si + delta);
        break;
    default: /* 0xAE */
        alu(c, 7, rget(c, CPU_EAX, size), mread(c, CPU_ES, di, size), size);
        idx_set(c, CPU_EDI, di + delta);
        break;
    }
    if (c->rep) {
        bool again;
        cnt--;
        cx_set(c, cnt);
        again = cnt != 0;
        if (cmp) {
            bool zf = (c->eflags & CPU_ZF) != 0;
            if (c->rep == 0xF3 && !zf) again = false;
            if (c->rep == 0xF2 && zf) again = false;
        }
        if (again) c->eip = c->start_eip;
    }
}

static void enter_frame(cpu_t *c, uint16_t alloc, unsigned level) {
    int osz = c->op32 ? 4 : 2;
    uint32_t frame, bp = R(CPU_EBP);
    unsigned i;
    level &= 31;
    push(c, osz, rget(c, CPU_EBP, osz));
    frame = R(CPU_ESP) & 0xFFFF;
    if (level > 0) {
        for (i = 1; i < level; i++) {
            bp -= (uint32_t)osz;
            push(c, osz, mread(c, CPU_SS, bp & 0xFFFF, osz));
        }
        push(c, osz, frame);
    }
    rset(c, CPU_EBP, osz, frame);
    R(CPU_ESP) = (R(CPU_ESP) & 0xFFFF0000u) | (((R(CPU_ESP) & 0xFFFF) - alloc) & 0xFFFF);
}

static hres h_ctrl(cpu_t *c, unsigned op) {
    int osz = c->op32 ? 4 : 2;
    switch (op) {
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int32_t rel = sext(fetch8(c), 1);
        if (cond_true(c, op & 15)) jump_rel(c, rel);
        return H_OK;
    }
    case 0x180: case 0x181: case 0x182: case 0x183: case 0x184: case 0x185: case 0x186: case 0x187:
    case 0x188: case 0x189: case 0x18A: case 0x18B: case 0x18C: case 0x18D: case 0x18E: case 0x18F: {
        int32_t rel = sext(fetch_sz(c, osz), osz);
        if (cond_true(c, op & 15)) jump_rel(c, rel);
        return H_OK;
    }
    case 0xEB: jump_rel(c, sext(fetch8(c), 1)); return H_OK;
    case 0xE9: jump_rel(c, sext(fetch_sz(c, osz), osz)); return H_OK;
    case 0xE8: {
        int32_t rel = sext(fetch_sz(c, osz), osz);
        push(c, osz, c->eip);
        jump_rel(c, rel);
        return H_OK;
    }
    case 0x9A: case 0xEA: {
        uint32_t off = fetch_sz(c, osz);
        uint16_t sel = fetch16(c);
        far_to(c, sel, off, op == 0x9A);
        return H_OK;
    }
    case 0xC3: {
        uint32_t ip = pop(c, osz);
        c->eip = c->op32 ? ip : (ip & 0xFFFF);
        return H_OK;
    }
    case 0xC2: {
        uint16_t n = fetch16(c);
        uint32_t ip = pop(c, osz);
        c->eip = c->op32 ? ip : (ip & 0xFFFF);
        R(CPU_ESP) = (R(CPU_ESP) & 0xFFFF0000u) | (((R(CPU_ESP) & 0xFFFF) + n) & 0xFFFF);
        return H_OK;
    }
    case 0xCB: ret_far(c, 0); return H_OK;
    case 0xCA: ret_far(c, fetch16(c)); return H_OK;
    case 0xCF: {
        uint32_t ip = pop(c, osz), cs = pop(c, osz), fl = pop(c, osz);
        if (cs == 0 || (c->mem.sel_valid && !c->mem.sel_valid(c->mem.ctx, (uint16_t)cs)))
            FAULT(c, CPU_E_GP);
        c->seg[CPU_CS] = (uint16_t)cs;
        c->eip = c->op32 ? ip : (ip & 0xFFFF);
        c->eflags = (c->eflags & ~FLAGS_DEFINED) | (fl & FLAGS_DEFINED) | 2u;
        return H_OK;
    }
    case 0xCC: do_int(c, 3); return H_OK;
    case 0xCD: do_int(c, fetch8(c)); return H_OK;
    case 0xCE: if (c->eflags & CPU_OF) do_int(c, 4); return H_OK;
    case 0xE0: case 0xE1: case 0xE2: {
        int32_t rel = sext(fetch8(c), 1);
        uint32_t cnt = cx_get(c) - 1;
        bool zf = (c->eflags & CPU_ZF) != 0;
        cx_set(c, cnt);
        if (cnt != 0 && (op == 0xE2 || (op == 0xE1 ? zf : !zf))) jump_rel(c, rel);
        return H_OK;
    }
    case 0xE3: {
        int32_t rel = sext(fetch8(c), 1);
        if (cx_get(c) == 0) jump_rel(c, rel);
        return H_OK;
    }
    case 0xC8: {
        uint16_t alloc = fetch16(c);
        enter_frame(c, alloc, fetch8(c));
        return H_OK;
    }
    case 0xC9:
        R(CPU_ESP) = (R(CPU_ESP) & 0xFFFF0000u) | (R(CPU_EBP) & 0xFFFF);
        rset(c, CPU_EBP, osz, pop(c, osz));
        return H_OK;
    case 0x62: {
        int32_t idx, lo, hi;
        modrm(c);
        if (c->mod == 3) FAULT(c, CPU_E_UD);
        idx = sext(get_reg(c, osz), osz);
        lo = sext(mread(c, c->ea_seg, c->ea_off, osz), osz);
        hi = sext(mread(c, c->ea_seg, c->ea_off + (uint32_t)osz, osz), osz);
        if (idx < lo || idx > hi) FAULT(c, CPU_E_BOUND);
        return H_OK;
    }
    case 0xD7: {
        uint32_t off = idx_get(c, CPU_EBX) + (R(CPU_EAX) & 0xFF);
        if (!c->ad32) off &= 0xFFFF;
        rset(c, CPU_EAX, 1, mread(c, c->seg_ovr >= 0 ? c->seg_ovr : CPU_DS, off, 1));
        return H_OK;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(c, op);
        return H_OK;
    case 0xF8: c->eflags &= ~CPU_CF; return H_OK;
    case 0xF9: c->eflags |= CPU_CF; return H_OK;
    case 0xF5: c->eflags ^= CPU_CF; return H_OK;
    case 0xFA: c->eflags &= ~CPU_IF; return H_OK;
    case 0xFB: c->eflags |= CPU_IF; return H_OK;
    case 0xFC: c->eflags &= ~CPU_DF; return H_OK;
    case 0xFD: c->eflags |= CPU_DF; return H_OK;
    case 0x9F:
        rset(c, 4, 1, (c->eflags & (CPU_SF | CPU_ZF | CPU_AF | CPU_PF | CPU_CF)) | 2u);
        return H_OK;
    case 0x9E:
        set_flags(c, CPU_SF | CPU_ZF | CPU_AF | CPU_PF | CPU_CF, rget(c, 4, 1));
        return H_OK;
    case 0x9B: return H_OK; /* WAIT: nothing pending without an FPU */
    default: return H_NO;
    }
}

/* ---- x87 ---------------------------------------------------------------- */

/* Decode the ModRM, fetch the memory operand, run the FPU, store the result.
 * A faulting store restores the FPU so the instruction can restart. */
static void do_fpu(cpu_t *c, unsigned esc) {
    uint8_t in[FPU_MAX_OPERAND] = {0}, out[FPU_MAX_OPERAND];
    uint16_t ax = 0;
    fpu_op op;
    uint8_t m;
    fpu_t saved;
    unsigned i;
    modrm(c);
    m = (uint8_t)(c->mod << 6 | c->reg << 3 | c->rm);
    if (!fpu_decode(esc, m, &op)) FAULT(c, CPU_E_UD);
    for (i = 0; i < op.rd; i++) in[i] = (uint8_t)mread(c, c->ea_seg, c->ea_off + i, 1);
    saved = c->fpu;
    fpu_exec(&c->fpu, esc, m, in, out, &ax);
    for (i = 0; i < op.wr; i++) {
        cpu_status s = rawwrite(c, c->seg[c->ea_seg], c->ea_off + i, 1, out[i]);
        if (s != CPU_OK) {
            c->fpu = saved;
            FAULT(c, s);
        }
    }
    if (op.ax) rset(c, CPU_EAX, 2, ax);
}

/* ---- decode loop ------------------------------------------------------- */

static void exec(cpu_t *c) {
    unsigned op, nprefix = 0;
    c->op32 = c->ad32 = false;
    c->seg_ovr = -1;
    c->rep = 0;
    for (;;) {
        op = fetch8(c);
        switch (op) {
        case 0x26: c->seg_ovr = CPU_ES; break;
        case 0x2E: c->seg_ovr = CPU_CS; break;
        case 0x36: c->seg_ovr = CPU_SS; break;
        case 0x3E: c->seg_ovr = CPU_DS; break;
        case 0x64: c->seg_ovr = CPU_FS; break;
        case 0x65: c->seg_ovr = CPU_GS; break;
        case 0x66: c->op32 = true; break;
        case 0x67: c->ad32 = true; break;
        case 0xF0: break; /* LOCK */
        case 0xF2: case 0xF3: c->rep = (uint8_t)op; break;
        default: goto decoded;
        }
        if (++nprefix > 14) FAULT(c, CPU_E_UD);
    }
decoded:
    if (op >= 0xD8 && op <= 0xDF) { do_fpu(c, op - 0xD8u); return; }
    if (op == 0x0F) op = 0x100u | fetch8(c);
    if (op == 0xF4) c->ret = CPU_HALT;
    if (h_move(c, op) == H_OK) return;
    if (h_alu(c, op) == H_OK) return;
    if (h_ctrl(c, op) == H_OK) return;
    FAULT(c, CPU_E_UD);
}

void cpu_set_step_hook(cpu_t *c, cpu_hook_fn fn, void *ctx) {
    c->step_fn = fn;
    c->step_ctx = ctx;
}

cpu_status cpu_step(cpu_t *c) {
    int j;
    if (c->step_fn) c->step_fn(c, c->step_ctx);
    if (c->nhooks) {
        unsigned h = ((unsigned)c->seg[CPU_CS] * 0x9E3779B1u ^ c->eip * 0x85EBCA6Bu) & (CPU_HOOK_SLOTS - 1);
        while (c->hooks[h].used) {
            if (c->hooks[h].sel == c->seg[CPU_CS] && c->hooks[h].off == c->eip) {
                uint16_t cs0 = c->seg[CPU_CS];
                uint32_t ip0 = c->eip;
                cpu_status s = c->hooks[h].fn(c, c->hooks[h].ctx);
                if (s != CPU_OK) return s;
                if (c->seg[CPU_CS] != cs0 || c->eip != ip0) return CPU_OK;
                break;
            }
            h = (h + 1) & (CPU_HOOK_SLOTS - 1);
        }
    }
    c->start_eip = c->eip;
    c->start_cs = c->seg[CPU_CS];
    c->start_esp = R(CPU_ESP);
    c->ret = CPU_OK;
    j = setjmp(c->jb);
    if (j) {
        c->eip = c->start_eip;
        c->seg[CPU_CS] = c->start_cs;
        R(CPU_ESP) = c->start_esp;
        return (cpu_status)j;
    }
    exec(c);
    return c->ret;
}

cpu_status cpu_run(cpu_t *c, uint64_t max_steps) {
    uint64_t i;
    for (i = 0; i < max_steps; i++) {
        cpu_status s = cpu_step(c);
        if (s != CPU_OK) return s;
    }
    return CPU_BUDGET;
}

/* ---- host helpers ------------------------------------------------------ */

void cpu_init(cpu_t *c, const cpu_mem *mem) {
    memset(c, 0, sizeof *c);
    c->mem = *mem;
    c->eflags = 2;
    c->seg_ovr = -1;
    fpu_init(&c->fpu);
}

static unsigned hook_hash(uint16_t sel, uint32_t off) {
    return ((unsigned)sel * 0x9E3779B1u ^ off * 0x85EBCA6Bu) & (CPU_HOOK_SLOTS - 1);
}

bool cpu_add_hook(cpu_t *c, uint16_t sel, uint32_t off, cpu_hook_fn fn, void *ctx) {
    unsigned h = hook_hash(sel, off);
    while (c->hooks[h].used) {
        if (c->hooks[h].sel == sel && c->hooks[h].off == off) goto store; /* replace */
        h = (h + 1) & (CPU_HOOK_SLOTS - 1);
    }
    if (c->nhooks >= CPU_HOOK_SLOTS / 2) return false;
    c->nhooks++;
store:
    c->hooks[h].sel = sel;
    c->hooks[h].off = off;
    c->hooks[h].fn = fn;
    c->hooks[h].ctx = ctx;
    c->hooks[h].used = true;
    return true;
}

void cpu_set_int_handler(cpu_t *c, cpu_int_fn fn, void *ctx) {
    c->int_fn = fn;
    c->int_ctx = ctx;
}

cpu_status cpu_read8(cpu_t *c, uint16_t sel, uint32_t off, uint8_t *v) {
    uint32_t t;
    cpu_status s = rawread(c, sel, off, 1, &t);
    if (s == CPU_OK) *v = (uint8_t)t;
    return s;
}
cpu_status cpu_read16(cpu_t *c, uint16_t sel, uint32_t off, uint16_t *v) {
    uint32_t t;
    cpu_status s = rawread(c, sel, off, 2, &t);
    if (s == CPU_OK) *v = (uint16_t)t;
    return s;
}
cpu_status cpu_read32(cpu_t *c, uint16_t sel, uint32_t off, uint32_t *v) {
    return rawread(c, sel, off, 4, v);
}
cpu_status cpu_write8(cpu_t *c, uint16_t sel, uint32_t off, uint8_t v) {
    return rawwrite(c, sel, off, 1, v);
}
cpu_status cpu_write16(cpu_t *c, uint16_t sel, uint32_t off, uint16_t v) {
    return rawwrite(c, sel, off, 2, v);
}
cpu_status cpu_write32(cpu_t *c, uint16_t sel, uint32_t off, uint32_t v) {
    return rawwrite(c, sel, off, 4, v);
}

cpu_status cpu_push16(cpu_t *c, uint16_t v) {
    uint32_t sp = ((R(CPU_ESP) & 0xFFFF) - 2u) & 0xFFFF;
    cpu_status s = rawwrite(c, c->seg[CPU_SS], sp, 2, v);
    if (s == CPU_OK) R(CPU_ESP) = (R(CPU_ESP) & 0xFFFF0000u) | sp;
    return s;
}
cpu_status cpu_pop16(cpu_t *c, uint16_t *v) {
    uint32_t sp = R(CPU_ESP) & 0xFFFF, t;
    cpu_status s = rawread(c, c->seg[CPU_SS], sp, 2, &t);
    if (s != CPU_OK) return s;
    *v = (uint16_t)t;
    R(CPU_ESP) = (R(CPU_ESP) & 0xFFFF0000u) | ((sp + 2) & 0xFFFF);
    return CPU_OK;
}
cpu_status cpu_retf(cpu_t *c, uint16_t nbytes) {
    uint32_t sp = R(CPU_ESP) & 0xFFFF, ip, cs;
    cpu_status s;
    if ((s = rawread(c, c->seg[CPU_SS], sp, 2, &ip)) != CPU_OK) return s;
    if ((s = rawread(c, c->seg[CPU_SS], (sp + 2) & 0xFFFF, 2, &cs)) != CPU_OK) return s;
    if (cs == 0 || (c->mem.sel_valid && !c->mem.sel_valid(c->mem.ctx, (uint16_t)cs))) return CPU_E_GP;
    c->eip = ip;
    c->seg[CPU_CS] = (uint16_t)cs;
    R(CPU_ESP) = (R(CPU_ESP) & 0xFFFF0000u) | ((sp + 4 + nbytes) & 0xFFFF);
    return CPU_OK;
}

/* ---- length decoder ---------------------------------------------------- */

enum {
    A_M = 1,      /* ModRM follows */
    A_B = 2,      /* imm8 */
    A_W = 4,      /* imm16 */
    A_Z = 8,      /* imm16/32 by operand size */
    A_MOFFS = 16, /* 16/32-bit offset by address size */
    A_G3 = 32,    /* F6/F7: TEST immediate only when reg < 2 */
    A_UNSUP = 64, /* not implemented by the interpreter */
    A_FPU = 128   /* x87 escape */
};

/* Attributes of a one-byte opcode, or of 0F xx when op >= 0x100. Must mirror
 * the handlers above; test_decode_matches_executor enforces it. */
static unsigned op_attr(unsigned op) {
    if (op >= 0x100) {
        unsigned x = op & 0xFF;
        if (x >= 0x80 && x <= 0x8F) return A_Z;
        if (x >= 0x90 && x <= 0x9F) return A_M;
        switch (x) {
        case 0xA0: case 0xA1: case 0xA8: case 0xA9: return 0;
        case 0x00: case 0x02: case 0x03:
        case 0xA3: case 0xAB: case 0xB3: case 0xBB: case 0xAF: case 0xB2: case 0xB4: case 0xB5:
        case 0xB6: case 0xB7: case 0xBE: case 0xBF: case 0xBC: case 0xBD: case 0xA5: case 0xAD:
            return A_M;
        case 0xA4: case 0xAC: case 0xBA: return A_M | A_B;
        default: break;
        }
        /* everything else in the 0F map is unsupported; lengths are a best guess */
        if (x <= 0x03 || (x >= 0x18 && x <= 0x23) || x == 0x24 || x == 0x26 || x >= 0x40) return A_M | A_UNSUP;
        return A_UNSUP;
    }
    if (op < 0x40) {
        unsigned lo = op & 7;
        if (lo < 4) return A_M;
        if (lo == 4) return A_B;
        if (lo == 5) return A_Z;
        if (op == 0x06 || op == 0x07 || op == 0x0E || op == 0x16 || op == 0x17 || op == 0x1E || op == 0x1F)
            return 0;
        return A_UNSUP; /* DAA DAS AAA AAS */
    }
    if (op < 0x60) return 0;
    switch (op) {
    case 0x60: case 0x61: return 0;
    case 0x62: return A_M;
    case 0x63: return A_M | A_UNSUP;
    case 0x68: return A_Z;
    case 0x69: return A_M | A_Z;
    case 0x6A: return A_B;
    case 0x6B: return A_M | A_B;
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: return A_UNSUP;
    case 0x80: case 0x83: return A_M | A_B;
    case 0x81: return A_M | A_Z;
    case 0x82: return A_M | A_B | A_UNSUP;
    case 0x9A: case 0xEA: return A_Z | A_W;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: return A_MOFFS;
    case 0xA8: return A_B;
    case 0xA9: return A_Z;
    case 0xC0: case 0xC1: case 0xC6: return A_M | A_B;
    case 0xC2: case 0xCA: return A_W;
    case 0xC7: return A_M | A_Z;
    case 0xC8: return A_W | A_B;
    case 0xCD: return 0; /* handled by cpu_decode */
    case 0xD4: case 0xD5: return A_B | A_UNSUP;
    case 0xD6: return A_UNSUP;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xEB: return A_B;
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: return A_B | A_UNSUP;
    case 0xE8: case 0xE9: return A_Z;
    case 0xEC: case 0xED: case 0xEE: case 0xEF: case 0xF1: return A_UNSUP;
    case 0xF6: return A_M | A_G3;
    case 0xF7: return A_M | A_G3 | A_Z;
    default: break;
    }
    if (op >= 0x70 && op <= 0x7F) return A_B;
    if (op >= 0xB0 && op <= 0xB7) return A_B;
    if (op >= 0xB8 && op <= 0xBF) return A_Z;
    if (op >= 0xD8 && op <= 0xDF) return A_M | A_FPU;
    if ((op >= 0x84 && op <= 0x8F) || op == 0xC4 || op == 0xC5 || (op >= 0xD0 && op <= 0xD3) ||
        op == 0xFE || op == 0xFF)
        return A_M;
    return 0;
}

cpu_status cpu_decode(const cpu_mem *m, uint16_t cs, uint32_t ip, cpu_insn *out) {
    unsigned n = 0, attr, op, reg = 0;
    bool op32 = false, ad32 = false;
    uint8_t b = 0;
    cpu_status s;
#define NEXT()                                                                  \
    do {                                                                        \
        if (n >= 15) return CPU_E_UD;                                           \
        if ((s = m->read8(m->ctx, cs, ip + n, &b)) != CPU_OK) return s;         \
        n++;                                                                    \
    } while (0)
    for (;;) {
        NEXT();
        if (b == 0x66) op32 = true;
        else if (b == 0x67) ad32 = true;
        else if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65 ||
                 b == 0xF0 || b == 0xF2 || b == 0xF3) continue;
        else break;
    }
    op = b;
    if (b == 0x0F) {
        NEXT();
        op = 0x100u | b;
    }
    attr = op_attr(op);
    if (op == 0xCD) { /* INT n: the WIN87EM vectors carry an escape ModRM (see emu_int) */
        NEXT();
        attr = 0;
        if (b >= 0x34 && b <= 0x3F) {
            attr = A_FPU;
            if (b == 0x3F) attr |= A_UNSUP;
            else if (b != 0x3D) {
                attr |= A_M;
                if (b == 0x3C || b == 0x3E) {
                    NEXT();
                    if (b < 0xD8 || b > 0xDF) attr = A_FPU | A_UNSUP;
                }
            }
        }
    }
    if (attr & A_M) {
        unsigned mod, rm;
        NEXT();
        mod = b >> 6; reg = (b >> 3) & 7; rm = b & 7;
        if (mod != 3) {
            unsigned disp = 0, k;
            if (!ad32) {
                if (mod == 0 && rm == 6) disp = 2;
                else disp = mod == 1 ? 1 : mod == 2 ? 2 : 0;
            } else {
                if (rm == 4) {
                    NEXT();
                    if ((b & 7) == 5 && mod == 0) disp = 4;
                }
                if (mod == 0 && rm == 5) disp = 4;
                else if (mod == 1) disp = 1;
                else if (mod == 2) disp = 4;
            }
            for (k = 0; k < disp; k++) NEXT();
        }
    }
    {
        unsigned imm = 0, k;
        if (attr & A_B) imm += 1;
        if (attr & A_W) imm += 2;
        if (attr & A_Z) imm += op32 ? 4 : 2;
        if (attr & A_MOFFS) imm += ad32 ? 4 : 2;
        if ((attr & A_G3) && reg >= 2) imm = 0;
        if (op == 0xF6 && reg < 2) imm = 1;
        for (k = 0; k < imm; k++) NEXT();
    }
#undef NEXT
    out->length = n;
    out->opcode = op >= 0x100 ? 0x0F00u | (op & 0xFF) : op;
    out->fpu = (attr & A_FPU) != 0;
    out->supported = !(attr & A_UNSUP);
    return CPU_OK;
}

const char *cpu_strstatus(cpu_status s) {
    switch (s) {
    case CPU_OK: return "ok";
    case CPU_HALT: return "halt";
    case CPU_BUDGET: return "budget exhausted";
    case CPU_E_UD: return "invalid opcode";
    case CPU_E_DE: return "divide error";
    case CPU_E_GP: return "general protection";
    case CPU_E_SEL: return "bad selector";
    case CPU_E_LIMIT: return "segment limit exceeded";
    case CPU_E_BOUND: return "BOUND range exceeded";
    case CPU_E_FPU: return "overlay manager call (INT 3Fh)";
    case CPU_E_INT: return "unhandled interrupt";
    }
    return "unknown";
}
