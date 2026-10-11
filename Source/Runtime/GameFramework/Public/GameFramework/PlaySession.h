#pragma once

// PlaySession: one Play of a World, the engine side of Unreal's Play In Editor. start()
// snapshots the authored scene, creates the session's subsystems (physics) and the
// GameMode; update() runs a fixed tick; stop() ends play and restores the snapshot
// exactly, so nothing done while playing sticks. The player uses the same class.

#include <GameFramework/GameInstance.h>

#include <memory>
#include <optional>

namespace shockwave {

class PlaySession {
public:
    enum class State { Editing, Playing, Paused };

    PlaySession(World& world, JobSystem& jobs, GameInstance& instance, Engine* engine = nullptr);
    ~PlaySession();
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    void start();   // editing -> playing; paused -> playing; playing: nothing
    void pause();
    void stop();   // end play, release the session's subsystems, restore the authored world
    void update(double dt);   // one fixed tick
    void frame(double dt);   // once per rendered frame

    State state() const { return m_state; }
    bool active() const { return m_state != State::Editing; }
    uint64_t ticks() const { return m_ticks; }
    const Schedule& schedule() const { return m_schedule; }
    GameMode* gameMode() const { return m_mode.get(); }
    SubsystemCollection& subsystems() { return m_subsystems; }
    AudioWorld* audio() const;   // null while editing
    PhysicsWorld* physics() const;   // null while editing
    UIWorld* ui() const;   // null while editing
    NavigationWorld* navigation() const;   // null while editing
    // Before start: false mixes into AudioWorld::mixer().render() instead of a device,
    // for offline tools such as player --audio-mix.
    void setAudioDevice(bool open) { m_audioDevice = open; }

private:
    void tearDown();

    GameInstance& m_instance;
    SubsystemCollection m_subsystems;
    GameContext m_context;
    std::unique_ptr<GameMode> m_mode;
    std::optional<World> m_snapshot;
    Schedule m_schedule;
    State m_state = State::Editing;
    uint64_t m_ticks = 0;
    bool m_audioDevice = true;
};

}   // namespace shockwave
