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
#include <limits>
#include <cstdio>
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
         field("scale", &ParticleEffect::scale, 0.01f, 0.001f, 1000.0f), field("color", &ParticleEffect::color),
         field("soft_distance", &ParticleEffect::softDistance, 0.01f, -1.0f, 100.0f),
         enumField("sorting", &ParticleEffect::sorting, {"whole", "parts"})},
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

std::string metres(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g m", value);
    return text;
}

const char* nodeKind(Effekseer::EffectNodeType type) {
    switch (type) {
    case Effekseer::EffectNodeType::Sprite: return "sprite";
    case Effekseer::EffectNodeType::Ribbon: return "ribbon";
    case Effekseer::EffectNodeType::Ring: return "ring";
    case Effekseer::EffectNodeType::Model: return "model";
    case Effekseer::EffectNodeType::Track: return "track";
    default: return nullptr;
    }
}

// The support report (EffectFeature) for a node and its children, from its decoded
// render parameters. Keep it to what the bridge and particle.frag really do.
void nodeFeatures(Effekseer::EffectNode* node, const std::string& path, EffectInfo& info) {
    if (!node) return;
    using Effekseer::RendererMaterialType;
    using Texture = Effekseer::RendererTextureType;
    if (const char* kind = nodeKind(node->GetType())) {
        ++info.nodes;
        const Effekseer::EffectBasicRenderParameter p = node->GetBasicRenderParameter();
        auto add = [&](std::string feature, const char* status, std::string detail, const char* setting = "") {
            info.features.push_back({path, std::move(feature), status, std::move(detail), setting});
        };
        auto uses = [&](Texture slot) { return p.TextureIndexes[static_cast<size_t>(slot)] >= 0; };
        const bool material = p.MaterialType == RendererMaterialType::File;
        if (material) {
            add(std::string(kind) + " with a custom .efkmat material", "unsupported",
                "not drawn: custom materials need compiling for Shockwave, which isn't done yet");
        } else if (p.MaterialType == RendererMaterialType::BackDistortion) {
            add(std::string(kind) + " (distortion)", "supported",
                "bends the scene behind it by up to " + metres(p.DistortionIntensity) +
                    " along its surface; one layer: see-through particles drawn before it are covered by "
                    "the scene it shows",
                "particle_distortion");
        } else if (p.MaterialType == RendererMaterialType::Lighting) {
            add(std::string(kind) + " (lit)", "supported",
                "lit by every light with particle_lighting scene, by the sun and ambient light with simple",
                "particle_lighting");
            if (uses(Texture::Normal))
                add("normal map", "approximated", "lit by the particle's surface normal; the normal map isn't read");
        } else {
            add(kind, "supported", "");
        }
        if (!material) {
            if (p.FlipbookParams.EnableInterpolation) add("flipbook blending", "supported", "");
            if (uses(Texture::Alpha)) add("alpha texture", "supported", "");
            if (p.EdgeParam.Threshold > 0.0f || p.EdgeParam.Color[3] > 0.0f) add("alpha cutoff edge", "supported", "");
            if (uses(Texture::UVDistortion))
                add("UV distortion", "unsupported", "drawn with its textures' UVs left as they are");
            if (uses(Texture::Blend)) add("blend texture", "unsupported", "drawn without the blend texture");
            if (p.EnableFalloff) add("falloff", "unsupported", "drawn without the colour change toward the edges");
            if (p.SoftParticleDistanceFar > 0.0f || p.SoftParticleDistanceNear > 0.0f) {
                std::string detail;
                if (p.SoftParticleDistanceFar > 0.0f)
                    detail = "fades over " + metres(p.SoftParticleDistanceFar) + " to the scene behind it";
                if (p.SoftParticleDistanceNear > 0.0f)
                    detail += std::string(detail.empty() ? "" : "; ") + "fades out over " +
                              metres(p.SoftParticleDistanceNear) + " toward " +
                              metres(p.SoftParticleDistanceNearOffset) + " from the camera";
                add("soft particles", "supported", detail, "soft_particles");
            }
            if (p.ZWrite && p.AlphaBlend != Effekseer::AlphaBlendType::Opacity)
                add("depth writes on a see-through part", "approximated",
                    "with soft particles on, see-through parts don't write depth", "soft_particles");
        }
    }
    for (int i = 0; i < node->GetChildrenCount(); ++i)
        nodeFeatures(node->GetChild(i), path.empty() ? std::to_string(i) : path + "/" + std::to_string(i), info);
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
        float softDistance = -1.0f;   // ParticleEffect's
        ParticleEffect::Sorting sorting = ParticleEffect::Sorting::Whole;
    };
    // An effect instance to draw this frame, and how.
    struct DrawItem {
        Handle handle;
        float softDistance;
        ParticleEffect::Sorting sorting;
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
    std::vector<DrawItem> drawOrder;
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
        // Parts of it that aren't drawn as made, as warnings, by node (the support report).
        EffectInfo info;
        nodeFeatures(l.effect->GetRoot(), "", info);
        for (const EffectFeature& f : info.features)
            if (f.status == "unsupported") problem("node " + f.node + ": " + f.feature + ": " + f.detail, true);
        if (l.effect->GetMaterialCount() > 0 &&
            std::none_of(info.features.begin(), info.features.end(), [](const EffectFeature& f) {
                return f.feature.find(".efkmat") != std::string::npos;
            }))
            problem("Has custom .efkmat materials, which aren't drawn yet", true);
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
            r.softDistance = pe.softDistance;
            r.sorting = pe.sorting;
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

    // Back to front, so see-through particles layer correctly. An effect sorted whole
    // keeps the order its parts were made in and goes by the middle of its particles' bounds;
    // one sorted in parts sorts each batch by the middle of its own (A04). Equal distances
    // keep the order they were drawn in.
    void sortBatches(const glm::vec3& eye, const glm::vec3& front) {
        const ParticleDrawList list = capture->drawList();
        const std::vector<uint32_t>& groups = capture->batchGroups();
        struct Bounds {
            glm::vec3 lo{std::numeric_limits<float>::max()}, hi{-std::numeric_limits<float>::max()};
            void add(const Bounds& b) {
                lo = glm::min(lo, b.lo);
                hi = glm::max(hi, b.hi);
            }
            bool empty() const { return lo.x > hi.x; }
        };
        std::vector<Bounds> batchBounds(list.batches.size()), groupBounds(drawOrder.size());
        for (size_t b = 0; b < list.batches.size(); ++b) {
            const ParticleBatch& batch = list.batches[b];
            for (uint32_t k = batch.firstIndex; k < batch.firstIndex + batch.indexCount; ++k) {
                const glm::vec3& p = list.vertices[list.indices[k]].position;
                batchBounds[b].lo = glm::min(batchBounds[b].lo, p);
                batchBounds[b].hi = glm::max(batchBounds[b].hi, p);
            }
            groupBounds[groups[b]].add(batchBounds[b]);
        }
        auto distance = [&](const Bounds& b, float fallback) {
            return b.empty() ? fallback : glm::dot((b.lo + b.hi) * 0.5f - eye, front);
        };
        std::vector<std::pair<float, uint32_t>> keys(list.batches.size());
        for (uint32_t b = 0; b < keys.size(); ++b) {
            const bool parts = drawOrder[groups[b]].sorting == ParticleEffect::Sorting::Parts;
            const float key = distance(parts ? batchBounds[b] : groupBounds[groups[b]], list.batches[b].depth);
            keys[b] = {key, b};
            capture->setBatchDepth(b, key);
        }
        std::stable_sort(keys.begin(), keys.end(), [&](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            // The same distance: a whole effect's batches stay together, in their order.
            return groups[a.second] < groups[b.second];
        });
        std::vector<uint32_t> order(keys.size());
        for (size_t i = 0; i < keys.size(); ++i) order[i] = keys[i].second;
        capture->reorderBatches(order);
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

        // Each effect is captured as a group of its own, then the groups (or, for effects
        // sorted in parts, their batches) are put back to front (sortBatches).
        drawOrder.clear();
        auto add = [&](Handle h, float softDistance, ParticleEffect::Sorting sorting) {
            if (alive(h)) drawOrder.push_back({h, softDistance, sorting});
        };
        for (const auto& [index, r] : running) add(r.handle, r.softDistance, r.sorting);
        for (Handle h : oneShots) add(h, -1.0f, ParticleEffect::Sorting::Whole);

        capture->begin(view, projection);
        for (uint32_t i = 0; i < drawOrder.size(); ++i) {
            const Effekseer::Matrix43 m = manager->GetMatrix(drawOrder[i].handle);
            capture->setDepth(glm::dot(glm::vec3(m.Value[3][0], m.Value[3][1], m.Value[3][2]) - eye, front));
            capture->beginGroup(i, drawOrder[i].softDistance);
            manager->DrawHandle(drawOrder[i].handle, parameter);
        }
        capture->end();
        sortBatches(eye, front);
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

EffectInfo Effects::info(std::string_view effect) {
    m->join();
    EffectInfo info;
    info.file = Impl::key(effect);
    const Impl::Loaded& l = m->load(effect);
    info.problems = l.problems;
    if (l.effect == nullptr) return info;
    info.loaded = true;
    info.duration = duration(effect);
    info.textures = static_cast<uint32_t>(
        l.effect->GetColorImageCount() + l.effect->GetNormalImageCount() + l.effect->GetDistortionImageCount());
    info.models = static_cast<uint32_t>(l.effect->GetModelCount());
    info.materials = static_cast<uint32_t>(l.effect->GetMaterialCount());
    info.sounds = static_cast<uint32_t>(l.effect->GetWaveCount());
    nodeFeatures(l.effect->GetRoot(), "", info);
    return info;
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
