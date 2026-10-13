#ifndef DIAG_H
#define DIAG_H
/* Freeze diagnostics: a watchdog on "the program makes no API calls" and a plain-text snapshot (cwfa --debug-live) of
 * what the program was doing: windows, timers, queues, device contexts and its last API calls. Nothing here changes
 * behaviour; it only reads. */
#include <stdint.h>
#include <stdio.h>

#include "hle.h"
#include "wm.h"

typedef struct {
    uint64_t progress;       /* the counter at the last change */
    uint32_t changed_ms;     /* when it changed */
    uint32_t reported_ms;    /* last report (0: none since the change) */
    int primed, reported;
} diag_watch;

void diag_watch_init(diag_watch *w);
/* One sample of a progress counter (the number of API calls). True when it has not moved for stall_ms; then again
 * after each further stall_ms. *stalled_ms gets the time without progress. stall_ms 0 never reports. */
int diag_watch_poll(diag_watch *w, uint64_t progress, uint32_t now_ms, uint32_t stall_ms, uint32_t *stalled_ms);

typedef struct {
    const hle_t *hle;
    const wm_t *wm;
    uint32_t now_ms;
    uint64_t api_calls;
    void (*extra)(void *ctx, FILE *f);   /* optional: more lines (GDI objects, audio...) */
    void *extra_ctx;
} diag_sources;

void diag_dump(FILE *f, const diag_sources *s, const char *reason);

#endif
