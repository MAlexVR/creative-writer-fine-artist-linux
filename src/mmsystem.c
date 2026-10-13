/* MMSYSTEM HLE: core, timers and the devices that are not there. See mmsystem.h.
 *
 * Ordinals and argument byte counts are the Win 3.1 MMSYSTEM export list. The 22 ordinals WRITER
 * imports pair one-to-one with the names in its import table, so those are VERIFIED; ARTIST imports the same ones minus 607. */
#include "mm_priv.h"

#include <stdlib.h>
#include <string.h>

uint32_t mm_now(mmsystem_t *m) { return user_now(m->u); }

void mm_callback(mmsystem_t *m, uint32_t cb, uint32_t type, uint16_t handle, uint16_t msg, uint32_t inst, uint32_t dw1,
                 uint32_t dw2) {
    switch (type & CALLBACK_TYPEMASK) {
    case CALLBACK_WINDOW:
        wm_post(user_wm(m->u), (uint16_t)cb, msg, handle, dw1);
        break;
    case CALLBACK_TASK:
        wm_post(user_wm(m->u), 0, msg, handle, dw1);
        break;
    case CALLBACK_FUNCTION:
        if (cb >> 16) {
            uint16_t args[8] = {handle, msg, (uint16_t)(inst >> 16), (uint16_t)inst, (uint16_t)(dw1 >> 16),
                                (uint16_t)dw1, (uint16_t)(dw2 >> 16), (uint16_t)dw2};
            uint32_t ret;
            user_call_guest(m->u, cb, args, 8, &ret);
        }
        break;
    default:
        break;
    }
}

/* ---- delivery of what is due ------------------------------------------------ */

static void fire_timers(mmsystem_t *m) {
    for (;;) {
        mm_timer *best = NULL, t;
        uint32_t now = mm_now(m);
        unsigned i;
        uint16_t args[8];
        uint32_t ret;
        for (i = 0; i < MM_MAX_TIMERS; i++) {
            mm_timer *x = &m->timers[i];
            if (x->used && (int32_t)(now - x->due) >= 0 && (!best || (int32_t)(x->due - best->due) < 0)) best = x;
        }
        if (!best) return;
        t = *best;
        if (t.periodic) {
            best->due += best->interval;
            if ((int32_t)(now - best->due) >= 0) best->due = now + best->interval;
        } else {
            best->used = false;
        }
        args[0] = t.id;
        args[1] = 0;
        args[2] = (uint16_t)(t.user >> 16);
        args[3] = (uint16_t)t.user;
        args[4] = args[5] = args[6] = args[7] = 0;
        if (!(t.proc >> 16)) continue;
        if (!user_call_guest(m->u, t.proc, args, 8, &ret)) return;
    }
}

void mm_tick(mmsystem_t *m) {
    if (m->in_tick || !m->p) return;
    m->in_tick = true;
    if (m->audio) audio_advance(m->audio, mm_now(m));
    mm_wave_tick(m);
    fire_timers(m);
    m->in_tick = false;
}

static void tick_hook(void *ctx) { mm_tick(ctx); }

/* ---- timing ----------------------------------------------------------------- */

static cpu_status t_gettime(hle_call *c) {
    MM_BEGIN;
    MM_TICK;
    user_refresh(m->u);
    hle_ret32(c, mm_now(m));
    return CPU_OK;
}

static cpu_status t_getsystemtime(hle_call *c) {
    MM_BEGIN;
    uint16_t sel, off, size;
    uint8_t *p;
    hle_far(&a, &sel, &off);
    size = hle_u16(&a);
    MM_CHECK;
    MM_TICK;
    if (size >= 6 && (p = hle_ptr(c, sel, off, 6))) {
        mm_st16(p, 1);                      /* TIME_MS */
        mm_st32(p + 2, mm_now(m));
    }
    return CPU_OK;
}

static cpu_status t_getdevcaps(hle_call *c) {
    MM_BEGIN;
    uint16_t sel, off, size;
    uint8_t *p;
    hle_far(&a, &sel, &off);
    size = hle_u16(&a);
    MM_CHECK;
    if (size < 4) {
        hle_ret16(c, TIMERR_STRUCT);
        return CPU_OK;
    }
    if (!(p = hle_ptr(c, sel, off, 4))) return CPU_E_SEL;
    mm_st16(p, 1);                          /* wPeriodMin */
    mm_st16(p + 2, 0xFFFF);                 /* wPeriodMax */
    hle_ret16(c, 0);
    return CPU_OK;
}

static cpu_status t_period(hle_call *c) {
    MM_BEGIN;
    uint16_t period = hle_u16(&a);
    MM_CHECK;
    hle_ret16(c, period >= 1 ? MMSYSERR_NOERROR : TIMERR_NOCANDO);
    return CPU_OK;
}

static cpu_status t_setevent(hle_call *c) {
    MM_BEGIN;
    uint16_t delay, res, flags;
    uint32_t proc, user;
    unsigned i;
    hle_args_init(&a, c);
    delay = hle_u16(&a);
    res = hle_u16(&a);
    proc = hle_u32(&a);
    user = hle_u32(&a);
    flags = hle_u16(&a);
    MM_CHECK;
    (void)res;
    MM_TICK;
    hle_ret16(c, 0);
    if (!proc) return CPU_OK;
    for (i = 0; i < MM_MAX_TIMERS; i++) {
        mm_timer *t = &m->timers[i];
        if (t->used) continue;
        if (++m->next_timer == 0) m->next_timer = 1;
        t->used = true;
        t->periodic = flags & 1;
        t->id = m->next_timer;
        t->interval = delay ? delay : 1;
        t->due = mm_now(m) + t->interval;
        t->proc = proc;
        t->user = user;
        hle_ret16(c, t->id);
        break;
    }
    return CPU_OK;
}

static cpu_status t_killevent(hle_call *c) {
    MM_BEGIN;
    uint16_t id = hle_u16(&a);
    unsigned i;
    MM_CHECK;
    hle_ret16(c, TIMERR_NOCANDO);
    for (i = 0; i < MM_MAX_TIMERS; i++)
        if (m->timers[i].used && m->timers[i].id == id) {
            m->timers[i].used = false;
            hle_ret16(c, MMSYSERR_NOERROR);
        }
    return CPU_OK;
}

/* ---- housekeeping ------------------------------------------------------------ */

static cpu_status s_wep(hle_call *c) {
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status s_getversion(hle_call *c) {
    hle_ret16(c, 0x030A);
    return CPU_OK;
}

static cpu_status s_nop(hle_call *c) {
    (void)c;
    return CPU_OK;
}

static cpu_status s_debugstr(hle_call *c) {
    MM_BEGIN;
    uint16_t sel, off;
    char buf[256];
    hle_far(&a, &sel, &off);
    MM_CHECK;
    if (m->log && hle_read_cstr(c, sel, off, buf, sizeof buf)) fprintf(m->log, "mmsystem: OutputDebugStr: %s\n", buf);
    return CPU_OK;
}

/* DriverCallback(dwCallback, uFlags, hDevice, uMessage, dwUser, dw1, dw2): DCB_WINDOW 1, DCB_TASK 2, DCB_FUNCTION 3. */
static cpu_status s_drivercallback(hle_call *c) {
    MM_BEGIN;
    uint32_t cb, user, dw1, dw2;
    uint16_t flags, dev, msg;
    cb = hle_u32(&a);
    flags = hle_u16(&a);
    dev = hle_u16(&a);
    msg = hle_u16(&a);
    user = hle_u32(&a);
    dw1 = hle_u32(&a);
    dw2 = hle_u32(&a);
    MM_CHECK;
    hle_ret16(c, 0);
    if ((flags & 7) == 0 || (flags & 7) > 3) return CPU_OK;
    mm_callback(m, cb, (uint32_t)(flags & 7) << 16, dev, msg, user, dw1, dw2);
    if (user_abort(m->u)) return user_abort(m->u);
    hle_ret16(c, 1);
    return CPU_OK;
}

/* ---- devices that are not there ---------------------------------------------- */

/* No capture, MIDI, aux or joystick hardware exists in this runtime. WRITER and ARTIST do import nine waveIn
 * functions (they look for a microphone; without one they carry on), and nothing of MIDI, aux or the joystick.
 * They answer as a PC without the device does, which is what Windows 3.1 itself returns for a missing device: zero devices, a bad device
 * id for a device number, an invalid handle for a handle. Each call is logged and listed. */
typedef struct {
    mmsystem_t *m;
    uint16_t ret;
} stub_ctx;

static cpu_status s_absent(hle_call *c) {
    stub_ctx *s = c->ctx;
    mm_note_stub(s->m, c);
    hle_ret32(c, s->ret);
    return CPU_OK;
}

void mm_note_stub(mmsystem_t *m, const hle_call *c) {
    if (m->nstubs < MM_MAX_STUBS) {
        m->stubs[m->nstubs].ordinal = c->entry->ordinal;
        m->stubs[m->nstubs].name = c->entry->name;
        m->nstubs++;
    }
    if (m->log) fprintf(m->log, "mmsystem: %s.%u %s: no such device (called from %04x:%04x)\n", c->entry->module,
                        c->entry->ordinal, c->entry->name, c->caller_cs, c->caller_ip);
}

size_t mmsystem_stub_count(const mmsystem_t *m) { return m->nstubs; }

const char *mmsystem_stub_name(const mmsystem_t *m, size_t i, unsigned *ordinal) {
    if (i >= m->nstubs) return NULL;
    if (ordinal) *ordinal = m->stubs[i].ordinal;
    return m->stubs[i].name;
}

#define NODEV MMSYSERR_BADDEVICEID
#define NOHND MMSYSERR_INVALHANDLE
#define NODRV MMSYSERR_NODRIVER
#define JOYUNPLUGGED 167

static const struct { uint16_t ord; const char *name; uint16_t bytes, ret; } absent[] = {
    {101, "joyGetNumDevs", 0, 0},          {102, "joyGetDevCaps", 8, NODRV},       {103, "joyGetPos", 6, JOYUNPLUGGED},
    {104, "joyGetThreshold", 6, NODRV},    {105, "joyReleaseCapture", 2, NODRV},   {106, "joySetCapture", 8, NODRV},
    {107, "joySetThreshold", 4, NODRV},    {109, "joySetCalibration", 26, NODRV},
    {201, "midiOutGetNumDevs", 0, 0},      {202, "midiOutGetDevCaps", 8, NODEV},   {204, "midiOutOpen", 18, NODEV},
    {205, "midiOutClose", 2, NOHND},       {206, "midiOutPrepareHeader", 8, NOHND}, {207, "midiOutUnprepareHeader", 8, NOHND},
    {208, "midiOutShortMsg", 6, NOHND},    {209, "midiOutLongMsg", 8, NOHND},      {210, "midiOutReset", 2, NOHND},
    {211, "midiOutGetVolume", 6, NODEV},   {212, "midiOutSetVolume", 6, NODEV},    {213, "midiOutCachePatches", 10, NOHND},
    {214, "midiOutCacheDrumPatches", 10, NOHND}, {215, "midiOutGetID", 6, NOHND},  {216, "midiOutMessage", 12, NODEV},
    {301, "midiInGetNumDevs", 0, 0},       {302, "midiInGetDevCaps", 8, NODEV},    {304, "midiInOpen", 18, NODEV},
    {305, "midiInClose", 2, NOHND},        {306, "midiInPrepareHeader", 8, NOHND}, {307, "midiInUnprepareHeader", 8, NOHND},
    {308, "midiInAddBuffer", 8, NOHND},    {309, "midiInStart", 2, NOHND},         {310, "midiInStop", 2, NOHND},
    {311, "midiInReset", 2, NOHND},        {312, "midiInGetID", 6, NOHND},         {313, "midiInMessage", 12, NODEV},
    {501, "waveInGetNumDevs", 0, 0},       {502, "waveInGetDevCaps", 8, NODEV},    {504, "waveInOpen", 22, NODEV},
    {505, "waveInClose", 2, NOHND},        {506, "waveInPrepareHeader", 8, NOHND}, {507, "waveInUnprepareHeader", 8, NOHND},
    {508, "waveInAddBuffer", 8, NOHND},    {509, "waveInStart", 2, NOHND},         {510, "waveInStop", 2, NOHND},
    {511, "waveInReset", 2, NOHND},        {512, "waveInGetPosition", 8, NOHND},   {513, "waveInGetID", 6, NOHND},
    {514, "waveInMessage", 12, NODEV},
    {350, "auxGetNumDevs", 0, 0},          {351, "auxGetDevCaps", 8, NODEV},       {352, "auxGetVolume", 6, NODEV},
    {353, "auxSetVolume", 6, NODEV},       {354, "auxOutMessage", 12, NODEV},
};

mmsystem_t *mmsystem_create(const mmsystem_config *cfg, hle_t *h, user_t *u, kernel_t *k) {
    static const hle_def defs[] = {
        {MM, 1, "WEP", 2, s_wep},
        {MM, 5, "mmsystemGetVersion", 0, s_getversion},
        {MM, 30, "OutputDebugStr", 4, s_debugstr},
        {MM, 31, "DriverCallback", 22, s_drivercallback},
        {MM, 32, "StackEnter", 0, s_nop},
        {MM, 33, "StackLeave", 2, s_nop},
        {MM, 601, "timeGetSystemTime", 6, t_getsystemtime},
        {MM, 602, "timeSetEvent", 14, t_setevent},
        {MM, 603, "timeKillEvent", 2, t_killevent},
        {MM, 604, "timeGetDevCaps", 6, t_getdevcaps},
        {MM, 605, "timeBeginPeriod", 2, t_period},
        {MM, 606, "timeEndPeriod", 2, t_period},
        {MM, 607, "timeGetTime", 0, t_gettime},
    };
    mmsystem_t *m = calloc(1, sizeof *m);
    stub_ctx *sc;
    size_t i, n = sizeof absent / sizeof absent[0];
    if (!m) return NULL;
    m->hle = h;
    m->u = u;
    m->k = k;
    m->audio = cfg->audio;
    m->log = cfg->log;
    sc = calloc(n, sizeof *sc);
    if (!sc || !hle_register_table(h, defs, sizeof defs / sizeof defs[0], m) || !mm_wave_register(m) ||
        !mm_mci_register(m)) {
        free(sc);
        free(m);
        return NULL;
    }
    m->hle = h;
    for (i = 0; i < n; i++) {
        hle_def d = {MM, absent[i].ord, absent[i].name, absent[i].bytes, s_absent};
        sc[i].m = m;
        sc[i].ret = absent[i].ret;
        if (!hle_register(h, &d, &sc[i])) {
            free(sc);
            free(m);
            return NULL;
        }
    }
    m->absent = sc;
    user_set_tick(u, tick_hook, m);
    return m;
}

bool mmsystem_attach(mmsystem_t *m, loader_proc *p) {
    m->p = p;
    return true;
}

void mmsystem_destroy(mmsystem_t *m) {
    if (!m) return;
    user_set_tick(m->u, NULL, NULL);
    mm_wave_shutdown(m);
    free(m->absent);
    free(m);
}
