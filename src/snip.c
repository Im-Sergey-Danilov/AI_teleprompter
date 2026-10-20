#include "snip.h"
#include "log.h"
#include "util.h"

#include <windows.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNIP_CLASS L"CallAssistSnip"

/* Declared locally so the build depends on neither INITGUID nor uuid.lib. */
static const CLSID CLSID_WICImagingFactory_ =
    {0xCACAF262,0x9370,0x4615,{0xA1,0x3B,0x9F,0x55,0x39,0xDA,0x4C,0x0A}};
static const IID IID_IWICImagingFactory_ =
    {0xEC5EC8A9,0xC395,0x4314,{0x9C,0x77,0x54,0xD7,0xA9,0x35,0xFF,0x70}};
static const GUID GUID_ContainerFormatPng_ =
    {0x1B7CFAF4,0x713F,0x473C,{0xBB,0xCD,0x61,0x37,0x42,0x5F,0xAE,0xAF}};
static const GUID GUID_WICPixelFormat32bppBGRA_ =
    {0x6FDDC324,0x4E03,0x4BFE,{0xB1,0x85,0x3D,0x77,0x76,0x8D,0xC9,0x00}};

/* ------------------------------------------------------------------ base64 */

char *base64_encode(const unsigned char *data, size_t n)
{
    DWORD len = 0;
    char *out;

    if (!data || !n) return NULL;
    if (!CryptBinaryToStringA(data, (DWORD)n,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &len))
        return NULL;
    out = (char *)malloc((size_t)len + 1);
    if (!out) return NULL;
    if (!CryptBinaryToStringA(data, (DWORD)n,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &len)) {
        free(out);
        return NULL;
    }
    out[len] = 0;
    return out;
}

/* --------------------------------------------------------------- png encode */

static int encode_png(const void *bgra, int w, int h,
                      unsigned char **png, size_t *png_len)
{
    IWICImagingFactory    *fac = NULL;
    IWICBitmapEncoder     *enc = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2         *opts = NULL;
    IStream               *stream = NULL;
    HGLOBAL                hg = NULL;
    GUID                   fmt = GUID_WICPixelFormat32bppBGRA_;
    HRESULT                hr;
    int                    rc = -1;

    *png = NULL;
    *png_len = 0;

    hr = CoCreateInstance(&CLSID_WICImagingFactory_, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory_, (void **)&fac);
    if (FAILED(hr)) {
        LOGE("snip: WIC factory failed (0x%08lX)", (unsigned long)hr);
        return -1;
    }

    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream))) goto done;
    if (FAILED(IWICImagingFactory_CreateEncoder(fac, &GUID_ContainerFormatPng_, NULL, &enc))) goto done;
    if (FAILED(IWICBitmapEncoder_Initialize(enc, stream, WICBitmapEncoderNoCache))) goto done;
    if (FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &frame, &opts))) goto done;
    if (FAILED(IWICBitmapFrameEncode_Initialize(frame, opts))) goto done;
    if (FAILED(IWICBitmapFrameEncode_SetSize(frame, (UINT)w, (UINT)h))) goto done;
    if (FAILED(IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt))) goto done;
    if (FAILED(IWICBitmapFrameEncode_WritePixels(frame, (UINT)h, (UINT)(w * 4),
                                                 (UINT)(w * 4 * h), (BYTE *)bgra))) goto done;
    if (FAILED(IWICBitmapFrameEncode_Commit(frame))) goto done;
    if (FAILED(IWICBitmapEncoder_Commit(enc))) goto done;

    if (FAILED(GetHGlobalFromStream(stream, &hg))) goto done;
    {
        SIZE_T n = GlobalSize(hg);
        void  *p = GlobalLock(hg);
        if (p && n) {
            *png = (unsigned char *)malloc(n);
            if (*png) {
                memcpy(*png, p, n);
                *png_len = (size_t)n;
                rc = 0;
            }
        }
        if (p) GlobalUnlock(hg);
    }

done:
    if (opts)   IPropertyBag2_Release(opts);
    if (frame)  IWICBitmapFrameEncode_Release(frame);
    if (enc)    IWICBitmapEncoder_Release(enc);
    if (stream) IStream_Release(stream);
    if (fac)    IWICImagingFactory_Release(fac);
    return rc;
}

/* -------------------------------------------------------- selection chrome */

typedef struct {
    HWND  hwnd;
    int   dragging, done, cancelled;
    POINT a, b;
} snip_state_t;

static void norm_rect(const snip_state_t *s, RECT *r)
{
    r->left   = s->a.x < s->b.x ? s->a.x : s->b.x;
    r->right  = s->a.x > s->b.x ? s->a.x : s->b.x;
    r->top    = s->a.y < s->b.y ? s->a.y : s->b.y;
    r->bottom = s->a.y > s->b.y ? s->a.y : s->b.y;
}

static LRESULT CALLBACK snip_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    snip_state_t *s = (snip_state_t *)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_CROSS));
        return TRUE;

    case WM_LBUTTONDOWN:
        if (s) {
            s->dragging = 1;
            s->a.x = (int)(short)LOWORD(lp);
            s->a.y = (int)(short)HIWORD(lp);
            s->b = s->a;
            SetCapture(h);
            InvalidateRect(h, NULL, TRUE);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (s && s->dragging) {
            s->b.x = (int)(short)LOWORD(lp);
            s->b.y = (int)(short)HIWORD(lp);
            InvalidateRect(h, NULL, TRUE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (s && s->dragging) {
            s->dragging = 0;
            s->b.x = (int)(short)LOWORD(lp);
            s->b.y = (int)(short)HIWORD(lp);
            ReleaseCapture();
            s->done = 1;
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && s) { s->cancelled = 1; s->done = 1; }
        return 0;

    case WM_RBUTTONDOWN:
        if (s) { s->cancelled = 1; s->done = 1; }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC  dc = BeginPaint(h, &ps);
        RECT rc;

        GetClientRect(h, &rc);
        {
            HBRUSH b = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(dc, &rc, b);
            DeleteObject(b);
        }
        if (s && (s->dragging || s->a.x != s->b.x)) {
            RECT   sel;
            HBRUSH hole = CreateSolidBrush(RGB(40, 40, 40));
            HPEN   pen  = CreatePen(PS_SOLID, 2, RGB(120, 180, 255));
            HGDIOBJ op;

            norm_rect(s, &sel);
            FillRect(dc, &sel, hole);       /* lighter, so the pick is visible */
            op = SelectObject(dc, pen);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, sel.left, sel.top, sel.right, sel.bottom);
            SelectObject(dc, op);
            DeleteObject(pen);
            DeleteObject(hole);
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int snip_region(HINSTANCE inst, unsigned char **png, size_t *png_len)
{
    static int   registered;
    snip_state_t st;
    WNDCLASSEXW  wc;
    MSG          msg;
    RECT         sel;
    int          vx, vy, vw, vh;
    int          rc = -1;

    *png = NULL;
    *png_len = 0;

    if (!registered) {
        memset(&wc, 0, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = snip_proc;
        wc.hInstance     = inst;
        wc.lpszClassName = SNIP_CLASS;
        wc.hCursor       = LoadCursorW(NULL, IDC_CROSS);
        if (!RegisterClassExW(&wc)) return -1;
        registered = 1;
    }

    vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    memset(&st, 0, sizeof(st));

    st.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                              SNIP_CLASS, L"", WS_POPUP,
                              vx, vy, vw, vh, NULL, NULL, inst, &st);
    if (!st.hwnd) return -1;

    SetLayeredWindowAttributes(st.hwnd, 0, 110, LWA_ALPHA);
    ShowWindow(st.hwnd, SW_SHOW);
    SetForegroundWindow(st.hwnd);
    SetFocus(st.hwnd);

    while (!st.done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    norm_rect(&st, &sel);

    /* Down before the pixels are read, so neither the dimming nor the
     * selection frame ends up in the shot. */
    ShowWindow(st.hwnd, SW_HIDE);
    DestroyWindow(st.hwnd);
    {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    if (st.cancelled) return 1;
    if (sel.right - sel.left < 8 || sel.bottom - sel.top < 8) return 1;

    {
        int w = sel.right - sel.left;
        int h = sel.bottom - sel.top;
        HDC screen = GetDC(NULL);
        HDC mem    = CreateCompatibleDC(screen);
        BITMAPINFO bi;
        void   *bits = NULL;
        HBITMAP dib;

        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;          /* top-down, as WIC expects */
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (dib && bits) {
            HGDIOBJ old = SelectObject(mem, dib);
            BitBlt(mem, 0, 0, w, h, screen,
                   sel.left + vx, sel.top + vy, SRCCOPY | CAPTUREBLT);
            SelectObject(mem, old);
            rc = encode_png(bits, w, h, png, png_len);
            if (rc == 0)
                LOGI("snip: captured %dx%d, %u bytes of png",
                     w, h, (unsigned)*png_len);
        }
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
    }
    return rc;
}
