/* MCI. See mmsystem.h.
 *
 * No MCI device exists here: the Windows 3.1 ones are drivers (MCIWAVE, MCISEQ, MCIAVI.DRV...) that need the
 * real MMSYSTEM.DLL, and neither WRITER nor ARTIST imports a single MCI function. So the interface is complete
 * enough to be called and honest: opening a device of a known type fails with MCIERR_CANNOT_LOAD_DRIVER (the
 * driver is "configured" but cannot be loaded, which is what a program already has to cope with), an unknown
 * type is MCIERR_INVALID_DEVICE_NAME, and every command on a device that cannot be open is
 * MCIERR_INVALID_DEVICE_ID. mciSendString is parsed (quoted words, the command, the device, open's options) so
 * the errors are the ones the real parser gives for the same text. Nothing here is from evidence of use; the
 * numbers are MCIERR_BASE (256) plus the offsets of mmsystem.h, UNVERIFIED against a real MMSYSTEM.
 *
 * Open item (video): AVI playback needs a MCI AVI device and an Indeo 3 (IV31/IV32) decoder. */
#include "mm_priv.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MCIERR_BASE 256
#define MCIERR_INVALID_DEVICE_ID (MCIERR_BASE + 1)
#define MCIERR_UNRECOGNIZED_COMMAND (MCIERR_BASE + 5)
#define MCIERR_INVALID_DEVICE_NAME (MCIERR_BASE + 7)
#define MCIERR_CANNOT_LOAD_DRIVER (MCIERR_BASE + 10)
#define MCIERR_MISSING_COMMAND_STRING (MCIERR_BASE + 11)
#define MCIERR_UNSUPPORTED_FUNCTION (MCIERR_BASE + 18)
#define MCIERR_CANNOT_USE_ALL (MCIERR_BASE + 23)
#define MCIERR_EXTENSION_NOT_FOUND (MCIERR_BASE + 25)
#define MCIERR_DEVICE_TYPE_REQUIRED (MCIERR_BASE + 31)
#define MCIERR_MISSING_DEVICE_NAME (MCIERR_BASE + 36)
#define MCIERR_NO_CLOSING_QUOTE (MCIERR_BASE + 38)
#define MCIERR_NULL_PARAMETER_BLOCK (MCIERR_BASE + 41)
#define MCI_ALL_DEVICE_ID 0xFFFFu

#define MCI_OPEN 0x0803
#define MCI_CLOSE 0x0804
#define MCI_STOP 0x0808
#define MCI_PAUSE 0x0809
#define MCI_SYSINFO 0x0810
#define MCI_RESUME 0x0855
#define MCI_NOTIFY 1u
#define MCI_OPEN_ELEMENT 0x200u
#define MCI_OPEN_TYPE_ID 0x1000u
#define MCI_OPEN_TYPE 0x2000u

static const char *const types[] = {"waveaudio", "sequencer", "avivideo", "cdaudio", "videodisc", "overlay", "vcr",
                                    "scanner", "dat", "digitalvideo", "mpegvideo", NULL};
static const char *const commands[] = {"open", "close", "escape", "play", "seek", "stop", "pause", "info", "capability",
                                       "spin", "set", "step", "record", "sysinfo", "break", "sound", "save", "status",
                                       "cue", "realize", "window", "put", "where", "freeze", "unfreeze", "load", "cut",
                                       "copy", "paste", "update", "resume", "delete", NULL};
/* the MCI_* message numbers of mmsystem.h */
static const uint16_t messages[] = {0x0803, 0x0804, 0x0805, 0x0806, 0x0807, 0x0808, 0x0809, 0x080A, 0x080B, 0x080C,
                                    0x080D, 0x080E, 0x080F, 0x0810, 0x0811, 0x0812, 0x0813, 0x0814, 0x0830, 0x0840,
                                    0x0841, 0x0842, 0x0843, 0x0844, 0x0845, 0x0850, 0x0851, 0x0852, 0x0853, 0x0854,
                                    0x0855, 0x0856};

static bool in_list(const char *const *l, const char *w) {
    for (; *l; l++)
        if (!strcasecmp(*l, w)) return true;
    return false;
}

static bool is_message(unsigned msg) {
    size_t i;
    for (i = 0; i < sizeof messages / sizeof messages[0]; i++)
        if (messages[i] == msg) return true;
    return false;
}

/* The device type an open resolves to: the explicit type, else the element if it is a type name, else the type
 * of the element's extension. Returns the error code (0: a known type, nothing can be loaded). */
static unsigned open_error(const char *type, const char *elem) {
    const char *dot;
    if (!type && !elem) return MCIERR_MISSING_DEVICE_NAME;
    if (!type && elem && in_list(types, elem)) type = elem;
    if (!type) {
        dot = strrchr(elem, '.');
        if (!dot || strchr(dot, '\\')) return MCIERR_EXTENSION_NOT_FOUND;
        if (!strcasecmp(dot, ".wav")) type = "waveaudio";
        else if (!strcasecmp(dot, ".mid") || !strcasecmp(dot, ".rmi")) type = "sequencer";
        else if (!strcasecmp(dot, ".avi")) type = "avivideo";
        else return MCIERR_EXTENSION_NOT_FOUND;
    }
    return in_list(types, type) ? MCIERR_CANNOT_LOAD_DRIVER : MCIERR_INVALID_DEVICE_NAME;
}

/* A command that needs no open device and so succeeds when "all" is named (nothing is open). */
static bool all_ok(const char *cmd) {
    return !strcasecmp(cmd, "close") || !strcasecmp(cmd, "stop") || !strcasecmp(cmd, "pause") ||
           !strcasecmp(cmd, "resume");
}

/* Next word of s: quoted ("..." without the quotes) or up to white space. NULL at the end; *err set for an
 * unterminated quote. */
static char *next_word(char **s, unsigned *err) {
    char *p = *s, *w;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) {
        *s = p;
        return NULL;
    }
    if (*p == '"') {
        w = ++p;
        while (*p && *p != '"') p++;
        if (!*p) {
            *err = MCIERR_NO_CLOSING_QUOTE;
            *s = p;
            return NULL;
        }
        *p++ = 0;
    } else {
        w = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = 0;
    }
    *s = p;
    return w;
}

/* mciSendString's parser and the answer for it. */
static unsigned run_string(char *text) {
    char *s = text, *cmd, *w, *dev, *type = NULL, *elem = NULL;
    unsigned err = 0;
    cmd = next_word(&s, &err);
    if (err) return err;
    if (!cmd) return MCIERR_MISSING_COMMAND_STRING;
    if (!in_list(commands, cmd)) return MCIERR_UNRECOGNIZED_COMMAND;
    if (!strcasecmp(cmd, "sysinfo")) return MCIERR_UNSUPPORTED_FUNCTION;
    if (!strcasecmp(cmd, "open")) {
        bool first = true;
        while ((w = next_word(&s, &err))) {
            if (!strcasecmp(w, "type") || !strcasecmp(w, "alias") || !strcasecmp(w, "style") ||
                !strcasecmp(w, "parent") || !strcasecmp(w, "buffer")) {
                char *v = next_word(&s, &err);
                if (!strcasecmp(w, "type") && v) type = v;
            } else if (first && strcasecmp(w, "shareable") && strcasecmp(w, "notify") && strcasecmp(w, "wait")) {
                elem = w;
                first = false;
            }
        }
        if (err) return err;
        return open_error(type, elem);
    }
    dev = next_word(&s, &err);
    if (err) return err;
    if (!dev) return MCIERR_MISSING_DEVICE_NAME;
    if (!strcasecmp(dev, "all")) return all_ok(cmd) ? 0 : MCIERR_CANNOT_USE_ALL;
    if (strspn(dev, "0123456789") == strlen(dev)) return MCIERR_INVALID_DEVICE_ID;
    return MCIERR_INVALID_DEVICE_NAME;
}

static cpu_status m_sendstring(hle_call *c) {
    MM_BEGIN;
    uint16_t csel, coff, rsel, roff, cch;
    char cmd[512];
    uint8_t *ret;
    unsigned rc;
    csel = 0; coff = 0;
    hle_far(&a, &csel, &coff);
    hle_far(&a, &rsel, &roff);
    cch = hle_u16(&a);
    hle_u16(&a);
    MM_CHECK;
    if (!hle_read_cstr(c, csel, coff, cmd, sizeof cmd)) return CPU_E_SEL;
    rc = run_string(cmd);
    if (rc == 0 && cch && (rsel || roff) && (ret = hle_ptr(c, rsel, roff, 1))) ret[0] = 0;
    hle_ret32(c, rc);
    return CPU_OK;
}

static cpu_status m_execute(hle_call *c) {
    MM_BEGIN;
    uint16_t csel, coff;
    char cmd[512];
    unsigned rc;
    hle_far(&a, &csel, &coff);
    MM_CHECK;
    if (!hle_read_cstr(c, csel, coff, cmd, sizeof cmd)) return CPU_E_SEL;
    rc = run_string(cmd);
    if (rc && m->log) fprintf(m->log, "mmsystem: mciExecute failed with MCI error %u\n", rc);
    hle_ret16(c, rc == 0);
    return CPU_OK;
}

static cpu_status m_sendcommand(hle_call *c) {
    MM_BEGIN;
    uint16_t id, msg, psel, poff;
    uint32_t flags, parms;
    const uint8_t *p = NULL;
    unsigned rc;
    id = hle_u16(&a);
    msg = hle_u16(&a);
    flags = hle_u32(&a);
    parms = hle_u32(&a);
    MM_CHECK;
    psel = (uint16_t)(parms >> 16);
    poff = (uint16_t)parms;
    if (msg == MCI_OPEN) {
        char type[64] = "", elem[260] = "";
        if (!psel && !poff) {
            hle_ret32(c, MCIERR_NULL_PARAMETER_BLOCK);
            return CPU_OK;
        }
        if (!(p = hle_ptr(c, psel, poff, 20))) return CPU_E_SEL;
        if ((flags & MCI_OPEN_TYPE) && !(flags & MCI_OPEN_TYPE_ID) && mm_ld32(p + 8))
            hle_read_cstr(c, (uint16_t)(mm_ld32(p + 8) >> 16), (uint16_t)mm_ld32(p + 8), type, sizeof type);
        if ((flags & MCI_OPEN_ELEMENT) && mm_ld32(p + 12))
            hle_read_cstr(c, (uint16_t)(mm_ld32(p + 12) >> 16), (uint16_t)mm_ld32(p + 12), elem, sizeof elem);
        rc = (flags & MCI_OPEN_TYPE_ID) ? MCIERR_CANNOT_LOAD_DRIVER
             : (!(flags & MCI_OPEN_TYPE) && !(flags & MCI_OPEN_ELEMENT)) ? MCIERR_DEVICE_TYPE_REQUIRED
                                                                          : open_error(type[0] ? type : NULL, elem[0] ? elem : NULL);
        hle_ret32(c, rc);
        return CPU_OK;
    }
    if (msg == MCI_SYSINFO) {
        hle_ret32(c, MCIERR_UNSUPPORTED_FUNCTION);
        return CPU_OK;
    }
    if (!is_message(msg)) {
        hle_ret32(c, MCIERR_UNRECOGNIZED_COMMAND);
        return CPU_OK;
    }
    if (id == MCI_ALL_DEVICE_ID) {
        bool ok = msg == MCI_CLOSE || msg == MCI_STOP || msg == MCI_PAUSE || msg == MCI_RESUME;
        if (ok && (flags & MCI_NOTIFY) && (psel || poff) && (p = hle_ptr(c, psel, poff, 4))) {
            wm_post(user_wm(m->u), (uint16_t)mm_ld32(p), 0x3B9, 1 /* MCI_NOTIFY_SUCCESSFUL */, id);
        }
        hle_ret32(c, ok ? 0 : MCIERR_CANNOT_USE_ALL);
        return CPU_OK;
    }
    hle_ret32(c, MCIERR_INVALID_DEVICE_ID);
    return CPU_OK;
}

static cpu_status m_getdeviceid(hle_call *c) {
    MM_BEGIN;
    uint16_t sel, off;
    char name[64];
    hle_far(&a, &sel, &off);
    MM_CHECK;
    hle_ret16(c, hle_read_cstr(c, sel, off, name, sizeof name) && !strcasecmp(name, "all") ? MCI_ALL_DEVICE_ID : 0);
    return CPU_OK;
}

static const char *mci_text(unsigned err) {
    switch (err) {
    case MCIERR_INVALID_DEVICE_ID: return "Invalid MCI device ID. Use the ID returned when opening the MCI device.";
    case MCIERR_UNRECOGNIZED_COMMAND: return "The driver cannot recognize the specified command.";
    case MCIERR_INVALID_DEVICE_NAME: return "The specified device is not open or is not recognized by MCI.";
    case MCIERR_CANNOT_LOAD_DRIVER: return "There is an undetectable problem in loading the specified device driver.";
    case MCIERR_MISSING_COMMAND_STRING: return "No command was specified.";
    case MCIERR_UNSUPPORTED_FUNCTION: return "The specified device does not support that function.";
    case MCIERR_CANNOT_USE_ALL: return "Cannot use 'all' as the device name with the specified command.";
    case MCIERR_EXTENSION_NOT_FOUND: return "Cannot determine the device type from the given filename extension.";
    case MCIERR_DEVICE_TYPE_REQUIRED: return "The device name cannot be found. Make sure the device is installed correctly.";
    case MCIERR_MISSING_DEVICE_NAME: return "The device name is missing from the command.";
    case MCIERR_NO_CLOSING_QUOTE: return "A closing quotation mark is missing.";
    case MCIERR_NULL_PARAMETER_BLOCK: return "A null parameter block was passed to MCI.";
    case MCIERR_BASE + 17: return "A required parameter is missing from the command.";
    default: return NULL;
    }
}

static cpu_status m_geterrorstring(hle_call *c) {
    MM_BEGIN;
    uint32_t err = hle_u32(&a);
    uint16_t sel, off, len;
    const char *t;
    char *dst;
    size_t n;
    hle_far(&a, &sel, &off);
    len = hle_u16(&a);
    MM_CHECK;
    t = err < MCIERR_BASE ? mm_error_text(err) : mci_text(err);
    if (!t) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (len) {
        n = strlen(t);
        if (n > (size_t)len - 1u) n = (size_t)len - 1u;
        if (!(dst = hle_ptr(c, sel, off, (uint32_t)n + 1))) return CPU_E_SEL;
        memcpy(dst, t, n);
        dst[n] = 0;
    }
    hle_ret16(c, 1);
    return CPU_OK;
}

static cpu_status m_setyieldproc(hle_call *c) {
    MM_BEGIN;
    hle_u16(&a);
    hle_u32(&a);
    hle_u32(&a);
    MM_CHECK;
    hle_ret16(c, 0);                      /* no such device */
    return CPU_OK;
}

static cpu_status m_getyieldproc(hle_call *c) {
    MM_BEGIN;
    uint16_t sel, off;
    uint8_t *p;
    hle_u16(&a);
    hle_far(&a, &sel, &off);
    MM_CHECK;
    if ((sel || off) && (p = hle_ptr(c, sel, off, 4))) mm_st32(p, 0);
    hle_ret32(c, 0);
    return CPU_OK;
}

static cpu_status m_getcreatortask(hle_call *c) {
    MM_BEGIN;
    hle_u16(&a);
    MM_CHECK;
    hle_ret16(c, 0);
    return CPU_OK;
}

bool mm_mci_register(mmsystem_t *m) {
    static const hle_def defs[] = {
        {MM, 701, "mciSendCommand", 12, m_sendcommand},
        {MM, 702, "mciSendString", 12, m_sendstring},
        {MM, 703, "mciGetDeviceID", 4, m_getdeviceid},
        {MM, 706, "mciGetErrorString", 10, m_geterrorstring},
        {MM, 712, "mciExecute", 4, m_execute},
        {MM, 714, "mciSetYieldProc", 10, m_setyieldproc},
        {MM, 716, "mciGetYieldProc", 6, m_getyieldproc},
        {MM, 717, "mciGetCreatorTask", 2, m_getcreatortask},
    };
    return hle_register_table(m->hle, defs, sizeof defs / sizeof defs[0], m);
}
