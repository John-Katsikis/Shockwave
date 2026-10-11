#include "BinaryInfo.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace shockwave::packaging {

namespace {

template <class T>
bool read(const std::vector<char>& data, uint64_t offset, T& out) {
    if (offset > data.size() || data.size() - offset < sizeof(T)) return false;
    std::memcpy(&out, data.data() + offset, sizeof(T));   // ELF64 little-endian, like the host
    return true;
}

std::string stringAt(const std::vector<char>& data, uint64_t offset) {
    if (offset >= data.size()) return {};
    const char* start = data.data() + offset;
    return std::string(start, strnlen(start, data.size() - offset));
}

// The newest "<prefix><version>" in the file, by version number.
std::string newestVersion(const std::vector<char>& data, const std::string& prefix) {
    std::string best;
    const std::string_view all(data.data(), data.size());
    for (size_t at = all.find(prefix); at != std::string_view::npos; at = all.find(prefix, at + 1)) {
        size_t end = at + prefix.size();
        while (end < all.size() && (std::isdigit(static_cast<unsigned char>(all[end])) || all[end] == '.')) ++end;
        // Whole symbol versions only ("GLIBC_2.34"), not "GLIBC_2.34x" or a trailing dot.
        if (end == at + prefix.size() || all[end - 1] == '.' || (end < all.size() && all[end] != '\0')) continue;
        const std::string number(all.substr(at + prefix.size(), end - at - prefix.size()));
        if (compareVersions(number, best) > 0) best = number;
    }
    return best;
}

}   // namespace

int compareVersions(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return a.empty() == b.empty() ? 0 : a.empty() ? -1 : 1;
    std::istringstream left(a), right(b);
    std::string x, y;
    while (true) {
        const bool hasX = static_cast<bool>(std::getline(left, x, '.'));
        const bool hasY = static_cast<bool>(std::getline(right, y, '.'));
        if (!hasX && !hasY) return 0;
        const long nx = hasX ? std::strtol(x.c_str(), nullptr, 10) : 0;
        const long ny = hasY ? std::strtol(y.c_str(), nullptr, 10) : 0;
        if (nx != ny) return nx < ny ? -1 : 1;
    }
}

std::string gccForGlibcxx(const std::string& glibcxx) {
    static const struct {
        const char* glibcxx;
        const char* gcc;
    } table[] = {{"3.4.28", "10"},   {"3.4.29", "11"}, {"3.4.30", "12"}, {"3.4.31", "13.1"},
                 {"3.4.32", "13.2"}, {"3.4.33", "14"}, {"3.4.34", "15"}, {"3.4.35", "16"}};
    for (const auto& row : table)
        if (glibcxx == row.glibcxx) return row.gcc;
    return {};
}

BinaryInfo readBinaryInfo(const std::filesystem::path& file) {
    BinaryInfo info;
    std::ifstream in(file, std::ios::binary);
    char magic[6] = {};
    if (!in.read(magic, sizeof(magic)) || std::memcmp(
                                              magic,
                                              "\x7f"
                                              "ELF",
                                              4) != 0)
        return info;
    if (magic[4] != 2 || magic[5] != 1) return info;   // 64-bit little-endian only
    in.seekg(0);
    const std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    uint64_t phoff = 0;
    uint16_t phentsize = 0, phnum = 0;
    if (!read(data, 0x20, phoff) || !read(data, 0x36, phentsize) || !read(data, 0x38, phnum)) return info;
    info.binary = true;

    struct Segment {
        uint32_t type;
        uint64_t offset, vaddr, filesz;
    };
    std::vector<Segment> segments;
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint64_t at = phoff + uint64_t(i) * phentsize;
        Segment s{};
        if (!read(data, at, s.type) || !read(data, at + 8, s.offset) || !read(data, at + 16, s.vaddr) ||
            !read(data, at + 32, s.filesz))
            break;
        segments.push_back(s);
    }
    auto fileOffset = [&](uint64_t vaddr) -> uint64_t {
        for (const Segment& s : segments)
            if (s.type == 1 && vaddr >= s.vaddr && vaddr < s.vaddr + s.filesz) return vaddr - s.vaddr + s.offset;
        return UINT64_MAX;
    };
    for (const Segment& dynamic : segments) {
        if (dynamic.type != 2) continue;   // PT_DYNAMIC
        uint64_t strtab = 0;
        std::vector<uint64_t> needed, paths;
        for (uint64_t at = dynamic.offset; at + 16 <= dynamic.offset + dynamic.filesz; at += 16) {
            int64_t tag = 0;
            uint64_t value = 0;
            if (!read(data, at, tag) || !read(data, at + 8, value) || tag == 0) break;
            if (tag == 1) needed.push_back(value);   // DT_NEEDED
            if (tag == 5) strtab = value;   // DT_STRTAB
            if (tag == 15 || tag == 29) paths.push_back(value);   // DT_RPATH, DT_RUNPATH
        }
        const uint64_t strings = fileOffset(strtab);
        if (strings == UINT64_MAX) break;
        for (const uint64_t name : needed) info.needed.push_back(stringAt(data, strings + name));
        for (const uint64_t list : paths) {
            std::istringstream entries(stringAt(data, strings + list));
            for (std::string entry; std::getline(entries, entry, ':');)
                if (!entry.empty()) info.searchPaths.push_back(entry);
        }
        break;
    }
    info.glibc = newestVersion(data, "GLIBC_");
    info.glibcxx = newestVersion(data, "GLIBCXX_");
    return info;
}

}   // namespace shockwave::packaging
