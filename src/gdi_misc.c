/* GDI imports outside the display model: GetMetaFile (a logged stub) and relative/absolute mode (fixed). Printing
 * lives in gdi_print.c. */
#include "gdi_priv.h"

#define TAG_META "unscheduled: GetMetaFile (metafiles on disk are not read yet)"

static cpu_status stub_getmetafile(hle_call *c) {
    gdi_log_stub(c, TAG_META);
    hle_ret16(c, 0);
    return CPU_OK;
}

static cpu_status g_getrelabs(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a);
    GCHECK;
    hle_ret16(c, gdc_get(g, h) ? 1 : 0);           /* ABSOLUTE */
    return CPU_OK;
}

static cpu_status g_setrelabs(hle_call *c) {
    GBEGIN;
    uint16_t h = hle_u16(&a), mode = hle_u16(&a);
    GCHECK;
    (void)mode;
    hle_ret16(c, gdc_get(g, h) ? 1 : 0);           /* the previous mode: always ABSOLUTE; the setting itself is ignored */
    return CPU_OK;
}

#define G GDI_MODULE
const hle_def gdi_misc_defs[] = {
    {G, 5, "SetRelAbs", 4, g_setrelabs},                         /* UNV */
    {G, 85, "GetRelAbs", 2, g_getrelabs},                        /* ARTIST */
    {G, 124, "GetMetaFile", 4, stub_getmetafile},
};
#undef G
const size_t gdi_misc_ndefs = sizeof gdi_misc_defs / sizeof gdi_misc_defs[0];
