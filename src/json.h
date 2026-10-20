#ifndef CALLASSIST_JSON_H
#define CALLASSIST_JSON_H

#include <stddef.h>

typedef enum {
    JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT
} json_type_t;

typedef struct json_value json_value_t;

struct json_value {
    json_type_t   type;
    /* JSON_STRING: decoded UTF-8. JSON_OBJECT member key lives in `key`. */
    char         *str;
    double        num;
    int           boolean;
    char         *key;          /* only set for object members */
    json_value_t *child;        /* first child (array elements / object members) */
    json_value_t *next;         /* next sibling */
};

/* Parses `text` (UTF-8, NUL-terminated). Returns NULL on syntax error. */
json_value_t *json_parse(const char *text);
void          json_free(json_value_t *v);

/* Convenience accessors; all tolerate NULL. */
const json_value_t *json_get(const json_value_t *obj, const char *key);
const json_value_t *json_at(const json_value_t *arr, size_t index);
const char         *json_str(const json_value_t *v);            /* NULL unless JSON_STRING */
/* Dotted path with [i] indexes, e.g. "choices[0].delta.content". */
const json_value_t *json_path(const json_value_t *root, const char *path);
const char         *json_path_str(const json_value_t *root, const char *path);

#endif
