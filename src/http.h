#ifndef CALLASSIST_HTTP_H
#define CALLASSIST_HTTP_H

#include <windows.h>
#include <stddef.h>

typedef struct {
    int    status;          /* HTTP status, 0 if the request never completed */
    char  *body;            /* NUL-terminated; caller frees */
    size_t body_len;
    char   error[256];      /* human-readable failure reason */
} http_response_t;

void http_response_free(http_response_t *r);

/* Blocking POST. `api_key` may be NULL/empty. Returns 0 if a response arrived. */
int http_post(const char *url,
              const char *content_type,
              const void *body, size_t body_len,
              const char *api_key,
              const char *extra_headers_utf8,     /* may be NULL */
              int timeout_ms,
              http_response_t *out);

/*
 * Streaming POST for SSE. `on_event` receives each `data:` payload, NUL-terminated,
 * without the prefix. Return non-zero from the callback to abort the stream.
 * `*cancel` (may be NULL) is polled between reads.
 */
typedef int (*http_sse_cb)(void *user, const char *data);

int http_post_sse(const char *url,
                  const char *content_type,
                  const void *body, size_t body_len,
                  const char *api_key,
                  int timeout_ms,
                  http_sse_cb on_event, void *user,
                  volatile LONG *cancel,
                  http_response_t *out);

#endif
