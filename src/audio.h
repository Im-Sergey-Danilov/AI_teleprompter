#ifndef CALLASSIST_AUDIO_H
#define CALLASSIST_AUDIO_H

#include "config.h"
#include <stddef.h>

typedef struct audio_engine audio_engine_t;

/* Called from the mixer thread; takes ownership of `pcm` (16 kHz mono float). */
typedef void (*audio_segment_cb)(void *user, float *pcm, size_t n);

typedef struct {
    char id[512];
    char name[256];
} audio_device_info_t;

/* render != 0 -> playback endpoints (for loopback); else capture endpoints. */
int  audio_enum_devices(int render, audio_device_info_t **out, int *count);

audio_engine_t *audio_create(void);
void  audio_destroy(audio_engine_t *e);
int   audio_start(audio_engine_t *e, const config_t *cfg, audio_segment_cb cb, void *user);
void  audio_stop(audio_engine_t *e);
/* Live-tunable knobs (gains + VAD); source/device changes need a restart. */
void  audio_update(audio_engine_t *e, const config_t *cfg);
float audio_level_db(audio_engine_t *e);
/* Level of one source on its own, before gain. Mic and system are summed into
 * a single stream, so a quiet mic is simply masked by loud system audio - this
 * is what makes that visible instead of guessable. */
float audio_source_level_db(audio_engine_t *e, int loopback);

/*
 * Push-to-talk capture. Between start and stop the mixed 16 kHz mono stream is
 * accumulated verbatim, with no VAD segmentation, so the whole utterance can be
 * transcribed in one request. audio_record_stop hands over the buffer and the
 * caller owns it; it returns NULL when nothing was captured.
 */
void   audio_record_start(audio_engine_t *e);
float *audio_record_stop(audio_engine_t *e, size_t *n_out);
int    audio_recording(audio_engine_t *e);
double audio_record_seconds(audio_engine_t *e);
int   audio_source_alive(audio_engine_t *e, int loopback);
void  audio_last_error(audio_engine_t *e, char *out, size_t cap);

#endif
