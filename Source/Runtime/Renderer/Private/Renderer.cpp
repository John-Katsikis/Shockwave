#include <Renderer/Renderer.h>

#include <Core/Log.h>
#include <Core/Plugin.h>
#include <Renderer/RenderBackend.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

namespace shockwave {

RenderViewId RenderViewId::create() {
    static std::atomic<uint64_t> next{1};
    return {next.fetch_add(1, std::memory_order_relaxed)};
}

const char* antiAliasingName(AntiAliasing aa) { return aa == AntiAliasing::Fxaa ? "fxaa" : "none"; }

const char* bloomQualityName(BloomQuality quality) {
    constexpr const char* names[] = {"off", "low", "medium", "high"};
    return names[std::min<uint32_t>(static_cast<uint32_t>(quality), 3)];
}

const char* gpuMemoryCategoryName(GpuMemoryCategory category) {
    switch (category) {
    case GpuMemoryCategory::RenderTargets: return "render targets";
    case GpuMemoryCategory::ShadowMaps: return "shadow maps";
    case GpuMemoryCategory::Environment: return "environment lighting";
    case GpuMemoryCategory::Bloom: return "bloom";
    case GpuMemoryCategory::AntiAliasing: return "anti-aliasing";
    case GpuMemoryCategory::Meshes: return "meshes";
    case GpuMemoryCategory::Textures: return "textures";
    case GpuMemoryCategory::FrameData: return "frame data";
    case GpuMemoryCategory::Particles: return "particles";
    case GpuMemoryCategory::GameUi: return "game ui";
    case GpuMemoryCategory::Readback: return "readback";
    case GpuMemoryCategory::Staging: return "staging";
    case GpuMemoryCategory::SurfaceBuffers: return "surface buffers";
    case GpuMemoryCategory::AmbientOcclusion: return "ambient occlusion";
    case GpuMemoryCategory::TemporalHistory: return "temporal history";
    case GpuMemoryCategory::Lightmaps: return "lightmaps";
    case GpuMemoryCategory::DiffuseProbes: return "diffuse probes";
    case GpuMemoryCategory::ReflectionProbes: return "reflection probes";
    case GpuMemoryCategory::Clusters: return "light clusters";
    case GpuMemoryCategory::Hzb: return "depth pyramid";
    case GpuMemoryCategory::Count: break;
    }
    return "?";
}

struct Renderer::State {
    Window& window;
    JobSystem& jobs;
    bool swappable = false;

    std::unique_ptr<Plugin> plugin;
    std::unique_ptr<RenderBackend> backend;
    std::string backendName;

    // Settings, replayed into a new backend after a swap.
    Color clearColor{0.02f, 0.02f, 0.03f, 1.0f};
    glm::mat4 view{1.0f}, projection{1.0f};
    RenderViewId viewId;
    GridSettings grid;
    LightingDesc lighting;
    DebugView debugView = DebugView::Lit;
    ShadowQuality shadowQuality = ShadowQuality::High;
    AntiAliasing antiAliasing = AntiAliasing::None;
    BloomQuality bloomQuality = BloomQuality::High;
    float bloomScale = 1.0f;
    LodSettings lodSettings;
    float renderScale = 1.0f;
    uint32_t viewportWidth = 0, viewportHeight = 0;
    bool vsync = true;
    bool lowLatency = false;
    bool gui = false;
    std::function<void()> restartGuiPlatform;

    // Resource ids are handed out here rather than by the backend, so they survive a swap.
    // Creating resources takes swapMutex shared (any number of threads at once); a swap
    // takes it exclusively, so nothing is half-created while backends change.
    std::shared_mutex swapMutex;
    std::mutex idMutex;
    std::vector<uint32_t> freeMeshes, freeTextures;
    uint32_t nextMesh = 0, nextTexture = 1;   // texture 0 is the backend's white texture
    // How many times each mesh slot has been freed, for RenderInstanceKey::meshGeneration.
    // Fixed size: drawMesh reads it from any thread while createMesh may run.
    static constexpr uint32_t kTrackedMeshes = 1u << 16;
    std::unique_ptr<std::atomic<uint32_t>[]> meshGenerations =
        std::make_unique<std::atomic<uint32_t>[]>(kTrackedMeshes);
    uint32_t meshGeneration(MeshHandle mesh) const {
        return mesh.id < kTrackedMeshes ? meshGenerations[mesh.id].load(std::memory_order_relaxed) : 0;
    }

    // CPU copies for replaying into a new backend (swappable only), ordered by id.
    struct TextureData {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;
        bool srgb = true;
    };
    std::mutex journalMutex;
    std::map<uint32_t, shockwave::TextureData> packedTextures;   // made from TextureData
    std::map<uint32_t, std::pair<std::vector<MeshLod>, std::vector<uint32_t>>> meshLods;
    std::map<uint32_t, MeshData> meshes;
    std::map<uint32_t, std::vector<VertexSkin>> skins;   // skinned meshes' weights
    std::map<uint32_t, TextureData> textures;

    State(Window& w, JobSystem& j) : window(w), jobs(j) {}

    uint32_t allocate(std::vector<uint32_t>& free, uint32_t& next, uint32_t maximum, const char* what) {
        std::lock_guard lock(idMutex);
        if (!free.empty()) {
            const uint32_t id = free.back();
            free.pop_back();
            return id;
        }
        if (next >= maximum) throw std::runtime_error(std::string("out of ") + what + " slots");
        return next++;
    }
    void release(std::vector<uint32_t>& free, uint32_t id) {
        std::lock_guard lock(idMutex);
        free.push_back(id);
    }

    // Creates the plugin's backend and brings it to the current state.
    void start(Plugin& from) {
        const auto& api = from.api<RendererPluginApi>();
        if (!api.create) throw std::runtime_error(std::string(from.desc().name) + " has no create function");
        backend = api.create(RenderBackendDesc{window, jobs, vsync});
        if (nextMesh > backend->maxMeshes() || nextTexture > backend->maxTextures())
            throw std::runtime_error(std::string(from.desc().name) + " can't hold this many meshes and textures");
        backend->setClearColor(clearColor);
        backend->setView(viewId);
        backend->setCamera(view, projection);
        backend->setGrid(grid);
        backend->setLighting(lighting);
        backend->setDebugView(debugView);
        backend->setShadowQuality(shadowQuality);
        backend->setAntiAliasing(antiAliasing);
        backend->setBloom(bloomQuality, bloomScale);
        backend->setRenderScale(renderScale);
        backend->setViewport(viewportWidth, viewportHeight);
        backend->setLowLatency(lowLatency);
        backend->setLodSettings(lodSettings);
        {
            std::lock_guard lock(journalMutex);
            for (const auto& [id, texture] : packedTextures) replayTexture({id}, texture);
            for (const auto& [id, texture] : textures)
                backend->createTexture({id}, texture.width, texture.height, texture.rgba, texture.srgb);
            for (const auto& [id, mesh] : meshes) backend->createMesh({id}, mesh.vertices, mesh.indices);
            for (const auto& [id, skin] : skins) backend->setMeshSkin({id}, skin);
        }
        {
            std::lock_guard lock(journalMutex);
            for (const auto& [id, lods] : meshLods) backend->setMeshLods({id}, lods.first, lods.second);
        }
        if (gui) {
            if (restartGuiPlatform) restartGuiPlatform();
            backend->initGui();
        }
    }

    // A new backend that can't sample a compressed texture's format shows it white.
    void replayTexture(TextureHandle id, const shockwave::TextureData& texture) {
        if (backend->supportsTextureFormat(texture.format)) {
            backend->createTextureData(id, texture);
            return;
        }
        SW_LOG(
            LogRender, Warning, "texture {} is {}, which this renderer can't use; showing white", id.id,
            textureFormatName(texture.format));
        const uint8_t white[4] = {255, 255, 255, 255};
        backend->createTexture(id, 1, 1, white, texture.srgb);
    }

    void stop() {
        if (!backend) return;
        backend->waitIdle();
        if (gui) backend->shutdownGui();
        backend.reset();
    }
};

Renderer::Renderer(Window& window, JobSystem& jobs, const RendererOptions& options)
    : m(std::make_unique<State>(window, jobs)) {
    m->swappable = options.swappable;
    m->vsync = options.vsync;
    std::string error;
    m->plugin = loadPlugin("renderer", options.backend, error);
    if (!m->plugin) throw std::runtime_error(error);
    m->backendName = options.backend;
    m->start(*m->plugin);
}

Renderer::~Renderer() {
    m->stop();   // before the plugin's code is unloaded
}

// ---- Resources ----

MeshHandle Renderer::createMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices) {
    if (vertices.empty() || indices.empty()) return {};
    std::shared_lock swap(m->swapMutex);
    const uint32_t id = m->allocate(m->freeMeshes, m->nextMesh, m->backend->maxMeshes(), "mesh");
    try {
        m->backend->createMesh({id}, vertices, indices);
    } catch (...) {
        m->release(m->freeMeshes, id);
        throw;
    }
    if (m->swappable) {
        std::lock_guard lock(m->journalMutex);
        m->meshes[id] = MeshData{{vertices.begin(), vertices.end()}, {indices.begin(), indices.end()}};
    }
    return {id};
}

MeshHandle Renderer::createSkinnedMesh(
    std::span<const Vertex> vertices, std::span<const uint32_t> indices, std::span<const VertexSkin> skin) {
    if (skin.size() != vertices.size())
        throw std::runtime_error("createSkinnedMesh: there must be one VertexSkin per vertex");
    const MeshHandle mesh = createMesh(vertices, indices);
    if (!mesh.valid()) return mesh;
    std::shared_lock swap(m->swapMutex);
    m->backend->setMeshSkin(mesh, skin);
    if (m->swappable) {
        std::lock_guard lock(m->journalMutex);
        m->skins[mesh.id] = {skin.begin(), skin.end()};
    }
    return mesh;
}

void Renderer::destroyMesh(MeshHandle mesh) {
    if (!mesh.valid()) return;
    std::shared_lock swap(m->swapMutex);
    m->backend->destroyMesh(mesh);
    if (m->swappable) {
        std::lock_guard lock(m->journalMutex);
        m->meshLods.erase(mesh.id);
        m->meshes.erase(mesh.id);
        m->skins.erase(mesh.id);
    }
    if (mesh.id < State::kTrackedMeshes) m->meshGenerations[mesh.id].fetch_add(1, std::memory_order_relaxed);
    m->release(m->freeMeshes, mesh.id);
}

TextureHandle Renderer::createTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) {
    if (!width || !height || rgba.size() < size_t(width) * height * 4)
        throw std::runtime_error("createTexture: pixel data is smaller than width x height x 4");
    std::shared_lock swap(m->swapMutex);
    const uint32_t id = m->allocate(m->freeTextures, m->nextTexture, m->backend->maxTextures(), "texture");
    try {
        m->backend->createTexture({id}, width, height, rgba, srgb);
    } catch (...) {
        m->release(m->freeTextures, id);
        throw;
    }
    if (m->swappable) {
        std::lock_guard lock(m->journalMutex);
        m->textures[id] = {width, height, {rgba.begin(), rgba.begin() + size_t(width) * height * 4}, srgb};
    }
    return {id};
}

void Renderer::updateTexture(
    TextureHandle texture, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) {
    if (!width || !height || rgba.size() < size_t(width) * height * 4)
        throw std::runtime_error("updateTexture: pixel data is smaller than width x height x 4");
    std::shared_lock swap(m->swapMutex);
    m->backend->updateTexture(texture, width, height, rgba, srgb);
    if (m->swappable) {
        std::lock_guard lock(m->journalMutex);
        m->textures[texture.id] = {width, height, {rgba.begin(), rgba.begin() + size_t(width) * height * 4}, srgb};
        m->packedTextures.erase(texture.id);
    }
}

void Renderer::destroyTexture(TextureHandle texture) {
    if (texture.id == 0) return;
    std::shared_lock swap(m->swapMutex);
    m->backend->destroyTexture(texture);
    if (m->swappable) {
        std::lock_guard lock(m->journalMutex);
        m->textures.erase(texture.id);
        m->packedTextures.erase(texture.id);
    }
    m->release(m->freeTextures, texture.id);
}

uint64_t Renderer::guiTexture(TextureHandle texture) { return m->backend->guiTexture(texture); }

// ---- Optimised assets ----

void Renderer::setMeshLods(MeshHandle mesh, std::span<const MeshLod> lods, std::span<const uint32_t> lodIndices) {
    if (!mesh.valid() || lods.size() > kMaxMeshLods)
        throw std::runtime_error("setMeshLods: invalid mesh or too many levels");
    for (const MeshLod& lod : lods)
        if (lod.indexCount == 0 || lod.indexCount % 3 || lod.firstIndex > lodIndices.size() ||
            lod.indexCount > lodIndices.size() - lod.firstIndex)
            throw std::runtime_error("setMeshLods: a level reaches past lodIndices");
    if (lods.empty()) return;
    std::shared_lock swap(m->swapMutex);
    m->backend->setMeshLods(mesh, lods, lodIndices);
    if (m->swappable) {
        std::lock_guard lock(m->journalMutex);
        m->meshLods[mesh.id] = {{lods.begin(), lods.end()}, {lodIndices.begin(), lodIndices.end()}};
    }
}

TextureHandle Renderer::createTexture(const TextureData& texture) {
    if (!texture.valid()) throw std::runtime_error("createTexture: the texture data is incomplete");
    std::shared_lock swap(m->swapMutex);
    if (!m->backend->supportsTextureFormat(texture.format))
        throw std::runtime_error(std::string("createTexture: this GPU can't use ") + textureFormatName(texture.format));
    const uint32_t id = m->allocate(m->freeTextures, m->nextTexture, m->backend->maxTextures(), "texture");
    try {
        m->backend->createTextureData({id}, texture);
    } catch (...) {
        m->release(m->freeTextures, id);
        throw;
    }
    if (m->swappable) {
        std::lock_guard lock(m->journalMutex);
        m->packedTextures[id] = texture;
    }
    return {id};
}

void Renderer::updateTexture(TextureHandle texture, const TextureData& data) {
    if (!data.valid()) throw std::runtime_error("updateTexture: the texture data is incomplete");
    std::shared_lock swap(m->swapMutex);
    if (!m->backend->supportsTextureFormat(data.format))
        throw std::runtime_error(std::string("updateTexture: this GPU can't use ") + textureFormatName(data.format));
    m->backend->updateTextureData(texture, data);
    if (m->swappable) {
        std::lock_guard lock(m->journalMutex);
        m->packedTextures[texture.id] = data;
        m->textures.erase(texture.id);
    }
}

bool Renderer::supportsTextureFormat(TextureFormat format) const {
    std::shared_lock swap(m->swapMutex);
    return m->backend->supportsTextureFormat(format);
}
void Renderer::setLodSettings(const LodSettings& settings) {
    m->lodSettings = settings;
    m->lodSettings.maxErrorPixels = std::max(settings.maxErrorPixels, 0.0f);
    m->backend->setLodSettings(m->lodSettings);
}
const LodSettings& Renderer::lodSettings() const { return m->lodSettings; }
LodStats Renderer::lodStats() const { return m->backend->lodStats(); }

// ---- Per frame ----

void Renderer::setClearColor(const Color& color) {
    m->clearColor = color;
    m->backend->setClearColor(color);
}
void Renderer::setCamera(const glm::mat4& view, const glm::mat4& projection, RenderViewId id) {
    m->view = view;
    m->projection = projection;
    m->viewId = id;
    m->backend->setView(id);
    m->backend->setCamera(view, projection);
}
RenderViewId Renderer::currentView() const { return m->viewId; }
const glm::mat4& Renderer::cameraView() const { return m->view; }
const glm::mat4& Renderer::cameraProjection() const { return m->projection; }
void Renderer::drawMesh(
    MeshHandle mesh, const glm::mat4& transform, TextureHandle texture, const glm::vec4& tint,
    const DrawMaterial& material, RenderInstanceKey key) {
    if (key.valid()) key.meshGeneration = m->meshGeneration(mesh);
    m->backend->drawMesh(mesh, transform, texture, tint, material, key);
}
void Renderer::drawSkinnedMesh(
    MeshHandle mesh, const glm::mat4& transform, std::span<const glm::mat4> joints, TextureHandle texture,
    const glm::vec4& tint, const DrawMaterial& material, RenderInstanceKey key) {
    if (key.valid()) key.meshGeneration = m->meshGeneration(mesh);
    m->backend->drawSkinnedMesh(mesh, transform, joints, texture, tint, material, key);
}
void Renderer::addLight(const LightDesc& light) { m->backend->addLight(light); }
void Renderer::drawParticles(const ParticleDrawList& particles) {
    if (!particles.batches.empty()) m->backend->drawParticles(particles);
}

// ---- Settings ----

void Renderer::setGrid(const GridSettings& grid) {
    m->grid = grid;
    m->backend->setGrid(grid);
}
void Renderer::setLighting(const LightingDesc& lighting) {
    m->lighting = lighting;
    m->backend->setLighting(lighting);
}
void Renderer::setDebugView(DebugView view) {
    m->debugView = view;
    m->backend->setDebugView(view);
}
DebugView Renderer::debugView() const { return m->debugView; }
void Renderer::setShadowQuality(ShadowQuality quality) {
    m->shadowQuality = quality;
    m->backend->setShadowQuality(quality);
}
ShadowQuality Renderer::shadowQuality() const { return m->shadowQuality; }
void Renderer::setAntiAliasing(AntiAliasing aa) {
    m->antiAliasing = aa;
    m->backend->setAntiAliasing(aa);
}
AntiAliasing Renderer::antiAliasing() const { return m->antiAliasing; }
void Renderer::setBloom(BloomQuality quality, float scale) {
    m->bloomQuality = quality;
    m->bloomScale = std::isfinite(scale) ? std::clamp(scale, 0.0f, 1.0f) : 1.0f;
    m->backend->setBloom(m->bloomQuality, m->bloomScale);
}
BloomQuality Renderer::bloomQuality() const { return m->bloomQuality; }
float Renderer::bloomScale() const { return m->bloomScale; }
void Renderer::setRenderScale(float scale) {
    m->renderScale = std::clamp(scale, 0.25f, 2.0f);
    m->backend->setRenderScale(m->renderScale);
}
float Renderer::renderScale() const { return m->renderScale; }
void Renderer::setViewport(uint32_t width, uint32_t height) {
    m->viewportWidth = width;
    m->viewportHeight = height;
    m->backend->setViewport(width, height);
}
void Renderer::setVsync(bool vsync) {
    m->vsync = vsync;
    m->backend->setVsync(vsync);
}
bool Renderer::vsync() const { return m->vsync; }
void Renderer::setLowLatency(bool lowLatency) {
    m->lowLatency = lowLatency;
    m->backend->setLowLatency(lowLatency);
}

// ---- Results ----

uint32_t Renderer::lightCount() const { return m->backend->lightCount(); }
uint32_t Renderer::droppedLights() const { return m->backend->droppedLights(); }
ShadowStats Renderer::shadowStats() const { return m->backend->shadowStats(); }
std::string Renderer::frameGraph() const { return m->backend->frameGraph(); }
uint32_t Renderer::renderWidth() const { return m->backend->renderWidth(); }
uint32_t Renderer::renderHeight() const { return m->backend->renderHeight(); }
uint64_t Renderer::sceneTexture() const { return m->backend->sceneTexture(); }
const std::vector<PassTiming>& Renderer::passTimings() const { return m->backend->passTimings(); }
float Renderer::gpuFrameMs() const { return m->backend->gpuFrameMs(); }
float Renderer::aspectRatio() const { return m->backend->aspectRatio(); }
const char* Renderer::gpuName() const { return m->backend->gpuName(); }
const GpuChoice* Renderer::gpuChoice() const { return m->backend->gpuChoice(); }
uint32_t Renderer::drawCount() const { return m->backend->drawCount(); }
uint32_t Renderer::culledDraws() const { return m->backend->culledDraws(); }
ParticleStats Renderer::particleStats() const { return m->backend->particleStats(); }
uint32_t Renderer::unkeyedDraws() const { return m->backend->unkeyedDraws(); }

RendererCapabilities Renderer::capabilities() const {
    auto caps = m->backend->capabilities();
    if (caps.backend.empty()) caps.backend = m->backendName;
    return caps;
}
std::vector<EffectiveSetting> Renderer::effectiveSettings() const { return m->backend->effectiveSettings(); }
RendererMemoryReport Renderer::memoryReport() const {
    RendererMemoryReport report = m->backend->memoryReport();
    std::lock_guard lock(m->journalMutex);
    for (const auto& [id, mesh] : m->meshes)
        report.cpuCopyBytes += mesh.vertices.size() * sizeof(Vertex) + mesh.indices.size() * sizeof(uint32_t);
    for (const auto& [id, lods] : m->meshLods) report.cpuCopyBytes += lods.second.size() * sizeof(uint32_t);
    for (const auto& [id, skin] : m->skins) report.cpuCopyBytes += skin.size() * sizeof(VertexSkin);
    for (const auto& [id, texture] : m->textures) report.cpuCopyBytes += texture.rgba.size();
    for (const auto& [id, texture] : m->packedTextures) report.cpuCopyBytes += texture.bytes.size();
    return report;
}
void Renderer::requestScreenshot(const std::string& path) { m->backend->requestScreenshot(path); }
bool Renderer::screenshotPending() const { return m->backend->screenshotPending(); }
const std::string& Renderer::screenshotResult() const { return m->backend->screenshotResult(); }
float Renderer::waitForPresent() { return m->backend->waitForPresent(); }
const char* Renderer::presentTiming() const { return m->backend->presentTiming(); }
const char* Renderer::presentModeName() const { return m->backend->presentModeName(); }

// ---- Engine calls ----

bool Renderer::beginFrame() { return m->backend->beginFrame(); }
void Renderer::endFrame() { m->backend->endFrame(); }
void Renderer::onResize() { m->backend->onResize(); }
void Renderer::waitIdle() { m->backend->waitIdle(); }
void Renderer::initGui(std::function<void()> restartPlatform) {
    m->gui = true;
    m->restartGuiPlatform = std::move(restartPlatform);
    m->backend->initGui();
}
void Renderer::shutdownGui() {
    if (!m->gui) return;
    m->backend->shutdownGui();
    m->gui = false;
    m->restartGuiPlatform = {};
}
void Renderer::newGuiFrame() { m->backend->newGuiFrame(); }
void Renderer::renderGuiWindows() { m->backend->renderGuiWindows(); }

// ---- Game UI overlay ----

glm::uvec2 Renderer::uiOutputSize() const { return m->backend->uiOutputSize(); }
void Renderer::drawUi(const UiDrawList& list) {
    if (!list.commands.empty()) m->backend->drawUi(list);
}

// ---- Backends ----

std::vector<std::string> Renderer::availableBackends() { return availablePlugins("renderer"); }
const std::string& Renderer::backendName() const { return m->backendName; }
const char* Renderer::backendDescription() const { return m->plugin->desc().description; }
uint32_t Renderer::backendFlags() const { return m->plugin->api<RendererPluginApi>().flags; }
bool Renderer::swappable() const { return m->swappable; }
const std::filesystem::path& Renderer::backendFile() const { return m->plugin->path(); }

bool Renderer::switchBackend(const std::string& name, std::string& error) {
    if (!m->swappable) {
        error = "this renderer was started without RendererOptions::swappable";
        return false;
    }
    std::unique_lock swap(m->swapMutex);
    std::unique_ptr<Plugin> next = loadPlugin("renderer", name, error);
    if (!next) return false;
    if (m->gui && !(next->api<RendererPluginApi>().flags & kRendererDrawsGui)) {
        error = "the " + name + " renderer can't draw the editor's interface";
        return false;
    }
    m->stop();
    try {
        m->start(*next);
    } catch (const std::exception& e) {
        error = "the " + name + " renderer couldn't start: " + e.what();
        m->backend.reset();
        // Back to the one that was running. If even that fails there's nothing to draw with.
        try {
            m->start(*m->plugin);
        } catch (const std::exception& again) {
            throw std::runtime_error(error + "; restarting " + m->backendName + " failed too: " + again.what());
        }
        return false;
    }
    m->plugin = std::move(next);   // unloads the old library now that nothing uses it
    m->backendName = name;
    SW_LOG(LogRender, Info, "renderer is now {} ({})", name, m->plugin->desc().description);
    return true;
}

bool Renderer::reloadBackend(std::string& error) {
    const std::string name = m->backendName;
    return switchBackend(name, error);
}

}   // namespace shockwave
