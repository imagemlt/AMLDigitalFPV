#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    /* Initialize spdlog default logger to stdout. Level is optional ("trace",
     * "debug", "info", "warn", "err", "critical", "off"). */
    void spdlog_init(const char *level);

    /* Change log level at runtime (same strings as init). */
    void spdlog_set_level(const char *level);

    /* printf-style logging helpers for C callers. */
    void spdlog_trace(const char *fmt, ...);
    void spdlog_debug(const char *fmt, ...);
    void spdlog_info(const char *fmt, ...);
    void spdlog_warn(const char *fmt, ...);
    void spdlog_error(const char *fmt, ...);
    void spdlog_critical(const char *fmt, ...);
    void set_priority(const char *thread_name, int level);

    /* Flush underlying logger. */
    void spdlog_flush(void);

#ifdef __cplusplus
}
#endif
