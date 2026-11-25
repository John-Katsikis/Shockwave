// The Foliage component, its drawing and checks: see Engine/Foliage.h and docs/Foliage.md.
#include <Engine/Foliage.h>

#include <Assets/Assets.h>
#include <Assets/FoliageData.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Renderer/Renderer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace shockwave {

namespace {

FoliageStats g_stats;

// The view's four side planes (ax + by + cz + d >= 0 inside), from its view-projection
// matrix. Near and far are left out: the types' distances already limit how far it goes.
std::array<glm::vec4, 4> sidePlanes(const glm::mat4& vp) {
    auto row = [&](int i) { return glm::vec4(vp[0][i], vp[1][i], vp[2][i], vp[3][i]); };
    const glm::vec4 w = row(3), x = row(0), y = row(1);
    std::array<glm::vec4, 4> planes{w + x, w - x, w + y, w - y};
    for (glm::vec4& p : planes) {
        const float length = glm::length(glm::vec3(p));
        if (length > 0.0f) p /= length;
    }
    return planes;
}

// Whether a world-space box (centre, half size) is at least partly inside every plane.
bool boxInside(const std::array<glm::vec4, 4>& planes, const glm::vec3& centre, const glm::vec3& half) {
    for (const glm::vec4& p : planes) {
        const float reach = std::abs(p.x) * half.x + std::abs(p.y) * half.y + std::abs(p.z) * half.z;
        if (glm::dot(glm::vec3(p), centre) + p.w < -reach) return false;
    }
    return true;
}

// The largest distance from a model's origin to a corner of its bounds.
float modelReach(const ModelAsset& model) {
    const Bounds& b = model.data.bounds;
    return glm::length(glm::max(glm::abs(b.min), glm::abs(b.max)));
}

std::string entityName(const World& world, Entity e) {
    const Name* n = world.get<Name>(e);
    return "entity '" + (n && !n->value.empty() ? n->value : std::to_string(e.index)) + "'";
}

}   // namespace

void registerFoliageComponents() {
    static bool done = false;
    if (done) return;
    done = true;
    registerComponent<Foliage>(
        "Foliage",
        {
            field("asset", &Foliage::asset),
            field("visible", &Foliage::visible),
        },
        "Rendering");
}

const FoliageStats& lastFoliageStats() { return g_stats; }

void foliageCellKeeps(
    const FoliageData& foliage, size_t cell, float distance, float density, float distanceScale,
    std::vector<uint32_t>& keep) {
    keep.clear();
    if (cell >= foliage.cells.size()) return;
    const FoliageCell& c = foliage.cells[cell];
    if (c.type >= foliage.types.size()) return;
    const FoliageType& type = foliage.types[c.type];
    const float maxDistance = type.distance * distanceScale;
    const float fade = std::min(type.fade * distanceScale, maxDistance);
    if (distance >= maxDistance || !(density > 0.0f)) return;
    const float fraction = foliageFade(distance, maxDistance, fade);
    const uint32_t end = std::min<uint32_t>(c.first + c.count, uint32_t(foliage.instances.size()));
    for (uint32_t i = c.first; i < end; ++i) {
        const float random = foliage.instances[i].random;
        if (foliageDense(random, density) && foliageSurvivesFade(random, fraction)) keep.push_back(i);
    }
}

void drawFoliage(
    const World& world, Renderer& renderer, const Assets& assets,
    const std::function<bool(const World&, Entity)>& include) {
    const auto started = std::chrono::steady_clock::now();
    FoliageStats stats;
    if (world.count<Foliage>() == 0) {
        g_stats = stats;
        return;
    }
    const FoliageSettings settings = renderer.foliageSettings();
    const glm::mat4 view = renderer.cameraView();
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    const auto planes = sidePlanes(renderer.cameraProjection() * view);

    // Per type, the instances kept near and far (beyond half its distance: simplest level).
    thread_local std::vector<uint32_t> keep;
    thread_local std::vector<std::vector<uint32_t>> nearKept, farKept;

    const Pool<Foliage>* pool = world.pool<Foliage>();
    for (size_t at = 0; pool && at < pool->size(); ++at) {
        const Entity e = pool->entities()[at];
        const Foliage& placed = pool->components()[at];
        if (!world.has<Transform>(e)) continue;
        if (!placed.visible || placed.asset.empty() || (include && !include(world, e))) continue;
        const auto foliage = assets.foliage(placed.asset);
        if (!foliage || foliage->types.empty()) continue;
        ++stats.foliages;
        const glm::mat4 entity = worldMatrix(world, e);
        const glm::vec3 entityScale(
            glm::length(glm::vec3(entity[0])), glm::length(glm::vec3(entity[1])), glm::length(glm::vec3(entity[2])));
        const float scaleMax = std::max({entityScale.x, entityScale.y, entityScale.z});
        // The camera in the entity's space, for distances that ignore its scale.
        const glm::vec3 localEye = glm::vec3(glm::inverse(entity) * glm::vec4(eye, 1.0f));
        const float localScale = scaleMax > 0.0f ? 1.0f / scaleMax : 1.0f;

        const size_t typeCount = foliage->types.size();
        std::vector<std::shared_ptr<const ModelAsset>> models(typeCount);
        std::vector<float> reach(typeCount, 0.0f);
        for (size_t t = 0; t < typeCount; ++t) {
            models[t] = assets.model(assets.findModel(foliage->types[t].model));
            if (models[t]) reach[t] = modelReach(*models[t]);
        }
        nearKept.resize(typeCount);
        farKept.resize(typeCount);
        for (size_t t = 0; t < typeCount; ++t) {
            nearKept[t].clear();
            farKept[t].clear();
        }

        for (size_t c = 0; c < foliage->cells.size(); ++c) {
            const FoliageCell& cell = foliage->cells[c];
            if (cell.type >= typeCount || !models[cell.type]) continue;
            stats.instances += cell.count;
            const FoliageType& type = foliage->types[cell.type];
            const float maxDistance = type.distance * settings.distance;
            // Metres in the world: the entity's scale stretches the cell's box.
            const float distance = foliageCellDistance(cell, localEye) / localScale;
            if (distance >= maxDistance) continue;
            ++stats.cells;
            const float grow = reach[cell.type] * cell.maxScale;
            const glm::vec3 lo = cell.bounds.min - glm::vec3(grow), hi = cell.bounds.max + glm::vec3(grow);
            const glm::vec3 centre = glm::vec3(entity * glm::vec4((lo + hi) * 0.5f, 1.0f));
            const glm::mat3 axes(entity);
            const glm::vec3 h = (hi - lo) * 0.5f;
            const glm::vec3 half = glm::abs(axes[0]) * h.x + glm::abs(axes[1]) * h.y + glm::abs(axes[2]) * h.z;
            if (!boxInside(planes, centre, half)) continue;
            ++stats.cellsDrawn;
            foliageCellKeeps(*foliage, c, distance, settings.density, settings.distance, keep);
            auto& into = distance > maxDistance * 0.5f ? farKept[cell.type] : nearKept[cell.type];
            into.insert(into.end(), keep.begin(), keep.end());
        }

        // One call per model part for each type's near and far sets, in chunks of up to
        // kChunk instances so the transforms being built stay in cache.
        struct Batch {
            const ModelAsset* model;
            const ModelNode* node;
            uint32_t mesh;
            DrawMaterial options;
            TextureHandle texture;
            glm::vec4 baseColor;
            RenderInstanceKey first;
            const std::vector<uint32_t>* kept;
            size_t begin, end;
            int lodLevel;
        };
        constexpr size_t kChunk = 2048;
        std::vector<Batch> batches;
        for (size_t t = 0; t < typeCount; ++t) {
            if (nearKept[t].empty() && farKept[t].empty()) continue;
            const ModelAsset& model = *models[t];
            const FoliageType& type = foliage->types[t];
            uint32_t slot = 0;
            for (size_t n = 0; n < model.data.nodes.size(); ++n) {
                const auto& node = model.data.nodes[n];
                for (const auto& index : node.meshes) {
                    const uint32_t part = slot++;
                    const auto& mesh = model.data.meshes[index];
                    if (mesh.skinned() || mesh.material >= model.data.materials.size()) continue;
                    if (index >= model.meshes.size() || !model.meshes[index].valid()) continue;
                    const auto& material = model.data.materials[mesh.material];
                    Batch batch{};
                    batch.model = &model;
                    batch.node = &node;
                    batch.mesh = uint32_t(index);
                    batch.options = modelDrawMaterial(model, mesh.material);
                    batch.options.castShadows = type.shadows;
                    batch.options.wind = type.wind;
                    batch.options.decalLayers = 0;   // impact marks don't land on foliage
                    const int baseMap = material.baseColorTexture;
                    batch.texture = baseMap < 0 || size_t(baseMap) >= model.textures.size()
                                        ? TextureHandle{}
                                        : model.textures[size_t(baseMap)];
                    batch.baseColor = material.baseColor;
                    // Each instance keeps its key whichever set it's in: the part in the top
                    // byte, its place in the sorted instances below.
                    batch.first = {world.lifetime(), e.index, e.generation, (part & 0xFFu) << 24, 0};
                    for (int far = 0; far < 2; ++far) {
                        const std::vector<uint32_t>& kept = far ? farKept[t] : nearKept[t];
                        batch.kept = &kept;
                        batch.lodLevel = far ? 99 : -1;
                        for (size_t from = 0; from < kept.size(); from += kChunk) {
                            batch.begin = from;
                            batch.end = std::min(kept.size(), from + kChunk);
                            batches.push_back(batch);
                        }
                    }
                }
            }
            stats.instancesDrawn += uint32_t(nearKept[t].size() + farKept[t].size());
        }
        auto submit = [&](size_t from, size_t to) {
            thread_local std::vector<glm::mat4> transforms;
            thread_local std::vector<uint32_t> parts;
            for (size_t b = from; b < to; ++b) {
                const Batch& batch = batches[b];
                const bool placedNode = batch.node->world != glm::mat4(1.0f);
                const bool placedEntity = entity != glm::mat4(1.0f);
                transforms.clear();
                parts.clear();
                for (size_t k = batch.begin; k < batch.end; ++k) {
                    const uint32_t i = (*batch.kept)[k];
                    const glm::mat4 own = foliageMatrix(foliage->instances[i]);
                    const glm::mat4 m = placedEntity ? entity * own : own;
                    transforms.push_back(placedNode ? m * batch.node->world : m);
                    parts.push_back(i & 0xFFFFFFu);
                }
                renderer.drawMeshInstances(
                    batch.model->meshes[batch.mesh], transforms, batch.texture, batch.baseColor, batch.options,
                    batch.first, batch.lodLevel, parts);
            }
        };
        // On one thread: shared out across the job system it was slower, with the GPU driver
        // (or llvmpipe) busy on the other cores.
        submit(0, batches.size());
        stats.calls += uint32_t(batches.size());
    }
    stats.submitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    g_stats = stats;
}

std::vector<std::string> foliageProblems(
    const World& world, const Assets& assets, const std::function<std::string(std::string_view)>& whyNotLoaded) {
    std::vector<std::string> problems;
    world.forEachEntity([&](Entity e) {
        const Foliage* found = world.get<Foliage>(e);
        if (!found) return;
        const Foliage& placed = *found;
        if (placed.asset.empty()) {
            problems.push_back(entityName(world, e) + ": the Foliage names no .swfoliage asset");
            return;
        }
        const auto foliage = assets.foliage(placed.asset);
        if (!foliage) {
            const std::string why = whyNotLoaded ? whyNotLoaded(placed.asset) : std::string();
            problems.push_back(
                entityName(world, e) + ": the foliage " + placed.asset + " isn't loaded" +
                (why.empty() ? "" : ": " + why));
            return;
        }
        for (size_t t = 0; t < foliage->types.size(); ++t) {
            const std::string& model = foliage->types[t].model;
            if (!assets.model(assets.findModel(model))) {
                const std::string why = whyNotLoaded ? whyNotLoaded(model) : std::string();
                problems.push_back(
                    placed.asset + ": type " + std::to_string(t) + "'s model " + model + " isn't loaded" +
                    (why.empty() ? "" : ": " + why));
            }
        }
    });
    return problems;
}

}   // namespace shockwave
