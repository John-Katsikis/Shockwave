#include "RendererImpl.h"

namespace shockwave {

// Clustered local lighting (G11, Renderer/Clusters.h, docs/ClusteredLighting.md). After the
// frame's light table is written, a compute pass (Shaders/clusters.comp) lists, for every
// tile-and-distance cluster of the view, the point and spot lights that reach it; the scene's
// fragments (Shaders/mesh.frag) loop over their cluster's list instead of every light. The
// lists hold light indices in table order, so a pixel adds up the same lights in the same
// order either way. Brute force (or a frame clustering doesn't apply to) leaves the header's
// cluster address 0 and every pixel loops over every light, as before G11.

namespace {
// Must match the push constants in shaders/clusters.comp.
struct ClusterPushConstants {
    VkDeviceAddress frame;
    VkDeviceAddress lists;
    VkDeviceAddress counters;
};
// ClusterCounters in shaders/clusters.comp: candidates over all clusters, the most in one,
// clusters that overflowed.
constexpr VkDeviceSize kCounterBytes = 4 * sizeof(uint32_t);
}   // namespace

void VulkanRenderer::Impl::createClusterPipeline() {
    VkDevice device = context->device();
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ClusterPushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    vk::check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &clusterLayout), "vkCreatePipelineLayout");

    VkShaderModule module = vk::loadShaderModule(device, shaderPath("clusters.comp"));
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = clusterLayout;
    const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &clusterPipeline);
    vkDestroyShaderModule(device, module, nullptr);
    vk::check(result, "vkCreateComputePipelines");
}

void VulkanRenderer::Impl::destroyClusters() {
    VkDevice device = context->device();
    VmaAllocator allocator = context->allocator();
    for (Frame& f : frames) {
        if (f.clusterBuffer.buffer) vmaDestroyBuffer(allocator, f.clusterBuffer.buffer, f.clusterBuffer.allocation);
        if (f.clusterCounters.buffer)
            vmaDestroyBuffer(allocator, f.clusterCounters.buffer, f.clusterCounters.allocation);
        f.clusterBuffer = {};
        f.clusterCounters = {};
        f.clusterCapacity = 0;
        f.counters = nullptr;
        f.countersPending = false;
    }
    vkDestroyPipeline(device, clusterPipeline, nullptr);
    vkDestroyPipelineLayout(device, clusterLayout, nullptr);
}

void VulkanRenderer::Impl::prepareClusters(Frame& f, FrameData& header, uint32_t localLights) {
    VmaAllocator allocator = context->allocator();
    // This slot's last frame is finished (beginFrame waited): its counts can be read.
    if (f.countersPending) {
        vk::check(vmaInvalidateAllocation(allocator, f.clusterCounters.allocation, 0, kCounterBytes), "vmaInvalidate");
        const uint32_t clusters = std::max(f.counters[3], 1u);
        lastClusterStats.averageCandidates = float(double(f.counters[0]) / double(clusters));
        lastClusterStats.maxCandidates = f.counters[1];
        lastClusterStats.overflowClusters = f.counters[2];
        f.countersPending = false;
    }

    ClusterStats stats;
    stats.requested = lightCulling.load(std::memory_order_relaxed);
    stats.localLights = localLights;
    stats.directionalLights = header.directionalCount;
    const float nearPlane = projectionNearPlane(projection);
    const float farPlane = lighting.clusterFar;
    if (stats.requested == LightCulling::BruteForce)
        stats.reason = "light_culling brute_force: every pixel tests every light";
    else if (captureRequested)
        stats.reason = "a reflection capture frame tests every light";
    else if (localLights == 0)
        stats.reason = "no point or spot lights";
    else if (stats.requested == LightCulling::Auto && localLights < kClusterAutoLights)
        stats.reason = "auto: fewer than 8 point and spot lights, cheaper to test them all";
    else if (nearPlane <= 0.0f)
        stats.reason = "orthographic view: every pixel tests every light";
    else if (!(farPlane > nearPlane * 1.001f))
        stats.reason = "cluster_far isn't past the camera's near plane";
    else
        stats.clustered = true;

    clustersThisFrame = stats.clustered;
    header.clusters = 0;
    if (!stats.clustered) {
        if (stats.requested == LightCulling::BruteForce && f.clusterBuffer.buffer) {
            // Off frees the lists, a slot at a time as each finishes.
            vmaDestroyBuffer(allocator, f.clusterBuffer.buffer, f.clusterBuffer.allocation);
            vmaDestroyBuffer(allocator, f.clusterCounters.buffer, f.clusterCounters.allocation);
            f.clusterBuffer = {};
            f.clusterCounters = {};
            f.clusterCapacity = 0;
            f.counters = nullptr;
            f.clusterState = {};
        }
        stats.maxCandidates = 0;
        for (const Frame& frame : frames) stats.listBytes += frame.clusterCapacity;
        lastClusterStats = stats;
        return;
    }

    stats.grid = clusterGrid(sceneColor.extent.width, sceneColor.extent.height);
    stats.farPlane = farPlane;
    if (stats.grid.bytes() > f.clusterCapacity) {
        if (f.clusterBuffer.buffer) vmaDestroyBuffer(allocator, f.clusterBuffer.buffer, f.clusterBuffer.allocation);
        f.clusterBuffer = createBuffer(
            stats.grid.bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, 0);
        f.clusterAddress = bufferAddress(f.clusterBuffer.buffer);
        f.clusterCapacity = stats.grid.bytes();
        f.clusterState = {};
    }
    if (!f.clusterCounters.buffer) {
        VmaAllocationInfo info{};
        f.clusterCounters = createBuffer(
            kCounterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, &info);
        f.counters = static_cast<uint32_t*>(info.pMappedData);
        f.countersAddress = bufferAddress(f.clusterCounters.buffer);
    }
    f.counters[0] = f.counters[1] = f.counters[2] = 0;
    f.counters[3] = stats.grid.clusters();
    vk::check(vmaFlushAllocation(allocator, f.clusterCounters.allocation, 0, kCounterBytes), "vmaFlushAllocation");
    f.countersPending = true;

    header.clusters = f.clusterAddress;
    header.clusterTile = stats.grid.tileSize;
    header.clusterTilesX = stats.grid.tilesX;
    header.clusterTilesY = stats.grid.tilesY;
    header.clusterSlices = stats.grid.slices;
    header.clusterNear = nearPlane;
    header.clusterFar = farPlane;
    for (const Frame& frame : frames) stats.listBytes += frame.clusterCapacity;
    // The GPU's counts arrive a frame or two later, from the slot that made them.
    stats.maxCandidates = lastClusterStats.maxCandidates;
    stats.averageCandidates = lastClusterStats.averageCandidates;
    stats.overflowClusters = lastClusterStats.overflowClusters;
    lastClusterStats = stats;
}

void VulkanRenderer::Impl::recordClusters(VkCommandBuffer cmd, Frame& f) {
    const ClusterGrid& grid = lastClusterStats.grid;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, clusterPipeline);
    ClusterPushConstants push{f.frameAddress, f.clusterAddress, f.countersAddress};
    vkCmdPushConstants(cmd, clusterLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (grid.clusters() + 63) / 64, 1, 1);
    // The counts go to the CPU once the frame is done; the render graph orders the lists
    // before the scene's fragment reads.
    VkMemoryBarrier2 toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &toHost;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

}   // namespace shockwave
