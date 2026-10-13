/* Scripted input. See inject.h. */
#include "inject.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { S_WAIT, S_WAITMS, S_KEY, S_KEYDOWN, S_KEYUP, S_MOVE, S_CLICK, S_DBLCLICK, S_RCLICK, S_DOWN, S_UP, S_QUIT, S_SHOT, S_STOP } skind;

typedef struct {
    skind kind;
    unsigned vk, mods;   /* mods: bit 0 shift, 1 ctrl, 2 alt */
    int x, y;
    unsigned n;
    unsigned ch;         /* S_KEY: the ANSI character the key produces when it is not derived from the VK (accents) */
    char path[256];
} step;

struct inject {
    step *s;
    size_t n, pos;
    unsigned wait;
    bool timing;         /* a waitms step is running */
    bool stopped;        /* the stop step was reached */
    uint32_t deadline;
};

#define M_SHIFT 1
#define M_CTRL 2
#define M_ALT 4

static const struct { const char *name; unsigned vk; } names[] = {
    {"RETURN", 0x0D}, {"ENTER", 0x0D}, {"ESCAPE", 0x1B}, {"ESC", 0x1B}, {"TAB", 0x09}, {"SPACE", 0x20}, {"BACK", 0x08},
    {"BACKSPACE", 0x08}, {"DELETE", 0x2E}, {"DEL", 0x2E}, {"INSERT", 0x2D}, {"HOME", 0x24}, {"END", 0x23}, {"PRIOR", 0x21},
    {"PAGEUP", 0x21}, {"NEXT", 0x22}, {"PAGEDOWN", 0x22}, {"LEFT", 0x25}, {"UP", 0x26}, {"RIGHT", 0x27}, {"DOWN", 0x28},
    {"SHIFT", 0x10}, {"CONTROL", 0x11}, {"CTRL", 0x11}, {"ALT", 0x12}, {"MENU", 0x12}};

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    return *a == *b;
}

static bool key_spec(const char *s, unsigned *vk, unsigned *mods) {
    char buf[64];
    size_t i;
    *mods = 0;
    for (;;) {
        const char *plus = strchr(s, '+');
        size_t n = plus ? (size_t)(plus - s) : 0;
        if (!plus || n == 0 || n >= sizeof buf || s[n + 1] == 0) break;
        memcpy(buf, s, n);
        buf[n] = 0;
        if (ieq(buf, "SHIFT")) *mods |= M_SHIFT;
        else if (ieq(buf, "CTRL") || ieq(buf, "CONTROL")) *mods |= M_CTRL;
        else if (ieq(buf, "ALT")) *mods |= M_ALT;
        else return false;
        s = plus + 1;
    }
    if (strlen(s) == 1 && isalnum((unsigned char)s[0])) {
        *vk = (unsigned)toupper((unsigned char)s[0]);
        return true;
    }
    if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1])) {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 12) {
            *vk = 0x6Fu + (unsigned)n;
            return true;
        }
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (ieq(s, names[i].name)) {
            *vk = names[i].vk;
            return true;
        }
    return false;
}

static bool point(const char *s, int *x, int *y) {
    char *end;
    long a = strtol(s, &end, 10), b;
    if (end == s || *end != ',') return false;
    b = strtol(end + 1, &end, 10);
    if (*end) return false;
    *x = (int)a;
    *y = (int)b;
    return true;
}

static bool add(inject_t *in, const step *s) {
    step *v = realloc(in->s, (in->n + 1) * sizeof *v);
    if (!v) return false;
    in->s = v;
    in->s[in->n++] = *s;
    return true;
}

/* The VK (and shift) of a character typed on a US keyboard. */
static bool char_key(char ch, unsigned *vk, unsigned *mods) {
    static const char shifted[] = ")!@#$%^&*(";
    const char *p;
    *mods = 0;
    if (isalpha((unsigned char)ch)) {
        *vk = (unsigned)toupper((unsigned char)ch);
        if (isupper((unsigned char)ch)) *mods = M_SHIFT;
        return true;
    }
    if (isdigit((unsigned char)ch)) {
        *vk = (unsigned)ch;
        return true;
    }
    if (ch != 0 && (p = strchr(shifted, ch))) {
        *vk = (unsigned)('0' + (p - shifted));
        *mods = M_SHIFT;
        return true;
    }
    switch (ch) {
    case ' ': *vk = 0x20; return true;
    case '.': *vk = 0xBE; return true;
    case ',': *vk = 0xBC; return true;
    case '-': *vk = 0xBD; return true;
    case '=': *vk = 0xBB; return true;
    case ';': *vk = 0xBA; return true;
    case '/': *vk = 0xBF; return true;
    case '\'': *vk = 0xDE; return true;
    case '?': *vk = 0xBF; *mods = M_SHIFT; return true;
    case ':': *vk = 0xBA; *mods = M_SHIFT; return true;
    case '"': *vk = 0xDE; *mods = M_SHIFT; return true;
    case '+': *vk = 0xBB; *mods = M_SHIFT; return true;
    default: return false;
    }
}

/* A Latin-1 character (UTF-8 in the script, ANSI 1252 on the wire) as the key that produces it on a Spanish
 * keyboard: vowels through the dead acute accent, N-tilde on its own key. Only the character matters to the
 * programs (WM_CHAR); the VK is the base letter or an OEM key. */
static bool latin1_key(unsigned cp, unsigned *vk, unsigned *mods) {
    static const struct { unsigned char ansi, vk, shift; } map[] = {
        {0xE1, 'A', 0}, {0xE9, 'E', 0}, {0xED, 'I', 0}, {0xF3, 'O', 0}, {0xFA, 'U', 0}, {0xFC, 'U', 0}, {0xE7, 'C', 0},
        {0xC1, 'A', 1}, {0xC9, 'E', 1}, {0xCD, 'I', 1}, {0xD3, 'O', 1}, {0xDA, 'U', 1}, {0xDC, 'U', 1}, {0xC7, 'C', 1},
        {0xF1, 0xC0, 0}, {0xD1, 0xC0, 1}, {0xBF, 0xDD, 1}, {0xA1, 0xDD, 0}};
    size_t i;
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].ansi == cp) {
            *vk = map[i].vk;
            *mods = map[i].shift ? M_SHIFT : 0;
            return true;
        }
    return false;
}

inject_t *inject_parse(const char *spec, char *err, size_t errcap) {
    inject_t *in = calloc(1, sizeof *in);
    char *copy, *tok, *save = NULL;
    if (!in) return NULL;
    copy = malloc(strlen(spec) + 1);
    if (!copy) {
        free(in);
        return NULL;
    }
    strcpy(copy, spec);
    for (tok = copy; tok; tok = save) {
        char *semi = strchr(tok, ';'), *arg;
        step s;
        memset(&s, 0, sizeof s);
        if (semi) {
            *semi = 0;
            save = semi + 1;
        } else {
            save = NULL;
        }
        while (*tok == ' ') tok++;
        if (!*tok) continue;
        arg = strchr(tok, ':');
        if (arg) *arg++ = 0;
        if (ieq(tok, "wait") && arg) {
            s.kind = S_WAIT;
            s.n = (unsigned)atoi(arg);
        } else if (ieq(tok, "stop")) {
            s.kind = S_STOP;
        } else if (ieq(tok, "waitms") && arg && isdigit((unsigned char)arg[0])) {
            s.kind = S_WAITMS;
            s.n = (unsigned)atoi(arg);
        } else if ((ieq(tok, "key") || ieq(tok, "keydown") || ieq(tok, "keyup")) && arg) {
            s.kind = ieq(tok, "key") ? S_KEY : ieq(tok, "keydown") ? S_KEYDOWN : S_KEYUP;
            if (!key_spec(arg, &s.vk, &s.mods)) {
                snprintf(err, errcap, "bad key \"%s\"", arg);
                goto bad;
            }
        } else if (ieq(tok, "type") && arg) {
            const char *p;
            for (p = arg; *p; p++) {
                step k;
                memset(&k, 0, sizeof k);
                k.kind = S_KEY;
                if ((unsigned char)p[0] >= 0xC2 && (unsigned char)p[0] <= 0xC3 && ((unsigned char)p[1] & 0xC0) == 0x80) {
                    unsigned cp = ((unsigned)(p[0] & 0x1F) << 6) | ((unsigned)p[1] & 0x3F);
                    if (!latin1_key(cp, &k.vk, &k.mods)) {
                        snprintf(err, errcap, "cannot type U+%04X", cp);
                        goto bad;
                    }
                    k.ch = cp;
                    p++;
                } else if (!char_key(*p, &k.vk, &k.mods)) {
                    snprintf(err, errcap, "cannot type '%c'", *p);
                    goto bad;
                }
                if (!add(in, &k)) goto bad;
            }
            continue;
        } else if ((ieq(tok, "move") || ieq(tok, "click") || ieq(tok, "dblclick") || ieq(tok, "rclick") || ieq(tok, "down") ||
                    ieq(tok, "up")) && arg) {
            s.kind = ieq(tok, "move") ? S_MOVE : ieq(tok, "click") ? S_CLICK : ieq(tok, "dblclick") ? S_DBLCLICK
                     : ieq(tok, "rclick") ? S_RCLICK : ieq(tok, "down") ? S_DOWN : S_UP;
            if (!point(arg, &s.x, &s.y)) {
                snprintf(err, errcap, "bad position \"%s\"", arg);
                goto bad;
            }
        } else if (ieq(tok, "quit")) {
            s.kind = S_QUIT;
        } else if (ieq(tok, "shot") && arg && *arg) {
            s.kind = S_SHOT;
            snprintf(s.path, sizeof s.path, "%s", arg);
        } else {
            snprintf(err, errcap, "unknown step \"%s\"", tok);
            goto bad;
        }
        if (!add(in, &s)) goto bad;
    }
    free(copy);
    return in;
bad:
    free(copy);
    inject_free(in);
    return NULL;
}

static void key_event(wm_t *w, wm_in_kind kind, unsigned vk, unsigned ch) {
    wm_input e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.vk = vk;
    e.ch = ch;
    e.scan = vk;
    wm_input_event(w, &e);
}

static void mods(wm_t *w, unsigned m, bool down) {
    static const unsigned vks[3] = {0x10, 0x11, 0x12};
    int i;
    for (i = 0; i < 3; i++)
        if (m & (1u << i)) key_event(w, down ? WM_IN_KEY_DOWN : WM_IN_KEY_UP, vks[i], 0);
}

static void mouse(wm_t *w, wm_in_kind kind, int x, int y, int button) {
    wm_input e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.x = x;
    e.y = y;
    e.button = button;
    wm_input_event(w, &e);
}

bool inject_run(inject_t *in, wm_t *w, const fb_t *fb) {
    if (in->wait) {
        in->wait--;
        return true;
    }
    if (in->timing) {                                  /* waitms: the window manager's clock, whatever the frame rate */
        if ((int32_t)(wm_now(w) - in->deadline) < 0) return true;
        in->timing = false;
    }
    while (in->pos < in->n) {
        const step *s = &in->s[in->pos++];
        switch (s->kind) {
        case S_WAIT:
            if (s->n) in->wait = s->n - 1;
            return true;
        case S_STOP:
            in->stopped = true;
            return true;
        case S_WAITMS:
            in->timing = true;
            in->deadline = wm_now(w) + s->n;
            return true;
        case S_KEY:
            mods(w, s->mods, true);
            key_event(w, WM_IN_KEY_DOWN, s->vk, s->ch);
            key_event(w, WM_IN_KEY_UP, s->vk, 0);
            mods(w, s->mods, false);
            return true;
        case S_KEYDOWN:
            mods(w, s->mods, true);
            key_event(w, WM_IN_KEY_DOWN, s->vk, s->ch);
            return true;
        case S_KEYUP:
            key_event(w, WM_IN_KEY_UP, s->vk, 0);
            mods(w, s->mods, false);
            return true;
        case S_MOVE: mouse(w, WM_IN_MOVE, s->x, s->y, 0); return true;
        case S_CLICK:
        case S_DBLCLICK:
        case S_RCLICK: {
            int b = s->kind == S_RCLICK ? 1 : 0, rep = s->kind == S_DBLCLICK ? 2 : 1, i;
            mouse(w, WM_IN_MOVE, s->x, s->y, 0);
            for (i = 0; i < rep; i++) {
                mouse(w, WM_IN_BUTTON_DOWN, s->x, s->y, b);
                mouse(w, WM_IN_BUTTON_UP, s->x, s->y, b);
            }
            return true;
        }
        case S_DOWN: mouse(w, WM_IN_MOVE, s->x, s->y, 0); mouse(w, WM_IN_BUTTON_DOWN, s->x, s->y, 0); return true;
        case S_UP: mouse(w, WM_IN_BUTTON_UP, s->x, s->y, 0); return true;
        case S_QUIT:
            mouse(w, WM_IN_QUIT, 0, 0, 0);
            return true;
        case S_SHOT:
            if (fb && !fb_save_ppm(fb, s->path)) fprintf(stderr, "inject: cannot write %s\n", s->path);
            break;
        }
    }
    return false;
}

bool inject_stopped(const inject_t *in) { return in->stopped; }
bool inject_done(const inject_t *in) { return in->pos >= in->n && in->wait == 0 && !in->timing; }
size_t inject_steps_left(const inject_t *in) { return in->n - in->pos; }

void inject_free(inject_t *in) {
    if (!in) return;
    free(in->s);
    free(in);
}
