#ifndef CALLASSIST_LLM_H
#define CALLASSIST_LLM_H

#include "config.h"

enum {
    LLM_EVT_START = 0,   /* text = NULL */
    LLM_EVT_DELTA,       /* text = UTF-8 fragment */
    LLM_EVT_DONE,        /* text = NULL */
    LLM_EVT_ERROR        /* text = message */
};

typedef void (*llm_event_cb)(void *user, int event, const char *text);

typedef struct llm_worker llm_worker_t;

/* One earlier exchange, replayed so a follow-up question makes sense. */
typedef struct {
    const char *question;
    const char *answer;
} llm_turn_t;

llm_worker_t *llm_create(void);
void          llm_destroy(llm_worker_t *w);
/* Cancels anything in flight, then runs a new completion. Returns 0 if queued. */
int           llm_request(llm_worker_t *w, const config_t *cfg,
                          const llm_turn_t *hist, int n_hist,
                          const char *transcript, const char *last,
                          const char *extra, llm_event_cb cb, void *user);
/* Same, but asks the model about a screenshot instead of the transcript.
 * png_b64 is a base64 PNG; it is sent as an OpenAI-style image_url data URI. */
int           llm_request_image(llm_worker_t *w, const config_t *cfg,
                                const llm_turn_t *hist, int n_hist,
                                const char *png_b64, const char *instruction,
                                llm_event_cb cb, void *user);
void          llm_cancel(llm_worker_t *w);
int           llm_busy(llm_worker_t *w);

/* Exposed for the settings dialog's "Test" button. */
int           llm_test(const config_t *cfg, char *out, size_t cap);

#endif
