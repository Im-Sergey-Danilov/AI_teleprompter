#ifndef CALLASSIST_RESAMPLE_H
#define CALLASSIST_RESAMPLE_H

#include <stddef.h>

/*
 * Streaming mono resampler: box-filter decimation (crude anti-aliasing) followed
 * by linear interpolation. Adequate for 8 kHz-band speech; not hi-fi.
 */
typedef struct {
    float  *in;        /* pending input samples */
    size_t  in_len, in_cap;
    float  *out;       /* scratch for the last push */
    size_t  out_cap;
    double  pos;       /* fractional read position within `in` */
    double  step;      /* in_rate / out_rate */
    size_t  box;       /* averaging window length in input samples */
} resampler_t;

int  resampler_init(resampler_t *r, unsigned in_rate, unsigned out_rate);
void resampler_free(resampler_t *r);
void resampler_reset(resampler_t *r);
/* Consumes n input samples; sets *out to an internal buffer, returns sample count. */
size_t resampler_push(resampler_t *r, const float *in, size_t n, const float **out);

#endif
