#include "transcript.h"
#include "util.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 600

typedef struct {
    unsigned long long ms;      /* monotonic arrival time */
    SYSTEMTIME         wall;
    char              *text;
} entry_t;

struct transcript {
    entry_t          e[MAX_ENTRIES];
    int              count;     /* number of valid entries, oldest first */
    CRITICAL_SECTION cs;
};

transcript_t *transcript_create(void)
{
    transcript_t *t = (transcript_t *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    InitializeCriticalSection(&t->cs);
    return t;
}

void transcript_destroy(transcript_t *t)
{
    int i;
    if (!t) return;
    for (i = 0; i < t->count; ++i) free(t->e[i].text);
    DeleteCriticalSection(&t->cs);
    free(t);
}

void transcript_add(transcript_t *t, const char *text_utf8)
{
    char *copy;
    if (!t || !text_utf8 || !*text_utf8) return;
    copy = xstrdup(text_utf8);
    if (!copy) return;

    EnterCriticalSection(&t->cs);
    if (t->count == MAX_ENTRIES) {
        free(t->e[0].text);
        memmove(&t->e[0], &t->e[1], (MAX_ENTRIES - 1) * sizeof(entry_t));
        t->count--;
    }
    t->e[t->count].ms   = now_ms();
    GetLocalTime(&t->e[t->count].wall);
    t->e[t->count].text = copy;
    t->count++;
    LeaveCriticalSection(&t->cs);
}

void transcript_clear(transcript_t *t)
{
    int i;
    if (!t) return;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < t->count; ++i) free(t->e[i].text);
    t->count = 0;
    LeaveCriticalSection(&t->cs);
}

char *transcript_recent(transcript_t *t, int seconds)
{
    sb_t sb;
    int i;
    unsigned long long cutoff;

    sb_init(&sb);
    if (!t) return xstrdup("");

    EnterCriticalSection(&t->cs);
    if (seconds <= 0) {
        cutoff = 0;                      /* no window: send the whole transcript */
    } else {
        unsigned long long now  = now_ms();
        unsigned long long span = (unsigned long long)seconds * 1000ULL;
        cutoff = now > span ? now - span : 0;
    }
    for (i = 0; i < t->count; ++i) {
        if (t->e[i].ms < cutoff) continue;
        sb_printf(&sb, "[%02d:%02d:%02d] %s\n",
                  t->e[i].wall.wHour, t->e[i].wall.wMinute, t->e[i].wall.wSecond,
                  t->e[i].text);
    }
    LeaveCriticalSection(&t->cs);

    return sb.data ? sb.data : xstrdup("");
}

char *transcript_last(transcript_t *t)
{
    char *s = NULL;
    if (!t) return xstrdup("");
    EnterCriticalSection(&t->cs);
    if (t->count) s = xstrdup(t->e[t->count - 1].text);
    LeaveCriticalSection(&t->cs);
    return s ? s : xstrdup("");
}

char *transcript_tail(transcript_t *t, int max_lines)
{
    sb_t sb;
    int i, from;

    sb_init(&sb);
    if (!t) return xstrdup("");

    EnterCriticalSection(&t->cs);
    from = t->count - max_lines;
    if (from < 0) from = 0;
    for (i = from; i < t->count; ++i) {
        sb_puts(&sb, t->e[i].text);
        sb_puts(&sb, "\n");
    }
    LeaveCriticalSection(&t->cs);

    return sb.data ? sb.data : xstrdup("");
}

int transcript_count(transcript_t *t)
{
    int n;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    n = t->count;
    LeaveCriticalSection(&t->cs);
    return n;
}
