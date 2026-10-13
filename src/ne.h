#ifndef NE_H
#define NE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    NE_OK = 0,
    NE_ERR_ARG,
    NE_ERR_IO,
    NE_ERR_NOMEM,
    NE_ERR_TRUNCATED,
    NE_ERR_BAD_MZ,
    NE_ERR_BAD_NE,
    NE_ERR_BAD_OFFSET,
    NE_ERR_BAD_TABLE,
    NE_ERR_NOT_FOUND,
    NE_ERR_BAD_CHAIN
} ne_err;

#define NE_NAME_MAX 255

#define NE_SEG_DATA 0x0001
#define NE_SEG_MOVEABLE 0x0010
#define NE_SEG_SHAREABLE 0x0020
#define NE_SEG_PRELOAD 0x0040
#define NE_SEG_READONLY 0x0080
#define NE_SEG_RELOCINFO 0x0100
#define NE_SEG_DISCARDABLE 0x1000

typedef enum {
    NE_SRC_LOBYTE = 0,
    NE_SRC_SELECTOR = 2,
    NE_SRC_FARPTR = 3, /* selector:offset */
    NE_SRC_OFFSET = 5
} ne_src_type;

typedef enum {
    NE_TGT_INTERNAL = 0,
    NE_TGT_IMPORT_ORDINAL = 1,
    NE_TGT_IMPORT_NAME = 2,
    NE_TGT_OSFIXUP = 3
} ne_target_type;

#define NE_SEG_MOVEABLE_REF 0xff /* internal target: b is an entry ordinal */

typedef struct {
    uint8_t src_type;    /* ne_src_type */
    uint8_t target_type; /* ne_target_type */
    uint8_t additive;    /* 0: src_offset heads a chain of sites in the segment data */
    uint16_t src_offset;
    /* internal:       a = segment (or NE_SEG_MOVEABLE_REF), b = offset or ordinal
     * import ordinal: a = module ref (1-based), b = ordinal
     * import name:    a = module ref (1-based), b = offset into imported names
     * OS fixup:       a = fixup type, b = 0 */
    uint16_t a, b;
} ne_reloc;

typedef struct {
    uint32_t file_offset;  /* 0 = no data in the file (e.g. BSS) */
    uint32_t file_length;  /* a stored 0 with a file offset means 64K */
    uint32_t alloc_size;   /* a stored 0 means 64K */
    uint16_t flags;
    ne_reloc *relocs;
    uint16_t reloc_count;
} ne_segment;

typedef struct {
    uint16_t modref; /* 1-based */
    uint8_t by_name;
    uint16_t ordinal; /* valid when !by_name */
    char name[NE_NAME_MAX + 1]; /* valid when by_name */
    uint32_t refs;    /* relocation records referencing this import */
} ne_import;

typedef struct {
    uint8_t len;
    uint16_t ordinal;
    char text[NE_NAME_MAX + 1];
} ne_name;

typedef enum { NE_ENTRY_UNUSED = 0, NE_ENTRY_FIXED, NE_ENTRY_MOVEABLE } ne_entry_kind;

typedef struct {
    ne_entry_kind kind;
    uint8_t flags;   /* bit 0 exported, bit 1 shared data */
    uint8_t segment; /* 1-based */
    uint16_t offset;
} ne_entry;

/* One resource. An integer id has name[0] == 0; a string id is upper-cased by
 * the linker but compared case-insensitively. offset/length are in bytes. */
typedef struct {
    uint16_t id;
    char name[NE_NAME_MAX + 1];
    uint32_t offset;
    uint32_t length;
    uint16_t flags; /* NE_RES_* */
} ne_resource;

#define NE_RES_MOVEABLE 0x0010
#define NE_RES_PURE 0x0020
#define NE_RES_PRELOAD 0x0040

typedef struct {
    uint16_t id; /* integer type (RT_*), valid when name[0] == 0 */
    char name[NE_NAME_MAX + 1];
    ne_resource *res;
    uint16_t count;
} ne_restype;

/* A resource type or name given as an integer (name == NULL) or a string. */
typedef struct {
    uint16_t id;
    const char *name;
} ne_resid;

typedef struct ne_image {
    uint8_t *data;   /* owned copy of the whole file */
    size_t size;

    uint32_t ne_off; /* file offset of the NE header */
    uint8_t linker_major, linker_minor;
    uint16_t flags;
    uint16_t auto_data_seg; /* 1-based segment number, 0 = none */
    uint16_t heap_size, stack_size;
    uint16_t cs, ip, ss, sp; /* cs/ss are 1-based segment numbers */
    uint16_t seg_count, modref_count;
    uint16_t movable_entries, resource_segs;
    uint8_t target_os;
    uint16_t win_version; /* expected Windows version, major in the high byte */
    uint8_t align_shift; /* never 0: a stored 0 means 9 */

    /* absolute file offsets of the tables */
    uint32_t segtab_off, resource_off, resname_off, modref_off, impname_off;
    uint32_t entry_off, nonres_off;
    uint16_t entry_len, nonres_len;

    ne_segment *segs;       /* seg_count entries, index = segment number - 1 */
    ne_name *resident_names; /* first entry is the module name (ordinal 0) */
    size_t resident_count;
    ne_name *nonres_names;   /* first entry is the module description */
    size_t nonres_count;
    ne_entry *entries;       /* index = ordinal - 1 */
    size_t entry_count;
    ne_restype *restypes;    /* empty when the module has no resource table */
    size_t restype_count;
} ne_image;

ne_err ne_open_buffer(ne_image *ne, const void *buf, size_t len);
ne_err ne_open_file(ne_image *ne, const char *path);
void ne_free(ne_image *ne);
const char *ne_strerror(ne_err err);

/* idx is 1-based, as stored in relocation records */
ne_err ne_modref_name(const ne_image *ne, unsigned idx, char out[NE_NAME_MAX + 1]);
/* off is relative to the start of the imported-names table */
ne_err ne_imported_name(const ne_image *ne, unsigned off, char out[NE_NAME_MAX + 1]);
/* Sites patched by a relocation: the whole chain when not additive.
 * *n receives the total count; at most cap values are written to out
 * (out may be NULL to only count). seg is a 0-based index. */
ne_err ne_reloc_sites(const ne_image *ne, unsigned seg, const ne_reloc *r,
                      uint16_t *out, size_t cap, size_t *n);

/* Distinct imports (module, ordinal or name) referenced by relocations,
 * sorted by module then ordinals before names. Release with ne_free_imports. */
ne_err ne_collect_imports(const ne_image *ne, ne_import **out, size_t *n);
void ne_free_imports(ne_import *imports);

ne_err ne_find_export(const ne_image *ne, const char *name, uint16_t *ordinal);
/* Looks a resource up by type and name (case-insensitive for strings). */
ne_err ne_find_resource(const ne_image *ne, ne_resid type, ne_resid name, const ne_resource **out);
ne_err ne_entry_for_ordinal(const ne_image *ne, unsigned ordinal, const ne_entry **entry);

#endif
