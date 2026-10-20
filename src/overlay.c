#include "overlay.h"
#include "util.h"
#include "log.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OVERLAY_CLASS   L"CallAssistOverlay"
#define WM_OVL_REFRESH  (WM_USER + 11)
#define PAD             12
#define GRIP            16
#define TIMER_SPIN      1
#define SPIN_MS         500
#define DOT_D           9      /* status dot diameter, logical px */

/* --------------------------------------------------------- wide text buffer */

typedef struct {
    wchar_t *d;
    size_t   len, cap;
} wbuf_t;

static void wbuf_clear(wbuf_t *b) { b->len = 0; if (b->d) b->d[0] = 0; }

static void wbuf_free(wbuf_t *b) { free(b->d); b->d = NULL; b->len = b->cap = 0; }

static void wbuf_append(wbuf_t *b, const wchar_t *s)
{
    size_t n = s ? wcslen(s) : 0;
    if (!n) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 512;
        wchar_t *p;
        while (cap < b->len + n + 1) cap *= 2;
        p = (wchar_t *)realloc(b->d, cap * sizeof(wchar_t));
        if (!p) return;
        b->d = p;
        b->cap = cap;
    }
    memcpy(b->d + b->len, s, n * sizeof(wchar_t));
    b->len += n;
    b->d[b->len] = 0;
}

static void wbuf_set(wbuf_t *b, const wchar_t *s) { wbuf_clear(b); wbuf_append(b, s); }

/* ------------------------------------------------------------------ overlay */

struct overlay {
    HWND      hwnd;
    HINSTANCE inst;
    config_t  cfg;

    CRITICAL_SECTION cs;
    wbuf_t    status;
    wbuf_t    pane[OVL_PANE_COUNT];
    int       pane_scroll[OVL_PANE_COUNT];
    int       pane_max[OVL_PANE_COUNT];
    RECT      pane_rect[OVL_PANE_COUNT];   /* body area from the last paint */
    int       focus_pane;
    int       interactive;   /* move mode: ignore click-through for now */
    wbuf_t    tail;
    int       busy;
    int       spin;
    int       state;        /* OVL_* */

    HFONT     font_body;
    HFONT     font_small;
    int       font_px;

    int       dragging;
    POINT     drag_origin;
    RECT      drag_rect;

};

static void apply_click_through(overlay_t *o);

static const wchar_t *kPaneTitle[OVL_PANE_COUNT] = {
    L"HEARD", L"ANSWER", L"CONTEXT", L"COULD ADD"
};
/* Relative heights. The answer gets the most room; what was heard the least. */
static const int kPaneWeight[OVL_PANE_COUNT] = { 2, 4, 3, 3 };

typedef BOOL (WINAPI *SetWindowDisplayAffinity_t)(HWND, DWORD);

static void apply_display_affinity(HWND hwnd, int hide)
{
    static SetWindowDisplayAffinity_t fn;
    static int resolved;
    if (!resolved) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) fn = (SetWindowDisplayAffinity_t)(void *)GetProcAddress(u, "SetWindowDisplayAffinity");
        resolved = 1;
    }
    if (fn) {
        /* WDA_EXCLUDEFROMCAPTURE = 0x11 (Win10 2004+); WDA_NONE = 0 */
        if (!fn(hwnd, hide ? 0x11 : 0x00) && hide)
            fn(hwnd, 0x01);                  /* WDA_MONITOR fallback */
    }
}

static void rebuild_fonts(overlay_t *o, HDC dc)
{
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    int px  = -MulDiv(o->cfg.overlay_font_size, dpi, 72);
    if (o->font_body && px == o->font_px) return;

    if (o->font_body)  DeleteObject(o->font_body);
    if (o->font_small) DeleteObject(o->font_small);
    o->font_px = px;

    o->font_body = CreateFontW(px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, FF_DONTCARE, L"Segoe UI");
    o->font_small = CreateFontW(-MulDiv(o->cfg.overlay_font_size - 2 < 7 ? 7
                                        : o->cfg.overlay_font_size - 2, dpi, 72),
                                0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, FF_DONTCARE, L"Segoe UI");
}

static COLORREF dim(COLORREF c, double f)
{
    int r = (int)(GetRValue(c) * f), g = (int)(GetGValue(c) * f), b = (int)(GetBValue(c) * f);
    return RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}

/* Colour of the status dot for the current state. */
static COLORREF state_color(const overlay_t *o)
{
    switch (o->state) {
    case OVL_LISTENING:    return RGB(0x3D, 0xDC, 0x84);   /* green  */
    case OVL_TRANSCRIBING: return RGB(0xFF, 0xB4, 0x54);   /* amber  */
    case OVL_THINKING:     return o->cfg.overlay_accent;   /* accent */
    default:               return RGB(0x8A, 0x8F, 0x98);   /* grey   */
    }
}

/* Filled dot while active, hollow ring while paused; the active dot breathes
 * so a frozen overlay is visibly different from a listening one. */
static void draw_status_dot(const overlay_t *o, HDC dc, int x, int cy)
{
    COLORREF c = state_color(o);
    int      live = (o->state != OVL_PAUSED);
    HBRUSH   br;
    HPEN     pen;
    HGDIOBJ  ob, op;

    if (live && (o->spin & 1)) c = dim(c, 0.62);

    br  = CreateSolidBrush(live ? c : o->cfg.overlay_bg);
    pen = CreatePen(PS_SOLID, 1, c);
    ob  = SelectObject(dc, br);
    op  = SelectObject(dc, pen);
    Ellipse(dc, x, cy - DOT_D / 2, x + DOT_D, cy - DOT_D / 2 + DOT_D);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void paint(overlay_t *o, HDC target, const RECT *client)
{
    const int w = client->right, h = client->bottom;
    HDC     mem = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, w, h);
    HGDIOBJ oldbmp = SelectObject(mem, bmp);
    HBRUSH  bg = CreateSolidBrush(o->cfg.overlay_bg);
    HPEN    pen = CreatePen(PS_SOLID, 1, dim(o->cfg.overlay_accent, 0.55));
    HGDIOBJ oldpen, oldbrush;
    RECT    r = *client;
    int     y = PAD;
    int     tail_h = 0;

    rebuild_fonts(o, mem);

    FillRect(mem, &r, bg);
    oldpen   = SelectObject(mem, pen);
    oldbrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
    Rectangle(mem, 0, 0, w, h);
    SelectObject(mem, oldbrush);
    SelectObject(mem, oldpen);
    DeleteObject(pen);

    SetBkMode(mem, TRANSPARENT);

    EnterCriticalSection(&o->cs);

    /* ---- status row: state dot + text ---- */
    {
        wchar_t line[512];
        RECT sr;
        int   line_h;

        SelectObject(mem, o->font_small);
        SetTextColor(mem, o->cfg.overlay_accent);
        if (o->busy) {
            static const wchar_t *spin[] = { L"", L".", L"..", L"..." };
            _snwprintf(line, 511, L"%s%s", o->status.d ? o->status.d : L"",
                       spin[(o->spin >> 1) & 3]);
        } else {
            _snwprintf(line, 511, L"%s", o->status.d ? o->status.d : L"");
        }
        line[511] = 0;

        sr.left = PAD + DOT_D + 7; sr.top = y; sr.right = w - PAD; sr.bottom = y + 200;
        line_h = DrawTextW(mem, line, -1, &sr,
                           DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        draw_status_dot(o, mem, PAD, y + line_h / 2);
        y += line_h + 6;
    }

    /* ---- transcript tail (bottom strip) ---- */
    if (o->cfg.overlay_show_transcript && o->tail.d && o->tail.len) {
        RECT tr;
        int  hh;
        SelectObject(mem, o->font_small);
        tr.left = PAD; tr.top = 0; tr.right = w - PAD; tr.bottom = 0;
        hh = DrawTextW(mem, o->tail.d, -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_LEFT);
        if (hh > (h / 3)) hh = h / 3;
        tail_h = hh + 8;

        {
            RECT sep;
            HBRUSH sb = CreateSolidBrush(dim(o->cfg.overlay_fg, 0.22));
            sep.left = PAD; sep.right = w - PAD;
            sep.top = h - PAD - tail_h; sep.bottom = sep.top + 1;
            FillRect(mem, &sep, sb);
            DeleteObject(sb);
        }
        tr.left = PAD; tr.right = w - PAD;
        tr.top = h - PAD - tail_h + 6; tr.bottom = h - PAD;
        SetTextColor(mem, dim(o->cfg.overlay_fg, 0.55));
        DrawTextW(mem, o->tail.d, -1, &tr,
                  DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    /* ---- four panes, each scrolling on its own ---- */
    {
        int top    = y;
        int bottom = h - PAD - (tail_h ? tail_h + 4 : 0);
        int total  = bottom - top;
        int wsum = 0, i, cur = top;

        for (i = 0; i < OVL_PANE_COUNT; ++i) wsum += kPaneWeight[i];
        if (total < OVL_PANE_COUNT * 30) total = OVL_PANE_COUNT * 30;

        for (i = 0; i < OVL_PANE_COUNT; ++i) {
            int      ph = (i == OVL_PANE_COUNT - 1) ? (top + total - cur)
                                                    : total * kPaneWeight[i] / wsum;
            RECT     ar, calc;
            int      avail, need, title_h;
            const wchar_t *txt = (o->pane[i].d && o->pane[i].len) ? o->pane[i].d : L"";

            /* title */
            SelectObject(mem, o->font_small);
            SetTextColor(mem, i == o->focus_pane ? o->cfg.overlay_accent
                                                 : dim(o->cfg.overlay_accent, 0.55));
            {
                RECT tr2;
                tr2.left = PAD; tr2.right = w - PAD; tr2.top = cur; tr2.bottom = cur + 200;
                title_h = DrawTextW(mem, kPaneTitle[i], -1, &tr2,
                                    DT_LEFT | DT_TOP | DT_SINGLELINE);
            }
            {   /* hairline under the title */
                RECT sep;
                HBRUSH sb = CreateSolidBrush(dim(o->cfg.overlay_fg, 0.18));
                sep.left = PAD; sep.right = w - PAD;
                sep.top = cur + title_h + 1; sep.bottom = sep.top + 1;
                FillRect(mem, &sep, sb);
                DeleteObject(sb);
            }

            ar.left = PAD; ar.right = w - PAD - 6;
            ar.top  = cur + title_h + 5;
            ar.bottom = cur + ph - 4;
            if (ar.bottom < ar.top + 10) ar.bottom = ar.top + 10;
            o->pane_rect[i] = ar;

            avail = ar.bottom - ar.top;
            SelectObject(mem, o->font_body);
            SetTextColor(mem, o->cfg.overlay_fg);

            calc = ar;
            need = DrawTextW(mem, txt, -1, &calc,
                             DT_CALCRECT | DT_WORDBREAK | DT_LEFT | DT_NOPREFIX);

            if (need > avail) {
                HRGN clip = CreateRectRgn(ar.left, ar.top, ar.right, ar.bottom);
                RECT sc = ar;
                int  overflow = need - avail;

                o->pane_max[i] = overflow;
                if (o->pane_scroll[i] > overflow) o->pane_scroll[i] = overflow;
                if (o->pane_scroll[i] < 0)        o->pane_scroll[i] = 0;

                SelectClipRgn(mem, clip);
                /* Scroll counts down from the start, so 0 holds the view at the
                 * first line and streaming text grows out of sight below rather
                 * than dragging the reader along with it. */
                sc.top    = ar.top - o->pane_scroll[i];
                sc.bottom = sc.top + need;
                DrawTextW(mem, txt, -1, &sc, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
                SelectClipRgn(mem, NULL);
                DeleteObject(clip);

                {   /* thumb showing position within this pane */
                    int track = avail;
                    int th    = track * avail / need;
                    int ty;
                    HBRUSH tb;
                    RECT thumb;
                    if (th < 10) th = 10;
                    ty = ar.top + (track - th) * o->pane_scroll[i] / overflow;
                    tb = CreateSolidBrush(dim(o->cfg.overlay_accent,
                                              i == o->focus_pane ? 1.0 : 0.5));
                    thumb.left = w - PAD - 3; thumb.right = w - PAD;
                    thumb.top = ty; thumb.bottom = ty + th;
                    FillRect(mem, &thumb, tb);
                    DeleteObject(tb);
                }
            } else {
                o->pane_max[i] = 0;
                o->pane_scroll[i] = 0;
                DrawTextW(mem, txt, -1, &ar, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            }
            cur += ph;
        }
    }

    LeaveCriticalSection(&o->cs);

    /* ---- resize grip (only when the window is interactive) ---- */
    if (!o->cfg.overlay_click_through || o->interactive) {
        HBRUSH gb = CreateSolidBrush(dim(o->cfg.overlay_fg, 0.35));
        int i;
        for (i = 0; i < 3; ++i) {
            RECT g;
            g.right  = w - 4 - i * 4;
            g.bottom = h - 4;
            g.left   = g.right - 2;
            g.top    = g.bottom - 2 - i * 4;
            FillRect(mem, &g, gb);
        }
        DeleteObject(gb);
    }

    BitBlt(target, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteObject(bg);
    DeleteDC(mem);
}

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    overlay_t *o = (overlay_t *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_OVL_REFRESH:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_SPIN && o) {
            EnterCriticalSection(&o->cs);
            o->spin++;
            LeaveCriticalSection(&o->cs);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (o) {
            POINT pt;
            int pane;
            pt.x = (int)(short)LOWORD(lp);
            pt.y = (int)(short)HIWORD(lp);
            pane = overlay_pane_at(o, pt);          /* lp is already screen space */
            if (pane < 0) pane = o->focus_pane;
            overlay_scroll_pane(o, pane, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        if (o) paint(o, dc, &rc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCHITTEST: {
        POINT pt;
        RECT  rc;
        if (!o || (o->cfg.overlay_click_through && !o->interactive)) return HTTRANSPARENT;
        pt.x = (int)(short)LOWORD(lp);
        pt.y = (int)(short)HIWORD(lp);
        GetWindowRect(hwnd, &rc);
        if (pt.x >= rc.right - GRIP && pt.y >= rc.bottom - GRIP) return HTBOTTOMRIGHT;
        return HTCAPTION;                       /* drag anywhere else */
    }

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

overlay_t *overlay_create(HINSTANCE inst, const config_t *cfg)
{
    overlay_t *o;
    WNDCLASSEXW wc;
    static int registered;

    o = (overlay_t *)calloc(1, sizeof(*o));
    if (!o) return NULL;
    InitializeCriticalSection(&o->cs);
    o->cfg  = *cfg;
    o->inst = inst;
    wbuf_set(&o->status, L"call_assist ready");

    if (!registered) {
        memset(&wc, 0, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = overlay_proc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = OVERLAY_CLASS;
        if (!RegisterClassExW(&wc)) {
            LOGE("overlay: RegisterClassEx failed (%lu)", (unsigned long)GetLastError());
            DeleteCriticalSection(&o->cs);
            free(o);
            return NULL;
        }
        registered = 1;
    }

    o->hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        OVERLAY_CLASS, L"call_assist", WS_POPUP,
        cfg->overlay_x, cfg->overlay_y, cfg->overlay_w, cfg->overlay_h,
        NULL, NULL, inst, o);
    if (!o->hwnd) {
        LOGE("overlay: CreateWindowEx failed (%lu)", (unsigned long)GetLastError());
        DeleteCriticalSection(&o->cs);
        free(o);
        return NULL;
    }

    overlay_apply_config(o, cfg);
    overlay_show(o, cfg->overlay_visible ? 1 : 0);
    return o;
}

void overlay_destroy(overlay_t *o)
{
    if (!o) return;
    KillTimer(o->hwnd, TIMER_SPIN);
    if (o->hwnd) DestroyWindow(o->hwnd);
    if (o->font_body)  DeleteObject(o->font_body);
    if (o->font_small) DeleteObject(o->font_small);
    wbuf_free(&o->status);
    { int i; for (i = 0; i < OVL_PANE_COUNT; ++i) wbuf_free(&o->pane[i]); }
    wbuf_free(&o->tail);
    DeleteCriticalSection(&o->cs);
    free(o);
}

void overlay_apply_config(overlay_t *o, const config_t *cfg)
{
    if (!o) return;

    EnterCriticalSection(&o->cs);
    o->cfg = *cfg;
    o->font_px = 0;                       /* force font rebuild */
    LeaveCriticalSection(&o->cs);

    apply_click_through(o);

    SetLayeredWindowAttributes(o->hwnd, 0, (BYTE)cfg->overlay_opacity, LWA_ALPHA);
    apply_display_affinity(o->hwnd, cfg->overlay_hide_from_capture);

    SetWindowPos(o->hwnd, HWND_TOPMOST,
                 cfg->overlay_x, cfg->overlay_y, cfg->overlay_w, cfg->overlay_h,
                 SWP_NOACTIVATE | (cfg->overlay_visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));

    /* the state dot only animates while the window is up */
    if (cfg->overlay_visible) SetTimer(o->hwnd, TIMER_SPIN, SPIN_MS, NULL);
    else                      KillTimer(o->hwnd, TIMER_SPIN);

    InvalidateRect(o->hwnd, NULL, FALSE);
}

void overlay_show(overlay_t *o, int show)
{
    if (!o) return;
    o->cfg.overlay_visible = show ? 1 : 0;
    ShowWindow(o->hwnd, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (show) {
        SetWindowPos(o->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetTimer(o->hwnd, TIMER_SPIN, SPIN_MS, NULL);
    } else {
        KillTimer(o->hwnd, TIMER_SPIN);
    }
}

int overlay_is_visible(overlay_t *o)
{
    return o ? (IsWindowVisible(o->hwnd) ? 1 : 0) : 0;
}

HWND overlay_hwnd(overlay_t *o) { return o ? o->hwnd : NULL; }

void overlay_get_rect(overlay_t *o, RECT *r)
{
    if (o && r) GetWindowRect(o->hwnd, r);
}

static void refresh(overlay_t *o)
{
    if (o && o->hwnd) PostMessageW(o->hwnd, WM_OVL_REFRESH, 0, 0);
}

void overlay_set_status(overlay_t *o, const char *utf8)
{
    wchar_t *w;
    if (!o) return;
    w = utf8_to_wide(utf8 ? utf8 : "");
    EnterCriticalSection(&o->cs);
    wbuf_set(&o->status, w ? w : L"");
    LeaveCriticalSection(&o->cs);
    free(w);
    refresh(o);
}

void overlay_set_busy(overlay_t *o, int busy)
{
    if (!o) return;
    EnterCriticalSection(&o->cs);
    o->busy = busy ? 1 : 0;
    LeaveCriticalSection(&o->cs);
    refresh(o);
}

void overlay_set_state(overlay_t *o, int state)
{
    int changed;
    if (!o) return;
    EnterCriticalSection(&o->cs);
    changed = (o->state != state);
    o->state = state;
    LeaveCriticalSection(&o->cs);
    if (changed) refresh(o);
}

void overlay_begin_answer(overlay_t *o)
{
    int i;
    if (!o) return;
    EnterCriticalSection(&o->cs);
    for (i = OVL_PANE_ANSWER; i < OVL_PANE_COUNT; ++i) {
        wbuf_clear(&o->pane[i]);
        o->pane_scroll[i] = 0;          /* re-pin to the newest text */
    }
    LeaveCriticalSection(&o->cs);
    refresh(o);
}

void overlay_clear_panes(overlay_t *o)
{
    int i;
    if (!o) return;
    EnterCriticalSection(&o->cs);
    for (i = 0; i < OVL_PANE_COUNT; ++i) { wbuf_clear(&o->pane[i]); o->pane_scroll[i] = 0; }
    LeaveCriticalSection(&o->cs);
    refresh(o);
}

void overlay_clear_details(overlay_t *o)
{
    int i;
    if (!o) return;
    EnterCriticalSection(&o->cs);
    for (i = OVL_PANE_CONTEXT; i < OVL_PANE_COUNT; ++i) {
        wbuf_clear(&o->pane[i]);
        o->pane_scroll[i] = 0;
    }
    LeaveCriticalSection(&o->cs);
    refresh(o);
}

void overlay_set_pane(overlay_t *o, int pane, const char *utf8)
{
    wchar_t *w;
    if (!o || pane < 0 || pane >= OVL_PANE_COUNT) return;
    w = utf8_to_wide(utf8 ? utf8 : "");
    EnterCriticalSection(&o->cs);
    wbuf_set(&o->pane[pane], w ? w : L"");
    LeaveCriticalSection(&o->cs);
    free(w);
    refresh(o);
}

void overlay_scroll_pane(overlay_t *o, int pane, int notches)
{
    if (!o || pane < 0 || pane >= OVL_PANE_COUNT) return;
    EnterCriticalSection(&o->cs);
    if (notches == 0) {
        o->pane_scroll[pane] = 0;               /* back to the first line */
    } else {
        int step = (o->font_px > 0 ? o->font_px : 16) * 3;
        o->pane_scroll[pane] -= notches * step;  /* notches > 0 moves towards the top */
        if (o->pane_scroll[pane] > o->pane_max[pane]) o->pane_scroll[pane] = o->pane_max[pane];
        if (o->pane_scroll[pane] < 0)                 o->pane_scroll[pane] = 0;
    }
    LeaveCriticalSection(&o->cs);
    refresh(o);
}

void overlay_scroll(overlay_t *o, int notches)
{
    if (!o) return;
    overlay_scroll_pane(o, o->focus_pane, notches);
}

static void apply_click_through(overlay_t *o)
{
    LONG_PTR ex = GetWindowLongPtrW(o->hwnd, GWL_EXSTYLE);
    if (o->cfg.overlay_click_through && !o->interactive) ex |=  WS_EX_TRANSPARENT;
    else                                                 ex &= ~(LONG_PTR)WS_EX_TRANSPARENT;
    SetWindowLongPtrW(o->hwnd, GWL_EXSTYLE, ex);
}

void overlay_set_interactive(overlay_t *o, int on)
{
    if (!o) return;
    EnterCriticalSection(&o->cs);
    o->interactive = on ? 1 : 0;
    LeaveCriticalSection(&o->cs);
    apply_click_through(o);
    if (on) {
        /* A click-through window is also no-activate; bring it forward so the
         * drag lands on it rather than on whatever is underneath. */
        SetWindowPos(o->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    refresh(o);
}

int overlay_is_interactive(overlay_t *o)
{
    return o ? o->interactive : 0;
}

void overlay_focus_next(overlay_t *o)
{
    if (!o) return;
    EnterCriticalSection(&o->cs);
    o->focus_pane = (o->focus_pane + 1) % OVL_PANE_COUNT;
    LeaveCriticalSection(&o->cs);
    refresh(o);
}

int overlay_pane_at(overlay_t *o, POINT screen_pt)
{
    int i, hit = -1;
    if (!o) return -1;
    ScreenToClient(o->hwnd, &screen_pt);
    EnterCriticalSection(&o->cs);
    for (i = 0; i < OVL_PANE_COUNT; ++i) {
        const RECT *r = &o->pane_rect[i];
        if (screen_pt.y >= r->top - 18 && screen_pt.y <= r->bottom) { hit = i; break; }
    }
    LeaveCriticalSection(&o->cs);
    return hit;
}

void overlay_append_answer(overlay_t *o, const char *utf8)
{
    wchar_t *w;
    if (!o || !utf8 || !*utf8) return;
    w = utf8_to_wide(utf8);
    if (!w) return;
    EnterCriticalSection(&o->cs);
    wbuf_append(&o->pane[OVL_PANE_ANSWER], w);
    LeaveCriticalSection(&o->cs);
    free(w);
    refresh(o);
}

void overlay_set_answer(overlay_t *o, const char *utf8)
{
    wchar_t *w;
    if (!o) return;
    w = utf8_to_wide(utf8 ? utf8 : "");
    EnterCriticalSection(&o->cs);
    wbuf_set(&o->pane[OVL_PANE_ANSWER], w ? w : L"");
    LeaveCriticalSection(&o->cs);
    free(w);
    refresh(o);
}

void overlay_set_transcript(overlay_t *o, const char *utf8)
{
    wchar_t *w;
    if (!o) return;
    w = utf8_to_wide(utf8 ? utf8 : "");
    EnterCriticalSection(&o->cs);
    wbuf_set(&o->tail, w ? w : L"");
    LeaveCriticalSection(&o->cs);
    free(w);
    refresh(o);
}
