#ifndef DOSPATH_H
#define DOSPATH_H

#include <stdbool.h>
#include <stddef.h>

/* DOS drive and path semantics over host directories: each drive letter maps
 * to a host directory, every path component is matched case-insensitively
 * against what exists on the host (the CD is upper case, host file systems
 * need not be), ".." never leaves the drive root. */

#define DP_PATH_MAX 512
#define DP_DOS_MAX 128

typedef struct {
    char *root[26];                 /* host directory behind each drive, or NULL */
    char cur_drive;                 /* 'A'..'Z' */
    char cur_dir[26][DP_DOS_MAX];   /* DOS cwd per drive: "\" or "\DIR\SUB" */
} dospath_t;

typedef struct {
    char host[DP_PATH_MAX];         /* host path of the target */
    char dos[DP_DOS_MAX];           /* canonical upper-case DOS path, "C:\DIR\FILE.EXT" */
    bool exists;
    bool is_dir;
} dp_result;

/* resolve flags */
#define DP_CREATE_DIRS 1 /* create missing intermediate directories (upper case) */

void dp_init(dospath_t *dp);
void dp_free(dospath_t *dp);
bool dp_map(dospath_t *dp, char drive, const char *host_dir);
/* False when the drive is unmapped, the path is malformed or an intermediate
 * directory is missing. A missing last component is fine (exists == false)
 * and takes the name as written, upper-cased. */
bool dp_resolve(const dospath_t *dp, const char *path, unsigned flags, dp_result *out);
bool dp_chdir(dospath_t *dp, const char *path);
/* Current directory of a drive (0 = current drive, 1 = A ...) without the drive
 * letter or leading backslash. */
bool dp_getcwd(const dospath_t *dp, unsigned drive, char *out, size_t cap);

#endif
