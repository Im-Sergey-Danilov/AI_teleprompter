#ifndef CALLASSIST_LOG_H
#define CALLASSIST_LOG_H

void log_init(void);
void log_shutdown(void);
void log_msg(const char *level, const char *fmt, ...);

#define LOGI(...) log_msg("INFO", __VA_ARGS__)
#define LOGW(...) log_msg("WARN", __VA_ARGS__)
#define LOGE(...) log_msg("ERR ", __VA_ARGS__)

#endif
