#include <GameFramework/LevelHost.h>

#include <Core/Log.h>
#include <Engine/Engine.h>

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

namespace {
constexpr size_t kKeptResults = 32;
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
    m_current = std::move(level);
    m_world.setLevelEpoch(++m_epoch);
    // The application's graphics, put back by stop(); levels keep the player's changes.
    if (m_engine && m_engine->guiEnabled() && !m_settings) m_settings = m_engine->graphics().requested();
    try {
        startSession(reason);
    } catch (...) {
        restoreSettings();
        throw;
    }
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
    if (m_pending) {
        const Pending pending = *m_pending;
        m_pending.reset();
        finish(pending.id, pending.request, TravelStatus::Cancelled, "Play stopped first");
    }
    endSession();
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
    while (m_pending) {
        const Pending next = *m_pending;
        m_pending.reset();
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
            finish(
                next.id, next.request, TravelStatus::Failed,
                "more than " + std::to_string(kMaxRedirects) +
                    " levels asked for another before playing (a cycle?): " + chain);
            return;
        }
        if (m_epoch == std::numeric_limits<uint64_t>::max()) {
            finish(next.id, next.request, TravelStatus::Failed, "level epochs ran out: restart the game");
            return;
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
            finish(next.id, next.request, TravelStatus::Cancelled, "Play stopped while it loaded");
            return;
        }
        if (!loaded) {
            finish(next.id, next.request, TravelStatus::Failed, error.empty() ? "it didn't load" : error);
            continue;   // a request made by a hook meanwhile still gets its turn
        }
        // Commit: the current level ends, then the new one starts in the same World.
        endSession();
        m_world = std::move(prepared);
        m_world.setLevelEpoch(++m_epoch);
        m_current = next.request;
        try {
            startSession(StartReason::Fresh);
        } catch (const std::exception& e) {
            // Nothing is playing now; the application goes back to somewhere safe.
            m_failure = "level \"" + next.request.level + "\" didn't start: " + e.what();
            SW_LOG(LogGame, Error, "{}", m_failure);
            if (m_pending) {   // asked for during the failed start
                const Pending orphan = *m_pending;
                m_pending.reset();
                finish(orphan.id, orphan.request, TravelStatus::Cancelled, "the level asking for it didn't start");
            }
            finish(next.id, next.request, TravelStatus::Failed, m_failure);
            return;
        }
        finish(next.id, next.request, TravelStatus::Succeeded, {});
    }
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
