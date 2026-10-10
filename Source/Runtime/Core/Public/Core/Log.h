#pragma once

// Logging with categories and levels, the role UE_LOG plays in Unreal:
//
//     SW_LOG_CATEGORY(LogPhysics);                       // once, in a header or .cpp
//     SW_LOG(LogPhysics, Warning, "{} bodies over budget", count);
//
// Safe from any thread. Messages go to every sink: by default standard error (so tool
// output on standard out stays clean), and the editor adds its Console. A category's
// `minimum` drops anything below it before formatting.

#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace shockwave {

enum class LogLevel { Verbose, Info, Warning, Error };

struct LogCategory {
    const char* name;
    LogLevel minimum = LogLevel::Info;
};

using LogSink = std::function<void(const LogCategory&, LogLevel, std::string_view message)>;

void logMessage(const LogCategory& category, LogLevel level, std::string_view message);

// Adds a sink; the id removes it again. Sinks run on the thread that logs, under the
// log's lock, so keep them short (queue the message, don't draw it).
int addLogSink(LogSink sink);
void removeLogSink(int id);
// Turns the standard error sink off or on (tests, or a host with its own output).
void setLogToStderr(bool enabled);

const char* logLevelName(LogLevel level);

template <class... Args>
void logFormat(const LogCategory& category, LogLevel level, std::format_string<Args...> format, Args&&... args) {
    if (level < category.minimum) return;
    logMessage(category, level, std::format(format, std::forward<Args>(args)...));
}

}   // namespace shockwave

#define SW_LOG_CATEGORY(name)                                                                                          \
    inline ::shockwave::LogCategory name { #name }
#define SW_LOG(category, level, ...) ::shockwave::logFormat(category, ::shockwave::LogLevel::level, __VA_ARGS__)

namespace shockwave {
// Categories the engine's own modules log under.
SW_LOG_CATEGORY(LogEngine);
SW_LOG_CATEGORY(LogRender);
SW_LOG_CATEGORY(LogPhysics);
SW_LOG_CATEGORY(LogGame);
}   // namespace shockwave
