#pragma once

// RenderBackend: what a renderer plugin implements. Games and tools never see it; they
// use Renderer, which forwards to the current backend and can swap it for another (or a
// rebuilt copy of the same one) while running. See docs/Plugins.md.
//
// A plugin library defines a PluginDesc of kind "renderer" whose api points to a
// RendererPluginApi:
//
//     const RendererPluginApi api{.flags = kRendererDrawsGui, .create = ...};
//     const PluginDesc plugin{.kind = "renderer", .name = "vulkan", .description = "...", .api = &api};
//     SHOCKWAVE_PLUGIN(plugin)
//
// The contract, beyond what Renderer documents for each call:
// - Renderer picks resource ids, so they stay the same when backends swap. Mesh ids are
//   below maxMeshes(); texture ids are below maxTextures(), and 0 is the backend's own
//   white texture, which it makes when it starts.
// - Every setting (vsync, render scale, lighting...) is applied again after a swap, so a
//   new backend starts from its defaults and doesn't need to read the old one's state.
// - The destructor waits for the GPU and frees everything. It runs before the library
//   is unloaded.

#include <Renderer/Mesh.h>
#include <Renderer/Particles.h>
#include <Renderer/RenderTypes.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace shockwave {

class JobSystem;
class Window;

class RenderBackend {
public:
    virtual ~RenderBackend() = default;

    // ---- Resources: Renderer picks the ids ----
    virtual uint32_t maxMeshes() const = 0;
    virtual uint32_t maxTextures() const = 0;
    virtual void createMesh(MeshHandle id, std::span<const Vertex> vertices, std::span<const uint32_t> indices) = 0;
    virtual void destroyMesh(MeshHandle id) = 0;
    virtual void
    createTexture(TextureHandle id, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) = 0;
    virtual void
    updateTexture(TextureHandle id, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) = 0;
    virtual void destroyTexture(TextureHandle id) = 0;
    virtual uint64_t guiTexture(TextureHandle id) = 0;

    // ---- Per frame ----
    virtual void setClearColor(const Color& color) = 0;
    virtual void setCamera(const glm::mat4& view, const glm::mat4& projection) = 0;
    virtual void drawMesh(
        MeshHandle mesh, const glm::mat4& transform, TextureHandle texture, const glm::vec4& tint,
        const DrawMaterial& material) = 0;
    virtual void addLight(const LightDesc& light) = 0;
    // This frame's particles (Renderer/Particles.h); copy what you need before returning.
    virtual void drawParticles(const ParticleDrawList& particles) = 0;

    // ---- Settings ----
    virtual void setGrid(const GridSettings& grid) = 0;
    virtual void setLighting(const LightingDesc& lighting) = 0;
    virtual void setDebugView(DebugView view) = 0;
    virtual void setShadowQuality(ShadowQuality quality) = 0;
    virtual void setRenderScale(float scale) = 0;   // already clamped by Renderer
    virtual void setViewport(uint32_t width, uint32_t height) = 0;
    virtual void setVsync(bool vsync) = 0;
    virtual void setLowLatency(bool lowLatency) = 0;

    // ---- Results of the last frame ----
    virtual uint32_t lightCount() const = 0;
    virtual uint32_t droppedLights() const = 0;
    virtual ShadowStats shadowStats() const = 0;
    virtual std::string frameGraph() const = 0;
    virtual uint32_t renderWidth() const = 0;
    virtual uint32_t renderHeight() const = 0;
    virtual uint64_t sceneTexture() const = 0;
    virtual const std::vector<PassTiming>& passTimings() const = 0;
    virtual float gpuFrameMs() const = 0;
    virtual float aspectRatio() const = 0;
    virtual const char* gpuName() const = 0;
    virtual uint32_t drawCount() const = 0;

    // ---- Screenshots ----
    virtual void requestScreenshot(const std::string& path) = 0;
    virtual bool screenshotPending() const = 0;
    virtual const std::string& screenshotResult() const = 0;

    // ---- Presentation ----
    virtual float waitForPresent() = 0;
    virtual const char* presentTiming() const = 0;
    virtual const char* presentModeName() const = 0;

    // ---- Engine calls ----
    virtual bool beginFrame() = 0;
    virtual void endFrame() = 0;
    virtual void onResize() = 0;
    virtual void waitIdle() = 0;
    virtual void initGui() = 0;
    virtual void shutdownGui() = 0;
    virtual void newGuiFrame() = 0;
    virtual void renderGuiWindows() = 0;

    // ---- Game UI overlay (RenderTypes.h) ----
    // The size in pixels the overlay draws at: the editor viewport, or the window.
    virtual glm::uvec2 uiOutputSize() const = 0;
    // This frame's overlay. It's drawn after tonemapping into the display image, so the
    // window, the editor viewport and screenshots all show it. The backend copies what it
    // needs before returning. Main thread, between beginFrame and endFrame.
    virtual void drawUi(const UiDrawList& list) = 0;
};

struct RenderBackendDesc {
    Window& window;
    JobSystem& jobs;
    bool vsync = true;
};

struct RendererPluginApi {
    uint32_t flags = 0;
    // Throws std::exception with a readable message when it can't start (no GPU, say).
    std::unique_ptr<RenderBackend> (*create)(const RenderBackendDesc& desc) = nullptr;
};

}   // namespace shockwave
