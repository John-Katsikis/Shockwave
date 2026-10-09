#pragma once

#include <shockwave/Mesh.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>

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

// Public face of the renderer. Vulkan stays behind this interface so games never
// include Vulkan headers, which keeps a later Metal/MoltenVK port contained.
//
// Threading: createMesh, createTexture, updateTexture and drawMesh may be called from
// any thread, including job system workers. Draws go into per-thread lists, so
// recording them doesn't contend. setClearColor and setCamera belong to the thread running
// Game::onRender, and setCamera must come before that frame's drawMesh calls.
class Renderer {
public:
    Renderer(Window& window, JobSystem& jobs, bool vsync);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Resources live until the renderer shuts down.
    MeshHandle createMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices);
    MeshHandle createMesh(const MeshData& mesh) { return createMesh(mesh.vertices, mesh.indices); }
    // RGBA8 pixels, rows top to bottom. Mipmaps are generated on the GPU. Every texture
    // lives in one bindless table, so any draw can use any texture at no extra cost.
    TextureHandle createTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb = true);
    // Replaces a texture's pixels (and size, if it changed) for hot reload. Everything
    // using the handle shows the new image from the next frame. Any thread.
    void updateTexture(TextureHandle texture, uint32_t width, uint32_t height, std::span<const uint8_t> rgba,
                       bool srgb = true);
    // An ImTextureID for showing a texture in the GUI (an asset browser thumbnail, say);
    // 0 when the GUI is off. Main thread. Valid for the current frame: ask every frame,
    // since a reload hands out a new one.
    uint64_t guiTexture(TextureHandle texture);

    // Per-frame commands, called from Game::onRender.
    void setClearColor(const Color& color);
    // view: a standard right-handed, Y-up view matrix (glm::lookAt).
    // projection: reverse-Z (see perspectiveReverseZ in Camera.h).
    void setCamera(const glm::mat4& view, const glm::mat4& projection);
    void drawMesh(MeshHandle mesh, const glm::mat4& transform, TextureHandle texture = {},
                  const glm::vec4& tint = glm::vec4(1.0f));

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

    float aspectRatio() const;   // of the scene's output (window or viewport), independent of render scale
    const char* gpuName() const;
    uint32_t drawCount() const;   // draws submitted last frame

    // Called by the engine. beginFrame returns false when there is nothing to
    // draw into (minimized window, swapchain being rebuilt).
    bool beginFrame();
    void endFrame();
    void onResize();
    void waitIdle();
    void initGui();       // after ImGui::CreateContext and the SDL3 backend
    void shutdownGui();
    void newGuiFrame();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shockwave
