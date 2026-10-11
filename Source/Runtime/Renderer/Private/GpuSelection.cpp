#include <Renderer/GpuSelection.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace shockwave {

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string readLine(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    return line;
}

int kindRank(GpuKind kind) {
    switch (kind) {
    case GpuKind::Discrete: return 0;
    case GpuKind::Integrated: return 1;
    case GpuKind::Virtual: return 2;
    case GpuKind::Other: return 3;
    case GpuKind::Software: return 4;
    }
    return 5;
}

int displayRank(GpuDisplay display) { return display == GpuDisplay::Yes ? 0 : display == GpuDisplay::Unknown ? 1 : 2; }

bool sameDevice(const GpuCandidate& gpu, const DisplayGpu& display) {
    if (gpu.drmMajor >= 0 && display.drmMajor >= 0)
        return gpu.drmMajor == display.drmMajor && gpu.drmMinor == display.drmMinor;
    return !gpu.pciAddress.empty() && lower(gpu.pciAddress) == lower(display.pciAddress);
}

std::string quote(std::string_view text) { return "\"" + std::string(text) + "\""; }

// The shortest way to ask for this GPU again: a distinctive word of its name, else its number.
std::string howToAsk(const GpuChoice& choice, size_t index) {
    const std::string name = lower(choice.gpus[index].name);
    std::istringstream words(name);
    std::string word;
    while (words >> word) {
        // "intel(r)" -> "intel"
        word.erase(
            std::find_if(word.begin(), word.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }),
            word.end());
        if (word.size() < 3 || word == "graphics" || word == "geforce") continue;
        size_t matches = 0;
        for (const GpuCandidate& other : choice.gpus) matches += lower(other.name).find(word) != std::string::npos;
        if (matches == 1) return word;
    }
    return std::to_string(index);
}

std::string displayName(const DisplayGpu& display) {
    std::string out = gpuVendorName(display.vendorId);
    out += " GPU (" + display.card;
    if (!display.pciAddress.empty()) out += ", PCI " + display.pciAddress;
    return out + ")";
}

// The kind as one word, for JSON.
const char* kindId(GpuKind kind) {
    switch (kind) {
    case GpuKind::Discrete: return "discrete";
    case GpuKind::Integrated: return "integrated";
    case GpuKind::Virtual: return "virtual";
    case GpuKind::Software: return "software";
    case GpuKind::Other: break;
    }
    return "other";
}

std::string jsonEscape(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\')
            out += '\\', out += c;
        else if (c == '\n')
            out += "\\n";
        else if (static_cast<unsigned char>(c) >= 0x20)
            out += c;
    }
    return out;
}

}   // namespace

const char* gpuKindName(GpuKind kind) {
    switch (kind) {
    case GpuKind::Discrete: return "dedicated";
    case GpuKind::Integrated: return "integrated";
    case GpuKind::Virtual: return "virtual";
    case GpuKind::Software: return "software (runs on the CPU)";
    case GpuKind::Other: break;
    }
    return "other";
}

const char* gpuVendorName(uint32_t vendorId) {
    switch (vendorId) {
    case 0x10de: return "NVIDIA";
    case 0x8086: return "Intel";
    case 0x1002: return "AMD";
    case 0x13b5: return "Arm";
    case 0x5143: return "Qualcomm";
    case 0x106b: return "Apple";
    case 0x10005: return "Mesa";
    default: return "unknown";
    }
}

std::string requestedGpu() {
    const char* value = std::getenv("SHOCKWAVE_GPU");
    return value ? std::string(value) : std::string();
}

void setRequestedGpu(std::string_view request) {
    const std::string value(request);
#ifdef _WIN32
    _putenv_s("SHOCKWAVE_GPU", value.c_str());
#else
    if (value.empty())
        unsetenv("SHOCKWAVE_GPU");
    else
        setenv("SHOCKWAVE_GPU", value.c_str(), 1);
#endif
}

std::vector<DisplayGpu> findDisplayGpus(const std::filesystem::path& sysfs) {
    std::vector<DisplayGpu> out;
    std::error_code ec;
    const std::filesystem::path drm = sysfs / "class" / "drm";
    if (!std::filesystem::is_directory(drm, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(drm, ec)) {
        // Connectors are named after their card: card1-DP-1, card1-HDMI-A-2.
        const std::string name = entry.path().filename().string();
        const size_t dash = name.find('-');
        if (name.rfind("card", 0) != 0 || dash == std::string::npos) continue;
        if (readLine(entry.path() / "status") != "connected") continue;
        const std::string card = name.substr(0, dash);
        auto it = std::find_if(out.begin(), out.end(), [&](const DisplayGpu& d) { return d.card == card; });
        if (it == out.end()) {
            DisplayGpu display;
            display.card = card;
            const std::filesystem::path cardPath = drm / card;
            const std::string dev = readLine(cardPath / "dev");   // "226:1"
            const size_t colon = dev.find(':');
            if (colon != std::string::npos) {
                std::from_chars(dev.data(), dev.data() + colon, display.drmMajor);
                std::from_chars(dev.data() + colon + 1, dev.data() + dev.size(), display.drmMinor);
            }
            const auto device = std::filesystem::canonical(cardPath / "device", ec);
            if (!ec) display.pciAddress = device.filename().string();
            const std::string vendor = readLine(cardPath / "device" / "vendor");   // "0x10de"
            if (vendor.size() > 2)
                std::from_chars(vendor.data() + 2, vendor.data() + vendor.size(), display.vendorId, 16);
            out.push_back(std::move(display));
            it = out.end() - 1;
        }
        it->connectors.push_back(name.substr(dash + 1));
    }
    std::sort(out.begin(), out.end(), [](const DisplayGpu& a, const DisplayGpu& b) { return a.card < b.card; });
    for (DisplayGpu& display : out) std::sort(display.connectors.begin(), display.connectors.end());
    return out;
}

GpuChoice chooseGpu(std::vector<GpuCandidate> gpus, std::vector<DisplayGpu> displays, std::string_view request) {
    GpuChoice choice;
    choice.gpus = std::move(gpus);
    choice.displays = std::move(displays);
    choice.request = std::string(request);
    const bool displaysKnown = !choice.displays.empty();
    for (GpuCandidate& gpu : choice.gpus) {
        if (!displaysKnown) continue;
        const bool drives = std::any_of(
            choice.displays.begin(), choice.displays.end(), [&](const DisplayGpu& d) { return sameDevice(gpu, d); });
        // Software GPUs have no hardware to plug a screen into.
        gpu.display = drives ? GpuDisplay::Yes
                      : gpu.kind == GpuKind::Software || gpu.drmMajor >= 0 || !gpu.pciAddress.empty()
                          ? GpuDisplay::No
                          : GpuDisplay::Unknown;
    }

    // 1. Asked for.
    if (!choice.request.empty()) {
        int match = -1;
        int index = -1;
        const auto [end, ec] = std::from_chars(request.data(), request.data() + request.size(), index);
        if (ec == std::errc{} && end == request.data() + request.size() && index >= 0 &&
            index < static_cast<int>(choice.gpus.size())) {
            match = index;
        } else {   // part of a name; "3080" is a name, not a number past the end of the list
            const std::string wanted = lower(request);
            for (size_t i = 0; i < choice.gpus.size() && match < 0; ++i)
                if (lower(choice.gpus[i].name).find(wanted) != std::string::npos) match = static_cast<int>(i);
        }
        if (match < 0) {
            std::string names;
            for (size_t i = 0; i < choice.gpus.size(); ++i)
                names += (i ? ", " : "") + std::to_string(i) + " " + quote(choice.gpus[i].name);
            choice.warnings.push_back(
                "No GPU matches " + quote(request) + ", so it was picked automatically. The GPUs are: " + names +
                ". Ask for one by its number or part of its name.");
        } else if (!choice.gpus[static_cast<size_t>(match)].problem.empty()) {
            choice.warnings.push_back(
                quote(choice.gpus[static_cast<size_t>(match)].name) + " was asked for but can't run Shockwave (" +
                choice.gpus[static_cast<size_t>(match)].problem + "), so another was picked automatically.");
        } else {
            choice.chosen = match;
            choice.reason = "You asked for it (" + quote(request) + ").";
        }
    }

    // 2. Automatic: the fastest kind, then the one driving a screen, then more memory.
    if (choice.chosen < 0) {
        std::vector<size_t> usable;
        for (size_t i = 0; i < choice.gpus.size(); ++i)
            if (choice.gpus[i].problem.empty()) usable.push_back(i);
        if (usable.empty()) {
            choice.reason = "No GPU can run Shockwave.";
            return choice;
        }
        const auto better = [&](size_t a, size_t b) {
            const GpuCandidate& x = choice.gpus[a];
            const GpuCandidate& y = choice.gpus[b];
            if (kindRank(x.kind) != kindRank(y.kind)) return kindRank(x.kind) < kindRank(y.kind);
            if (displayRank(x.display) != displayRank(y.display))
                return displayRank(x.display) < displayRank(y.display);
            if (x.memoryMB != y.memoryMB) return x.memoryMB > y.memoryMB;
            return a < b;
        };
        const size_t best = *std::min_element(usable.begin(), usable.end(), better);
        choice.chosen = static_cast<int>(best);
        const GpuCandidate& gpu = choice.gpus[best];
        if (usable.size() == 1)
            choice.reason = "It's the only GPU that can run Shockwave.";
        else {
            const bool sameKindRival = std::any_of(
                usable.begin(), usable.end(), [&](size_t i) { return i != best && choice.gpus[i].kind == gpu.kind; });
            choice.reason = std::string("It's the fastest kind available (") + gpuKindName(gpu.kind) + ")";
            if (sameKindRival && gpu.display == GpuDisplay::Yes)
                choice.reason += " and your screen is plugged into it";
            else if (sameKindRival)
                choice.reason += " and has the most memory";
            choice.reason += ".";
        }
    }

    const size_t chosen = static_cast<size_t>(choice.chosen);
    const GpuCandidate& gpu = choice.gpus[chosen];

    // Warnings: anything that makes the result slower or more fragile than it could be.
    if (gpu.kind == GpuKind::Software) {
        std::string text = quote(gpu.name) + " renders on the CPU, so expect very low frame rates.";
        for (const GpuCandidate& other : choice.gpus)
            if (other.kind != GpuKind::Software && !other.problem.empty())
                text += " " + quote(other.name) + " can't be used: " + other.problem + ".";
        choice.warnings.push_back(text);
    }
    for (size_t i = 0; i < choice.gpus.size(); ++i) {
        const GpuCandidate& other = choice.gpus[i];
        if (i != chosen && other.problem.empty() == false && kindRank(other.kind) < kindRank(gpu.kind) &&
            gpu.kind != GpuKind::Software)
            choice.warnings.push_back(
                quote(other.name) + " is faster but can't run Shockwave (" + other.problem + "), so " +
                quote(gpu.name) + " is used.");
    }
    if (displaysKnown && gpu.display == GpuDisplay::No) {
        // Which GPU does the screen hang off, and can Vulkan see it?
        int screenGpu = -1;
        for (size_t i = 0; i < choice.gpus.size() && screenGpu < 0; ++i)
            if (choice.gpus[i].display == GpuDisplay::Yes) screenGpu = static_cast<int>(i);
        const DisplayGpu* unseen = nullptr;
        for (const DisplayGpu& display : choice.displays) {
            const bool seen = std::any_of(
                choice.gpus.begin(), choice.gpus.end(), [&](const GpuCandidate& g) { return sameDevice(g, display); });
            if (!seen && !unseen) unseen = &display;
        }
        if (screenGpu >= 0) {
            const GpuCandidate& screen = choice.gpus[static_cast<size_t>(screenGpu)];
            std::string text = quote(gpu.name) + " is rendering, but your screen is plugged into " +
                               quote(screen.name) + ". Every frame is copied across, which adds latency and fails " +
                               "on some drivers.";
            if (screen.problem.empty())
                text += " To render on " + quote(screen.name) + ", start with --gpu " +
                        howToAsk(choice, static_cast<size_t>(screenGpu)) +
                        " (or set it in Preferences). On a laptop with switchable graphics this is normal.";
            else
                text += " It can't be used: " + screen.problem + ".";
            choice.warnings.push_back(text);
        } else if (unseen) {
            std::string text = "Your screen is plugged into an " + displayName(*unseen) +
                               " that Vulkan can't see, so " + quote(gpu.name) +
                               " renders instead and every frame is copied across. That GPU's Vulkan driver is "
                               "missing or doesn't match its kernel module: reboot after a driver update, and "
                               "check that `vulkaninfo --summary` lists it.";
            if (unseen->vendorId == 0x10de)
                text += " On NVIDIA, install nvidia-utils (Arch: sudo pacman -S nvidia-utils).";
            else if (unseen->vendorId == 0x1002 || unseen->vendorId == 0x8086)
                text += " Install Mesa's Vulkan driver (Arch: sudo pacman -S vulkan-radeon or vulkan-intel).";
            choice.warnings.push_back(text);
        }
    }
    return choice;
}

std::string describeGpuChoice(const GpuChoice& choice) {
    std::ostringstream out;
    out << "GPUs Vulkan can see:\n";
    if (choice.gpus.empty()) out << "  none\n";
    for (size_t i = 0; i < choice.gpus.size(); ++i) {
        const GpuCandidate& gpu = choice.gpus[i];
        out << (static_cast<int>(i) == choice.chosen ? "* " : "  ") << i << "  " << gpu.name << "\n"
            << "     " << gpuKindName(gpu.kind) << ", " << gpuVendorName(gpu.vendorId) << ", Vulkan "
            << gpu.vulkanVersion;
        if (!gpu.driver.empty()) out << ", driver " << gpu.driver;
        if (gpu.memoryMB) out << ", " << gpu.memoryMB << " MB";
        if (!gpu.pciAddress.empty()) out << ", PCI " << gpu.pciAddress;
        out << "\n";
        if (gpu.display == GpuDisplay::Yes) out << "     your screen is plugged into it\n";
        if (!gpu.problem.empty()) out << "     can't run Shockwave: " << gpu.problem << "\n";
    }
    out << "Screens are plugged into: ";
    if (choice.displays.empty()) out << "unknown";
    for (size_t i = 0; i < choice.displays.size(); ++i) {
        const DisplayGpu& d = choice.displays[i];
        out << (i ? "; " : "") << displayName(d) << " ";
        for (size_t c = 0; c < d.connectors.size(); ++c) out << (c ? ", " : "") << d.connectors[c];
    }
    out << "\n";
    if (const GpuCandidate* gpu = choice.chosenGpu())
        out << "Using " << choice.chosen << " " << gpu->name << ". " << choice.reason << "\n";
    else
        out << "No GPU can run Shockwave.\n";
    if (!choice.request.empty()) out << "Asked for: " << choice.request << "\n";
    for (const std::string& warning : choice.warnings) out << "Warning: " << warning << "\n";
    return out.str();
}

std::string gpuChoiceJson(const GpuChoice& choice) {
    std::ostringstream out;
    const auto displayText = [](GpuDisplay d) {
        return d == GpuDisplay::Yes ? "yes" : d == GpuDisplay::No ? "no" : "unknown";
    };
    out << "{\n  \"chosen\": " << choice.chosen << ",\n  \"request\": \"" << jsonEscape(choice.request)
        << "\",\n  \"reason\": \"" << jsonEscape(choice.reason) << "\",\n  \"gpus\": [";
    for (size_t i = 0; i < choice.gpus.size(); ++i) {
        const GpuCandidate& gpu = choice.gpus[i];
        out << (i ? "," : "") << "\n    {\"index\": " << i << ", \"name\": \"" << jsonEscape(gpu.name)
            << "\", \"kind\": \"" << kindId(gpu.kind) << "\", \"vendor\": \"" << gpuVendorName(gpu.vendorId)
            << "\", \"vulkan\": \"" << jsonEscape(gpu.vulkanVersion) << "\", \"driver\": \"" << jsonEscape(gpu.driver)
            << "\", \"memory_mb\": " << gpu.memoryMB << ", \"pci\": \"" << jsonEscape(gpu.pciAddress)
            << "\", \"drives_screen\": \"" << displayText(gpu.display)
            << "\", \"usable\": " << (gpu.problem.empty() ? "true" : "false") << ", \"problem\": \""
            << jsonEscape(gpu.problem) << "\"}";
    }
    out << "\n  ],\n  \"screens\": [";
    for (size_t i = 0; i < choice.displays.size(); ++i) {
        const DisplayGpu& d = choice.displays[i];
        out << (i ? "," : "") << "\n    {\"card\": \"" << jsonEscape(d.card) << "\", \"pci\": \""
            << jsonEscape(d.pciAddress) << "\", \"vendor\": \"" << gpuVendorName(d.vendorId) << "\", \"connectors\": [";
        for (size_t c = 0; c < d.connectors.size(); ++c)
            out << (c ? ", " : "") << "\"" << jsonEscape(d.connectors[c]) << "\"";
        out << "]}";
    }
    out << "\n  ],\n  \"warnings\": [";
    for (size_t i = 0; i < choice.warnings.size(); ++i)
        out << (i ? "," : "") << "\n    \"" << jsonEscape(choice.warnings[i]) << "\"";
    out << "\n  ]\n}\n";
    return out.str();
}

}   // namespace shockwave
