#include <shockwave/Renderer.h>
#include <shockwave/Hierarchy.h>
#include <shockwave/Systems.h>

#include <glm/gtc/constants.hpp>

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

void renderWorld(World& world, Renderer& renderer, JobSystem& jobs, double alpha) {
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
            renderer.drawMesh(m.mesh, model, m.texture, m.tint);
        },
        256);
}

} // namespace shockwave
