#ifndef KWAJ_H
#define KWAJ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Expansion of the Microsoft KWAJ ("KWAJ" 88 F0 27 D1) and SZDD ("SZDD" 88 F0 27 33) compressed files that
 * the Windows 3.x setup programs copy from the installation disks (WRITER.EX_, MAX100.AR_, ...). Dependency
 * free; the format is documented in src/kwaj.c.
 *
 * KWAJ methods: 0 stored, 1 XOR 0xFF, 2 LZSS (the SZDD scheme), 3 LZ + Huffman. Method 4 (MSZIP) is
 * recognised and refused (KWAJ_BAD_METHOD). SZDD ("normal" mode 'A') is expanded like method 2. */

typedef enum {
    KWAJ_OK = 0,
    KWAJ_NOT_COMPRESSED,  /* no KWAJ/SZDD signature */
    KWAJ_TRUNCATED,       /* the file ends inside the header, a table or the data */
    KWAJ_BAD_METHOD,      /* unknown or unsupported compression method */
    KWAJ_CORRUPT,         /* invalid Huffman table or impossible header */
    KWAJ_NOMEM
} kwaj_err;

/* True when the first bytes carry a KWAJ or SZDD signature. */
bool kwaj_is_compressed(const uint8_t *buf, size_t len);

/* Expands the whole file in buf. On KWAJ_OK *out is a malloc'ed buffer (release with free) of *out_len
 * bytes; on failure *out is NULL and *out_len 0. The uncompressed length comes from the header; a KWAJ file
 * without one is expanded until its input ends (UNVERIFIED: no real file lacks the field). */
kwaj_err kwaj_expand(const uint8_t *buf, size_t len, uint8_t **out, size_t *out_len);

const char *kwaj_strerror(kwaj_err err);

#endif
