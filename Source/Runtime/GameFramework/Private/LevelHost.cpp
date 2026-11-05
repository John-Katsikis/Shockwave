#include <GameFramework/LevelHost.h>

#include <Core/Log.h>
#include <Engine/Engine.h>

#include <chrono>
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
    try {
        startSession(reason);
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
}

void LevelHost::startSession(StartReason reason) {
    m_session = std::make_unique<PlaySession>(m_world, m_jobs, m_instance, m_engine);
    m_session->setWorldPolicy(SessionWorldPolicy::OwnedRuntimeWorld);
    m_session->setSettingsPolicy(SessionSettingsPolicy::PreserveUserSettings);
    m_session->setStartReason(reason);
    m_session->setLevelEpoch(m_epoch);
    m_session->setTravel([this, epoch = m_epoch](const LevelRequest& request) {
        if (epoch != m_epoch) {
            SW_LOG(LogGame, Warning, "Ignored a request for level \"{}\" from an earlier level", request.level);
            return false;
        }
        return this->request(request) != 0;
    });
    if (m_hooks.configure) m_hooks.configure(*m_session, m_current);
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
    if (m_pending) {
        const Pending pending = *m_pending;
        m_pending.reset();
        finish(pending.id, pending.request, TravelStatus::Cancelled, "Play stopped first");
    }
    endSession();
    m_lease.release();   // after the session: nothing reads its assets any more
    restoreSettings();
    m_redirects = 0;
    m_chain.clear();
}

uint64_t LevelHost::request(LevelRequest level) {
    if (!m_session || (!m_session->active() && !m_starting)) {
        SW_LOG(LogGame, Warning, "Can't go to level \"{}\": nothing is playing", level.level);
        return 0;
    }
    if (m_pending) {
        const Pending replaced = *m_pending;
        m_pending.reset();
        finish(
            replaced.id, replaced.request, TravelStatus::Cancelled,
            "replaced by a request for \"" + level.level + "\"");
    }
    const uint64_t id = m_nextId++;
    m_pending = Pending{id, std::move(level), m_starting || m_session->ticks() == 0};
    return id;
}

void LevelHost::process() {
    if (m_processing) return;   // a hook called back in: the loop below picks it up
    m_processing = true;
    struct Done {
        bool& flag;
        ~Done() { flag = false; }
    } done{m_processing};
    m_stopped = false;
    while (m_pending) {
        const Pending next = *m_pending;
        m_pending.reset();
        LoadReport load;
        load.request = next.request;
        load.from = m_current.level;
        if (m_residency) m_countsAtStart = {m_residency->loadedCount(), m_residency->unloadedCount()};
        load.epoch = m_epoch;
        auto clock = Clock::now();
        auto fail = [&](TravelStatus status, std::string why, double LoadReport::*phase) {
            load.*phase += msSince(clock);
            load.status = status;
            load.error = why;
            load.epoch = m_epoch;
            report(load);
            finish(next.id, next.request, status, std::move(why));
        };
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
            fail(
                TravelStatus::Failed,
                "more than " + std::to_string(kMaxRedirects) +
                    " levels asked for another before playing (a cycle?): " + chain,
                &LoadReport::validateMs);
            return;
        }
        if (m_epoch == std::numeric_limits<uint64_t>::max()) {
            fail(TravelStatus::Failed, "level epochs ran out: restart the game", &LoadReport::validateMs);
            return;
        }
        load.validateMs = msSince(clock);
        if (m_residency && m_residency->loader() && m_hooks.assets) {
            if (!travelOnDemand(next, load, clock)) return;
            continue;
        }
        // Prepare: the next level loads beside the current one, which keeps playing if it fails.
        World prepared;
        std::string error;
        bool loaded = false;
        try {
            loaded = m_hooks.load && m_hooks.load(next.request, prepared, error);
            if (!m_hooks.load) error = "this application can't load levels";
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!m_session) {   // Stop (a quit, say) came while it loaded
            fail(TravelStatus::Cancelled, "Play stopped while it loaded", &LoadReport::readMs);
            return;
        }
        if (!loaded) {
            fail(TravelStatus::Failed, error.empty() ? "it didn't load" : error, &LoadReport::readMs);
            continue;   // a request made by a hook meanwhile still gets its turn
        }
        load.readMs = msSince(clock);
        // What it needs, against what's resident: refused here, the current level plays on.
        std::vector<AssetCost> assets;
        if (!planAssets(next.request, prepared, assets, load, error)) {
            fail(TravelStatus::Failed, error, &LoadReport::prepareMs);
            continue;
        }
        load.prepareMs = msSince(clock);
        // Commit: the current level ends, then the new one starts in the same World. Its
        // lease goes after its session, so what both levels use never leaves.
        endSession();
        // The new lease is taken before the old one goes, so what both use keeps a ref
        // throughout; only the old level's own assets retire.
        AssetResidency::Lease arriving;
        if (m_residency && m_hooks.assets)
            arriving = m_residency->acquire(leaseOwner(next.request, m_epoch + 1), assets);
        m_lease.release();
        m_lease = std::move(arriving);
        load.retireMs = msSince(clock);
        m_world = std::move(prepared);
        m_world.setLevelEpoch(++m_epoch);
        m_current = next.request;
        load.epoch = m_epoch;
        load.uploadMs = msSince(clock);
        try {
            startSession(StartReason::Fresh);
        } catch (const std::exception& e) {
            load.startMs = msSince(clock);
            failStarted(next, load, "level \"" + next.request.level + "\" didn't start: " + e.what());
            return;
        }
        load.startMs = msSince(clock);
        report(load);
        finish(next.id, next.request, TravelStatus::Succeeded, {});
    }
}

void LevelHost::failStarted(const Pending& next, LoadReport& load, const std::string& why) {
    m_lease.release();
    // Nothing is playing now; the application goes back to somewhere safe.
    m_failure = why;
    SW_LOG(LogGame, Error, "{}", m_failure);
    if (m_pending) {   // asked for during the failed start
        const Pending orphan = *m_pending;
        m_pending.reset();
        finish(orphan.id, orphan.request, TravelStatus::Cancelled, "the level asking for it didn't start");
    }
    load.status = TravelStatus::Failed;
    load.error = m_failure;
    report(load);
    finish(next.id, next.request, TravelStatus::Failed, m_failure);
}

bool LevelHost::travelOnDemand(const Pending& next, LoadReport& load, Clock::time_point& clock) {
    auto refuse = [&](std::string why, double LoadReport::*phase) {
        load.*phase += msSince(clock);
        load.status = TravelStatus::Failed;
        load.error = why;
        report(load);
        finish(next.id, next.request, TravelStatus::Failed, std::move(why));
    };
    // Prepare: its closure against what's resident, read from its files. Refused here, the
    // current level plays on.
    const World unread;
    std::vector<AssetCost> assets;
    std::string error;
    if (!planAssets(next.request, unread, assets, load, error)) {
        refuse(error, &LoadReport::prepareMs);
        return true;
    }
    load.prepareMs = msSince(clock);
    // Retire: the current level ends and its own assets unload (their GPU memory follows
    // once no frame reads it). The new lease is taken first, so shared assets stay in.
    endSession();
    AssetResidency::Lease arriving = m_residency->acquire(leaseOwner(next.request, m_epoch + 1), assets);
    m_lease.release();
    m_lease = std::move(arriving);
    load.retireMs = msSince(clock);
    // Upload: what the new level needs that isn't in. Past the point of no return.
    if (!m_residency->load(m_lease, [this] { return m_stopped; }, error)) {
        load.uploadMs = msSince(clock);
        if (m_stopped) {
            m_lease.release();
            load.status = TravelStatus::Cancelled;
            load.error = "Play stopped while it loaded";
            report(load);
            finish(next.id, next.request, TravelStatus::Cancelled, load.error);
            return false;
        }
        failStarted(next, load, "level \"" + next.request.level + "\" didn't load: " + error);
        return false;
    }
    load.uploadMs = msSince(clock);
    // Read: its scene, now that the names it uses resolve.
    World prepared;
    bool read = false;
    try {
        read = m_hooks.load && m_hooks.load(next.request, prepared, error);
        if (!m_hooks.load) error = "this application can't load levels";
    } catch (const std::exception& e) {
        error = e.what();
    }
    load.readMs = msSince(clock);
    if (m_stopped) {
        m_lease.release();
        load.status = TravelStatus::Cancelled;
        load.error = "Play stopped while it loaded";
        report(load);
        finish(next.id, next.request, TravelStatus::Cancelled, load.error);
        return false;
    }
    if (!read) {
        failStarted(
            next, load,
            "level \"" + next.request.level + "\" didn't load: " + (error.empty() ? "it didn't load" : error));
        return false;
    }
    m_world = std::move(prepared);
    m_world.setLevelEpoch(++m_epoch);
    m_current = next.request;
    load.epoch = m_epoch;
    try {
        startSession(StartReason::Fresh);
    } catch (const std::exception& e) {
        load.startMs = msSince(clock);
        failStarted(next, load, "level \"" + next.request.level + "\" didn't start: " + e.what());
        return false;
    }
    load.startMs = msSince(clock);
    report(load);
    finish(next.id, next.request, TravelStatus::Succeeded, {});
    return true;
}

void LevelHost::finish(uint64_t id, const LevelRequest& request, TravelStatus status, std::string error) {
    TravelResult result{id, request, status, std::move(error), m_epoch};
    if (status == TravelStatus::Succeeded)
        SW_LOG(LogGame, Info, "Level \"{}\" (epoch {})", request.level, m_epoch);
    else
        SW_LOG(LogGame, Warning, "Level \"{}\" {}: {}", request.level, travelStatusName(status), result.error);
    m_results.push_back(result);
    if (m_results.size() > kKeptResults) m_results.erase(m_results.begin());
    if (onResult) onResult(result);
}

}   // namespace shockwave
