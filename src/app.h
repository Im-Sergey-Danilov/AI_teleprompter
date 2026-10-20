#ifndef CALLASSIST_APP_H
#define CALLASSIST_APP_H

#include "config.h"
#include <windows.h>

/* Implemented in main.c; used by the settings dialog. */
void  app_get_config(config_t *out);
int   app_apply_config(const config_t *in, char *msg, size_t msg_cap);
float app_level_db(void);
float app_source_level_db(int loopback);
void  app_overlay_rect(RECT *r);
void  app_status_text(char *out, size_t cap);
HWND  app_main_window(void);
HINSTANCE app_instance(void);

void  app_trigger_answer(void);
void  app_clear_transcript(void);
void  app_set_listening(int on);
int   app_listening(void);
void  app_toggle_overlay(void);
void  app_toggle_move_mode(void);

/* Implemented in settings_ui.c. */
void  settings_show(HINSTANCE inst);
HWND  settings_hwnd(void);

#endif
