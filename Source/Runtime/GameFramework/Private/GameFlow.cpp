// GameFlow's names, observers and LevelFlow: see GameFramework/GameFlow.h.
#include <GameFramework/GameFlow.h>

#include <Core/Log.h>

#include <exception>
#include <vector>

namespace shockwave {

const char* travelReasonName(TravelReason reason) {
    switch (reason) {
    case TravelReason::NewGame: return "new_game";
    case TravelReason::Exit: return "exit";
    case TravelReason::Restart: return "restart";
    case TravelReason::Checkpoint: return "checkpoint";
    case TravelReason::LoadSave: return "load_save";
    case TravelReason::Menu: return "menu";
    case TravelReason::OnlineMap: return "online_map";
    }
    return "exit";
}

const char* travelPhaseName(TravelPhase phase) {
    switch (phase) {
    case TravelPhase::Queued: return "queued";
    case TravelPhase::Validating: return "validating";
    case TravelPhase::Reading: return "reading";
    case TravelPhase::Preparing: return "preparing";
    case TravelPhase::AwaitingBoundary: return "awaiting_boundary";
    case TravelPhase::Retiring: return "retiring";
    case TravelPhase::Uploading: return "uploading";
    case TravelPhase::Starting: return "starting";
    case TravelPhase::Succeeded: return "succeeded";
    case TravelPhase::Failed: return "failed";
    case TravelPhase::Cancelled: return "cancelled";
    }
    return "queued";
}

bool travelPhaseTerminal(TravelPhase phase) {
    return phase == TravelPhase::Succeeded || phase == TravelPhase::Failed || phase == TravelPhase::Cancelled;
}

bool travelPhaseCancellable(TravelPhase phase) {
    return phase == TravelPhase::Queued || phase == TravelPhase::Validating || phase == TravelPhase::Reading ||
           phase == TravelPhase::Preparing || phase == TravelPhase::AwaitingBoundary;
}

float travelPhaseProgress(TravelPhase phase) {
    switch (phase) {
    case TravelPhase::Queued:
    case TravelPhase::Validating: return 0.0f;
    case TravelPhase::Reading: return 0.05f;
    case TravelPhase::Preparing: return 0.20f;
    case TravelPhase::AwaitingBoundary:
    case TravelPhase::Retiring: return 0.45f;
    case TravelPhase::Uploading: return 0.55f;
    case TravelPhase::Starting: return 0.80f;
    case TravelPhase::Succeeded: return 1.0f;
    case TravelPhase::Failed:
    case TravelPhase::Cancelled: return 0.0f;   // keep what it had reached instead
    }
    return 0.0f;
}

const char* travelErrorName(TravelError error) {
    switch (error) {
    case TravelError::None: return "None";
    case TravelError::Busy: return "Busy";
    case TravelError::NotPlaying: return "NotPlaying";
    case TravelError::StaleLevel: return "StaleLevel";
    case TravelError::UnknownLevel: return "UnknownLevel";
    case TravelError::UnknownSpawn: return "UnknownSpawn";
    case TravelError::InvalidRequest: return "InvalidRequest";
    case TravelError::AuthorityRequired: return "AuthorityRequired";
    case TravelError::UnknownTicket: return "UnknownTicket";
    case TravelError::AlreadyFinished: return "AlreadyFinished";
    case TravelError::TooLateToCancel: return "TooLateToCancel";
    case TravelError::LoadFailed: return "LoadFailed";
    case TravelError::BudgetExceeded: return "BudgetExceeded";
    case TravelError::StartFailed: return "StartFailed";
    case TravelError::Cancelled: return "Cancelled";
    case TravelError::RedirectCycle: return "RedirectCycle";
    case TravelError::RotationAborted: return "RotationAborted";
    }
    return "None";
}

// ---- Observers ------------------------------------------------------------------------------

void GameFlow::Subscription::reset() {
    if (auto list = m_list.lock()) list->erase(m_id);
    m_list.reset();
    m_id = 0;
}

GameFlow::Subscription GameFlow::subscribe(std::function<void(const TravelCompletion&)> observer) {
    Subscription subscription;
    if (!observer) return subscription;
    subscription.m_id = m_nextObserver++;
    (*m_observers)[subscription.m_id] = std::move(observer);
    subscription.m_list = m_observers;
    return subscription;
}

void GameFlow::notifyObservers(const TravelCompletion& completion) {
    // A copy of the ids: an observer may unsubscribe itself or another one.
    std::vector<uint64_t> ids;
    for (const auto& [id, observer] : *m_observers) ids.push_back(id);
    for (const uint64_t id : ids) {
        const auto it = m_observers->find(id);
        if (it == m_observers->end()) continue;
        const auto observer = it->second;   // it may unsubscribe while it runs
        try {
            observer(completion);
        } catch (const std::exception& e) {
            SW_LOG(LogGame, Error, "A travel observer threw: {}", e.what());
        }
    }
}

// ---- LevelFlow ------------------------------------------------------------------------------

namespace {
TravelTicket refused() { return {0, TravelError::NotPlaying, "nothing here can travel"}; }
}   // namespace

TravelTicket LevelFlow::request(std::string level, std::string spawn, TravelReason reason) const {
    TravelRequest r;
    r.level = std::move(level);
    r.spawn = std::move(spawn);
    r.reason = reason;
    return request(std::move(r));
}

TravelTicket LevelFlow::request(TravelRequest request) const {
    if (!m_flow) return refused();
    request.fromEpoch = m_epoch;
    return m_flow->request(std::move(request));
}

TravelTicket LevelFlow::restartLevel() const { return m_flow ? m_flow->restartLevel(m_epoch) : refused(); }
TravelTicket LevelFlow::returnToMenu() const { return m_flow ? m_flow->returnToMenu(m_epoch) : refused(); }
TravelTicket LevelFlow::useExit(Entity exit) const { return m_flow ? m_flow->useExit(exit, m_epoch) : refused(); }
TravelTicket LevelFlow::rotateMap() const { return m_flow ? m_flow->rotateMap(m_epoch) : refused(); }
TravelError LevelFlow::cancel(uint64_t ticket) const {
    return m_flow ? m_flow->cancel(ticket) : TravelError::UnknownTicket;
}
std::optional<TravelProgress> LevelFlow::status(uint64_t ticket) const {
    return m_flow ? m_flow->status(ticket) : std::nullopt;
}

}   // namespace shockwave
