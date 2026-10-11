#pragma once

// Which GPU renders, and why. A PC can have several GPUs (a dedicated card plus the CPU's
// built-in one, and a software one from Mesa), and picking the wrong one is the usual
// cause of "it runs at 20 fps" or "the swapchain can't be created". The Vulkan renderer
// lists every GPU it finds, decides with chooseGpu, logs the result, and keeps it so the
// editor's Stats panel and `player --gpus` can show it.
//
// How the choice is made:
//   1. A GPU asked for by name or number wins, when it can run Shockwave: the player's
//      and editor's --gpu option, the SHOCKWAVE_GPU environment variable, or the
//      editor's Preferences > Rendering > GPU.
//   2. Otherwise the fastest kind: dedicated, then integrated, then virtual, then
//      software. Between two of a kind, the one driving a screen, then the one with more
//      memory.
// Then it warns when the chosen GPU isn't the one your screen is plugged into (frames
// have to be copied between GPUs), and when the screen's GPU is invisible to Vulkan
// (its driver is missing or doesn't match the kernel module).
//
// Nothing here touches Vulkan, so it can be tested without a GPU.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

enum class GpuKind { Discrete, Integrated, Virtual, Software, Other };
// Whether a screen is plugged into this GPU. Unknown off Linux, or when the system
// doesn't say.
enum class GpuDisplay { Unknown, Yes, No };

struct GpuCandidate {
    std::string name;
    GpuKind kind = GpuKind::Other;
    uint32_t vendorId = 0;
    std::string vulkanVersion;   // "1.4.312"
    std::string driver;   // "NVIDIA 580.95", "Mesa 26.2.4"
    uint64_t memoryMB = 0;   // device-local memory
    std::string problem;   // why it can't run Shockwave; empty when it can
    // Where it sits, to match it with the screens (either may be unknown).
    std::string pciAddress;   // "0000:01:00.0"
    int drmMajor = -1, drmMinor = -1;   // its /dev/dri/cardN
    GpuDisplay display = GpuDisplay::Unknown;   // filled in by chooseGpu
};

// A GPU with a screen plugged in, as the OS sees it (Linux: /sys/class/drm).
struct DisplayGpu {
    std::string card;   // "card1"
    std::string pciAddress;
    uint32_t vendorId = 0;
    int drmMajor = -1, drmMinor = -1;
    std::vector<std::string> connectors;   // "DP-1", "HDMI-A-2"
};

struct GpuChoice {
    std::vector<GpuCandidate> gpus;   // every GPU Vulkan listed, in its order
    std::vector<DisplayGpu> displays;   // empty when unknown
    int chosen = -1;   // index into gpus; -1 when none can run Shockwave
    std::string request;   // what was asked for ("" = automatic)
    std::string reason;   // why the chosen one, in a sentence
    std::vector<std::string> warnings;   // each a sentence or two, with what to do

    const GpuCandidate* chosenGpu() const { return chosen >= 0 ? &gpus[static_cast<size_t>(chosen)] : nullptr; }
};

// Decides; see the top of this file. `request` is a GPU number from the list (0, 1...) or
// part of its name, any case ("nvidia", "3080").
GpuChoice chooseGpu(std::vector<GpuCandidate> gpus, std::vector<DisplayGpu> displays, std::string_view request);

// The GPUs that have a connected screen, read from <sysfs>/class/drm. Empty off Linux or
// when it can't tell.
std::vector<DisplayGpu> findDisplayGpus(const std::filesystem::path& sysfs = "/sys");

// The GPU asked for in the SHOCKWAVE_GPU environment variable, or "". The player's and
// editor's --gpu option set it.
std::string requestedGpu();
// Sets (or, with "", clears) SHOCKWAVE_GPU for this process. It applies the next time a
// renderer starts, so the editor reloads its renderer after changing it.
void setRequestedGpu(std::string_view request);

const char* gpuKindName(GpuKind kind);   // "dedicated", "integrated", ...
const char* gpuVendorName(uint32_t vendorId);   // "NVIDIA", "Intel", "AMD", ... or "unknown"

// The report `player --gpus` prints: one line per GPU, then the choice, reason and
// warnings.
std::string describeGpuChoice(const GpuChoice& choice);
// The same as JSON, for tools (`player --gpus --json file`).
std::string gpuChoiceJson(const GpuChoice& choice);

}   // namespace shockwave
