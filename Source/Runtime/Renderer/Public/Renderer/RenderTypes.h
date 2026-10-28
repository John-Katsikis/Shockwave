#pragma once

// The values renderers work with: resource handles, materials, lights and settings.
// Shared by the Renderer games use and the RenderBackend plugins implement.

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace shockwave {

struct Color {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

// Opaque handles to GPU resources.
struct MeshHandle {
    uint32_t id = UINT32_MAX;
    bool valid() const { return id != UINT32_MAX; }
};
struct TextureHandle {
    uint32_t id = 0;   // 0 is the built-in white texture, so "no texture" just works
};

// ---- View and object identity ----
// Temporal effects (motion vectors, TAA, previous skinning) keep history per view and per
// object. Neither identity is a position in a sorted list, a transform, or an ImGui window,
// and neither is saved: they're only stable within one run.

// One place the renderer draws from: the editor's viewport, the game's camera, later a
// reflection capture. Each owner makes its own once with create() and passes it to
// Renderer::setCamera, so two views never share history. Default: unknown, no history.
struct RenderViewId {
    uint64_t value = 0;
    bool valid() const { return value != 0; }
    bool operator==(const RenderViewId&) const = default;
    static RenderViewId create();   // unique within this run
};

// Which object a draw is, the same from one frame to the next. World::lifetime() tells a
// copied world (play mode, undo) from the original; entity index and generation tell a
// reused slot from its old owner; part names the model node and mesh. The renderer adds the
// mesh slot's generation, so a reloaded mesh doesn't inherit its old history. A draw without
// a key renders normally, but has no history (Renderer::unkeyedDraws counts them).
struct RenderInstanceKey {
    uint64_t world = 0;
    uint32_t entity = UINT32_MAX;
    uint32_t generation = 0;
    uint32_t part = 0;
    uint32_t meshGeneration = 0;   // filled in by Renderer
    bool valid() const { return world != 0 && entity != UINT32_MAX; }
    bool operator==(const RenderInstanceKey&) const = default;
};
// A ModelRenderer's part: the node, then which of its meshes.
constexpr uint32_t modelPartKey(uint32_t node, uint32_t mesh) { return (node << 8) | (mesh & 0xFF); }

// A reference grid on the ground plane (y = 0), drawn into the scene with depth testing,
// for editors and debug views. Lines fade out towards fadeDistance from the camera.
struct GridSettings {
    bool enabled = false;
    float cellSize = 1.0f;   // units between minor lines
    int majorEvery = 10;   // a stronger line every this many cells
    float fadeDistance = 150.0f;   // units
    float opacity = 1.0f;
};

struct DrawMaterial {
    float alphaCutoff = -1.0f;   // negative: opaque/blended, otherwise alpha test
    bool doubleSided = false;
    bool blend = false;
    glm::vec2 uvScale{1.0f};   // repeats the texture across the surface (MeshRenderer::tiling)
    float roughness = 0.6f;   // 0 mirror-smooth .. 1 fully matte
    float metallic = 0.0f;
    glm::vec3 emissive{0.0f};   // linear HDR color the surface emits
    bool castShadows = true;   // blended (transparent) surfaces never cast
    // Material maps (glTF's), read through the same UVs as the base color texture. 0, the
    // white texture, leaves the factors above as they are.
    TextureHandle normalTexture;   // tangent space, linear (srgb = false); 0: none
    TextureHandle metallicRoughnessTexture;   // linear: green scales roughness, blue metallic
    TextureHandle emissiveTexture;   // sRGB, scales emissive
    TextureHandle occlusionTexture;   // linear: red darkens ambient light only
    float normalScale = 1.0f;   // how strongly the normal map tilts the surface
    float occlusionStrength = 1.0f;   // 0 ignores the occlusion map .. 1 full
};

// A light for this frame, in world space. See the Light component for the meaning of
// each value; spot angles here are cosines.
struct LightDesc {
    enum class Type : uint32_t { Directional, Point, Spot };
    Type type = Type::Point;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};   // where it shines (directional, spot)
    glm::vec3 color{1.0f};   // already multiplied by intensity
    float range = 10.0f;
    float cosOuter = 0.0f, cosInner = 0.0f;   // spot cone, as cosines of the half-angles
    // Shadows (see the Light component). The first shadow-casting directional light gets
    // cascaded shadow maps; spot and point lights share an atlas, nearest first.
    bool castShadows = false;
    float shadowStrength = 1.0f;   // 0 no shadow .. 1 fully dark
    float shadowSoftness = 0.3f;   // 0 sharp .. 1 very soft edges
    float shadowBias = 1.0f;   // multiplies the automatic bias; raise if surfaces shadow themselves
};

// Scene-wide lighting and the HDR-to-display step. Kept until changed.
struct LightingDesc {
    enum class Tonemapper : uint32_t { ACES, AgX, Reinhard, None };
    glm::vec3 skyAmbient{0.32f, 0.42f, 0.58f};   // ambient from above, already scaled
    glm::vec3 groundAmbient{0.18f, 0.16f, 0.14f};   // ambient from below
    float exposure = 0.0f;   // stops
    Tonemapper tonemapper = Tonemapper::ACES;
    float shadowDistance = 80.0f;   // meters from the camera the sun's shadows reach
    uint32_t shadowCascades = 4;   // 1..4: more keeps nearby shadows sharp over long distances
    // Image-based lighting: the light arriving from every direction around the scene, which
    // lights surfaces softly and shows in their reflections. Without a map it's the sky
    // above and the ground below (skyAmbient, groundAmbient).
    TextureHandle environmentMap{};   // equirectangular (2:1), ideally HDR (.hdr); 0: none
    float environmentIntensity = 1.0f;   // multiplies the map's light
    float environmentRotation = 0.0f;   // radians about +Y, counterclockwise seen from above
    bool showEnvironment = true;   // draw the map behind everything instead of the clear color
    // Bright light spreading into its surroundings: how much of the blurred image is mixed
    // into the sharp one. 0 turns it off.
    float bloomIntensity = 0.04f;
};

// What the scene shows: the lit result, or a view for checking content. ShadowCascades
// tints the sun's shadow cascades (red, green, blue, yellow nearest to farthest).
enum class DebugView : uint32_t { Lit, Unlit, Normals, Lighting, ShadowCascades };

// Shadow map resolution, filtering and how many lights can cast at once: a per-device
// performance setting, unlike the scene's shadow distance.
//            sun cascades   light tiles   lights (spot / point)   filter taps
//   Low      1024           256           64 tiles in 2048         4
//   Medium   2048           512           64 tiles in 4096         8
//   High     2048           512           64 tiles in 4096         16
//   Ultra    4096           1024          16 tiles in 4096         32
// A spot light takes one tile, a point light six.
enum class ShadowQuality : uint32_t { Off, Low, Medium, High, Ultra };

// Lights used per frame; more are dropped and counted (Renderer::droppedLights).
inline constexpr uint32_t kMaxLights = 256;

// What a renderer plugin can do (Renderer::backendFlags).
// The renderer draws Dear ImGui (the editor's UI). Without it the editor can't use it.
inline constexpr uint32_t kRendererDrawsGui = 1u << 0;
// The renderer shows pictures on screen; without it (the headless "null" renderer) only
// game logic and tools run.
inline constexpr uint32_t kRendererPresents = 1u << 1;

struct ShadowStats {
    uint32_t cascades = 0;   // the sun's cascades drawn last frame (0: no sun shadow)
    uint32_t shadowedLights = 0;   // spot and point lights that cast shadows
    uint32_t skippedLights = 0;   // wanted to cast but the atlas was full
    uint32_t casterDraws = 0;   // draws into all shadow maps, after culling
};

struct PassTiming {
    std::string name;
    float gpuMs = 0;
};

// ---- Capabilities, effective settings and memory (Renderer::capabilities, ...) ----

// One thing the device can or can't do that an effect depends on.
struct RenderCapability {
    std::string name;   // "storage image rgba16f", "multi draw indirect", ...
    bool supported = false;
    std::string detail;   // the value, or why it's missing
};
struct RenderLimit {
    std::string name;
    uint64_t value = 0;
};
struct RendererCapabilities {
    std::string backend;   // the plugin: "vulkan", "null"
    std::string device, driver, api;   // empty without a GPU
    std::vector<RenderCapability> features;
    std::vector<RenderLimit> limits;
};

// A graphics setting as asked for, and as the renderer actually runs it. They differ when
// the device can't do what was asked; reason says why. A fallback is never silent.
struct EffectiveSetting {
    std::string name;   // as spelled in .swgraphics: "render_scale", "vsync", ...
    std::string requested, effective;
    std::string reason;   // empty when they agree
};

// GPU memory by what it's for. Later effects (prepass, AO, history, baked lighting,
// clusters, HZB) report into their own categories from the start.
enum class GpuMemoryCategory : uint32_t {
    RenderTargets,   // scene color, depth, display
    ShadowMaps,
    Environment,   // image-based lighting cubes and BRDF table
    Bloom,
    Meshes,   // vertices, indices, levels of detail, skin weights
    Textures,
    FrameData,   // per-frame draw tables, lights, joints, skinned vertices
    Particles,
    GameUi,
    Readback,   // screenshots
    Staging,   // uploads in flight
    SurfaceBuffers,
    AmbientOcclusion,
    TemporalHistory,
    Lightmaps,
    DiffuseProbes,
    ReflectionProbes,
    Clusters,
    Hzb,
    Count
};
const char* gpuMemoryCategoryName(GpuMemoryCategory category);   // "render targets", ...
struct GpuMemoryUsage {
    uint64_t bytes = 0;   // allocated, as the driver sized it
    uint32_t allocations = 0;
};
struct RendererMemoryReport {
    std::array<GpuMemoryUsage, size_t(GpuMemoryCategory::Count)> gpu{};
    uint64_t pipelineCacheBytes = 0;
    // The driver's view of device-local memory, all heaps: what this process uses and how
    // much it may use before things slow down or fail. 0: unknown.
    uint64_t deviceUsage = 0, deviceBudget = 0;
    // CPU copies of meshes and textures kept for swapping the backend (editor). Shared
    // memory GPUs don't make these free: they're separate copies.
    uint64_t cpuCopyBytes = 0;
    uint64_t gpuBytes() const {
        uint64_t total = 0;
        for (const auto& usage : gpu) total += usage.bytes;
        return total;
    }
    GpuMemoryUsage& operator[](GpuMemoryCategory c) { return gpu[size_t(c)]; }
    const GpuMemoryUsage& operator[](GpuMemoryCategory c) const { return gpu[size_t(c)]; }
};

// ---- Game UI overlay (GameUI module, docs/GameUI.md) ----
// 2D geometry drawn over the finished frame at output resolution: menus, HUDs, text.
// Positions are in output pixels from the top-left corner. Colors and textures are
// premultiplied alpha in sRGB, blended in sRGB like a web browser, so text looks the same
// as in other UI tools. A UI texture is an ordinary texture made with srgb = false.
struct UiVertex {
    glm::vec2 position;
    uint32_t color;   // premultiplied RGBA8, red in the lowest byte
    glm::vec2 uv;
};
inline constexpr uint32_t kNoUiTransform = UINT32_MAX;
struct UiDrawCommand {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    TextureHandle texture;   // 0: plain color
    uint32_t transform = kNoUiTransform;   // index into UiDrawList::transforms
    glm::ivec4 scissor{0, 0, -1, -1};   // x, y, width, height in output pixels; negative width: none
};
struct UiDrawList {
    std::span<const UiVertex> vertices;
    std::span<const uint32_t> indices;   // into vertices, three per triangle
    std::span<const UiDrawCommand> commands;   // in drawing order
    std::span<const glm::mat4> transforms;   // CSS transforms, in output pixels
    glm::uvec2 size{0};   // the output size the UI was laid out for
};

}   // namespace shockwave
