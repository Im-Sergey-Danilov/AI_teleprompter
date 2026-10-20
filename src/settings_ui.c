#include "app.h"
#include "audio.h"
#include "llm.h"
#include "stt.h"
#include "util.h"
#include "log.h"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define WM_TEST_RESULT (WM_USER + 31)
#define TIMER_METER    7

static HWND       g_dlg;
static HWND       g_page[5];
static config_t   g_work;                 /* edited copy */
static audio_device_info_t *g_mic_devs, *g_sys_devs;
static int        g_mic_count, g_sys_count;

static const struct { int id; const wchar_t *title; } kPages[] = {
    { IDD_TAB_STT,     L"Speech-to-text" },
    { IDD_TAB_LLM,     L"LLM and prompt" },
    { IDD_TAB_AUDIO,   L"Audio" },
    { IDD_TAB_TRIGGER, L"Triggers" },
    { IDD_TAB_OVERLAY, L"Overlay" },
};
#define PAGE_COUNT ((int)(sizeof(kPages) / sizeof(kPages[0])))

static const wchar_t *kMouseBtns[] = {
    L"(none)", L"Middle click", L"Mouse 4 (back)", L"Mouse 5 (forward)"
};
#define MOUSE_BTN_COUNT ((int)(sizeof(kMouseBtns) / sizeof(kMouseBtns[0])))

static const char *kLangs[] = {
    "ru","en","auto","uk","de","fr","es","it","pt","nl","pl","tr",
    "cs","sv","da","fi","no","ro","hu","el","he","ar","hi","zh","ja","ko","vi","id"
};
#define LANG_COUNT ((int)(sizeof(kLangs) / sizeof(kLangs[0])))

/* --------------------------------------------------------------- helpers -- */

static void set_text(HWND h, int id, const char *utf8)
{
    wchar_t *w = utf8_to_wide(utf8 ? utf8 : "");
    SetDlgItemTextW(h, id, w ? w : L"");
    free(w);
}

static void get_text(HWND h, int id, char *out, size_t cap)
{
    wchar_t buf[CFG_PROMPT_MAX];
    char *u;
    buf[0] = 0;
    GetDlgItemTextW(h, id, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    u = wide_to_utf8(buf);
    snprintf(out, cap, "%s", u ? u : "");
    free(u);
}

/*
 * Multiline edit controls only render CRLF line breaks, while the config file
 * (and the JSON we send) carries bare LF. Convert on the way in and out.
 */
static void set_text_ml(HWND h, int id, const char *utf8)
{
    sb_t sb;
    const char *p;
    sb_init(&sb);
    for (p = utf8 ? utf8 : ""; *p; ++p) {
        if (*p == '\r') continue;
        if (*p == '\n') sb_append(&sb, "\r\n", 2);
        else             sb_append(&sb, p, 1);
    }
    set_text(h, id, sb.data ? sb.data : "");
    sb_free(&sb);
}

static void get_text_ml(HWND h, int id, char *out, size_t cap)
{
    char *r = out, *w = out;
    get_text(h, id, out, cap);
    while (*r) { if (*r != '\r') *w++ = *r; r++; }
    *w = '\0';
}

static void set_int(HWND h, int id, int v)
{
    wchar_t b[32];
    _snwprintf(b, 31, L"%d", v);
    b[31] = 0;
    SetDlgItemTextW(h, id, b);
}

static int get_int(HWND h, int id, int def)
{
    char b[64];
    get_text(h, id, b, sizeof(b));
    str_trim(b);
    return b[0] ? atoi(b) : def;
}

static void set_dbl(HWND h, int id, double v)
{
    wchar_t b[32];
    _snwprintf(b, 31, L"%.2f", v);
    b[31] = 0;
    SetDlgItemTextW(h, id, b);
}

static double get_dbl(HWND h, int id, double def)
{
    char b[64];
    get_text(h, id, b, sizeof(b));
    str_trim(b);
    return b[0] ? atof(b) : def;
}

/*
 * Blank-able numeric fields: an empty box means "do not send this parameter"
 * (temperature, max tokens) or "no window" (transcript context).
 */
static void set_int_blank(HWND h, int id, int v)
{
    if (v > 0) set_int(h, id, v);
    else       SetDlgItemTextW(h, id, L"");
}

static int get_int_blank(HWND h, int id)
{
    char b[64];
    get_text(h, id, b, sizeof(b));
    str_trim(b);
    return b[0] ? atoi(b) : 0;
}

static void set_dbl_blank(HWND h, int id, double v)
{
    if (v >= 0.0) set_dbl(h, id, v);
    else          SetDlgItemTextW(h, id, L"");
}

static double get_dbl_blank(HWND h, int id)
{
    char b[64];
    get_text(h, id, b, sizeof(b));
    str_trim(b);
    return b[0] ? atof(b) : -1.0;
}

static void set_check(HWND h, int id, int on)
{
    CheckDlgButton(h, id, on ? BST_CHECKED : BST_UNCHECKED);
}

static int get_check(HWND h, int id)
{
    return IsDlgButtonChecked(h, id) == BST_CHECKED;
}

/* HOTKEYF_* <-> MOD_* */
static WORD mods_to_hkf(UINT m)
{
    WORD f = 0;
    if (m & MOD_SHIFT)   f |= HOTKEYF_SHIFT;
    if (m & MOD_CONTROL) f |= HOTKEYF_CONTROL;
    if (m & MOD_ALT)     f |= HOTKEYF_ALT;
    return f;
}

static UINT hkf_to_mods(WORD f)
{
    UINT m = 0;
    if (f & HOTKEYF_SHIFT)   m |= MOD_SHIFT;
    if (f & HOTKEYF_CONTROL) m |= MOD_CONTROL;
    if (f & HOTKEYF_ALT)     m |= MOD_ALT;
    return m;
}

static void set_hotkey(HWND h, int id, const hotkey_t *hk)
{
    SendDlgItemMessageW(h, id, HKM_SETHOTKEY,
                        MAKEWORD(hk->vk, mods_to_hkf(hk->mods)), 0);
}

static void get_hotkey(HWND h, int id, hotkey_t *hk)
{
    LRESULT r = SendDlgItemMessageW(h, id, HKM_GETHOTKEY, 0, 0);
    hk->vk   = LOBYTE(LOWORD(r));
    hk->mods = hkf_to_mods(HIBYTE(LOWORD(r)));
    if (!hk->vk) hk->mods = 0;
}

static void fill_devices(HWND page, int ctrl, int render,
                         audio_device_info_t **list, int *count, const char *selected)
{
    int i, sel = 0;
    HWND cb = GetDlgItem(page, ctrl);

    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"(system default)");

    free(*list);
    *list = NULL;
    *count = 0;
    if (audio_enum_devices(render, list, count) != 0) { SendMessageW(cb, CB_SETCURSEL, 0, 0); return; }

    for (i = 0; i < *count; ++i) {
        wchar_t *w = utf8_to_wide((*list)[i].name);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)(w ? w : L"?"));
        free(w);
        if (selected && *selected && strcmp(selected, (*list)[i].id) == 0) sel = i + 1;
    }
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

static void read_device(HWND page, int ctrl, audio_device_info_t *list, int count,
                        char *out, size_t cap)
{
    LRESULT sel = SendDlgItemMessageW(page, ctrl, CB_GETCURSEL, 0, 0);
    if (sel <= 0 || (int)sel > count) { out[0] = '\0'; return; }
    snprintf(out, cap, "%s", list[sel - 1].id);
}

static void status(const char *fmt, ...)
{
    char buf[512];
    wchar_t *w;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    w = utf8_to_wide(buf);
    if (g_dlg) SetDlgItemTextW(g_dlg, IDC_STATUSBAR, w ? w : L"");
    free(w);
}

/* ------------------------------------------------------- page: load / save */

static void load_pages(void)
{
    HWND p;
    int i;

    /* --- STT --- */
    p = g_page[0];
    set_check(p, IDC_STT_LOCAL,  g_work.stt_backend == STT_BACKEND_LOCAL);
    set_check(p, IDC_STT_REMOTE, g_work.stt_backend == STT_BACKEND_REMOTE);
    set_text(p, IDC_WMODEL, g_work.whisper_model);
    set_int(p, IDC_WTHREADS, g_work.whisper_threads);
    set_check(p, IDC_WGPU, g_work.whisper_gpu);
    set_text(p, IDC_STT_URL, g_work.stt_endpoint);
    set_text(p, IDC_STT_KEY, g_work.stt_api_key);
    set_text(p, IDC_STT_MODEL, g_work.stt_model);
    set_check(p, IDC_TRANSLATE, g_work.whisper_translate);
    set_text(p, IDC_STT_HINT, stt_local_available()
             ? "ggml-*.bin from huggingface.co/ggerganov/whisper.cpp (base = fast, large-v3-turbo = best)"
             : "This build has no whisper.cpp - only the remote engine works.");
    {
        HWND cb = GetDlgItem(p, IDC_LANG);
        int sel = 0;
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        for (i = 0; i < LANG_COUNT; ++i) {
            wchar_t w[8];
            MultiByteToWideChar(CP_UTF8, 0, kLangs[i], -1, w, 8);
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)w);
            if (strcmp(kLangs[i], g_work.whisper_language) == 0) sel = i;
        }
        SendMessageW(cb, CB_SETCURSEL, sel, 0);
    }

    /* --- LLM --- */
    p = g_page[1];
    set_text(p, IDC_LLM_URL, g_work.llm_endpoint);
    set_text(p, IDC_LLM_KEY, g_work.llm_api_key);
    set_text(p, IDC_LLM_MODEL, g_work.llm_model);
    set_dbl_blank(p, IDC_TEMP, g_work.temperature);
    set_int_blank(p, IDC_MAXTOK, g_work.max_tokens);
    set_int_blank(p, IDC_CTXSEC, g_work.context_seconds);
    set_check(p, IDC_STREAM, g_work.llm_stream);
    set_check(p, IDC_TWO_STAGE, g_work.two_stage);
    set_text_ml(p, IDC_SYSPROMPT, g_work.system_prompt);
    set_text_ml(p, IDC_USERTPL, g_work.user_template);
    /* A prompt owned by .env is shown but not editable here - typing into it
     * would look like it worked and then be overridden on the next load. */
    SendDlgItemMessageW(p, IDC_SYSPROMPT, EM_SETREADONLY,
                        (g_work.env_managed & CFG_ENV_SYSTEM_PROMPT) ? TRUE : FALSE, 0);
    SendDlgItemMessageW(p, IDC_USERTPL, EM_SETREADONLY,
                        (g_work.env_managed & CFG_ENV_USER_TEMPLATE) ? TRUE : FALSE, 0);

    /* --- audio --- */
    p = g_page[2];
    set_check(p, IDC_CAP_MIC, g_work.capture_mic);
    set_check(p, IDC_CAP_SYS, g_work.capture_loopback);
    set_dbl(p, IDC_MIC_GAIN, g_work.mic_gain);
    set_dbl(p, IDC_SYS_GAIN, g_work.sys_gain);
    fill_devices(p, IDC_MIC_DEV, 0, &g_mic_devs, &g_mic_count, g_work.mic_device_id);
    fill_devices(p, IDC_SYS_DEV, 1, &g_sys_devs, &g_sys_count, g_work.sys_device_id);
    SendDlgItemMessageW(p, IDC_VAD_TH, TBM_SETRANGE, TRUE, MAKELPARAM(0, 70));  /* -70..0 dB */
    SendDlgItemMessageW(p, IDC_VAD_TH, TBM_SETPOS, TRUE, 70 + g_work.vad_threshold_db);
    set_int(p, IDC_VAD_SIL, g_work.vad_silence_ms);
    set_int(p, IDC_VAD_MIN, g_work.vad_min_speech_ms);
    set_int(p, IDC_VAD_MAX, g_work.vad_max_segment_ms);
    set_int(p, IDC_VAD_SOFT, g_work.vad_soft_cap_ms);
    SendDlgItemMessageW(p, IDC_LEVEL, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    {
        char t[32];
        snprintf(t, sizeof(t), "%d dBFS", g_work.vad_threshold_db);
        set_text(p, IDC_VAD_TH_TXT, t);
    }

    /* --- triggers --- */
    p = g_page[3];
    set_check(p, IDC_AUTO, g_work.auto_respond);
    set_int(p, IDC_AUTO_MIN, g_work.auto_min_chars);
    set_int(p, IDC_AUTO_COOL, g_work.auto_cooldown_ms);
    set_check(p, IDC_AUTO_Q, g_work.auto_require_question);
    set_check(p, IDC_START_LISTENING, g_work.listening);
    set_check(p, IDC_PTT, g_work.push_to_talk);
    {
        HWND cb = GetDlgItem(p, IDC_MOUSE_BTN);
        int k;
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        for (k = 0; k < MOUSE_BTN_COUNT; ++k)
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)kMouseBtns[k]);
        SendMessageW(cb, CB_SETCURSEL,
                     (g_work.trigger_mouse_button >= 0 &&
                      g_work.trigger_mouse_button < MOUSE_BTN_COUNT)
                         ? g_work.trigger_mouse_button : 0, 0);
    }
    set_hotkey(p, IDC_HK_RESPOND, &g_work.hk_respond);
    set_hotkey(p, IDC_HK_OVERLAY, &g_work.hk_toggle_overlay);
    set_hotkey(p, IDC_HK_CLEAR,   &g_work.hk_clear);
    set_hotkey(p, IDC_HK_LISTEN,  &g_work.hk_toggle_listen);
    set_hotkey(p, IDC_HK_SNIP,    &g_work.hk_snip);
    set_hotkey(p, IDC_HK_SCROLL_UP,   &g_work.hk_scroll_up);
    set_hotkey(p, IDC_HK_SCROLL_DOWN, &g_work.hk_scroll_down);
    set_hotkey(p, IDC_HK_PANE_NEXT,   &g_work.hk_pane_next);
    set_hotkey(p, IDC_HK_MOVE,        &g_work.hk_move_overlay);

    /* --- overlay --- */
    p = g_page[4];
    SendDlgItemMessageW(p, IDC_OPACITY, TBM_SETRANGE, TRUE, MAKELPARAM(16, 255));
    SendDlgItemMessageW(p, IDC_OPACITY, TBM_SETPOS, TRUE, g_work.overlay_opacity);
    set_int(p, IDC_FONTSIZE, g_work.overlay_font_size);
    set_check(p, IDC_CLICKTHRU, g_work.overlay_click_through);
    set_check(p, IDC_HIDECAP, g_work.overlay_hide_from_capture);
    set_check(p, IDC_SHOWTR, g_work.overlay_show_transcript);
    set_int(p, IDC_OVX, g_work.overlay_x);
    set_int(p, IDC_OVY, g_work.overlay_y);
    set_int(p, IDC_OVW, g_work.overlay_w);
    set_int(p, IDC_OVH, g_work.overlay_h);
    {
        char t[64];
        snprintf(t, sizeof(t), "%d / 255", g_work.overlay_opacity);
        set_text(p, IDC_OPACITY_TXT, t);
        snprintf(t, sizeof(t), "Background #%02X%02X%02X...",
                 GetRValue(g_work.overlay_bg), GetGValue(g_work.overlay_bg), GetBValue(g_work.overlay_bg));
        set_text(p, IDC_COL_BG, t);
        snprintf(t, sizeof(t), "Text #%02X%02X%02X...",
                 GetRValue(g_work.overlay_fg), GetGValue(g_work.overlay_fg), GetBValue(g_work.overlay_fg));
        set_text(p, IDC_COL_FG, t);
        snprintf(t, sizeof(t), "Accent #%02X%02X%02X...",
                 GetRValue(g_work.overlay_accent), GetGValue(g_work.overlay_accent),
                 GetBValue(g_work.overlay_accent));
        set_text(p, IDC_COL_ACCENT, t);
    }
}

static void save_pages(void)
{
    HWND p;

    p = g_page[0];
    g_work.stt_backend = get_check(p, IDC_STT_REMOTE) ? STT_BACKEND_REMOTE : STT_BACKEND_LOCAL;
    get_text(p, IDC_WMODEL, g_work.whisper_model, sizeof(g_work.whisper_model));
    g_work.whisper_threads = get_int(p, IDC_WTHREADS, 4);
    g_work.whisper_gpu = get_check(p, IDC_WGPU);
    get_text(p, IDC_STT_URL, g_work.stt_endpoint, sizeof(g_work.stt_endpoint));
    get_text(p, IDC_STT_KEY, g_work.stt_api_key, sizeof(g_work.stt_api_key));
    get_text(p, IDC_STT_MODEL, g_work.stt_model, sizeof(g_work.stt_model));
    g_work.whisper_translate = get_check(p, IDC_TRANSLATE);
    {
        LRESULT sel = SendDlgItemMessageW(p, IDC_LANG, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < LANG_COUNT)
            snprintf(g_work.whisper_language, sizeof(g_work.whisper_language), "%s", kLangs[sel]);
    }

    p = g_page[1];
    get_text(p, IDC_LLM_URL, g_work.llm_endpoint, sizeof(g_work.llm_endpoint));
    get_text(p, IDC_LLM_KEY, g_work.llm_api_key, sizeof(g_work.llm_api_key));
    get_text(p, IDC_LLM_MODEL, g_work.llm_model, sizeof(g_work.llm_model));
    g_work.temperature = get_dbl_blank(p, IDC_TEMP);
    g_work.max_tokens = get_int_blank(p, IDC_MAXTOK);
    g_work.context_seconds = get_int_blank(p, IDC_CTXSEC);
    g_work.llm_stream = get_check(p, IDC_STREAM);
    g_work.two_stage = get_check(p, IDC_TWO_STAGE);
    if (!(g_work.env_managed & CFG_ENV_SYSTEM_PROMPT))
        get_text_ml(p, IDC_SYSPROMPT, g_work.system_prompt, sizeof(g_work.system_prompt));
    if (!(g_work.env_managed & CFG_ENV_USER_TEMPLATE))
        get_text_ml(p, IDC_USERTPL, g_work.user_template, sizeof(g_work.user_template));

    p = g_page[2];
    g_work.capture_mic = get_check(p, IDC_CAP_MIC);
    g_work.capture_loopback = get_check(p, IDC_CAP_SYS);
    g_work.mic_gain = (float)get_dbl(p, IDC_MIC_GAIN, 1.0);
    g_work.sys_gain = (float)get_dbl(p, IDC_SYS_GAIN, 1.0);
    read_device(p, IDC_MIC_DEV, g_mic_devs, g_mic_count,
                g_work.mic_device_id, sizeof(g_work.mic_device_id));
    read_device(p, IDC_SYS_DEV, g_sys_devs, g_sys_count,
                g_work.sys_device_id, sizeof(g_work.sys_device_id));
    g_work.vad_threshold_db =
        (int)SendDlgItemMessageW(p, IDC_VAD_TH, TBM_GETPOS, 0, 0) - 70;
    g_work.vad_silence_ms = get_int(p, IDC_VAD_SIL, 700);
    g_work.vad_min_speech_ms = get_int(p, IDC_VAD_MIN, 240);
    g_work.vad_max_segment_ms = get_int(p, IDC_VAD_MAX, 20000);
    g_work.vad_soft_cap_ms = get_int(p, IDC_VAD_SOFT, 2500);

    p = g_page[3];
    g_work.auto_respond = get_check(p, IDC_AUTO);
    g_work.auto_min_chars = get_int(p, IDC_AUTO_MIN, 12);
    g_work.auto_cooldown_ms = get_int(p, IDC_AUTO_COOL, 2500);
    g_work.auto_require_question = get_check(p, IDC_AUTO_Q);
    g_work.listening = get_check(p, IDC_START_LISTENING);
    g_work.push_to_talk = get_check(p, IDC_PTT);
    {
        LRESULT sel = SendDlgItemMessageW(p, IDC_MOUSE_BTN, CB_GETCURSEL, 0, 0);
        g_work.trigger_mouse_button = (sel > 0 && sel < MOUSE_BTN_COUNT) ? (int)sel : 0;
    }
    get_hotkey(p, IDC_HK_RESPOND, &g_work.hk_respond);
    get_hotkey(p, IDC_HK_OVERLAY, &g_work.hk_toggle_overlay);
    get_hotkey(p, IDC_HK_CLEAR,   &g_work.hk_clear);
    get_hotkey(p, IDC_HK_LISTEN,  &g_work.hk_toggle_listen);
    get_hotkey(p, IDC_HK_SNIP,    &g_work.hk_snip);
    get_hotkey(p, IDC_HK_SCROLL_UP,   &g_work.hk_scroll_up);
    get_hotkey(p, IDC_HK_SCROLL_DOWN, &g_work.hk_scroll_down);
    get_hotkey(p, IDC_HK_PANE_NEXT,   &g_work.hk_pane_next);
    get_hotkey(p, IDC_HK_MOVE,        &g_work.hk_move_overlay);

    p = g_page[4];
    g_work.overlay_opacity = (int)SendDlgItemMessageW(p, IDC_OPACITY, TBM_GETPOS, 0, 0);
    g_work.overlay_font_size = get_int(p, IDC_FONTSIZE, 12);
    g_work.overlay_click_through = get_check(p, IDC_CLICKTHRU);
    g_work.overlay_hide_from_capture = get_check(p, IDC_HIDECAP);
    g_work.overlay_show_transcript = get_check(p, IDC_SHOWTR);
    g_work.overlay_x = get_int(p, IDC_OVX, 60);
    g_work.overlay_y = get_int(p, IDC_OVY, 60);
    g_work.overlay_w = get_int(p, IDC_OVW, 460);
    g_work.overlay_h = get_int(p, IDC_OVH, 320);
}

/* ---------------------------------------------------------- page dialogs -- */

static int pick_color(HWND owner, COLORREF *c)
{
    static COLORREF custom[16];
    CHOOSECOLORW cc;
    memset(&cc, 0, sizeof(cc));
    cc.lStructSize  = sizeof(cc);
    cc.hwndOwner    = owner;
    cc.rgbResult    = *c;
    cc.lpCustColors = custom;
    cc.Flags        = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return 0;
    *c = cc.rgbResult;
    return 1;
}

/* g_work snapshot + which test to run; the worker owns and frees it. */
typedef struct { config_t cfg; int which; } test_job_t;   /* which: 0 = LLM, 1 = STT */

static DWORD WINAPI test_thread(LPVOID arg)
{
    test_job_t *job = (test_job_t *)arg;
    char *msg = (char *)malloc(600);
    if (msg) {
        HWND dlg;
        if (job->which) stt_test(&job->cfg, msg, 600);
        else            llm_test(&job->cfg, msg, 600);
        dlg = g_dlg;
        if (!dlg || !PostMessageW(dlg, WM_TEST_RESULT, (WPARAM)job->which, (LPARAM)msg))
            free(msg);
    }
    free(job);
    return 0;
}

static void start_test(HWND page, int which, int button_id)
{
    test_job_t *job;
    HANDLE t;

    save_pages();
    job = (test_job_t *)malloc(sizeof(*job));
    if (!job) return;
    job->cfg   = g_work;
    job->which = which;

    status("Testing %s ...", which ? g_work.stt_endpoint : g_work.llm_endpoint);
    EnableWindow(GetDlgItem(page, button_id), FALSE);

    t = CreateThread(NULL, 0, test_thread, job, 0, NULL);
    if (t) CloseHandle(t);
    else { free(job); EnableWindow(GetDlgItem(page, button_id), TRUE); }
}

static INT_PTR CALLBACK page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        return TRUE;

    case WM_HSCROLL: {
        HWND ctl = (HWND)lp;
        char t[64];
        if (ctl == GetDlgItem(h, IDC_VAD_TH)) {
            int db = (int)SendMessageW(ctl, TBM_GETPOS, 0, 0) - 70;
            snprintf(t, sizeof(t), "%d dBFS", db);
            set_text(h, IDC_VAD_TH_TXT, t);
        } else if (ctl == GetDlgItem(h, IDC_OPACITY)) {
            int v = (int)SendMessageW(ctl, TBM_GETPOS, 0, 0);
            snprintf(t, sizeof(t), "%d / 255", v);
            set_text(h, IDC_OPACITY_TXT, t);
        }
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_WBROWSE: {
            OPENFILENAMEW ofn;
            wchar_t file[MAX_PATH] = L"";
            char cur[MAX_PATH];
            get_text(h, IDC_WMODEL, cur, sizeof(cur));
            if (cur[0]) {
                wchar_t *w = utf8_to_wide(cur);
                if (w) { wcsncpy(file, w, MAX_PATH - 1); free(w); }
            }
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = g_dlg;
            ofn.lpstrFilter = L"Whisper models (*.bin;*.gguf)\0*.bin;*.gguf\0All files\0*.*\0";
            ofn.lpstrFile   = file;
            ofn.nMaxFile    = MAX_PATH;
            ofn.lpstrTitle  = L"Select a whisper.cpp model";
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                char *u = wide_to_utf8(file);
                if (u) { set_text(h, IDC_WMODEL, u); free(u); }
            }
            return TRUE;
        }
        case IDC_LLM_TEST:
            start_test(h, 0, IDC_LLM_TEST);
            return TRUE;
        case IDC_STT_TEST:
            start_test(h, 1, IDC_STT_TEST);
            return TRUE;
        case IDC_COL_BG:
        case IDC_COL_FG:
        case IDC_COL_ACCENT: {
            COLORREF *target = (LOWORD(wp) == IDC_COL_BG)     ? &g_work.overlay_bg
                             : (LOWORD(wp) == IDC_COL_FG)     ? &g_work.overlay_fg
                                                              : &g_work.overlay_accent;
            int id = LOWORD(wp);
            if (pick_color(g_dlg, target)) {
                char t[64];
                const char *label = (id == IDC_COL_BG) ? "Background"
                                  : (id == IDC_COL_FG) ? "Text" : "Accent";
                snprintf(t, sizeof(t), "%s #%02X%02X%02X...", label,
                         GetRValue(*target), GetGValue(*target), GetBValue(*target));
                set_text(h, id, t);
            }
            return TRUE;
        }
        case IDC_OV_GRAB: {
            RECT r;
            app_overlay_rect(&r);
            set_int(h, IDC_OVX, r.left);
            set_int(h, IDC_OVY, r.top);
            set_int(h, IDC_OVW, r.right - r.left);
            set_int(h, IDC_OVH, r.bottom - r.top);
            return TRUE;
        }
        case IDC_OV_RESET: {
            config_t d;
            config_defaults(&d);
            set_int(h, IDC_OVX, d.overlay_x);
            set_int(h, IDC_OVY, d.overlay_y);
            set_int(h, IDC_OVW, d.overlay_w);
            set_int(h, IDC_OVH, d.overlay_h);
            return TRUE;
        }
        }
        return FALSE;
    }
    return FALSE;
}

/* ---------------------------------------------------------- main dialog --- */

static void layout_pages(void)
{
    RECT r;
    HWND tab = GetDlgItem(g_dlg, IDC_TAB);
    int i;

    GetWindowRect(tab, &r);
    MapWindowPoints(NULL, g_dlg, (POINT *)&r, 2);
    TabCtrl_AdjustRect(tab, FALSE, &r);

    for (i = 0; i < PAGE_COUNT; ++i)
        SetWindowPos(g_page[i], HWND_TOP, r.left, r.top,
                     r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
}

static void select_page(int idx)
{
    int i;
    for (i = 0; i < PAGE_COUNT; ++i)
        ShowWindow(g_page[i], i == idx ? SW_SHOW : SW_HIDE);
}

static void apply_now(int close_after)
{
    char msg[512] = "";
    save_pages();
    if (app_apply_config(&g_work, msg, sizeof(msg)) == 0) {
        if (msg[0]) status("%s", msg);
        else        status("Settings applied and saved.");
    } else {
        status("%s", msg[0] ? msg : "Could not apply settings.");
    }
    if (close_after) DestroyWindow(g_dlg);
}

static INT_PTR CALLBACK settings_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        HWND tab = GetDlgItem(h, IDC_TAB);
        int i;

        g_dlg = h;
        app_get_config(&g_work);

        SendMessageW(h, WM_SETICON, ICON_SMALL,
                     (LPARAM)LoadImageW(app_instance(), MAKEINTRESOURCEW(IDI_APPICON),
                                        IMAGE_ICON, 16, 16, 0));

        for (i = 0; i < PAGE_COUNT; ++i) {
            TCITEMW ti;
            memset(&ti, 0, sizeof(ti));
            ti.mask    = TCIF_TEXT;
            ti.pszText = (LPWSTR)kPages[i].title;
            TabCtrl_InsertItem(tab, i, &ti);
            g_page[i] = CreateDialogParamW(app_instance(),
                                           MAKEINTRESOURCEW(kPages[i].id),
                                           h, page_proc, 0);
        }
        layout_pages();
        load_pages();
        select_page(0);
        SetTimer(h, TIMER_METER, 100, NULL);
        status("Ready. Changes take effect on Apply or OK.");
        return TRUE;
    }

    case WM_TIMER:
        if (wp == TIMER_METER && g_page[2]) {
            float db = app_level_db();
            int pos = (int)((db + 70.0f) * 100.0f / 70.0f);
            char t[64];
            if (pos < 0) pos = 0;
            if (pos > 100) pos = 100;
            SendDlgItemMessageW(g_page[2], IDC_LEVEL, PBM_SETPOS, pos, 0);
            snprintf(t, sizeof(t), "%.0f dBFS", (double)db);
            set_text(g_page[2], IDC_LEVEL_TXT, t);
            {
                /* Both sources land in one stream, so a quiet mic is masked by
                 * loud system audio rather than showing up as a fault. Show
                 * them separately so the gains can be set against something. */
                float m = app_source_level_db(0), y = app_source_level_db(1);
                char line[160];
                snprintf(line, sizeof(line),
                         "microphone %.0f dBFS   |   system %.0f dBFS   "
                         "(gate opens near %d dBFS - raise mic gain if your voice sits below it)",
                         (double)m, (double)y, g_work.vad_threshold_db);
                set_text(g_page[2], IDC_SRC_LEVELS, line);
            }
        }
        return TRUE;

    case WM_TEST_RESULT: {
        char *m = (char *)lp;
        if (wp) EnableWindow(GetDlgItem(g_page[0], IDC_STT_TEST), TRUE);
        else    EnableWindow(GetDlgItem(g_page[1], IDC_LLM_TEST), TRUE);
        status("%s", m ? m : "");
        free(m);
        return TRUE;
    }

    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->idFrom == IDC_TAB && nh->code == TCN_SELCHANGE)
            select_page(TabCtrl_GetCurSel(GetDlgItem(h, IDC_TAB)));
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:      apply_now(1); return TRUE;
        case IDC_APPLY: apply_now(0); return TRUE;
        case IDCANCEL:  DestroyWindow(h); return TRUE;
        }
        return FALSE;

    case WM_DESTROY:
        KillTimer(h, TIMER_METER);
        free(g_mic_devs); g_mic_devs = NULL; g_mic_count = 0;
        free(g_sys_devs); g_sys_devs = NULL; g_sys_count = 0;
        g_dlg = NULL;
        return TRUE;
    }
    return FALSE;
}

void settings_show(HINSTANCE inst)
{
    if (g_dlg) {
        ShowWindow(g_dlg, SW_SHOW);
        SetForegroundWindow(g_dlg);
        return;
    }
    g_dlg = CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, settings_proc, 0);
    if (!g_dlg) {
        LOGE("settings: CreateDialogParam failed (%lu)", (unsigned long)GetLastError());
        return;
    }
    ShowWindow(g_dlg, SW_SHOW);
    SetForegroundWindow(g_dlg);
}

HWND settings_hwnd(void) { return g_dlg; }
