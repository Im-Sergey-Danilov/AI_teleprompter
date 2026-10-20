#include "audio.h"
#include "ringbuf.h"
#include "resample.h"
#include "vad.h"
#include "util.h"
#include "log.h"

#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>

/* Declared locally so we depend on neither INITGUID nor uuid.lib layout. */
static const CLSID CLSID_MMDeviceEnumerator_ =
    {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const IID IID_IMMDeviceEnumerator_ =
    {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const IID IID_IAudioClient_ =
    {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const IID IID_IAudioCaptureClient_ =
    {0xC8ADBD64,0xE71E,0x48A0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};
static const GUID kSubtypeFloat =
    {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID kSubtypePCM =
    {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const PROPERTYKEY kPKEY_Device_FriendlyName =
    {{0xA45C254E,0xDF1C,0x4EFD,{0x80,0x20,0x67,0xD1,0x46,0xA8,0x50,0xE0}}, 14};

#define OUT_RATE       VAD_SAMPLE_RATE           /* 16000 */
#define RING_SECONDS   8
#define MAX_BACKLOG    (OUT_RATE / 4)            /* 250 ms before we resync */
#define REC_MAX_SECONDS 600                      /* 10 min ceiling on one take */

typedef struct audio_engine audio_engine_t;

typedef struct {
    audio_engine_t *eng;
    int             loopback;
    char            device_id[512];
    fring_t         ring;
    HANDLE          thread;
    volatile LONG   run;
    volatile LONG   alive;
    resampler_t     rs;
    float          *mono;        /* scratch: interleaved -> mono */
    size_t          mono_cap;
    unsigned long long dry_since; /* mixer-side: when the ring first ran dry */
} source_t;

struct audio_engine {
    source_t        mic;
    source_t        sys;
    HANDLE          mixer;
    volatile LONG   run;

    audio_segment_cb cb;
    void            *user;

    CRITICAL_SECTION cs;
    float           *rec;              /* push-to-talk buffer, guarded by cs */
    size_t           rec_len, rec_cap;
    volatile LONG    recording;
    float            mic_gain, sys_gain;
    vad_t            vad;
    volatile LONG    level_millidb;
    volatile LONG    mic_level_millidb, sys_level_millidb;
    char             err[256];
    int              started;
};

/* ------------------------------------------------------------- device enum */

int audio_enum_devices(int render, audio_device_info_t **out, int *count)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDeviceCollection *col = NULL;
    UINT n = 0, i;
    audio_device_info_t *list = NULL;
    HRESULT hr;
    int rc = -1;

    *out = NULL;
    *count = 0;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator_, (void **)&en);
    if (FAILED(hr)) return -1;

    hr = IMMDeviceEnumerator_EnumAudioEndpoints(en, render ? eRender : eCapture,
                                                DEVICE_STATE_ACTIVE, &col);
    if (FAILED(hr)) goto done;
    if (FAILED(IMMDeviceCollection_GetCount(col, &n))) goto done;

    list = (audio_device_info_t *)calloc(n ? n : 1, sizeof(*list));
    if (!list) goto done;

    for (i = 0; i < n; ++i) {
        IMMDevice     *dev = NULL;
        IPropertyStore *ps = NULL;
        LPWSTR wid = NULL;
        PROPVARIANT pv;

        if (FAILED(IMMDeviceCollection_Item(col, i, &dev))) continue;
        if (SUCCEEDED(IMMDevice_GetId(dev, &wid)) && wid) {
            char *u = wide_to_utf8(wid);
            if (u) { snprintf(list[i].id, sizeof(list[i].id), "%s", u); free(u); }
            CoTaskMemFree(wid);
        }
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)) && ps) {
            PropVariantInit(&pv);
            if (SUCCEEDED(IPropertyStore_GetValue(ps, &kPKEY_Device_FriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR && pv.pwszVal) {
                char *u = wide_to_utf8(pv.pwszVal);
                if (u) { snprintf(list[i].name, sizeof(list[i].name), "%s", u); free(u); }
            }
            PropVariantClear(&pv);
            IPropertyStore_Release(ps);
        }
        if (!list[i].name[0]) snprintf(list[i].name, sizeof(list[i].name), "(device %u)", i);
        IMMDevice_Release(dev);
    }

    *out = list;
    *count = (int)n;
    rc = 0;

done:
    if (col) IMMDeviceCollection_Release(col);
    if (en)  IMMDeviceEnumerator_Release(en);
    if (rc != 0) free(list);
    return rc;
}

/* ------------------------------------------------------- format conversion */

static int format_kind(const WAVEFORMATEX *wfx, int *bits)
{
    /* returns 1 = float, 2 = signed int PCM, 0 = unsupported */
    *bits = wfx->wBitsPerSample;
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return (*bits == 32) ? 1 : 0;
    if (wfx->wFormatTag == WAVE_FORMAT_PCM)
        return (*bits == 16 || *bits == 24 || *bits == 32) ? 2 : 0;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE *ex = (const WAVEFORMATEXTENSIBLE *)wfx;
        if (IsEqualGUID(&ex->SubFormat, &kSubtypeFloat)) return (*bits == 32) ? 1 : 0;
        if (IsEqualGUID(&ex->SubFormat, &kSubtypePCM))
            return (*bits == 16 || *bits == 24 || *bits == 32) ? 2 : 0;
    }
    return 0;
}

static int mono_reserve(source_t *s, size_t n)
{
    if (n <= s->mono_cap) return 1;
    {
        float *p = (float *)realloc(s->mono, n * sizeof(float));
        if (!p) return 0;
        s->mono = p;
        s->mono_cap = n;
    }
    return 1;
}

static size_t to_mono(source_t *s, const BYTE *data, UINT32 frames,
                      const WAVEFORMATEX *wfx, int kind, int bits, int silent)
{
    const unsigned ch     = wfx->nChannels ? wfx->nChannels : 1;
    const unsigned sample = (unsigned)(bits / 8);
    const size_t   stride = wfx->nBlockAlign ? wfx->nBlockAlign : (size_t)ch * sample;
    UINT32 f;
    unsigned c;

    if (!mono_reserve(s, frames)) return 0;
    if (silent || !data) {
        memset(s->mono, 0, frames * sizeof(float));
        return frames;
    }

    for (f = 0; f < frames; ++f) {
        double acc = 0.0;
        for (c = 0; c < ch; ++c) {
            const BYTE *p = data + (size_t)f * stride + (size_t)c * sample;
            if (kind == 1) {
                float v;
                memcpy(&v, p, 4);
                acc += v;
            } else if (bits == 16) {
                short v;
                memcpy(&v, p, 2);
                acc += (double)v / 32768.0;
            } else if (bits == 24) {
                int v = (int)((unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16));
                if (v & 0x800000) v |= (int)0xFF000000;
                acc += (double)v / 8388608.0;
            } else { /* 32-bit int */
                int v;
                memcpy(&v, p, 4);
                acc += (double)v / 2147483648.0;
            }
        }
        s->mono[f] = (float)(acc / (double)ch);
    }
    return frames;
}

/* ---------------------------------------------------------- capture thread */

static void clear_error(audio_engine_t *e)
{
    EnterCriticalSection(&e->cs);
    e->err[0] = '\0';
    LeaveCriticalSection(&e->cs);
}

static void set_error(audio_engine_t *e, const char *fmt, ...)
{
    va_list ap;
    EnterCriticalSection(&e->cs);
    va_start(ap, fmt);
    vsnprintf(e->err, sizeof(e->err), fmt, ap);
    va_end(ap);
    LeaveCriticalSection(&e->cs);
    LOGE("audio: %s", e->err);
}

/*
 * Returns the endpoint plus the HRESULT that actually explains a failure -
 * collapsing everything to E_FAIL here once cost a long debugging session,
 * because "no capture device on this machine" (0x80070490) and "the device
 * is in use" look identical in the log.
 */
static HRESULT open_device(const char *id, int render, IMMDevice **out)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    HRESULT hr;

    *out = NULL;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator_, (void **)&en);
    if (FAILED(hr)) return hr;

    if (id && *id) {
        wchar_t *wid = utf8_to_wide(id);
        if (wid) {
            if (FAILED(IMMDeviceEnumerator_GetDevice(en, wid, &dev))) dev = NULL;
            free(wid);
        }
    }
    if (!dev)
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
                 en, render ? eRender : eCapture, eConsole, &dev);
    else
        hr = S_OK;

    IMMDeviceEnumerator_Release(en);
    if (FAILED(hr)) return hr;
    if (!dev) return E_FAIL;
    *out = dev;
    return S_OK;
}

/* One connected session; returns when the stream dies or run goes false. */
static HRESULT capture_session(source_t *s)
{
    IMMDevice          *dev = NULL;
    IAudioClient       *client = NULL;
    IAudioCaptureClient *cap = NULL;
    WAVEFORMATEX       *wfx = NULL;
    HRESULT hr;
    int kind, bits;
    DWORD flags;

    hr = open_device(s->device_id, s->loopback, &dev);
    if (FAILED(hr) || !dev) return FAILED(hr) ? hr : E_FAIL;

    hr = IMMDevice_Activate(dev, &IID_IAudioClient_, CLSCTX_ALL, NULL, (void **)&client);
    if (FAILED(hr)) goto done;

    hr = IAudioClient_GetMixFormat(client, &wfx);
    if (FAILED(hr)) goto done;

    kind = format_kind(wfx, &bits);
    if (!kind) { hr = E_NOTIMPL; goto done; }

    flags = s->loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, flags,
                                 2000000 /* 200 ms */, 0, wfx, NULL);
    if (FAILED(hr)) goto done;

    hr = IAudioClient_GetService(client, &IID_IAudioCaptureClient_, (void **)&cap);
    if (FAILED(hr)) goto done;

    if (resampler_init(&s->rs, wfx->nSamplesPerSec, OUT_RATE) != 0) { hr = E_FAIL; goto done; }

    hr = IAudioClient_Start(client);
    if (FAILED(hr)) goto done;

    LOGI("audio: %s stream up (%u Hz, %u ch, %u bit, %s)",
         s->loopback ? "loopback" : "mic",
         (unsigned)wfx->nSamplesPerSec, (unsigned)wfx->nChannels,
         (unsigned)wfx->wBitsPerSample, kind == 1 ? "float" : "pcm");
    InterlockedExchange(&s->alive, 1);
    clear_error(s->eng);      /* the stream recovered; stop showing the old error */

    while (InterlockedCompareExchange(&s->run, 1, 1)) {
        UINT32 packet = 0;
        hr = IAudioCaptureClient_GetNextPacketSize(cap, &packet);
        if (FAILED(hr)) break;

        while (packet) {
            BYTE   *data = NULL;
            UINT32  frames = 0;
            DWORD   bflags = 0;

            hr = IAudioCaptureClient_GetBuffer(cap, &data, &frames, &bflags, NULL, NULL);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
            if (FAILED(hr)) goto stopped;

            if (frames) {
                size_t n = to_mono(s, data, frames, wfx, kind, bits,
                                   (bflags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
                if (n) {
                    const float *out = NULL;
                    size_t m = resampler_push(&s->rs, s->mono, n, &out);
                    if (m && out) fring_write(&s->ring, out, m);
                }
            }
            IAudioCaptureClient_ReleaseBuffer(cap, frames);

            hr = IAudioCaptureClient_GetNextPacketSize(cap, &packet);
            if (FAILED(hr)) goto stopped;
        }
        Sleep(8);
    }

stopped:
    IAudioClient_Stop(client);

done:
    InterlockedExchange(&s->alive, 0);
    resampler_free(&s->rs);
    if (cap)    IAudioCaptureClient_Release(cap);
    if (wfx)    CoTaskMemFree(wfx);
    if (client) IAudioClient_Release(client);
    if (dev)    IMMDevice_Release(dev);
    return hr;
}

static DWORD WINAPI capture_thread(LPVOID arg)
{
    source_t *s = (source_t *)arg;
    int attempts = 0;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    while (InterlockedCompareExchange(&s->run, 1, 1)) {
        HRESULT hr = capture_session(s);
        if (!InterlockedCompareExchange(&s->run, 1, 1)) break;

        /* Log the first failure and then roughly every 30 s, so a source that
         * is still down stays visible without flooding the file. */
        if (attempts == 0 || (attempts % 20) == 0)
            set_error(s->eng, "%s capture failed (hr=0x%08lX); retrying",
                      s->loopback ? "Loopback" : "Microphone", (unsigned long)hr);
        attempts++;
        Sleep(1500);                       /* device switched or unplugged */
    }
    CoUninitialize();
    return 0;
}

/* ------------------------------------------------------------ mixer thread */

static void drain_backlog(fring_t *r, size_t want)
{
    size_t avail = fring_avail(r);
    if (avail > want + MAX_BACKLOG) {
        size_t excess = avail - want;
        float sink[4096];
        while (excess) {
            size_t take = excess > 4096 ? 4096 : excess;
            size_t got = fring_read(r, sink, take);
            if (!got) break;
            excess -= got;
        }
    }
}


#define DRY_GRACE_MS 120        /* late source: wait. quiet source: zero-fill. */

/*
 * How many samples the mixer may take this tick without inventing any.
 * A source that is behind but still delivering holds the whole mixer back for
 * up to DRY_GRACE_MS; after that it is treated as idle and zero-filled.
 */
static size_t source_limit(source_t *s, size_t want, unsigned long long now)
{
    size_t avail;

    if (!InterlockedCompareExchange(&s->alive, 0, 0)) return want;  /* not up */

    avail = fring_avail(&s->ring);
    if (avail >= want) { s->dry_since = 0; return want; }

    if (!s->dry_since) s->dry_since = now;
    if (now - s->dry_since >= DRY_GRACE_MS) return want;            /* gone quiet */
    return avail;                                                   /* just late */
}

static size_t pace_want(audio_engine_t *e, size_t want, unsigned long long now)
{
    size_t lim = source_limit(&e->mic, want, now);
    size_t lim2 = source_limit(&e->sys, want, now);
    if (lim2 < lim) lim = lim2;
    return lim;
}

static DWORD WINAPI mixer_thread(LPVOID arg)
{
    audio_engine_t *e = (audio_engine_t *)arg;
    float *a = NULL, *b = NULL, *mix = NULL;
    size_t cap = OUT_RATE;                 /* 1 s of headroom per tick */
    float frame[VAD_FRAME_LEN];
    size_t frame_fill = 0;
    unsigned long long t0 = now_ms();
    unsigned long long emitted = 0;

    a   = (float *)malloc(cap * sizeof(float));
    b   = (float *)malloc(cap * sizeof(float));
    mix = (float *)malloc(cap * sizeof(float));
    if (!a || !b || !mix) goto done;

    while (InterlockedCompareExchange(&e->run, 1, 1)) {
        unsigned long long elapsed = now_ms() - t0;
        unsigned long long target  = elapsed * OUT_RATE / 1000ULL;
        size_t want, got_a, got_b, i;
        float mg, sg;

        if (target <= emitted) { Sleep(5); continue; }
        want = (size_t)(target - emitted);
        if (want > cap) want = cap;        /* we fell far behind; catch up gradually */

        drain_backlog(&e->mic.ring, want);
        drain_backlog(&e->sys.ring, want);

        /*
         * WASAPI hands over ~10 ms packets while this loop wakes every 5 ms,
         * so a live source is regularly a few hundred samples behind. Zero-
         * filling that gap punches digital-silence holes into the middle of
         * speech, which wrecks both the level estimate and what Whisper hears.
         * Wait for a source that is merely late; only synthesise silence for
         * one that has genuinely gone quiet - which is the normal state of a
         * loopback stream, since it delivers nothing at all while idle.
         */
        want = pace_want(e, want, now_ms());
        if (!want) { Sleep(2); continue; }

        got_a = fring_read(&e->mic.ring, a, want);
        got_b = fring_read(&e->sys.ring, b, want);
        if (got_a < want) memset(a + got_a, 0, (want - got_a) * sizeof(float));
        if (got_b < want) memset(b + got_b, 0, (want - got_b) * sizeof(float));

        EnterCriticalSection(&e->cs);
        mg = e->mic_gain;
        sg = e->sys_gain;
        LeaveCriticalSection(&e->cs);

        {
            double sa = 0.0, sb = 0.0;
            for (i = 0; i < want; ++i) { sa += (double)a[i] * a[i]; sb += (double)b[i] * b[i]; }
            if (want) {
                InterlockedExchange(&e->mic_level_millidb,
                    (LONG)(10.0 * log10(sa / (double)want + 1e-12) * 100.0));
                InterlockedExchange(&e->sys_level_millidb,
                    (LONG)(10.0 * log10(sb / (double)want + 1e-12) * 100.0));
            }
        }

        for (i = 0; i < want; ++i) {
            float v = a[i] * mg + b[i] * sg;
            if (v >  1.0f) v =  1.0f;
            if (v < -1.0f) v = -1.0f;
            mix[i] = v;
        }
        emitted += want;

        if (InterlockedCompareExchange(&e->recording, 1, 1)) {
            EnterCriticalSection(&e->cs);
            if (e->rec_len + want > e->rec_cap) {
                size_t cap = e->rec_cap ? e->rec_cap : (size_t)OUT_RATE * 8;
                size_t max = (size_t)OUT_RATE * REC_MAX_SECONDS;
                float *p;
                while (cap < e->rec_len + want) cap *= 2;
                if (cap > max) cap = max;
                p = (float *)realloc(e->rec, cap * sizeof(float));
                if (p) { e->rec = p; e->rec_cap = cap; }
            }
            if (e->rec && e->rec_len < e->rec_cap) {
                size_t room = e->rec_cap - e->rec_len;
                size_t take = want < room ? want : room;
                memcpy(e->rec + e->rec_len, mix, take * sizeof(float));
                e->rec_len += take;
            }
            LeaveCriticalSection(&e->cs);
        }

        for (i = 0; i < want; ++i) {
            frame[frame_fill++] = mix[i];
            if (frame_fill == VAD_FRAME_LEN) {
                float *seg = NULL;
                size_t seg_len = 0;
                int have;

                frame_fill = 0;
                EnterCriticalSection(&e->cs);
                have = vad_push_frame(&e->vad, frame, &seg, &seg_len);
                InterlockedExchange(&e->level_millidb, (LONG)(e->vad.last_rms_db * 100.0f));
                LeaveCriticalSection(&e->cs);

                if (have && seg) {
                    if (e->cb) e->cb(e->user, seg, seg_len);
                    else       free(seg);
                }
            }
        }
        Sleep(5);
    }

done:
    free(a); free(b); free(mix);
    return 0;
}

/* --------------------------------------------------------------- lifecycle */

audio_engine_t *audio_create(void)
{
    audio_engine_t *e = (audio_engine_t *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    InitializeCriticalSection(&e->cs);
    if (vad_init(&e->vad) != 0) {
        DeleteCriticalSection(&e->cs);
        free(e);
        return NULL;
    }
    e->mic_gain = e->sys_gain = 1.0f;
    e->level_millidb = -10000;
    e->mic_level_millidb = e->sys_level_millidb = -10000;
    return e;
}

void audio_destroy(audio_engine_t *e)
{
    if (!e) return;
    audio_stop(e);
    free(e->rec);
    vad_free(&e->vad);
    DeleteCriticalSection(&e->cs);
    free(e);
}

static int source_start(audio_engine_t *e, source_t *s, int loopback, const char *device_id)
{
    memset(&s->rs, 0, sizeof(s->rs));
    s->eng      = e;
    s->loopback = loopback;
    s->mono     = NULL;
    s->mono_cap = 0;
    s->dry_since = 0;
    snprintf(s->device_id, sizeof(s->device_id), "%s", device_id ? device_id : "");

    if (fring_init(&s->ring, (size_t)OUT_RATE * RING_SECONDS) != 0) return -1;
    InterlockedExchange(&s->run, 1);
    s->thread = CreateThread(NULL, 0, capture_thread, s, 0, NULL);
    if (!s->thread) {
        InterlockedExchange(&s->run, 0);
        fring_free(&s->ring);
        return -1;
    }
    return 0;
}

static void source_stop(source_t *s)
{
    InterlockedExchange(&s->run, 0);
    if (s->thread) {
        if (WaitForSingleObject(s->thread, 4000) == WAIT_TIMEOUT)
            LOGW("audio: capture thread did not exit in time");
        CloseHandle(s->thread);
        s->thread = NULL;
    }
    fring_free(&s->ring);
    free(s->mono);
    s->mono = NULL;
    s->mono_cap = 0;
}

int audio_start(audio_engine_t *e, const config_t *cfg, audio_segment_cb cb, void *user)
{
    if (!e || e->started) return 0;

    e->cb   = cb;
    e->user = user;
    e->err[0] = '\0';
    audio_update(e, cfg);
    vad_reset(&e->vad);

    InterlockedExchange(&e->run, 1);

    if (cfg->capture_mic && source_start(e, &e->mic, 0, cfg->mic_device_id) != 0)
        set_error(e, "could not start microphone capture");
    if (cfg->capture_loopback && source_start(e, &e->sys, 1, cfg->sys_device_id) != 0)
        set_error(e, "could not start system-audio (loopback) capture");

    e->mixer = CreateThread(NULL, 0, mixer_thread, e, 0, NULL);
    if (!e->mixer) {
        InterlockedExchange(&e->run, 0);
        source_stop(&e->mic);
        source_stop(&e->sys);
        set_error(e, "could not start mixer thread");
        return -1;
    }
    e->started = 1;
    LOGI("audio: engine started (mic=%d loopback=%d)", cfg->capture_mic, cfg->capture_loopback);
    return 0;
}

void audio_stop(audio_engine_t *e)
{
    if (!e || !e->started) return;
    InterlockedExchange(&e->run, 0);
    if (e->mixer) {
        WaitForSingleObject(e->mixer, 4000);
        CloseHandle(e->mixer);
        e->mixer = NULL;
    }
    source_stop(&e->mic);
    source_stop(&e->sys);
    e->started = 0;
    LOGI("audio: engine stopped");
}

void audio_update(audio_engine_t *e, const config_t *cfg)
{
    if (!e) return;
    EnterCriticalSection(&e->cs);
    e->mic_gain = cfg->capture_mic      ? cfg->mic_gain : 0.0f;
    e->sys_gain = cfg->capture_loopback ? cfg->sys_gain : 0.0f;
    vad_configure(&e->vad, cfg->vad_threshold_db, cfg->vad_silence_ms,
                  cfg->vad_min_speech_ms, cfg->vad_max_segment_ms,
                  cfg->vad_soft_cap_ms);
    LeaveCriticalSection(&e->cs);
}

void audio_record_start(audio_engine_t *e)
{
    if (!e) return;
    EnterCriticalSection(&e->cs);
    e->rec_len = 0;
    LeaveCriticalSection(&e->cs);
    InterlockedExchange(&e->recording, 1);
    LOGI("audio: recording started");
}

float *audio_record_stop(audio_engine_t *e, size_t *n_out)
{
    float *out = NULL;
    size_t n = 0;

    *n_out = 0;
    if (!e) return NULL;

    InterlockedExchange(&e->recording, 0);
    EnterCriticalSection(&e->cs);
    if (e->rec && e->rec_len) {
        n = e->rec_len;
        out = (float *)malloc(n * sizeof(float));
        if (out) memcpy(out, e->rec, n * sizeof(float));
        else n = 0;
    }
    e->rec_len = 0;
    LeaveCriticalSection(&e->cs);

    LOGI("audio: recording stopped (%.1fs captured)", (double)n / (double)OUT_RATE);
    *n_out = n;
    return out;
}

int audio_recording(audio_engine_t *e)
{
    return e ? (int)InterlockedCompareExchange(&e->recording, 1, 1) : 0;
}

double audio_record_seconds(audio_engine_t *e)
{
    double s;
    if (!e) return 0.0;
    EnterCriticalSection(&e->cs);
    s = (double)e->rec_len / (double)OUT_RATE;
    LeaveCriticalSection(&e->cs);
    return s;
}

float audio_level_db(audio_engine_t *e)
{
    if (!e) return -100.0f;
    return (float)InterlockedCompareExchange(&e->level_millidb, 0, 0) / 100.0f;
}

float audio_source_level_db(audio_engine_t *e, int loopback)
{
    if (!e) return -100.0f;
    return (float)InterlockedCompareExchange(
        loopback ? &e->sys_level_millidb : &e->mic_level_millidb, 0, 0) / 100.0f;
}

int audio_source_alive(audio_engine_t *e, int loopback)
{
    if (!e) return 0;
    return (int)InterlockedCompareExchange(loopback ? &e->sys.alive : &e->mic.alive, 0, 0);
}

void audio_last_error(audio_engine_t *e, char *out, size_t cap)
{
    if (!e) { if (cap) out[0] = '\0'; return; }
    EnterCriticalSection(&e->cs);
    snprintf(out, cap, "%s", e->err);
    LeaveCriticalSection(&e->cs);
}
