#include <SteamServices/MockSteamApi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace shockwave::net {

namespace {

constexpr uint32_t kNone = UINT32_MAX;
constexpr int32_t kEndNoListener = 4002;   // in Steam's "remote" range: nobody listening there
constexpr uint32_t kTicketBytes = 240;
constexpr uint32_t kMaxCallbacks = 8192;

struct Callback {
    enum Kind : uint8_t {
        Connection,
        TicketReady,
        Validated,
        LobbyCreated,
        LobbyList,
        LobbyEntered,
        MembersChanged,
        JoinRequested,
        ServersConnected
    };
    Kind kind = Connection;
    SteamConnectionChange change;
    bool ok = false;
    uint32_t u = 0;
    uint64_t a = 0, b = 0;
    SteamAuthResponse response = SteamAuthResponse::Ok;
};

struct Message {
    bool used = false;
    bool handedOut = false;
    bool reliable = false;
    uint32_t connection = 0;   // the receiving connection's handle
    uint32_t size = 0;
    uint32_t next = kNone;
    int64_t receivedUs = 0;
};

struct Connection {
    bool used = false;
    uint32_t handle = 0;
    MockSteamApi* owner = nullptr;
    uint32_t peer = kNone;   // index of the far end, while it's open
    uint32_t listenSocket = 0;   // set on the listener's side
    uint64_t remoteSteamId = 0;
    SteamConnectionState state = SteamConnectionState::None;
    int32_t endReason = 0;
    uint32_t pollGroup = 0;
    uint32_t head = kNone, tail = kNone;   // received messages, oldest first
};

struct Listener {
    MockSteamApi* owner = nullptr;
    uint32_t handle = 0;
    int port = 0;
};

struct Ticket {
    uint32_t handle = 0;
    uint64_t owner = 0, remote = 0;
    std::vector<uint64_t> validators;   // who began a session with it
};

struct Lobby {
    uint64_t id = 0;
    uint64_t owner = 0;
    uint32_t limit = 0;
    bool joinable = true;
    std::vector<uint64_t> members;
    std::vector<std::pair<std::string, std::string>> data;
};

}   // namespace

struct MockSteamNetwork::Impl {
    MockSteamConfig config;
    uint64_t now = 1'000'000;
    uint32_t nextHandle = 100;
    uint64_t nextCall = 5000;
    uint64_t nextLobby = 109775240000000000ull;   // in Steam's lobby ID range
    std::vector<Connection> connections;
    std::vector<Message> messages;
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> freeMessages;
    std::vector<Listener> listeners;
    std::vector<MockSteamApi*> apis;
    std::vector<Ticket> tickets;
    std::vector<Lobby> lobbies;

    explicit Impl(const MockSteamConfig& c) : config(c) {
        connections.resize(512);
        messages.resize(c.messagePool);
        bytes.resize(size_t(c.messagePool) * c.messageBytes);
        freeMessages.reserve(c.messagePool);
        for (uint32_t i = c.messagePool; i-- > 0;) freeMessages.push_back(i);
        listeners.reserve(64);
        apis.reserve(64);
    }

    uint8_t* data(uint32_t message) { return bytes.data() + size_t(message) * config.messageBytes; }

    uint32_t find(uint32_t handle, const MockSteamApi* owner) const {
        if (!handle) return kNone;
        for (uint32_t i = 0; i < connections.size(); ++i)
            if (connections[i].used && connections[i].handle == handle && connections[i].owner == owner) return i;
        return kNone;
    }

    uint32_t newConnection() {
        for (uint32_t i = 0; i < connections.size(); ++i)
            if (!connections[i].used) return i;
        return kNone;
    }

    MockSteamApi* apiFor(uint64_t steamId) const;
    Lobby* lobby(uint64_t id) {
        for (Lobby& l : lobbies)
            if (l.id == id) return &l;
        return nullptr;
    }
    const Lobby* lobby(uint64_t id) const { return const_cast<Impl*>(this)->lobby(id); }
    void freeInbox(Connection& c) {
        for (uint32_t m = c.head; m != kNone;) {
            const uint32_t next = messages[m].next;
            if (!messages[m].handedOut) {
                messages[m] = {};
                freeMessages.push_back(m);
            } else {
                messages[m].next = kNone;   // handed out: freed by its release
            }
            m = next;
        }
        c.head = c.tail = kNone;
    }
};

struct MockSteamApi::State {
    uint64_t steamId = 0;
    MockSteamFaults faults;
    MockSteamCounts counts;
    bool running = false;
    SteamMode mode = SteamMode::Client;
    SteamCallbacks* callbacks = nullptr;
    std::vector<Callback> queue, held;
    uint32_t nextPollGroup = 1;
    std::vector<uint32_t> pollGroups;
    std::vector<std::pair<std::string, std::string>> filters;
    std::vector<uint64_t> lastList;
    std::vector<uint64_t> authSessions;
};

MockSteamApi* MockSteamNetwork::Impl::apiFor(uint64_t steamId) const {
    for (MockSteamApi* api : apis)
        if (api->localSteamId() == steamId && api->m_state->running) return api;
    return nullptr;
}

namespace {

void post(MockSteamApi::State& s, const Callback& c) {
    if (s.queue.size() < kMaxCallbacks) s.queue.push_back(c);   // reserved: no allocation
}

}   // namespace

MockSteamNetwork::MockSteamNetwork(const MockSteamConfig& config) : m_impl(std::make_unique<Impl>(config)) {}
MockSteamNetwork::~MockSteamNetwork() = default;

std::unique_ptr<MockSteamApi> MockSteamNetwork::makeApi(uint64_t steamId) {
    std::unique_ptr<MockSteamApi> api(new MockSteamApi(*m_impl, steamId));
    m_impl->apis.push_back(api.get());
    return api;
}

void MockSteamNetwork::advance(uint64_t microseconds) { m_impl->now += microseconds; }
uint64_t MockSteamNetwork::nowUs() const { return m_impl->now; }

MockSteamApi::MockSteamApi(MockSteamNetwork::Impl& network, uint64_t steamId)
    : m_network(network), m_state(std::make_unique<State>()) {
    m_state->steamId = steamId;
    m_state->queue.reserve(kMaxCallbacks);
    m_state->pollGroups.reserve(16);
}

MockSteamApi::~MockSteamApi() {
    if (m_state->running) shutdown();
    std::erase(m_network.apis, this);
}

MockSteamFaults& MockSteamApi::faults() { return m_state->faults; }
const MockSteamCounts& MockSteamApi::counts() const { return m_state->counts; }

void MockSteamApi::injectConnectionChange(const SteamConnectionChange& change) {
    Callback c;
    c.kind = Callback::Connection;
    c.change = change;
    post(*m_state, c);
}

void MockSteamApi::injectValidation(uint64_t steamId, SteamAuthResponse response) {
    Callback c;
    c.kind = Callback::Validated;
    c.a = steamId;
    c.b = steamId;
    c.response = response;
    post(*m_state, c);
}

void MockSteamApi::injectJoinRequest(uint64_t lobby) {
    Callback c;
    c.kind = Callback::JoinRequested;
    c.a = lobby;
    post(*m_state, c);
}

void MockSteamApi::releaseHeldResults() {
    for (const Callback& c : m_state->held) post(*m_state, c);
    m_state->held.clear();
}

uint32_t MockSteamApi::connectionTo(uint64_t steamId) const {
    uint32_t newest = 0;
    for (const Connection& c : m_network.connections)
        if (c.used && c.owner == this && c.remoteSteamId == steamId) newest = std::max(newest, c.handle);
    return newest;
}

bool MockSteamApi::init(
    SteamMode mode, uint32_t, const SteamServerParams&, SteamCallbacks& callbacks, std::span<char> error) {
    ++m_state->counts.inits;
    if (m_state->faults.failInit) {
        if (!error.empty()) std::snprintf(error.data(), error.size(), "mock: init failed on purpose");
        return false;
    }
    m_state->running = true;
    m_state->mode = mode;
    m_state->callbacks = &callbacks;
    Callback c;
    c.kind = Callback::ServersConnected;
    c.ok = m_state->faults.loggedOn;
    post(*m_state, c);
    return true;
}

void MockSteamApi::runCallbacks() {
    State& s = *m_state;
    ++s.counts.runCallbacks;
    if (!s.running) return;
    // Handlers only queue; anything they cause lands in the next runCallbacks().
    const size_t count = s.queue.size();
    for (size_t i = 0; i < count && s.running; ++i) {
        const Callback c = s.queue[i];
        SteamCallbacks& cb = *s.callbacks;
        switch (c.kind) {
        case Callback::Connection: cb.onConnectionChanged(c.change); break;
        case Callback::TicketReady: cb.onAuthTicketReady(c.u, c.ok); break;
        case Callback::Validated: cb.onAuthValidated(c.a, c.b, c.response); break;
        case Callback::LobbyCreated: cb.onLobbyCreated(c.a, c.ok, c.b); break;
        case Callback::LobbyList: cb.onLobbyList(c.a, c.ok, c.u); break;
        case Callback::LobbyEntered: cb.onLobbyEntered(c.a, c.ok, c.b); break;
        case Callback::MembersChanged: cb.onLobbyMembersChanged(c.a); break;
        case Callback::JoinRequested: cb.onLobbyJoinRequested(c.a); break;
        case Callback::ServersConnected: cb.onServersConnected(c.ok); break;
        }
    }
    s.queue.erase(s.queue.begin(), s.queue.begin() + std::ptrdiff_t(std::min(count, s.queue.size())));
}

void MockSteamApi::shutdown() {
    State& s = *m_state;
    if (!s.running) return;
    ++s.counts.shutdowns;
    for (Connection& c : m_network.connections)
        if (c.used && c.owner == this) closeConnection(c.handle, 0, false);
    std::erase_if(m_network.listeners, [&](const Listener& l) {
        if (l.owner == this) --s.counts.listenSockets;
        return l.owner == this;
    });
    std::vector<uint64_t> joined;
    for (const Lobby& l : m_network.lobbies)
        if (std::find(l.members.begin(), l.members.end(), s.steamId) != l.members.end()) joined.push_back(l.id);
    for (uint64_t lobby : joined) leaveLobby(lobby);
    s.running = false;
    s.callbacks = nullptr;
    s.queue.clear();
    s.held.clear();
}

uint64_t MockSteamApi::localSteamId() const { return m_state->steamId; }
bool MockSteamApi::loggedOn() const { return m_state->running && m_state->faults.loggedOn; }
SteamRelayStatus MockSteamApi::relayStatus() const { return m_state->faults.relay; }
uint64_t MockSteamApi::nowUs() const { return m_network.now; }

uint32_t MockSteamApi::createListenSocketP2P(int virtualPort) {
    if (!m_state->running || m_state->faults.failListen) return 0;
    for (const Listener& l : m_network.listeners)
        if (l.owner == this && l.port == virtualPort) return 0;
    const uint32_t handle = m_network.nextHandle++;
    m_network.listeners.push_back({this, handle, virtualPort});
    ++m_state->counts.listenSockets;
    return handle;
}

void MockSteamApi::closeListenSocket(uint32_t socket) {
    auto it = std::find_if(m_network.listeners.begin(), m_network.listeners.end(), [&](const Listener& l) {
        return l.owner == this && l.handle == socket;
    });
    if (it == m_network.listeners.end()) return;
    m_network.listeners.erase(it);
    --m_state->counts.listenSockets;
    // Steam closes the connections that came in through it.
    for (Connection& c : m_network.connections)
        if (c.used && c.owner == this && c.listenSocket == socket) closeConnection(c.handle, 0, false);
}

uint32_t MockSteamApi::createPollGroup() {
    if (!m_state->running || m_state->faults.failPollGroup) return 0;
    const uint32_t group = m_state->nextPollGroup++;
    m_state->pollGroups.push_back(group);
    ++m_state->counts.pollGroups;
    return group;
}

void MockSteamApi::destroyPollGroup(uint32_t group) {
    auto it = std::find(m_state->pollGroups.begin(), m_state->pollGroups.end(), group);
    if (it == m_state->pollGroups.end()) return;
    m_state->pollGroups.erase(it);
    --m_state->counts.pollGroups;
    for (Connection& c : m_network.connections)
        if (c.used && c.owner == this && c.pollGroup == group) c.pollGroup = 0;
}

uint32_t MockSteamApi::connectP2P(uint64_t steamId, int virtualPort) {
    if (!m_state->running || m_state->faults.failConnect) return 0;
    MockSteamNetwork::Impl& n = m_network;
    const uint32_t mine = n.newConnection();
    if (mine == kNone) return 0;
    Connection& c = n.connections[mine];
    c = {};
    c.used = true;
    c.handle = n.nextHandle++;
    c.owner = this;
    c.remoteSteamId = steamId;
    c.state = SteamConnectionState::Connecting;
    ++m_state->counts.openConnections;

    Callback ours;
    ours.change = {c.handle, 0, SteamConnectionState::Connecting, SteamConnectionState::None, steamId, 0};
    post(*m_state, ours);

    MockSteamApi* remote = n.apiFor(steamId);
    const Listener* listener = nullptr;
    for (const Listener& l : n.listeners)
        if (remote && l.owner == remote && l.port == virtualPort) listener = &l;
    const uint32_t theirs = listener ? n.newConnection() : kNone;
    if (theirs == kNone) {
        c.state = SteamConnectionState::ProblemDetectedLocally;
        c.endReason = kEndNoListener;
        Callback failed;
        failed.change = {c.handle, 0, c.state, SteamConnectionState::Connecting, steamId, kEndNoListener};
        post(*m_state, failed);
        return c.handle;
    }
    Connection& t = n.connections[theirs];
    t = {};
    t.used = true;
    t.handle = n.nextHandle++;
    t.owner = remote;
    t.peer = mine;
    t.listenSocket = listener->handle;
    t.remoteSteamId = m_state->steamId;
    t.state = SteamConnectionState::Connecting;
    n.connections[mine].peer = theirs;
    ++remote->m_state->counts.openConnections;
    Callback incoming;
    incoming.change = {
        t.handle, t.listenSocket, SteamConnectionState::Connecting, SteamConnectionState::None, m_state->steamId, 0};
    post(*remote->m_state, incoming);
    return n.connections[mine].handle;
}

SteamResult MockSteamApi::acceptConnection(uint32_t connection) {
    MockSteamNetwork::Impl& n = m_network;
    const uint32_t i = n.find(connection, this);
    if (i == kNone) return SteamResult::InvalidParam;
    Connection& c = n.connections[i];
    if (c.state != SteamConnectionState::Connecting || !c.listenSocket) return SteamResult::InvalidState;
    if (m_state->faults.failAccept) return SteamResult::Fail;
    if (c.peer == kNone) return SteamResult::NoConnection;
    Connection& p = n.connections[c.peer];
    c.state = p.state = SteamConnectionState::Connected;
    Callback mine;
    mine.change = {c.handle, c.listenSocket, c.state, SteamConnectionState::Connecting, c.remoteSteamId, 0};
    post(*m_state, mine);
    Callback theirs;
    theirs.change = {p.handle, 0, p.state, SteamConnectionState::Connecting, p.remoteSteamId, 0};
    post(*p.owner->m_state, theirs);
    return SteamResult::Ok;
}

bool MockSteamApi::closeConnection(uint32_t connection, int32_t endReason, bool) {
    MockSteamNetwork::Impl& n = m_network;
    const uint32_t i = n.find(connection, this);
    if (i == kNone) return false;
    Connection& c = n.connections[i];
    if (c.peer != kNone) {
        Connection& p = n.connections[c.peer];
        p.peer = kNone;
        if (p.state == SteamConnectionState::Connecting || p.state == SteamConnectionState::Connected) {
            const SteamConnectionState old = p.state;
            p.state = SteamConnectionState::ClosedByPeer;
            p.endReason = endReason;
            Callback closed;
            closed.change = {p.handle, p.listenSocket, p.state, old, p.remoteSteamId, endReason};
            post(*p.owner->m_state, closed);
        }
    }
    n.freeInbox(c);
    c = {};
    --m_state->counts.openConnections;
    return true;
}

bool MockSteamApi::setConnectionPollGroup(uint32_t connection, uint32_t group) {
    const uint32_t i = m_network.find(connection, this);
    if (i == kNone || m_state->faults.failSetPollGroup) return false;
    if (std::find(m_state->pollGroups.begin(), m_state->pollGroups.end(), group) == m_state->pollGroups.end())
        return false;
    m_network.connections[i].pollGroup = group;
    return true;
}

int MockSteamApi::receiveMessagesOnPollGroup(uint32_t group, std::span<SteamMessage> out) {
    MockSteamNetwork::Impl& n = m_network;
    int count = 0;
    // Oldest first across connections: take the earliest head each time.
    while (size_t(count) < out.size()) {
        uint32_t best = kNone;
        for (uint32_t i = 0; i < n.connections.size(); ++i) {
            const Connection& c = n.connections[i];
            if (!c.used || c.owner != this || c.pollGroup != group || !group || c.head == kNone) continue;
            if (best == kNone || n.messages[c.head].receivedUs < n.messages[n.connections[best].head].receivedUs ||
                (n.messages[c.head].receivedUs == n.messages[n.connections[best].head].receivedUs &&
                 c.head < n.connections[best].head))
                best = i;
        }
        if (best == kNone) break;
        Connection& c = n.connections[best];
        const uint32_t m = c.head;
        c.head = n.messages[m].next;
        if (c.head == kNone) c.tail = kNone;
        Message& message = n.messages[m];
        message.next = kNone;
        message.handedOut = true;
        SteamMessage& o = out[size_t(count++)];
        o.handle = &message;
        o.connection = message.connection;
        o.data = n.data(m);
        o.size = message.size;
        o.reliable = message.reliable;
        o.receivedUs = message.receivedUs;
        ++m_state->counts.outstandingMessages;
        ++m_state->counts.receivedMessages;
    }
    return count;
}

void MockSteamApi::releaseMessage(void* handle) {
    MockSteamNetwork::Impl& n = m_network;
    Message* message = static_cast<Message*>(handle);
    if (!message || message < n.messages.data() || message >= n.messages.data() + n.messages.size() ||
        !message->handedOut) {
        ++m_state->counts.badReleases;
        return;
    }
    *message = {};
    n.freeMessages.push_back(uint32_t(message - n.messages.data()));
    --m_state->counts.outstandingMessages;
    ++m_state->counts.releasedMessages;
}

SteamResult MockSteamApi::sendMessage(uint32_t connection, std::span<const uint8_t> bytes, SteamSend flags) {
    MockSteamNetwork::Impl& n = m_network;
    ++m_state->counts.sends;
    if (m_state->faults.sendResult) return *m_state->faults.sendResult;
    const uint32_t i = n.find(connection, this);
    if (i == kNone) return SteamResult::InvalidParam;
    const Connection& c = n.connections[i];
    if (c.state != SteamConnectionState::Connected || c.peer == kNone) return SteamResult::NoConnection;
    if (bytes.size() > n.config.messageBytes) return SteamResult::InvalidParam;
    if (n.freeMessages.empty()) return SteamResult::LimitExceeded;
    const uint32_t m = n.freeMessages.back();
    n.freeMessages.pop_back();
    Connection& p = n.connections[c.peer];
    Message& message = n.messages[m];
    message = {};
    message.used = true;
    message.reliable = flags == SteamSend::ReliableNoNagle;
    message.connection = p.handle;
    message.size = uint32_t(bytes.size());
    message.receivedUs = int64_t(n.now);
    if (!bytes.empty()) std::memcpy(n.data(m), bytes.data(), bytes.size());
    if (p.tail == kNone)
        p.head = m;
    else
        n.messages[p.tail].next = m;
    p.tail = m;
    return SteamResult::Ok;
}

bool MockSteamApi::connectionStatus(uint32_t connection, SteamConnectionStatus& out) const {
    const uint32_t i = m_network.find(connection, this);
    if (i == kNone) return false;
    out.state = m_network.connections[i].state;
    out.pingMs = m_state->faults.pingMs;
    out.pendingReliableBytes = m_state->faults.pendingReliableBytes;
    out.pendingUnreliableBytes = m_state->faults.pendingUnreliableBytes;
    return true;
}

// Tickets: kTicketBytes bytes that name who made it and for whom, so beginAuthSession can
// tell a ticket for someone else.

uint32_t MockSteamApi::getAuthSessionTicket(uint64_t remoteSteamId, std::span<uint8_t> out, uint32_t& size) {
    size = 0;
    if (!m_state->running || out.size() < kTicketBytes) return 0;
    std::memset(out.data(), 0xA5, kTicketBytes);
    std::memcpy(out.data(), &m_state->steamId, 8);
    std::memcpy(out.data() + 8, &remoteSteamId, 8);
    size = kTicketBytes;
    const uint32_t handle = m_network.nextHandle++;
    std::memcpy(out.data() + 16, &handle, 4);
    m_network.tickets.push_back({handle, m_state->steamId, remoteSteamId, {}});
    ++m_state->counts.ticketsIssued;
    if (!m_state->faults.holdTickets) {
        Callback c;
        c.kind = Callback::TicketReady;
        c.u = handle;
        c.ok = !m_state->faults.failTickets;
        post(*m_state, c);
    }
    return handle;
}

void MockSteamApi::cancelAuthTicket(uint32_t ticket) {
    auto it = std::find_if(m_network.tickets.begin(), m_network.tickets.end(), [&](const Ticket& t) {
        return t.handle == ticket && t.owner == m_state->steamId;
    });
    if (it == m_network.tickets.end()) return;
    ++m_state->counts.ticketsCancelled;
    // Whoever validated it hears that it was cancelled.
    for (uint64_t validator : it->validators)
        if (MockSteamApi* api = m_network.apiFor(validator))
            api->injectValidation(it->owner, SteamAuthResponse::TicketCanceled);
    m_network.tickets.erase(it);
}

SteamBeginAuth MockSteamApi::beginAuthSession(std::span<const uint8_t> ticket, uint64_t steamId) {
    if (m_state->faults.beginAuth != SteamBeginAuth::Ok) return m_state->faults.beginAuth;
    if (std::find(m_state->authSessions.begin(), m_state->authSessions.end(), steamId) != m_state->authSessions.end())
        return SteamBeginAuth::Duplicate;
    if (ticket.size() != kTicketBytes) return SteamBeginAuth::InvalidTicket;
    m_state->authSessions.push_back(steamId);
    ++m_state->counts.authSessionsBegun;
    uint64_t owner = 0, remote = 0;
    uint32_t handle = 0;
    std::memcpy(&owner, ticket.data(), 8);
    std::memcpy(&remote, ticket.data() + 8, 8);
    std::memcpy(&handle, ticket.data() + 16, 4);
    auto it = std::find_if(m_network.tickets.begin(), m_network.tickets.end(), [&](const Ticket& t) {
        return t.handle == handle && t.owner == owner && t.remote == remote;
    });
    std::optional<SteamAuthResponse> response = m_state->faults.validation;
    if (it == m_network.tickets.end() || owner != steamId || remote != m_state->steamId)
        response = SteamAuthResponse::TicketInvalid;   // cancelled, forged, or made for someone else
    else
        it->validators.push_back(m_state->steamId);
    if (response) injectValidation(steamId, *response);
    return SteamBeginAuth::Ok;
}

void MockSteamApi::endAuthSession(uint64_t steamId) {
    if (std::erase(m_state->authSessions, steamId)) ++m_state->counts.authSessionsEnded;
}

// Lobbies.

uint64_t MockSteamApi::createLobby(SteamLobbyType, uint32_t maxMembers) {
    if (!m_state->running || m_state->mode != SteamMode::Client) return 0;
    const uint64_t call = m_network.nextCall++;
    Lobby lobby;
    lobby.id = m_network.nextLobby++;
    lobby.owner = m_state->steamId;
    lobby.limit = maxMembers;
    lobby.members.push_back(m_state->steamId);
    m_network.lobbies.push_back(std::move(lobby));
    Callback c;
    c.kind = Callback::LobbyCreated;
    c.a = call;
    c.ok = true;
    c.b = m_network.lobbies.back().id;
    (m_state->faults.holdLobbyResults ? m_state->held : m_state->queue).push_back(c);
    return call;
}

void MockSteamApi::addLobbyListFilter(std::string_view key, std::string_view value) {
    m_state->filters.emplace_back(std::string(key), std::string(value));
}

uint64_t MockSteamApi::requestLobbyList(uint32_t maxResults) {
    if (!m_state->running) return 0;
    m_state->lastList.clear();
    for (const Lobby& l : m_network.lobbies) {
        if (m_state->lastList.size() >= maxResults || !l.joinable || l.members.size() >= l.limit) continue;
        const bool match = std::all_of(m_state->filters.begin(), m_state->filters.end(), [&](const auto& f) {
            return std::find(l.data.begin(), l.data.end(), f) != l.data.end();
        });
        if (match) m_state->lastList.push_back(l.id);
    }
    m_state->filters.clear();
    const uint64_t call = m_network.nextCall++;
    Callback c;
    c.kind = Callback::LobbyList;
    c.a = call;
    c.ok = true;
    c.u = uint32_t(m_state->lastList.size());
    (m_state->faults.holdLobbyResults ? m_state->held : m_state->queue).push_back(c);
    return call;
}

uint64_t MockSteamApi::lobbyByIndex(uint32_t index) const {
    return index < m_state->lastList.size() ? m_state->lastList[index] : 0;
}

uint64_t MockSteamApi::joinLobby(uint64_t id) {
    if (!m_state->running) return 0;
    const uint64_t call = m_network.nextCall++;
    Lobby* lobby = m_network.lobby(id);
    const bool ok = lobby && lobby->joinable && lobby->members.size() < lobby->limit;
    if (ok) {
        lobby->members.push_back(m_state->steamId);
        for (uint64_t member : lobby->members)
            if (MockSteamApi* api = m_network.apiFor(member); api && member != m_state->steamId) {
                Callback changed;
                changed.kind = Callback::MembersChanged;
                changed.a = id;
                post(*api->m_state, changed);
            }
    }
    Callback c;
    c.kind = Callback::LobbyEntered;
    c.a = call;
    c.ok = ok;
    c.b = id;
    (m_state->faults.holdLobbyResults ? m_state->held : m_state->queue).push_back(c);
    return call;
}

void MockSteamApi::leaveLobby(uint64_t id) {
    Lobby* lobby = m_network.lobby(id);
    if (!lobby || !std::erase(lobby->members, m_state->steamId)) return;
    ++m_state->counts.lobbiesLeft;
    if (lobby->members.empty()) {
        std::erase_if(m_network.lobbies, [&](const Lobby& l) { return l.id == id; });
        return;
    }
    if (lobby->owner == m_state->steamId) lobby->owner = lobby->members.front();   // Steam picks a new owner
    for (uint64_t member : lobby->members)
        if (MockSteamApi* api = m_network.apiFor(member)) {
            Callback changed;
            changed.kind = Callback::MembersChanged;
            changed.a = id;
            post(*api->m_state, changed);
        }
}

bool MockSteamApi::setLobbyData(uint64_t id, std::string_view key, std::string_view value) {
    Lobby* lobby = m_network.lobby(id);
    if (!lobby || lobby->owner != m_state->steamId) return false;
    for (auto& [k, v] : lobby->data)
        if (k == key) {
            v = value;
            return true;
        }
    lobby->data.emplace_back(std::string(key), std::string(value));
    return true;
}

bool MockSteamApi::setLobbyJoinable(uint64_t id, bool joinable) {
    Lobby* lobby = m_network.lobby(id);
    if (!lobby || lobby->owner != m_state->steamId) return false;
    lobby->joinable = joinable;
    return true;
}

uint64_t MockSteamApi::lobbyOwner(uint64_t id) const {
    const Lobby* lobby = m_network.lobby(id);
    return lobby ? lobby->owner : 0;
}
uint32_t MockSteamApi::lobbyMemberCount(uint64_t id) const {
    const Lobby* lobby = m_network.lobby(id);
    return lobby ? uint32_t(lobby->members.size()) : 0;
}
uint64_t MockSteamApi::lobbyMember(uint64_t id, uint32_t index) const {
    const Lobby* lobby = m_network.lobby(id);
    return lobby && index < lobby->members.size() ? lobby->members[index] : 0;
}
uint32_t MockSteamApi::lobbyMemberLimit(uint64_t id) const {
    const Lobby* lobby = m_network.lobby(id);
    return lobby ? lobby->limit : 0;
}
uint32_t MockSteamApi::lobbyDataCount(uint64_t id) const {
    const Lobby* lobby = m_network.lobby(id);
    return lobby ? uint32_t(lobby->data.size()) : 0;
}
bool MockSteamApi::lobbyDataByIndex(uint64_t id, uint32_t index, std::span<char> key, std::span<char> value) const {
    const Lobby* lobby = m_network.lobby(id);
    if (!lobby || index >= lobby->data.size() || key.empty() || value.empty()) return false;
    std::snprintf(key.data(), key.size(), "%s", lobby->data[index].first.c_str());
    std::snprintf(value.data(), value.size(), "%s", lobby->data[index].second.c_str());
    return true;
}
void MockSteamApi::inviteToLobby(uint64_t) { ++m_state->counts.invites; }

}   // namespace shockwave::net
