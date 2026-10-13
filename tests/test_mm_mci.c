/* MCI: no MCI device exists in this runtime (neither program imports any MCI function: WRITER and ARTIST do
 * not play AVI through MCI), so the interface answers with the error codes Windows gives for a device that
 * cannot be loaded or opened, and never pretends success. Parsing is exercised through mciSendString.
 * MCI error numbers are MCIERR_BASE 256 + n, from the API description (UNVERIFIED: no program here uses them). */
#include "mmfixture.h"

#define MCIERR_INVALID_DEVICE_ID 257
#define MCIERR_UNRECOGNIZED_COMMAND 261
#define MCIERR_INVALID_DEVICE_NAME 263
#define MCIERR_CANNOT_LOAD_DRIVER 266
#define MCIERR_MISSING_COMMAND_STRING 267
#define MCIERR_UNSUPPORTED_FUNCTION 274
#define MCIERR_EXTENSION_NOT_FOUND 281
#define MCIERR_DEVICE_TYPE_REQUIRED 287
#define MCIERR_MISSING_DEVICE_NAME 292
#define MCI_OPEN 0x0803
#define MCI_CLOSE 0x0804
#define MCI_PLAY 0x0806
#define MCI_SYSINFO 0x0810
#define MCI_STATUS 0x0814
#define MCI_NOTIFY 1u
#define MCI_OPEN_ELEMENT 0x200u
#define MCI_OPEN_TYPE 0x2000u

static uint16_t g_parm, g_ret;

static void boot(void) {
    mmboot(true);
    g_parm = useg(0x100);
    g_ret = useg(0x100);
}

/* mciSendCommand(id, msg, flags, parms); the result is DX:AX */
static uint32_t send(uint16_t id, uint16_t msg, uint32_t flags, uint16_t parms_sel) {
    CHECK_EQ(MCALL(701, id, msg, LONG(flags), LONG((uint32_t)parms_sel << 16)), LOADER_STOP_HLT);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    return fx_dxax(&uf);
}

/* mciSendString(cmd, ret, cch, hwndCallback) */
static uint32_t sstr(const char *cmd, uint16_t cch) {
    uint16_t cs = ustr(cmd);
    CHECK_EQ(MCALL(702, FARP(cs, 0), FARP(g_ret, 0), cch, 0), LOADER_STOP_HLT);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    return fx_dxax(&uf) & 0xFFFF;
}

/* MCI_OPEN_PARMS: dwCallback 0, wDeviceID 4, wReserved0 6, lpstrDeviceType 8, lpstrElementName 12, lpstrAlias 16 */
static void open_parms(const char *type, const char *elem) {
    uwr32(g_parm, 0, 0); uwr16(g_parm, 4, 0x7777); uwr16(g_parm, 6, 0);
    uwr32(g_parm, 8, type ? (uint32_t)ustr(type) << 16 : 0);
    uwr32(g_parm, 12, elem ? (uint32_t)ustr(elem) << 16 : 0);
    uwr32(g_parm, 16, 0);
}

static void test_send_command(void) {
    printf("test_mm_mci_send_command\n");
    boot();
    open_parms("waveaudio", NULL);
    CHECK_EQ(send(0, MCI_OPEN, MCI_OPEN_TYPE, g_parm), MCIERR_CANNOT_LOAD_DRIVER);   /* a real type whose driver is not there */
    CHECK_EQ(urd16(g_parm, 4), 0x7777);                                              /* no device id was handed out */
    open_parms("AVIVideo", "C:\\MOVIE.AVI");
    CHECK_EQ(send(0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_ELEMENT, g_parm), MCIERR_CANNOT_LOAD_DRIVER);
    open_parms("nosuchdevice", NULL);
    CHECK_EQ(send(0, MCI_OPEN, MCI_OPEN_TYPE, g_parm), MCIERR_INVALID_DEVICE_NAME);
    open_parms(NULL, NULL);
    CHECK_EQ(send(0, MCI_OPEN, 0, g_parm), MCIERR_DEVICE_TYPE_REQUIRED);
    open_parms(NULL, "C:\\CLIP.AVI");                                                /* the extension names the device type */
    CHECK_EQ(send(0, MCI_OPEN, MCI_OPEN_ELEMENT, g_parm), MCIERR_CANNOT_LOAD_DRIVER);
    open_parms(NULL, "C:\\SONG.MID");
    CHECK_EQ(send(0, MCI_OPEN, MCI_OPEN_ELEMENT, g_parm), MCIERR_CANNOT_LOAD_DRIVER);
    open_parms(NULL, "C:\\NOISE.WAV");
    CHECK_EQ(send(0, MCI_OPEN, MCI_OPEN_ELEMENT, g_parm), MCIERR_CANNOT_LOAD_DRIVER);
    open_parms(NULL, "C:\\FILE.XYZ");
    CHECK_EQ(send(0, MCI_OPEN, MCI_OPEN_ELEMENT, g_parm), MCIERR_EXTENSION_NOT_FOUND);
    CHECK_EQ(send(0, MCI_OPEN, 0, 0), 0x0129);                                       /* MCIERR_NULL_PARAMETER_BLOCK = 256 + 41 */
    /* every other command needs an open device, and none can be open */
    CHECK_EQ(send(1, MCI_PLAY, 0, g_parm), MCIERR_INVALID_DEVICE_ID);
    CHECK_EQ(send(7, MCI_STATUS, 0, g_parm), MCIERR_INVALID_DEVICE_ID);
    CHECK_EQ(send(1, MCI_CLOSE, 0, g_parm), MCIERR_INVALID_DEVICE_ID);
    CHECK_EQ(send(1, MCI_SYSINFO, 0, g_parm), MCIERR_UNSUPPORTED_FUNCTION);
    CHECK_EQ(send(1, 0x7777, 0, g_parm), MCIERR_UNRECOGNIZED_COMMAND);
    /* MCI_ALL_DEVICE_ID: closing "everything" with nothing open succeeds, and MCI_NOTIFY is answered */
    {
        uint16_t cw, w, msg = useg(0x20);
        u_register("MCIW", u_far(U_CODE_A), 0, 0, 0, 1, 1);
        w = u_create("MCIW", "m", 0, 0, 0, 50, 50, 0, 0, 1);
        (void)cw;
        uwr32(g_parm, 0, w);
        CHECK_EQ(send(0xFFFF, MCI_CLOSE, 0, g_parm), 0);
        CHECK_EQ(UCALL(109, FARP(msg, 0), 0, 0, 0, 1), LOADER_STOP_HLT);
        CHECK_EQ(fx_ax(&uf), 0);                                                     /* no notification without the flag */
        CHECK_EQ(send(0xFFFF, MCI_CLOSE, MCI_NOTIFY, g_parm), 0);
        CHECK_EQ(UCALL(109, FARP(msg, 0), 0, 0, 0, 1), LOADER_STOP_HLT);
        CHECK_EQ(fx_ax(&uf), 1);
        CHECK_EQ(urd16(msg, 0), w);
        CHECK_EQ(urd16(msg, 2), 0x3B9);                                              /* MM_MCINOTIFY */
        CHECK_EQ(urd16(msg, 4), 1);                                                  /* MCI_NOTIFY_SUCCESSFUL */
        CHECK_EQ(urd16(msg, 6), 0xFFFF);                                             /* lParam: the device id */
    }
    mmdown();
}

static void test_send_string(void) {
    printf("test_mm_mci_send_string\n");
    boot();
    memset(umem(g_ret, 0, 0x20), 'x', 0x20);
    CHECK_EQ(sstr("", 16), MCIERR_MISSING_COMMAND_STRING);
    CHECK_EQ(sstr("   ", 16), MCIERR_MISSING_COMMAND_STRING);
    CHECK_EQ(sstr("frobnicate x", 16), MCIERR_UNRECOGNIZED_COMMAND);
    CHECK_EQ(sstr("open", 16), MCIERR_MISSING_DEVICE_NAME);
    CHECK_EQ(sstr("open waveaudio", 16), MCIERR_CANNOT_LOAD_DRIVER);
    CHECK_EQ(sstr("OPEN WaveAudio ALIAS snd", 16), MCIERR_CANNOT_LOAD_DRIVER);
    CHECK_EQ(sstr("open \"C:\\MY FILES\\M.AVI\" type avivideo alias movie wait", 16), MCIERR_CANNOT_LOAD_DRIVER);
    CHECK_EQ(sstr("open C:\\X.AVI", 16), MCIERR_CANNOT_LOAD_DRIVER);
    CHECK_EQ(sstr("open C:\\X.QQQ", 16), MCIERR_EXTENSION_NOT_FOUND);
    CHECK_EQ(sstr("open C:\\X.AVI type nosuch", 16), MCIERR_INVALID_DEVICE_NAME);
    CHECK_EQ(sstr("open \"unterminated", 16), 0x126);                                /* MCIERR_NO_CLOSING_QUOTE = 256 + 38 */
    CHECK_EQ(sstr("play movie", 16), MCIERR_INVALID_DEVICE_NAME);                    /* no such alias or device */
    CHECK_EQ(sstr("status movie mode", 16), MCIERR_INVALID_DEVICE_NAME);
    CHECK_EQ(sstr("play 3 from 0", 16), MCIERR_INVALID_DEVICE_ID);                   /* a numeric id that is not open */
    CHECK_EQ(sstr("play", 16), MCIERR_MISSING_DEVICE_NAME);                          /* the device is a required word */
    CHECK_EQ(sstr("close all", 16), 0);
    CHECK_EQ(umem(g_ret, 0, 1)[0], 0);                                               /* the return string is emptied on success */
    CHECK_EQ(umem(g_ret, 1, 1)[0], 'x');
    CHECK_EQ(sstr("stop all", 0), 0);                                                /* no return buffer: nothing written */
    CHECK_EQ(sstr("sysinfo all quantity", 16), MCIERR_UNSUPPORTED_FUNCTION);
    mmdown();
}

static void test_device_id_and_errors(void) {
    uint16_t buf, s;
    printf("test_mm_mci_ids_errors\n");
    boot();
    s = ustr("all");
    MCALL(703, FARP(s, 0));
    CHECK_EQ(fx_ax(&uf), 0xFFFF);                                                    /* mciGetDeviceID("all") */
    CHECK_EQ(uf.sp_after, uf.sp_before);
    s = ustr("movie");
    MCALL(703, FARP(s, 0));
    CHECK_EQ(fx_ax(&uf), 0);                                                         /* nothing is open */
    s = ustr("5");
    MCALL(703, FARP(s, 0));
    CHECK_EQ(fx_ax(&uf), 0);
    buf = useg(0x100);
    memset(umem(buf, 0, 0x100), 'x', 0x100);
    MCALL(706, LONG(MCIERR_CANNOT_LOAD_DRIVER), FARP(buf, 0), 0x80);                  /* mciGetErrorString */
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    CHECK(strlen((char *)umem(buf, 0, 1)) > 5);
    CHECK(strstr((char *)umem(buf, 0, 1), "driver") != NULL);
    MCALL(706, LONG(MCIERR_CANNOT_LOAD_DRIVER), FARP(buf, 0), 6);                     /* truncated, NUL terminated */
    CHECK_EQ(strlen((char *)umem(buf, 0, 1)), 5);
    MCALL(706, LONG(2), FARP(buf, 0), 0x80);                                          /* an MMSYSERR code is explained too */
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK(strlen((char *)umem(buf, 0, 1)) > 5);
    MCALL(706, LONG(0x7FFF), FARP(buf, 0), 0x80);
    CHECK_EQ(fx_ax(&uf), 0);                                                         /* unknown code: FALSE */
    MCALL(714, 1, LONG(0), LONG(0));                                                 /* mciSetYieldProc: no such device */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    s = useg(0x10);
    MCALL(716, 1, FARP(s, 0));                                                       /* mciGetYieldProc */
    CHECK_EQ(fx_dxax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    MCALL(717, 1);                                                                   /* mciGetCreatorTask */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    s = ustr("play movie");
    MCALL(712, FARP(s, 0));                                                          /* mciExecute: FALSE on failure */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    s = ustr("close all");
    MCALL(712, FARP(s, 0));
    CHECK_EQ(fx_ax(&uf), 1);
    mmdown();
}

int main(void) {
    test_send_command();
    test_send_string();
    test_device_id_and_errors();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
