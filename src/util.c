#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <shlobj.h>
#include <wincrypt.h>

void *xmalloc(size_t n)            { return malloc(n ? n : 1); }
void *xrealloc(void *p, size_t n)  { return realloc(p, n ? n : 1); }

char *xstrdup(const char *s)
{
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

wchar_t *utf8_to_wide(const char *s)
{
    int n;
    wchar_t *w;
    if (!s) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

char *wide_to_utf8(const wchar_t *s)
{
    int n;
    char *c;
    if (!s) return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    c = (char *)malloc((size_t)n);
    if (!c) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, s, -1, c, n, NULL, NULL);
    return c;
}

void sb_init(sb_t *sb) { sb->data = NULL; sb->len = 0; sb->cap = 0; }

void sb_free(sb_t *sb)
{
    free(sb->data);
    sb->data = NULL; sb->len = 0; sb->cap = 0;
}

static int sb_reserve(sb_t *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    size_t cap;
    char *p;
    if (need <= sb->cap) return 1;
    cap = sb->cap ? sb->cap : 256;
    while (cap < need) cap *= 2;
    p = (char *)realloc(sb->data, cap);
    if (!p) return 0;
    sb->data = p;
    sb->cap  = cap;
    return 1;
}

int sb_append(sb_t *sb, const void *bytes, size_t n)
{
    if (!n) return 1;
    if (!sb_reserve(sb, n)) return 0;
    memcpy(sb->data + sb->len, bytes, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 1;
}

int sb_puts(sb_t *sb, const char *s)
{
    return s ? sb_append(sb, s, strlen(s)) : 1;
}

int sb_printf(sb_t *sb, const char *fmt, ...)
{
    char stack[1024];
    char *buf = stack;
    int n;
    va_list ap;

    va_start(ap, fmt);
    n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) return 0;

    if ((size_t)n >= sizeof(stack)) {
        buf = (char *)malloc((size_t)n + 1);
        if (!buf) return 0;
        va_start(ap, fmt);
        vsnprintf(buf, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    {
        int ok = sb_append(sb, buf, (size_t)n);
        if (buf != stack) free(buf);
        return ok;
    }
}

int sb_put_json_string(sb_t *sb, const char *s)
{
    if (!sb_append(sb, "\"", 1)) return 0;
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  if (!sb_append(sb, "\\\"", 2)) return 0; break;
        case '\\': if (!sb_append(sb, "\\\\", 2)) return 0; break;
        case '\n': if (!sb_append(sb, "\\n", 2))  return 0; break;
        case '\r': if (!sb_append(sb, "\\r", 2))  return 0; break;
        case '\t': if (!sb_append(sb, "\\t", 2))  return 0; break;
        case '\b': if (!sb_append(sb, "\\b", 2))  return 0; break;
        case '\f': if (!sb_append(sb, "\\f", 2))  return 0; break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                if (!sb_append(sb, esc, 6)) return 0;
            } else {
                /* pass UTF-8 bytes through verbatim */
                if (!sb_append(sb, &c, 1)) return 0;
            }
        }
    }
    return sb_append(sb, "\"", 1);
}

void str_trim(char *s)
{
    char *p = s;
    size_t n;
    if (!s) return;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

const char *utf8_clip(char *out, size_t cap, const char *s, size_t max_bytes)
{
    size_t n;
    if (!cap) return out;
    if (!s) { out[0] = 0; return out; }
    if (max_bytes > cap - 1) max_bytes = cap - 1;

    n = strlen(s);
    if (n > max_bytes) {
        n = max_bytes;
        /* back off over any continuation bytes, then over the lead byte */
        while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

int str_ieq(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

int app_data_path(const char *name, char *out, size_t out_sz)
{
    wchar_t wdir[MAX_PATH];
    char *dir;
    int rc = 0;

    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, wdir)))
        return -1;

    dir = wide_to_utf8(wdir);
    if (!dir) return -1;

    if (snprintf(out, out_sz, "%s\\CallAssist", dir) >= (int)out_sz) rc = -1;
    if (rc == 0) {
        wchar_t *wsub = utf8_to_wide(out);
        if (wsub) { CreateDirectoryW(wsub, NULL); free(wsub); }
        if (name && *name) {
            char tmp[1024];
            snprintf(tmp, sizeof(tmp), "%s\\%s", out, name);
            if (snprintf(out, out_sz, "%s", tmp) >= (int)out_sz) rc = -1;
        }
    }
    free(dir);
    return rc;
}

int exe_dir_path(const char *name, char *out, size_t out_sz)
{
    wchar_t wpath[MAX_PATH];
    char   *dir;
    int     rc = 0;
    DWORD   n;
    wchar_t *slash;

    n = GetModuleFileNameW(NULL, wpath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return -1;

    slash = wcsrchr(wpath, L'\\');
    if (!slash) return -1;
    *slash = L'\0';

    dir = wide_to_utf8(wpath);
    if (!dir) return -1;

    if (name && *name) {
        if (snprintf(out, out_sz, "%s\\%s", dir, name) >= (int)out_sz) rc = -1;
    } else {
        if (snprintf(out, out_sz, "%s", dir) >= (int)out_sz) rc = -1;
    }
    free(dir);
    return rc;
}

unsigned long long now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (unsigned long long)((c.QuadPart * 1000ULL) / (unsigned long long)freq.QuadPart);
}

static const char kHex[] = "0123456789abcdef";

char *secret_encrypt_hex(const char *plain)
{
    DATA_BLOB in, out;
    char *hex;
    size_t i;

    if (!plain || !*plain) return xstrdup("");

    in.pbData = (BYTE *)plain;
    in.cbData = (DWORD)strlen(plain);
    out.pbData = NULL; out.cbData = 0;

    if (!CryptProtectData(&in, L"CallAssist", NULL, NULL, NULL, 0, &out))
        return NULL;

    hex = (char *)malloc((size_t)out.cbData * 2 + 1);
    if (hex) {
        for (i = 0; i < out.cbData; ++i) {
            hex[i * 2]     = kHex[out.pbData[i] >> 4];
            hex[i * 2 + 1] = kHex[out.pbData[i] & 0xF];
        }
        hex[(size_t)out.cbData * 2] = '\0';
    }
    LocalFree(out.pbData);
    return hex;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *secret_decrypt_hex(const char *hex)
{
    DATA_BLOB in, out;
    size_t n, i;
    BYTE *raw;
    char *plain = NULL;

    if (!hex || !*hex) return xstrdup("");
    n = strlen(hex);
    if (n % 2) return NULL;

    raw = (BYTE *)malloc(n / 2);
    if (!raw) return NULL;
    for (i = 0; i < n / 2; ++i) {
        int hi = hexval(hex[i * 2]), lo = hexval(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) { free(raw); return NULL; }
        raw[i] = (BYTE)((hi << 4) | lo);
    }

    in.pbData = raw;
    in.cbData = (DWORD)(n / 2);
    out.pbData = NULL; out.cbData = 0;

    if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &out)) {
        plain = (char *)malloc((size_t)out.cbData + 1);
        if (plain) {
            memcpy(plain, out.pbData, out.cbData);
            plain[out.cbData] = '\0';
        }
        LocalFree(out.pbData);
    }
    free(raw);
    return plain;
}
