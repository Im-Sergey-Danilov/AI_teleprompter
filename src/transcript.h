#ifndef CALLASSIST_TRANSCRIPT_H
#define CALLASSIST_TRANSCRIPT_H

typedef struct transcript transcript_t;

transcript_t *transcript_create(void);
void          transcript_destroy(transcript_t *t);
void          transcript_add(transcript_t *t, const char *text_utf8);
void          transcript_clear(transcript_t *t);

/* All getters return malloc'd UTF-8 (never NULL unless out of memory). */
char *transcript_recent(transcript_t *t, int seconds);   /* "[hh:mm:ss] line" per entry */
char *transcript_last(transcript_t *t);
char *transcript_tail(transcript_t *t, int max_lines);   /* raw text, newest last */
int   transcript_count(transcript_t *t);

#endif
