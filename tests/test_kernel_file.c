#include "hostdir.h"
#include "kfixture.h"

#define F_GETPROFILEINT 57
#define F_GETPROFILESTRING 58
#define F_WRITEPROFILESTRING 59
#define F_ACCESSRESOURCE 64
#define F_FINDRESOURCE 60
#define F_OPENFILE 74
#define F_LCLOSE 81
#define F_LREAD 82
#define F_LCREAT 83
#define F_LLSEEK 84
#define F_LOPEN 85
#define F_LWRITE 86
#define F_GETTEMPFILENAME 97
#define F_DOS3CALL 102
#define F_GETPRIVATEPROFILEINT 127
#define F_GETPRIVATEPROFILESTRING 128
#define F_WRITEPRIVATEPROFILESTRING 129
#define F_GETWINDOWSDIRECTORY 134
#define F_GETSYSTEMDIRECTORY 135
#define F_HREAD 349
#define F_HWRITE 350

static char host[512];
static kernel_config cfg;
static const char *g_label; /* cd_label for the next boot_files */

static void boot_files(void) {
    char *r = hd_make();
    snprintf(host, sizeof host, "%s", r);
    hd_mkdir(host, "DATA");
    hd_mkdir(host, "WINDOWS");
    hd_mkdir(host, "WINDOWS/SYSTEM");
    hd_write(host, "DATA/HELLO.TXT", "Hello, DOS world!", 17);
    hd_write(host, "WINDOWS/SYSTEM/SYSFILE.DAT", "sys", 3);
    memset(&cfg, 0, sizeof cfg);
    cfg.host_root['C' - 'A'] = host;
    cfg.host_root['D' - 'A'] = host;
    cfg.exe_path = "D:\\MSKIDS\\WRITER.EXE";
    cfg.cur_dir = "C:\\";
    cfg.cd_label = g_label;
    kboot_ex(&cfg, 0, 0x800, 0x1000);
}

static void end_files(void) {
    kdown();
    hd_remove(host);
}

static uint16_t ax(void) { return fx_ax(&kf); }

static uint16_t lopen(const char *path, unsigned mode) {
    uint16_t s = kstr(path);
    FX_CALL(&kf, "KERNEL", F_LOPEN, FARP(s, 0), mode);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return ax();
}

static uint16_t lread(uint16_t h, uint16_t sel, uint16_t off, unsigned n) {
    FX_CALL(&kf, "KERNEL", F_LREAD, h, FARP(sel, off), n);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return ax();
}

static uint16_t lclose(uint16_t h) {
    FX_CALL(&kf, "KERNEL", F_LCLOSE, h);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return ax();
}

static void test_read_file(void) {
    uint16_t h, buf;
    printf("test_read_file\n");
    boot_files();
    h = lopen("d:\\data\\hello.txt", 0);                          /* DOS names, any case */
    CHECK(h != 0xFFFF && h >= 5);                                 /* 0-4 are the standard handles */
    buf = kseg(0x100);
    CHECK_EQ(lread(h, buf, 0x10, 5), 5);
    CHECK(!memcmp(kmem(buf, 0x10, 5), "Hello", 5));
    CHECK_EQ(lread(h, buf, 0x20, 100), 12);                       /* short read at EOF */
    CHECK(!memcmp(kmem(buf, 0x20, 12), ", DOS world!", 12));
    CHECK_EQ(lread(h, buf, 0x20, 100), 0);                        /* nothing left */
    CHECK_EQ(lclose(h), 0);
    CHECK_EQ(lclose(h), 0xFFFF);                                  /* HFILE_ERROR */
    CHECK_EQ(lread(h, buf, 0, 1), 0xFFFF);
    CHECK_EQ(lopen("D:\\DATA\\NOPE.TXT", 0), 0xFFFF);
    CHECK_EQ(lopen("E:\\DATA\\HELLO.TXT", 0), 0xFFFF);            /* unmapped drive */
    CHECK_EQ(lread(5, 0x1234, 0, 1), 0xFFFF);
    end_files();
}

static void test_seek(void) {
    uint16_t h, buf;
    printf("test_seek\n");
    boot_files();
    h = lopen("D:\\DATA\\HELLO.TXT", 0);
    buf = kseg(0x100);
    FX_CALL(&kf, "KERNEL", F_LLSEEK, h, LONG(7), 0);
    CHECK_EQ(fx_dxax(&kf), 7);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(lread(h, buf, 0, 3), 3);
    CHECK(!memcmp(kmem(buf, 0, 3), "DOS", 3));
    FX_CALL(&kf, "KERNEL", F_LLSEEK, h, LONG(0), 1);              /* current position */
    CHECK_EQ(fx_dxax(&kf), 10);
    FX_CALL(&kf, "KERNEL", F_LLSEEK, h, LONG((uint32_t)-3), 2);   /* from the end */
    CHECK_EQ(fx_dxax(&kf), 14);
    FX_CALL(&kf, "KERNEL", F_LLSEEK, h, LONG(0), 2);
    CHECK_EQ(fx_dxax(&kf), 17);
    FX_CALL(&kf, "KERNEL", F_LLSEEK, h, LONG(0), 3);              /* bad origin */
    CHECK_EQ(fx_dxax(&kf), 0xFFFFFFFFu);
    FX_CALL(&kf, "KERNEL", F_LLSEEK, h, LONG((uint32_t)-1), 0);   /* before the start */
    CHECK_EQ(fx_dxax(&kf), 0xFFFFFFFFu);
    FX_CALL(&kf, "KERNEL", F_LLSEEK, 0x77, LONG(0), 0);           /* bad handle */
    CHECK_EQ(fx_dxax(&kf), 0xFFFFFFFFu);
    lclose(h);
    end_files();
}

static void test_create_write(void) {
    uint16_t h, name, buf;
    char got[64];
    printf("test_create_write\n");
    boot_files();
    name = kstr("c:\\out.bin");
    FX_CALL(&kf, "KERNEL", F_LCREAT, FARP(name, 0), 0);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    h = ax();
    CHECK(h != 0xFFFF);
    buf = kstr("0123456789");
    FX_CALL(&kf, "KERNEL", F_LWRITE, h, FARP(buf, 0), 10);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(ax(), 10);
    CHECK_EQ(lclose(h), 0);
    CHECK_EQ(hd_read(host, "OUT.BIN", got, sizeof got), 10);       /* new files get upper-case names */
    CHECK(!strcmp(got, "0123456789"));
    h = lopen("C:\\OUT.BIN", 1);                                   /* write access, existing content kept */
    FX_CALL(&kf, "KERNEL", F_LWRITE, h, FARP(buf, 0), 3);
    lclose(h);
    hd_read(host, "OUT.BIN", got, sizeof got);
    CHECK(!strcmp(got, "0123456789"));
    h = lopen("C:\\OUT.BIN", 0);                                   /* read-only handle cannot write */
    FX_CALL(&kf, "KERNEL", F_LWRITE, h, FARP(buf, 0), 3);
    CHECK_EQ(ax(), 0xFFFF);
    lclose(h);
    FX_CALL(&kf, "KERNEL", F_LCREAT, FARP(name, 0), 0);            /* recreate truncates */
    lclose(ax());
    CHECK_EQ(hd_read(host, "OUT.BIN", got, sizeof got), 0);
    end_files();
}

static void test_huge_io(void) {
    uint16_t a, b, name, h;
    uint32_t n = 0x24000, i;
    char got[8];
    printf("test_huge_io\n");
    boot_files();
    a = mem_alloc_huge(&kf.p->mem, 0x30000, MEM_DATA);
    b = mem_alloc_huge(&kf.p->mem, 0x30000, MEM_DATA);
    for (i = 0; i < n; i += 0x100) {
        uint8_t *p = kmem(mem_huge_sel(a, i >> 16), i & 0xFFFF, 1);
        if (p) *p = (uint8_t)(i >> 8 ^ 0x3C);
    }
    name = kstr("C:\\BIG.BIN");
    FX_CALL(&kf, "KERNEL", F_LCREAT, FARP(name, 0), 0);
    h = ax();
    FX_CALL(&kf, "KERNEL", F_HWRITE, h, FARP(a, 0xFF00), LONG(n - 0xFF00));
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(fx_dxax(&kf), n - 0xFF00);
    lclose(h);
    h = lopen("C:\\BIG.BIN", 0);
    FX_CALL(&kf, "KERNEL", F_HREAD, h, FARP(b, 0xFF00), LONG(n));
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(fx_dxax(&kf), n - 0xFF00);                             /* the file is shorter than asked */
    for (i = 0x10000; i < n; i += 0x100) {
        const uint8_t *pb = kmem(mem_huge_sel(b, i >> 16), i & 0xFFFF, 1);
        const uint8_t *pa = kmem(mem_huge_sel(a, i >> 16), i & 0xFFFF, 1);
        CHECK(pa && pb && *pa == *pb);
    }
    lclose(h);
    (void)got;
    end_files();
}

/* ---- OpenFile --------------------------------------------------------------- */

#define OF_READ 0x0000
#define OF_WRITE 0x0001
#define OF_PARSE 0x0100
#define OF_DELETE 0x0200
#define OF_CREATE 0x1000
#define OF_EXIST 0x4000
#define OF_REOPEN 0x8000

static uint16_t openfile(const char *name, uint16_t of, unsigned style) {
    uint16_t s = kstr(name);
    FX_CALL(&kf, "KERNEL", F_OPENFILE, FARP(s, 0), FARP(of, 0), style);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return ax();
}

static void test_openfile(void) {
    uint16_t of, h, buf;
    const uint8_t *o;
    printf("test_openfile\n");
    boot_files();
    of = kseg(0x100);
    h = openfile("d:\\data\\hello.txt", of, OF_READ);
    CHECK(h != 0xFFFF);
    o = kmem(of, 0, 136);
    CHECK(o && o[0] == 136 && o[1] == 1);                          /* cBytes, fFixedDisk */
    CHECK(o && o[2] == 0 && o[3] == 0);                            /* nErrCode */
    CHECK(!strcmp(kcstr(of, 8), "D:\\DATA\\HELLO.TXT"));           /* canonical path */
    buf = kseg(0x20);
    CHECK_EQ(lread(h, buf, 0, 5), 5);
    lclose(h);
    /* reopen from the OFSTRUCT alone */
    h = openfile("", of, OF_REOPEN | OF_READ);
    CHECK(h != 0xFFFF);
    lclose(h);
    /* failure: -1 with the DOS error in nErrCode */
    CHECK_EQ(openfile("d:\\data\\nope.txt", of, OF_READ), 0xFFFF);
    CHECK_EQ(kmem(of, 2, 1)[0], 2);                                /* file not found */
    CHECK_EQ(openfile("d:\\nodir\\nope.txt", of, OF_READ), 0xFFFF);
    CHECK_EQ(kmem(of, 2, 1)[0], 3);                                /* path not found */
    /* OF_EXIST, OF_PARSE */
    CHECK(openfile("D:\\DATA\\HELLO.TXT", of, OF_EXIST) != 0xFFFF);
    CHECK_EQ(openfile("D:\\DATA\\NOPE.TXT", of, OF_EXIST), 0xFFFF);
    CHECK_EQ(openfile("data\\New.Txt", of, OF_PARSE), 0);          /* relative to C:\ here */
    CHECK(!strcmp(kcstr(of, 8), "C:\\DATA\\NEW.TXT"));
    CHECK(!hd_exists(host, "DATA/NEW.TXT"));
    /* OF_CREATE and OF_DELETE */
    h = openfile("c:\\data\\made.txt", of, OF_CREATE | OF_WRITE);
    CHECK(h != 0xFFFF);
    lclose(h);
    CHECK(hd_exists(host, "DATA/MADE.TXT"));
    CHECK(openfile("c:\\data\\made.txt", of, OF_DELETE) != 0xFFFF);
    CHECK(!hd_exists(host, "DATA/MADE.TXT"));
    CHECK_EQ(openfile("c:\\data\\made.txt", of, OF_DELETE), 0xFFFF);
    /* a bare name is searched in the current dir, then the Windows directories */
    h = openfile("sysfile.dat", of, OF_READ);
    CHECK(h != 0xFFFF);
    CHECK(!strcmp(kcstr(of, 8), "C:\\WINDOWS\\SYSTEM\\SYSFILE.DAT"));
    lclose(h);
    end_files();
}

/* ---- DOS3Call --------------------------------------------------------------- */

static uint16_t dos(unsigned ah_al, uint16_t ds, uint16_t bx, uint16_t cx, uint16_t dx) {
    cpu_t *c = &kf.p->cpu;
    uint16_t save_ds = c->seg[CPU_DS];
    c->r[CPU_EAX] = ah_al;
    c->r[CPU_EBX] = bx;
    c->r[CPU_ECX] = cx;
    c->r[CPU_EDX] = dx;
    if (ds) c->seg[CPU_DS] = ds;
    FX_CALL(&kf, "KERNEL", F_DOS3CALL);
    c->seg[CPU_DS] = save_ds;
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return ax();
}
static bool carry(void) { return (kf.p->cpu.eflags & CPU_CF) != 0; }

/* AH=2Fh gives back the DTA that AH=1Ah set (KIDSWTCH saves it, points it at its own buffer for FindFirst and
 * restores it). */
static void test_get_dta(void) {
    uint16_t dta;
    printf("test_get_dta\n");
    boot_files();
    dta = kseg(0x100);
    dos(0x1A00, dta, 0, 0, 0x20);
    dos(0x2F00, 0, 0, 0, 0);
    CHECK(!carry());
    CHECK_EQ(kf.p->cpu.seg[CPU_ES], dta);
    CHECK_EQ(kf.p->cpu.r[CPU_EBX] & 0xFFFF, 0x20);
    end_files();
}

/* AH=59h reports the error of the last failed call: the programs probe for free file names with AH=43h and read
 * the extended error to tell "not found" (use the name) from a real failure. */
static void test_extended_error(void) {
    printf("test_extended_error\n");
    boot_files();
    dos(0x4300, kstr("D:\\DATA\\NOPE.TXT"), 0, 0, 0);
    CHECK(carry());
    CHECK_EQ(ax(), 2);
    dos(0x5900, 0, 0, 0, 0);
    CHECK(!carry());
    CHECK_EQ(ax(), 2);                                             /* the code of the failure */
    CHECK_EQ(kf.p->cpu.r[CPU_EBX] >> 8 & 0xFF, 8);                 /* class: not found */
    dos(0x3B00, kstr("D:\\NOWHERE"), 0, 0, 0);
    dos(0x5900, 0, 0, 0, 0);
    CHECK_EQ(ax(), 3);                                             /* path not found */
    dos(0x5900, 0, 0, 0, 0);
    CHECK_EQ(ax(), 3);                                             /* reading it again does not clear it */
    end_files();
}

/* With the HLE trace on, every DOS call is logged with its register arguments, the path it names and its outcome. */
static void test_dos_trace(void) {
    FILE *log = tmpfile();
    char buf[1024];
    size_t n;
    printf("test_dos_trace\n");
    boot_files();
    kh.log = log;
    kh.trace = true;
    dos(0x3D00, kstr("D:\\DATA\\NOPE.TXT"), 0, 0, 0);
    dos(0x3000, 0, 0, 0, 0);
    kh.log = NULL;
    kh.trace = false;
    rewind(log);
    n = fread(buf, 1, sizeof buf - 1, log);
    buf[n] = 0;
    CHECK(strstr(buf, "dos: AH=3D AL=00") != NULL);
    CHECK(strstr(buf, "\"D:\\DATA\\NOPE.TXT\"") != NULL);
    CHECK(strstr(buf, "error 2") != NULL);
    CHECK(strstr(buf, "dos: AH=30 ") != NULL);
    CHECK(strstr(buf, "ok") != NULL);
    fclose(log);
    end_files();
}

/* A program that executes INT 21h itself (the C runtime's _dos_ calls and int86x) gets the same service as Dos3Call. */
static void test_int21(void) {
    cpu_t *c;
    uint8_t *code;
    uint16_t s;
    printf("test_int21\n");
    boot_files();
    c = &kf.p->cpu;
    code = mem_ptr(&kf.p->mem, kf.p->seg_sel[0], FX_CALL_IP, 3);
    CHECK(code != NULL);
    if (!code) {
        end_files();
        return;
    }
    code[0] = 0xCD; code[1] = 0x21; code[2] = 0xF4;
    c->seg[CPU_CS] = kf.p->seg_sel[0];
    c->eip = FX_CALL_IP;
    c->r[CPU_EAX] = 0x3000;
    CHECK_EQ(loader_run(kf.p, 1000), LOADER_STOP_HLT);
    CHECK_EQ(c->r[CPU_EAX] & 0xFFFF, 0x0006);
    CHECK(!carry());
    s = kstr("D:\\DATA\\NOPE.TXT");                                 /* an error comes back as carry + code in AX */
    c->seg[CPU_CS] = kf.p->seg_sel[0];
    c->eip = FX_CALL_IP;
    c->r[CPU_EAX] = 0x3D00;
    c->r[CPU_EDX] = 0;
    c->seg[CPU_DS] = s;
    CHECK_EQ(loader_run(kf.p, 1000), LOADER_STOP_HLT);
    CHECK(carry());
    CHECK_EQ(c->r[CPU_EAX] & 0xFFFF, 2);
    c->seg[CPU_DS] = kf.p->auto_ds;
    end_files();
}

/* FindFirst/FindNext fill the DTA: attribute at 15h, size at 1Ah, the 8.3 name at 1Eh. */
static void test_find(void) {
    uint16_t dta, pat;
    const uint8_t *d;
    char names[8][16];
    unsigned n = 0, i;
    printf("test_find\n");
    boot_files();
    hd_write(host, "DATA/B.SCR", "12345", 5);
    hd_write(host, "DATA/A.SCR", "x", 1);
    hd_write(host, "DATA/C.TXT", "", 0);
    hd_write(host, "DATA/LONGFILENAME.SCR", "", 0);
    hd_mkdir(host, "DATA/SUB.SCR");
    dta = kseg(0x100);
    dos(0x1A00, dta, 0, 0, 0);                                     /* set DTA to dta:0 */
    pat = kstr("D:\\data\\*.scr");
    dos(0x4E00, pat, 0, 0, 0);
    while (!carry() && n < 8) {
        d = kmem(dta, 0, 0x2B);
        snprintf(names[n], sizeof names[n], "%s", (const char *)d + 0x1E);
        if (n == 1) CHECK_EQ(d[0x1A], 5);                          /* B.SCR is 5 bytes */
        CHECK_EQ(d[0x15], 0x20);
        n++;
        dos(0x4F00, 0, 0, 0, 0);
    }
    CHECK_EQ(n, 2);                                                /* files only: no directory, no 8.3 violation */
    CHECK_EQ(ax(), 18);                                            /* no more files */
    for (i = 0; i < n; i++) CHECK(!strcmp(names[i], i ? "B.SCR" : "A.SCR"));
    dos(0x4E00, pat, 0, 0x10, 0);                                  /* with the directory attribute */
    n = 0;
    while (!carry() && n < 8) {
        n++;
        dos(0x4F00, 0, 0, 0, 0);
    }
    CHECK_EQ(n, 3);
    pat = kstr("D:\\data\\*.tl4");
    dos(0x4E00, pat, 0, 0, 0);
    CHECK(carry());
    CHECK_EQ(ax(), 18);                                            /* an empty match is "no more files" */
    pat = kstr("D:\\nodir\\*.*");
    dos(0x4E00, pat, 0, 0, 0);
    CHECK(carry());
    CHECK_EQ(ax(), 3);                                             /* path not found */
    pat = kstr("D:\\data\\?.*");
    dos(0x4E00, pat, 0, 0, 0);
    n = 0;
    while (!carry() && n < 8) {
        n++;
        dos(0x4F00, 0, 0, 0, 0);
    }
    CHECK_EQ(n, 3);                                                /* A.SCR B.SCR C.TXT */
    dos(0x5900, 0, 0, 0, 0);                                       /* get extended error: supported */
    CHECK(!carry());
    end_files();
}

/* FindFirst with the volume attribute on a drive's root returns its label: the programs look for the CD by
 * it ("?:\\*.*", attribute 08h). Only the CD drive has one, and it is named like DOS shows it (8.3). */
static void test_volume_label(void) {
    uint16_t dta, pat;
    const uint8_t *d;
    printf("test_volume_label\n");
    g_label = "KIDS_CD_01";
    boot_files();
    dta = kseg(0x100);
    dos(0x1A00, dta, 0, 0, 0);
    pat = kstr("D:\\*.*");
    dos(0x4E00, pat, 0, 0x08, 0);
    CHECK(!carry());
    d = kmem(dta, 0, 0x2B);
    CHECK_EQ(d[0x15], 0x08);                                       /* the volume attribute */
    CHECK(!strcmp((const char *)d + 0x1E, "KIDS_CD_.01"));        /* 11 characters: 8 + dot + 3 */
    dos(0x4F00, 0, 0, 0, 0);                                       /* one label per drive */
    CHECK(carry());
    CHECK_EQ(ax(), 18);
    dos(0x4E00, kstr("D:\\DATA\\*.*"), 0, 0, 0);                   /* without the attribute: ordinary files */
    CHECK(!carry());
    CHECK_EQ(kmem(dta, 0, 0x2B)[0x15] & 0x08, 0);
    dos(0x4E00, kstr("C:\\*.*"), 0, 0x08, 0);                     /* C: has no label */
    CHECK(carry());
    CHECK_EQ(ax(), 18);
    dos(0x4E00, kstr("D:\\DATA\\*.*"), 0, 0x08, 0);               /* labels live in the root only */
    CHECK(carry());
    CHECK_EQ(ax(), 18);
    end_files();
    g_label = "CWFA";
    boot_files();
    dta = kseg(0x100);
    dos(0x1A00, dta, 0, 0, 0);
    dos(0x4E00, kstr("D:*.*"), 0, 0x08, 0);                        /* drive-relative root pattern */
    CHECK(!carry());
    CHECK(!strcmp((const char *)kmem(dta, 0, 0x2B) + 0x1E, "CWFA"));
    end_files();
    g_label = NULL;
}

/* AH=57h: a program finds the newest saved file by its date and time (Fine Artist picks "the last thing you saved") */
static void test_dos_filetime(void) {
    uint16_t s, buf, h, date = (15 << 9) | (6 << 5) | 15, tim = (12 << 11) | (34 << 5) | 28;
    printf("test_dos_filetime\n");
    boot_files();
    s = kstr("C:\\FT.TXT");
    h = dos(0x3C00, s, 0, 0, 0);
    buf = kstr("abc");
    dos(0x4000, buf, h, 3, 0);
    dos(0x5700, 0, h, 0, 0);                                       /* get: just written, so after 2020 */
    CHECK(!carry());
    CHECK((kf.p->cpu.r[CPU_EDX] & 0xFFFF) >> 9 > 40);              /* year - 1980 */
    dos(0x5701, 0, h, tim, date);                                  /* set 1995-06-15 12:34:56 */
    CHECK(!carry());
    dos(0x5700, 0, h, 0, 0);
    CHECK(!carry());
    CHECK_EQ(kf.p->cpu.r[CPU_ECX] & 0xFFFF, tim);
    CHECK_EQ(kf.p->cpu.r[CPU_EDX] & 0xFFFF, date);
    dos(0x3E00, 0, h, 0, 0);
    dos(0x5700, 0, h, 0, 0);                                       /* closed handle */
    CHECK(carry());
    CHECK_EQ(ax(), 6);
    dos(0x4100, s, 0, 0, 0);
    end_files();
}

static void test_dos3call(void) {
    uint16_t s, buf, h, big;
    printf("test_dos3call\n");
    boot_files();
    CHECK_EQ(dos(0x3000, 0, 0, 0, 0), 0x0006);                     /* DOS 6.00: AL major, AH minor */
    CHECK(!carry());
    CHECK_EQ(dos(0x1900, 0, 0, 0, 0) & 0xFF, 2);                   /* current drive C: */
    s = kstr("D:\\DATA\\HELLO.TXT");
    h = dos(0x3D00, s, 0, 0, 0);                                   /* open for reading */
    CHECK(!carry() && h >= 5);
    buf = kseg(0x100);
    CHECK_EQ(dos(0x3F00, buf, h, 5, 0), 5);                        /* read 5 bytes to buf:0 */
    CHECK(!carry() && !memcmp(kmem(buf, 0, 5), "Hello", 5));
    CHECK_EQ(dos(0x4200, 0, h, 0, 7), 7);                          /* lseek set 7, DX:AX */
    CHECK_EQ(kf.p->cpu.r[CPU_EDX] & 0xFFFF, 0);
    CHECK_EQ(dos(0x3F00, buf, h, 3, 0), 3);
    CHECK(!memcmp(kmem(buf, 0, 3), "DOS", 3));
    dos(0x3E00, 0, h, 0, 0);                                       /* close */
    CHECK(!carry());
    dos(0x3E00, 0, h, 0, 0);
    CHECK(carry());                                                /* invalid handle */
    CHECK_EQ(ax(), 6);
    s = kstr("D:\\DATA\\NOPE.TXT");
    dos(0x3D00, s, 0, 0, 0);
    CHECK(carry());
    CHECK_EQ(ax(), 2);                                             /* file not found */
    /* create, write, delete */
    s = kstr("C:\\DOSOUT.TXT");
    h = dos(0x3C00, s, 0, 0, 0);
    CHECK(!carry());
    buf = kstr("abc");
    CHECK_EQ(dos(0x4000, buf, h, 3, 0), 3);
    dos(0x3E00, 0, h, 0, 0);
    CHECK(hd_exists(host, "DOSOUT.TXT"));
    CHECK_EQ(dos(0x4300, s, 0, 0, 0) != 0xFFFF && !carry(), 1);    /* get attributes */
    dos(0x4100, s, 0, 0, 0);
    CHECK(!carry() && !hd_exists(host, "DOSOUT.TXT"));
    /* directories */
    s = kstr("D:\\DATA");
    dos(0x3B00, s, 0, 0, 0);                                       /* chdir */
    CHECK(!carry());
    big = kseg(0x100);
    kf.p->cpu.r[CPU_ESI] = 0x10;
    dos(0x4700, big, 0, 0, 4);                                     /* DL = drive 4 (D:), DS:SI buffer */
    CHECK(!carry() && !strcmp(kcstr(big, 0x10), "DATA"));
    s = kstr("D:\\NOSUCH");
    dos(0x3B00, s, 0, 0, 0);
    CHECK(carry() && ax() == 3);
    /* commit file (AH=68h, 6Ah): flush the handle's data to the host file */
    s = kstr("C:\\COMMIT.TXT");
    h = dos(0x3C00, s, 0, 0, 0);
    buf = kstr("xyz");
    dos(0x4000, buf, h, 3, 0);
    dos(0x6800, 0, h, 0, 0);
    CHECK(!carry());
    {
        char got[8];
        CHECK_EQ(hd_read(host, "COMMIT.TXT", got, sizeof got), 3);
    }
    dos(0x6A00, 0, h, 0, 0);
    CHECK(!carry());
    dos(0x3E00, 0, h, 0, 0);
    dos(0x6800, 0, h, 0, 0);
    CHECK(carry() && ax() == 6);                                   /* closed: invalid handle */
    dos(0x4100, s, 0, 0, 0);
    /* mkdir / rmdir (AH=39h, 3Ah): found by the login flow, which creates the new user's directory */
    s = kstr("C:\\NEWDIR");
    dos(0x3900, s, 0, 0, 0);
    CHECK(!carry() && hd_exists(host, "NEWDIR"));
    dos(0x3900, s, 0, 0, 0);
    CHECK(carry() && ax() == 5);                                   /* it exists: access denied */
    s = kstr("C:\\NOPARENT\\SUB");
    dos(0x3900, s, 0, 0, 0);
    CHECK(carry() && ax() == 3);                                   /* no parent: path not found */
    CHECK(!hd_exists(host, "NOPARENT"));
    s = kstr("C:\\NEWDIR\\INNER");
    dos(0x3900, s, 0, 0, 0);
    CHECK(!carry() && hd_exists(host, "NEWDIR/INNER"));
    s = kstr("C:\\NEWDIR");
    dos(0x3A00, s, 0, 0, 0);
    CHECK(carry() && ax() == 5 && hd_exists(host, "NEWDIR"));      /* not empty */
    s = kstr("C:\\NEWDIR\\INNER");
    dos(0x3A00, s, 0, 0, 0);
    CHECK(!carry() && !hd_exists(host, "NEWDIR/INNER"));
    s = kstr("C:\\NEWDIR");
    dos(0x3A00, s, 0, 0, 0);
    CHECK(!carry() && !hd_exists(host, "NEWDIR"));
    dos(0x3A00, s, 0, 0, 0);
    CHECK(carry() && ax() == 3);                                   /* gone: path not found */
    /* unknown function: carry + "invalid function", the call is reported */
    dos(0x7F00, 0, 0, 0, 0);
    CHECK(carry() && ax() == 1);
    /* AH=4Ch terminates the program */
    kf.p->cpu.r[CPU_EAX] = 0x4C00;
    CHECK_EQ(FX_CALL(&kf, "KERNEL", F_DOS3CALL), LOADER_STOP_REQUESTED);
    end_files();
}

/* ---- GetTempFileName -------------------------------------------------------- */

static void test_tempfilename(void) {
    uint16_t pfx, out;
    char n[64];
    printf("test_tempfilename\n");
    boot_files();
    pfx = kstr("CWF_LONGPREFIX");
    out = kseg(0x100);
    FX_CALL(&kf, "KERNEL", F_GETTEMPFILENAME, 'C', FARP(pfx, 0), 0, FARP(out, 0));
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK(ax() != 0);
    snprintf(n, sizeof n, "C:\\CWF%04X.TMP", ax());               /* prefix cut to 3, 4 hex digits */
    CHECK(!strcmp(kcstr(out, 0), n));
    CHECK(hd_exists(host, n + 3));                                  /* unique 0: the file is created */
    FX_CALL(&kf, "KERNEL", F_GETTEMPFILENAME, 'C', FARP(pfx, 0), 0x1234, FARP(out, 0));
    CHECK_EQ(ax(), 0x1234);
    CHECK(!strcmp(kcstr(out, 0), "C:\\CWF1234.TMP"));
    CHECK(!hd_exists(host, "CWF1234.TMP"));                         /* explicit number: name only */
    FX_CALL(&kf, "KERNEL", F_GETTEMPFILENAME, 0x80 | 'C', FARP(pfx, 0), 0, FARP(out, 0));
    snprintf(n, sizeof n, "CWF%04X.TMP", ax());
    CHECK(ax() != 0 && !hd_exists(host, n));                        /* bit 7: do not create */
    hd_mkdir(host, "TEMP");
    FX_CALL(&kf, "KERNEL", F_GETTEMPFILENAME, 0, FARP(pfx, 0), 0x0042, FARP(out, 0));
    CHECK(!strcmp(kcstr(out, 0), "C:\\TEMP\\CWF0042.TMP"));         /* drive 0 = current, TEMP dir if present */
    FX_CALL(&kf, "KERNEL", F_GETTEMPFILENAME, 'Q', FARP(pfx, 0), 0x0042, FARP(out, 0));
    CHECK_EQ(ax(), 0);                                              /* no such drive */
    end_files();
}

/* ---- profiles ----------------------------------------------------------------- */

static uint16_t wprof(unsigned ord, const char *app, const char *key, const char *val, const char *file) {
    uint16_t a = app ? kstr(app) : 0, k = key ? kstr(key) : 0, v = val ? kstr(val) : 0;
    if (file) {
        uint16_t f = kstr(file);
        FX_CALL(&kf, "KERNEL", ord, FARP(a, 0), FARP(k, 0), FARP(v, 0), FARP(f, 0));
    } else {
        FX_CALL(&kf, "KERNEL", ord, FARP(a, 0), FARP(k, 0), FARP(v, 0));
    }
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return ax();
}

static void test_profiles(void) {
    uint16_t a, k, d, f, buf;
    char got[256];
    printf("test_profiles\n");
    boot_files();
    CHECK_EQ(wprof(F_WRITEPROFILESTRING, "Creative Writer", "Volume", "7", NULL), 1);
    CHECK(hd_read(host, "WINDOWS/WIN.INI", got, sizeof got) > 0);   /* WIN.INI in the Windows directory */
    CHECK(strstr(got, "[Creative Writer]\nVolume=7\n") != NULL);
    a = kstr("creative writer");
    k = kstr("VOLUME");
    FX_CALL(&kf, "KERNEL", F_GETPROFILEINT, FARP(a, 0), FARP(k, 0), 99);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(ax(), 7);
    k = kstr("Missing");
    FX_CALL(&kf, "KERNEL", F_GETPROFILEINT, FARP(a, 0), FARP(k, 0), 99);
    CHECK_EQ(ax(), 99);                                             /* default when absent */
    d = kstr("none");
    buf = kseg(0x100);
    k = kstr("Volume");
    FX_CALL(&kf, "KERNEL", F_GETPROFILESTRING, FARP(a, 0), FARP(k, 0), FARP(d, 0), FARP(buf, 0), 0x40);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(ax(), 1);
    CHECK(!strcmp(kcstr(buf, 0), "7"));
    k = kstr("Other");
    FX_CALL(&kf, "KERNEL", F_GETPROFILESTRING, FARP(a, 0), FARP(k, 0), FARP(d, 0), FARP(buf, 0), 0x40);
    CHECK_EQ(ax(), 4);
    CHECK(!strcmp(kcstr(buf, 0), "none"));
    FX_CALL(&kf, "KERNEL", F_GETPROFILESTRING, FARP(a, 0), FARP(k, 0), FARP(d, 0), FARP(buf, 0), 3);
    CHECK(!strcmp(kcstr(buf, 0), "no"));                            /* truncated to size-1 */
    /* private profiles: a bare name lives in the Windows directory, a path is mapped */
    CHECK_EQ(wprof(F_WRITEPRIVATEPROFILESTRING, "Game", "Level", "12", "kids.ini"), 1);
    CHECK(hd_exists(host, "WINDOWS/KIDS.INI"));
    CHECK_EQ(wprof(F_WRITEPRIVATEPROFILESTRING, "Game", "Hero", "Zed", "C:\\DATA\\GAME.INI"), 1);
    hd_read(host, "DATA/GAME.INI", got, sizeof got);
    CHECK(strstr(got, "[Game]\nHero=Zed\n") != NULL);
    a = kstr("Game");
    k = kstr("Level");
    f = kstr("KIDS.INI");
    FX_CALL(&kf, "KERNEL", F_GETPRIVATEPROFILEINT, FARP(a, 0), FARP(k, 0), 5, FARP(f, 0));
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(ax(), 12);
    k = kstr("Hero");
    f = kstr("c:\\data\\game.ini");
    FX_CALL(&kf, "KERNEL", F_GETPRIVATEPROFILESTRING, FARP(a, 0), FARP(k, 0), FARP(d, 0), FARP(buf, 0), 0x40, FARP(f, 0));
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK(!strcmp(kcstr(buf, 0), "Zed"));
    /* delete a key with a NULL value */
    CHECK_EQ(wprof(F_WRITEPRIVATEPROFILESTRING, "Game", "Hero", NULL, "C:\\DATA\\GAME.INI"), 1);
    hd_read(host, "DATA/GAME.INI", got, sizeof got);
    CHECK(strstr(got, "Hero") == NULL);
    /* non-numeric values read as 0 */
    wprof(F_WRITEPROFILESTRING, "Creative Writer", "Name", "abc", NULL);
    a = kstr("Creative Writer");
    k = kstr("Name");
    FX_CALL(&kf, "KERNEL", F_GETPROFILEINT, FARP(a, 0), FARP(k, 0), 99);
    CHECK_EQ(ax(), 0);
    end_files();
}

static void test_directories(void) {
    uint16_t buf;
    printf("test_directories\n");
    boot_files();
    buf = kseg(0x100);
    FX_CALL(&kf, "KERNEL", F_GETWINDOWSDIRECTORY, FARP(buf, 0), 0x40);
    CHECK_EQ(ax(), 10);
    CHECK(!strcmp(kcstr(buf, 0), "C:\\WINDOWS"));
    CHECK_EQ(kf.sp_after, kf.sp_before);
    FX_CALL(&kf, "KERNEL", F_GETSYSTEMDIRECTORY, FARP(buf, 0), 0x40);
    CHECK(!strcmp(kcstr(buf, 0), "C:\\WINDOWS\\SYSTEM"));
    CHECK_EQ(ax(), 17);
    end_files();
}

static void test_access_resource(void) {
    uint16_t r, h, buf;
    char exe[600];
    FILE *f;
    printf("test_access_resource\n");
    {
        char *t = hd_make();
        snprintf(host, sizeof host, "%s", t);
    }
    snprintf(exe, sizeof exe, "%s/WRITER.EXE", host);
    f = fopen(exe, "wb");
    if (f) {
        char pad[0x80] = {0};
        fwrite(pad, 1, sizeof pad, f);
        fwrite("RESOURCE-DATA", 1, 13, f);
        fclose(f);
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.host_root['C' - 'A'] = host;
    cfg.exe_host_path = exe;
    kboot_ex(&cfg, 0, 0x800, 0x1000);
    fx_add_resource(&kf, 2, NULL, 1, NULL, 0x80, 13);
    FX_CALL(&kf, "KERNEL", F_FINDRESOURCE, kf.p->auto_ds, FARP(0, 1), FARP(0, 2));
    r = ax();
    CHECK(r != 0);
    FX_CALL(&kf, "KERNEL", F_ACCESSRESOURCE, kf.p->auto_ds, r);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    h = ax();
    CHECK(h != 0xFFFF);
    buf = kseg(0x40);
    CHECK_EQ(lread(h, buf, 0, 13), 13);                             /* positioned at the resource */
    CHECK(!memcmp(kmem(buf, 0, 13), "RESOURCE-DATA", 13));
    lclose(h);
    end_files();
}

static void test_handle_table_limit(void) {
    uint16_t h[80];
    unsigned n = 0, i;
    printf("test_handle_table_limit\n");
    boot_files();
    while (n < 80 && (h[n] = lopen("D:\\DATA\\HELLO.TXT", 0)) != 0xFFFF) n++;
    CHECK(n >= 20 && n < 80);                                       /* a bounded table, like DOS FILES= */
    for (i = 0; i < n; i++) CHECK_EQ(lclose(h[i]), 0);
    CHECK(lopen("D:\\DATA\\HELLO.TXT", 0) != 0xFFFF);              /* handles are reusable */
    end_files();
}

/* GetDriveType: C fixed, A removable, the CD drive "remote" (MSCDEX), others absent; hide_cd_drive makes the programs'
 * drive lists (the save-location page) show only the data drive while D: keeps working for files. */
static uint16_t drive_type(unsigned d) {
    FX_CALL(&kf, "KERNEL", 136, d);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    return ax();
}

static void test_drive_types(void) {
    printf("test_drive_types\n");
    boot_files();
    CHECK_EQ(drive_type(2), 3);                                    /* C: */
    CHECK_EQ(drive_type(3), 4);                                    /* D: the CD */
    CHECK_EQ(drive_type(5), 0);                                    /* F: absent */
    end_files();
    {
        char *r = hd_make();
        snprintf(host, sizeof host, "%s", r);
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.host_root['C' - 'A'] = host;
    cfg.host_root['D' - 'A'] = host;
    cfg.hide_cd_drive = true;
    kboot_ex(&cfg, 0, 0x800, 0x1000);
    CHECK_EQ(drive_type(2), 3);
    CHECK_EQ(drive_type(3), 0);                                    /* not listed ... */
    hd_mkdir(host, "DATA");
    hd_write(host, "DATA/HELLO.TXT", "Hello, DOS world!", 17);
    {
        uint16_t h = lopen("D:\\DATA\\HELLO.TXT", 0);               /* ... but its files are still reachable */
        CHECK(h != 0xFFFF);
        lclose(h);
    }
    end_files();
}

/* WinExec (KERNEL.166, called by KIDSWTCH's FSwitchToFspApp with SW_SHOW): the command line's program is resolved
 * to its host file and handed to the host; the result is the host's (>= 32 success); a program that does not
 * exist is error 2 and the host is not asked; with no host hook the call fails with 0 (out of memory/resources). */
static struct { int calls; char exe[512]; char dos[128]; char args[128]; unsigned show; unsigned ret; } g_exec;
static unsigned exec_hook(void *ctx, const char *exe, const char *dos, const char *args, unsigned show) {
    (void)ctx;
    g_exec.calls++;
    snprintf(g_exec.exe, sizeof g_exec.exe, "%s", exe);
    snprintf(g_exec.dos, sizeof g_exec.dos, "%s", dos);
    snprintf(g_exec.args, sizeof g_exec.args, "%s", args);
    g_exec.show = show;
    return g_exec.ret;
}

static void test_winexec(void) {
    uint16_t s;
    printf("test_winexec\n");
    boot_files();
    hd_mkdir(host, "MSKIDS");
    hd_write(host, "MSKIDS/WRITER.EXE", "MZ", 2);
    s = kstr("C:\\MSKIDS\\WRITER.EXE /u Ana");
    FX_CALL(&kf, "KERNEL", 166, FARP(s, 0), 5);
    CHECK_EQ(kf.sp_after, kf.sp_before);
    CHECK_EQ(ax(), 0);                                               /* no hook: cannot start another program */
    end_files();
    memset(&g_exec, 0, sizeof g_exec);
    g_exec.ret = 33;
    {
        char *r = hd_make();
        snprintf(host, sizeof host, "%s", r);
        hd_mkdir(host, "MSKIDS");
        hd_write(host, "MSKIDS/WRITER.EXE", "MZ", 2);
        memset(&cfg, 0, sizeof cfg);
        cfg.host_root['C' - 'A'] = host;
        cfg.exe_path = "C:\\MSKIDS\\ARTIST.EXE";
        cfg.cur_dir = "C:\\";
        cfg.winexec = exec_hook;
        kboot_ex(&cfg, 0, 0x800, 0x1000);
    }
    s = kstr("C:\\MSKIDS\\WRITER.EXE /u Ana");
    FX_CALL(&kf, "KERNEL", 166, FARP(s, 0), 5);
    CHECK_EQ(ax(), 33);
    CHECK_EQ(g_exec.calls, 1);
    CHECK(strstr(g_exec.exe, "WRITER.EXE") != NULL && g_exec.exe[0] == '/');
    CHECK(!strcmp(g_exec.dos, "C:\\MSKIDS\\WRITER.EXE"));
    CHECK(!strcmp(g_exec.args, "/u Ana"));
    CHECK_EQ(g_exec.show, 5);
    s = kstr("C:\\MSKIDS\\NOPE.EXE");
    FX_CALL(&kf, "KERNEL", 166, FARP(s, 0), 1);
    CHECK_EQ(ax(), 2);                                               /* file not found, the host is not asked */
    CHECK_EQ(g_exec.calls, 1);
    end_files();
}

int main(void) {
    test_dos_trace();
    test_extended_error();
    test_get_dta();
    test_read_file();
    test_seek();
    test_create_write();
    test_huge_io();
    test_openfile();
    test_int21();
    test_find();
    test_volume_label();
    test_dos3call();
    test_dos_filetime();
    test_tempfilename();
    test_profiles();
    test_directories();
    test_access_resource();
    test_handle_table_limit();
    test_drive_types();
    test_winexec();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
