#ifndef CALLASSIST_DOTENV_H
#define CALLASSIST_DOTENV_H

#include <stddef.h>

typedef struct dotenv dotenv_t;

/* Parses .env text already held in memory. Returns NULL only on allocation
 * failure - a malformed line is skipped, never fatal. Free with dotenv_free(). */
dotenv_t   *dotenv_parse(const char *text);
void        dotenv_free(dotenv_t *e);

/* Value for key, or NULL when the key is absent. Case-sensitive. A key present
 * with an empty value returns "", which is distinct from NULL. */
const char *dotenv_get(const dotenv_t *e, const char *key);

size_t      dotenv_count(const dotenv_t *e);

#endif
