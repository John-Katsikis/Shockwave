// SteamServices' lifecycle (docs/SteamMultiplayerImplementation.md section 4.1): starting
// Steam as a client or dedicated server, the callback pump, routing connection changes to
// transports, and shutting down exactly once. Also the public class, which forwards here.
#include <SteamServices/SteamServices.h>

#include "SteamServicesImpl.h"

#include <Core/Log.h>

#include <algorithm>
#include <cstring>
#include <string_view>

namespace shockwave::net {

namespace {

// At most one SDK runtime per process (mocks don't count).
bool g_sdkRunning = false;

}   // namespace

const char* steamServiceStateName(SteamServiceState state) {
    switch (state) {
    case SteamServiceState::Off: return "off";
    case SteamServiceState::Client: return "client";
    case SteamServiceState::DedicatedServer: return "dedicated server";
    }
    return "?";
}

const char* steamRelayStatusName(SteamRelayStatus status) {
    switch (status) {
    case SteamRelayStatus::Unknown: return "unknown";
    case SteamRelayStatus::Waiting: return "waiting";
    case SteamRelayStatus::Current: return "current";
    case SteamRelayStatus::Failed: return "failed";
    }
    return "?";
}

SteamServicesImpl::SteamServicesImpl(std::unique_ptr<SteamApi> steam) : api(std::move(steam)) {}

SteamServicesImpl::~SteamServicesImpl() { shutdown(); }

NetError SteamServicesImpl::notRunning() const {
    return {
        NetErrorCode::Unavailable, api ? "Steam isn't running: initialise SteamServices first"
                                       : "this build has no Steamworks SDK (configure with -DSHOCKWAVE_STEAMWORKS=ON)"};
}

NetError SteamServicesImpl::noLobbies() const {
    if (state == SteamServiceState::DedicatedServer)
        return {NetErrorCode::Unavailable, "a dedicated server has no lobbies"};
    return notRunning();
}

NetResult<void>
SteamServicesImpl::initialize(SteamMode mode, const SteamServicesSettings& s, const SteamServerParams& server) {
    if (!api) return notRunning();
    if (state != SteamServiceState::Off)
        return NetError{NetErrorCode::InvalidState, "Steam is already running; shut it down first"};
    if (s.appId == 0) return NetError{NetErrorCode::InvalidArgument, "steam_app_id is 0"};
    if (s.maxUnreliableBytes == 0 || s.maxUnreliableBytes > 1152 || s.maxPendingSendBytes == 0 ||
        !(s.authTimeoutSeconds > 0.0 && s.authTimeoutSeconds <= 600.0))
        return NetError{NetErrorCode::InvalidArgument, "Steam services settings out of range"};
    if (api->isSdk() && g_sdkRunning)
        return NetError{NetErrorCode::InvalidState, "another Steam runtime is already running in this process"};

    lastError.fill('\0');
    if (!api->init(mode, s.appId, server, *this, std::span(lastError).first(lastError.size() - 1))) {
        lastError.back() = '\0';
        SW_LOG(
            LogNetwork, Warning, "Steam didn't start as {}: {}",
            mode == SteamMode::Client ? "a client" : "a dedicated server", lastError.data());
        return NetError{
            NetErrorCode::Unavailable, mode == SteamMode::Client
                                           ? "Steam didn't start: is the Steam client running and signed in?"
                                           : "the Steam game server didn't start"};
    }
    if (api->isSdk()) g_sdkRunning = true;
    settings = s;
    state = mode == SteamMode::Client ? SteamServiceState::Client : SteamServiceState::DedicatedServer;
    loggedOn = api->loggedOn();
    ++m_generation;
    SW_LOG(
        LogNetwork, Info, "Steam started as {} for AppID {}, SteamID {}", steamServiceStateName(state), s.appId,
        api->localSteamId());
    return {};
}

void SteamServicesImpl::framePass() {
    if (state == SteamServiceState::Off) return;
    api->runCallbacks();
    // Act on what arrived, now that Steam's dispatch has returned.
    for (uint32_t i = 0; i < deferredCount && state != SteamServiceState::Off; ++i) {
        const Deferred d = deferred[i];
        switch (d.kind) {
        case Deferred::TicketReady: ticketReady(d.u, d.ok); break;
        case Deferred::AuthValidated: authValidated(d.a, d.response); break;
        case Deferred::LobbyCreated: lobbyCreated(d.a, d.ok, d.b); break;
        case Deferred::LobbyList: lobbyList(d.a, d.ok, d.u); break;
        case Deferred::LobbyEntered: lobbyEntered(d.a, d.ok, d.b); break;
        case Deferred::MembersChanged: {
            OnlineEvent event;
            event.kind = OnlineEventKind::LobbyMembersChanged;
            event.lobby = LobbyId{d.a};
            push(std::move(event));
            break;
        }
        case Deferred::JoinRequested: {
            OnlineEvent event;
            event.kind = OnlineEventKind::JoinRequested;
            event.lobby = LobbyId{d.a};
            push(std::move(event));
            break;
        }
        case Deferred::ServersConnected: loggedOn = d.ok; break;
        case Deferred::CloseOrphan: api->closeConnection(d.u, 0, false); break;
        }
    }
    deferredCount = 0;
    if (state != SteamServiceState::Off) expireAuthSessions();
}

void SteamServicesImpl::shutdown() {
    if (state == SteamServiceState::Off) return;
    // Everything that holds a Steam handle lets go while Steam still runs.
    for (Adapter*& adapter : adapters)
        if (adapter) {
            Adapter* a = adapter;
            adapter = nullptr;
            a->detach();
        }
    endAllAuth();
    leaveAllLobbies();
    calls.fill({});
    deferredCount = 0;
    eventHead = eventCount = 0;
    for (OnlineEvent& e : events) e = {};
    const bool sdk = api->isSdk();
    api->shutdown();
    if (sdk) g_sdkRunning = false;
    state = SteamServiceState::Off;
    loggedOn = false;
    SW_LOG(LogNetwork, Info, "Steam shut down");
}

void SteamServicesImpl::push(OnlineEvent&& event) {
    if (eventCount == events.size()) {
        if (droppedEvents++ == 0) SW_LOG(LogNetwork, Warning, "Steam service events overflowed; poll them every frame");
        return;
    }
    events[(eventHead + eventCount++) % events.size()] = std::move(event);
}

void SteamServicesImpl::defer(const Deferred& d) {
    if (deferredCount == deferred.size()) {
        if (droppedEvents++ == 0)
            SW_LOG(LogNetwork, Warning, "too many Steam callbacks in one frame; some were dropped");
        return;
    }
    deferred[deferredCount++] = d;
}

void SteamServicesImpl::dropQueued(RequestId request) {
    // A cancelled request's result is never reported, even if it's already queued.
    uint32_t kept = 0;
    for (uint32_t i = 0; i < eventCount; ++i) {
        OnlineEvent& e = events[(eventHead + i) % events.size()];
        if (e.request == request) continue;
        if (kept != i) events[(eventHead + kept) % events.size()] = std::move(e);
        ++kept;
    }
    for (uint32_t i = kept; i < eventCount; ++i) events[(eventHead + i) % events.size()] = {};
    eventCount = kept;
}

NetResult<std::unique_ptr<SteamNetworkAdapter>> SteamServicesImpl::createAdapter() {
    if (state == SteamServiceState::Off) return notRunning();
    auto free = std::find(adapters.begin(), adapters.end(), nullptr);
    if (free == adapters.end()) return NetError{NetErrorCode::CapacityExceeded, "too many Steam transports"};
    const SteamTransportSettings transport{
        settings.virtualPort, settings.maxUnreliableBytes, settings.maxPendingSendBytes};
    std::unique_ptr<SteamNetworkAdapter> adapter(new SteamNetworkAdapter(*this, *api, transport));
    *free = adapter->m_impl.get();
    return adapter;
}

void SteamServicesImpl::unregisterAdapter(Adapter* adapter) {
    for (Adapter*& a : adapters)
        if (a == adapter) a = nullptr;
}

void SteamServicesImpl::onConnectionChanged(const SteamConnectionChange& change) {
    for (Adapter* adapter : adapters)
        if (adapter && adapter->owns(change)) {
            adapter->queue(change);
            return;
        }
    // Nobody's: a listener or connection already closed. Steam frees it once we close it.
    if (change.state == SteamConnectionState::ClosedByPeer ||
        change.state == SteamConnectionState::ProblemDetectedLocally)
        defer({Deferred::CloseOrphan, false, change.connection, 0, 0, SteamAuthResponse::Ok});
}

void SteamServicesImpl::onAuthTicketReady(uint32_t ticket, bool ok) {
    defer({Deferred::TicketReady, ok, ticket, 0, 0, SteamAuthResponse::Ok});
}
void SteamServicesImpl::onAuthValidated(uint64_t steamId, uint64_t ownerSteamId, SteamAuthResponse response) {
    defer({Deferred::AuthValidated, false, 0, steamId, ownerSteamId, response});
}
void SteamServicesImpl::onLobbyCreated(uint64_t call, bool ok, uint64_t lobby) {
    defer({Deferred::LobbyCreated, ok, 0, call, lobby, SteamAuthResponse::Ok});
}
void SteamServicesImpl::onLobbyList(uint64_t call, bool ok, uint32_t count) {
    defer({Deferred::LobbyList, ok, count, call, 0, SteamAuthResponse::Ok});
}
void SteamServicesImpl::onLobbyEntered(uint64_t call, bool ok, uint64_t lobby) {
    defer({Deferred::LobbyEntered, ok, 0, call, lobby, SteamAuthResponse::Ok});
}
void SteamServicesImpl::onLobbyMembersChanged(uint64_t lobby) {
    defer({Deferred::MembersChanged, false, 0, lobby, 0, SteamAuthResponse::Ok});
}
void SteamServicesImpl::onLobbyJoinRequested(uint64_t lobby) {
    defer({Deferred::JoinRequested, false, 0, lobby, 0, SteamAuthResponse::Ok});
}
void SteamServicesImpl::onServersConnected(bool connected) {
    defer({Deferred::ServersConnected, connected, 0, 0, 0, SteamAuthResponse::Ok});
}

// SteamServices.

std::unique_ptr<SteamServices> SteamServices::create() { return std::make_unique<SteamServices>(makeSdkSteamApi()); }

SteamServices::SteamServices(std::unique_ptr<SteamApi> api)
    : m_impl(std::make_unique<SteamServicesImpl>(std::move(api))) {}

SteamServices::~SteamServices() = default;

NetResult<void> SteamServices::initializeClient(const SteamServicesSettings& settings) {
    return m_impl->initialize(SteamMode::Client, settings, {});
}

NetResult<void>
SteamServices::initializeDedicatedServer(const SteamServicesSettings& settings, const SteamServerParams& server) {
    if (server.gamePort == 0) return NetError{NetErrorCode::InvalidArgument, "a dedicated server needs a game port"};
    return m_impl->initialize(SteamMode::DedicatedServer, settings, server);
}

void SteamServices::framePass() { m_impl->framePass(); }
void SteamServices::shutdown() { m_impl->shutdown(); }
SteamServiceState SteamServices::state() const { return m_impl->state; }
uint32_t SteamServices::generation() const { return m_impl->m_generation; }

SteamRelayStatus SteamServices::relayStatus() const {
    return m_impl->state != SteamServiceState::Off ? m_impl->api->relayStatus() : SteamRelayStatus::Unknown;
}

const char* SteamServices::lastError() const { return m_impl->lastError.data(); }

OnlineCapabilities SteamServices::capabilities() const {
    // A mock in a Steam build still isn't the SDK.
    return {m_impl->api && m_impl->api->isSdk(), m_impl->state != SteamServiceState::Off && m_impl->loggedOn};
}

PeerId SteamServices::localPeer() const {
    return m_impl->state != SteamServiceState::Off ? PeerId{m_impl->api->localSteamId()} : PeerId{};
}

bool SteamServices::pollEvent(OnlineEvent& out) {
    SteamServicesImpl& s = *m_impl;
    if (s.eventCount == 0) return false;
    out = std::move(s.events[s.eventHead]);
    s.events[s.eventHead] = {};
    s.eventHead = (s.eventHead + 1) % s.events.size();
    --s.eventCount;
    return true;
}

void SteamServices::cancel(RequestId request) {
    if (!m_impl->current(request)) return;
    if (!m_impl->cancelTicket(request)) m_impl->cancelCall(request);
    m_impl->dropQueued(request);
}

NetResult<std::unique_ptr<SteamNetworkAdapter>> SteamServices::createSteamTransport() {
    return m_impl->createAdapter();
}

NetResult<std::unique_ptr<ITransport>> SteamServices::createTransport() {
    NetResult<std::unique_ptr<SteamNetworkAdapter>> adapter = createSteamTransport();
    if (!adapter) return adapter.error();
    return std::unique_ptr<ITransport>(std::move(adapter.value()));
}

NetResult<RequestId> SteamServices::createLobby(const LobbyCreateRequest& request) {
    return m_impl->createLobby(request);
}
NetResult<RequestId> SteamServices::findLobbies(const LobbyQuery& query) { return m_impl->findLobbies(query); }
NetResult<RequestId> SteamServices::joinLobby(LobbyId lobby) { return m_impl->joinLobby(lobby); }
NetResult<void> SteamServices::leaveLobby(LobbyId lobby) { return m_impl->leaveLobby(lobby); }
NetResult<void> SteamServices::inviteFriend(LobbyId lobby) { return m_impl->inviteFriend(lobby); }
NetResult<void> SteamServices::setLobbyJoinable(LobbyId lobby, bool joinable) {
    return m_impl->setLobbyJoinable(lobby, joinable);
}
NetResult<void> SteamServices::setLobbyMetadata(LobbyId lobby, const std::string& key, const std::string& value) {
    return m_impl->setLobbyMetadata(lobby, key, value);
}
NetResult<std::vector<PeerId>> SteamServices::lobbyMembers(LobbyId lobby) const { return m_impl->lobbyMembers(lobby); }
NetResult<RequestId> SteamServices::requestAuthTicket(PeerId remote) { return m_impl->requestAuthTicket(remote); }
NetResult<std::span<const uint8_t>> SteamServices::authTicket(RequestId ticket) const {
    return m_impl->authTicket(ticket);
}
NetResult<void> SteamServices::beginAuthSession(PeerId remote, std::span<const uint8_t> ticket) {
    return m_impl->beginAuthSession(remote, ticket);
}
void SteamServices::endAuthSession(PeerId remote) { m_impl->endAuthSession(remote); }

}   // namespace shockwave::net
