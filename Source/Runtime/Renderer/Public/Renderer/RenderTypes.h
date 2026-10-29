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

// ---- How a material reads its textures (G02, docs/Graphics.md) ----

// Addressing outside 0..1 and filtering, per texture slot. glTF samplers map onto these
// one to one; a texture that asks for nearest filtering (pixel art) keeps it whatever the
// graphics settings say.
enum class TextureWrap : uint8_t { Repeat, Clamp, Mirror };
enum class TextureFilter : uint8_t { Linear, Nearest };
const char* textureWrapName(TextureWrap wrap);   // "repeat", "clamp", "mirror"
const char* textureFilterName(TextureFilter filter);   // "linear", "nearest"
struct SamplerDesc {
    TextureWrap wrapU = TextureWrap::Repeat, wrapV = TextureWrap::Repeat;
    TextureFilter magFilter = TextureFilter::Linear, minFilter = TextureFilter::Linear;
    TextureFilter mipFilter = TextureFilter::Linear;
    bool mipmaps = true;   // false: only the largest resident level (glTF's NEAREST / LINEAR min filters)
    // What the asset asks for, 1..16. The anisotropy graphics setting and the device cap it.
    uint8_t maxAnisotropy = 16;
    bool operator==(const SamplerDesc&) const = default;
};
// A cached sampler (Renderer::sampler). 0 is the default: SamplerDesc{}.
struct SamplerHandle {
    uint32_t id = 0;
    bool operator==(const SamplerHandle&) const = default;
};
inline constexpr uint32_t kMaxSamplers = 64;   // distinct SamplerDescs, the default included

// glTF's KHR_texture_transform: uv' = offset + R(rotation) * (scale * uv), rotating about
// the UV origin, with R = [cos sin; -sin cos] as glTF defines it.
struct UvTransform {
    glm::vec2 offset{0.0f};
    glm::vec2 scale{1.0f};
    float rotation = 0.0f;   // radians
    bool identity() const { return offset == glm::vec2(0.0f) && scale == glm::vec2(1.0f) && rotation == 0.0f; }
    glm::vec2 apply(glm::vec2 uv) const;   // the formula above, as the shaders do it
    bool operator==(const UvTransform&) const = default;
};
// The texture slots of a material, in this order everywhere (ModelMaterial, DrawMaterial).
enum class MaterialSlot : uint8_t { BaseColor, Normal, MetallicRoughness, Emissive, Occlusion, Count };
inline constexpr size_t kMaterialSlots = size_t(MaterialSlot::Count);
const char* materialSlotName(MaterialSlot slot);   // "base_color", "normal", ...
// How one slot reads its texture: which UV set, transformed how, sampled how.
struct TextureBinding {
    uint32_t uvSet = 0;   // 0: TEXCOORD_0, 1: TEXCOORD_1 (Vertex::uv, then MeshData::uv1)
    UvTransform transform;
    SamplerHandle sampler;
    bool operator==(const TextureBinding&) const = default;
};

// What a texture is for decides whether the texture_quality setting may shrink it (A01).
// Material textures (base color, normal and data maps, decals, particle sprites) drop
// their largest mip levels at lower quality; Fixed ones (UI, fonts, lookup tables,
// environment maps) always stay whole.
enum class TextureUsage : uint8_t { Fixed, Material };
// Texture detail: how many of a material texture's largest mip levels stay on the GPU.
//            first level kept   memory (about)
//   Low      3 (1/8 size)       1/64
//   Medium   2 (1/4 size)       1/16
//   High     1 (1/2 size)       1/4
//   Ultra    0 (full size)      all
// Levels are dropped from the GPU copy, not just skipped when sampling, so the memory
// really is freed. Textures never shrink below kMinResidentSize on their longer side.
enum class TextureQuality : uint32_t { Low, Medium, High, Ultra };
const char* textureQualityName(TextureQuality quality);   // "low", "medium", "high", "ultra"
uint32_t textureQualityFirstMip(TextureQuality quality);   // 3, 2, 1, 0
inline constexpr uint32_t kMinResidentSize = 32;
// What the texture_quality setting is doing, for reports (Renderer::textureResidency).
struct TextureResidencyStats {
    uint32_t textures = 0;   // material textures the setting applies to
    uint32_t reduced = 0;   // of those, how many have levels dropped now
    uint32_t pending = 0;   // waiting for, or in the middle of, a change of levels
    uint64_t residentBytes = 0;   // what those textures take on the GPU now
    uint64_t fullBytes = 0;   // ...and what they would take at ultra
    uint64_t retainedBytes = 0;   // CPU copies kept so their levels can change without reloading
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
    // Each slot's UV set, UV transform and sampler, indexed by MaterialSlot. uvScale above
    // applies first, to every slot.
    std::array<TextureBinding, kMaterialSlots> bindings{};
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

// Spatial anti-aliasing after tone mapping, before game UI (G01). TAA arrives with G10.
enum class AntiAliasing : uint32_t { None, Fxaa };
const char* antiAliasingName(AntiAliasing aa);   // "none", "fxaa"
// Bloom's workload (A00). The scene's look stays Environment::bloom_intensity; this picks
// how finely it's filtered:
//            first level   levels
//   Low      1/4 scene     4
//   Medium   1/2 scene     5
//   High     1/2 scene     6      (what bloom always was before A00)
// Off skips the passes and frees the bloom chain.
enum class BloomQuality : uint32_t { Off, Low, Medium, High };
const char* bloomQualityName(BloomQuality quality);   // "off", "low", "medium", "high"

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
    std::string detail;   // what the effective value costs or comes to: atlas sizes, levels, ...
};

// GPU memory by what it's for. Later effects (prepass, AO, history, baked lighting,
// clusters, HZB) report into their own categories from the start.
enum class GpuMemoryCategory : uint32_t {
    RenderTargets,   // scene color, depth, display
    ShadowMaps,
    Environment,   // image-based lighting cubes and BRDF table
    Bloom,
    AntiAliasing,   // FXAA's input image
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
    // CPU copies of meshes and textures kept for swapping the backend (editor), and of
    // material textures kept so texture_quality can change their levels (A01). Shared
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
