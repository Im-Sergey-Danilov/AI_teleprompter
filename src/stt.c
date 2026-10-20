#include "stt.h"
#include "util.h"
#include "log.h"

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define MAX_QUEUE 8

typedef struct seg_node {
    float           *pcm;
    size_t           n;
    struct seg_node *next;
} seg_node_t;

struct stt_worker {
    HANDLE           thread;
    HANDLE           wake;
    volatile LONG    run;
    volatile LONG    busy;

    CRITICAL_SECTION cs;
    seg_node_t      *head, *tail;
    int              count;

    config_t         cfg;
    stt_backend_t   *backend;
    stt_result_cb    cb;
    void            *user;
    char             status[256];
};

static void set_status(stt_worker_t *w, const char *fmt, ...)
{
    va_list ap;
    EnterCriticalSection(&w->cs);
    va_start(ap, fmt);
    vsnprintf(w->status, sizeof(w->status), fmt, ap);
    va_end(ap);
    LeaveCriticalSection(&w->cs);
}

/* Drops whisper's classic silence artifacts: "[BLANK_AUDIO]", "(music)", "***". */
static int is_noise_text(const char *s)
{
    int alnum = 0;
    const char *p;
    if (!s) return 1;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return 1;
    if ((s[0] == '[' || s[0] == '(') ) {
        size_t len = strlen(s);
        char last = len ? s[len - 1] : 0;
        if (last == ']' || last == ')') return 1;
    }
    for (p = s; *p; ++p)
        if (isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80) alnum++;
    return alnum == 0;
}


/* ------------------------------------------------------------------- test -- */

/*
 * Sends one second of a quiet tone through the configured engine. The point is
 * to prove the transport - endpoint, auth, response shape - before a call, not
 * to check what Whisper makes of it; an empty transcript still counts as a
 * working round trip.
 */
int stt_test(const config_t *cfg, char *out, size_t cap)
{
    stt_backend_t *b;
    float *pcm;
    char  *text = NULL;
    size_t i, n = 16000;
    unsigned long long t0;
    int rc = -1;

    if (!cfg || !out || !cap) return -1;

    b = (cfg->stt_backend == STT_BACKEND_LOCAL) ? stt_local_create() : stt_remote_create();
    if (!b) { snprintf(out, cap, "Could not create the speech-to-text backend."); return -1; }

    if (b->open(b, cfg) != 0) {
        snprintf(out, cap, "Failed to open %s: %s", b->name, b->error);
        b->destroy(b);
        return -1;
    }

    pcm = (float *)malloc(n * sizeof(float));
    if (!pcm) {
        snprintf(out, cap, "Out of memory.");
        b->close(b);
        b->destroy(b);
        return -1;
    }
    for (i = 0; i < n; ++i)
        pcm[i] = (float)(0.05 * sin(2.0 * 3.14159265358979 * 440.0 * (double)i / 16000.0));

    t0 = now_ms();
    if (b->transcribe(b, pcm, n, &text) != 0) {
        snprintf(out, cap, "Failed: %s", b->error);
        LOGW("stt test: %s", b->error);
    } else {
        snprintf(out, cap, "OK - %s answered in %llu ms. Transcript of the test tone: '%.60s'",
                 b->name, (unsigned long long)(now_ms() - t0), text ? text : "");
        LOGI("stt test: ok via %s in %llu ms", b->name,
             (unsigned long long)(now_ms() - t0));
        rc = 0;
    }

    free(pcm);
    free(text);
    b->close(b);
    b->destroy(b);
    return rc;
}

static DWORD WINAPI worker_thread(LPVOID arg)
{
    stt_worker_t *w = (stt_worker_t *)arg;

    if (w->backend->open(w->backend, &w->cfg) != 0) {
        set_status(w, "STT unavailable: %s", w->backend->error);
        LOGE("stt: backend '%s' failed to open: %s", w->backend->name, w->backend->error);
    } else {
        set_status(w, "STT ready (%s)", w->backend->name);
        LOGI("stt: backend '%s' ready", w->backend->name);
    }

    while (InterlockedCompareExchange(&w->run, 1, 1)) {
        seg_node_t *node = NULL;
        char *text = NULL;
        char  clip[192];
        unsigned long long t0;

        EnterCriticalSection(&w->cs);
        if (w->head) {
            node = w->head;
            w->head = node->next;
            if (!w->head) w->tail = NULL;
            w->count--;
        }
        LeaveCriticalSection(&w->cs);

        if (!node) {
            WaitForSingleObject(w->wake, 200);
            continue;
        }

        InterlockedExchange(&w->busy, 1);
        t0 = now_ms();
        if (w->backend->transcribe(w->backend, node->pcm, node->n, &text) != 0) {
            set_status(w, "STT error: %s", w->backend->error);
            LOGW("stt: transcribe failed: %s", w->backend->error);
        } else {
            double audio_s = (double)node->n / 16000.0;
            double took_s  = (double)(now_ms() - t0) / 1000.0;
            set_status(w, "STT ok (%.1fs audio in %.1fs, %s)",
                       audio_s, took_s, w->backend->name);
            if (text && !is_noise_text(text)) {
                LOGI("stt: %.1fs audio in %.1fs -> '%s'", audio_s, took_s,
                     utf8_clip(clip, sizeof(clip), text, 160));
                if (w->cb) w->cb(w->user, text, audio_s);
            } else {
                LOGI("stt: %.1fs audio in %.1fs -> (nothing usable: '%s')",
                     audio_s, took_s, utf8_clip(clip, sizeof(clip), text, 60));
            }
        }
        InterlockedExchange(&w->busy, 0);

        free(text);
        free(node->pcm);
        free(node);
    }

    w->backend->close(w->backend);
    return 0;
}

stt_worker_t *stt_worker_create(void)
{
    stt_worker_t *w = (stt_worker_t *)calloc(1, sizeof(*w));
    if (!w) return NULL;
    InitializeCriticalSection(&w->cs);
    w->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!w->wake) { DeleteCriticalSection(&w->cs); free(w); return NULL; }
    snprintf(w->status, sizeof(w->status), "STT idle");
    return w;
}

void stt_worker_destroy(stt_worker_t *w)
{
    if (!w) return;
    stt_worker_stop(w);
    CloseHandle(w->wake);
    DeleteCriticalSection(&w->cs);
    free(w);
}

int stt_worker_start(stt_worker_t *w, const config_t *cfg, stt_result_cb cb, void *user)
{
    if (!w || w->thread) return 0;

    w->cfg  = *cfg;
    w->cb   = cb;
    w->user = user;

    w->backend = (cfg->stt_backend == STT_BACKEND_LOCAL) ? stt_local_create()
                                                         : stt_remote_create();
    if (!w->backend) { set_status(w, "STT backend allocation failed"); return -1; }

    InterlockedExchange(&w->run, 1);
    w->thread = CreateThread(NULL, 0, worker_thread, w, 0, NULL);
    if (!w->thread) {
        InterlockedExchange(&w->run, 0);
        w->backend->destroy(w->backend);
        w->backend = NULL;
        set_status(w, "STT thread creation failed");
        return -1;
    }
    return 0;
}

void stt_worker_stop(stt_worker_t *w)
{
    seg_node_t *n;
    if (!w || !w->thread) return;

    InterlockedExchange(&w->run, 0);
    SetEvent(w->wake);
    if (WaitForSingleObject(w->thread, 60000) == WAIT_TIMEOUT) {
        /* A huge segment is still inside whisper_full. Leak rather than free
         * memory the running thread is still reading. */
        LOGE("stt: worker did not stop in 60s; leaving it detached");
        CloseHandle(w->thread);
        w->thread  = NULL;
        w->backend = NULL;
        set_status(w, "STT stop timed out");
        return;
    }
    CloseHandle(w->thread);
    w->thread = NULL;

    EnterCriticalSection(&w->cs);
    n = w->head;
    w->head = w->tail = NULL;
    w->count = 0;
    LeaveCriticalSection(&w->cs);
    while (n) { seg_node_t *nx = n->next; free(n->pcm); free(n); n = nx; }

    if (w->backend) { w->backend->destroy(w->backend); w->backend = NULL; }
    set_status(w, "STT stopped");
}

void stt_worker_submit(stt_worker_t *w, float *pcm, size_t n)
{
    seg_node_t *node;
    int queued;

    if (!w || !w->thread) { free(pcm); return; }
    node = (seg_node_t *)calloc(1, sizeof(*node));
    if (!node) { free(pcm); return; }
    node->pcm = pcm;
    node->n   = n;

    EnterCriticalSection(&w->cs);
    if (w->count >= MAX_QUEUE) {          /* stay real-time: drop the oldest */
        seg_node_t *old = w->head;
        w->head = old->next;
        if (!w->head) w->tail = NULL;
        w->count--;
        free(old->pcm);
        free(old);
        LOGW("stt: queue full, dropped oldest segment");
    }
    if (w->tail) w->tail->next = node; else w->head = node;
    w->tail = node;
    w->count++;
    queued = w->count;
    LeaveCriticalSection(&w->cs);

    LOGI("stt: queued %.1fs segment (%d waiting)", (double)n / 16000.0, queued);
    SetEvent(w->wake);
}

int stt_worker_backlog(stt_worker_t *w)
{
    int n;
    if (!w) return 0;
    EnterCriticalSection(&w->cs);
    n = w->count;
    LeaveCriticalSection(&w->cs);
    return n;
}

int stt_worker_busy(stt_worker_t *w)
{
    return w ? (int)InterlockedCompareExchange(&w->busy, 0, 0) : 0;
}

void stt_worker_status(stt_worker_t *w, char *out, size_t cap)
{
    if (!w) { if (cap) out[0] = '\0'; return; }
    EnterCriticalSection(&w->cs);
    snprintf(out, cap, "%s", w->status);
    LeaveCriticalSection(&w->cs);
}
