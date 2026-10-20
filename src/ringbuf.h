#ifndef CALLASSIST_RINGBUF_H
#define CALLASSIST_RINGBUF_H

#include <windows.h>
#include <stddef.h>

/* Thread-safe float ring buffer. Writes overwrite the oldest data on overflow. */
typedef struct {
    float            *d;
    size_t            cap;
    size_t            r, w, count;
    CRITICAL_SECTION  cs;
    int               ready;
    unsigned long long dropped;
} fring_t;

int    fring_init(fring_t *rb, size_t capacity_samples);
void   fring_free(fring_t *rb);
size_t fring_write(fring_t *rb, const float *src, size_t n);
size_t fring_read(fring_t *rb, float *dst, size_t n);
size_t fring_avail(fring_t *rb);
void   fring_clear(fring_t *rb);

#endif
