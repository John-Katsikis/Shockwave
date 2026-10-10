#include <RHI/RenderGraph.h>

#include <Core/Profile.h>

namespace shockwave::vk {

namespace {

struct UseInfo {
    ImageState state;
    bool writes;
    const char* label;
};

UseInfo useInfo(ImageUse use) {
    switch (use) {
    case ImageUse::ColorAttachment:
        return {
            {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
             VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
            true,
            "color attachment"};
    case ImageUse::DepthAttachment:
        return {
            {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
            true,
            "depth attachment"};
    case ImageUse::Sampled:
        return {
            {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT},
            false,
            "sampled"};
    case ImageUse::TransferSrc:
        return {
            {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT},
            false,
            "transfer source"};
    case ImageUse::TransferDst:
        return {
            {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
             VK_ACCESS_2_TRANSFER_WRITE_BIT},
            true,
            "transfer destination"};
    case ImageUse::Present:
        // Ordered before the submit's signal (ALL_COMMANDS), which presentation waits on.
        return {
            {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE},
            false,
            "present"};
    }
    return {};
}

constexpr VkAccessFlags2 kWriteAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

const char* layoutName(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED: return "undefined";
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "color attachment";
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL: return "depth attachment";
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "sampled";
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "transfer source";
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "transfer destination";
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "present";
    default: return "other";
    }
}

}   // namespace

void RenderGraph::reset() {
    m_images.clear();
    m_passes.clear();
}

RenderGraph::ImageId
RenderGraph::import(std::string name, VkImage image, VkImageAspectFlags aspect, ImageState& state, bool discard) {
    m_images.push_back({std::move(name), image, aspect, &state, discard});
    return static_cast<ImageId>(m_images.size() - 1);
}

void RenderGraph::addPass(std::string name, std::vector<Use> uses, std::function<void(VkCommandBuffer)> record) {
    m_passes.push_back({std::move(name), std::move(uses), std::move(record)});
}

void RenderGraph::execute(VkCommandBuffer cmd) {
    m_description.clear();
    m_timedPasses.clear();
    if (m_queryPool) {
        vkCmdResetQueryPool(cmd, m_queryPool, 0, kMaxTimedPasses + 1);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_queryPool, 0);
    }
    int number = 1;
    std::vector<VkImageMemoryBarrier2> barriers;
    for (Pass& pass : m_passes) {
        barriers.clear();
        std::string uses, synced;
        for (const Use& use : pass.uses) {
            Image& image = m_images[use.image];
            ImageState& from = *image.state;
            const UseInfo to = useInfo(use.use);
            const bool firstUse = !image.touched;
            const VkImageLayout oldLayout = firstUse && image.discard ? VK_IMAGE_LAYOUT_UNDEFINED : from.layout;
            image.touched = true;

            uses += (uses.empty() ? "" : ", ") + image.name + " (" + to.label + ")";
            // Reads after reads in the same layout need nothing; anything else does.
            const bool hazard = (from.access & kWriteAccess) || to.writes;
            if (oldLayout == to.state.layout && !hazard) {
                from.stage |= to.state.stage;
                from.access |= to.state.access;
                continue;
            }
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = from.stage ? from.stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            barrier.srcAccessMask = from.access & kWriteAccess;   // only writes need making available
            barrier.dstStageMask = to.state.stage;
            barrier.dstAccessMask = to.state.access;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = to.state.layout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.image;
            barrier.subresourceRange = {image.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            barriers.push_back(barrier);
            synced += std::string("\n     barrier: ") + image.name + " " + layoutName(oldLayout) + " -> " +
                      layoutName(to.state.layout);
            from = to.state;
        }
        if (!barriers.empty()) {
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
            dep.pImageMemoryBarriers = barriers.data();
            vkCmdPipelineBarrier2(cmd, &dep);
        }
        if (pass.record) {
            SW_PROFILE_SCOPE_DYNAMIC(pass.name.c_str(), pass.name.size());
            pass.record(cmd);
            if (m_queryPool && m_timedPasses.size() < kMaxTimedPasses) {
                m_timedPasses.push_back(pass.name);
                vkCmdWriteTimestamp2(
                    cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_queryPool,
                    static_cast<uint32_t>(m_timedPasses.size()));
            }
        }
        m_description += std::to_string(number++) + ". " + pass.name + ": " + uses + synced + "\n";
    }
}

}   // namespace shockwave::vk
