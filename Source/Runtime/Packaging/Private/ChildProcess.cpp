#include "ChildProcess.h"

#include <cstdio>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace shockwave::packaging {

std::string shellQuote(std::string_view text) {
#if defined(_WIN32)
    std::string out = "\"";
    for (const char c : text) out += c == '"' ? std::string("\\\"") : std::string(1, c);
    return out + "\"";
#else
    std::string out = "'";
    for (const char c : text) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
#endif
}

std::string shellQuote(const std::filesystem::path& path) { return shellQuote(std::string_view(path.string())); }

int runCommand(const std::string& command, const std::function<void(std::string_view)>& line) {
#if defined(_WIN32)
    FILE* pipe = _popen((command + " 2>&1").c_str(), "r");
#else
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
#endif
    if (!pipe) return -1;
    std::string text;
    char buffer[1024];
    while (std::fgets(buffer, sizeof(buffer), pipe)) {
        text += buffer;
        if (text.back() != '\n') continue;   // a long line arrives in pieces
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        if (line) line(text);
        text.clear();
    }
    if (!text.empty() && line) line(text);
#if defined(_WIN32)
    return _pclose(pipe);
#else
    const int status = pclose(pipe);
    return status == -1 ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
}

}   // namespace shockwave::packaging
