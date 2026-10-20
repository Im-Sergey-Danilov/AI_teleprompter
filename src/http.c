#include "http.h"
#include "util.h"
#include "log.h"

#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

#define UA L"call_assist/1.0"

void http_response_free(http_response_t *r)
{
    if (!r) return;
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
}

typedef struct {
    HINTERNET session, connect, request;
} conn_t;

static void conn_close(conn_t *c)
{
    if (c->request) WinHttpCloseHandle(c->request);
    if (c->connect) WinHttpCloseHandle(c->connect);
    if (c->session) WinHttpCloseHandle(c->session);
    memset(c, 0, sizeof(*c));
}

static void fail(http_response_t *out, const char *what)
{
    DWORD e = GetLastError();
    snprintf(out->error, sizeof(out->error), "%s (winhttp error %lu)", what, (unsigned long)e);
    LOGE("http: %s", out->error);
}

/*
 * Opens the connection and sends headers + body. Leaves the response ready to read.
 * Returns 0 on success.
 */
static int send_request(conn_t *c,
                        const char *url,
                        const char *content_type,
                        const void *body, size_t body_len,
                        const char *api_key,
                        const char *extra_headers_utf8,
                        int timeout_ms,
                        int want_stream,
                        http_response_t *out)
{
    wchar_t *wurl = NULL;
    wchar_t  host[256], path[2048];
    URL_COMPONENTS uc;
    sb_t hdr;
    wchar_t *whdr = NULL;
    DWORD    flags = 0;
    int      rc = -1;

    memset(c, 0, sizeof(*c));
    memset(out, 0, sizeof(*out));
    sb_init(&hdr);

    wurl = utf8_to_wide(url);
    if (!wurl) { snprintf(out->error, sizeof(out->error), "bad url"); goto done; }

    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize     = sizeof(uc);
    uc.lpszHostName     = host; uc.dwHostNameLength    = (DWORD)(sizeof(host) / sizeof(host[0]));
    uc.lpszUrlPath      = path; uc.dwUrlPathLength     = (DWORD)(sizeof(path) / sizeof(path[0]));
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) { fail(out, "cannot parse URL"); goto done; }

    c->session = WinHttpOpen(UA, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c->session) {
        c->session = WinHttpOpen(UA, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!c->session) { fail(out, "WinHttpOpen failed"); goto done; }

    WinHttpSetTimeouts(c->session, 15000, 15000, timeout_ms, timeout_ms);

    c->connect = WinHttpConnect(c->session, host, uc.nPort, 0);
    if (!c->connect) { fail(out, "WinHttpConnect failed"); goto done; }

    if (uc.nScheme == INTERNET_SCHEME_HTTPS) flags |= WINHTTP_FLAG_SECURE;

    c->request = WinHttpOpenRequest(c->connect, L"POST", path, NULL,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!c->request) { fail(out, "WinHttpOpenRequest failed"); goto done; }

    if (want_stream) {
        DWORD opt = WINHTTP_DISABLE_KEEP_ALIVE;
        (void)opt;   /* streaming works with keep-alive; kept for clarity */
    }

    sb_printf(&hdr, "Content-Type: %s\r\n", content_type ? content_type : "application/json");
    if (api_key && *api_key) sb_printf(&hdr, "Authorization: Bearer %s\r\n", api_key);
    if (extra_headers_utf8 && *extra_headers_utf8) sb_puts(&hdr, extra_headers_utf8);
    if (want_stream) sb_puts(&hdr, "Accept: text/event-stream\r\n");

    whdr = utf8_to_wide(hdr.data ? hdr.data : "");
    if (!whdr) { snprintf(out->error, sizeof(out->error), "out of memory"); goto done; }

    if (!WinHttpSendRequest(c->request, whdr, (DWORD)-1,
                            (LPVOID)body, (DWORD)body_len, (DWORD)body_len, 0)) {
        fail(out, "WinHttpSendRequest failed");
        goto done;
    }
    if (!WinHttpReceiveResponse(c->request, NULL)) {
        fail(out, "WinHttpReceiveResponse failed");
        goto done;
    }

    {
        DWORD code = 0, sz = sizeof(code);
        if (WinHttpQueryHeaders(c->request,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX))
            out->status = (int)code;
    }
    rc = 0;

done:
    free(wurl);
    free(whdr);
    sb_free(&hdr);
    return rc;
}

static int read_all(HINTERNET request, http_response_t *out)
{
    sb_t sb;
    sb_init(&sb);
    for (;;) {
        DWORD avail = 0, got = 0;
        char  buf[8192];
        if (!WinHttpQueryDataAvailable(request, &avail)) break;
        if (!avail) break;
        while (avail) {
            DWORD take = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
            if (!WinHttpReadData(request, buf, take, &got) || !got) { avail = 0; break; }
            sb_append(&sb, buf, got);
            avail -= got;
        }
    }
    out->body     = sb.data ? sb.data : xstrdup("");
    out->body_len = sb.len;
    return 0;
}

int http_post(const char *url, const char *content_type,
              const void *body, size_t body_len,
              const char *api_key, const char *extra_headers_utf8,
              int timeout_ms, http_response_t *out)
{
    conn_t c;
    int rc = send_request(&c, url, content_type, body, body_len, api_key,
                          extra_headers_utf8, timeout_ms, 0, out);
    if (rc == 0) read_all(c.request, out);
    conn_close(&c);
    return rc;
}

int http_post_sse(const char *url, const char *content_type,
                  const void *body, size_t body_len,
                  const char *api_key, int timeout_ms,
                  http_sse_cb on_event, void *user,
                  volatile LONG *cancel, http_response_t *out)
{
    conn_t c;
    sb_t   pending;      /* incomplete tail of the byte stream */
    sb_t   evt;          /* accumulated data: lines of the current event */
    int    rc;
    int    aborted = 0;

    rc = send_request(&c, url, content_type, body, body_len, api_key, NULL, timeout_ms, 1, out);
    if (rc != 0) { conn_close(&c); return rc; }

    if (out->status < 200 || out->status >= 300) {   /* error payload is plain JSON */
        read_all(c.request, out);
        conn_close(&c);
        return 0;
    }

    sb_init(&pending);
    sb_init(&evt);

    for (;;) {
        DWORD avail = 0, got = 0;
        char  buf[8192];
        char *nl;

        if (cancel && InterlockedCompareExchange(cancel, 0, 0)) { aborted = 1; break; }
        if (!WinHttpQueryDataAvailable(c.request, &avail)) break;
        if (!avail) break;

        while (avail && !aborted) {
            DWORD take = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
            if (!WinHttpReadData(c.request, buf, take, &got) || !got) { avail = 0; break; }
            sb_append(&pending, buf, got);
            avail -= got;

            /* split off complete lines */
            while (pending.data && pending.len &&
                   (nl = (char *)memchr(pending.data, '\n', pending.len)) != NULL) {
                size_t line_len = (size_t)(nl - pending.data);
                char  *line = pending.data;
                size_t rest;

                if (line_len && line[line_len - 1] == '\r') line[line_len - 1] = '\0';
                line[line_len] = '\0';

                if (line[0] == '\0') {                       /* event boundary */
                    if (evt.len) {
                        if (on_event && on_event(user, evt.data)) aborted = 1;
                        evt.len = 0;
                        if (evt.data) evt.data[0] = '\0';
                    }
                } else if (strncmp(line, "data:", 5) == 0) {
                    const char *p = line + 5;
                    if (*p == ' ') p++;
                    if (evt.len) sb_append(&evt, "\n", 1);
                    sb_puts(&evt, p);
                }
                /* other SSE fields (event:, id:, :comment) are ignored */

                rest = pending.len - line_len - 1;
                memmove(pending.data, pending.data + line_len + 1, rest);
                pending.len = rest;
                pending.data[rest] = '\0';
                if (aborted) break;
            }
        }
        if (aborted) break;
    }

    if (!aborted && evt.len && on_event) on_event(user, evt.data);

    sb_free(&pending);
    sb_free(&evt);
    conn_close(&c);
    if (!out->body) out->body = xstrdup("");
    return 0;
}
