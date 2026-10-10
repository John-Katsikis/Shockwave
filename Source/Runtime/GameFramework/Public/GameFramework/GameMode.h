#pragma once

// GameMode: the rules of one Play session of a scene, as in Unreal. It adds the game's
// systems to the schedule, and gets BeginPlay/EndPlay and per-tick hooks. A fresh
// GameMode is made for every Play, by the project's GameInstance.

#include <Engine/Schedule.h>
#include <Engine/Subsystem.h>

namespace shockwave {

class Engine;
class PhysicsWorld;
class AudioWorld;

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
    PhysicsWorld& physics() const;   // always there during Play
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
