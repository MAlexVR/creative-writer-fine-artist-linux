/* MMSYSTEM timing and housekeeping: timeGetTime, timeGetSystemTime, timeGetDevCaps, timeBeginPeriod/EndPeriod,
 * timeSetEvent/timeKillEvent with callbacks into the guest, DriverCallback, version and debug output, and the
 * clock reads that deliver due events. Ordinals are those of the Win 3.1 export list (MMSYSTEM's exports). */
#include "mmfixture.h"

static void test_time_get(void) {
    uint16_t buf;
    printf("test_mm_time_get\n");
    mmboot(false);
    uclock = 1000 + 1234;
    CHECK_EQ(MCALL(607), LOADER_STOP_HLT);                           /* timeGetTime: the program clock */
    CHECK_EQ(fx_dxax(&uf), 1234);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    uclock = 1000 + 70000;
    MCALL(607);
    CHECK_EQ(fx_dxax(&uf), 70000);
    buf = useg(0x40);
    CHECK_EQ(MCALL(601, FARP(buf, 0), 6), LOADER_STOP_HLT);          /* timeGetSystemTime fills an MMTIME with TIME_MS */
    CHECK_EQ(uf.sp_after, uf.sp_before);
    CHECK_EQ(urd16(buf, 0), 1);
    CHECK_EQ(urd32(buf, 2), 70000);
    MCALL(5);
    CHECK_EQ(fx_ax(&uf), 0x030A);                                    /* mmsystemGetVersion: 3.10 */
    mmdown();
    /* a timeGetTime delay loop (Creative Writer's splash) lets the host show the screen, once per 16 ms at most */
    mmboot(false);
    for (unsigned i = 0; i < 20; i++) {
        uclock += 5;
        MCALL(607);
    }
    CHECK(urefresh_calls >= 5 && urefresh_calls <= 7);
    mmdown();
}

static void test_period_and_caps(void) {
    uint16_t buf;
    printf("test_mm_time_period\n");
    mmboot(false);
    MCALL(605, 1);
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    MCALL(605, 0);
    CHECK_EQ(fx_ax(&uf), 97);                                        /* TIMERR_NOCANDO */
    MCALL(606, 1);
    CHECK_EQ(fx_ax(&uf), 0);
    MCALL(606, 0);
    CHECK_EQ(fx_ax(&uf), 97);
    buf = useg(0x40);
    MCALL(604, FARP(buf, 0), 4);                                     /* timeGetDevCaps */
    CHECK_EQ(fx_ax(&uf), 0);
    CHECK_EQ(urd16(buf, 0), 1);
    CHECK_EQ(urd16(buf, 2), 0xFFFF);
    MCALL(604, FARP(buf, 0), 2);
    CHECK_EQ(fx_ax(&uf), 129);                                       /* TIMERR_STRUCT */
    mmdown();
}

static uint32_t far_rec(void) { return (uint32_t)u_cs() << 16 | M_REC; }

static void test_timer_events(void) {
    uint16_t id1, id2;
    printf("test_mm_time_events\n");
    mmboot(false);
    /* periodic every 100 ms with user data 0xAABBCCDD */
    CHECK_EQ(MCALL(602, 100, 10, LONG(far_rec()), LONG(0xAABBCCDDu), 1), LOADER_STOP_HLT);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    id1 = fx_ax(&uf);
    CHECK(id1 != 0);
    CHECK_EQ(MCALL(602, 250, 10, LONG(far_rec()), LONG(0x11112222u), 0), LOADER_STOP_HLT);   /* one-shot */
    id2 = fx_ax(&uf);
    CHECK(id2 != 0 && id2 != id1);
    uclock = 1000 + 99;
    MCALL(607);                                                      /* reading the clock delivers what is due */
    CHECK_EQ(ulog_n(), 0);
    uclock = 1000 + 100;
    MCALL(607);
    CHECK_EQ(ulog_n(), 1);
    CHECK_EQ(mrec_id(0), id1);
    CHECK_EQ(mrec_msg(0), 0);
    CHECK_EQ(mrec_user(0), 0xAABBCCDDu);
    CHECK_EQ(mrec_dw1(0), 0);
    CHECK_EQ(mrec_dw2(0), 0);
    uclock = 1000 + 260;
    UCALL(13);                                                       /* GetTickCount is a clock read too */
    CHECK_EQ(ulog_n(), 3);                                           /* periodic at 200, one-shot at 250, in due order */
    CHECK_EQ(mrec_id(1), id1);
    CHECK_EQ(mrec_id(2), id2);
    CHECK_EQ(mrec_user(2), 0x11112222u);
    uclock = 1000 + 1000;
    MCALL(607);
    CHECK_EQ(ulog_n(), 3 + 1);                                       /* a late periodic timer fires once, then re-arms from now */
    CHECK_EQ(mrec_id(3), id1);
    CHECK_EQ(MCALL(603, id2), LOADER_STOP_HLT);                      /* timeKillEvent of a one-shot that already fired */
    CHECK_EQ(fx_ax(&uf), 97);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    MCALL(603, id1);
    CHECK_EQ(fx_ax(&uf), 0);
    uclock = 1000 + 5000;
    MCALL(607);
    CHECK_EQ(ulog_n(), 4);                                           /* killed: silent */
    MCALL(603, id1);
    CHECK_EQ(fx_ax(&uf), 97);
    MCALL(602, 100, 10, LONG(0), LONG(0), 0);                        /* no callback */
    CHECK_EQ(fx_ax(&uf), 0);
    mmdown();
}

static void test_driver_callback_and_debug(void) {
    uint16_t str;
    uint8_t *m;
    printf("test_mm_time_driver_callback\n");
    mmboot(false);
    str = ustr("hello debug");
    /* DriverCallback(dwCallback, uFlags, hDevice, uMessage, dwUser, dw1, dw2) with DCB_FUNCTION */
    CHECK_EQ(MCALL(31, LONG(far_rec()), 3, 0x1234, 0x3BD, LONG(0xCAFE0001u), LONG(0x11), LONG(0x22)), LOADER_STOP_HLT);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    CHECK_EQ(fx_ax(&uf), 1);
    CHECK_EQ(ulog_n(), 1);
    CHECK_EQ(mrec_id(0), 0x1234);
    CHECK_EQ(mrec_msg(0), 0x3BD);
    CHECK_EQ(mrec_user(0), 0xCAFE0001u);
    CHECK_EQ(mrec_dw1(0), 0x11);
    CHECK_EQ(mrec_dw2(0), 0x22);
    /* DCB_WINDOW posts to the window in the callback's low word */
    {
        uint16_t cs, w;
        w = u_register("MMCls", u_far(U_CODE_A), 0, 0, 0, 1, 1);
        CHECK(w != 0);
        w = u_create("MMCls", "t", 0, 0, 0, 50, 50, 0, 0, 1);
        CHECK(w != 0);
        cs = useg(0x40);
        CHECK_EQ(MCALL(31, LONG(w), 1, 0x4321, 0x3BD, LONG(0), LONG(0x77), LONG(0)), LOADER_STOP_HLT);
        CHECK_EQ(fx_ax(&uf), 1);
        CHECK_EQ(UCALL(109, FARP(cs, 0), 0, 0, 0, 1), LOADER_STOP_HLT);     /* PeekMessage(PM_REMOVE) */
        CHECK_EQ(fx_ax(&uf), 1);
        CHECK_EQ(urd16(cs, 0), w);
        CHECK_EQ(urd16(cs, 2), 0x3BD);
        CHECK_EQ(urd16(cs, 4), 0x4321);                              /* wParam = hDevice */
        CHECK_EQ(urd32(cs, 6), 0x77);                                /* lParam = dw1 */
    }
    MCALL(31, LONG(0), 0, 0, 0, LONG(0), LONG(0), LONG(0));         /* DCB_NULL: nothing to deliver */
    CHECK_EQ(fx_ax(&uf), 0);
    m = umem(str, 0, 1);
    CHECK(m != NULL);
    CHECK_EQ(MCALL(30, FARP(str, 0)), LOADER_STOP_HLT);              /* OutputDebugStr */
    CHECK_EQ(uf.sp_after, uf.sp_before);
    MCALL(32);                                                       /* StackEnter / StackLeave: nothing to do */
    CHECK_EQ(uf.sp_after, uf.sp_before);
    MCALL(33, 0);
    CHECK_EQ(uf.sp_after, uf.sp_before);
    mmdown();
}

int main(void) {
    test_time_get();
    test_period_and_caps();
    test_timer_events();
    test_driver_callback_and_debug();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
