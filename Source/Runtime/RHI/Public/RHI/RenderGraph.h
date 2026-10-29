#pragma once

#include <RHI/Common.h>

#include <functional>
#include <string>
#include <vector>

namespace shockwave::vk {

// How a pass uses an image. Each use implies a layout, the pipeline stages that touch
// the image and whether it is written, which is everything needed to place barriers.
enum class ImageUse {
    ColorAttachment,   // rendered into (load or clear, then store)
    DepthAttachment,   // depth tested and written
    DepthReadOnly,   // depth tested without writes (read-only depth layout)
    DepthSampled,   // read in a fragment shader from the read-only depth layout; may share
    // a pass with DepthReadOnly on the same image
    Sampled,   // read in a fragment shader
    ComputeSampled,   // read in a compute shader
    StorageRead,   // storage image read in a compute shader (general layout)
    StorageWrite,   // storage image written in a compute shader
    StorageReadWrite,   // both, in the same dispatch
    TransferSrc,   // copied or blitted from
    TransferDst,   // copied or blitted to
    Present,   // handed to the presentation engine
};

// How a pass uses a buffer. The whole buffer is tracked as one range for now.
enum class BufferUse {
    TransferSrc,
    TransferDst,
    ComputeRead,   // storage reads in a compute shader
    ComputeWrite,
    ComputeReadWrite,
    VertexShaderRead,   // vertex pulling: storage or device-address reads in a vertex shader
    FragmentShaderRead,
    IndexRead,   // bound as an index buffer
    IndirectRead,   // indirect draw or dispatch arguments
};

// Where an image or buffer was last left. The owner keeps one per resource so the state
// carries over from one frame to the next.
struct ImageState {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
};
struct BufferState {
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
};

// Part of an image: a range of mip levels and array layers. The default is all of it.
struct Subresource {
    uint32_t baseMip = 0;
    uint32_t mipCount = VK_REMAINING_MIP_LEVELS;
    uint32_t baseLayer = 0;
    uint32_t layerCount = VK_REMAINING_ARRAY_LAYERS;
};

// A small per-frame render graph. Passes declare the images and buffers they use;
// execute() records them in order with the synchronization2 barriers they need between
// them (layout changes, read-after-write, write-after-read, write-after-write), batched
// one barrier call per pass. Nothing is synchronized by hand in pass code, so passes can
// be added, removed or reordered without touching their neighbours.
//
// Each mip level and array layer of an image is tracked separately, so a pass can write
// one mip of a chain while the next samples the one above it. Reads since the last write
// accumulate: a later read in a stage the last barrier didn't cover gets its own barrier,
// and the next write waits for every one of them.
//
// Built and executed on the render thread each frame; cheap enough to rebuild every time.
class RenderGraph {
public:
    using ImageId = uint32_t;
    using BufferId = uint32_t;
    struct Use {
        ImageId image;
        ImageUse use;
        Subresource range{};
    };
    struct BufferAccess {
        BufferId buffer;
        BufferUse use;
    };

    void reset();
    // discard: the previous contents aren't needed this frame, so the first barrier can
    // start from an undefined layout (still ordered after the previous frame's use). Never
    // discard an image whose earlier contents are read, like a history buffer.
    // mips and layers: the image's own counts, for tracking parts of it separately.
    ImageId import(
        std::string name, VkImage image, VkImageAspectFlags aspect, ImageState& state, bool discard, uint32_t mips = 1,
        uint32_t layers = 1);
    BufferId importBuffer(std::string name, VkBuffer buffer, BufferState& state);
    void addPass(
        std::string name, std::vector<Use> uses, std::function<void(VkCommandBuffer)> record,
        std::vector<BufferAccess> buffers = {});

    // Works out every pass's barriers and updates the imported states, without recording
    // anything. execute() calls it; tests call it on its own (no GPU needed). Throws
    // std::logic_error when a pass declares contradictory uses of the same subresource.
    void plan();
    void execute(VkCommandBuffer cmd);

    // The barriers plan() chose for each pass, in pass order. A trailing entry beyond the
    // last pass, when present, returns images to one layout for the next frame.
    struct PassBarriers {
        std::vector<VkImageMemoryBarrier2> images;
        std::vector<VkBufferMemoryBarrier2> buffers;
    };
    const std::vector<PassBarriers>& barriers() const { return m_barriers; }

    // The last plan, as text: passes in order with their uses and inserted barriers.
    const std::string& description() const { return m_description; }

    // GPU timing. With a query pool set, execute() writes a timestamp before the first pass
    // and after every pass that records commands, so each pass is timed with no code of its
    // own. Query i + 1 minus query i is timedPasses()[i]. The pool needs at least
    // kMaxTimedPasses + 1 queries; passes beyond that go untimed and are counted in
    // untimedPasses(). Null turns timing off.
    static constexpr uint32_t kMaxTimedPasses = 31;
    void setTimestampPool(VkQueryPool pool) { m_queryPool = pool; }
    const std::vector<std::string>& timedPasses() const { return m_timedPasses; }
    uint32_t untimedPasses() const { return m_untimedPasses; }

private:
    // One mip level of one layer.
    struct SubState {
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkPipelineStageFlags2 writeStages = 0;   // the last write, or the transition after it
        VkAccessFlags2 pendingWrites = 0;   // written and not yet made available by a barrier
        VkPipelineStageFlags2 readStages = 0;   // reads since the last write
        VkPipelineStageFlags2 visibleStages = 0;   // stages the last write is visible to
        VkAccessFlags2 visibleAccess = 0;
    };
    struct Image {
        std::string name;
        VkImage image;
        VkImageAspectFlags aspect;
        ImageState* state;
        bool discard;
        uint32_t mips, layers;
        std::vector<SubState> subs;   // layer-major
        bool touched = false;
    };
    struct Buffer {
        std::string name;
        VkBuffer buffer;
        BufferState* state;
        SubState sync;   // layout unused
    };
    struct Pass {
        std::string name;
        std::vector<Use> uses;
        std::vector<BufferAccess> buffers;
        std::function<void(VkCommandBuffer)> record;
    };
    std::vector<Image> m_images;
    std::vector<Buffer> m_buffers;
    std::vector<Pass> m_passes;
    std::vector<PassBarriers> m_barriers;
    std::string m_description;
    VkQueryPool m_queryPool = VK_NULL_HANDLE;
    std::vector<std::string> m_timedPasses;
    uint32_t m_untimedPasses = 0;
};

}   // namespace shockwave::vk
