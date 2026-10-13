/* waveOut, sndPlaySound and the error texts. See mmsystem.h.
 *
 * A device is one stream of the audio engine. waveOutWrite decodes the buffer (PCM as is, Microsoft ADPCM through
 * the "mapper": the card itself, device 0, plays PCM only, WAVE_MAPPER 0xFFFF converts) into the stream queue and
 * remembers the header; when the engine reports the buffer played, the header's flags get WHDR_DONE and the
 * callback is delivered. WAVEHDR (32 bytes): lpData 0, dwBufferLength 4, dwBytesRecorded 8, dwUser 12, dwFlags 16,
 * dwLoops 20, lpNext 24, reserved 28. */
#include "mm_priv.h"

#include <stdlib.h>
#include <string.h>

#define WAVE_MAPPER 0xFFFFu
#define WAVE_FORMAT_QUERY 1u
#define HWAVE_BASE 0x0E00u

#define SND_ASYNC 1u
#define SND_NODEFAULT 2u
#define SND_MEMORY 4u
#define SND_LOOP 8u
#define SND_NOSTOP 16u

/* An error answer, shown with --trace. */
static void fail(hle_call *c, mmsystem_t *m, unsigned err) {
    if (m->log && m->hle->trace)
        fprintf(m->log, "mmsystem: %s failed with error %u (called from %04x:%04x)\n", c->entry->name, err, c->caller_cs,
                c->caller_ip);
    hle_ret16(c, (uint16_t)err);
}

static mm_wave *wave_of(mmsystem_t *m, uint16_t h) {
    unsigned i;
    for (i = 0; i < MM_MAX_WAVE; i++)
        if (m->waves[i].used && m->waves[i].handle == h) return &m->waves[i];
    return NULL;
}

/* A device id is 0, the mapper or an open handle; volume and position calls take either. */
static bool device_ok(mmsystem_t *m, uint16_t id) { return id == 0 || id == WAVE_MAPPER || wave_of(m, id); }

/* ---- completion ------------------------------------------------------------- */

static void drain(mmsystem_t *m, mm_wave *w) {
    uint32_t tok;
    while (w->used && audio_stream_pop_done(w->s, &tok)) {
        mm_write *wr;
        uint8_t *hp;
        uint32_t hdr;
        if (tok >= MM_MAX_PENDING || !m->writes[tok].used) continue;
        wr = &m->writes[tok];
        hdr = wr->hdr;
        wr->used = false;
        w->pending--;
        if ((hp = mem_ptr(&m->p->mem, (uint16_t)(hdr >> 16), hdr & 0xFFFF, 32)))
            mm_st32(hp + 16, (mm_ld32(hp + 16) & ~WHDR_INQUEUE) | WHDR_DONE);
        mm_callback(m, w->cb, w->flags, w->handle, MM_WOM_DONE, w->inst, hdr, 0);
    }
}

void mm_wave_tick(mmsystem_t *m) {
    unsigned i;
    for (i = 0; i < MM_MAX_WAVE; i++)
        if (m->waves[i].used) drain(m, &m->waves[i]);
    if (m->snd && !audio_stream_busy(m->snd)) {
        audio_stream_close(m->snd);
        m->snd = NULL;
    }
}

void mm_wave_shutdown(mmsystem_t *m) {
    unsigned i;
    for (i = 0; i < MM_MAX_WAVE; i++)
        if (m->waves[i].used) audio_stream_close(m->waves[i].s);
    if (m->snd) audio_stream_close(m->snd);
    m->snd = NULL;
}

/* ---- devices ------------------------------------------------------------------ */

static cpu_status w_getnumdevs(hle_call *c) {
    MM_BEGIN;
    hle_ret16(c, m->audio ? 1 : 0);
    return CPU_OK;
}

static cpu_status w_getdevcaps(hle_call *c) {
    MM_BEGIN;
    uint16_t id, sel, off, size, n;
    uint8_t caps[48] = {0}, *p;
    id = hle_u16(&a);
    hle_far(&a, &sel, &off);
    size = hle_u16(&a);
    MM_CHECK;
    if (!m->audio || (id != 0 && id != WAVE_MAPPER)) {
        fail(c, m, MMSYSERR_BADDEVICEID);
        return CPU_OK;
    }
    mm_st16(caps, 1);                          /* wMid: MM_MICROSOFT */
    mm_st16(caps + 2, 0x20);                   /* wPid */
    mm_st16(caps + 4, 0x0100);                 /* vDriverVersion */
    strcpy((char *)caps + 6, "win16rt wave output");
    mm_st32(caps + 38, 0xFFF);                 /* 11.025, 22.05, 44.1 kHz, mono/stereo, 8/16 bit (and 48, 96 kHz class: all) */
    mm_st16(caps + 42, 2);
    mm_st32(caps + 44, 0x0C);                  /* WAVECAPS_VOLUME | WAVECAPS_LRVOLUME */
    n = size < sizeof caps ? size : (uint16_t)sizeof caps;
    if (n && !(p = hle_ptr(c, sel, off, n))) return CPU_E_SEL;
    if (n) memcpy(hle_ptr(c, sel, off, n), caps, n);
    hle_ret16(c, MMSYSERR_NOERROR);
    return CPU_OK;
}

/* PCMWAVEFORMAT or WAVEFORMATEX at the far pointer; false when it cannot be read. */
static bool read_format(hle_call *c, uint16_t sel, uint16_t off, wav_format *f) {
    const uint8_t *p = hle_ptr(c, sel, off, 16), *x;
    unsigned len = 16;
    if (!p) return false;
    if (mm_ld16(p) == WAVE_FORMAT_ADPCM && (x = hle_ptr(c, sel, off, 18))) {
        len = 18 + mm_ld16(x + 16);
        if (len > 200) len = 200;
        if (!(p = hle_ptr(c, sel, off, len))) return false;
    }
    return wav_format_parse(p, len, f) == WAV_OK;
}

static cpu_status w_open(hle_call *c) {
    MM_BEGIN;
    uint16_t hsel, hoff, id, fsel, foff;
    uint32_t cb, inst, flags;
    wav_format f;
    mm_wave *w = NULL;
    uint8_t *hp = NULL;
    unsigned i;
    hle_far(&a, &hsel, &hoff);
    id = hle_u16(&a);
    hle_far(&a, &fsel, &foff);
    cb = hle_u32(&a);
    inst = hle_u32(&a);
    flags = hle_u32(&a);
    MM_CHECK;
    MM_TICK;
    if (!m->audio || (id != 0 && id != WAVE_MAPPER)) {
        fail(c, m, MMSYSERR_BADDEVICEID);
        return CPU_OK;
    }
    if (!read_format(c, fsel, foff, &f) || !wav_format_supported(&f) || (id == 0 && f.tag != WAVE_FORMAT_PCM)) {
        fail(c, m, WAVERR_BADFORMAT);
        return CPU_OK;
    }
    if (flags & WAVE_FORMAT_QUERY) {
        hle_ret16(c, MMSYSERR_NOERROR);
        return CPU_OK;
    }
    if (hsel && !(hp = hle_ptr(c, hsel, hoff, 2))) return CPU_E_SEL;
    if (!hp) {
        fail(c, m, MMSYSERR_INVALPARAM);
        return CPU_OK;
    }
    for (i = 0; i < MM_MAX_WAVE && m->waves[i].used; i++) {}
    if (i == MM_MAX_WAVE) {
        fail(c, m, MMSYSERR_ALLOCATED);
        return CPU_OK;
    }
    w = &m->waves[i];
    memset(w, 0, sizeof *w);
    if (!(w->s = audio_stream_open(m->audio, f.rate, f.channels))) {
        fail(c, m, MMSYSERR_NOMEM);
        return CPU_OK;
    }
    do {
        if (++m->next_wave < HWAVE_BASE || m->next_wave >= HWAVE_BASE + 0x100) m->next_wave = HWAVE_BASE + 1;
    } while (wave_of(m, m->next_wave));
    w->used = true;
    w->handle = m->next_wave;
    w->fmt = f;
    w->cb = cb;
    w->inst = inst;
    w->flags = flags;
    mm_st16(hp, w->handle);
    if (m->log && m->hle->trace)
        fprintf(m->log, "mmsystem: waveOutOpen device %04x tag %u %u Hz %u ch %u bit, callback type %x -> %04x\n", id, f.tag,
                (unsigned)f.rate, f.channels, f.bits, (unsigned)(flags & CALLBACK_TYPEMASK), w->handle);
    mm_callback(m, cb, flags, w->handle, MM_WOM_OPEN, inst, 0, 0);
    if (user_abort(m->u)) return user_abort(m->u);
    hle_ret16(c, MMSYSERR_NOERROR);
    return CPU_OK;
}

static cpu_status w_close(hle_call *c) {
    MM_BEGIN;
    uint16_t h = hle_u16(&a);
    mm_wave *w;
    MM_CHECK;
    MM_TICK;
    if (!(w = wave_of(m, h))) {
        fail(c, m, MMSYSERR_INVALHANDLE);
        return CPU_OK;
    }
    if (w->pending) {
        fail(c, m, WAVERR_STILLPLAYING);
        return CPU_OK;
    }
    audio_stream_close(w->s);
    w->used = false;
    mm_callback(m, w->cb, w->flags, h, MM_WOM_CLOSE, w->inst, 0, 0);
    if (user_abort(m->u)) return user_abort(m->u);
    hle_ret16(c, MMSYSERR_NOERROR);
    return CPU_OK;
}

/* ---- buffers ------------------------------------------------------------------ */

static cpu_status w_prepare(hle_call *c) {
    MM_BEGIN;
    uint16_t h = hle_u16(&a), sel, off, size;
    uint8_t *hp;
    hle_far(&a, &sel, &off);
    size = hle_u16(&a);
    MM_CHECK;
    if (!wave_of(m, h)) {
        fail(c, m, MMSYSERR_INVALHANDLE);
        return CPU_OK;
    }
    if (size < 32 || !(hp = hle_ptr(c, sel, off, 32))) {
        fail(c, m, MMSYSERR_INVALPARAM);
        return CPU_OK;
    }
    mm_st32(hp + 16, (mm_ld32(hp + 16) | WHDR_PREPARED) & ~(WHDR_DONE | WHDR_INQUEUE));
    hle_ret16(c, MMSYSERR_NOERROR);
    return CPU_OK;
}

static cpu_status w_unprepare(hle_call *c) {
    MM_BEGIN;
    uint16_t h = hle_u16(&a), sel, off, size;
    uint8_t *hp;
    hle_far(&a, &sel, &off);
    size = hle_u16(&a);
    MM_CHECK;
    MM_TICK;
    if (!wave_of(m, h)) {
        fail(c, m, MMSYSERR_INVALHANDLE);
        return CPU_OK;
    }
    if (size < 32 || !(hp = hle_ptr(c, sel, off, 32))) {
        fail(c, m, MMSYSERR_INVALPARAM);
        return CPU_OK;
    }
    if (mm_ld32(hp + 16) & WHDR_INQUEUE) {
        fail(c, m, WAVERR_STILLPLAYING);
        return CPU_OK;
    }
    mm_st32(hp + 16, mm_ld32(hp + 16) & ~WHDR_PREPARED);
    hle_ret16(c, MMSYSERR_NOERROR);
    return CPU_OK;
}

static cpu_status w_write(hle_call *c) {
    MM_BEGIN;
    uint16_t h = hle_u16(&a), sel, off, size, tok;
    uint8_t *hp;
    const uint8_t *dp = NULL;
    uint32_t flags, len, data, loops;
    unsigned lf = 0;
    int16_t *pcm = NULL;
    size_t frames = 0;
    mm_wave *w;
    hle_far(&a, &sel, &off);
    size = hle_u16(&a);
    MM_CHECK;
    MM_TICK;
    if (!(w = wave_of(m, h))) {
        fail(c, m, MMSYSERR_INVALHANDLE);
        return CPU_OK;
    }
    if (size < 32 || !(hp = hle_ptr(c, sel, off, 32))) {
        fail(c, m, MMSYSERR_INVALPARAM);
        return CPU_OK;
    }
    flags = mm_ld32(hp + 16);
    if (!(flags & WHDR_PREPARED)) {
        fail(c, m, WAVERR_UNPREPARED);
        return CPU_OK;
    }
    if (flags & WHDR_INQUEUE) {
        fail(c, m, WAVERR_STILLPLAYING);
        return CPU_OK;
    }
    data = mm_ld32(hp);
    len = mm_ld32(hp + 4);
    loops = mm_ld32(hp + 20);
    if (len && !(dp = hle_ptr(c, (uint16_t)(data >> 16), data & 0xFFFF, len))) {
        fail(c, m, MMSYSERR_INVALPARAM);
        return CPU_OK;
    }
    for (tok = 0; tok < MM_MAX_PENDING && m->writes[tok].used; tok++) {}
    if (tok == MM_MAX_PENDING) {
        fail(c, m, MMSYSERR_NOMEM);
        return CPU_OK;
    }
    if (len) frames = wav_decode(&w->fmt, dp, len, &pcm);
    if (flags & WHDR_BEGINLOOP) lf |= AUDIO_LOOP_BEGIN;
    if (flags & WHDR_ENDLOOP) lf |= AUDIO_LOOP_END;
    m->writes[tok] = (mm_write){true, h, (uint32_t)sel << 16 | off};
    w->pending++;
    mm_st32(hp + 16, (flags | WHDR_INQUEUE) & ~WHDR_DONE);
    if (!audio_stream_queue(w->s, pcm, frames, lf, loops, tok)) {
        m->writes[tok].used = false;
        w->pending--;
        mm_st32(hp + 16, flags);
        fail(c, m, MMSYSERR_NOMEM);
        return CPU_OK;
    }
    if (m->log && m->hle->trace)
        fprintf(m->log, "mmsystem: waveOutWrite %04x: %u bytes -> %u frames, flags %x loops %u\n", h, (unsigned)len,
                (unsigned)frames, (unsigned)flags, (unsigned)loops);
    hle_ret16(c, MMSYSERR_NOERROR);
    return CPU_OK;
}

/* One-handle calls: reset, pause, restart, break loop. */
static cpu_status w_simple(hle_call *c, int op) {
    MM_BEGIN;
    uint16_t h = hle_u16(&a);
    mm_wave *w;
    MM_CHECK;
    MM_TICK;
    if (!(w = wave_of(m, h))) {
        fail(c, m, MMSYSERR_INVALHANDLE);
        return CPU_OK;
    }
    switch (op) {
    case 0:
        audio_stream_reset(w->s);
        drain(m, w);
        break;
    case 1: audio_stream_pause(w->s, true); break;
    case 2: audio_stream_pause(w->s, false); break;
    default: audio_stream_break_loop(w->s); break;
    }
    if (user_abort(m->u)) return user_abort(m->u);
    hle_ret16(c, MMSYSERR_NOERROR);
    return CPU_OK;
}

static cpu_status w_reset(hle_call *c) { return w_simple(c, 0); }
static cpu_status w_pause(hle_call *c) { return w_simple(c, 1); }
static cpu_status w_restart(hle_call *c) { return w_simple(c, 2); }
static cpu_status w_breakloop(hle_call *c) { return w_simple(c, 3); }

static cpu_status w_getposition(hle_call *c) {
    MM_BEGIN;
    uint16_t h = hle_u16(&a), sel, off, size;
    uint8_t *p;
    mm_wave *w;
    uint64_t frames;
    hle_far(&a, &sel, &off);
    size = hle_u16(&a);
    MM_CHECK;
    MM_TICK;
    if (!(w = wave_of(m, h))) {
        fail(c, m, MMSYSERR_INVALHANDLE);
        return CPU_OK;
    }
    if (size < 6 || !(p = hle_ptr(c, sel, off, 6))) {
        fail(c, m, MMSYSERR_INVALPARAM);
        return CPU_OK;
    }
    frames = audio_stream_position(w->s);
    switch (mm_ld16(p)) {
    case 1: mm_st32(p + 2, (uint32_t)(frames * 1000 / w->fmt.rate)); break;
    case 2: mm_st32(p + 2, (uint32_t)frames); break;
    default:                                           /* TIME_BYTES, also the answer to a format that is not kept */
        mm_st16(p, 4);
        mm_st32(p + 2, (uint32_t)(w->fmt.tag == WAVE_FORMAT_PCM ? frames * w->fmt.block_align
                                                                 : frames * w->fmt.avg_bytes / w->fmt.rate));
        break;
    }
    hle_ret16(c, MMSYSERR_NOERROR);
    return CPU_OK;
}

/* ---- volume ------------------------------------------------------------------- */

static cpu_status w_getvolume(hle_call *c) {
    MM_BEGIN;
    uint16_t id = hle_u16(&a), sel, off, l, r;
    uint8_t *p;
    hle_far(&a, &sel, &off);
    MM_CHECK;
    if (!m->audio || !device_ok(m, id)) {
        fail(c, m, MMSYSERR_BADDEVICEID);
        return CPU_OK;
    }
    if (!(p = hle_ptr(c, sel, off, 4))) return CPU_E_SEL;
    audio_get_volume(m->audio, &l, &r);
    mm_st32(p, (uint32_t)r << 16 | l);
    hle_ret16(c, MMSYSERR_NOERROR);
    return CPU_OK;
}

static cpu_status w_setvolume(hle_call *c) {
    MM_BEGIN;
    uint16_t id = hle_u16(&a);
    uint32_t v = hle_u32(&a);
    MM_CHECK;
    if (!m->audio || !device_ok(m, id)) {
        fail(c, m, MMSYSERR_BADDEVICEID);
        return CPU_OK;
    }
    audio_set_volume(m->audio, (uint16_t)v, (uint16_t)(v >> 16));
    hle_ret16(c, MMSYSERR_NOERROR);
    return CPU_OK;
}

static cpu_status w_getid(hle_call *c) {
    MM_BEGIN;
    uint16_t h = hle_u16(&a), sel, off;
    uint8_t *p;
    hle_far(&a, &sel, &off);
    MM_CHECK;
    if (!wave_of(m, h)) {
        fail(c, m, MMSYSERR_INVALHANDLE);
        return CPU_OK;
    }
    if (!(p = hle_ptr(c, sel, off, 2))) return CPU_E_SEL;
    mm_st16(p, 0);
    hle_ret16(c, MMSYSERR_NOERROR);
    return CPU_OK;
}

/* Pitch and playback rate (the device does not claim WAVECAPS_PITCH / PLAYBACKRATE), and the driver message. */
static cpu_status w_notsupported(hle_call *c) {
    hle_ret32(c, MMSYSERR_NOTSUPPORTED);
    return CPU_OK;
}

/* ---- error texts (waveOut/waveIn/midiOut/midiIn GetErrorText share them) -------- */

const char *mm_error_text(unsigned err) {
    switch (err) {
    case MMSYSERR_ERROR: return "Unspecified error.";
    case MMSYSERR_BADDEVICEID: return "A device ID has been used that is out of range for your system.";
    case MMSYSERR_NOTENABLED: return "The driver was not enabled.";
    case MMSYSERR_ALLOCATED: return "The specified device is already in use. Wait until it is free, and then try again.";
    case MMSYSERR_INVALHANDLE: return "The specified device handle is invalid.";
    case MMSYSERR_NODRIVER: return "There is no driver installed on your system.";
    case MMSYSERR_NOMEM: return "Not enough memory available for this task. Quit one or more applications to increase available memory, and then try again.";
    case MMSYSERR_NOTSUPPORTED: return "This function is not supported.";
    case MMSYSERR_BADERRNUM: return "An error number was specified that is not defined in the system.";
    case MMSYSERR_INVALFLAG: return "An invalid flag was passed to a system function.";
    case MMSYSERR_INVALPARAM: return "An invalid parameter was passed to a system function.";
    case WAVERR_BADFORMAT: return "The specified format is not supported or cannot be translated.";
    case WAVERR_STILLPLAYING: return "The wave header is still playing.";
    case WAVERR_UNPREPARED: return "The wave header is not prepared.";
    case WAVERR_SYNC: return "Cannot open the device without using the WAVE_ALLOWSYNC flag.";
    default: return NULL;
    }
}

static cpu_status w_geterrortext(hle_call *c) {
    MM_BEGIN;
    uint16_t err = hle_u16(&a), sel, off, len;
    const char *t;
    char *dst;
    size_t n;
    hle_far(&a, &sel, &off);
    len = hle_u16(&a);
    MM_CHECK;
    if (!(t = mm_error_text(err)) && err != MMSYSERR_NOERROR) {
        fail(c, m, MMSYSERR_BADERRNUM);
        return CPU_OK;
    }
    if (!t) t = "No error.";
    if (len) {
        n = strlen(t);
        if (n > (size_t)len - 1u) n = len - 1u;
        if (!(dst = hle_ptr(c, sel, off, (uint32_t)n + 1))) return CPU_E_SEL;
        memcpy(dst, t, n);
        dst[n] = 0;
    }
    hle_ret16(c, MMSYSERR_NOERROR);
    return CPU_OK;
}

/* ---- sndPlaySound -------------------------------------------------------------- */

static void snd_stop(mmsystem_t *m) {
    if (m->snd) audio_stream_close(m->snd);
    m->snd = NULL;
}

static uint8_t *read_host_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *b = NULL;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && n < (16L << 20) && fseek(f, 0, SEEK_SET) == 0 &&
        (b = malloc((size_t)n)) && fread(b, 1, (size_t)n, f) == (size_t)n)
        *len = (size_t)n;
    else {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

static cpu_status s_sndplaysound(hle_call *c) {
    MM_BEGIN;
    uint16_t sel, off, flags;
    const uint8_t *img = NULL;
    uint8_t *own = NULL;
    size_t len = 0;
    wav_format f;
    const uint8_t *data;
    size_t dlen, frames;
    int16_t *pcm = NULL;
    audio_stream *s;
    hle_far(&a, &sel, &off);
    flags = hle_u16(&a);
    MM_CHECK;
    MM_TICK;
    hle_ret16(c, 0);
    if (!sel && !off) {                                 /* sndPlaySound(NULL, ...): stop the sound */
        snd_stop(m);
        hle_ret16(c, 1);
        return CPU_OK;
    }
    if ((flags & SND_NOSTOP) && m->snd && audio_stream_busy(m->snd)) return CPU_OK;
    snd_stop(m);
    if (!m->audio) return CPU_OK;
    if (flags & SND_MEMORY) {
        const uint8_t *h = hle_ptr(c, sel, off, 12);
        if (!h || memcmp(h, "RIFF", 4)) return CPU_OK;
        len = 8 + (size_t)mm_ld32(h + 4);
        if (!(img = hle_ptr(c, sel, off, (uint32_t)len))) return CPU_OK;
    } else {
        char name[260], host[1024];
        if (!hle_read_cstr(c, sel, off, name, sizeof name) || !m->k || !kernel_host_path(m->k, name, host, sizeof host)) {
            if (m->log) fprintf(m->log, "mmsystem: sndPlaySound: no such sound\n");
            return CPU_OK;
        }
        if (!(own = read_host_file(host, &len))) return CPU_OK;
        img = own;
    }
    if (wav_parse(img, len, &f, &data, &dlen) != WAV_OK || !wav_format_supported(&f) ||
        !(frames = wav_decode(&f, data, dlen, &pcm))) {
        free(own);
        free(pcm);
        return CPU_OK;
    }
    free(own);
    if (!(s = audio_stream_open(m->audio, f.rate, f.channels))) {
        free(pcm);
        return CPU_OK;
    }
    if (m->log && m->hle->trace)
        fprintf(m->log, "mmsystem: sndPlaySound flags %x: tag %u %u Hz %u ch -> %u frames\n", flags, f.tag, (unsigned)f.rate,
                f.channels, (unsigned)frames);
    if ((flags & SND_LOOP) && (flags & SND_ASYNC)) audio_stream_queue(s, pcm, frames, AUDIO_LOOP_BEGIN | AUDIO_LOOP_END, 0xFFFFFFFFu, 0);
    else audio_stream_queue(s, pcm, frames, 0, 0, 0);
    if (flags & SND_ASYNC) {
        m->snd = s;
    } else {
        audio_stream_wait(s);
        audio_stream_close(s);
    }
    hle_ret16(c, 1);
    return CPU_OK;
}

bool mm_wave_register(mmsystem_t *m) {
    static const hle_def defs[] = {
        {MM, 2, "sndPlaySound", 6, s_sndplaysound},
        {MM, 203, "midiOutGetErrorText", 8, w_geterrortext},
        {MM, 303, "midiInGetErrorText", 8, w_geterrortext},
        {MM, 401, "waveOutGetNumDevs", 0, w_getnumdevs},
        {MM, 402, "waveOutGetDevCaps", 8, w_getdevcaps},
        {MM, 403, "waveOutGetErrorText", 8, w_geterrortext},
        {MM, 404, "waveOutOpen", 22, w_open},
        {MM, 405, "waveOutClose", 2, w_close},
        {MM, 406, "waveOutPrepareHeader", 8, w_prepare},
        {MM, 407, "waveOutUnprepareHeader", 8, w_unprepare},
        {MM, 408, "waveOutWrite", 8, w_write},
        {MM, 409, "waveOutPause", 2, w_pause},
        {MM, 410, "waveOutRestart", 2, w_restart},
        {MM, 411, "waveOutReset", 2, w_reset},
        {MM, 412, "waveOutGetPosition", 8, w_getposition},
        {MM, 413, "waveOutGetPitch", 6, w_notsupported},
        {MM, 414, "waveOutSetPitch", 6, w_notsupported},
        {MM, 415, "waveOutGetVolume", 6, w_getvolume},
        {MM, 416, "waveOutSetVolume", 6, w_setvolume},
        {MM, 417, "waveOutGetPlaybackRate", 6, w_notsupported},
        {MM, 418, "waveOutSetPlaybackRate", 6, w_notsupported},
        {MM, 419, "waveOutBreakLoop", 2, w_breakloop},
        {MM, 420, "waveOutGetID", 6, w_getid},
        {MM, 421, "waveOutMessage", 12, w_notsupported},
        {MM, 503, "waveInGetErrorText", 8, w_geterrortext},
    };
    return hle_register_table(m->hle, defs, sizeof defs / sizeof defs[0], m);
}
