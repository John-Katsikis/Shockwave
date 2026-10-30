#include <Renderer/Renderer.h>

#include <Core/Jobs.h>
#include <Core/Log.h>
#include <Core/Plugin.h>
#include <Renderer/RenderBackend.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <iterator>
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

const char* ambientOcclusionName(AmbientOcclusion quality) {
    constexpr const char* names[] = {"off", "low", "medium", "high"};
    return names[std::min<uint32_t>(static_cast<uint32_t>(quality), 3)];
}
uint32_t ambientOcclusionSamples(AmbientOcclusion quality) {
    constexpr uint32_t samples[] = {0, 8, 16, 32};
    return samples[std::min<uint32_t>(static_cast<uint32_t>(quality), 3)];
}

const char* textureWrapName(TextureWrap wrap) {
    constexpr const char* names[] = {"repeat", "clamp", "mirror"};
    return names[std::min<uint32_t>(static_cast<uint32_t>(wrap), 2)];
}
const char* textureFilterName(TextureFilter filter) { return filter == TextureFilter::Nearest ? "nearest" : "linear"; }
const char* materialSlotName(MaterialSlot slot) {
    constexpr const char* names[] = {
        "base_color",   "normal",     "metallic_roughness", "emissive",         "occlusion",    "detail_color",
        "detail_normal", "detail_mask", "layer_mask",        "layer_base_color", "layer_normal", "layer_metallic_roughness"};
    static_assert(std::size(names) == kAllMaterialSlots);
    return names[std::min<uint32_t>(static_cast<uint32_t>(slot), kAllMaterialSlots - 1)];
}
const char* materialQualityName(MaterialQuality quality) { return quality == MaterialQuality::Low ? "low" : "high"; }
glm::vec2 UvTransform::apply(glm::vec2 uv) const {
    const glm::vec2 s = scale * uv;
    if (rotation == 0.0f) return offset + s;
    const float c = std::cos(rotation), n = std::sin(rotation);
    return offset + glm::vec2(c * s.x + n * s.y, -n * s.x + c * s.y);
}
const char* textureQualityName(TextureQuality quality) {
    constexpr const char* names[] = {"low", "medium", "high", "ultra"};
    return names[std::min<uint32_t>(static_cast<uint32_t>(quality), 3)];
}
uint32_t textureQualityFirstMip(TextureQuality quality) {
    return 3 - std::min<uint32_t>(static_cast<uint32_t>(quality), 3);
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
    AmbientOcclusion ambientOcclusion = AmbientOcclusion::Off;
    BloomQuality bloomQuality = BloomQuality::High;
    float bloomScale = 1.0f;
    LodSettings lodSettings;
    uint32_t anisotropy = 16;
    MaterialQuality materialQuality = MaterialQuality::High;
    std::atomic<bool> bakedLighting{true};
    bool localReflections = true;
    std::vector<ReflectionProbeDesc> reflectionProbes;   // after the textures they name, on a swap
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
    std::mutex journalMutex;
    std::map<uint32_t, std::pair<std::vector<MeshLod>, std::vector<uint32_t>>> meshLods;
    std::map<uint32_t, MeshData> meshes;
    std::map<uint32_t, std::vector<VertexSkin>> skins;   // skinned meshes' weights

    // ---- Samplers (G02): one per distinct desc; 0 is SamplerDesc{} ----
    mutable std::mutex samplerMutex;
    std::vector<SamplerDesc> samplers{SamplerDesc{}};
    bool samplersFull = false;

    // ---- Textures and their resident levels (A01) ----
    // Every texture has a record. Its source (all its pixels, as given) is kept for material
    // textures, whose levels texture_quality changes, and for every texture when swappable.
    // Lock order: swapMutex (shared), then a record's mutex, then residencyMutex.
    struct TextureRecord {
        std::mutex mutex;   // one change of its levels at a time
        std::shared_ptr<const TextureData> source;
        TextureUsage usage = TextureUsage::Fixed;
        TextureFormat format = TextureFormat::RGBA8;
        uint32_t width = 0, height = 0, levels = 1;   // the source's
        uint32_t resident = 0;   // the source level the GPU's level 0 is
        uint64_t residentBytes = 0;
        bool alive = true;
        // Under residencyMutex:
        bool queued = false, busy = false;
        uint32_t failed = UINT32_MAX;   // a level that couldn't be made; not retried
    };
    mutable std::mutex residencyMutex;
    std::map<uint32_t, std::shared_ptr<TextureRecord>> records;
    std::deque<uint32_t> residencyQueue;
    std::atomic<uint32_t> textureQuality{uint32_t(TextureQuality::Ultra)};
    JobCounter residencyJobs;
    // Changes started per frame, and the most at once: each holds a source-sized tail.
    static constexpr uint32_t kResidencyStartsPerFrame = 8, kResidencyInFlight = 16;
    uint32_t residencyBusy = 0;

    State(Window& w, JobSystem& j) : window(w), jobs(j) {}

    // The level a record should start at, at the current quality.
    uint32_t wantedFirstMip(const TextureRecord& r) const {
        if (r.usage != TextureUsage::Material || !r.source ||
            (r.format == TextureFormat::RGBA16F && r.source->levels.size() == 1))
            return 0;
        const auto quality = static_cast<TextureQuality>(textureQuality.load(std::memory_order_relaxed));
        return residentFirstMip(textureQualityFirstMip(quality), r.width, r.height, r.levels);
    }
    std::shared_ptr<TextureRecord> record(uint32_t id) const {
        std::lock_guard lock(residencyMutex);
        const auto it = records.find(id);
        return it == records.end() ? nullptr : it->second;
    }
    // Queues a record whose levels don't match the quality. Holds residencyMutex.
    void requeueLocked(uint32_t id, TextureRecord& r) {
        const uint32_t wanted = wantedFirstMip(r);
        if (r.alive && !r.queued && !r.busy && wanted != r.resident && wanted != r.failed) {
            r.queued = true;
            residencyQueue.push_back(id);
        }
    }

    // Sends a texture's levels from first down to the backend, as a new texture or in
    // place of the old. A lone RGBA8 level goes the way it always has (its mipmaps made on
    // the GPU); a format the backend can't sample shows white, with a warning.
    void uploadTexture(TextureHandle id, const TextureData& source, uint32_t first, bool create, uint64_t& bytes) {
        const TextureData tail = textureTail(source, first);
        bytes = tail.gpuBytes();
        if (tail.shape != TextureShape::Texture2D && !backend->supportsTextureShape(tail.shape)) {
            // A swap to a backend without cube maps: a placeholder keeps the id, and that
            // backend draws no local reflections anyway.
            SW_LOG(
                LogRender, Warning, "texture {} is a {} map, which this renderer can't use", id.id,
                textureShapeName(tail.shape));
            const uint8_t white[4] = {255, 255, 255, 255};
            if (create)
                backend->createTexture(id, 1, 1, white, false);
            else
                backend->updateTexture(id, 1, 1, white, false);
            bytes = 4;
            return;
        }
        if (tail.shape == TextureShape::Texture2D && tail.format == TextureFormat::RGBA8 && tail.levels.size() == 1) {
            const std::span<const uint8_t> rgba(tail.bytes.data() + tail.levels[0].offset, tail.levels[0].size);
            if (create)
                backend->createTexture(id, tail.width, tail.height, rgba, tail.srgb);
            else
                backend->updateTexture(id, tail.width, tail.height, rgba, tail.srgb);
            return;
        }
        if (backend->supportsTextureFormat(tail.format)) {
            if (create)
                backend->createTextureData(id, tail);
            else
                backend->updateTextureData(id, tail);
            return;
        }
        SW_LOG(
            LogRender, Warning, "texture {} is {}, which this renderer can't use; showing white", id.id,
            textureFormatName(tail.format));
        const uint8_t white[4] = {255, 255, 255, 255};
        if (create)
            backend->createTexture(id, 1, 1, white, tail.srgb);
        else
            backend->updateTexture(id, 1, 1, white, tail.srgb);
        bytes = 4;
    }

    // Main thread, each frame: starts a few level changes on workers. Each makes its tail,
    // checks it's still wanted, and hands it to the backend, which swaps it in at a frame
    // boundary and frees the old image once no frame uses it.
    void pumpResidency() {
        std::vector<std::pair<uint32_t, std::shared_ptr<TextureRecord>>> start;
        {
            std::lock_guard lock(residencyMutex);
            while (!residencyQueue.empty() && start.size() < kResidencyStartsPerFrame &&
                   residencyBusy < kResidencyInFlight) {
                const uint32_t id = residencyQueue.front();
                residencyQueue.pop_front();
                const auto it = records.find(id);
                if (it == records.end()) continue;
                TextureRecord& r = *it->second;
                r.queued = false;
                if (!r.alive || r.busy || wantedFirstMip(r) == r.resident) continue;
                r.busy = true;
                ++residencyBusy;
                start.emplace_back(id, it->second);
            }
        }
        for (auto& [id, r] : start) jobs.run([this, id = id, r = r] { changeLevels(id, *r); }, &residencyJobs);
    }
    void changeLevels(uint32_t id, TextureRecord& r) {
        {
            std::shared_lock swap(swapMutex);
            std::lock_guard levels(r.mutex);
            const uint32_t wanted = wantedFirstMip(r);
            if (r.alive && r.source && wanted != r.resident) {
                try {
                    uint64_t bytes = 0;
                    // Made from this source, at this level: if either changed meanwhile (an
                    // update, the quality again) it's dropped before it reaches the GPU.
                    uploadTexture({id}, *r.source, wanted, false, bytes);
                    std::lock_guard lock(residencyMutex);
                    r.resident = wanted;
                    r.residentBytes = bytes;
                } catch (const std::exception& e) {
                    // The old image stays; this level isn't tried again until the quality changes.
                    SW_LOG(LogRender, Warning, "texture {} couldn't change to level {}: {}", id, wanted, e.what());
                    std::lock_guard lock(residencyMutex);
                    r.failed = wanted;
                }
            }
        }
        std::lock_guard lock(residencyMutex);
        r.busy = false;
        --residencyBusy;
        requeueLocked(id, r);
    }

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
        backend->setAmbientOcclusion(ambientOcclusion);
        backend->setBloom(bloomQuality, bloomScale);
        backend->setRenderScale(renderScale);
        backend->setViewport(viewportWidth, viewportHeight);
        backend->setLowLatency(lowLatency);
        backend->setLodSettings(lodSettings);
        backend->setAnisotropy(anisotropy);
        backend->setMaterialQuality(materialQuality);
        {
            std::lock_guard lock(samplerMutex);
            for (uint32_t id = 1; id < samplers.size(); ++id) backend->createSampler({id}, samplers[id]);
        }
        {
            // swapMutex is held exclusively: no level change is part way through.
            std::lock_guard lock(residencyMutex);
            for (const auto& [id, r] : records) {
                if (!r->source) continue;
                uint64_t bytes = 0;
                uploadTexture({id}, *r->source, r->resident, true, bytes);
                r->residentBytes = bytes;
            }
        }
        {
            std::lock_guard lock(journalMutex);
            for (const auto& [id, mesh] : meshes) {
                backend->createMesh({id}, mesh.vertices, mesh.indices);
                if (!mesh.uv1.empty()) backend->setMeshUv1({id}, mesh.uv1);
                if (!mesh.lightmapUv.empty()) backend->setMeshLightmapUv({id}, mesh.lightmapUv);
            }
            for (const auto& [id, skin] : skins) backend->setMeshSkin({id}, skin);
        }
        {
            std::lock_guard lock(journalMutex);
            for (const auto& [id, lods] : meshLods) backend->setMeshLods({id}, lods.first, lods.second);
        }
        backend->setLocalReflections(localReflections);
        backend->setReflectionProbes(reflectionProbes);
        if (gui) {
            if (restartGuiPlatform) restartGuiPlatform();
            backend->initGui();
        }
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
    m->jobs.wait(m->residencyJobs);   // level changes use the backend
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

MeshHandle Renderer::createMesh(const MeshData& mesh) {
    const MeshHandle handle = createMesh(mesh.vertices, mesh.indices);
    if (handle.valid() && !mesh.uv1.empty()) setMeshUv1(handle, mesh.uv1);
    if (handle.valid() && !mesh.lightmapUv.empty()) setMeshLightmapUv(handle, mesh.lightmapUv);
    return handle;
}

void Renderer::setMeshLightmapUv(MeshHandle mesh, std::span<const glm::vec2> uv) {
    if (!mesh.valid() || uv.empty()) return;
    std::shared_lock swap(m->swapMutex);
    m->backend->setMeshLightmapUv(mesh, uv);
    if (m->swappable) {
        std::lock_guard lock(m->journalMutex);
        if (auto it = m->meshes.find(mesh.id); it != m->meshes.end())
            it->second.lightmapUv.assign(uv.begin(), uv.end());
    }
}

void Renderer::setMeshUv1(MeshHandle mesh, std::span<const glm::vec2> uv1) {
    if (!mesh.valid() || uv1.empty()) return;
    std::shared_lock swap(m->swapMutex);
    m->backend->setMeshUv1(mesh, uv1);
    if (m->swappable) {
        std::lock_guard lock(m->journalMutex);
        if (auto it = m->meshes.find(mesh.id); it != m->meshes.end()) it->second.uv1.assign(uv1.begin(), uv1.end());
    }
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

namespace {
// One RGBA8 level as TextureData, which is how the renderer keeps every source.
std::shared_ptr<const TextureData>
rgbaSource(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) {
    auto data = std::make_shared<TextureData>();
    data->format = TextureFormat::RGBA8;
    data->srgb = srgb;
    data->width = width;
    data->height = height;
    data->bytes.assign(rgba.begin(), rgba.begin() + ptrdiff_t(size_t(width) * height * 4));
    data->levels = {{width, height, 0, data->bytes.size()}};
    return data;
}
}   // namespace

// Makes or replaces a texture from source, at the level texture_quality wants for it.
// make(first) sends it to the backend; source is only kept when it will be needed again.
TextureHandle
Renderer::createTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb, TextureUsage usage) {
    if (!width || !height || rgba.size() < size_t(width) * height * 4)
        throw std::runtime_error("createTexture: pixel data is smaller than width x height x 4");
    std::shared_lock swap(m->swapMutex);
    const uint32_t id = m->allocate(m->freeTextures, m->nextTexture, m->backend->maxTextures(), "texture");
    auto r = std::make_shared<State::TextureRecord>();
    r->usage = usage;
    r->format = TextureFormat::RGBA8;
    r->width = width;
    r->height = height;
    r->levels = textureMipCount(width, height);
    if (usage == TextureUsage::Material || m->swappable) r->source = rgbaSource(width, height, rgba, srgb);
    try {
        const uint32_t first = m->wantedFirstMip(*r);
        if (first == 0) {
            m->backend->createTexture({id}, width, height, rgba, srgb);
            r->residentBytes = textureChainBytes(TextureFormat::RGBA8, width, height);
        } else {
            m->uploadTexture({id}, *r->source, first, true, r->residentBytes);
        }
        r->resident = first;
    } catch (...) {
        m->release(m->freeTextures, id);
        throw;
    }
    std::lock_guard lock(m->residencyMutex);
    m->records[id] = r;
    m->requeueLocked(id, *r);   // the quality changed while it was being made
    return {id};
}

void Renderer::updateTexture(
    TextureHandle texture, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb,
    std::optional<TextureUsage> usage) {
    if (!width || !height || rgba.size() < size_t(width) * height * 4)
        throw std::runtime_error("updateTexture: pixel data is smaller than width x height x 4");
    std::shared_lock swap(m->swapMutex);
    const auto r = m->record(texture.id);
    if (!r) {
        m->backend->updateTexture(texture, width, height, rgba, srgb);
        return;
    }
    // r->mutex keeps level changes out until this is done; residencyMutex guards the
    // fields that stats and the queue read.
    std::lock_guard levels(r->mutex);
    const auto kept = usage.value_or(r->usage);
    auto source = kept == TextureUsage::Material || m->swappable ? rgbaSource(width, height, rgba, srgb) : nullptr;
    uint32_t first = 0;
    {
        std::lock_guard lock(m->residencyMutex);
        r->usage = kept;
        r->format = TextureFormat::RGBA8;
        r->width = width;
        r->height = height;
        r->levels = textureMipCount(width, height);
        r->source = std::move(source);
        first = m->wantedFirstMip(*r);
    }
    uint64_t bytes = 0;
    if (first == 0) {
        m->backend->updateTexture(texture, width, height, rgba, srgb);
        bytes = textureChainBytes(TextureFormat::RGBA8, width, height);
    } else {
        m->uploadTexture(texture, *r->source, first, false, bytes);
    }
    std::lock_guard lock(m->residencyMutex);
    r->resident = first;
    r->residentBytes = bytes;
    r->failed = UINT32_MAX;
    m->requeueLocked(texture.id, *r);
}

void Renderer::destroyTexture(TextureHandle texture) {
    if (texture.id == 0) return;
    std::shared_lock swap(m->swapMutex);
    if (const auto r = m->record(texture.id)) {
        std::lock_guard levels(r->mutex);   // after any level change part way through
        std::lock_guard lock(m->residencyMutex);
        r->alive = false;
        r->source.reset();
    }
    m->backend->destroyTexture(texture);
    {
        std::lock_guard lock(m->residencyMutex);
        m->records.erase(texture.id);
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

TextureHandle Renderer::createTexture(const TextureData& texture, TextureUsage usage) {
    if (!texture.valid()) throw std::runtime_error("createTexture: the texture data is incomplete");
    std::shared_lock swap(m->swapMutex);
    if (!m->backend->supportsTextureFormat(texture.format))
        throw std::runtime_error(std::string("createTexture: this GPU can't use ") + textureFormatName(texture.format));
    if (!m->backend->supportsTextureShape(texture.shape))
        throw std::runtime_error(
            std::string("createTexture: this renderer can't use ") + textureShapeName(texture.shape) + " textures");
    if (texture.shape != TextureShape::Texture2D) usage = TextureUsage::Fixed;   // every level is needed
    const uint32_t id = m->allocate(m->freeTextures, m->nextTexture, m->backend->maxTextures(), "texture");
    auto r = std::make_shared<State::TextureRecord>();
    r->usage = usage;
    r->format = texture.format;
    r->width = texture.width;
    r->height = texture.height;
    r->levels =
        texture.levels.size() > 1 ? uint32_t(texture.levels.size()) : textureMipCount(texture.width, texture.height);
    auto source = std::make_shared<const TextureData>(texture);
    if (usage == TextureUsage::Material || m->swappable) r->source = source;
    try {
        const uint32_t first = m->wantedFirstMip(*r);
        m->uploadTexture({id}, *source, first, true, r->residentBytes);
        r->resident = first;
    } catch (...) {
        m->release(m->freeTextures, id);
        throw;
    }
    std::lock_guard lock(m->residencyMutex);
    m->records[id] = r;
    m->requeueLocked(id, *r);
    return {id};
}

void Renderer::updateTexture(TextureHandle texture, const TextureData& data, std::optional<TextureUsage> usage) {
    if (!data.valid()) throw std::runtime_error("updateTexture: the texture data is incomplete");
    std::shared_lock swap(m->swapMutex);
    if (!m->backend->supportsTextureFormat(data.format))
        throw std::runtime_error(std::string("updateTexture: this GPU can't use ") + textureFormatName(data.format));
    if (data.shape != TextureShape::Texture2D)
        throw std::runtime_error("updateTexture: cube textures are replaced by making a new one, not updated");
    const auto r = m->record(texture.id);
    if (!r) {
        m->backend->updateTextureData(texture, data);
        return;
    }
    std::lock_guard levels(r->mutex);
    auto source = std::make_shared<const TextureData>(data);
    uint32_t first = 0;
    {
        std::lock_guard lock(m->residencyMutex);
        if (usage) r->usage = *usage;
        r->format = data.format;
        r->width = data.width;
        r->height = data.height;
        r->levels = data.levels.size() > 1 ? uint32_t(data.levels.size()) : textureMipCount(data.width, data.height);
        r->source = r->usage == TextureUsage::Material || m->swappable ? source : nullptr;
        first = r->source ? m->wantedFirstMip(*r) : 0;
    }
    uint64_t bytes = 0;
    m->uploadTexture(texture, *source, first, false, bytes);
    std::lock_guard lock(m->residencyMutex);
    r->resident = first;
    r->residentBytes = bytes;
    r->failed = UINT32_MAX;
    m->requeueLocked(texture.id, *r);
}

// ---- Material bindings (G02) ----

SamplerHandle Renderer::sampler(const SamplerDesc& wanted) {
    SamplerDesc desc = wanted;
    desc.maxAnisotropy = std::clamp<uint8_t>(desc.maxAnisotropy, 1, 16);
    if (!desc.mipmaps) desc.mipFilter = TextureFilter::Linear;   // unused, so one desc either way
    std::shared_lock swap(m->swapMutex);
    std::lock_guard lock(m->samplerMutex);
    for (uint32_t id = 0; id < m->samplers.size(); ++id)
        if (m->samplers[id] == desc) return {id};
    if (m->samplers.size() >= kMaxSamplers) {
        if (!m->samplersFull)
            SW_LOG(LogRender, Warning, "more than {} different samplers; the rest use the default", kMaxSamplers);
        m->samplersFull = true;
        return {};
    }
    const SamplerHandle handle{uint32_t(m->samplers.size())};
    m->backend->createSampler(handle, desc);
    m->samplers.push_back(desc);
    return handle;
}
SamplerDesc Renderer::samplerDesc(SamplerHandle sampler) const {
    std::lock_guard lock(m->samplerMutex);
    return sampler.id < m->samplers.size() ? m->samplers[sampler.id] : SamplerDesc{};
}
void Renderer::setAnisotropy(uint32_t anisotropy) {
    m->anisotropy = std::clamp(anisotropy, 1u, 16u);
    m->backend->setAnisotropy(m->anisotropy);
}
uint32_t Renderer::anisotropy() const { return m->anisotropy; }
void Renderer::setMaterialQuality(MaterialQuality quality) {
    m->materialQuality = quality;
    m->backend->setMaterialQuality(quality);
}
MaterialQuality Renderer::materialQuality() const { return m->materialQuality; }
void Renderer::setBakedLighting(bool enabled) { m->bakedLighting.store(enabled, std::memory_order_relaxed); }
bool Renderer::bakedLighting() const { return m->bakedLighting.load(std::memory_order_relaxed); }

// ---- Texture quality (A01) ----

void Renderer::setTextureQuality(TextureQuality quality) {
    std::lock_guard lock(m->residencyMutex);
    if (m->textureQuality.exchange(uint32_t(quality)) == uint32_t(quality)) return;
    for (auto& [id, r] : m->records) {
        r->failed = UINT32_MAX;
        m->requeueLocked(id, *r);
    }
}
TextureQuality Renderer::textureQuality() const {
    return static_cast<TextureQuality>(m->textureQuality.load(std::memory_order_relaxed));
}
TextureResidencyStats Renderer::textureResidency() const {
    TextureResidencyStats stats;
    std::lock_guard lock(m->residencyMutex);
    for (const auto& [id, r] : m->records) {
        if (r->usage != TextureUsage::Material) continue;
        ++stats.textures;
        stats.reduced += r->resident > 0;
        stats.pending +=
            r->queued || r->busy || (m->wantedFirstMip(*r) != r->resident && m->wantedFirstMip(*r) != r->failed);
        stats.residentBytes += r->residentBytes;
        if (r->source) {
            stats.fullBytes += r->source->levels.size() > 1 ? r->source->gpuBytes()
                                                            : textureChainBytes(r->format, r->width, r->height);
            stats.retainedBytes += r->source->bytes.size();
        }
    }
    return stats;
}
bool Renderer::textureResidency(TextureHandle texture, uint32_t& width, uint32_t& height, uint32_t& firstMip) const {
    const auto r = m->record(texture.id);
    if (!r) return false;
    std::lock_guard lock(m->residencyMutex);
    width = r->width;
    height = r->height;
    firstMip = r->resident;
    return true;
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
    if (material.indirect && !m->bakedLighting.load(std::memory_order_relaxed)) {
        DrawMaterial environment = material;
        environment.indirect = nullptr;
        m->backend->drawMesh(mesh, transform, texture, tint, environment, key);
        return;
    }
    m->backend->drawMesh(mesh, transform, texture, tint, material, key);
}
void Renderer::drawSkinnedMesh(
    MeshHandle mesh, const glm::mat4& transform, std::span<const glm::mat4> joints, TextureHandle texture,
    const glm::vec4& tint, const DrawMaterial& material, RenderInstanceKey key) {
    if (key.valid()) key.meshGeneration = m->meshGeneration(mesh);
    if (material.indirect && !m->bakedLighting.load(std::memory_order_relaxed)) {
        DrawMaterial environment = material;
        environment.indirect = nullptr;
        m->backend->drawSkinnedMesh(mesh, transform, joints, texture, tint, environment, key);
        return;
    }
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
void Renderer::setAmbientOcclusion(AmbientOcclusion quality) {
    m->ambientOcclusion = quality;
    m->backend->setAmbientOcclusion(quality);
}
AmbientOcclusion Renderer::ambientOcclusion() const { return m->ambientOcclusion; }
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
std::vector<EffectiveSetting> Renderer::effectiveSettings() const {
    std::vector<EffectiveSetting> settings = m->backend->effectiveSettings();
    // Texture levels are the Renderer's own work, whatever the backend.
    const TextureResidencyStats stats = textureResidency();
    EffectiveSetting quality;
    quality.name = "texture_quality";
    quality.requested = quality.effective = textureQualityName(textureQuality());
    if (stats.pending) quality.reason = "pending: " + std::to_string(stats.pending) + " textures changing levels";
    const auto mib = [](uint64_t bytes) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", double(bytes) / (1024.0 * 1024.0));
        return std::string(text);
    };
    quality.detail = std::to_string(stats.reduced) + " of " + std::to_string(stats.textures) +
                     " material textures reduced; " + mib(stats.residentBytes) + " MiB on the GPU (" +
                     mib(stats.fullBytes) + " MiB at ultra); " + mib(stats.retainedBytes) + " MiB CPU copies";
    settings.push_back(std::move(quality));
    return settings;
}
RendererMemoryReport Renderer::memoryReport() const {
    RendererMemoryReport report = m->backend->memoryReport();
    std::lock_guard lock(m->journalMutex);
    for (const auto& [id, mesh] : m->meshes)
        report.cpuCopyBytes += mesh.vertices.size() * sizeof(Vertex) + mesh.indices.size() * sizeof(uint32_t);
    for (const auto& [id, lods] : m->meshLods) report.cpuCopyBytes += lods.second.size() * sizeof(uint32_t);
    for (const auto& [id, skin] : m->skins) report.cpuCopyBytes += skin.size() * sizeof(VertexSkin);
    for (const auto& [id, mesh] : m->meshes)
        report.cpuCopyBytes += (mesh.uv1.size() + mesh.lightmapUv.size()) * sizeof(glm::vec2);
    std::lock_guard records(m->residencyMutex);
    for (const auto& [id, r] : m->records)
        if (r->source) report.cpuCopyBytes += r->source->bytes.size();
    return report;
}
bool Renderer::supportsTextureShape(TextureShape shape) const {
    std::shared_lock swap(m->swapMutex);
    return m->backend->supportsTextureShape(shape);
}
void Renderer::setReflectionProbes(std::span<const ReflectionProbeDesc> probes) {
    if (probes.size() > kMaxReflectionProbes && m->reflectionProbes.size() != kMaxReflectionProbes)
        SW_LOG(
            LogRender, Warning, "{} reflection probes; only the first {} are used", probes.size(),
            kMaxReflectionProbes);
    m->reflectionProbes.assign(probes.begin(), probes.begin() + std::min<size_t>(probes.size(), kMaxReflectionProbes));
    m->backend->setReflectionProbes(m->reflectionProbes);
}
void Renderer::setLocalReflections(bool enabled) {
    m->localReflections = enabled;
    m->backend->setLocalReflections(enabled);
}
bool Renderer::localReflections() const { return m->localReflections; }
bool Renderer::requestSceneCapture(std::string& error) { return m->backend->requestSceneCapture(error); }
bool Renderer::sceneCapturePending() const { return m->backend->sceneCapturePending(); }
SceneCapture Renderer::takeSceneCapture() { return m->backend->takeSceneCapture(); }

void Renderer::requestScreenshot(const std::string& path) { m->backend->requestScreenshot(path); }
bool Renderer::screenshotPending() const { return m->backend->screenshotPending(); }
const std::string& Renderer::screenshotResult() const { return m->backend->screenshotResult(); }
float Renderer::waitForPresent() { return m->backend->waitForPresent(); }
const char* Renderer::presentTiming() const { return m->backend->presentTiming(); }
const char* Renderer::presentModeName() const { return m->backend->presentModeName(); }

// ---- Engine calls ----

bool Renderer::beginFrame() {
    m->pumpResidency();
    return m->backend->beginFrame();
}
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
