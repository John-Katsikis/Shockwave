// GPU selection (Renderer/GpuSelection.h): which GPU is picked, why, and the warnings,
// on made-up machines. No GPU needed.
#include <Renderer/GpuSelection.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace shockwave;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
static bool anyWarning(const GpuChoice& c, const char* part) {
    for (const std::string& w : c.warnings)
        if (contains(w, part)) return true;
    return false;
}

static GpuCandidate gpu(const char* name, GpuKind kind, uint32_t vendor, int minor, const char* pci, uint64_t mb) {
    GpuCandidate g;
    g.name = name;
    g.kind = kind;
    g.vendorId = vendor;
    g.vulkanVersion = "1.4.0";
    g.memoryMB = mb;
    if (minor >= 0) g.drmMajor = 226, g.drmMinor = minor;
    g.pciAddress = pci;
    return g;
}

static GpuCandidate intel() {
    return gpu("Intel(R) UHD Graphics 770 (ADL-S GT1)", GpuKind::Integrated, 0x8086, 0, "0000:00:02.0", 0);
}
static GpuCandidate rtx() {
    return gpu("NVIDIA GeForce RTX 3080", GpuKind::Discrete, 0x10de, 1, "0000:01:00.0", 10240);
}
static GpuCandidate llvmpipe() {
    return gpu("llvmpipe (LLVM 20.1.8, 256 bits)", GpuKind::Software, 0x10005, -1, "", 0);
}

static DisplayGpu screenOn(const char* card, uint32_t vendor, int minor, const char* pci) {
    DisplayGpu d;
    d.card = card;
    d.vendorId = vendor;
    d.drmMajor = 226;
    d.drmMinor = minor;
    d.pciAddress = pci;
    d.connectors = {"DP-1"};
    return d;
}

static void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text << "\n";
}

int main() {
    const DisplayGpu screenOnRtx = screenOn("card1", 0x10de, 1, "0000:01:00.0");

    // A desktop like Yianni's: the screen on the 3080.
    {
        const GpuChoice c = chooseGpu({intel(), rtx(), llvmpipe()}, {screenOnRtx}, "");
        CHECK(c.chosen == 1);
        CHECK(c.warnings.empty());
        CHECK(contains(c.reason, "dedicated"));
        CHECK(c.gpus[1].display == GpuDisplay::Yes);
        CHECK(c.gpus[0].display == GpuDisplay::No);
        CHECK(contains(describeGpuChoice(c), "* 1  NVIDIA GeForce RTX 3080"));
        CHECK(contains(gpuChoiceJson(c), "\"chosen\": 1"));
    }
    // Same desktop after a driver update without a reboot: Vulkan can't see the 3080.
    {
        const GpuChoice c = chooseGpu({intel(), llvmpipe()}, {screenOnRtx}, "");
        CHECK(c.chosen == 0);
        CHECK(anyWarning(c, "Vulkan can't see"));
        CHECK(anyWarning(c, "nvidia-utils"));
    }
    // Asking for the iGPU on that desktop works, with a warning that frames are copied.
    {
        const GpuChoice c = chooseGpu({intel(), rtx(), llvmpipe()}, {screenOnRtx}, "intel");
        CHECK(c.chosen == 0);
        CHECK(contains(c.reason, "asked"));
        CHECK(anyWarning(c, "plugged into \"NVIDIA GeForce RTX 3080\""));
        CHECK(anyWarning(c, "--gpu nvidia"));
    }
    // By number, any case of name, and a request that matches nothing.
    {
        CHECK(chooseGpu({intel(), rtx()}, {}, "0").chosen == 0);
        CHECK(chooseGpu({intel(), rtx()}, {}, "rTx 3080").chosen == 1);
        const GpuChoice c = chooseGpu({intel(), rtx()}, {}, "radeon");
        CHECK(c.chosen == 1);
        CHECK(anyWarning(c, "No GPU matches \"radeon\""));
        CHECK(chooseGpu({intel(), rtx()}, {}, "9").chosen == 1);   // past the list, in no name
        CHECK(chooseGpu({intel(), rtx()}, {}, "770").chosen == 0);   // a model number is a name
    }
    // A laptop: the screen on the iGPU, the dedicated GPU still renders, with a note.
    {
        const GpuChoice c = chooseGpu({intel(), rtx()}, {screenOn("card0", 0x8086, 0, "0000:00:02.0")}, "");
        CHECK(c.chosen == 1);
        CHECK(anyWarning(c, "--gpu intel "));
        CHECK(anyWarning(c, "laptop"));
    }
    // Two of a kind: the one with the screen wins over more memory.
    {
        GpuCandidate big = gpu("Big GPU", GpuKind::Discrete, 0x1002, 2, "0000:02:00.0", 24000);
        const GpuChoice c = chooseGpu({big, rtx()}, {screenOnRtx}, "");
        CHECK(c.chosen == 1);
        CHECK(contains(c.reason, "screen"));
        // Unknown screens: more memory wins.
        CHECK(chooseGpu({rtx(), big}, {}, "").chosen == 1);
    }
    // A GPU that can't run the engine is skipped, even when asked for.
    {
        GpuCandidate old = rtx();
        old.problem = "its driver supports Vulkan 1.2.0, Shockwave needs 1.3 (update the driver)";
        const GpuChoice c = chooseGpu({intel(), old, llvmpipe()}, {screenOnRtx}, "3080");
        CHECK(c.chosen == 0);
        CHECK(anyWarning(c, "was asked for but can't run"));
        CHECK(anyWarning(c, "is faster but can't run"));
        CHECK(anyWarning(c, "It can't be used"));
    }
    // Software only: a loud warning. Nothing usable: no choice.
    {
        GpuCandidate broken = rtx();
        broken.problem = "it can't draw into this window";
        const GpuChoice c = chooseGpu({broken, llvmpipe()}, {}, "");
        CHECK(c.chosen == 1);
        CHECK(anyWarning(c, "renders on the CPU"));
        CHECK(anyWarning(c, "can't draw into this window"));
        CHECK(chooseGpu({broken}, {}, "").chosen == -1);
        CHECK(chooseGpu({}, {}, "").chosen == -1);
    }
    // Reading screens from a made-up /sys. That's how Linux finds them; the other systems
    // don't have /sys, and Windows can't make the symlink below without admin rights.
#if defined(__linux__)
    {
        const fs::path sys = fs::temp_directory_path() / "shockwave_gpu_sysfs";
        fs::remove_all(sys);
        const fs::path pci = sys / "devices" / "pci0000:00" / "0000:01:00.0";
        write(pci / "vendor", "0x10de");
        const fs::path drm = sys / "class" / "drm";
        fs::create_directories(drm / "card1");
        fs::create_directory_symlink(pci, drm / "card1" / "device");
        write(drm / "card1" / "dev", "226:1");
        write(drm / "card1-DP-1" / "status", "connected");
        write(drm / "card1-HDMI-A-1" / "status", "disconnected");
        write(drm / "card0" / "dev", "226:0");
        write(drm / "card0-eDP-1" / "status", "disconnected");
        const auto found = findDisplayGpus(sys);
        CHECK(found.size() == 1);
        if (found.size() == 1) {
            CHECK(found[0].card == "card1");
            CHECK(found[0].vendorId == 0x10de);
            CHECK(found[0].drmMajor == 226 && found[0].drmMinor == 1);
            CHECK(found[0].pciAddress == "0000:01:00.0");
            CHECK(found[0].connectors.size() == 1 && found[0].connectors[0] == "DP-1");
        }
        CHECK(findDisplayGpus(sys / "missing").empty());
        fs::remove_all(sys);
    }
#endif
    // The environment variable round trip.
    setRequestedGpu("3080");
    CHECK(requestedGpu() == "3080");
    setRequestedGpu("");
    CHECK(requestedGpu().empty());

    if (failures) {
        std::fprintf(stderr, "%d GPU selection checks failed\n", failures);
        return 1;
    }
    std::puts("GPU selection: all checks passed");
    return 0;
}
