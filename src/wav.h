#ifndef CALLASSIST_WAV_H
#define CALLASSIST_WAV_H

#include <stddef.h>

/* Packs mono float PCM into a 16-bit WAV image. Caller frees *out. */
int wav_from_float(const float *pcm, size_t n, unsigned sample_rate,
                   unsigned char **out, size_t *out_len);

#endif
