#pragma once

// The values renderers work with: resource handles, materials, lights and settings.
// Shared by the Renderer games use and the RenderBackend plugins implement.

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

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

}   // namespace shockwave
