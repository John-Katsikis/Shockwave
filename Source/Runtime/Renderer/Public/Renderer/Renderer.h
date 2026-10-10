#pragma once

#include <Renderer/Mesh.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace shockwave {

class JobSystem;
class Window;

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

// Public face of the renderer. Vulkan stays behind this interface so games never
// include Vulkan headers, which keeps a later Metal/MoltenVK port contained.
//
// Threading: createMesh, createTexture, updateTexture and drawMesh may be called from
// any thread, including job system workers. Draws go into per-thread lists, so
// recording them doesn't contend. setClearColor and setCamera belong to the thread running
// Application::onRender, and setCamera must come before that frame's drawMesh calls.
struct DrawMaterial {
    float alphaCutoff = -1.0f;   // negative: opaque/blended, otherwise alpha test
    bool doubleSided = false;
    bool blend = false;
    glm::vec2 uvScale{1.0f};   // repeats the texture across the surface (MeshRenderer::tiling)
    float roughness = 0.6f;   // 0 mirror-smooth .. 1 fully matte
    float metallic = 0.0f;
    glm::vec3 emissive{0.0f};   // linear HDR color the surface emits
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
};

// Scene-wide lighting and the HDR-to-display step. Kept until changed.
struct LightingDesc {
    enum class Tonemapper : uint32_t { ACES, AgX, Reinhard, None };
    glm::vec3 skyAmbient{0.32f, 0.42f, 0.58f};   // ambient from above, already scaled
    glm::vec3 groundAmbient{0.18f, 0.16f, 0.14f};   // ambient from below
    float exposure = 0.0f;   // stops
    Tonemapper tonemapper = Tonemapper::ACES;
};

// What the scene shows: the lit result, or a view for checking content.
enum class DebugView : uint32_t { Lit, Unlit, Normals, Lighting };

class Renderer {
public:
    Renderer(Window& window, JobSystem& jobs, bool vsync);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Resources live until the renderer shuts down.
    MeshHandle createMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices);
    MeshHandle createMesh(const MeshData& mesh) { return createMesh(mesh.vertices, mesh.indices); }
    // Main thread, between frames, after users have dropped these handles. Waits
    // for GPU readers and recycles the slot. Used by transactional model reload.
    void destroyMesh(MeshHandle mesh);
    void destroyTexture(TextureHandle texture);
    // RGBA8 pixels, rows top to bottom. Mipmaps are generated on the GPU. Every texture
    // lives in one bindless table, so any draw can use any texture at no extra cost.
    TextureHandle createTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb = true);
    // Replaces a texture's pixels (and size, if it changed) for hot reload. Everything
    // using the handle shows the new image from the next frame. Any thread.
    void updateTexture(
        TextureHandle texture, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb = true);
    // An ImTextureID for showing a texture in the GUI (an asset browser thumbnail, say);
    // 0 when the GUI is off. Main thread. Valid for the current frame: ask every frame,
    // since a reload hands out a new one.
    uint64_t guiTexture(TextureHandle texture);

    // Per-frame commands, called from Application::onRender.
    void setClearColor(const Color& color);
    // view: a standard right-handed, Y-up view matrix (glm::lookAt).
    // projection: reverse-Z (see perspectiveReverseZ in Camera.h).
    void setCamera(const glm::mat4& view, const glm::mat4& projection);
    void drawMesh(
        MeshHandle mesh, const glm::mat4& transform, TextureHandle texture = {},
        const glm::vec4& tint = glm::vec4(1.0f), const DrawMaterial& material = {});
    // Kept until changed. Same thread as setCamera.
    void setGrid(const GridSettings& grid);

    // Lights for this frame; cleared after each frame. Same thread as setCamera. Up to
    // kMaxLights are used, the rest are dropped (and counted in droppedLights()). With no
    // lights at all, a default sun keeps unlit scenes readable.
    static constexpr uint32_t kMaxLights = 256;
    void addLight(const LightDesc& light);
    void setLighting(const LightingDesc& lighting);
    void setDebugView(DebugView view);
    DebugView debugView() const;
    uint32_t lightCount() const;   // lights used last frame, not counting the default sun
    uint32_t droppedLights() const;   // lights over kMaxLights last frame

    // Saves the next finished frame, tonemapped, as a PNG at output resolution (the
    // editor viewport or the window, without GUI). Main thread. The result is reported
    // by screenshotResult() once screenshotPending() turns false: empty on success,
    // otherwise why it failed.
    void requestScreenshot(const std::string& path);
    bool screenshotPending() const;
    const std::string& screenshotResult() const;

    // The last frame's render graph as text: each pass in order, what it reads and
    // writes, and the barriers the graph inserted. For the Stats window and tools.
    std::string frameGraph() const;

    // Resolution independence: the scene renders into its own HDR target at
    // renderScale x window resolution, then is scaled to the window. 1.0 = native,
    // below 1 trades sharpness for speed, above 1 supersamples. Clamped to 0.25..2.
    void setRenderScale(float scale);
    float renderScale() const;
    uint32_t renderWidth() const;
    uint32_t renderHeight() const;

    // Editor viewports: render the scene offscreen at width x height pixels (before render
    // scale) instead of to the window, and show it in the GUI with
    // ImGui::Image(renderer.sceneTexture(), size). (0, 0) goes back to the window.
    void setViewport(uint32_t width, uint32_t height);
    uint64_t sceneTexture() const;   // an ImTextureID; 0 when the GUI is off

    // Vsync on caps the frame rate to the display (FIFO); off renders flat out.
    void setVsync(bool vsync);
    bool vsync() const;

    // ---- Frame pacing and GPU timing ----
    // With vsync, show the newest finished frame at each refresh rather than queueing
    // frames (FIFO latest ready, where the driver has it).
    void setLowLatency(bool lowLatency);
    // Blocks until the last presented frame has reached the screen (Vulkan present wait),
    // or, without present wait, until the GPU has finished it. The engine's low-latency
    // mode calls this before reading input, so input is as fresh as possible when the
    // frame is shown. Returns the time between the last two frames reaching the screen,
    // in milliseconds, or 0 when that's unknown.
    float waitForPresent();
    const char* presentTiming() const;   // how presentation is timed: "present wait 2", ...
    const char* presentModeName() const;   // "FIFO", "mailbox", ...
    struct PassTiming {
        std::string name;
        float gpuMs = 0;
    };
    // The GPU time of each render graph pass in the latest frame the GPU has finished, and
    // their total. Empty and 0 when the GPU has no timestamp support.
    const std::vector<PassTiming>& passTimings() const;
    float gpuFrameMs() const;

    float aspectRatio() const;   // of the scene's output (window or viewport), independent of render scale
    const char* gpuName() const;
    uint32_t drawCount() const;   // draws submitted last frame

    // Called by the engine. beginFrame returns false when there is nothing to
    // draw into (minimized window, swapchain being rebuilt).
    bool beginFrame();
    void endFrame();
    void onResize();
    void waitIdle();
    void initGui();   // after ImGui::CreateContext and the SDL3 backend
    void shutdownGui();
    void newGuiFrame();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}   // namespace shockwave
