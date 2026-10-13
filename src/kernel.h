#ifndef KERNEL_H
#define KERNEL_H

#include <stdbool.h>
#include <stdio.h>

#include "hle.h"

/* Win16 KERNEL.EXE semantics implemented on the host (HLE) over mem.h/loader.h.
 *
 * The caller owns an hle_t; kernel_create registers the KERNEL exports in it,
 * the loader is created with hle_resolve as its resolver, and kernel_attach
 * binds the finished loader_proc (task data block, instance data header and
 * the auto data segment's local heap). LoadLibrary knows the system modules
 * the HLE implements and loads every other library from a file through
 * kernel_open_module (see loader.h). */

typedef struct {
    /* Host directory behind each DOS drive letter, NULL when the drive does
     * not exist. host_root['C' - 'A'] is also where WINDOWS\WIN.INI lives. */
    const char *host_root[26];
    const char *exe_path;      /* DOS path of the program, default C:\CWFA\APP.EXE */
    const char *exe_host_path; /* its host file, only needed by AccessResource */
    const char *windows_dir;   /* DOS path, default C:\WINDOWS */
    const char *module_path;   /* ';'-separated DOS directories searched for libraries; default: the
                                  program's directory, the current directory, <windows>\SYSTEM, <windows> */
    const char *cur_dir;       /* DOS current directory, default C:\ */
    char cd_drive;             /* letter of the CD-ROM drive, default D (reported as remote) */
    bool hide_cd_drive;        /* GetDriveType reports the CD drive as absent (a drive list shows only the data drive); its files still work */
    const char *cd_label;      /* its volume label (FindFirst with attribute 08h), NULL: none */
    FILE *debug;               /* OutputDebugString, FatalExit text; NULL: dropped */
    uint32_t (*now_ms)(void *ctx); /* NULL: host monotonic clock */
    void *now_ctx;
    /* WinExec: the runtime runs one program per process, so another program is started by the host. Called with the
     * host file of the program, its DOS path as given, the rest of the command line and the SW_ value; returns what WinExec returns
     * (>= 32 on success, else a DOS error code). NULL: WinExec fails with 0. */
    unsigned (*winexec)(void *ctx, const char *host_exe, const char *dos_exe, const char *args, unsigned show);
    void *winexec_ctx;
} kernel_config;

typedef struct kernel kernel_t;

/* Flags for kernel_register. */
#define KERNEL_REG_USER_HELPERS 0x1 /* also USER.InitApp, GetTickCount, lstrcmp, Ansi*: T6 owns USER */

/* loader_open_fn: finds a library by module name or DOS path (a name without
 * extension means .DLL) along the module path and opens it. The system modules
 * the HLE implements answer "not found" so imports of them stay with the HLE. */
int kernel_open_module(void *kernel, const char *spec, ne_image *out, char dos_path[LOADER_PATH_MAX]);

kernel_t *kernel_create(const kernel_config *cfg, hle_t *h, unsigned flags);
void kernel_destroy(kernel_t *k);
bool kernel_attach(kernel_t *k, loader_proc *p);

/* The one task the runtime runs: the handle GetCurrentTask returns and the module name (the program's base name). */
uint16_t kernel_task(const kernel_t *k);
const char *kernel_module_name(const kernel_t *k);
const char *kernel_exe_path(const kernel_t *k); /* DOS path the program reports for itself */
/* Host file behind a DOS path (drive letters, current directory, any case); false when it does not exist. */
bool kernel_host_path(kernel_t *k, const char *dos, char *host, size_t cap);

#endif
