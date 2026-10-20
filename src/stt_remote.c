#include "stt.h"
#include "http.h"
#include "wav.h"
#include "json.h"
#include "util.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOUNDARY "----callassistboundary7f3c9a1e"

typedef struct {
    char endpoint[CFG_URL_MAX];
    char api_key[CFG_KEY_MAX];
    char model[CFG_NAME_MAX];
    char language[16];
    int  translate;
} remote_t;

static int remote_open(stt_backend_t *b, const config_t *cfg)
{
    remote_t *R = (remote_t *)b->impl;
    snprintf(R->endpoint, sizeof(R->endpoint), "%s", cfg->stt_endpoint);
    snprintf(R->api_key,  sizeof(R->api_key),  "%s", cfg->stt_api_key);
    snprintf(R->model,    sizeof(R->model),    "%s", cfg->stt_model);
    snprintf(R->language, sizeof(R->language), "%s", cfg->whisper_language);
    R->translate = cfg->whisper_translate;

    if (!R->endpoint[0]) {
        snprintf(b->error, sizeof(b->error), "no remote STT endpoint configured");
        return -1;
    }
    return 0;
}

static void remote_close(stt_backend_t *b) { (void)b; }

static void field(sb_t *sb, const char *name, const char *value)
{
    sb_printf(sb, "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n",
              name, value);
}

static int remote_transcribe(stt_backend_t *b, const float *pcm, size_t n, char **out)
{
    remote_t *R = (remote_t *)b->impl;
    unsigned char *wav = NULL;
    size_t wav_len = 0;
    sb_t body;
    char url[CFG_URL_MAX + 16];
    http_response_t resp;
    json_value_t *root;
    const char *text;
    int rc = -1;

    *out = NULL;

    if (wav_from_float(pcm, n, 16000, &wav, &wav_len) != 0) {
        snprintf(b->error, sizeof(b->error), "wav encoding failed");
        return -1;
    }

    /* OpenAI exposes translation as a sibling route, not a form field. */
    snprintf(url, sizeof(url), "%s", R->endpoint);
    if (R->translate) {
        char *tail = strstr(url, "/transcriptions");
        if (tail) snprintf(tail, sizeof(url) - (size_t)(tail - url), "/translations");
    }

    sb_init(&body);
    field(&body, "model", R->model[0] ? R->model : "whisper-1");
    field(&body, "response_format", "json");
    if (!R->translate && R->language[0] && strcmp(R->language, "auto") != 0)
        field(&body, "language", R->language);

    sb_printf(&body, "--" BOUNDARY "\r\n"
                     "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                     "Content-Type: audio/wav\r\n\r\n");
    sb_append(&body, wav, wav_len);
    sb_printf(&body, "\r\n--" BOUNDARY "--\r\n");
    free(wav);

    memset(&resp, 0, sizeof(resp));
    if (http_post(url, "multipart/form-data; boundary=" BOUNDARY,
                  body.data, body.len, R->api_key, NULL, 60000, &resp) != 0) {
        snprintf(b->error, sizeof(b->error), "%s", resp.error[0] ? resp.error : "request failed");
        goto done;
    }
    if (resp.status < 200 || resp.status >= 300) {
        snprintf(b->error, sizeof(b->error), "HTTP %d: %.160s",
                 resp.status, resp.body ? resp.body : "");
        goto done;
    }

    root = json_parse(resp.body ? resp.body : "");
    if (!root) {
        snprintf(b->error, sizeof(b->error), "malformed STT response");
        goto done;
    }
    text = json_path_str(root, "text");
    if (!text) text = json_path_str(root, "results[0].text");   /* some gateways */
    *out = xstrdup(text ? text : "");
    if (*out) str_trim(*out);
    json_free(root);
    rc = 0;

done:
    sb_free(&body);
    http_response_free(&resp);
    return rc;
}

static void remote_destroy(stt_backend_t *b)
{
    free(b->impl);
    free(b);
}

stt_backend_t *stt_remote_create(void)
{
    stt_backend_t *b = (stt_backend_t *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->impl = calloc(1, sizeof(remote_t));
    if (!b->impl) { free(b); return NULL; }
    b->open       = remote_open;
    b->close      = remote_close;
    b->transcribe = remote_transcribe;
    b->destroy    = remote_destroy;
    snprintf(b->name, sizeof(b->name), "remote API");
    return b;
}
