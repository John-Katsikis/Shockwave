#pragma once

// GameMode: the rules of one Play session of a scene, as in Unreal. It adds the game's
// systems to the schedule, and gets BeginPlay/EndPlay and per-tick hooks. A fresh
// GameMode is made for every Play, by the project's GameInstance.

#include <Engine/EntityRef.h>
#include <Engine/Schedule.h>
#include <Engine/Subsystem.h>

#include <glm/vec3.hpp>

#include <string_view>

namespace shockwave {

class Engine;
class Input;
class PhysicsWorld;
class AudioWorld;
class EffectsWorld;
class AnimationWorld;
class UIWorld;
class NavigationWorld;

// What game code can reach during Play.
struct GameContext {
    World& world;
    JobSystem& jobs;
    Engine* engine;   // null only in tools and tests that run without a window
    SubsystemCollection& subsystems;   // this session's: physics, and any the game adds

    template <class T>
    T* subsystem() const {
        return subsystems.find<T>();
    }
    AudioWorld& audio() const;   // always there during Play
    // Particle effects (docs/Effects.md). Always there during Play; without a window it
    // does nothing, so game code doesn't need to check.
    EffectsWorld& effects() const;
    // Skeletal animation (docs/Animation.md): switch clips, find joints. Always there
    // during Play.
    AnimationWorld& animation() const;
    PhysicsWorld& physics() const;   // always there during Play
    // Menus and HUDs (docs/GameUI.md). Always there during Play; without a window it
    // does nothing, so game code doesn't need to check.
    UIWorld& ui() const;
    NavigationWorld& navigation() const;   // always there during Play; paths and NavAgents
    // The engine's input. In tools and tests without a window it's one where nothing is
    // ever pressed, so game code doesn't need to check.
    Input& input() const;
    // The first entity with this Name, or an empty EntityRef (false) when there's none.
    EntityRef find(std::string_view name) const { return findEntity(world, name); }
    // A new copy of a prefab ("Prefabs/Enemy.swprefab", see docs/Prefabs.md), where the
    // prefab puts it or at position. An empty EntityRef (false) if it can't be loaded.
    EntityRef spawn(std::string_view prefab) const;
    EntityRef spawn(std::string_view prefab, const glm::vec3& position) const;
};

class GameMode {
public:
    virtual ~GameMode() = default;
    // Add the game's systems; they run in parallel by declared access (see Schedule).
    virtual void configureSystems(GameContext&, Schedule&) {}
    // The scene is loaded and physics exists; spawn things, add subsystems.
    virtual void beginPlay(GameContext&) {}
    // Main thread, every fixed tick after the systems and the physics step.
    virtual void fixedUpdate(GameContext&, double dt) { (void)dt; }
    // Main thread, once per rendered frame with the real frame time.
    virtual void frame(GameContext&, double frameDt) { (void)frameDt; }
    // Play is stopping; the authored scene is restored right after.
    virtual void endPlay(GameContext&) {}
};

}   // namespace shockwave
