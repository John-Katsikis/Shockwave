#include "RendererImpl.h"

namespace shockwave {

Renderer::Renderer(Window& window, JobSystem& jobs, bool vsync) : m_impl(std::make_unique<Impl>(window, jobs, vsync)) {}
Renderer::~Renderer() = default;

void Renderer::destroyMesh(MeshHandle handle) { m_impl->destroyMesh(handle); }
void Renderer::destroyTexture(TextureHandle handle) { m_impl->destroyTexture(handle); }

MeshHandle Renderer::createMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices) {
    return m_impl->createMesh(vertices, indices);
}
TextureHandle Renderer::createTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) {
    return m_impl->createTexture(width, height, rgba, srgb);
}

void Renderer::updateTexture(
    TextureHandle texture, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) {
    m_impl->updateTexture(texture, width, height, rgba, srgb);
}

uint64_t Renderer::guiTexture(TextureHandle texture) { return m_impl->guiTexture(texture); }
void Renderer::setClearColor(const Color& color) { m_impl->clearColor = color; }
void Renderer::setCamera(const glm::mat4& view, const glm::mat4& projection) {
    m_impl->viewProjection = projection * view;
    m_impl->cameraPosition = glm::vec3(glm::inverse(view)[3]);
}
void Renderer::setGrid(const GridSettings& grid) { m_impl->grid = grid; }
void Renderer::addLight(const LightDesc& light) {
    std::lock_guard lock(m_impl->lightMutex);
    m_impl->lights.push_back(light);
}
void Renderer::setLighting(const LightingDesc& lighting) { m_impl->lighting = lighting; }
void Renderer::setDebugView(DebugView view) { m_impl->debugView = view; }
DebugView Renderer::debugView() const { return m_impl->debugView; }
uint32_t Renderer::lightCount() const { return m_impl->lastLightCount; }
uint32_t Renderer::droppedLights() const { return m_impl->lastDroppedLights; }
void Renderer::requestScreenshot(const std::string& path) {
    m_impl->screenshotPath = path;
    m_impl->screenshotRequested = true;
    m_impl->screenshotMessage.clear();
}
bool Renderer::screenshotPending() const { return m_impl->screenshotRequested; }
const std::string& Renderer::screenshotResult() const { return m_impl->screenshotMessage; }
std::string Renderer::frameGraph() const { return m_impl->frameGraphText; }
void Renderer::drawMesh(
    MeshHandle mesh, const glm::mat4& transform, TextureHandle texture, const glm::vec4& tint,
    const DrawMaterial& material) {
    m_impl->drawMesh(mesh, transform, texture, tint, material);
}
void Renderer::setRenderScale(float scale) {
    scale = std::clamp(scale, 0.25f, 2.0f);
    if (scale != m_impl->renderScale) {
        m_impl->renderScale = scale;
        m_impl->needsResize = true;
    }
}
float Renderer::renderScale() const { return m_impl->renderScale; }
uint32_t Renderer::renderWidth() const { return m_impl->sceneColor.extent.width; }
uint32_t Renderer::renderHeight() const { return m_impl->sceneColor.extent.height; }
void Renderer::setViewport(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) width = height = 0;
    if (width != m_impl->viewportSize.width || height != m_impl->viewportSize.height) {
        m_impl->viewportSize = {width, height};
        m_impl->needsSceneResize = true;
    }
}
uint64_t Renderer::sceneTexture() const { return reinterpret_cast<uint64_t>(m_impl->guiSceneTexture); }
void Renderer::setVsync(bool vsync) {
    if (vsync != m_impl->swapchain->vsync()) {
        m_impl->swapchain->setVsync(vsync);
        m_impl->needsResize = true;
    }
}
bool Renderer::vsync() const { return m_impl->swapchain->vsync(); }
void Renderer::setLowLatency(bool lowLatency) {
    m_impl->swapchain->setLowLatency(lowLatency);
    // Only the vsync present mode depends on it.
    if (m_impl->swapchain->vsync() && m_impl->context->fifoLatestReady()) m_impl->needsResize = true;
}
float Renderer::waitForPresent() { return m_impl->waitForPresent(); }
const char* Renderer::presentTiming() const { return m_impl->context->presentTimingName(); }
const char* Renderer::presentModeName() const {
    switch (m_impl->swapchain->presentMode()) {
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (vsync)";
    case VK_PRESENT_MODE_FIFO_LATEST_READY_KHR: return "FIFO latest ready (vsync, low latency)";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO relaxed";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox (uncapped, no tearing)";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate (uncapped, may tear)";
    default: return "other";
    }
}
const std::vector<Renderer::PassTiming>& Renderer::passTimings() const { return m_impl->passTimings; }
float Renderer::gpuFrameMs() const { return m_impl->gpuFrameMs; }
float Renderer::aspectRatio() const {
    const VkExtent2D e = m_impl->outputExtent();
    return e.height ? static_cast<float>(e.width) / static_cast<float>(e.height) : 1.0f;
}
const char* Renderer::gpuName() const { return m_impl->context->gpuName(); }
uint32_t Renderer::drawCount() const { return m_impl->lastDrawCount; }
bool Renderer::beginFrame() { return m_impl->beginFrame(); }
void Renderer::endFrame() { m_impl->endFrame(); }
void Renderer::onResize() { m_impl->needsResize = true; }
void Renderer::waitIdle() { vkDeviceWaitIdle(m_impl->context->device()); }
void Renderer::initGui() { m_impl->initGui(); }
void Renderer::shutdownGui() { m_impl->shutdownGui(); }
void Renderer::newGuiFrame() { ImGui_ImplVulkan_NewFrame(); }
void Renderer::renderGuiWindows() { m_impl->renderGuiWindows(); }

}   // namespace shockwave
