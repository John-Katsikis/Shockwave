#pragma once

// Paths that come from data files (a cook's asset IDs, a package's file list) checked
// before anything is read, written or deleted with them (S12, docs/Diagnostics.md).

#include <string>
#include <string_view>

namespace shockwave {

// A relative path that stays where it's put on every platform: '/' between parts, none of
// them empty, "." or ".."; not absolute, no drive ("C:"), no '\\' or ':' and no control
// characters; no part ending in '.' or ' ' and none named CON, PRN, AUX, NUL, COM1-9 or
// LPT1-9 (Windows can't make those). False, with why, otherwise.
bool checkRelativePath(std::string_view path, std::string& why);

// The key two paths collide under on a filesystem that ignores case (Windows, macOS):
// ASCII letters lowercased.
std::string pathCaseKey(std::string_view path);

}   // namespace shockwave
