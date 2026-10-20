/*
 * Host-side tests for the parts of call_assist that are pure C and portable:
 * the JSON parser, the resampler, the VAD segmenter, the WAV writer and the
 * .env reader. Build and run with tests/run.sh (Linux/gcc) - no Win32 needed.
 */
#include "dotenv.h"
#include "json.h"
#include "resample.h"
#include "vad.h"
#include "wav.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) { g_fail++;                                    \
        printf("  FAIL %s:%d  ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__); printf("\n"); }                    \
} while (0)

/* ------------------------------------------------------------------- json */

static void test_json(void)
{
    printf("json\n");

    {   /* a real streaming chunk from an OpenAI-compatible server */
        const char *chunk =
            "{\"id\":\"chatcmpl-1\",\"object\":\"chat.completion.chunk\","
            "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello\"},"
            "\"finish_reason\":null}]}";
        json_value_t *v = json_parse(chunk);
        CHECK(v != NULL, "chunk did not parse");
        CHECK(v && strcmp(json_path_str(v, "choices[0].delta.content"), "Hello") == 0,
              "delta.content wrong");
        CHECK(v && json_path_str(v, "choices[0].delta.missing") == NULL,
              "missing key should be NULL");
        json_free(v);
    }
    {   /* non-streaming completion */
        const char *full =
            "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"line1\\nline2\"}}]}";
        json_value_t *v = json_parse(full);
        const char *c = json_path_str(v, "choices[0].message.content");
        CHECK(c && strcmp(c, "line1\nline2") == 0, "escaped newline not decoded");
        json_free(v);
    }
    {   /* error payload + unicode + surrogate pair */
        json_value_t *v = json_parse("{\"error\":{\"message\":\"\\u041f\\u0440\\u0438 \\ud83d\\ude00\"}}");
        const char *m = json_path_str(v, "error.message");
        CHECK(m && strcmp(m, "\xd0\x9f\xd1\x80\xd0\xb8 \xf0\x9f\x98\x80") == 0,
              "unicode/surrogate decode wrong: %s", m ? m : "(null)");
        json_free(v);
    }
    {   /* whisper transcription response */
        json_value_t *v = json_parse("{\"text\":\"  hi there  \"}");
        CHECK(v && strcmp(json_path_str(v, "text"), "  hi there  ") == 0, "text field");
        json_free(v);
    }
    {   /* numbers, bools, null, empty containers, deep nesting */
        json_value_t *v = json_parse("{\"a\":[1,-2.5e3,true,false,null,{},[]],\"b\":{}}");
        CHECK(v != NULL, "mixed types failed to parse");
        CHECK(v && json_at(json_path(v, "a"), 1) &&
              fabs(json_at(json_path(v, "a"), 1)->num + 2500.0) < 1e-9, "number parse");
        json_free(v);
    }
    {   /* malformed input must return NULL, not crash */
        const char *bad[] = { "{", "{\"a\":}", "[1,]", "\"unterminated", "{\"a\" 1}", "" };
        size_t i;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            json_value_t *v = json_parse(bad[i]);
            CHECK(v == NULL, "malformed input %zu accepted: %s", i, bad[i]);
            json_free(v);
        }
    }
    {   /* runaway nesting must be rejected, not blow the stack */
        char deep[600];
        int i;
        for (i = 0; i < 299; ++i) deep[i] = '[';
        for (i = 299; i < 598; ++i) deep[i] = ']';
        deep[598] = 0;
        CHECK(json_parse(deep) == NULL, "depth limit not enforced");
    }
}

/* -------------------------------------------------------------- resampler */

static void test_resample(void)
{
    resampler_t r;
    size_t total = 0, i;
    double err_max = 0.0;

    printf("resampler 48000 -> 16000\n");
    CHECK(resampler_init(&r, 48000, 16000) == 0, "init failed");

    /* one second of 440 Hz, pushed in 10 ms chunks like WASAPI would */
    for (i = 0; i < 100; ++i) {
        float in[480];
        const float *out = NULL;
        size_t k, n;
        for (k = 0; k < 480; ++k) {
            double t = (double)(i * 480 + k) / 48000.0;
            in[k] = (float)(0.5 * sin(2.0 * M_PI * 440.0 * t));
        }
        n = resampler_push(&r, in, 480, &out);
        total += n;
    }
    CHECK(total >= 15900 && total <= 16010, "expected ~16000 samples, got %zu", total);
    resampler_free(&r);

    /* rate stays correct across odd chunk sizes (no drift, no accumulation) */
    {
        resampler_t r2;
        size_t t2 = 0;
        resampler_init(&r2, 44100, 16000);
        for (i = 0; i < 441; ++i) {
            float in[100];
            const float *out = NULL;
            size_t k;
            for (k = 0; k < 100; ++k) in[k] = 0.1f;
            t2 += resampler_push(&r2, in, 100, &out);
        }
        CHECK(t2 >= 15900 && t2 <= 16010, "44.1k: expected ~16000, got %zu", t2);
        /* a DC input must come back as DC, not garbage */
        {
            const float *out = NULL;
            float in[100];
            size_t k, n;
            for (k = 0; k < 100; ++k) in[k] = 0.1f;
            n = resampler_push(&r2, in, 100, &out);
            for (k = 0; k < n; ++k) {
                double e = fabs((double)out[k] - 0.1);
                if (e > err_max) err_max = e;
            }
        }
        CHECK(err_max < 1e-4, "DC not preserved, max error %g", err_max);
        resampler_free(&r2);
    }
}

/* --------------------------------------------------------------------- vad */

static void feed(vad_t *v, const float *pcm, size_t n, int *segments, size_t *last_len)
{
    size_t i;
    for (i = 0; i + VAD_FRAME_LEN <= n; i += VAD_FRAME_LEN) {
        float *seg = NULL;
        size_t len = 0;
        if (vad_push_frame(v, pcm + i, &seg, &len)) {
            (*segments)++;
            *last_len = len;
            free(seg);
        }
    }
}

static void test_vad(void)
{
    vad_t v;
    float *buf;
    size_t n, i;
    int segments = 0;
    size_t last_len = 0;
    const size_t sil1 = 16000 / 2, speech = 16000, sil2 = 16000;

    printf("vad segmentation\n");
    CHECK(vad_init(&v) == 0, "vad_init failed");
    vad_configure(&v, -42, 700, 240, 20000, 20000);

    n = sil1 + speech + sil2;
    buf = (float *)calloc(n, sizeof(float));
    for (i = 0; i < speech; ++i) {          /* -20 dBFS tone */
        double t = (double)i / 16000.0;
        buf[sil1 + i] = (float)(0.1 * sin(2.0 * M_PI * 300.0 * t));
    }
    feed(&v, buf, n, &segments, &last_len);

    CHECK(segments == 1, "expected exactly 1 segment, got %d", segments);
    /* pre-roll (<=320 ms) + 1000 ms speech + ~200 ms kept tail */
    CHECK(last_len > 16000 && last_len < (size_t)(16000 * 1.8),
          "segment length %zu samples out of range", last_len);

    /* pure silence must never emit */
    segments = 0;
    memset(buf, 0, n * sizeof(float));
    vad_reset(&v);
    feed(&v, buf, n, &segments, &last_len);
    CHECK(segments == 0, "silence produced %d segments", segments);

    /* max_segment_ms must force a cut on continuous speech */
    segments = 0;
    vad_reset(&v);
    vad_configure(&v, -42, 700, 240, 2000, 2000);
    free(buf);
    n = 16000 * 5;
    buf = (float *)malloc(n * sizeof(float));
    for (i = 0; i < n; ++i) {
        double t = (double)i / 16000.0;
        buf[i] = (float)(0.1 * sin(2.0 * M_PI * 300.0 * t));
    }
    feed(&v, buf, n, &segments, &last_len);
    CHECK(segments >= 2, "long speech should be cut into >=2 segments, got %d", segments);

    free(buf);
    vad_free(&v);
}

/* --------------------------------------------------------------------- wav */

static unsigned rd32(const unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

static void test_wav(void)
{
    float pcm[4] = { 0.0f, 1.0f, -1.0f, 2.0f };   /* last one must clamp */
    unsigned char *out = NULL;
    size_t len = 0;

    printf("wav writer\n");
    CHECK(wav_from_float(pcm, 4, 16000, &out, &len) == 0, "wav_from_float failed");
    CHECK(len == 44 + 8, "unexpected size %zu", len);
    CHECK(memcmp(out, "RIFF", 4) == 0 && memcmp(out + 8, "WAVE", 4) == 0, "riff header");
    CHECK(rd32(out + 24) == 16000, "sample rate");
    CHECK(rd32(out + 28) == 32000, "byte rate");
    CHECK(out[22] == 1 && out[34] == 16, "mono 16-bit");
    CHECK(rd32(out + 40) == 8, "data chunk size");
    CHECK((short)(out[46] | (out[47] << 8)) == 32767, "full scale");
    CHECK((short)(out[48] | (out[49] << 8)) == -32767, "negative full scale");
    CHECK((short)(out[50] | (out[51] << 8)) == 32767, "clamping");
    free(out);
}

/* ----------------------------------------------------------------- dotenv */

static void test_dotenv(void)
{
    const char *src =
        "\xEF\xBB\xBF"                         /* BOM must be skipped */
        "# a comment\n"
        "\n"
        "  PLAIN=hello world  \n"              /* trailing blanks trimmed */
        "export EXPORTED=yes\n"
        "SPACED  =  tight\n"                   /* blanks around '=' */
        "QUOTED=\"line1\\nline2\"\n"           /* \n escape -> newline */
        "LITERAL='raw\\nkeeps'\n"              /* single quotes: no escapes */
        "MULTI=\"first\nsecond\"\n"            /* real newline inside quotes */
        "CRLF=\"a\r\nb\"\n"                    /* CRLF folded to LF */
        "HASHY=value # not a comment\n"        /* bare values keep '#' */
        "EMPTY=\n"
        "DUP=one\n"
        "DUP=two\n"                            /* last duplicate wins */
        "no_equals_here\n"                     /* skipped, not fatal */
        "=novalue\n"                           /* empty key, skipped */
        "ESCAPES=\"tab\\there\\\\done\\\"q\\\"\"\n";

    dotenv_t *e = dotenv_parse(src);
    printf("dotenv\n");
    CHECK(e != NULL, "parse returned NULL");
    if (!e) return;

    CHECK(dotenv_get(e, "PLAIN") && strcmp(dotenv_get(e, "PLAIN"), "hello world") == 0,
          "PLAIN = %s", dotenv_get(e, "PLAIN"));
    CHECK(dotenv_get(e, "EXPORTED") && strcmp(dotenv_get(e, "EXPORTED"), "yes") == 0,
          "EXPORTED = %s", dotenv_get(e, "EXPORTED"));
    CHECK(dotenv_get(e, "SPACED") && strcmp(dotenv_get(e, "SPACED"), "tight") == 0,
          "SPACED = %s", dotenv_get(e, "SPACED"));
    CHECK(dotenv_get(e, "QUOTED") && strcmp(dotenv_get(e, "QUOTED"), "line1\nline2") == 0,
          "QUOTED = %s", dotenv_get(e, "QUOTED"));
    CHECK(dotenv_get(e, "LITERAL") && strcmp(dotenv_get(e, "LITERAL"), "raw\\nkeeps") == 0,
          "LITERAL = %s", dotenv_get(e, "LITERAL"));
    CHECK(dotenv_get(e, "MULTI") && strcmp(dotenv_get(e, "MULTI"), "first\nsecond") == 0,
          "MULTI = %s", dotenv_get(e, "MULTI"));
    CHECK(dotenv_get(e, "CRLF") && strcmp(dotenv_get(e, "CRLF"), "a\nb") == 0,
          "CRLF kept a carriage return");
    CHECK(dotenv_get(e, "HASHY") && strcmp(dotenv_get(e, "HASHY"), "value # not a comment") == 0,
          "HASHY = %s", dotenv_get(e, "HASHY"));
    CHECK(dotenv_get(e, "EMPTY") && dotenv_get(e, "EMPTY")[0] == '\0',
          "EMPTY should be present and blank");
    CHECK(dotenv_get(e, "DUP") && strcmp(dotenv_get(e, "DUP"), "two") == 0,
          "DUP = %s (last should win)", dotenv_get(e, "DUP"));
    CHECK(dotenv_get(e, "ESCAPES") &&
          strcmp(dotenv_get(e, "ESCAPES"), "tab\there\\done\"q\"") == 0,
          "ESCAPES = %s", dotenv_get(e, "ESCAPES"));

    CHECK(dotenv_get(e, "MISSING") == NULL, "absent key must be NULL");
    CHECK(dotenv_get(e, "no_equals_here") == NULL, "line without '=' must be skipped");
    CHECK(dotenv_get(e, "plain") == NULL, "lookup must be case-sensitive");
    CHECK(dotenv_count(e) == 11, "expected 11 keys, got %zu", dotenv_count(e));

    dotenv_free(e);

    /* degenerate inputs must not crash */
    e = dotenv_parse("");
    CHECK(e && dotenv_count(e) == 0, "empty text");
    dotenv_free(e);
    e = dotenv_parse(NULL);
    CHECK(e && dotenv_count(e) == 0, "NULL text");
    dotenv_free(e);
    e = dotenv_parse("UNTERMINATED=\"still taken\n");
    CHECK(e && dotenv_get(e, "UNTERMINATED") != NULL, "unterminated quote");
    dotenv_free(e);
    dotenv_free(NULL);
}

int main(void)
{
    test_json();
    test_resample();
    test_vad();
    test_wav();
    test_dotenv();
    if (g_fail) { printf("\n%d check(s) FAILED\n", g_fail); return 1; }
    printf("\nall checks passed\n");
    return 0;
}
