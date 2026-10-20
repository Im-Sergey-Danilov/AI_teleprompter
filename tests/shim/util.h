/* Host-test stand-in for src/util.h: same API, no Win32. */
#ifndef CALLASSIST_UTIL_H
#define CALLASSIST_UTIL_H

#include <stddef.h>

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

typedef struct { char *data; size_t len; size_t cap; } sb_t;

void sb_init(sb_t *sb);
void sb_free(sb_t *sb);
int  sb_append(sb_t *sb, const void *bytes, size_t n);
int  sb_puts(sb_t *sb, const char *s);
int  sb_printf(sb_t *sb, const char *fmt, ...);
int  sb_put_json_string(sb_t *sb, const char *s);
void str_trim(char *s);

#endif
