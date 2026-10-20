#include "log.h"
#include "util.h"

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static CRITICAL_SECTION g_cs;
static FILE            *g_fp;
static int              g_ready;

void log_init(void)
{
    char path[1024];
    if (g_ready) return;
    InitializeCriticalSection(&g_cs);
    g_ready = 1;
    if (app_data_path("call_assist.log", path, sizeof(path)) == 0) {
        wchar_t *w = utf8_to_wide(path);
        if (w) { g_fp = _wfopen(w, L"a"); free(w); }   /* UTF-8 bytes */
    }
    log_msg("INFO", "---- call_assist started ----");
}

void log_shutdown(void)
{
    if (!g_ready) return;
    EnterCriticalSection(&g_cs);
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
    LeaveCriticalSection(&g_cs);
    DeleteCriticalSection(&g_cs);
    g_ready = 0;
}

void log_msg(const char *level, const char *fmt, ...)
{
    char line[2048];
    int  n;
    SYSTEMTIME st;
    va_list ap;

    if (!g_ready) return;
    GetLocalTime(&st);
    n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d %s ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level);
    if (n < 0 || (size_t)n >= sizeof(line)) return;

    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_cs);
    if (g_fp) {
        fputs(line, g_fp);
        fputc('\n', g_fp);
        fflush(g_fp);
    }
    LeaveCriticalSection(&g_cs);

    OutputDebugStringA(line);
    OutputDebugStringA("\n");
}
