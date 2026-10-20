#ifndef CALLASSIST_UTIL_H
#define CALLASSIST_UTIL_H

#include <windows.h>
#include <stddef.h>

/* ---- allocation helpers (abort-free; callers must check) ---- */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* ---- UTF-8 <-> UTF-16. Returned buffers must be free()d. ---- */
wchar_t *utf8_to_wide(const char *s);
char    *wide_to_utf8(const wchar_t *s);

/* ---- growable byte/string builder ---- */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} sb_t;

void  sb_init(sb_t *sb);
void  sb_free(sb_t *sb);
int   sb_append(sb_t *sb, const void *bytes, size_t n);
int   sb_puts(sb_t *sb, const char *s);
int   sb_printf(sb_t *sb, const char *fmt, ...);
/* appends s as a JSON string literal, quotes included */
int   sb_put_json_string(sb_t *sb, const char *s);

/* ---- misc ---- */
void  str_trim(char *s);
/* Copies s into out, stopping at max_bytes without splitting a UTF-8
 * sequence. Returns out, so it can be used inline in a log call. */
const char *utf8_clip(char *out, size_t cap, const char *s, size_t max_bytes);
int   str_ieq(const char *a, const char *b);
/* %APPDATA%\CallAssist\<name>; returns 0 on success */
int   app_data_path(const char *name, char *out, size_t out_sz);
/* <dir of the running .exe>\<name>; returns 0 on success. Pass NULL or "" for
 * the directory itself. Creates nothing. */
int   exe_dir_path(const char *name, char *out, size_t out_sz);
unsigned long long now_ms(void);

/* ---- DPAPI-backed secret storage (per-user). Returns malloc'd hex. ---- */
char *secret_encrypt_hex(const char *plain);
char *secret_decrypt_hex(const char *hex);

#endif
