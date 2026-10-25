// IOnlineServices on a LoopbackHub, for tests and player --net-test (Network/LoopbackServices.h).
#include <Network/LoopbackServices.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace shockwave::net {

namespace {

constexpr NetError kNoLobby{NetErrorCode::Unavailable, "loopback services can't find or join other lobbies"};

std::atomic<uint32_t> g_generations{0};

void putU64(uint8_t* out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out[i] = uint8_t(v >> (8 * i));
}
uint64_t getU64(const uint8_t* in) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(in[i]) << (8 * i);
    return v;
}

}   // namespace

LoopbackOnlineServices::LoopbackOnlineServices(LoopbackHub& hub, PeerId identity)
    : m_hub(hub), m_identity(identity), m_generation(++g_generations) {}

LoopbackOnlineServices::~LoopbackOnlineServices() = default;

RequestId LoopbackOnlineServices::nextRequest() { return {m_nextRequest++, m_generation}; }

size_t LoopbackOnlineServices::liveTickets() const { return m_tickets.size(); }

void LoopbackOnlineServices::pump() {
    ++m_pumps;
    while (!m_pending.empty()) {
        OnlineEvent event = std::move(m_pending.front());
        m_pending.pop_front();
        // A ticket cancelled, or a session ended, since: its result never arrives.
        if (event.kind == OnlineEventKind::AuthTicketReady) {
            auto it = std::find_if(
                m_tickets.begin(), m_tickets.end(), [&](const Ticket& t) { return t.id == event.request; });
            if (it == m_tickets.end()) continue;
            it->ready = true;
        }
        if (event.kind == OnlineEventKind::RequestFailed && event.request.valid()) {
            auto it = std::find_if(
                m_tickets.begin(), m_tickets.end(), [&](const Ticket& t) { return t.id == event.request; });
            if (it == m_tickets.end()) continue;
            m_tickets.erase(it);
        }
        if ((event.kind == OnlineEventKind::AuthSucceeded || event.kind == OnlineEventKind::AuthFailed) &&
            std::find(m_authSessions.begin(), m_authSessions.end(), event.peer) == m_authSessions.end())
            continue;
        m_ready.push_back(std::move(event));
    }
}

bool LoopbackOnlineServices::pollEvent(OnlineEvent& out) {
    if (m_ready.empty()) return false;
    out = std::move(m_ready.front());
    m_ready.pop_front();
    return true;
}

void LoopbackOnlineServices::cancel(RequestId request) {
    m_calls.push_back("cancel");
    std::erase_if(m_tickets, [&](const Ticket& t) { return t.id == request; });
}

NetResult<RequestId> LoopbackOnlineServices::createLobby(const LobbyCreateRequest& request) {
    m_calls.push_back("createLobby");
    const RequestId id = nextRequest();
    m_lobby = {0x0186000000000000ull | m_identity.value};
    m_lobbyJoinable = true;
    m_lobbyCapacity = request.maxMembers;
    m_lobbyMetadata.clear();
    OnlineEvent event;
    event.kind = OnlineEventKind::LobbyCreated;
    event.request = id;
    event.lobby = m_lobby;
    m_pending.push_back(std::move(event));
    return id;
}

NetResult<RequestId> LoopbackOnlineServices::findLobbies(const LobbyQuery&) { return kNoLobby; }
NetResult<RequestId> LoopbackOnlineServices::joinLobby(LobbyId) { return kNoLobby; }

NetResult<void> LoopbackOnlineServices::leaveLobby(LobbyId lobby) {
    m_calls.push_back("leaveLobby");
    if (!lobby.valid() || lobby != m_lobby) return NetError{NetErrorCode::InvalidArgument, "not in that lobby"};
    m_lobby = {};
    m_lobbyJoinable = false;
    return {};
}

NetResult<void> LoopbackOnlineServices::inviteFriend(LobbyId) { return kNoLobby; }

NetResult<void> LoopbackOnlineServices::setLobbyJoinable(LobbyId lobby, bool joinable) {
    m_calls.push_back(joinable ? "setLobbyJoinable 1" : "setLobbyJoinable 0");
    if (!lobby.valid() || lobby != m_lobby) return NetError{NetErrorCode::InvalidArgument, "not in that lobby"};
    m_lobbyJoinable = joinable;
    return {};
}

NetResult<void>
LoopbackOnlineServices::setLobbyMetadata(LobbyId lobby, const std::string& key, const std::string& value) {
    if (!lobby.valid() || lobby != m_lobby) return NetError{NetErrorCode::InvalidArgument, "not in that lobby"};
    for (auto& [k, v] : m_lobbyMetadata)
        if (k == key) {
            v = value;
            return {};
        }
    m_lobbyMetadata.emplace_back(key, value);
    return {};
}

NetResult<std::vector<PeerId>> LoopbackOnlineServices::lobbyMembers(LobbyId lobby) const {
    if (!lobby.valid() || lobby != m_lobby) return NetError{NetErrorCode::InvalidArgument, "not in that lobby"};
    return std::vector<PeerId>{m_identity};
}

NetResult<LobbySummary> LoopbackOnlineServices::lobbyInfo(LobbyId lobby) const {
    if (!lobby.valid() || lobby != m_lobby) return NetError{NetErrorCode::InvalidArgument, "not in that lobby"};
    LobbySummary info;
    info.id = m_lobby;
    info.owner = m_identity;
    info.members = 1;
    info.capacity = m_lobbyCapacity;
    info.metadata = m_lobbyMetadata;
    return info;
}

NetResult<std::unique_ptr<ITransport>> LoopbackOnlineServices::createTransport() {
    m_calls.push_back("createTransport");
    if (m_transportFails) return NetError{NetErrorCode::Unavailable, "the loopback transport was told to fail"};
    return m_hub.createTransport(m_identity);
}

NetResult<RequestId> LoopbackOnlineServices::requestAuthTicket(PeerId remote) {
    m_calls.push_back("requestAuthTicket");
    if (!remote.valid()) return NetError{NetErrorCode::InvalidArgument, "no remote identity"};
    if (m_ticketFailsNow) return NetError{NetErrorCode::Unavailable, "the loopback ticket was told to fail"};
    Ticket ticket;
    ticket.id = nextRequest();
    ticket.remote = remote;
    ticket.bytes.resize(kTicketBytes);
    std::memcpy(ticket.bytes.data(), "SWLT", 4);
    putU64(ticket.bytes.data() + 4, m_identity.value);
    putU64(ticket.bytes.data() + 12, remote.value);
    const RequestId id = ticket.id;
    m_tickets.push_back(std::move(ticket));
    OnlineEvent event;
    event.request = id;
    if (m_ticketFailsLater) {
        event.kind = OnlineEventKind::RequestFailed;
        event.error = {NetErrorCode::Unavailable, "the loopback ticket was told to fail"};
    } else {
        event.kind = OnlineEventKind::AuthTicketReady;
    }
    m_pending.push_back(std::move(event));
    return id;
}

NetResult<std::span<const uint8_t>> LoopbackOnlineServices::authTicket(RequestId id) const {
    for (const Ticket& t : m_tickets)
        if (t.id == id && t.ready) return std::span<const uint8_t>(t.bytes);
    return NetError{NetErrorCode::InvalidState, "no ticket ready for that request"};
}

NetResult<void> LoopbackOnlineServices::beginAuthSession(PeerId remote, std::span<const uint8_t> ticket) {
    m_calls.push_back("beginAuthSession");
    if (!remote.valid()) return NetError{NetErrorCode::InvalidArgument, "no remote identity"};
    if (std::find(m_authSessions.begin(), m_authSessions.end(), remote) != m_authSessions.end())
        return NetError{NetErrorCode::InvalidState, "an auth session with that peer is already open"};
    m_authSessions.push_back(remote);
    if (m_validation == Validation::NoAnswer) return {};
    const bool valid = m_validation == Validation::Check && ticket.size() == kTicketBytes &&
                       std::memcmp(ticket.data(), "SWLT", 4) == 0 && getU64(ticket.data() + 4) == remote.value &&
                       getU64(ticket.data() + 12) == m_identity.value;
    OnlineEvent event;
    event.kind = valid ? OnlineEventKind::AuthSucceeded : OnlineEventKind::AuthFailed;
    event.peer = remote;
    event.reason = valid ? ReasonCode::Normal : ReasonCode::AuthenticationFailed;
    m_pending.push_back(std::move(event));
    return {};
}

void LoopbackOnlineServices::endAuthSession(PeerId remote) {
    m_calls.push_back("endAuthSession");
    std::erase(m_authSessions, remote);
}

}   // namespace shockwave::net
