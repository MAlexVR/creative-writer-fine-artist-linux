#define _XOPEN_SOURCE 700
/* The cwfa command line, run as a child process (headless: no display needed).
 * The real-program runs are skipped unless CWFA_WRITER_EXE / CWFA_ARTIST_EXE
 * are set; CWFA_CD_ROOT maps the CD to drive D:. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fixture.h"

#define CWFA "./build/cwfa"

/* Runs the shell command, captures stdout+stderr and the exit status. */
static int run(const char *cmd, char *out, size_t cap) {
    char full[16384];
    FILE *p;
    size_t n = 0;
    int c, status;
    snprintf(full, sizeof full, "%s 2>&1", cmd);
    p = popen(full, "r");
    if (!p) return -1;
    while ((c = fgetc(p)) != EOF)
        if (n + 1 < cap) out[n++] = (char)c;
    out[n] = 0;
    status = pclose(p);
    return status == -1 ? -1 : (status & 0x7F) ? 128 + (status & 0x7F) : (status >> 8) & 0xFF;
}

/* Like run, for runs whose --trace output is too long to keep: only the lines matching the egrep pattern are kept
 * (each distinct line once) and the program's own exit status comes back. */
static int run_grep(const char *cmd, const char *pattern, char *out, size_t cap) {
    char full[4096], line[16];
    char *rcp;
    snprintf(full, sizeof full, "( %s ; echo \"cwfa-rc=$?\" ) 2>&1 | grep -aE '^(cwfa-rc=|%s)' | awk '!seen[$0]++'", cmd, pattern);
    if (run(full, out, cap) < 0) return -1;
    rcp = strstr(out, "cwfa-rc=");
    if (!rcp) return -1;
    snprintf(line, sizeof line, "%.3s", rcp + 8);
    return atoi(line);
}

static void test_usage_errors(void) {
    static char out[1 << 16];
    printf("test_cwfa_usage\n");
    CHECK_EQ(run(CWFA, out, sizeof out), 2);
    CHECK(strstr(out, "usage"));
    CHECK_EQ(run(CWFA " --exe /nonexistent/NOPE.EXE --cd .", out, sizeof out), 2);
    CHECK(strstr(out, "NOPE.EXE"));
    CHECK_EQ(run(CWFA " --exe x --cd . --frames abc", out, sizeof out), 2);
    CHECK(strstr(out, "frames"));
    CHECK_EQ(run(CWFA " --bogus", out, sizeof out), 2);
    CHECK_EQ(run(CWFA " --exe", out, sizeof out), 2);                  /* option without its value */
    CHECK_EQ(run(CWFA " --exe x --probe TOOLHELP.63", out, sizeof out), 2);   /* a probe needs its argument bytes */
    CHECK_EQ(run(CWFA " --exe x --probe 63:4", out, sizeof out), 2);
    CHECK_EQ(run(CWFA " --exe x --probe KEYBOARD.6:8:bogus", out, sizeof out), 2);
    CHECK(strstr(out, "strcpy"));
    CHECK_EQ(run(CWFA " --exe x --probe KIDSWTCH.1:4:ret=zz", out, sizeof out), 2);
    CHECK(strstr(out, "ret=N"));
    CHECK_EQ(run(CWFA " --exe x --probe MMSYSTEM.607:0:inc=zz", out, sizeof out), 2);
    CHECK(strstr(out, "inc=N"));
    CHECK_EQ(run(CWFA " --exe x --audio", out, sizeof out), 2);
    CHECK_EQ(run(CWFA " --exe x --audio bogus", out, sizeof out), 2);
    CHECK(strstr(out, "sdl, null or off"));
    CHECK_EQ(run(CWFA " --exe x --screenshot", out, sizeof out), 2);
    CHECK_EQ(run(CWFA " --exe x --inject", out, sizeof out), 2);
    CHECK_EQ(run(CWFA " --exe x --inject 'key:NOSUCH'", out, sizeof out), 2);   /* a bad script is a usage error */
    CHECK(strstr(out, "NOSUCH"));
    CHECK_EQ(run(CWFA " --exe x --install-dir /tmp/x --c-root /tmp/y", out, sizeof out), 2);   /* where C: comes from: one way only */
    CHECK(strstr(out, "exclude each other"));
    CHECK_EQ(run(CWFA " --exe x --no-install --c-root /tmp/y", out, sizeof out), 2);
    CHECK_EQ(run(CWFA " --exe Makefile --install-dir build/cwfa_c_nocd", out, sizeof out), 2);  /* staging needs the CD */
    CHECK_EQ(run(CWFA " --exe x --cd-label", out, sizeof out), 2);
    CHECK_EQ(run(CWFA, out, sizeof out), 2);
    CHECK(strstr(out, "--screenshot PATH"));
    CHECK(strstr(out, "--inject"));
    CHECK(strstr(out, "--install-dir DIR"));
    CHECK(strstr(out, "--cd-label L"));
    CHECK(strstr(out, "--audio MODE"));
    CHECK(strstr(out, "--fullscreen"));
    CHECK(strstr(out, "--hide-cd-drive"));
    CHECK(strstr(out, "--trace-ip"));
    CHECK(strstr(out, "--trace-msgs"));
    CHECK_EQ(run(CWFA " --exe x --trace-ip nonsense", out, sizeof out), 2);
    CHECK(strstr(out, "--trace-ip needs"));
    CHECK(strstr(out, "--data-dir DIR"));
    CHECK(strstr(out, "F11"));
}

/* the window options are accepted by a headless run (dummy driver): fullscreen, integer scale, fixed size */
static void test_window_options(void) {
    static char out[1 << 16];
    printf("test_cwfa_window_options\n");
    CHECK_EQ(run(CWFA " --exe Makefile --cd . --headless --frames 1 --fullscreen --integer-scale --fixed-size", out, sizeof out), 2);
    CHECK(!strstr(out, "unknown option"));                        /* it fails on the file (not an NE), not on the options */
    CHECK(strstr(out, "Makefile"));
    CHECK_EQ(run(CWFA " --exe x --data-dir /tmp/x --c-root /tmp/y", out, sizeof out), 2);   /* --data-dir is --install-dir */
    CHECK(strstr(out, "exclude each other"));
}

static void test_not_an_exe(void) {
    static char out[1 << 16];
    printf("test_cwfa_not_an_ne\n");
    CHECK_EQ(run(CWFA " --exe Makefile --cd . --headless --frames 1", out, sizeof out), 2);
    CHECK(strstr(out, "Makefile"));
}

static const char *cdroot(void) {
    const char *cd = getenv("CWFA_CD_ROOT");
    return cd && *cd ? cd : ".";
}

/* Without any stand-in both programs get through start-up with the real KIDSWTCH.DLL (loaded as the program's
 * dependency from the staged C:\\WINDOWS\\SYSTEM) and the real MMSYSTEM: they ask the sound card what it can do,
 * play McZee's greeting through waveOut (the program decodes its ADPCM sound resource itself and writes 16 bit PCM)
 * and settle at the "who are you" screen until the frame limit; no call is unimplemented and nothing faults. */
static void real(const char *label, const char *env, bool dll) {
    static char out[1 << 20];
    char cmd[1024];
    const char *path = getenv(env);
    int rc;
    printf("test_cwfa_%s\n", label);
    if (!path || !*path) {
        printf("  SKIP (%s not set)\n", env);
        return;
    }
    snprintf(cmd, sizeof cmd, CWFA " --exe '%s' --cd '%s' --headless --frames 300 --trace", path, cdroot());
    rc = run_grep(cmd,
                  "cwfa: installed C:|stopped:|hle: (unimplemented|USER.179 GetSystemMetrics|TOOLHELP.63 TaskFirst|GDI.80 "
                  "GetDeviceCaps|MMSYSTEM)|mmsystem:|dll: KIDSWTCH",
                  out, sizeof out);
    CHECK_EQ(rc, 0);                                 /* stopped on the frame limit: no fault, no unimplemented call */
    CHECK(strstr(out, "cwfa: installed C: in /tmp/cwfa-c-"));
    CHECK(strstr(out, "stopped: stop requested"));
    CHECK(!strstr(out, "unimplemented"));
    CHECK(strstr(out, "USER.179 GetSystemMetrics"));
    CHECK(strstr(out, "TOOLHELP.63 TaskFirst"));
    CHECK(strstr(out, "GDI.80 GetDeviceCaps"));
    if (dll) {                                       /* ARTIST connects before it reaches MMSYSTEM; WRITER after */
        CHECK(strstr(out, "dll: KIDSWTCH.FCONNECTSWITCHER (1)"));   /* real code of the real library */
        CHECK(strstr(out, "dll: KIDSWTCH.FCONNECTKIDSAPP (3)"));
    }
    CHECK(strstr(out, "MMSYSTEM.401 waveOutGetNumDevs"));
    CHECK(strstr(out, "MMSYSTEM.402 waveOutGetDevCaps"));
    CHECK(strstr(out, "MMSYSTEM.415 waveOutGetVolume"));
    CHECK(strstr(out, "MMSYSTEM.404 waveOutOpen"));
    CHECK(strstr(out, "MMSYSTEM.406 waveOutPrepareHeader"));
    CHECK(strstr(out, "MMSYSTEM.408 waveOutWrite"));
    CHECK(strstr(out, "mmsystem: waveOutOpen device ffff tag 1 11127 Hz 1 ch 16 bit"));   /* WAVE_MAPPER, PCM: decoded by the program */
    CHECK(strstr(out, "mmsystem: waveOutWrite 0e01: 22216 bytes -> 11108 frames"));
    if (!strcmp(env, "CWFA_WRITER_EXE")) CHECK(strstr(out, "MMSYSTEM.607 timeGetTime"));
}

/* Both programs start up on the install cwfa stages from the CD: the .SND library
 * loads, the programs no longer ask to open a file named "\r" (the HEY! box), and they wait at the "who are you"
 * screen; the framebuffer then holds that picture (many colours), not a blank screen. */
static void startup(const char *label, const char *env, const char *prog) {
    static char out[1 << 20];
    static unsigned char pix[640 * 480 * 3 + 15];
    char cmd[8192], root[64], shot[64], p[256];
    const char *exe = getenv(env);
    FILE *f;
    size_t n = 0, i, k;
    unsigned seen[64][3], nseen = 0;
    int rc;
    printf("test_cwfa_startup_%s\n", label);
    if (!exe || !*exe) {
        printf("  SKIP (%s not set)\n", env);
        return;
    }
    snprintf(root, sizeof root, "build/cwfa_c_%s", label);
    snprintf(shot, sizeof shot, "build/cwfa_startup_%s.ppm", label);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    CHECK_EQ(system(cmd), 0);
    remove(shot);
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 50 --max-steps 600000000 --sequence --install-dir '%s' "
                  "--screenshot %s",
             exe, cdroot(), root, shot);
    rc = run(cmd, out, sizeof out);
    CHECK_EQ(rc, 0);                                 /* stopped on the frame limit: no fault, no unimplemented call */
    CHECK(strstr(out, "stopped: stop requested"));
    snprintf(p, sizeof p, "LoadLibrary(C:\\MSKIDS\\%s\\%s.SND) = ", prog, prog);
    CHECK(strstr(out, p));
    CHECK(!strstr(out, "files is missing"));
    CHECK(!strstr(out, "OpenFile(\".\\\", "));         /* the empty document name of the CR-terminated command tail */
    CHECK(!strstr(out, "<== NOT IMPLEMENTED"));
    snprintf(p, sizeof p, "%s/WINDOWS/SYSTEM/KIDSWTCH.DLL", root);   /* the staged tree is kept for --install-dir */
    CHECK(access(p, R_OK) == 0);
    snprintf(p, sizeof p, "%s/MSKIDS/%s/%s.SND", root, prog, prog);
    CHECK(access(p, R_OK) == 0);
    f = fopen(shot, "rb");
    CHECK(f != NULL);
    if (!f) return;
    n = fread(pix, 1, sizeof pix, f);
    fclose(f);
    CHECK_EQ(n, sizeof pix);
    for (i = 15; i + 2 < n && nseen < 64; i += 3) {
        for (k = 0; k < nseen; k++)
            if (seen[k][0] == pix[i] && seen[k][1] == pix[i + 1] && seen[k][2] == pix[i + 2]) break;
        if (k == nseen) {
            seen[nseen][0] = pix[i];
            seen[nseen][1] = pix[i + 1];
            seen[nseen][2] = pix[i + 2];
            nseen++;
        }
    }
    CHECK(nseen >= 8);                               /* a drawn picture, not a blank desktop */
}

/* --screenshot writes the 640x480 framebuffer as a PPM after the run, whatever stopped it. */
static void screenshot(const char *label, const char *env) {
    static char out[1 << 20];
    char cmd[1024], path[128], head[16];
    const char *exe = getenv(env);
    FILE *f;
    long size = -1;
    printf("test_cwfa_screenshot_%s\n", label);
    if (!exe || !*exe) {
        printf("  SKIP (%s not set)\n", env);
        return;
    }
    snprintf(path, sizeof path, "build/cwfa_%s.ppm", label);
    remove(path);
    snprintf(cmd, sizeof cmd, CWFA " --exe '%s' --cd '%s' --headless --frames 5 --quiet --screenshot %s", exe, cdroot(),
             path);
    run(cmd, out, sizeof out);
    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) return;
    memset(head, 0, sizeof head);
    CHECK(fread(head, 1, 15, f) == 15);
    CHECK(memcmp(head, "P6\n640 480\n255\n", 15) == 0);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fclose(f);
    CHECK_EQ(size, 15 + 640 * 480 * 3);
    snprintf(cmd, sizeof cmd, CWFA " --exe '%s' --cd '%s' --headless --frames 5 --quiet --screenshot /no/such/dir/x.ppm",
             exe, cdroot());
    CHECK_EQ(run(cmd, out, sizeof out), 2);          /* a screenshot that cannot be written is an error */
    CHECK(strstr(out, "x.ppm"));
}

/* --inject delivers scripted input on the idle frames: a shot step taken after some waits is written while
 * the program is still running, and the run stops at the frame limit, not at the shot. A click on the "who are
 * you" screen reaches the real KIDSWTCH.FGETFSPLOGIN (the call that used to stop the run as KIDSWTCH.5). */
static void inject(const char *label, const char *env, const char *prog) {
    static char out[1 << 20];
    char cmd[8192], root[64], shot[64];
    const char *exe = getenv(env);
    FILE *f;
    printf("test_cwfa_inject_%s\n", label);
    if (!exe || !*exe) {
        printf("  SKIP (%s not set)\n", env);
        return;
    }
    snprintf(root, sizeof root, "build/cwfa_c_inject_%s", label);
    snprintf(shot, sizeof shot, "build/cwfa_inject_%s.ppm", label);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    CHECK_EQ(system(cmd), 0);
    remove(shot);
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 60 --max-steps 600000000 --quiet --install-dir '%s' "
                  "--inject 'wait:20; move:100,100; shot:%s'",
             exe, cdroot(), root, shot);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(strstr(out, "stopped: stop requested"));
    f = fopen(shot, "rb");
    CHECK(f != NULL);
    if (f) fclose(f);
    if (strcmp(prog, "WRITER")) return;
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 200 --max-steps 600000000 --trace --install-dir '%s' "
                  "--inject 'wait:30; click:340,254'",
             exe, cdroot(), root);
    CHECK_EQ(run_grep(cmd, "dll: KIDSWTCH|first unimplemented|hle: unimplemented", out, sizeof out), 0);   /* no unimplemented call */
    CHECK(strstr(out, "dll: KIDSWTCH.FGETFSPLOGIN (5)"));
    CHECK(strstr(out, "dll: KIDSWTCH.FREADUSERLIST (10)"));
    CHECK(!strstr(out, "unimplemented"));
}

/* The new-user flow of the login screen, driven with real mouse steps (move first, then press and release on
 * separate frames: the programs track the pointer over their own buttons, so an instantaneous click is ignored):
 * I'M NEW, a name typed into the edit box, NEXT, NEXT, NEXT, then "Go straight to the Writing Studio" (WRITER,
 * the bullet at 98,177). The programs create the user's directory (DOS mkdir), write ~USER.INF, play McZee's
 * lines through waveOut on every screen and both programs arrive at their studio: the toolbar, a white page and
 * McZee's hint. WRITER takes "Go straight to the Writing Studio" (98,158), ARTIST "Go straight to the Painting
 * Studio" (119,155). Until the HCURSOR was a lockable global block ARTIST reported a memory or disk space error
 * there. */
#define LOGIN_STEPS \
    "wait:30; move:340,254; wait:2; down:340,254; wait:2; up:340,254; wait:20; type:Ana; wait:3; " \
    "move:340,186; wait:2; down:340,186; wait:2; up:340,186; wait:20; " \
    "move:318,186; wait:2; down:318,186; wait:2; up:318,186; wait:20; " \
    "move:340,186; wait:2; down:340,186; wait:2; up:340,186; wait:20"

static bool shot_pixel(const char *path, int x, int y, unsigned char rgb[3]) {
    unsigned char b[640 * 480 * 3 + 15];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return false;
    n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n != sizeof b) return false;
    memcpy(rgb, b + 15 + 3 * (y * 640 + x), 3);
    return true;
}

/* How many pixels of the rectangle are not white (-1: the file cannot be read). */
static int shot_nonwhite(const char *path, int x0, int y0, int x1, int y1) {
    static unsigned char b[640 * 480 * 3 + 15];
    int x, y, n = 0;
    FILE *f = fopen(path, "rb");
    size_t got;
    if (!f) return -1;
    got = fread(b, 1, sizeof b, f);
    fclose(f);
    if (got != sizeof b) return -1;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            const unsigned char *p = b + 15 + 3 * (y * 640 + x);
            if (p[0] != 255 || p[1] != 255 || p[2] != 255) n++;
        }
    return n;
}

static int shot_count_rgb(const char *path, int x0, int y0, int x1, int y1, int r, int g, int bl) {
    static unsigned char b[640 * 480 * 3 + 15];
    int x, y, n = 0;
    FILE *f = fopen(path, "rb");
    size_t got;
    if (!f) return -1;
    got = fread(b, 1, sizeof b, f);
    fclose(f);
    if (got != sizeof b) return -1;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            const unsigned char *p = b + 15 + 3 * (y * 640 + x);
            if (p[0] == r && p[1] == g && p[2] == bl) n++;
        }
    return n;
}

static int shot_red(const char *path, int x0, int y0, int x1, int y1) {
    static unsigned char b[640 * 480 * 3 + 15];
    int x, y, n = 0;
    FILE *f = fopen(path, "rb");
    size_t got;
    if (!f) return -1;
    got = fread(b, 1, sizeof b, f);
    fclose(f);
    if (got != sizeof b) return -1;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            const unsigned char *p = b + 15 + 3 * (y * 640 + x);
            if (p[0] > 180 && p[1] < 40 && p[2] < 40) n++;
        }
    return n;
}

static void login(const char *label, const char *env, int bx, int by) {
    static char out[1 << 20];
    char cmd[8192], root[64], shot[64], p[256];
    static const char *const steps = LOGIN_STEPS;
    const char *exe = getenv(env);
    unsigned char rgb[3];
    printf("test_cwfa_login_%s\n", label);
    if (!exe || !*exe) {
        printf("  SKIP (%s not set)\n", env);
        return;
    }
    snprintf(root, sizeof root, "build/cwfa_c_login_%s", label);
    snprintf(shot, sizeof shot, "build/cwfa_login_%s.ppm", label);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    CHECK_EQ(system(cmd), 0);
    remove(shot);
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 600 --max-steps 6000000000 --quiet --install-dir '%s' "
                  "--inject '%s; move:%d,%d; wait:2; down:%d,%d; wait:2; up:%d,%d; wait:100; shot:%s'",
             exe, cdroot(), root, steps,
             bx, by, bx, by, bx, by, shot);
    CHECK_EQ(run(cmd, out, sizeof out), 0);          /* the frame limit: no unimplemented call, no fault */
    CHECK(strstr(out, "stopped: stop requested"));
    CHECK(!strstr(out, "NOT IMPLEMENTED"));
    snprintf(p, sizeof p, "%s/MSKIDS/USERS/ANA/~USER.INF", root);
    CHECK(access(p, R_OK) == 0);                     /* DOS mkdir and the user's file, made by the program */
    CHECK(shot_pixel(shot, 320, 5, rgb));
    CHECK(!(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 128));   /* no navy title bar: the client area covers the frame */
    CHECK(shot_pixel(shot, 300, 300, rgb));
    CHECK(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255);   /* the blank page, no HEY! box over it */
    CHECK(shot_pixel(shot, 600, 60, rgb));
    CHECK(!(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255)); /* the EXIT door of the toolbar */
}

/* The Sound Picker tool asks for the CD: the programs look for it by the volume label of drive D: (the CD is
 * "CWFA110  01", given as CWFA110__01), which cwfa reports by default. Without it Spike's HEY! box covers the page. */
static void cd_present(void) {
    static char out[1 << 20];
    char cmd[8192], root[64], shot[64];
    const char *exe = getenv("CWFA_WRITER_EXE");
    printf("test_cwfa_cd_present\n");
    if (!exe || !*exe) {
        printf("  SKIP (CWFA_WRITER_EXE not set)\n");
        return;
    }
    snprintf(root, sizeof root, "build/cwfa_c_cd_writer");
    snprintf(shot, sizeof shot, "build/cwfa_cd_writer.ppm");
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    CHECK_EQ(system(cmd), 0);
    remove(shot);
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 700 --max-steps 6000000000 --quiet --install-dir '%s' "
                  "--inject '%s; move:98,158; wait:2; down:98,158; wait:2; up:98,158; wait:40; "
                  "move:125,26; wait:2; down:125,26; wait:2; up:125,26; wait:40; shot:%s'",
             exe, cdroot(), root, LOGIN_STEPS, shot);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK_EQ(shot_nonwhite(shot, 150, 200, 420, 320), 0);      /* the page, no HEY! box */
}

/* A short real writing session in Creative Writer, headless: log in as the new user, click the page and type a
 * sentence, leave through the EXIT door and save it under a name ("Save this work?"), then quit the program from
 * the Writing Studio ("Quit Creative Writer?"), which ends the run by itself with DOS terminate code 0. A second run
 * on the same install logs in as the user from the list, opens the portfolio and the saved document and the
 * sentence is on the page again. The click positions are the programs' own buttons (looked up in screenshots). */
#define CLICK(x, y) "move:" #x "," #y "; wait:2; down:" #x "," #y "; wait:2; up:" #x "," #y "; wait:40; "

static void session(void) {
    static char out[1 << 20];
    char cmd[8192], root[64], shot[64];
    const char *exe = getenv("CWFA_WRITER_EXE");
    printf("test_cwfa_session\n");
    if (!exe || !*exe) {
        printf("  SKIP (CWFA_WRITER_EXE not set)\n");
        return;
    }
    snprintf(root, sizeof root, "build/cwfa_c_session");
    snprintf(shot, sizeof shot, "build/cwfa_session.ppm");
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    CHECK_EQ(system(cmd), 0);
    remove(shot);
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 2500 --max-steps 12000000000 --install-dir '%s' "
                  "--inject '%s; move:98,158; wait:2; down:98,158; wait:2; up:98,158; wait:60; " CLICK(200, 231)
                  "type:Hello World this is my story; wait:10; " CLICK(612, 41) "wait:60; " CLICK(203, 235)
                  "wait:60; type:Story1; wait:20; key:enter; wait:100; " CLICK(487, 207) "wait:60; " CLICK(228, 255)
                  "wait:100'",
             exe, cdroot(), root, LOGIN_STEPS);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(strstr(out, "DOS terminate, code 0"));      /* the program quit by itself, cleanly */
    CHECK(!strstr(out, "NOT IMPLEMENTED"));
    CHECK(!strstr(out, "fault"));
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 2500 --max-steps 12000000000 --quiet --install-dir '%s' "
                  "--inject 'wait:60; " CLICK(160, 220) "wait:20; " CLICK(98, 158) "wait:60; " CLICK(612, 41)
                  "wait:300; " CLICK(345, 395) "wait:150; " CLICK(264, 343) "wait:300; shot:%s'",
             exe, cdroot(), root, shot);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(shot_nonwhite(shot, 90, 175, 270, 200) > 150);          /* the saved sentence, black on the white page */
    CHECK_EQ(shot_nonwhite(shot, 90, 230, 400, 440), 0);          /* nothing else on the page */
    /* Max's sample document: its pictures are metafiles (SetMetaFileBits, PlayMetaFile): the flower border and
     * the headline come out; before the metafile support the page stayed blank */
    remove(shot);
    CHECK_EQ(system("rm -rf build/cwfa_c_sample"), 0);       /* a user with no documents: the wall holds the sample */
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 3000 --max-steps 12000000000 --quiet --install-dir '%s' "
                  "--inject '%s; move:98,158; wait:2; down:98,158; wait:2; up:98,158; wait:60; " CLICK(612, 41)
                  "wait:300; move:190,150; wait:2; down:190,150; wait:1; up:190,150; wait:2; down:190,150; wait:1; "
                  "up:190,150; wait:400; shot:%s; " CLICK(30, 40) "wait:60; shot:build/cwfa_session_tool.ppm'",
             exe, cdroot(), "build/cwfa_c_sample", LOGIN_STEPS, shot);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(shot_nonwhite(shot, 60, 150, 100, 450) > 3000);         /* the border's flowers down the left side */
    CHECK(shot_nonwhite(shot, 130, 200, 500, 260) > 1500);        /* the coloured title */
    /* The Word tool with this big document open: the program fits the drop capital with GetGlyphOutline metrics; with
     * the call failing it drew its own "You're running out of space" box (Spike's pale yellow HEY! card) over the page */
    CHECK_EQ(shot_count_rgb("build/cwfa_session_tool.ppm", 220, 110, 420, 340, 255, 238, 76), 0);
    CHECK(shot_count_rgb("build/cwfa_session_tool.ppm", 470, 110, 640, 330, 255, 238, 76) > 3000);   /* McZee's hint bubble */
    /* Closing the host window is the close box: the program asks "Quit Creative Writer?" itself and ends with
     * code 0 on the first answer */
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 1500 --max-steps 12000000000 --install-dir '%s' "
                  "--inject 'wait:60; " CLICK(160, 220) "wait:20; " CLICK(98, 158) "wait:100; quit; wait:100; "
                  CLICK(228, 255) "wait:100'",
             exe, cdroot(), root);
    CHECK_EQ(run_grep(cmd, "DOS terminate|NOT IMPLEMENTED|fault", out, sizeof out), 0);
    CHECK(strstr(out, "DOS terminate, code 0"));
    CHECK(!strstr(out, "NOT IMPLEMENTED"));
}

/* A short real painting session in Fine Artist, headless: log in as the new user and enter the Painting Studio,
 * pick red and draw a rectangle with the Paint Brush (mouse down, moves, up), choose another colour and flood the
 * inside with the Paint Bucket, pick a cat from the Sticker Picker and drop it on the canvas, leave through the EXIT
 * door ("Do you want to: Save this work?") and name the painting; the program stores it as MY_CAT.MAG and shows the
 * Painting Studio room, where "Quit" -> "Quit Fine Artist?" ends the run by itself with DOS terminate code 0. A
 * second run on the same install logs in from the list, leaves the new painting for the room, opens the portfolio
 * ("ANA / Painting Studio / My Cat") and GO brings the saved picture back. Click positions are the programs' own
 * buttons, looked up in screenshots. */
static int shot_orange(const char *path, int x0, int y0, int x1, int y1) {
    static unsigned char b[640 * 480 * 3 + 15];
    int x, y, n = 0;
    FILE *f = fopen(path, "rb");
    size_t got;
    if (!f) return -1;
    got = fread(b, 1, sizeof b, f);
    fclose(f);
    if (got != sizeof b) return -1;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            const unsigned char *p = b + 15 + 3 * (y * 640 + x);
            if (p[0] > 200 && p[1] > 80 && p[1] < 200 && p[2] < 80) n++;
        }
    return n;
}

static void artist_session(void) {
    static char out[1 << 20];
    char cmd[8192], root[64], shot[64], p[256];
    const char *exe = getenv("CWFA_ARTIST_EXE");
    unsigned char rgb[3];
    printf("test_cwfa_artist_session\n");
    if (!exe || !*exe) {
        printf("  SKIP (CWFA_ARTIST_EXE not set)\n");
        return;
    }
    snprintf(root, sizeof root, "build/cwfa_c_artist_session");
    snprintf(shot, sizeof shot, "build/cwfa_artist_session.ppm");
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    CHECK_EQ(system(cmd), 0);
    remove(shot);
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 6000 --max-steps 12000000000 --install-dir '%s' "
                  "--inject '%s; " CLICK(119, 155) "wait:110; "
                  CLICK(14, 190)                                         /* red */
                  "move:150,200; wait:2; down:150,200; wait:2; move:300,200; wait:2; move:300,300; wait:2; "
                  "move:150,300; wait:2; move:150,200; wait:2; up:150,200; wait:20; "
                  CLICK(80, 30) CLICK(14, 280) CLICK(220, 250)           /* Paint Bucket, blue, inside the frame */
                  CLICK(128, 30) CLICK(210, 78) "wait:40; " CLICK(210, 242) "wait:40; "   /* Sticker Picker: a cat */
                  CLICK(480, 330) "wait:60; "
                  CLICK(612, 41) "wait:60; " CLICK(209, 228) "wait:80; "   /* EXIT, "Save this work?" */
                  "type:My Cat; wait:20; key:enter; wait:150; "
                  CLICK(493, 267) "wait:60; " CLICK(229, 255) "wait:100'",   /* Quit, "Quit Fine Artist?" */
             exe, cdroot(), root, LOGIN_STEPS);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(strstr(out, "DOS terminate, code 0"));      /* the program quit by itself, cleanly */
    CHECK(!strstr(out, "NOT IMPLEMENTED"));
    CHECK(!strstr(out, "fault"));
    snprintf(p, sizeof p, "%s/MSKIDS/USERS/ANA/MY_CAT.MAG", root);
    CHECK(access(p, R_OK) == 0);                      /* the saved painting */
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 7000 --max-steps 12000000000 --quiet --install-dir '%s' "
                  "--inject 'wait:60; " CLICK(160, 220) "wait:20; " CLICK(119, 155) "wait:110; " CLICK(612, 41)
                  "wait:60; " CLICK(167, 333) "wait:100; " CLICK(360, 365) "wait:200; " CLICK(262, 340)
                  "wait:300; shot:%s'",
             exe, cdroot(), root, shot);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(shot_pixel(shot, 152, 250, rgb));
    CHECK(rgb[0] > 180 && rgb[1] < 40 && rgb[2] < 40);            /* the red brush stroke */
    CHECK(shot_pixel(shot, 220, 250, rgb));
    CHECK(rgb[2] > 180 && rgb[0] < 140 && rgb[1] < 100);          /* the bucket's blue inside it */
    CHECK(shot_pixel(shot, 200, 150, rgb));
    CHECK(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255);       /* the rest is still the white canvas */
    CHECK(shot_orange(shot, 400, 280, 452, 380) > 300);           /* the cat sticker */
}

/* Fine Artist's Undo Egg (undo and redo of the last stroke), the EXIT door (open and dark while "Do you want to:
 * Save this work?" is asked, orange again after "Keep working?") and "Work on the last thing you saved?": the room
 * picks the newest file by its DOS date and time (INT 21h AH=57h, which used to fail, so the wall showed the CD's
 * sample painting instead of the one just saved). The second run reopens the saved painting from the wall. */
static void artist_tools(void) {
    static char out[1 << 20];
    char cmd[8192], root[64];
    const char *exe = getenv("CWFA_ARTIST_EXE");
    unsigned char rgb[3];
    printf("test_cwfa_artist_tools\n");
    if (!exe || !*exe) {
        printf("  SKIP (CWFA_ARTIST_EXE not set)\n");
        return;
    }
    snprintf(root, sizeof root, "build/cwfa_c_artist_tools");
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    CHECK_EQ(system(cmd), 0);
    remove("build/cwfa_at_undo.ppm");
    remove("build/cwfa_at_redo.ppm");
    remove("build/cwfa_at_door.ppm");
    remove("build/cwfa_at_keep.ppm");
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 7000 --max-steps 12000000000 --install-dir '%s' "
                  "--inject '%s; " CLICK(119, 155) "wait:110; " CLICK(14, 190)
                  "move:150,200; wait:2; down:150,200; wait:2; move:300,200; wait:2; move:300,300; wait:2; "
                  "up:300,300; wait:20; "
                  CLICK(283, 30) "shot:build/cwfa_at_undo.ppm; " CLICK(283, 30) "shot:build/cwfa_at_redo.ppm; "
                  CLICK(612, 41) "shot:build/cwfa_at_door.ppm; " CLICK(210, 303) "wait:20; shot:build/cwfa_at_keep.ppm; "
                  CLICK(612, 41) CLICK(209, 228) "wait:80; type:My Red; wait:20; key:enter; wait:150; "
                  CLICK(493, 267) "wait:60; " CLICK(229, 255) "wait:100'",
             exe, cdroot(), root, LOGIN_STEPS);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(strstr(out, "DOS terminate, code 0"));
    CHECK(!strstr(out, "NOT IMPLEMENTED"));
    CHECK(shot_pixel("build/cwfa_at_undo.ppm", 200, 200, rgb));
    CHECK(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255);        /* undone: the stroke is gone */
    CHECK(shot_pixel("build/cwfa_at_redo.ppm", 200, 200, rgb));
    CHECK(rgb[0] > 180 && rgb[1] < 40 && rgb[2] < 40);             /* redone: back */
    CHECK(shot_count_rgb("build/cwfa_at_door.ppm", 585, 25, 630, 95, 0, 0, 0) > 2500);   /* the open, dark door */
    CHECK(shot_count_rgb("build/cwfa_at_keep.ppm", 585, 25, 630, 95, 0, 0, 0) < 1500);    /* closed again */
    CHECK(shot_pixel("build/cwfa_at_keep.ppm", 200, 200, rgb));
    CHECK(rgb[0] > 180 && rgb[1] < 40 && rgb[2] < 40);             /* and the painting is still there */
    remove("build/cwfa_at_wall.ppm");
    remove("build/cwfa_at_last.ppm");
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 5000 --max-steps 12000000000 --quiet --install-dir '%s' "
                  "--inject 'wait:60; " CLICK(160, 220) "wait:20; " CLICK(119, 155) "wait:150; " CLICK(612, 41)
                  "wait:80; shot:build/cwfa_at_wall.ppm; " CLICK(167, 296) "dblclick:360,120; wait:300; "
                  "shot:build/cwfa_at_last.ppm'",
             exe, cdroot(), root);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(shot_red("build/cwfa_at_wall.ppm", 290, 70, 370, 150) > 100);   /* My Red's stroke on the wall */
    CHECK(shot_pixel("build/cwfa_at_last.ppm", 200, 200, rgb));
    CHECK(rgb[0] > 180 && rgb[1] < 40 && rgb[2] < 40);             /* the saved painting is open again */
}

/* "Go straight to Imaginopolis": the town map is animated by a loop that makes a memory DC, a region, a bitmap and a
 * pattern brush per frame and frees them through IsGDIObject(hdc) + DeleteDC/DeleteObject. IsGDIObject said 0 for a
 * DC (and for a window DC), nothing was freed, the 8192 GDI objects ran out and, live, Spike's "I am almost out of
 * space!" card covered the map. The run is long enough to exhaust the table many times over: the program must free
 * its DCs (DeleteDC calls in the trace) and the map is on screen. */
static void imaginopolis(void) {
    static char out[1 << 16];
    char cmd[8192], root[64];
    const char *exe = getenv("CWFA_WRITER_EXE");
    printf("test_cwfa_imaginopolis\n");
    if (!exe || !*exe) {
        printf("  SKIP (CWFA_WRITER_EXE not set)\n");
        return;
    }
    snprintf(root, sizeof root, "build/cwfa_c_imaginopolis");
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    CHECK_EQ(system(cmd), 0);
    remove("build/cwfa_imaginopolis.ppm");
    snprintf(cmd, sizeof cmd,
             "( " CWFA " --exe '%s' --cd '%s' --headless --frames 6000 --max-steps 12000000000 --trace --install-dir '%s' "
             "--inject '%s; wait:100; " CLICK(98, 91) "wait:1000; shot:build/cwfa_imaginopolis.ppm' ) 2>&1 "
             "| grep -ac 'GDI.68 DeleteDC called'",
             exe, cdroot(), root, LOGIN_STEPS);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(atoi(out) > 50);                                          /* before the fix: 5, all from start-up */
    CHECK(shot_count_rgb("build/cwfa_imaginopolis.ppm", 0, 0, 640, 480, 0, 0, 0) > 1000);   /* the map's outlines */
}

/* The splash: Creative Writer invalidates its splash window, calls UpdateWindow on the main window and then loads for
 * seconds in one message handler, polling timeGetTime. UpdateWindow reaches the children with an update region, and
 * the display is refreshed from the clock reads, so the splash is on screen during the load (not only afterwards). */
static void splash(void) {
    static char out[1 << 16];
    char cmd[2048];
    const char *exe = getenv("CWFA_WRITER_EXE");
    printf("test_cwfa_splash\n");
    if (!exe || !*exe) {
        printf("  SKIP (CWFA_WRITER_EXE not set)\n");
        return;
    }
    remove("build/cwfa_splash.ppm");
    CHECK_EQ(system("rm -rf build/cwfa_c_splash"), 0);
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 50 --quiet --install-dir build/cwfa_c_splash "
                  "--refresh-shot 700:build/cwfa_splash.ppm",
             exe, cdroot());
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(shot_nonwhite("build/cwfa_splash.ppm", 100, 100, 540, 380) > 20000);           /* the white card with its drawing */
    CHECK(shot_count_rgb("build/cwfa_splash.ppm", 0, 0, 640, 480, 0, 0, 0) > 20000);     /* the ink-blot border */
}

/* The task switcher of Fine Artist lists the other MSKids programs (those registered in MSKIDS.INI) and draws their icons
 * with SHELL.ExtractIcon (the first icon group of C:\MSKIDS\WRITER.EXE). It used to stop the run as unimplemented. */
static void switcher(void) {
    static char out[1 << 16];
    char cmd[8192], root[64];
    const char *wexe = getenv("CWFA_WRITER_EXE"), *aexe = getenv("CWFA_ARTIST_EXE");
    printf("test_cwfa_switcher\n");
    if (!wexe || !*wexe || !aexe || !*aexe) {
        printf("  SKIP (CWFA_WRITER_EXE / CWFA_ARTIST_EXE not set)\n");
        return;
    }
    snprintf(root, sizeof root, "build/cwfa_c_switcher");
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    CHECK_EQ(system(cmd), 0);
    snprintf(cmd, sizeof cmd,                                    /* Creative Writer registers itself in MSKIDS.INI */
             CWFA " --exe '%s' --cd '%s' --headless --frames 3000 --max-steps 12000000000 --quiet --install-dir '%s' "
                  "--inject '%s; " CLICK(98, 158) "wait:60'",
             wexe, cdroot(), root, LOGIN_STEPS);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 4000 --max-steps 12000000000 --install-dir '%s' "
                  "--inject 'wait:60; " CLICK(160, 220) "wait:100; " CLICK(119, 155) "wait:100; " CLICK(556, 22) "wait:200; "
                  "shot:build/cwfa_switcher_icon.ppm'",
             aexe, cdroot(), root);
    remove("build/cwfa_switcher_icon.ppm");
    CHECK_EQ(run_grep(cmd, "unimplemented|user: ExtractIcon|stopped", out, sizeof out), 0);
    /* the program icon is drawn into an offscreen DC with a viewport origin: DrawIcon takes logical coordinates */
    CHECK(shot_nonwhite("build/cwfa_switcher_icon.ppm", 300, 235, 340, 270) > 300);
    CHECK(!strstr(out, "unimplemented"));
    CHECK(strstr(out, "ExtractIcon C:\\MSKIDS\\WRITER.EXE #0"));
    /* Clicking the other program asks "Do you want to switch to Creative Writer?"; Yes makes KIDSWTCH walk the tasks
     * (TOOLHELP), start the program with WinExec (KERNEL.166): the runtime
     * starts it as a second cwfa process on the same drive C:, which draws the Creative Writer splash. */
    remove("build/cwfa_switch_child.ppm");
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 20000 --max-steps 12000000000 --trace --install-dir '%s' "
                  "--child-opt --inject --child-opt 'waitms:3000;shot:build/cwfa_switch_child.ppm;stop' "
                  "--inject 'wait:60; " CLICK(160, 220) "wait:100; " CLICK(119, 155) "wait:100; " CLICK(556, 22) "wait:200; "
                  CLICK(320, 265) CLICK(218, 256) "waitms:6000; stop'",
             aexe, cdroot(), root);
    CHECK_EQ(run_grep(cmd, "unimplemented|hle: KERNEL.166 |hle: USER.224 |cwfa: WinExec", out, sizeof out), 0);
    CHECK(!strstr(out, "unimplemented"));
    CHECK(strstr(out, "KERNEL.166 WinExec"));
    CHECK(strstr(out, "cwfa: WinExec started"));
    {   /* the second process finishes on its own (stop); wait for its screenshot */
        int i;
        for (i = 0; i < 30 && access("build/cwfa_switch_child.ppm", F_OK) != 0; i++) sleep(1);
    }
    CHECK(shot_nonwhite("build/cwfa_switch_child.ppm", 100, 100, 540, 380) > 20000);   /* the Creative Writer splash card */
}

/* A temporary drive C: that started a program with WinExec is removed by the last process that uses it, not leaked in
 * /tmp: --temp-c marks the --c-root copy of the switcher's drive as temporary. */
static void switcher_temp_c(void) {
    static char out[1 << 16];
    char cmd[8192];
    const char *aexe = getenv("CWFA_ARTIST_EXE");
    int i;
    printf("test_cwfa_switcher_temp_c\n");
    if (!aexe || !*aexe || access("build/cwfa_c_switcher", F_OK) != 0) {
        printf("  SKIP (CWFA_ARTIST_EXE not set or no switcher drive)\n");
        return;
    }
    CHECK_EQ(system("rm -rf build/cwfa_c_tempc && cp -a build/cwfa_c_switcher build/cwfa_c_tempc"), 0);
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 20000 --max-steps 12000000000 --quiet --c-root build/cwfa_c_tempc "
                  "--exe-path C:\\\\MSKIDS\\\\ARTIST.EXE --temp-c --child-opt --inject --child-opt 'waitms:1500;stop' "
                  "--inject 'wait:60; " CLICK(160, 220) "wait:100; " CLICK(119, 155) "wait:100; " CLICK(556, 22) "wait:200; "
                  CLICK(320, 265) CLICK(218, 256) "waitms:3000; stop'",
             aexe, cdroot());
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    for (i = 0; i < 40 && access("build/cwfa_c_tempc", F_OK) == 0; i++) sleep(1);
    CHECK(access("build/cwfa_c_tempc", F_OK) != 0);                   /* gone once the parent and its child have exited */
}

/* Fine Artist's Vacuum ("Aspiradibujos"), exercised in its three ways on one painting. There is NO lasso in this
 * program (an earlier investigation took the tool for one): the program's own help text says the Paint sucker is a
 * drag-over tool ("drag the mouse over the paint you want to get rid of", Shift+drag = a rectangle), the Area sucker
 * takes the area you click, and the Spewer puts the contents down. The Paint sucker stamps the tool's cursor shape
 * (a thin slash, 12 rows tall, read from the XOR plane of the HCURSOR block: the program copies the plane out of the
 * GlobalLock'ed block, NOT in place, inverts it into a bitmap and inverts it back) along the path into a mono
 * mask, so only a band under the drag is taken, never the inside of a loop.
 * The Area sucker blits the canvas into a mono bitmap with the clicked colour as the background colour (those pixels
 * become 1 in the mono bitmap; a 1 bit comes out white in the composition), FloodFill(border = black) from the
 * click and composes the picked image with XOR/AND/XOR.
 * The run: a red frame filled blue (bucket); Area sucker on the blue; Spewer; Paint sucker along the top edge;
 * Shift+drag over the whole frame; Spewer again. */
#define VAC_STEPS \
    CLICK(14, 190) \
    "move:150,150; wait:2; down:150,150; wait:2; move:300,150; wait:2; move:300,300; wait:2; move:150,300; wait:2; " \
    "move:150,152; wait:2; up:150,152; wait:20; " \
    CLICK(14, 285) CLICK(80, 30) CLICK(225, 225) "shot:build/cwfa_vac_0fill.ppm; " \
    CLICK(325, 30) CLICK(155, 76) CLICK(225, 225) "shot:build/cwfa_vac_1area.ppm; " \
    CLICK(118, 76) CLICK(450, 330) "shot:build/cwfa_vac_2spew.ppm; " \
    CLICK(40, 76) "move:140,150; wait:2; down:140,150; wait:2; move:310,150; wait:2; up:310,150; wait:40; " \
    "shot:build/cwfa_vac_3sweep.ppm; " \
    "keydown:shift; wait:2; move:130,130; wait:2; down:130,130; wait:2; move:320,320; wait:2; up:320,320; wait:2; " \
    "keyup:shift; wait:20; shot:build/cwfa_vac_4rect.ppm; " \
    CLICK(118, 76) CLICK(380, 130) "shot:build/cwfa_vac_5spew.ppm; stop"

static void vacuum_checks(void) {
    unsigned char rgb[3];
    CHECK(shot_pixel("build/cwfa_vac_0fill.ppm", 225, 225, rgb));
    CHECK(rgb[0] == 97 && rgb[1] == 65 && rgb[2] == 219);              /* the bucket filled the frame blue */
    CHECK(shot_pixel("build/cwfa_vac_1area.ppm", 225, 225, rgb));
    CHECK(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255);            /* Area sucker: the clicked area is gone */
    CHECK(shot_pixel("build/cwfa_vac_1area.ppm", 155, 225, rgb));
    CHECK(rgb[0] > 180 && rgb[1] < 40 && rgb[2] < 40);                 /* ... and only that area: the red frame stays */
    CHECK_EQ(shot_count_rgb("build/cwfa_vac_1area.ppm", 390, 360, 510, 392, 97, 65, 219), 0);
    CHECK(shot_count_rgb("build/cwfa_vac_2spew.ppm", 390, 360, 510, 392, 97, 65, 219) > 3500);   /* Spewer puts it down */
    CHECK(shot_pixel("build/cwfa_vac_3sweep.ppm", 225, 150, rgb));
    CHECK(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255);            /* Paint sucker: the band under the drag is white */
    CHECK(shot_pixel("build/cwfa_vac_3sweep.ppm", 225, 143, rgb));
    CHECK(rgb[0] > 180 && rgb[1] < 40 && rgb[2] < 40);                 /* the edge of the stroke outside the band remains */
    CHECK(shot_pixel("build/cwfa_vac_3sweep.ppm", 155, 225, rgb));
    CHECK(rgb[0] > 180 && rgb[1] < 40 && rgb[2] < 40);                 /* nothing else was touched (no lasso interior) */
    CHECK_EQ(shot_red("build/cwfa_vac_4rect.ppm", 20, 110, 330, 330), 0);   /* Shift+drag: the whole rectangle is taken */
    CHECK(shot_red("build/cwfa_vac_5spew.ppm", 330, 20, 620, 300) > 500);   /* and put down again by the Spewer */
}

static void artist_vacuum(void) {
    static char out[1 << 16];
    char cmd[8192];
    const char *exe = getenv("CWFA_ARTIST_EXE");
    printf("test_cwfa_artist_vacuum\n");
    if (!exe || !*exe) {
        printf("  SKIP (CWFA_ARTIST_EXE not set)\n");
        return;
    }
    CHECK_EQ(system("rm -rf build/cwfa_c_vacuum build/cwfa_vac_*.ppm"), 0);
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --quiet --frames 9000 --max-steps 12000000000 --install-dir build/cwfa_c_vacuum "
                  "--inject '%s; " CLICK(119, 155) "wait:110; " VAC_STEPS "'",
             exe, cdroot(), LOGIN_STEPS);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    vacuum_checks();
}

/* The same in the Spanish Fine Artist 1.0 (Aspiradibujos): first-time login, Imaginopolis, the Painting Studio
 * room and a double click on the blank frame lead to the studio. */
#define ES_ARTIST_STUDIO \
    "waitms:1500;move:365,263;waitms:100;click:365,263;waitms:2500;move:245,128;waitms:100;click:245,128;waitms:300;" \
    "type:Ana;key:TAB;waitms:300;type:Lopez;waitms:300;move:331,217;waitms:100;click:331,217;waitms:2500;" \
    "move:320,231;waitms:100;click:320,231;waitms:2500;move:350,230;waitms:100;click:350,230;waitms:3500;" \
    "move:335,186;waitms:100;click:335,186;waitms:3500;move:327,171;waitms:100;click:327,171;waitms:4000;" \
    "move:180,150;waitms:100;click:180,150;waitms:5000;move:302,143;waitms:300;click:302,143;waitms:1500;click:302,143;waitms:6000;" \
    "move:340,360;waitms:300;click:340,360;waitms:1500;click:340,360;waitms:6000;" \
    "move:150,87;waitms:300;click:150,87;waitms:1500;click:150,87;waitms:6000;move:190,200;waitms:300;dblclick:190,200;waitms:6000;"

static void es_artist_vacuum(void) {
    static char out[1 << 16];
    char cmd[8192];
    const char *es = getenv("CWFA_ES_FA");
    printf("test_cwfa_es_artist_vacuum\n");
    if (!es || !*es) {
        printf("  SKIP (CWFA_ES_FA not set)\n");
        return;
    }
    CHECK_EQ(system("rm -rf build/cwfa_c_es_vacuum build/cwfa_vac_*.ppm"), 0);
    snprintf(cmd, sizeof cmd,
             CWFA " --es-artist '%s' --headless --quiet --frames 4000000 --max-steps 12000000000 --install-dir build/cwfa_c_es_vacuum "
                  "--inject '" ES_ARTIST_STUDIO VAC_STEPS "'",
             es);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    vacuum_checks();
}

/* Without --cd the program still runs but nothing is staged; with --no-install the libraries come from D:\SETUP
 * (the module path ends there), the .SND of the program is simply not found. */
static void no_install(void) {
    static char out[1 << 20];
    char cmd[1024];
    const char *exe = getenv("CWFA_WRITER_EXE");
    printf("test_cwfa_no_install\n");
    if (!exe || !*exe) {
        printf("  SKIP (CWFA_WRITER_EXE not set)\n");
        return;
    }
    snprintf(cmd, sizeof cmd, CWFA " --exe '%s' --cd '%s' --no-install --headless --frames 5 --trace", exe, cdroot());
    run_grep(cmd, "cwfa: installed C:|dll: KIDSWTCH", out, sizeof out);
    CHECK(!strstr(out, "installed C:"));
    CHECK(strstr(out, "dll: KIDSWTCH.FCONNECTSWITCHER (1)"));   /* found on the CD's SETUP directory */
}

/* Printing end to end (R4a): the programs print through the virtual "PDF Printer". Each program's Print tool opens a
 * wizard whose McZee speech takes real time (a click while he speaks only interrupts the speech), so the script waits
 * on the program's clock (waitms) and clicks NEXT twice. The programs print with Escape NEXTBAND (banding): the
 * band is the whole page, an empty RECT ends it. The result is a real PDF (pdftotext checked when it is installed). */
#define CLICK2(x, y) "move:" #x "," #y "; wait:2; down:" #x "," #y "; wait:2; up:" #x "," #y "; waitms:1000; " \
                     "move:" #x "," #y "; wait:2; down:" #x "," #y "; wait:2; up:" #x "," #y "; waitms:5000; "

static void print_program(const char *label, const char *env, const char *steps, const char *docname, const char *text) {
    static char out[1 << 16];
    char cmd[8192], root[64], dir[64], path[600], pat[64];
    const char *exe = getenv(env);
    FILE *f;
    printf("test_cwfa_print_%s\n", label);
    if (!exe || !*exe) {
        printf("  SKIP (%s not set)\n", env);
        return;
    }
    snprintf(root, sizeof root, "build/cwfa_c_print_%s", label);
    snprintf(dir, sizeof dir, "build/cwfa_pdf_%s", label);
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", root, dir);
    CHECK_EQ(system(cmd), 0);
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --frames 200000 --max-steps 12000000000 --install-dir '%s' --print-dir '%s' "
                  "--inject '%s%s; stop'",
             exe, cdroot(), root, dir, LOGIN_STEPS, steps);
    snprintf(pat, sizeof pat, "gdi: printed|StartDoc|EndDoc|NOT IMPLEMENTED");
    CHECK_EQ(run_grep(cmd, pat, out, sizeof out), 0);
    CHECK(!strstr(out, "NOT IMPLEMENTED"));
    CHECK(strstr(out, "gdi: printed "));
    snprintf(cmd, sizeof cmd, "ls '%s'/cwfa-%s-*.pdf 2>/dev/null | head -1", dir, docname);
    CHECK_EQ(run(cmd, path, sizeof path), 0);
    path[strcspn(path, "\n")] = 0;
    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f) {
        char head[8] = {0};
        CHECK(fread(head, 1, 5, f) == 5 && !memcmp(head, "%PDF-", 5));
        fclose(f);
        if (text && system("command -v pdftotext >/dev/null 2>&1") == 0) {
            snprintf(cmd, sizeof cmd, "pdftotext '%s' -", path);
            CHECK_EQ(run(cmd, out, sizeof out), 0);
            CHECK(strstr(out, text));
        }
    }
}

/* --debug-live: a snapshot of what the program was doing (windows, timers, queues, DCs, GDI objects, last API calls) and
 * the 100 ms heartbeat timer that both programs start in WM_ACTIVATEAPP. */
static void debug_live(void) {
    static char out[1 << 16], dump[1 << 16];
    char cmd[4096];
    FILE *f;
    size_t n = 0;
    const char *exe = getenv("CWFA_WRITER_EXE");
    printf("test_cwfa_debug_live\n");
    if (!exe || !*exe) {
        printf("  SKIP (CWFA_WRITER_EXE not set)\n");
        return;
    }
    remove("build/cwfa_debug_live.txt");
    snprintf(cmd, sizeof cmd, "rm -rf build/cwfa_c_debug && " CWFA " --exe '%s' --cd '%s' --headless --frames 300 --max-steps 6000000000 "
                              "--quiet --install-dir build/cwfa_c_debug --debug-live build/cwfa_debug_live.txt --watchdog 0", exe, cdroot());
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    f = fopen("build/cwfa_debug_live.txt", "r");
    CHECK(f != NULL);
    if (!f) return;
    n = fread(dump, 1, sizeof dump - 1, f);
    dump[n] = 0;
    fclose(f);
    CHECK(strstr(dump, "reason: exit") != NULL);
    CHECK(strstr(dump, "class=Creative Writer") != NULL);
    CHECK(strstr(dump, "recent_api_calls (oldest first, 256)") != NULL);
    CHECK(strstr(dump, "gdi_objects:") != NULL);
    CHECK(strstr(dump, "timers: 1\n") != NULL);                 /* SetTimer(100 ms) from WM_ACTIVATEAPP */
    CHECK(strstr(dump, "interval=100ms") != NULL);
    /* SIGUSR1 while it runs: a snapshot with reason "signal" without stopping the program */
    remove("build/cwfa_debug_sig.txt");
    snprintf(cmd, sizeof cmd,
             CWFA " --exe '%s' --cd '%s' --headless --max-steps 6000000000 --quiet "
             "--install-dir build/cwfa_c_debug --debug-live build/cwfa_debug_sig.txt --watchdog 0 --inject 'waitms:6000;stop' "
             "& pid=$!; sleep 3; kill -USR1 $pid; wait $pid", exe, cdroot());
    run(cmd, out, sizeof out);
    f = fopen("build/cwfa_debug_sig.txt", "r");
    CHECK(f != NULL);
    if (f) {
        n = fread(dump, 1, sizeof dump - 1, f);
        dump[n] = 0;
        fclose(f);
        CHECK(strstr(dump, "reason: signal") != NULL);
        CHECK(strstr(dump, "reason: exit") != NULL);            /* and the final one at the end */
    }
}

/* Spanish Creative Writer, first run: McZee's whole story ("Si, cuentamela") and on to Imaginopolis. Two bugs froze it
 * in a real window: no WM_ACTIVATEAPP (no 100 ms heartbeat, so the speech chain only advanced on mouse events) and
 * GetDC on the CS_OWNDC window never released (DC table full after 31 repaints: no balloon, then an empty sky). */
#define ES_STORY_LOGIN \
    "waitms:1500;move:365,263;waitms:100;click:365,263;waitms:2500;move:245,128;waitms:100;click:245,128;waitms:300;" \
    "type:José;key:TAB;waitms:300;type:Peña Núñez;waitms:300;move:331,217;waitms:100;click:331,217;waitms:2500;" \
    "move:320,231;waitms:100;click:320,231;waitms:2500;move:350,230;waitms:100;click:350,230;waitms:3500;" \
    "move:335,186;waitms:100;click:335,186;waitms:3500;"
static void es_story(void) {
    static char out[1 << 16], dump[1 << 16];
    char cmd[8192], steps[4096];
    const char *es = getenv("CWFA_ES_CD");
    int i;
    FILE *f;
    size_t n;
    printf("test_cwfa_es_story\n");
    if (!es || !*es) {
        printf("  SKIP (CWFA_ES_CD not set)\n");
        return;
    }
    snprintf(steps, sizeof steps, "%smove:100,124;waitms:100;click:100,124;", ES_STORY_LOGIN);
    for (i = 0; i < 9; i++) strcat(steps, "waitms:2500;move:321,183;waitms:100;click:321,183;");
    strcat(steps, "waitms:8000;shot:build/cwfa_es_story.ppm;stop");
    remove("build/cwfa_es_story.ppm");
    remove("build/cwfa_es_story.txt");
    snprintf(cmd, sizeof cmd,
             "rm -rf build/cwfa_c_es_story; " CWFA " --es-writer '%s' --headless --quiet --frames 4000000 --max-steps 12000000000 "
             "--install-dir build/cwfa_c_es_story --debug-live build/cwfa_es_story.txt --inject '%s'", es, steps);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(!strstr(out, "NOT IMPLEMENTED"));
    CHECK(!strstr(out, "watchdog"));                                       /* never a silent stretch */
    CHECK(shot_count_rgb("build/cwfa_es_story.ppm", 0, 0, 640, 480, 0, 0, 0) > 1000);   /* Imaginopolis' outlines, not an empty sky */
    f = fopen("build/cwfa_es_story.txt", "r");
    CHECK(f != NULL);
    if (f) {
        char *d;
        n = fread(dump, 1, sizeof dump - 1, f);
        dump[n] = 0;
        fclose(f);
        d = strstr(dump, "dcs_in_use: ");
        CHECK(d != NULL);
        CHECK(d && atoi(d + 12) < 8);                                      /* no leaked window DCs */
        CHECK(strstr(dump, "timers: 1\n") != NULL);
    }
}

/* English Creative Writer / Fine Artist: McZee's story from "Learn more about Imaginopolis" to the map. */
static void en_story(const char *label, const char *env, int ox, int oy) {
    static char out[1 << 16];
    char cmd[8192], steps[4096], shot[64];
    const char *exe = getenv(env);
    int i;
    printf("test_cwfa_en_story_%s\n", label);
    if (!exe || !*exe) {
        printf("  SKIP (%s not set)\n", env);
        return;
    }
    snprintf(shot, sizeof shot, "build/cwfa_en_story_%s.ppm", label);
    remove(shot);
    snprintf(steps, sizeof steps,
             "waitms:2500;move:340,254;waitms:100;click:340,254;waitms:2500;type:Ana;waitms:300;move:340,186;waitms:100;click:340,186;"
             "waitms:3000;move:318,186;waitms:100;click:318,186;waitms:3000;move:340,186;waitms:100;click:340,186;waitms:5000;"
             "move:%d,%d;waitms:100;click:%d,%d;waitms:4000;", ox, oy, ox, oy);
    for (i = 0; i < 9; i++) strcat(steps, "move:316,181;waitms:100;click:316,181;waitms:3000;");
    strcat(steps, "waitms:6000;shot:");
    strcat(steps, shot);
    strcat(steps, ";stop");
    snprintf(cmd, sizeof cmd, "rm -rf build/cwfa_c_en_story_%s; " CWFA " --exe '%s' --cd '%s' --headless --quiet --frames 4000000 --max-steps 12000000000 "
                              "--install-dir build/cwfa_c_en_story_%s --inject '%s'", label, exe, cdroot(), label, steps);
    CHECK_EQ(run(cmd, out, sizeof out), 0);
    CHECK(!strstr(out, "watchdog"));
    CHECK(shot_count_rgb(shot, 0, 0, 640, 480, 0, 0, 0) > 1000);        /* the town's outlines, not an empty sky */
}

/* Spanish Creative Writer prints too (the Spanish staging forgot to register the virtual printer in WIN.INI, so the
 * print wizard ended in "Hay algun problema con tu impresora" and no page was produced). */
static void es_print_writer(void) {
    static char out[1 << 16];
    char cmd[8192], path[600];
    const char *es = getenv("CWFA_ES_CD");
    FILE *f;
    printf("test_cwfa_es_print_writer\n");
    if (!es || !*es) {
        printf("  SKIP (CWFA_ES_CD not set)\n");
        return;
    }
    CHECK_EQ(system("rm -rf build/cwfa_c_es_print build/cwfa_pdf_es_print"), 0);
    snprintf(cmd, sizeof cmd,
             CWFA " --es-writer '%s' --headless --frames 4000000 --max-steps 12000000000 --install-dir build/cwfa_c_es_print "
             "--print-dir build/cwfa_pdf_es_print --inject '" ES_STORY_LOGIN
             "move:100,148;waitms:300;click:100,148;waitms:8000;move:300,143;waitms:200;click:300,143;waitms:6000;"
             "move:340,330;waitms:200;click:340,330;waitms:6000;move:143,265;waitms:200;click:143,265;waitms:6000;"
             "move:135,88;waitms:200;click:135,88;waitms:6000;move:375,160;waitms:300;dblclick:375,160;waitms:8000;"
             "move:300,250;waitms:200;click:300,250;waitms:500;type:Hola esta es una prueba;waitms:1000;"
             "move:493,27;waitms:200;click:493,27;waitms:4000;move:160,77;waitms:200;click:160,77;waitms:6000;"
             "move:540,212;waitms:300;click:540,212;waitms:3000;click:540,212;waitms:6000;stop'", es);
    CHECK_EQ(run_grep(cmd, "gdi: printed|NOT IMPLEMENTED", out, sizeof out), 0);
    CHECK(strstr(out, "gdi: printed "));
    CHECK_EQ(run("ls build/cwfa_pdf_es_print/cwfa-Documento_nuevo-*.pdf 2>/dev/null | head -1", path, sizeof path), 0);
    path[strcspn(path, "\n")] = 0;
    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f) {
        char head[8] = {0};
        CHECK(fread(head, 1, 5, f) == 5 && !memcmp(head, "%PDF-", 5));
        fclose(f);
        if (system("command -v pdftotext >/dev/null 2>&1") == 0) {
            snprintf(cmd, sizeof cmd, "pdftotext '%s' -", path);
            CHECK_EQ(run(cmd, out, sizeof out), 0);
            CHECK(strstr(out, "Hola"));
            CHECK(strstr(out, "prueba"));
        }
    }
}

static void print_writer(void) {
    print_program("writer", "CWFA_WRITER_EXE",
                  "; move:98,158; wait:2; down:98,158; wait:2; up:98,158; wait:100; " CLICK(300, 200)
                  "type:Hello World this is my story; wait:20; " CLICK(493, 27) "waitms:4000; " CLICK(160, 77) "waitms:6000; "
                  CLICK2(530, 243),
                  "A_New_Document", "Hello World this is my story");
}

static void print_artist(void) {
    print_program("artist", "CWFA_ARTIST_EXE",
                  "; move:119,155; wait:2; down:119,155; wait:2; up:119,155; wait:100; "
                  "move:200,250; wait:2; down:200,250; wait:2; move:300,300; wait:2; move:400,250; wait:2; up:400,250; wait:20; "
                  CLICK(475, 28) "waitms:6000; " CLICK(160, 77) "waitms:15000; " CLICK2(545, 243),
                  "A_New_Painting", NULL);
}

int main(void) {
    test_usage_errors();
    test_window_options();
    test_not_an_exe();
    real("writer", "CWFA_WRITER_EXE", false);
    real("artist", "CWFA_ARTIST_EXE", true);
    startup("writer", "CWFA_WRITER_EXE", "WRITER");
    startup("artist", "CWFA_ARTIST_EXE", "ARTIST");
    inject("writer", "CWFA_WRITER_EXE", "WRITER");
    login("writer", "CWFA_WRITER_EXE", 98, 158);
    login("artist", "CWFA_ARTIST_EXE", 119, 155);
    cd_present();
    session();
    artist_session();
    artist_tools();
    artist_vacuum();
    es_artist_vacuum();
    imaginopolis();
    splash();
    switcher();
    switcher_temp_c();
    no_install();
    screenshot("writer", "CWFA_WRITER_EXE");
    screenshot("artist", "CWFA_ARTIST_EXE");
    debug_live();
    es_story();
    en_story("writer", "CWFA_WRITER_EXE", 98, 116);
    en_story("artist", "CWFA_ARTIST_EXE", 121, 117);
    print_writer();
    print_artist();
    es_print_writer();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
