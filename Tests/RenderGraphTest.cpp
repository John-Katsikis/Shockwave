// The render graph's barrier planning (RHI/RenderGraph.h), without a GPU: plan() works out
// every barrier from the declared uses, so the synchronization each future effect relies on
// is checked here before any pass is written.

#include <RHI/RenderGraph.h>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace shockwave::vk;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (ok) return;
    std::fprintf(stderr, "FAILED: %s\n", what.c_str());
    ++failures;
}

VkImage fakeImage(uintptr_t n) { return reinterpret_cast<VkImage>(n * 0x100); }
VkBuffer fakeBuffer(uintptr_t n) { return reinterpret_cast<VkBuffer>(n * 0x100 + 0x10000); }

const RenderGraph::PassBarriers& pass(const RenderGraph& graph, size_t index) { return graph.barriers().at(index); }

// The barrier for one image whose range starts at a mip, or null.
const VkImageMemoryBarrier2* findImage(const RenderGraph::PassBarriers& b, VkImage image, uint32_t mip = 0) {
    for (const auto& barrier : b.images)
        if (barrier.image == image && barrier.subresourceRange.baseMipLevel == mip) return &barrier;
    return nullptr;
}

bool throws(RenderGraph& graph) {
    try {
        graph.plan();
    } catch (const std::logic_error&) {
        return true;
    }
    return false;
}

void mipChain() {
    // Bloom-style downsampling: each pass samples the level above and writes the next one.
    RenderGraph graph;
    ImageState state;
    const VkImage chain = fakeImage(1);
    const auto id = graph.import("chain", chain, VK_IMAGE_ASPECT_COLOR_BIT, state, true, 4);
    graph.addPass("fill", {{id, ImageUse::ColorAttachment, {0, 1}}}, nullptr);
    for (uint32_t mip = 1; mip < 4; ++mip)
        graph.addPass(
            "down " + std::to_string(mip),
            {{id, ImageUse::ComputeSampled, {mip - 1, 1}}, {id, ImageUse::StorageWrite, {mip, 1}}}, nullptr);
    graph.plan();

    const auto* fill = findImage(pass(graph, 0), chain, 0);
    check(fill && fill->subresourceRange.levelCount == 1, "mip chain: the fill touches only mip 0");
    check(pass(graph, 0).images.size() == 1, "mip chain: no barrier on mips the fill doesn't use");
    for (uint32_t mip = 1; mip < 4; ++mip) {
        const auto& b = pass(graph, mip);
        const auto* read = findImage(b, chain, mip - 1);
        const auto* write = findImage(b, chain, mip);
        check(
            read && write && b.images.size() == 2,
            "mip chain: one barrier per touched mip in pass " + std::to_string(mip));
        if (!read || !write) continue;
        check(read->newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "mip chain: the source mip is sampled");
        check(read->dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, "mip chain: sampled in compute");
        const VkAccessFlags2 wrote =
            mip == 1 ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        check(read->srcAccessMask == wrote, "mip chain: the source mip's last write is made available");
        check(write->newLayout == VK_IMAGE_LAYOUT_GENERAL, "mip chain: the target mip becomes a storage image");
        check(write->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED, "mip chain: a discarded mip starts undefined");
    }
    // Mip 3 ends in GENERAL, mips 0-2 sampled: the tail returns them to one layout.
    check(graph.barriers().size() == 5, "mip chain: mixed layouts get a closing barrier");
    check(
        state.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
            state.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        "mip chain: the owner's state is one layout");
    if (graph.barriers().size() == 5) {
        const auto& tail = graph.barriers().back();
        check(
            tail.images.size() == 1 && tail.images[0].subresourceRange.baseMipLevel == 3 &&
                tail.images[0].oldLayout == VK_IMAGE_LAYOUT_GENERAL &&
                tail.images[0].srcAccessMask == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            "mip chain: only the last mip moves, after its write");
    }
}

void computeToFragment() {
    RenderGraph graph;
    ImageState state;
    const VkImage ao = fakeImage(2);
    const auto id = graph.import("ao", ao, VK_IMAGE_ASPECT_COLOR_BIT, state, true);
    graph.addPass("ao", {{id, ImageUse::StorageWrite}}, nullptr);
    graph.addPass("scene", {{id, ImageUse::Sampled}}, nullptr);
    graph.plan();
    const auto* b = findImage(pass(graph, 1), ao);
    check(b != nullptr, "compute to fragment: a barrier before the read");
    if (!b) return;
    check(
        b->srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
            b->srcAccessMask == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        "compute to fragment: waits for the compute write");
    check(
        b->dstStageMask == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT &&
            b->dstAccessMask == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        "compute to fragment: before fragment sampling");
    check(
        b->oldLayout == VK_IMAGE_LAYOUT_GENERAL && b->newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        "compute to fragment: general to sampled");
}

void readsInSeveralStages() {
    // A write sampled by the fragment shader and then by compute: the second read needs its
    // own dependency even though the layout doesn't change.
    RenderGraph graph;
    ImageState state;
    const VkImage color = fakeImage(3);
    const auto id = graph.import("color", color, VK_IMAGE_ASPECT_COLOR_BIT, state, true);
    graph.addPass("scene", {{id, ImageUse::ColorAttachment}}, nullptr);
    graph.addPass("tonemap", {{id, ImageUse::Sampled}}, nullptr);
    graph.addPass("histogram", {{id, ImageUse::ComputeSampled}}, nullptr);
    graph.addPass("tonemap again", {{id, ImageUse::Sampled}}, nullptr);
    graph.addPass("scene again", {{id, ImageUse::ColorAttachment}}, nullptr);
    graph.plan();
    const auto* second = findImage(pass(graph, 2), color);
    check(second && second->oldLayout == second->newLayout, "second reader: no layout change");
    check(
        second && (second->srcStageMask & VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) &&
            second->dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        "second reader: chained from the color write to compute");
    check(pass(graph, 3).images.empty(), "a read already made visible needs nothing");
    const auto* again = findImage(pass(graph, 4), color);
    check(again != nullptr, "write after reads gets a barrier");
    check(
        again && (again->srcStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) &&
            (again->srcStageMask & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) && again->srcAccessMask == 0,
        "write after read waits for every reader without making reads available");
}

void skinningToVertex() {
    BufferState state;
    const VkBuffer skinned = fakeBuffer(1);
    for (int frame = 0; frame < 2; ++frame) {
        RenderGraph graph;
        const auto id = graph.importBuffer("skinned vertices", skinned, state);
        graph.addPass("skinning", {}, nullptr, {{id, BufferUse::ComputeWrite}});
        graph.addPass("shadows", {}, nullptr, {{id, BufferUse::VertexShaderRead}});
        graph.addPass("scene", {}, nullptr, {{id, BufferUse::VertexShaderRead}});
        graph.plan();
        const auto& write = pass(graph, 0).buffers;
        if (frame == 1) {
            check(
                write.size() == 1 && (write[0].srcStageMask & VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT) &&
                    write[0].srcAccessMask == 0,
                "skinning: next frame's write waits for last frame's vertex reads");
        }
        const auto& read = pass(graph, 1).buffers;
        check(read.size() == 1, "skinning: one barrier before the first vertex read");
        if (read.size() == 1) {
            check(
                read[0].srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
                    read[0].srcAccessMask == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                "skinning: after the compute write");
            check(
                read[0].dstStageMask == VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT &&
                    read[0].dstAccessMask == VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                "skinning: before vertex shader storage reads, not just vertex input");
        }
        check(pass(graph, 2).buffers.empty(), "skinning: the second vertex read needs nothing");
    }
}

void computeToIndirect() {
    RenderGraph graph;
    BufferState state;
    const VkBuffer args = fakeBuffer(2);
    const auto id = graph.importBuffer("draw arguments", args, state);
    graph.addPass("cull", {}, nullptr, {{id, BufferUse::ComputeWrite}});
    graph.addPass("draw", {}, nullptr, {{id, BufferUse::IndirectRead}, {id, BufferUse::VertexShaderRead}});
    graph.plan();
    const auto& b = pass(graph, 1).buffers;
    check(b.size() == 1, "indirect: repeated declarations merge into one barrier");
    if (b.size() == 1) {
        check(
            b[0].dstStageMask == (VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT),
            "indirect: both the argument fetch and the shader read wait");
        check(b[0].dstAccessMask & VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, "indirect: indirect command read access");
    }
}

void depthReadAndSample() {
    RenderGraph graph;
    ImageState state;
    const VkImage depth = fakeImage(4);
    const auto id = graph.import("depth", depth, VK_IMAGE_ASPECT_DEPTH_BIT, state, true);
    graph.addPass("prepass", {{id, ImageUse::DepthAttachment}}, nullptr);
    graph.addPass("decals", {{id, ImageUse::DepthReadOnly}, {id, ImageUse::DepthSampled}}, nullptr);
    graph.plan();
    const auto& b = pass(graph, 1).images;
    check(b.size() == 1, "depth read + sample: one combined barrier");
    if (b.size() != 1) return;
    check(b[0].newLayout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, "depth read + sample: read-only depth layout");
    check(
        (b[0].dstStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) &&
            (b[0].dstStageMask & VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT),
        "depth read + sample: depth tests and fragment sampling both wait");
    check(
        b[0].srcAccessMask == VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        "depth read + sample: after depth writes");
}

void contradictions() {
    {
        RenderGraph graph;
        ImageState state;
        const auto id = graph.import("depth", fakeImage(5), VK_IMAGE_ASPECT_DEPTH_BIT, state, true);
        graph.addPass("bad", {{id, ImageUse::DepthAttachment}, {id, ImageUse::DepthSampled}}, nullptr);
        check(throws(graph), "a written depth attachment can't be sampled in the same pass");
    }
    {
        RenderGraph graph;
        ImageState state;
        const auto id = graph.import("chain", fakeImage(6), VK_IMAGE_ASPECT_COLOR_BIT, state, true, 4);
        graph.addPass("bad", {{id, ImageUse::ComputeSampled, {0, 2}}, {id, ImageUse::StorageWrite, {1, 1}}}, nullptr);
        check(throws(graph), "sampling and writing one mip at once is rejected");
    }
    {
        RenderGraph graph;
        ImageState state;
        const auto id = graph.import("chain", fakeImage(7), VK_IMAGE_ASPECT_COLOR_BIT, state, true, 2);
        graph.addPass("bad", {{id, ImageUse::Sampled, {1, 4}}}, nullptr);
        check(throws(graph), "a range past the image's mips is rejected");
    }
}

void historyReuse() {
    // Two history images swap roles each frame. They're never discarded: last frame's
    // contents are this frame's input.
    ImageState a, b;
    const VkImage imageA = fakeImage(8), imageB = fakeImage(9);
    for (int frame = 0; frame < 3; ++frame) {
        RenderGraph graph;
        const bool even = frame % 2 == 0;
        const auto current =
            graph.import("history current", even ? imageA : imageB, VK_IMAGE_ASPECT_COLOR_BIT, even ? a : b, false);
        const auto previous =
            graph.import("history previous", even ? imageB : imageA, VK_IMAGE_ASPECT_COLOR_BIT, even ? b : a, false);
        graph.addPass("resolve", {{previous, ImageUse::Sampled}, {current, ImageUse::ColorAttachment}}, nullptr);
        graph.addPass("post", {{current, ImageUse::Sampled}}, nullptr);
        graph.plan();
        if (frame == 0) continue;
        const auto* read = findImage(pass(graph, 0), even ? imageB : imageA);
        // Last frame's post pass left it sampled and visible to fragment shaders: reading it
        // again needs no barrier, and certainly no transition from UNDEFINED.
        check(!read, "history: last frame's layout carries over (contents kept)");
        const auto* write = findImage(pass(graph, 0), even ? imageA : imageB);
        check(write && write->oldLayout != VK_IMAGE_LAYOUT_UNDEFINED, "history: never discarded");
        check(
            write && (write->srcStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT),
            "history: overwriting waits for last frame's reads");
    }
}

}   // namespace

int main() {
    mipChain();
    computeToFragment();
    readsInSeveralStages();
    skinningToVertex();
    computeToIndirect();
    depthReadAndSample();
    contradictions();
    historyReuse();
    if (failures) {
        std::fprintf(stderr, "%d render graph check(s) failed\n", failures);
        return 1;
    }
    std::puts("render graph: all checks passed");
    return 0;
}
