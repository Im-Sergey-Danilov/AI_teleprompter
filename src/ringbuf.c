#include "ringbuf.h"

#include <stdlib.h>
#include <string.h>

int fring_init(fring_t *rb, size_t capacity_samples)
{
    memset(rb, 0, sizeof(*rb));
    rb->d = (float *)malloc(capacity_samples * sizeof(float));
    if (!rb->d) return -1;
    rb->cap = capacity_samples;
    InitializeCriticalSection(&rb->cs);
    rb->ready = 1;
    return 0;
}

void fring_free(fring_t *rb)
{
    if (!rb->ready) return;
    DeleteCriticalSection(&rb->cs);
    free(rb->d);
    memset(rb, 0, sizeof(*rb));
}

size_t fring_write(fring_t *rb, const float *src, size_t n)
{
    size_t i;
    if (!rb->ready || !n) return 0;

    EnterCriticalSection(&rb->cs);
    if (n >= rb->cap) {                 /* keep only the newest cap samples */
        src += n - rb->cap;
        rb->dropped += n - rb->cap;
        n = rb->cap;
        rb->r = rb->w = rb->count = 0;
    }
    for (i = 0; i < n; ++i) {
        rb->d[rb->w] = src[i];
        rb->w = (rb->w + 1) % rb->cap;
        if (rb->count == rb->cap) {     /* full: advance read cursor */
            rb->r = (rb->r + 1) % rb->cap;
            rb->dropped++;
        } else {
            rb->count++;
        }
    }
    LeaveCriticalSection(&rb->cs);
    return n;
}

size_t fring_read(fring_t *rb, float *dst, size_t n)
{
    size_t i;
    if (!rb->ready || !n) return 0;

    EnterCriticalSection(&rb->cs);
    if (n > rb->count) n = rb->count;
    for (i = 0; i < n; ++i) {
        dst[i] = rb->d[rb->r];
        rb->r = (rb->r + 1) % rb->cap;
    }
    rb->count -= n;
    LeaveCriticalSection(&rb->cs);
    return n;
}

size_t fring_avail(fring_t *rb)
{
    size_t n;
    if (!rb->ready) return 0;
    EnterCriticalSection(&rb->cs);
    n = rb->count;
    LeaveCriticalSection(&rb->cs);
    return n;
}

void fring_clear(fring_t *rb)
{
    if (!rb->ready) return;
    EnterCriticalSection(&rb->cs);
    rb->r = rb->w = rb->count = 0;
    LeaveCriticalSection(&rb->cs);
}
