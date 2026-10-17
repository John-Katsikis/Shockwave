#pragma once

// Runs a command line and hands its output over line by line, for the packaging steps
// that call other programs (CMake, the player's --check, tar).

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace shockwave::packaging {

// Quotes one argument for the shell, so paths with spaces or quotes survive.
std::string shellQuote(const std::filesystem::path& path);
std::string shellQuote(std::string_view text);

// Runs `command` through the shell with standard error merged into standard output, calls
// `line` for each line, and returns the exit code (-1 if it couldn't start).
int runCommand(const std::string& command, const std::function<void(std::string_view)>& line);

}   // namespace shockwave::packaging
