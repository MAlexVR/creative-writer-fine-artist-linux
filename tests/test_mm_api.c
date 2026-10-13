/* The MMSYSTEM import surface: the registered table against the Win 3.1 export list (ordinal, name, argument
 * bytes), and, with CWFA_WRITER_EXE / CWFA_ARTIST_EXE, every MMSYSTEM import of the two programs handled. */
#include <stdlib.h>
#include <string.h>

#include "mmfixture.h"
#include "win16_imports.h"
#include "ne.h"

typedef struct { unsigned ord; const char *name; int purge; } mmexp;

/* the Win 3.1 MMSYSTEM export list (PURGE: bytes the callee pops, -1 unknown there) */
static const mmexp win31_exports[] = {
    {1, "WEP", 2}, {2, "SNDPLAYSOUND", 6}, {5, "MMSYSTEMGETVERSION", 0}, {6, "DRIVERPROC", 16},
    {30, "OUTPUTDEBUGSTR", 4}, {31, "DRIVERCALLBACK", 22}, {32, "STACKENTER", 0}, {33, "STACKLEAVE", 2},
    {34, "MMDRVINSTALL", 8}, {101, "JOYGETNUMDEVS", 0}, {102, "JOYGETDEVCAPS", 8}, {103, "JOYGETPOS", 6},
    {104, "JOYGETTHRESHOLD", 6}, {105, "JOYRELEASECAPTURE", 2}, {106, "JOYSETCAPTURE", 8}, {107, "JOYSETTHRESHOLD", 4},
    {109, "JOYSETCALIBRATION", 26}, {201, "MIDIOUTGETNUMDEVS", 0}, {202, "MIDIOUTGETDEVCAPS", 8},
    {203, "MIDIOUTGETERRORTEXT", 8}, {204, "MIDIOUTOPEN", 18}, {205, "MIDIOUTCLOSE", 2},
    {206, "MIDIOUTPREPAREHEADER", 8}, {207, "MIDIOUTUNPREPAREHEADER", 8}, {208, "MIDIOUTSHORTMSG", 6},
    {209, "MIDIOUTLONGMSG", 8}, {210, "MIDIOUTRESET", 2}, {211, "MIDIOUTGETVOLUME", 6}, {212, "MIDIOUTSETVOLUME", 6},
    {213, "MIDIOUTCACHEPATCHES", 10}, {214, "MIDIOUTCACHEDRUMPATCHES", 10}, {215, "MIDIOUTGETID", 6},
    {216, "MIDIOUTMESSAGE", 12}, {301, "MIDIINGETNUMDEVS", 0}, {302, "MIDIINGETDEVCAPS", 8},
    {303, "MIDIINGETERRORTEXT", 8}, {304, "MIDIINOPEN", 18}, {305, "MIDIINCLOSE", 2}, {306, "MIDIINPREPAREHEADER", 8},
    {307, "MIDIINUNPREPAREHEADER", 8}, {308, "MIDIINADDBUFFER", 8}, {309, "MIDIINSTART", 2}, {310, "MIDIINSTOP", 2},
    {311, "MIDIINRESET", 2}, {312, "MIDIINGETID", 6}, {313, "MIDIINMESSAGE", 12}, {350, "AUXGETNUMDEVS", 0},
    {351, "AUXGETDEVCAPS", 8}, {352, "AUXGETVOLUME", 6}, {353, "AUXSETVOLUME", 6}, {354, "AUXOUTMESSAGE", 12},
    {401, "WAVEOUTGETNUMDEVS", 0}, {402, "WAVEOUTGETDEVCAPS", 8}, {403, "WAVEOUTGETERRORTEXT", 8},
    {404, "WAVEOUTOPEN", 22}, {405, "WAVEOUTCLOSE", 2}, {406, "WAVEOUTPREPAREHEADER", 8},
    {407, "WAVEOUTUNPREPAREHEADER", 8}, {408, "WAVEOUTWRITE", 8}, {409, "WAVEOUTPAUSE", 2}, {410, "WAVEOUTRESTART", 2},
    {411, "WAVEOUTRESET", 2}, {412, "WAVEOUTGETPOSITION", 8}, {413, "WAVEOUTGETPITCH", 6}, {414, "WAVEOUTSETPITCH", 6},
    {415, "WAVEOUTGETVOLUME", 6}, {416, "WAVEOUTSETVOLUME", 6}, {417, "WAVEOUTGETPLAYBACKRATE", 6},
    {418, "WAVEOUTSETPLAYBACKRATE", 6}, {419, "WAVEOUTBREAKLOOP", 2}, {420, "WAVEOUTGETID", 6},
    {421, "WAVEOUTMESSAGE", 12}, {501, "WAVEINGETNUMDEVS", 0}, {502, "WAVEINGETDEVCAPS", 8},
    {503, "WAVEINGETERRORTEXT", 8}, {504, "WAVEINOPEN", 22}, {505, "WAVEINCLOSE", 2}, {506, "WAVEINPREPAREHEADER", 8},
    {507, "WAVEINUNPREPAREHEADER", 8}, {508, "WAVEINADDBUFFER", 8}, {509, "WAVEINSTART", 2}, {510, "WAVEINSTOP", 2},
    {511, "WAVEINRESET", 2}, {512, "WAVEINGETPOSITION", 8}, {513, "WAVEINGETID", 6}, {514, "WAVEINMESSAGE", 12},
    {601, "TIMEGETSYSTEMTIME", 6}, {602, "TIMESETEVENT", 14}, {603, "TIMEKILLEVENT", -1}, {604, "TIMEGETDEVCAPS", 6},
    {605, "TIMEBEGINPERIOD", -1}, {606, "TIMEENDPERIOD", -1}, {607, "TIMEGETTIME", -1}, {701, "MCISENDCOMMAND", 12},
    {702, "MCISENDSTRING", 12}, {703, "MCIGETDEVICEID", 4}, {704, "MCIPARSECOMMAND", 18},
    {705, "MCILOADCOMMANDRESOURCE", 8}, {706, "MCIGETERRORSTRING", 10}, {707, "MCISETDRIVERDATA", 6},
    {708, "MCIGETDRIVERDATA", 2}, {710, "MCIDRIVERYIELD", 2}, {711, "MCIDRIVERNOTIFY", -1}, {712, "MCIEXECUTE", 4},
    {713, "MCIFREECOMMANDRESOURCE", 2}, {714, "MCISETYIELDPROC", 10}, {715, "MCIGETDEVICEIDFROMELEMENTID", 8},
    {716, "MCIGETYIELDPROC", 6}, {717, "MCIGETCREATORTASK", 2}, {900, "MMTASKCREATE", 12}, {902, "MMTASKBLOCK", 2},
    {903, "MMTASKSIGNAL", -1}, {904, "MMGETCURRENTTASK", -1}, {905, "MMTASKYIELD", 0}, {1100, "DRVOPEN", -1},
    {1101, "DRVCLOSE", -1}, {1102, "DRVSENDMESSAGE", -1}, {1103, "DRVGETMODULEHANDLE", -1},
    {1104, "DRVDEFDRIVERPROC", -1}, {1210, "MMIOOPEN", 12}, {1211, "MMIOCLOSE", 4}, {1212, "MMIOREAD", 10},
    {1213, "MMIOWRITE", 10}, {1214, "MMIOSEEK", 8}, {1215, "MMIOGETINFO", 8}, {1216, "MMIOSETINFO", 8},
    {1217, "MMIOSETBUFFER", 12}, {1218, "MMIOFLUSH", 4}, {1219, "MMIOADVANCE", 8}, {1220, "MMIOSTRINGTOFOURCC", 6},
    {1221, "MMIOINSTALLIOPROC", 12}, {1222, "MMIOSENDMESSAGE", 12}, {1223, "MMIODESCEND", 12}, {1224, "MMIOASCEND", 8},
    {1225, "MMIOCREATECHUNK", 8}, {1226, "MMIORENAME", 16},
};

/* Exports of the list this runtime does not implement: the driver-side services (DrvOpen..., mciDriver*,
 * mmTask*), mciParseCommand and the command-table resources, and the whole mmio family. None of them is
 * imported by WRITER or ARTIST. An unimplemented call still stops the run loudly (hle's logging stub). */
static const unsigned unimplemented[] = {6, 34, 704, 705, 707, 708, 710, 711, 713, 715, 900, 902, 903, 904, 905, 1100, 1101,
                                         1102, 1103, 1104, 1210, 1211, 1212, 1213, 1214, 1215, 1216, 1217, 1218, 1219,
                                         1220, 1221, 1222, 1223, 1224, 1225, 1226};

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20)) return 0;
    return *a == *b;
}

static bool is_unimpl(unsigned ord) {
    size_t i;
    for (i = 0; i < sizeof unimplemented / sizeof unimplemented[0]; i++)
        if (unimplemented[i] == ord) return true;
    return false;
}

static void test_table_against_exports(void) {
    size_t i, impl = 0, none = 0;
    printf("test_mm_api_table\n");
    mmboot(true);
    for (i = 0; i < sizeof win31_exports / sizeof win31_exports[0]; i++) {
        const hle_entry *e = hle_find(&uh, "MMSYSTEM", (uint16_t)win31_exports[i].ord);
        if (is_unimpl(win31_exports[i].ord)) {
            CHECK(e == NULL);
            none++;
            continue;
        }
        CHECK(e != NULL && e->fn != NULL);
        if (!e) {
            printf("    MMSYSTEM.%u %s is not registered\n", win31_exports[i].ord, win31_exports[i].name);
            continue;
        }
        impl++;
        CHECK(ieq(e->name, win31_exports[i].name));
        if (win31_exports[i].purge >= 0) CHECK_EQ(e->arg_bytes, win31_exports[i].purge);
        else if (e->arg_bytes != 2 && e->arg_bytes != 0) printf("    MMSYSTEM.%u %s: %u bytes (no purge in the list)\n", e->ordinal, e->name, e->arg_bytes);
    }
    CHECK_EQ(impl + none, sizeof win31_exports / sizeof win31_exports[0]);
    CHECK_EQ(none, sizeof unimplemented / sizeof unimplemented[0]);
    printf("  %zu of the %zu exports handled, %zu unimplemented on purpose\n", impl, impl + none, none);
    mmdown();
}

/* The names in WRITER's import table for its MMSYSTEM imports are the names registered
 * for the ordinals WRITER imports: with the ordinals themselves checked against the import table above, the
 * 22 ordinals are VERIFIED (a pairing by set only would not see two names swapped). */
static void test_real_imports(void) {
    static const char *const envs[2] = {"CWFA_WRITER_EXE", "CWFA_ARTIST_EXE"};
    unsigned seen[64], nseen = 0, wavein = 0;
    size_t e_i;
    printf("test_mm_api_real_imports\n");
    mmboot(true);
    for (e_i = 0; e_i < 2; e_i++) {
        const char *path = getenv(envs[e_i]);
        ne_image ne;
        ne_import *imp = NULL;
        size_t n = 0, i;
        unsigned total = 0;
        char name[NE_NAME_MAX + 1];
        if (!path || !*path) {
            printf("  SKIP (%s not set)\n", envs[e_i]);
            continue;
        }
        CHECK_EQ(ne_open_file(&ne, path), NE_OK);
        CHECK_EQ(ne_collect_imports(&ne, &imp, &n), NE_OK);
        for (i = 0; i < n; i++) {
            const hle_entry *e;
            size_t g;
            unsigned k;
            if (ne_modref_name(&ne, imp[i].modref, name) != NE_OK || strcmp(name, "MMSYSTEM")) continue;
            CHECK(!imp[i].by_name);
            total++;
            e = hle_find(&uh, "MMSYSTEM", imp[i].ordinal);
            CHECK(e != NULL && e->fn != NULL);
            for (g = 0; g < sizeof win31_exports / sizeof win31_exports[0] && win31_exports[g].ord != imp[i].ordinal; g++) {}
            CHECK(g < sizeof win31_exports / sizeof win31_exports[0]);
            for (k = 0; k < nseen && seen[k] != imp[i].ordinal; k++) {}
            if (k == nseen && nseen < 64) {
                seen[nseen++] = imp[i].ordinal;
                if (imp[i].ordinal >= 501 && imp[i].ordinal <= 514) wavein++;
            }
        }
        printf("  %s: %u MMSYSTEM imports, all handled\n", envs[e_i], total);
        CHECK_EQ(total, e_i == 0 ? 22 : 21);
        ne_free_imports(imp);
        ne_free(&ne);
    }
    if (nseen) {
        printf("  union: %u ordinals, %u of them waveIn (answered as an absent capture device)\n", nseen, wavein);
        CHECK_EQ(nseen, 22);
        CHECK_EQ(wavein, 9);
    }
    mmdown();
}

/* tests/win16_imports.h: the 22 names WRITER's import table gives its MMSYSTEM imports are exactly the names of the
 * registered handlers for the 22 imported ordinals (the same set, and each ordinal's name checked above). */
static void test_import_names(void) {
    static const unsigned writer[22] = {2, 401, 402, 404, 405, 406, 407, 408, 411, 415, 416, 419, 502, 504, 505, 506, 507,
                                        508, 509, 511, 512, 607};
    unsigned tsv = 0, found = 0, i, k;
    printf("test_mm_api_import_names\n");
    mmboot(true);
    for (k = 0; k < WIN16_IMPORT_COUNT; k++) {
        if (strcmp(win16_imports[k].module, "MMSYSTEM")) continue;
        tsv++;
        for (i = 0; i < 22; i++) {
            const hle_entry *e = hle_find(&uh, "MMSYSTEM", (uint16_t)writer[i]);
            if (e && ieq(e->name, win16_imports[k].name)) { found++; break; }
        }
    }
    CHECK_EQ(tsv, 22);
    CHECK_EQ(found, 22);
    mmdown();
}

int main(void) {
    test_table_against_exports();
    test_import_names();
    test_real_imports();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
