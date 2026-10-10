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
    Sampled,   // read in a fragment shader
    TransferSrc,   // copied or blitted from
    TransferDst,   // copied or blitted to
    Present,   // handed to the presentation engine
};

// Where an image was last left. The owner keeps one per image so the state carries
// over from one frame to the next.
struct ImageState {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
};

// A small per-frame render graph. Passes declare the images they use; execute() records
// them in order with the synchronization2 barriers they need between them (layout
// changes, read-after-write, write-after-read, write-after-write), batched one barrier
// call per pass. Nothing is synchronized by hand in pass code, so passes can be added,
// removed or reordered without touching their neighbours.
//
// Built and executed on the render thread each frame; cheap enough to rebuild every time.
class RenderGraph {
public:
    using ImageId = uint32_t;
    struct Use {
        ImageId image;
        ImageUse use;
    };

    void reset();
    // discard: the previous contents aren't needed this frame, so the first barrier can
    // start from an undefined layout (still ordered after the previous frame's use).
    ImageId import(std::string name, VkImage image, VkImageAspectFlags aspect, ImageState& state, bool discard);
    void addPass(std::string name, std::vector<Use> uses, std::function<void(VkCommandBuffer)> record);
    void execute(VkCommandBuffer cmd);

    // The last execute(), as text: passes in order with their uses and inserted barriers.
    const std::string& description() const { return m_description; }

private:
    struct Image {
        std::string name;
        VkImage image;
        VkImageAspectFlags aspect;
        ImageState* state;
        bool discard;
        bool touched = false;
    };
    struct Pass {
        std::string name;
        std::vector<Use> uses;
        std::function<void(VkCommandBuffer)> record;
    };
    std::vector<Image> m_images;
    std::vector<Pass> m_passes;
    std::string m_description;
};

}   // namespace shockwave::vk
