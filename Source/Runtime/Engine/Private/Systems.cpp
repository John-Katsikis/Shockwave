#include <Engine/Systems.h>

#include <Assets/Assets.h>
#include <Engine/BakedLighting.h>
#include <Engine/Camera.h>
#include <Engine/Decals.h>
#include <Engine/Foliage.h>
#include <Engine/Hierarchy.h>
#include <Engine/Pose.h>
#include <Engine/Terrain.h>
#include <Renderer/Renderer.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace shockwave {

namespace {

// Which world, entity and part of it a draw is: the same every frame while the entity lives.
RenderInstanceKey instanceKey(const World& world, Entity e, uint32_t part = 0) {
    return {world.lifetime(), e.index, e.generation, part, 0};
}

}   // namespace

void addBuiltinSystems(Schedule& schedule) {
    schedule.each<Transform, const Spin>("Spin", [](const SystemContext& ctx, Transform& t, const Spin& spin) {
        const float length = glm::length(spin.axis);
        if (length < 1e-6f) return;
        const glm::quat step = glm::angleAxis(glm::radians(spin.speed * ctx.dt), spin.axis / length);
        t.rotation = glm::normalize(step * t.rotation);
    });
    // Applies the change in height rather than setting it, so Bob stacks with anything
    // else that moves the entity.
    schedule.each<Transform, Bob>("Bob", [](const SystemContext& ctx, Transform& t, Bob& bob) {
        bob.time += ctx.dt;
        const float offset = bob.amplitude * std::sin(glm::two_pi<float>() * bob.frequency * bob.time + bob.phase);
        t.position.y += offset - bob.offset;
        bob.offset = offset;
    });
    // Impact marks fade and go (Engine/Decals.h). Destroys entities, so it runs alone.
    Schedule::Access lifetimes;
    lifetimes.exclusive = true;
    schedule.add("DecalLifetime", lifetimes, [](const SystemContext& ctx) { updateDecalLifetimes(ctx.world, ctx.dt); });
}

void storePreviousTransforms(World& world, JobSystem& jobs) {
    Pool<Transform>* transforms = world.pool<Transform>();
    if (!transforms) return;
    if (world.count<PreviousTransform>() != transforms->size()) {
        // New entities since last tick: start them where they are, so they don't
        // blend in from the origin.
        const std::vector<Entity> entities(transforms->entities().begin(), transforms->entities().end());
        for (Entity e : entities) {
            if (!world.has<PreviousTransform>(e)) world.add<PreviousTransform>(e, {*world.get<Transform>(e)});
        }
    }
    world.parallelEach<const Transform, PreviousTransform>(
        jobs, [](const Transform& t, PreviousTransform& previous) { previous.value = t; }, 1024);
}

Transform presentedTransform(const World& world, Entity e, double alpha) {
    const Transform* t = world.get<Transform>(e);
    if (!t) return {};
    if (const auto* shown = world.get<PresentationTransform>(e)) return shown->value;
    if (const auto* previous = world.get<PreviousTransform>(e))
        return Transform::blend(previous->value, *t, float(std::clamp(alpha, 0.0, 1.0)));
    return *t;
}

void renderWorld(
    World& world, Renderer& renderer, JobSystem& jobs, double alpha, const Assets* assets,
    const BakedLightingRuntime* baked, bool (*include)(const World&, Entity)) {
    // The first Environment sets the sky, ambient light and tonemapping; defaults without one.
    Environment environment;
    if (const Pool<Environment>* environments = world.pool<Environment>(); environments && environments->size()) {
        environment = environments->components()[0];
    }
    const glm::vec4 sky = environment.skyColor;
    renderer.setClearColor({sky.r, sky.g, sky.b, 1.0f});
    LightingDesc lighting;
    lighting.skyAmbient = glm::vec3(environment.skyColor) * environment.ambientIntensity;
    lighting.groundAmbient = glm::vec3(environment.groundColor) * environment.ambientIntensity;
    lighting.environmentMap = environment.environmentMap;
    lighting.environmentIntensity = std::max(environment.environmentIntensity, 0.0f);
    lighting.environmentRotation = glm::radians(environment.environmentRotation);
    lighting.showEnvironment = environment.showEnvironment;
    lighting.bloomIntensity = std::clamp(environment.bloomIntensity, 0.0f, 1.0f);
    lighting.exposure = environment.exposure;
    lighting.tonemapper = static_cast<LightingDesc::Tonemapper>(environment.tonemapper);
    lighting.shadowDistance = environment.shadowDistance;
    lighting.shadowCascades = static_cast<uint32_t>(std::clamp(environment.shadowCascades, 1, 4));
    lighting.aoRadius = std::clamp(environment.aoRadius, 0.01f, 10.0f);
    lighting.aoIntensity = std::clamp(environment.aoIntensity, 0.0f, 4.0f);
    lighting.clusterFar = std::isfinite(environment.clusterFar) ? std::clamp(environment.clusterFar, 1.0f, 100000.0f)
                                                                : kDefaultClusterFar;
    renderer.setLighting(lighting);

    const float a = static_cast<float>(alpha);
    const Pool<PreviousTransform>* previous = a < 1.0f ? world.pool<PreviousTransform>() : nullptr;
    const Pool<Transform>* transforms = world.pool<Transform>();
    const bool hasParents = world.count<Parent>() > 0;
    const Pool<PresentationTransform>* presented = world.pool<PresentationTransform>();
    if (presented && presented->size() == 0) presented = nullptr;
    // The local matrix as of this frame: a presentation pose when one is set, else blended
    // between the last two ticks when playing.
    auto local = [&](Entity e, const Transform& t) {
        if (const PresentationTransform* shown = presented ? presented->find(e.index) : nullptr)
            return shown->value.matrix();
        const PreviousTransform* p = previous ? previous->find(e.index) : nullptr;
        return p ? Transform::blend(p->value, t, a).matrix() : t.matrix();
    };
    // Baked lighting (G05, G06): a static receiver bound to its bake draws its own mesh with
    // lightmap UVs; anything else inside a probe volume gets a probe sample at its centre
    // (a MeshRenderer's origin, a model's bounds centre). One sample per object: a long
    // object spanning two rooms gets one room's light.
    const bool probes = baked && baked->usesProbes();
    auto probeLit = [&](Entity e, const glm::vec3& point, IndirectLighting& out) {
        if (!probes) return false;
        if (const BakeGeometry* g = world.get<BakeGeometry>(e);
            g && g->mobility == BakeGeometry::Mobility::Static && g->receiveLightmap)
            return false;   // a receiver without a valid lightmap keeps the environment
        return baked->probeLighting(point, out);
    };
    // Decal receiver layers (G08): layer 1 for anything without a DecalReceiver.
    const Pool<DecalReceiver>* receivers = world.pool<DecalReceiver>();
    if (receivers && receivers->size() == 0) receivers = nullptr;
    auto decalLayers = [&](Entity e) {
        const DecalReceiver* r = receivers ? receivers->find(e.index) : nullptr;
        return r ? static_cast<uint32_t>(r->layers) & kDecalLayerMask : 1u;
    };
    // drawMesh is thread-safe, so draw submission splits across cores like any system.
    // Each entity works out its own world matrix by walking up its parents: nothing is
    // written, so there's no ordering to get right, and hierarchies are shallow.
    world.parallelEach<const Transform, const MeshRenderer>(
        jobs,
        [&](Entity e, const Transform& t, const MeshRenderer& m) {
            if (!m.visible || !m.mesh.valid() || (include && !include(world, e))) return;
            glm::mat4 model = local(e, t);
            if (hasParents) {
                Entity parent = parentOf(world, e);
                for (int depth = 0; parent.valid() && depth < kMaxHierarchyDepth; ++depth) {
                    if (const Transform* pt = transforms->find(parent.index)) model = local(parent, *pt) * model;
                    parent = parentOf(world, parent);
                }
            }
            MeshHandle mesh = m.mesh;
            IndirectLighting probe;
            const IndirectLighting* indirect = nullptr;
            if (const BakedLightingRuntime::Receiver* r = baked ? baked->receiver(e, 0) : nullptr) {
                mesh = r->mesh;
                indirect = &r->lighting;
            } else if (probeLit(e, glm::vec3(model[3]), probe)) {
                indirect = &probe;
            }
            // A material asset decides the surface; the component's own values otherwise.
            if (const auto asset = m.material.valid() && assets ? assets->material(m.material) : nullptr) {
                DrawMaterial material = asset->draw;
                material.uvScale = m.tiling;
                material.castShadows = m.castShadows;
                material.indirect = indirect;
                material.decalLayers = decalLayers(e);
                renderer.drawMesh(
                    mesh, model, asset->baseColorTexture, asset->baseColor * m.tint, material, instanceKey(world, e));
                return;
            }
            DrawMaterial material;
            material.uvScale = m.tiling;
            material.roughness = m.roughness;
            material.metallic = m.metallic;
            material.emissive = glm::vec3(m.emissive) * m.emissiveIntensity;
            material.castShadows = m.castShadows;
            material.indirect = indirect;
            material.decalLayers = decalLayers(e);
            renderer.drawMesh(mesh, model, m.texture, m.tint, material, instanceKey(world, e));
        },
        256);
    const Pool<ModelPose>* poses = world.pool<ModelPose>();
    if (assets)
        world.parallelEach<const Transform, const ModelRenderer>(
            jobs,
            [&](Entity e, const Transform& t, const ModelRenderer& instance) {
                if (!instance.visible || (include && !include(world, e))) return;
                const auto asset = assets->model(instance.model);
                if (!asset) return;
                auto model = local(e, t);
                if (hasParents) {
                    Entity parent = parentOf(world, e);
                    for (int depth = 0; parent.valid() && depth < kMaxHierarchyDepth; ++depth) {
                        if (const auto* pt = transforms->find(parent.index)) model = local(parent, *pt) * model;
                        parent = parentOf(world, parent);
                    }
                }
                // Animated (Animation module): where its Animator has put the nodes this frame.
                const ModelPose* pose = poses ? poses->find(e.index) : nullptr;
                if (pose && (pose->model != asset || pose->nodes.size() != asset->data.nodes.size())) pose = nullptr;
                std::vector<glm::mat4> rest;   // the nodes as modelled, for skinned meshes without a pose
                thread_local std::vector<glm::mat4> joints;
                // Parts drawn with a material asset instead of their own (A02), by source material.
                thread_local std::vector<std::shared_ptr<const MaterialAsset>> overrides;
                overrides.assign(asset->data.materials.size(), nullptr);
                const uint32_t layers = decalLayers(e);
                IndirectLighting probe;
                const bool probeLitModel = probeLit(
                    e, glm::vec3(model * glm::vec4((asset->data.bounds.min + asset->data.bounds.max) * 0.5f, 1.0f)),
                    probe);
                for (const MaterialOverride& o : instance.materials)
                    for (size_t i = 0; i < asset->data.materials.size(); ++i)
                        if (asset->data.materials[i].name == o.part) overrides[i] = assets->material(o.material);
                for (size_t n = 0; n < asset->data.nodes.size(); ++n) {
                    const auto& node = asset->data.nodes[n];
                    for (const auto& index : node.meshes) {
                        const auto& mesh = asset->data.meshes[index];
                        if (mesh.material >= asset->data.materials.size()) continue;
                        const auto& material = asset->data.materials[mesh.material];
                        const MaterialAsset* replaced = overrides[mesh.material].get();
                        DrawMaterial options = replaced ? replaced->draw : modelDrawMaterial(*asset, mesh.material);
                        options.castShadows = instance.castShadows;
                        options.indirect = probeLitModel ? &probe : nullptr;
                        options.decalLayers = layers;
                        const int baseMap = material.baseColorTexture;
                        const TextureHandle texture = replaced ? replaced->baseColorTexture
                                                      : baseMap < 0 || size_t(baseMap) >= asset->textures.size()
                                                          ? TextureHandle{}
                                                          : asset->textures[size_t(baseMap)];
                        const glm::vec4 baseColor =
                            (replaced ? replaced->baseColor : material.baseColor) * instance.tint;
                        // Node and slot within it: the same part of the model every frame.
                        const auto slot = static_cast<uint32_t>(&index - node.meshes.data());
                        const RenderInstanceKey key = instanceKey(world, e, modelPartKey(uint32_t(n), slot));
                        if (mesh.skinned()) {
                            // Skinned vertices end up in the model's space, whatever node holds the mesh.
                            if (!pose && rest.empty())
                                for (const auto& each : asset->data.nodes) rest.push_back(each.world);
                            skinMatrices(mesh, pose ? std::span<const glm::mat4>(pose->nodes) : rest, joints);
                            renderer.drawSkinnedMesh(
                                asset->meshes[index], model, joints, texture, baseColor, options, key);
                            continue;
                        }
                        MeshHandle drawn = asset->meshes[index];
                        if (const BakedLightingRuntime::Receiver* r = baked ? baked->receiver(e, key.part) : nullptr) {
                            drawn = r->mesh;
                            options.indirect = &r->lighting;
                        }
                        renderer.drawMesh(
                            drawn, model * (pose ? pose->nodes[n] : node.world), texture, baseColor, options, key);
                    }
                }
            },
            64);

    // Terrains (W1a): one draw per chunk; the renderer picks each chunk's detail level by
    // distance (lod_error_pixels), like model levels. Only the entity's position counts.
    if (assets && world.count<Terrain>() > 0)
        world.each<const Transform, const Terrain>([&](Entity e, const Transform&, const Terrain& terrain) {
            if (!terrain.visible || terrain.asset.empty() || (include && !include(world, e))) return;
            const auto asset = assets->terrain(terrain.asset);
            if (!asset || !asset->data) return;
            const glm::mat4 placed = glm::translate(glm::mat4(1.0f), glm::vec3(worldMatrix(world, e)[3]));
            DrawMaterial options;
            TextureHandle texture{};
            glm::vec4 baseColor{0.55f, 0.55f, 0.5f, 1.0f};
            if (!asset->data->material.empty())
                if (const auto material = assets->material(assets->findMaterial(asset->data->material))) {
                    options = material->draw;
                    texture = material->baseColorTexture;
                    baseColor = material->baseColor;
                }
            // Painted (W1b): up to four layers blended by the weights, in the terrain family.
            TerrainShading layered;
            MaterialShading family;
            const bool painted = asset->data->painted() && asset->weights.id != 0;
            if (painted) {
                options = DrawMaterial{};
                texture = TextureHandle{};
                baseColor = glm::vec4(1.0f);
                layered.weights = asset->weights;
                SamplerDesc clamped;
                clamped.wrapU = clamped.wrapV = TextureWrap::Clamp;
                layered.weightsSampler = renderer.sampler(clamped);
                layered.corner = glm::vec2(placed[3].x, placed[3].z);
                layered.spacing = asset->data->spacing;
                layered.samples = asset->data->samples;
                for (uint32_t i = 0; i < kTerrainLayerCount; ++i) {
                    const TerrainLayer& source = asset->data->layers[i];
                    TerrainLayerShading& l = layered.layer[i];
                    l.uvMetres = source.uvMetres;
                    l.baseColor = glm::vec4(0.55f, 0.55f, 0.5f, 1.0f);   // the default surface
                    if (!source.used || source.material.empty()) continue;
                    if (const auto m = assets->material(assets->findMaterial(source.material))) {
                        l.baseColorTexture = m->baseColorTexture;
                        l.normalTexture = m->draw.normalTexture;
                        l.metallicRoughnessTexture = m->draw.metallicRoughnessTexture;
                        l.sampler = m->draw.bindings[size_t(MaterialSlot::BaseColor)].sampler;
                        l.baseColor = m->baseColor;
                        l.roughness = m->draw.roughness;
                        l.metallic = m->draw.metallic;
                        l.normalScale = m->draw.normalScale;
                    }
                }
                family.family = ShadingFamily::Terrain;
                family.terrain = &layered;
                options.shading = &family;
            }
            options.castShadows = terrain.castShadows;
            options.decalLayers = decalLayers(e);
            IndirectLighting probe;
            for (uint32_t c = 0; c < asset->chunks.size(); ++c) {
                const TerrainChunk& chunk = asset->chunks[c];
                if (!chunk.mesh.valid()) continue;
                layered.layers = chunk.layers;
                DrawMaterial each = options;
                const glm::vec3 centre =
                    glm::vec3(placed * glm::vec4((chunk.bounds.min + chunk.bounds.max) * 0.5f, 1.0f));
                each.indirect = probeLit(e, centre, probe) ? &probe : nullptr;
                renderer.drawMesh(chunk.mesh, placed, texture, baseColor, each, instanceKey(world, e, c));
            }
        });

    // Foliage (W2a): instanced cells within each type's distance and in view.
    if (assets && world.count<Foliage>() > 0) drawFoliage(world, renderer, *assets, include);

    // Decals (G08): every enabled one, with its box where the hierarchy puts it this frame.
    // The renderer picks which to draw within the player's decals_quality budget.
    if (world.count<Decal>() > 0) {
        const Pool<DecalLifetime>* lifetimes = world.pool<DecalLifetime>();
        thread_local std::vector<DecalDesc> decals;
        decals.clear();
        world.each<const Transform, const Decal>([&](Entity e, const Transform& t, const Decal& decal) {
            if (!decal.enabled || (include && !include(world, e))) return;
            const DecalLifetime* lifetime = lifetimes ? lifetimes->find(e.index) : nullptr;
            // Stuck to something that's gone: it's removed next tick; don't show it adrift.
            if (lifetime && lifetime->receiver.valid() && !world.alive(lifetime->receiver)) return;
            const float fade = lifetime ? decalLifetimeFade(*lifetime) : 1.0f;
            if (!(fade > 0.0f) || !(decal.opacity > 0.0f)) return;
            glm::mat4 model = local(e, t);
            if (hasParents) {
                Entity parent = parentOf(world, e);
                for (int depth = 0; parent.valid() && depth < kMaxHierarchyDepth; ++depth) {
                    if (const Transform* pt = transforms->find(parent.index)) model = local(parent, *pt) * model;
                    parent = parentOf(world, parent);
                }
            }
            const std::shared_ptr<const MaterialAsset> material =
                decal.material.valid() && assets ? assets->material(decal.material) : nullptr;
            decals.push_back(decalDesc(decal, model, material.get(), decalOrder(e, lifetime), fade));
        });
        renderer.drawDecals(decals);
    }

    // Lights shine along their entity's forward axis (-Z), wherever the hierarchy puts it.
    world.each<const Transform, const Light>([&](Entity e, const Transform&, const Light& light) {
        if (!light.enabled || light.intensity <= 0.0f) return;
        const glm::mat4 m = worldMatrix(world, e);
        LightDesc desc;
        desc.type = static_cast<LightDesc::Type>(light.type);
        desc.position = glm::vec3(m[3]);
        desc.direction = -glm::vec3(m[2]);
        desc.color = glm::vec3(light.color) * light.intensity;
        desc.range = light.range;
        const float outer = glm::radians(std::clamp(light.spotAngle, 1.0f, 89.0f));
        desc.cosOuter = std::cos(outer);
        desc.cosInner = std::cos(outer * (1.0f - std::clamp(light.spotSoftness, 0.0f, 1.0f)));
        desc.castShadows = light.castShadows;
        desc.shadowStrength = light.shadowStrength;
        desc.shadowSoftness = light.shadowSoftness;
        desc.shadowBias = light.shadowBias;
        renderer.addLight(desc);
    });
}

void skinMatrices(const ModelMesh& mesh, std::span<const glm::mat4> nodes, std::vector<glm::mat4>& joints) {
    joints.resize(mesh.bones.size());
    for (size_t b = 0; b < mesh.bones.size(); ++b) {
        const uint32_t node = mesh.bones[b].node;
        joints[b] = (node < nodes.size() ? nodes[node] : glm::mat4(1.0f)) * mesh.bones[b].offset;
    }
}

Entity primaryCamera(const World& world) {
    const Pool<Camera>* cameras = world.pool<Camera>();
    if (!cameras) return kNullEntity;
    // Lowest entity index wins ties, which is the order cameras appear in a scene file.
    Entity first = kNullEntity, primary = kNullEntity;
    const auto entities = cameras->entities();
    const auto components = cameras->components();
    for (size_t i = 0; i < entities.size(); ++i) {
        const Entity e = entities[i];
        if (!world.has<Transform>(e)) continue;
        if (!first.valid() || e.index < first.index) first = e;
        if (components[i].primary && (!primary.valid() || e.index < primary.index)) primary = e;
    }
    return primary.valid() ? primary : first;
}

bool cameraMatrices(const World& world, Entity e, float aspect, glm::mat4& view, glm::mat4& projection) {
    const Camera* camera = world.get<Camera>(e);
    if (!camera || !world.has<Transform>(e)) return false;
    // Drop scale and shear: a camera is a position and an orientation.
    const glm::mat4 m = worldMatrix(world, e);
    glm::mat4 rigid(1.0f);
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 axis(m[i]);
        const float length = glm::length(axis);
        rigid[i] = glm::vec4(length > 1e-8f ? axis / length : glm::vec3(i == 0, i == 1, i == 2), 0.0f);
    }
    rigid[3] = m[3];
    view = glm::inverse(rigid);
    projection = perspectiveReverseZ(
        glm::radians(glm::clamp(camera->fov, 1.0f, 170.0f)), aspect, std::max(camera->nearPlane, 0.001f));
    return true;
}

}   // namespace shockwave
