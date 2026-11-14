#include <Core/SafePath.h>

#include <cctype>

namespace shockwave {

namespace {

bool reservedName(std::string_view part) {
    // Windows reserves these with any extension: "nul.txt" too.
    const std::string_view stem = part.substr(0, part.find('.'));
    std::string upper;
    for (const char c : stem) upper += char(std::toupper(static_cast<unsigned char>(c)));
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") return true;
    return upper.size() == 4 && (upper.rfind("COM", 0) == 0 || upper.rfind("LPT", 0) == 0) && upper[3] >= '1' &&
           upper[3] <= '9';
}

}   // namespace

bool checkRelativePath(std::string_view path, std::string& why) {
    if (path.empty()) {
        why = "an empty path";
        return false;
    }
    if (path.front() == '/') {
        why = "an absolute path";
        return false;
    }
    for (const char c : path) {
        if (static_cast<unsigned char>(c) < 32 || c == 127) {
            why = "a control character";
            return false;
        }
        if (c == '\\' || c == ':') {
            why = std::string("'") + c + "', which some systems read as a folder or drive";
            return false;
        }
    }
    for (size_t at = 0; at <= path.size();) {
        const size_t end = path.find('/', at) == std::string_view::npos ? path.size() : path.find('/', at);
        const std::string_view part = path.substr(at, end - at);
        if (part.empty()) {
            why = "an empty part ('//' or a trailing '/')";
            return false;
        }
        if (part == "." || part == "..") {
            why = "'" + std::string(part) + "', which leaves its folder";
            return false;
        }
        if (part.back() == '.' || part.back() == ' ') {
            why = "'" + std::string(part) + "', which Windows can't name (a trailing '.' or space)";
            return false;
        }
        if (reservedName(part)) {
            why = "'" + std::string(part) + "', a name Windows reserves";
            return false;
        }
        at = end + 1;
    }
    why.clear();
    return true;
}

std::string pathCaseKey(std::string_view path) {
    std::string key(path);
    for (char& c : key)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return key;
}

}   // namespace shockwave
