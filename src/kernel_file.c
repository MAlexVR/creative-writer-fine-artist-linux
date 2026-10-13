/* KERNEL file I/O, DOS services and profile (.INI) functions over dospath/ini. */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "ini.h"
#include "kernel_priv.h"

#define HFILE_ERROR 0xFFFFu
#define DOS_FIRST_HANDLE 5
#define OFSTRUCT_SIZE 136

#define OF_PARSE 0x0100
#define OF_DELETE 0x0200
#define OF_CREATE 0x1000
#define OF_EXIST 0x4000
#define OF_REOPEN 0x8000

/* DOS error codes */
#define E_BADFUNC 1
#define E_NOFILE 2
#define E_NOPATH 3
#define E_NOHANDLES 4
#define E_ACCESS 5
#define E_BADHANDLE 6
#define E_NOMOREFILES 18

static bool read_path(hle_call *c, uint16_t sel, uint16_t off, char *out, size_t cap) {
    return hle_read_cstr(c, sel, off, out, cap);
}

/* ---- handle table --------------------------------------------------------- */

static FILE *file_of(kernel_t *k, uint16_t h) {
    return h >= DOS_FIRST_HANDLE && h - DOS_FIRST_HANDLE < KERNEL_MAX_FILES
               ? k->files[h - DOS_FIRST_HANDLE] : NULL;
}

static int file_add(kernel_t *k, FILE *fp) {
    unsigned i;
    for (i = 0; i < KERNEL_MAX_FILES; i++)
        if (!k->files[i]) {
            k->files[i] = fp;
            return (int)(i + DOS_FIRST_HANDLE);
        }
    return -1;
}

static bool file_close(kernel_t *k, uint16_t h) {
    FILE *fp = file_of(k, h);
    if (!fp) return false;
    k->files[h - DOS_FIRST_HANDLE] = NULL;
    fclose(fp);
    return true;
}

void kernel_file_destroy(kernel_t *k) {
    unsigned i;
    for (i = 0; i < KERNEL_MAX_FILES; i++)
        if (k->files[i]) fclose(k->files[i]);
    memset(k->files, 0, sizeof k->files);
    dp_free(&k->dp);
}

/* Opens a DOS path. Returns 0 or a DOS error code; *res tells where it landed. */
static int open_dos(kernel_t *k, const char *dos, unsigned mode, bool create, int *handle, dp_result *res) {
    FILE *fp;
    *handle = -1;
    if (!dp_resolve(&k->dp, dos, create ? DP_CREATE_DIRS : 0, res)) return E_NOPATH;
    if (!create && !res->exists) return E_NOFILE;
    if (res->exists && res->is_dir) return E_ACCESS;
    fp = fopen(res->host, create ? "w+b" : (mode & 3) ? "r+b" : "rb");
    if (!fp) return E_ACCESS;
    if ((*handle = file_add(k, fp)) < 0) {
        fclose(fp);
        return E_NOHANDLES;
    }
    return 0;
}

/* ---- _l* ------------------------------------------------------------------- */

cpu_status k_lopen(hle_call *c) {
    hle_args a;
    uint16_t sel, off, mode;
    char path[DP_DOS_MAX * 4];
    dp_result r;
    int h;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    mode = hle_u16(&a);
    if (a.st) return a.st;
    if (!read_path(c, sel, off, path, sizeof path) || open_dos(c->ctx, path, mode, false, &h, &r))
        h = -1;
    hle_ret16(c, (uint16_t)h);
    return CPU_OK;
}

cpu_status k_lcreat(hle_call *c) {
    hle_args a;
    uint16_t sel, off;
    char path[DP_DOS_MAX * 4];
    dp_result r;
    int h;
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    hle_u16(&a);
    if (a.st) return a.st;
    if (!read_path(c, sel, off, path, sizeof path) || open_dos(c->ctx, path, 2, true, &h, &r)) h = -1;
    hle_ret16(c, (uint16_t)h);
    return CPU_OK;
}

cpu_status k_lclose(hle_call *c) {
    hle_args a;
    hle_args_init(&a, c);
    hle_ret16(c, file_close(c->ctx, hle_u16(&a)) ? 0 : (uint16_t)HFILE_ERROR);
    return a.st;
}

static cpu_status rw16(hle_call *c, bool write) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t h, sel, off, n;
    FILE *fp;
    uint8_t *p;
    size_t done;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    hle_far(&a, &sel, &off);
    n = hle_u16(&a);
    if (a.st) return a.st;
    if (!(fp = file_of(k, h))) {
        hle_ret16(c, (uint16_t)HFILE_ERROR);
        return CPU_OK;
    }
    if (n == 0) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (!(p = hle_ptr(c, sel, off, n))) return CPU_E_LIMIT;
    done = write ? fwrite(p, 1, n, fp) : fread(p, 1, n, fp);
    if (write) fflush(fp);
    hle_ret16(c, done < n && ferror(fp) ? (uint16_t)HFILE_ERROR : (uint16_t)done);
    if (ferror(fp)) clearerr(fp);
    return CPU_OK;
}

cpu_status k_lread(hle_call *c) { return rw16(c, false); }
cpu_status k_lwrite(hle_call *c) { return rw16(c, true); }

static cpu_status rw32(hle_call *c, bool write) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t h, sel, off16;
    uint32_t n, off, total = 0;
    FILE *fp;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    hle_far(&a, &sel, &off16);
    n = hle_u32(&a);
    if (a.st) return a.st;
    off = off16;
    if (!(fp = file_of(k, h))) {
        hle_ret32(c, 0xFFFFFFFFu);
        return CPU_OK;
    }
    while (n) {
        uint32_t chunk = n < 0x10000 - off ? n : 0x10000 - off;
        uint8_t *p = hle_ptr(c, sel, off, chunk);
        size_t done;
        if (!p) return CPU_E_LIMIT;
        done = write ? fwrite(p, 1, chunk, fp) : fread(p, 1, chunk, fp);
        total += (uint32_t)done;
        if (done < chunk) break;
        n -= chunk;
        hle_huge_add(&sel, &off, chunk);
    }
    if (write) fflush(fp);
    hle_ret32(c, ferror(fp) ? 0xFFFFFFFFu : total);
    clearerr(fp);
    return CPU_OK;
}

cpu_status k_hread(hle_call *c) { return rw32(c, false); }
cpu_status k_hwrite(hle_call *c) { return rw32(c, true); }

static long seek_file(FILE *fp, long off, int origin) {
    static const int whence[3] = {SEEK_SET, SEEK_CUR, SEEK_END};
    if (origin < 0 || origin > 2 || fseek(fp, off, whence[origin]) != 0) return -1;
    return ftell(fp);
}

cpu_status k_llseek(hle_call *c) {
    hle_args a;
    uint16_t h, origin;
    int32_t off;
    FILE *fp;
    long pos;
    hle_args_init(&a, c);
    h = hle_u16(&a);
    off = (int32_t)hle_u32(&a);
    origin = hle_u16(&a);
    if (a.st) return a.st;
    fp = file_of(c->ctx, h);
    pos = fp ? seek_file(fp, off, origin) : -1;
    hle_ret32(c, pos < 0 || pos > 0x7FFFFFFF ? 0xFFFFFFFFu : (uint32_t)pos);
    return CPU_OK;
}

/* ---- OpenFile ---------------------------------------------------------------- */

static bool has_dir(const char *s) { return strpbrk(s, "\\/:") != NULL; }

/* Where does a bare name live: current directory, Windows, system, program dir. */
static bool locate(kernel_t *k, const char *name, dp_result *r) {
    char cand[DP_DOS_MAX * 4], *slash;
    unsigned i;
    if (has_dir(name)) return dp_resolve(&k->dp, name, 0, r) && r->exists;
    for (i = 0; i < 4; i++) {
        if (i == 0) snprintf(cand, sizeof cand, "%s", name);
        else if (i == 1) snprintf(cand, sizeof cand, "%s\\%.255s", k->windows_dir, name);
        else if (i == 2) snprintf(cand, sizeof cand, "%s\\SYSTEM\\%.255s", k->windows_dir, name);
        else {
            snprintf(cand, sizeof cand, "%s", k->exe_path);
            if (!(slash = strrchr(cand, '\\'))) break;
            snprintf(slash + 1, sizeof cand - (size_t)(slash + 1 - cand), "%s", name);
        }
        if (dp_resolve(&k->dp, cand, 0, r) && r->exists && !r->is_dir) return true;
    }
    return false;
}

static void fill_ofstruct(kernel_t *k, uint8_t *of, const char *dos, unsigned err) {
    unsigned drive = dos[0] ? (unsigned)(toupper((unsigned char)dos[0]) - 'A') : 2;
    memset(of, 0, OFSTRUCT_SIZE);
    of[0] = OFSTRUCT_SIZE;
    of[1] = drive >= 2; /* fFixedDisk */
    of[2] = (uint8_t)err;
    of[3] = (uint8_t)(err >> 8);
    snprintf((char *)of + 8, 128, "%s", dos);
    (void)k;
}

cpu_status k_openfile(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t nsel, noff, osel, ooff, style;
    char name[DP_DOS_MAX * 4];
    uint8_t *of;
    dp_result r;
    int h = -1;
    unsigned err = 0;
    hle_args_init(&a, c);
    hle_far(&a, &nsel, &noff);
    hle_far(&a, &osel, &ooff);
    style = hle_u16(&a);
    if (a.st) return a.st;
    if (!(of = hle_ptr(c, osel, ooff, OFSTRUCT_SIZE))) return CPU_E_LIMIT;
    if (style & OF_REOPEN) snprintf(name, sizeof name, "%.127s", (char *)of + 8);
    else if (!read_path(c, nsel, noff, name, sizeof name)) return CPU_E_SEL;
    memset(&r, 0, sizeof r);
    if (style & OF_PARSE) { /* canonical name only */
        char full[DP_DOS_MAX * 4];
        snprintf(full, sizeof full, "%s", name);
        if (dp_resolve(&k->dp, full, 0, &r)) fill_ofstruct(k, of, r.dos, 0);
        else fill_ofstruct(k, of, name, E_NOPATH);
        if (k->cfg.debug) fprintf(k->cfg.debug, "OpenFile(\"%s\", PARSE) -> %s\n", name, r.dos[0] ? r.dos : "path error");
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (style & OF_CREATE) {
        err = (unsigned)open_dos(k, name, 2, true, &h, &r);
    } else if (!locate(k, name, &r)) {
        err = dp_resolve(&k->dp, name, 0, &r) || !has_dir(name) ? E_NOFILE : E_NOPATH;
        if (has_dir(name) && !dp_resolve(&k->dp, name, 0, &r)) err = E_NOPATH;
    } else if (style & OF_DELETE) {
        err = remove(r.host) == 0 ? 0 : E_ACCESS;
        h = err ? -1 : 1;
    } else if (style & OF_EXIST) {
        h = 1;
    } else {
        char found[DP_DOS_MAX]; /* open_dos rewrites r: keep the path it is given */
        snprintf(found, sizeof found, "%s", r.dos);
        err = (unsigned)open_dos(k, found, style & 3, false, &h, &r);
    }
    fill_ofstruct(k, of, r.dos[0] ? r.dos : name, err);
    if (k->cfg.debug)
        fprintf(k->cfg.debug, "OpenFile(\"%s\", %04X) -> %s (error %u)\n", name, style, err ? "failed" : "ok", err);
    hle_ret16(c, err ? (uint16_t)HFILE_ERROR : (uint16_t)h);
    return CPU_OK;
}

/* ---- AccessResource ------------------------------------------------------------ */

/* The file a module was loaded from: the program's host file, a library's path through the DOS drives. */
FILE *kernel_module_file(kernel_t *k, uint16_t inst) {
    const loader_module *lm = loader_module_by_handle(k->p, inst);
    dp_result r;
    if (!lm) return NULL;
    if (lm == k->p->mods[0]) return k->cfg.exe_host_path ? fopen(k->cfg.exe_host_path, "rb") : NULL;
    return dp_resolve(&k->dp, lm->path, 0, &r) && r.exists ? fopen(r.host, "rb") : NULL;
}

cpu_status k_accessresource(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t inst, hr;
    const ne_resource *r;
    FILE *fp;
    int h = -1;
    hle_args_init(&a, c);
    inst = hle_u16(&a);
    hr = hle_u16(&a);
    if (a.st) return a.st;
    if ((r = kernel_res_get(k, inst, hr)) && (fp = kernel_module_file(k, inst))) {
        if (fseek(fp, (long)r->offset, SEEK_SET) == 0 && (h = file_add(k, fp)) >= 0) {
            hle_ret16(c, (uint16_t)h);
            return CPU_OK;
        }
        fclose(fp);
    }
    hle_ret16(c, (uint16_t)HFILE_ERROR);
    return CPU_OK;
}

/* ---- GetTempFileName ------------------------------------------------------------- */

cpu_status k_gettempfilename(hle_call *c) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t drive, psel, poff, unique, osel, ooff;
    char prefix[16], dir[DP_DOS_MAX], name[DP_DOS_MAX * 4], letter;
    dp_result r;
    uint8_t *out;
    unsigned tries;
    hle_args_init(&a, c);
    drive = hle_u16(&a);
    hle_far(&a, &psel, &poff);
    unique = hle_u16(&a);
    hle_far(&a, &osel, &ooff);
    if (a.st) return a.st;
    hle_ret16(c, 0);
    letter = (char)((drive & 0x7F) ? toupper(drive & 0x7F) : k->dp.cur_drive);
    if (!hle_read_cstr(c, psel, poff, prefix, sizeof prefix)) return CPU_OK;
    prefix[3] = 0;
    snprintf(dir, sizeof dir, "%c:\\TEMP", letter);
    if (!dp_resolve(&k->dp, dir, 0, &r) || !r.exists || !r.is_dir) snprintf(dir, sizeof dir, "%c:", letter);
    for (tries = 0; tries < 0x10000; tries++) {
        unsigned u = unique ? unique : (++k->tmpseq & 0xFFFF);
        if (!u) continue;
        snprintf(name, sizeof name, "%s\\%s%04X.TMP", dir, prefix, u);
        if (!dp_resolve(&k->dp, name, 0, &r)) return CPU_OK;
        if (unique) {
            unique = (uint16_t)u;
            break;
        }
        if (!r.exists) {
            FILE *fp = (drive & 0x80) ? NULL : fopen(r.host, "wb");
            if ((drive & 0x80) || fp) {
                if (fp) fclose(fp);
                unique = (uint16_t)u;
                break;
            }
            return CPU_OK;
        }
    }
    if (!unique || !(out = hle_ptr(c, osel, ooff, (uint32_t)strlen(r.dos) + 1))) return CPU_OK;
    memcpy(out, r.dos, strlen(r.dos) + 1);
    hle_ret16(c, unique);
    return CPU_OK;
}

/* ---- profiles ------------------------------------------------------------------------ */

/* Host path of an .INI: NULL = WIN.INI, a bare name lives in the Windows directory. */
static bool ini_path(kernel_t *k, const char *file, bool create, char *host, size_t cap) {
    char dos[DP_DOS_MAX * 4];
    dp_result r;
    if (!file) snprintf(dos, sizeof dos, "%s\\WIN.INI", k->windows_dir);
    else if (has_dir(file)) snprintf(dos, sizeof dos, "%s", file);
    else snprintf(dos, sizeof dos, "%s\\%.255s", k->windows_dir, file);
    if (!dp_resolve(&k->dp, dos, create ? DP_CREATE_DIRS : 0, &r) || strlen(r.host) >= cap) return false;
    strcpy(host, r.host);
    return true;
}

/* A NULL far pointer (segment 0) reads as a NULL string. */
static bool opt_str(hle_call *c, uint16_t sel, uint16_t off, char *buf, size_t cap, const char **out) {
    if (sel == 0) {
        *out = NULL;
        return true;
    }
    *out = buf;
    return hle_read_cstr(c, sel, off, buf, cap);
}

static cpu_status prof_string(hle_call *c, bool priv) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t as, ao, ks, ko, ds, dof, bs, bo, size, fs = 0, fo = 0;
    char ab[128], kb[128], db[256], fb[DP_DOS_MAX * 4], host[DP_PATH_MAX];
    const char *app, *key, *def, *file = NULL;
    char *tmp;
    uint8_t *dst;
    size_t n;
    hle_args_init(&a, c);
    hle_far(&a, &as, &ao);
    hle_far(&a, &ks, &ko);
    hle_far(&a, &ds, &dof);
    hle_far(&a, &bs, &bo);
    size = hle_u16(&a);
    if (priv) hle_far(&a, &fs, &fo);
    if (a.st) return a.st;
    hle_ret16(c, 0);
    if (!opt_str(c, as, ao, ab, sizeof ab, &app) || !opt_str(c, ks, ko, kb, sizeof kb, &key) ||
        !opt_str(c, ds, dof, db, sizeof db, &def) || (priv && !opt_str(c, fs, fo, fb, sizeof fb, &file)))
        return CPU_E_SEL;
    if (size == 0) return CPU_OK;
    if (!(dst = hle_ptr(c, bs, bo, size))) return CPU_E_LIMIT;
    if (!ini_path(k, file, false, host, sizeof host)) host[0] = 0;
    if (!(tmp = malloc(size))) return CPU_OK;
    n = ini_get_string(host, app, key, def, tmp, size);
    memcpy(dst, tmp, n + 1 < size ? n + 1 : size);
    if (!app || !key) memcpy(dst, tmp, size > n + 2 ? n + 2 : size); /* keep the double NUL */
    free(tmp);
    hle_ret16(c, (uint16_t)n);
    return CPU_OK;
}

cpu_status k_getprofilestring(hle_call *c) { return prof_string(c, false); }
cpu_status k_getprivateprofilestring(hle_call *c) { return prof_string(c, true); }

static cpu_status prof_int(hle_call *c, bool priv) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t as, ao, ks, ko, fs = 0, fo = 0, def;
    char ab[128], kb[128], fb[DP_DOS_MAX * 4], host[DP_PATH_MAX], val[64];
    const char *app, *key, *file = NULL;
    hle_args_init(&a, c);
    hle_far(&a, &as, &ao);
    hle_far(&a, &ks, &ko);
    def = hle_u16(&a);
    if (priv) hle_far(&a, &fs, &fo);
    if (a.st) return a.st;
    hle_ret16(c, def);
    if (!opt_str(c, as, ao, ab, sizeof ab, &app) || !opt_str(c, ks, ko, kb, sizeof kb, &key) ||
        (priv && !opt_str(c, fs, fo, fb, sizeof fb, &file)) || !app || !key)
        return CPU_OK;
    if (ini_path(k, file, false, host, sizeof host) && ini_lookup(host, app, key, val, sizeof val))
        hle_ret16(c, (uint16_t)strtol(val, NULL, 10)); /* no digits: 0 */
    return CPU_OK;
}

cpu_status k_getprofileint(hle_call *c) { return prof_int(c, false); }
cpu_status k_getprivateprofileint(hle_call *c) { return prof_int(c, true); }

static cpu_status prof_write(hle_call *c, bool priv) {
    kernel_t *k = c->ctx;
    hle_args a;
    uint16_t as, ao, ks, ko, vs, vo, fs = 0, fo = 0;
    char ab[128], kb[128], vb[512], fb[DP_DOS_MAX * 4], host[DP_PATH_MAX];
    const char *app, *key, *val, *file = NULL;
    hle_args_init(&a, c);
    hle_far(&a, &as, &ao);
    hle_far(&a, &ks, &ko);
    hle_far(&a, &vs, &vo);
    if (priv) hle_far(&a, &fs, &fo);
    if (a.st) return a.st;
    hle_ret16(c, 0);
    if (!opt_str(c, as, ao, ab, sizeof ab, &app) || !opt_str(c, ks, ko, kb, sizeof kb, &key) ||
        !opt_str(c, vs, vo, vb, sizeof vb, &val) || (priv && !opt_str(c, fs, fo, fb, sizeof fb, &file)) ||
        !app)
        return CPU_OK;
    if (ini_path(k, file, true, host, sizeof host) && ini_write(host, app, key, val)) hle_ret16(c, 1);
    return CPU_OK;
}

cpu_status k_writeprofilestring(hle_call *c) { return prof_write(c, false); }
cpu_status k_writeprivateprofilestring(hle_call *c) { return prof_write(c, true); }

/* ---- directories ------------------------------------------------------------------------- */

static cpu_status dir_out(hle_call *c, const char *s) {
    hle_args a;
    uint16_t sel, off, size;
    uint8_t *dst;
    size_t n = strlen(s);
    hle_args_init(&a, c);
    hle_far(&a, &sel, &off);
    size = hle_u16(&a);
    if (a.st) return a.st;
    if (size == 0) {
        hle_ret16(c, 0);
        return CPU_OK;
    }
    if (n > (size_t)size - 1) n = size - 1;
    if (!(dst = hle_ptr(c, sel, off, (uint32_t)n + 1))) return CPU_E_LIMIT;
    memcpy(dst, s, n);
    dst[n] = 0;
    hle_ret16(c, (uint16_t)n);
    return CPU_OK;
}

cpu_status k_getwindowsdirectory(hle_call *c) {
    kernel_t *k = c->ctx;
    return dir_out(c, k->windows_dir);
}

cpu_status k_getsystemdirectory(hle_call *c) {
    kernel_t *k = c->ctx;
    char s[DP_DOS_MAX + 8];
    snprintf(s, sizeof s, "%s\\SYSTEM", k->windows_dir);
    return dir_out(c, s);
}

/* ---- DOS3Call: the INT 21h subset programs use through KERNEL ------------------------------ */

static cpu_status dos_fail(hle_call *c, unsigned code) {
    ((kernel_t *)c->ctx)->dos_error = (uint16_t)code;
    c->cpu->eflags |= CPU_CF;
    hle_ret16(c, (uint16_t)code);
    return CPU_OK;
}

static cpu_status dos_ok(hle_call *c) {
    c->cpu->eflags &= ~(uint32_t)CPU_CF;
    return CPU_OK;
}

static void set_reg16(cpu_t *cpu, int r, unsigned v) {
    cpu->r[r] = (cpu->r[r] & 0xFFFF0000u) | (v & 0xFFFF);
}


/* ---- FindFirst / FindNext ------------------------------------------------------- */

/* DOS wildcard match of one name part: '*' takes the rest, '?' one character. */
static bool wild_part(const char *p, const char *n) {
    for (; *p; p++) {
        if (*p == '*') return true;
        if (*p == '?') {
            if (*n) n++;
        } else if (toupper((unsigned char)*p) != toupper((unsigned char)*n++)) {
            return false;
        }
    }
    return *n == 0;
}

static bool wild_match(const char *pat, const char *name) {
    char pb[16], pe[16], nb[16], ne[16];
    const char *pd = strchr(pat, '.'), *nd = strchr(name, '.');
    snprintf(pb, sizeof pb, "%.*s", pd ? (int)(pd - pat) : 15, pat);
    snprintf(pe, sizeof pe, "%s", pd ? pd + 1 : "");
    snprintf(nb, sizeof nb, "%.*s", nd ? (int)(nd - name) : 15, name);
    snprintf(ne, sizeof ne, "%s", nd ? nd + 1 : "");
    return wild_part(pb, nb) && wild_part(pe, ne);
}

static bool valid_83(const char *n) {
    const char *d = strchr(n, '.');
    size_t base = d ? (size_t)(d - n) : strlen(n);
    return base >= 1 && base <= 8 && (!d || (strlen(d + 1) <= 3 && !strchr(d + 1, '.')));
}

static int cmp_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Fills the DTA with the entry at index f->next (in name order) that matches; false at the end. */
static bool find_emit(kernel_t *k, hle_call *c, struct kfind *f, unsigned slot) {
    DIR *d = opendir(f->host);
    struct dirent *e;
    char *names[512];
    unsigned n = 0, i, seen = 0;
    bool ok = false;
    uint16_t sel = k->dta_sel ? k->dta_sel : k->task_sel, off = k->dta_sel ? k->dta_off : 0x180;
    if (!d) return false;
    while (n < 512 && (e = readdir(d))) {
        char up[16], full[DP_PATH_MAX + 300];
        struct stat st;
        size_t j;
        if (strlen(e->d_name) > 12) continue;
        for (j = 0; (up[j] = (char)toupper((unsigned char)e->d_name[j])); j++) {}
        if (!strcmp(up, ".") || !strcmp(up, "..") || !valid_83(up) || !wild_match(f->pattern, up)) continue;
        snprintf(full, sizeof full, "%s/%s", f->host, e->d_name);
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode) && !(f->attr & 0x10)) continue;
        if (!(names[n] = strdup(up))) break;
        n++;
    }
    closedir(d);
    qsort(names, n, sizeof names[0], cmp_name);
    for (i = 0; i < n; i++) {
        if (!ok && seen++ == f->next) {
            char path[DP_PATH_MAX + 300];
            struct stat st;
            struct tm tm;
            uint8_t *dta = hle_ptr(c, sel, off, 0x2B);
            snprintf(path, sizeof path, "%s", f->host);
            {
                /* the host name may differ in case: find it again, case-insensitively */
                DIR *d2 = opendir(f->host);
                while (d2 && (e = readdir(d2)))
                    if (!strcasecmp(e->d_name, names[i])) {
                        snprintf(path, sizeof path, "%s/%s", f->host, e->d_name);
                        break;
                    }
                if (d2) closedir(d2);
            }
            if (dta && stat(path, &st) == 0) {
                unsigned date, tim;
                localtime_r(&st.st_mtime, &tm);
                date = (unsigned)(tm.tm_year - 80) << 9 | (unsigned)(tm.tm_mon + 1) << 5 | (unsigned)tm.tm_mday;
                tim = (unsigned)tm.tm_hour << 11 | (unsigned)tm.tm_min << 5 | (unsigned)tm.tm_sec / 2;
                memset(dta, 0, 0x2B);
                dta[0] = (uint8_t)(slot + 1);
                dta[1] = (uint8_t)(f->next + 1);
                dta[2] = (uint8_t)((f->next + 1) >> 8);
                dta[0x15] = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
                dta[0x16] = (uint8_t)tim; dta[0x17] = (uint8_t)(tim >> 8);
                dta[0x18] = (uint8_t)date; dta[0x19] = (uint8_t)(date >> 8);
                dta[0x1A] = (uint8_t)st.st_size; dta[0x1B] = (uint8_t)(st.st_size >> 8);
                dta[0x1C] = (uint8_t)(st.st_size >> 16); dta[0x1D] = (uint8_t)(st.st_size >> 24);
                snprintf((char *)dta + 0x1E, 13, "%s", names[i]);
                f->next++;
                ok = true;
            }
        }
        free(names[i]);
    }
    return ok;
}

/* The volume label of a drive's root as FindFirst reports it, in 8.3 form (the 9th character follows a dot). */
static bool volume_label(kernel_t *k, hle_call *c, const char *dir, unsigned slot) {
    char drive = dir[0] && dir[1] == ':' ? (char)toupper((unsigned char)dir[0]) : k->dp.cur_drive;
    const char *rest = dir[0] && dir[1] == ':' ? dir + 2 : dir;
    char cd = k->cfg.cd_drive ? (char)toupper((unsigned char)k->cfg.cd_drive) : 'D';
    uint16_t sel = k->dta_sel ? k->dta_sel : k->task_sel, off = k->dta_sel ? k->dta_off : 0x180;
    uint8_t *dta;
    size_t n;
    if (!k->cfg.cd_label || drive != cd || (strcmp(rest, "\\") && rest[0]) || !(dta = hle_ptr(c, sel, off, 0x2B)))
        return false;
    n = strlen(k->cfg.cd_label);
    memset(dta, 0, 0x2B);
    dta[0] = (uint8_t)(slot + 1);
    dta[1] = 0xFF;                                   /* nothing follows: FindNext reports the end */
    dta[2] = 0xFF;
    dta[0x15] = 0x08;
    snprintf((char *)dta + 0x1E, 13, "%.8s%s%.3s", k->cfg.cd_label, n > 8 ? "." : "", n > 8 ? k->cfg.cd_label + 8 : "");
    return true;
}

static cpu_status dos_findfirst(hle_call *c, uint16_t ds, uint16_t dx, unsigned attr) {
    kernel_t *k = c->ctx;
    char path[DP_DOS_MAX * 4], dir[DP_DOS_MAX * 4];
    const char *slash;
    dp_result r;
    unsigned s;
    struct kfind *f;
    if (!read_path(c, ds, dx, path, sizeof path)) return CPU_E_SEL;
    slash = strrchr(path, '\\');
    if (!slash && path[0] && path[1] == ':') slash = path + 1;
    if (slash) snprintf(dir, sizeof dir, "%.*s", (int)(slash - path) + 1, path);
    else snprintf(dir, sizeof dir, "%c:%s", k->dp.cur_drive, k->dp.cur_dir[k->dp.cur_drive - 'A']);
    if (slash && slash - path > 2 && slash[-1] != ':') dir[slash - path] = 0;      /* "D:\\DIR\\" -> "D:\\DIR" */
    if (!dp_resolve(&k->dp, dir, 0, &r) || !r.exists || !r.is_dir) return dos_fail(c, E_NOPATH);
    s = k->find_seq++ % 8;
    f = &k->finds[s];
    memset(f, 0, sizeof *f);
    if (attr & 0x08) {                               /* the volume label, if the drive has one */
        if (!volume_label(k, c, dir, s)) return dos_fail(c, E_NOMOREFILES);
        snprintf(f->host, sizeof f->host, "%s", r.host);
        f->pattern[0] = 0;
        f->attr = attr;
        f->used = true;
        f->next = 0xFFFF;
        return dos_ok(c);
    }
    snprintf(f->host, sizeof f->host, "%s", r.host);
    snprintf(f->pattern, sizeof f->pattern, "%.15s", slash ? slash + 1 : path);
    f->attr = attr;
    f->used = true;
    if (k->cfg.debug) fprintf(k->cfg.debug, "FindFirst(\"%s\", attr %02X) in %s\n", path, attr, r.dos);
    if (!find_emit(k, c, f, s)) return dos_fail(c, E_NOMOREFILES);
    return dos_ok(c);
}

static cpu_status dos_findnext(hle_call *c) {
    kernel_t *k = c->ctx;
    uint16_t sel = k->dta_sel ? k->dta_sel : k->task_sel, off = k->dta_sel ? k->dta_off : 0x180;
    const uint8_t *dta = hle_ptr(c, sel, off, 0x2B);
    unsigned slot;
    if (!dta) return CPU_E_SEL;
    slot = dta[0];
    if (slot < 1 || slot > 8 || !k->finds[slot - 1].used) return dos_fail(c, E_NOMOREFILES);
    k->finds[slot - 1].next = (unsigned)(dta[1] | dta[2] << 8);
    if (!find_emit(k, c, &k->finds[slot - 1], slot - 1)) return dos_fail(c, E_NOMOREFILES);
    return dos_ok(c);
}

static cpu_status dos_dispatch(hle_call *c) {
    kernel_t *k = c->ctx;
    cpu_t *cpu = c->cpu;
    unsigned ah = cpu->r[CPU_EAX] >> 8 & 0xFF, al = cpu->r[CPU_EAX] & 0xFF;
    uint16_t bx = (uint16_t)cpu->r[CPU_EBX], cx = (uint16_t)cpu->r[CPU_ECX], dx = (uint16_t)cpu->r[CPU_EDX];
    uint16_t ds = cpu->seg[CPU_DS];
    char path[DP_DOS_MAX * 4];
    dp_result r;
    FILE *fp;
    int h;
    int err;
    time_t now;
    struct tm tm;
    switch (ah) {
    case 0x19:
        hle_ret16(c, (uint16_t)(k->dp.cur_drive - 'A'));
        return dos_ok(c);
    case 0x0E:
        if ((dx & 0xFF) < 26 && k->dp.root[dx & 0xFF]) k->dp.cur_drive = (char)('A' + (dx & 0xFF));
        hle_ret16(c, 26);
        return dos_ok(c);
    case 0x30:
        hle_ret16(c, 0x0006);
        set_reg16(cpu, CPU_EBX, 0);
        set_reg16(cpu, CPU_ECX, 0);
        return dos_ok(c);
    case 0x1A: /* set DTA */
        k->dta_sel = ds;
        k->dta_off = dx;
        return dos_ok(c);
    case 0x2F: /* get DTA into ES:BX */
        cpu->seg[CPU_ES] = k->dta_sel ? k->dta_sel : k->task_sel;
        set_reg16(cpu, CPU_EBX, k->dta_sel ? k->dta_off : 0x180);
        return dos_ok(c);
    case 0x4E:
        return dos_findfirst(c, ds, dx, cx);
    case 0x4F:
        return dos_findnext(c);
    case 0x59: /* extended error: the last failure, class and action as DOS reports them */
        hle_ret16(c, k->dos_error);
        set_reg16(cpu, CPU_EBX, k->dos_error == E_NOFILE || k->dos_error == E_NOPATH || k->dos_error == E_NOMOREFILES
                                    ? 0x0803
                                    : k->dos_error ? 0x0D01 : 0);   /* class 8 not found / 13 unknown; action 3 user, 1 retry */
        set_reg16(cpu, CPU_ECX, k->dos_error ? 0x0001 : 0);          /* locus: unknown */
        return dos_ok(c);
    case 0x25: /* set vector */
        return dos_ok(c);
    case 0x35: /* get vector: nothing installed */
        set_reg16(cpu, CPU_EBX, 0);
        cpu->seg[CPU_ES] = 0;
        return dos_ok(c);
    case 0x2A:
        now = time(NULL);
        localtime_r(&now, &tm);
        set_reg16(cpu, CPU_ECX, (unsigned)tm.tm_year + 1900);
        set_reg16(cpu, CPU_EDX, (unsigned)(tm.tm_mon + 1) << 8 | (unsigned)tm.tm_mday);
        hle_ret16(c, (uint16_t)tm.tm_wday);
        return dos_ok(c);
    case 0x2C:
        now = time(NULL);
        localtime_r(&now, &tm);
        set_reg16(cpu, CPU_ECX, (unsigned)tm.tm_hour << 8 | (unsigned)tm.tm_min);
        set_reg16(cpu, CPU_EDX, (unsigned)tm.tm_sec << 8);
        return dos_ok(c);
    case 0x3B:
        if (!read_path(c, ds, dx, path, sizeof path) || !dp_chdir(&k->dp, path)) return dos_fail(c, E_NOPATH);
        return dos_ok(c);
    case 0x47: { /* DS:SI = 64 byte buffer for the directory without drive and leading backslash */
        char buf[DP_DOS_MAX];
        uint8_t *dst;
        if (!dp_getcwd(&k->dp, dx & 0xFF, buf, sizeof buf)) return dos_fail(c, 0x0F);
        if (!(dst = hle_ptr(c, ds, (uint16_t)cpu->r[CPU_ESI], (uint32_t)strlen(buf) + 1))) return CPU_E_LIMIT;
        memcpy(dst, buf, strlen(buf) + 1);
        hle_ret16(c, 0x0100);
        return dos_ok(c);
    }
    case 0x68: /* commit file (6Ah: the DOS 4 name): writes are already flushed, flush again to be sure */
    case 0x6A:
        if (!(fp = file_of(k, bx))) return dos_fail(c, E_BADHANDLE);
        fflush(fp);
        return dos_ok(c);
    case 0x39: /* mkdir: the parent must exist, the name must not */
        if (!read_path(c, ds, dx, path, sizeof path)) return CPU_E_SEL;
        if (!dp_resolve(&k->dp, path, 0, &r)) return dos_fail(c, E_NOPATH);
        if (r.exists) return dos_fail(c, E_ACCESS);
        return mkdir(r.host, 0777) == 0 ? dos_ok(c) : dos_fail(c, E_ACCESS);
    case 0x3A: /* rmdir: an existing, empty directory */
        if (!read_path(c, ds, dx, path, sizeof path)) return CPU_E_SEL;
        if (!dp_resolve(&k->dp, path, 0, &r) || !r.exists || !r.is_dir) return dos_fail(c, E_NOPATH);
        return rmdir(r.host) == 0 ? dos_ok(c) : dos_fail(c, E_ACCESS);
    case 0x3C:
    case 0x3D:
        if (!read_path(c, ds, dx, path, sizeof path)) return CPU_E_SEL;
        err = open_dos(k, path, ah == 0x3D ? al : 2, ah == 0x3C, &h, &r);
        if (err) return dos_fail(c, (unsigned)err);
        hle_ret16(c, (uint16_t)h);
        return dos_ok(c);
    case 0x3E:
        return file_close(k, bx) ? dos_ok(c) : dos_fail(c, E_BADHANDLE);
    case 0x3F:
    case 0x40: {
        uint8_t *p;
        size_t done;
        if (!(fp = file_of(k, bx))) return dos_fail(c, E_BADHANDLE);
        if (cx == 0) {
            hle_ret16(c, 0);
            return dos_ok(c);
        }
        if (!(p = hle_ptr(c, ds, dx, cx))) return CPU_E_LIMIT;
        done = ah == 0x3F ? fread(p, 1, cx, fp) : fwrite(p, 1, cx, fp);
        if (ah == 0x40) fflush(fp);
        if (ferror(fp)) {
            clearerr(fp);
            return dos_fail(c, E_ACCESS);
        }
        hle_ret16(c, (uint16_t)done);
        return dos_ok(c);
    }
    case 0x41:
        if (!read_path(c, ds, dx, path, sizeof path)) return CPU_E_SEL;
        if (!dp_resolve(&k->dp, path, 0, &r) || !r.exists) return dos_fail(c, E_NOFILE);
        return remove(r.host) == 0 ? dos_ok(c) : dos_fail(c, E_ACCESS);
    case 0x42: {
        long pos;
        if (!(fp = file_of(k, bx))) return dos_fail(c, E_BADHANDLE);
        pos = seek_file(fp, (int32_t)((uint32_t)cx << 16 | dx), (int)al);
        if (pos < 0) return dos_fail(c, E_BADFUNC);
        hle_ret32(c, (uint32_t)pos);
        return dos_ok(c);
    }
    case 0x43:
        if (!read_path(c, ds, dx, path, sizeof path)) return CPU_E_SEL;
        if (!dp_resolve(&k->dp, path, 0, &r) || !r.exists) return dos_fail(c, E_NOFILE);
        if (al == 0) set_reg16(cpu, CPU_ECX, r.is_dir ? 0x10 : 0x20);
        return dos_ok(c);
    case 0x57: {                                     /* get (AL=0) / set (AL=1) a file's date and time by handle */
        struct stat st;
        struct tm tm;
        if (!(fp = file_of(k, bx))) return dos_fail(c, E_BADHANDLE);
        if (al > 1 || fstat(fileno(fp), &st) != 0) return dos_fail(c, E_BADFUNC);
        if (al == 0) {
            localtime_r(&st.st_mtime, &tm);
            set_reg16(cpu, CPU_EDX, (unsigned)(tm.tm_year - 80) << 9 | (unsigned)(tm.tm_mon + 1) << 5 | (unsigned)tm.tm_mday);
            set_reg16(cpu, CPU_ECX, (unsigned)tm.tm_hour << 11 | (unsigned)tm.tm_min << 5 | (unsigned)tm.tm_sec / 2);
        } else {
            struct timespec ts[2];
            memset(&tm, 0, sizeof tm);
            tm.tm_year = (int)(dx >> 9) + 80;
            tm.tm_mon = (int)(dx >> 5 & 15) - 1;
            tm.tm_mday = (int)(dx & 31);
            tm.tm_hour = (int)(cx >> 11);
            tm.tm_min = (int)(cx >> 5 & 63);
            tm.tm_sec = (int)(cx & 31) * 2;
            tm.tm_isdst = -1;
            ts[0].tv_sec = ts[1].tv_sec = mktime(&tm);
            ts[0].tv_nsec = ts[1].tv_nsec = 0;
            fflush(fp);
            if (ts[0].tv_sec == (time_t)-1 || futimens(fileno(fp), ts) != 0) return dos_fail(c, E_ACCESS);
        }
        return dos_ok(c);
    }
    case 0x4C:
        if (k->cfg.debug) fprintf(k->cfg.debug, "DOS terminate, code %u\n", al);
        return loader_request_stop(c->proc);
    default:
        if (k->cfg.debug) fprintf(k->cfg.debug, "Dos3Call: unsupported AH=%02X from %04X:%04X\n", ah, c->caller_cs, c->caller_ip);
        return dos_fail(c, E_BADFUNC);
    }
}

/* DOS services for Dos3Call and INT 21h; with the HLE trace on each call is logged with its arguments and outcome. */
cpu_status k_dos3call(hle_call *c) {
    kernel_t *k = c->ctx;
    FILE *log = k->hle && k->hle->trace ? k->hle->log : NULL;
    cpu_t *cpu = c->cpu;
    unsigned ah = cpu->r[CPU_EAX] >> 8 & 0xFF, al = cpu->r[CPU_EAX] & 0xFF;
    unsigned bx = cpu->r[CPU_EBX] & 0xFFFF, cx = cpu->r[CPU_ECX] & 0xFFFF, dx = cpu->r[CPU_EDX] & 0xFFFF;
    char path[DP_DOS_MAX * 4] = "";
    bool named = ah == 0x3B || ah == 0x39 || ah == 0x3A || ah == 0x3C || ah == 0x3D || ah == 0x41 || ah == 0x43 ||
                 ah == 0x4E || ah == 0x56;
    cpu_status st;
    if (!log) return dos_dispatch(c);
    if (named && !read_path(c, cpu->seg[CPU_DS], (uint16_t)dx, path, sizeof path)) path[0] = 0;
    st = dos_dispatch(c);
    fprintf(log, "dos: AH=%02X AL=%02X BX=%04X CX=%04X DX=%04X", ah, al, bx, cx, dx);
    if (named) fprintf(log, " \"%s\"", path);
    if (cpu->eflags & CPU_CF) fprintf(log, " -> error %u\n", (unsigned)(cpu->r[CPU_EAX] & 0xFFFF));
    else fprintf(log, " -> ok AX=%04X\n", (unsigned)(cpu->r[CPU_EAX] & 0xFFFF));
    return st;
}
