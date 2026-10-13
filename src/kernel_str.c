/* KERNEL string and memory helpers, plus the USER ones that belong with them
 * (lstrcmp, Ansi*, tick count), registered only on request. */
#include <ctype.h>
#include <string.h>
#include <time.h>

#include "kernel_priv.h"

bool kernel_guest_str(hle_call *c, uint16_t sel, uint16_t off, char **s, size_t *len) {
    uint32_t avail = mem_size(&c->proc->mem, sel);
    char *p = hle_ptr(c, sel, off, 1);
    const char *nul;
    if (!p || off >= avail) return false;
    nul = memchr(p, 0, avail - off);
    if (!nul) return false;
    *s = p;
    *len = (size_t)(nul - p);
    return true;
}

bool kernel_huge_copy(loader_proc *p, uint16_t dsel, uint32_t doff, uint16_t ssel, uint32_t soff,
                      uint32_t n) {
    while (n) {
        uint32_t chunk = n, a = 0x10000 - doff, b = 0x10000 - soff;
        uint8_t *d, *s;
        if (chunk > a) chunk = a;
        if (chunk > b) chunk = b;
        d = mem_ptr(&p->mem, dsel, doff, chunk);
        s = mem_ptr(&p->mem, ssel, soff, chunk);
        if (!d || !s) return false;
        memmove(d, s, chunk);
        n -= chunk;
        hle_huge_add(&dsel, &doff, chunk);
        hle_huge_add(&ssel, &soff, chunk);
    }
    return true;
}

uint32_t kernel_now_ms(kernel_t *k) {
    struct timespec ts;
    if (k->cfg.now_ms) return k->cfg.now_ms(k->cfg.now_ctx);
    timespec_get(&ts, TIME_UTC);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u) - k->t0_ms;
}

/* ---- lstr* / hmemcpy ---------------------------------------------------- */

cpu_status k_lstrcpy(hle_call *c) {
    hle_args a;
    uint16_t ds, doff, ss, soff;
    char *src, *dst;
    size_t n;
    hle_args_init(&a, c);
    hle_far(&a, &ds, &doff);
    hle_far(&a, &ss, &soff);
    if (a.st) return a.st;
    if (!kernel_guest_str(c, ss, soff, &src, &n) || !(dst = hle_ptr(c, ds, doff, (uint32_t)n + 1)))
        return CPU_E_SEL;
    memmove(dst, src, n + 1);
    hle_ret32(c, (uint32_t)ds << 16 | doff);
    return CPU_OK;
}

cpu_status k_lstrcat(hle_call *c) {
    hle_args a;
    uint16_t ds, doff, ss, soff;
    char *src, *dst;
    size_t n, dn;
    hle_args_init(&a, c);
    hle_far(&a, &ds, &doff);
    hle_far(&a, &ss, &soff);
    if (a.st) return a.st;
    if (!kernel_guest_str(c, ss, soff, &src, &n) || !kernel_guest_str(c, ds, doff, &dst, &dn) ||
        !hle_ptr(c, ds, (uint32_t)doff + (uint32_t)dn, (uint32_t)n + 1))
        return CPU_E_SEL;
    memmove(dst + dn, src, n + 1);
    hle_ret32(c, (uint32_t)ds << 16 | doff);
    return CPU_OK;
}

cpu_status k_lstrlen(hle_call *c) {
    hle_args a;
    uint16_t sel, off;
    char *s;
    size_t n;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    if (a.st) return a.st;
    if (!kernel_guest_str(c, sel, off, &s, &n)) return CPU_E_SEL;
    hle_ret16(c, (uint16_t)n);
    return CPU_OK;
}

cpu_status k_hmemcpy(hle_call *c) {
    hle_args a;
    uint16_t ds, doff, ss, soff;
    uint32_t n;
    hle_args_init(&a, c);
    hle_far(&a, &ds, &doff);
    hle_far(&a, &ss, &soff);
    n = hle_u32(&a);
    if (a.st) return a.st;
    return kernel_huge_copy(c->proc, ds, doff, ss, soff, n) ? CPU_OK : CPU_E_LIMIT;
}

/* ---- USER helpers --------------------------------------------------------- */

/* Windows ANSI (Latin-1 letters) case mapping */
unsigned kernel_ansi_upper(unsigned ch) {
    if (ch >= 'a' && ch <= 'z') return ch - 32;
    if (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7) return ch - 32;
    return ch;
}

unsigned kernel_ansi_lower(unsigned ch) {
    if (ch >= 'A' && ch <= 'Z') return ch + 32;
    if (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) return ch + 32;
    return ch;
}

static cpu_status case_map(hle_call *c, unsigned (*map)(unsigned)) {
    hle_args a;
    uint16_t sel, off;
    char *s;
    size_t n, i;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    if (a.st) return a.st;
    if (sel == 0) { /* a single character in the low word */
        hle_ret32(c, map(off & 0xFF));
        return CPU_OK;
    }
    if (!kernel_guest_str(c, sel, off, &s, &n)) return CPU_E_SEL;
    for (i = 0; i < n; i++) s[i] = (char)map((unsigned char)s[i]);
    hle_ret32(c, (uint32_t)sel << 16 | off);
    return CPU_OK;
}

static cpu_status u_ansiupper(hle_call *c) { return case_map(c, kernel_ansi_upper); }
static cpu_status u_ansilower(hle_call *c) { return case_map(c, kernel_ansi_lower); }

static cpu_status compare(hle_call *c, bool fold) {
    hle_args a;
    uint16_t s1, o1, s2, o2;
    char *x, *y;
    size_t nx, ny, i;
    int r = 0;
    hle_args_init(&a, c);
    hle_far(&a, &s1, &o1);
    hle_far(&a, &s2, &o2);
    if (a.st) return a.st;
    if (!kernel_guest_str(c, s1, o1, &x, &nx) || !kernel_guest_str(c, s2, o2, &y, &ny))
        return CPU_E_SEL;
    for (i = 0; i <= nx && i <= ny && !r; i++) {
        unsigned p = (unsigned char)x[i], q = (unsigned char)y[i];
        if (fold) {
            p = kernel_ansi_lower(p);
            q = kernel_ansi_lower(q);
        }
        r = p < q ? -1 : p > q ? 1 : 0;
    }
    hle_ret16(c, (uint16_t)(int16_t)r);
    return CPU_OK;
}

static cpu_status u_lstrcmp(hle_call *c) { return compare(c, false); }
static cpu_status u_lstrcmpi(hle_call *c) { return compare(c, true); }

static cpu_status u_ansinext(hle_call *c) {
    hle_args a;
    uint16_t sel, off;
    const uint8_t *p;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    if (a.st) return a.st;
    if (!(p = hle_ptr(c, sel, off, 1))) return CPU_E_SEL;
    if (*p) off++;
    hle_ret32(c, (uint32_t)sel << 16 | off);
    return CPU_OK;
}

static cpu_status u_ansiprev(hle_call *c) {
    hle_args a;
    uint16_t ss, so, cs, co;
    hle_args_init(&a, c);
    hle_far(&a, &ss, &so);
    hle_far(&a, &cs, &co);
    if (a.st) return a.st;
    if (co > so) co--;
    hle_ret32(c, (uint32_t)cs << 16 | co);
    return CPU_OK;
}

static cpu_status u_gettickcount(hle_call *c) {
    hle_ret32(c, kernel_now_ms(c->ctx));
    return CPU_OK;
}

static cpu_status u_initapp(hle_call *c) {
    hle_ret16(c, 1);
    return CPU_OK;
}

#define U "USER"
const hle_def kernel_user_defs[] = {
    {U, 5, "InitApp", 2, u_initapp},
    {U, 13, "GetTickCount", 0, u_gettickcount},
    {U, 15, "GetCurrentTime", 0, u_gettickcount},
    {U, 430, "lstrcmp", 8, u_lstrcmp},
    {U, 431, "AnsiUpper", 4, u_ansiupper},
    {U, 432, "AnsiLower", 4, u_ansilower},
    {U, 471, "lstrcmpi", 8, u_lstrcmpi},
    {U, 472, "AnsiNext", 4, u_ansinext},
    {U, 473, "AnsiPrev", 8, u_ansiprev},
};
#undef U
const size_t kernel_user_ndefs = sizeof kernel_user_defs / sizeof kernel_user_defs[0];
