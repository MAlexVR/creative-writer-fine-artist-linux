#include <stdio.h>
#include <string.h>

#include "ne.h"

static void print_flags(unsigned f) {
    static const struct { unsigned bit; const char *name; } names[] = {
        {NE_SEG_DATA, "DATA"},         {NE_SEG_MOVEABLE, "MOVEABLE"},
        {NE_SEG_SHAREABLE, "SHAREABLE"}, {NE_SEG_PRELOAD, "PRELOAD"},
        {NE_SEG_READONLY, "READONLY"}, {NE_SEG_RELOCINFO, "RELOCINFO"},
        {NE_SEG_DISCARDABLE, "DISCARDABLE"},
    };
    size_t i;
    if (!(f & NE_SEG_DATA)) printf(" CODE");
    for (i = 0; i < sizeof names / sizeof *names; i++)
        if (f & names[i].bit) printf(" %s", names[i].name);
}

static void dump(const ne_image *ne, const char *path) {
    char name[NE_NAME_MAX + 1];
    ne_import *imp = NULL;
    size_t n = 0, i, used = 0, relocs = 0, by_kind[4] = {0, 0, 0, 0};
    unsigned s, j;

    printf("File: %s\nSize: %zu\n", path, ne->size);
    printf("NE header at 0x%x, linker %u.%u, target OS %u, expects Windows %u.%02u\n", ne->ne_off,
           ne->linker_major, ne->linker_minor, ne->target_os, ne->win_version >> 8,
           ne->win_version & 0xff);
    if (ne->resident_count) printf("Module: %s\n", ne->resident_names[0].text);
    if (ne->nonres_count) printf("Description: %s\n", ne->nonres_names[0].text);
    printf("Flags: 0x%04x\nAuto data segment: %u\n", ne->flags, ne->auto_data_seg);
    printf("Heap: 0x%04x  Stack: 0x%04x\n", ne->heap_size, ne->stack_size);
    printf("CS:IP = %u:0x%04x   SS:SP = %u:0x%04x\n", ne->cs, ne->ip, ne->ss, ne->sp);
    printf("Alignment shift: %u\n", ne->align_shift);

    printf("Segments: %u\n", ne->seg_count);
    for (s = 0; s < ne->seg_count; s++) {
        const ne_segment *g = &ne->segs[s];
        printf("  #%3u file 0x%08x len 0x%05x alloc 0x%05x flags 0x%04x", s + 1, g->file_offset,
               g->file_length, g->alloc_size, g->flags);
        print_flags(g->flags);
        printf(" relocs %u\n", g->reloc_count);
        relocs += g->reloc_count;
        for (j = 0; j < g->reloc_count; j++) by_kind[g->relocs[j].target_type]++;
    }
    printf("Resident names: %zu\nNon-resident names: %zu\n", ne->resident_count, ne->nonres_count);
    for (i = 0; i < ne->entry_count; i++) used += ne->entries[i].kind != NE_ENTRY_UNUSED;
    printf("Entry table: %zu entries (%zu used)\n", ne->entry_count, used);

    printf("Exports: %zu\n", used);
    for (i = 0; i < ne->resident_count + ne->nonres_count; i++) {
        const ne_name *nm = i < ne->resident_count ? &ne->resident_names[i] : &ne->nonres_names[i - ne->resident_count];
        const ne_entry *en = NULL;
        if (!nm->ordinal) continue;
        printf("  %5u %-4s %s", nm->ordinal, i < ne->resident_count ? "res" : "nres", nm->text);
        if (ne_entry_for_ordinal(ne, nm->ordinal, &en) == NE_OK)
            printf("  -> %u:0x%04x%s", en->segment, en->offset, en->kind == NE_ENTRY_MOVEABLE ? " (moveable)" : "");
        printf("\n");
    }
    printf("Module references: %u\n", ne->modref_count);
    for (j = 1; j <= ne->modref_count; j++) {
        if (ne_modref_name(ne, j, name) == NE_OK) printf("  %u %s\n", j, name);
    }
    printf("Relocations: %zu (internal %zu, ordinal %zu, name %zu, osfixup %zu)\n", relocs,
           by_kind[NE_TGT_INTERNAL], by_kind[NE_TGT_IMPORT_ORDINAL], by_kind[NE_TGT_IMPORT_NAME],
           by_kind[NE_TGT_OSFIXUP]);

    if (ne_collect_imports(ne, &imp, &n) == NE_OK) {
        printf("Imported: %zu distinct imports\n", n);
        for (i = 0; i < n; i++) {
            if (ne_modref_name(ne, imp[i].modref, name) != NE_OK) strcpy(name, "?");
            if (imp[i].by_name)
                printf("  %s.%s (%u refs)\n", name, imp[i].name, imp[i].refs);
            else
                printf("  %s.%u (%u refs)\n", name, imp[i].ordinal, imp[i].refs);
        }
        ne_free_imports(imp);
    }
}

int main(int argc, char **argv) {
    ne_image ne;
    ne_err err;
    if (argc != 2) {
        fprintf(stderr, "usage: %s file.exe\n", argv[0]);
        return 2;
    }
    err = ne_open_file(&ne, argv[1]);
    if (err != NE_OK) {
        fprintf(stderr, "%s: %s\n", argv[1], ne_strerror(err));
        return 1;
    }
    dump(&ne, argv[1]);
    ne_free(&ne);
    return 0;
}
