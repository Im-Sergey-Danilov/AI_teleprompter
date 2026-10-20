#include "wav.h"

#include <stdlib.h>
#include <string.h>

static void put_u32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v);       p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static void put_u16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v);       p[1] = (unsigned char)(v >> 8);
}

int wav_from_float(const float *pcm, size_t n, unsigned sample_rate,
                   unsigned char **out, size_t *out_len)
{
    const unsigned data_bytes = (unsigned)(n * 2);
    const size_t   total      = 44 + data_bytes;
    unsigned char *b = (unsigned char *)malloc(total);
    size_t i;

    if (!b) return -1;

    memcpy(b, "RIFF", 4);
    put_u32(b + 4, (unsigned)(total - 8));
    memcpy(b + 8, "WAVEfmt ", 8);
    put_u32(b + 16, 16);                       /* fmt chunk size */
    put_u16(b + 20, 1);                        /* PCM */
    put_u16(b + 22, 1);                        /* mono */
    put_u32(b + 24, sample_rate);
    put_u32(b + 28, sample_rate * 2);          /* byte rate */
    put_u16(b + 32, 2);                        /* block align */
    put_u16(b + 34, 16);                       /* bits */
    memcpy(b + 36, "data", 4);
    put_u32(b + 40, data_bytes);

    for (i = 0; i < n; ++i) {
        float f = pcm[i];
        int   s;
        if (f >  1.0f) f =  1.0f;
        if (f < -1.0f) f = -1.0f;
        s = (int)(f * 32767.0f);
        put_u16(b + 44 + i * 2, (unsigned)(s & 0xFFFF));
    }

    *out = b;
    *out_len = total;
    return 0;
}
