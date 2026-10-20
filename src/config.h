#ifndef CALLASSIST_CONFIG_H
#define CALLASSIST_CONFIG_H

#include <windows.h>

enum { STT_BACKEND_LOCAL = 0, STT_BACKEND_REMOTE = 1 };

#define CFG_PROMPT_MAX   8192
#define CFG_TEMPLATE_MAX 2048
#define CFG_URL_MAX       512
#define CFG_KEY_MAX       512
#define CFG_NAME_MAX      128

typedef struct {
    UINT mods;      /* MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN */
    UINT vk;        /* 0 = disabled */
} hotkey_t;

/* Prompts supplied by .env, as a bitmask in config_t.env_managed. A prompt
 * that is env-managed is authoritative: config.ini does not store it and the
 * admin panel shows it read-only, so there is never a second stale copy. */
enum {
    CFG_ENV_SYSTEM_PROMPT = 1 << 0,
    CFG_ENV_USER_TEMPLATE = 1 << 1,
    CFG_ENV_SNIP_PROMPT   = 1 << 2,
    CFG_ENV_STAGE1_PROMPT = 1 << 3,
    CFG_ENV_STAGE2_PROMPT = 1 << 4
};

typedef struct {
    /* ---------------- speech to text ---------------- */
    int   stt_backend;                       /* STT_BACKEND_LOCAL / _REMOTE */
    char  whisper_model[MAX_PATH];           /* ggml-*.bin */
    int   whisper_threads;
    char  whisper_language[16];              /* "auto", "en", "ru", ... */
    int   whisper_translate;                 /* translate to English */
    int   whisper_gpu;
    char  stt_endpoint[CFG_URL_MAX];         /* .../v1/audio/transcriptions */
    char  stt_api_key[CFG_KEY_MAX];
    char  stt_model[CFG_NAME_MAX];           /* whisper-1 */

    /* ---------------- llm ---------------- */
    char   llm_endpoint[CFG_URL_MAX];        /* .../v1/chat/completions */
    char   llm_api_key[CFG_KEY_MAX];
    char   llm_model[CFG_NAME_MAX];
    char   system_prompt[CFG_PROMPT_MAX];
    char   user_template[CFG_TEMPLATE_MAX];  /* {transcript} {last} {time} */
    char   snip_prompt[CFG_TEMPLATE_MAX];    /* sent alongside a screenshot */
    int    two_stage;                        /* answer first, details after */
    char   stage1_prompt[CFG_TEMPLATE_MAX];  /* "ANSWER: only" instruction */
    char   stage2_prompt[CFG_TEMPLATE_MAX];  /* "now CONTEXT: and BONUS:" */
    double temperature;
    int    max_tokens;
    int    context_seconds;                  /* how much transcript to send */
    int    llm_stream;                       /* SSE streaming on/off */

    /* ---------------- audio ---------------- */
    int   capture_mic;
    int   capture_loopback;
    char  mic_device_id[512];                /* empty = default endpoint */
    char  sys_device_id[512];
    float mic_gain;
    float sys_gain;
    int   vad_threshold_db;                  /* dBFS, e.g. -42 */
    int   vad_silence_ms;                    /* end-of-utterance hangover */
    int   vad_min_speech_ms;
    int   vad_max_segment_ms;
    int   vad_soft_cap_ms;                   /* chunk continuous speech */

    /* ---------------- triggering ---------------- */
    int   listening;                         /* start listening at launch */
    int   auto_respond;
    int   auto_min_chars;
    int   auto_cooldown_ms;
    int   auto_require_question;
    int   push_to_talk;                      /* hotkey records; answer on stop */
    int   trigger_mouse_button;              /* 0 none, 1 middle, 2 X1, 3 X2 */

    hotkey_t hk_respond;
    hotkey_t hk_toggle_overlay;
    hotkey_t hk_clear;
    hotkey_t hk_toggle_listen;
    hotkey_t hk_snip;                        /* grab a region and ask about it */
    hotkey_t hk_scroll_up;                   /* the overlay is click-through, so */
    hotkey_t hk_scroll_down;                 /* the wheel never reaches it */
    hotkey_t hk_pane_next;
    hotkey_t hk_move_overlay;                /* drag/resize the overlay */

    /* ---------------- overlay ---------------- */
    int      overlay_x, overlay_y, overlay_w, overlay_h;
    int      overlay_opacity;                /* 16..255 */
    int      overlay_font_size;              /* points */
    int      overlay_click_through;
    int      overlay_hide_from_capture;
    int      overlay_show_transcript;
    int      overlay_visible;
    COLORREF overlay_bg;
    COLORREF overlay_fg;
    COLORREF overlay_accent;

    /* ---------------- provenance (not persisted) ---------------- */
    unsigned env_managed;                    /* CFG_ENV_* bits from .env */
} config_t;

void config_defaults(config_t *c);
int  config_load(config_t *c);               /* 0 = loaded, 1 = defaults used */
int  config_save(const config_t *c);         /* 0 = ok */
int  config_path(char *out, size_t out_sz);
/* Path of the .env that would be used: next to the .exe first, then
 * %APPDATA%\CallAssist\.env. Returns 0 when one exists, -1 when none does
 * (out still receives the preferred location to create). */
int  config_env_path(char *out, size_t out_sz);

#endif
