// The child the `process` test starts (Tests/ProcessTest.cpp): it reports what it was
// given, exactly, so the test can compare.
//
//   process_child args <arg>...        each argument as hex, one per line
//   process_child env <name>...        "<name>=<value>" or "<name> unset"
//   process_child cwd                  the working directory
//   process_child lines                output with \n, \r\n, stderr and no final newline
//   process_child exit <code>
//   process_child sleep <seconds>
//   process_child tree <pidfile>       starts a grandchild that writes its pid and sleeps
//   process_child grandchild <pidfile>
//   process_child write <file>         writes "started" to the file
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string hex(const std::string& text) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : text) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out.empty() ? "-" : out;
}

#if defined(_WIN32)
std::string narrow(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(size > 0 ? size - 1 : 0), '\0');
    if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}
#endif

fs::path pathOf(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

void sleepSeconds(double seconds) { std::this_thread::sleep_for(std::chrono::duration<double>(seconds)); }

long currentPid() {
#if defined(_WIN32)
    return long(GetCurrentProcessId());
#else
    return long(getpid());
#endif
}

}   // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(narrow(wargv[i]));
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);
#endif
    if (args.size() < 2) return 2;
    const std::string mode = args[1];
    if (mode == "args") {
        for (size_t i = 2; i < args.size(); ++i) std::printf("%s\n", hex(args[i]).c_str());
        return 0;
    }
    if (mode == "env") {
        for (size_t i = 2; i < args.size(); ++i) {
#if defined(_WIN32)
            std::vector<wchar_t> name(args[i].begin(), args[i].end());
            name.push_back(0);
            wchar_t value[4096];
            const DWORD n = GetEnvironmentVariableW(name.data(), value, 4096);
            if (n == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
                std::printf("%s unset\n", args[i].c_str());
            else
                std::printf("%s=%s\n", args[i].c_str(), narrow(std::wstring(value, n).c_str()).c_str());
#else
            const char* value = std::getenv(args[i].c_str());
            if (value)
                std::printf("%s=%s\n", args[i].c_str(), value);
            else
                std::printf("%s unset\n", args[i].c_str());
#endif
        }
        return 0;
    }
    if (mode == "cwd") {
        const std::u8string cwd = fs::current_path().u8string();
        std::printf("%s\n", hex(std::string(cwd.begin(), cwd.end())).c_str());
        return 0;
    }
    if (mode == "lines") {
        std::fputs("one\ntwo\r\n", stdout);
        std::fflush(stdout);
        std::fputs("three on stderr\n", stderr);
        std::fflush(stderr);
        std::fputs("four without an end", stdout);
        return 0;
    }
    if (mode == "exit" && args.size() > 2) return std::atoi(args[2].c_str());
    if (mode == "sleep" && args.size() > 2) {
        std::printf("sleeping\n");
        std::fflush(stdout);
        sleepSeconds(std::atof(args[2].c_str()));
        return 0;
    }
    if (mode == "tree" && args.size() > 2) {
        // Through the C library's own child, in our process group (and job), as a build
        // tool's compilers would be.
        const std::string command = "\"" + args[0] + "\" grandchild \"" + args[2] + "\"";
        std::printf("tree\n");
        std::fflush(stdout);
#if defined(_WIN32)
        return std::system(("\"" + command + "\"").c_str());
#else
        return std::system(command.c_str());
#endif
    }
    if (mode == "grandchild" && args.size() > 2) {
        {
            std::ofstream out(pathOf(args[2] + ".partial"));
            out << currentPid() << "\n";
        }
        std::error_code ec;
        fs::rename(pathOf(args[2] + ".partial"), pathOf(args[2]), ec);
        sleepSeconds(60);
        return 0;
    }
    if (mode == "write" && args.size() > 2) {
        std::ofstream(pathOf(args[2])) << "started\n";
        return 0;
    }
    return 2;
}
