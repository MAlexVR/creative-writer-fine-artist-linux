/* The USER, KEYBOARD and COMMDLG import surface of WRITER and ARTIST: every import has a handler;
 * the ones that are logged stubs or answers instead of real behaviour are listed with the reason. */
#include <stdlib.h>
#include <string.h>

#include "ne.h"

#include "ufixture.h"
#include "gfixture.h"

typedef struct { const char *module; unsigned ordinal; } mo;

/* Not real behaviour: COMMDLG's file and
 * print dialogs are answered as cancelled. */
static const mo stubs[] = {{"COMMDLG", 1}, {"COMMDLG", 2}, {"COMMDLG", 20}};

static bool is_stub(const char *m, unsigned o) {
    size_t i;
    for (i = 0; i < sizeof stubs / sizeof stubs[0]; i++)
        if (!strcmp(stubs[i].module, m) && stubs[i].ordinal == o) return true;
    return false;
}

static void test_real_imports(void) {
    static const char *const envs[2] = {"CWFA_WRITER_EXE", "CWFA_ARTIST_EXE"};
    static const char *const mods[3] = {"USER", "KEYBOARD", "COMMDLG"};
    unsigned seen[3][512], nseen[3] = {0, 0, 0}, impl[3] = {0, 0, 0}, stub[3] = {0, 0, 0}, e_i, m_i;
    printf("test_ui_real_imports\n");
    gboot();
    for (e_i = 0; e_i < 2; e_i++) {
        const char *path = getenv(envs[e_i]);
        ne_image ne;
        ne_import *imp = NULL;
        size_t n = 0, i;
        char name[NE_NAME_MAX + 1];
        unsigned total[3] = {0, 0, 0}, missing = 0, by_name = 0;
        if (!path || !*path) {
            printf("  SKIP (%s not set)\n", envs[e_i]);
            continue;
        }
        CHECK_EQ(ne_open_file(&ne, path), NE_OK);
        CHECK_EQ(ne_collect_imports(&ne, &imp, &n), NE_OK);
        for (i = 0; i < n; i++) {
            const hle_entry *e;
            unsigned k;
            if (ne_modref_name(&ne, imp[i].modref, name) != NE_OK) continue;
            for (m_i = 0; m_i < 3 && strcmp(name, mods[m_i]); m_i++) {}
            if (m_i == 3) continue;
            if (imp[i].by_name) { by_name++; continue; }
            total[m_i]++;
            e = hle_find(&uh, mods[m_i], imp[i].ordinal);
            if (!e || !e->fn) {
                missing++;
                printf("    %s.%u has no handler\n", mods[m_i], imp[i].ordinal);
            }
            for (k = 0; k < nseen[m_i] && seen[m_i][k] != imp[i].ordinal; k++) {}
            if (k == nseen[m_i] && nseen[m_i] < 512) {
                seen[m_i][nseen[m_i]++] = imp[i].ordinal;
                if (e && e->fn) {
                    if (is_stub(mods[m_i], imp[i].ordinal)) stub[m_i]++;
                    else impl[m_i]++;
                }
            }
        }
        printf("  %s: USER %u, KEYBOARD %u, COMMDLG %u imports (%u by name), %u without a handler\n", envs[e_i],
               total[0], total[1], total[2], by_name, missing);
        CHECK_EQ(missing, 0);
        CHECK_EQ(by_name, 0);
        ne_free_imports(imp);
        ne_free(&ne);
    }
    if (nseen[0]) {
        for (m_i = 0; m_i < 3; m_i++)
            printf("  union %s: %u ordinals, %u implemented, %u stubbed\n", mods[m_i], nseen[m_i], impl[m_i], stub[m_i]);
        CHECK_EQ(nseen[0], 106u);
        CHECK_EQ(stub[0], 0u);
        CHECK_EQ(impl[0], 106u);
        CHECK_EQ(nseen[1], 3u);
        CHECK_EQ(stub[1], 0u);
        CHECK_EQ(nseen[2], 3u);
        CHECK_EQ(stub[2], 2u);
    }
    gdown();
}

int main(void) {
    test_real_imports();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
