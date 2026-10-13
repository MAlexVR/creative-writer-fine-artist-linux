#ifndef INI_H
#define INI_H

#include <stdbool.h>
#include <stddef.h>

/* Windows .INI files on the host: [section] / key=value, case-insensitive
 * names, ';' comments. Reads parse the file each time, writes rewrite it and
 * keep every line they do not touch. */

/* GetPrivateProfileString semantics: copies the value (or def when the key is
 * missing) truncated to size-1 bytes and returns its length. A NULL key lists
 * the key names of the section, a NULL section the section names, each
 * NUL-terminated and the list ended by an extra NUL. */
size_t ini_get_string(const char *file, const char *section, const char *key, const char *def,
                      char *out, size_t size);
/* Key present? Fills *value (NUL-terminated, truncated to cap). */
bool ini_lookup(const char *file, const char *section, const char *key, char *value, size_t cap);
/* Writes section/key=value. A NULL value deletes the key, a NULL key the whole
 * section. The file is created when missing. False on I/O failure. */
bool ini_write(const char *file, const char *section, const char *key, const char *value);

#endif
