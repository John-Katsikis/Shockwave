#pragma once
#include <Engine/Schedule.h>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace shockwave {
class Engine;
class PhysicsWorld;
struct GameplayContext {
    World& world;
    JobSystem& jobs;
    // Non-null in editor/player; nullable only for CPU-only session tests/tools.
    Engine* engine = nullptr;
    PhysicsWorld* physics = nullptr; // valid during Play, including onStop
};
// Each Play session gets a fresh instance. Register types before loading any scene.
class Gameplay {
public:
    virtual ~Gameplay() = default;
    virtual void configureSystems(GameplayContext&, Schedule&) {}
    virtual void onStart(GameplayContext&) {}
    virtual void onPhysicsStep(GameplayContext&, double) {} // main thread, after fixed-step physics
    virtual void onFrame(GameplayContext&, double) {}
    virtual void onStop(GameplayContext&) {}
};
using GameplayFactory = std::function<std::unique_ptr<Gameplay>()>;

// Persistent setup is separate from per-session gameplay state. Asset handles belong
// in the shared Assets registry; each Play instance can look them up through Engine.
struct GameModule {
    std::string_view id;
    GameplayFactory create;
    std::function<void()> registerTypes;
    std::function<void(Engine&)> createAssets;
};
// Implemented by the project library, linked into both editor and player.
const GameModule& projectGameModule();

class GameSession {
public:
    enum class State { Editing, Playing, Paused };
    GameSession(World& world, JobSystem& jobs, GameplayFactory factory, Engine* engine = nullptr);
    ~GameSession();
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;
    void start();     // paused -> resume; already playing -> no-op
    void pause();
    void stop();      // clean up game-owned resources, then restore authored ECS state
    void update(double dt);
    void frame(double dt);
    State state() const { return m_state; }
    bool active() const { return m_state != State::Editing; }
    const Schedule& schedule() const { return m_schedule; }
    PhysicsWorld* physics() const { return m_physics.get(); }
    uint64_t ticks() const { return m_ticks; }
private:
    GameplayContext m_context;
    GameplayFactory m_factory;
    std::unique_ptr<Gameplay> m_game;
    std::unique_ptr<PhysicsWorld> m_physics;
    std::optional<World> m_snapshot;
    Schedule m_schedule;
    State m_state = State::Editing;
    uint64_t m_ticks = 0;
};
} // namespace shockwave
