/*
 * call_assist - live call assistant for Windows.
 *
 *   WASAPI mic + loopback  ->  16 kHz mono mix  ->  VAD segments
 *        -> Whisper (in-process or remote HTTP)  ->  rolling transcript
 *        -> OpenAI-compatible chat completion    ->  on-screen overlay
 */

#include "app.h"
#include "audio.h"
#include "config.h"
#include "llm.h"
#include "log.h"
#include "overlay.h"
#include "resource.h"
#include "snip.h"
#include "stt.h"
#include "transcript.h"
#include "util.h"

#include <windows.h>
#include <objbase.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define WND_CLASS      L"CallAssistMain"
#define WM_APP_TRAY    (WM_APP + 1)
#define WM_APP_STT     (WM_APP + 2)
#define WM_APP_LLM     (WM_APP + 3)
#define TRAY_UID       1
#define TIMER_STATUS   3

enum { HK_RESPOND = 1, HK_OVERLAY, HK_CLEAR, HK_LISTEN, HK_SNIP,
       HK_SCROLL_UP, HK_SCROLL_DOWN, HK_PANE_NEXT, HK_MOVE };

typedef struct {
    HINSTANCE        inst;
    HWND             hwnd;
    NOTIFYICONDATAW  nid;
    HICON            icon;

    CRITICAL_SECTION cfg_cs;
    config_t         cfg;

    audio_engine_t  *audio;
    stt_worker_t    *stt;
    llm_worker_t    *llm;
    transcript_t    *tr;
    overlay_t       *ovl;

    volatile LONG    listening;
    unsigned long long last_answer_ms;
    int              llm_active;
    int              answer_on_next_transcript;   /* push-to-talk take pending */
    int              llm_stage;                   /* 0 one-shot, 1 answer, 2 details */
    char            *stage1_answer;               /* what stage two follows on from */
    char             last_error[256];
} app_t;

static app_t g;

/* ------------------------------------------------------------ config access */

void app_get_config(config_t *out)
{
    EnterCriticalSection(&g.cfg_cs);
    *out = g.cfg;
    LeaveCriticalSection(&g.cfg_cs);
}

HWND      app_main_window(void) { return g.hwnd; }
HINSTANCE app_instance(void)    { return g.inst; }

void app_overlay_rect(RECT *r)
{
    if (g.ovl) overlay_get_rect(g.ovl, r);
    else       SetRect(r, 0, 0, 0, 0);
}

float app_level_db(void) { return audio_level_db(g.audio); }
float app_source_level_db(int loopback) { return audio_source_level_db(g.audio, loopback); }

int app_listening(void) { return (int)InterlockedCompareExchange(&g.listening, 0, 0); }

/* ------------------------------------------------------------------ hotkeys */

static void unregister_hotkeys(void)
{
    int i;
    for (i = HK_RESPOND; i <= HK_MOVE; ++i) UnregisterHotKey(g.hwnd, i);
}

static int register_hotkeys(const config_t *c, char *msg, size_t cap)
{
    struct { int id; const hotkey_t *hk; const char *name; } list[] = {
        { HK_RESPOND, &c->hk_respond,        "Answer now"      },
        { HK_OVERLAY, &c->hk_toggle_overlay, "Toggle overlay"  },
        { HK_CLEAR,   &c->hk_clear,          "Clear transcript"},
        { HK_LISTEN,  &c->hk_toggle_listen,  "Toggle listening"},
        { HK_SNIP,    &c->hk_snip,           "Screenshot"      },
        { HK_SCROLL_UP,   &c->hk_scroll_up,   "Scroll up"       },
        { HK_SCROLL_DOWN, &c->hk_scroll_down, "Scroll down"     },
        { HK_PANE_NEXT,   &c->hk_pane_next,   "Next pane"       },
        { HK_MOVE,        &c->hk_move_overlay,"Move overlay"    },
    };
    int i, failed = 0;
    char names[256] = "";

    unregister_hotkeys();
    for (i = 0; i < (int)(sizeof(list) / sizeof(list[0])); ++i) {
        if (!list[i].hk->vk) continue;
        if (!RegisterHotKey(g.hwnd, list[i].id, list[i].hk->mods | MOD_NOREPEAT,
                            list[i].hk->vk)) {
            failed++;
            snprintf(names + strlen(names), sizeof(names) - strlen(names),
                     "%s%s", names[0] ? ", " : "", list[i].name);
        }
    }
    if (failed && msg)
        snprintf(msg, cap, "Applied, but these hotkeys are taken by another app: %s", names);
    return failed;
}


/* ------------------------------------------------------- mouse-button bind --
 * RegisterHotKey cannot see mouse buttons, so a bound side button needs a
 * low-level hook. Only the bound button is swallowed; everything else passes
 * through untouched.
 *
 * The same hook gives the overlay its wheel: a click-through window never
 * receives WM_MOUSEWHEEL, so a wheel turn over one of its panes is taken here
 * and forwarded to it instead of scrolling whatever is underneath.
 */
static HHOOK g_mouse_hook;
static volatile LONG g_mouse_btn;      /* 0 none, 1 middle, 2 X1, 3 X2 */
static volatile LONG g_mouse_down;     /* swallow the matching release too */

static LRESULT CALLBACK mouse_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)lp;
        LONG want = InterlockedCompareExchange(&g_mouse_btn, 0, 0);
        int  which = (int)HIWORD(m->mouseData);   /* 1 = X1, 2 = X2 */

        if (want) {
            int down = (want == 1 && wp == WM_MBUTTONDOWN) ||
                       (wp == WM_XBUTTONDOWN && ((want == 2 && which == 1) ||
                                                 (want == 3 && which == 2)));
            int up   = (want == 1 && wp == WM_MBUTTONUP) ||
                       (wp == WM_XBUTTONUP && ((want == 2 && which == 1) ||
                                               (want == 3 && which == 2)));
            if (down) {
                InterlockedExchange(&g_mouse_down, 1);
                PostMessageW(g.hwnd, WM_COMMAND, MAKEWPARAM(IDM_LISTEN, 0), 0);
                return 1;
            }
            if (up && InterlockedExchange(&g_mouse_down, 0)) return 1;
        }

        if (wp == WM_MOUSEWHEEL && g.ovl && overlay_is_visible(g.ovl)) {
            RECT r;
            HWND ovl = overlay_hwnd(g.ovl);
            GetWindowRect(ovl, &r);
            if (PtInRect(&r, m->pt) && overlay_pane_at(g.ovl, m->pt) >= 0) {
                /* Same shape as a real WM_MOUSEWHEEL: delta in the high word
                 * of wParam, screen coordinates in lParam. */
                PostMessageW(ovl, WM_MOUSEWHEEL,
                             MAKEWPARAM(0, HIWORD(m->mouseData)),
                             MAKELPARAM(m->pt.x, m->pt.y));
                return 1;
            }
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

/* The hook stays in for the whole run so the overlay always gets its wheel;
 * the button binding only changes which button it swallows. */
static void install_mouse_hook(int on)
{
    if (on && !g_mouse_hook) {
        g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, g.inst, 0);
        if (!g_mouse_hook) LOGW("could not install the mouse hook (%lu)",
                                (unsigned long)GetLastError());
    } else if (!on && g_mouse_hook) {
        UnhookWindowsHookEx(g_mouse_hook);
        g_mouse_hook = NULL;
    }
}

static void apply_mouse_binding(int button)
{
    LONG prev = InterlockedExchange(&g_mouse_btn, button);
    install_mouse_hook(1);
    if (button && button != prev) LOGI("mouse trigger bound to button %d", button);
    else if (!button && prev)     LOGI("mouse trigger unbound");
}

/* ---------------------------------------------------------------- overlay UI */

static void update_status_line(void)
{
    char buf[420];
    char stt_status[256];
    char audio_err[256];
    config_t cfg;
    int backlog;

    if (!g.ovl) return;

    app_get_config(&cfg);
    stt_worker_status(g.stt, stt_status, sizeof(stt_status));
    audio_last_error(g.audio, audio_err, sizeof(audio_err));
    backlog = stt_worker_backlog(g.stt);

    if (overlay_is_interactive(g.ovl)) {
        overlay_set_state(g.ovl, OVL_PAUSED);
        snprintf(buf, sizeof(buf),
                 "Move mode - drag the overlay, corner resizes, wheel scrolls. "
                 "Press the move hotkey again when done");
        overlay_set_status(g.ovl, buf);
        return;
    }

    if (cfg.push_to_talk && audio_recording(g.audio)) {
        double secs = audio_record_seconds(g.audio);
        overlay_set_state(g.ovl, OVL_LISTENING);
        snprintf(buf, sizeof(buf), "Recording %d:%02d - press the listen hotkey to stop and answer",
                 (int)secs / 60, (int)secs % 60);
    } else if (!app_listening()) {
        overlay_set_state(g.ovl, OVL_PAUSED);
        snprintf(buf, sizeof(buf), cfg.push_to_talk
                 ? "Ready - press the listen hotkey to record"
                 : "Paused - press the listen hotkey to start");
    } else if (g.llm_active) {
        overlay_set_state(g.ovl, OVL_THINKING);
        snprintf(buf, sizeof(buf), "Thinking");
    } else if (stt_worker_busy(g.stt) || backlog) {
        overlay_set_state(g.ovl, OVL_TRANSCRIBING);
        snprintf(buf, sizeof(buf), "Transcribing%s%d queued%s",
                 backlog ? " (" : "", backlog, backlog ? ")" : "");
    } else {
        overlay_set_state(g.ovl, OVL_LISTENING);
        snprintf(buf, sizeof(buf), "Listening");
    }

    /* Surface a dead capture stream: silence otherwise looks like "listening". */
    if (app_listening()) {
        int mic_down = cfg.capture_mic      && !audio_source_alive(g.audio, 0);
        int sys_down = cfg.capture_loopback && !audio_source_alive(g.audio, 1);
        if (mic_down || sys_down)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "  -  no %s%s%s audio",
                     mic_down ? "microphone" : "",
                     (mic_down && sys_down) ? " or " : "",
                     sys_down ? "system" : "");
    }

    if (g.last_error[0])
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "  -  %s", g.last_error);
    else if (strncmp(stt_status, "STT error", 9) == 0 ||
             strncmp(stt_status, "STT unavailable", 15) == 0)
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "  -  %s", stt_status);
    else if (audio_err[0])
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "  -  %s", audio_err);

    overlay_set_status(g.ovl, buf);
}

static void refresh_tray_tip(void)
{
    wchar_t tip[128];
    _snwprintf(tip, 127, L"call_assist - %s", app_listening() ? L"listening" : L"paused");
    tip[127] = 0;
    wcsncpy(g.nid.szTip, tip, sizeof(g.nid.szTip) / sizeof(wchar_t) - 1);
    g.nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
}

/* ------------------------------------------------------------- LLM plumbing */

static void llm_cb(void *user, int event, const char *text)
{
    /* Marshal to the UI thread so all window work happens on one thread. */
    char *copy = text ? xstrdup(text) : NULL;
    (void)user;
    if (!PostMessageW(g.hwnd, WM_APP_LLM, (WPARAM)event, (LPARAM)copy))
        free(copy);
}

/* ------------------------------------------------------------- Q&A history --
 * The overlay only ever shows the newest answer, but a follow-up question is
 * meaningless without the ones before it, so past exchanges are replayed to the
 * model as real user/assistant turns.
 */
#define QA_MAX 8

static struct { char *q, *a; } g_qa[QA_MAX];
static int g_qa_count;

static void qa_push(const char *q, const char *a)
{
    if (!q || !*q || !a || !*a) return;
    if (g_qa_count == QA_MAX) {
        free(g_qa[0].q);
        free(g_qa[0].a);
        memmove(&g_qa[0], &g_qa[1], (QA_MAX - 1) * sizeof(g_qa[0]));
        g_qa_count--;
    }
    g_qa[g_qa_count].q = xstrdup(q);
    g_qa[g_qa_count].a = xstrdup(a);
    if (g_qa[g_qa_count].q && g_qa[g_qa_count].a) g_qa_count++;
    else { free(g_qa[g_qa_count].q); free(g_qa[g_qa_count].a); }
}

static void qa_clear(void)
{
    int i;
    for (i = 0; i < g_qa_count; ++i) { free(g_qa[i].q); free(g_qa[i].a); }
    g_qa_count = 0;
}

static int qa_turns(llm_turn_t *out, int cap)
{
    int i, n = g_qa_count < cap ? g_qa_count : cap;
    int from = g_qa_count - n;
    for (i = 0; i < n; ++i) { out[i].question = g_qa[from + i].q; out[i].answer = g_qa[from + i].a; }
    return n;
}

/* The question the current request is about, kept for the history entry. */
static char *g_pending_question;

void app_trigger_answer(void)
{
    config_t cfg;
    char *ctx, *last;
    llm_turn_t hist[QA_MAX];
    int n_hist;

    app_get_config(&cfg);
    if (!cfg.llm_endpoint[0]) {
        snprintf(g.last_error, sizeof(g.last_error), "no LLM endpoint configured");
        update_status_line();
        return;
    }
    if (transcript_count(g.tr) == 0) {
        overlay_show(g.ovl, 1);
        overlay_set_answer(g.ovl, "Nothing transcribed yet.");
        return;
    }

    ctx  = transcript_recent(g.tr, cfg.context_seconds);
    last = transcript_last(g.tr);

    free(g_pending_question);
    g_pending_question = xstrdup(last && *last ? last : (ctx ? ctx : ""));

    n_hist = qa_turns(hist, QA_MAX);
    g.llm_stage = cfg.two_stage ? 1 : 0;

    g.last_answer_ms = now_ms();
    overlay_set_pane(g.ovl, OVL_PANE_INPUT, last && *last ? last : ctx);
    overlay_show(g.ovl, 1);
    if (llm_request(g.llm, &cfg, hist, n_hist, ctx, last,
                    g.llm_stage == 1 ? cfg.stage1_prompt : NULL, llm_cb, NULL) != 0) {
        snprintf(g.last_error, sizeof(g.last_error), "could not queue the LLM request");
        update_status_line();
    }
    free(ctx);
    free(last);
}

/* Second half of a two-stage answer: the details, once the reply itself is up. */
static void trigger_stage_two(void)
{
    config_t cfg;
    char *ctx, *last;
    llm_turn_t hist[QA_MAX + 1];
    int n_hist;

    app_get_config(&cfg);
    ctx  = transcript_recent(g.tr, cfg.context_seconds);
    last = transcript_last(g.tr);

    n_hist = qa_turns(hist, QA_MAX);
    /* The short answer just given is the last turn, so the details follow on
     * from it instead of restating it. */
    if (g_pending_question && g.stage1_answer) {
        hist[n_hist].question = g_pending_question;
        hist[n_hist].answer   = g.stage1_answer;
        n_hist++;
    }

    g.llm_stage = 2;
    if (llm_request(g.llm, &cfg, hist, n_hist, ctx, last,
                    cfg.stage2_prompt, llm_cb, NULL) != 0)
        g.llm_stage = 0;

    free(ctx);
    free(last);
}

/*
 * Grab a region and ask the model about it. The overlay is taken down for the
 * duration so it cannot appear in the shot - the answer would otherwise feed
 * on its own previous output.
 */
void app_snip_and_ask(void)
{
    config_t cfg;
    unsigned char *png = NULL;
    size_t png_len = 0;
    char *b64;
    int was_visible, rc;

    app_get_config(&cfg);
    if (!cfg.llm_endpoint[0]) {
        snprintf(g.last_error, sizeof(g.last_error), "no LLM endpoint configured");
        update_status_line();
        return;
    }

    was_visible = overlay_is_visible(g.ovl);
    if (was_visible) overlay_show(g.ovl, 0);

    rc = snip_region(g.inst, &png, &png_len);

    if (was_visible) overlay_show(g.ovl, 1);
    if (rc != 0) {
        if (rc < 0) {
            snprintf(g.last_error, sizeof(g.last_error), "screenshot failed");
            update_status_line();
        }
        free(png);
        return;
    }

    b64 = base64_encode(png, png_len);
    free(png);
    if (!b64) {
        snprintf(g.last_error, sizeof(g.last_error), "could not encode the screenshot");
        update_status_line();
        return;
    }

    LOGI("snip: sending %u bytes of base64 to the model", (unsigned)strlen(b64));
    g.last_answer_ms = now_ms();
    overlay_set_pane(g.ovl, OVL_PANE_INPUT, "(screenshot)");
    overlay_show(g.ovl, 1);
    {
        llm_turn_t hist[QA_MAX];
        int n_hist = qa_turns(hist, QA_MAX);
        free(g_pending_question);
        g_pending_question = xstrdup("(screenshot)");
        g.llm_stage = 0;
        if (llm_request_image(g.llm, &cfg, hist, n_hist, b64, cfg.snip_prompt,
                              llm_cb, NULL) != 0)
            snprintf(g.last_error, sizeof(g.last_error),
                     "could not queue the screenshot request");
    }
    free(b64);
    update_status_line();
}

void app_clear_transcript(void)
{
    transcript_clear(g.tr);
    overlay_set_transcript(g.ovl, "");
    overlay_set_answer(g.ovl, "");
    g.last_error[0] = '\0';
    update_status_line();
}

/*
 * In push-to-talk mode the listen hotkey is a record button: the first press
 * starts capturing, the second stops it and sends the whole take through
 * Whisper and then to the model. Otherwise it toggles continuous listening.
 */
void app_set_listening(int on)
{
    config_t cfg;
    app_get_config(&cfg);

    InterlockedExchange(&g.listening, on ? 1 : 0);
    /* Keep the saved startup flag in step, otherwise toggling with the hotkey
     * is silently undone by the next config write. */
    EnterCriticalSection(&g.cfg_cs);
    g.cfg.listening = on ? 1 : 0;
    LeaveCriticalSection(&g.cfg_cs);

    if (cfg.push_to_talk) {
        if (on) {
            g.last_error[0] = '\0';
            audio_record_start(g.audio);
        } else {
            size_t n = 0;
            float *pcm = audio_record_stop(g.audio, &n);
            if (pcm && n) {
                g.answer_on_next_transcript = 1;
                stt_worker_submit(g.stt, pcm, n);   /* takes ownership */
            } else {
                free(pcm);
                snprintf(g.last_error, sizeof(g.last_error), "nothing was recorded");
            }
        }
    }

    LOGI("%s %s", cfg.push_to_talk ? "recording" : "listening", on ? "ON" : "OFF");
    if (on) overlay_show(g.ovl, 1);          /* so the state dot is visible */
    refresh_tray_tip();
    update_status_line();
}

/*
 * Move mode. The overlay is click-through by default, which also makes it
 * undraggable, so this lends it the mouse for a moment; leaving move mode puts
 * click-through back and remembers where it was dropped.
 */
void app_toggle_move_mode(void)
{
    int on = !overlay_is_interactive(g.ovl);

    overlay_set_interactive(g.ovl, on);
    if (on) {
        overlay_show(g.ovl, 1);
    } else {
        RECT r;
        overlay_get_rect(g.ovl, &r);
        EnterCriticalSection(&g.cfg_cs);
        g.cfg.overlay_x = r.left;
        g.cfg.overlay_y = r.top;
        g.cfg.overlay_w = r.right - r.left;
        g.cfg.overlay_h = r.bottom - r.top;
        LeaveCriticalSection(&g.cfg_cs);
        {
            config_t c;
            app_get_config(&c);
            config_save(&c);
        }
        LOGI("overlay moved to %d,%d %dx%d", (int)r.left, (int)r.top,
             (int)(r.right - r.left), (int)(r.bottom - r.top));
    }
    update_status_line();
}

void app_toggle_overlay(void)
{
    int vis = overlay_is_visible(g.ovl);
    overlay_show(g.ovl, !vis);
    EnterCriticalSection(&g.cfg_cs);
    g.cfg.overlay_visible = !vis;
    LeaveCriticalSection(&g.cfg_cs);
}

/* ------------------------------------------------------- pipeline callbacks */

static void on_segment(void *user, float *pcm, size_t n)
{
    static int warned_paused;
    config_t cfg;
    (void)user;

    app_get_config(&cfg);
    if (cfg.push_to_talk) { free(pcm); return; }   /* the take is sent on stop */

    if (!app_listening()) {
        if (!warned_paused) {
            LOGI("audio: utterance detected while paused - press the listen hotkey");
            warned_paused = 1;
        }
        free(pcm);
        return;
    }
    warned_paused = 0;
    stt_worker_submit(g.stt, pcm, n);       /* takes ownership */
}

static void on_transcript(void *user, const char *text, double seconds)
{
    char *copy = xstrdup(text);
    (void)user; (void)seconds;
    if (!PostMessageW(g.hwnd, WM_APP_STT, 0, (LPARAM)copy))
        free(copy);
}

static int looks_like_question(const char *s)
{
    static const char *cues[] = {
        "?", "what", "how", "why", "when", "where", "who", "which",
        "can you", "could you", "do you", "would you", "tell me", "explain",
        "\xd0\xba\xd0\xb0\xd0\xba", "\xd1\x87\xd1\x82\xd0\xbe",
        "\xd0\xbf\xd0\xbe\xd1\x87\xd0\xb5\xd0\xbc\xd1\x83",
        "\xd0\xb7\xd0\xb0\xd1\x87\xd0\xb5\xd0\xbc",
        "\xd0\xba\xd0\xbe\xd0\xb3\xd0\xb4\xd0\xb0",
        "\xd0\xb3\xd0\xb4\xd0\xb5"
    };
    char lower[1024];
    size_t i;
    int k;

    for (i = 0; i < sizeof(lower) - 1 && s[i]; ++i)
        lower[i] = (char)tolower((unsigned char)s[i]);
    lower[i] = '\0';

    for (k = 0; k < (int)(sizeof(cues) / sizeof(cues[0])); ++k)
        if (strstr(lower, cues[k])) return 1;
    return 0;
}

static void handle_transcript(char *text)
{
    config_t cfg;
    char *tail;

    if (!text) return;
    app_get_config(&cfg);

    {
        char clip[192];
        LOGI("transcript += '%s'", utf8_clip(clip, sizeof(clip), text, 160));
    }
    transcript_add(g.tr, text);

    tail = transcript_tail(g.tr, 3);
    overlay_set_transcript(g.ovl, tail);
    free(tail);

    if (g.answer_on_next_transcript) {
        g.answer_on_next_transcript = 0;
        free(text);
        app_trigger_answer();
        update_status_line();
        return;
    }

    if (cfg.auto_respond && app_listening()) {
        unsigned long long since = now_ms() - g.last_answer_ms;
        int long_enough = (int)strlen(text) >= cfg.auto_min_chars;
        int cooled      = since >= (unsigned long long)cfg.auto_cooldown_ms;
        int question_ok = !cfg.auto_require_question || looks_like_question(text);

        /* Utterances now arrive every couple of seconds, which is faster than
         * the model can finish talking. Queuing a new request cancels the one
         * in flight, so auto-answering on every utterance would leave the
         * overlay showing nothing but truncated openings. Let the current
         * answer finish; the listen-now hotkey still forces a fresh one. */
        if (long_enough && cooled && question_ok && !g.llm_active)
            app_trigger_answer();
    }
    free(text);
    update_status_line();
}


/* ------------------------------------------------------- reasoning filter --
 * Reasoning models on OpenAI-compatible gateways stream their scratchpad as
 * ordinary content wrapped in <think>...</think>. Drop those spans on the way
 * to the overlay. Tags can straddle two deltas, so an unresolved tail that is
 * still a possible tag prefix is held back rather than printed.
 */
#define THINK_OPEN  "<think>"
#define THINK_CLOSE "</think>"

static sb_t g_ans_tail;
static sb_t g_answer_raw;      /* model text with reasoning already stripped */
static int  g_in_think;

/* The model answers in three labelled sections; find one at a line start. */
static const char *marker_at_line(const char *base, const char *m)
{
    size_t ml = strlen(m);
    const char *p = base;
    while ((p = strstr(p, m)) != NULL) {
        if (p == base || p[-1] == '\n') return p;
        p += ml;
    }
    return NULL;
}

static char *span_dup(const char *from, const char *to)
{
    size_t n;
    char *out;
    if (!from) return NULL;
    while (*from == ' ' || *from == '\r' || *from == '\n') from++;
    n = to ? (size_t)(to - from) : strlen(from);
    out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, from, n);
    out[n] = 0;
    str_trim(out);
    return out;
}

/* Re-split the accumulated answer and push each section into its own pane. */
static void panes_from_answer(void)
{
    const char *base = g_answer_raw.data ? g_answer_raw.data : "";
    const char *a = marker_at_line(base, "ANSWER:");
    const char *c = marker_at_line(base, "CONTEXT:");
    const char *b = marker_at_line(base, "BONUS:");
    char *sa, *sc, *sb;

    if (!a && !c && !b) {
        /* No markers yet, or the model ignored them. Stage two must not land on
         * top of the answer it was asked to elaborate on. */
        overlay_set_pane(g.ovl, g.llm_stage == 2 ? OVL_PANE_CONTEXT : OVL_PANE_ANSWER, base);
        return;
    }

    sa = a ? span_dup(a + 7, c ? c : b) : NULL;
    sc = c ? span_dup(c + 8, b)         : NULL;
    sb = b ? span_dup(b + 6, NULL)      : NULL;

    if (sa) { overlay_set_pane(g.ovl, OVL_PANE_ANSWER,  sa); free(sa); }
    if (sc) { overlay_set_pane(g.ovl, OVL_PANE_CONTEXT, sc); free(sc); }
    if (sb) { overlay_set_pane(g.ovl, OVL_PANE_BONUS,   sb); free(sb); }
}

static void think_reset(void)
{
    sb_free(&g_ans_tail);
    sb_init(&g_ans_tail);
    sb_free(&g_answer_raw);
    sb_init(&g_answer_raw);
    g_in_think = 0;
}

/* longest suffix of buf that is a proper prefix of tag */
static size_t partial_tag(const char *buf, size_t len, const char *tag)
{
    size_t tlen = strlen(tag);
    size_t k = len < tlen - 1 ? len : tlen - 1;
    for (; k > 0; --k)
        if (memcmp(buf + len - k, tag, k) == 0) return k;
    return 0;
}

static void tail_consume(size_t n)
{
    if (n >= g_ans_tail.len) { g_ans_tail.len = 0; }
    else {
        memmove(g_ans_tail.data, g_ans_tail.data + n, g_ans_tail.len - n);
        g_ans_tail.len -= n;
    }
    if (g_ans_tail.data) g_ans_tail.data[g_ans_tail.len] = '\0';
}

static void emit_answer(const char *text)
{
    if (!text || !*text) return;
    if (!sb_puts(&g_ans_tail, text)) return;

    for (;;) {
        char *p;
        size_t keep;

        if (!g_ans_tail.data || !g_ans_tail.len) break;

        if (!g_in_think) {
            p = strstr(g_ans_tail.data, THINK_OPEN);
            if (p) {
                size_t before = (size_t)(p - g_ans_tail.data);
                if (before) sb_append(&g_answer_raw, g_ans_tail.data, before);
                tail_consume(before + strlen(THINK_OPEN));
                g_in_think = 1;
                continue;
            }
            keep = partial_tag(g_ans_tail.data, g_ans_tail.len, THINK_OPEN);
            if (g_ans_tail.len > keep) {
                sb_append(&g_answer_raw, g_ans_tail.data, g_ans_tail.len - keep);
                tail_consume(g_ans_tail.len - keep);
            }
            break;
        }

        p = strstr(g_ans_tail.data, THINK_CLOSE);
        if (p) {
            tail_consume((size_t)(p - g_ans_tail.data) + strlen(THINK_CLOSE));
            g_in_think = 0;
            continue;
        }
        keep = partial_tag(g_ans_tail.data, g_ans_tail.len, THINK_CLOSE);
        if (g_ans_tail.len > keep) tail_consume(g_ans_tail.len - keep);
        break;
    }
}

static void handle_llm_event(int event, char *text)
{
    switch (event) {
    case LLM_EVT_START:
        g.llm_active = 1;
        g.last_error[0] = '\0';
        /* Stage two adds to the answer already on screen, so only a fresh
         * question wipes the panes. */
        if (g.llm_stage != 2) overlay_begin_answer(g.ovl);
        else                  overlay_clear_details(g.ovl);
        overlay_set_busy(g.ovl, 1);
        think_reset();
        break;
    case LLM_EVT_DELTA:
        emit_answer(text);
        panes_from_answer();
        break;
    case LLM_EVT_ERROR:
        snprintf(g.last_error, sizeof(g.last_error), "%s", text ? text : "LLM error");
        overlay_append_answer(g.ovl, "\r\n[error] ");
        overlay_append_answer(g.ovl, text ? text : "unknown");
        break;
    case LLM_EVT_DONE: {
        int stage = g.llm_stage;

        g.llm_active = 0;
        /* flush whatever was held back as a possible tag prefix */
        if (!g_in_think && g_ans_tail.data && g_ans_tail.len)
            sb_append(&g_answer_raw, g_ans_tail.data, g_ans_tail.len);
        panes_from_answer();
        {
            char clip[256];
            LOGI("llm: stage %d complete (%u chars) '%s'", stage,
                 (unsigned)g_answer_raw.len,
                 utf8_clip(clip, sizeof(clip), g_answer_raw.data ? g_answer_raw.data : "", 200));
        }

        if (stage == 1 && !g.last_error[0]) {
            /* Keep the short answer, then go back for the details. */
            free(g.stage1_answer);
            g.stage1_answer = xstrdup(g_answer_raw.data ? g_answer_raw.data : "");
            think_reset();
            trigger_stage_two();
            update_status_line();
            free(text);
            return;
        }

        if (g_pending_question) {
            const char *ans = (stage == 2 && g.stage1_answer) ? g.stage1_answer
                                                              : g_answer_raw.data;
            qa_push(g_pending_question, ans ? ans : "");
        }
        g.llm_stage = 0;
        think_reset();
        overlay_set_busy(g.ovl, 0);
        break;
    }
    }
    free(text);
    update_status_line();
}

/* ------------------------------------------------------------ apply config */

static int stt_settings_changed(const config_t *a, const config_t *b)
{
    return a->stt_backend != b->stt_backend
        || strcmp(a->whisper_model, b->whisper_model)
        || a->whisper_threads != b->whisper_threads
        || a->whisper_gpu != b->whisper_gpu
        || strcmp(a->whisper_language, b->whisper_language)
        || a->whisper_translate != b->whisper_translate
        || strcmp(a->stt_endpoint, b->stt_endpoint)
        || strcmp(a->stt_api_key, b->stt_api_key)
        || strcmp(a->stt_model, b->stt_model);
}

static int audio_settings_changed(const config_t *a, const config_t *b)
{
    return a->capture_mic != b->capture_mic
        || a->capture_loopback != b->capture_loopback
        || strcmp(a->mic_device_id, b->mic_device_id)
        || strcmp(a->sys_device_id, b->sys_device_id);
}

int app_apply_config(const config_t *in, char *msg, size_t cap)
{
    config_t old, next = *in;
    int restart_stt, restart_audio;

    app_get_config(&old);
    restart_stt   = stt_settings_changed(&old, in);
    restart_audio = audio_settings_changed(&old, in);

    /* If the geometry fields were not edited, honour where the user dragged
     * the overlay instead of snapping it back to the saved rectangle. */
    if (in->overlay_x == old.overlay_x && in->overlay_y == old.overlay_y &&
        in->overlay_w == old.overlay_w && in->overlay_h == old.overlay_h) {
        RECT r;
        overlay_get_rect(g.ovl, &r);
        if (r.right > r.left && r.bottom > r.top) {
            next.overlay_x = r.left;
            next.overlay_y = r.top;
            next.overlay_w = r.right - r.left;
            next.overlay_h = r.bottom - r.top;
        }
    }

    EnterCriticalSection(&g.cfg_cs);
    g.cfg = next;
    g.cfg.overlay_visible = overlay_is_visible(g.ovl);
    LeaveCriticalSection(&g.cfg_cs);

    if (restart_audio) {
        audio_stop(g.audio);
        audio_start(g.audio, &next, on_segment, NULL);
    } else {
        audio_update(g.audio, &next);
    }

    if (restart_stt) {
        stt_worker_stop(g.stt);
        stt_worker_start(g.stt, &next, on_transcript, NULL);
    }

    overlay_apply_config(g.ovl, &next);
    g.last_error[0] = '\0';

    if (msg && cap) msg[0] = '\0';
    register_hotkeys(&next, msg, cap);

    {
        config_t save;
        app_get_config(&save);
        if (config_save(&save) != 0 && msg && cap && !msg[0])
            snprintf(msg, cap, "Applied, but the config file could not be written.");
    }
    update_status_line();
    return 0;
}

/* -------------------------------------------------------------------- tray */

static void show_tray_menu(void)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    UINT cmd;

    AppendMenuW(m, MF_STRING | (overlay_is_visible(g.ovl) ? MF_CHECKED : 0),
                IDM_SHOW_OVERLAY, L"Show overlay");
    AppendMenuW(m, MF_STRING | (app_listening() ? MF_CHECKED : 0),
                IDM_LISTEN, L"Listening");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_RESPOND, L"Answer now");
    AppendMenuW(m, MF_STRING, IDM_CLEAR, L"Clear transcript");
    AppendMenuW(m, MF_STRING, IDM_SNIP, L"Screenshot a region and ask");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(m, MF_STRING, IDM_OPEN_LOG, L"Open log folder");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");

    GetCursorPos(&pt);
    SetForegroundWindow(g.hwnd);
    cmd = (UINT)TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                               pt.x, pt.y, 0, g.hwnd, NULL);
    DestroyMenu(m);
    if (cmd) PostMessageW(g.hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

static void open_log_folder(void)
{
    char dir[1024];
    if (app_data_path("", dir, sizeof(dir)) == 0) {
        wchar_t *w = utf8_to_wide(dir);
        if (w) { ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL); free(w); }
    }
}

/* -------------------------------------------------------------- window proc */

static void persist_geometry(void)
{
    RECT r;
    if (!g.ovl) return;
    overlay_get_rect(g.ovl, &r);
    EnterCriticalSection(&g.cfg_cs);
    g.cfg.overlay_x = r.left;
    g.cfg.overlay_y = r.top;
    g.cfg.overlay_w = r.right - r.left;
    g.cfg.overlay_h = r.bottom - r.top;
    g.cfg.overlay_visible = overlay_is_visible(g.ovl);
    LeaveCriticalSection(&g.cfg_cs);
}

static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) show_tray_menu();
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) settings_show(g.inst);
        return 0;

    case WM_APP_STT:
        handle_transcript((char *)lp);
        return 0;

    case WM_APP_LLM:
        handle_llm_event((int)wp, (char *)lp);
        return 0;

    case WM_HOTKEY:
        switch ((int)wp) {
        case HK_RESPOND: app_trigger_answer(); break;
        case HK_OVERLAY: app_toggle_overlay(); break;
        case HK_CLEAR:   app_clear_transcript(); break;
        case HK_LISTEN:  app_set_listening(!app_listening()); break;
        case HK_SNIP:    app_snip_and_ask(); break;
        /* The wheel over a pane works too (see mouse_proc); these cover the
         * case where the mouse is busy elsewhere. */
        case HK_SCROLL_UP:   overlay_scroll(g.ovl,  1); break;
        case HK_SCROLL_DOWN: overlay_scroll(g.ovl, -1); break;
        case HK_PANE_NEXT:   overlay_focus_next(g.ovl); break;
        case HK_MOVE:        app_toggle_move_mode(); break;
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_STATUS) update_status_line();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SHOW_OVERLAY: app_toggle_overlay(); break;
        case IDM_LISTEN:       app_set_listening(!app_listening()); break;
        case IDM_RESPOND:      app_trigger_answer(); break;
        case IDM_CLEAR:        app_clear_transcript(); break;
        case IDM_SNIP:         app_snip_and_ask(); break;
        case IDM_SETTINGS:     settings_show(g.inst); break;
        case IDM_OPEN_LOG:     open_log_folder(); break;
        case IDM_EXIT:         DestroyWindow(h); break;
        }
        return 0;

    case WM_QUERYENDSESSION:
        persist_geometry();
        { config_t c; app_get_config(&c); config_save(&c); }
        return TRUE;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* -------------------------------------------------------------------- main */

static int create_main_window(void)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = main_proc;
    wc.hInstance     = g.inst;
    wc.lpszClassName = WND_CLASS;
    if (!RegisterClassExW(&wc)) return -1;

    g.hwnd = CreateWindowExW(0, WND_CLASS, L"call_assist", 0,
                             0, 0, 0, 0, HWND_MESSAGE, NULL, g.inst, NULL);
    return g.hwnd ? 0 : -1;
}

static int create_tray(void)
{
    memset(&g.nid, 0, sizeof(g.nid));
    g.nid.cbSize           = sizeof(g.nid);
    g.nid.hWnd             = g.hwnd;
    g.nid.uID              = TRAY_UID;
    g.nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g.nid.uCallbackMessage = WM_APP_TRAY;
    g.nid.hIcon            = g.icon;
    wcscpy(g.nid.szTip, L"call_assist");
    return Shell_NotifyIconW(NIM_ADD, &g.nid) ? 0 : -1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    MSG msg;
    INITCOMMONCONTROLSEX icc;
    HANDLE once;
    int first_run;

    (void)prev; (void)cmdline; (void)show;

    once = CreateMutexW(NULL, TRUE, L"Local\\CallAssistSingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"call_assist is already running (look in the tray).",
                    L"call_assist", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    g.inst = inst;
    log_init();
    InitializeCriticalSection(&g.cfg_cs);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES |
                 ICC_PROGRESS_CLASS | ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    first_run = config_load(&g.cfg) != 0;
    InterlockedExchange(&g.listening, g.cfg.listening ? 1 : 0);

    g.icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                               GetSystemMetrics(SM_CXSMICON),
                               GetSystemMetrics(SM_CYSMICON), 0);
    if (!g.icon) g.icon = LoadIconW(NULL, IDI_APPLICATION);

    if (create_main_window() != 0) {
        MessageBoxW(NULL, L"Could not create the main window.", L"call_assist", MB_ICONERROR);
        return 1;
    }
    if (create_tray() != 0) LOGW("tray icon could not be created");

    g.tr  = transcript_create();
    g.llm = llm_create();
    g.stt = stt_worker_create();
    g.ovl = overlay_create(inst, &g.cfg);
    g.audio = audio_create();

    if (!g.tr || !g.llm || !g.stt || !g.ovl || !g.audio) {
        MessageBoxW(NULL, L"Initialisation failed - see call_assist.log in %APPDATA%\\CallAssist.",
                    L"call_assist", MB_ICONERROR);
        return 1;
    }

    stt_worker_start(g.stt, &g.cfg, on_transcript, NULL);
    audio_start(g.audio, &g.cfg, on_segment, NULL);
    {
        char hkmsg[256] = "";
        if (register_hotkeys(&g.cfg, hkmsg, sizeof(hkmsg)) && hkmsg[0])
            LOGW("%s", hkmsg);
    }
    apply_mouse_binding(g.cfg.trigger_mouse_button);
    SetTimer(g.hwnd, TIMER_STATUS, 700, NULL);
    update_status_line();

    if (first_run) {
        overlay_set_answer(g.ovl,
            "Welcome. Open Settings (tray icon) and set:\r\n"
            "  1. a Whisper model file, or the remote STT endpoint\r\n"
            "  2. your LLM endpoint, API key and model\r\n"
            "Then talk - answers appear here.");
        settings_show(inst);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        HWND dlg = settings_hwnd();
        if (dlg && IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    persist_geometry();
    { config_t c; app_get_config(&c); config_save(&c); }

    unregister_hotkeys();
    apply_mouse_binding(0);
    install_mouse_hook(0);
    qa_clear();
    free(g_pending_question);
    free(g.stage1_answer);
    Shell_NotifyIconW(NIM_DELETE, &g.nid);

    audio_stop(g.audio);
    audio_destroy(g.audio);
    stt_worker_destroy(g.stt);
    llm_destroy(g.llm);
    overlay_destroy(g.ovl);
    transcript_destroy(g.tr);

    DeleteCriticalSection(&g.cfg_cs);
    CoUninitialize();
    LOGI("---- call_assist exited ----");
    log_shutdown();
    if (once) CloseHandle(once);
    return 0;
}
