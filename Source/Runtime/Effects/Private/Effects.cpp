// Effects: an Effekseer Manager simulating every effect, fed from the world's
// ParticleEffects and game code, and drawn through the capture renderer in
// EffekseerBridge. The simulation runs as one job between update() and render(); every
// other call waits for it first (join), so Effekseer is never touched from two threads.
#include <Effects/Effects.h>

#include "EffekseerBridge.h"

#include <Core/Jobs.h>
#include <Core/Log.h>
#include <Core/Profile.h>
#include <Engine/Components.h>
#include <Engine/Engine.h>
#include <Engine/Hierarchy.h>
#include <Renderer/Renderer.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

namespace shockwave {

SW_LOG_CATEGORY(LogEffects);

namespace fs = std::filesystem;

void registerEffectsComponents() {
    registerComponent<ParticleEffect>(
        "ParticleEffect",
        {field("effect", &ParticleEffect::effect), field("playing", &ParticleEffect::playing),
         field("loop", &ParticleEffect::loop), field("speed", &ParticleEffect::speed, 0.01f, 0.0f, 10.0f),
         field("scale", &ParticleEffect::scale, 0.01f, 0.001f, 1000.0f), field("color", &ParticleEffect::color)},
        "Effects");
}

namespace {

using Clock = std::chrono::steady_clock;

float msSince(Clock::time_point start) {
    return std::chrono::duration<float, std::milli>(Clock::now() - start).count();
}

// Effekseer counts time in frames of a 60 fps editor.
constexpr float kEffekseerFps = 60.0f;
constexpr double kPollSeconds = 0.5;

std::mutex& registryMutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<const Engine*, Effects*>& registry() {
    static std::unordered_map<const Engine*, Effects*> r;
    return r;
}

// glm's columns are Effekseer's rows: the X, Y and Z axes, then the position.
Effekseer::Matrix43 toEffekseer(const glm::mat4& m) {
    Effekseer::Matrix43 out;
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 3; ++column) out.Value[row][column] = m[row][column];
    return out;
}

Effekseer::Color toEffekseer(const glm::vec4& color) {
    const glm::vec4 c = glm::clamp(color, 0.0f, 1.0f) * 255.0f + 0.5f;
    return Effekseer::Color(
        static_cast<uint8_t>(c.r), static_cast<uint8_t>(c.g), static_cast<uint8_t>(c.b), static_cast<uint8_t>(c.a));
}

std::u16string toUtf16(const fs::path& path) {
    const std::u8string utf8 = path.generic_u8string();
    std::u16string out(utf8.size() + 1, u'\0');
    const int32_t n = Effekseer::ConvertUtf8ToUtf16(
        out.data(), static_cast<int32_t>(out.size()), reinterpret_cast<const char*>(utf8.c_str()));
    out.resize(static_cast<size_t>(std::max(n, 0)));
    return out;
}

fs::file_time_type modified(const fs::path& path) {
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type::min() : time;
}

// Features in an effect the bridge doesn't draw, as warnings.
void unsupportedFeatures(Effekseer::EffectNode* node, bool& distortion, bool& material, bool& falloff) {
    if (!node) return;
    if (node->GetType() != Effekseer::EffectNodeType::Root && node->GetType() != Effekseer::EffectNodeType::NoneType) {
        const Effekseer::EffectBasicRenderParameter p = node->GetBasicRenderParameter();
        distortion |= p.MaterialType == Effekseer::RendererMaterialType::BackDistortion;
        material |= p.MaterialType == Effekseer::RendererMaterialType::File;
        falloff |= p.EnableFalloff;
    }
    for (int i = 0; i < node->GetChildrenCount(); ++i)
        unsupportedFeatures(node->GetChild(i), distortion, material, falloff);
}

}   // namespace

// ---- Impl ---------------------------------------------------------------------------------

struct Effects::Impl {
    struct Loaded {
        Effekseer::EffectRef effect;   // null when it failed to load
        std::vector<std::pair<fs::path, fs::file_time_type>> files;   // the effect, its textures and models
        std::vector<EffectProblem> problems;
    };
    // An entity's ParticleEffect and the Effekseer instance showing it.
    struct Running {
        Entity entity;
        std::string effect;
        Handle handle = -1;
        bool wasPlaying = false;
        bool started = false;   // played at least once (a non-looping effect plays once)
        bool stopping = false;   // StopRoot sent: no new particles, the live ones finish
        uint64_t seen = 0;
    };

    Engine* engine;
    effects::TextureGraveyard graveyard;
    Effekseer::ManagerRef manager;
    Effekseer::RefPtr<effects::CaptureRenderer> capture;
    Effekseer::RefPtr<effects::BridgeTextureLoader> textureLoader;
    Effekseer::RefPtr<effects::BridgeModelLoader> modelLoader;
    fs::path content;
    std::map<std::string, Loaded, std::less<>> cache;
    std::unordered_map<uint32_t, Running> running;   // by entity index
    std::vector<Handle> oneShots;   // game code's, and entity effects left to finish
    std::vector<std::pair<float, Handle>> drawOrder;
    uint64_t frame = 0;
    double pollTimer = 0.0;
    bool enabled = true;
    JobCounter simulation;
    bool simulating = false;
    float updateMs = 0.0f;
    // The scene's sun and ambient light, for lit particles.
    glm::vec3 lightDirection = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));
    glm::vec3 lightColor{1.0f};
    glm::vec3 ambient{0.3f};
    EffectsStats stats;

    explicit Impl(Engine* engine_) : engine(engine_) {
        ::shockwave::Renderer* renderer = engine ? &engine->renderer() : nullptr;
        manager = Effekseer::Manager::Create(kMaxParticles);
        manager->SetCoordinateSystem(Effekseer::CoordinateSystem::RH);
        capture = Effekseer::MakeRefPtr<effects::CaptureRenderer>(renderer, &graveyard);
        manager->SetSpriteRenderer(capture->CreateSpriteRenderer());
        manager->SetRibbonRenderer(capture->CreateRibbonRenderer());
        manager->SetRingRenderer(capture->CreateRingRenderer());
        manager->SetTrackRenderer(capture->CreateTrackRenderer());
        manager->SetModelRenderer(capture->CreateModelRenderer());
        textureLoader = Effekseer::MakeRefPtr<effects::BridgeTextureLoader>(&capture->device());
        modelLoader = Effekseer::MakeRefPtr<effects::BridgeModelLoader>();
        manager->SetTextureLoader(textureLoader);
        manager->SetModelLoader(modelLoader);
    }

    ~Impl() {
        join();
        manager->StopAllEffects();
        manager->Update(0.0f);
        running.clear();
        oneShots.clear();
        cache.clear();
        manager.Reset();
        capture.Reset();
        textureLoader.Reset();
        modelLoader.Reset();
        graveyard.flush(engine ? &engine->renderer() : nullptr);
    }

    // Waits for this frame's simulation; every use of the manager comes after one.
    void join() {
        if (!simulating) return;
        engine->jobs().wait(simulation);
        simulating = false;
    }

    void simulate(double dt) {
        const float deltaFrame = static_cast<float>(dt) * kEffekseerFps;
        auto step = [this, deltaFrame] {
            SW_PROFILE_SCOPE("Effects simulation");
            const auto start = Clock::now();
            manager->Update(deltaFrame);
            updateMs = msSince(start);
        };
        if (engine && engine->jobs().workerCount() > 0) {
            simulating = true;
            engine->jobs().run(step, &simulation);
        } else {
            step();
        }
    }

    static std::string key(std::string_view effect) { return fs::path(effect).lexically_normal().generic_string(); }

    Loaded& load(std::string_view effect) {
        std::string name = key(effect);
        if (auto it = cache.find(name); it != cache.end()) return it->second;
        SW_PROFILE_SCOPE("Effect load");
        Loaded& l = cache[name];
        const fs::path file = content / name;
        l.files.emplace_back(file, modified(file));
        auto problem = [&](std::string message, bool warning = false) {
            l.problems.push_back({name, std::move(message), warning});
        };
        std::error_code ec;
        if (effect.empty()) {
            problem("No effect file is set");
            return l;
        }
        if (file.extension() == ".efkproj") {
            problem("This is an Effekseer project file; save it as .efkefc in the Effekseer editor (File > Save As)");
            return l;
        }
        if (!fs::is_regular_file(file, ec)) {
            problem("The file isn't there: " + file.generic_string());
            return l;
        }
        effects::LoadReport report;
        textureLoader->report = &report;
        modelLoader->report = &report;
        const std::u16string path = toUtf16(fs::absolute(file, ec));
        l.effect = Effekseer::Effect::Create(manager, path.c_str(), 1.0f);
        textureLoader->report = nullptr;
        modelLoader->report = nullptr;
        for (const fs::path& dependency : report.files) l.files.emplace_back(dependency, modified(dependency));
        if (l.effect == nullptr) {
            problem("Effekseer couldn't read it; is it an .efkefc saved by Effekseer 1.5 or later?");
            return l;
        }
        for (const std::string& missing : report.missing) problem("Missing " + missing);
        bool distortion = false, material = false, falloff = false;
        unsupportedFeatures(l.effect->GetRoot(), distortion, material, falloff);
        if (distortion) problem("Uses distortion, which isn't drawn yet; those parts are skipped", true);
        if (material || l.effect->GetMaterialCount() > 0)
            problem("Uses custom .efkmat materials, which aren't drawn yet; those parts are skipped", true);
        if (falloff) problem("Uses falloff, which is drawn without it", true);
        if (l.effect->GetWaveCount() > 0)
            problem("Has sounds, which Effects doesn't play; use an AudioSource next to it", true);
        for (const EffectProblem& p : l.problems) SW_LOG(LogEffects, Warning, "{}: {}", p.file, p.message);
        return l;
    }

    Handle play(std::string_view effect, const glm::mat4& transform) {
        Loaded& l = load(effect);
        if (l.effect == nullptr) return -1;
        const Handle handle = manager->Play(l.effect, 0.0f, 0.0f, 0.0f);
        if (handle >= 0) manager->SetMatrix(handle, toEffekseer(transform));
        return handle;
    }

    bool alive(Handle handle) const { return handle >= 0 && manager->Exists(handle); }

    void stopEverything() {
        manager->StopAllEffects();
        running.clear();
        oneShots.clear();
    }

    // Reloads effects whose file, textures or models changed; running ones start again.
    void pollFiles(double dt) {
        pollTimer += dt;
        if (pollTimer < kPollSeconds) return;
        pollTimer = 0.0;
        for (auto it = cache.begin(); it != cache.end();) {
            const bool changed = std::any_of(it->second.files.begin(), it->second.files.end(), [](const auto& f) {
                return modified(f.first) != f.second;
            });
            if (!changed) {
                ++it;
                continue;
            }
            SW_LOG(LogEffects, Info, "Reloading {}", it->first);
            if (it->second.effect != nullptr) manager->StopRoot(it->second.effect);
            for (auto& [index, r] : running)
                if (key(r.effect) == it->first) {
                    if (r.handle >= 0) manager->StopEffect(r.handle);
                    r.handle = -1;
                    r.started = false;
                }
            it = cache.erase(it);
        }
    }

    void sync(World& world) {
        ++frame;
        world.each<const Transform, const ParticleEffect>([&](Entity e, const Transform&, const ParticleEffect& pe) {
            Running& r = running[e.index];
            if (r.entity != e || r.effect != pe.effect) {
                if (r.handle >= 0) manager->StopEffect(r.handle);
                r = Running{};
                r.entity = e;
                r.effect = pe.effect;
            }
            r.seen = frame;
            const glm::mat4 transform =
                worldMatrix(world, e) * glm::scale(glm::mat4(1.0f), glm::vec3(std::max(pe.scale, 0.0f)));
            const bool live = alive(r.handle);
            if (pe.playing && (!live || r.stopping) && (!r.wasPlaying || !r.started || pe.loop)) {
                // Particles from before still finish, as a one-shot.
                if (live) oneShots.push_back(r.handle);
                r.handle = play(pe.effect, transform);
                r.started = true;
                r.stopping = false;
            } else if (!pe.playing && live && !r.stopping) {
                manager->StopRoot(r.handle);
                r.stopping = true;
            }
            r.wasPlaying = pe.playing;
            if (r.handle >= 0) {
                manager->SetMatrix(r.handle, toEffekseer(transform));
                manager->SetSpeed(r.handle, std::max(pe.speed, 0.0f));
                manager->SetAllColor(r.handle, toEffekseer(pe.color));
            }
        });
        // Entities that lost their ParticleEffect, or are gone.
        for (auto it = running.begin(); it != running.end();) {
            if (it->second.seen == frame) {
                ++it;
                continue;
            }
            if (it->second.handle >= 0) manager->StopEffect(it->second.handle);
            it = running.erase(it);
        }
        std::erase_if(oneShots, [&](Handle h) { return !alive(h); });
    }

    // The first enabled directional light and Environment, as the renderer sees them.
    void readLighting(World& world) {
        bool found = false;
        world.each<const Transform, const Light>([&](Entity e, const Transform&, const Light& light) {
            if (found || !light.enabled || light.type != Light::Type::Directional) return;
            found = true;
            const glm::mat4 m = worldMatrix(world, e);
            lightDirection = glm::normalize(-glm::vec3(m[2]));
            lightColor = glm::vec3(light.color) * light.intensity;
        });
        if (!found) {
            lightDirection = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));
            lightColor = glm::vec3(1.0f);
        }
        Environment environment;
        if (const Pool<Environment>* pool = world.pool<Environment>(); pool && pool->size())
            environment = pool->components()[0];
        ambient = glm::vec3(environment.skyColor + environment.groundColor) * 0.5f * environment.ambientIntensity;
    }

    ParticleDrawList build(const glm::mat4& view, const glm::mat4& projection) {
        SW_PROFILE_SCOPE("Effects build");
        const auto waitStart = Clock::now();
        join();
        const float waitMs = msSince(waitStart);
        const auto start = Clock::now();
        const glm::mat4 cameraWorld = glm::inverse(view);
        const glm::vec3 eye(cameraWorld[3]);
        const glm::vec3 front = -glm::normalize(glm::vec3(cameraWorld[2]));

        Effekseer::Manager::DrawParameter parameter;
        const glm::mat4 viewProjection = projection * view;
        std::memcpy(parameter.ViewProjectionMatrix.Values, &viewProjection[0][0], sizeof(float) * 16);
        parameter.CameraPosition = {eye.x, eye.y, eye.z};
        parameter.CameraFrontDirection = {front.x, front.y, front.z};
        parameter.CameraCullingMask = manager->GetCameraCullingMaskToShowAllEffects();

        // Whole effects back to front, so blended ones layer correctly with each other.
        drawOrder.clear();
        auto add = [&](Handle h) {
            if (!alive(h)) return;
            const Effekseer::Matrix43 m = manager->GetMatrix(h);
            const glm::vec3 position(m.Value[3][0], m.Value[3][1], m.Value[3][2]);
            drawOrder.emplace_back(glm::dot(position - eye, front), h);
        };
        for (const auto& [index, r] : running) add(r.handle);
        for (Handle h : oneShots) add(h);
        std::sort(drawOrder.begin(), drawOrder.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

        capture->begin(view, projection);
        for (const auto& [depth, handle] : drawOrder) {
            capture->setDepth(depth);
            manager->DrawHandle(handle, parameter);
        }
        capture->end();
        ParticleDrawList list = capture->drawList();
        list.lightDirection = lightDirection;
        list.lightColor = lightColor;
        list.ambient = ambient;

        stats.effectsLoaded = 0;
        for (const auto& [name, l] : cache) stats.effectsLoaded += l.effect != nullptr;
        stats.playing = static_cast<uint32_t>(drawOrder.size());
        stats.particles = static_cast<uint32_t>(std::max(manager->GetTotalInstanceCount(), 0));
        stats.batches = static_cast<uint32_t>(list.batches.size());
        stats.vertices = static_cast<uint32_t>(list.vertices.size());
        stats.skippedBatches = capture->skippedBatches();
        stats.updateMs = updateMs;
        stats.buildMs = msSince(start);
        stats.waitMs = waitMs;
        return list;
    }
};

// ---- Effects ------------------------------------------------------------------------------

Effects::Effects(Engine* engine) : m(std::make_unique<Impl>(engine)) {
    if (engine) {
        std::lock_guard lock(registryMutex());
        registry()[engine] = this;
    }
}

Effects::~Effects() {
    if (m->engine) {
        std::lock_guard lock(registryMutex());
        if (auto it = registry().find(m->engine); it != registry().end() && it->second == this) registry().erase(it);
    }
}

Effects* Effects::forEngine(const Engine* engine) {
    if (!engine) return nullptr;
    std::lock_guard lock(registryMutex());
    const auto it = registry().find(engine);
    return it == registry().end() ? nullptr : it->second;
}

void Effects::setContentFolder(const fs::path& folder) {
    const fs::path normal = folder.lexically_normal();
    if (normal == m->content) return;
    m->join();
    m->stopEverything();
    m->cache.clear();
    m->content = normal;
}

const fs::path& Effects::contentFolder() const { return m->content; }

void Effects::update(World& world, double dt) {
    SW_PROFILE_SCOPE("Effects update");
    m->join();
    m->graveyard.flush(m->engine ? &m->engine->renderer() : nullptr);
    if (!m->enabled) return;
    m->pollFiles(dt);
    m->sync(world);
    m->readLighting(world);
    m->simulate(std::max(dt, 0.0));
}

void Effects::render(Renderer& renderer) {
    if (!m->enabled) return;
    SW_PROFILE_SCOPE("Effects render");
    const ParticleDrawList list = m->build(renderer.cameraView(), renderer.cameraProjection());
    if (!list.batches.empty()) renderer.drawParticles(list);
}

ParticleDrawList Effects::build(const glm::mat4& view, const glm::mat4& projection) {
    return m->build(view, projection);
}

void Effects::restart() {
    m->join();
    m->stopEverything();
}

void Effects::restart(Entity entity) {
    m->join();
    const auto it = m->running.find(entity.index);
    if (it == m->running.end() || it->second.entity != entity) return;
    if (it->second.handle >= 0) m->manager->StopEffect(it->second.handle);
    it->second.handle = -1;
    it->second.started = false;
    it->second.wasPlaying = false;
}

uint32_t Effects::particleCount(Entity entity) const {
    m->join();
    const Handle h = handleOf(entity);
    return h >= 0 ? static_cast<uint32_t>(std::max(m->manager->GetInstanceCount(h), 0)) : 0;
}

float Effects::duration(std::string_view effect) {
    m->join();
    const Impl::Loaded& l = m->load(effect);
    if (l.effect == nullptr) return 0.0f;
    const Effekseer::EffectTerm term = l.effect->CalculateTerm();
    if (term.TermMax >= INT_MAX / 2) return -1.0f;
    return static_cast<float>(term.TermMax) / kEffekseerFps;
}

std::vector<EffectProblem> Effects::validate(std::string_view effect) {
    m->join();
    return m->load(effect).problems;
}

bool Effects::preload(std::string_view effect) {
    m->join();
    return m->load(effect).effect != nullptr;
}

std::vector<std::string> Effects::effectFiles() const {
    std::vector<std::string> files;
    std::error_code ec;
    if (m->content.empty() || !fs::is_directory(m->content, ec)) return files;
    for (auto it = fs::recursive_directory_iterator(m->content, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".efkefc")
            files.push_back(it->path().lexically_relative(m->content).generic_string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

void Effects::reloadAll() {
    m->join();
    m->stopEverything();
    m->cache.clear();
}

Effects::Handle Effects::play(std::string_view effect, const glm::mat4& transform) {
    m->join();
    const Handle h = m->play(effect, transform);
    if (h >= 0) m->oneShots.push_back(h);
    return h;
}

bool Effects::exists(Handle handle) const {
    m->join();
    return m->alive(handle);
}

void Effects::stop(Handle handle, bool immediately) {
    m->join();
    if (!m->alive(handle)) return;
    if (immediately)
        m->manager->StopEffect(handle);
    else
        m->manager->StopRoot(handle);
}

void Effects::stopAll() {
    m->join();
    for (Handle h : m->oneShots) m->manager->StopEffect(h);
    m->oneShots.clear();
}

void Effects::setTransform(Handle handle, const glm::mat4& transform) {
    m->join();
    if (m->alive(handle)) m->manager->SetMatrix(handle, toEffekseer(transform));
}

void Effects::setSpeed(Handle handle, float speed) {
    m->join();
    if (m->alive(handle)) m->manager->SetSpeed(handle, std::max(speed, 0.0f));
}

void Effects::setColor(Handle handle, const glm::vec4& color) {
    m->join();
    if (m->alive(handle)) m->manager->SetAllColor(handle, toEffekseer(color));
}

void Effects::setPaused(Handle handle, bool paused) {
    m->join();
    if (m->alive(handle)) m->manager->SetPaused(handle, paused);
}

void Effects::setInput(Handle handle, int index, float value) {
    m->join();
    if (m->alive(handle) && index >= 0 && index < 4) m->manager->SetDynamicInput(handle, index, value);
}

void Effects::sendTrigger(Handle handle, int index) {
    m->join();
    if (m->alive(handle) && index >= 0 && index < 4) m->manager->SendTrigger(handle, index);
}

Effects::Handle Effects::handleOf(Entity entity) const {
    m->join();
    const auto it = m->running.find(entity.index);
    if (it == m->running.end() || it->second.entity != entity || !m->alive(it->second.handle)) return -1;
    return it->second.handle;
}

void Effects::setEnabled(bool enabled) {
    if (enabled == m->enabled) return;
    m->join();
    m->enabled = enabled;
    if (!enabled) {
        m->stopEverything();
        m->stats = {};
    }
}

bool Effects::enabled() const { return m->enabled; }

EffectsStats Effects::stats() const { return m->stats; }

}   // namespace shockwave
