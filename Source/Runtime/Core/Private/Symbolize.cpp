// Reading crash reports and naming their frames (Core/Diagnostics.h). Linux reports carry
// raw addresses and /proc/self/maps: each address is turned into a binary and an offset,
// then named from that binary's ELF symbol table (or the unstripped copy kept by build ID
// beside a package). macOS reports already carry backtrace_symbols_fd's names; Windows
// reports carry module+offset, named with the minidump and a debugger.
#include <Core/Diagnostics.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>

#if defined(__has_include)
#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define SHOCKWAVE_HAS_CXXABI 1
#endif
#endif

namespace shockwave::diagnostics {

namespace fs = std::filesystem;

namespace {

std::string demangle(const std::string& name) {
#if defined(SHOCKWAVE_HAS_CXXABI)
    int status = 0;
    std::unique_ptr<char, void (*)(void*)> text(
        abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status), std::free);
    if (status == 0 && text) return text.get();
#endif
    return name;
}

// ---- ELF64, little endian (x86-64, AArch64) -----------------------------------------------

struct Elf {
    std::ifstream file;
    bool ok = false;
    uint64_t shoff = 0, phoff = 0;
    uint16_t shentsize = 0, shnum = 0, shstrndx = 0, phentsize = 0, phnum = 0;

    template <class T>
    bool at(uint64_t offset, T& out) {
        file.clear();
        file.seekg(std::streamoff(offset));
        file.read(reinterpret_cast<char*>(&out), sizeof(T));
        return bool(file);
    }
    bool bytes(uint64_t offset, size_t n, std::vector<char>& out) {
        if (n > (256u << 20)) return false;   // no symbol table is that big
        out.resize(n);
        file.clear();
        file.seekg(std::streamoff(offset));
        file.read(out.data(), std::streamsize(n));
        return bool(file);
    }

    explicit Elf(const fs::path& path) : file(path, std::ios::binary) {
        unsigned char ident[16];
        if (!file.read(reinterpret_cast<char*>(ident), 16)) return;
        if (std::memcmp(
                ident,
                "\x7f"
                "ELF",
                4) != 0 ||
            ident[4] != 2 || ident[5] != 1)
            return;   // ELF64, little endian only
        ok = at(0x20, phoff) && at(0x28, shoff) && at(0x36, phentsize) && at(0x38, phnum) && at(0x3a, shentsize) &&
             at(0x3c, shnum) && at(0x3e, shstrndx);
    }

    struct Section {
        uint32_t type = 0;
        uint64_t offset = 0, size = 0, entsize = 0;
        uint32_t link = 0;
    };
    bool section(uint16_t index, Section& s) {
        const uint64_t base = shoff + uint64_t(index) * shentsize;
        return at(base + 4, s.type) && at(base + 0x18, s.offset) && at(base + 0x20, s.size) &&
               at(base + 0x28, s.link) && at(base + 0x38, s.entsize);
    }

    // File offset -> virtual address, through the PT_LOAD segment holding it.
    bool fileOffsetToAddress(uint64_t offset, uint64_t& address) {
        for (uint16_t i = 0; i < phnum; ++i) {
            const uint64_t base = phoff + uint64_t(i) * phentsize;
            uint32_t type = 0;
            uint64_t pOffset = 0, pVaddr = 0, pFilesz = 0;
            if (!at(base, type) || !at(base + 8, pOffset) || !at(base + 0x10, pVaddr) || !at(base + 0x20, pFilesz))
                return false;
            if (type == 1 /* PT_LOAD */ && offset >= pOffset && offset < pOffset + pFilesz) {
                address = offset - pOffset + pVaddr;
                return true;
            }
        }
        return false;
    }
};

}   // namespace

std::string elfBuildId(const fs::path& binary) {
    Elf elf(binary);
    if (!elf.ok) return {};
    for (uint16_t i = 0; i < elf.shnum; ++i) {
        Elf::Section s;
        if (!elf.section(i, s) || s.type != 7 /* SHT_NOTE */) continue;
        std::vector<char> data;
        if (!elf.bytes(s.offset, size_t(s.size), data)) continue;
        for (size_t at = 0; at + 12 <= data.size();) {
            uint32_t nameSize, descSize, type;
            std::memcpy(&nameSize, &data[at], 4);
            std::memcpy(&descSize, &data[at + 4], 4);
            std::memcpy(&type, &data[at + 8], 4);
            const size_t name = at + 12, desc = name + ((nameSize + 3) & ~3u);
            if (desc + descSize > data.size()) break;
            if (type == 3 /* NT_GNU_BUILD_ID */ && nameSize == 4 && std::memcmp(&data[name], "GNU", 4) == 0) {
                std::string hex;
                for (uint32_t k = 0; k < descSize; ++k) {
                    const auto byte = static_cast<unsigned char>(data[desc + k]);
                    hex += "0123456789abcdef"[byte >> 4];
                    hex += "0123456789abcdef"[byte & 15];
                }
                return hex;
            }
            at = desc + ((descSize + 3) & ~3u);
        }
    }
    return {};
}

std::string elfFunctionAt(const fs::path& binary, uint64_t offset) {
    Elf elf(binary);
    if (!elf.ok) return {};
    std::string best;
    uint64_t bestStart = 0;
    bool fromSymtab = false;
    for (uint16_t i = 0; i < elf.shnum; ++i) {
        Elf::Section s;
        if (!elf.section(i, s) || (s.type != 2 /* SHT_SYMTAB */ && s.type != 11 /* SHT_DYNSYM */)) continue;
        if (fromSymtab && s.type == 11) continue;   // the full table already answered
        Elf::Section strings;
        if (s.entsize != 24 || !elf.section(uint16_t(s.link), strings)) continue;
        std::vector<char> table, names;
        if (!elf.bytes(s.offset, size_t(s.size), table) || !elf.bytes(strings.offset, size_t(strings.size), names))
            continue;
        std::string found;
        uint64_t foundStart = 0;
        for (size_t at = 0; at + 24 <= table.size(); at += 24) {
            uint32_t name;
            uint64_t value, size;
            std::memcpy(&name, &table[at], 4);
            const unsigned char info = static_cast<unsigned char>(table[at + 4]);
            std::memcpy(&value, &table[at + 8], 8);
            std::memcpy(&size, &table[at + 16], 8);
            if ((info & 0xf) != 2 /* STT_FUNC */ || value == 0 || name >= names.size()) continue;
            const uint64_t end = value + std::max<uint64_t>(size, 1);
            if (offset >= value && offset < end && (found.empty() || value > foundStart)) {
                found = std::string(&names[name], strnlen(&names[name], names.size() - name));
                foundStart = value;
            }
        }
        if (!found.empty()) {
            best = found;
            bestStart = foundStart;
            if (s.type == 2) fromSymtab = true;
        }
    }
    (void)bestStart;
    return best.empty() ? std::string() : demangle(best);
}

namespace {

struct Mapping {
    uint64_t start = 0, end = 0, fileOffset = 0;
    std::string path;
};

uint64_t parseHex(std::string_view text) {
    if (text.starts_with("0x")) text.remove_prefix(2);
    uint64_t value = 0;
    for (char c : text) {
        int digit = c >= '0' && c <= '9'   ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                           : -1;
        if (digit < 0) break;
        value = value * 16 + uint64_t(digit);
    }
    return value;
}

}   // namespace

bool symbolizeCrashReport(
    const fs::path& report, const std::vector<fs::path>& symbolFolders, CrashReport& out, std::string& error) {
    out = {};
    std::ifstream in(report, std::ios::binary);
    if (!in) {
        error = "couldn't open " + report.string();
        return false;
    }
    std::string first;
    std::getline(in, first);
    if (first.rfind("shockwave_crash", 0) != 0) {
        error = report.string() + " isn't a Shockwave crash report";
        return false;
    }
    std::vector<Mapping> maps;
    std::vector<std::string> symbols;
    std::string section;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        if (section.empty()) {
            const size_t space = line.find(' ');
            const std::string key = line.substr(0, space),
                              value = space == std::string::npos ? "" : line.substr(space + 1);
            if (key == "program")
                out.program = value;
            else if (key == "signal")
                out.signal = value;
        } else if (section == "context") {
            const size_t colon = line.find(": ");
            if (colon != std::string::npos) out.context.emplace_back(line.substr(0, colon), line.substr(colon + 2));
        } else if (section == "frames") {
            if (line.empty()) continue;
            Frame frame;
            frame.index = int(out.frames.size());
            std::istringstream words(line);
            std::string address, offset;
            words >> address >> offset;
            frame.address = parseHex(address);
            if (!offset.empty() && offset.front() == '+') {   // Windows: +offset module
                frame.offset = parseHex(offset.substr(1));
                std::getline(words, frame.module);
                if (!frame.module.empty() && frame.module.front() == ' ') frame.module.erase(0, 1);
            }
            out.frames.push_back(std::move(frame));
        } else if (section == "modules") {
            // 55d0c1a00000-55d0c1a2f000 r-xp 00012000 08:01 1234   /path/to/binary
            std::istringstream words(line);
            std::string range, perms, offset, device, inode;
            words >> range >> perms >> offset >> device >> inode;
            std::string path;
            std::getline(words, path);
            path.erase(0, path.find_first_not_of(' '));
            const size_t dash = range.find('-');
            if (dash == std::string::npos || path.empty() || path.front() != '/') continue;
            maps.push_back({parseHex(range.substr(0, dash)), parseHex(range.substr(dash + 1)), parseHex(offset), path});
        } else if (section == "symbols") {
            symbols.push_back(line);
        } else if (section == "log") {
            if (!line.empty()) out.recentLog.push_back(line);
        }
    }
    if (const auto it =
            std::find_if(out.context.begin(), out.context.end(), [](const auto& kv) { return kv.first == "started"; });
        it != out.context.end())
        out.startedAt = it->second;

    std::vector<std::pair<std::string, fs::path>> resolved;   // module -> file with symbols
    const auto symbolFileFor = [&](const std::string& module) -> fs::path {
        for (const auto& [name, file] : resolved)
            if (name == module) return file;
        fs::path file;
        std::error_code ec;
        const std::string id = elfBuildId(module);
        if (!id.empty())
            for (const fs::path& folder : symbolFolders) {
                const fs::path candidate = folder / id / fs::path(module).filename();
                if (fs::is_regular_file(candidate, ec)) {
                    file = candidate;
                    break;
                }
            }
        if (file.empty() && fs::is_regular_file(module, ec)) file = module;
        resolved.emplace_back(module, file);
        return file;
    };

    for (Frame& frame : out.frames) {
        if (frame.index < int(symbols.size())) {
            // macOS: "3   player   0x0000000100003f20 main + 32"
            const std::string& text = symbols[size_t(frame.index)];
            const size_t hex = text.find(" 0x");
            if (hex != std::string::npos) {
                const size_t name = text.find(' ', hex + 3);
                if (name != std::string::npos)
                    frame.function = demangle(text.substr(name + 1, text.find(" + ", name + 1) - name - 1));
            }
            continue;
        }
        if (frame.module.empty()) {
            for (const Mapping& m : maps)
                if (frame.address >= m.start && frame.address < m.end) {
                    frame.module = m.path;
                    frame.offset = frame.address - m.start + m.fileOffset;   // a file offset for now
                    break;
                }
            if (frame.module.empty()) continue;
            const fs::path file = symbolFileFor(frame.module);
            if (file.empty()) continue;
            // A return address points after its call: look up the byte before it, except
            // for the innermost frame (where it faulted).
            uint64_t address = 0;
            Elf elf(file);
            if (!elf.ok || !elf.fileOffsetToAddress(frame.offset, address)) continue;
            frame.offset = address;
            frame.function = elfFunctionAt(file, frame.index > 0 && address > 0 ? address - 1 : address);
            if (!frame.function.empty()) frame.symbolFile = file.string();
        }
    }
    // Leave out the crash handler's own frames and the system's signal trampoline after
    // them: the first frame shown is where it crashed.
    for (size_t i = out.frames.size(); i-- > 0;)
        if (out.frames[i].function.find("diagnostics::(anonymous namespace)::onSignal(") != std::string::npos) {
            const size_t drop = std::min(out.frames.size(), i + 2);
            out.frames.erase(out.frames.begin(), out.frames.begin() + std::ptrdiff_t(drop));
            for (size_t k = 0; k < out.frames.size(); ++k) out.frames[k].index = int(k);
            break;
        }
    return true;
}

}   // namespace shockwave::diagnostics
