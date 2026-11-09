// The machine and this process's memory, per OS (see SystemInfo.h).
#include <Platform/SystemInfo.h>

#include <SDL3/SDL.h>

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#else
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace shockwave {

namespace {

std::string trim(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r' || text.back() == '\t'))
        text.pop_back();
    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) ++start;
    return text.substr(start);
}

SystemInfo readSystemInfo() {
    SystemInfo info;
    info.logicalCores = static_cast<uint32_t>(std::max(SDL_GetNumLogicalCPUCores(), 0));
    info.ramMib = static_cast<uint64_t>(std::max(SDL_GetSystemRAM(), 0));
#if defined(_WIN32)
    info.os = "Windows";
    // RtlGetVersion tells the truth where GetVersionEx lies about compatibility.
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
        if (auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(ntdll, "RtlGetVersion"))) {
            OSVERSIONINFOW v{};
            v.dwOSVersionInfoSize = sizeof v;
            if (get(&v) == 0)
                info.osVersion = std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." +
                                 std::to_string(v.dwBuildNumber);
        }
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key) ==
        ERROR_SUCCESS) {
        char name[256] = {};
        DWORD size = sizeof name - 1;
        if (RegQueryValueExA(key, "ProcessorNameString", nullptr, nullptr, reinterpret_cast<LPBYTE>(name), &size) ==
            ERROR_SUCCESS)
            info.cpu = trim(name);
        RegCloseKey(key);
    }
#elif defined(__APPLE__)
    info.os = "macOS";
    char text[256] = {};
    size_t size = sizeof text - 1;
    if (sysctlbyname("kern.osproductversion", text, &size, nullptr, 0) == 0) info.osVersion = trim(text);
    size = sizeof text - 1;
    std::memset(text, 0, sizeof text);
    if (sysctlbyname("machdep.cpu.brand_string", text, &size, nullptr, 0) == 0) info.cpu = trim(text);
#else
    info.os = "Linux";
    if (utsname u{}; uname(&u) == 0) info.osVersion = u.release;
    std::ifstream cpuinfo("/proc/cpuinfo");
    for (std::string line; std::getline(cpuinfo, line);) {
        // x86 says "model name"; many ARM kernels (Jetson Orin among them) say only "Model"
        // or nothing per core.
        if (line.rfind("model name", 0) == 0 || line.rfind("Model", 0) == 0) {
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                info.cpu = trim(line.substr(colon + 1));
                if (!info.cpu.empty()) break;
            }
        }
    }
    if (info.cpu.empty()) {
        std::ifstream model("/proc/device-tree/model");   // ARM boards: "NVIDIA Jetson AGX Orin ..."
        std::string text;
        std::getline(model, text, '\0');
        info.cpu = trim(text);
    }
#endif
    return info;
}

}   // namespace

const SystemInfo& systemInfo() {
    static const SystemInfo info = readSystemInfo();
    return info;
}

ProcessMemory processMemory() {
    ProcessMemory memory;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters)) {
        memory.residentBytes = counters.WorkingSetSize;
        memory.peakResidentBytes = counters.PeakWorkingSetSize;
    }
#elif defined(__APPLE__)
    mach_task_basic_info_data_t task{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&task), &count) == KERN_SUCCESS)
        memory.residentBytes = task.resident_size;
    if (rusage usage{}; getrusage(RUSAGE_SELF, &usage) == 0)
        memory.peakResidentBytes = static_cast<uint64_t>(usage.ru_maxrss);   // bytes on macOS
#else
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);) {
        auto kib = [&](const char* key) -> uint64_t {
            if (line.rfind(key, 0) != 0) return 0;
            return std::strtoull(line.c_str() + std::strlen(key), nullptr, 10) * 1024;
        };
        if (const uint64_t v = kib("VmRSS:")) memory.residentBytes = v;
        if (const uint64_t v = kib("VmHWM:")) memory.peakResidentBytes = v;
    }
#endif
    return memory;
}

}   // namespace shockwave
