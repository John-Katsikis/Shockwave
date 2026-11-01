// The pipeline cache (G12, docs/Drawing.md): every pipeline is created through one
// VkPipelineCache, loaded from the user's data folder before the first pipeline and saved
// when the renderer shuts down, so the next start compiles far less. The file belongs to
// this machine: it's keyed to the GPU, its driver and a hash of the compiled shaders, and
// anything that doesn't match (or doesn't parse) is ignored and replaced, never trusted.
// It's disposable: deleting it (player --clear-pipeline-cache) only costs one slower start.
//
// File layout: a CacheHeader, then the driver's own cache data (which the driver checks
// again on load).
#include "RendererImpl.h"

#include <Core/Log.h>

#include <random>

#include <fstream>
#include <iterator>
#include <system_error>

namespace shockwave {

namespace {

constexpr uint32_t kCacheMagic = 0x43505753;   // "SWPC"
constexpr uint32_t kCacheVersion = 1;
struct CacheHeader {
    uint32_t magic = kCacheMagic;
    uint32_t version = kCacheVersion;
    uint32_t vendorId = 0, deviceId = 0, driverVersion = 0, pad = 0;
    uint8_t uuid[VK_UUID_SIZE] = {};
    uint64_t shaderHash = 0;
    uint64_t dataSize = 0;
};

// FNV-1a over every compiled shader's name and bytes: new shaders, a new cache.
uint64_t shaderHash() {
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&](const char* data, size_t size) {
        for (size_t i = 0; i < size; ++i) hash = (hash ^ uint8_t(data[i])) * 1099511628211ull;
    };
    std::error_code ec;
    const std::filesystem::path folder = std::filesystem::path(shaderPath("x")).parent_path();
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
        if (entry.path().extension() == ".spv") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        const std::string name = file.filename().string();
        mix(name.data(), name.size());
        std::ifstream in(file, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        mix(bytes.data(), bytes.size());
    }
    return hash;
}

CacheHeader headerFor(const VkPhysicalDeviceProperties& props, uint64_t shaders) {
    CacheHeader header;
    header.vendorId = props.vendorID;
    header.deviceId = props.deviceID;
    header.driverVersion = props.driverVersion;
    std::memcpy(header.uuid, props.pipelineCacheUUID, VK_UUID_SIZE);
    header.shaderHash = shaders;
    return header;
}

}   // namespace

void VulkanRenderer::Impl::openPipelineCache(const std::string& folder) {
    if (folder.empty()) return;   // "off"
    const VkPhysicalDeviceProperties& props = context->vkbDevice().physical_device.properties;
    char name[64];
    std::snprintf(name, sizeof(name), "%04x-%04x.swpipelines", props.vendorID, props.deviceID);
    pipelineCacheFile = std::filesystem::path(folder) / name;
    const CacheHeader want = headerFor(props, shaderHash());

    std::vector<char> data;
    pipelineCacheState = "new";
    std::ifstream in(pipelineCacheFile, std::ios::binary);
    if (in) {
        CacheHeader have;
        in.read(reinterpret_cast<char*>(&have), sizeof(have));
        const bool same = in && have.magic == want.magic && have.version == want.version &&
                          have.vendorId == want.vendorId && have.deviceId == want.deviceId &&
                          have.driverVersion == want.driverVersion &&
                          std::memcmp(have.uuid, want.uuid, VK_UUID_SIZE) == 0 && have.shaderHash == want.shaderHash &&
                          have.dataSize < (uint64_t(1) << 31);
        if (same) {
            data.resize(size_t(have.dataSize));
            in.read(data.data(), std::streamsize(data.size()));
            if (!in) data.clear();
        }
        pipelineCacheState = !data.empty() ? "loaded" : "replaced: the driver or the shaders changed";
    }
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(context->device(), &info, nullptr, &pipelineCacheHandle) != VK_SUCCESS) {
        // The driver refused the data: start empty rather than not at all.
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        pipelineCacheState = "replaced: the driver refused the saved data";
        data.clear();
        vk::check(
            vkCreatePipelineCache(context->device(), &info, nullptr, &pipelineCacheHandle), "vkCreatePipelineCache");
    }
    pipelineCacheLoadedBytes = data.size();
    vk::setPipelineCache(pipelineCacheHandle);
}

void VulkanRenderer::Impl::closePipelineCache() {
    if (!pipelineCacheHandle) return;
    VkDevice device = context->device();
    vk::setPipelineCache(VK_NULL_HANDLE);
    size_t size = 0;
    std::vector<char> data;
    if (vkGetPipelineCacheData(device, pipelineCacheHandle, &size, nullptr) == VK_SUCCESS && size) {
        data.resize(size);
        if (vkGetPipelineCacheData(device, pipelineCacheHandle, &size, data.data()) != VK_SUCCESS) data.clear();
        data.resize(std::min(size, data.size()));
    }
    vkDestroyPipelineCache(device, pipelineCacheHandle, nullptr);
    pipelineCacheHandle = VK_NULL_HANDLE;
    // Unchanged since it was loaded: nothing new to keep.
    if (data.empty() || data.size() == pipelineCacheLoadedBytes) return;

    // Written beside it, then renamed over it: another run reading it sees the old file or
    // the new one, never half of one.
    std::error_code ec;
    std::filesystem::create_directories(pipelineCacheFile.parent_path(), ec);
    const VkPhysicalDeviceProperties& props = context->vkbDevice().physical_device.properties;
    CacheHeader header = headerFor(props, shaderHash());
    header.dataSize = data.size();
    std::filesystem::path temporary = pipelineCacheFile;
    // Unique to this run: two games closing at once must never share a temporary file.
    temporary += "." + std::to_string(std::random_device{}()) + std::to_string(uint64_t(SDL_GetTicksNS())) + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(data.data(), std::streamsize(data.size()));
        if (!out) {
            SW_LOG(LogRender, Warning, "couldn't save the pipeline cache to {}", temporary.string());
            std::filesystem::remove(temporary, ec);
            return;
        }
    }
    std::filesystem::rename(temporary, pipelineCacheFile, ec);
    if (ec) {
        SW_LOG(
            LogRender, Warning, "couldn't save the pipeline cache to {}: {}", pipelineCacheFile.string(), ec.message());
        std::filesystem::remove(temporary, ec);
    }
}

}   // namespace shockwave
