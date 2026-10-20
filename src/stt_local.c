#include "stt.h"
#include "util.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CALLASSIST_WITH_WHISPER
#include "whisper.h"

#define MIN_SAMPLES 17600            /* whisper wants >= ~1.1 s of audio */

typedef struct {
    struct whisper_context *ctx;
    void *model_blob;              /* only used for non-ASCII paths */
    char  model_path[MAX_PATH];
    char  language[16];
    int   threads;
    int   translate;
    int   gpu;
} local_t;

static int path_is_ascii(const char *p)
{
    for (; *p; ++p) if ((unsigned char)*p > 0x7F) return 0;
    return 1;
}

/*
 * whisper.cpp opens model files through the narrow CRT, which mangles paths
 * outside the active ANSI code page. Prefer the 8.3 short path; if the volume
 * has no short names, fall back to loading the blob ourselves.
 */
static int open_model(stt_backend_t *b, local_t *L, struct whisper_context_params cp)
{
    wchar_t *wpath = NULL;
    wchar_t  shortw[MAX_PATH];
    char    *narrow = NULL;
    FILE    *f = NULL;
    long     size;

    if (path_is_ascii(L->model_path)) {
        L->ctx = whisper_init_from_file_with_params(L->model_path, cp);
        if (L->ctx) return 0;
        snprintf(b->error, sizeof(b->error), "cannot load model '%s'", L->model_path);
        return -1;
    }

    wpath = utf8_to_wide(L->model_path);
    if (wpath && GetShortPathNameW(wpath, shortw, MAX_PATH)) {
        narrow = wide_to_utf8(shortw);
        if (narrow && path_is_ascii(narrow)) {
            L->ctx = whisper_init_from_file_with_params(narrow, cp);
            free(narrow);
            free(wpath);
            if (L->ctx) return 0;
            snprintf(b->error, sizeof(b->error), "cannot load model '%s'", L->model_path);
            return -1;
        }
        free(narrow);
    }

    /* last resort: read the whole file and hand whisper a buffer */
    if (wpath) f = _wfopen(wpath, L"rb");
    free(wpath);
    if (!f) {
        snprintf(b->error, sizeof(b->error), "cannot open model '%s'", L->model_path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); snprintf(b->error, sizeof(b->error), "empty model file"); return -1; }

    L->model_blob = malloc((size_t)size);
    if (!L->model_blob || fread(L->model_blob, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(L->model_blob);
        L->model_blob = NULL;
        snprintf(b->error, sizeof(b->error), "could not read the model file");
        return -1;
    }
    fclose(f);

    L->ctx = whisper_init_from_buffer_with_params(L->model_blob, (size_t)size, cp);
    if (!L->ctx) {
        free(L->model_blob);
        L->model_blob = NULL;
        snprintf(b->error, sizeof(b->error), "cannot load model '%s'", L->model_path);
        return -1;
    }
    return 0;
}

static int local_open(stt_backend_t *b, const config_t *cfg)
{
    local_t *L = (local_t *)b->impl;
    struct whisper_context_params cp = whisper_context_default_params();

    snprintf(L->model_path, sizeof(L->model_path), "%s", cfg->whisper_model);
    snprintf(L->language,   sizeof(L->language),   "%s", cfg->whisper_language);
    L->threads   = cfg->whisper_threads;
    L->translate = cfg->whisper_translate;
    L->gpu       = cfg->whisper_gpu;

    if (!L->model_path[0]) {
        snprintf(b->error, sizeof(b->error),
                 "no Whisper model selected (Settings > Speech-to-Text)");
        return -1;
    }

    cp.use_gpu = L->gpu ? true : false;

    if (open_model(b, L, cp) != 0) return -1;
    LOGI("whisper: loaded %s (multilingual=%d)",
         L->model_path, whisper_is_multilingual(L->ctx));
    return 0;
}

static void local_close(stt_backend_t *b)
{
    local_t *L = (local_t *)b->impl;
    if (L->ctx) { whisper_free(L->ctx); L->ctx = NULL; }
    free(L->model_blob);
    L->model_blob = NULL;
}

static int local_transcribe(stt_backend_t *b, const float *pcm, size_t n, char **out)
{
    local_t *L = (local_t *)b->impl;
    struct whisper_full_params p;
    float *padded = NULL;
    int rc, i, nseg;
    sb_t sb;

    *out = NULL;
    if (!L->ctx) {
        snprintf(b->error, sizeof(b->error), "model not loaded");
        return -1;
    }

    if (n < MIN_SAMPLES) {
        padded = (float *)calloc(MIN_SAMPLES, sizeof(float));
        if (!padded) { snprintf(b->error, sizeof(b->error), "out of memory"); return -1; }
        memcpy(padded, pcm, n * sizeof(float));
        pcm = padded;
        n   = MIN_SAMPLES;
    }

    p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.n_threads        = L->threads;
    p.translate        = L->translate ? true : false;
    p.no_context       = true;           /* segments are independent utterances */
    p.single_segment   = false;
    p.no_timestamps    = true;
    p.print_progress   = false;
    p.print_realtime   = false;
    p.print_timestamps = false;
    p.print_special    = false;
    p.suppress_blank   = true;
    p.language         = L->language[0] ? L->language : "auto";

    rc = whisper_full(L->ctx, p, pcm, (int)n);
    free(padded);
    if (rc != 0) {
        snprintf(b->error, sizeof(b->error), "whisper_full failed (%d)", rc);
        return -1;
    }

    sb_init(&sb);
    nseg = whisper_full_n_segments(L->ctx);
    for (i = 0; i < nseg; ++i) {
        const char *t = whisper_full_get_segment_text(L->ctx, i);
        if (t) sb_puts(&sb, t);
    }
    if (sb.data) str_trim(sb.data);
    *out = sb.data ? sb.data : xstrdup("");
    return 0;
}

static void local_destroy(stt_backend_t *b)
{
    local_close(b);
    free(b->impl);
    free(b);
}

stt_backend_t *stt_local_create(void)
{
    stt_backend_t *b = (stt_backend_t *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->impl = calloc(1, sizeof(local_t));
    if (!b->impl) { free(b); return NULL; }
    b->open       = local_open;
    b->close      = local_close;
    b->transcribe = local_transcribe;
    b->destroy    = local_destroy;
    snprintf(b->name, sizeof(b->name), "whisper.cpp");
    return b;
}

int stt_local_available(void) { return 1; }

#else  /* built without whisper.cpp */

static int stub_open(stt_backend_t *b, const config_t *cfg)
{
    (void)cfg;
    snprintf(b->error, sizeof(b->error),
             "this build has no local Whisper (rebuild with -DCALLASSIST_WITH_WHISPER=ON) "
             "- switch Speech-to-Text to Remote");
    return -1;
}
static void stub_close(stt_backend_t *b) { (void)b; }
static int  stub_transcribe(stt_backend_t *b, const float *pcm, size_t n, char **out)
{
    (void)pcm; (void)n;
    *out = NULL;
    snprintf(b->error, sizeof(b->error), "local Whisper not compiled in");
    return -1;
}
static void stub_destroy(stt_backend_t *b) { free(b); }

stt_backend_t *stt_local_create(void)
{
    stt_backend_t *b = (stt_backend_t *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->open       = stub_open;
    b->close      = stub_close;
    b->transcribe = stub_transcribe;
    b->destroy    = stub_destroy;
    snprintf(b->name, sizeof(b->name), "local (absent)");
    return b;
}

int stt_local_available(void) { return 0; }

#endif
