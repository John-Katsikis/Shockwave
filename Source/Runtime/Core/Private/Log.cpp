#include <Core/Log.h>

#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

namespace shockwave {

namespace {

struct Registry {
    std::mutex mutex;
    std::vector<std::pair<int, LogSink>> sinks;
    int nextId = 1;
    bool toStderr = true;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

}   // namespace

const char* logLevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Verbose: return "verbose";
    case LogLevel::Info: return "info";
    case LogLevel::Warning: return "warning";
    case LogLevel::Error: return "error";
    }
    return "?";
}

void logMessage(const LogCategory& category, LogLevel level, std::string_view message) {
    if (level < category.minimum) return;
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    if (r.toStderr) {
        if (level >= LogLevel::Warning)
            std::fprintf(
                stderr, "%s: %s: %.*s\n", category.name, logLevelName(level), static_cast<int>(message.size()),
                message.data());
        else
            std::fprintf(stderr, "%s: %.*s\n", category.name, static_cast<int>(message.size()), message.data());
    }
    for (const auto& [id, sink] : r.sinks) sink(category, level, message);
}

int addLogSink(LogSink sink) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    r.sinks.emplace_back(r.nextId, std::move(sink));
    return r.nextId++;
}

void removeLogSink(int id) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    std::erase_if(r.sinks, [id](const auto& entry) { return entry.first == id; });
}

void setLogToStderr(bool enabled) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    r.toStderr = enabled;
}

}   // namespace shockwave
