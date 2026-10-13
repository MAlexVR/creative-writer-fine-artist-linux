#define _POSIX_C_SOURCE 200809L
/* Audio engine. See audio.h. */
#include "audio.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct entry {
    int16_t *pcm;
    size_t frames;
    unsigned flags;
    uint32_t loops, token;
    struct entry *next;
} entry;

struct audio_stream {
    audio_t *a;
    unsigned rate, channels;
    entry *head, *tail;         /* the queue; head is playing */
    entry *loop_start;          /* first buffer of the loop being played, or NULL */
    uint32_t loop_left;         /* passes left, counting the current one */
    uint64_t pos;               /* 16.16 frame position inside head */
    uint64_t played;            /* source frames of finished buffers */
    uint32_t *done;
    size_t ndone, capdone;
    uint16_t vol_l, vol_r;
    bool paused;
};

struct audio {
    audio_config cfg;
    audio_stream **streams;
    size_t nstreams, capstreams;
    uint64_t mixed;
    uint16_t vol_l, vol_r;
    bool have_origin;
    uint32_t last_ms;
    uint32_t carry;             /* ms * rate not yet turned into whole frames */
    bool guard_origin;          /* real-time watchdog: program clock and frames mixed at its first look */
    uint32_t guard_ms;
    uint64_t guard_mixed;
    uint32_t seen_ms, seen_host;   /* the program clock at the last audio_advance and the host clock then */
};

static void lock(audio_t *a) { if (a->cfg.lock) a->cfg.lock(a->cfg.sink_ctx); }
static void unlock(audio_t *a) { if (a->cfg.unlock) a->cfg.unlock(a->cfg.sink_ctx); }

audio_t *audio_create(const audio_config *cfg) {
    audio_t *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->cfg = *cfg;
    if (!a->cfg.rate) a->cfg.rate = 44100;
    if (!a->cfg.stall_ms) a->cfg.stall_ms = 250;
    a->vol_l = a->vol_r = 0xFFFF;
    return a;
}

static void free_entry(entry *e) {
    free(e->pcm);
    free(e);
}

static void drop_queue(audio_stream *s) {
    entry *e = s->loop_start ? s->loop_start : s->head;
    while (e) {
        entry *n = e->next;
        free_entry(e);
        e = n;
    }
    s->head = s->tail = s->loop_start = NULL;
    s->loop_left = 0;
    s->pos = 0;
}

void audio_destroy(audio_t *a) {
    size_t i;
    if (!a) return;
    for (i = 0; i < a->nstreams; i++) {
        drop_queue(a->streams[i]);
        free(a->streams[i]->done);
        free(a->streams[i]);
    }
    free(a->streams);
    free(a);
}

uint64_t audio_frames_mixed(const audio_t *a) { return a->mixed; }

void audio_set_volume(audio_t *a, uint16_t left, uint16_t right) {
    lock(a);
    a->vol_l = left;
    a->vol_r = right;
    unlock(a);
}

void audio_get_volume(audio_t *a, uint16_t *left, uint16_t *right) {
    lock(a);
    *left = a->vol_l;
    *right = a->vol_r;
    unlock(a);
}

audio_stream *audio_stream_open(audio_t *a, unsigned rate, unsigned channels) {
    audio_stream *s;
    if (!rate || channels < 1 || channels > 2) return NULL;
    if (!(s = calloc(1, sizeof *s))) return NULL;
    s->a = a;
    s->rate = rate;
    s->channels = channels;
    s->vol_l = s->vol_r = 0xFFFF;
    lock(a);
    if (a->nstreams == a->capstreams) {
        size_t cap = a->capstreams ? a->capstreams * 2 : 8;
        audio_stream **v = realloc(a->streams, cap * sizeof *v);
        if (!v) {
            unlock(a);
            free(s);
            return NULL;
        }
        a->streams = v;
        a->capstreams = cap;
    }
    a->streams[a->nstreams++] = s;
    unlock(a);
    return s;
}

void audio_stream_close(audio_stream *s) {
    audio_t *a = s->a;
    size_t i;
    lock(a);
    for (i = 0; i < a->nstreams; i++)
        if (a->streams[i] == s) {
            memmove(&a->streams[i], &a->streams[i + 1], (a->nstreams - i - 1) * sizeof *a->streams);
            a->nstreams--;
            break;
        }
    drop_queue(s);
    unlock(a);
    free(s->done);
    free(s);
}

static void push_done(audio_stream *s, uint32_t token) {
    if (s->ndone == s->capdone) {
        size_t cap = s->capdone ? s->capdone * 2 : 8;
        uint32_t *v = realloc(s->done, cap * sizeof *v);
        if (!v) return;
        s->done = v;
        s->capdone = cap;
    }
    s->done[s->ndone++] = token;
}

bool audio_stream_queue(audio_stream *s, int16_t *pcm, size_t frames, unsigned flags, uint32_t loops, uint32_t token) {
    entry *e = calloc(1, sizeof *e);
    if (!e) {
        free(pcm);
        return false;
    }
    e->pcm = pcm;
    e->frames = frames;
    e->flags = flags;
    e->loops = loops;
    e->token = token;
    lock(s->a);
    if (s->tail) s->tail->next = e;
    if (!s->head) s->head = e;
    s->tail = e;
    unlock(s->a);
    return true;
}

bool audio_stream_pop_done(audio_stream *s, uint32_t *token) {
    bool ok = false;
    lock(s->a);
    if (s->ndone) {
        *token = s->done[0];
        memmove(s->done, s->done + 1, --s->ndone * sizeof *s->done);
        ok = true;
    }
    unlock(s->a);
    return ok;
}

void audio_stream_pause(audio_stream *s, bool paused) {
    lock(s->a);
    s->paused = paused;
    unlock(s->a);
}

bool audio_stream_paused(audio_stream *s) { return s->paused; }

void audio_stream_reset(audio_stream *s) {
    lock(s->a);
    {
        entry *e = s->loop_start ? s->loop_start : s->head;
        while (e) {
            entry *n = e->next;
            push_done(s, e->token);
            free_entry(e);
            e = n;
        }
    }
    s->head = s->tail = s->loop_start = NULL;
    s->loop_left = 0;
    s->pos = 0;
    s->played = 0;
    unlock(s->a);
}

void audio_stream_break_loop(audio_stream *s) {
    lock(s->a);
    if (s->loop_start) s->loop_left = 1;
    unlock(s->a);
}

uint64_t audio_stream_position(audio_stream *s) {
    uint64_t p;
    lock(s->a);
    p = s->played + (s->head ? s->pos >> 16 : 0);
    unlock(s->a);
    return p;
}

bool audio_stream_busy(audio_stream *s) {
    bool b;
    lock(s->a);
    b = s->head != NULL;
    unlock(s->a);
    return b;
}

void audio_stream_set_volume(audio_stream *s, uint16_t left, uint16_t right) {
    lock(s->a);
    s->vol_l = left;
    s->vol_r = right;
    unlock(s->a);
}

/* The playhead ran off the end of head: move on, loop back, or leave the loop (finishing all of its buffers). */
static void finish_head(audio_stream *s, uint64_t over) {
    entry *e = s->head;
    s->played += e->frames;
    s->pos = over;
    if (!s->loop_start && (e->flags & AUDIO_LOOP_BEGIN)) {
        s->loop_start = e;
        s->loop_left = e->loops ? e->loops : 1;
    }
    if (s->loop_start) {
        if (!(e->flags & AUDIO_LOOP_END)) {
            s->head = e->next;
            return;
        }
        if (s->loop_left > 1) {
            if (s->loop_left != 0xFFFFFFFFu) s->loop_left--;
            s->head = s->loop_start;
            return;
        }
        {
            entry *x = s->loop_start, *last = e->next;
            for (;;) {
                entry *n = x->next;
                push_done(s, x->token);
                free_entry(x);
                if (x == e) break;
                x = n;
            }
            s->head = last;
            if (!last) s->tail = NULL;
            s->loop_start = NULL;
            s->loop_left = 0;
        }
        return;
    }
    s->head = e->next;
    if (!s->head) s->tail = NULL;
    push_done(s, e->token);
    free_entry(e);
}

static void settle(audio_stream *s) {
    unsigned guard = 64;
    while (s->head && (s->pos >> 16) >= s->head->frames && guard--)
        finish_head(s, s->pos - ((uint64_t)s->head->frames << 16));
    if (!guard && s->head) s->pos = 0;      /* a loop of empty buffers: stop spinning */
}

static int16_t clamp16(int64_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

void audio_mix(audio_t *a, int16_t *out, size_t frames) {
    size_t f, i;
    for (i = 0; i < a->nstreams; i++) settle(a->streams[i]);
    for (f = 0; f < frames; f++) {
        int64_t sl = 0, sr = 0;
        for (i = 0; i < a->nstreams; i++) {
            audio_stream *s = a->streams[i];
            const entry *e = s->head;
            const int16_t *p, *n;
            int64_t l, r;
            unsigned ch = s->channels;
            uint64_t fr;
            if (s->paused || !e) continue;
            p = e->pcm + (s->pos >> 16) * ch;
            n = (s->pos >> 16) + 1 < e->frames ? p + ch : p;
            fr = s->pos & 0xFFFF;
            l = p[0] + (((int64_t)(n[0] - p[0]) * (int64_t)fr) >> 16);
            r = ch == 2 ? p[1] + (((int64_t)(n[1] - p[1]) * (int64_t)fr) >> 16) : l;
            sl += l * s->vol_l / 65535;
            sr += r * s->vol_r / 65535;
            s->pos += ((uint64_t)s->rate << 16) / a->cfg.rate;
            settle(s);
        }
        if (out) {
            out[2 * f] = clamp16(sl * a->vol_l / 65535);
            out[2 * f + 1] = clamp16(sr * a->vol_r / 65535);
        }
    }
    a->mixed += frames;
}

static uint32_t host_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* Real-time watchdog: frames the device should have pulled by now_ms against the frames mixed since the first look. */
static void guard(audio_t *a, uint32_t now_ms) {
    uint64_t expect, got, slack;
    a->seen_ms = now_ms;
    a->seen_host = host_ms();
    if (!a->guard_origin) {
        a->guard_origin = true;
        a->guard_ms = now_ms;
        a->guard_mixed = a->mixed;
        return;
    }
    if (now_ms - a->guard_ms > 0x80000000u) return;                  /* a clock that went back */
    expect = (uint64_t)(now_ms - a->guard_ms) * a->cfg.rate / 1000;
    got = a->mixed - a->guard_mixed;
    slack = (uint64_t)a->cfg.stall_ms * a->cfg.rate / 1000;
    for (; expect > got + slack;) {                                  /* chunks keep the work between locks short */
        uint64_t n = expect - got - slack;
        if (n > 4096) n = 4096;
        lock(a);
        audio_mix(a, NULL, (size_t)n);
        unlock(a);
        got += n;
    }
}

void audio_advance(audio_t *a, uint32_t now_ms) {
    uint64_t acc;
    uint32_t dt;
    if (a->cfg.real_time) {
        guard(a, now_ms);
        return;
    }
    if (!a->have_origin) {
        a->have_origin = true;
        a->last_ms = now_ms;
        return;
    }
    dt = now_ms - a->last_ms;
    if (dt == 0 || dt > 0x80000000u) return;       /* same instant, or a clock that went back */
    a->last_ms = now_ms;
    acc = (uint64_t)dt * a->cfg.rate + a->carry;
    a->carry = (uint32_t)(acc % 1000);
    /* long gaps are consumed in chunks so the stack buffer stays small */
    for (acc /= 1000; acc;) {
        size_t n = acc > 4096 ? 4096 : (size_t)acc;
        lock(a);
        audio_mix(a, NULL, n);
        unlock(a);
        acc -= n;
    }
}

void audio_stream_wait(audio_stream *s) {
    audio_t *a = s->a;
    while (audio_stream_busy(s)) {
        if (a->cfg.real_time) {
            if (!a->cfg.wait_ms) break;
            a->cfg.wait_ms(a->cfg.sink_ctx, 5);
            if (a->cfg.now_ms) guard(a, a->cfg.now_ms(a->cfg.sink_ctx));
            else if (a->guard_origin) guard(a, a->seen_ms + (host_ms() - a->seen_host));   /* the program clock, advanced by the host's */
        } else {
            lock(a);
            audio_mix(a, NULL, a->cfg.rate / 100 + 1);
            unlock(a);
        }
    }
}
