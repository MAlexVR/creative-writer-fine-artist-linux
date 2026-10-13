#ifndef INSTALL_ES_H
#define INSTALL_ES_H

#include <stdbool.h>

#include "install.h"

/* Staging of an installed C: drive from the Spanish (1.0) Creative Writer / Fine Artist disks, the way their
 * INSTALAR.EXE would: every file on the media is KWAJ-compressed (src/kwaj.h), and the media's own setup
 * script (_SETUP.IN_, plain or compressed) names, per row, the source file (column 1, "name.ex_" for
 * "name.exe"), an optional append-to target (column 2: the row's data is appended to that destination, which
 * is how ARTIST.PIC is split over two disks) and the destination under C:\MSKIDS (column 11).
 * [Files] and [Shared Fonts] are installed; rows without destination are skipped. Unlike the English CD
 * staging the files are expanded copies, not links. Existing destinations are left alone.
 *
 * src is either one directory that holds INSTALAR.LST (a CD layout, or a single floppy) or a directory of
 * disk directories (DISCO1..n, d1..n, in any case) searched in name order. */

/* True when dir looks like Spanish setup media (see above). */
bool install_es_detect(const char *dir);

bool install_stage_es(const char *src, const char *c_root, const char *exe_host, install_report *rep);

#endif
