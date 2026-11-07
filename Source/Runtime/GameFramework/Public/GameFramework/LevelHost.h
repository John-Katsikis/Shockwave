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
// It is the GameFlow (GameFramework/GameFlow.h) game code reaches as game.flow(): game
// code asks with game.flow().request("b") or game.requestLevel("b"), and gets a ticket.
// Nothing changes until process(), which the application calls on the main thread once
// the frame's ticks, frame and UI callbacks have returned, so a GameMode is never
// destroyed on its own call stack. process() loads the next level into a separate world
// first: if that fails, the current level keeps playing. Then it ends the current session
// (endPlay, subsystems), moves the new level into the world with a new epoch, and starts
// a new session.
//
// Staged (setStaged, S05; the player and editor's default): the change takes two frames.
// The first process() validates, reads and prepares the next level and pauses the current
// one (AwaitingBoundary); a frame renders, so a loading screen can show and the ticket can
// still be cancelled; the next process() retires the old level and starts the new one.
// Unstaged, all of it happens in one process(), as in S03.
//
// Rules:
//   - one level simulates at a time, and one travel is active at a time: another request
//     meanwhile is refused as Busy (a redirect from beginPlay excepted);
//   - every accepted request ends exactly once (onResult, subscribe): Succeeded, Failed,
//     or Cancelled when it's cancelled or Play stops first;
//   - a request from an older level (a stale callback) is refused;
//   - a level's requests made before it has ticked (from beginPlay, say) are redirects;
//     more than kMaxRedirects in a row fail as a cycle instead of loading forever. The
//     application's (fromEpoch 0) never are;
//   - after a level failed to start nothing plays, but the application can still request
//     a level (an error screen's Retry or Main menu); stop() ends that;
//   - EntityRefs from an earlier level stop resolving (World::levelEpoch);
//   - with a residency (setResidency), each level holds a lease on its dependency closure
//     (Hooks::assets). The next level's is planned before the current one ends: one that
//     doesn't fit the budget fails as BudgetExceeded and the current level plays on. The
//     old lease goes after its session, so assets both levels use stay resident.
//
// Main thread only. Not copyable; the World and GameInstance must outlive it.

#include <Assets/AssetResidency.h>
#include <GameFramework/GameFlow.h>
#include <GameFramework/LevelLifetime.h>
#include <GameFramework/PlaySession.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace shockwave {

class LevelHost final : public GameFlow {
public:
    static constexpr int kMaxRedirects = 8;
    static constexpr size_t kKeptTickets = 64;   // ended tickets status() still knows

    struct Hooks {
        // Loads the requested level's scene into `into`, an empty world. False, with the
        // reason in error, when it can't (an unknown level, a broken scene).
        std::function<bool(const LevelRequest&, World& into, std::string& error)> load;
        // Before each level's session starts: save folder, game circuit, online match.
        std::function<void(PlaySession&, const LevelRequest&)> configure;
        // Before a request is accepted: is the level known (else false, with UnknownLevel
        // or another code), and the spawn it arrives at when none was asked for (the
        // catalog's default_spawn). Unset: every level is accepted, as asked.
        std::function<bool(TravelRequest&, TravelError& code, std::string& error)> resolve;
        // After a level's session started (Play or travel): report physics problems.
        std::function<void(PlaySession&, const LevelRequest&)> started;
        // Just before a level's session stops for travel or Stop.
        std::function<void(PlaySession&)> stopping;
        // What a level needs resident: its scene's dependency closure, with costs. Called
        // with the prepared world before the current level ends (and for start()). False,
        // with the reason, refuses the level. Unset: levels hold no lease.
        //
        // When the residency has a loader (assets load on demand, docs/AssetResidency.md),
        // travel runs in the order section 6.1 of the shipping plan gives: this is asked
        // first, with an empty world, since the scene can't be read before its assets are
        // in; then the current level ends and its own assets unload; then the new lease is
        // loaded, the scene read (load) and the level started. A failure after the current
        // level ended is a failed start: nothing is playing (failure() says why).
        std::function<bool(const LevelRequest&, const World& level, std::vector<AssetCost>& assets, std::string& error)>
            assets;
    };

    LevelHost(World& world, JobSystem& jobs, GameInstance& instance, Engine* engine, Hooks hooks);
    ~LevelHost() override;
    LevelHost(const LevelHost&) = delete;
    LevelHost& operator=(const LevelHost&) = delete;

    // Starts the level that's already in the world. Throws when its session can't start
    // (nothing is playing then).
    void start(LevelRequest level, StartReason reason = StartReason::Fresh);
    // Ends Play: a pending request is cancelled, the session ends. Safe to repeat.
    void stop();
    // Queues travel (GameFlow::request with reason Exit, from the application). Returns
    // the ticket, or 0 when refused (why is logged).
    uint64_t requestLevel(LevelRequest level);
    // Main thread, after the frame: carries out the waiting request, if any (staged: one
    // phase of it).
    void process();

    // ---- GameFlow ----
    TravelTicket request(TravelRequest request) override;
    TravelError cancel(uint64_t ticket) override;
    std::optional<TravelProgress> status(uint64_t ticket) const override;
    uint64_t activeTicket() const override;
    const LevelRequest& currentLevel() const override { return m_current; }
    uint64_t currentEpoch() const override { return m_epoch; }
    TravelTicket restartLevel(uint64_t fromEpoch = 0) override;
    TravelTicket returnToMenu(uint64_t fromEpoch = 0) override;
    TravelTicket useExit(Entity exit, uint64_t fromEpoch = 0) override;

    // The level returnToMenu goes to (the player's: the catalog's menu level).
    void setMenuLevel(std::string level) { m_menuLevel = std::move(level); }
    const std::string& menuLevel() const { return m_menuLevel; }
    // Two frames per change, with a frame between preparing and retiring (see above).
    void setStaged(bool staged) { m_staged = staged; }
    bool staged() const { return m_staged; }

    PlaySession* session() const { return m_session.get(); }
    bool playing() const { return m_session && m_session->active(); }
    // A request is waiting for process(), or prepared and waiting for the frame boundary:
    // hot reload and saving wait for it.
    bool transitioning() const { return m_pending.has_value() || m_candidate.has_value(); }
    const LevelRequest& current() const { return m_current; }
    uint64_t epoch() const { return m_epoch; }
    // The last travel that failed and left nothing playing, or empty.
    const std::string& failure() const { return m_failure; }
    // The most recent results, oldest first (at most 32 kept).
    const std::vector<TravelResult>& results() const { return m_results; }
    // Called once for every result, as it happens.
    std::function<void(const TravelResult&)> onResult;

    // ---- Residency (S04, docs/AssetResidency.md) ----
    // Where levels take their leases; null (the default) turns it off. Set before start();
    // it must outlive the host.
    void setResidency(AssetResidency* residency) { m_residency = residency; }
    AssetResidency* residency() const { return m_residency; }
    const AssetResidency::Lease& lease() const { return m_lease; }
    // One per start() and per carried-out request (not cancelled ones), oldest first (at
    // most 32 kept), and as each finishes.
    const std::vector<LoadReport>& loadReports() const { return m_reports; }
    std::function<void(const LoadReport&)> onLoadReport;

    // ---- Campaign (S06, docs/SaveGames.md#campaign) ----
    // game.campaign() of every level: it outlives each one. New game with resetCampaign
    // clears it, Restart puts back what it held when the level was entered, and a save
    // brings back its own.
    CampaignState& campaign() { return m_campaign; }
    const CampaignState& campaign() const { return m_campaign; }
    // Authored entities with a PersistentIdentity destroyed in a level stay destroyed when
    // it's entered again (Restart level and New game bring them back). On by default; the
    // player's project key remember_destroyed turns it off.
    void setRememberDestroyed(bool on) { m_rememberDestroyed = on; }
    bool rememberDestroyed() const { return m_rememberDestroyed; }

    // For tests: the epoch the next level would get. Overflow fails travel explicitly.
    void setEpochForTesting(uint64_t epoch) { m_epoch = epoch; }

private:
    using Clock = std::chrono::steady_clock;
    struct Pending {
        uint64_t id;
        TravelRequest request;
        bool redirect;   // made before the current level ticked
    };
    // A level prepared beside the current one, waiting for the frame boundary (staged).
    struct Candidate {
        Pending next;
        std::unique_ptr<World> prepared;   // null on demand: it's read after the old level goes
        std::vector<AssetCost> assets;
        LoadReport load;
        bool onDemand = false;
        bool paused = false;   // the current session was paused for it
    };
    // The phases before the boundary. False: it ended (finished already).
    bool prepare(const Pending& next, Candidate& out);
    // From the boundary on. False: stop processing (nothing plays, or Play stopped).
    bool commit(Candidate candidate);
    // Before the boundary: puts the current level back as it was.
    void resume(Candidate& candidate);
    void setPhase(uint64_t id, TravelPhase phase);
    void finish(uint64_t id, TravelStatus status, TravelError code, std::string error);
    // Hands completions to onResult, observers and circuits, outside any change.
    void deliver();
    TravelTicket refuse(TravelError code, std::string message, const TravelRequest& request) const;
    void startSession(StartReason reason, const LevelArrival& arrival);
    void startLevel(const TravelRequest& request);
    void noteAuthored();
    void recordDestroyed(const TravelRequest& next);
    void arrived();
    void endSession();
    void restoreSettings();
    bool planAssets(
        const LevelRequest& request, const World& level, std::vector<AssetCost>& assets, LoadReport& report,
        std::string& error);
    void report(LoadReport report);
    // Travel when assets load on demand (Hooks::assets). False: stop processing (nothing plays).
    bool travelOnDemand(Candidate& candidate, Clock::time_point& clock);
    // After the current level ended: the new one didn't start, so nothing plays.
    void failStarted(const Pending& next, LoadReport& load, TravelError code, const std::string& why);

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
    std::optional<Candidate> m_candidate;   // staged: prepared, waiting for the boundary
    uint64_t m_inFlight = 0;   // the ticket past the boundary, inside process()
    std::map<uint64_t, TravelProgress> m_tickets;   // active
    std::vector<TravelProgress> m_ended;   // the last kKeptTickets, oldest first
    std::map<uint64_t, Clock::time_point> m_accepted;
    std::vector<TravelCompletion> m_outbox;   // waiting for deliver()
    bool m_delivering = false;
    bool m_staged = false;
    std::string m_menuLevel;
    bool m_processing = false;
    bool m_stopped = false;   // stop() came while process() was loading
    std::pair<uint64_t, uint64_t> m_countsAtStart{};   // the residency's loaded/unloaded when a change began
    bool m_starting = false;   // inside a session's start(): beginPlay may request
    std::vector<std::string> m_chain;   // the redirects so far, for the cycle message
    std::optional<GraphicsSettings> m_settings;   // the application's, put back by stop()
    std::string m_failure;
    std::vector<TravelResult> m_results;
    AssetResidency* m_residency = nullptr;
    AssetResidency::Lease m_lease;   // the current level's
    std::vector<LoadReport> m_reports;
    CampaignState m_campaign;
    CampaignState m_entryCampaign;   // as the current level was entered
    bool m_rememberDestroyed = true;
    std::set<std::string> m_authored;   // the current level's PersistentIdentity ids at its start
};

}   // namespace shockwave
