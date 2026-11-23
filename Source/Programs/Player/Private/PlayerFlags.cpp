// The flag table (PlayerFlags.h) and the number parsers its rows share.
#include "PlayerFlags.h"

#include <charconv>

namespace player {

uint64_t wholeNumber(const std::string& value, uint64_t max, const char* what) {
    uint64_t n = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
    if (ec != std::errc{} || end != value.data() + value.size() || n > max)
        throw player::UsageError(std::string(what) + " takes a whole number up to " + std::to_string(max));
    return n;
}

uint64_t steamNumber(const std::string& value, const char* what) {
    uint64_t n = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
    if (ec != std::errc{} || end != value.data() + value.size() || n == 0 || value.size() > 20)
        throw player::UsageError(std::string(what) + " takes a decimal Steam ID (a nonzero 64-bit number)");
    return n;
}

const std::vector<Flag>& playerFlags() {
    static const std::vector<Flag> flags = [] {
        std::vector<Flag> list;
        addProjectFlags(list);
        addGraphicsFlags(list);
        addReleaseFlags(list);
        addNetworkFlags(list);
        return list;
    }();
    return flags;
}

const Flag* findFlag(std::string_view word, int left) {
    for (const Flag& flag : playerFlags())
        for (std::string_view name : flag.names)
            if (name == word && flag.values <= left) return &flag;
    return nullptr;
}

}   // namespace player
