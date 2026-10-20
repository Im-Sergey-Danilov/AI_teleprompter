#ifndef CALLASSIST_VAD_H
#define CALLASSIST_VAD_H

#include <stddef.h>

#define VAD_SAMPLE_RATE 16000
#define VAD_FRAME_MS    20
#define VAD_FRAME_LEN   (VAD_SAMPLE_RATE * VAD_FRAME_MS / 1000)   /* 320 */

/* How far above the tracked noise floor a frame must sit to count as speech,
 * and how much lower it may fall before the utterance is closed again. The
 * hysteresis stops a single quiet syllable from ending the segment. */
#define VAD_MARGIN_DB      9.0f
#define VAD_HYSTERESIS_DB  4.0f

/* Continuous audio - music, a talkative far end - may never fall below the
 * gate. Rather than hand Whisper one max-length blob, cut at the quietest
 * moment once the utterance has run this long. This is the dominant source of
 * end-to-end latency whenever the speaker does not pause, so it is a setting
 * rather than a constant: transcription itself costs ~0.2 s. */
#define VAD_SOFT_CAP_MIN_MS 600

typedef struct {
    int   threshold_db;      /* absolute floor: never open below this */
    int   silence_ms;
    int   min_speech_ms;
    int   max_segment_ms;
    int   soft_cap_ms;       /* cut continuous speech after this long */

    int   in_speech;
    int   speech_ms;
    int   silence_run_ms;

    float *seg;              /* accumulating segment (incl. pre-roll) */
    size_t seg_len, seg_cap;

    float *pre;              /* pre-roll ring, PRE_FRAMES frames */
    size_t pre_frames, pre_pos, pre_filled;

    float  last_rms_db;      /* for the UI level meter */
    float  peak_db;
    float  noise_db;         /* tracked background level */
    float  quiet_db;         /* quietest frame seen since the soft-cut point */
    size_t quiet_pos;        /* ... and where it ended, as a seg offset */
} vad_t;

int  vad_init(vad_t *v);
void vad_free(vad_t *v);
void vad_configure(vad_t *v, int threshold_db, int silence_ms, int min_speech_ms,
                  int max_segment_ms, int soft_cap_ms);
void vad_reset(vad_t *v);

/* The level a frame must currently exceed to open an utterance. */
float vad_gate_db(const vad_t *v);

/*
 * Feeds exactly VAD_FRAME_LEN samples. When an utterance ends, returns 1 and
 * hands ownership of a malloc'd buffer via *seg_out / *seg_len_out.
 */
int  vad_push_frame(vad_t *v, const float *frame, float **seg_out, size_t *seg_len_out);

#endif
