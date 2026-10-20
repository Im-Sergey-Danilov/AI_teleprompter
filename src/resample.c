#include "resample.h"

#include <stdlib.h>
#include <string.h>

int resampler_init(resampler_t *r, unsigned in_rate, unsigned out_rate)
{
    memset(r, 0, sizeof(*r));
    if (!in_rate || !out_rate) return -1;
    r->step = (double)in_rate / (double)out_rate;
    r->box  = (size_t)(r->step);
    if (r->box < 1) r->box = 1;
    if (r->box > 64) r->box = 64;
    return 0;
}

void resampler_free(resampler_t *r)
{
    free(r->in);
    free(r->out);
    memset(r, 0, sizeof(*r));
}

void resampler_reset(resampler_t *r)
{
    r->in_len = 0;
    r->pos    = 0.0;
}

static int ensure_in(resampler_t *r, size_t need)
{
    if (need <= r->in_cap) return 1;
    {
        size_t cap = r->in_cap ? r->in_cap : 4096;
        float *p;
        while (cap < need) cap *= 2;
        p = (float *)realloc(r->in, cap * sizeof(float));
        if (!p) return 0;
        r->in = p;
        r->in_cap = cap;
    }
    return 1;
}

static int ensure_out(resampler_t *r, size_t need)
{
    if (need <= r->out_cap) return 1;
    {
        size_t cap = r->out_cap ? r->out_cap : 4096;
        float *p;
        while (cap < need) cap *= 2;
        p = (float *)realloc(r->out, cap * sizeof(float));
        if (!p) return 0;
        r->out = p;
        r->out_cap = cap;
    }
    return 1;
}

/* box-filtered input sample at integer index i */
static float filtered(const resampler_t *r, size_t i)
{
    size_t k;
    float acc = 0.0f;
    for (k = 0; k < r->box; ++k) acc += r->in[i + k];
    return acc / (float)r->box;
}

size_t resampler_push(resampler_t *r, const float *in, size_t n, const float **out)
{
    size_t produced = 0;
    size_t max_out;

    *out = NULL;
    if (!ensure_in(r, r->in_len + n)) return 0;
    memcpy(r->in + r->in_len, in, n * sizeof(float));
    r->in_len += n;

    max_out = (size_t)((double)r->in_len / r->step) + 4;
    if (!ensure_out(r, max_out)) return 0;

    /* Need samples up to index floor(pos)+1+box-1 to interpolate. */
    for (;;) {
        size_t i = (size_t)r->pos;
        double frac = r->pos - (double)i;
        float a, b;

        if (i + r->box + 1 > r->in_len) break;
        if (produced >= max_out) break;

        a = filtered(r, i);
        b = filtered(r, i + 1 <= r->in_len - r->box ? i + 1 : i);
        r->out[produced++] = a + (float)frac * (b - a);
        r->pos += r->step;
    }

    /* Drop fully consumed input, keeping the filter tail. */
    {
        size_t keep_from = (size_t)r->pos;
        if (keep_from > 0) {
            if (keep_from > r->in_len) keep_from = r->in_len;
            memmove(r->in, r->in + keep_from, (r->in_len - keep_from) * sizeof(float));
            r->in_len -= keep_from;
            r->pos -= (double)keep_from;
        }
    }

    *out = r->out;
    return produced;
}
