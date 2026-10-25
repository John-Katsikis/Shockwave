#include "RendererImpl.h"

#include <Core/Plugin.h>

namespace shockwave {

VulkanRenderer::VulkanRenderer(Window& window, JobSystem& jobs, bool vsync)
    : m_impl(std::make_unique<Impl>(window, jobs, vsync)) {}
VulkanRenderer::~VulkanRenderer() = default;

uint32_t VulkanRenderer::maxMeshes() const { return kMaxMeshes; }
uint32_t VulkanRenderer::maxTextures() const { return m_impl->textureCapacity(); }

void VulkanRenderer::destroyMesh(MeshHandle handle) { m_impl->destroyMesh(handle); }
void VulkanRenderer::destroyTexture(TextureHandle handle) { m_impl->destroyTexture(handle); }

void VulkanRenderer::createMesh(MeshHandle id, std::span<const Vertex> vertices, std::span<const uint32_t> indices) {
    m_impl->createMesh(id, vertices, indices);
}
void VulkanRenderer::createTexture(
    TextureHandle id, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) {
    m_impl->createTexture(id, width, height, rgba, srgb);
}

void VulkanRenderer::updateTexture(
    TextureHandle texture, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) {
    m_impl->updateTexture(texture, width, height, rgba, srgb);
}

uint64_t VulkanRenderer::guiTexture(TextureHandle texture) { return m_impl->guiTexture(texture); }

void VulkanRenderer::setMeshLods(MeshHandle id, std::span<const MeshLod> lods, std::span<const uint32_t> lodIndices) {
    m_impl->setMeshLods(id, lods, lodIndices);
}
bool VulkanRenderer::supportsTextureFormat(TextureFormat format) const { return m_impl->supportsTextureFormat(format); }
void VulkanRenderer::createTextureData(TextureHandle id, const TextureData& data) {
    m_impl->createTextureData(id, data);
}
void VulkanRenderer::updateTextureData(TextureHandle id, const TextureData& data) {
    m_impl->updateTextureData(id, data);
}
void VulkanRenderer::setLodSettings(const LodSettings& settings) { m_impl->lodSettings = settings; }
LodStats VulkanRenderer::lodStats() const { return m_impl->lastLodStats; }
void VulkanRenderer::setClearColor(const Color& color) { m_impl->clearColor = color; }
void VulkanRenderer::setCamera(const glm::mat4& view, const glm::mat4& projection) {
    m_impl->viewProjection = projection * view;
    m_impl->viewFrustum = Frustum(m_impl->viewProjection);
    m_impl->view = view;
    m_impl->projection = projection;
    m_impl->cameraPosition = glm::vec3(glm::inverse(view)[3]);
}
void VulkanRenderer::setGrid(const GridSettings& grid) { m_impl->grid = grid; }
void VulkanRenderer::drawParticles(const ParticleDrawList& particles) { m_impl->drawParticles(particles); }

void VulkanRenderer::addLight(const LightDesc& light) {
    std::lock_guard lock(m_impl->lightMutex);
    m_impl->lights.push_back(light);
}
void VulkanRenderer::setLighting(const LightingDesc& lighting) { m_impl->lighting = lighting; }
void VulkanRenderer::setDebugView(DebugView view) { m_impl->debugView = view; }
uint32_t VulkanRenderer::lightCount() const { return m_impl->lastLightCount; }
uint32_t VulkanRenderer::droppedLights() const { return m_impl->lastDroppedLights; }
void VulkanRenderer::setShadowQuality(ShadowQuality quality) {
    if (quality == m_impl->shadowQuality) return;
    m_impl->shadowQuality = quality;
    m_impl->needsShadowRebuild = true;
}
ShadowStats VulkanRenderer::shadowStats() const { return m_impl->shadowStats; }
void VulkanRenderer::requestScreenshot(const std::string& path) {
    m_impl->screenshotPath = path;
    m_impl->screenshotRequested = true;
    m_impl->screenshotMessage.clear();
}
bool VulkanRenderer::screenshotPending() const { return m_impl->screenshotRequested; }
const std::string& VulkanRenderer::screenshotResult() const { return m_impl->screenshotMessage; }
std::string VulkanRenderer::frameGraph() const { return m_impl->frameGraphText; }
void VulkanRenderer::drawMesh(
    MeshHandle mesh, const glm::mat4& transform, TextureHandle texture, const glm::vec4& tint,
    const DrawMaterial& material) {
    m_impl->drawMesh(mesh, transform, texture, tint, material);
}
void VulkanRenderer::setMeshSkin(MeshHandle id, std::span<const VertexSkin> skin) { m_impl->setMeshSkin(id, skin); }
void VulkanRenderer::drawSkinnedMesh(
    MeshHandle mesh, const glm::mat4& transform, std::span<const glm::mat4> joints, TextureHandle texture,
    const glm::vec4& tint, const DrawMaterial& material) {
    m_impl->drawSkinnedMesh(mesh, transform, joints, texture, tint, material);
}
void VulkanRenderer::setRenderScale(float scale) {
    if (scale != m_impl->renderScale) {
        m_impl->renderScale = scale;
        m_impl->needsResize = true;
    }
}
uint32_t VulkanRenderer::renderWidth() const { return m_impl->sceneColor.extent.width; }
uint32_t VulkanRenderer::renderHeight() const { return m_impl->sceneColor.extent.height; }
void VulkanRenderer::setViewport(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) width = height = 0;
    if (width != m_impl->viewportSize.width || height != m_impl->viewportSize.height) {
        m_impl->viewportSize = {width, height};
        m_impl->needsSceneResize = true;
    }
}
uint64_t VulkanRenderer::sceneTexture() const { return reinterpret_cast<uint64_t>(m_impl->guiSceneTexture); }
void VulkanRenderer::setVsync(bool vsync) {
    if (vsync != m_impl->swapchain->vsync()) {
        m_impl->swapchain->setVsync(vsync);
        m_impl->needsResize = true;
    }
}
void VulkanRenderer::setLowLatency(bool lowLatency) {
    m_impl->swapchain->setLowLatency(lowLatency);
    // Only the vsync present mode depends on it.
    if (m_impl->swapchain->vsync() && m_impl->context->fifoLatestReady()) m_impl->needsResize = true;
}
float VulkanRenderer::waitForPresent() { return m_impl->waitForPresent(); }
const char* VulkanRenderer::presentTiming() const { return m_impl->context->presentTimingName(); }
const char* VulkanRenderer::presentModeName() const {
    switch (m_impl->swapchain->presentMode()) {
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (vsync)";
    case VK_PRESENT_MODE_FIFO_LATEST_READY_KHR: return "FIFO latest ready (vsync, low latency)";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO relaxed";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox (uncapped, no tearing)";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate (uncapped, may tear)";
    default: return "other";
    }
}
const std::vector<PassTiming>& VulkanRenderer::passTimings() const { return m_impl->passTimings; }
float VulkanRenderer::gpuFrameMs() const { return m_impl->gpuFrameMs; }
float VulkanRenderer::aspectRatio() const {
    const VkExtent2D e = m_impl->outputExtent();
    return e.height ? static_cast<float>(e.width) / static_cast<float>(e.height) : 1.0f;
}
const char* VulkanRenderer::gpuName() const { return m_impl->context->gpuName(); }
const GpuChoice* VulkanRenderer::gpuChoice() const { return &m_impl->context->gpuChoice(); }
uint32_t VulkanRenderer::drawCount() const { return m_impl->lastDrawCount; }
ParticleStats VulkanRenderer::particleStats() const { return m_impl->lastParticleStats; }
uint32_t VulkanRenderer::culledDraws() const { return m_impl->lastCulledDraws; }
bool VulkanRenderer::beginFrame() { return m_impl->beginFrame(); }
void VulkanRenderer::endFrame() { m_impl->endFrame(); }
void VulkanRenderer::onResize() { m_impl->needsResize = true; }
void VulkanRenderer::waitIdle() { vkDeviceWaitIdle(m_impl->context->device()); }
void VulkanRenderer::initGui() { m_impl->initGui(); }
void VulkanRenderer::shutdownGui() { m_impl->shutdownGui(); }
void VulkanRenderer::newGuiFrame() { ImGui_ImplVulkan_NewFrame(); }
void VulkanRenderer::renderGuiWindows() { m_impl->renderGuiWindows(); }
glm::uvec2 VulkanRenderer::uiOutputSize() const {
    const VkExtent2D e = m_impl->outputExtent();
    return {e.width, e.height};
}
void VulkanRenderer::drawUi(const UiDrawList& list) { m_impl->drawUi(list); }

namespace {
std::unique_ptr<RenderBackend> create(const RenderBackendDesc& desc) {
    return std::make_unique<VulkanRenderer>(desc.window, desc.jobs, desc.vsync);
}
const RendererPluginApi api{.flags = kRendererDrawsGui | kRendererPresents, .create = create};
const PluginDesc plugin{
    .kind = "renderer",
    .name = "vulkan",
    .description = "Vulkan 1.3: forward PBR, cascaded shadows, render graph",
    .api = &api,
};
}   // namespace

}   // namespace shockwave

SHOCKWAVE_PLUGIN(shockwave::plugin)
