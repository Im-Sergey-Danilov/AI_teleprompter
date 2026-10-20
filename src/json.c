#include "json.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

typedef struct {
    const char *p;
    int         depth;
} jctx;

#define JSON_MAX_DEPTH 64

static json_value_t *parse_value(jctx *c);

static void skip_ws(jctx *c)
{
    while (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r') c->p++;
}

static json_value_t *node_new(json_type_t t)
{
    json_value_t *v = (json_value_t *)calloc(1, sizeof(*v));
    if (v) v->type = t;
    return v;
}

/* Encodes one code point as UTF-8 into sb. */
static int put_utf8(sb_t *sb, unsigned cp)
{
    char buf[4];
    int n = 0;
    if (cp < 0x80) {
        buf[n++] = (char)cp;
    } else if (cp < 0x800) {
        buf[n++] = (char)(0xC0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        buf[n++] = (char)(0xE0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = (char)(0xF0 | (cp >> 18));
        buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    }
    return sb_append(sb, buf, (size_t)n);
}

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; ++i) {
        char ch = s[i];
        v <<= 4;
        if      (ch >= '0' && ch <= '9') v |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= (unsigned)(ch - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* Parses a string literal starting at the opening quote. Returns malloc'd UTF-8. */
static char *parse_string(jctx *c)
{
    sb_t sb;
    if (*c->p != '"') return NULL;
    c->p++;
    sb_init(&sb);

    for (;;) {
        unsigned char ch = (unsigned char)*c->p;
        if (ch == '\0') { sb_free(&sb); return NULL; }
        if (ch == '"')  { c->p++; break; }

        if (ch == '\\') {
            c->p++;
            switch (*c->p) {
            case '"':  sb_append(&sb, "\"", 1); c->p++; break;
            case '\\': sb_append(&sb, "\\", 1); c->p++; break;
            case '/':  sb_append(&sb, "/",  1); c->p++; break;
            case 'b':  sb_append(&sb, "\b", 1); c->p++; break;
            case 'f':  sb_append(&sb, "\f", 1); c->p++; break;
            case 'n':  sb_append(&sb, "\n", 1); c->p++; break;
            case 'r':  sb_append(&sb, "\r", 1); c->p++; break;
            case 't':  sb_append(&sb, "\t", 1); c->p++; break;
            case 'u': {
                unsigned cp;
                c->p++;
                if (!hex4(c->p, &cp)) { sb_free(&sb); return NULL; }
                c->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && c->p[0] == '\\' && c->p[1] == 'u') {
                    unsigned lo;
                    if (hex4(c->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        c->p += 6;
                    }
                }
                put_utf8(&sb, cp);
                break;
            }
            default: sb_free(&sb); return NULL;
            }
        } else {
            sb_append(&sb, &ch, 1);
            c->p++;
        }
    }

    if (!sb.data) sb_append(&sb, "", 0);
    return sb.data ? sb.data : xstrdup("");
}

static json_value_t *parse_object(jctx *c)
{
    json_value_t *obj = node_new(JSON_OBJECT), *tail = NULL;
    if (!obj) return NULL;
    c->p++;                                     /* '{' */
    skip_ws(c);
    if (*c->p == '}') { c->p++; return obj; }

    for (;;) {
        json_value_t *m;
        char *key;
        skip_ws(c);
        key = parse_string(c);
        if (!key) goto fail;
        skip_ws(c);
        if (*c->p != ':') { free(key); goto fail; }
        c->p++;
        m = parse_value(c);
        if (!m) { free(key); goto fail; }
        m->key = key;

        if (tail) tail->next = m; else obj->child = m;
        tail = m;

        skip_ws(c);
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == '}') { c->p++; return obj; }
        goto fail;
    }
fail:
    json_free(obj);
    return NULL;
}

static json_value_t *parse_array(jctx *c)
{
    json_value_t *arr = node_new(JSON_ARRAY), *tail = NULL;
    if (!arr) return NULL;
    c->p++;                                     /* '[' */
    skip_ws(c);
    if (*c->p == ']') { c->p++; return arr; }

    for (;;) {
        json_value_t *e = parse_value(c);
        if (!e) goto fail;
        if (tail) tail->next = e; else arr->child = e;
        tail = e;
        skip_ws(c);
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == ']') { c->p++; return arr; }
        goto fail;
    }
fail:
    json_free(arr);
    return NULL;
}

static json_value_t *parse_value(jctx *c)
{
    json_value_t *v;

    if (++c->depth > JSON_MAX_DEPTH) { c->depth--; return NULL; }
    skip_ws(c);

    switch (*c->p) {
    case '{': v = parse_object(c); c->depth--; return v;
    case '[': v = parse_array(c);  c->depth--; return v;
    case '"': {
        char *s = parse_string(c);
        c->depth--;
        if (!s) return NULL;
        v = node_new(JSON_STRING);
        if (!v) { free(s); return NULL; }
        v->str = s;
        return v;
    }
    case 't':
        c->depth--;
        if (strncmp(c->p, "true", 4)) return NULL;
        c->p += 4;
        v = node_new(JSON_BOOL); if (v) v->boolean = 1;
        return v;
    case 'f':
        c->depth--;
        if (strncmp(c->p, "false", 5)) return NULL;
        c->p += 5;
        return node_new(JSON_BOOL);
    case 'n':
        c->depth--;
        if (strncmp(c->p, "null", 4)) return NULL;
        c->p += 4;
        return node_new(JSON_NULL);
    default: {
        char *end = NULL;
        double d = strtod(c->p, &end);
        c->depth--;
        if (!end || end == c->p) return NULL;
        c->p = end;
        v = node_new(JSON_NUMBER);
        if (v) v->num = d;
        return v;
    }
    }
}

json_value_t *json_parse(const char *text)
{
    jctx c;
    json_value_t *v;
    if (!text) return NULL;
    c.p = text;
    c.depth = 0;
    v = parse_value(&c);
    if (!v) return NULL;
    skip_ws(&c);
    return v;
}

void json_free(json_value_t *v)
{
    while (v) {
        json_value_t *next = v->next;
        json_free(v->child);
        free(v->str);
        free(v->key);
        free(v);
        v = next;
    }
}

const json_value_t *json_get(const json_value_t *obj, const char *key)
{
    const json_value_t *m;
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    for (m = obj->child; m; m = m->next)
        if (m->key && strcmp(m->key, key) == 0) return m;
    return NULL;
}

const json_value_t *json_at(const json_value_t *arr, size_t index)
{
    const json_value_t *e;
    if (!arr || arr->type != JSON_ARRAY) return NULL;
    for (e = arr->child; e; e = e->next, --index)
        if (index == 0) return e;
    return NULL;
}

const char *json_str(const json_value_t *v)
{
    return (v && v->type == JSON_STRING) ? v->str : NULL;
}

const json_value_t *json_path(const json_value_t *root, const char *path)
{
    char seg[128];
    const json_value_t *cur = root;

    while (path && *path && cur) {
        size_t n = 0;
        while (*path == '.') path++;
        while (*path && *path != '.' && *path != '[' && n + 1 < sizeof(seg))
            seg[n++] = *path++;
        seg[n] = '\0';
        if (n) cur = json_get(cur, seg);

        while (*path == '[') {
            size_t idx = 0;
            path++;
            while (*path >= '0' && *path <= '9') idx = idx * 10 + (size_t)(*path++ - '0');
            if (*path == ']') path++;
            cur = json_at(cur, idx);
            if (!cur) return NULL;
        }
    }
    return cur;
}

const char *json_path_str(const json_value_t *root, const char *path)
{
    return json_str(json_path(root, path));
}
