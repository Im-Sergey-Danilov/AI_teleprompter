#ifndef CALLASSIST_OVERLAY_H
#define CALLASSIST_OVERLAY_H

#include "config.h"
#include <windows.h>

typedef struct overlay overlay_t;

/* What the status dot shows. */
enum {
    OVL_PAUSED = 0,
    OVL_LISTENING,
    OVL_TRANSCRIBING,
    OVL_THINKING
};

/*
 * The answer area is split into four independently scrolling panes: what was
 * heard and sent to the model, the reply to give now, background on the topic,
 * and an optional stronger point to add.
 */
enum {
    OVL_PANE_INPUT = 0,
    OVL_PANE_ANSWER,
    OVL_PANE_CONTEXT,
    OVL_PANE_BONUS,
    OVL_PANE_COUNT
};

overlay_t *overlay_create(HINSTANCE inst, const config_t *cfg);
void  overlay_destroy(overlay_t *o);
void  overlay_apply_config(overlay_t *o, const config_t *cfg);
void  overlay_show(overlay_t *o, int show);
int   overlay_is_visible(overlay_t *o);
HWND  overlay_hwnd(overlay_t *o);
void  overlay_get_rect(overlay_t *o, RECT *r);

/* All of these are safe to call from worker threads. */
void  overlay_set_status(overlay_t *o, const char *utf8);
void  overlay_set_state(overlay_t *o, int state);   /* OVL_* above */
void  overlay_set_busy(overlay_t *o, int busy);
void  overlay_begin_answer(overlay_t *o);
void  overlay_append_answer(overlay_t *o, const char *utf8);
void  overlay_set_answer(overlay_t *o, const char *utf8);
void  overlay_set_transcript(overlay_t *o, const char *utf8);

/* Scroll the answer pane. notches > 0 scrolls back towards older text, < 0
 * forwards; 0 re-pins to the newest line. Answers can outrun the window. With
 * click-through on the wheel never reaches us directly, so the mouse hook in
 * main.c forwards it, and this is also driven by a global hotkey. */
void  overlay_scroll(overlay_t *o, int notches);          /* the focused pane */
void  overlay_set_pane(overlay_t *o, int pane, const char *utf8);
void  overlay_clear_panes(overlay_t *o);
/* CONTEXT + COULD ADD only, so a follow-up request can refill them
 * without disturbing the answer already on screen. */
void  overlay_clear_details(overlay_t *o);
void  overlay_scroll_pane(overlay_t *o, int pane, int notches);
/* Which pane a screen point falls in, or -1. */
int   overlay_pane_at(overlay_t *o, POINT screen_pt);
/* The pane the scroll hotkeys act on; cycles with overlay_focus_next. */
void  overlay_focus_next(overlay_t *o);

/*
 * Temporarily take mouse input so the window can be dragged and resized, even
 * though it is normally click-through. Leaving move mode restores whatever
 * click-through the config asks for.
 */
void  overlay_set_interactive(overlay_t *o, int on);
int   overlay_is_interactive(overlay_t *o);

#endif
