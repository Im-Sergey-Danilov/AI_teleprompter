#include "config.h"
#include "dotenv.h"
#include "util.h"
#include "log.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Built-in fallbacks only. The prompts actually in use come from .env - see
 * .env.example for the keys and the file format. These exist so a build with
 * no .env still starts and still produces the ANSWER:/CONTEXT:/BONUS: sections
 * that main.c routes into the overlay panes; keep that contract in any prompt.
 */
#define DEFAULT_SYSTEM_PROMPT \
    "You assist someone during a live call. You receive the running " \
    "transcript; the microphone and the system audio are mixed into one " \
    "stream, so there are no speaker labels - work out from context who is " \
    "talking.\n\n" \
    "Answer strictly in this shape, each marker at the start of its own " \
    "line:\n\n" \
    "ANSWER:\n" \
    "what to say right now. As short as possible - a phrase beats a " \
    "sentence. Straight to the point, no preamble, no restating the " \
    "question.\n\n" \
    "CONTEXT:\n" \
    "a few facts on the topic that may be needed: numbers, names, " \
    "definitions. Short lines, no filler.\n\n" \
    "BONUS:\n" \
    "one point that genuinely adds something. If it is merely correct " \
    "rather than strong, put a dash.\n\n" \
    "Never write anything outside these three sections. If there is nothing " \
    "to say yet, put a dash under each marker."

#define DEFAULT_SNIP_PROMPT \
    "This is my screen right now. It usually holds a task or a question, " \
    "often about code: what it does, what it prints, where the bug is, how " \
    "to fix it.\n\n" \
    "Solve it or answer it. If it is code, say what it does and give a " \
    "concrete answer: the result, the complexity, the bug, the fix. Do not " \
    "describe the picture. If code is needed, give only the code.\n\n" \
    "Use exactly the same three markers, each at the start of a line: " \
    "ANSWER:, then CONTEXT:, then BONUS:. Nothing outside them."

#define DEFAULT_STAGE1_PROMPT \
    "Only the quick answer is needed now. Output ONLY the ANSWER: section " \
    "and nothing else - no CONTEXT: and no BONUS:."

#define DEFAULT_STAGE2_PROMPT \
    "The short answer is already given above. Now output ONLY the CONTEXT: " \
    "and BONUS: sections. Do not repeat the ANSWER: section."

#define DEFAULT_USER_TEMPLATE \
    "Transcript (oldest first):\n{transcript}\n\nMost recent line: {last}\n\nRespond now."

int config_path(char *out, size_t out_sz)
{
    return app_data_path("config.ini", out, out_sz);
}

static int file_exists(const char *path)
{
    wchar_t *w = utf8_to_wide(path);
    DWORD    a;
    if (!w) return 0;
    a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int config_env_path(char *out, size_t out_sz)
{
    char beside[1024] = "";

    /* Next to the .exe wins: it keeps a portable copy self-contained, and it
     * is where someone unpacking a release will look first. */
    if (exe_dir_path(".env", beside, sizeof(beside)) == 0) {
        if (file_exists(beside)) {
            snprintf(out, out_sz, "%s", beside);
            return 0;
        }
    }
    if (app_data_path(".env", out, out_sz) == 0 && file_exists(out))
        return 0;

    /* Nothing there yet - hand back the preferred location to create. */
    if (beside[0]) snprintf(out, out_sz, "%s", beside);
    return -1;
}

/* Reads a whole file as bytes. Returns malloc'd, NUL-terminated text. */
static char *read_text_file(const char *path)
{
    wchar_t *wpath;
    FILE    *f;
    char    *buf;
    long     n;
    size_t   got;

    wpath = utf8_to_wide(path);
    if (!wpath) return NULL;
    f = _wfopen(wpath, L"rb");
    free(wpath);
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0 || n > 4 * 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);

    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/*
 * .env is the source of truth for prompts, so this runs last - after both the
 * built-in defaults and config.ini. A key present but empty is treated as
 * "not set" rather than "blank prompt", which would leave the model with no
 * instructions at all.
 */
static void apply_dotenv(config_t *c)
{
    char       path[1024];
    char      *text;
    dotenv_t  *e;
    size_t     i;
    int        applied = 0;

    static const struct {
        const char *key;
        size_t      offset;
        size_t      cap;
        unsigned    bit;
    } map[] = {
        { "CALLASSIST_SYSTEM_PROMPT", offsetof(config_t, system_prompt),
          CFG_PROMPT_MAX,   CFG_ENV_SYSTEM_PROMPT },
        { "CALLASSIST_USER_TEMPLATE", offsetof(config_t, user_template),
          CFG_TEMPLATE_MAX, CFG_ENV_USER_TEMPLATE },
        { "CALLASSIST_SNIP_PROMPT",   offsetof(config_t, snip_prompt),
          CFG_TEMPLATE_MAX, CFG_ENV_SNIP_PROMPT   },
        { "CALLASSIST_STAGE1_PROMPT", offsetof(config_t, stage1_prompt),
          CFG_TEMPLATE_MAX, CFG_ENV_STAGE1_PROMPT },
        { "CALLASSIST_STAGE2_PROMPT", offsetof(config_t, stage2_prompt),
          CFG_TEMPLATE_MAX, CFG_ENV_STAGE2_PROMPT }
    };

    if (config_env_path(path, sizeof(path)) != 0) {
        LOGI("dotenv: no .env found, prompts come from defaults and config.ini");
        return;
    }

    text = read_text_file(path);
    if (!text) { LOGE("dotenv: cannot read %s", path); return; }

    e = dotenv_parse(text);
    free(text);
    if (!e) { LOGE("dotenv: out of memory parsing %s", path); return; }

    for (i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        const char *v = dotenv_get(e, map[i].key);
        char       *dst;
        if (!v || !*v) continue;
        dst = (char *)c + map[i].offset;
        snprintf(dst, map[i].cap, "%s", v);
        c->env_managed |= map[i].bit;
        applied++;
    }

    LOGI("dotenv: %s - %u key(s) read, %d prompt(s) applied",
         path, (unsigned)dotenv_count(e), applied);
    dotenv_free(e);
}

void config_defaults(config_t *c)
{
    memset(c, 0, sizeof(*c));

    c->stt_backend      = STT_BACKEND_LOCAL;
    c->whisper_model[0] = '\0';
    c->whisper_threads  = 4;
    strcpy(c->whisper_language, "ru");
    c->whisper_translate = 0;
    c->whisper_gpu       = 1;
    strcpy(c->stt_endpoint, "https://api.openai.com/v1/audio/transcriptions");
    strcpy(c->stt_model,    "whisper-1");

    strcpy(c->llm_endpoint, "https://api.openai.com/v1/chat/completions");
    strcpy(c->llm_model,    "gpt-4o-mini");
    strcpy(c->system_prompt, DEFAULT_SYSTEM_PROMPT);
    strcpy(c->user_template, DEFAULT_USER_TEMPLATE);
    strcpy(c->snip_prompt, DEFAULT_SNIP_PROMPT);
    strcpy(c->stage1_prompt, DEFAULT_STAGE1_PROMPT);
    strcpy(c->stage2_prompt, DEFAULT_STAGE2_PROMPT);
    c->two_stage = 1;
    /* Omitted unless the user fills the boxes in: it keeps the request minimal
     * and several gateways reject parameters their model no longer accepts. */
    c->temperature     = -1.0;
    c->max_tokens      = 0;
    c->context_seconds = 120;
    c->llm_stream      = 1;

    c->capture_mic        = 1;
    c->capture_loopback   = 1;
    c->mic_device_id[0]   = '\0';
    c->sys_device_id[0]   = '\0';
    c->mic_gain           = 1.0f;
    c->sys_gain           = 1.0f;
    c->vad_threshold_db   = -42;
    c->vad_silence_ms     = 400;
    c->vad_min_speech_ms  = 240;
    c->vad_max_segment_ms = 20000;
    c->vad_soft_cap_ms    = 2500;

    c->listening             = 0;   /* boot idle; the hotkey starts it */
    c->auto_respond          = 1;
    c->auto_min_chars        = 12;
    c->auto_cooldown_ms      = 2500;
    c->auto_require_question = 0;
    c->push_to_talk          = 1;
    c->trigger_mouse_button  = 0;

    c->hk_respond.mods        = MOD_CONTROL | MOD_ALT; c->hk_respond.vk        = VK_SPACE;
    c->hk_toggle_overlay.mods = MOD_CONTROL | MOD_ALT; c->hk_toggle_overlay.vk = 'H';
    c->hk_clear.mods          = MOD_CONTROL | MOD_ALT; c->hk_clear.vk          = 'X';
    c->hk_toggle_listen.mods  = MOD_CONTROL | MOD_ALT; c->hk_toggle_listen.vk  = '1';
    c->hk_snip.mods           = MOD_CONTROL | MOD_ALT; c->hk_snip.vk           = 'S';
    c->hk_scroll_up.mods      = MOD_CONTROL | MOD_ALT; c->hk_scroll_up.vk      = VK_UP;
    c->hk_scroll_down.mods    = MOD_CONTROL | MOD_ALT; c->hk_scroll_down.vk    = VK_DOWN;
    c->hk_pane_next.mods      = MOD_CONTROL | MOD_ALT; c->hk_pane_next.vk      = VK_RIGHT;
    c->hk_move_overlay.mods   = MOD_CONTROL | MOD_ALT; c->hk_move_overlay.vk   = 'M';

    /* Right half of the primary screen: big enough to read at a glance during
     * a call without covering what is being shared. */
    {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        if (sw < 800)  sw = 1920;
        if (sh < 600)  sh = 1080;
        c->overlay_w = sw / 2 - 40;
        c->overlay_h = sh - 120;
        c->overlay_x = sw - c->overlay_w - 20;
        c->overlay_y = 60;
    }
    c->overlay_opacity            = 225;
    c->overlay_font_size          = 16;
    c->overlay_click_through      = 1;
    c->overlay_hide_from_capture  = 0;
    c->overlay_show_transcript    = 1;
    c->overlay_visible            = 1;
    c->overlay_bg     = RGB(16, 18, 24);
    c->overlay_fg     = RGB(233, 236, 242);
    c->overlay_accent = RGB(120, 180, 255);
}

/* ------------------------------------------------------------------ ini I/O */

static void put_str(const char *key, const char *val, FILE *f)
{
    const char *p;
    fprintf(f, "%s=", key);
    for (p = val; *p; ++p) {
        if      (*p == '\n') fputs("\\n", f);
        else if (*p == '\r') ;                       /* dropped */
        else if (*p == '\\') fputs("\\\\", f);
        else                 fputc(*p, f);
    }
    fputc('\n', f);
}

static void unescape(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '\\' && r[1]) {
            r++;
            switch (*r) {
            case 'n': *w++ = '\n'; break;
            case 't': *w++ = '\t'; break;
            default:  *w++ = *r;   break;
            }
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void copy_field(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

static void hotkey_to_str(const hotkey_t *h, char *out, size_t cap)
{
    snprintf(out, cap, "%u:%u", h->mods, h->vk);
}

static void hotkey_from_str(hotkey_t *h, const char *s)
{
    unsigned m = 0, v = 0;
    if (sscanf(s, "%u:%u", &m, &v) == 2) { h->mods = m; h->vk = v; }
}

int config_save(const config_t *c)
{
    char path[1024];
    wchar_t *wpath;
    FILE *f;
    char tmp[64];
    char *enc;

    if (config_path(path, sizeof(path)) != 0) return -1;
    wpath = utf8_to_wide(path);
    if (!wpath) return -1;
    f = _wfopen(wpath, L"w");   /* byte-oriented; we write UTF-8 ourselves */
    free(wpath);
    if (!f) { LOGE("config_save: cannot open %s", path); return -1; }

    fputs("\xEF\xBB\xBF; call_assist configuration (UTF-8)\n[stt]\n", f);
    fprintf(f, "backend=%d\n", c->stt_backend);
    put_str("whisper_model", c->whisper_model, f);
    fprintf(f, "whisper_threads=%d\n", c->whisper_threads);
    put_str("whisper_language", c->whisper_language, f);
    fprintf(f, "whisper_translate=%d\n", c->whisper_translate);
    fprintf(f, "whisper_gpu=%d\n", c->whisper_gpu);
    put_str("endpoint", c->stt_endpoint, f);
    put_str("model", c->stt_model, f);
    enc = secret_encrypt_hex(c->stt_api_key);
    put_str("api_key_enc", enc ? enc : "", f);
    free(enc);

    fputs("\n[llm]\n", f);
    put_str("endpoint", c->llm_endpoint, f);
    put_str("model", c->llm_model, f);
    enc = secret_encrypt_hex(c->llm_api_key);
    put_str("api_key_enc", enc ? enc : "", f);
    free(enc);
    /* A prompt that came from .env is not mirrored here. .env is the source of
     * truth for prompts, and a second copy in config.ini would quietly go
     * stale the moment .env was edited. */
#define PUT_PROMPT(bit, name, field)                                          \
    do {                                                                      \
        if (c->env_managed & (bit)) fputs("; " name " comes from .env\n", f); \
        else                        put_str(name, c->field, f);               \
    } while (0)
    PUT_PROMPT(CFG_ENV_SYSTEM_PROMPT, "system_prompt", system_prompt);
    PUT_PROMPT(CFG_ENV_USER_TEMPLATE, "user_template", user_template);
    PUT_PROMPT(CFG_ENV_SNIP_PROMPT,   "snip_prompt",   snip_prompt);
    PUT_PROMPT(CFG_ENV_STAGE1_PROMPT, "stage1_prompt", stage1_prompt);
    PUT_PROMPT(CFG_ENV_STAGE2_PROMPT, "stage2_prompt", stage2_prompt);
#undef PUT_PROMPT
    fprintf(f, "two_stage=%d\n", c->two_stage);
    fprintf(f, "temperature=%.3f\n", c->temperature);
    fprintf(f, "max_tokens=%d\n", c->max_tokens);
    fprintf(f, "context_seconds=%d\n", c->context_seconds);
    fprintf(f, "stream=%d\n", c->llm_stream);

    fputs("\n[audio]\n", f);
    fprintf(f, "capture_mic=%d\n", c->capture_mic);
    fprintf(f, "capture_loopback=%d\n", c->capture_loopback);
    put_str("mic_device_id", c->mic_device_id, f);
    put_str("sys_device_id", c->sys_device_id, f);
    fprintf(f, "mic_gain=%.3f\n", (double)c->mic_gain);
    fprintf(f, "sys_gain=%.3f\n", (double)c->sys_gain);
    fprintf(f, "vad_threshold_db=%d\n", c->vad_threshold_db);
    fprintf(f, "vad_silence_ms=%d\n", c->vad_silence_ms);
    fprintf(f, "vad_min_speech_ms=%d\n", c->vad_min_speech_ms);
    fprintf(f, "vad_max_segment_ms=%d\n", c->vad_max_segment_ms);

    fputs("\n[trigger]\n", f);
    fprintf(f, "listening=%d\n", c->listening);
    fprintf(f, "auto_respond=%d\n", c->auto_respond);
    fprintf(f, "auto_min_chars=%d\n", c->auto_min_chars);
    fprintf(f, "auto_cooldown_ms=%d\n", c->auto_cooldown_ms);
    fprintf(f, "auto_require_question=%d\n", c->auto_require_question);
    hotkey_to_str(&c->hk_respond, tmp, sizeof(tmp));        put_str("hk_respond", tmp, f);
    hotkey_to_str(&c->hk_toggle_overlay, tmp, sizeof(tmp)); put_str("hk_toggle_overlay", tmp, f);
    hotkey_to_str(&c->hk_clear, tmp, sizeof(tmp));          put_str("hk_clear", tmp, f);
    hotkey_to_str(&c->hk_toggle_listen, tmp, sizeof(tmp));  put_str("hk_toggle_listen", tmp, f);
    hotkey_to_str(&c->hk_snip, tmp, sizeof(tmp));           put_str("hk_snip", tmp, f);
    hotkey_to_str(&c->hk_scroll_up, tmp, sizeof(tmp));      put_str("hk_scroll_up", tmp, f);
    hotkey_to_str(&c->hk_scroll_down, tmp, sizeof(tmp));    put_str("hk_scroll_down", tmp, f);
    hotkey_to_str(&c->hk_pane_next, tmp, sizeof(tmp));      put_str("hk_pane_next", tmp, f);
    hotkey_to_str(&c->hk_move_overlay, tmp, sizeof(tmp));   put_str("hk_move_overlay", tmp, f);

    fputs("\n[overlay]\n", f);
    fprintf(f, "x=%d\ny=%d\nw=%d\nh=%d\n", c->overlay_x, c->overlay_y, c->overlay_w, c->overlay_h);
    fprintf(f, "opacity=%d\n", c->overlay_opacity);
    fprintf(f, "font_size=%d\n", c->overlay_font_size);
    fprintf(f, "click_through=%d\n", c->overlay_click_through);
    fprintf(f, "hide_from_capture=%d\n", c->overlay_hide_from_capture);
    fprintf(f, "show_transcript=%d\n", c->overlay_show_transcript);
    fprintf(f, "visible=%d\n", c->overlay_visible);
    fprintf(f, "bg=%06lX\n",     (unsigned long)(c->overlay_bg     & 0xFFFFFF));
    fprintf(f, "fg=%06lX\n",     (unsigned long)(c->overlay_fg     & 0xFFFFFF));
    fprintf(f, "accent=%06lX\n", (unsigned long)(c->overlay_accent & 0xFFFFFF));

    fclose(f);
    return 0;
}

static int config_load_ini(config_t *c)
{
    char path[1024];
    wchar_t *wpath;
    FILE *f;
    char line_buf[CFG_PROMPT_MAX];
    char section[32] = "";
    int  first_line = 1;

    config_defaults(c);

    if (config_path(path, sizeof(path)) != 0) return 1;
    wpath = utf8_to_wide(path);
    if (!wpath) return 1;
    f = _wfopen(wpath, L"r");
    free(wpath);
    if (!f) { LOGI("config: no file at %s, using defaults", path); return 1; }

    while (fgets(line_buf, (int)sizeof(line_buf), f)) {
        char *line = line_buf;
        char *eq, *key, *val;

        if (first_line) {                       /* skip a UTF-8 BOM */
            first_line = 0;
            if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF)
                line += 3;
        }
        str_trim(line);

        if (!*line || *line == ';' || *line == '#') continue;
        if (*line == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", line + 1);
            }
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        key = line;
        val = eq + 1;
        str_trim(key);
        unescape(val);

#define IS(sec, k) (strcmp(section, sec) == 0 && strcmp(key, k) == 0)
        if      (IS("stt", "backend"))            c->stt_backend = atoi(val);
        else if (IS("stt", "whisper_model"))      copy_field(c->whisper_model, sizeof(c->whisper_model), val);
        else if (IS("stt", "whisper_threads"))    c->whisper_threads = atoi(val);
        else if (IS("stt", "whisper_language"))   copy_field(c->whisper_language, sizeof(c->whisper_language), val);
        else if (IS("stt", "whisper_translate"))  c->whisper_translate = atoi(val);
        else if (IS("stt", "whisper_gpu"))        c->whisper_gpu = atoi(val);
        else if (IS("stt", "endpoint"))           copy_field(c->stt_endpoint, sizeof(c->stt_endpoint), val);
        else if (IS("stt", "model"))              copy_field(c->stt_model, sizeof(c->stt_model), val);
        else if (IS("stt", "api_key"))            copy_field(c->stt_api_key, sizeof(c->stt_api_key), val);
        else if (IS("stt", "api_key_enc")) {
            char *p = secret_decrypt_hex(val);
            if (p) { copy_field(c->stt_api_key, sizeof(c->stt_api_key), p); free(p); }
        }
        else if (IS("llm", "endpoint"))           copy_field(c->llm_endpoint, sizeof(c->llm_endpoint), val);
        else if (IS("llm", "model"))              copy_field(c->llm_model, sizeof(c->llm_model), val);
        else if (IS("llm", "api_key"))            copy_field(c->llm_api_key, sizeof(c->llm_api_key), val);
        else if (IS("llm", "api_key_enc")) {
            char *p = secret_decrypt_hex(val);
            if (p) { copy_field(c->llm_api_key, sizeof(c->llm_api_key), p); free(p); }
        }
        else if (IS("llm", "system_prompt"))      copy_field(c->system_prompt, sizeof(c->system_prompt), val);
        else if (IS("llm", "user_template"))      copy_field(c->user_template, sizeof(c->user_template), val);
        else if (IS("llm", "snip_prompt"))        copy_field(c->snip_prompt, sizeof(c->snip_prompt), val);
        else if (IS("llm", "stage1_prompt"))      copy_field(c->stage1_prompt, sizeof(c->stage1_prompt), val);
        else if (IS("llm", "stage2_prompt"))      copy_field(c->stage2_prompt, sizeof(c->stage2_prompt), val);
        else if (IS("llm", "two_stage"))          c->two_stage = atoi(val);
        else if (IS("llm", "temperature"))        c->temperature = atof(val);
        else if (IS("llm", "max_tokens"))         c->max_tokens = atoi(val);
        else if (IS("llm", "context_seconds"))    c->context_seconds = atoi(val);
        else if (IS("llm", "stream"))             c->llm_stream = atoi(val);
        else if (IS("audio", "capture_mic"))      c->capture_mic = atoi(val);
        else if (IS("audio", "capture_loopback")) c->capture_loopback = atoi(val);
        else if (IS("audio", "mic_device_id")) copy_field(c->mic_device_id, sizeof(c->mic_device_id), val);
        else if (IS("audio", "sys_device_id")) copy_field(c->sys_device_id, sizeof(c->sys_device_id), val);
        else if (IS("audio", "mic_gain"))         c->mic_gain = (float)atof(val);
        else if (IS("audio", "sys_gain"))         c->sys_gain = (float)atof(val);
        else if (IS("audio", "vad_threshold_db")) c->vad_threshold_db = atoi(val);
        else if (IS("audio", "vad_silence_ms"))   c->vad_silence_ms = atoi(val);
        else if (IS("audio", "vad_min_speech_ms"))  c->vad_min_speech_ms = atoi(val);
        else if (IS("audio", "vad_max_segment_ms")) c->vad_max_segment_ms = atoi(val);
        else if (IS("audio", "vad_soft_cap_ms"))   c->vad_soft_cap_ms = atoi(val);
        else if (IS("trigger", "listening"))      c->listening = atoi(val);
        else if (IS("trigger", "auto_respond"))   c->auto_respond = atoi(val);
        else if (IS("trigger", "auto_min_chars")) c->auto_min_chars = atoi(val);
        else if (IS("trigger", "auto_cooldown_ms")) c->auto_cooldown_ms = atoi(val);
        else if (IS("trigger", "auto_require_question")) c->auto_require_question = atoi(val);
        else if (IS("trigger", "push_to_talk"))    c->push_to_talk = atoi(val);
        else if (IS("trigger", "trigger_mouse_button")) c->trigger_mouse_button = atoi(val);
        else if (IS("trigger", "hk_respond"))        hotkey_from_str(&c->hk_respond, val);
        else if (IS("trigger", "hk_toggle_overlay")) hotkey_from_str(&c->hk_toggle_overlay, val);
        else if (IS("trigger", "hk_clear"))          hotkey_from_str(&c->hk_clear, val);
        else if (IS("trigger", "hk_toggle_listen"))  hotkey_from_str(&c->hk_toggle_listen, val);
        else if (IS("trigger", "hk_snip"))           hotkey_from_str(&c->hk_snip, val);
        else if (IS("trigger", "hk_scroll_up"))      hotkey_from_str(&c->hk_scroll_up, val);
        else if (IS("trigger", "hk_scroll_down"))    hotkey_from_str(&c->hk_scroll_down, val);
        else if (IS("trigger", "hk_pane_next"))      hotkey_from_str(&c->hk_pane_next, val);
        else if (IS("trigger", "hk_move_overlay"))   hotkey_from_str(&c->hk_move_overlay, val);
        else if (IS("overlay", "x")) c->overlay_x = atoi(val);
        else if (IS("overlay", "y")) c->overlay_y = atoi(val);
        else if (IS("overlay", "w")) c->overlay_w = atoi(val);
        else if (IS("overlay", "h")) c->overlay_h = atoi(val);
        else if (IS("overlay", "opacity"))           c->overlay_opacity = atoi(val);
        else if (IS("overlay", "font_size"))         c->overlay_font_size = atoi(val);
        else if (IS("overlay", "click_through"))     c->overlay_click_through = atoi(val);
        else if (IS("overlay", "hide_from_capture")) c->overlay_hide_from_capture = atoi(val);
        else if (IS("overlay", "show_transcript"))   c->overlay_show_transcript = atoi(val);
        else if (IS("overlay", "visible"))           c->overlay_visible = atoi(val);
        else if (IS("overlay", "bg"))     c->overlay_bg     = (COLORREF)strtoul(val, NULL, 16);
        else if (IS("overlay", "fg"))     c->overlay_fg     = (COLORREF)strtoul(val, NULL, 16);
        else if (IS("overlay", "accent")) c->overlay_accent = (COLORREF)strtoul(val, NULL, 16);
#undef IS
    }
    fclose(f);

    /* sanity clamps */
    if (c->whisper_threads  < 1)   c->whisper_threads = 1;
    if (c->whisper_threads  > 32)  c->whisper_threads = 32;
    if (c->overlay_opacity  < 16)  c->overlay_opacity = 16;
    if (c->overlay_opacity  > 255) c->overlay_opacity = 255;
    if (c->overlay_font_size < 7)  c->overlay_font_size = 7;
    if (c->overlay_font_size > 48) c->overlay_font_size = 48;
    if (c->overlay_w < 200) c->overlay_w = 200;
    if (c->overlay_h < 120) c->overlay_h = 120;
    /* 0 / negative are meaningful here: "omit the parameter" and
     * "send the whole transcript". Only guard against nonsense. */
    if (c->context_seconds < 0)    c->context_seconds = 0;
    if (c->max_tokens < 0)         c->max_tokens = 0;
    if (c->temperature < 0.0)      c->temperature = -1.0;
    if (c->temperature > 2.0)      c->temperature = 2.0;
    if (c->vad_silence_ms < 150)   c->vad_silence_ms = 150;
    if (c->vad_max_segment_ms < 2000) c->vad_max_segment_ms = 2000;
    if (c->vad_soft_cap_ms < 600)  c->vad_soft_cap_ms = 600;
    if (c->vad_soft_cap_ms > c->vad_max_segment_ms)
        c->vad_soft_cap_ms = c->vad_max_segment_ms;

    LOGI("config loaded from %s", path);
    return 0;
}

int config_load(config_t *c)
{
    int rc = config_load_ini(c);
    /* Last, so .env overrides both the built-in defaults and config.ini
     * regardless of which path above returned. */
    apply_dotenv(c);
    return rc;
}
