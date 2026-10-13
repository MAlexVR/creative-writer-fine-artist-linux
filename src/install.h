#ifndef INSTALL_H
#define INSTALL_H

#include <stdbool.h>
#include <stddef.h>

/* Lays out an installed C: drive from the CD, the way the programs' setup
 * would: <cd>/SETUP/SETUP.INF names, per section, the file to copy (column 1)
 * and where it goes (column 11). [Files] go under C:\MSKIDS, [WinSysStuff]
 * (KIDSWTCH.DLL) under C:\WINDOWS\SYSTEM; compressed sources (name ends in "_",
 * the EXEs) are skipped. The CD's MSKIDS tree is mirrored beside them
 * (C:\MSKIDS\SHARED\..., one directory per program) and the program itself is
 * linked in as C:\MSKIDS\<NAME>.EXE. Directories are real, files are symbolic
 * links into the CD (never copies), names are upper case, existing entries are
 * left alone, and the user directories (SHARED\USERS, USERS) start empty because
 * the programs write there. Nothing under the CD is ever modified. */

typedef struct {
    unsigned dirs;     /* directories created */
    unsigned linked;   /* files linked */
    unsigned skipped;  /* entries that already existed */
    unsigned missing;  /* INF entries whose source is not on the CD */
    bool had_inf;      /* SETUP\SETUP.INF was found and read */
    char err[200];     /* why install_stage failed */
} install_report;

/* exe_host: the program file to link in (may be NULL). Returns false when the
 * CD or C: directory is unusable (rep->err says why). */
bool install_stage(const char *cd_root, const char *c_root, const char *exe_host, install_report *rep);

#endif
