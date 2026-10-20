#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

void *xmalloc(size_t n) { return malloc(n ? n : 1); }
void *xrealloc(void *p, size_t n) { return realloc(p, n ? n : 1); }
char *xstrdup(const char *s)
{
    size_t n; char *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
void sb_init(sb_t *sb) { sb->data = NULL; sb->len = 0; sb->cap = 0; }
void sb_free(sb_t *sb) { free(sb->data); sb->data = NULL; sb->len = sb->cap = 0; }
static int reserve(sb_t *sb, size_t extra)
{
    size_t need = sb->len + extra + 1, cap;
    char *p;
    if (need <= sb->cap) return 1;
    cap = sb->cap ? sb->cap : 256;
    while (cap < need) cap *= 2;
    p = (char *)realloc(sb->data, cap);
    if (!p) return 0;
    sb->data = p; sb->cap = cap;
    return 1;
}
int sb_append(sb_t *sb, const void *b, size_t n)
{
    if (!n) return 1;
    if (!reserve(sb, n)) return 0;
    memcpy(sb->data + sb->len, b, n);
    sb->len += n;
    sb->data[sb->len] = 0;
    return 1;
}
int sb_puts(sb_t *sb, const char *s) { return s ? sb_append(sb, s, strlen(s)) : 1; }
int sb_printf(sb_t *sb, const char *fmt, ...)
{
    char buf[2048]; int n; va_list ap;
    va_start(ap, fmt); n = vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    return n > 0 ? sb_append(sb, buf, (size_t)n) : 0;
}
int sb_put_json_string(sb_t *sb, const char *s) { return sb_puts(sb, s); }
void str_trim(char *s)
{
    char *p = s; size_t n;
    if (!s) return;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}
