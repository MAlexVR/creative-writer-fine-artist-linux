#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
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

/* ---- flat test machine ------------------------------------------------- */

#define MEMSZ 0x100000u
#define S_CS 0x10
#define S_DS 0x18
#define S_ES 0x20
#define S_SS 0x28
#define S_FS 0x30
#define S_GS 0x38
#define S_SMALL 0x40 /* limit 0x0F */
#define S_DS2 0x48
#define S_RO 0x50  /* read-only data, limit 0xFF */
#define S_XO 0x58  /* execute-only code, limit 0x1F */
#define S_KRN 0x60 /* DPL 0 data */
#define S_NP 0x68  /* not-present data */
#define CSB 0x00000u
#define DSB 0x10000u
#define ESB 0x20000u
#define SSB 0x30000u
#define FSB 0x40000u
#define GSB 0x50000u
#define SMB 0x60000u
#define DS2B 0x70000u
#define CODE_OFF 0x100u

typedef struct { uint16_t sel; uint32_t base, limit; } tseg;
static const tseg g_segs[] = {
    {S_CS, CSB, 0xFFFF}, {S_DS, DSB, 0xFFFF}, {S_ES, ESB, 0xFFFF},
    {S_SS, SSB, 0xFFFF}, {S_FS, FSB, 0xFFFF}, {S_GS, GSB, 0xFFFF},
    {S_SMALL, SMB, 0x0F}, {S_DS2, DS2B, 0xFFFF},
    {S_RO, 0x80000, 0xFF}, {S_XO, 0x90000, 0x1F}, {S_KRN, 0xA0000, 0xFFFF}, {S_NP, 0xB0000, 0xFFFF},
};
static uint8_t g_mem[MEMSZ];
static cpu_t g_cpu;
static unsigned g_n16, g_n32; /* wide-callback usage counters */

static const tseg *find_seg(uint16_t sel) {
    size_t i;
    for (i = 0; i < sizeof g_segs / sizeof g_segs[0]; i++)
        if (g_segs[i].sel == sel) return &g_segs[i];
    return NULL;
}

static cpu_status t_xlate(uint16_t sel, uint32_t off, uint32_t *lin) {
    const tseg *s = find_seg(sel);
    if (!s) return CPU_E_SEL;
    if (off > s->limit) return CPU_E_LIMIT;
    *lin = s->base + off;
    return *lin < MEMSZ ? CPU_OK : CPU_E_LIMIT;
}
static cpu_status t_r8(void *x, uint16_t sel, uint32_t off, uint8_t *v) {
    uint32_t l; cpu_status s = t_xlate(sel, off, &l); (void)x;
    if (s == CPU_OK) *v = g_mem[l];
    return s;
}
static cpu_status t_w8(void *x, uint16_t sel, uint32_t off, uint8_t v) {
    uint32_t l; cpu_status s = t_xlate(sel, off, &l); (void)x;
    if (s == CPU_OK) g_mem[l] = v;
    return s;
}
static bool t_valid(void *x, uint16_t sel) { (void)x; return find_seg(sel) != NULL; }
static bool t_info(void *x, uint16_t sel, cpu_selinfo *o) {
    const tseg *s = find_seg(sel);
    (void)x;
    if (!s) return false;
    o->limit = s->limit;
    o->access = sel == S_CS ? 0xFA : sel == S_RO ? 0xF0 : sel == S_XO ? 0xF8 :
                sel == S_KRN ? 0x92 : sel == S_NP ? 0x72 : 0xF2;
    return true;
}

static cpu_status t_r16(void *x, uint16_t sel, uint32_t off, uint16_t *v) {
    uint8_t a, b; cpu_status s;
    g_n16++;
    if ((s = t_r8(x, sel, off, &a)) != CPU_OK) return s;
    if ((s = t_r8(x, sel, off + 1, &b)) != CPU_OK) return s;
    *v = (uint16_t)(a | b << 8);
    return CPU_OK;
}
static cpu_status t_w16(void *x, uint16_t sel, uint32_t off, uint16_t v) {
    uint8_t d; cpu_status s;
    g_n16++;
    if ((s = t_r8(x, sel, off + 1, &d)) != CPU_OK) return s; /* probe the high byte first */
    if ((s = t_w8(x, sel, off, (uint8_t)v)) != CPU_OK) return s;
    return t_w8(x, sel, off + 1, (uint8_t)(v >> 8));
}
static cpu_status t_r32(void *x, uint16_t sel, uint32_t off, uint32_t *v) {
    uint16_t a, b; cpu_status s;
    g_n32++;
    if ((s = t_r16(x, sel, off, &a)) != CPU_OK) return s;
    if ((s = t_r16(x, sel, off + 2, &b)) != CPU_OK) return s;
    *v = (uint32_t)a | (uint32_t)b << 16;
    return CPU_OK;
}

static cpu_mem t_iface(bool wide) {
    cpu_mem m;
    memset(&m, 0, sizeof m);
    m.read8 = t_r8; m.write8 = t_w8; m.sel_valid = t_valid; m.sel_info = t_info;
    if (wide) { m.read16 = t_r16; m.write16 = t_w16; m.read32 = t_r32; }
    return m;
}

static void reset(void) {
    cpu_mem m = t_iface(false);
    memset(g_mem, 0, sizeof g_mem);
    cpu_init(&g_cpu, &m);
    g_cpu.seg[CPU_CS] = S_CS; g_cpu.seg[CPU_DS] = S_DS; g_cpu.seg[CPU_ES] = S_ES;
    g_cpu.seg[CPU_SS] = S_SS; g_cpu.seg[CPU_FS] = S_FS; g_cpu.seg[CPU_GS] = S_GS;
    g_cpu.eip = CODE_OFF;
    g_cpu.r[CPU_ESP] = 0x8000;
}

static cpu_status run_code(const uint8_t *code, size_t n) {
    g_cpu.eip = CODE_OFF;
    memcpy(g_mem + CSB + CODE_OFF, code, n);
    g_mem[CSB + CODE_OFF + n] = 0xF4; /* HLT */
    return cpu_run(&g_cpu, 100000);
}
#define RUN(...) run_code((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))
/* run and expect the closing HLT */
#define RUN_OK(...) CHECK_EQ(RUN(__VA_ARGS__), CPU_HALT)

#define EAX g_cpu.r[CPU_EAX]
#define ECX g_cpu.r[CPU_ECX]
#define EDX g_cpu.r[CPU_EDX]
#define EBX g_cpu.r[CPU_EBX]
#define ESP g_cpu.r[CPU_ESP]
#define EBP g_cpu.r[CPU_EBP]
#define ESI g_cpu.r[CPU_ESI]
#define EDI g_cpu.r[CPU_EDI]
#define EIP g_cpu.eip
#define FL g_cpu.eflags
#define SEG(i) g_cpu.seg[i]

static uint8_t rd8(uint32_t l) { return g_mem[l]; }
static uint16_t rd16(uint32_t l) { return (uint16_t)(g_mem[l] | g_mem[l + 1] << 8); }
static uint32_t rd32(uint32_t l) { return rd16(l) | (uint32_t)rd16(l + 2) << 16; }
static void wr8(uint32_t l, unsigned v) { g_mem[l] = (uint8_t)v; }
static void wr16(uint32_t l, unsigned v) { wr8(l, v); wr8(l + 1, v >> 8); }
static void wr32(uint32_t l, uint32_t v) { wr16(l, v & 0xFFFF); wr16(l + 2, v >> 16); }

#define FLAGS_MASK (CPU_CF | CPU_PF | CPU_AF | CPU_ZF | CPU_SF | CPU_OF)
#define CHECK_FLAGS(want) CHECK_EQ(FL & FLAGS_MASK, (want))

/* ======================================================================
 * Group 1: data movement, addressing, stack, hooks, faults
 * ====================================================================== */

static void test_mov_imm(void) {
    printf("test_mov_imm\n");
    reset();
    RUN_OK(0xB0, 0x11, 0xB1, 0x22, 0xB2, 0x33, 0xB3, 0x44,
           0xB4, 0x55, 0xB5, 0x66, 0xB6, 0x77, 0xB7, 0x88);
    CHECK_EQ(EAX, 0x5511); CHECK_EQ(ECX, 0x6622);
    CHECK_EQ(EDX, 0x7733); CHECK_EQ(EBX, 0x8844);
    CHECK_EQ(EIP, CODE_OFF + 17); /* 16 bytes + HLT consumed */
    reset();
    EAX = 0xAABB0000; ESI = 0x12345678;
    RUN_OK(0xB8, 0x34, 0x12, 0xBE, 0xCD, 0xAB); /* mov ax,1234 ; mov si,abcd */
    CHECK_EQ(EAX, 0xAABB1234); CHECK_EQ(ESI, 0x1234ABCD);
    reset();
    RUN_OK(0x66, 0xB8, 0x78, 0x56, 0x34, 0x12, 0x66, 0xBF, 1, 2, 3, 4);
    CHECK_EQ(EAX, 0x12345678); CHECK_EQ(EDI, 0x04030201);
    reset(); /* mov r/m, imm and r/m <- r forms */
    EBX = 0x10;
    RUN_OK(0xC6, 0x07, 0x9A,             /* mov byte [bx],9a */
           0xC7, 0x47, 0x02, 0xEF, 0xBE, /* mov word [bx+2],beef */
           0x66, 0xC7, 0x47, 0x04, 1, 2, 3, 4);
    CHECK_EQ(rd8(DSB + 0x10), 0x9A); CHECK_EQ(rd16(DSB + 0x12), 0xBEEF);
    CHECK_EQ(rd32(DSB + 0x14), 0x04030201);
    reset(); /* register to register, 8-bit high/low views */
    EAX = 0x1234; ECX = 0xFFFFFFFF;
    RUN_OK(0x88, 0xE0, /* mov al,ah */ 0x88, 0xC5, /* mov ch,al */ 0x89, 0xC2 /* mov dx,ax */);
    CHECK_EQ(EAX, 0x1212); CHECK_EQ(ECX, 0xFFFF12FF); CHECK_EQ(EDX, 0x1212);
    reset();
    EAX = 0xDEADBEEF;
    RUN_OK(0x66, 0x89, 0xC3, 0x8B, 0xCB); /* mov ebx,eax ; mov cx,bx */
    CHECK_EQ(EBX, 0xDEADBEEF); CHECK_EQ(ECX, 0xBEEF);
}

static void test_modrm16(void) {
    printf("test_modrm16\n");
    reset();
    EBX = 0x10; ESI = 0x20; EDI = 0x05; EBP = 0x40;
    wr16(DSB + 0x30, 0xBEEF); wr16(DSB + 0x15, 0x1111); wr16(DSB + 0x20, 0x2222);
    wr16(DSB + 0x05, 0x3333); wr16(DSB + 0x10, 0x4444); wr16(DSB + 0x0E, 0x5555);
    wr16(DSB + 0x300, 0x6666);
    wr16(SSB + 0x60, 0xA0A0); wr16(SSB + 0x45, 0xB0B0); wr16(SSB + 0x40, 0xC0C0);
    wr16(SSB + 0x44, 0xD0D0);
    RUN_OK(0x8B, 0x00); CHECK_EQ(EAX, 0xBEEF);               /* [bx+si] */
    reset(); EBX = 0x10; EDI = 5; wr16(DSB + 0x15, 0x1111);
    RUN_OK(0x8B, 0x01); CHECK_EQ(EAX, 0x1111);               /* [bx+di] */
    reset(); EBP = 0x40; ESI = 0x20; wr16(SSB + 0x60, 0xA0A0); wr16(DSB + 0x60, 1);
    RUN_OK(0x8B, 0x02); CHECK_EQ(EAX, 0xA0A0);               /* [bp+si] -> SS */
    reset(); EBP = 0x40; EDI = 5; wr16(SSB + 0x45, 0xB0B0);
    RUN_OK(0x8B, 0x03); CHECK_EQ(EAX, 0xB0B0);               /* [bp+di] -> SS */
    reset(); ESI = 0x20; wr16(DSB + 0x20, 0x2222);
    RUN_OK(0x8B, 0x04); CHECK_EQ(EAX, 0x2222);               /* [si] */
    reset(); EDI = 5; wr16(DSB + 0x05, 0x3333);
    RUN_OK(0x8B, 0x05); CHECK_EQ(EAX, 0x3333);               /* [di] */
    reset(); wr16(DSB + 0x300, 0x6666);
    RUN_OK(0x8B, 0x06, 0x00, 0x03); CHECK_EQ(EAX, 0x6666);   /* [disp16] -> DS */
    reset(); EBX = 0x10; wr16(DSB + 0x10, 0x4444);
    RUN_OK(0x8B, 0x07); CHECK_EQ(EAX, 0x4444);               /* [bx] */
    reset(); EBX = 0x10; wr16(DSB + 0x0E, 0x5555);
    RUN_OK(0x8B, 0x47, 0xFE); CHECK_EQ(EAX, 0x5555);         /* [bx-2] disp8 sign */
    reset(); EBP = 0x40; wr16(SSB + 0x44, 0xD0D0); wr16(DSB + 0x44, 7);
    RUN_OK(0x8B, 0x46, 0x04); CHECK_EQ(EAX, 0xD0D0);         /* [bp+4] -> SS */
    reset(); EBX = 0x10; wr16(DSB + 0x310, 0x7777);
    RUN_OK(0x8B, 0x87, 0x00, 0x03); CHECK_EQ(EAX, 0x7777);   /* [bx+disp16] */
    reset(); EBX = 0xFFF0; wr16(DSB + 0x10, 0x8888);
    RUN_OK(0x8B, 0x87, 0x20, 0x00); CHECK_EQ(EAX, 0x8888);   /* 16-bit EA wraps */
    reset(); EBX = 0xFFFF0010; wr16(DSB + 0x10, 0x9999);
    RUN_OK(0x8B, 0x07); CHECK_EQ(EAX, 0x9999);               /* upper EBX ignored */
    reset(); EBX = 0x10; EAX = 0xCAFE;
    RUN_OK(0x89, 0x07, 0x88, 0x67, 0x04); /* mov [bx],ax ; mov [bx+4],ah */
    CHECK_EQ(rd16(DSB + 0x10), 0xCAFE); CHECK_EQ(rd8(DSB + 0x14), 0xCA);
    reset(); EBX = 0x10; wr8(DSB + 0x10, 0x5A);
    RUN_OK(0x8A, 0x27); CHECK_EQ(EAX, 0x5A00);               /* mov ah,[bx] */
    reset(); EBX = 0x10; wr32(DSB + 0x10, 0x11223344);
    RUN_OK(0x66, 0x8B, 0x07); CHECK_EQ(EAX, 0x11223344);
}

static void test_segment_overrides(void) {
    printf("test_segment_overrides\n");
    reset(); EBX = 0x10;
    wr16(DSB + 0x10, 1); wr16(ESB + 0x10, 2); wr16(SSB + 0x10, 3);
    wr16(FSB + 0x10, 4); wr16(GSB + 0x10, 5);
    RUN_OK(0x26, 0x8B, 0x07); CHECK_EQ(EAX, 2);
    RUN_OK(0x36, 0x8B, 0x07); CHECK_EQ(EAX, 3);
    RUN_OK(0x3E, 0x8B, 0x07); CHECK_EQ(EAX, 1);
    RUN_OK(0x64, 0x8B, 0x07); CHECK_EQ(EAX, 4);
    RUN_OK(0x65, 0x8B, 0x07); CHECK_EQ(EAX, 5);
    reset(); EBP = 0x10; wr16(DSB + 0x14, 0x1234); wr16(SSB + 0x14, 0x5678);
    RUN_OK(0x8B, 0x46, 0x04); CHECK_EQ(EAX, 0x5678);         /* BP default SS */
    reset(); EBP = 0x10; wr16(DSB + 0x14, 0x1234); wr16(SSB + 0x14, 0x5678);
    RUN_OK(0x3E, 0x8B, 0x46, 0x04); CHECK_EQ(EAX, 0x1234);   /* override beats SS */
    reset(); EBX = 0x10; EAX = 0xABCD; /* write through ES override */
    RUN_OK(0x26, 0x89, 0x07); CHECK_EQ(rd16(ESB + 0x10), 0xABCD);
    CHECK_EQ(rd16(DSB + 0x10), 0);
}

static void test_modrm32(void) {
    printf("test_modrm32\n");
    reset(); EBX = 0x20; wr16(DSB + 0x20, 0x1111);
    RUN_OK(0x67, 0x8B, 0x03); CHECK_EQ(EAX, 0x1111);                 /* [ebx] */
    reset(); EAX = 0x10; ECX = 0x4; wr16(DSB + 0x20, 0x2222); /* eax + ecx*4 */
    RUN_OK(0x67, 0x8B, 0x1C, 0x88); CHECK_EQ(EBX, 0x2222);          /* mov bx,[eax+ecx*4] */
    reset(); EAX = 0x10; ECX = 0x4; wr16(DSB + 0x28, 0x3333);
    RUN_OK(0x67, 0x8B, 0x5C, 0x88, 0x08); CHECK_EQ(EBX, 0x3333);     /* +disp8 */
    reset(); wr16(DSB + 0x40, 0x4444);
    RUN_OK(0x67, 0x8B, 0x05, 0x40, 0, 0, 0); CHECK_EQ(EAX, 0x4444);  /* [disp32] */
    reset(); wr16(DSB + 0x44, 0x5555);
    RUN_OK(0x67, 0x8B, 0x04, 0x25, 0x44, 0, 0, 0); CHECK_EQ(EAX, 0x5555); /* SIB, no base */
    reset(); EBP = 0x10; wr16(SSB + 0x10, 0x6666); wr16(DSB + 0x10, 9);
    RUN_OK(0x67, 0x8B, 0x45, 0x00); CHECK_EQ(EAX, 0x6666);           /* [ebp+0] -> SS */
    reset(); wr16(SSB + 0x8000, 0x7777);
    RUN_OK(0x67, 0x8B, 0x04, 0x24); CHECK_EQ(EAX, 0x7777);           /* [esp] -> SS */
    reset(); ESI = 0x10; wr16(DSB + 0x120, 0x8888);
    RUN_OK(0x67, 0x8B, 0x04, 0x75, 0x00, 0x01, 0, 0);                /* [esi*2+0x100] */
    CHECK_EQ(EAX, 0x8888);
    reset(); EBX = 0x10000; /* beyond the 64K limit */
    CHECK_EQ(RUN(0x67, 0x8B, 0x03), CPU_E_LIMIT);
    reset(); EBX = 0x20; wr32(DSB + 0x20, 0xA1B2C3D4);
    RUN_OK(0x66, 0x67, 0x8B, 0x03); CHECK_EQ(EAX, 0xA1B2C3D4);       /* mov eax,[ebx] */
    reset(); EBX = 0x20; wr16(DSB + 0x1C, 0x9999);
    RUN_OK(0x67, 0x8B, 0x4B, 0xFC); CHECK_EQ(ECX, 0x9999);           /* [ebx-4] */
    reset(); EAX = 1; ECX = 2; EDX = 4; wr16(DSB + 0x1 + 0x2 * 8 + 0, 0x1357);
    RUN_OK(0x67, 0x8B, 0x34, 0xC8); CHECK_EQ(ESI, 0x1357);           /* [eax+ecx*8] */
}

static void test_moffs(void) {
    printf("test_moffs\n");
    reset(); wr8(DSB + 0x1234, 0x5A); wr16(DSB + 0x1236, 0xBEEF);
    RUN_OK(0xA0, 0x34, 0x12); CHECK_EQ(EAX, 0x5A);
    RUN_OK(0xA1, 0x36, 0x12); CHECK_EQ(EAX, 0xBEEF);
    EAX = 0x77; RUN_OK(0xA2, 0x40, 0x12); CHECK_EQ(rd8(DSB + 0x1240), 0x77);
    EAX = 0x1234; RUN_OK(0xA3, 0x42, 0x12); CHECK_EQ(rd16(DSB + 0x1242), 0x1234);
    reset(); wr32(DSB + 0x100, 0x01020304); wr32(ESB + 0x100, 0x0A0B0C0D);
    RUN_OK(0x67, 0xA1, 0x00, 0x01, 0, 0); CHECK_EQ(EAX, 0x0304);     /* 32-bit moffs */
    RUN_OK(0x66, 0x67, 0xA1, 0x00, 0x01, 0, 0); CHECK_EQ(EAX, 0x01020304);
    EAX = 0; RUN_OK(0x26, 0xA1, 0x00, 0x01); CHECK_EQ(EAX, 0x0C0D);  /* override */
    EAX = 0x01020304;
    RUN_OK(0x66, 0xA3, 0x08, 0x01); CHECK_EQ(rd32(DSB + 0x108), 0x01020304);
}

static void test_segment_moves(void) {
    printf("test_segment_moves\n");
    reset(); EAX = S_DS2;
    RUN_OK(0x8E, 0xD8); CHECK_EQ(SEG(CPU_DS), S_DS2);                /* mov ds,ax */
    reset(); EAX = S_DS2;
    RUN_OK(0x8E, 0xC0, 0x8E, 0xE0, 0x8E, 0xE8, 0x8E, 0xD0);          /* es fs gs ss */
    CHECK_EQ(SEG(CPU_ES), S_DS2); CHECK_EQ(SEG(CPU_FS), S_DS2);
    CHECK_EQ(SEG(CPU_GS), S_DS2); CHECK_EQ(SEG(CPU_SS), S_DS2);
    reset(); EAX = 0xFFFF0000;
    RUN_OK(0x8C, 0xD8); CHECK_EQ(EAX, 0xFFFF0000 | S_DS);            /* mov ax,ds */
    reset(); EAX = 0xFFFFFFFF;
    RUN_OK(0x66, 0x8C, 0xC0); CHECK_EQ(EAX, S_ES);                   /* zero-extended */
    reset(); EBX = 0x10;
    RUN_OK(0x8C, 0x27); CHECK_EQ(rd16(DSB + 0x10), S_FS);            /* mov [bx],fs: reg=4 */
    reset(); EBX = 0x10; wr16(DSB + 0x10, S_DS2);
    RUN_OK(0x8E, 0x1F); CHECK_EQ(SEG(CPU_DS), S_DS2);                /* mov ds,[bx] */
    reset(); EAX = 0;
    RUN_OK(0x8E, 0xC0); CHECK_EQ(SEG(CPU_ES), 0);                    /* null selector OK */
    CHECK_EQ(RUN(0x26, 0xA1, 0, 0), CPU_E_SEL);                      /* ...but unusable */
    reset(); EAX = 0x99; /* not in the table */
    CHECK_EQ(RUN(0x8E, 0xD8), CPU_E_GP); CHECK_EQ(SEG(CPU_DS), S_DS);
    CHECK_EQ(EIP, CODE_OFF);
    reset(); EAX = 0;
    CHECK_EQ(RUN(0x8E, 0xD0), CPU_E_GP);                             /* null SS */
    reset(); EAX = S_DS2; CHECK_EQ(RUN(0x8E, 0xC8), CPU_E_UD);       /* mov cs,ax */
    reset(); CHECK_EQ(RUN(0x8C, 0xF0), CPU_E_UD);                    /* sreg 6 */
    reset(); CHECK_EQ(RUN(0x8E, 0xF0), CPU_E_UD);
    reset(); /* LES/LDS/LFS/LGS/LSS */
    EBX = 0x10; wr16(DSB + 0x10, 0x1234); wr16(DSB + 0x12, S_DS2);
    RUN_OK(0xC4, 0x0F); CHECK_EQ(ECX & 0xFFFF, 0x1234); CHECK_EQ(SEG(CPU_ES), S_DS2); /* les cx,[bx] */
    RUN_OK(0xC5, 0x17); CHECK_EQ(EDX & 0xFFFF, 0x1234); CHECK_EQ(SEG(CPU_DS), S_DS2); /* lds dx,[bx] */
    reset(); EBX = 0x10; wr16(DSB + 0x10, 0x4321); wr16(DSB + 0x12, S_ES);
    RUN_OK(0x0F, 0xB4, 0x0F); CHECK_EQ(ECX & 0xFFFF, 0x4321); CHECK_EQ(SEG(CPU_FS), S_ES);
    RUN_OK(0x0F, 0xB5, 0x17); CHECK_EQ(SEG(CPU_GS), S_ES);
    RUN_OK(0x0F, 0xB2, 0x1F); CHECK_EQ(SEG(CPU_SS), S_ES);
    reset(); EBX = 0x10; wr32(DSB + 0x10, 0xAABBCCDD); wr16(DSB + 0x14, S_ES);
    RUN_OK(0x66, 0xC4, 0x0F); CHECK_EQ(ECX, 0xAABBCCDD); CHECK_EQ(SEG(CPU_ES), S_ES); /* 16:32 */
    reset(); CHECK_EQ(RUN(0xC4, 0xC0), CPU_E_UD);                    /* les ax,ax */
    reset(); EBX = 0x10; wr16(DSB + 0x12, 0x99);
    CHECK_EQ(RUN(0xC5, 0x0F), CPU_E_GP); CHECK_EQ(SEG(CPU_DS), S_DS);
}

static void test_lea_xchg(void) {
    printf("test_lea_xchg\n");
    reset(); EBX = 1; ESI = 2;
    RUN_OK(0x8D, 0x40, 0x05); CHECK_EQ(EAX, 8);                      /* lea ax,[bx+si+5] */
    reset(); EBX = 0xFFFF; EAX = 0xAAAA0000;
    RUN_OK(0x8D, 0x47, 0x02); CHECK_EQ(EAX, 0xAAAA0001);             /* wraps at 16 bits */
    reset(); EBX = 3; EAX = 0xFFFFFFFF;
    RUN_OK(0x26, 0x8D, 0x47, 0x01); CHECK_EQ(EAX, 0xFFFF0004);       /* override ignored */
    reset(); EBX = 0x10; RUN_OK(0x66, 0x8D, 0x47, 0x01); CHECK_EQ(EAX, 0x11);
    reset(); EAX = 7;
    RUN_OK(0x66, 0x67, 0x8D, 0x04, 0x40); CHECK_EQ(EAX, 21);         /* eax*3 */
    reset(); EAX = 0x20000; EBX = 0x20000;
    RUN_OK(0x67, 0x8D, 0x0C, 0x03); CHECK_EQ(ECX & 0xFFFF, 0);       /* 16-bit dest of 32-bit EA */
    RUN_OK(0x66, 0x67, 0x8D, 0x0C, 0x03); CHECK_EQ(ECX, 0x40000);
    reset(); CHECK_EQ(RUN(0x8D, 0xC0), CPU_E_UD);
    reset(); EBX = 0x10; EAX = 0x1111; wr16(DSB + 0x10, 0x2222);
    RUN_OK(0x87, 0x07); CHECK_EQ(EAX, 0x2222); CHECK_EQ(rd16(DSB + 0x10), 0x1111);
    reset(); EAX = 0x00AA; ECX = 0x0055;
    RUN_OK(0x86, 0xC1); CHECK_EQ(EAX, 0x55); CHECK_EQ(ECX, 0xAA);    /* xchg cl,al */
    reset(); EAX = 1; EBX = 0xFFFF0002;
    RUN_OK(0x93); CHECK_EQ(EAX, 2); CHECK_EQ(EBX, 0xFFFF0001);
    reset(); EAX = 0x11111111; ECX = 0x22222222;
    RUN_OK(0x66, 0x91); CHECK_EQ(EAX, 0x22222222); CHECK_EQ(ECX, 0x11111111);
    reset(); EAX = 5; FL = 0x2 | CPU_CF;
    RUN_OK(0x90); CHECK_EQ(EAX, 5); CHECK_EQ(FL, 0x2 | CPU_CF);      /* nop */
    reset(); EAX = 5; EDI = 9; RUN_OK(0x87, 0xF8); CHECK_EQ(EAX, 9); CHECK_EQ(EDI, 5); /* xchg ax,di */
}

static void test_stack(void) {
    printf("test_stack\n");
    reset(); EAX = 0x1234; EBX = 0xDEAD;
    RUN_OK(0x50, 0x53); CHECK_EQ(ESP, 0x7FFC);
    CHECK_EQ(rd16(SSB + 0x7FFE), 0x1234); CHECK_EQ(rd16(SSB + 0x7FFC), 0xDEAD);
    RUN_OK(0x59, 0x5A); CHECK_EQ(ECX & 0xFFFF, 0xDEAD); CHECK_EQ(EDX & 0xFFFF, 0x1234);
    CHECK_EQ(ESP, 0x8000);
    reset(); RUN_OK(0x54); CHECK_EQ(rd16(SSB + 0x7FFE), 0x8000);     /* push sp: old value */
    reset(); EAX = 0x11223344; RUN_OK(0x66, 0x50); CHECK_EQ(ESP, 0x7FFC); CHECK_EQ(rd32(SSB + 0x7FFC), 0x11223344);
    RUN_OK(0x66, 0x5B); CHECK_EQ(EBX, 0x11223344); CHECK_EQ(ESP, 0x8000);
    reset(); ESP = 0xABCD0000; EAX = 0x1234; /* SP wraps, upper ESP untouched */
    RUN_OK(0x50); CHECK_EQ(ESP, 0xABCDFFFE); CHECK_EQ(rd16(SSB + 0xFFFE), 0x1234);
    reset();
    RUN_OK(0x68, 0x34, 0x12, 0x6A, 0xFF, 0x6A, 0x7F);
    CHECK_EQ(rd16(SSB + 0x7FFE), 0x1234); CHECK_EQ(rd16(SSB + 0x7FFC), 0xFFFF);
    CHECK_EQ(rd16(SSB + 0x7FFA), 0x007F);
    reset(); RUN_OK(0x66, 0x6A, 0xFF, 0x66, 0x68, 1, 2, 3, 4);
    CHECK_EQ(rd32(SSB + 0x7FFC), 0xFFFFFFFF); CHECK_EQ(rd32(SSB + 0x7FF8), 0x04030201);
    reset(); /* segment push/pop */
    RUN_OK(0x0E, 0x1E, 0x06, 0x16);
    CHECK_EQ(rd16(SSB + 0x7FFE), S_CS); CHECK_EQ(rd16(SSB + 0x7FFC), S_DS);
    CHECK_EQ(rd16(SSB + 0x7FFA), S_ES); CHECK_EQ(rd16(SSB + 0x7FF8), S_SS);
    wr16(SSB + 0x7FF8, S_DS2); wr16(SSB + 0x7FFA, S_ES);
    RUN_OK(0x07, 0x1F); CHECK_EQ(SEG(CPU_ES), S_DS2); CHECK_EQ(SEG(CPU_DS), S_ES);
    reset(); wr16(SSB + 0x8000, S_DS2); ESP = 0x8000;
    RUN_OK(0x17); CHECK_EQ(SEG(CPU_SS), S_DS2);
    reset(); wr16(SSB + 0x8000, 0x99);
    CHECK_EQ(RUN(0x1F), CPU_E_GP); CHECK_EQ(ESP, 0x8000); CHECK_EQ(SEG(CPU_DS), S_DS);
    reset(); RUN_OK(0x0F, 0xA0, 0x0F, 0xA8);
    CHECK_EQ(rd16(SSB + 0x7FFE), S_FS); CHECK_EQ(rd16(SSB + 0x7FFC), S_GS);
    wr16(SSB + 0x7FFC, S_ES); wr16(SSB + 0x7FFE, S_DS2);
    RUN_OK(0x0F, 0xA1, 0x0F, 0xA9); CHECK_EQ(SEG(CPU_FS), S_ES); CHECK_EQ(SEG(CPU_GS), S_DS2);
    reset(); RUN_OK(0x66, 0x0E); CHECK_EQ(ESP, 0x7FFC); CHECK_EQ(rd16(SSB + 0x7FFC), S_CS);
    reset(); EBX = 0x10; wr16(DSB + 0x10, 0x4242); /* push/pop r/m */
    RUN_OK(0xFF, 0x37); CHECK_EQ(rd16(SSB + 0x7FFE), 0x4242);
    EBX = 0x20; RUN_OK(0x8F, 0x07); CHECK_EQ(rd16(DSB + 0x20), 0x4242); CHECK_EQ(ESP, 0x8000);
    reset(); CHECK_EQ(RUN(0x8F, 0xC8), CPU_E_UD);                    /* pop /1 */
}

static void test_pusha_popf(void) {
    printf("test_pusha_popf\n");
    reset();
    EAX = 0xA; ECX = 0xC; EDX = 0xD; EBX = 0xB; EBP = 0x55; ESI = 0x51; EDI = 0xD1;
    RUN_OK(0x60); CHECK_EQ(ESP, 0x7FF0);
    CHECK_EQ(rd16(SSB + 0x7FFE), 0xA); CHECK_EQ(rd16(SSB + 0x7FFC), 0xC);
    CHECK_EQ(rd16(SSB + 0x7FFA), 0xD); CHECK_EQ(rd16(SSB + 0x7FF8), 0xB);
    CHECK_EQ(rd16(SSB + 0x7FF6), 0x8000); CHECK_EQ(rd16(SSB + 0x7FF4), 0x55);
    CHECK_EQ(rd16(SSB + 0x7FF2), 0x51); CHECK_EQ(rd16(SSB + 0x7FF0), 0xD1);
    EAX = EBX = ECX = EDX = EBP = ESI = EDI = 0;
    RUN_OK(0x61); CHECK_EQ(ESP, 0x8000);
    CHECK_EQ(EAX, 0xA); CHECK_EQ(ECX, 0xC); CHECK_EQ(EDX, 0xD); CHECK_EQ(EBX, 0xB);
    CHECK_EQ(EBP, 0x55); CHECK_EQ(ESI, 0x51); CHECK_EQ(EDI, 0xD1);
    reset(); /* POPA skips the stored SP */
    wr16(SSB + 0x7FF6, 0x1234); ESP = 0x7FF0;
    RUN_OK(0x61); CHECK_EQ(ESP, 0x8000);
    reset(); EAX = 0x11111111; EDI = 0x22222222;
    RUN_OK(0x66, 0x60); CHECK_EQ(ESP, 0x7FE0);
    CHECK_EQ(rd32(SSB + 0x7FFC), 0x11111111); CHECK_EQ(rd32(SSB + 0x7FE0), 0x22222222);
    EAX = EDI = 0; RUN_OK(0x66, 0x61); CHECK_EQ(EAX, 0x11111111); CHECK_EQ(EDI, 0x22222222);
    reset(); FL = 0x2 | CPU_CF | CPU_ZF | CPU_SF;
    RUN_OK(0x9C); CHECK_EQ(rd16(SSB + 0x7FFE), 0x2 | CPU_CF | CPU_ZF | CPU_SF);
    reset(); wr16(SSB + 0x7FFE, 0xFFFF); ESP = 0x7FFE;
    RUN_OK(0x9D); CHECK_EQ(FL, 0x0FD5 | 2); CHECK_EQ(ESP, 0x8000);   /* only defined bits */
    reset(); FL = 0x2 | CPU_DF | CPU_IF; wr16(SSB + 0x7FFE, 0); ESP = 0x7FFE;
    RUN_OK(0x9D); CHECK_EQ(FL, 0x2);
    reset(); FL = 0x2 | CPU_OF; RUN_OK(0x66, 0x9C); CHECK_EQ(ESP, 0x7FFC);
    CHECK_EQ(rd32(SSB + 0x7FFC), 0x2 | CPU_OF);
    wr32(SSB + 0x7FFC, 0xFFFFFFFF); RUN_OK(0x66, 0x9D); CHECK_EQ(FL, 0x0FD5 | 2);
}

static cpu_status hook_set(cpu_t *c, void *ctx) { c->r[CPU_EAX] = 7; c->eip = *(uint32_t *)ctx; return CPU_OK; }
static cpu_status hook_halt(cpu_t *c, void *ctx) { (void)c; (void)ctx; return CPU_HALT; }
static cpu_status hook_nop(cpu_t *c, void *ctx) { c->r[CPU_EDX]++; (void)ctx; return CPU_OK; }
static cpu_status hook_fault(cpu_t *c, void *ctx) { (void)c; (void)ctx; return CPU_E_GP; }

static void test_hooks(void) {
    uint32_t dest = CODE_OFF + 0x20;
    unsigned i;
    printf("test_hooks\n");
    reset();
    CHECK(cpu_add_hook(&g_cpu, S_CS, CODE_OFF, hook_set, &dest));
    g_mem[CSB + dest] = 0xF4;
    CHECK_EQ(cpu_run(&g_cpu, 100), CPU_HALT);
    CHECK_EQ(EAX, 7); CHECK_EQ(EIP, dest + 1);
    reset(); /* hook that does not move: instruction executes */
    CHECK(cpu_add_hook(&g_cpu, S_CS, CODE_OFF, hook_nop, NULL));
    RUN_OK(0xB8, 0x34, 0x12); CHECK_EQ(EAX, 0x1234); CHECK_EQ(EDX, 1);
    reset(); /* hook may stop the run, or fault */
    cpu_add_hook(&g_cpu, S_CS, CODE_OFF + 3, hook_halt, NULL);
    CHECK_EQ(RUN(0x90, 0x90, 0x90, 0x90), CPU_HALT); CHECK_EQ(EIP, CODE_OFF + 3);
    reset(); cpu_add_hook(&g_cpu, S_CS, CODE_OFF, hook_fault, NULL);
    CHECK_EQ(RUN(0x90), CPU_E_GP);
    reset(); /* other selector/offset do not match */
    cpu_add_hook(&g_cpu, S_DS, CODE_OFF, hook_fault, NULL);
    cpu_add_hook(&g_cpu, S_CS, CODE_OFF + 2, hook_fault, NULL);
    RUN_OK(0x90);
    reset(); /* many hooks, fixed capacity */
    for (i = 0; i < CPU_HOOK_SLOTS / 2; i++)
        if (!cpu_add_hook(&g_cpu, 0x1000, i, hook_nop, NULL)) break;
    CHECK_EQ(i, CPU_HOOK_SLOTS / 2);
    CHECK(!cpu_add_hook(&g_cpu, 0x1000, 0xFFFF, hook_nop, NULL));
    RUN_OK(0x90);
}

static cpu_status step_count(cpu_t *c, void *ctx) { (*(unsigned *)ctx)++; (void)c; return CPU_OK; }

/* the step hook sees every instruction before it runs (cwfa --trace-ip builds on it) */
static void test_step_hook(void) {
    unsigned n = 0;
    printf("test_step_hook\n");
    reset();
    cpu_set_step_hook(&g_cpu, step_count, &n);
    RUN_OK(0x90, 0x90, 0xB8, 0x34, 0x12);                          /* nop, nop, mov ax,1234h (then HLT) */
    CHECK_EQ(n, 4);                                                /* the three instructions and the HLT */
    CHECK_EQ(EAX, 0x1234);
    reset();
    n = 0;
    RUN_OK(0x90);                                                  /* reset() cleared the hook */
    CHECK_EQ(n, 0);
}

static void test_halt_and_faults(void) {
    printf("test_halt_and_faults\n");
    reset();
    CHECK_EQ(RUN(0x90), CPU_HALT); CHECK_EQ(EIP, CODE_OFF + 2);
    reset(); CHECK_EQ(RUN(0x0F, 0xFF), CPU_E_UD); CHECK_EQ(EIP, CODE_OFF);
    reset(); CHECK_EQ(RUN(0xD9, 0xD1), CPU_E_UD); CHECK_EQ(EIP, CODE_OFF); /* undefined x87 encoding */
    reset(); RUN_OK(0xDF, 0xE0); CHECK_EQ(EAX & 0xFFFF, 0);            /* fnstsw ax */
    reset(); EAX = S_SMALL;
    CHECK_EQ(RUN(0x8E, 0xD8, 0x8A, 0x06, 0x20, 0x00), CPU_E_LIMIT);  /* mov al,[0x20] */
    CHECK_EQ(EIP, CODE_OFF + 2);
    reset(); EAX = S_SMALL;
    CHECK_EQ(RUN(0x8E, 0xD8, 0x8B, 0x06, 0x0F, 0x00), CPU_E_LIMIT);  /* word straddles limit */
    reset(); EAX = S_SMALL; wr8(SMB + 0x0F, 0x5A);
    RUN_OK(0x8E, 0xD8, 0x8A, 0x06, 0x0F, 0x00); CHECK_EQ(EAX & 0xFF, 0x5A);
    reset(); EAX = S_SMALL; EBX = 0x1234;
    CHECK_EQ(RUN(0x8E, 0xD8, 0x89, 0x1E, 0x0F, 0x00), CPU_E_LIMIT);  /* store faults, no write */
    CHECK_EQ(rd8(SMB + 0x0F), 0);
    reset(); SEG(CPU_SS) = S_SMALL; ESP = 0x8000;
    CHECK_EQ(RUN(0x50), CPU_E_LIMIT); CHECK_EQ(ESP, 0x8000);         /* ESP restored */
    reset(); SEG(CPU_CS) = 0x99; CHECK_EQ(cpu_step(&g_cpu), CPU_E_SEL);
    reset(); /* flat 16-bit code wraps? fetch beyond limit faults */
    EIP = 0xFFFF; g_mem[CSB + 0xFFFF] = 0xB8;
    CHECK_EQ(cpu_step(&g_cpu), CPU_E_LIMIT); CHECK_EQ(EIP, 0xFFFF);
    reset(); CHECK_EQ(RUN(0x6C), CPU_E_UD);                          /* INS: unsupported */
    reset(); CHECK_EQ(RUN(0xE4, 0x60), CPU_E_UD);                    /* IN */
    CHECK(strcmp(cpu_strstatus(CPU_E_DE), "divide error") == 0);
    CHECK(strcmp(cpu_strstatus(CPU_E_FPU), "overlay manager call (INT 3Fh)") == 0);
}

static void test_wide_callbacks(void) {
    cpu_mem m = t_iface(true);
    printf("test_wide_callbacks\n");
    reset(); cpu_init(&g_cpu, &m);
    g_cpu.seg[CPU_CS] = S_CS; g_cpu.seg[CPU_DS] = S_DS; g_cpu.seg[CPU_SS] = S_SS;
    g_cpu.eip = CODE_OFF; ESP = 0x8000;
    g_n16 = g_n32 = 0; EBX = 0x10; wr32(DSB + 0x10, 0x01020304);
    RUN_OK(0x66, 0x8B, 0x07); CHECK_EQ(EAX, 0x01020304);
    CHECK(g_n32 >= 1);
    RUN_OK(0x8B, 0x07); CHECK_EQ(EAX & 0xFFFF, 0x0304); CHECK(g_n16 >= 1);
    g_n16 = 0; EAX = 0xBEEF; RUN_OK(0x89, 0x47, 0x04); /* write16 path */
    CHECK_EQ(rd16(DSB + 0x14), 0xBEEF); CHECK(g_n16 >= 1);
    /* write32 not provided: derived from write16/write8 */
    EAX = 0x0A0B0C0D; RUN_OK(0x66, 0x89, 0x47, 0x08); CHECK_EQ(rd32(DSB + 0x18), 0x0A0B0C0D);
}

/* ======================================================================
 * Group 2: integer ALU, shifts, multiply/divide, 0F bit ops
 * ====================================================================== */

static uint32_t szmask(int bits) { return bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1; }
static int64_t sx64(uint32_t v, int bits) {
    return bits == 8 ? (int8_t)v : bits == 16 ? (int16_t)v : (int32_t)v;
}
static int ref_parity(uint32_t v) {
    int n = 0, i;
    for (i = 0; i < 8; i++) n += (v >> i) & 1;
    return (n & 1) == 0;
}
static uint32_t ref_szp(uint32_t res, int bits) {
    uint32_t f = 0;
    if (res & (1u << (bits - 1))) f |= CPU_SF;
    if (res == 0) f |= CPU_ZF;
    if (ref_parity(res)) f |= CPU_PF;
    return f;
}

/* Independent reference for the eight classic ALU ops (op = 0..7). */
static void ref_alu(int op, uint32_t a, uint32_t b, int cin, int bits, uint32_t *res, uint32_t *fl) {
    uint32_t m = szmask(bits), ua = a & m, ub = b & m, f = 0;
    int64_t full, sfull, lo, hi = ((int64_t)1 << (bits - 1)) - 1;
    lo = -hi - 1;
    if (op == 0 || op == 2) { /* ADD, ADC */
        int c = op == 2 ? cin : 0;
        full = (int64_t)ua + ub + c;
        sfull = sx64(ua, bits) + sx64(ub, bits) + c;
        *res = (uint32_t)full & m;
        if (full > (int64_t)m) f |= CPU_CF;
        if ((ua & 15) + (ub & 15) + (uint32_t)c > 15) f |= CPU_AF;
        if (sfull > hi || sfull < lo) f |= CPU_OF;
    } else if (op == 3 || op == 5 || op == 7) { /* SBB, SUB, CMP */
        int c = op == 3 ? cin : 0;
        full = (int64_t)ua - ub - c;
        sfull = sx64(ua, bits) - sx64(ub, bits) - c;
        *res = (uint32_t)full & m;
        if (full < 0) f |= CPU_CF;
        if ((int)(ua & 15) - (int)(ub & 15) - c < 0) f |= CPU_AF;
        if (sfull > hi || sfull < lo) f |= CPU_OF;
    } else {
        *res = (op == 1 ? (ua | ub) : op == 4 ? (ua & ub) : (ua ^ ub)) & m;
    }
    if (op == 7) *res = ua;
    *fl = f | ref_szp((op == 7) ? (uint32_t)((int64_t)ua - ub) & m : *res, bits);
}

static const uint32_t GRID[] = {0, 1, 2, 0x0F, 0x10, 0x1F, 0x7F, 0x80, 0x81, 0xFF, 0x100,
                                0x7FFF, 0x8000, 0x8001, 0xFFFF, 0x12345678, 0x7FFFFFFF,
                                0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF};
#define NGRID (sizeof GRID / sizeof GRID[0])

static void test_alu_known(void) {
    printf("test_alu_known\n");
    reset(); EAX = 0x7F; RUN_OK(0x04, 0x01);
    CHECK_EQ(EAX, 0x80); CHECK_FLAGS(CPU_OF | CPU_SF | CPU_AF);
    reset(); EAX = 0xFF; RUN_OK(0x04, 0x01);
    CHECK_EQ(EAX, 0); CHECK_FLAGS(CPU_CF | CPU_ZF | CPU_PF | CPU_AF);
    reset(); EAX = 0; RUN_OK(0x2C, 0x01);
    CHECK_EQ(EAX, 0xFF); CHECK_FLAGS(CPU_CF | CPU_SF | CPU_PF | CPU_AF);
    reset(); EAX = 0x80; RUN_OK(0x2C, 0x01);
    CHECK_EQ(EAX, 0x7F); CHECK_FLAGS(CPU_OF | CPU_AF);
    reset(); EAX = 0x42; RUN_OK(0x38, 0xC0);              /* cmp al,al */
    CHECK_EQ(EAX, 0x42); CHECK_FLAGS(CPU_ZF | CPU_PF);
    reset(); EAX = 0xF0; FL |= CPU_CF | CPU_OF; RUN_OK(0x24, 0x0F);
    CHECK_EQ(EAX, 0); CHECK_FLAGS(CPU_ZF | CPU_PF);
    reset(); EAX = 0; RUN_OK(0x0D, 0x00, 0x80);
    CHECK_EQ(EAX, 0x8000); CHECK_FLAGS(CPU_SF | CPU_PF);
    reset(); EAX = 0xFF; FL |= CPU_CF; RUN_OK(0x14, 0x00);  /* adc al,0 */
    CHECK_EQ(EAX, 0); CHECK_FLAGS(CPU_CF | CPU_ZF | CPU_PF | CPU_AF);
    reset(); EAX = 0; FL |= CPU_CF; RUN_OK(0x1C, 0x00);     /* sbb al,0 */
    CHECK_EQ(EAX, 0xFF); CHECK_FLAGS(CPU_CF | CPU_SF | CPU_PF | CPU_AF);
    reset(); EAX = 0x7FFFFFFF; RUN_OK(0x66, 0x05, 1, 0, 0, 0);
    CHECK_EQ(EAX, 0x80000000); CHECK_FLAGS(CPU_OF | CPU_SF | CPU_AF | CPU_PF);
    reset(); EAX = 0; RUN_OK(0x83, 0xC0, 0xFF);             /* add ax,-1 */
    CHECK_EQ(EAX, 0xFFFF); CHECK_FLAGS(CPU_SF | CPU_PF);
    reset(); EAX = 5; RUN_OK(0x83, 0xF8, 0x05);             /* cmp ax,5 */
    CHECK_FLAGS(CPU_ZF | CPU_PF);
    reset(); EAX = 0x10; RUN_OK(0x81, 0xE8, 0x01, 0x00);    /* sub ax,1 */
    CHECK_EQ(EAX, 0x0F); CHECK_FLAGS(CPU_AF | CPU_PF);
    reset(); EAX = 0x10; RUN_OK(0x80, 0xC0, 0x05);          /* add al,5 */
    CHECK_EQ(EAX, 0x15);
    reset(); EBX = 0x10; EAX = 3; wr16(DSB + 0x10, 4);
    RUN_OK(0x01, 0x07);                                      /* add [bx],ax */
    CHECK_EQ(rd16(DSB + 0x10), 7);
    RUN_OK(0x03, 0x07);                                      /* add ax,[bx] */
    CHECK_EQ(EAX, 10);
    RUN_OK(0x80, 0x07, 0x01); CHECK_EQ(rd8(DSB + 0x10), 8); /* add byte [bx],1 */
    RUN_OK(0x66, 0x81, 0x07, 0, 0, 1, 0); CHECK_EQ(rd32(DSB + 0x10), 0x10008);
    RUN_OK(0x39, 0x07); CHECK_EQ(rd16(DSB + 0x10), 8); /* cmp: no store */
    reset(); /* 32-bit add through ADD/ADC */
    EAX = 0xFFFF; EDX = 0; ECX = 1; EBX = 0;
    RUN_OK(0x01, 0xC8, 0x11, 0xDA);                          /* add ax,cx ; adc dx,bx */
    CHECK_EQ(EAX & 0xFFFF, 0); CHECK_EQ(EDX & 0xFFFF, 1);
    reset(); EAX = 0x12345678; EBX = 0x11111111; /* upper bits kept for 16-bit op */
    RUN_OK(0x01, 0xD8); CHECK_EQ(EAX, 0x12346789);
    reset(); EAX = 0xAAAAAA7F; RUN_OK(0x04, 0x01); CHECK_EQ(EAX, 0xAAAAAA80);
}

static void test_alu_grid(void) {
    int bits, op, cin;
    size_t i, j;
    unsigned bad = 0, runs = 0;
    printf("test_alu_grid\n");
    reset();
    for (bits = 8; bits <= 32; bits *= 2)
        for (op = 0; op < 8; op++)
            for (cin = 0; cin < 2; cin++)
                for (i = 0; i < NGRID; i++)
                    for (j = 0; j < NGRID; j++) {
                        uint32_t a = GRID[i] & szmask(bits), b = GRID[j] & szmask(bits);
                        uint32_t res, fl, hiA = 0x5A5A5A5Au & ~szmask(bits), got;
                        uint8_t code[3]; size_t n = 0;
                        if (bits == 32) code[n++] = 0x66;
                        code[n++] = (uint8_t)(op << 3 | (bits == 8 ? 0 : 1));
                        code[n++] = 0xD8; /* op al/ax/eax, bl/bx/ebx */
                        EAX = a | hiA; EBX = b; FL = 2 | (cin ? CPU_CF : 0) | CPU_DF;
                        run_code(code, n);
                        ref_alu(op, a, b, cin, bits, &res, &fl);
                        got = EAX & szmask(bits);
                        runs++;
                        if (got != res || (FL & FLAGS_MASK) != fl || !(FL & CPU_DF) ||
                            (EAX & ~szmask(bits)) != hiA) {
                            if (bad++ < 5)
                                printf("  grid mismatch bits=%d op=%d cin=%d a=%x b=%x got=%x/%x want=%x/%x\n",
                                       bits, op, cin, a, b, got, FL & FLAGS_MASK, res, fl);
                        }
                    }
    CHECK_EQ(bad, 0);
    CHECK(runs > 10000);
}

static void test_alu_forms(void) {
    printf("test_alu_forms\n");
    /* every op through opcodes 0x80/0x81/0x83 with a register destination */
    {
        int op;
        for (op = 0; op < 8; op++) {
            uint32_t res, fl;
            reset(); EAX = 0x8001; FL |= CPU_CF;
            RUN_OK(0x83, (uint8_t)(0xC0 | op << 3), 0xFE); /* op ax,-2 */
            ref_alu(op, 0x8001, 0xFFFE, 1, 16, &res, &fl);
            CHECK_EQ(EAX & 0xFFFF, res); CHECK_FLAGS(fl);
            reset(); EAX = 0x81; FL |= CPU_CF;
            RUN_OK(0x80, (uint8_t)(0xC0 | op << 3), 0x7F);
            ref_alu(op, 0x81, 0x7F, 1, 8, &res, &fl);
            CHECK_EQ(EAX & 0xFF, res); CHECK_FLAGS(fl);
            reset(); EAX = 0xF0F0F0F0; FL |= CPU_CF;
            RUN_OK(0x66, 0x81, (uint8_t)(0xC0 | op << 3), 0x0F, 0x0F, 0xFF, 0x7F);
            ref_alu(op, 0xF0F0F0F0, 0x7FFF0F0F, 1, 32, &res, &fl);
            CHECK_EQ(EAX, res); CHECK_FLAGS(fl);
            reset(); EAX = 0x10; EBX = 0x20; FL |= CPU_CF;       /* 0x02/0x03 register source forms */
            RUN_OK((uint8_t)(op << 3 | 2), 0xC3);                /* op al,bl */
            ref_alu(op, 0x10, 0x20, 1, 8, &res, &fl);
            CHECK_EQ(EAX & 0xFF, res); CHECK_FLAGS(fl);
            reset(); EAX = 0x1000; EBX = 0x2000; FL |= CPU_CF;
            RUN_OK((uint8_t)(op << 3 | 3), 0xC3);                /* op ax,bx */
            ref_alu(op, 0x1000, 0x2000, 1, 16, &res, &fl);
            CHECK_EQ(EAX & 0xFFFF, res); CHECK_FLAGS(fl);
            reset(); EAX = 0x70; FL |= CPU_CF;                   /* AL,imm8 and AX,imm16 */
            RUN_OK((uint8_t)(op << 3 | 4), 0x90);
            ref_alu(op, 0x70, 0x90, 1, 8, &res, &fl);
            CHECK_EQ(EAX & 0xFF, res); CHECK_FLAGS(fl);
            reset(); EAX = 0x7000; FL |= CPU_CF;
            RUN_OK((uint8_t)(op << 3 | 5), 0x00, 0x90);
            ref_alu(op, 0x7000, 0x9000, 1, 16, &res, &fl);
            CHECK_EQ(EAX & 0xFFFF, res); CHECK_FLAGS(fl);
        }
    }
    /* TEST */
    reset(); EAX = 0x80F0; FL |= CPU_CF | CPU_OF; RUN_OK(0xA9, 0x00, 0x80);
    CHECK_EQ(EAX, 0x80F0); CHECK_FLAGS(CPU_SF | CPU_PF);
    reset(); EAX = 0x0F; RUN_OK(0xA8, 0xF0); CHECK_FLAGS(CPU_ZF | CPU_PF);
    reset(); EAX = 0x3; EBX = 0x2; RUN_OK(0x84, 0xD8); CHECK_FLAGS(0);          /* test al,bl */
    reset(); EAX = 0x8000; EBX = 0x8000; RUN_OK(0x85, 0xD8); CHECK_FLAGS(CPU_SF | CPU_PF);
    reset(); EAX = 0x00FF; RUN_OK(0xF6, 0xC0, 0x0F); CHECK_FLAGS(CPU_PF);       /* test al,0f */
    reset(); EAX = 0x00FF; RUN_OK(0xF7, 0xC0, 0x00, 0x01); CHECK_FLAGS(CPU_ZF | CPU_PF);
    reset(); EAX = 1; RUN_OK(0xF6, 0xC8, 0x01); CHECK_FLAGS(0);                 /* /1 alias */
    reset(); EBX = 0x10; wr8(DSB + 0x10, 0xF0); RUN_OK(0xF6, 0x07, 0x80); CHECK_FLAGS(CPU_SF);
}

static void test_incdec_negnot(void) {
    printf("test_incdec_negnot\n");
    reset(); EAX = 0x7FFF; FL |= CPU_CF; RUN_OK(0x40);
    CHECK_EQ(EAX, 0x8000); CHECK_FLAGS(CPU_CF | CPU_OF | CPU_SF | CPU_AF | CPU_PF);
    reset(); EAX = 0x8000; RUN_OK(0x48);
    CHECK_EQ(EAX, 0x7FFF); CHECK_FLAGS(CPU_OF | CPU_AF | CPU_PF);
    reset(); EAX = 0; RUN_OK(0x48);
    CHECK_EQ(EAX, 0xFFFF); CHECK_FLAGS(CPU_SF | CPU_PF | CPU_AF);
    reset(); EAX = 0xFFFF; FL |= CPU_CF; RUN_OK(0x40);
    CHECK_EQ(EAX, 0); CHECK_FLAGS(CPU_CF | CPU_ZF | CPU_PF | CPU_AF);
    reset(); EAX = 0xFFFFFFFF; RUN_OK(0x66, 0x40); CHECK_EQ(EAX, 0); CHECK_FLAGS(CPU_ZF | CPU_PF | CPU_AF);
    reset(); ECX = 0x80000000; RUN_OK(0x66, 0x49); CHECK_EQ(ECX, 0x7FFFFFFF); CHECK_FLAGS(CPU_OF | CPU_AF | CPU_PF);
    reset(); EAX = 0x7F; RUN_OK(0xFE, 0xC0); CHECK_EQ(EAX, 0x80); CHECK_FLAGS(CPU_OF | CPU_SF | CPU_AF);
    reset(); EAX = 0x80; RUN_OK(0xFE, 0xC8); CHECK_EQ(EAX, 0x7F); CHECK_FLAGS(CPU_OF | CPU_AF);
    reset(); EBX = 0x10; wr16(DSB + 0x10, 0xFFFF);
    RUN_OK(0xFF, 0x07); CHECK_EQ(rd16(DSB + 0x10), 0); CHECK_FLAGS(CPU_ZF | CPU_PF | CPU_AF);
    RUN_OK(0xFF, 0x0F); CHECK_EQ(rd16(DSB + 0x10), 0xFFFF);
    reset(); EAX = 0xAAAA0000; RUN_OK(0x48); CHECK_EQ(EAX, 0xAAAAFFFF);        /* upper kept */
    reset(); EAX = 0; RUN_OK(0xF7, 0xD8);                                       /* neg ax */
    CHECK_EQ(EAX, 0); CHECK_FLAGS(CPU_ZF | CPU_PF);
    reset(); EAX = 1; RUN_OK(0xF7, 0xD8);
    CHECK_EQ(EAX, 0xFFFF); CHECK_FLAGS(CPU_CF | CPU_SF | CPU_PF | CPU_AF);
    reset(); EAX = 0x80; RUN_OK(0xF6, 0xD8);
    CHECK_EQ(EAX, 0x80); CHECK_FLAGS(CPU_CF | CPU_OF | CPU_SF);
    reset(); EAX = 0x80000000; RUN_OK(0x66, 0xF7, 0xD8);
    CHECK_EQ(EAX, 0x80000000); CHECK_FLAGS(CPU_CF | CPU_OF | CPU_SF | CPU_PF);
    reset(); EAX = 0x00FF; FL |= CPU_CF | CPU_ZF; RUN_OK(0xF7, 0xD0);          /* not ax */
    CHECK_EQ(EAX, 0xFF00); CHECK_EQ(FL & (CPU_CF | CPU_ZF), CPU_CF | CPU_ZF);
    reset(); EAX = 0x0F; RUN_OK(0xF6, 0xD0); CHECK_EQ(EAX, 0xF0);
    reset(); EBX = 0x10; wr16(DSB + 0x10, 0x1234); RUN_OK(0xF7, 0x1F);          /* neg [bx] */
    CHECK_EQ(rd16(DSB + 0x10), 0xEDCC);
}

/* Bit-at-a-time reference for ROL ROR RCL RCR SHL SHR SAL SAR. */
static void ref_shift(int op, uint32_t v, unsigned cnt, int cin, int bits, uint32_t inflags,
                      uint32_t *res, uint32_t *fl, uint32_t *undef) {
    uint32_t m = szmask(bits), top = 1u << (bits - 1), x = v & m, orig = x;
    int cf = cin;
    unsigned n = cnt & 31, i;
    uint32_t f = inflags & ~(CPU_CF | CPU_OF);
    for (i = 0; i < n; i++) {
        int o;
        switch (op) {
        case 0: cf = !!(x & top); x = ((x << 1) | (uint32_t)cf) & m; break;
        case 1: cf = (int)(x & 1); x = (x >> 1) | (cf ? top : 0); break;
        case 2: o = !!(x & top); x = ((x << 1) | (uint32_t)cf) & m; cf = o; break;
        case 3: o = (int)(x & 1); x = (x >> 1) | (cf ? top : 0); cf = o; break;
        case 4: case 6: cf = !!(x & top); x = (x << 1) & m; break;
        case 5: cf = (int)(x & 1); x >>= 1; break;
        default: cf = (int)(x & 1); x = (x >> 1) | (x & top); break;
        }
    }
    *res = x;
    *undef = CPU_AF;
    if (n == 0) { *fl = inflags; return; }
    if (cf) f |= CPU_CF;
    if (op >= 4) f = (f & ~(CPU_SF | CPU_ZF | CPU_PF)) | ref_szp(x, bits);
    if (n == 1) {
        int msb = !!(x & top), of;
        switch (op) {
        case 0: case 2: case 4: case 6: of = msb ^ cf; break;
        case 1: case 3: of = msb ^ !!(x & (top >> 1)); break;
        case 5: of = !!(orig & top); break;
        default: of = 0; break;
        }
        if (of) f |= CPU_OF;
    } else {
        *undef |= CPU_OF;
    }
    *fl = f;
}

static void test_shift_grid(void) {
    static const unsigned counts[] = {0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 24, 31, 32, 33, 63};
    int bits, op, cin;
    size_t i, k;
    unsigned bad = 0, runs = 0;
    printf("test_shift_grid\n");
    reset();
    for (bits = 8; bits <= 32; bits *= 2)
        for (op = 0; op < 8; op++)
            for (cin = 0; cin < 2; cin++)
                for (i = 0; i < NGRID; i++)
                    for (k = 0; k < sizeof counts / sizeof counts[0]; k++) {
                        uint32_t a = GRID[i] & szmask(bits), res, fl, undef, inflags, got;
                        uint8_t code[3]; size_t n = 0;
                        if (bits == 32) code[n++] = 0x66;
                        code[n++] = bits == 8 ? 0xD2 : 0xD3;
                        code[n++] = (uint8_t)(0xC0 | op << 3); /* op al/ax/eax, cl */
                        inflags = 2 | (cin ? CPU_CF : 0) | CPU_DF | CPU_ZF | CPU_PF;
                        EAX = a; ECX = counts[k]; FL = inflags;
                        run_code(code, n);
                        ref_shift(op, a, counts[k], cin, bits, inflags, &res, &fl, &undef);
                        got = EAX & szmask(bits);
                        runs++;
                        if (got != res || ((FL ^ fl) & FLAGS_MASK & ~undef) || !(FL & CPU_DF)) {
                            if (bad++ < 5)
                                printf("  shift mismatch bits=%d op=%d cin=%d a=%x cnt=%u got=%x/%x want=%x/%x\n",
                                       bits, op, cin, a, counts[k], got, FL, res, fl);
                        }
                    }
    CHECK_EQ(bad, 0);
    CHECK(runs > 10000);
}

static void test_shift_forms(void) {
    printf("test_shift_forms\n");
    reset(); EAX = 0x81; RUN_OK(0xD0, 0xE0);                  /* shl al,1 */
    CHECK_EQ(EAX, 0x02); CHECK_FLAGS(CPU_CF | CPU_OF);
    reset(); EAX = 0x8001; RUN_OK(0xD1, 0xC0);                /* rol ax,1 */
    CHECK_EQ(EAX, 0x0003); CHECK_FLAGS(CPU_CF | CPU_OF | 0);
    reset(); EAX = 0x8000; RUN_OK(0xD1, 0xF8);                /* sar ax,1 */
    CHECK_EQ(EAX, 0xC000); CHECK_FLAGS(CPU_SF | CPU_PF);
    reset(); EAX = 0x8001; RUN_OK(0xD1, 0xE8);                /* shr ax,1 */
    CHECK_EQ(EAX, 0x4000); CHECK_FLAGS(CPU_CF | CPU_OF | CPU_PF);
    reset(); EAX = 0x0001; FL |= CPU_CF; RUN_OK(0xD1, 0xD0);  /* rcl ax,1 */
    CHECK_EQ(EAX, 3); CHECK_FLAGS(0);
    reset(); EAX = 0x0001; RUN_OK(0xD1, 0xD8);                /* rcr ax,1 */
    CHECK_EQ(EAX, 0); CHECK_FLAGS(CPU_CF);
    reset(); EAX = 0x1234; RUN_OK(0xC1, 0xE0, 0x04);          /* shl ax,4 imm */
    CHECK_EQ(EAX, 0x2340); CHECK_EQ(FL & CPU_CF, CPU_CF);
    reset(); EAX = 0xF0; RUN_OK(0xC0, 0xE8, 0x04);            /* shr al,4 imm */
    CHECK_EQ(EAX, 0x0F);
    reset(); EAX = 0xDEADBEEF; RUN_OK(0x66, 0xC1, 0xC8, 0x08);/* ror eax,8 */
    CHECK_EQ(EAX, 0xEFDEADBE); CHECK_EQ(FL & CPU_CF, CPU_CF);
    reset(); EAX = 0x1234; ECX = 0; FL |= CPU_CF | CPU_ZF | CPU_SF; /* count 0: no flag change */
    RUN_OK(0xD3, 0xE0); CHECK_EQ(EAX, 0x1234); CHECK_EQ(FL & FLAGS_MASK, CPU_CF | CPU_ZF | CPU_SF);
    reset(); EAX = 0x1234; ECX = 32; FL |= CPU_CF;            /* count 32 masks to 0 */
    RUN_OK(0x66, 0xD3, 0xE0); CHECK_EQ(EAX, 0x1234); CHECK_FLAGS(CPU_CF);
    reset(); EAX = 0x1234; ECX = 16;                           /* shl ax,16 clears, CF=bit0 */
    RUN_OK(0xD3, 0xE0); CHECK_EQ(EAX, 0); CHECK_FLAGS(CPU_ZF | CPU_PF);
    reset(); EAX = 0x1235; ECX = 16; RUN_OK(0xD3, 0xE0); CHECK_EQ(FL & CPU_CF, CPU_CF);
    reset(); EAX = 0x80; ECX = 40;                             /* sar al by 40 -> 8 */
    RUN_OK(0xD2, 0xF8); CHECK_EQ(EAX, 0xFF);
    reset(); EBX = 0x10; wr16(DSB + 0x10, 0x0101); RUN_OK(0xD1, 0x27); CHECK_EQ(rd16(DSB + 0x10), 0x0202);
    reset(); EAX = 0x1234; ECX = 0x0004; EDX = 0xFFFF;         /* sal alias /6 */
    RUN_OK(0xD3, 0xF0); CHECK_EQ(EAX, 0x2340);
    reset(); EAX = 0xAAAA1234; RUN_OK(0xD1, 0xE0); CHECK_EQ(EAX, 0xAAAA2468); /* upper kept */
}

static void ref_dshift(bool left, uint32_t dst, uint32_t src, unsigned cnt, int bits,
                       uint32_t *res, int *cf) {
    uint32_t m = szmask(bits), top = 1u << (bits - 1);
    unsigned i;
    dst &= m; src &= m;
    for (i = 0; i < cnt; i++) {
        if (left) {
            *cf = !!(dst & top);
            dst = ((dst << 1) | (src & top ? 1u : 0u)) & m;
            src = (src << 1) & m;
        } else {
            *cf = (int)(dst & 1);
            dst = (dst >> 1) | (src & 1 ? top : 0);
            src >>= 1;
        }
    }
    *res = dst;
}

static void test_shld_shrd(void) {
    int bits, left;
    size_t i, j;
    unsigned cnt, bad = 0;
    printf("test_shld_shrd\n");
    reset(); EAX = 0x1234; EBX = 0xABCD;
    RUN_OK(0x0F, 0xA4, 0xD8, 0x04);                           /* shld ax,bx,4 */
    CHECK_EQ(EAX, 0x234A); CHECK_EQ(FL & CPU_CF, CPU_CF);
    reset(); EAX = 0x1234; EBX = 0xABCD;
    RUN_OK(0x0F, 0xAC, 0xD8, 0x04);                           /* shrd ax,bx,4 */
    CHECK_EQ(EAX, 0xD123); CHECK_EQ(FL & CPU_CF, 0);
    reset(); EAX = 0x12345678; EDX = 0x9ABCDEF0; ECX = 8;
    RUN_OK(0x66, 0x0F, 0xA5, 0xD0);                           /* shld eax,edx,cl */
    CHECK_EQ(EAX, 0x3456789A);
    reset(); EAX = 0x12345678; EDX = 0x9ABCDEF0; ECX = 8;
    RUN_OK(0x66, 0x0F, 0xAD, 0xD0);                           /* shrd eax,edx,cl */
    CHECK_EQ(EAX, 0xF0123456);
    reset(); EAX = 0x1234; EBX = 0xFFFF; ECX = 0; FL |= CPU_CF | CPU_ZF;
    RUN_OK(0x0F, 0xA5, 0xD8); CHECK_EQ(EAX, 0x1234); CHECK_FLAGS(CPU_CF | CPU_ZF);
    reset(); EBX = 0x10; wr16(DSB + 0x10, 0x00FF); ECX = 0xFF00;
    RUN_OK(0x0F, 0xA4, 0x0F, 0x08);                           /* shld [bx],cx,8 */
    CHECK_EQ(rd16(DSB + 0x10), 0xFFFF);
    for (bits = 16; bits <= 32; bits *= 2)
        for (left = 0; left < 2; left++)
            for (i = 0; i < NGRID; i++)
                for (j = 0; j < NGRID; j += 3)
                    for (cnt = 1; cnt <= (unsigned)bits; cnt += (bits == 16 ? 1 : 3)) {
                        uint32_t res, got; int cf = 0;
                        uint8_t code[5]; size_t n = 0;
                        if (bits == 32) code[n++] = 0x66;
                        code[n++] = 0x0F; code[n++] = left ? 0xA4 : 0xAC;
                        code[n++] = 0xD8; code[n++] = (uint8_t)cnt; /* op ax,bx,imm */
                        EAX = GRID[i] & szmask(bits); EBX = GRID[j] & szmask(bits); FL = 2;
                        run_code(code, n);
                        ref_dshift(left, GRID[i], GRID[j], cnt, bits, &res, &cf);
                        got = EAX & szmask(bits);
                        if (got != res || (FL & CPU_CF) != (cf ? CPU_CF : 0)) {
                            if (bad++ < 5)
                                printf("  dshift mismatch bits=%d left=%d a=%x b=%x cnt=%u got=%x want=%x\n",
                                       bits, left, GRID[i], GRID[j], cnt, got, res);
                        }
                    }
    CHECK_EQ(bad, 0);
}

static void test_muldiv(void) {
    printf("test_muldiv\n");
    reset(); EAX = 0x0F; EBX = 0x0F; RUN_OK(0xF6, 0xE3);       /* mul bl */
    CHECK_EQ(EAX & 0xFFFF, 0xE1); CHECK_EQ(FL & (CPU_CF | CPU_OF), 0);
    reset(); EAX = 0x80; EBX = 2; RUN_OK(0xF6, 0xE3);
    CHECK_EQ(EAX & 0xFFFF, 0x100); CHECK_EQ(FL & (CPU_CF | CPU_OF), CPU_CF | CPU_OF);
    reset(); EAX = 0x8000; EBX = 2; RUN_OK(0xF7, 0xE3);       /* mul bx */
    CHECK_EQ(EAX & 0xFFFF, 0); CHECK_EQ(EDX & 0xFFFF, 1); CHECK_EQ(FL & (CPU_CF | CPU_OF), CPU_CF | CPU_OF);
    reset(); EAX = 0x80000000; EBX = 2; RUN_OK(0x66, 0xF7, 0xE3);
    CHECK_EQ(EAX, 0); CHECK_EQ(EDX, 1); CHECK_EQ(FL & CPU_CF, CPU_CF);
    reset(); EAX = 0xFFFFFFFF; EBX = 0xFFFFFFFF; RUN_OK(0x66, 0xF7, 0xE3);
    CHECK_EQ(EAX, 1); CHECK_EQ(EDX, 0xFFFFFFFE);
    reset(); EAX = 0x1234; EBX = 0x10; wr16(DSB + 0x10, 3); RUN_OK(0xF7, 0x27); /* mul word [bx] */
    CHECK_EQ(EAX & 0xFFFF, 0x369C);
    reset(); EAX = 0x80; EBX = 0xFF; RUN_OK(0xF6, 0xEB);      /* imul bl: -128*-1 */
    CHECK_EQ(EAX & 0xFFFF, 0x0080); CHECK_EQ(FL & (CPU_CF | CPU_OF), CPU_CF | CPU_OF);
    reset(); EAX = 0xFE; EBX = 3; RUN_OK(0xF6, 0xEB);         /* -2*3 */
    CHECK_EQ(EAX & 0xFFFF, 0xFFFA); CHECK_EQ(FL & (CPU_CF | CPU_OF), 0);
    reset(); EAX = 0x4000; EBX = 4; RUN_OK(0xF7, 0xEB);
    CHECK_EQ(EAX & 0xFFFF, 0); CHECK_EQ(EDX & 0xFFFF, 1); CHECK_EQ(FL & (CPU_CF | CPU_OF), CPU_CF | CPU_OF);
    reset(); EAX = 0xFFFF; EBX = 0xFFFF; RUN_OK(0xF7, 0xEB);  /* -1*-1 = 1 */
    CHECK_EQ(EAX & 0xFFFF, 1); CHECK_EQ(EDX & 0xFFFF, 0); CHECK_EQ(FL & (CPU_CF | CPU_OF), 0);
    reset(); EAX = 0x7FFFFFFF; EBX = 0x7FFFFFFF; RUN_OK(0x66, 0xF7, 0xEB);
    CHECK_EQ(EAX, 1); CHECK_EQ(EDX, 0x3FFFFFFF); CHECK_EQ(FL & CPU_CF, CPU_CF);
    /* two- and three-operand IMUL */
    reset(); EAX = 0x0100; EBX = 0x0100; RUN_OK(0x0F, 0xAF, 0xC3);
    CHECK_EQ(EAX & 0xFFFF, 0); CHECK_EQ(FL & (CPU_CF | CPU_OF), CPU_CF | CPU_OF);
    reset(); EAX = 0xFFFE; EBX = 3; RUN_OK(0x0F, 0xAF, 0xC3);  /* -2*3 = -6 */
    CHECK_EQ(EAX & 0xFFFF, 0xFFFA); CHECK_EQ(FL & (CPU_CF | CPU_OF), 0);
    reset(); EAX = 0x10000; EBX = 0x10000; RUN_OK(0x66, 0x0F, 0xAF, 0xC3);
    CHECK_EQ(EAX, 0); CHECK_EQ(FL & CPU_CF, CPU_CF);
    reset(); EBX = 7; RUN_OK(0x69, 0xC3, 0x0A, 0x00);          /* imul ax,bx,10 */
    CHECK_EQ(EAX & 0xFFFF, 70); CHECK_EQ(FL & (CPU_CF | CPU_OF), 0);
    reset(); EBX = 7; RUN_OK(0x6B, 0xC3, 0xFD);                /* imul ax,bx,-3 */
    CHECK_EQ(EAX & 0xFFFF, 0xFFEB);
    reset(); EBX = 0x1000; RUN_OK(0x69, 0xC3, 0x00, 0x10);     /* overflow */
    CHECK_EQ(EAX & 0xFFFF, 0); CHECK_EQ(FL & (CPU_CF | CPU_OF), CPU_CF | CPU_OF);
    reset(); EBX = 0x10; wr32(DSB + 0x10, 100000); RUN_OK(0x66, 0x6B, 0x0F, 0xFF); /* imul ecx,[bx],-1 */
    CHECK_EQ(ECX, (uint32_t)-100000);
    reset(); EBX = 0x10; wr32(DSB + 0x10, 3); RUN_OK(0x66, 0x69, 0x0F, 0x00, 0x00, 0x01, 0x00);
    CHECK_EQ(ECX, 0x30000);
    /* DIV */
    reset(); EAX = 0x0123; EBX = 0x10; RUN_OK(0xF6, 0xF3);
    CHECK_EQ(EAX & 0xFFFF, 0x0312);                            /* AH=3 AL=0x12 */
    reset(); EAX = 0x1000; EBX = 0x10; CHECK_EQ(RUN(0xF6, 0xF3), CPU_E_DE); CHECK_EQ(EIP, CODE_OFF);
    CHECK_EQ(EAX & 0xFFFF, 0x1000);
    reset(); EAX = 5; EBX = 0; CHECK_EQ(RUN(0xF6, 0xF3), CPU_E_DE);
    reset(); EAX = 0x2345; EDX = 0x0001; EBX = 0x0002; RUN_OK(0xF7, 0xF3); /* 0x12345/2 */
    CHECK_EQ(EAX & 0xFFFF, 0x91A2); CHECK_EQ(EDX & 0xFFFF, 1);
    reset(); EAX = 0; EDX = 2; EBX = 2; CHECK_EQ(RUN(0xF7, 0xF3), CPU_E_DE);       /* quotient 0x10000 */
    reset(); EAX = 0; EDX = 0; EBX = 0; CHECK_EQ(RUN(0xF7, 0xF3), CPU_E_DE);
    reset(); EAX = 7; EDX = 1; EBX = 3; RUN_OK(0x66, 0xF7, 0xF3);     /* 0x100000007/3 */
    CHECK_EQ(EAX, 0x55555557); CHECK_EQ(EDX, 2);
    reset(); EAX = 0; EDX = 3; EBX = 3; CHECK_EQ(RUN(0x66, 0xF7, 0xF3), CPU_E_DE);
    /* IDIV */
    reset(); EAX = 0xFFF9; EBX = 2; RUN_OK(0xF6, 0xFB);       /* -7/2 = -3 r -1 (8-bit: AX=0xFFF9) */
    CHECK_EQ(EAX & 0xFFFF, 0xFFFD);
    reset(); EAX = 0xFF80; EBX = 0xFF; CHECK_EQ(RUN(0xF6, 0xFB), CPU_E_DE);       /* -128/-1 */
    reset(); EAX = 0x8000; EBX = 0xFF; CHECK_EQ(RUN(0xF6, 0xFB), CPU_E_DE);
    reset(); EAX = 0xFF80; EBX = 1; RUN_OK(0xF6, 0xFB); CHECK_EQ(EAX & 0xFF, 0x80);
    reset(); EAX = 0xFFF9; EDX = 0xFFFF; EBX = 2; RUN_OK(0xF7, 0xFB);     /* -7/2 */
    CHECK_EQ(EAX & 0xFFFF, 0xFFFD); CHECK_EQ(EDX & 0xFFFF, 0xFFFF);
    reset(); EAX = 0x8000; EDX = 0xFFFF; EBX = 0xFFFF; CHECK_EQ(RUN(0xF7, 0xFB), CPU_E_DE);
    reset(); EAX = 0x0007; EDX = 0; EBX = 0xFFFE; RUN_OK(0xF7, 0xFB);     /* 7/-2 = -3 r 1 */
    CHECK_EQ(EAX & 0xFFFF, 0xFFFD); CHECK_EQ(EDX & 0xFFFF, 1);
    reset(); EAX = 0; EDX = 0x80000000; EBX = 0xFFFFFFFF; CHECK_EQ(RUN(0x66, 0xF7, 0xFB), CPU_E_DE);
    reset(); EAX = 0x80000000; EDX = 0xFFFFFFFF; EBX = 0xFFFFFFFF;        /* -2^31 / -1 = 2^31: overflow */
    CHECK_EQ(RUN(0x66, 0xF7, 0xFB), CPU_E_DE);
    reset(); EAX = 0xFFFFFFF9; EDX = 0xFFFFFFFF; EBX = 2; RUN_OK(0x66, 0xF7, 0xFB);
    CHECK_EQ(EAX, 0xFFFFFFFD); CHECK_EQ(EDX, 0xFFFFFFFF);
}

static void test_extend_moves(void) {
    printf("test_extend_moves\n");
    reset(); EAX = 0x80; RUN_OK(0x98); CHECK_EQ(EAX, 0xFF80);               /* cbw */
    reset(); EAX = 0x7F; RUN_OK(0x98); CHECK_EQ(EAX, 0x7F);
    reset(); EAX = 0xAAAA8000; RUN_OK(0x66, 0x98); CHECK_EQ(EAX, 0xFFFF8000); /* cwde */
    reset(); EAX = 0x8000; RUN_OK(0x99); CHECK_EQ(EDX & 0xFFFF, 0xFFFF);
    reset(); EAX = 0x7FFF; EDX = 0xFFFF; RUN_OK(0x99); CHECK_EQ(EDX & 0xFFFF, 0);
    reset(); EAX = 0x80000000; RUN_OK(0x66, 0x99); CHECK_EQ(EDX, 0xFFFFFFFF);
    reset(); EAX = 0x7FFFFFFF; EDX = 5; RUN_OK(0x66, 0x99); CHECK_EQ(EDX, 0);
    reset(); EBX = 0x80; RUN_OK(0x0F, 0xB6, 0xC3);                           /* movzx ax,bl */
    CHECK_EQ(EAX & 0xFFFF, 0x80);
    reset(); EBX = 0x80; EAX = 0xFFFFFFFF; RUN_OK(0x66, 0x0F, 0xB6, 0xC3);   /* movzx eax,bl */
    CHECK_EQ(EAX, 0x80);
    reset(); EBX = 0x80; RUN_OK(0x0F, 0xBE, 0xC3); CHECK_EQ(EAX & 0xFFFF, 0xFF80); /* movsx ax,bl */
    reset(); EBX = 0x80; RUN_OK(0x66, 0x0F, 0xBE, 0xC3); CHECK_EQ(EAX, 0xFFFFFF80);
    reset(); EBX = 0x8001; RUN_OK(0x66, 0x0F, 0xB7, 0xC3); CHECK_EQ(EAX, 0x8001); /* movzx eax,bx */
    reset(); EBX = 0x8001; RUN_OK(0x66, 0x0F, 0xBF, 0xC3); CHECK_EQ(EAX, 0xFFFF8001);
    reset(); EBX = 0x10; wr16(DSB + 0x10, 0xFFFE); RUN_OK(0x66, 0x0F, 0xBF, 0x0F); CHECK_EQ(ECX, 0xFFFFFFFE);
    reset(); EBX = 0x10; wr8(DSB + 0x10, 0xFE); RUN_OK(0x0F, 0xB6, 0x0F); CHECK_EQ(ECX & 0xFFFF, 0xFE);
    reset(); EAX = 0x1234; EBX = 0xF0; RUN_OK(0x0F, 0xBE, 0xCC);             /* movsx cx,ah?: reg=ecx? rm=4(AH) */
    CHECK_EQ(ECX & 0xFFFF, 0x12);
}

static int ref_cond(int cc, unsigned fl) {
    int cf = !!(fl & CPU_CF), pf = !!(fl & CPU_PF), zf = !!(fl & CPU_ZF);
    int sf = !!(fl & CPU_SF), of = !!(fl & CPU_OF), r = 0;
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

static void test_setcc(void) {
    unsigned bad = 0, combo;
    int cc;
    printf("test_setcc\n");
    reset();
    for (cc = 0; cc < 16; cc++)
        for (combo = 0; combo < 32; combo++) {
            unsigned fl = 2 | ((combo & 1) ? CPU_CF : 0) | ((combo & 2) ? CPU_PF : 0) |
                          ((combo & 4) ? CPU_ZF : 0) | ((combo & 8) ? CPU_SF : 0) |
                          ((combo & 16) ? CPU_OF : 0);
            EAX = 0xFFFFFF55; FL = fl;
            run_code((const uint8_t[]){0x0F, (uint8_t)(0x90 + cc), 0xC0}, 3);
            if ((EAX & 0xFF) != (unsigned)ref_cond(cc, fl) || (EAX >> 8) != 0xFFFFFF || FL != fl) bad++;
        }
    CHECK_EQ(bad, 0);
    reset(); EBX = 0x10; FL |= CPU_ZF; wr16(DSB + 0x10, 0xFFFF);
    RUN_OK(0x0F, 0x94, 0x07); CHECK_EQ(rd16(DSB + 0x10), 0xFF01);   /* sete [bx]: one byte */
}

static void test_bit_ops(void) {
    printf("test_bit_ops\n");
    reset(); EAX = 0x0A; ECX = 1; RUN_OK(0x0F, 0xA3, 0xC8);                /* bt ax,cx */
    CHECK_EQ(EAX, 0x0A); CHECK_EQ(FL & CPU_CF, CPU_CF);
    reset(); EAX = 0x0A; ECX = 0; RUN_OK(0x0F, 0xA3, 0xC8); CHECK_EQ(FL & CPU_CF, 0);
    reset(); EAX = 0x0A; ECX = 17; RUN_OK(0x0F, 0xA3, 0xC8); CHECK_EQ(FL & CPU_CF, CPU_CF); /* mod 16 */
    reset(); EAX = 0x0A; ECX = 0; RUN_OK(0x0F, 0xAB, 0xC8);                /* bts */
    CHECK_EQ(EAX, 0x0B); CHECK_EQ(FL & CPU_CF, 0);
    reset(); EAX = 0x0A; ECX = 1; RUN_OK(0x0F, 0xB3, 0xC8);                /* btr */
    CHECK_EQ(EAX, 0x08); CHECK_EQ(FL & CPU_CF, CPU_CF);
    reset(); EAX = 0x0A; ECX = 0; RUN_OK(0x0F, 0xBB, 0xC8);                /* btc */
    CHECK_EQ(EAX, 0x0B);
    RUN_OK(0x0F, 0xBB, 0xC8); CHECK_EQ(EAX, 0x0A); CHECK_EQ(FL & CPU_CF, CPU_CF);
    reset(); EAX = 0x80000000; RUN_OK(0x66, 0x0F, 0xBA, 0xE0, 31); CHECK_EQ(FL & CPU_CF, CPU_CF); /* bt eax,31 */
    reset(); EAX = 0; RUN_OK(0x0F, 0xBA, 0xE8, 0x13);                      /* bts ax,19 -> bit 3 */
    CHECK_EQ(EAX, 0x08);
    reset(); EAX = 0xFFFF; RUN_OK(0x0F, 0xBA, 0xF0, 0x04); CHECK_EQ(EAX, 0xFFEF);  /* btr ax,4 */
    reset(); EAX = 0; RUN_OK(0x0F, 0xBA, 0xF8, 0x04); CHECK_EQ(EAX, 0x10);         /* btc ax,4 */
    reset(); FL |= CPU_ZF | CPU_SF | CPU_OF; EAX = 1; ECX = 0;
    RUN_OK(0x0F, 0xA3, 0xC8); CHECK_EQ(FL & (CPU_ZF | CPU_SF | CPU_OF), CPU_ZF | CPU_SF | CPU_OF);
    /* memory form with a bit offset beyond the operand */
    reset(); EBX = 0x10; ECX = 35; wr8(DSB + 0x14, 0x08);                  /* word: +4 bytes, bit 3 */
    RUN_OK(0x0F, 0xA3, 0x0F); CHECK_EQ(FL & CPU_CF, CPU_CF);
    reset(); EBX = 0x10; ECX = 100; wr8(DSB + 0x1C, 0x10);                 /* +12 bytes, bit 4 */
    RUN_OK(0x0F, 0xAB, 0x0F); CHECK_EQ(rd8(DSB + 0x1C), 0x10);
    reset(); EBX = 0x10; ECX = 100; RUN_OK(0x66, 0x0F, 0xAB, 0x0F);        /* 32-bit: +12, bit 4 */
    CHECK_EQ(rd8(DSB + 0x1C), 0x10); CHECK_EQ(FL & CPU_CF, 0);
    reset(); EBX = 0x20; ECX = 0xFFFF; wr16(DSB + 0x1E, 0x8000);           /* -1: word at -2, bit 15 */
    RUN_OK(0x0F, 0xA3, 0x0F); CHECK_EQ(FL & CPU_CF, CPU_CF);
    reset(); EBX = 0x10; wr16(DSB + 0x10, 0x0004);                         /* imm form stays in operand */
    RUN_OK(0x0F, 0xBA, 0x27, 0x12); CHECK_EQ(FL & CPU_CF, CPU_CF);
    /* BSF / BSR */
    reset(); EBX = 0x0100; EAX = 0x1111; RUN_OK(0x0F, 0xBC, 0xC3);
    CHECK_EQ(EAX & 0xFFFF, 8); CHECK_EQ(FL & CPU_ZF, 0);
    reset(); EBX = 0x00F0; RUN_OK(0x0F, 0xBC, 0xC3); CHECK_EQ(EAX & 0xFFFF, 4);
    reset(); EBX = 0x00F0; RUN_OK(0x0F, 0xBD, 0xC3); CHECK_EQ(EAX & 0xFFFF, 7);
    reset(); EBX = 0; EAX = 0x1234; RUN_OK(0x0F, 0xBC, 0xC3);
    CHECK_EQ(EAX, 0x1234); CHECK_EQ(FL & CPU_ZF, CPU_ZF);
    reset(); EBX = 0; EAX = 0x1234; RUN_OK(0x0F, 0xBD, 0xC3); CHECK_EQ(EAX, 0x1234); CHECK_EQ(FL & CPU_ZF, CPU_ZF);
    reset(); EBX = 0x80000000; RUN_OK(0x66, 0x0F, 0xBD, 0xC3); CHECK_EQ(EAX, 31);
    reset(); EBX = 0x00010000; RUN_OK(0x66, 0x0F, 0xBC, 0xC3); CHECK_EQ(EAX, 16);
    reset(); EBX = 0x10; wr16(DSB + 0x10, 0x0800); RUN_OK(0x0F, 0xBC, 0x07); CHECK_EQ(EAX & 0xFFFF, 11);
}

/* ======================================================================
 * Group 3: control flow, interrupts, string ops, flag ops
 * ====================================================================== */

static void set_flags_combo(unsigned combo) {
    FL = 2 | ((combo & 1) ? CPU_CF : 0) | ((combo & 2) ? CPU_PF : 0) |
         ((combo & 4) ? CPU_ZF : 0) | ((combo & 8) ? CPU_SF : 0) | ((combo & 16) ? CPU_OF : 0);
}

static void test_jcc(void) {
    unsigned bad = 0, combo;
    int cc;
    printf("test_jcc\n");
    reset();
    for (cc = 0; cc < 16; cc++)
        for (combo = 0; combo < 32; combo++) {
            unsigned want, fl;
            set_flags_combo(combo); fl = FL; want = ref_cond(cc, fl) ? 0 : 1;
            EBX = 0; run_code((const uint8_t[]){(uint8_t)(0x70 + cc), 0x02, 0xB3, 0x01}, 4);
            if ((EBX & 0xFF) != want || FL != fl) bad++;
            EBX = 0; run_code((const uint8_t[]){0x0F, (uint8_t)(0x80 + cc), 0x02, 0x00, 0xB3, 0x01}, 6);
            if ((EBX & 0xFF) != want) bad++;
            EBX = 0;
            run_code((const uint8_t[]){0x66, 0x0F, (uint8_t)(0x80 + cc), 0x02, 0, 0, 0, 0xB3, 0x01}, 9);
            if ((EBX & 0xFF) != want) bad++;
        }
    CHECK_EQ(bad, 0);
    reset(); /* backward jumps */
    CHECK_EQ(RUN(0xEB, 0x03, 0xB0, 0x07, 0xF4, 0xEB, 0xFB), CPU_HALT);
    CHECK_EQ(EAX & 0xFF, 7); CHECK_EQ(EIP, CODE_OFF + 5);
    reset(); /* JMP near rel16 backward and forward */
    CHECK_EQ(RUN(0xE9, 0x03, 0x00, 0xB0, 0x01, 0xF4, 0xB0, 0x09, 0xE9, 0xF8, 0xFF), CPU_HALT);
    CHECK_EQ(EAX & 0xFF, 1);
    reset(); /* 16-bit IP wraps on near jumps */
    g_mem[CSB + 0xFFF0] = 0xE9; g_mem[CSB + 0xFFF1] = 0x20; g_mem[CSB + 0xFFF2] = 0x00;
    g_mem[CSB + 0x13] = 0xF4;
    EIP = 0xFFF0; CHECK_EQ(cpu_run(&g_cpu, 10), CPU_HALT); CHECK_EQ(EIP, 0x14);
    reset(); /* JMP rel32 with operand-size prefix */
    CHECK_EQ(RUN(0x66, 0xE9, 0x02, 0x00, 0x00, 0x00, 0xB0, 0x05), CPU_HALT);
    CHECK_EQ(EAX & 0xFF, 0);
}

static void test_call_ret(void) {
    printf("test_call_ret\n");
    reset();
    RUN_OK(0xE8, 0x04, 0x00, 0xF4, 0x90, 0x90, 0x90, 0xB0, 0x55, 0xC3);
    CHECK_EQ(EAX & 0xFF, 0x55); CHECK_EQ(ESP, 0x8000); CHECK_EQ(EIP, CODE_OFF + 4);
    reset(); /* return address on the stack */
    CHECK_EQ(RUN(0xE8, 0x01, 0x00, 0xF4, 0xF4), CPU_HALT);
    CHECK_EQ(ESP, 0x7FFE); CHECK_EQ(rd16(SSB + 0x7FFE), CODE_OFF + 3);
    reset(); /* RET n pops arguments */
    ESP = 0x7FF0; wr16(SSB + 0x7FF0, CODE_OFF + 0x20); g_mem[CSB + CODE_OFF + 0x20] = 0xF4;
    CHECK_EQ(RUN(0xC2, 0x04, 0x00), CPU_HALT);
    CHECK_EQ(ESP, 0x7FF6); CHECK_EQ(EIP, CODE_OFF + 0x21);
    reset(); /* 32-bit call/ret */
    RUN_OK(0x66, 0xE8, 0x03, 0x00, 0x00, 0x00, 0xF4, 0x90, 0x90, 0x90, 0x66, 0xC3);
    CHECK_EQ(ESP, 0x8000); CHECK_EQ(EIP, CODE_OFF + 7);
    reset(); RUN(0x66, 0xE8, 0x03, 0x00, 0x00, 0x00, 0xF4, 0xF4);
    CHECK_EQ(rd32(SSB + 0x7FFC), CODE_OFF + 6);
    reset(); /* indirect near */
    EBX = CODE_OFF + 6;
    RUN_OK(0xFF, 0xD3, 0xF4, 0x90, 0x90, 0x90, 0xB0, 0x33, 0xC3);          /* call bx */
    CHECK_EQ(EAX & 0xFF, 0x33); CHECK_EQ(ESP, 0x8000);
    reset(); EBX = 0x10; wr16(DSB + 0x10, CODE_OFF + 6);
    RUN_OK(0xFF, 0x17, 0xF4, 0x90, 0x90, 0x90, 0xB0, 0x44, 0xC3);          /* call [bx] */
    CHECK_EQ(EAX & 0xFF, 0x44);
    reset(); EBX = CODE_OFF + 4;
    RUN_OK(0xFF, 0xE3, 0xF4, 0xF4, 0xB0, 0x66);                            /* jmp bx */
    CHECK_EQ(EAX & 0xFF, 0x66);
    reset(); EBX = 0x10; wr16(DSB + 0x10, CODE_OFF + 4);
    RUN_OK(0xFF, 0x27, 0xF4, 0xF4, 0xB0, 0x77);                            /* jmp [bx] */
    CHECK_EQ(EAX & 0xFF, 0x77);
    reset(); CHECK_EQ(RUN(0xFF, 0xF8), CPU_E_UD);                          /* FF /7 */
    reset(); CHECK_EQ(RUN(0xFF, 0xDB), CPU_E_UD);                          /* call far reg */
}

static void test_far_transfers(void) {
    printf("test_far_transfers\n");
    reset(); /* far call into another code segment and RETF back */
    wr8(DS2B + 0x20, 0xB0); wr8(DS2B + 0x21, 0x77); wr8(DS2B + 0x22, 0xCB);
    RUN_OK(0x9A, 0x20, 0x00, S_DS2, 0x00);
    CHECK_EQ(EAX & 0xFF, 0x77); CHECK_EQ(SEG(CPU_CS), S_CS); CHECK_EQ(ESP, 0x8000);
    CHECK_EQ(EIP, CODE_OFF + 6);
    reset(); /* stack layout: IP below CS */
    wr8(DS2B + 0x20, 0xF4);
    CHECK_EQ(RUN(0x9A, 0x20, 0x00, S_DS2, 0x00), CPU_HALT);
    CHECK_EQ(SEG(CPU_CS), S_DS2); CHECK_EQ(ESP, 0x7FFC);
    CHECK_EQ(rd16(SSB + 0x7FFC), CODE_OFF + 5); CHECK_EQ(rd16(SSB + 0x7FFE), S_CS);
    reset(); /* bad selector faults without changing state */
    CHECK_EQ(RUN(0x9A, 0x20, 0x00, 0x99, 0x00), CPU_E_GP);
    CHECK_EQ(ESP, 0x8000); CHECK_EQ(EIP, CODE_OFF); CHECK_EQ(SEG(CPU_CS), S_CS);
    reset(); wr8(DS2B + 0x30, 0xF4); /* jmp far */
    CHECK_EQ(RUN(0xEA, 0x30, 0x00, S_DS2, 0x00), CPU_HALT);
    CHECK_EQ(SEG(CPU_CS), S_DS2); CHECK_EQ(EIP, 0x31); CHECK_EQ(ESP, 0x8000);
    reset(); /* RETF n */
    ESP = 0x7FF0; wr16(SSB + 0x7FF0, 0x40); wr16(SSB + 0x7FF2, S_DS2); wr8(DS2B + 0x40, 0xF4);
    CHECK_EQ(RUN(0xCA, 0x06, 0x00), CPU_HALT);
    CHECK_EQ(SEG(CPU_CS), S_DS2); CHECK_EQ(ESP, 0x7FFA); CHECK_EQ(EIP, 0x41);
    reset(); ESP = 0x7FF0; wr16(SSB + 0x7FF0, 0x40); wr16(SSB + 0x7FF2, 0x99);
    CHECK_EQ(RUN(0xCB), CPU_E_GP); CHECK_EQ(ESP, 0x7FF0);
    reset(); /* indirect far */
    EBX = 0x10; wr16(DSB + 0x10, 0x20); wr16(DSB + 0x12, S_DS2); wr8(DS2B + 0x20, 0xB0); wr8(DS2B + 0x21, 0x12); wr8(DS2B + 0x22, 0xCB);
    RUN_OK(0xFF, 0x1F); CHECK_EQ(EAX & 0xFF, 0x12); CHECK_EQ(SEG(CPU_CS), S_CS);
    reset(); EBX = 0x10; wr16(DSB + 0x10, 0x50); wr16(DSB + 0x12, S_DS2); wr8(DS2B + 0x50, 0xF4);
    CHECK_EQ(RUN(0xFF, 0x2F), CPU_HALT); CHECK_EQ(SEG(CPU_CS), S_DS2); CHECK_EQ(EIP, 0x51);
    reset(); /* 16:32 far call and return */
    EBX = 0x10; wr32(DSB + 0x10, 0x20); wr16(DSB + 0x14, S_DS2); wr8(DS2B + 0x20, 0x66); wr8(DS2B + 0x21, 0xCB);
    RUN_OK(0x66, 0xFF, 0x1F); CHECK_EQ(SEG(CPU_CS), S_CS); CHECK_EQ(ESP, 0x8000);
    CHECK_EQ(EIP, CODE_OFF + 4);
}

static cpu_status int_rec(cpu_t *c, uint8_t vec, void *ctx) {
    int *p = ctx;
    c->r[CPU_EAX] = 0x1000u + vec;
    *p = vec;
    return vec == 0x10 ? CPU_HALT : vec == 0x11 ? CPU_E_GP : CPU_OK;
}

static void test_int_iret(void) {
    int vec = -1;
    printf("test_int_iret\n");
    reset(); cpu_set_int_handler(&g_cpu, int_rec, &vec);
    RUN_OK(0xCD, 0x21); CHECK_EQ(vec, 0x21); CHECK_EQ(EAX, 0x1021); CHECK_EQ(ESP, 0x8000);
    RUN_OK(0xCC); CHECK_EQ(vec, 3);
    vec = -1; FL &= ~CPU_OF; RUN_OK(0xCE); CHECK_EQ(vec, -1);               /* INTO, OF clear */
    FL |= CPU_OF; RUN_OK(0xCE); CHECK_EQ(vec, 4);
    CHECK_EQ(RUN(0xCD, 0x10), CPU_HALT); CHECK_EQ(EIP, CODE_OFF + 2);         /* handler stops */
    CHECK_EQ(RUN(0xCD, 0x11), CPU_E_GP); CHECK_EQ(EIP, CODE_OFF);             /* handler fault rewinds */
    vec = -1;
    RUN_OK(0xCD, 0x34, 0xC1); CHECK_EQ(vec, -1);                              /* WIN87EM escape: FPU, not the host */
    CHECK_EQ(RUN(0xCD, 0x3F), CPU_E_FPU);
    RUN_OK(0xCD, 0x33); CHECK_EQ(vec, 0x33); RUN_OK(0xCD, 0x40); CHECK_EQ(vec, 0x40);
    reset(); CHECK_EQ(RUN(0xCD, 0x21), CPU_E_INT); CHECK_EQ(EIP, CODE_OFF);
    reset(); CHECK_EQ(RUN(0xCC), CPU_E_INT);
    reset(); /* IRET: IP, CS, FLAGS */
    ESP = 0x7FFA; wr16(SSB + 0x7FFA, 0x40); wr16(SSB + 0x7FFC, S_DS2);
    wr16(SSB + 0x7FFE, CPU_CF | CPU_ZF | CPU_DF | 0xF000); wr8(DS2B + 0x40, 0xF4);
    CHECK_EQ(RUN(0xCF), CPU_HALT);
    CHECK_EQ(SEG(CPU_CS), S_DS2); CHECK_EQ(EIP, 0x41); CHECK_EQ(ESP, 0x8000);
    CHECK_EQ(FL, 0x2 | CPU_CF | CPU_ZF | CPU_DF);
    reset(); ESP = 0x7FFA; wr16(SSB + 0x7FFA, 0x40); wr16(SSB + 0x7FFC, 0x99);
    CHECK_EQ(RUN(0xCF), CPU_E_GP); CHECK_EQ(ESP, 0x7FFA); CHECK_EQ(SEG(CPU_CS), S_CS);
    reset(); ESP = 0x7FF4; wr32(SSB + 0x7FF4, 0x40); wr32(SSB + 0x7FF8, S_DS2);
    wr32(SSB + 0x7FFC, CPU_SF | CPU_IF); wr8(DS2B + 0x40, 0xF4);
    CHECK_EQ(RUN(0x66, 0xCF), CPU_HALT); CHECK_EQ(ESP, 0x8000); CHECK_EQ(FL, 0x2 | CPU_SF | CPU_IF);
}

static cpu_status thunk_hook(cpu_t *c, void *ctx) {
    (void)ctx;
    c->r[CPU_EAX] = 0x4242;
    return cpu_retf(c, 4); /* Pascal: callee pops two word arguments */
}

static void test_hle_thunk(void) {
    printf("test_hle_thunk\n");
    reset();
    CHECK(cpu_add_hook(&g_cpu, S_DS2, 0x20, thunk_hook, NULL));
    RUN_OK(0x6A, 0x01, 0x6A, 0x02, 0x9A, 0x20, 0x00, S_DS2, 0x00);
    CHECK_EQ(EAX, 0x4242); CHECK_EQ(ESP, 0x8000); CHECK_EQ(SEG(CPU_CS), S_CS);
    CHECK_EQ(EIP, CODE_OFF + 10);
    reset(); /* thunk fails when the return frame is unreadable */
    cpu_add_hook(&g_cpu, S_DS2, 0x20, thunk_hook, NULL);
    SEG(CPU_CS) = S_DS2; EIP = 0x20; ESP = 0x7FF0; wr16(SSB + 0x7FF2, 0x99);
    CHECK_EQ(cpu_step(&g_cpu), CPU_E_GP); CHECK_EQ(ESP, 0x7FF0);
}

static void test_enter_leave(void) {
    printf("test_enter_leave\n");
    reset(); EBP = 0x1234;
    RUN_OK(0xC8, 0x08, 0x00, 0x00);
    CHECK_EQ(rd16(SSB + 0x7FFE), 0x1234); CHECK_EQ(EBP, 0x7FFE); CHECK_EQ(ESP, 0x7FF6);
    RUN_OK(0xC9); CHECK_EQ(EBP, 0x1234); CHECK_EQ(ESP, 0x8000);
    reset(); EBP = 0x1234;
    RUN_OK(0xC8, 0x04, 0x00, 0x01);
    CHECK_EQ(rd16(SSB + 0x7FFE), 0x1234); CHECK_EQ(rd16(SSB + 0x7FFC), 0x7FFE);
    CHECK_EQ(EBP, 0x7FFE); CHECK_EQ(ESP, 0x7FF8);
    reset(); EBP = 0x6000; wr16(SSB + 0x5FFE, 0xABCD);
    RUN_OK(0xC8, 0x00, 0x00, 0x02);
    CHECK_EQ(rd16(SSB + 0x7FFE), 0x6000); CHECK_EQ(rd16(SSB + 0x7FFC), 0xABCD);
    CHECK_EQ(rd16(SSB + 0x7FFA), 0x7FFE); CHECK_EQ(EBP, 0x7FFE); CHECK_EQ(ESP, 0x7FFA);
    reset(); EBP = 0x12345678;
    RUN_OK(0x66, 0xC8, 0x08, 0x00, 0x00);
    CHECK_EQ(rd32(SSB + 0x7FFC), 0x12345678); CHECK_EQ(EBP, 0x7FFC); CHECK_EQ(ESP, 0x7FF4);
    RUN_OK(0x66, 0xC9); CHECK_EQ(EBP, 0x12345678); CHECK_EQ(ESP, 0x8000);
    reset(); EBP = 0x7000; wr16(SSB + 0x7000, 0x4321); ESP = 0x6000;   /* LEAVE: SP=BP, pop BP */
    RUN_OK(0xC9); CHECK_EQ(EBP, 0x4321); CHECK_EQ(ESP, 0x7002);
}

static void test_loops(void) {
    printf("test_loops\n");
    reset();
    RUN_OK(0xB9, 0x03, 0x00, 0x40, 0xE2, 0xFD);                            /* mov cx,3; L: inc ax; loop L */
    CHECK_EQ(EAX & 0xFFFF, 3); CHECK_EQ(ECX & 0xFFFF, 0);
    reset(); ECX = 0xABCD0001; RUN_OK(0xE2, 0x02, 0xB0, 0x01);              /* CX=1 -> 0: not taken */
    CHECK_EQ(EAX & 0xFF, 1); CHECK_EQ(ECX, 0xABCD0000);
    reset(); ECX = 0; RUN_OK(0xE2, 0x02, 0xB0, 0x01);                       /* CX=0 -> FFFF: taken */
    CHECK_EQ(EAX & 0xFF, 0); CHECK_EQ(ECX & 0xFFFF, 0xFFFF);
    reset(); ECX = 5; EBX = 0; /* loope: cmp bx,bx keeps ZF set -> 5 iterations */
    RUN_OK(0x42, 0x39, 0xDB, 0xE1, 0xFB);
    CHECK_EQ(EDX & 0xFFFF, 5); CHECK_EQ(ECX & 0xFFFF, 0);
    reset(); ECX = 5; EBX = 0;                                              /* loopne with ZF set stops */
    RUN_OK(0x42, 0x39, 0xDB, 0xE0, 0xFB);
    CHECK_EQ(EDX & 0xFFFF, 1); CHECK_EQ(ECX & 0xFFFF, 4);
    reset(); ECX = 5; EBX = 0; /* loopne: cmp bx,1 gives ZF=0 -> 5 iterations */
    RUN_OK(0x42, 0x83, 0xFB, 0x01, 0xE0, 0xFA);
    CHECK_EQ(EDX & 0xFFFF, 5); CHECK_EQ(ECX & 0xFFFF, 0);
    reset(); ECX = 5; EBX = 1;                                              /* loope with ZF clear stops */
    RUN_OK(0x42, 0x83, 0xFB, 0x01, 0x83, 0xFB, 0x02, 0xE1, 0xF7);
    CHECK_EQ(EDX & 0xFFFF, 1); CHECK_EQ(ECX & 0xFFFF, 4);
    reset(); ECX = 0x10000; RUN_OK(0xE3, 0x02, 0xB0, 0x01);                 /* jcxz: only CX counts */
    CHECK_EQ(EAX & 0xFF, 0);
    reset(); ECX = 1; RUN_OK(0xE3, 0x02, 0xB0, 0x01); CHECK_EQ(EAX & 0xFF, 1);
    reset(); ECX = 0x10000; RUN_OK(0x67, 0xE3, 0x02, 0xB0, 0x01); CHECK_EQ(EAX & 0xFF, 1); /* jecxz */
    reset(); ECX = 0x10000; RUN_OK(0x67, 0xE2, 0x02, 0xB0, 0x01);           /* loop uses ECX */
    CHECK_EQ(ECX, 0xFFFF); CHECK_EQ(EAX & 0xFF, 0);
}

static void test_flag_ops(void) {
    printf("test_flag_ops\n");
    reset(); RUN_OK(0xF9); CHECK_EQ(FL & CPU_CF, CPU_CF);
    RUN_OK(0xF5); CHECK_EQ(FL & CPU_CF, 0); RUN_OK(0xF5); CHECK_EQ(FL & CPU_CF, CPU_CF);
    RUN_OK(0xF8); CHECK_EQ(FL & CPU_CF, 0);
    reset(); RUN_OK(0xFD); CHECK_EQ(FL & CPU_DF, CPU_DF); RUN_OK(0xFC); CHECK_EQ(FL & CPU_DF, 0);
    reset(); RUN_OK(0xFB); CHECK_EQ(FL & CPU_IF, CPU_IF); RUN_OK(0xFA); CHECK_EQ(FL & CPU_IF, 0);
    reset(); FL |= CPU_CF | CPU_ZF; RUN_OK(0x9F); CHECK_EQ(EAX & 0xFF00, 0x4300);     /* lahf */
    reset(); FL |= CPU_CF | CPU_ZF | CPU_SF | CPU_AF | CPU_PF | CPU_OF; RUN_OK(0x9F);
    CHECK_EQ(EAX & 0xFF00, 0xD700);
    reset(); EAX = 0xFF00; FL |= CPU_OF | CPU_DF; RUN_OK(0x9E);                        /* sahf */
    CHECK_EQ(FL & FLAGS_MASK, CPU_CF | CPU_PF | CPU_AF | CPU_ZF | CPU_SF | CPU_OF);
    CHECK_EQ(FL & CPU_DF, CPU_DF);
    reset(); EAX = 0x0000; FL |= CPU_CF | CPU_ZF | CPU_SF | CPU_AF | CPU_PF | CPU_OF; RUN_OK(0x9E);
    CHECK_EQ(FL & FLAGS_MASK, CPU_OF); CHECK_EQ(FL & 2, 2);
    reset(); EAX = 1; EBX = 0x10; RUN_OK(0xF0, 0x01, 0x07);                            /* lock add [bx],ax */
    CHECK_EQ(rd16(DSB + 0x10), 1);
    reset(); EAX = 5; RUN_OK(0x9B); CHECK_EQ(EAX, 5);                                  /* wait */
    reset(); /* too many prefixes */
    {
        uint8_t code[20];
        memset(code, 0x66, sizeof code); code[19] = 0x90;
        CHECK_EQ(run_code(code, 20), CPU_E_UD);
    }
}

static void test_bound_xlat(void) {
    printf("test_bound_xlat\n");
    reset(); EBX = 0x10; wr16(DSB + 0x10, 0xFFF0); wr16(DSB + 0x12, 0x0010); /* -16..16 */
    EAX = 0; RUN_OK(0x62, 0x07);
    EAX = 0x10; RUN_OK(0x62, 0x07); EAX = 0xFFF0; RUN_OK(0x62, 0x07);
    EAX = 0x11; CHECK_EQ(RUN(0x62, 0x07), CPU_E_BOUND); CHECK_EQ(EIP, CODE_OFF);
    EAX = 0xFFEF; CHECK_EQ(RUN(0x62, 0x07), CPU_E_BOUND);
    reset(); CHECK_EQ(RUN(0x62, 0xC0), CPU_E_UD);
    reset(); EBX = 0x10; wr32(DSB + 0x10, 0); wr32(DSB + 0x14, 100); EAX = 101;
    CHECK_EQ(RUN(0x66, 0x62, 0x07), CPU_E_BOUND);
    EAX = 100; RUN_OK(0x66, 0x62, 0x07);
    reset(); EBX = 0x20; EAX = 3; wr8(DSB + 0x23, 0x99); RUN_OK(0xD7); CHECK_EQ(EAX & 0xFF, 0x99);  /* xlat */
    reset(); EBX = 0x20; EAX = 0xFF03; wr8(ESB + 0x23, 0x42); RUN_OK(0x26, 0xD7);
    CHECK_EQ(EAX, 0xFF42);
    reset(); EBX = 0xFFFF; EAX = 2; wr8(DSB + 0x01, 0x77); RUN_OK(0xD7); CHECK_EQ(EAX & 0xFF, 0x77); /* 16-bit wrap */
}

static void test_string_ops(void) {
    int i;
    printf("test_string_ops\n");
    reset(); ESI = 0x10; EDI = 0x20; wr8(DSB + 0x10, 0xAB);
    RUN_OK(0xA4); CHECK_EQ(rd8(ESB + 0x20), 0xAB); CHECK_EQ(ESI, 0x11); CHECK_EQ(EDI, 0x21);
    reset(); ESI = 0x10; EDI = 0x20; wr16(DSB + 0x10, 0xABCD); FL |= CPU_DF;
    RUN_OK(0xA5); CHECK_EQ(rd16(ESB + 0x20), 0xABCD); CHECK_EQ(ESI, 0x0E); CHECK_EQ(EDI, 0x1E);
    reset(); ESI = 0x10; EDI = 0x20; wr32(DSB + 0x10, 0x01020304);
    RUN_OK(0x66, 0xA5); CHECK_EQ(rd32(ESB + 0x20), 0x01020304); CHECK_EQ(ESI, 0x14); CHECK_EQ(EDI, 0x24);
    reset(); ESI = 0xFFFF; EDI = 0xFFFF; /* SI/DI wrap at 16 bits, upper bits preserved */
    ESI |= 0xAAAA0000; EDI |= 0xBBBB0000; wr8(DSB + 0xFFFF, 1);
    RUN_OK(0xA4); CHECK_EQ(ESI, 0xAAAA0000); CHECK_EQ(EDI, 0xBBBB0000);
    reset(); ESI = 0x10; EDI = 0x20; wr8(ESB + 0x10, 0x5E); wr8(DSB + 0x10, 1); /* source override */
    RUN_OK(0x26, 0xA4); CHECK_EQ(rd8(ESB + 0x20), 0x5E);
    reset(); ESI = 0x10; EDI = 0x20; wr8(GSB + 0x10, 0x6E); /* dest stays ES */
    RUN_OK(0x65, 0xA4); CHECK_EQ(rd8(ESB + 0x20), 0x6E);
    reset(); ESI = 0x10; EDI = 0x20; ECX = 5;
    for (i = 0; i < 5; i++) wr8(DSB + 0x10 + (unsigned)i, 0x11 * (i + 1));
    RUN_OK(0xF3, 0xA4);
    CHECK_EQ(ECX, 0); CHECK_EQ(ESI, 0x15); CHECK_EQ(EDI, 0x25);
    for (i = 0; i < 5; i++) CHECK_EQ(rd8(ESB + 0x20 + (unsigned)i), 0x11 * (i + 1));
    reset(); ESI = 0x10; EDI = 0x20; ECX = 3; FL |= CPU_DF; /* backward */
    for (i = 0; i < 3; i++) wr16(DSB + 0x10 - 2 * (unsigned)i, 0x100 + (unsigned)i);
    RUN_OK(0xF3, 0xA5); CHECK_EQ(ESI, 0x0A); CHECK_EQ(EDI, 0x1A);
    for (i = 0; i < 3; i++) CHECK_EQ(rd16(ESB + 0x20 - 2 * (unsigned)i), 0x100 + (unsigned)i);
    reset(); ESI = EDI = 0x500; ECX = 0; FL |= CPU_ZF | CPU_CF; /* rep with CX=0: nothing, flags kept */
    RUN_OK(0xF3, 0xA6); CHECK_EQ(ESI, 0x500); CHECK_EQ(FL & (CPU_ZF | CPU_CF), CPU_ZF | CPU_CF);
    reset(); SEG(CPU_ES) = 0x99; ECX = 0; RUN_OK(0xF3, 0xA4); /* no memory touched */
    reset(); ESI = 0x10; EDI = 0x20; ECX = 3; /* one iteration per step */
    memcpy(g_mem + CODE_OFF, (const uint8_t[]){0xF3, 0xA4}, 2);
    CHECK_EQ(cpu_step(&g_cpu), CPU_OK);
    CHECK_EQ(ECX, 2); CHECK_EQ(EIP, CODE_OFF); CHECK_EQ(ESI, 0x11);
    CHECK_EQ(cpu_step(&g_cpu), CPU_OK); CHECK_EQ(cpu_step(&g_cpu), CPU_OK);
    CHECK_EQ(ECX, 0); CHECK_EQ(EIP, CODE_OFF + 2);
    reset(); ESI = 0x10; EDI = 0x08; ECX = 0x20; SEG(CPU_ES) = S_SMALL; /* fault in the middle of a REP */
    memset(g_mem + DSB + 0x10, 0x5A, 0x20);
    CHECK_EQ(RUN(0xF3, 0xA4), CPU_E_LIMIT);
    CHECK_EQ(EIP, CODE_OFF); CHECK_EQ(EDI, 0x10); CHECK_EQ(ECX, 0x18); CHECK_EQ(rd8(SMB + 0x0F), 0x5A);
    /* CMPS */
    reset(); ESI = 0x10; EDI = 0x20; wr8(DSB + 0x10, 5); wr8(ESB + 0x20, 7);
    RUN_OK(0xA6); CHECK_FLAGS(CPU_CF | CPU_SF | CPU_AF);              /* 5-7 */
    reset(); ESI = 0x10; EDI = 0x20; wr16(DSB + 0x10, 0x8000); wr16(ESB + 0x20, 1);
    RUN_OK(0xA7); CHECK_FLAGS(CPU_OF | CPU_AF | CPU_PF);               /* 0x8000-1 */
    reset(); ESI = 0x10; EDI = 0x20; ECX = 8; memcpy(g_mem + DSB + 0x10, "abcdefgh", 8);
    memcpy(g_mem + ESB + 0x20, "abcXefgh", 8);
    RUN_OK(0xF3, 0xA6);                                                /* repe cmpsb stops at mismatch */
    CHECK_EQ(ECX, 4); CHECK_EQ(ESI, 0x14); CHECK_EQ(EDI, 0x24); CHECK_EQ(FL & CPU_ZF, 0);
    reset(); ESI = 0x10; EDI = 0x20; ECX = 4; memcpy(g_mem + DSB + 0x10, "abcd", 4);
    memcpy(g_mem + ESB + 0x20, "abcd", 4);
    RUN_OK(0xF3, 0xA6); CHECK_EQ(ECX, 0); CHECK_EQ(FL & CPU_ZF, CPU_ZF);
    reset(); ESI = 0x10; EDI = 0x20; ECX = 8; memcpy(g_mem + DSB + 0x10, "abcdefgh", 8);
    memcpy(g_mem + ESB + 0x20, "xbyyyyyy", 8);
    RUN_OK(0xF2, 0xA6);                                                /* repne cmpsb stops at match */
    CHECK_EQ(ECX, 6); CHECK_EQ(FL & CPU_ZF, CPU_ZF);
    /* SCAS */
    reset(); EDI = 0x20; EAX = 'z'; ECX = 10; memcpy(g_mem + ESB + 0x20, "hello z!!!", 10);
    RUN_OK(0xF2, 0xAE); CHECK_EQ(EDI, 0x27); CHECK_EQ(ECX, 3); CHECK_EQ(FL & CPU_ZF, CPU_ZF);
    reset(); EDI = 0x20; EAX = 'q'; ECX = 4; memcpy(g_mem + ESB + 0x20, "abcd", 4);
    RUN_OK(0xF2, 0xAE); CHECK_EQ(ECX, 0); CHECK_EQ(EDI, 0x24); CHECK_EQ(FL & CPU_ZF, 0);
    reset(); EDI = 0x20; EAX = 0x1234; ECX = 3; wr16(ESB + 0x20, 0); wr16(ESB + 0x22, 0x1234);
    RUN_OK(0xF2, 0xAF); CHECK_EQ(ECX, 1); CHECK_EQ(EDI, 0x24);
    reset(); EDI = 0x20; EAX = 0x12345678; wr32(ESB + 0x20, 0x12345678); RUN_OK(0x66, 0xAF);
    CHECK_FLAGS(CPU_ZF | CPU_PF); CHECK_EQ(EDI, 0x24);
    /* STOS / LODS */
    reset(); EDI = 0x20; EAX = 0xBEEF; ECX = 4; RUN_OK(0xF3, 0xAB);
    for (i = 0; i < 4; i++) CHECK_EQ(rd16(ESB + 0x20 + 2 * (unsigned)i), 0xBEEF);
    CHECK_EQ(EDI, 0x28); CHECK_EQ(rd16(ESB + 0x28), 0);
    reset(); EDI = 0x20; EAX = 0x7E; ECX = 3; FL |= CPU_DF; RUN_OK(0xF3, 0xAA);
    CHECK_EQ(rd8(ESB + 0x20), 0x7E); CHECK_EQ(rd8(ESB + 0x1F), 0x7E); CHECK_EQ(rd8(ESB + 0x1E), 0x7E);
    CHECK_EQ(rd8(ESB + 0x1D), 0); CHECK_EQ(EDI, 0x1D);
    reset(); EDI = 0x20; EAX = 0xCAFEBABE; RUN_OK(0x66, 0xAB); CHECK_EQ(rd32(ESB + 0x20), 0xCAFEBABE);
    reset(); ESI = 0x10; wr16(DSB + 0x10, 0x1234); EAX = 0xFFFF0000;
    RUN_OK(0xAD); CHECK_EQ(EAX, 0xFFFF1234); CHECK_EQ(ESI, 0x12);
    reset(); ESI = 0x10; wr8(DSB + 0x10, 0x9C); RUN_OK(0xAC); CHECK_EQ(EAX & 0xFF, 0x9C);
    reset(); ESI = 0x10; ECX = 3; memcpy(g_mem + DSB + 0x10, "xyz", 3); RUN_OK(0xF3, 0xAC);
    CHECK_EQ(EAX & 0xFF, 'z'); CHECK_EQ(ECX, 0);
    reset(); ESI = 0x10; wr32(DSB + 0x10, 0xA0B0C0D0); RUN_OK(0x66, 0xAD); CHECK_EQ(EAX, 0xA0B0C0D0);
    /* 32-bit address size: ESI/EDI/ECX */
    reset(); ESI = 0x10; EDI = 0x20; ECX = 3; memcpy(g_mem + DSB + 0x10, "abc", 3);
    RUN_OK(0x67, 0xF3, 0xA4);
    CHECK_EQ(ECX, 0); CHECK_EQ(ESI, 0x13); CHECK_EQ(EDI, 0x23); CHECK_EQ(rd8(ESB + 0x22), 'c');
    reset(); ESI = 0x10; EDI = 0x20; ECX = 0x10000; /* with 32-bit addressing ECX counts: runs until the limit */
    CHECK_EQ(RUN(0x67, 0xF3, 0xA4), CPU_E_LIMIT); CHECK(ECX != 0x10000);
    reset(); ESI = 0x10; EDI = 0x20; ECX = 0x10000; RUN_OK(0xF3, 0xA4);          /* CX=0: no-op */
    CHECK_EQ(ECX, 0x10000); CHECK_EQ(EDI, 0x20);
    reset(); ESI = 0x10010; EDI = 0x20;                                          /* 32-bit offset beyond limit */
    CHECK_EQ(RUN(0x67, 0xA4), CPU_E_LIMIT); CHECK_EQ(rd8(ESB + 0x20), 0); CHECK_EQ(ESI, 0x10010);
}

/* ======================================================================
 * Group 4: length decoder, decode/execute consistency, real-binary smoke
 * ====================================================================== */

static void dec(const uint8_t *code, size_t n, cpu_insn *out, cpu_status *st) {
    cpu_mem m = t_iface(false);
    memset(out, 0, sizeof *out);
    memcpy(g_mem + CSB + CODE_OFF, code, n);
    memset(g_mem + CSB + CODE_OFF + n, 0, 16);
    *st = cpu_decode(&m, S_CS, CODE_OFF, out);
}
#define DECODE_LEN(want, ...)                                                       \
    do {                                                                            \
        cpu_insn in_; cpu_status st_;                                               \
        dec((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}), &in_, &st_); \
        CHECK_EQ(st_, CPU_OK); CHECK_EQ(in_.length, (want));                        \
    } while (0)

static void test_decode_lengths(void) {
    cpu_insn in; cpu_status st;
    printf("test_decode_lengths\n");
    reset();
    DECODE_LEN(1, 0x90);
    DECODE_LEN(2, 0xB0, 0x12);
    DECODE_LEN(3, 0xB8, 0x34, 0x12);
    DECODE_LEN(6, 0x66, 0xB8, 1, 2, 3, 4);
    DECODE_LEN(2, 0x8B, 0xC3);                 /* mov ax,bx */
    DECODE_LEN(2, 0x8B, 0x07);                 /* [bx] */
    DECODE_LEN(3, 0x8B, 0x46, 0x04);           /* [bp+4] */
    DECODE_LEN(4, 0x8B, 0x06, 0x00, 0x03);     /* [disp16] */
    DECODE_LEN(4, 0x8B, 0x87, 0x00, 0x03);     /* [bx+disp16] */
    DECODE_LEN(3, 0x67, 0x8B, 0x03);           /* [ebx] */
    DECODE_LEN(4, 0x67, 0x8B, 0x04, 0x88);     /* SIB */
    DECODE_LEN(5, 0x67, 0x8B, 0x44, 0x88, 0x08);
    DECODE_LEN(7, 0x67, 0x8B, 0x05, 0, 0, 0, 0);
    DECODE_LEN(8, 0x67, 0x8B, 0x04, 0x25, 0, 0, 0, 0);
    DECODE_LEN(3, 0x26, 0x8B, 0x07);           /* segment override */
    DECODE_LEN(5, 0x9A, 0, 0, 0, 0);
    DECODE_LEN(8, 0x66, 0x9A, 0, 0, 0, 0, 0, 0);
    DECODE_LEN(3, 0xA1, 0, 0);
    DECODE_LEN(6, 0x67, 0xA1, 0, 0, 0, 0);
    DECODE_LEN(4, 0xC8, 0, 0, 0);
    DECODE_LEN(3, 0xF6, 0xC0, 0x0F);           /* test al,imm8 */
    DECODE_LEN(2, 0xF6, 0xD0);                 /* not al */
    DECODE_LEN(4, 0xF7, 0xC0, 0, 1);           /* test ax,imm16 */
    DECODE_LEN(7, 0x66, 0xF7, 0xC0, 0, 0, 0, 1);
    DECODE_LEN(2, 0xF7, 0xE3);                 /* mul bx */
    DECODE_LEN(4, 0x0F, 0x84, 0, 0);
    DECODE_LEN(7, 0x66, 0x0F, 0x84, 0, 0, 0, 0);
    DECODE_LEN(4, 0x0F, 0xA4, 0xD8, 4);        /* shld imm8 */
    DECODE_LEN(4, 0x0F, 0xBA, 0xE0, 3);
    DECODE_LEN(3, 0x0F, 0xAF, 0xC3);
    DECODE_LEN(4, 0x69, 0xC3, 0x0A, 0x00);
    DECODE_LEN(3, 0x6B, 0xC3, 0x0A);
    DECODE_LEN(2, 0xCD, 0x21);
    DECODE_LEN(3, 0x68, 0x34, 0x12);
    DECODE_LEN(6, 0x66, 0x68, 1, 2, 3, 4);
    DECODE_LEN(6, 0x81, 0x06, 0x00, 0x03, 0x01, 0x02);
    DECODE_LEN(3, 0xC1, 0xE0, 0x04);
    DECODE_LEN(5, 0xC7, 0x47, 0x02, 0x34, 0x12);
    DECODE_LEN(2, 0xEB, 0xFE);
    dec((const uint8_t[]){0x0F, 0xFF, 0xC0}, 3, &in, &st);
    CHECK_EQ(st, CPU_OK); CHECK(!in.supported); CHECK_EQ(in.opcode, 0x0FFF);
    dec((const uint8_t[]){0xD9, 0xFE}, 2, &in, &st);
    CHECK_EQ(st, CPU_OK); CHECK(in.fpu); CHECK(in.supported); CHECK_EQ(in.length, 2);
    dec((const uint8_t[]){0xCD, 0x35, 0xE8}, 3, &in, &st);      /* WIN87EM escape + ModRM */
    CHECK(in.fpu); CHECK(in.supported); CHECK_EQ(in.length, 3);
    dec((const uint8_t[]){0xCD, 0x3F}, 2, &in, &st);            /* overlay manager */
    CHECK(in.fpu); CHECK(!in.supported); CHECK_EQ(in.length, 2);
    dec((const uint8_t[]){0xCD, 0x21}, 2, &in, &st);
    CHECK(!in.fpu); CHECK(in.supported);
    dec((const uint8_t[]){0x8B, 0x07}, 2, &in, &st);
    CHECK(in.supported); CHECK_EQ(in.opcode, 0x8B);
    dec((const uint8_t[]){0x6C}, 1, &in, &st); CHECK(!in.supported);     /* INS */
    { /* over-long instruction and fetch faults */
        uint8_t code[20];
        cpu_mem m = t_iface(false);
        memset(code, 0x66, sizeof code);
        dec(code, sizeof code, &in, &st);
        CHECK_EQ(st, CPU_E_UD);
        g_mem[CSB + 0xFFFF] = 0xB8;
        CHECK_EQ(cpu_decode(&m, S_CS, 0xFFFF, &in), CPU_E_LIMIT);
        CHECK_EQ(cpu_decode(&m, 0x99, 0, &in), CPU_E_SEL);
    }
}

static void reset_light(void) {
    cpu_mem m = t_iface(false);
    memset(g_mem + CSB + CODE_OFF, 0, 0x40);
    memset(g_mem + DSB, 0, 0x200); memset(g_mem + ESB, 0, 0x200);
    memset(g_mem + FSB, 0, 0x40); memset(g_mem + GSB, 0, 0x40);
    memset(g_mem + SSB + 0x7F00, 0, 0x300);
    cpu_init(&g_cpu, &m);
    g_cpu.seg[CPU_CS] = S_CS; g_cpu.seg[CPU_DS] = S_DS; g_cpu.seg[CPU_ES] = S_ES;
    g_cpu.seg[CPU_SS] = S_SS; g_cpu.seg[CPU_FS] = S_FS; g_cpu.seg[CPU_GS] = S_GS;
    g_cpu.eip = CODE_OFF; ESP = 0x8000;
}

/* The decoder's "supported" flag and lengths must agree with the executor for
 * every opcode, so a decode gap cannot hide behind a lying table. */
static void test_decode_matches_executor(void) {
    static const uint8_t modrms[] = {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38};
    static const uint8_t prefixes[][2] = {{0, 0}, {0x66, 0}, {0x67, 0}, {0x66, 0x67}};
    unsigned op, p, mi, bad_support = 0, bad_len = 0, bad_fpu = 0, checked = 0;
    printf("test_decode_matches_executor\n");
    reset();
    for (op = 0; op < 0x200; op++) {
        if (op < 0x100 && (op == 0x0F || op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E ||
                           (op >= 0x64 && op <= 0x67) || op == 0xF0 || op == 0xF2 || op == 0xF3))
            continue; /* prefixes and the 0F escape are not instructions on their own */
        for (p = 0; p < 4; p++) {
            bool accepted = false, decoded_supported = false, decoded_fpu = false;
            for (mi = 0; mi < sizeof modrms; mi++) {
                uint8_t code[24];
                size_t n = 0;
                cpu_insn in; cpu_status dst, est;
                uint32_t start = CODE_OFF;
                memset(code, 0, sizeof code);
                if (prefixes[p][0]) code[n++] = prefixes[p][0];
                if (prefixes[p][1]) code[n++] = prefixes[p][1];
                if (op >= 0x100) { code[n++] = 0x0F; code[n++] = (uint8_t)op; } else code[n++] = (uint8_t)op;
                code[n++] = modrms[mi];
                dec(code, sizeof code, &in, &dst);
                if (dst != CPU_OK) continue;
                decoded_supported = in.supported; decoded_fpu = in.fpu;
                reset_light();
                memcpy(g_mem + CSB + CODE_OFF, code, sizeof code);
                est = cpu_step(&g_cpu);
                if (est != CPU_E_UD) accepted = true;
                if (in.fpu != ((op >= 0xD8 && op <= 0xDF) || (op == 0xCD && modrms[mi] >= 0x34))) bad_fpu++;
                if ((est == CPU_OK || est == CPU_HALT) && g_cpu.seg[CPU_CS] == S_CS) {
                    bool branch = (op >= 0x70 && op <= 0x7F) || (op >= 0x180 && op <= 0x18F) ||
                                  (op >= 0xE0 && op <= 0xEB) || op == 0x9A || op == 0xC2 || op == 0xC3 ||
                                  op == 0xCA || op == 0xCB || op == 0xCF || op == 0xFF || op == 0xC9 ||
                                  op == 0xC8;
                    if (!branch || (EIP == start + in.length)) {
                        if (EIP != start + in.length) {
                            if (bad_len++ < 5)
                                printf("  length mismatch op=%x prefix=%02x%02x modrm=%02x decoded=%u executed=%u\n",
                                       op, prefixes[p][0], prefixes[p][1], modrms[mi], in.length, EIP - start);
                        }
                    }
                    checked++;
                }
            }
            /* decoded_supported must equal "some ModRM form executes" */
            if (decoded_supported != accepted && !decoded_fpu) {
                if (bad_support++ < 10)
                    printf("  support mismatch op=%x prefix=%02x%02x decoder=%d executor=%d\n",
                           op, prefixes[p][0], prefixes[p][1], decoded_supported, accepted);
            }
        }
    }
    CHECK_EQ(bad_support, 0); CHECK_EQ(bad_len, 0); CHECK_EQ(bad_fpu, 0);
    CHECK(checked > 3000);
}

/* ---- real WRITER.EXE decode smoke test ---------------------------------- */

typedef struct { unsigned op; unsigned count; uint16_t seg; uint32_t off; } gap_t;

static bool is_prefix(uint8_t b) {
    return b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65 ||
           b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3;
}

static void test_real_writer_decode(void) {
    const char *path = getenv("CWFA_WRITER_EXE");
    ne_image ne;
    cpu_mem m = t_iface(false);
    unsigned s;
    gap_t gaps[64];
    unsigned ngaps = 0, fpu_count = 0, fpu_seen = 0, fpu_bad = 0, total_insn = 0, code_segs = 0, scanned_segs = 0;
    unsigned long total_bytes = 0, code_bytes = 0;
    if (!path || !*path) {
        printf("test_real_writer_decode: SKIP (CWFA_WRITER_EXE not set)\n");
        return;
    }
    printf("test_real_writer_decode\n");
    memset(&ne, 0, sizeof ne);
    CHECK_EQ(ne_open_file(&ne, path), NE_OK);
    if (!ne.data) return;
    memset(gaps, 0, sizeof gaps);
    /* 1. linear sweep from the program entry point until the first unsupported opcode */
    {
        const ne_segment *sg = &ne.segs[ne.cs - 1];
        uint32_t len = sg->file_length ? sg->file_length : 0x10000, ip = ne.ip;
        unsigned n = 0, max_n = 100000;
        cpu_insn in = {0};
        memset(g_mem, 0, MEMSZ);
        memcpy(g_mem + CSB, ne.data + sg->file_offset, len > 0x10000 ? 0x10000 : len);
        while (n < max_n && ip < len) {
            if (cpu_decode(&m, S_CS, ip, &in) != CPU_OK) break;
            if (!in.supported) break;
            ip += in.length;
            n++;
        }
        printf("  entry %u:%04x linear decode: %u instructions, %u bytes, stopped at %04x opcode %x (%s)\n",
               ne.cs, ne.ip, n, ip - ne.ip, ip, in.opcode,
               ip >= len ? "end of segment" : in.fpu ? "FPU" : in.supported ? "fault" : "unsupported");
        CHECK(n > 0);
    }
    /* 2. recursive descent over every code segment from its entry points, following
     * relative jumps and calls; tallies unsupported opcodes. */
    for (s = 0; s < ne.seg_count; s++) {
        const ne_segment *sg = &ne.segs[s];
        uint32_t len;
        uint32_t *work;
        uint8_t *seen;
        size_t nwork = 0, cap = 0, i, t;
        if ((sg->flags & NE_SEG_DATA) || sg->file_offset == 0) continue;
        code_segs++;
        len = sg->file_length ? sg->file_length : 0x10000;
        code_bytes += len;
        seen = calloc(0x10000, 1);
        cap = 0x10000; work = malloc(cap * sizeof *work);
        if (!seen || !work) { free(seen); free(work); break; }
        memset(g_mem, 0, 0x10000);
        memcpy(g_mem + CSB, ne.data + sg->file_offset, len > 0x10000 ? 0x10000 : len);
        for (i = 0; i < ne.entry_count; i++)
            if (ne.entries[i].kind != NE_ENTRY_UNUSED && ne.entries[i].segment == s + 1 &&
                ne.entries[i].offset < len && nwork < cap)
                work[nwork++] = ne.entries[i].offset;
        if (ne.cs == s + 1 && nwork < cap) work[nwork++] = ne.ip;
        /* Internal far calls/jumps (9A/EA off16 sel16) relocate only the selector
         * word: the offset is literal in the code. Collect those that land here. */
        for (t = 0; t < ne.seg_count; t++) {
            const uint8_t *tc = ne.segs[t].file_offset ? ne.data + ne.segs[t].file_offset : NULL;
            if (!tc) continue;
            for (i = 0; i < ne.segs[t].reloc_count; i++) {
                const ne_reloc *r = &ne.segs[t].relocs[i];
                unsigned tseg = r->a, sx;
                if (r->target_type != NE_TGT_INTERNAL || r->src_type != NE_SRC_SELECTOR) continue;
                if (r->a == NE_SEG_MOVEABLE_REF) {
                    if (r->b == 0 || r->b > ne.entry_count) continue;
                    tseg = ne.entries[r->b - 1].segment;
                }
                sx = r->src_offset;
                if (tseg != s + 1 || sx < 3 || sx + 2u > ne.segs[t].file_length) continue;
                if ((tc[sx - 3] == 0x9A || tc[sx - 3] == 0xEA) && nwork < cap) {
                    unsigned toff = (unsigned)(tc[sx - 2] | tc[sx - 1] << 8);
                    if (toff < len) work[nwork++] = toff;
                }
            }
        }
        if (nwork == 0) { free(seen); free(work); continue; }
        scanned_segs++;
        while (nwork) {
            uint32_t ip = work[--nwork];
            for (;;) {
                cpu_insn in;
                uint8_t b[8];
                unsigned pre = 0, k;
                if (ip >= len || seen[ip]) break;
                if (cpu_decode(&m, S_CS, ip, &in) != CPU_OK || ip + in.length > len) break;
                seen[ip] = 1;
                total_insn++;
                total_bytes += in.length;
                if (!in.supported) {
                    unsigned g;
                    if (in.fpu) { if (fpu_count++ < 6) printf("    FPU opcode %04x at %u:%04x\n", in.opcode, s + 1, ip); }
                    else {
                        for (g = 0; g < ngaps; g++) if (gaps[g].op == in.opcode) break;
                        if (g == ngaps && ngaps < 64) { gaps[g].op = in.opcode; gaps[g].seg = (uint16_t)(s + 1); gaps[g].off = ip; ngaps++; }
                        if (g < 64) gaps[g].count++;
                        break; /* cannot follow: length not trusted */
                    }
                }
                for (k = 0; k < 8; k++) b[k] = g_mem[CSB + ip + k];
                while (pre < 6 && is_prefix(b[pre])) pre++;
                if (in.fpu && b[pre] >= 0xD8 && b[pre] <= 0xDF) { /* every linked escape must be a defined 387 encoding */
                    fpu_op fo;
                    fpu_seen++;
                    if (!fpu_decode(b[pre] - 0xD8u, b[pre + 1], &fo) && fpu_bad++ < 6)
                        printf("    undefined x87 encoding %02x %02x at %u:%04x\n", b[pre], b[pre + 1], s + 1, ip);
                }
                {
                    unsigned op = b[pre], next = ip + in.length;
                    int32_t rel = 0;
                    bool has_target = false, stop = false;
                    if ((op >= 0x70 && op <= 0x7F) || op == 0xEB || (op >= 0xE0 && op <= 0xE3)) {
                        rel = (int8_t)b[in.length - 1]; has_target = true; stop = op == 0xEB;
                    } else if ((op == 0xE8 || op == 0xE9) && in.length - pre == 3) {
                        rel = (int16_t)(b[pre + 1] | b[pre + 2] << 8); has_target = true; stop = op == 0xE9;
                    } else if (op == 0x0F && (b[pre + 1] & 0xF0) == 0x80 && in.length - pre == 4) {
                        rel = (int16_t)(b[pre + 2] | b[pre + 3] << 8); has_target = true;
                    } else if (op == 0xC3 || op == 0xC2 || op == 0xCB || op == 0xCA || op == 0xCF ||
                               op == 0xEA || op == 0xF4 || (op == 0xFF && (((b[pre + 1] >> 3) & 7) == 4 || ((b[pre + 1] >> 3) & 7) == 5))) {
                        stop = true;
                    }
                    if (has_target && nwork < cap) work[nwork++] = (uint32_t)((int32_t)next + rel) & 0xFFFF;
                    if (stop) break;
                    ip = next;
                }
            }
        }
        free(seen); free(work);
    }
    printf("  descent: %u code segments (%u with seeds), %u instructions / %lu of %lu code bytes, %u FPU escapes, %u distinct unsupported opcodes\n",
           code_segs, scanned_segs, total_insn, total_bytes, code_bytes, fpu_count, ngaps);
    {
        unsigned g;
        for (g = 0; g < ngaps; g++)
            printf("    unsupported opcode %04x: %u hits, first at %u:%04x\n", gaps[g].op, gaps[g].count, gaps[g].seg, gaps[g].off);
    }
    printf("  x87: %u escapes reached, %u undefined encodings\n", fpu_seen, fpu_bad);
    CHECK_EQ(fpu_bad, 0);
    CHECK(total_insn > 1000);
    ne_free(&ne);
}

/* ---- descriptor queries: VERR VERW LAR LSL ------------------------------ */

static bool zf(void) { return (FL & CPU_ZF) != 0; }

static void test_verr_verw(void) {
    static const struct { uint16_t sel; bool r, w; } cases[] = {
        {S_DS, true, true}, {S_CS, true, false}, {S_RO, true, false}, {S_XO, false, false},
        {S_KRN, false, false}, {S_NP, false, false}, {0, false, false}, {0x1234, false, false},
    };
    size_t i;
    printf("test_verr_verw\n");
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset(); EAX = cases[i].sel; FL |= CPU_ZF * !cases[i].r;
        RUN_OK(0x0F, 0x00, 0xE0); /* verr ax */
        CHECK_EQ(zf(), cases[i].r); CHECK_EQ(EAX, cases[i].sel);
        reset(); EAX = cases[i].sel; FL |= CPU_ZF * !cases[i].w;
        RUN_OK(0x0F, 0x00, 0xE8); /* verw ax */
        CHECK_EQ(zf(), cases[i].w);
    }
    /* memory operand: the selector is read from ds:[0x10] */
    reset(); wr16(DSB + 0x10, S_DS);
    RUN_OK(0x0F, 0x00, 0x26, 0x10, 0x00); CHECK(zf());
    reset(); wr16(DSB + 0x10, S_XO); FL |= CPU_ZF;
    RUN_OK(0x0F, 0x00, 0x26, 0x10, 0x00); CHECK(!zf());
    /* the other reg forms of 0F 00 stay unsupported */
    reset(); CHECK_EQ(RUN(0x0F, 0x00, 0xC0), CPU_E_UD); CHECK_EQ(EIP, CODE_OFF);
    reset(); CHECK_EQ(RUN(0x0F, 0x00, 0xF8), CPU_E_UD);
}

static void test_lar_lsl(void) {
    printf("test_lar_lsl\n");
    reset(); EBX = S_CS; EAX = 0x1111;
    RUN_OK(0x0F, 0x02, 0xC3); /* lar ax,bx */
    CHECK(zf()); CHECK_EQ(EAX, 0xFA00);
    reset(); EBX = S_DS; EAX = 0xAAAA5555;
    RUN_OK(0x66, 0x0F, 0x02, 0xC3); /* lar eax,bx */
    CHECK(zf()); CHECK_EQ(EAX, 0xF200);
    reset(); EBX = S_XO; RUN_OK(0x0F, 0x02, 0xC3); CHECK(zf()); CHECK_EQ(EAX, 0xF800);
    reset(); EBX = S_NP; RUN_OK(0x0F, 0x02, 0xC3); CHECK(zf()); CHECK_EQ(EAX, 0x7200); /* LAR ignores presence */
    /* inaccessible: ZF=0 and the destination is left alone */
    reset(); EBX = S_KRN; EAX = 0x1234; FL |= 0;
    RUN_OK(0x0F, 0x02, 0xC3); CHECK(!zf()); CHECK_EQ(EAX, 0x1234);
    reset(); EBX = 0; EAX = 0x1234; FL |= CPU_ZF;
    RUN_OK(0x0F, 0x02, 0xC3); CHECK(!zf()); CHECK_EQ(EAX, 0x1234);
    reset(); EBX = 0x4321; EAX = 0x1234; FL |= CPU_ZF;
    RUN_OK(0x0F, 0x02, 0xC3); CHECK(!zf());
    /* LSL */
    reset(); EBX = S_SMALL; EAX = 0xFFFF;
    RUN_OK(0x0F, 0x03, 0xC3); CHECK(zf()); CHECK_EQ(EAX, 0x0F);
    reset(); EBX = S_DS; EAX = 0xAAAA5555;
    RUN_OK(0x66, 0x0F, 0x03, 0xC3); CHECK(zf()); CHECK_EQ(EAX, 0xFFFF);
    reset(); EBX = S_KRN; EAX = 0x77; RUN_OK(0x0F, 0x03, 0xC3); CHECK(!zf()); CHECK_EQ(EAX, 0x77);
    reset(); EBX = 0x4321; EAX = 0x77; FL |= CPU_ZF; RUN_OK(0x0F, 0x03, 0xC3); CHECK(!zf());
    /* a host without sel_info accepts every valid selector with a 64K limit */
    reset(); g_cpu.mem.sel_info = NULL; EBX = S_DS;
    RUN_OK(0x0F, 0x03, 0xC3); CHECK(zf()); CHECK_EQ(EAX, 0xFFFF);
    reset(); g_cpu.mem.sel_info = NULL; EBX = 0x4321; FL |= CPU_ZF;
    RUN_OK(0x0F, 0x03, 0xC3); CHECK(!zf());
}

int main(void) {
    test_mov_imm();
    test_modrm16();
    test_segment_overrides();
    test_modrm32();
    test_moffs();
    test_segment_moves();
    test_lea_xchg();
    test_stack();
    test_pusha_popf();
    test_hooks();
    test_step_hook();
    test_halt_and_faults();
    test_wide_callbacks();
    test_alu_known();
    test_alu_grid();
    test_alu_forms();
    test_incdec_negnot();
    test_shift_grid();
    test_shift_forms();
    test_shld_shrd();
    test_muldiv();
    test_extend_moves();
    test_setcc();
    test_bit_ops();
    test_jcc();
    test_call_ret();
    test_far_transfers();
    test_int_iret();
    test_enter_leave();
    test_loops();
    test_flag_ops();
    test_bound_xlat();
    test_string_ops();
    test_hle_thunk();
    test_decode_lengths();
    test_decode_matches_executor();
    test_verr_verw();
    test_lar_lsl();
    test_real_writer_decode();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
