#pragma once

// LevelHost: the one active runtime level and the moves between levels (S03,
// docs/LevelLifetime.md). It owns the level's PlaySession, which always runs with
// OwnedRuntimeWorld, in a World the application owns and keeps at one address.
//
//   LevelHost host(runtimeWorld, jobs, instance, &engine, hooks);
//   host.start({"menu"});          // the world already holds the menu's scene
//   ... every frame: update/frame through host.session(), then
//   host.process();                // after the frame's callbacks: travel happens here
//   host.stop();
//
// Game code asks with game.requestLevel("b"). Nothing changes until process(), which the
// application calls on the main thread once the frame's ticks, frame and UI callbacks
// have returned, so a GameMode is never destroyed on its own call stack. process() loads
// the next level into a separate world first: if that fails, the current level keeps
// playing. Then it ends the current session (endPlay, subsystems), moves the new level
// into the world with a new epoch, and starts a new session.
//
// Rules:
//   - one level simulates at a time;
//   - every accepted request ends exactly once (onResult): Succeeded, Failed, or
//     Cancelled when another request replaces it or Play stops first;
//   - a request from an older level (a stale callback) is refused;
//   - requests made before a level has ticked (from beginPlay, say) are redirects; more
//     than kMaxRedirects in a row fail as a cycle instead of loading forever;
//   - EntityRefs from an earlier level stop resolving (World::levelEpoch).
//
// Main thread only. Not copyable; the World and GameInstance must outlive it.

#include <GameFramework/LevelLifetime.h>
#include <GameFramework/PlaySession.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace shockwave {

class LevelHost {
public:
    static constexpr int kMaxRedirects = 8;

    struct Hooks {
        // Loads the requested level's scene into `into`, an empty world. False, with the
        // reason in error, when it can't (an unknown level, a broken scene).
        std::function<bool(const LevelRequest&, World& into, std::string& error)> load;
        // Before each level's session starts: save folder, game circuit, online match.
        std::function<void(PlaySession&, const LevelRequest&)> configure;
        // After a level's session started (Play or travel): place the player at the
        // request's spawn, report physics problems.
        std::function<void(PlaySession&, const LevelRequest&)> started;
        // Just before a level's session stops for travel or Stop.
        std::function<void(PlaySession&)> stopping;
    };

    LevelHost(World& world, JobSystem& jobs, GameInstance& instance, Engine* engine, Hooks hooks);
    ~LevelHost();
    LevelHost(const LevelHost&) = delete;
    LevelHost& operator=(const LevelHost&) = delete;

    // Starts the level that's already in the world. Throws when its session can't start
    // (nothing is playing then).
    void start(LevelRequest level, StartReason reason = StartReason::Fresh);
    // Ends Play: a pending request is cancelled, the session ends. Safe to repeat.
    void stop();
    // Queues travel. Returns the request's id, or 0 when refused (nothing is playing).
    // A request already waiting is cancelled in its favour.
    uint64_t request(LevelRequest level);
    // Main thread, after the frame: carries out the waiting request, if any.
    void process();

    PlaySession* session() const { return m_session.get(); }
    bool playing() const { return m_session && m_session->active(); }
    // A request is waiting for process(): hot reload and saving wait for it.
    bool transitioning() const { return m_pending.has_value(); }
    const LevelRequest& current() const { return m_current; }
    uint64_t epoch() const { return m_epoch; }
    // The last travel that failed and left nothing playing, or empty.
    const std::string& failure() const { return m_failure; }
    // The most recent results, oldest first (at most 32 kept).
    const std::vector<TravelResult>& results() const { return m_results; }
    // Called once for every result, as it happens.
    std::function<void(const TravelResult&)> onResult;

    // For tests: the epoch the next level would get. Overflow fails travel explicitly.
    void setEpochForTesting(uint64_t epoch) { m_epoch = epoch; }

private:
    struct Pending {
        uint64_t id;
        LevelRequest request;
        bool redirect;   // made before the current level ticked
    };
    void finish(uint64_t id, const LevelRequest& request, TravelStatus status, std::string error);
    void startSession(StartReason reason);
    void endSession();
    void restoreSettings();

    World& m_world;
    JobSystem& m_jobs;
    GameInstance& m_instance;
    Engine* m_engine;
    Hooks m_hooks;
    std::unique_ptr<PlaySession> m_session;
    LevelRequest m_current;
    uint64_t m_epoch = 0;
    uint64_t m_nextId = 1;
    int m_redirects = 0;
    std::optional<Pending> m_pending;
    bool m_processing = false;
    bool m_starting = false;   // inside a session's start(): beginPlay may request
    std::vector<std::string> m_chain;   // the redirects so far, for the cycle message
    std::optional<GraphicsSettings> m_settings;   // the application's, put back by stop()
    std::string m_failure;
    std::vector<TravelResult> m_results;
};

}   // namespace shockwave
