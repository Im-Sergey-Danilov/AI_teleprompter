#include "llm.h"
#include "http.h"
#include "json.h"
#include "util.h"
#include "log.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *endpoint;
    char *api_key;
    char *body;
    int   stream;
    llm_event_cb cb;
    void *user;
    volatile LONG cancel;
} job_t;

struct llm_worker {
    HANDLE           thread;
    HANDLE           wake;
    volatile LONG    run;
    volatile LONG    busy;
    CRITICAL_SECTION cs;
    job_t           *pending;
    job_t           *running;
};

static void job_free(job_t *j)
{
    if (!j) return;
    free(j->endpoint);
    free(j->api_key);
    free(j->body);
    free(j);
}

/* --------------------------------------------------------- prompt building */

static char *expand_template(const char *tpl, const char *transcript, const char *last)
{
    sb_t sb;
    SYSTEMTIME st;
    char timebuf[32];

    GetLocalTime(&st);
    snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

    sb_init(&sb);
    while (tpl && *tpl) {
        if (strncmp(tpl, "{transcript}", 12) == 0)      { sb_puts(&sb, transcript ? transcript : ""); tpl += 12; }
        else if (strncmp(tpl, "{last}", 6) == 0)        { sb_puts(&sb, last ? last : "");             tpl += 6;  }
        else if (strncmp(tpl, "{time}", 6) == 0)        { sb_puts(&sb, timebuf);                      tpl += 6;  }
        else                                            { sb_append(&sb, tpl, 1);                     tpl += 1;  }
    }
    return sb.data ? sb.data : xstrdup("");
}

/* Opening brace through the system message, shared by both body builders. */
static void body_open(sb_t *sb, const config_t *cfg, int stream)
{
    sb_puts(sb, "{\"model\":");
    sb_put_json_string(sb, cfg->llm_model);
    sb_printf(sb, ",\"stream\":%s", stream ? "true" : "false");
    /* Negative / zero means "leave it out and let the server decide" - some
     * gateways reject parameters they do not implement. */
    if (cfg->temperature >= 0.0)
        sb_printf(sb, ",\"temperature\":%.3f", cfg->temperature);
    if (cfg->max_tokens > 0)
        sb_printf(sb, ",\"max_tokens\":%d", cfg->max_tokens);
    sb_puts(sb, ",\"messages\":[{\"role\":\"system\",\"content\":");
    sb_put_json_string(sb, cfg->system_prompt);
    sb_puts(sb, "}");
}

/*
 * Earlier exchanges as real user/assistant turns. The model needs them to
 * follow up on what was already asked, even though the overlay only ever shows
 * the newest answer.
 */
static void body_history(sb_t *sb, const llm_turn_t *hist, int n)
{
    int i;
    for (i = 0; i < n; ++i) {
        if (!hist[i].question || !hist[i].answer) continue;
        sb_puts(sb, ",{\"role\":\"user\",\"content\":");
        sb_put_json_string(sb, hist[i].question);
        sb_puts(sb, "},{\"role\":\"assistant\",\"content\":");
        sb_put_json_string(sb, hist[i].answer);
        sb_puts(sb, "}");
    }
}

/* Vision request: the user turn carries a text part plus a data-URI image. */
static char *build_image_body(const config_t *cfg, const llm_turn_t *hist, int n_hist,
                              const char *png_b64, const char *instruction, int stream)
{
    sb_t sb;

    sb_init(&sb);
    body_open(&sb, cfg, stream);
    body_history(&sb, hist, n_hist);
    sb_puts(&sb, ",{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":");
    sb_put_json_string(&sb, instruction ? instruction : "");
    sb_puts(&sb, "},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,");
    sb_puts(&sb, png_b64 ? png_b64 : "");
    sb_puts(&sb, "\"}}]}]}");
    return sb.data ? sb.data : xstrdup("{}");
}

static char *build_body(const config_t *cfg, const llm_turn_t *hist, int n_hist,
                        const char *transcript, const char *last,
                        const char *extra, int stream)
{
    sb_t sb;
    char *user_msg = expand_template(cfg->user_template, transcript, last);

    sb_init(&sb);
    body_open(&sb, cfg, stream);
    body_history(&sb, hist, n_hist);
    sb_puts(&sb, ",{\"role\":\"user\",\"content\":");
    if (extra && *extra) {
        sb_t joined;
        sb_init(&joined);
        sb_puts(&joined, user_msg ? user_msg : "");
        sb_puts(&joined, "\n\n");
        sb_puts(&joined, extra);
        sb_put_json_string(&sb, joined.data ? joined.data : "");
        sb_free(&joined);
    } else {
        sb_put_json_string(&sb, user_msg);
    }
    sb_puts(&sb, "}]}");

    free(user_msg);
    return sb.data ? sb.data : xstrdup("{}");
}

/* ------------------------------------------------------------ response bits */

static const char *extract_error(const json_value_t *root)
{
    const char *m = json_path_str(root, "error.message");
    if (!m) m = json_path_str(root, "message");
    if (!m) m = json_path_str(root, "detail");
    return m;
}

typedef struct {
    job_t *job;
    int    got_any;
    int    got_error;   /* the server already told us what went wrong */
    int    done;
} sse_ctx_t;

static int on_sse(void *user, const char *data)
{
    sse_ctx_t *ctx = (sse_ctx_t *)user;
    json_value_t *root;
    const char *piece;

    if (!data || !*data) return 0;
    if (strcmp(data, "[DONE]") == 0) { ctx->done = 1; return 1; }

    root = json_parse(data);
    if (!root) return 0;

    piece = json_path_str(root, "choices[0].delta.content");
    if (!piece) piece = json_path_str(root, "choices[0].message.content");
    if (!piece) piece = json_path_str(root, "choices[0].text");

    if (piece && *piece) {
        ctx->got_any = 1;
        ctx->job->cb(ctx->job->user, LLM_EVT_DELTA, piece);
    } else {
        const char *err = extract_error(root);
        if (err) {
            ctx->got_error = 1;
            ctx->job->cb(ctx->job->user, LLM_EVT_ERROR, err);
            json_free(root);
            return 1;
        }
    }
    json_free(root);
    return InterlockedCompareExchange(&ctx->job->cancel, 0, 0) ? 1 : 0;
}

static void run_job(job_t *j)
{
    http_response_t resp;
    memset(&resp, 0, sizeof(resp));

    j->cb(j->user, LLM_EVT_START, NULL);

    if (j->stream) {
        sse_ctx_t ctx;
        ctx.job = j; ctx.got_any = 0; ctx.got_error = 0; ctx.done = 0;

        if (http_post_sse(j->endpoint, "application/json", j->body, strlen(j->body),
                          j->api_key, 120000, on_sse, &ctx, &j->cancel, &resp) != 0) {
            j->cb(j->user, LLM_EVT_ERROR, resp.error[0] ? resp.error : "request failed");
            http_response_free(&resp);
            return;
        }
        if (resp.status < 200 || resp.status >= 300) {
            char msg[512];
            json_value_t *root = json_parse(resp.body ? resp.body : "");
            const char *e = root ? extract_error(root) : NULL;
            snprintf(msg, sizeof(msg), "HTTP %d: %.400s", resp.status,
                     e ? e : (resp.body ? resp.body : ""));
            json_free(root);
            j->cb(j->user, LLM_EVT_ERROR, msg);
        } else if (!ctx.got_any && !ctx.got_error &&
                   !InterlockedCompareExchange(&j->cancel, 0, 0)) {
            /* Log what actually came back - "empty stream" on its own says
             * nothing about whether the server refused, streamed only
             * reasoning tokens, or answered in a shape we do not read. */
            LOGW("llm: stream produced no content (HTTP %d, %d bytes): %.300s",
                 resp.status, (int)resp.body_len, resp.body ? resp.body : "(no body)");
            j->cb(j->user, LLM_EVT_ERROR,
                  "empty stream from the API (try turning streaming off in Settings > LLM)");
        }
        http_response_free(&resp);
        j->cb(j->user, LLM_EVT_DONE, NULL);
        return;
    }

    if (http_post(j->endpoint, "application/json", j->body, strlen(j->body),
                  j->api_key, NULL, 120000, &resp) != 0) {
        j->cb(j->user, LLM_EVT_ERROR, resp.error[0] ? resp.error : "request failed");
        http_response_free(&resp);
        return;
    }
    {
        json_value_t *root = json_parse(resp.body ? resp.body : "");
        const char *text = root ? json_path_str(root, "choices[0].message.content") : NULL;
        if (!text && root) text = json_path_str(root, "choices[0].text");

        if (resp.status < 200 || resp.status >= 300) {
            char msg[512];
            const char *e = root ? extract_error(root) : NULL;
            snprintf(msg, sizeof(msg), "HTTP %d: %.400s", resp.status,
                     e ? e : (resp.body ? resp.body : ""));
            j->cb(j->user, LLM_EVT_ERROR, msg);
        } else if (text) {
            j->cb(j->user, LLM_EVT_DELTA, text);
        } else {
            j->cb(j->user, LLM_EVT_ERROR, "no content in API response");
        }
        json_free(root);
    }
    http_response_free(&resp);
    j->cb(j->user, LLM_EVT_DONE, NULL);
}

/* ----------------------------------------------------------------- worker  */

static DWORD WINAPI llm_thread(LPVOID arg)
{
    llm_worker_t *w = (llm_worker_t *)arg;

    while (InterlockedCompareExchange(&w->run, 1, 1)) {
        job_t *j = NULL;

        EnterCriticalSection(&w->cs);
        if (w->pending) { j = w->pending; w->pending = NULL; w->running = j; }
        LeaveCriticalSection(&w->cs);

        if (!j) { WaitForSingleObject(w->wake, 200); continue; }

        InterlockedExchange(&w->busy, 1);
        run_job(j);
        InterlockedExchange(&w->busy, 0);

        EnterCriticalSection(&w->cs);
        w->running = NULL;
        LeaveCriticalSection(&w->cs);
        job_free(j);
    }
    return 0;
}

llm_worker_t *llm_create(void)
{
    llm_worker_t *w = (llm_worker_t *)calloc(1, sizeof(*w));
    if (!w) return NULL;
    InitializeCriticalSection(&w->cs);
    w->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!w->wake) { DeleteCriticalSection(&w->cs); free(w); return NULL; }
    InterlockedExchange(&w->run, 1);
    w->thread = CreateThread(NULL, 0, llm_thread, w, 0, NULL);
    if (!w->thread) {
        CloseHandle(w->wake);
        DeleteCriticalSection(&w->cs);
        free(w);
        return NULL;
    }
    return w;
}

void llm_destroy(llm_worker_t *w)
{
    if (!w) return;
    llm_cancel(w);
    InterlockedExchange(&w->run, 0);
    SetEvent(w->wake);
    WaitForSingleObject(w->thread, 8000);
    CloseHandle(w->thread);
    CloseHandle(w->wake);
    EnterCriticalSection(&w->cs);
    job_free(w->pending);
    w->pending = NULL;
    LeaveCriticalSection(&w->cs);
    DeleteCriticalSection(&w->cs);
    free(w);
}

int llm_request(llm_worker_t *w, const config_t *cfg,
                const llm_turn_t *hist, int n_hist,
                const char *transcript, const char *last,
                const char *extra, llm_event_cb cb, void *user)
{
    job_t *j;

    if (!w || !cfg || !cb) return -1;
    if (!cfg->llm_endpoint[0]) return -1;

    j = (job_t *)calloc(1, sizeof(*j));
    if (!j) return -1;
    j->endpoint = xstrdup(cfg->llm_endpoint);
    j->api_key  = xstrdup(cfg->llm_api_key);
    j->stream   = cfg->llm_stream;
    j->body     = build_body(cfg, hist, n_hist, transcript, last, extra, j->stream);
    j->cb       = cb;
    j->user     = user;

    if (!j->endpoint || !j->body) { job_free(j); return -1; }

    EnterCriticalSection(&w->cs);
    if (w->pending) { job_free(w->pending); w->pending = NULL; }
    if (w->running) InterlockedExchange(&w->running->cancel, 1);
    w->pending = j;
    LeaveCriticalSection(&w->cs);

    SetEvent(w->wake);
    return 0;
}

int llm_request_image(llm_worker_t *w, const config_t *cfg,
                      const llm_turn_t *hist, int n_hist,
                      const char *png_b64, const char *instruction,
                      llm_event_cb cb, void *user)
{
    job_t *j;

    if (!w || !cfg || !cb) return -1;
    if (!cfg->llm_endpoint[0]) return -1;

    j = (job_t *)calloc(1, sizeof(*j));
    if (!j) return -1;
    j->endpoint = xstrdup(cfg->llm_endpoint);
    j->api_key  = xstrdup(cfg->llm_api_key);
    j->stream   = cfg->llm_stream;
    j->body     = build_image_body(cfg, hist, n_hist, png_b64, instruction, j->stream);
    j->cb       = cb;
    j->user     = user;

    if (!j->endpoint || !j->body) { job_free(j); return -1; }

    EnterCriticalSection(&w->cs);
    if (w->pending) { job_free(w->pending); w->pending = NULL; }
    if (w->running) InterlockedExchange(&w->running->cancel, 1);
    w->pending = j;
    LeaveCriticalSection(&w->cs);

    SetEvent(w->wake);
    return 0;
}

void llm_cancel(llm_worker_t *w)
{
    if (!w) return;
    EnterCriticalSection(&w->cs);
    if (w->pending) { job_free(w->pending); w->pending = NULL; }
    if (w->running) InterlockedExchange(&w->running->cancel, 1);
    LeaveCriticalSection(&w->cs);
}

int llm_busy(llm_worker_t *w)
{
    return w ? (int)InterlockedCompareExchange(&w->busy, 0, 0) : 0;
}

int llm_test(const config_t *cfg, char *out, size_t cap)
{
    sb_t sb;
    http_response_t resp;
    json_value_t *root;
    const char *text;
    int rc = -1;

    sb_init(&sb);
    sb_puts(&sb, "{\"model\":");
    sb_put_json_string(&sb, cfg->llm_model);
    sb_puts(&sb, ",\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}]}");

    memset(&resp, 0, sizeof(resp));
    if (http_post(cfg->llm_endpoint, "application/json", sb.data, sb.len,
                  cfg->llm_api_key, NULL, 30000, &resp) != 0) {
        snprintf(out, cap, "Failed: %s", resp.error[0] ? resp.error : "no response");
        goto done;
    }

    root = json_parse(resp.body ? resp.body : "");
    text = root ? json_path_str(root, "choices[0].message.content") : NULL;

    if (resp.status >= 200 && resp.status < 300 && text) {
        snprintf(out, cap, "OK (HTTP %d). Model replied: %.100s", resp.status, text);
        rc = 0;
    } else {
        const char *e = root ? extract_error(root) : NULL;
        snprintf(out, cap, "HTTP %d: %.300s", resp.status,
                 e ? e : (resp.body ? resp.body : "(empty)"));
    }
    json_free(root);

done:
    sb_free(&sb);
    http_response_free(&resp);
    return rc;
}
