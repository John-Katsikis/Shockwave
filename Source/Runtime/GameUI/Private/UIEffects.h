#pragma once

// The parts of RmlUi's drawing that need more than textured triangles: gradients
// (linear-, radial- and conic-gradient decorators) and box-shadow.
//
// - Gradients are baked once into a small texture per decorator, and drawn as ordinary UI
//   geometry with texture coordinates, so the Vulkan UI pass needs no extra shader.
// - box-shadow is rendered by RmlUi into an off-screen layer once, blurred, and saved as a
//   texture. Shockwave does that on the CPU: these layers record what's drawn into them
//   and rasterize it when RmlUi saves the layer. Layers composited straight back onto the
//   screen (CSS filter) are drawn without their filter.
//
// Main thread only, like the rest of GameUI; rasterizeLayer can spread its rows over the
// job system's threads and returns once they're done.

#include <Renderer/RenderTypes.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace shockwave::ui {

struct Gradient {
    enum class Kind : uint8_t { Linear, Radial, Conic };
    Kind kind = Kind::Linear;
    bool repeating = false;
    glm::vec2 p{0.0f};   // linear: start; radial and conic: center
    glm::vec2 v{0.0f};   // linear: start to end; radial: 1 / radius; conic: angle as a unit vector
    std::vector<float> positions;   // stops, 0 at the start and 1 at the end
    std::vector<glm::vec4> colors;   // premultiplied, 0..1

    float param(glm::vec2 point) const;   // where a point falls along the gradient
    glm::vec4 color(float t) const;   // premultiplied
};

// A texture for drawing a gradient over some geometry, and that geometry's texture
// coordinates into it.
struct BakedGradient {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;   // premultiplied
    std::vector<glm::vec2> uvs;   // one per vertex
};
// points: the geometry's gradient-space coordinates (RmlUi's vertex tex_coord).
BakedGradient bakeGradient(const Gradient& gradient, std::span<const glm::vec2> points);

struct LayerFilter {
    enum class Kind : uint8_t { Blur, Opacity };
    Kind kind = Kind::Blur;
    float value = 0.0f;   // blur: sigma in pixels; opacity: 0..1
};

// One thing drawn into an off-screen layer, kept until the layer is saved or composited.
struct LayerOp {
    enum class Kind : uint8_t { Draw, ClipMask, ClipEnable, Composite };
    enum class ClipOp : uint8_t { Set, SetInverse, Intersect };
    Kind kind = Kind::Draw;
    // Draw and ClipMask: triangles in output pixels (translation applied).
    std::vector<UiVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<glm::vec2> gradientPoints;   // Draw with a gradient: per vertex
    const Gradient* gradient = nullptr;   // Draw: a gradient instead of a texture
    uintptr_t texture = 0;   // Draw: RmlUi's texture handle (0: none)
    uint32_t transform = kNoUiTransform;
    glm::ivec4 scissor{0, 0, -1, -1};
    ClipOp clip = ClipOp::Set;
    bool enable = false;   // ClipEnable
    // Composite: a layer drawn into this one.
    std::vector<LayerOp> children;
    std::vector<LayerFilter> filters;
    bool replace = false;
};

// Runs fn(begin, end) over parts of [0, count), on worker threads; empty runs it inline.
using ParallelFor = std::function<void(size_t count, const std::function<void(size_t begin, size_t end)>& fn)>;

// Rasterizes a layer's ops into the region (x, y, width, height, in output pixels) and
// returns premultiplied RGBA8 rows. Draws with a texture are skipped (box-shadow has none).
// Rows are split across `parallel`'s threads.
std::vector<uint8_t> rasterizeLayer(std::span<const LayerOp> ops, glm::ivec4 region, const ParallelFor& parallel = {});

}   // namespace shockwave::ui
