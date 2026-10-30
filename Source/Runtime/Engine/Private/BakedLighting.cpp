#include <Engine/BakedLighting.h>

#include <Assets/Assets.h>
#include <Assets/Material.h>
#include <Assets/Model.h>
#include <Core/Log.h>
#include <Engine/Hierarchy.h>
#include <Engine/Prefab.h>
#include <Renderer/Renderer.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <system_error>

namespace shockwave {

namespace fs = std::filesystem;

// ---- Identity ----

namespace {
// Copy roots enclosing e, innermost first (e itself when it is one).
std::vector<Entity> prefabChain(const World& world, Entity e) {
    std::vector<Entity> chain;
    if (world.has<PrefabInstance>(e)) chain.push_back(e);
    for (Entity owner = prefabOwnerOf(world, e); owner.valid() && chain.size() < 64;
         owner = prefabOwnerOf(world, owner))
        chain.push_back(owner);
    return chain;
}
std::string entityLabel(const World& world, Entity e) {
    const Name* name = world.get<Name>(e);
    return name && !name->value.empty() ? "\"" + name->value + "\"" : "entity " + std::to_string(e.index);
}
bool participates(const World& world, Entity e) {
    if (const BakeGeometry* g = world.get<BakeGeometry>(e))
        if (g->mobility == BakeGeometry::Mobility::Static && (g->castIndirect || g->receiveLightmap) &&
            (world.has<MeshRenderer>(e) || world.has<ModelRenderer>(e)))
            return true;
    if (const DiffuseProbeVolume* v = world.get<DiffuseProbeVolume>(e); v && v->enabled) return true;
    if (const ReflectionProbe* r = world.get<ReflectionProbe>(e)) return r->enabled;
    return false;
}
std::string* localId(World& world, Entity e) {
    if (BakeGeometry* g = world.get<BakeGeometry>(e)) return &g->bindingId;
    if (DiffuseProbeVolume* v = world.get<DiffuseProbeVolume>(e)) return &v->bindingId;
    if (ReflectionProbe* r = world.get<ReflectionProbe>(e)) return &r->bindingId;
    return nullptr;
}
const std::string* localId(const World& world, Entity e) {
    if (const BakeGeometry* g = world.get<BakeGeometry>(e)) return &g->bindingId;
    if (const DiffuseProbeVolume* v = world.get<DiffuseProbeVolume>(e)) return &v->bindingId;
    if (const ReflectionProbe* r = world.get<ReflectionProbe>(e)) return &r->bindingId;
    return nullptr;
}
std::vector<Entity> participants(const World& world) {
    std::vector<Entity> out;
    world.forEachEntity([&](Entity e) {
        if (participates(world, e)) out.push_back(e);
    });
    return out;
}
}   // namespace

BindingId effectiveBindingId(const World& world, Entity e, const std::string& local, std::string* problem) {
    BindingId id;
    if (local.empty()) {
        if (problem) *problem = entityLabel(world, e) + " has no binding_id";
        return {};
    }
    if (!parseBindingId(local, id) || !id.valid()) {
        if (problem) *problem = entityLabel(world, e) + " has a binding_id that isn't 32 lowercase hex digits";
        return {};
    }
    for (Entity root : prefabChain(world, e)) {
        const PrefabInstance* instance = world.get<PrefabInstance>(root);
        BindingId copy;
        if (!instance || instance->bakeId.empty() || !parseBindingId(instance->bakeId, copy) || !copy.valid()) {
            if (problem)
                *problem = entityLabel(world, e) + " is in prefab copy " + entityLabel(world, root) +
                           ", which has no valid bake_id";
            return {};
        }
        id = deriveBindingId(copy, id);
    }
    return id;
}

std::vector<std::string> checkLightingIds(const World& world) {
    std::vector<std::string> problems;
    std::map<BindingId, Entity> seen;
    for (Entity e : participants(world)) {
        std::string problem;
        const BindingId id = effectiveBindingId(world, e, *localId(world, e), &problem);
        if (!id.valid()) {
            problems.push_back(problem);
            continue;
        }
        auto [it, fresh] = seen.emplace(id, e);
        if (!fresh)
            problems.push_back(
                entityLabel(world, e) + " has the same binding id as " + entityLabel(world, it->second) +
                " (duplicated without a fresh id?)");
    }
    return problems;
}

size_t assignLightingIds(World& world) {
    size_t changed = 0;
    std::set<BindingId> seen;
    for (Entity e : participants(world)) {
        // Prefab copies get their own id; parts keep the prefab file's, which must be set there.
        for (Entity root : prefabChain(world, e)) {
            PrefabInstance* instance = world.get<PrefabInstance>(root);
            BindingId id;
            if (instance && !world.has<PrefabPart>(root) && (!parseBindingId(instance->bakeId, id) || !id.valid())) {
                instance->bakeId = formatBindingId(newBindingId());
                ++changed;
            }
        }
        std::string* local = localId(world, e);
        if (!world.has<PrefabPart>(e)) {
            BindingId id;
            if (!parseBindingId(*local, id) || !id.valid()) {
                *local = formatBindingId(newBindingId());
                ++changed;
            }
        }
        BindingId effective = effectiveBindingId(world, e, *local);
        if (effective.valid() && !seen.insert(effective).second && !world.has<PrefabPart>(e)) {
            // A duplicate: the first keeps its id, this one gets a fresh one.
            *local = formatBindingId(newBindingId());
            ++changed;
            effective = effectiveBindingId(world, e, *local);
            seen.insert(effective);
        }
    }
    return changed;
}

void refreshLightingIds(World& world, Entity e) {
    for (Entity x : subtree(world, e)) {
        if (world.has<PrefabPart>(x)) continue;
        if (std::string* local = localId(world, x); local && !local->empty()) *local = formatBindingId(newBindingId());
        if (PrefabInstance* instance = world.get<PrefabInstance>(x); instance && !instance->bakeId.empty())
            instance->bakeId = formatBindingId(newBindingId());
    }
}

// ---- Description ----

glm::vec3 BakeMaterial::reflectance(const glm::vec3& texel) const {
    return glm::clamp(albedo * texel * (1.0f - std::clamp(metallic, 0.0f, 1.0f)) * 0.96f, 0.0f, 0.96f);
}

namespace {
float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

struct DecodedCache {
    std::mutex mutex;
    struct Entry {
        fs::file_time_type stamp{};
        uintmax_t size = 0;
        std::shared_ptr<const Image> image;
        std::shared_ptr<const HdrImage> hdr;
        uint64_t hash = 0;
    };
    std::map<std::string, Entry> entries;
};
DecodedCache& decoded() {
    static DecodedCache cache;
    return cache;
}
// A texture or environment map file, decoded and hashed once per version of the file.
const DecodedCache::Entry* decodeFile(const fs::path& file, bool hdr) {
    std::error_code ec;
    const auto stamp = fs::last_write_time(file, ec);
    if (ec) return nullptr;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec) return nullptr;
    DecodedCache& cache = decoded();
    std::lock_guard lock(cache.mutex);
    const std::string key = file.generic_string() + (hdr ? "#hdr" : "");
    auto it = cache.entries.find(key);
    if (it != cache.entries.end() && it->second.stamp == stamp && it->second.size == size) return &it->second;
    DecodedCache::Entry entry;
    entry.stamp = stamp;
    entry.size = size;
    std::vector<uint8_t> bytes;
    std::string error;
    if (!readLightingFile(file, bytes, error)) return nullptr;
    entry.hash = hashLightingBytes(bytes);
    if (hdr && isHdrImage(bytes)) {
        auto image = std::make_shared<HdrImage>();
        if (decodeHdrImage(bytes, *image, error)) entry.hdr = image;
    } else {
        auto image = std::make_shared<Image>();
        if (decodeImage(bytes, *image, error)) {
            if (hdr) {
                // An 8-bit environment map is sRGB, as the renderer reads it.
                auto linear = std::make_shared<HdrImage>();
                linear->width = image->width;
                linear->height = image->height;
                linear->rgba.resize(image->rgba.size());
                for (size_t i = 0; i < image->rgba.size(); ++i)
                    linear->rgba[i] = i % 4 == 3 ? image->rgba[i] / 255.0f : srgbToLinear(image->rgba[i] / 255.0f);
                entry.hdr = linear;
            } else {
                entry.image = image;
            }
        }
    }
    return &(cache.entries[key] = std::move(entry));
}

LightingHasher& hashMaterial(LightingHasher& h, const BakeMaterial& m, std::string_view texture) {
    h.vec(m.albedo)
        .f32(m.alpha)
        .f32(m.metallic)
        .vec(m.emissive)
        .f32(m.alphaCutoff)
        .u32(m.blend)
        .u32(m.doubleSided)
        .text(texture);
    h.u32(m.binding.uvSet)
        .vec(m.binding.transform.offset)
        .vec(m.binding.transform.scale)
        .f32(m.binding.transform.rotation);
    return h.vec(m.uvScale);
}

struct MeshHashCache {
    std::mutex mutex;
    std::map<const MeshData*, std::pair<std::weak_ptr<const void>, uint64_t>> entries;
};
uint64_t cachedMeshHash(const std::shared_ptr<const void>& owner, const MeshData& mesh) {
    static MeshHashCache cache;
    std::lock_guard lock(cache.mutex);
    auto it = cache.entries.find(&mesh);
    if (it != cache.entries.end() && it->second.first.lock() == owner) return it->second.second;
    const uint64_t hash = lightingMeshHash(mesh);
    if (cache.entries.size() > 4096) cache.entries.clear();
    cache.entries[&mesh] = {owner, hash};
    return hash;
}

LightDesc describeLight(const World& world, Entity e, const Light& light) {
    const glm::mat4 m = worldMatrix(world, e);
    LightDesc desc;
    desc.type = static_cast<LightDesc::Type>(light.type);
    desc.position = glm::vec3(m[3]);
    desc.direction = glm::normalize(-glm::vec3(m[2]));
    desc.color = glm::vec3(light.color) * light.intensity;
    desc.range = light.range;
    const float outer = glm::radians(std::clamp(light.spotAngle, 1.0f, 89.0f));
    desc.cosOuter = std::cos(outer);
    desc.cosInner = std::cos(outer * (1.0f - std::clamp(light.spotSoftness, 0.0f, 1.0f)));
    return desc;
}
}   // namespace

glm::vec3 BakeEnvironment::radiance(const glm::vec3& d) const {
    if (!map || map->width == 0 || map->height == 0) {
        const float t = std::clamp((d.y + 0.02f) / 0.04f, 0.0f, 1.0f);
        const float s = t * t * (3.0f - 2.0f * t);
        return glm::max(glm::mix(ground, sky, s), glm::vec3(0.0f));
    }
    const float c = std::cos(rotation), s = std::sin(rotation);
    const glm::vec3 p(c * d.x - s * d.z, d.y, s * d.x + c * d.z);
    const float u = 0.5f + std::atan2(p.x, -p.z) / glm::two_pi<float>();
    const float v = std::acos(std::clamp(p.y, -1.0f, 1.0f)) / glm::pi<float>();
    // Bilinear, wrapping around horizontally and clamped at the poles.
    const float x = u * float(map->width) - 0.5f, y = v * float(map->height) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    glm::vec3 sum(0.0f);
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int px = (int(fx) + i) % int(map->width);
            if (px < 0) px += int(map->width);
            const int py = std::clamp(int(fy) + j, 0, int(map->height) - 1);
            const float w = (i ? x - fx : 1.0f - (x - fx)) * (j ? y - fy : 1.0f - (y - fy));
            const float* texel = &map->rgba[(size_t(py) * map->width + size_t(px)) * 4];
            sum += glm::vec3(texel[0], texel[1], texel[2]) * w;
        }
    return glm::max(sum * intensity, glm::vec3(0.0f));
}

const BakeSurface* BakeSceneDescription::surface(const BindingId& binding, uint32_t part) const {
    for (const BakeSurface& s : surfaces)
        if (s.binding == binding && s.part == part) return &s;
    return nullptr;
}

uint64_t lightingMeshHash(const MeshData& mesh) {
    LightingHasher h;
    h.u64(mesh.vertices.size()).u64(mesh.indices.size()).u64(mesh.uv1.size());
    for (const Vertex& v : mesh.vertices) h.vec(v.position).vec(v.normal).vec(v.uv).f32(v.color.a);
    for (uint32_t i : mesh.indices) h.u32(i);
    for (const glm::vec2& uv : mesh.uv1) h.vec(uv);
    return h.value();
}

uint64_t lightingManifestHash(uint64_t sceneLightingHash, const LightingManifest& m) {
    return LightingHasher()
        .u64(sceneLightingHash)
        .u32(m.algorithm)
        .text(m.quality)
        .u32(m.samples)
        .u32(m.bounces)
        .u32(m.seed)
        .u32(m.atlasSize)
        .u32(m.probeRays)
        .value();
}

bool receiverMesh(const MeshData& source, const LightingReceiver& receiver, MeshData& out) {
    MeshData mesh;
    mesh.vertices.reserve(receiver.sourceVertex.size());
    for (uint32_t s : receiver.sourceVertex) {
        if (s >= source.vertices.size()) return false;
        mesh.vertices.push_back(source.vertices[s]);
        if (source.uv1.size() == source.vertices.size()) mesh.uv1.push_back(source.uv1[s]);
    }
    mesh.indices = receiver.indices;
    for (uint32_t i : mesh.indices)
        if (i >= mesh.vertices.size()) return false;
    mesh.lightmapUv = receiver.lightmapUv;
    out = std::move(mesh);
    return true;
}

BakeSceneDescription describeBakeScene(const World& world, const Assets& assets, const fs::path& contentRoot) {
    BakeSceneDescription d;
    auto textureFile = [&](TextureHandle t) -> std::string { return t.id ? assets.textureName(t) : std::string(); };
    auto loadTexture = [&](const std::string& name) -> std::shared_ptr<const Image> {
        if (name.empty() || contentRoot.empty()) return nullptr;
        const DecodedCache::Entry* entry = decodeFile(contentRoot / name, false);
        if (!entry || !entry->image) {
            d.warnings.push_back(
                "texture \"" + name + "\" couldn't be decoded for the bake; its factors alone are used");
            return nullptr;
        }
        return entry->image;
    };

    // Static surfaces.
    if (const Pool<BakeGeometry>* geometry = world.pool<BakeGeometry>()) {
        const auto entities = geometry->entities();
        for (size_t i = 0; i < entities.size(); ++i) {
            const Entity e = entities[i];
            const BakeGeometry& g = geometry->components()[i];
            if (g.mobility != BakeGeometry::Mobility::Static || (!g.castIndirect && !g.receiveLightmap)) continue;
            if (!world.has<Transform>(e)) continue;
            std::string problem;
            const BindingId binding = effectiveBindingId(world, e, g.bindingId, &problem);
            if (!binding.valid()) {
                d.warnings.push_back(problem + "; it's left out of the bake");
                continue;
            }
            const glm::mat4 toWorld = worldMatrix(world, e);
            const float det = glm::determinant(glm::mat3(toWorld));
            if (!std::isfinite(det) || std::abs(det) < 1e-12f) {
                d.warnings.push_back(entityLabel(world, e) + " has a singular transform; it's left out of the bake");
                continue;
            }
            auto add = [&](BakeSurface s, const std::string& texture) {
                s.entity = e;
                s.binding = binding;
                s.receiver = g.receiveLightmap && !s.material.blend;
                s.occluder = g.castIndirect && !s.material.blend;
                if (s.material.blend)
                    d.warnings.push_back(
                        entityLabel(world, e) + " is see-through (blended): it's left out of the bake");
                if (!s.receiver && !s.occluder) return;
                s.texelsPerMeter = std::clamp(g.texelsPerMeter, 0.25f, 256.0f);
                LightingHasher h;
                h.u64(s.geometryHash).mat(s.toWorld).u32(s.part).u32(s.receiver).u32(s.occluder).f32(s.texelsPerMeter);
                hashMaterial(h, s.material, texture);
                s.hash = h.value();
                d.surfaces.push_back(std::move(s));
            };
            if (const MeshRenderer* m = world.get<MeshRenderer>(e); m && m->visible && m->mesh.valid()) {
                BakeSurface s;
                s.mesh = assets.meshData(m->mesh);
                if (!s.mesh || s.mesh->indices.empty()) {
                    d.warnings.push_back(entityLabel(world, e) + "'s mesh has no CPU copy; it's left out of the bake");
                } else {
                    s.toWorld = toWorld;
                    s.geometryHash = cachedMeshHash(s.mesh, *s.mesh);
                    std::string texture;
                    if (const auto asset = m->material.valid() ? assets.material(m->material) : nullptr) {
                        s.material.albedo = glm::vec3(asset->baseColor * m->tint);
                        s.material.alpha = asset->baseColor.a * m->tint.a;
                        s.material.metallic = asset->draw.metallic;
                        s.material.emissive = asset->draw.emissive;
                        s.material.alphaCutoff = asset->draw.alphaCutoff;
                        s.material.blend = asset->draw.blend;
                        s.material.doubleSided = asset->draw.doubleSided;
                        s.material.binding = asset->draw.bindings[0];
                        texture =
                            "material:" + assets.materialName(m->material) + ":" + textureFile(asset->baseColorTexture);
                        s.material.baseColor = loadTexture(textureFile(asset->baseColorTexture));
                    } else {
                        s.material.albedo = glm::vec3(m->tint);
                        s.material.alpha = m->tint.a;
                        s.material.metallic = m->metallic;
                        s.material.emissive = glm::vec3(m->emissive) * m->emissiveIntensity;
                        texture = textureFile(m->texture);
                        s.material.baseColor = loadTexture(texture);
                    }
                    s.material.uvScale = m->tiling;
                    add(std::move(s), texture);
                }
            }
            if (const ModelRenderer* instance = world.get<ModelRenderer>(e); instance && instance->visible) {
                const auto model = assets.model(instance->model);
                if (!model) continue;
                const std::string modelName = assets.modelName(instance->model);
                for (size_t n = 0; n < model->data.nodes.size(); ++n) {
                    const ModelNode& node = model->data.nodes[n];
                    for (size_t slot = 0; slot < node.meshes.size(); ++slot) {
                        const uint32_t index = node.meshes[slot];
                        if (index >= model->data.meshes.size()) continue;
                        const ModelMesh& mesh = model->data.meshes[index];
                        if (mesh.skinned()) {
                            d.warnings.push_back(entityLabel(world, e) + " has a skinned mesh, which isn't baked");
                            continue;
                        }
                        if (mesh.material >= model->data.materials.size() || mesh.data.indices.empty()) continue;
                        const ModelMaterial& material = model->data.materials[mesh.material];
                        BakeSurface s;
                        s.part = modelPartKey(uint32_t(n), uint32_t(slot));
                        s.model = model;
                        s.mesh = std::shared_ptr<const MeshData>(model, &mesh.data);
                        s.toWorld = toWorld * node.world;
                        s.geometryHash = cachedMeshHash(model, mesh.data);
                        std::string texture = modelName + "#" + std::to_string(mesh.material);
                        const MaterialOverride* o = findMaterialOverride(instance->materials, material.name);
                        const auto replaced = o ? assets.material(o->material) : nullptr;
                        if (replaced) {
                            s.material.albedo = glm::vec3(replaced->baseColor * instance->tint);
                            s.material.alpha = replaced->baseColor.a * instance->tint.a;
                            s.material.metallic = replaced->draw.metallic;
                            s.material.emissive = replaced->draw.emissive;
                            s.material.alphaCutoff = replaced->draw.alphaCutoff;
                            s.material.blend = replaced->draw.blend;
                            s.material.doubleSided = replaced->draw.doubleSided;
                            s.material.binding = replaced->draw.bindings[0];
                            texture += ":material:" + assets.materialName(o->material);
                            s.material.baseColor = loadTexture(textureFile(replaced->baseColorTexture));
                        } else {
                            const DrawMaterial draw = modelDrawMaterial(*model, mesh.material);
                            s.material.albedo = glm::vec3(material.baseColor * instance->tint);
                            s.material.alpha = material.baseColor.a * instance->tint.a;
                            s.material.metallic = material.metallic;
                            s.material.emissive = material.emissive;
                            s.material.alphaCutoff = draw.alphaCutoff;
                            s.material.blend = draw.blend;
                            s.material.doubleSided = draw.doubleSided;
                            s.material.binding = draw.bindings[0];
                            const int map = material.baseColorTexture;
                            if (map >= 0 && size_t(map) < model->data.textures.size() &&
                                !model->data.textures[size_t(map)].image.rgba.empty())
                                s.material.baseColor =
                                    std::shared_ptr<const Image>(model, &model->data.textures[size_t(map)].image);
                        }
                        add(std::move(s), texture);
                    }
                }
            }
        }
    }
    std::sort(d.surfaces.begin(), d.surfaces.end(), [](const BakeSurface& a, const BakeSurface& b) {
        return a.binding != b.binding ? a.binding < b.binding : a.part < b.part;
    });
    {
        LightingHasher h;
        for (const BakeSurface& s : d.surfaces) h.id(s.binding).u32(s.part).u64(s.hash);
        d.geometryHash = h.value();
    }

    // Lights whose bounce is baked, and the surroundings.
    std::vector<uint64_t> lightHashes;
    if (const Pool<BakeLight>* bakeLights = world.pool<BakeLight>()) {
        const auto entities = bakeLights->entities();
        for (size_t i = 0; i < entities.size(); ++i) {
            const Entity e = entities[i];
            const Light* light = world.get<Light>(e);
            if (!bakeLights->components()[i].includeIndirect || !light || !light->enabled || light->intensity <= 0.0f ||
                !world.has<Transform>(e))
                continue;
            BakeLightSource source{e, describeLight(world, e, *light)};
            const LightDesc& l = source.light;
            lightHashes.push_back(LightingHasher()
                                      .u32(uint32_t(l.type))
                                      .vec(l.position)
                                      .vec(l.direction)
                                      .vec(l.color)
                                      .f32(l.range)
                                      .f32(l.cosOuter)
                                      .f32(l.cosInner)
                                      .value());
            d.lights.push_back(source);
        }
    }
    std::sort(lightHashes.begin(), lightHashes.end());
    Environment environment;
    if (const Pool<Environment>* environments = world.pool<Environment>(); environments && environments->size())
        environment = environments->components()[0];
    d.environment.sky = glm::vec3(environment.skyColor) * environment.ambientIntensity;
    d.environment.ground = glm::vec3(environment.groundColor) * environment.ambientIntensity;
    d.environment.intensity = std::max(environment.environmentIntensity, 0.0f);
    d.environment.rotation = glm::radians(environment.environmentRotation);
    uint64_t mapHash = 0;
    if (environment.environmentMap.id) {
        d.environment.mapName = assets.textureName(environment.environmentMap);
        if (!contentRoot.empty() && !d.environment.mapName.empty()) {
            if (const DecodedCache::Entry* entry = decodeFile(contentRoot / d.environment.mapName, true)) {
                d.environment.map = entry->hdr;
                mapHash = entry->hash;
            }
            if (!d.environment.map)
                d.warnings.push_back(
                    "environment map \"" + d.environment.mapName +
                    "\" couldn't be decoded; the bake uses sky and ground");
        }
    }
    {
        LightingHasher h;
        h.u64(lightHashes.size());
        for (uint64_t l : lightHashes) h.u64(l);
        h.vec(d.environment.sky).vec(d.environment.ground).f32(d.environment.intensity).f32(d.environment.rotation);
        h.text(d.environment.mapName).u64(mapHash);
        d.lightingHash = h.value();
    }

    // Probe volumes.
    if (const Pool<DiffuseProbeVolume>* volumes = world.pool<DiffuseProbeVolume>()) {
        const auto entities = volumes->entities();
        for (size_t i = 0; i < entities.size(); ++i) {
            const Entity e = entities[i];
            const DiffuseProbeVolume& v = volumes->components()[i];
            if (!v.enabled || !world.has<Transform>(e)) continue;
            std::string problem;
            const BindingId binding = effectiveBindingId(world, e, v.bindingId, &problem);
            if (!binding.valid()) {
                d.warnings.push_back(problem + "; the volume is left out of the bake");
                continue;
            }
            BakeVolume volume{e, binding, v, worldMatrix(world, e), 0};
            const float det = glm::determinant(glm::mat3(volume.toWorld));
            if (!std::isfinite(det) || std::abs(det) < 1e-9f ||
                glm::any(glm::lessThanEqual(v.halfExtents, glm::vec3(0.0f))) || v.spacing <= 0.0f) {
                d.warnings.push_back(entityLabel(world, e) + " is a probe volume with no size; it's left out");
                continue;
            }
            volume.hash = LightingHasher()
                              .mat(volume.toWorld)
                              .vec(v.halfExtents)
                              .f32(v.spacing)
                              .f32(v.blendDistance)
                              .u32(uint32_t(v.priority))
                              .value();
            d.volumes.push_back(volume);
        }
    }
    std::sort(d.volumes.begin(), d.volumes.end(), [](const BakeVolume& a, const BakeVolume& b) {
        return a.binding < b.binding;
    });
    return d;
}

// ---- Runtime ----

struct BakedLightingRuntime::Loaded {
    LightingManifest manifest;
    std::vector<TextureHandle> lightmaps;
    struct Mesh {
        BindingId binding;
        uint32_t part = 0;
        uint32_t atlas = 0;
        uint64_t hash = 0;   // the manifest's receiver hash
        MeshHandle mesh;
    };
    std::vector<Mesh> meshes;
    LightingProbeSet probes;
    std::vector<LightingBinding> volumes;
    uint64_t gpuBytes = 0;
};

BakedLightingRuntime::BakedLightingRuntime() = default;
BakedLightingRuntime::~BakedLightingRuntime() { release(); }

const char* BakedLightingRuntime::stateName(State state) {
    switch (state) {
    case State::None: return "none";
    case State::Disabled: return "disabled";
    case State::Missing: return "missing";
    case State::Invalid: return "invalid";
    case State::Stale: return "stale";
    case State::Ready: return "ready";
    }
    return "?";
}

void BakedLightingRuntime::free(Loaded& loaded) {
    if (!m_renderer) return;
    for (const Loaded::Mesh& m : loaded.meshes)
        if (m.mesh.valid()) m_renderer->destroyMesh(m.mesh);
    for (TextureHandle t : loaded.lightmaps)
        if (t.id) m_renderer->destroyTexture(t);
    loaded.meshes.clear();
    loaded.lightmaps.clear();
}

void BakedLightingRuntime::release() {
    m_bound.clear();
    if (m_loaded) free(*m_loaded);
    m_loaded.reset();
    m_probes = {};
    m_probeMask.clear();
    m_probesReady = false;
    m_status = {};
    m_asset.clear();
    m_loadState = State::None;
    m_loadError.clear();
}

void BakedLightingRuntime::report(const std::string& message) {
    if (message == m_lastReport) return;
    m_lastReport = message;
    if (!message.empty()) SW_LOG(LogRender, Warning, "baked lighting: {}", message);
}

void BakedLightingRuntime::load(
    const World& world, const Assets& assets, Renderer& renderer, const std::string& asset,
    const fs::path& contentRoot) {
    std::string error;
    auto fail = [&](State state, const std::string& reason) {
        // The last good bake stays; it's only replaced by a complete new one.
        m_loadState = state;
        m_loadError = reason;
        report(asset + ": " + reason);
    };
    auto next = std::make_unique<Loaded>();
    if (!loadLightingManifest(contentRoot / asset, next->manifest, error)) {
        std::error_code ec;
        fail(
            fs::exists(contentRoot / asset, ec) ? State::Invalid : State::Missing,
            fs::exists(contentRoot / asset, ec) ? error : "not baked yet (no " + asset + ")");
        return;
    }
    // Every payload whole and as the manifest vouches, before anything reaches the GPU.
    std::vector<std::vector<float>> lightmaps;
    std::vector<glm::uvec2> sizes;
    std::vector<LightingReceiver> receivers;
    for (const LightingResource& r : next->manifest.resources) {
        std::vector<uint8_t> bytes;
        if (!readLightingFile(contentRoot / r.path, bytes, error)) return fail(State::Invalid, error);
        if (hashLightingBytes(bytes) != r.hash || bytes.size() != r.bytes)
            return fail(
                State::Invalid, r.path + " doesn't match its manifest (an interrupted bake or an edit); rebake");
        if (r.kind == LightingResource::Kind::Lightmap) {
            uint32_t w = 0, h = 0;
            std::vector<float> rgba;
            if (!decodeLightmapKtx2(bytes, w, h, rgba, error)) return fail(State::Invalid, r.path + ": " + error);
            if (w != r.width || h != r.height)
                return fail(State::Invalid, r.path + " isn't the size its manifest says");
            lightmaps.push_back(std::move(rgba));
            sizes.push_back({w, h});
        } else if (r.kind == LightingResource::Kind::Receivers) {
            if (!readLightingReceivers(bytes, receivers, error)) return fail(State::Invalid, r.path + ": " + error);
        } else if (!readLightingProbes(bytes, next->probes, error)) {
            return fail(State::Invalid, r.path + ": " + error);
        }
    }
    next->volumes = next->manifest.volumes;
    // Upload: lightmaps, then each receiver's mesh made from its source mesh as the scene has it now.
    m_renderer = &renderer;
    for (size_t i = 0; i < lightmaps.size(); ++i) {
        TextureData texture;
        texture.format = TextureFormat::RGBA16F;
        texture.srgb = false;
        texture.width = sizes[i].x;
        texture.height = sizes[i].y;
        texture.bytes = lightmapHalfFloats(lightmaps[i]);
        texture.levels = {{sizes[i].x, sizes[i].y, 0, texture.bytes.size()}};
        next->lightmaps.push_back(renderer.createTexture(texture, TextureUsage::Fixed));
        next->gpuBytes += texture.bytes.size();
    }
    const BakeSceneDescription scene = describeBakeScene(world, assets, {});
    for (const LightingReceiver& r : receivers) {
        const LightingBinding* binding = nullptr;
        for (const LightingBinding& b : next->manifest.receivers)
            if (b.binding == r.binding && b.part == r.part) binding = &b;
        const BakeSurface* surface = scene.surface(r.binding, r.part);
        if (!binding || r.atlas >= next->lightmaps.size() || !surface || surface->geometryHash != r.geometryHash)
            continue;
        MeshData mesh;
        if (!receiverMesh(*surface->mesh, r, mesh)) continue;
        next->meshes.push_back({r.binding, r.part, r.atlas, binding->hash, renderer.createMesh(mesh)});
        next->gpuBytes += mesh.vertices.size() * (sizeof(Vertex) + sizeof(glm::vec2)) + mesh.indices.size() * 4;
    }
    m_bound.clear();
    if (m_loaded) free(*m_loaded);
    m_loaded = std::move(next);
    m_status.generation++;
    m_loadState = State::None;
    m_loadError.clear();
    m_probeMask.clear();
    m_lastReport.clear();
}

void BakedLightingRuntime::update(
    const World& world, const Assets& assets, Renderer& renderer, double dt, double interval) {
    m_renderer = &renderer;
    const BakedLighting* bake = nullptr;
    if (const Pool<BakedLighting>* pool = world.pool<BakedLighting>(); pool && pool->size())
        bake = &pool->components()[0];
    const uint64_t generation = m_status.generation;
    m_bound.clear();
    m_probesReady = false;
    m_status = {};
    m_status.generation = generation;
    if (!bake || bake->asset.empty()) {
        if (m_loaded) release();
        m_status.generation = generation;
        return;
    }
    m_status.asset = bake->asset;
    const fs::path contentRoot = assets.contentRoot();
    std::error_code ec;
    m_poll += dt;
    if (bake->asset != m_asset || m_forceReload || m_poll >= interval) {
        m_poll = 0.0;
        const auto stamp = fs::last_write_time(contentRoot / bake->asset, ec);
        if (bake->asset != m_asset || m_forceReload || ec || stamp != m_stamp) {
            if (bake->asset != m_asset && m_loaded) {
                free(*m_loaded);
                m_loaded.reset();
            }
            m_asset = bake->asset;
            m_stamp = ec ? fs::file_time_type{} : stamp;
            m_forceReload = false;
            load(world, assets, renderer, bake->asset, contentRoot);
        }
    }
    if (!m_loaded) {
        m_status.state = m_loadState == State::None ? State::Missing : m_loadState;
        m_status.reason = m_loadError.empty() ? "not baked yet (no " + bake->asset + ")" : m_loadError;
        return;
    }
    const Loaded& loaded = *m_loaded;
    m_status.receivers = uint32_t(loaded.manifest.receivers.size());
    m_status.gpuBytes = loaded.gpuBytes;
    if (!bake->enabled) {
        m_status.state = State::Disabled;
        m_status.reason = "BakedLighting is disabled";
        return;
    }
    const BakeSceneDescription scene = describeBakeScene(world, assets, contentRoot);
    if (lightingManifestHash(scene.lightingHash, loaded.manifest) != loaded.manifest.lightingHash) {
        m_status.state = State::Invalid;
        m_status.reason = "lights or surroundings changed since the bake; rebake";
        m_status.invalidReceivers = m_status.receivers;
        report(bake->asset + ": " + m_status.reason);
        return;
    }
    for (const Loaded::Mesh& m : loaded.meshes) {
        const BakeSurface* surface = scene.surface(m.binding, m.part);
        if (!surface || surface->hash != m.hash || !surface->receiver) continue;
        Receiver r;
        r.mesh = m.mesh;
        r.lighting.source = IndirectLighting::Source::Lightmap;
        r.lighting.lightmap = loaded.lightmaps[m.atlas];
        m_bound[(uint64_t(surface->entity.index) << 32) | m.part] = r;
    }
    m_status.boundReceivers = uint32_t(m_bound.size());
    m_status.invalidReceivers = m_status.receivers - std::min(m_status.receivers, m_status.boundReceivers);
    // Volumes still where and as they were baked; the set is rebuilt only when that changes.
    std::vector<bool> mask;
    for (const LightingProbeVolume& v : loaded.probes.volumes) {
        bool current = false;
        for (const LightingBinding& b : loaded.volumes)
            if (b.binding == v.binding)
                for (const BakeVolume& now : scene.volumes)
                    if (now.binding == v.binding && now.hash == b.hash) current = true;
        mask.push_back(current);
    }
    if (mask != m_probeMask || m_probeGeneration != m_status.generation) {
        m_probeMask = mask;
        m_probeGeneration = m_status.generation;
        m_probes.environment = loaded.probes.environment;
        m_probes.volumes.clear();
        for (size_t i = 0; i < mask.size(); ++i)
            if (mask[i]) m_probes.volumes.push_back(loaded.probes.volumes[i]);
    }
    for (const LightingProbeVolume& v : m_probes.volumes) m_status.probes += uint32_t(v.probes.size());
    m_status.volumes = uint32_t(m_probes.volumes.size());
    m_probesReady = !m_probes.volumes.empty();
    const bool stale = scene.geometryHash != loaded.manifest.geometryHash || m_status.invalidReceivers > 0 ||
                       m_status.volumes != loaded.probes.volumes.size();
    m_status.state = stale ? State::Stale : State::Ready;
    if (stale) {
        m_status.reason = m_status.invalidReceivers
                              ? std::to_string(m_status.invalidReceivers) +
                                    " receivers moved or changed since the bake (drawn without it); rebake"
                              : "static geometry or probe volumes changed since the bake; rebake";
        report(bake->asset + ": " + m_status.reason);
    } else {
        report("");
    }
}

const BakedLightingRuntime::Receiver* BakedLightingRuntime::receiver(Entity e, uint32_t part) const {
    if (m_bound.empty()) return nullptr;
    const auto it = m_bound.find((uint64_t(e.index) << 32) | part);
    return it == m_bound.end() ? nullptr : &it->second;
}

bool BakedLightingRuntime::probeLighting(const glm::vec3& point, IndirectLighting& out) const {
    if (!m_probesReady) return false;
    const ProbeSample sample = sampleProbes(m_probes, point);
    if (sample.coverage <= 0.0f) return false;
    out.source = IndirectLighting::Source::Probes;
    out.sh = sample.sh;
    return true;
}

LightingBakeInfo inspectLightingBake(const World& world, const Assets& assets, const fs::path& contentRoot) {
    using State = BakedLightingRuntime::State;
    LightingBakeInfo info;
    const BakedLighting* bake = nullptr;
    if (const Pool<BakedLighting>* pool = world.pool<BakedLighting>(); pool && pool->size())
        bake = &pool->components()[0];
    if (!bake || bake->asset.empty()) return info;
    info.asset = bake->asset;
    std::string error;
    std::error_code ec;
    if (!loadLightingManifest(contentRoot / bake->asset, info.manifest, error)) {
        const bool exists = fs::exists(contentRoot / bake->asset, ec);
        info.state = exists ? State::Invalid : State::Missing;
        info.reason = exists ? error : "not baked yet (no " + bake->asset + ")";
        info.broken = exists;
        return info;
    }
    for (const LightingResource& r : info.manifest.resources) {
        std::vector<uint8_t> bytes;
        if (!readLightingFile(contentRoot / r.path, bytes, error)) {
            info.state = State::Invalid;
            info.reason = error;
            info.broken = true;
            return info;
        }
        if (hashLightingBytes(bytes) != r.hash || bytes.size() != r.bytes) {
            info.state = State::Invalid;
            info.broken = true;
            info.reason = r.path + " doesn't match its manifest (an interrupted bake or an edit); rebake";
            return info;
        }
        info.payloadBytes += bytes.size();
    }
    const BakeSceneDescription scene = describeBakeScene(world, assets, contentRoot);
    auto label = [&](Entity e, uint32_t part) {
        const Name* name = world.alive(e) ? world.get<Name>(e) : nullptr;
        return (name ? name->value : "entity " + std::to_string(e.index)) + " (part " + std::to_string(part) + ")";
    };
    for (const LightingBinding& b : info.manifest.receivers) {
        const BakeSurface* surface = scene.surface(b.binding, b.part);
        if (surface && surface->receiver && surface->hash == b.hash)
            ++info.currentReceivers;
        else
            info.changedReceivers.push_back(
                surface ? label(surface->entity, b.part) : formatBindingId(b.binding) + " (gone)");
    }
    for (const BakeSurface& surface : scene.surfaces) {
        if (!surface.receiver) continue;
        bool baked = false;
        for (const LightingBinding& b : info.manifest.receivers)
            if (b.binding == surface.binding && b.part == surface.part) baked = true;
        if (!baked) info.unbakedReceivers.push_back(label(surface.entity, surface.part));
    }
    for (const LightingBinding& b : info.manifest.volumes) {
        bool current = false;
        for (const BakeVolume& v : scene.volumes)
            if (v.binding == b.binding && v.hash == b.hash) current = true;
        if (current)
            ++info.currentVolumes;
        else
            info.changedVolumes.push_back(formatBindingId(b.binding));
    }
    for (const BakeVolume& v : scene.volumes) {
        bool baked = false;
        for (const LightingBinding& b : info.manifest.volumes) baked |= b.binding == v.binding;
        if (!baked) info.changedVolumes.push_back(label(v.entity, 0) + " (new)");
    }
    info.geometryChanged = scene.geometryHash != info.manifest.geometryHash;
    if (!bake->enabled) {
        info.state = State::Disabled;
        info.reason = "BakedLighting is disabled";
    } else if (lightingManifestHash(scene.lightingHash, info.manifest) != info.manifest.lightingHash) {
        info.state = State::Invalid;
        info.reason = "lights or surroundings changed since the bake; rebake";
    } else if (
        info.geometryChanged || !info.changedReceivers.empty() || !info.unbakedReceivers.empty() ||
        !info.changedVolumes.empty()) {
        info.state = State::Stale;
        info.reason = !info.changedReceivers.empty()
                          ? std::to_string(info.changedReceivers.size()) + " receivers moved or changed since the bake"
                      : !info.unbakedReceivers.empty()
                          ? std::to_string(info.unbakedReceivers.size()) + " receivers aren't in the bake yet"
                      : !info.changedVolumes.empty() ? "probe volumes changed since the bake"
                                                     : "static geometry changed since the bake";
        info.reason += "; rebake";
    } else {
        info.state = State::Ready;
    }
    return info;
}

}   // namespace shockwave
