/**********************************************************************************************************************
    > File: logger.h
    > 云端轻量日志（零依赖；生产可替换 spdlog 接口不变）
**********************************************************************************************************************/
#ifndef VWISE_CLOUD_LOGGER_H
#define VWISE_CLOUD_LOGGER_H

#include <cstdio>
#include <cstdarg>
#include <cstdint>

namespace vwise {
namespace cloud {

enum LogLevel { VWLL_DEBUG = 0, VWLL_INFO = 1, VWLL_WARN = 2, VWLL_ERROR = 3 };

inline LogLevel g_log_level = VWLL_INFO;

inline void log_printf(LogLevel lvl, const char *fmt, ...) {
    if ((int)lvl < (int)g_log_level) return;
    static const char *tag[] = {"[DBG]", "[INF]", "[WRN]", "[ERR]"};
    fprintf(stderr, "%s ", tag[(int)lvl]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

#define LOGD(...) ::vwise::cloud::log_printf(::vwise::cloud::VWLL_DEBUG, __VA_ARGS__)
#define LOGI(...) ::vwise::cloud::log_printf(::vwise::cloud::VWLL_INFO,  __VA_ARGS__)
#define LOGW(...) ::vwise::cloud::log_printf(::vwise::cloud::VWLL_WARN,  __VA_ARGS__)
#define LOGE(...) ::vwise::cloud::log_printf(::vwise::cloud::VWLL_ERROR, __VA_ARGS__)

} // namespace cloud
} // namespace vwise

#endif // VWISE_CLOUD_LOGGER_H
