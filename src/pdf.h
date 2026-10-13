#ifndef PDF_H
#define PDF_H
/* A minimal PDF writer, no dependencies: each page is one 8-bit indexed raster image (RunLengthDecode) covering the
 * page, plus an optional invisible text layer (rendering mode 3, Helvetica/WinAnsi) so the pages can be searched and
 * copied, and pdftotext finds the words. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct pdf pdf_t;

pdf_t *pdf_create(void);
void pdf_free(pdf_t *p);

/* Appends a page of w_pt x h_pt points showing the iw x ih image (one palette index per byte, row 0 at the top). pal
 * holds 256 RGB entries. false: out of memory or bad arguments. */
bool pdf_add_page(pdf_t *p, double w_pt, double h_pt, int iw, int ih, const uint8_t *idx, const uint8_t pal[256][3]);
/* Adds text to the last page: (x_pt, y_pt) is the baseline start measured from the top-left corner of the page. The
 * text is Windows-1252 bytes. false when there is no page. */
bool pdf_page_text(pdf_t *p, double x_pt, double y_pt, double size_pt, const char *text, size_t n);

size_t pdf_page_count(const pdf_t *p);
/* Serialises the document; false on a write error. */
bool pdf_save(const pdf_t *p, const char *path);

/* PDF RunLengthDecode encoder (exposed for the test): returns the encoded size, writes up to cap bytes, EOD included. */
size_t pdf_rle_encode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);

#endif
