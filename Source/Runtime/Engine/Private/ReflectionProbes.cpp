#include <Engine/ReflectionProbes.h>

#include <Assets/Assets.h>
#include <Core/Log.h>
#include <Engine/BakedLighting.h>
#include <Engine/Hierarchy.h>
#include <Engine/PhysicsComponents.h>
#include <Engine/Pose.h>
#include <Engine/Systems.h>
#include <Renderer/Renderer.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <system_error>

namespace shockwave {

namespace fs = std::filesystem;

// ---- What a capture sees ----

bool reflectionCaptureIncludes(const World& world, Entity e) {
    for (int depth = 0; e.valid() && depth < 64; ++depth, e = parentOf(world, e)) {
        if (const BakeGeometry* g = world.get<BakeGeometry>(e); g && g->mobility == BakeGeometry::Mobility::Dynamic)
            return false;
        if (const RigidBody* body = world.get<RigidBody>(e); body && body->motion != RigidBody::Motion::Static)
            return false;
        if (world.has<CharacterController>(e) || world.has<ModelPose>(e) || world.has<Spin>(e) || world.has<Bob>(e))
            return false;
    }
    return true;
}

namespace {
std::string label(const World& world, Entity e) {
    const Name* name = world.alive(e) ? world.get<Name>(e) : nullptr;
    return name && !name->value.empty() ? name->value : "entity " + std::to_string(e.index);
}
// What one captured renderer contributes to a capture: placement, mesh and surface.
uint64_t rendererHash(const World& world, const Assets& assets, Entity e) {
    LightingHasher h;
    h.mat(worldMatrix(world, e));
    if (const MeshRenderer* m = world.get<MeshRenderer>(e); m && m->visible) {
        h.text("mesh").text(assets.meshName(m->mesh)).text(assets.textureName(m->texture));
        h.text(assets.materialName(m->material)).vec(m->tint).vec(m->tiling).f32(m->roughness).f32(m->metallic);
        h.vec(glm::vec3(m->emissive) * m->emissiveIntensity).u32(m->castShadows);
    }
    if (const ModelRenderer* r = world.get<ModelRenderer>(e); r && r->visible) {
        h.text("model").text(assets.modelName(r->model)).vec(r->tint).u32(r->castShadows);
        for (const MaterialOverride& o : r->materials) h.text(o.part).text(assets.materialName(o.material));
    }
    return h.value();
}
}   // namespace

ReflectionSceneDescription
describeReflectionProbes(const World& world, const Assets& assets, const fs::path& contentRoot, bool withSceneHash) {
    ReflectionSceneDescription d;
    if (const Pool<ReflectionProbe>* pool = world.pool<ReflectionProbe>()) {
        const auto entities = pool->entities();
        for (size_t i = 0; i < entities.size(); ++i) {
            const Entity e = entities[i];
            const ReflectionProbe& p = pool->components()[i];
            if (!p.enabled || !world.has<Transform>(e)) continue;
            std::string problem;
            const BindingId binding = effectiveBindingId(world, e, p.bindingId, &problem);
            if (!binding.valid()) {
                d.warnings.push_back(problem + "; the reflection probe is left out");
                continue;
            }
            // The box's own frame: the entity's rotation and position; its scale goes into
            // the extents, so distances in the box are metres.
            const glm::mat4 m = worldMatrix(world, e);
            const glm::vec3 scale(
                glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
            const glm::vec3 extents = p.halfExtents * scale;
            const float det = glm::determinant(glm::mat3(m));
            if (!std::isfinite(det) || std::abs(det) < 1e-9f ||
                !(glm::all(glm::greaterThan(extents, glm::vec3(0.0f))))) {
                d.warnings.push_back(label(world, e) + " is a reflection probe with no size; it's left out");
                continue;
            }
            if (glm::any(glm::greaterThan(glm::abs(p.captureOffset), p.halfExtents))) {
                d.warnings.push_back(
                    label(world, e) + "'s capture_offset is outside its box; move it inside (the probe is left out)");
                continue;
            }
            if (!(p.nearPlane > 0.0f) || !(p.farPlane > p.nearPlane) || !std::isfinite(p.farPlane)) {
                d.warnings.push_back(label(world, e) + " needs 0 < near < far; the reflection probe is left out");
                continue;
            }
            ReflectionProbeSource s;
            s.entity = e;
            s.binding = binding;
            s.probe = p;
            glm::mat4 rigid(1.0f);
            for (int c = 0; c < 3; ++c) rigid[c] = glm::vec4(glm::vec3(m[c]) / scale[c], 0.0f);
            rigid[3] = m[3];
            s.desc.boxToWorld = rigid;
            s.desc.halfExtents = extents;
            s.desc.capturePosition = glm::vec3(m * glm::vec4(p.captureOffset, 1.0f));
            s.desc.blendDistance = std::max(p.blendDistance, 0.0f);
            s.desc.priority = p.priority;
            s.desc.order = binding.hi;
            const uint32_t size = reflectionProbeSize(p.resolution);
            s.hash = LightingHasher()
                         .id(binding)
                         .mat(rigid)
                         .vec(extents)
                         .vec(s.desc.capturePosition)
                         .f32(s.desc.blendDistance)
                         .u32(uint32_t(p.priority))
                         .u32(size)
                         .f32(p.nearPlane)
                         .f32(p.farPlane)
                         .value();
            d.budgetBytes += reflectionCubeBytes(size);
            d.probes.push_back(s);
        }
    }
    std::sort(d.probes.begin(), d.probes.end(), [](const ReflectionProbeSource& a, const ReflectionProbeSource& b) {
        return a.binding < b.binding;
    });
    for (size_t i = 1; i < d.probes.size(); ++i)
        if (d.probes[i].binding == d.probes[i - 1].binding)
            d.warnings.push_back(
                label(world, d.probes[i].entity) + " has the same binding id as " +
                label(world, d.probes[i - 1].entity) + " (player --lighting-assign-ids fixes it)");
    if (!withSceneHash) return d;

    // Static content, as a set: the same objects in any order hash the same.
    std::vector<uint64_t> objects;
    world.forEachEntity([&](Entity e) {
        if ((world.has<MeshRenderer>(e) || world.has<ModelRenderer>(e)) && reflectionCaptureIncludes(world, e))
            objects.push_back(rendererHash(world, assets, e));
        if (const Light* l = world.get<Light>(e); l && l->enabled)
            objects.push_back(LightingHasher()
                                  .text("light")
                                  .mat(worldMatrix(world, e))
                                  .u32(uint32_t(l->type))
                                  .vec(glm::vec3(l->color) * l->intensity)
                                  .f32(l->range)
                                  .f32(l->spotAngle)
                                  .f32(l->spotSoftness)
                                  .u32(l->castShadows)
                                  .f32(l->shadowStrength)
                                  .f32(l->shadowSoftness)
                                  .value());
    });
    std::sort(objects.begin(), objects.end());
    LightingHasher h;
    h.u64(objects.size());
    for (uint64_t o : objects) h.u64(o);
    if (const Pool<Environment>* environments = world.pool<Environment>(); environments && environments->size()) {
        const Environment& env = environments->components()[0];
        h.vec(env.skyColor).vec(env.groundColor).f32(env.ambientIntensity).text(assets.textureName(env.environmentMap));
        h.f32(env.environmentIntensity).f32(env.environmentRotation).u32(env.showEnvironment);
    }
    // The lighting bake, by its manifest (which vouches for every payload by hash).
    if (const Pool<BakedLighting>* pool = world.pool<BakedLighting>(); pool && pool->size()) {
        const BakedLighting& bake = pool->components()[0];
        std::vector<uint8_t> bytes;
        std::string ignored;
        if (bake.enabled && !bake.asset.empty() && readLightingFile(contentRoot / bake.asset, bytes, ignored))
            h.text("bake").u64(hashLightingBytes(bytes));
    }
    d.sceneHash = h.value();
    return d;
}

std::string reflectionManifestPath(const World& world, const std::string& scenePath) {
    if (const Pool<BakedLighting>* pool = world.pool<BakedLighting>(); pool && pool->size())
        if (!pool->components()[0].reflections.empty()) return pool->components()[0].reflections;
    return "Lighting/" + fs::path(scenePath).stem().generic_string() + ".swreflection";
}

std::string reflectionCubePath(const std::string& manifest, const BindingId& binding) {
    // Not the lighting bake's folder ("<manifest>/"): that one is replaced whole by a rebake.
    return fs::path(manifest).replace_extension().generic_string() + "_reflections/Probe_" + formatBindingId(binding) +
           ".ktx2";
}

namespace {
const BakedLighting* sceneBake(const World& world) {
    const Pool<BakedLighting>* pool = world.pool<BakedLighting>();
    return pool && pool->size() ? &pool->components()[0] : nullptr;
}
}   // namespace

// ---- Runtime ----

struct ReflectionProbesRuntime::Loaded {
    ReflectionManifest manifest;
    struct Cube {
        BindingId binding;
        uint64_t probeHash = 0;
        TextureHandle texture;
    };
    std::vector<Cube> cubes;
    uint64_t gpuBytes = 0;
};

ReflectionProbesRuntime::ReflectionProbesRuntime() = default;
ReflectionProbesRuntime::~ReflectionProbesRuntime() { release(); }

const char* ReflectionProbesRuntime::stateName(State state) {
    switch (state) {
    case State::None: return "none";
    case State::Missing: return "missing";
    case State::Invalid: return "invalid";
    case State::Stale: return "stale";
    case State::Ready: return "ready";
    case State::Unsupported: return "unsupported";
    }
    return "?";
}

void ReflectionProbesRuntime::free(Loaded& loaded) {
    if (!m_renderer) return;
    for (const Loaded::Cube& c : loaded.cubes)
        if (c.texture.id) m_renderer->destroyTexture(c.texture);
    loaded.cubes.clear();
}

void ReflectionProbesRuntime::release() {
    if (m_renderer && !m_bound.empty()) m_renderer->setReflectionProbes({});
    m_bound.clear();
    if (m_loaded) free(*m_loaded);
    m_loaded.reset();
    m_status = {};
    m_asset.clear();
    m_loadState = State::None;
    m_loadError.clear();
}

void ReflectionProbesRuntime::load(const Assets& assets, Renderer& renderer, const std::string& asset) {
    const fs::path contentRoot = assets.contentRoot();
    std::string error;
    auto fail = [&](State state, const std::string& reason) {
        // The last good capture stays; only a complete new one replaces it.
        m_loadState = state;
        m_loadError = reason;
    };
    auto next = std::make_unique<Loaded>();
    std::error_code ec;
    if (!loadReflectionManifest(contentRoot / asset, next->manifest, error)) {
        const bool exists = fs::exists(contentRoot / asset, ec);
        return fail(exists ? State::Invalid : State::Missing, exists ? error : "not captured yet (no " + asset + ")");
    }
    // Every cube whole and as the manifest vouches, before anything reaches the GPU.
    std::vector<std::pair<const ReflectionProbeRecord*, ReflectionCube>> cubes;
    for (const ReflectionProbeRecord& r : next->manifest.probes) {
        std::vector<uint8_t> bytes;
        if (!readLightingFile(contentRoot / r.path, bytes, error)) return fail(State::Invalid, error);
        if (hashLightingBytes(bytes) != r.hash || bytes.size() != r.bytes)
            return fail(State::Invalid, r.path + " doesn't match its manifest (an interrupted capture or an edit)");
        ReflectionCube cube;
        if (!decodeReflectionKtx2(bytes, cube, error)) return fail(State::Invalid, r.path + ": " + error);
        if (cube.size != r.size) return fail(State::Invalid, r.path + " isn't the size its manifest says");
        cubes.push_back({&r, std::move(cube)});
    }
    m_renderer = &renderer;
    if (!renderer.supportsTextureShape(TextureShape::Cube))
        return fail(State::Unsupported, "this renderer can't draw reflection probes (no cube textures)");
    for (auto& [record, cube] : cubes) {
        const TextureData data = reflectionTextureData(cube);
        try {
            next->cubes.push_back({record->binding, record->probeHash, renderer.createTexture(data)});
        } catch (const std::exception& e) {
            free(*next);
            return fail(State::Invalid, std::string("can't upload ") + record->path + ": " + e.what());
        }
        next->gpuBytes += data.gpuBytes();
    }
    // The new cubes are all in; the renderer switches over at the next update, and the old
    // ones are freed once no frame reads them (destroyTexture waits for that).
    if (!m_bound.empty()) renderer.setReflectionProbes({});
    m_bound.clear();
    if (m_loaded) free(*m_loaded);
    m_loaded = std::move(next);
    m_status.generation++;
    m_loadState = State::None;
    m_loadError.clear();
}

void ReflectionProbesRuntime::update(
    const World& world, const Assets& assets, Renderer& renderer, double dt, double interval) {
    m_renderer = &renderer;
    const BakedLighting* bake = sceneBake(world);
    const uint64_t generation = m_status.generation;
    m_status = {};
    m_status.generation = generation;
    auto bind = [&](std::vector<ReflectionProbeDesc> probes) {
        if (probes.size() == m_bound.size() &&
            std::equal(probes.begin(), probes.end(), m_bound.begin(), [](const auto& a, const auto& b) {
                return a.cube.id == b.cube.id && a.boxToWorld == b.boxToWorld && a.halfExtents == b.halfExtents &&
                       a.capturePosition == b.capturePosition && a.blendDistance == b.blendDistance &&
                       a.priority == b.priority && a.order == b.order;
            }))
            return;
        m_bound = std::move(probes);
        renderer.setReflectionProbes(m_bound);
    };
    if (!bake || bake->reflections.empty()) {
        if (m_loaded || !m_bound.empty()) release();
        m_status.generation = generation;
        return;
    }
    m_status.asset = bake->reflections;
    const fs::path contentRoot = assets.contentRoot();
    std::error_code ec;
    m_poll += dt;
    bool polled = false;
    if (bake->reflections != m_asset || m_forceReload || m_poll >= interval) {
        m_poll = 0.0;
        polled = true;
        const auto stamp = fs::last_write_time(contentRoot / bake->reflections, ec);
        if (bake->reflections != m_asset || m_forceReload || ec || stamp != m_stamp) {
            if (bake->reflections != m_asset && m_loaded) {
                bind({});
                free(*m_loaded);
                m_loaded.reset();
            }
            m_asset = bake->reflections;
            m_stamp = ec ? fs::file_time_type{} : stamp;
            m_forceReload = false;
            load(assets, renderer, bake->reflections);
        }
    }
    const ReflectionSceneDescription scene = describeReflectionProbes(world, assets, contentRoot, polled);
    m_status.probes = uint32_t(scene.probes.size());
    if (!m_loaded) {
        bind({});
        m_status.state = m_loadState == State::None ? State::Missing : m_loadState;
        m_status.reason = m_loadError.empty() ? "not captured yet (no " + bake->reflections + ")" : m_loadError;
        if (m_status.reason != m_lastReport) SW_LOG(LogRender, Warning, "reflection probes: {}", m_status.reason);
        m_lastReport = m_status.reason;
        return;
    }
    const Loaded& loaded = *m_loaded;
    m_status.captured = uint32_t(loaded.manifest.probes.size());
    m_status.gpuBytes = loaded.gpuBytes;
    if (polled) m_staleScene = scene.sceneHash != loaded.manifest.sceneHash;
    // Probes still where and as they were captured; moved or edited ones are left out (their
    // surfaces get the global environment) until they're captured again.
    std::vector<ReflectionProbeDesc> probes;
    uint32_t changed = 0;
    for (const ReflectionProbeSource& s : scene.probes) {
        const Loaded::Cube* cube = nullptr;
        for (const Loaded::Cube& c : loaded.cubes)
            if (c.binding == s.binding) cube = &c;
        if (!cube) continue;
        if (cube->probeHash != s.hash) {
            ++changed;
            continue;
        }
        ReflectionProbeDesc desc = s.desc;
        desc.cube = cube->texture;
        probes.push_back(desc);
    }
    m_status.bound = uint32_t(probes.size());
    bind(std::move(probes));
    const uint32_t uncaptured = m_status.probes - m_status.bound - changed;
    if (m_staleScene || changed || uncaptured) {
        m_status.state = State::Stale;
        m_status.reason = changed
                              ? std::to_string(changed) + " probes moved or changed since the capture; capture again"
                          : uncaptured ? std::to_string(uncaptured) + " probes aren't captured yet"
                                       : "static objects, lights or the lighting bake changed since the capture";
    } else {
        m_status.state = State::Ready;
    }
    const std::string report = m_status.state == State::Ready ? "" : m_status.reason;
    if (report != m_lastReport && !report.empty()) SW_LOG(LogRender, Warning, "reflection probes: {}", report);
    m_lastReport = report;
}

// ---- Inspect ----

ReflectionInfo inspectReflectionProbes(const World& world, const Assets& assets, const fs::path& contentRoot) {
    ReflectionInfo info;
    const ReflectionSceneDescription scene = describeReflectionProbes(world, assets, contentRoot);
    info.probes = uint32_t(scene.probes.size());
    info.budgetBytes = scene.budgetBytes;
    info.warnings = scene.warnings;
    const BakedLighting* bake = sceneBake(world);
    if (!bake || bake->reflections.empty()) {
        if (!scene.probes.empty()) {
            info.state = ReflectionInfo::State::Missing;
            info.reason = "the scene has reflection probes but no BakedLighting reflections; capture them";
        }
        return info;
    }
    info.asset = bake->reflections;
    std::string error;
    std::error_code ec;
    if (!loadReflectionManifest(contentRoot / bake->reflections, info.manifest, error)) {
        const bool exists = fs::exists(contentRoot / bake->reflections, ec);
        info.state = exists ? ReflectionInfo::State::Invalid : ReflectionInfo::State::Missing;
        info.reason = exists ? error : "not captured yet (no " + bake->reflections + ")";
        info.broken = exists;
        return info;
    }
    for (const ReflectionProbeRecord& r : info.manifest.probes) {
        std::vector<uint8_t> bytes;
        ReflectionCube cube;
        if (!readLightingFile(contentRoot / r.path, bytes, error) || hashLightingBytes(bytes) != r.hash ||
            bytes.size() != r.bytes || !decodeReflectionKtx2(bytes, cube, error)) {
            info.state = ReflectionInfo::State::Invalid;
            info.broken = true;
            info.reason = r.path + (error.empty() ? " doesn't match its manifest; capture again" : ": " + error);
            return info;
        }
        info.payloadBytes += bytes.size();
    }
    for (const ReflectionProbeSource& s : scene.probes) {
        const ReflectionProbeRecord* r = info.manifest.probe(s.binding);
        if (!r)
            info.uncapturedProbes.push_back(label(world, s.entity));
        else if (r->probeHash != s.hash)
            info.changedProbes.push_back(label(world, s.entity));
        else
            ++info.currentProbes;
    }
    info.sceneChanged = scene.sceneHash != info.manifest.sceneHash;
    if (!info.changedProbes.empty() || !info.uncapturedProbes.empty() || info.sceneChanged) {
        info.state = ReflectionInfo::State::Stale;
        info.reason = !info.changedProbes.empty()
                          ? std::to_string(info.changedProbes.size()) + " probes moved or changed since the capture"
                      : !info.uncapturedProbes.empty()
                          ? std::to_string(info.uncapturedProbes.size()) + " probes aren't captured yet"
                          : "static objects, lights or the lighting bake changed since the capture";
        info.reason += "; capture again";
    } else {
        info.state = ReflectionInfo::State::Ready;
    }
    return info;
}

// ---- Capture ----

bool ReflectionCapture::start(
    const World& world, const Assets& assets, Renderer& renderer, const fs::path& contentRoot,
    const std::string& scenePath, std::string& error, const std::vector<BindingId>& only) {
    if (!renderer.supportsTextureShape(TextureShape::Cube)) {
        error = "reflection capture needs a GPU renderer (this one has no cube maps or scene readback)";
        return false;
    }
    ReflectionSceneDescription scene = describeReflectionProbes(world, assets, contentRoot);
    for (const std::string& w : scene.warnings) SW_LOG(LogRender, Warning, "reflection capture: {}", w);
    m_probes.clear();
    for (ReflectionProbeSource& s : scene.probes)
        if (only.empty() || std::find(only.begin(), only.end(), s.binding) != only.end()) m_probes.push_back(s);
    if (m_probes.empty()) {
        error = scene.probes.empty() ? "the scene has no enabled ReflectionProbe with a binding id"
                                     : "none of the probes asked for are in the scene";
        return false;
    }
    m_contentRoot = contentRoot;
    m_scene = scenePath;
    m_manifestPath = reflectionManifestPath(world, scenePath);
    m_sceneHash = scene.sceneHash;
    m_manifest = {};
    // Capturing some probes keeps the others' last capture, when it was of the same scene.
    if (!only.empty()) {
        ReflectionManifest old;
        std::string ignored;
        if (loadReflectionManifest(contentRoot / m_manifestPath, old, ignored) && old.sceneHash == m_sceneHash)
            for (const ReflectionProbeRecord& r : old.probes)
                if (std::find(only.begin(), only.end(), r.binding) == only.end()) m_manifest.probes.push_back(r);
    }
    m_manifest.scene = scenePath;
    m_manifest.sceneHash = m_sceneHash;
    m_manifest.tool = "shockwave reflections 1";
    m_probe = 0;
    m_face = 0;
    m_waiting = false;
    m_faces.clear();
    m_lines.clear();
    m_bytes = 0;
    m_running = true;
    m_finished = m_ok = false;
    m_cancelled = false;
    m_error.clear();
    m_started = std::chrono::steady_clock::now();
    return true;
}

void ReflectionCapture::fail(const std::string& message) {
    m_error = message;
    m_running = false;
    m_finished = true;
    m_ok = false;
}

float ReflectionCapture::fraction() const {
    if (m_probes.empty()) return 0.0f;
    return std::min(1.0f, (float(m_probe) + float(m_face) / 6.0f) / float(m_probes.size()));
}

std::string ReflectionCapture::stage() const {
    if (!m_running) return m_finished ? (m_ok ? "done" : "failed") : "idle";
    if (m_probe >= m_probes.size()) return "writing";
    return "probe " + std::to_string(m_probe + 1) + " of " + std::to_string(m_probes.size()) + ", face " +
           std::to_string(std::min(m_face + 1, 6u));
}

std::string ReflectionCapture::report() const {
    std::ostringstream out;
    out << "manifest " << m_manifestPath << "\n";
    for (const std::string& line : m_lines) out << line << "\n";
    out << "cubes " << (m_bytes + 1023) / 1024 << " KiB on disk; took "
        << std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count() << " s\n";
    return out.str();
}

bool ReflectionCapture::finishProbe() {
    const ReflectionProbeSource& s = m_probes[m_probe];
    const uint32_t size = reflectionProbeSize(s.probe.resolution);
    ReflectionCube cube;
    cube.size = size;
    cube.levels.push_back(std::move(m_faces));
    m_faces.clear();
    prefilterReflectionCube(cube);
    std::vector<uint8_t> bytes;
    std::string error;
    if (!encodeReflectionKtx2(cube, bytes, error)) {
        fail(error);
        return false;
    }
    const std::string path = reflectionCubePath(m_manifestPath, s.binding);
    std::error_code ec;
    fs::create_directories((m_contentRoot / path).parent_path(), ec);
    if (!writeLightingFileAtomic(m_contentRoot / path, bytes, error)) {
        fail(error);
        return false;
    }
    std::erase_if(m_manifest.probes, [&](const ReflectionProbeRecord& r) { return r.binding == s.binding; });
    m_manifest.probes.push_back({s.binding, path, hashLightingBytes(bytes), bytes.size(), size, s.hash});
    m_bytes += bytes.size();
    const glm::vec3 average = [&] {
        glm::vec3 sum(0.0f);
        for (const glm::vec3& m : reflectionFaceMeans(cube)) sum += m;
        return sum / 6.0f;
    }();
    char line[256];
    std::snprintf(
        line, sizeof(line), "probe %s %u x 6 faces, mean (%.3f, %.3f, %.3f)", formatBindingId(s.binding).c_str(), size,
        double(average.r), double(average.g), double(average.b));
    m_lines.push_back(line);
    return true;
}

bool ReflectionCapture::writeManifest() {
    std::sort(m_manifest.probes.begin(), m_manifest.probes.end(), [](const auto& a, const auto& b) {
        return a.binding < b.binding;
    });
    const std::string text = reflectionManifestText(m_manifest);
    ReflectionManifest check;
    std::string error;
    if (!parseReflectionManifest(text, m_manifestPath, check, error)) {
        fail(error);
        return false;
    }
    std::error_code ec;
    fs::create_directories((m_contentRoot / m_manifestPath).parent_path(), ec);
    if (!writeLightingFileAtomic(
            m_contentRoot / m_manifestPath, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()),
            error)) {
        fail(error);
        return false;
    }
    return true;
}

bool ReflectionCapture::render(
    World& world, Renderer& renderer, JobSystem& jobs, const Assets& assets, const BakedLightingRuntime* baked) {
    if (!m_running) return false;
    if (m_cancelled) {
        fail("cancelled; the last capture is still in place");
        return false;
    }
    if (m_waiting) {
        if (renderer.sceneCapturePending()) return true;   // the frame was skipped: keep drawing it
        m_waiting = false;
        const SceneCapture capture = renderer.takeSceneCapture();
        const ReflectionProbeSource& s = m_probes[m_probe];
        glm::mat4 view, projection;
        reflectionFaceCamera(m_face, s.desc.capturePosition, s.probe.nearPlane, s.probe.farPlane, view, projection);
        std::string error;
        if (!resampleReflectionFace(
                capture, m_face, projection * view, reflectionProbeSize(s.probe.resolution), m_faces, error)) {
            fail("capturing face " + std::to_string(m_face) + ": " + error);
            return false;
        }
        if (++m_face == 6) {
            if (!finishProbe()) return false;
            m_face = 0;
            ++m_probe;
        }
        if (m_probe == m_probes.size()) {
            if (writeManifest()) {
                m_ok = true;
                m_running = false;
                m_finished = true;
            }
            return false;
        }
    }
    // The next face: its camera, the readback, and only what belongs in a reflection.
    const ReflectionProbeSource& s = m_probes[m_probe];
    glm::mat4 view, projection;
    reflectionFaceCamera(m_face, s.desc.capturePosition, s.probe.nearPlane, s.probe.farPlane, view, projection);
    renderer.setCamera(view, projection);
    std::string error;
    if (!renderer.requestSceneCapture(error)) {
        fail(error);
        return false;
    }
    renderWorld(world, renderer, jobs, 1.0, &assets, baked, &reflectionCaptureIncludes);
    m_waiting = true;
    return true;
}

}   // namespace shockwave
