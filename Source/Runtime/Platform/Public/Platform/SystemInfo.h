#pragma once

// The machine a measurement ran on, and how much memory this process holds (S10,
// docs/Performance.md). A performance record names its hardware, so "60 fps" always
// says where; nothing here is used to change how the game runs.

#include <cstdint>
#include <string>

namespace shockwave {

struct SystemInfo {
    std::string os;   // "Linux", "Windows", "macOS"
    std::string osVersion;   // the kernel or OS release: "6.8.0-45-generic", "10.0.22631"
    std::string cpu;   // the CPU's model name, or "" when the system doesn't say
    uint32_t logicalCores = 0;
    uint64_t ramMib = 0;   // installed system memory
};

// Read once and kept; cheap after the first call.
const SystemInfo& systemInfo();

struct ProcessMemory {
    uint64_t residentBytes = 0;   // memory this process holds right now (RSS / working set)
    uint64_t peakResidentBytes = 0;   // the most it has held since it started; 0 if unknown
};

// This process's memory now. Zeros where the system can't say.
ProcessMemory processMemory();

}   // namespace shockwave
