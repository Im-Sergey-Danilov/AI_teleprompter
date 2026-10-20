#ifndef CALLASSIST_STT_H
#define CALLASSIST_STT_H

#include "config.h"
#include <stddef.h>

typedef struct stt_backend stt_backend_t;

struct stt_backend {
    /* All calls happen on the STT worker thread. */
    int  (*open)(stt_backend_t *b, const config_t *cfg);
    void (*close)(stt_backend_t *b);
    /* pcm is 16 kHz mono float; *out receives malloc'd UTF-8 (may be NULL/empty). */
    int  (*transcribe)(stt_backend_t *b, const float *pcm, size_t n, char **out);
    void (*destroy)(stt_backend_t *b);
    void  *impl;
    char   error[256];
    char   name[32];
};

/* One round trip against the configured engine; writes a human-readable
 * verdict into out. Returns 0 when the round trip succeeded. */
int            stt_test(const config_t *cfg, char *out, size_t cap);

stt_backend_t *stt_local_create(void);
stt_backend_t *stt_remote_create(void);
int            stt_local_available(void);      /* compiled with whisper.cpp? */

/* ------------------------------------------------------------ worker ----- */

typedef struct stt_worker stt_worker_t;
typedef void (*stt_result_cb)(void *user, const char *text_utf8, double seconds);

stt_worker_t *stt_worker_create(void);
void          stt_worker_destroy(stt_worker_t *w);
int           stt_worker_start(stt_worker_t *w, const config_t *cfg,
                               stt_result_cb cb, void *user);
void          stt_worker_stop(stt_worker_t *w);
/* Takes ownership of `pcm`. */
void          stt_worker_submit(stt_worker_t *w, float *pcm, size_t n);
int           stt_worker_backlog(stt_worker_t *w);
int           stt_worker_busy(stt_worker_t *w);
void          stt_worker_status(stt_worker_t *w, char *out, size_t cap);

#endif
