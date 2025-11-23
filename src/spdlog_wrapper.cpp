#include "spdlog_wrapper.h"
#include "scheduling_helper.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <strings.h>

namespace
{
    std::string format_message(const char *fmt, va_list ap)
    {
        if (!fmt)
            return {};
        va_list ap_copy;
        va_copy(ap_copy, ap);
        int needed = vsnprintf(nullptr, 0, fmt, ap_copy);
        va_end(ap_copy);
        if (needed < 0)
            return {};
        std::vector<char> buf(static_cast<size_t>(needed) + 1);
        vsnprintf(buf.data(), buf.size(), fmt, ap);
        return std::string(buf.data());
    }

    spdlog::level::level_enum level_from_str(const char *lvl)
    {
        if (!lvl)
            return spdlog::level::info;
        auto l = spdlog::level::from_str(lvl);
        if (l == spdlog::level::off && strcasecmp(lvl, "off") != 0)
            return spdlog::level::info;
        return l;
    }

    void ensure_logger()
    {
        if (!spdlog::default_logger())
        {
            try
            {
                auto logger = spdlog::stdout_color_mt("osd_logger");
                spdlog::set_default_logger(logger);
                spdlog::set_pattern("[%H:%M:%S.%f] [%l] %v");
            }
            catch (const spdlog::spdlog_ex &)
            {
                // ignore; will use existing default if present
            }
        }
    }
} // namespace

extern "C"
{
    void spdlog_init(const char *level)
    {
        ensure_logger();
        spdlog::set_level(level_from_str(level));
    }

    void spdlog_set_level(const char *level)
    {
        ensure_logger();
        spdlog::set_level(level_from_str(level));
    }

    void spdlog_trace(const char *fmt, ...)
    {
        ensure_logger();
        va_list ap;
        va_start(ap, fmt);
        auto msg = format_message(fmt, ap);
        va_end(ap);
        spdlog::trace("{}", msg);
    }

    void spdlog_debug(const char *fmt, ...)
    {
        ensure_logger();
        va_list ap;
        va_start(ap, fmt);
        auto msg = format_message(fmt, ap);
        va_end(ap);
        spdlog::debug("{}", msg);
    }

    void spdlog_info(const char *fmt, ...)
    {
        ensure_logger();
        va_list ap;
        va_start(ap, fmt);
        auto msg = format_message(fmt, ap);
        va_end(ap);
        spdlog::info("{}", msg);
    }

    void spdlog_warn(const char *fmt, ...)
    {
        ensure_logger();
        va_list ap;
        va_start(ap, fmt);
        auto msg = format_message(fmt, ap);
        va_end(ap);
        spdlog::warn("{}", msg);
    }

    void spdlog_error(const char *fmt, ...)
    {
        ensure_logger();
        va_list ap;
        va_start(ap, fmt);
        auto msg = format_message(fmt, ap);
        va_end(ap);
        spdlog::error("{}", msg);
    }

    void spdlog_critical(const char *fmt, ...)
    {
        ensure_logger();
        va_list ap;
        va_start(ap, fmt);
        auto msg = format_message(fmt, ap);
        va_end(ap);
        spdlog::critical("{}", msg);
    }

    void spdlog_flush(void)
    {
        ensure_logger();
        spdlog::default_logger()->flush();
    }
}

void set_priority(const char *thread_name, int level)
{
    SchedulingHelper::set_thread_params_max_realtime(thread_name, level);
}