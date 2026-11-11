#include <GameFramework/LevelHost.h>

#include <Circuits/CircuitWorld.h>
#include <Core/Log.h>
#include <Engine/Engine.h>
#include <Engine/Hierarchy.h>
#include <Engine/Levels.h>
#include <Input/Input.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>

namespace shockwave {

const char* startReasonName(StartReason reason) {
    switch (reason) {
    case StartReason::Fresh: return "fresh";
    case StartReason::RestoreSave: return "restore_save";
    case StartReason::OnlineBaseline: return "online_baseline";
    }
    return "fresh";
}

const char* travelStatusName(TravelStatus status) {
    switch (status) {
    case TravelStatus::Succeeded: return "succeeded";
    case TravelStatus::Failed: return "failed";
    case TravelStatus::Cancelled: return "cancelled";
    }
    return "failed";
}

const char* loadPhaseName(int phase) {
    static const char* const names[] = {"validate", "read", "prepare", "retire", "upload", "start"};
    return phase >= 0 && phase < 6 ? names[phase] : "";
}

namespace {
constexpr size_t kKeptResults = 32;

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point& from) {
    const auto now = Clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - from).count();
    from = now;
    return ms;
}

std::string leaseOwner(const LevelRequest& level, uint64_t epoch) {
    return "level " + level.level + " (epoch " + std::to_string(epoch) + ")";
}

LevelRequest levelOf(const TravelRequest& request) { return {request.level, request.spawn}; }

// A plan or load that failed: the budget, or anything else that kept it from loading.
TravelError codeFor(const std::string& error) {
    return error.rfind("BudgetExceeded", 0) == 0 ? TravelError::BudgetExceeded : TravelError::LoadFailed;
}

struct Reset {
    uint64_t& value;
    ~Reset() { value = 0; }
};
}   // namespace

// The level's closure against what's resident, before anything ends. False when it can't
// be listed or doesn't fit the budget.
bool LevelHost::planAssets(
    const LevelRequest& request, const World& level, std::vector<AssetCost>& assets, LoadReport& load,
    std::string& error) {
    if (!m_residency || !m_hooks.assets) return true;
    try {
        if (!m_hooks.assets(request, level, assets, error)) return false;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    const AssetResidency::Plan plan = m_residency->plan(assets, m_lease.valid() ? &m_lease : nullptr);
    load.assets = plan.shared + plan.added;
    load.shared = plan.shared;
    load.added = plan.added;
    load.retired = plan.retired;
    load.addedCpuBytes = plan.addedCpu;
    load.addedGpuBytes = plan.addedGpu;
    load.retiredCpuBytes = plan.retiredCpu;
    load.retiredGpuBytes = plan.retiredGpu;
    load.residentCpuBytes = plan.afterCpu;
    load.residentGpuBytes = plan.afterGpu;
    load.peakCpuBytes = plan.peakCpu;
    load.peakGpuBytes = plan.peakGpu;
    return m_residency->fits(plan, error);
}

void LevelHost::report(LoadReport load) {
    load.totalMs = load.validateMs + load.readMs + load.prepareMs + load.retireMs + load.uploadMs + load.startMs;
    if (m_residency) {
        load.loadedAssets = size_t(m_residency->loadedCount() - m_countsAtStart.first);
        load.unloadedAssets = size_t(m_residency->unloadedCount() - m_countsAtStart.second);
        load.pendingFrees = m_residency->pendingFrees();
    }
    m_reports.push_back(load);
    if (m_reports.size() > kKeptResults) m_reports.erase(m_reports.begin());
    if (onLoadReport) onLoadReport(load);
}

LevelHost::LevelHost(World& world, JobSystem& jobs, GameInstance& instance, Engine* engine, Hooks hooks)
    : m_world(world), m_jobs(jobs), m_instance(instance), m_engine(engine), m_hooks(std::move(hooks)) {}

LevelHost::~LevelHost() {
    try {
        stop();
    } catch (const std::exception& e) {
        SW_LOG(LogGame, Error, "Level cleanup: {}", e.what());
    } catch (...) {
        SW_LOG(LogGame, Error, "Level cleanup failed");
    }
}

void LevelHost::start(LevelRequest level, StartReason reason) {
    if (m_session) throw std::logic_error("LevelHost::start: a level is already playing");
    if (m_epoch == std::numeric_limits<uint64_t>::max())
        throw std::runtime_error("level epochs ran out: restart the game");
    m_failure.clear();
    m_redirects = 0;
    m_chain.clear();
    LoadReport load;
    load.request = level;
    auto clock = Clock::now();
    if (m_residency) m_countsAtStart = {m_residency->loadedCount(), m_residency->unloadedCount()};
    // The level is already in the world: its lease first, so a budget it can't fit stops
    // it before anything starts.
    std::vector<AssetCost> assets;
    std::string error;
    if (!planAssets(level, m_world, assets, load, error)) {
        load.status = TravelStatus::Failed;
        load.error = error;
        load.epoch = m_epoch;
        load.prepareMs = msSince(clock);
        report(load);
        throw std::runtime_error("level \"" + level.level + "\": " + error);
    }
    load.prepareMs = msSince(clock);
    m_current = std::move(level);
    m_world.setLevelEpoch(++m_epoch);
    load.epoch = m_epoch;
    if (m_residency && m_hooks.assets) {
        m_lease = m_residency->acquire(leaseOwner(m_current, m_epoch), assets);
        // On demand: what isn't in yet. The world is already loaded, so the caller brought
        // the scene's assets in first; this is a no-op then, and catches what it missed.
        if (!m_residency->load(m_lease, error)) {
            m_lease.release();
            load.uploadMs = msSince(clock);
            load.status = TravelStatus::Failed;
            load.error = error;
            report(load);
            throw std::runtime_error("level \"" + m_current.level + "\": " + error);
        }
    }
    load.uploadMs = msSince(clock);
    // The application's graphics, put back by stop(); levels keep the player's changes.
    if (m_engine && m_engine->guiEnabled() && !m_settings) m_settings = m_engine->graphics().requested();
    const LevelArrival arrival = levelArrival(m_world, m_current.spawn);
    if (!m_current.spawn.empty() && arrival.entity == kNullEntity)
        SW_LOG(LogGame, Warning, "Level \"{}\" has no LevelSpawn \"{}\"", m_current.level, m_current.spawn);
    noteAuthored();
    try {
        startSession(reason, arrival);
    } catch (const std::exception& e) {
        m_lease.release();
        restoreSettings();
        load.startMs = msSince(clock);
        load.status = TravelStatus::Failed;
        load.error = e.what();
        report(load);
        throw;
    } catch (...) {
        m_lease.release();
        restoreSettings();
        throw;
    }
    load.startMs = msSince(clock);
    report(load);
    arrived();
    m_entryCampaign = m_campaign;
    deliver();
}

void LevelHost::startSession(StartReason reason, const LevelArrival& arrival) {
    m_session = std::make_unique<PlaySession>(m_world, m_jobs, m_instance, m_engine);
    m_session->setWorldPolicy(SessionWorldPolicy::OwnedRuntimeWorld);
    m_session->setSettingsPolicy(SessionSettingsPolicy::PreserveUserSettings);
    m_session->setStartReason(reason);
    m_session->setLevelEpoch(m_epoch);
    m_session->setFlow(this);
    m_session->setArrival(arrival);
    m_session->setCampaign(&m_campaign);
    if (m_hooks.configure) m_hooks.configure(*m_session, m_current);
    if (m_nextNetwork) {   // the next map of a rotation: a client rejoins with a new baseline
        if (m_nextNetwork->role == net::NetworkRole::Client) m_session->setStartReason(StartReason::OnlineBaseline);
        m_session->setNetworkStart(std::move(*m_nextNetwork), m_nextServices);
        m_nextNetwork.reset();
    }
    m_starting = true;
    try {
        m_session->start();
    } catch (...) {
        m_starting = false;
        m_session.reset();   // start() tore its subsystems down already
        throw;
    }
    m_starting = false;
    if (m_hooks.started) m_hooks.started(*m_session, m_current);
}

// A new level starts clean: nothing held through the loading screen counts, the camera
// doesn't blend from the old level's view, and the time spent loading isn't caught up.
// (The session's start already made every previous transform equal to its transform.)
void LevelHost::arrived() {
    m_failure.clear();   // something plays again
    if (!m_engine) return;
    m_engine->input().holdUntilReleased();
    m_engine->renderer().cutCamera();
    m_engine->discardFrameTime();
}

void LevelHost::endSession() {
    if (!m_session) return;
    try {
        if (m_hooks.stopping && m_session->active()) m_hooks.stopping(*m_session);
        m_session->stop();
    } catch (const std::exception& e) {
        // The level is ending either way: its subsystems are gone, so carry on.
        SW_LOG(LogGame, Error, "Level \"{}\" didn't end cleanly: {}", m_current.level, e.what());
    }
    m_session.reset();
}

void LevelHost::restoreSettings() {
    if (!m_settings) return;
    std::string error;
    if (m_engine && !m_engine->graphics().apply(*m_settings, error))
        SW_LOG(LogGame, Warning, "Couldn't put the graphics settings back: {}", error);
    m_settings.reset();
}

void LevelHost::stop() {
    if (m_processing) m_stopped = true;
    if (m_candidate) {
        const uint64_t id = m_candidate->next.id;
        m_candidate.reset();   // the session ends below: nothing to resume
        finish(id, TravelStatus::Cancelled, TravelError::Cancelled, "Play stopped first");
    }
    if (m_pending) {
        const uint64_t id = m_pending->id;
        m_pending.reset();
        finish(id, TravelStatus::Cancelled, TravelError::Cancelled, "Play stopped first");
    }
    endSession();
    m_lease.release();   // after the session: nothing reads its assets any more
    restoreSettings();
    m_failure.clear();   // Play has ended: there's nothing to recover
    m_redirects = 0;
    m_chain.clear();
    deliver();
}

// ---- Requests ---------------------------------------------------------------------------------

TravelTicket LevelHost::refuse(TravelError code, std::string message, const TravelRequest& request) const {
    SW_LOG(LogGame, Warning, "Can't go to level \"{}\": {} ({})", request.level, message, travelErrorName(code));
    return {0, code, std::move(message)};
}

uint64_t LevelHost::requestLevel(LevelRequest level) {
    TravelRequest r;
    r.level = std::move(level.level);
    r.spawn = std::move(level.spawn);
    return request(std::move(r)).id;
}

TravelTicket LevelHost::request(TravelRequest r) {
    // After a level failed to start nothing plays, but the application may still go
    // somewhere (the error screen's Retry and Main menu); levels can't, they've ended.
    const bool recovering = !m_failure.empty() && !m_session;
    if (!recovering && (!m_session || (!m_session->active() && !m_starting)))
        return refuse(TravelError::NotPlaying, "nothing is playing", r);
    if (recovering && r.fromEpoch) return refuse(TravelError::StaleLevel, "asked from a level that didn't start", r);
    if (r.fromEpoch && r.fromEpoch != m_epoch)
        return refuse(
            TravelError::StaleLevel,
            "asked from level epoch " + std::to_string(r.fromEpoch) + ", which has ended (now " +
                std::to_string(m_epoch) + ")",
            r);
    if (r.level.empty()) return refuse(TravelError::InvalidRequest, "no level named", r);
    if (r.resetCampaign && r.reason != TravelReason::NewGame)
        return refuse(TravelError::InvalidRequest, "only a new game resets the campaign", r);
    // Online only the host changes maps, for everyone: a map rotation (S11).
    net::NetworkWorld* network = m_session && m_session->online() ? m_session->network() : nullptr;
    if (network && !m_followingRotation) {
        if (!network->authority()) return refuse(TravelError::AuthorityRequired, "only the host changes maps", r);
        if (network->state() != net::NetworkState::Active || !m_hooks.networkScene)
            return refuse(TravelError::AuthorityRequired, "this network session can't change maps", r);
        r.reason = TravelReason::OnlineMap;
        r.restore.reset();
    }
    // One at a time; a level asking for another from its beginPlay is a redirect.
    const uint64_t active = m_pending ? m_pending->id : m_candidate ? m_candidate->next.id : m_inFlight;
    if (active && !(m_starting && !m_pending)) {
        const auto it = m_tickets.find(active);
        const std::string where = it != m_tickets.end() ? it->second.request.level : std::string("another level");
        return refuse(
            TravelError::Busy, "travel to \"" + where + "\" is under way (ticket " + std::to_string(active) + ")", r);
    }
    if (m_hooks.resolve) {
        TravelError code = TravelError::None;
        std::string error;
        bool ok = false;
        try {
            ok = m_hooks.resolve(r, code, error);
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!ok) return refuse(code == TravelError::None ? TravelError::UnknownLevel : code, error, r);
    }
    // The host announces the map now, so every player loads it while the host does.
    if (network && !m_followingRotation) {
        std::string error;
        const uint16_t scene = m_hooks.networkScene(r.level, error);
        if (!scene)
            return refuse(
                TravelError::UnknownLevel,
                error.empty() ? "level \"" + r.level + "\" isn't a scene of the network manifest" : error, r);
        const net::NetworkSettings& s = network->start().settings;
        net::MapRotationRequest rotation;
        rotation.level = r.level;
        rotation.sceneId = scene;
        rotation.readySeconds = s.rotationReadySeconds;
        rotation.dropUnready = s.rotationDropUnready;
        if (auto prepared = network->prepareRotation(rotation); !prepared)
            return refuse(TravelError::Busy, prepared.error().message, r);
    }
    const uint64_t id = m_nextId++;
    if (network) {
        m_rotationTicket = id;
        m_rotationReadySent = false;
    }
    TravelProgress progress;
    progress.ticket = id;
    progress.request = r;
    progress.request.restore.reset();   // the pending request holds the save; tickets only describe it
    progress.from = m_current.level;
    progress.epoch = m_epoch;
    m_tickets[id] = progress;
    m_accepted[id] = Clock::now();
    // A level asking before it has ticked (from beginPlay) redirects; the application never
    // does: its request gets the loading screen and the boundary like any other.
    const bool redirect = m_starting || (r.fromEpoch && m_session && m_session->ticks() == 0);
    m_pending = Pending{id, std::move(r), redirect};
    deliver();
    return {id, TravelError::None, {}};
}

TravelError LevelHost::cancel(uint64_t ticket) {
    if (m_pending && m_pending->id == ticket) {
        m_pending.reset();
        finish(ticket, TravelStatus::Cancelled, TravelError::Cancelled, "cancelled");
        deliver();
        return TravelError::None;
    }
    if (m_candidate && m_candidate->next.id == ticket) {
        Candidate candidate = std::move(*m_candidate);
        m_candidate.reset();
        resume(candidate);   // the prepared world goes with it; the current level plays on
        finish(ticket, TravelStatus::Cancelled, TravelError::Cancelled, "cancelled before the level changed");
        deliver();
        return TravelError::None;
    }
    if (m_tickets.count(ticket)) return TravelError::TooLateToCancel;   // past the boundary: it carries on
    for (const TravelProgress& ended : m_ended)
        if (ended.ticket == ticket) return TravelError::AlreadyFinished;
    return TravelError::UnknownTicket;
}

std::optional<TravelProgress> LevelHost::status(uint64_t ticket) const {
    if (const auto it = m_tickets.find(ticket); it != m_tickets.end()) return it->second;
    for (auto it = m_ended.rbegin(); it != m_ended.rend(); ++it)
        if (it->ticket == ticket) return *it;
    return std::nullopt;
}

uint64_t LevelHost::activeTicket() const {
    if (m_inFlight) return m_inFlight;
    if (m_candidate) return m_candidate->next.id;
    return m_pending ? m_pending->id : 0;
}

TravelTicket LevelHost::restartLevel(uint64_t fromEpoch) {
    TravelRequest r;
    r.level = m_current.level;
    r.spawn = m_current.spawn;
    r.reason = TravelReason::Restart;
    r.fromEpoch = fromEpoch;
    return request(std::move(r));
}

TravelTicket LevelHost::rotateMap(uint64_t fromEpoch) {
    TravelRequest r;
    r.reason = TravelReason::OnlineMap;
    r.fromEpoch = fromEpoch;
    net::NetworkWorld* network = m_session && m_session->online() ? m_session->network() : nullptr;
    if (!network) return refuse(TravelError::InvalidRequest, "map rotation is for network play", r);
    const std::vector<std::string> levels = net::mapRotationLevels(network->start().settings);
    if (levels.empty()) return refuse(TravelError::InvalidRequest, "the network settings have no map_rotation", r);
    auto it = std::find(levels.begin(), levels.end(), m_current.level);
    r.level = it == levels.end() || std::next(it) == levels.end() ? levels.front() : *std::next(it);
    return request(std::move(r));
}

// A client follows the host: each announced map starts a travel of its own, held at the
// boundary until the commit. One it can't prepare is refused (MapReject).
void LevelHost::followRotation() {
    net::NetworkWorld* network = m_session && m_session->online() ? m_session->network() : nullptr;
    if (!network || network->authority()) return;
    const net::MapRotation& rotation = network->rotation();
    if (rotation.phase != net::RotationPhase::Preparing || rotation.transitionId == m_rotationSeen) return;
    m_rotationSeen = rotation.transitionId;
    std::string error;
    const uint16_t scene = m_hooks.networkScene ? m_hooks.networkScene(rotation.level, error) : 0;
    if (scene == 0 || scene != rotation.sceneId) {
        SW_LOG(
            LogGame, Warning, "Can't follow the host to \"{}\": {}", rotation.level,
            error.empty() ? std::string("its scene differs here") : error);
        network->rejectRotation(net::ReasonCode::ContentMismatch);
        return;
    }
    TravelRequest r;
    r.level = rotation.level;
    r.reason = TravelReason::OnlineMap;
    m_followingRotation = true;
    const TravelTicket ticket = request(std::move(r));
    m_followingRotation = false;
    if (!ticket) {
        SW_LOG(LogGame, Warning, "Can't follow the host to \"{}\": {}", rotation.level, ticket.message);
        network->rejectRotation(net::ReasonCode::MapNotLoaded);
        return;
    }
    m_rotationTicket = ticket.id;
    m_rotationReadySent = false;
}

bool LevelHost::holdForRotation() {
    if (!m_rotationTicket || m_candidate->next.id != m_rotationTicket) return false;
    net::NetworkWorld* network = m_session && m_session->online() ? m_session->network() : nullptr;
    const net::RotationPhase phase = network && network->state() != net::NetworkState::Failed
                                         ? network->rotation().phase
                                         : net::RotationPhase::Aborted;
    if (phase == net::RotationPhase::Committed) return false;
    if (phase == net::RotationPhase::Preparing) {
        if (!m_rotationReadySent) {   // prepared: say so (the host may commit at once)
            m_rotationReadySent = true;
            network->rotationReady();
            if (network->rotation().phase == net::RotationPhase::Committed) return false;
        }
        return true;
    }
    // Called off (or the session ended): the current map plays on.
    const uint64_t id = m_candidate->next.id;
    const char* why = network ? net::reasonName(network->rotation().abortReason) : nullptr;
    resume(*m_candidate);
    m_candidate.reset();
    finish(
        id, TravelStatus::Failed, TravelError::RotationAborted,
        std::string("the map change was called off") + (why ? std::string(" (") + why + ")" : std::string()));
    return true;
}

TravelTicket LevelHost::returnToMenu(uint64_t fromEpoch) {
    TravelRequest r;
    r.level = m_menuLevel;
    r.reason = TravelReason::Menu;
    r.fromEpoch = fromEpoch;
    if (m_menuLevel.empty()) return refuse(TravelError::InvalidRequest, "this game has no menu level", r);
    return request(std::move(r));
}

TravelTicket LevelHost::useExit(Entity exit, uint64_t fromEpoch) {
    TravelRequest r;
    r.reason = TravelReason::Exit;
    r.fromEpoch = fromEpoch;
    const LevelExit* found = m_world.alive(exit) ? m_world.get<LevelExit>(exit) : nullptr;
    if (!found) return refuse(TravelError::InvalidRequest, "that isn't a LevelExit", r);
    r.level = found->destination;
    r.spawn = found->spawn;
    if (!found->enabled) return refuse(TravelError::InvalidRequest, "this exit is disabled", r);
    if (fromEpoch && fromEpoch != m_epoch) return request(std::move(r));   // refused as stale
    const bool oneShot = found->oneShot;
    const TravelTicket ticket = request(std::move(r));
    if (ticket && oneShot)
        if (LevelExit* used = m_world.get<LevelExit>(exit)) used->enabled = false;   // a second contact can't
    return ticket;
}

// ---- Tickets ----------------------------------------------------------------------------------

void LevelHost::setPhase(uint64_t id, TravelPhase phase) {
    const auto it = m_tickets.find(id);
    if (it == m_tickets.end()) return;
    TravelProgress& p = it->second;
    p.phase = phase;
    p.progress = std::max(p.progress, travelPhaseProgress(phase));
    p.cancellable = travelPhaseCancellable(phase);
}

void LevelHost::finish(uint64_t id, TravelStatus status, TravelError code, std::string error) {
    if (id == m_rotationTicket) {
        m_rotationTicket = 0;
        // This side can't go: the host calls the rotation off, a client says it can't follow.
        if (status != TravelStatus::Succeeded && m_session)
            if (net::NetworkWorld* network = m_session->network();
                network && network->rotation().phase == net::RotationPhase::Preparing) {
                if (network->authority())
                    network->abortRotation(net::ReasonCode::MapNotLoaded);
                else
                    network->rejectRotation(net::ReasonCode::MapNotLoaded);
            }
    }
    TravelProgress p;
    if (const auto it = m_tickets.find(id); it != m_tickets.end()) {
        p = std::move(it->second);
        m_tickets.erase(it);
    }
    p.ticket = id;
    p.phase = status == TravelStatus::Succeeded   ? TravelPhase::Succeeded
              : status == TravelStatus::Cancelled ? TravelPhase::Cancelled
                                                  : TravelPhase::Failed;
    if (status == TravelStatus::Succeeded) p.progress = 1.0f;
    p.code = code;
    p.message = std::move(error);
    p.cancellable = false;
    p.epoch = m_epoch;
    if (const auto it = m_accepted.find(id); it != m_accepted.end()) {
        p.durationMs = std::chrono::duration<double, std::milli>(Clock::now() - it->second).count();
        m_accepted.erase(it);
    }
    if (status == TravelStatus::Succeeded)
        SW_LOG(LogGame, Info, "Level \"{}\" (epoch {})", p.request.level, m_epoch);
    else
        SW_LOG(
            LogGame, Warning, "Level \"{}\" {} ({}): {}", p.request.level, travelStatusName(status),
            travelErrorName(code), p.message);
    m_ended.push_back(p);
    if (m_ended.size() > kKeptTickets) m_ended.erase(m_ended.begin());
    TravelResult result{id, levelOf(p.request), status, p.message, m_epoch};
    m_results.push_back(result);
    if (m_results.size() > kKeptResults) m_results.erase(m_results.begin());
    TravelCompletion completion;
    completion.ticket = id;
    completion.request = p.request;
    completion.from = p.from;
    completion.to = playing() ? m_current.level : std::string();
    completion.status = status;
    completion.code = code;
    completion.message = p.message;
    completion.epoch = m_epoch;
    completion.durationMs = p.durationMs;
    m_outbox.push_back(std::move(completion));
}

void LevelHost::deliver() {
    if (m_processing || m_delivering) return;
    m_delivering = true;
    struct Done {
        bool& flag;
        ~Done() { flag = false; }
    } done{m_delivering};
    while (!m_outbox.empty()) {
        const TravelCompletion c = std::move(m_outbox.front());
        m_outbox.erase(m_outbox.begin());
        if (onResult) onResult(TravelResult{c.ticket, levelOf(c.request), c.status, c.message, c.epoch});
        notifyObservers(c);
        // The circuits of the level playing now: the new one after a change, the same one
        // after a failure or cancel before it, none when nothing plays. A level that ended
        // never hears of the one after it.
        if (m_session && m_session->active())
            if (CircuitWorld* circuits = m_session->circuits())
                circuits->travelFinished(
                    c.ticket <= uint64_t(std::numeric_limits<int32_t>::max()) ? int32_t(c.ticket) : 0,
                    c.status == TravelStatus::Succeeded, c.request.level, c.from, travelErrorName(c.code), c.message);
    }
}

// ---- Carrying it out --------------------------------------------------------------------------

void LevelHost::process() {
    if (m_processing) return;   // a hook called back in: the loop below picks it up
    {
        m_processing = true;
        struct Done {
            bool& flag;
            ~Done() { flag = false; }
        } done{m_processing};
        m_stopped = false;
        followRotation();
        while (true) {
            if (m_candidate) {   // prepared last frame: across the boundary now
                if (holdForRotation()) break;   // online: until every player is ready
                Candidate candidate = std::move(*m_candidate);
                m_candidate.reset();
                if (!commit(std::move(candidate))) break;
                continue;   // a redirect from its beginPlay
            }
            if (!m_pending) break;
            const Pending next = *m_pending;
            m_pending.reset();
            Candidate candidate;
            if (!prepare(next, candidate)) continue;   // a request made by a hook meanwhile still gets its turn
            setPhase(next.id, TravelPhase::AwaitingBoundary);
            if ((m_staged || next.id == m_rotationTicket) && !next.redirect) {
                // A frame renders before the current level ends: a loading screen shows, and
                // the ticket can still be cancelled. The level waits, paused, meanwhile.
                if (m_session && m_session->state() == PlaySession::State::Playing && !m_session->online())
                    candidate.paused = m_session->pause();
                m_candidate = std::move(candidate);
                break;
            }
            if (!commit(std::move(candidate))) break;
        }
    }
    deliver();
}

void LevelHost::resume(Candidate& candidate) {
    if (candidate.paused && m_session && m_session->state() == PlaySession::State::Paused) m_session->start();
    candidate.paused = false;
}

bool LevelHost::prepare(const Pending& next, Candidate& out) {
    out.next = next;
    LoadReport& load = out.load;
    load.request = levelOf(next.request);
    load.from = m_current.level;
    if (m_residency) m_countsAtStart = {m_residency->loadedCount(), m_residency->unloadedCount()};
    load.epoch = m_epoch;
    auto clock = Clock::now();
    auto fail = [&](TravelStatus status, TravelError code, std::string why, double LoadReport::*phase) {
        load.*phase += msSince(clock);
        load.status = status;
        load.error = why;
        load.epoch = m_epoch;
        report(load);
        finish(next.id, status, code, std::move(why));
        return false;
    };
    setPhase(next.id, TravelPhase::Validating);
    if (next.redirect) {
        ++m_redirects;
        m_chain.push_back(next.request.level);
    } else {
        m_redirects = 1;
        m_chain = {next.request.level};
    }
    if (m_redirects > kMaxRedirects) {
        std::string chain;
        for (const auto& id : m_chain) chain += (chain.empty() ? "" : " -> ") + id;
        m_redirects = 0;
        m_chain.clear();
        return fail(
            TravelStatus::Failed, TravelError::RedirectCycle,
            "more than " + std::to_string(kMaxRedirects) +
                " levels asked for another before playing (a cycle?): " + chain,
            &LoadReport::validateMs);
    }
    if (m_epoch == std::numeric_limits<uint64_t>::max())
        return fail(
            TravelStatus::Failed, TravelError::StartFailed, "level epochs ran out: restart the game",
            &LoadReport::validateMs);
    load.validateMs = msSince(clock);
    std::string error;
    if (m_residency && m_residency->loader() && m_hooks.assets) {
        // On demand: its closure from its files, against what's resident. The scene is read
        // once its assets are in, after the current level has gone.
        out.onDemand = true;
        setPhase(next.id, TravelPhase::Preparing);
        const World unread;
        if (!planAssets(load.request, unread, out.assets, load, error))
            return fail(TravelStatus::Failed, codeFor(error), error, &LoadReport::prepareMs);
        load.prepareMs = msSince(clock);
        return true;
    }
    // Read: the next level loads beside the current one, which keeps playing if it fails.
    setPhase(next.id, TravelPhase::Reading);
    out.prepared = std::make_unique<World>();
    const bool hadSession = m_session != nullptr;   // none after a failed start
    bool loaded = false;
    try {
        loaded = m_hooks.load && m_hooks.load(load.request, *out.prepared, error);
        if (!m_hooks.load) error = "this application can't load levels";
    } catch (const std::exception& e) {
        error = e.what();
    }
    if (m_stopped || (hadSession && !m_session))   // Stop (a quit, say) came while it loaded
        return fail(
            TravelStatus::Cancelled, TravelError::Cancelled, "Play stopped while it loaded", &LoadReport::readMs);
    if (!loaded)
        return fail(
            TravelStatus::Failed, TravelError::LoadFailed, error.empty() ? "it didn't load" : error,
            &LoadReport::readMs);
    if (!next.request.spawn.empty() && findLevelSpawn(*out.prepared, next.request.spawn) == kNullEntity)
        return fail(
            TravelStatus::Failed, TravelError::UnknownSpawn,
            "level \"" + next.request.level + "\" has no LevelSpawn \"" + next.request.spawn + "\"",
            &LoadReport::readMs);
    load.readMs = msSince(clock);
    // Prepare: what it needs, against what's resident. Refused here, the current level plays on.
    setPhase(next.id, TravelPhase::Preparing);
    if (!planAssets(load.request, *out.prepared, out.assets, load, error))
        return fail(TravelStatus::Failed, codeFor(error), error, &LoadReport::prepareMs);
    load.prepareMs = msSince(clock);
    return true;
}

bool LevelHost::commit(Candidate candidate) {
    // A committed map rotation: the next session is the one the old one announced.
    if (candidate.next.id == m_rotationTicket && m_session)
        if (net::NetworkWorld* network = m_session->network();
            network && network->rotation().phase == net::RotationPhase::Committed) {
            m_nextNetwork = network->nextStart();
            m_nextServices = m_session->networkServices();
        }
    m_inFlight = candidate.next.id;
    Reset clear{m_inFlight};
    auto clock = Clock::now();
    const Pending& next = candidate.next;
    LoadReport& load = candidate.load;
    if (candidate.onDemand) return travelOnDemand(candidate, clock);
    // Retire: the current level ends, then the new one starts in the same World. The new
    // lease is taken before the old one goes, so what both use keeps a ref throughout;
    // only the old level's own assets retire.
    setPhase(next.id, TravelPhase::Retiring);
    recordDestroyed(next.request);
    endSession();
    AssetResidency::Lease arriving;
    if (m_residency && m_hooks.assets)
        arriving = m_residency->acquire(leaseOwner(load.request, m_epoch + 1), candidate.assets);
    m_lease.release();
    m_lease = std::move(arriving);
    load.retireMs = msSince(clock);
    setPhase(next.id, TravelPhase::Uploading);
    m_world = std::move(*candidate.prepared);
    m_world.setLevelEpoch(++m_epoch);
    m_current = load.request;
    load.epoch = m_epoch;
    load.uploadMs = msSince(clock);
    setPhase(next.id, TravelPhase::Starting);
    try {
        startLevel(next.request);
    } catch (const std::exception& e) {
        load.startMs = msSince(clock);
        failStarted(
            next, load, TravelError::StartFailed, "level \"" + m_current.level + "\" didn't start: " + e.what());
        return false;
    }
    load.startMs = msSince(clock);
    report(load);
    arrived();
    finish(next.id, TravelStatus::Succeeded, TravelError::None, {});
    return true;
}

// The campaign as the request leaves it, the session, and a save to put in: what every
// change does once the new level's world is in place.
void LevelHost::startLevel(const TravelRequest& request) {
    // New game, confirmed, starts the campaign over; Restart level goes back to how it
    // was on entering, so rewards taken since aren't kept (and then taken again).
    if (request.reason == TravelReason::NewGame && request.resetCampaign) m_campaign.clear();
    if (request.reason == TravelReason::Restart) m_campaign = m_entryCampaign;
    // A save's campaign is in before anything of the level runs.
    const std::shared_ptr<SaveRestore>& restore = request.restore;
    if (restore && restore->info.format >= 2) m_campaign.read(restore->info);
    const bool restoring = restore && !restore->info.fresh;
    noteAuthored();
    startSession(restoring ? StartReason::RestoreSave : StartReason::Fresh, levelArrival(m_world, m_current.spawn));
    if (restoring) {
        try {
            m_session->restore(std::move(*restore));   // before the first tick or frame
        } catch (...) {
            endSession();
            throw;
        }
    }
    m_entryCampaign = m_campaign;
}

// ---- Destroyed authored entities (docs/SaveGames.md#things-that-stay-gone) ----------------

namespace {

std::set<std::string> persistentIds(World& world) {
    std::set<std::string> ids;
    world.each<const PersistentIdentity>([&](Entity, const PersistentIdentity& p) {
        if (!p.id.empty()) ids.insert(p.id);
    });
    return ids;
}

}   // namespace

// The level's authored identities, as it starts: what was destroyed in an earlier visit
// is taken out first, so it doesn't come back.
void LevelHost::noteAuthored() {
    m_authored.clear();
    if (!m_rememberDestroyed) return;
    const std::set<std::string>& gone = m_campaign.destroyed(m_current.level);
    if (!gone.empty()) {
        std::vector<Entity> doomed;
        m_world.each<const PersistentIdentity>([&](Entity e, const PersistentIdentity& p) {
            if (gone.count(p.id)) doomed.push_back(e);
        });
        for (Entity e : doomed)
            if (m_world.alive(e)) destroyWithChildren(m_world, e);
    }
    m_authored = persistentIds(m_world);
}

// Leaving the level (not restarting it): what was authored and isn't there any more
// goes into the campaign.
void LevelHost::recordDestroyed(const TravelRequest& next) {
    if (!m_rememberDestroyed || !m_session || next.reason == TravelReason::Restart) return;
    const std::set<std::string> alive = persistentIds(m_world);
    std::set<std::string> gone;
    for (const std::string& id : m_authored)
        if (!alive.count(id)) gone.insert(id);
    m_campaign.addDestroyed(m_current.level, gone);
}

void LevelHost::failStarted(const Pending& next, LoadReport& load, TravelError code, const std::string& why) {
    m_lease.release();
    // Nothing is playing now; the application goes back to somewhere safe.
    m_failure = why;
    SW_LOG(LogGame, Error, "{}", m_failure);
    if (m_pending) {   // asked for during the failed start
        const uint64_t orphan = m_pending->id;
        m_pending.reset();
        finish(orphan, TravelStatus::Cancelled, TravelError::Cancelled, "the level asking for it didn't start");
    }
    load.status = TravelStatus::Failed;
    load.error = m_failure;
    report(load);
    finish(next.id, TravelStatus::Failed, code, m_failure);
}

bool LevelHost::travelOnDemand(Candidate& candidate, Clock::time_point& clock) {
    const Pending& next = candidate.next;
    LoadReport& load = candidate.load;
    auto cancelled = [&] {
        m_lease.release();
        load.status = TravelStatus::Cancelled;
        load.error = "Play stopped while it loaded";
        report(load);
        finish(next.id, TravelStatus::Cancelled, TravelError::Cancelled, load.error);
        return false;
    };
    // Retire: the current level ends and its own assets unload (their GPU memory follows
    // once no frame reads it). The new lease is taken first, so shared assets stay in.
    setPhase(next.id, TravelPhase::Retiring);
    recordDestroyed(next.request);
    endSession();
    AssetResidency::Lease arriving = m_residency->acquire(leaseOwner(load.request, m_epoch + 1), candidate.assets);
    m_lease.release();
    m_lease = std::move(arriving);
    load.retireMs = msSince(clock);
    // Upload: what the new level needs that isn't in. Past the point of no return.
    setPhase(next.id, TravelPhase::Uploading);
    std::string error;
    if (!m_residency->load(m_lease, [this] { return m_stopped; }, error)) {
        load.uploadMs = msSince(clock);
        if (m_stopped) return cancelled();
        failStarted(next, load, codeFor(error), "level \"" + next.request.level + "\" didn't load: " + error);
        return false;
    }
    load.uploadMs = msSince(clock);
    // Read: its scene, now that the names it uses resolve.
    World prepared;
    bool read = false;
    try {
        read = m_hooks.load && m_hooks.load(load.request, prepared, error);
        if (!m_hooks.load) error = "this application can't load levels";
    } catch (const std::exception& e) {
        error = e.what();
    }
    load.readMs = msSince(clock);
    if (m_stopped) return cancelled();
    if (!read) {
        failStarted(
            next, load, TravelError::LoadFailed,
            "level \"" + next.request.level + "\" didn't load: " + (error.empty() ? "it didn't load" : error));
        return false;
    }
    if (!next.request.spawn.empty() && findLevelSpawn(prepared, next.request.spawn) == kNullEntity) {
        failStarted(
            next, load, TravelError::UnknownSpawn,
            "level \"" + next.request.level + "\" has no LevelSpawn \"" + next.request.spawn + "\"");
        return false;
    }
    m_world = std::move(prepared);
    m_world.setLevelEpoch(++m_epoch);
    m_current = load.request;
    load.epoch = m_epoch;
    setPhase(next.id, TravelPhase::Starting);
    try {
        startLevel(next.request);
    } catch (const std::exception& e) {
        load.startMs = msSince(clock);
        failStarted(
            next, load, TravelError::StartFailed, "level \"" + next.request.level + "\" didn't start: " + e.what());
        return false;
    }
    load.startMs = msSince(clock);
    report(load);
    arrived();
    finish(next.id, TravelStatus::Succeeded, TravelError::None, {});
    return true;
}

}   // namespace shockwave
