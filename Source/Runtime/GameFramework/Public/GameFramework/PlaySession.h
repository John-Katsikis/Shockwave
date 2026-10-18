#pragma once

// PlaySession: one Play of a World, the engine side of Unreal's Play In Editor. start()
// snapshots the authored scene, creates the session's subsystems (physics) and the
// GameMode; update() runs a fixed tick; stop() ends play and restores the snapshot
// exactly, so nothing done while playing sticks. The player uses the same class.

#include <Engine/GraphicsSettings.h>
#include <GameFramework/GameInstance.h>
#include <GameFramework/SaveGame.h>

#include <filesystem>
#include <memory>
#include <optional>

namespace shockwave {

class CircuitWorld;

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
    EffectsWorld* effects() const;   // null while editing
    AnimationWorld* animation() const;   // null while editing
    PhysicsWorld* physics() const;   // null while editing
    SaveGames* saves() const;   // null while editing
    // Where game.saves() keeps its slots (usually <project>/Saved/SaveGames), and the
    // scene's project-relative name to note in them. Set before start(); without it
    // saving is off.
    void setSaveFolder(std::filesystem::path folder, std::string scene = {}) {
        m_saveFolder = std::move(folder);
        m_saveScene = std::move(scene);
    }
    UIWorld* ui() const;   // null while editing
    // Circuits (docs/Circuits.md): entity circuits and the game circuit. Null while editing.
    CircuitWorld* circuits() const;
    // The project's game circuit, inside Content ("Circuits/Game.swcircuit"), or empty for
    // none. Set before start().
    void setGameCircuit(std::string path) { m_gameCircuit = std::move(path); }
    NavigationWorld* navigation() const;   // null while editing
    // Before start: false mixes into AudioWorld::mixer().render() instead of a device,
    // for offline tools such as player --audio-mix.
    void setAudioDevice(bool open) { m_audioDevice = open; }

private:
    void tearDown();
    void applyLoadedSave();

    GameInstance& m_instance;
    SubsystemCollection m_subsystems;
    GameContext m_context;
    std::unique_ptr<GameMode> m_mode;
    std::optional<World> m_snapshot;
    std::optional<GraphicsSettings> m_graphicsSnapshot;
    std::filesystem::path m_saveFolder;   // setSaveFolder
    std::string m_saveScene;
    std::string m_gameCircuit;   // setGameCircuit
    Schedule m_schedule;
    State m_state = State::Editing;
    uint64_t m_ticks = 0;
    bool m_audioDevice = true;
};

}   // namespace shockwave
