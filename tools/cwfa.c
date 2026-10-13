#define _XOPEN_SOURCE 700
/* cwfa: runs an unmodified Win16 NE program (Creative Writer, Fine Artist) on
 * the interpreter + HLE. The program comes from the user's own CD image.
 *
 *   cwfa --exe PATH --cd DIR [--install-dir DIR | --c-root DIR | --no-install] [--headless] [--frames N]
 *        [--scale N] [--max-steps N] [--quiet] [--trace] [--trace-args] [--sequence]
 *        [--exe-path DOSPATH] [--screenshot PATH] [--inject EVENTS] [--audio sdl|null|off] [--probe MODULE.ORDINAL:ARGBYTES]...
 *
 * Drive C: is an installed tree staged from the CD per SETUP\SETUP.INF (src/install.h): C:\MSKIDS with the
 * program, its .PIC/.SND/.DCT files and the shared data, KIDSWTCH.DLL in C:\WINDOWS\SYSTEM. The default is a
 * temporary one removed at exit; --install-dir DIR stages (and keeps) it in DIR, --c-root DIR uses DIR as it is
 * (nothing staged), --no-install uses an empty temporary one. Libraries are loaded from the module path:
 * the program's directory, C:\, C:\WINDOWS\SYSTEM, C:\WINDOWS and D:\SETUP.
 *
 * --audio chooses where the sound goes: sdl (default) opens the SDL2 audio device (the dummy driver with --headless)
 * and falls back to null when there is none; null mixes into nothing, deterministically, following the program's
 * clock; off reports no wave devices at all.
 *
 * --inject EVENTS delivers scripted input, one step per idle frame; steps are separated by ';' (see
 * src/inject.h): wait:N, key:[ctrl+|alt+|shift+]NAME, keydown:/keyup:, type:TEXT, move:X,Y, click:X,Y,
 * dblclick:X,Y, rclick:X,Y, down:X,Y, up:X,Y, quit, shot:PATH.
 *
 * --probe is a characterization aid: that import returns 0 and pops ARGBYTES
 * bytes of Pascal arguments instead of stopping the run. With the suffix
 * ":strcpy" it first copies the first string argument over the second (a
 * stand-in for AnsiToOem/OemToAnsi) and returns 1; ":ret=N" returns N instead
 * of 0 and ":inc=N" returns a counter that grows by N per call (a clock).
 *
 * Exit status: 0 the run ended on request (window closed, frame limit) or at a
 * HLT, 1 it stopped on an unimplemented API call or a CPU fault, 2 bad usage or
 * a file that cannot be loaded. */
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_sdl.h"
#include "diag.h"
#include "fb.h"
#include "gdi.h"
#include "appicon.h"
#include "inject.h"
#include "install.h"
#include "kernel.h"
#include "kwaj.h"
#include "mmsystem.h"
#include "present_sdl.h"
#include "toolhelp.h"
#include "ttf.h"
#include "user.h"
#include "ver.h"
#include "win87em.h"

#define MAX_PROBES 32

typedef struct {
    char module[16];
    unsigned ordinal, arg_bytes;
    bool copy;
    uint32_t ret, inc;
} probe;

typedef struct {
    const char *exe, *cd, *cd_label, *c_root, *install_dir, *screenshot, *exe_path, *inject, *refresh_shot, *font_dir, *print_dir, *es_prog, *export_icon, *export_icon_size;
    unsigned long refresh_shot_ms;
    unsigned ip_seg;                     /* --trace-ip SEG:LO-HI[:N]: NE segment (1-based, 0 = any), hex offsets, instruction budget */
    bool ip_on;
    const char *debug_live;              /* --debug-live FILE: snapshot on SIGUSR1 / Ctrl+\\, on a stall and at exit */
    unsigned watchdog_s;                 /* --watchdog SECONDS (0: off, default 15): report a program that makes no API call */
    bool watchdog_set;
    unsigned long ip_from_ms;            /* --trace-ip-from MS: start logging when the program clock reaches MS */
    unsigned long ip_lo, ip_hi, ip_max;
    probe probes[MAX_PROBES];
    unsigned nprobes;
    bool headless, quiet, trace, sequence, no_install, no_ttf, fullscreen, integer_scale, fixed_size, hide_cd_drive, trace_msgs, no_fs_button, trace_args, temp_c;
    const char *audio;
    const char *child_opt[16];           /* --child-opt ARG: extra arguments for a program started by WinExec */
    unsigned nchild_opt;
    unsigned long frames;
    int scale;
    unsigned long long max_steps;
} options;

static int usage(const char *why) {
    if (why) fprintf(stderr, "cwfa: %s\n", why);
    fprintf(stderr,
            "usage: cwfa --exe PATH --cd DIR [--install-dir DIR | --c-root DIR | --no-install] [--headless] [--frames N] [--scale N]\n"
            "            (Spanish disks: --es-writer DIR | --es-artist DIR instead of --exe and --cd; DIR = INSTALAR.LST directory or DISCOn set)\n"
            "            [--max-steps N] [--quiet] [--trace] [--sequence]\n"
            "            [--exe-path DOSPATH] [--screenshot PATH] [--inject EVENTS] [--audio sdl|null|off] [--probe MODULE.ORD:ARGBYTES[:strcpy|:ret=N|:inc=N]]...\n"
            "  --exe PATH     the Win16 program (WRITER.EXE or ARTIST.EXE)\n"
            "  --cd DIR       directory that holds the CD contents (drive D:)\n"
            "  --install-dir DIR  stage an installed C: drive from the CD into DIR (kept after the run)\n"
            "  --c-root DIR   use DIR as drive C: exactly as it is (nothing is staged)\n"
            "  --no-install   drive C: is an empty temporary directory\n"
            "  --temp-c       with --c-root: DIR is a temporary drive C:, removed when the last program started from it has exited\n"
            "                 (default: a temporary C: staged from the CD like --install-dir, removed at exit)\n"
            "  --cd-label L   volume label of the CD drive D: (FindFirst with attribute 08h; default CWFA110__01 with --cd, or $CWFA_CD_LABEL; "" = none)\n"
            "  --headless     no display: SDL's dummy video driver\n"
            "  --frames N     stop after N idle/poll frames of the message loop\n"
            "  --data-dir DIR same as --install-dir: DIR is drive C: (users, saved documents, WIN.INI)\n"
            "  --hide-cd-drive  the programs' drive lists show only drive C: (the CD drive D: stays usable for files). The English\n"
            "                 programs look for their CD on D: at run time, so use it only if nothing asks for the CD\n"
            "  --trace-ip S:LO-HI[:N]  log registers before each instruction of the program's NE segment S (1-based; 0 = any segment) at hex offsets\n"
            "                 LO..HI, at most N (default 20000) lines: instruction-level debugging\n"
            "  --watchdog SEC report on stderr when the program makes no API call for SEC seconds (default 15, 0 = off)\n"
            "  --debug-live FILE  write a snapshot (windows, timers, queues, DCs, GDI objects, last 256 API calls) to FILE on SIGUSR1,\n"
            "                 Ctrl+\\ (SIGQUIT), when the watchdog fires and at exit: send it with a bug report\n"
            "  --trace-ip-from MS  with --trace-ip: start logging when the program clock reaches MS milliseconds\n"
            "  --scale N      initial window scale (default 1); the window can be resized, the picture keeps its 4:3 shape\n"
            "  --fullscreen   start in desktop fullscreen with black bars (F11 or Alt+Enter toggles it at any time, not sent to the program)\n"
            "  --integer-scale  only whole-number picture scales in a resized/fullscreen window (default: fill as far as 4:3 allows)\n"
            "  --no-fs-button no in-window fullscreen button in the top-right corner (or CWFA_NO_FS_BUTTON=1)\n"
            "  --fixed-size   the window cannot be resized\n"
            "  --max-steps N  instruction budget (default 4000000000)\n"
            "  --quiet        do not log USER calls   --trace  log every API call   --trace-args  --trace plus each call's argument words\n"
            "  --trace-msgs   log every message a program window gets\n"
            "  --child-opt A  an extra argument for a program started with WinExec (the program switcher); repeatable\n"
            "  --sequence     print the whole API call sequence at the end\n"
            "  --exe-path P   DOS path the program reports for itself (default C:\\CWFA\\NAME.EXE); it looks\n"
            "                 for MSKIDS.INF and its .PIC beside it, e.g. D:\\SETUP\\WRITER.EXE on the CD\n"
            "  --refresh-shot MS:PATH  save the framebuffer at the first clock read after MS ms (splash screens)\n"
            "  --screenshot PATH  write the final framebuffer as a PPM (P6) with the current palette\n"
            "  --inject EVT   scripted input, one ';'-separated step per idle frame: wait:N key:[ctrl+]NAME\n"
            "                 keydown:K keyup:K type:TEXT move:X,Y click:X,Y dblclick:X,Y rclick:X,Y down:X,Y up:X,Y quit shot:PATH\n"
            "  --export-icon FILE.png  write the program's own icon (32x32 PNG, for a launcher) and exit\n"
            "  --export-icon-size N    with --export-icon: scale the icon up by a whole factor to about N pixels (64, 128, 256)\n"
            "  --font-dir DIR TrueType faces (Liberation Sans/Serif/Mono *-Regular/Bold.ttf) for Arial, MS Sans Serif, Times, Courier...;\n"
            "                 default: $CWFA_FONT_DIR, <exe dir>/../fonts, <exe dir>/fonts, third_party/fonts, then the system font directories\n"
            "  --print-dir DIR where the virtual \"PDF Printer\" writes each printed document (default: $CWFA_PRINT_DIR, else <your Documents folder>/Creative Writer, e.g. ~/Documentos/Creative Writer)\n"
            "  --no-ttf       keep the built-in 5x7 glyphs for faces without a raster font\n"
            "  --audio MODE   sdl (default): SDL2 audio device, dummy driver with --headless, null when unavailable;\n"
            "                 null: a silent sink that follows the program clock; off: no wave devices\n"
            "  --probe M.N:B  answer an import the HLE does not implement with 0 instead of stopping (characterization only);\n"
            "                 M.N:B:strcpy copies string arg 1 to arg 2 and returns 1; M.N:B:ret=N returns N; M.N:B:inc=N counts up by N\n");
    return 2;
}

static bool parse_ulong(const char *s, unsigned long long *out) {
    char *end;
    errno = 0;
    if (!*s || !isdigit((unsigned char)*s)) return false;
    *out = strtoull(s, &end, 10);
    return !*end && !errno;
}

static bool parse_probe(const char *s, probe *pr) {
    const char *dot = strchr(s, '.'), *colon = dot ? strchr(dot, ':') : NULL;
    unsigned long long ord, bytes;
    char num[16];
    size_t n;
    const char *tail;
    if (!dot || !colon || dot == s || (size_t)(dot - s) >= sizeof pr->module) return false;
    tail = strchr(colon + 1, ':');
    pr->copy = false;
    pr->ret = pr->inc = 0;
    if (tail) {
        unsigned long long rv;
        if (!strcmp(tail, ":strcpy")) pr->copy = true;
        else if (!strncmp(tail, ":ret=", 5) && parse_ulong(tail + 5, &rv) && rv <= 0xFFFFFFFFull) pr->ret = (uint32_t)rv;
        else if (!strncmp(tail, ":inc=", 5) && parse_ulong(tail + 5, &rv) && rv <= 0xFFFFFFFFull) pr->inc = (uint32_t)rv;
        else return false;
    }
    n = (size_t)(colon - dot - 1);
    if (n == 0 || n >= sizeof num) return false;
    memcpy(num, dot + 1, n);
    num[n] = 0;
    {
        char b[16];
        size_t bl = tail ? (size_t)(tail - colon - 1) : strlen(colon + 1);
        if (bl >= sizeof b) return false;
        memcpy(b, colon + 1, bl);
        b[bl] = 0;
        if (!parse_ulong(b, &bytes)) return false;
    }
    if (!parse_ulong(num, &ord) || ord > 0xFFFF || bytes > 64) return false;
    memcpy(pr->module, s, (size_t)(dot - s));
    pr->module[dot - s] = 0;
    pr->ordinal = (unsigned)ord;
    pr->arg_bytes = (unsigned)bytes;
    return true;
}

static int parse(int argc, char **argv, options *o) {
    int i;
    memset(o, 0, sizeof *o);
    o->scale = 1;
    o->max_steps = 4000000000ull;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        unsigned long long v;
        if (!strcmp(a, "--headless")) o->headless = true;
        else if (!strcmp(a, "--quiet")) o->quiet = true;
        else if (!strcmp(a, "--trace")) o->trace = true;
        else if (!strcmp(a, "--sequence")) o->sequence = true;
        else if (!strcmp(a, "--refresh-shot")) {                  /* MS:PATH, from a clock read of a program with no message loop */
            char *colon, num[24];
            unsigned long long ms;
            if (i + 1 >= argc) return usage("missing value after option");
            o->refresh_shot = argv[++i];
            colon = strchr(o->refresh_shot, ':');
            if (!colon || colon == o->refresh_shot || colon - o->refresh_shot >= (long)sizeof num || colon[1] == 0)
                return usage("--refresh-shot needs MS:PATH");
            snprintf(num, sizeof num, "%.*s", (int)(colon - o->refresh_shot), o->refresh_shot);
            if (!parse_ulong(num, &ms)) return usage("--refresh-shot needs MS:PATH");
            o->refresh_shot_ms = (unsigned long)ms;
            o->refresh_shot = colon + 1;
        }
        else if (!strcmp(a, "--no-install")) o->no_install = true;
        else if (!strcmp(a, "--temp-c")) o->temp_c = true;
        else if (!strcmp(a, "--no-ttf")) o->no_ttf = true;
        else if (!strcmp(a, "--fullscreen")) o->fullscreen = true;
        else if (!strcmp(a, "--no-fs-button")) o->no_fs_button = true;
        else if (!strcmp(a, "--hide-cd-drive")) o->hide_cd_drive = true;
        else if (!strcmp(a, "--trace-msgs")) o->trace_msgs = true;
        else if (!strcmp(a, "--trace-args")) o->trace = o->trace_args = true;
        else if (!strcmp(a, "--child-opt")) {
            if (i + 1 >= argc || o->nchild_opt == 16) return usage("--child-opt needs an argument (at most 16)");
            o->child_opt[o->nchild_opt++] = argv[++i];
        }
        else if (!strcmp(a, "--debug-live")) {
            if (i + 1 >= argc) return usage("--debug-live needs a file name");
            o->debug_live = argv[++i];
        }
        else if (!strcmp(a, "--watchdog")) {
            unsigned long long sec;
            if (i + 1 >= argc || !parse_ulong(argv[++i], &sec) || sec > 86400) return usage("--watchdog needs seconds (0 = off)");
            o->watchdog_s = (unsigned)sec;
            o->watchdog_set = true;
        }
        else if (!strcmp(a, "--trace-ip-from")) {
            unsigned long long ms;
            if (i + 1 >= argc || !parse_ulong(argv[++i], &ms)) return usage("--trace-ip-from needs milliseconds");
            o->ip_from_ms = (unsigned long)ms;
        }
        else if (!strcmp(a, "--trace-ip")) {                      /* SEG:LO-HI[:N] */
            unsigned long lo, hi, max = 20000;
            unsigned seg;
            int n = 0;
            if (i + 1 >= argc) return usage("--trace-ip needs SEG:LO-HI[:N]");
            if (sscanf(argv[++i], "%u:%lx-%lx:%lu%n", &seg, &lo, &hi, &max, &n) < 3) return usage("--trace-ip needs SEG:LO-HI[:N] (hex offsets)");
            o->ip_seg = seg;
            o->ip_on = true;
            o->ip_lo = lo;
            o->ip_hi = hi;
            o->ip_max = max;
        }
        else if (!strcmp(a, "--integer-scale")) o->integer_scale = true;
        else if (!strcmp(a, "--fixed-size")) o->fixed_size = true;
        else if (!strcmp(a, "--probe")) {
            if (i + 1 >= argc) return usage("missing value after option");
            if (o->nprobes == MAX_PROBES || !parse_probe(argv[++i], &o->probes[o->nprobes]))
                return usage("--probe needs MODULE.ORDINAL:ARGBYTES[:strcpy|:ret=N]");
            o->nprobes++;
        } else if (!strcmp(a, "--exe") || !strcmp(a, "--cd") || !strcmp(a, "--c-root") || !strcmp(a, "--cd-label") || !strcmp(a, "--install-dir") || !strcmp(a, "--data-dir") || !strcmp(a, "--frames") ||
                 !strcmp(a, "--scale") || !strcmp(a, "--max-steps") || !strcmp(a, "--screenshot") ||
                 !strcmp(a, "--exe-path") || !strcmp(a, "--audio") || !strcmp(a, "--font-dir") || !strcmp(a, "--print-dir") || !strcmp(a, "--export-icon") || !strcmp(a, "--export-icon-size") ||
                 !strcmp(a, "--es-writer") || !strcmp(a, "--es-artist") || !strcmp(a, "--inject")) {
            const char *val;
            if (i + 1 >= argc) return usage("missing value after option");
            val = argv[++i];
            if (!strcmp(a, "--exe")) o->exe = val;
            else if (!strcmp(a, "--cd")) o->cd = val;
            else if (!strcmp(a, "--c-root")) o->c_root = val;
            else if (!strcmp(a, "--cd-label")) o->cd_label = val;
            else if (!strcmp(a, "--install-dir") || !strcmp(a, "--data-dir")) o->install_dir = val;
            else if (!strcmp(a, "--screenshot")) o->screenshot = val;
            else if (!strcmp(a, "--exe-path")) o->exe_path = val;
            else if (!strcmp(a, "--font-dir")) o->font_dir = val;
            else if (!strcmp(a, "--print-dir")) o->print_dir = val;
            else if (!strcmp(a, "--export-icon")) o->export_icon = val;
            else if (!strcmp(a, "--export-icon-size")) o->export_icon_size = val;
            else if (!strcmp(a, "--es-writer") || !strcmp(a, "--es-artist")) {   /* Spanish setup media: stage from KWAJ files */
                o->cd = val;
                o->es_prog = a[5] == 'w' ? "WRITER" : "ARTIST";
            }
            else if (!strcmp(a, "--audio")) {
                if (strcmp(val, "sdl") && strcmp(val, "null") && strcmp(val, "off")) return usage("--audio needs sdl, null or off");
                o->audio = val;
            }
            else if (!strcmp(a, "--inject")) {
                char err[96];
                inject_t *t = inject_parse(val, err, sizeof err);
                if (!t) return usage(err);
                inject_free(t);
                o->inject = val;
            }
            else if (!strcmp(a, "--frames")) {
                if (!parse_ulong(val, &v)) return usage("--frames needs a number");
                o->frames = (unsigned long)v;
            } else if (!strcmp(a, "--scale")) {
                if (!parse_ulong(val, &v) || v < 1 || v > 8) return usage("--scale needs a number from 1 to 8");
                o->scale = (int)v;
            } else {
                if (!parse_ulong(val, &v)) return usage("--max-steps needs a number");
                o->max_steps = v;
            }
        } else {
            return usage("unknown option");
        }
    }
    if (!o->exe && !o->es_prog) return usage("--exe (or --es-writer/--es-artist) is required");
    if ((o->c_root != NULL) + (o->install_dir != NULL) + o->no_install > 1)
        return usage("--c-root, --install-dir and --no-install exclude each other");
    return 0;
}

/* True when the file starts with a KWAJ/SZDD signature (a compressed program from the Spanish media). */
static bool file_is_compressed(const char *path) {
    uint8_t b[8];
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(b, 1, sizeof b, f) : 0;
    if (f) fclose(f);
    return kwaj_is_compressed(b, n);
}

static int rm_entry(const char *path, const struct stat *sb, int type, struct FTW *f) {
    (void)sb; (void)type; (void)f;
    return remove(path);
}

typedef struct {
    present_t *pr;
    inject_t *in;
    user_t *u;
    const char *shot;      /* --refresh-shot: written once, at the first clock-read refresh past shot_ms */
    unsigned long shot_ms;
} pump_ref;

static cpu_status probe_fn(hle_call *c) {
    probe *pr = c->ctx;
    if (pr->copy) {
        hle_args a;
        uint16_t ss, so, ds, dof;
        char *src, *dst;
        hle_args_init(&a, c);
        hle_far(&a, &ss, &so);
        hle_far(&a, &ds, &dof);
        if (a.st) return a.st;
        src = hle_ptr(c, ss, so, 1);
        dst = hle_ptr(c, ds, dof, 1);
        if (!src || !dst) return CPU_E_SEL;
        for (;;) {
            if (!(src = hle_ptr(c, ss, so, 1)) || !(dst = hle_ptr(c, ds, dof, 1))) return CPU_E_SEL;
            if (!(*dst = *src)) break;
            so++;
            dof++;
        }
        hle_ret32(c, 1);
        return CPU_OK;
    }
    hle_ret32(c, pr->ret);
    pr->ret += pr->inc;
    return CPU_OK;
}

/* --trace of calls into libraries loaded from files: a hook on every exported entry logs the call and lets the
 * instruction run (a hook that does not move CS:IP executes it normally). */
typedef struct {
    const loader_module *m;
    uint16_t ordinal;
    const char *name;
    loader_proc *p;
    FILE *log;
} dll_hook;

static cpu_status dll_hook_fn(cpu_t *c, void *ctx) {
    const dll_hook *d = ctx;
    uint16_t ip = 0, cs = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0, sp = (uint16_t)c->r[CPU_ESP];
    unsigned seg = 0, i;
    cpu_read16(c, c->seg[CPU_SS], sp, &ip);
    cpu_read16(c, c->seg[CPU_SS], (uint16_t)(sp + 2), &cs);
    cpu_read16(c, c->seg[CPU_SS], (uint16_t)(sp + 4), &a0);
    cpu_read16(c, c->seg[CPU_SS], (uint16_t)(sp + 6), &a1);
    cpu_read16(c, c->seg[CPU_SS], (uint16_t)(sp + 8), &a2);
    cpu_read16(c, c->seg[CPU_SS], (uint16_t)(sp + 10), &a3);
    for (i = 0; i < d->p->ne->seg_count; i++)
        if (d->p->seg_sel[i] == cs) seg = i + 1;
    fprintf(d->log, "dll: %s.%s (%u) from %u:%04x  stack words %04x %04x %04x %04x\n", d->m->name, d->name,
            d->ordinal, seg, ip, a0, a1, a2, a3);
    return CPU_OK;
}

/* WinExec: the runtime runs one program per process, so another program (the switcher's Creative Writer or Fine
 * Artist) is a new cwfa process on the same drive C: and CD, with the display options of this one. A program that
 * is already running in another process is not found: a second copy starts (the programs share their data files, as
 * two copies on one DOS machine would). The command line the guest passes after the program name is NOT forwarded
 * (the programs start by themselves; only this run's options and --child-opt reach the child). Returns what WinExec
 * returns: 33 on success, 8 (out of memory) otherwise. */
typedef struct { const options *o; const char *croot; bool *spawned; const char *lockpath; int lockfd; } spawn_ctx;

/* A temporary C: outlives the process that made it when programs were started from it. Every process of the family
 * holds a shared flock on <C:>.lock (the descriptor is inherited by each WinExec child), so the lock is free exactly
 * when the last one has exited; hand_over_temp_c() leaves a detached janitor that waits for that and removes the tree. */
static void hand_over_temp_c(spawn_ctx *sc) {
    pid_t pid;
    int fd;
    fflush(NULL);
    if ((pid = fork()) != 0) return;
    setsid();
    if (sc->lockfd >= 0) close(sc->lockfd);                    /* the janitor must not hold the lock it waits for */
    for (fd = 0; fd < 3; fd++) close(fd);                      /* a caller reading our output must see end of file */
    if ((fd = open(sc->lockpath, O_RDWR)) >= 0 && flock(fd, LOCK_EX) == 0) {
        nftw(sc->croot, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
        unlink(sc->lockpath);
    }
    _exit(0);
}

static unsigned spawn_program(void *ctx, const char *host_exe, const char *dos_exe, const char *args, unsigned show) {
    spawn_ctx *sc = ctx;
    const options *o = sc->o;
    const char *av[48];
    char frames[32], scale[16], steps[32];
    unsigned n = 0, i;
    pid_t pid;
    (void)args;                               /* not forwarded: see the comment above */
    (void)show;
    if (sc->lockpath && sc->lockfd < 0) {     /* from now on the temporary C: is shared with the children */
        sc->lockfd = open(sc->lockpath, O_RDWR | O_CREAT, 0600);
        if (sc->lockfd >= 0) flock(sc->lockfd, LOCK_SH);
    }
    signal(SIGCHLD, SIG_IGN);                 /* the children are never waited for: do not keep them as zombies */
    av[n++] = "cwfa";
    av[n++] = "--exe"; av[n++] = host_exe;
    av[n++] = "--c-root"; av[n++] = sc->croot;
    av[n++] = "--exe-path"; av[n++] = dos_exe;       /* the program looks for MSKIDS.INF and its .PIC beside itself */
    if (o->cd && !o->es_prog) { av[n++] = "--cd"; av[n++] = o->cd; }
    if (o->headless) av[n++] = "--headless";
    if (o->quiet) av[n++] = "--quiet";
    if (o->no_ttf) av[n++] = "--no-ttf";
    if (o->fullscreen) av[n++] = "--fullscreen";
    if (o->integer_scale) av[n++] = "--integer-scale";
    if (o->fixed_size) av[n++] = "--fixed-size";
    if (o->no_fs_button) av[n++] = "--no-fs-button";
    if (o->hide_cd_drive) av[n++] = "--hide-cd-drive";
    if (o->audio) { av[n++] = "--audio"; av[n++] = o->audio; }
    if (o->font_dir) { av[n++] = "--font-dir"; av[n++] = o->font_dir; }
    if (o->print_dir) { av[n++] = "--print-dir"; av[n++] = o->print_dir; }
    snprintf(scale, sizeof scale, "%d", o->scale);
    av[n++] = "--scale"; av[n++] = scale;
    if (o->frames) { snprintf(frames, sizeof frames, "%lu", o->frames); av[n++] = "--frames"; av[n++] = frames; }
    snprintf(steps, sizeof steps, "%llu", o->max_steps);
    av[n++] = "--max-steps"; av[n++] = steps;
    for (i = 0; i < o->nchild_opt && n < 46; i++) av[n++] = o->child_opt[i];
    av[n] = NULL;
    fflush(NULL);
    pid = fork();
    if (pid < 0) return 8;
    if (pid == 0) {
        setsid();
        execv("/proc/self/exe", (char *const *)av);
        _exit(127);
    }
    *sc->spawned = true;                      /* a temporary C: must outlive this process */
    if (!o->quiet) fprintf(stderr, "cwfa: WinExec started %s as process %ld\n", host_exe, (long)pid);
    return 33;
}

static void trace_libraries(loader_proc *p, FILE *log) {
    size_t i, k;
    for (i = 1; i < p->nmods; i++) {
        const loader_module *m = p->mods[i];
        for (k = 0; k < m->ne->resident_count + m->ne->nonres_count; k++) {
            const ne_name *nm = k < m->ne->resident_count ? &m->ne->resident_names[k]
                                                           : &m->ne->nonres_names[k - m->ne->resident_count];
            dll_hook *d;
            uint16_t sel, off;
            if (!nm->ordinal || !loader_module_proc(m, NULL, nm->ordinal, &sel, &off)) continue;
            if (!(d = calloc(1, sizeof *d))) return;       /* freed at exit with the process */
            d->m = m;
            d->ordinal = nm->ordinal;
            d->name = nm->text;
            d->p = p;
            d->log = log;
            cpu_add_hook(&p->cpu, sel, off, dll_hook_fn, d);
        }
    }
}

typedef struct { uint16_t sel; unsigned seg; uint32_t lo, hi; unsigned long left; const loader_proc *p; user_t *u; unsigned long from_ms; } ip_trace;

static cpu_status ip_trace_fn(cpu_t *c, void *ctx) {
    ip_trace *t = ctx;
    unsigned seg = t->seg;
    if (!t->left || c->eip < t->lo || c->eip > t->hi) return CPU_OK;
    if (t->from_ms && user_now(t->u) < t->from_ms) return CPU_OK;
    if (seg) {
        if (c->seg[CPU_CS] != t->sel) return CPU_OK;
    } else {                                      /* SEG 0 = any segment of the program (selector = 8 * segment + 7) */
        seg = (unsigned)(c->seg[CPU_CS] >> 3);
        if (seg < 1 || seg > t->p->ne->seg_count || t->p->seg_sel[seg - 1] != c->seg[CPU_CS]) return CPU_OK;
    }
    t->left--;
    fprintf(stderr, "ip: %u:%04x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x bp=%04x sp=%04x ds=%04x es=%04x fl=%04x\n", seg,
            (unsigned)c->eip, (unsigned)(c->r[CPU_EAX] & 0xFFFF), (unsigned)(c->r[CPU_EBX] & 0xFFFF), (unsigned)(c->r[CPU_ECX] & 0xFFFF),
            (unsigned)(c->r[CPU_EDX] & 0xFFFF), (unsigned)(c->r[CPU_ESI] & 0xFFFF), (unsigned)(c->r[CPU_EDI] & 0xFFFF),
            (unsigned)(c->r[CPU_EBP] & 0xFFFF), (unsigned)(c->r[CPU_ESP] & 0xFFFF), (unsigned)c->seg[CPU_DS], (unsigned)c->seg[CPU_ES],
            (unsigned)(c->eflags & 0xFFFF));
    return CPU_OK;
}

/* ---- freeze diagnostics: watchdog thread, signals and the --debug-live snapshot ------------------------------ */

typedef struct {
    hle_t *h;
    user_t *u;
    gdi_t *g;
    audio_t *a;
    loader_proc *p;
    const char *file;
    unsigned stall_ms;
    volatile int where;       /* what the main thread is doing: 0 guest code, 1 message pump, 2 clock-read refresh */
    volatile int stop;
    pthread_t th;
    bool started;
} live_t;

static volatile sig_atomic_t g_dump_req;
static void on_dump_signal(int sig) { (void)sig; g_dump_req = 1; }

static void live_extra(void *ctx, FILE *f) {
    live_t *L = ctx;
    unsigned objs = 0, dcs = 0;
    static const char *const where[] = {"guest code", "message pump", "clock-read refresh"};
    if (L->g) gdi_stats(L->g, &objs, &dcs);
    fprintf(f, "gdi_objects: %u (device contexts %u)\n", objs, dcs);
    if (L->a) fprintf(f, "audio_frames_mixed: %llu\n", (unsigned long long)audio_frames_mixed(L->a));
    fprintf(f, "main_thread_in: %s\n", where[L->where >= 0 && L->where <= 2 ? L->where : 0]);
    if (L->p) fprintf(f, "cpu: cs:ip=%04x:%04x ss:sp=%04x:%04x\n", L->p->cpu.seg[CPU_CS], (unsigned)(uint16_t)L->p->cpu.eip,
                      L->p->cpu.seg[CPU_SS], (unsigned)(L->p->cpu.r[CPU_ESP] & 0xFFFF));
}

static void live_dump(live_t *L, const char *reason, bool to_stderr) {
    diag_sources s;
    FILE *f = to_stderr || !L->file ? stderr : fopen(L->file, "a");
    if (!f) return;
    memset(&s, 0, sizeof s);
    s.hle = L->h;
    s.wm = user_wm(L->u);
    s.now_ms = user_now(L->u);
    s.api_calls = L->h->ncalls;
    s.extra = live_extra;
    s.extra_ctx = L;
    diag_dump(f, &s, reason);
    if (f != stderr) fclose(f);
}

static void *live_thread(void *arg) {
    live_t *L = arg;
    diag_watch w;
    sigset_t set;
    diag_watch_init(&w);
    sigemptyset(&set);                      /* SDL blocks SIGUSR1 in its threads: this one takes the dump signals */
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGQUIT);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    while (!L->stop) {
        uint32_t stalled = 0;
        struct timespec ts = {0, 250 * 1000000L};
        nanosleep(&ts, NULL);
        if (g_dump_req) {
            g_dump_req = 0;
            live_dump(L, "signal (SIGUSR1 / Ctrl+\\)", false);
            if (L->file) fprintf(stderr, "cwfa: debug snapshot written to %s\n", L->file);
        }
        if (L->stall_ms && diag_watch_poll(&w, L->h->ncalls, user_now(L->u), L->stall_ms, &stalled)) {
            hle_record last;
            const char *where = L->where == 1 ? "waiting in the message pump" : L->where == 2 ? "in a clock-read refresh" : "running guest code";
            if (!hle_recent(L->h, &last, 1)) memset(&last, 0, sizeof last);
            fprintf(stderr, "cwfa: watchdog: no API call for %u s (main thread %s; last call %s.%u %s from %04x:%04x\n",
                    (unsigned)(stalled / 1000), where, last.module, last.ordinal, last.name, last.caller_cs, last.caller_ip);
            if (L->file) {
                live_dump(L, "watchdog: no API call", false);
                fprintf(stderr, "cwfa: debug snapshot written to %s\n", L->file);
            }
        }
    }
    return NULL;
}

static live_t *g_live;

static bool pump(void *ctx, bool wait) {
    pump_ref *r = ctx;
    bool more;
    if (g_live) g_live->where = 1;
    if (r->in) inject_run(r->in, user_wm(r->u), user_fb(r->u));
    more = present_pump(r->pr, wait) && !(r->in && inject_stopped(r->in));
    if (g_live) g_live->where = 0;
    return more;
}

static bool font_host_path(void *ctx, const char *dos, char *host, size_t cap) { return kernel_host_path(ctx, dos, host, cap); }

static void refresh(void *ctx) {
    pump_ref *r = ctx;
    if (g_live) g_live->where = 2;
    if (r->pr) present_refresh(r->pr);
    if (g_live) g_live->where = 0;
    if (r->shot && r->u && user_now(r->u) >= r->shot_ms) {
        fb_save_ppm(user_fb(r->u), r->shot);
        r->shot = NULL;
    }
}

static void print_call(const hle_record *r, const loader_proc *p, size_t i) {
    unsigned s, seg = 0;
    for (s = 0; s < p->ne->seg_count; s++)
        if (p->seg_sel[s] == r->caller_cs) seg = s + 1;
    fprintf(stderr, "  %5zu %s.%u %s%s  from %u:%04x\n", i + 1, r->module, r->ordinal, r->name,
            r->implemented ? "" : "  <== NOT IMPLEMENTED", seg, r->caller_ip);
}

static void report(const hle_t *h, const loader_proc *p, user_t *u) {
    size_t i, n = h->nrec;
    fprintf(stderr, "stopped: %s", loader_strstop(p->stop));
    if (p->stop == LOADER_STOP_FAULT) fprintf(stderr, " (%s)", cpu_strstatus(p->fault));
    fprintf(stderr, " at %04x:%04x after %llu API calls\n", p->cpu.seg[CPU_CS], (uint16_t)p->cpu.eip,
            (unsigned long long)p->api_calls);
    if (p->stop == LOADER_STOP_FAULT) {
        const cpu_t *c = &p->cpu;
        unsigned seg = 0;
        for (i = 0; i < p->ne->seg_count; i++)
            if (p->seg_sel[i] == c->seg[CPU_CS]) seg = (unsigned)i + 1;
        fprintf(stderr, "  fault in NE segment %u: AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x BP=%04x SP=%04x\n"
                        "    DS=%04x ES=%04x SS=%04x  ESI=%08x EDI=%08x ECX=%08x EDX=%08x\n",
                seg, (unsigned)(c->r[CPU_EAX] & 0xFFFF), (unsigned)(c->r[CPU_EBX] & 0xFFFF),
                (unsigned)(c->r[CPU_ECX] & 0xFFFF), (unsigned)(c->r[CPU_EDX] & 0xFFFF),
                (unsigned)(c->r[CPU_ESI] & 0xFFFF), (unsigned)(c->r[CPU_EDI] & 0xFFFF),
                (unsigned)(c->r[CPU_EBP] & 0xFFFF), (unsigned)(c->r[CPU_ESP] & 0xFFFF), c->seg[CPU_DS],
                c->seg[CPU_ES], c->seg[CPU_SS], (unsigned)c->r[CPU_ESI], (unsigned)c->r[CPU_EDI], (unsigned)c->r[CPU_ECX], (unsigned)c->r[CPU_EDX]);
    }
    if (n && !h->rec[n - 1].implemented) {
        const hle_record *r = &h->rec[n - 1];
        fprintf(stderr, "  first unimplemented call: %s.%u %s from %04x:%04x\n", r->module, r->ordinal, r->name,
                r->caller_cs, r->caller_ip);
    }
    if (user_stub_count(u)) {
        fprintf(stderr, "  USER stubs called (logged, returned 0): %zu\n", user_stub_count(u));
        for (i = 0; i < user_stub_count(u) && i < 8; i++) {
            const user_stub_rec *s = user_stub_at(u, i);
            fprintf(stderr, "    USER.%u %s (%s) from %04x:%04x\n", s->ordinal, s->name, s->task, s->caller_cs,
                    s->caller_ip);
        }
    }
    fprintf(stderr, "API sequence (%zu calls recorded):\n", n);
    for (i = 0; i < n; i++) {
        if (i < 60 || i + 10 >= n) print_call(&h->rec[i], p, i);
        else if (i == 60) fprintf(stderr, "  ... (use --sequence for all)\n");
    }
}

static void report_all(const hle_t *h, const loader_proc *p) {
    size_t i;
    fprintf(stderr, "full API sequence:\n");
    for (i = 0; i < h->nrec; i++) print_call(&h->rec[i], p, i);
}

/* The TrueType font set: explicit directory, environment, next to the executable (the portable package), the source tree,
 * then the system. NULL when no face is found anywhere (text then keeps the built-in 5x7 glyphs). */
static ttf_set *open_fonts(const options *o, const char *argv0) {
    ttf_set *s;
    char exe[512], dir[600];
    ssize_t n;
    const char *env = getenv("CWFA_FONT_DIR");
    int have = 0;
    if (o->no_ttf) return NULL;
    s = ttf_set_create();
    if (!s) return NULL;
    if (o->font_dir) have = ttf_set_add_dir(s, o->font_dir);
    if (env && *env) have = ttf_set_add_dir(s, env);
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        char *slash;
        exe[n] = 0;
        slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            snprintf(dir, sizeof dir, "%s/../fonts", exe);
            have = ttf_set_add_dir(s, dir);
            snprintf(dir, sizeof dir, "%s/fonts", exe);
            have = ttf_set_add_dir(s, dir);
        }
    }
    (void)argv0;
    have = ttf_set_add_dir(s, "third_party/fonts");
    if (have < 6) have = ttf_set_add_system(s);
    if (have == 0) {
        ttf_set_free(s);
        return NULL;
    }
    ttf_set_use_freetype(s, true);                /* hinted 1-bit glyphs when libfreetype is installed; stb otherwise */
    return s;
}

int main(int argc, char **argv) {
    options o;
    appicon icon = {0, 0, NULL};
    ne_image ne;
    ne_err nerr;
    hle_t h;
    kernel_t *k = NULL;
    win87em_t *w87 = NULL;
    user_t *u = NULL;
    gdi_t *g = NULL;
    toolhelp_t *th = NULL;
    ver_t *ver = NULL;
    mmsystem_t *mm = NULL;
    audio_t *audio = NULL;
    audio_sdl_t *adev = NULL;
    present_t *pr = NULL;
    pump_ref ref = {NULL, NULL, NULL, NULL, 0};
    kernel_config kc;
    gdi_config gc;
    ttf_set *fonts = NULL;
    user_config uc;
    present_config pc;
    loader_config lc;
    loader_proc *p = NULL;
    char tmpl[] = "/tmp/cwfa-c-XXXXXX", lockpath[1100], dos_exe[64], mpath[256], es_exe[1000];
    const char *base, *croot;
    bool temp_c = false, staged = false, spawned = false;
    spawn_ctx sc;
    int rc, status = 2;
    size_t i;

    if ((rc = parse(argc, argv, &o))) return rc;
    if (o.exe && !o.es_prog && file_is_compressed(o.exe)) {    /* WRITER.EX_ given directly: use the staged copy */
        const char *b = strrchr(o.exe, '/');
        o.es_prog = b ? b + 1 : o.exe;
    }
    if (!o.es_prog && (nerr = ne_open_file(&ne, o.exe)) != NE_OK) {
        fprintf(stderr, "cwfa: %s: %s\n", o.exe, ne_strerror(nerr));
        return 2;
    }
    if (o.install_dir && !o.cd) {
        fprintf(stderr, "cwfa: --install-dir needs --cd\n");
        ne_free(&ne);
        return 2;
    }
    croot = o.c_root ? o.c_root : o.install_dir;
    temp_c = o.temp_c && croot;
    if (!croot) {
        temp_c = true;
        if (!(croot = mkdtemp(tmpl))) {
            fprintf(stderr, "cwfa: cannot create a temporary C: directory\n");
            ne_free(&ne);
            return 2;
        }
    }
    if (!o.c_root && !o.no_install && o.cd) {
        install_report ir;
        if (!install_stage(o.cd, croot, o.es_prog ? NULL : o.exe, &ir)) {
            fprintf(stderr, "cwfa: cannot stage the install: %s\n", ir.err);
            if (temp_c) nftw(croot, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
            ne_free(&ne);
            return 2;
        }
        staged = true;
        if (!o.quiet)
            fprintf(stderr, "cwfa: installed C: in %s (%u files linked, %u already there%s)\n", croot, ir.linked,
                    ir.skipped, ir.had_inf ? "" : ", no SETUP.INF on the CD");
    }
    if (o.es_prog) {                       /* the program was expanded from the media into C:\MSKIDS */
        char name[64];
        size_t k;
        snprintf(name, sizeof name, "%.40s", o.es_prog);
        for (k = 0; name[k] && name[k] != '.'; k++) name[k] = (char)toupper((unsigned char)name[k]);
        name[k] = 0;
        snprintf(es_exe, sizeof es_exe, "%.900s/MSKIDS/%.40s.EXE", croot, name);
        o.exe = es_exe;
        if ((nerr = ne_open_file(&ne, o.exe)) != NE_OK) {
            fprintf(stderr, "cwfa: %s: %s\n", o.exe, ne_strerror(nerr));
            if (temp_c) nftw(croot, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
            return 2;
        }
    }
    base = strrchr(o.exe, '/');
    base = base ? base + 1 : o.exe;
    if (o.export_icon) {                           /* only the icon wanted: write it and leave */
        uint8_t *png = NULL;
        size_t plen = 0;
        int rc = 1;
        FILE *fp;
        int want = o.export_icon_size ? atoi(o.export_icon_size) : 0;
        if (appicon_from_ne(&ne, 32, &icon) && want > icon.w) {      /* a whole-number factor keeps the pixels sharp */
            appicon big;
            if (appicon_scale(&icon, want / icon.w, &big)) {
                appicon_free(&icon);
                icon = big;
            }
        }
        if (icon.argb && appicon_png(&icon, &png, &plen) && (fp = fopen(o.export_icon, "wb")) != NULL) {
            rc = fwrite(png, 1, plen, fp) == plen ? 0 : 1;
            if (fclose(fp)) rc = 1;
        }
        if (rc) fprintf(stderr, "cwfa: cannot write the icon of %s to %s\n", base, o.export_icon);
        free(png);
        appicon_free(&icon);
        ne_free(&ne);
        if (temp_c) nftw(croot, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
        return rc ? 1 : 0;
    }
    snprintf(dos_exe, sizeof dos_exe, staged ? "C:\\MSKIDS\\%.40s" : "C:\\CWFA\\%.40s", base);
    for (i = 8; dos_exe[i]; i++) dos_exe[i] = (char)toupper((unsigned char)dos_exe[i]);

    memset(&kc, 0, sizeof kc);
    kc.host_root['C' - 'A'] = croot;
    if (o.cd) kc.host_root['D' - 'A'] = o.cd;
    kc.hide_cd_drive = o.hide_cd_drive;
    kc.cd_label = o.cd_label ? o.cd_label : getenv("CWFA_CD_LABEL");
    if (!kc.cd_label && o.cd) kc.cd_label = "CWFA110__01";   /* the label of the Creative Writer & Fine Artist CD */
    if (kc.cd_label && !*kc.cd_label) kc.cd_label = NULL;     /* --cd-label "": no label */
    kc.exe_path = o.exe_path ? o.exe_path : dos_exe;
    kc.exe_host_path = o.exe;
    {   /* libraries: the program's directory, C:\\, the system directories, then the CD's SETUP as a last resort */
        const char *ep = kc.exe_path, *bs = strrchr(ep, '\\');
        snprintf(mpath, sizeof mpath, "%.*s;C:\\;C:\\WINDOWS\\SYSTEM;C:\\WINDOWS%s", bs ? (int)(bs - ep) : 0, bs ? ep : "",
                 o.cd ? ";D:\\SETUP" : "");
        kc.module_path = bs ? mpath : mpath + 1;
    }
    kc.debug = o.quiet ? NULL : stderr;
    sc.o = &o;
    sc.croot = croot;
    sc.spawned = &spawned;
    sc.lockfd = -1;
    sc.lockpath = NULL;
    if (temp_c) {
        size_t len = strlen(croot);
        while (len > 1 && croot[len - 1] == '/') len--;
        if (snprintf(lockpath, sizeof lockpath, "%.*s.lock", (int)len, croot) < (int)sizeof lockpath) sc.lockpath = lockpath;
    }
    kc.winexec = spawn_program;
    kc.winexec_ctx = &sc;
    memset(&uc, 0, sizeof uc);
    uc.log = o.quiet && !o.trace_msgs ? NULL : stderr;
    uc.log_messages = o.trace_msgs;
    uc.pump = pump;
    uc.pump_ctx = &ref;
    uc.refresh = refresh;
    ref.shot = o.refresh_shot;
    ref.shot_ms = o.refresh_shot_ms;
    hle_init(&h);
    h.stub_unknown = true;
    h.record = true;
    h.trace = o.trace;
    h.trace_args = o.trace_args;
    h.log = o.quiet ? NULL : stderr;
    k = kernel_create(&kc, &h, 0);
    w87 = win87em_create(&h);
    u = user_create(&uc, &h);
    memset(&gc, 0, sizeof gc);
    gc.log = o.quiet ? NULL : stderr;
    gc.host_path = font_host_path;
    gc.host_ctx = k;
    fonts = open_fonts(&o, argv[0]);
    gc.fonts = fonts;
    gc.print_dir = o.print_dir ? o.print_dir : getenv("CWFA_PRINT_DIR");
    if (fonts && o.cd) {                    /* the TrueType files the CD's setup installs (Lucida Sans, McZee, Impact...) */
        char sd[1024];
        snprintf(sd, sizeof sd, "%s/SETUP", o.cd);
        ttf_set_add_installed(fonts, sd);
    }
    if (fonts && staged) {                  /* fonts the setup copied (Spanish media: expanded .TTF files) */
        char sd[1100];
        snprintf(sd, sizeof sd, "%.900s/MSKIDS/SHARED/STUFF", croot);
        ttf_set_add_installed(fonts, sd);
    }
    if (u) g = gdi_create(&gc, &h, u);
    if (k) th = toolhelp_create(&h, k);
    if (k) ver = ver_create(&h, k);
    if (!o.audio || !strcmp(o.audio, "sdl")) {
        audio = audio_sdl_create(&(audio_sdl_config){.dummy = o.headless}, &adev);
        if (!audio && !o.quiet) fprintf(stderr, "cwfa: no SDL audio device, sound follows the program clock without output\n");
    }
    if (!audio && (!o.audio || strcmp(o.audio, "off"))) audio = audio_create(&(audio_config){.rate = 44100});
    if (k && u) mm = mmsystem_create(&(mmsystem_config){.audio = audio, .log = o.quiet ? NULL : stderr}, &h, u, k);
    for (i = 0; i < o.nprobes && u; i++) {
        hle_def d = {o.probes[i].module, (uint16_t)o.probes[i].ordinal, "probe", (uint16_t)o.probes[i].arg_bytes,
                     probe_fn};
        if (!hle_register(&h, &d, &o.probes[i])) {
            fprintf(stderr, "cwfa: --probe %s.%u is already implemented\n", o.probes[i].module, o.probes[i].ordinal);
            status = 2;
            goto done;
        }
    }
    if (!k || !w87 || !u || !g || !th || !ver || !mm) {
        fprintf(stderr, "cwfa: out of memory\n");
        goto done;
    }
    memset(&pc, 0, sizeof pc);
    {   /* the program's own icon and an app id for the window (the launcher is matched by it, see scripts/install-desktop.sh) */
        static char appid[40];
        char lower[24];
        size_t m;
        snprintf(lower, sizeof lower, "%.20s", base);
        for (m = 0; lower[m] && lower[m] != '.'; m++) lower[m] = (char)tolower((unsigned char)lower[m]);
        lower[m] = 0;
        snprintf(appid, sizeof appid, o.es_prog ? "cwfa-es-%s" : "cwfa-%s", lower);
        pc.app_id = appid;
        if (appicon_from_ne(&ne, 32, &icon)) {
            pc.icon_argb = icon.argb;
            pc.icon_w = icon.w;
            pc.icon_h = icon.h;
        }
    }
    pc.headless = o.headless;
    pc.scale = o.scale;
    pc.fullscreen = o.fullscreen && !o.headless;
    pc.integer_scale = o.integer_scale;
    pc.fixed_size = o.fixed_size;
    pc.no_fs_button = o.no_fs_button || (getenv("CWFA_NO_FS_BUTTON") && *getenv("CWFA_NO_FS_BUTTON") == '1');
    pc.max_frames = o.frames;
    pr = present_create(&pc, user_fb(u), user_wm(u));
    if (!pr) {
        fprintf(stderr, "cwfa: cannot open the display (try --headless)\n");
        goto done;
    }
    ref.pr = pr;
    ref.u = u;
    if (o.inject && !(ref.in = inject_parse(o.inject, (char[96]){0}, 96))) {
        fprintf(stderr, "cwfa: out of memory\n");
        goto done;
    }
    memset(&lc, 0, sizeof lc);
    lc.resolve = hle_resolve;
    lc.resolve_ctx = &h;
    lc.cmdline = "";
    lc.open_module = kernel_open_module;
    lc.open_ctx = k;
    {
        loader_err le = loader_create(&ne, &lc, &p);
        if (le != LOADER_OK) {
            fprintf(stderr, "cwfa: %s: %s\n", o.exe, loader_strerror(le));
            goto done;
        }
    }
    if (o.trace) trace_libraries(p, stderr);
    if (o.ip_on) {
        static ip_trace it;
        if (o.ip_seg > p->ne->seg_count) {
            fprintf(stderr, "cwfa: --trace-ip: the program has %u segments\n", (unsigned)p->ne->seg_count);
            status = 2;
            goto done;
        }
        it.p = p;
        it.u = u;
        it.from_ms = o.ip_from_ms;
        it.sel = o.ip_seg ? p->seg_sel[o.ip_seg - 1] : 0;
        it.seg = o.ip_seg;
        it.lo = (uint32_t)o.ip_lo;
        it.hi = (uint32_t)o.ip_hi;
        it.left = o.ip_max;
        cpu_set_step_hook(&p->cpu, ip_trace_fn, &it);
    }
    if (!kernel_attach(k, p) || !user_attach(u, p) || !gdi_attach(g, p) || !toolhelp_attach(th, p) || !mmsystem_attach(mm, p)) {
        fprintf(stderr, "cwfa: cannot attach the runtime to the process\n");
        goto done;
    }
    {
        static live_t live;
        live.h = &h;
        live.u = u;
        live.g = g;
        live.a = audio;
        live.p = p;
        live.file = o.debug_live;
        live.stall_ms = (o.watchdog_set ? o.watchdog_s : 15u) * 1000u;
        g_live = &live;
        signal(SIGUSR1, on_dump_signal);
        signal(SIGQUIT, on_dump_signal);
        live.started = pthread_create(&live.th, NULL, live_thread, &live) == 0;
        loader_run(p, o.max_steps);
        live.stop = 1;
        if (live.started) pthread_join(live.th, NULL);
        if (o.debug_live) live_dump(&live, "exit", false);
        g_live = NULL;
    }
    present_flush(pr);
    report(&h, p, u);
    if (o.sequence) report_all(&h, p);
    if (o.screenshot && !fb_save_ppm(user_fb(u), o.screenshot)) {
        fprintf(stderr, "cwfa: cannot write the screenshot %s\n", o.screenshot);
        status = 2;
        goto done;
    }
    status = (p->stop == LOADER_STOP_HLT || p->stop == LOADER_STOP_REQUESTED) &&
                     !(h.nrec && !h.rec[h.nrec - 1].implemented)
                 ? 0
                 : 1;
done:
    if (p) loader_destroy(p);
    inject_free(ref.in);
    present_destroy(pr);
    gdi_destroy(g);
    ttf_set_free(fonts);
    toolhelp_destroy(th);
    ver_destroy(ver);
    mmsystem_destroy(mm);
    if (adev) audio_sdl_destroy(adev, audio);
    else audio_destroy(audio);
    user_destroy(u);
    win87em_destroy(w87);
    kernel_destroy(k);
    hle_destroy(&h);
    appicon_free(&icon);
    ne_free(&ne);
    if (temp_c && !spawned) nftw(croot, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
    else if (temp_c && sc.lockpath) hand_over_temp_c(&sc);
    return status;
}
