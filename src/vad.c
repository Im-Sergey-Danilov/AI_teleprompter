#include "vad.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PRE_ROLL_MS 320
#define PRE_FRAMES  (PRE_ROLL_MS / VAD_FRAME_MS)

int vad_init(vad_t *v)
{
    memset(v, 0, sizeof(*v));
    v->threshold_db   = -42;
    v->silence_ms     = 700;
    v->min_speech_ms  = 240;
    v->max_segment_ms = 20000;
    v->soft_cap_ms    = 2500;
    v->pre_frames     = PRE_FRAMES;
    v->pre = (float *)calloc(v->pre_frames * VAD_FRAME_LEN, sizeof(float));
    if (!v->pre) return -1;
    v->last_rms_db = -100.0f;
    v->peak_db     = -100.0f;
    v->noise_db    = -80.0f;
    return 0;
}

void vad_free(vad_t *v)
{
    free(v->seg);
    free(v->pre);
    memset(v, 0, sizeof(*v));
}

void vad_configure(vad_t *v, int threshold_db, int silence_ms, int min_speech_ms,
                   int max_segment_ms, int soft_cap_ms)
{
    v->threshold_db   = threshold_db;
    v->silence_ms     = silence_ms   > 100  ? silence_ms   : 100;
    v->min_speech_ms  = min_speech_ms > 40  ? min_speech_ms : 40;
    v->max_segment_ms = max_segment_ms > 2000 ? max_segment_ms : 2000;
    v->soft_cap_ms    = soft_cap_ms > VAD_SOFT_CAP_MIN_MS ? soft_cap_ms
                                                         : VAD_SOFT_CAP_MIN_MS;
}

void vad_reset(vad_t *v)
{
    v->in_speech = 0;
    v->speech_ms = 0;
    v->silence_run_ms = 0;
    v->seg_len = 0;
    v->pre_pos = v->pre_filled = 0;
    v->noise_db = -80.0f;
    v->quiet_db = 1e9f;
    v->quiet_pos = 0;
}

/*
 * A fixed dBFS gate cannot segment a live call: anything with a continuous
 * bed - music, line noise, the far end's comfort noise - stays above it
 * forever, so the utterance never closes and the only thing that ever fires
 * is the max-segment cap. Track the background instead and put the gate a
 * fixed margin above it; the configured threshold stays as an absolute floor
 * so a quiet room still cannot trigger on nothing.
 */
float vad_gate_db(const vad_t *v)
{
    float adaptive = v->noise_db + VAD_MARGIN_DB;
    float floor_db = (float)v->threshold_db;
    return adaptive > floor_db ? adaptive : floor_db;
}

static void track_noise(vad_t *v, float db)
{
    /* Fall fast towards a new quiet level, crawl back up, so the estimate
     * settles on the floor of the signal rather than its average. */
    float a = (db < v->noise_db) ? 0.35f : 0.0005f;
    v->noise_db += a * (db - v->noise_db);
    if (v->noise_db < -120.0f) v->noise_db = -120.0f;
}

static int seg_append(vad_t *v, const float *s, size_t n)
{
    if (v->seg_len + n > v->seg_cap) {
        size_t cap = v->seg_cap ? v->seg_cap : VAD_SAMPLE_RATE * 4;
        float *p;
        while (cap < v->seg_len + n) cap *= 2;
        p = (float *)realloc(v->seg, cap * sizeof(float));
        if (!p) return 0;
        v->seg = p;
        v->seg_cap = cap;
    }
    memcpy(v->seg + v->seg_len, s, n * sizeof(float));
    v->seg_len += n;
    return 1;
}

static void pre_push(vad_t *v, const float *frame)
{
    memcpy(v->pre + v->pre_pos * VAD_FRAME_LEN, frame, VAD_FRAME_LEN * sizeof(float));
    v->pre_pos = (v->pre_pos + 1) % v->pre_frames;
    if (v->pre_filled < v->pre_frames) v->pre_filled++;
}

static void pre_flush(vad_t *v)
{
    size_t i;
    size_t start = (v->pre_pos + v->pre_frames - v->pre_filled) % v->pre_frames;
    for (i = 0; i < v->pre_filled; ++i) {
        size_t idx = (start + i) % v->pre_frames;
        seg_append(v, v->pre + idx * VAD_FRAME_LEN, VAD_FRAME_LEN);
    }
    v->pre_filled = 0;
}

/*
 * Emits the first cut_len samples of the segment and shifts whatever follows
 * down to the front, so a long utterance streams out in pieces without losing
 * the audio that straddles the cut.
 */
static int emit_at(vad_t *v, float **seg_out, size_t *seg_len_out, size_t cut_len)
{
    float *copy;
    size_t rest;

    if (cut_len > v->seg_len) cut_len = v->seg_len;
    if (cut_len < (size_t)(VAD_SAMPLE_RATE * v->min_speech_ms / 1000)) return 0;

    copy = (float *)malloc(cut_len * sizeof(float));
    if (!copy) return 0;
    memcpy(copy, v->seg, cut_len * sizeof(float));

    rest = v->seg_len - cut_len;
    if (rest) memmove(v->seg, v->seg + cut_len, rest * sizeof(float));
    v->seg_len = rest;

    v->quiet_db  = 1e9f;
    v->quiet_pos = 0;

    *seg_out     = copy;
    *seg_len_out = cut_len;
    return 1;
}

static int emit(vad_t *v, float **seg_out, size_t *seg_len_out, size_t trim_samples)
{
    size_t len = v->seg_len;
    float *copy;

    if (len > trim_samples) len -= trim_samples;
    if (len < (size_t)(VAD_SAMPLE_RATE * v->min_speech_ms / 1000)) {
        v->seg_len = 0;
        return 0;
    }
    copy = (float *)malloc(len * sizeof(float));
    if (!copy) { v->seg_len = 0; return 0; }
    memcpy(copy, v->seg, len * sizeof(float));

    *seg_out     = copy;
    *seg_len_out = len;
    v->seg_len   = 0;
    v->quiet_db  = 1e9f;
    v->quiet_pos = 0;
    return 1;
}

int vad_push_frame(vad_t *v, const float *frame, float **seg_out, size_t *seg_len_out)
{
    double acc = 0.0;
    int i, is_speech;
    float db;

    for (i = 0; i < VAD_FRAME_LEN; ++i) acc += (double)frame[i] * (double)frame[i];
    db = (float)(10.0 * log10(acc / VAD_FRAME_LEN + 1e-12));

    v->last_rms_db = db;
    if (db > v->peak_db) v->peak_db = db;
    else                 v->peak_db -= 0.5f;          /* slow decay for the meter */

    track_noise(v, db);
    {
        float gate = vad_gate_db(v);
        /* Once open, hold the utterance until the level drops clearly below
         * the gate - otherwise every dip between syllables cuts a segment. */
        is_speech = v->in_speech ? (db > gate - VAD_HYSTERESIS_DB) : (db > gate);
    }

    if (!v->in_speech) {
        pre_push(v, frame);
        if (is_speech) {
            v->speech_ms += VAD_FRAME_MS;
            if (v->speech_ms >= v->min_speech_ms) {
                v->in_speech      = 1;
                v->silence_run_ms = 0;
                v->seg_len        = 0;
                pre_flush(v);                          /* keep the word onset */
            }
        } else {
            v->speech_ms = 0;
        }
        return 0;
    }

    seg_append(v, frame, VAD_FRAME_LEN);

    if (is_speech) {
        v->silence_run_ms = 0;
    } else {
        v->silence_run_ms += VAD_FRAME_MS;
        if (v->silence_run_ms >= v->silence_ms) {
            /* keep ~200 ms of the trailing pause, drop the rest */
            size_t keep = (size_t)(VAD_SAMPLE_RATE * 200 / 1000);
            size_t tail = (size_t)((long long)VAD_SAMPLE_RATE * v->silence_run_ms / 1000);
            size_t trim = tail > keep ? tail - keep : 0;
            v->in_speech = 0;
            v->speech_ms = 0;
            v->silence_run_ms = 0;
            return emit(v, seg_out, seg_len_out, trim);
        }
    }

    {
        size_t soft = (size_t)((long long)VAD_SAMPLE_RATE * v->soft_cap_ms / 1000);
        size_t hard = (size_t)((long long)VAD_SAMPLE_RATE * v->max_segment_ms / 1000);
        size_t half = soft / 2;
        size_t least = (size_t)(VAD_SAMPLE_RATE * v->min_speech_ms / 1000);
        if (half < least) half = least;

        if (soft > hard) soft = hard;

        /* Past the halfway mark, remember the quietest frame boundary; it is
         * the least damaging place to cut if the talking never stops. */
        if (v->seg_len >= half && db < v->quiet_db) {
            v->quiet_db  = db;
            v->quiet_pos = v->seg_len;
        }

        if (v->seg_len >= soft && v->quiet_pos) {
            /* Stay in speech: a monologue is cut into pieces, not truncated
             * and then re-acquired 240 ms later with the gap thrown away. */
            v->silence_run_ms = 0;
            if (emit_at(v, seg_out, seg_len_out, v->quiet_pos)) return 1;
        }
        if (v->seg_len >= hard) {
            v->silence_run_ms = 0;
            return emit(v, seg_out, seg_len_out, 0);
        }
    }
    return 0;
}
