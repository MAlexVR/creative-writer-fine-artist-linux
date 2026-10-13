#include "diag.h"

#include <string.h>
#include <time.h>

void diag_watch_init(diag_watch *w) { memset(w, 0, sizeof *w); }

int diag_watch_poll(diag_watch *w, uint64_t progress, uint32_t now_ms, uint32_t stall_ms, uint32_t *stalled_ms) {
    uint32_t still;
    if (!w->primed || progress != w->progress) {
        w->primed = 1;
        w->progress = progress;
        w->changed_ms = now_ms;
        w->reported = 0;
        return 0;
    }
    if (!stall_ms) return 0;
    still = now_ms - w->changed_ms;
    if (still < stall_ms) return 0;
    if (w->reported && now_ms - w->reported_ms < stall_ms) return 0;
    w->reported = 1;
    w->reported_ms = now_ms;
    if (stalled_ms) *stalled_ms = still;
    return 1;
}

void diag_dump(FILE *f, const diag_sources *s, const char *reason) {
    const wm_t *w = s->wm;
    unsigned i, used = 0, own = 0, n = 0;
    hle_record r[HLE_RING];
    size_t k, cnt;
    time_t t = time(NULL);
    fprintf(f, "cwfa debug snapshot\nreason: %s\nwall_time: %s", reason ? reason : "", ctime(&t));
    fprintf(f, "clock_ms: %u\napi_calls: %llu\n", (unsigned)s->now_ms, (unsigned long long)s->api_calls);
    fprintf(f, "active=%04x focus=%04x capture=%04x quit_pending=%d\n", w->active, w->focus, w->capture, w->quit_pending);
    fprintf(f, "posted_messages: %u\ninput_messages: %u\n", w->posted.count, w->input.count);
    for (i = 0; i < WM_MAX_TIMERS; i++)
        if (w->timers[i].used) n++;
    fprintf(f, "timers: %u\n", n);
    for (i = 0; i < WM_MAX_TIMERS; i++)
        if (w->timers[i].used)
            fprintf(f, "  timer hwnd=%04x id=%u interval=%ums due=%u (%+d ms)\n", w->timers[i].hwnd, w->timers[i].id,
                    (unsigned)w->timers[i].interval, (unsigned)w->timers[i].due, (int)(w->timers[i].due - s->now_ms));
    for (i = 0; i < WM_MAX_DCS; i++) {
        if (w->dcs[i].used) used++;
        if (w->dcs[i].used && w->dcs[i].own) own++;
    }
    fprintf(f, "dcs_in_use: %u of %d (%u private)\n", used, WM_MAX_DCS, own);
    fprintf(f, "windows:\n");
    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        const wm_window *x = w->win[i];
        if (!x) continue;
        fprintf(f, "  hwnd=%04x parent=%04x class=%s style=%08x rect=%d,%d,%d,%d%s%s\n", x->hwnd, x->parent,
                x->cls ? x->cls->name : "?", (unsigned)x->style, x->rect.left, x->rect.top, x->rect.right,
                x->rect.bottom, x->has_update ? " update-pending" : "", (x->style & 0x10000000u) ? " visible" : "");
    }
    if (s->extra) s->extra(s->extra_ctx, f);
    cnt = s->hle ? hle_recent(s->hle, r, HLE_RING) : 0;
    fprintf(f, "recent_api_calls (oldest first, %zu):\n", cnt);
    for (k = 0; k < cnt; k++)
        fprintf(f, "  %s.%u %s from %04x:%04x\n", r[k].module, r[k].ordinal, r[k].name, r[k].caller_cs, r[k].caller_ip);
    fflush(f);
}
