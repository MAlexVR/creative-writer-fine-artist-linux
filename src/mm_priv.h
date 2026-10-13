#ifndef MM_PRIV_H
#define MM_PRIV_H
/* Internal to mmsystem.c, mm_wave.c and mm_mci.c. */
#include "mmsystem.h"
#include "wav.h"

#define MM "MMSYSTEM"

/* MMSYSERR / WAVERR / TIMERR */
#define MMSYSERR_NOERROR 0
#define MMSYSERR_ERROR 1
#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_NOTENABLED 3
#define MMSYSERR_ALLOCATED 4
#define MMSYSERR_INVALHANDLE 5
#define MMSYSERR_NODRIVER 6
#define MMSYSERR_NOMEM 7
#define MMSYSERR_NOTSUPPORTED 8
#define MMSYSERR_BADERRNUM 9
#define MMSYSERR_INVALFLAG 10
#define MMSYSERR_INVALPARAM 11
#define WAVERR_BADFORMAT 32
#define WAVERR_STILLPLAYING 33
#define WAVERR_UNPREPARED 34
#define WAVERR_SYNC 35
#define TIMERR_NOCANDO 97
#define TIMERR_STRUCT 129

/* callback types of the open calls (dwFlags) and of DriverCallback (uFlags) */
#define CALLBACK_TYPEMASK 0x70000u
#define CALLBACK_WINDOW 0x10000u
#define CALLBACK_TASK 0x20000u
#define CALLBACK_FUNCTION 0x30000u

#define MM_MCINOTIFY 0x3B9
#define MM_WOM_OPEN 0x3BB
#define MM_WOM_CLOSE 0x3BC
#define MM_WOM_DONE 0x3BD

#define WHDR_DONE 1u
#define WHDR_PREPARED 2u
#define WHDR_BEGINLOOP 4u
#define WHDR_ENDLOOP 8u
#define WHDR_INQUEUE 0x10u

#define MM_MAX_WAVE 8
#define MM_MAX_PENDING 64
#define MM_MAX_TIMERS 16
#define MM_MAX_STUBS 64

typedef struct {
    bool used;
    uint16_t handle;
    audio_stream *s;
    wav_format fmt;
    uint32_t cb, inst, flags;     /* callback, instance data, open flags */
    uint32_t pending;             /* writes not yet reported done */
} mm_wave;

typedef struct {
    bool used;
    uint16_t hwo;
    uint32_t hdr;                 /* far pointer to the WAVEHDR */
} mm_write;

typedef struct {
    bool used, periodic;
    uint16_t id;
    uint32_t due, interval, proc, user;
} mm_timer;

typedef struct {
    unsigned ordinal;
    const char *name;
} mm_stub_rec;

struct mmsystem {
    hle_t *hle;
    user_t *u;
    kernel_t *k;
    loader_proc *p;
    audio_t *audio;
    FILE *log;
    bool in_tick;
    mm_wave waves[MM_MAX_WAVE];
    uint16_t next_wave;
    mm_write writes[MM_MAX_PENDING];
    mm_timer timers[MM_MAX_TIMERS];
    uint16_t next_timer;
    audio_stream *snd;            /* the sound sndPlaySound is playing */
    mm_stub_rec stubs[MM_MAX_STUBS];
    size_t nstubs;
    void *absent;                 /* contexts of the absent-device stubs */
};

static inline uint16_t mm_ld16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t mm_ld32(const uint8_t *p) { return mm_ld16(p) | (uint32_t)mm_ld16(p + 2) << 16; }
static inline void mm_st16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void mm_st32(uint8_t *p, uint32_t v) { mm_st16(p, (uint16_t)v); mm_st16(p + 2, (uint16_t)(v >> 16)); }

/* mmsystem.c */
void mm_tick(mmsystem_t *m);
uint32_t mm_now(mmsystem_t *m);
/* Delivers one device message: a window gets it posted, a function is called (dw1, dw2 as the API defines). */
void mm_callback(mmsystem_t *m, uint32_t cb, uint32_t type, uint16_t handle, uint16_t msg, uint32_t inst, uint32_t dw1,
                 uint32_t dw2);
void mm_note_stub(mmsystem_t *m, const hle_call *c);
#define MM_BEGIN mmsystem_t *m = c->ctx; hle_args a; hle_args_init(&a, c); (void)m
#define MM_CHECK do { if (a.st) return a.st; } while (0)
/* Runs due timers and completed audio, then reports a guest fault if one happened. */
#define MM_TICK do { mm_tick(m); if (user_abort(m->u)) return user_abort(m->u); } while (0)

/* mm_wave.c */
const char *mm_error_text(unsigned err);   /* MMSYSERR_* / WAVERR_* texts, NULL when unknown */
bool mm_wave_register(mmsystem_t *m);
void mm_wave_tick(mmsystem_t *m);
void mm_wave_shutdown(mmsystem_t *m);

/* mm_mci.c */
bool mm_mci_register(mmsystem_t *m);

#endif
