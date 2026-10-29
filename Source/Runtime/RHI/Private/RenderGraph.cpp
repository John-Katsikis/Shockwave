#include <RHI/RenderGraph.h>

#include <Core/Profile.h>

#include <stdexcept>

namespace shockwave::vk {

namespace {

struct UseInfo {
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    bool writes;
    std::string label;
};

UseInfo useInfo(ImageUse use) {
    switch (use) {
    case ImageUse::ColorAttachment:
        return {
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true, "color attachment"};
    case ImageUse::DepthAttachment:
        return {
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, true,
            "depth attachment"};
    case ImageUse::DepthReadOnly:
        return {
            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, false, "depth read-only"};
    case ImageUse::DepthSampled:
        return {
            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false, "depth sampled"};
    case ImageUse::Sampled:
        return {
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false, "sampled"};
    case ImageUse::ComputeSampled:
        return {
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false, "sampled (compute)"};
    case ImageUse::StorageRead:
        return {
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, false,
            "storage read"};
    case ImageUse::StorageWrite:
        return {
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true,
            "storage write"};
    case ImageUse::StorageReadWrite:
        return {
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true, "storage read/write"};
    case ImageUse::TransferSrc:
        return {
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            false, "transfer source"};
    case ImageUse::TransferDst:
        return {
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            true, "transfer destination"};
    case ImageUse::Present:
        // Ordered before the submit's signal (ALL_COMMANDS), which presentation waits on.
        return {
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, false, "present"};
    }
    return {};
}

UseInfo useInfo(BufferUse use) {
    constexpr VkImageLayout none = VK_IMAGE_LAYOUT_UNDEFINED;
    switch (use) {
    case BufferUse::TransferSrc:
        return {none, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false, "transfer source"};
    case BufferUse::TransferDst:
        return {
            none, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true, "transfer destination"};
    case BufferUse::ComputeRead:
        return {
            none, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, false, "compute read"};
    case BufferUse::ComputeWrite:
        return {
            none, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true, "compute write"};
    case BufferUse::ComputeReadWrite:
        return {
            none, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true, "compute read/write"};
    case BufferUse::VertexShaderRead:
        return {
            none, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, false,
            "vertex shader read"};
    case BufferUse::FragmentShaderRead:
        return {
            none, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, false,
            "fragment shader read"};
    case BufferUse::IndexRead:
        return {none, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT, false, "index read"};
    case BufferUse::IndirectRead:
        return {
            none, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, false, "indirect read"};
    }
    return {};
}

constexpr VkAccessFlags2 kWriteAccess =
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
    VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;

const char* layoutName(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED: return "undefined";
    case VK_IMAGE_LAYOUT_GENERAL: return "general";
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "color attachment";
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL: return "depth attachment";
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL: return "depth read-only";
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "sampled";
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "transfer source";
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "transfer destination";
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "present";
    default: return "other";
    }
}

// The dependency one use needs on one tracked subresource or buffer.
struct Dependency {
    bool needed = false;
    VkPipelineStageFlags2 srcStage = 0;
    VkAccessFlags2 srcAccess = 0;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

template <typename Sub>
Dependency resolve(Sub& s, const UseInfo& to, bool image, bool discard) {
    Dependency d;
    d.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : s.layout;
    const bool layoutChange = image && d.oldLayout != to.layout;
    if (layoutChange || to.writes) {
        // A transition or a write waits for every earlier use: the last write and every
        // read since (write-after-read needs an execution dependency, nothing more).
        d.needed = true;
        d.srcStage = s.writeStages | s.readStages;
        d.srcAccess = s.pendingWrites;
        s.layout = to.layout;
        s.pendingWrites = to.writes ? to.access & kWriteAccess : 0;
        // A layout transition is itself a write, ordered before the stages it was made
        // for: later dependencies chain from those as well as from the original writer.
        s.writeStages = to.writes ? to.stage : s.writeStages | to.stage;
        s.readStages = to.writes ? 0 : to.stage;
        s.visibleStages = to.writes ? 0 : to.stage;
        s.visibleAccess = to.writes ? 0 : to.access;
        return d;
    }
    // A read in the same layout: free when an earlier barrier already made the last write
    // visible to this stage and access, otherwise it chains from the writer's stages.
    const bool covered = !s.pendingWrites && !(to.stage & ~s.visibleStages) && !(to.access & ~s.visibleAccess);
    if (!covered) {
        d.needed = true;
        d.srcStage = s.writeStages;
        d.srcAccess = s.pendingWrites;
        s.pendingWrites = 0;
        s.visibleStages |= to.stage;
        s.visibleAccess |= to.access;
    }
    s.readStages |= to.stage;
    return d;
}

template <typename Sub, typename State>
void importState(Sub& s, const State& state) {
    s.writeStages = state.stage;
    s.pendingWrites = state.access & kWriteAccess;
    s.readStages = state.stage;
    s.visibleStages = s.pendingWrites ? 0 : state.stage;
    s.visibleAccess = s.pendingWrites ? 0 : state.access;
}

template <typename Sub>
void exportState(const Sub& s, VkPipelineStageFlags2& stage, VkAccessFlags2& access) {
    stage |= s.writeStages | s.readStages;
    access |= s.pendingWrites ? s.pendingWrites : s.visibleAccess;
}

bool overlaps(const Subresource& a, const Subresource& b) {
    return a.baseMip < b.baseMip + b.mipCount && b.baseMip < a.baseMip + a.mipCount &&
           a.baseLayer < b.baseLayer + b.layerCount && b.baseLayer < a.baseLayer + a.layerCount;
}

bool sameRange(const Subresource& a, const Subresource& b) {
    return a.baseMip == b.baseMip && a.mipCount == b.mipCount && a.baseLayer == b.baseLayer &&
           a.layerCount == b.layerCount;
}

std::string rangeText(const Subresource& r, uint32_t mips, uint32_t layers) {
    std::string text;
    if (r.baseMip != 0 || r.mipCount != mips)
        text += r.mipCount == 1
                    ? ", mip " + std::to_string(r.baseMip)
                    : ", mips " + std::to_string(r.baseMip) + "-" + std::to_string(r.baseMip + r.mipCount - 1);
    if (r.baseLayer != 0 || r.layerCount != layers)
        text += r.layerCount == 1
                    ? ", layer " + std::to_string(r.baseLayer)
                    : ", layers " + std::to_string(r.baseLayer) + "-" + std::to_string(r.baseLayer + r.layerCount - 1);
    return text;
}

VkImageMemoryBarrier2 imageBarrier(
    VkImage image, VkImageAspectFlags aspect, const Dependency& d, const UseInfo& to, uint32_t mip, uint32_t layer) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = d.srcStage ? d.srcStage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    barrier.srcAccessMask = d.srcAccess;   // only writes need making available
    barrier.dstStageMask = to.stage;
    barrier.dstAccessMask = to.access;
    barrier.oldLayout = d.oldLayout;
    barrier.newLayout = to.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, mip, 1, layer, 1};
    return barrier;
}

bool sameExceptRange(const VkImageMemoryBarrier2& a, const VkImageMemoryBarrier2& b) {
    return a.image == b.image && a.srcStageMask == b.srcStageMask && a.srcAccessMask == b.srcAccessMask &&
           a.dstStageMask == b.dstStageMask && a.dstAccessMask == b.dstAccessMask && a.oldLayout == b.oldLayout &&
           a.newLayout == b.newLayout;
}

// Per-subresource barriers, merged into as few ranges as possible: consecutive mips of a
// layer first, then consecutive layers with the same mip range.
void appendMerged(std::vector<VkImageMemoryBarrier2>& out, std::vector<VkImageMemoryBarrier2>& subs) {
    std::vector<VkImageMemoryBarrier2> mips;
    for (const auto& b : subs) {
        if (!mips.empty()) {
            auto& last = mips.back();
            const auto& lr = last.subresourceRange;
            if (sameExceptRange(last, b) && lr.baseArrayLayer == b.subresourceRange.baseArrayLayer &&
                lr.baseMipLevel + lr.levelCount == b.subresourceRange.baseMipLevel) {
                ++last.subresourceRange.levelCount;
                continue;
            }
        }
        mips.push_back(b);
    }
    const size_t first = out.size();
    for (const auto& b : mips) {
        if (out.size() > first) {
            auto& last = out.back();
            const auto& lr = last.subresourceRange;
            const auto& br = b.subresourceRange;
            if (sameExceptRange(last, b) && lr.baseMipLevel == br.baseMipLevel && lr.levelCount == br.levelCount &&
                lr.baseArrayLayer + lr.layerCount == br.baseArrayLayer) {
                ++last.subresourceRange.layerCount;
                continue;
            }
        }
        out.push_back(b);
    }
    subs.clear();
}

}   // namespace

void RenderGraph::reset() {
    m_images.clear();
    m_buffers.clear();
    m_passes.clear();
    m_barriers.clear();
}

RenderGraph::ImageId RenderGraph::import(
    std::string name, VkImage image, VkImageAspectFlags aspect, ImageState& state, bool discard, uint32_t mips,
    uint32_t layers) {
    Image entry{std::move(name), image, aspect, &state, discard, std::max(mips, 1u), std::max(layers, 1u), {}};
    m_images.push_back(std::move(entry));
    return static_cast<ImageId>(m_images.size() - 1);
}

RenderGraph::BufferId RenderGraph::importBuffer(std::string name, VkBuffer buffer, BufferState& state) {
    m_buffers.push_back({std::move(name), buffer, &state, {}});
    return static_cast<BufferId>(m_buffers.size() - 1);
}

void RenderGraph::addPass(
    std::string name, std::vector<Use> uses, std::function<void(VkCommandBuffer)> record,
    std::vector<BufferAccess> buffers) {
    m_passes.push_back({std::move(name), std::move(uses), std::move(buffers), std::move(record)});
}

void RenderGraph::plan() {
    m_description.clear();
    m_barriers.assign(m_passes.size(), {});
    for (Image& image : m_images) {
        image.subs.assign(size_t(image.mips) * image.layers, {});
        for (SubState& s : image.subs) {
            s.layout = image.state->layout;
            importState(s, *image.state);
        }
        image.touched = false;
    }
    for (Buffer& buffer : m_buffers) importState(buffer.sync, *buffer.state);

    struct Resolved {
        ImageId image;
        Subresource range;
        UseInfo info;
    };
    std::vector<Resolved> resolved;
    std::vector<VkImageMemoryBarrier2> subBarriers;
    for (size_t p = 0; p < m_passes.size(); ++p) {
        const Pass& pass = m_passes[p];
        PassBarriers& out = m_barriers[p];
        auto fail = [&](const std::string& why) {
            throw std::logic_error("render graph: pass \"" + pass.name + "\" " + why);
        };

        // Combine this pass's declarations: the same subresource twice in one layout is one
        // combined use (a read-only depth attachment that is also sampled, say); two
        // layouts at once, or ranges that only partly overlap, are mistakes.
        resolved.clear();
        for (const Use& use : pass.uses) {
            if (use.image >= m_images.size()) fail("uses an image that was never imported");
            const Image& image = m_images[use.image];
            Subresource range = use.range;
            if (range.mipCount == VK_REMAINING_MIP_LEVELS && range.baseMip < image.mips)
                range.mipCount = image.mips - range.baseMip;
            if (range.layerCount == VK_REMAINING_ARRAY_LAYERS && range.baseLayer < image.layers)
                range.layerCount = image.layers - range.baseLayer;
            if (range.mipCount == 0 || range.layerCount == 0 || range.baseMip + range.mipCount > image.mips ||
                range.baseLayer + range.layerCount > image.layers)
                fail("uses mips or layers " + image.name + " doesn't have");
            UseInfo info = useInfo(use.use);
            bool merged = false;
            for (Resolved& other : resolved) {
                if (other.image != use.image || !overlaps(other.range, range)) continue;
                if (other.info.layout != info.layout)
                    fail(
                        "uses " + image.name + " as " + other.info.label + " and " + info.label +
                        ", which need different layouts");
                if (!sameRange(other.range, range))
                    fail("uses partly overlapping parts of " + image.name + "; declare identical ranges");
                other.info.stage |= info.stage;
                other.info.access |= info.access;
                other.info.writes = other.info.writes || info.writes;
                other.info.label += " + " + info.label;
                merged = true;
                break;
            }
            if (!merged) resolved.push_back({use.image, range, std::move(info)});
        }

        std::string uses, synced;
        for (const Resolved& r : resolved) {
            Image& image = m_images[r.image];
            const bool discard = !image.touched && image.discard;
            image.touched = true;
            uses += (uses.empty() ? "" : ", ") + image.name + " (" + r.info.label +
                    rangeText(r.range, image.mips, image.layers) + ")";
            for (uint32_t layer = r.range.baseLayer; layer < r.range.baseLayer + r.range.layerCount; ++layer) {
                for (uint32_t mip = r.range.baseMip; mip < r.range.baseMip + r.range.mipCount; ++mip) {
                    SubState& s = image.subs[size_t(layer) * image.mips + mip];
                    const Dependency d = resolve(s, r.info, true, discard);
                    if (d.needed) subBarriers.push_back(imageBarrier(image.image, image.aspect, d, r.info, mip, layer));
                }
            }
            const size_t before = out.images.size();
            appendMerged(out.images, subBarriers);
            for (size_t i = before; i < out.images.size(); ++i) {
                const auto& b = out.images[i];
                const Subresource range{
                    b.subresourceRange.baseMipLevel, b.subresourceRange.levelCount, b.subresourceRange.baseArrayLayer,
                    b.subresourceRange.layerCount};
                synced += "\n     barrier: " + image.name + rangeText(range, image.mips, image.layers) + " " +
                          layoutName(b.oldLayout) + " -> " + layoutName(b.newLayout);
            }
        }

        // Buffers: the same rules without layouts. Repeated declarations merge.
        std::vector<std::pair<BufferId, UseInfo>> bufferUses;
        for (const BufferAccess& access : pass.buffers) {
            if (access.buffer >= m_buffers.size()) fail("uses a buffer that was never imported");
            UseInfo info = useInfo(access.use);
            bool merged = false;
            for (auto& [id, other] : bufferUses) {
                if (id != access.buffer) continue;
                other.stage |= info.stage;
                other.access |= info.access;
                other.writes = other.writes || info.writes;
                other.label += " + " + info.label;
                merged = true;
            }
            if (!merged) bufferUses.emplace_back(access.buffer, std::move(info));
        }
        for (const auto& [id, info] : bufferUses) {
            Buffer& buffer = m_buffers[id];
            uses += (uses.empty() ? "" : ", ") + buffer.name + " (" + info.label + ")";
            const Dependency d = resolve(buffer.sync, info, false, false);
            if (!d.needed) continue;
            VkBufferMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = d.srcStage ? d.srcStage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            barrier.srcAccessMask = d.srcAccess;
            barrier.dstStageMask = info.stage;
            barrier.dstAccessMask = info.access;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = buffer.buffer;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            out.buffers.push_back(barrier);
            synced += "\n     barrier: " + buffer.name + " (" + info.label + ")";
        }
        m_description += std::to_string(p + 1) + ". " + pass.name + ": " + uses + synced + "\n";
    }

    // Hand the states back for next frame. An image left in mixed layouts (one mip
    // written as storage, another sampled) goes back to its first subresource's layout,
    // so the owner's single state stays true.
    PassBarriers tail;
    for (Image& image : m_images) {
        if (!image.touched) continue;
        const UseInfo whole{
            image.subs[0].layout, image.subs[0].readStages | image.subs[0].writeStages, image.subs[0].visibleAccess,
            false, ""};
        bool mixed = false;
        for (SubState& s : image.subs) mixed = mixed || s.layout != whole.layout;
        if (mixed) {
            for (uint32_t layer = 0; layer < image.layers; ++layer) {
                for (uint32_t mip = 0; mip < image.mips; ++mip) {
                    SubState& s = image.subs[size_t(layer) * image.mips + mip];
                    if (s.layout == whole.layout) continue;
                    UseInfo to = whole;
                    to.stage = to.stage ? to.stage : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    const Dependency d = resolve(s, to, true, false);
                    subBarriers.push_back(imageBarrier(image.image, image.aspect, d, to, mip, layer));
                }
            }
            appendMerged(tail.images, subBarriers);
            m_description += "   end: " + image.name + " back to " + layoutName(whole.layout) + "\n";
        }
        ImageState state{image.subs[0].layout, 0, 0};
        for (const SubState& s : image.subs) exportState(s, state.stage, state.access);
        *image.state = state;
    }
    for (Buffer& buffer : m_buffers) {
        BufferState state;
        exportState(buffer.sync, state.stage, state.access);
        *buffer.state = state;
    }
    if (!tail.images.empty()) m_barriers.push_back(std::move(tail));
}

void RenderGraph::execute(VkCommandBuffer cmd) {
    plan();
    m_timedPasses.clear();
    m_untimedPasses = 0;
    if (m_queryPool) {
        vkCmdResetQueryPool(cmd, m_queryPool, 0, kMaxTimedPasses + 1);
        // After everything submitted before has finished: the previous frame may still be
        // drawing, and with TOP_OF_PIPE its tail would be counted against the first pass.
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_queryPool, 0);
    }
    auto record = [cmd](const PassBarriers& barriers) {
        if (barriers.images.empty() && barriers.buffers.empty()) return;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.images.size());
        dep.pImageMemoryBarriers = barriers.images.data();
        dep.bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.buffers.size());
        dep.pBufferMemoryBarriers = barriers.buffers.data();
        vkCmdPipelineBarrier2(cmd, &dep);
    };
    for (size_t p = 0; p < m_passes.size(); ++p) {
        Pass& pass = m_passes[p];
        record(m_barriers[p]);
        if (!pass.record) continue;
        SW_PROFILE_SCOPE_DYNAMIC(pass.name.c_str(), pass.name.size());
        pass.record(cmd);
        if (!m_queryPool) continue;
        if (m_timedPasses.size() < kMaxTimedPasses) {
            m_timedPasses.push_back(pass.name);
            vkCmdWriteTimestamp2(
                cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_queryPool, static_cast<uint32_t>(m_timedPasses.size()));
        } else {
            ++m_untimedPasses;
        }
    }
    if (m_barriers.size() > m_passes.size()) record(m_barriers.back());
}

}   // namespace shockwave::vk
