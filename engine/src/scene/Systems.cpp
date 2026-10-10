#include <shockwave/Camera.h>
#include <shockwave/Renderer.h>
#include <shockwave/Assets.h>
#include <shockwave/Hierarchy.h>
#include <shockwave/Systems.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace shockwave {

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
        const float offset =
            bob.amplitude * std::sin(glm::two_pi<float>() * bob.frequency * bob.time + bob.phase);
        t.position.y += offset - bob.offset;
        bob.offset = offset;
    });
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

void renderWorld(World& world, Renderer& renderer, JobSystem& jobs, double alpha, const Assets* assets) {
    if (const Pool<Environment>* environments = world.pool<Environment>(); environments && environments->size()) {
        const glm::vec4 sky = environments->components()[0].skyColor;
        renderer.setClearColor({sky.r, sky.g, sky.b, 1.0f});
    }

    const float a = static_cast<float>(alpha);
    const Pool<PreviousTransform>* previous = a < 1.0f ? world.pool<PreviousTransform>() : nullptr;
    const Pool<Transform>* transforms = world.pool<Transform>();
    const bool hasParents = world.count<Parent>() > 0;
    // The local matrix as of this frame: blended between the last two ticks when playing.
    auto local = [&](Entity e, const Transform& t) {
        const PreviousTransform* p = previous ? previous->find(e.index) : nullptr;
        return p ? Transform::blend(p->value, t, a).matrix() : t.matrix();
    };
    // drawMesh is thread-safe, so draw submission splits across cores like any system.
    // Each entity works out its own world matrix by walking up its parents: nothing is
    // written, so there's no ordering to get right, and hierarchies are shallow.
    world.parallelEach<const Transform, const MeshRenderer>(
        jobs,
        [&](Entity e, const Transform& t, const MeshRenderer& m) {
            if (!m.visible || !m.mesh.valid()) return;
            glm::mat4 model = local(e, t);
            if (hasParents) {
                Entity parent = parentOf(world, e);
                for (int depth = 0; parent.valid() && depth < kMaxHierarchyDepth; ++depth) {
                    if (const Transform* pt = transforms->find(parent.index)) model = local(parent, *pt) * model;
                    parent = parentOf(world, parent);
                }
            }
            DrawMaterial material;
            material.uvScale = m.tiling;
            renderer.drawMesh(m.mesh, model, m.texture, m.tint, material);
        },
        256);
    if (assets) world.parallelEach<const Transform,const ModelRenderer>(jobs,
        [&](Entity e,const Transform& t,const ModelRenderer& instance) {
            if (!instance.visible) return;
            const auto asset=assets->model(instance.model);
            if (!asset) return;
            auto model=local(e,t);
            if (hasParents) {
                Entity parent=parentOf(world,e);
                for (int depth=0;parent.valid() && depth<kMaxHierarchyDepth;++depth) {
                    if (const auto* pt=transforms->find(parent.index)) model=local(parent,*pt)*model;
                    parent=parentOf(world,parent);
                }
            }
            for (const auto& node:asset->data.nodes) for (auto index:node.meshes) {
                const auto& mesh=asset->data.meshes[index];
                const auto& material=asset->data.materials[mesh.material];
                const auto texture=material.baseColorTexture<0 ? TextureHandle{} : asset->textures[material.baseColorTexture];
                DrawMaterial options;
                options.alphaCutoff=material.alpha==ModelMaterial::Alpha::Mask ? material.alphaCutoff : -1.0f;
                options.doubleSided=material.doubleSided;
                options.blend=material.alpha==ModelMaterial::Alpha::Blend;
                renderer.drawMesh(asset->meshes[index],model*node.world,texture,material.baseColor*instance.tint,options);
            }
        },64);

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
    projection = perspectiveReverseZ(glm::radians(glm::clamp(camera->fov, 1.0f, 170.0f)), aspect,
                                     std::max(camera->nearPlane, 0.001f));
    return true;
}

} // namespace shockwave
