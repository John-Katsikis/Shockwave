#pragma once

// PlaySession: one Play of a World, the engine side of Unreal's Play In Editor. start()
// snapshots the authored scene, creates the session's subsystems (physics) and the
// GameMode; update() runs a fixed tick; stop() ends play and restores the snapshot
// exactly, so nothing done while playing sticks. The player uses the same class.
//
// Network play (docs/Networking.md, "Session integration"): setNetworkStart() before
// start() makes the session a host or a client. Its NetworkWorld (game.network()) starts
// before GameMode::beginPlay, and Stop closes it first. While online the session never
// pauses (pause() refuses: the host doesn't stop for one player), circuit breakpoints are
// off, and saving and loading fail with UnavailableDuringNetworkPlay. Hot reload of game
// code and content waits for Stop: callers check online().

#include <Engine/GraphicsSettings.h>
#include <GameFramework/GameInstance.h>
#include <GameFramework/SaveGame.h>
#include <Multiplayer/NetworkWorld.h>

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

    // editing -> playing; paused -> playing; playing: nothing. Throws when something can't
    // start, network play included; the authored world is then restored.
    void start();
    // playing -> paused. False (and nothing changes) during network play.
    bool pause();
    void stop();   // end play, release the session's subsystems, restore the authored world
    void update(double dt);   // one fixed tick
    void frame(double dt);   // once per rendered frame

    // ---- Network play ----
    // Before start(): play online as `start.role`. services is the application's, not the
    // session's, and must outlive it. An Offline role (the default) plays offline.
    void setNetworkStart(net::NetworkStart start, net::IOnlineServices* services) {
        m_networkStart = std::move(start);
        m_services = services;
    }
    const net::NetworkStart& networkStart() const { return m_networkStart; }
    // The application's match control, which game code sees as game.onlineMatch. Must
    // outlive the session.
    void setOnlineMatch(net::OnlineMatch* match) { m_context.onlineMatch = match; }
    // Null while editing; offline Play has one too, with role Offline.
    net::NetworkWorld* network() const;
    // Playing (or paused) as a host or client. Stays true after a network failure until
    // Stop, since the game's state is still a network game's.
    bool online() const;
    // The application calls these once per frame, in this order, every frame (menus and
    // editing included; they do nothing then): see GameFramework/NetworkPump.h.
    void pumpNetwork(double nowSeconds);
    bool handleOnlineEvent(const net::OnlineEvent& event);
    void collectInput(double frameDt);

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
    net::NetworkStart m_networkStart;
    net::IOnlineServices* m_services = nullptr;
};

}   // namespace shockwave
