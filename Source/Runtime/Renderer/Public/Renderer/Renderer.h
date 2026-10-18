#pragma once

#include <Renderer/GpuSelection.h>
#include <Renderer/Mesh.h>
#include <Renderer/Particles.h>
#include <Renderer/RenderTypes.h>
#include <Renderer/TextureData.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace shockwave {

class JobSystem;
class Window;

struct RendererOptions {
    std::string backend = "vulkan";   // a renderer plugin: "vulkan", "null"; see availableBackends()
    bool vsync = true;
    // Keep a CPU copy of every mesh and texture so the backend can be swapped or reloaded
    // while running. The editor turns this on; it costs memory equal to the asset data.
    bool swappable = false;
};

// Public face of the renderer. The drawing itself is done by a backend plugin (Vulkan by
// default; see RenderBackend.h), so games never include Vulkan headers, a Metal or
// headless renderer can stand in, and the editor can swap or reload the backend while
// it runs without anyone re-creating their meshes and textures.
//
// Threading: createMesh, createTexture, updateTexture and drawMesh may be called from
// any thread, including job system workers. Draws go into per-thread lists, so
// recording them doesn't contend. setClearColor and setCamera belong to the thread running
// Application::onRender, and setCamera must come before that frame's drawMesh calls.
//
// Swapping: switchBackend and reloadBackend tear the backend down and start the new one
// between frames, then replay every setting and resource. Handles stay valid. It needs
// RendererOptions::swappable, which keeps a CPU copy of each mesh and texture.
class Renderer {
public:
    // Throws std::runtime_error when the backend can't be loaded or started.
    Renderer(Window& window, JobSystem& jobs, const RendererOptions& options);
    ~Renderer();

    using ShadowStats = shockwave::ShadowStats;
    using PassTiming = shockwave::PassTiming;
    static constexpr uint32_t kMaxLights = shockwave::kMaxLights;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Resources live until the renderer shuts down.
    MeshHandle createMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices);
    MeshHandle createMesh(const MeshData& mesh) { return createMesh(mesh.vertices, mesh.indices); }
    // A mesh that bends with a skeleton: one VertexSkin per vertex. Draw it with
    // drawSkinnedMesh to pose it; drawMesh shows it as modelled (its bind pose).
    MeshHandle createSkinnedMesh(
        std::span<const Vertex> vertices, std::span<const uint32_t> indices, std::span<const VertexSkin> skin);
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

    // ---- Optimised assets (docs/Optimisation.md) ----
    // Simpler levels of detail for a mesh (MeshData::lods and lodIndices), right after
    // createMesh. Each draw then uses the simplest level that looks the same at its
    // distance (setLodSettings). Same threads as createMesh.
    void setMeshLods(MeshHandle mesh, std::span<const MeshLod> lods, std::span<const uint32_t> lodIndices);
    // A texture in a format the backend supports (supportsTextureFormat), mipmaps
    // included: BC7 from a .ktx2 file, say. Same threads as createTexture.
    TextureHandle createTexture(const TextureData& texture);
    void updateTexture(TextureHandle texture, const TextureData& data);
    bool supportsTextureFormat(TextureFormat format) const;
    // Kept until changed. Main thread.
    void setLodSettings(const LodSettings& settings);
    const LodSettings& lodSettings() const;
    LodStats lodStats() const;   // last frame's

    // Per-frame commands, called from Application::onRender.
    void setClearColor(const Color& color);
    // view: a standard right-handed, Y-up view matrix (glm::lookAt).
    // projection: reverse-Z (see perspectiveReverseZ in Camera.h).
    void setCamera(const glm::mat4& view, const glm::mat4& projection);
    // The camera set last, for systems that build geometry for it (particle effects).
    const glm::mat4& cameraView() const;
    const glm::mat4& cameraProjection() const;
    void drawMesh(
        MeshHandle mesh, const glm::mat4& transform, TextureHandle texture = {},
        const glm::vec4& tint = glm::vec4(1.0f), const DrawMaterial& material = {});
    // A skinned mesh in a pose. joints: one matrix per joint the mesh's VertexSkins name,
    // taking a vertex from the mesh as modelled to its posed place in the mesh's space
    // (the joint's current matrix times its inverse bind matrix). The renderer skins the
    // vertices on the GPU once and the scene and every shadow map draw the result. Same
    // threads as drawMesh; joints is copied before it returns.
    void drawSkinnedMesh(
        MeshHandle mesh, const glm::mat4& transform, std::span<const glm::mat4> joints, TextureHandle texture = {},
        const glm::vec4& tint = glm::vec4(1.0f), const DrawMaterial& material = {});
    // Kept until changed. Same thread as setCamera.
    void setGrid(const GridSettings& grid);
    // Particle geometry for this frame (Renderer/Particles.h), drawn into the HDR scene
    // after the opaque geometry, depth tested and blended in the order given. Same thread
    // as setCamera; the data is copied, so it can be reused straight away. Several calls
    // in one frame add up.
    void drawParticles(const ParticleDrawList& particles);

    // Lights for this frame; cleared after each frame. Same thread as setCamera. Up to
    // kMaxLights are used, the rest are dropped (and counted in droppedLights()). With no
    // lights at all, a default sun keeps unlit scenes readable.
    void addLight(const LightDesc& light);
    void setLighting(const LightingDesc& lighting);
    void setDebugView(DebugView view);
    DebugView debugView() const;
    uint32_t lightCount() const;   // lights used last frame, not counting the default sun
    uint32_t droppedLights() const;   // lights over kMaxLights last frame
    // Main thread. Changing it rebuilds the shadow maps before the next frame.
    void setShadowQuality(ShadowQuality quality);
    ShadowQuality shadowQuality() const;
    ShadowStats shadowStats() const;

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
    // The GPU time of each render graph pass in the latest frame the GPU has finished, and
    // their total. Empty and 0 when the GPU has no timestamp support.
    const std::vector<PassTiming>& passTimings() const;
    float gpuFrameMs() const;

    float aspectRatio() const;   // of the scene's output (window or viewport), independent of render scale
    const char* gpuName() const;
    // Every GPU found and why this one renders (GpuSelection.h); null without a GPU.
    const GpuChoice* gpuChoice() const;
    uint32_t drawCount() const;   // draws submitted last frame
    uint32_t culledDraws() const;   // of those, outside the camera's view, so not drawn in the scene

    // Called by the engine. beginFrame returns false when there is nothing to
    // draw into (minimized window, swapchain being rebuilt).
    bool beginFrame();
    void endFrame();
    void onResize();
    void waitIdle();
    // After ImGui::CreateContext and the SDL3 backend. A renderer's GUI shutdown takes down
    // every GUI platform window with it, the main one included, so after a swap
    // restartPlatform is called to set the platform side up again before the new backend's
    // GUI starts.
    void initGui(std::function<void()> restartPlatform = {});
    void shutdownGui();
    void newGuiFrame();
    // Creates, moves and draws the windows of GUI panels dragged out of the main window.
    // After endFrame, on the main thread; nothing to do unless multi-viewports are on.
    void renderGuiWindows();

    // ---- Backends ----
    // The renderer plugins that can be loaded, built-in ones first.
    static std::vector<std::string> availableBackends();
    const std::string& backendName() const;
    const char* backendDescription() const;
    uint32_t backendFlags() const;   // kRendererDrawsGui, kRendererPresents (RenderBackend.h)
    bool swappable() const;
    // Replaces the backend with another plugin, between frames on the main thread. Every
    // handle stays valid. On failure (missing plugin, it can't start) the current backend
    // keeps running and error says why.
    bool switchBackend(const std::string& name, std::string& error);
    // Starts the current backend again from its library as it is on disk now: rebuild a
    // renderer plugin (or its shaders), reload, and see the change without restarting.
    bool reloadBackend(std::string& error);
    // The current backend's library, to watch for rebuilds; empty when it's built in.
    const std::filesystem::path& backendFile() const;

    // ---- Game UI overlay (GameUI module, RenderTypes.h) ----
    // The size in pixels UI draws at: the editor viewport, or the window. Main thread.
    glm::uvec2 uiOutputSize() const;
    // Draws this frame's UI over the tonemapped scene; screenshots include it. Call from
    // Application::onRender, on the main thread. The list is copied before it returns.
    void drawUi(const UiDrawList& list);

private:
    struct State;
    std::unique_ptr<State> m;
};

}   // namespace shockwave
