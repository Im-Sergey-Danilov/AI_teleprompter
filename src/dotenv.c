/*
 * Minimal .env reader.
 *
 * Deliberately free of Windows headers so tests/run.sh can build and exercise
 * it on any host. Reading the file is the caller's job; this parses text that
 * is already in memory.
 *
 * Supported:
 *   KEY=value          bare - taken literally to the end of the line
 *   KEY="value"        escapes processed, may span lines
 *   KEY='value'        literal, may span lines
 *   export KEY=value   the export prefix is ignored
 *   # comment          whole-line comments, and blank lines
 *
 * Escapes inside double quotes: \n \r \t \\ \" \'. An unknown escape keeps the
 * character and drops the backslash. CRLF becomes LF in every form, because the
 * rest of the program works in bare LF.
 *
 * A bare value keeps '#' and everything after it. Prompts are prose, and
 * silently truncating one at a hash is worse than asking for quotes around a
 * value that needs a trailing comment. A later duplicate of a key wins.
 */
#include "dotenv.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char  *key;
    char  *val;
} pair_t;

struct dotenv {
    pair_t *v;
    size_t  n, cap;
};

/* ------------------------------------------------------------ small buffer */

typedef struct {
    char  *p;
    size_t n, cap;
} buf_t;

static int bput(buf_t *b, char c)
{
    if (b->n + 1 >= b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        char  *np  = (char *)realloc(b->p, cap);
        if (!np) return -1;
        b->p   = np;
        b->cap = cap;
    }
    b->p[b->n++] = c;
    return 0;
}

/* NUL-terminates and hands the buffer over; the caller frees it. */
static char *btake(buf_t *b)
{
    if (bput(b, '\0') != 0) { free(b->p); b->p = NULL; return NULL; }
    return b->p;
}

/* ------------------------------------------------------------------ parsing */

static const char *next_line(const char *p)
{
    while (*p && *p != '\n') p++;
    return *p ? p + 1 : p;
}

static int push(struct dotenv *e, char *key, char *val)
{
    size_t i;

    for (i = 0; i < e->n; ++i) {               /* last duplicate wins */
        if (strcmp(e->v[i].key, key) == 0) {
            free(e->v[i].val);
            free(key);
            e->v[i].val = val;
            return 0;
        }
    }
    if (e->n == e->cap) {
        size_t cap  = e->cap ? e->cap * 2 : 16;
        pair_t *nv  = (pair_t *)realloc(e->v, cap * sizeof(*nv));
        if (!nv) return -1;
        e->v   = nv;
        e->cap = cap;
    }
    e->v[e->n].key = key;
    e->v[e->n].val = val;
    e->n++;
    return 0;
}

/* Reads a double- or single-quoted value. *pp points just past the opening
 * quote; on return it points just past the closing one (or at the terminator
 * for an unterminated value, which is accepted as-is). */
static char *read_quoted(const char **pp, char quote)
{
    const char *p = *pp;
    buf_t b = { NULL, 0, 0 };

    while (*p && *p != quote) {
        if (quote == '"' && *p == '\\' && p[1]) {
            char c = p[1];
            p += 2;
            switch (c) {
            case 'n':  if (bput(&b, '\n') != 0) return NULL; break;
            case 'r':  if (bput(&b, '\r') != 0) return NULL; break;
            case 't':  if (bput(&b, '\t') != 0) return NULL; break;
            case '\\': if (bput(&b, '\\') != 0) return NULL; break;
            case '"':  if (bput(&b, '"')  != 0) return NULL; break;
            case '\'': if (bput(&b, '\'') != 0) return NULL; break;
            default:   if (bput(&b, c)    != 0) return NULL; break;
            }
            continue;
        }
        if (*p == '\r') {                      /* CRLF -> LF */
            p++;
            if (*p == '\n') { if (bput(&b, '\n') != 0) return NULL; p++; }
            else            { if (bput(&b, '\n') != 0) return NULL; }
            continue;
        }
        if (bput(&b, *p) != 0) return NULL;
        p++;
    }
    if (*p == quote) p++;
    *pp = p;
    return btake(&b);
}

static char *read_bare(const char **pp)
{
    const char *p   = *pp;
    const char *s   = p;
    const char *end;

    while (*p && *p != '\n') p++;
    end = p;
    if (end > s && end[-1] == '\r') end--;
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;

    *pp = p;
    {
        size_t n   = (size_t)(end - s);
        char  *out = (char *)malloc(n + 1);
        if (!out) return NULL;
        memcpy(out, s, n);
        out[n] = '\0';
        return out;
    }
}

dotenv_t *dotenv_parse(const char *text)
{
    struct dotenv *e;
    const char    *p;

    e = (struct dotenv *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    if (!text) return e;

    if ((unsigned char)text[0] == 0xEF &&       /* skip a UTF-8 BOM */
        (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text += 3;

    p = text;
    while (*p) {
        const char *ks, *ke;
        char *key, *val;

        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        if (*p == '\r' || *p == '\n' || *p == '#') { p = next_line(p); continue; }

        if (strncmp(p, "export", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
            p += 6;
            while (*p == ' ' || *p == '\t') p++;
        }

        ks = p;
        while (*p && *p != '=' && *p != '\n' && *p != '\r') p++;
        if (*p != '=') { p = next_line(p); continue; }   /* no '=' on the line */
        ke = p;
        p++;                                             /* past '=' */
        while (ke > ks && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        if (ke == ks) { p = next_line(p); continue; }     /* empty key */

        key = (char *)malloc((size_t)(ke - ks) + 1);
        if (!key) { dotenv_free(e); return NULL; }
        memcpy(key, ks, (size_t)(ke - ks));
        key[ke - ks] = '\0';

        while (*p == ' ' || *p == '\t') p++;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            val = read_quoted(&p, q);
            p = next_line(p);                  /* ignore any trailing junk */
        } else {
            val = read_bare(&p);
        }
        if (!val) { free(key); dotenv_free(e); return NULL; }

        if (push(e, key, val) != 0) {
            free(key); free(val);
            dotenv_free(e);
            return NULL;
        }
    }
    return e;
}

void dotenv_free(dotenv_t *e)
{
    size_t i;
    if (!e) return;
    for (i = 0; i < e->n; ++i) { free(e->v[i].key); free(e->v[i].val); }
    free(e->v);
    free(e);
}

const char *dotenv_get(const dotenv_t *e, const char *key)
{
    size_t i;
    if (!e || !key) return NULL;
    for (i = 0; i < e->n; ++i)
        if (strcmp(e->v[i].key, key) == 0) return e->v[i].val;
    return NULL;
}

size_t dotenv_count(const dotenv_t *e)
{
    return e ? e->n : 0;
}
