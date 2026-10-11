#pragma once

// What a program or library needs from the system it runs on, read straight from the
// file, so packaging can tell whether a package stands on its own. Linux (ELF) only for
// now; other formats come back as not a binary.

#include <filesystem>
#include <string>
#include <vector>

namespace shockwave::packaging {

struct BinaryInfo {
    bool binary = false;   // an ELF program or library
    std::vector<std::string> needed;   // libraries it links against (DT_NEEDED)
    std::vector<std::string> searchPaths;   // where it looks for them (RUNPATH or RPATH)
    std::string glibc;   // newest glibc symbol version it uses ("2.38"), empty for none
    std::string glibcxx;   // newest libstdc++ symbol version it uses ("3.4.32"), empty for none
};

BinaryInfo readBinaryInfo(const std::filesystem::path& file);

// Compares dotted version numbers: <0, 0 or >0, like strcmp. "" is older than anything.
int compareVersions(const std::string& a, const std::string& b);

// The GCC release whose libstdc++ first had this GLIBCXX version ("3.4.32" -> "13.2"),
// empty when it isn't known.
std::string gccForGlibcxx(const std::string& glibcxx);

}   // namespace shockwave::packaging
