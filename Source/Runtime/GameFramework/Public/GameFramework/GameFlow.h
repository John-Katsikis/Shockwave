#pragma once

// GameFlow: asking to go to another level, and following how it goes (S05,
// docs/LevelLifetime.md, section 6 of the shipping plan).
//
// A travel request gets a ticket. The ticket moves through phases, has an honest progress
// and ends exactly once: Succeeded, Failed or Cancelled, with a stable code.
//
//   const TravelTicket t = game.flow().request("factory", "entrance");
//   if (!t) log(t.message);                       // Busy, UnknownLevel, ...
//   ...
//   if (auto s = game.flow().status(t.id)) bar.set(s->progress);
//   game.flow().cancel(t.id);                     // only until the old level retires
//
// One travel at a time: a request while another ticket is active is refused as Busy and
// the active ticket carries on. A request made from beginPlay (a redirect) is the
// exception: it runs right after the level that made it starts.
//
// LevelHost implements it for the player, the editor and tests; tests can give a
// GameContext a fake. Game code reaches it through LevelFlow (game.flow()), which carries
// its level's epoch, so a callback left over from an earlier level can't travel.
//
// Main thread only. Completions are delivered after the change has finished (never in
// the middle of one), so an observer may request again but never sees half a level.

#include <Engine/World.h>
#include <GameFramework/LevelLifetime.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace shockwave {

enum class TravelReason { NewGame, Exit, Restart, Checkpoint, LoadSave, Menu, OnlineMap };
const char* travelReasonName(TravelReason reason);   // "new_game", "exit", "restart", ...

enum class TravelPhase {
    Queued,   // accepted; nothing has happened yet
    Validating,
    Reading,   // the scene, into a separate world
    Preparing,   // its dependency closure and the budget
    AwaitingBoundary,   // ready: the current level ends at the next frame boundary
    Retiring,   // the current level is ending: no going back from here
    Uploading,   // the new level's world and assets
    Starting,   // its session and beginPlay
    Succeeded,
    Failed,
    Cancelled,
};
const char* travelPhaseName(TravelPhase phase);   // "queued", ..., "awaiting_boundary", ...
bool travelPhaseTerminal(TravelPhase phase);
// Until Retiring: the current level still plays on if it's cancelled.
bool travelPhaseCancellable(TravelPhase phase);
// The progress a ticket shows on reaching a phase: the shipping plan's weights (validate
// .05, read .15, prepare .25, retire .10, upload .25, start .20). Only Succeeded is 1.
float travelPhaseProgress(TravelPhase phase);

// Stable codes, for UI, the command line and reports (shipping plan section 24). The
// message beside one says what it was about, and may change.
enum class TravelError {
    None,
    Busy,   // another travel is active; it carries on
    NotPlaying,   // no level is playing to travel from
    StaleLevel,   // asked from a level that has already ended
    UnknownLevel,   // not in the level catalog
    UnknownSpawn,   // the destination has no LevelSpawn with that id
    InvalidRequest,   // an empty level, a reset outside NewGame, a disabled exit, no menu level
    AuthorityRequired,   // online: map travel is the host's, and it isn't supported yet (S11)
    UnknownTicket,   // never issued, or too old to remember
    AlreadyFinished,   // the ticket has ended
    TooLateToCancel,   // the old level is already retiring; the travel carries on
    LoadFailed,   // the scene or its assets didn't load
    BudgetExceeded,   // the level wouldn't fit the asset budget
    StartFailed,   // the new level didn't start: nothing is playing
    Cancelled,   // cancelled, replaced by Stop, or Play ended first
    RedirectCycle,   // levels kept asking for another before playing
};
const char* travelErrorName(TravelError error);   // "None", "Busy", ... as in the enum

struct TravelRequest {
    std::string level;
    std::string spawn;   // empty: the catalog's default_spawn
    TravelReason reason = TravelReason::Exit;
    bool resetCampaign = false;   // only with NewGame, after the player confirmed (S06)
    uint64_t fromEpoch = 0;   // the level asking (LevelFlow sets it); 0: the application
};

// What a request returned: a ticket, or why there's none.
struct TravelTicket {
    uint64_t id = 0;
    TravelError error = TravelError::None;
    std::string message;
    explicit operator bool() const { return id != 0; }
};

// A ticket now.
struct TravelProgress {
    uint64_t ticket = 0;
    TravelRequest request;
    std::string from;   // the level it leaves
    TravelPhase phase = TravelPhase::Queued;
    float progress = 0;   // finite, 0..1, never decreasing for one ticket; 1 only once Succeeded
    TravelError code = TravelError::None;
    std::string message;   // why it failed or was cancelled
    bool cancellable = true;
    uint64_t epoch = 0;   // the epoch it arrived in (Succeeded), else the one it left
    double durationMs = 0;   // accepted to ended, once ended
};

// Delivered once per ticket, after the change has finished.
struct TravelCompletion {
    uint64_t ticket = 0;
    TravelRequest request;
    std::string from, to;   // to: the level playing now (empty if nothing is)
    TravelStatus status = TravelStatus::Failed;
    TravelError code = TravelError::None;
    std::string message;
    uint64_t epoch = 0;
    double durationMs = 0;
};

class GameFlow {
public:
    virtual ~GameFlow() = default;

    virtual TravelTicket request(TravelRequest request) = 0;
    // TravelError::None when it was cancelled; UnknownTicket, AlreadyFinished or
    // TooLateToCancel (the travel then carries on) otherwise. Never touches another ticket.
    virtual TravelError cancel(uint64_t ticket) = 0;
    // An active ticket, or one of the last 64 that ended. Nothing for an unknown one.
    virtual std::optional<TravelProgress> status(uint64_t ticket) const = 0;
    // The active ticket, or 0.
    virtual uint64_t activeTicket() const = 0;
    // The level playing now, and its epoch.
    virtual const LevelRequest& currentLevel() const = 0;
    virtual uint64_t currentEpoch() const = 0;
    // The current level again, at the spawn it was entered at (reason Restart).
    virtual TravelTicket restartLevel(uint64_t fromEpoch = 0) = 0;
    // The game's menu level (reason Menu): InvalidRequest when it has none.
    virtual TravelTicket returnToMenu(uint64_t fromEpoch = 0) = 0;
    // Leaves through a LevelExit of the current level: its destination and spawn, reason
    // Exit. A disabled exit is InvalidRequest; a one-shot exit is disabled once accepted,
    // so a second contact doesn't travel again. Busy while another travel is active.
    virtual TravelTicket useExit(Entity exit, uint64_t fromEpoch = 0) = 0;

    // ---- Observers ----
    // Ends the subscription when it goes (or with reset()). Safe to outlive the flow.
    class Subscription {
    public:
        Subscription() = default;
        ~Subscription() { reset(); }
        Subscription(Subscription&& other) noexcept : m_list(std::move(other.m_list)), m_id(other.m_id) {}
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                reset();
                m_list = std::move(other.m_list);
                m_id = other.m_id;
            }
            return *this;
        }
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        void reset();
        bool active() const { return !m_list.expired(); }

    private:
        friend class GameFlow;
        std::weak_ptr<std::map<uint64_t, std::function<void(const TravelCompletion&)>>> m_list;
        uint64_t m_id = 0;
    };
    // Called once for every ticket as it ends, after the change. Main thread.
    [[nodiscard]] Subscription subscribe(std::function<void(const TravelCompletion&)> observer);

protected:
    // The implementation calls this with each completion, outside any change.
    void notifyObservers(const TravelCompletion& completion);

private:
    using Observers = std::map<uint64_t, std::function<void(const TravelCompletion&)>>;
    std::shared_ptr<Observers> m_observers = std::make_shared<Observers>();
    uint64_t m_nextObserver = 1;
};

// game.flow(): the flow as seen from one level. Every request says which level asked, so
// one from a level that has ended is refused (StaleLevel). Without a flow (tools, tests,
// a session without a LevelHost) every request is refused with NotPlaying.
class LevelFlow {
public:
    LevelFlow() = default;
    LevelFlow(GameFlow* flow, uint64_t epoch) : m_flow(flow), m_epoch(epoch) {}

    bool available() const { return m_flow != nullptr; }
    TravelTicket request(std::string level, std::string spawn = {}, TravelReason reason = TravelReason::Exit) const;
    TravelTicket request(TravelRequest request) const;
    TravelTicket restartLevel() const;
    TravelTicket returnToMenu() const;
    TravelTicket useExit(Entity exit) const;
    TravelError cancel(uint64_t ticket) const;
    std::optional<TravelProgress> status(uint64_t ticket) const;
    uint64_t activeTicket() const { return m_flow ? m_flow->activeTicket() : 0; }
    // This level's id, as the flow knows it ("" without one).
    std::string level() const { return m_flow ? m_flow->currentLevel().level : std::string(); }
    uint64_t epoch() const { return m_epoch; }

private:
    GameFlow* m_flow = nullptr;
    uint64_t m_epoch = 0;
};

}   // namespace shockwave
