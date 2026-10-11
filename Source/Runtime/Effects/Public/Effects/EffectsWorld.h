#pragma once

// EffectsWorld: game code's way into particle effects during Play, as game.effects().
// One per Play session, like AudioWorld and UIWorld. When Play stops, every one-shot it
// started is stopped.
//
//     auto boom = game.effects().play("Effects/explosion.efkefc", position);
//     game.effects().play(ship);                 // the entity's ParticleEffect, from the start
//     game.effects().stop(ship);                 // lets its particles finish
//     game.effects().trigger(boom, 0);           // a trigger set up in the effect
//
// Effects are named by their path inside Content. Calls with an unknown handle or an
// entity without a running effect do nothing and return false.
//
// Threading: main thread only (GameMode::beginPlay, frame, fixedUpdate, endPlay), never
// from systems.

#include <Effects/Effects.h>
#include <Engine/EntityRef.h>
#include <Engine/Subsystem.h>
#include <Engine/World.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string_view>
#include <vector>

namespace shockwave {

class EffectsWorld final : public Subsystem {
public:
    using Handle = Effects::Handle;

    // effects: null in tools and tests without one; then everything is a harmless no-op.
    EffectsWorld(World& world, Effects* effects);
    ~EffectsWorld() override;

    // ---- One-shots ----
    // Plays an effect once at a place; returns its handle, or -1 when it can't be loaded.
    Handle play(
        std::string_view effect, const glm::vec3& position, const glm::quat& rotation = glm::quat(1, 0, 0, 0),
        float scale = 1.0f);
    // Plays an effect at an entity's place in the world, once, without following it.
    Handle playAt(std::string_view effect, Entity entity);
    bool playing(Handle handle) const;
    // Lets live particles finish; immediately removes them too.
    bool stop(Handle handle, bool immediately = false);
    bool move(
        Handle handle, const glm::vec3& position, const glm::quat& rotation = glm::quat(1, 0, 0, 0),
        float scale = 1.0f);
    bool setSpeed(Handle handle, float speed);
    bool setColor(Handle handle, const glm::vec4& color);
    bool setInput(Handle handle, int index, float value);
    bool trigger(Handle handle, int index);

    // ---- An entity's ParticleEffect ----
    // Starts it from its first frame (and turns playing on).
    bool play(Entity entity);
    bool play(const EntityRef& entity) { return play(entity.entity()); }
    // Turns playing off; immediately also removes its live particles.
    bool stop(Entity entity, bool immediately = false);
    bool stop(const EntityRef& entity, bool immediately = false) { return stop(entity.entity(), immediately); }
    bool trigger(Entity entity, int index);
    bool setInput(Entity entity, int index, float value);

    // Stops every one-shot this session started.
    void stopAll();
    // Loads an effect ahead of time, so its first play doesn't stall. False if it can't.
    bool preload(std::string_view effect);
    // Live particles across every effect.
    uint32_t particleCount() const;

private:
    World& m_world;
    Effects* m_effects;
    std::vector<Handle> m_started;
};

}   // namespace shockwave
