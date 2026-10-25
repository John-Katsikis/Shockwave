// A Play session's network side: roles, states, the handshake and shutdown order
// (Multiplayer/NetworkWorld.h). Replication is NetworkReplication.cpp.
#include <Multiplayer/NetworkWorld.h>

#include "NetworkWorldImpl.h"

#include <Core/Log.h>
#include <Network/CaptureTransport.h>

#include <algorithm>
#include <cstdio>

namespace shockwave::net {

const char* networkRoleName(NetworkRole role) {
    switch (role) {
    case NetworkRole::Offline: return "offline";
    case NetworkRole::Authority: return "authority";
    case NetworkRole::Client: return "client";
    }
    return "?";
}

const char* networkStateName(NetworkState state) {
    switch (state) {
    case NetworkState::Offline: return "offline";
    case NetworkState::Starting: return "starting";
    case NetworkState::Connecting: return "connecting";
    case NetworkState::Authenticating: return "authenticating";
    case NetworkState::Synchronizing: return "synchronizing";
    case NetworkState::Active: return "active";
    case NetworkState::Closing: return "closing";
    case NetworkState::Failed: return "failed";
    }
    return "?";
}

namespace {

// A client waiting on the host gives up this much later than the host would.
constexpr double kHostReasonGraceSeconds = 5;

const char* reasonText(ReasonCode reason) {
    const char* name = reasonName(reason);
    return name ? name : "unknown";
}

}   // namespace

NetworkWorld::Impl::Impl(NetworkWorld& world) : w(world) {}

uint32_t NetworkWorld::Impl::maxClients() const {
    return std::clamp<uint32_t>(settings().maxPlayers, 2, kMaxPlayers) - 1;
}

Peer* NetworkWorld::Impl::find(ConnectionId connection) {
    for (Peer& p : peers)
        if (p.phase != Peer::Phase::Free && p.connection == connection) return &p;
    return nullptr;
}

uint32_t NetworkWorld::Impl::count(Peer::Phase phase) const {
    uint32_t n = 0;
    for (const Peer& p : peers) n += p.phase == phase;
    return n;
}

uint32_t NetworkWorld::Impl::admittedCount() const {
    return count(Peer::Phase::Synchronizing) + count(Peer::Phase::Active);
}

uint32_t NetworkWorld::Impl::used() const { return uint32_t(peers.size()) - count(Peer::Phase::Free); }

void NetworkWorld::Impl::enter(Peer& p, Peer::Phase phase) {
    p.phase = phase;
    p.since = haveClock ? now : -1;
}

bool NetworkWorld::Impl::send(ConnectionId connection, const MessageBody& body) {
    if (!w.m_transport || !connection.valid()) return false;
    uint32_t written = 0;
    if (!encodeMessage(body, session, tick, out, written)) return false;
    const MessageDescriptor* d = findMessage(uint8_t(kindOf(body)));
    const SendResult result = w.m_transport->send(connection, std::span(out.data(), written), d->delivery);
    stats.messagesSent += result == SendResult::Queued;
    return result == SendResult::Queued;
}

// Host: forgets a peer whose connection is already closed (or being closed).
void NetworkWorld::Impl::drop(Peer& p, ReasonCode reason) {
    if (p.authBegun && w.m_services) w.m_services->endAuthSession(p.peer);
    if (p.ticketRequest.valid() && w.m_services) w.m_services->cancel(p.ticketRequest);
    const bool left = p.admitted() && host();
    const PeerId peer = p.peer;
    const uint16_t slot = p.slot;
    p = Peer{};
    if (left) {
        SW_LOG(LogNetwork, Info, "Player in slot {} left ({})", slot, reasonText(reason));
        w.push({NetworkEventKind::PlayerLeft, w.m_state, peer, slot, reason});
        ownerLeft(slot);
    }
}

// Host: refuses a peer with a reason it can show, then closes its connection.
void NetworkWorld::Impl::reject(Peer& p, ReasonCode reason, std::string_view diagnostic) {
    Reject message;
    message.reason = reason;
    message.diagnostic = diagnostic;
    send(p.connection, message);
    ++stats.rejected;
    SW_LOG(LogNetwork, Info, "Refused a connection: {} ({})", diagnostic, reasonText(reason));
    if (w.m_transport) w.m_transport->close(p.connection, reason);
    drop(p, reason);
}

void NetworkWorld::Impl::admit(Peer& p) {
    if (admittedCount() >= maxClients()) return reject(p, ReasonCode::ServerFull, "server full");
    uint16_t slot = 2;
    for (; slot <= maxClients() + 1; ++slot) {
        const bool taken =
            std::any_of(peers.begin(), peers.end(), [&](const Peer& o) { return o.admitted() && o.slot == slot; });
        if (!taken) break;
    }
    Welcome welcome;
    welcome.playerSlot = slot;
    welcome.baselineId = 1;
    welcome.serverTick = tick;
    welcome.maxEntities = uint16_t(std::clamp<uint32_t>(settings().maxReplicatedEntities, 12, kMaxNetworkObjects));
    if (!send(p.connection, welcome)) return reject(p, ReasonCode::ServiceUnavailable, "couldn't send Welcome");
    p.slot = slot;
    enter(p, Peer::Phase::Synchronizing);
    SW_LOG(LogNetwork, Info, "Player {} joined in slot {}", p.peer.value, slot);
    w.push({NetworkEventKind::PlayerJoined, w.m_state, p.peer, slot, ReasonCode::Normal});
    startBaseline(p);
}

void NetworkWorld::Impl::maybeAdmit(Peer& p) {
    if (p.phase != Peer::Phase::Authenticating) return;
    if (w.m_start.authenticate && !(p.validated && p.ticketSent)) return;
    admit(p);
}

// Client: Synchronizing once welcomed and (with authentication) the host is proven.
void NetworkWorld::Impl::maybeSynchronized() {
    Peer& h = peers[0];
    if (!welcomed || w.m_state != NetworkState::Authenticating) return;
    if (w.m_start.authenticate && !(h.validated && h.ticketSent)) return;
    enter(h, Peer::Phase::Synchronizing);
    w.setState(NetworkState::Synchronizing);
    tryCommit();   // the baseline may already be here
}

// ---- Transport ----

void NetworkWorld::Impl::poll() {
    ReceiveBudget budget;
    budget.maxMessages = settings().maxReceiveMessagesPerPump;
    budget.maxBytes = settings().maxReceiveBytesPerPump;
    polling = true;
    w.m_transport->poll(*this, budget);
    polling = false;
    if (failPending) finishFail();
}

void NetworkWorld::Impl::finishFail() {
    failPending = false;
    SW_LOG(LogNetwork, Warning, "Network play ended: {} ({})", w.m_failureText, reasonText(w.m_failure));
    w.release(w.m_failure);
    w.setState(NetworkState::Failed);
}

void NetworkWorld::Impl::onEvent(const TransportEvent& event) {
    if (discarding || failPending) return;
    if (event.endpointGeneration != endpointGeneration) {
        ++stats.staleEvents;
        return;
    }
    if (host())
        hostEvent(event);
    else
        clientEvent(event);
}

void NetworkWorld::Impl::hostEvent(const TransportEvent& event) {
    switch (event.kind) {
    case TransportEventKind::Incoming: {
        Peer* free = nullptr;
        for (Peer& p : peers)
            if (p.phase == Peer::Phase::Free) {
                free = &p;
                break;
            }
        if (!free || used() >= maxClients() + kMaxPendingBeyondCapacity) {
            w.m_transport->close(event.connection, ReasonCode::ServerFull);
            ++stats.rejected;
            return;
        }
        if (!w.m_transport->accept(event.connection)) {
            w.m_transport->close(event.connection, ReasonCode::ServiceUnavailable);
            return;
        }
        *free = Peer{};
        free->connection = event.connection;
        free->peer = event.peer;
        enter(*free, Peer::Phase::AwaitingHello);
        return;
    }
    case TransportEventKind::Connected:
        if (Peer* p = find(event.connection)) p->connected = true;
        return;
    case TransportEventKind::Closed:
        if (Peer* p = find(event.connection))
            drop(*p, event.reason == ReasonCode::Normal ? ReasonCode::Normal : event.reason);
        else
            ++stats.staleEvents;
        return;
    }
}

void NetworkWorld::Impl::clientEvent(const TransportEvent& event) {
    Peer& h = peers[0];
    if (event.kind == TransportEventKind::Incoming) {   // clients don't listen
        w.m_transport->close(event.connection, ReasonCode::UnsupportedFeature);
        return;
    }
    if (h.phase == Peer::Phase::Free || !(event.connection == h.connection)) {
        ++stats.staleEvents;
        return;
    }
    if (event.kind == TransportEventKind::Connected) {
        if (h.connected) return;
        h.connected = true;
        h.peer = event.peer;
        Hello hello;
        hello.gameProtocol = w.m_start.compatibility.gameProtocol;
        hello.schemaDigest = w.m_start.compatibility.schemaDigest;
        hello.contentDigest = w.m_start.compatibility.contentDigest;
        hello.sceneId = w.m_start.compatibility.sceneId;
        hello.lobbyId = w.m_start.lobby.value;
        hello.clientNonce = clientNonce;
        if (!send(h.connection, hello)) return failClient(ReasonCode::ServiceUnavailable, "couldn't send Hello");
        enter(h, Peer::Phase::Authenticating);
        w.setState(NetworkState::Authenticating);
        return;
    }
    // Closed: the connection is gone, so there's no one to say Goodbye to.
    h.connection = {};
    h.connected = false;
    ReasonCode reason = event.reason;
    const char* why = "the host closed the connection";
    if (reason == ReasonCode::Normal) {
        reason = w.m_state == NetworkState::Connecting ? ReasonCode::ServiceUnavailable : ReasonCode::HostLeft;
        why = w.m_state == NetworkState::Connecting ? "couldn't reach the host" : "the host left";
    }
    failClient(reason, why);
}

void NetworkWorld::Impl::onMessage(const ReceivedMessage& message) {
    if (discarding || failPending) return;
    if (message.endpointGeneration != endpointGeneration) {
        ++stats.staleEvents;
        return;
    }
    ++stats.messagesReceived;
    stats.bytesReceived += message.bytes.size();
    Peer* p = host() ? find(message.connection) : (peers[0].connection == message.connection ? &peers[0] : nullptr);
    if (!p) {
        ++stats.staleEvents;
        return;
    }
    p->heard = now;
    if (host())
        hostMessage(*p, message);
    else
        clientMessage(*p, message);
}

void NetworkWorld::Impl::hostMessage(Peer& p, const ReceivedMessage& message) {
    // Before a peer is admitted, it gets a small receive budget.
    if (!p.admitted()) {
        if (p.tokensAt >= 0)
            p.tokens = std::min(kPreAuthBurstBytes, p.tokens + (now - p.tokensAt) * kPreAuthBytesPerSecond);
        p.tokensAt = now;
        p.tokens -= double(message.bytes.size());
        if (p.tokens < 0) return reject(p, ReasonCode::RateLimited, "too much data before authentication");
    }
    DecodeOptions options;
    options.delivery = message.delivery;
    options.receiver = PeerRole::Host;
    DecodedMessage decoded;
    if (const WireStatus status = decodeMessage(message.bytes, options, decoded); !status) {
        ++stats.malformed;
        return reject(p, status.reason(), "malformed message");
    }
    const MessageKind kind = kindOf(decoded.body);
    if (kind != MessageKind::Hello && decoded.header.session != session) {
        ++stats.malformed;
        return reject(p, ReasonCode::MalformedPacket, "wrong session");
    }
    switch (kind) {
    case MessageKind::Hello: return hello(p, std::get<Hello>(decoded.body));
    case MessageKind::AuthTicket: {
        const auto& ticket = std::get<AuthTicket>(decoded.body);
        if (!w.m_start.authenticate || p.phase != Peer::Phase::Authenticating || p.authBegun)
            return reject(p, ReasonCode::MalformedPacket, "unexpected AuthTicket");
        if (!w.m_services || !w.m_services->beginAuthSession(p.peer, ticket.ticket))
            return reject(p, ReasonCode::AuthUnavailable, "couldn't validate the ticket");
        p.authBegun = true;
        return;
    }
    case MessageKind::Goodbye: {
        const ConnectionId connection = p.connection;
        drop(p, std::get<Goodbye>(decoded.body).reason);
        w.m_transport->close(connection, ReasonCode::Normal);
        return;
    }
    default:
        if (!p.admitted()) return reject(p, ReasonCode::MalformedPacket, "message before the handshake finished");
        if (kind == MessageKind::Ready) return ready(p, std::get<Ready>(decoded.body));
        if (kind == MessageKind::ResyncRequest) return resyncRequest(p);
        if (kind == MessageKind::Inputs) return inputsMessage(p, std::get<Inputs>(decoded.body));
        if (kind == MessageKind::Command) return commandMessage(p, std::get<Command>(decoded.body));
        if (kind == MessageKind::Ping) return pingMessage(p, std::get<Ping>(decoded.body));
        if (kind == MessageKind::Pong) return hostPong(p, std::get<Pong>(decoded.body));
        ++stats.ignored;
        return;
    }
}

void NetworkWorld::Impl::hello(Peer& p, const Hello& hello) {
    if (p.phase != Peer::Phase::AwaitingHello) return reject(p, ReasonCode::MalformedPacket, "Hello twice");
    const NetworkCompatibility& c = w.m_start.compatibility;
    if (hello.gameProtocol != c.gameProtocol) return reject(p, ReasonCode::ProtocolMismatch, "game protocol differs");
    if (hello.schemaDigest != c.schemaDigest) return reject(p, ReasonCode::ContentMismatch, "schema differs");
    if (hello.contentDigest != c.contentDigest) return reject(p, ReasonCode::ContentMismatch, "content differs");
    if (hello.sceneId != c.sceneId) return reject(p, ReasonCode::MapNotLoaded, "the host is playing another scene");
    if (w.m_start.lobby.valid()) {
        bool member = false;
        if (hello.lobbyId == w.m_start.lobby.value && w.m_services)
            if (auto members = w.m_services->lobbyMembers(w.m_start.lobby))
                member = std::find(members->begin(), members->end(), p.peer) != members->end();
        if (!member) return reject(p, ReasonCode::NotLobbyMember, "not in the host's lobby");
    } else if (hello.lobbyId != 0) {
        return reject(p, ReasonCode::NotLobbyMember, "the host has no lobby");
    }
    if (admittedCount() >= maxClients()) return reject(p, ReasonCode::ServerFull, "server full");
    const NetworkSettings& s = settings();
    Challenge challenge;
    challenge.clientNonce = hello.clientNonce;
    challenge.gameProtocol = c.gameProtocol;
    challenge.schemaDigest = c.schemaDigest;
    challenge.contentDigest = c.contentDigest;
    challenge.sceneId = c.sceneId;
    challenge.hostPeer = w.m_transport->localPeer().value;
    challenge.tickRate = uint16_t(s.tickRate);
    challenge.snapshotRate = uint16_t(s.snapshotRate);
    challenge.interpolationMs = uint16_t(s.interpolationMs);
    challenge.maxRewindMs = uint16_t(s.maxRewindMs);
    if (!send(p.connection, challenge)) return reject(p, ReasonCode::ServiceUnavailable, "couldn't send Challenge");
    enter(p, Peer::Phase::Authenticating);
    if (!w.m_start.authenticate) return admit(p);
    auto ticket = w.m_services->requestAuthTicket(p.peer);
    if (!ticket) return reject(p, ReasonCode::AuthUnavailable, "couldn't make a session ticket");
    p.ticketRequest = ticket.value();
}

void NetworkWorld::Impl::clientMessage(Peer& h, const ReceivedMessage& message) {
    DecodeOptions options;
    options.delivery = message.delivery;
    options.receiver = PeerRole::Client;
    DecodedMessage decoded;
    if (const WireStatus status = decodeMessage(message.bytes, options, decoded); !status) {
        ++stats.malformed;
        return failClient(status.reason(), "malformed message from the host");
    }
    const MessageKind kind = kindOf(decoded.body);
    if (challenged && decoded.header.session != session) {
        ++stats.malformed;
        return failClient(ReasonCode::MalformedPacket, "wrong session from the host");
    }
    switch (kind) {
    case MessageKind::Challenge: return challenge(h, decoded.header, std::get<Challenge>(decoded.body));
    case MessageKind::AuthTicket: {
        const auto& ticket = std::get<AuthTicket>(decoded.body);
        if (!w.m_start.authenticate || !challenged || h.authBegun)
            return failClient(ReasonCode::MalformedPacket, "unexpected AuthTicket from the host");
        if (!w.m_services || !w.m_services->beginAuthSession(h.peer, ticket.ticket))
            return failClient(ReasonCode::AuthUnavailable, "couldn't validate the host's ticket");
        h.authBegun = true;
        return;
    }
    case MessageKind::Welcome: {
        const auto& welcome = std::get<Welcome>(decoded.body);
        if (!challenged || welcomed) return failClient(ReasonCode::MalformedPacket, "unexpected Welcome");
        welcomed = true;
        w.m_localSlot = welcome.playerSlot;
        h.slot = welcome.playerSlot;
        baselineId = welcome.baselineId;
        maxEntities = welcome.maxEntities;
        return maybeSynchronized();
    }
    case MessageKind::Reject: {
        ++stats.rejected;
        const ReasonCode reason = std::get<Reject>(decoded.body).reason;
        // The diagnostic is the host's text: shown nowhere, only the reason is.
        return failClient(reason == ReasonCode::Normal ? ReasonCode::HostLeft : reason, "the host refused to admit us");
    }
    case MessageKind::Goodbye: {
        const ReasonCode reason = std::get<Goodbye>(decoded.body).reason;
        return failClient(reason == ReasonCode::Normal ? ReasonCode::HostLeft : reason, "the host ended the session");
    }
    default:
        if (!welcomed) return failClient(ReasonCode::MalformedPacket, "message before the handshake finished");
        if (kind == MessageKind::Snapshot) {
            // Applied now, or held (with its bytes) until its control serial arrives.
            const auto& snapshot = std::get<Snapshot>(decoded.body);
            ++stats.snapshotPartsReceived;
            if (!activated) return;   // Active comes first; until then the baseline is the state
            if (newer(snapshot.requiredControlSerial, serial))
                return holdSnapshot(message.bytes, snapshot.requiredControlSerial);
            applySnapshot(snapshot);
            return;
        }
        if (kind == MessageKind::Pong) return pongMessage(std::get<Pong>(decoded.body));
        if (kind == MessageKind::Ping) return clientPing(h, std::get<Ping>(decoded.body));
        if (kind == MessageKind::GameEvent) return gameEventMessage(std::get<GameEvent>(decoded.body));
        if (!replicationMessage(decoded)) ++stats.ignored;
        return;
    }
}

void NetworkWorld::Impl::challenge(Peer& h, const MessageHeader& header, const Challenge& c) {
    if (challenged) return failClient(ReasonCode::MalformedPacket, "Challenge twice");
    const NetworkCompatibility& ours = w.m_start.compatibility;
    if (c.clientNonce != clientNonce) return failClient(ReasonCode::MalformedPacket, "Challenge for another Hello");
    if (c.hostPeer != h.peer.value)
        return failClient(ReasonCode::AuthenticationFailed, "the host isn't who the transport says");
    if (c.gameProtocol != ours.gameProtocol) return failClient(ReasonCode::ProtocolMismatch, "game protocol differs");
    if (c.schemaDigest != ours.schemaDigest || c.contentDigest != ours.contentDigest)
        return failClient(ReasonCode::ContentMismatch, "the host's content differs");
    if (c.sceneId != ours.sceneId) return failClient(ReasonCode::MapNotLoaded, "the host is playing another scene");
    if (c.tickRate != settings().tickRate || c.snapshotRate != settings().snapshotRate)
        return failClient(ReasonCode::ContentMismatch, "the host's network settings differ");
    session = header.session;
    challenged = true;
    if (!w.m_start.authenticate) return;
    auto ticket = w.m_services->requestAuthTicket(h.peer);
    if (!ticket) return failClient(ReasonCode::AuthUnavailable, "couldn't make a session ticket");
    h.ticketRequest = ticket.value();
}

// ---- Timeouts ----

void NetworkWorld::Impl::checkTimeouts() {
    const NetworkSettings& s = settings();
    auto expired = [&](const Peer& p, double limit) { return p.since >= 0 && now - p.since > limit; };
    // Silence: nothing at all from the other side (inputs, snapshots and pings flow all the
    // time) for the idle timeout. Transports that time out on their own may get there first.
    auto silent = [&](const Peer& p) {
        const double last = std::max(p.since, p.heard);
        return last >= 0 && now - last > s.idleTimeoutSeconds;
    };
    if (host()) {
        for (Peer& p : peers) {
            switch (p.phase) {
            case Peer::Phase::Free: break;
            case Peer::Phase::Active:
                if (silent(p)) reject(p, ReasonCode::Timeout, "the player stopped answering");
                break;
            case Peer::Phase::AwaitingHello:
                if (expired(p, s.handshakeTimeoutSeconds)) reject(p, ReasonCode::Timeout, "no Hello");
                break;
            case Peer::Phase::Authenticating:
                if (expired(p, s.handshakeTimeoutSeconds))
                    reject(
                        p, p.authBegun && !p.validated ? ReasonCode::AuthUnavailable : ReasonCode::Timeout,
                        "authentication didn't finish");
                break;
            case Peer::Phase::Synchronizing:
                if (expired(p, w.m_start.synchronizeTimeoutSeconds))
                    reject(p, ReasonCode::Timeout, "synchronization didn't finish");
                break;
            }
        }
        return;
    }
    const Peer& h = peers[0];
    switch (w.m_state) {
    case NetworkState::Connecting:
        if (expired(h, s.connectTimeoutSeconds)) failClient(ReasonCode::Timeout, "couldn't connect to the host");
        break;
    case NetworkState::Authenticating:
        // Waiting on the host (our side is done): give its own timeout, and the
        // Reject that says why, time to arrive first.
        if (expired(h, s.handshakeTimeoutSeconds + (h.authBegun && !h.validated ? 0 : kHostReasonGraceSeconds)))
            failClient(
                h.authBegun && !h.validated ? ReasonCode::AuthUnavailable : ReasonCode::Timeout,
                "authentication didn't finish");
        break;
    case NetworkState::Synchronizing:
        // The host gives up at the same moment and says so; give its Reject time too.
        if (expired(h, w.m_start.synchronizeTimeoutSeconds + kHostReasonGraceSeconds))
            failClient(ReasonCode::Timeout, "synchronization didn't finish");
        break;
    case NetworkState::Active:
        if (silent(h)) failClient(ReasonCode::Timeout, "the host stopped answering");
        break;
    default: break;
    }
}

// ---- NetworkWorld ----

NetworkWorld::NetworkWorld() : m_impl(std::make_unique<Impl>(*this)) {}

NetworkWorld::NetworkWorld(const NetworkStart& start, IOnlineServices* services, World* world)
    : m_start(start), m_services(services), m_world(world), m_impl(std::make_unique<Impl>(*this)), m_role(start.role) {
    m_localSlot = m_role == NetworkRole::Client ? 0 : 1;
}

NetworkWorld::~NetworkWorld() { close(); }

size_t NetworkWorld::memoryBytes() const {
    const Impl& impl = *m_impl;
    auto vec = [](const auto& v) { return v.capacity() * sizeof(v[0]); };
    // An unordered_map node: the pair and a next pointer; plus its bucket array.
    auto map = [](const auto& m) {
        using Node = std::pair<typename std::decay_t<decltype(m)>::value_type, void*>;
        return m.size() * sizeof(Node) + m.bucket_count() * sizeof(void*);
    };
    size_t bytes = sizeof(Impl) + vec(impl.objects) + map(impl.byId) + map(impl.localKeys) + vec(impl.pick) +
                   vec(impl.partEnds) + vec(impl.clientTokens) + vec(impl.clientTokensAt) + vec(impl.freeSlots) +
                   vec(impl.predictedScratch) + vec(impl.rewindStates) + vec(impl.rewindEntities) +
                   vec(impl.shotIgnore);
    for (const NetObject& o : impl.objects) bytes += o.key.capacity() > 15 ? o.key.capacity() + 1 : 0;
    for (const Peer& p : impl.peers) bytes += vec(p.queue) + vec(p.commandTokens) + vec(p.commandTokensAt);
    if (impl.interpolation) bytes += sizeof(InterpolationEngine) + impl.interpolation->memoryBytes();
    if (impl.rewind) bytes += sizeof(RewindHistoryBuffer) + impl.rewind->memoryBytes();
    return bytes;
}

const NetworkWorldStats& NetworkWorld::stats() const {
    Impl& impl = *m_impl;
    impl.stats.admitted = impl.admittedCount();
    impl.stats.activePeers = impl.count(Peer::Phase::Active);
    impl.stats.pending = impl.used() - impl.stats.admitted;
    if (m_role == NetworkRole::Client) impl.stats.admitted = impl.stats.pending = impl.stats.activePeers = 0;
    impl.stats.objects = uint32_t(impl.objects.size());
    return impl.stats;
}

void NetworkWorld::peerInfo(std::vector<NetworkPeerInfo>& out) const {
    out.clear();
    const Impl& impl = *m_impl;
    if (!m_transport) return;
    for (const Peer& p : impl.peers) {
        if (p.phase == Peer::Phase::Free) continue;
        NetworkPeerInfo info;
        info.slot = p.admitted() ? p.slot : 0;
        info.peer = p.peer;
        switch (p.phase) {
        case Peer::Phase::AwaitingHello: info.status = "connecting"; break;
        case Peer::Phase::Authenticating: info.status = "authenticating"; break;
        case Peer::Phase::Synchronizing: info.status = "synchronizing"; break;
        case Peer::Phase::Active: info.status = "active"; break;
        case Peer::Phase::Free: break;
        }
        info.rttMs = 1000 * (m_role == NetworkRole::Client ? impl.bestRtt() : impl.peerRoundTrip(p));
        if (const auto stats = m_transport->stats(p.connection)) {
            info.pendingReliableBytes = stats->pendingReliableBytes;
            info.pendingUnreliableBytes = stats->pendingUnreliableBytes;
            info.bytesReceived = stats->bytesReceived;
            info.bytesSent = stats->bytesSent;
            if (!info.peer.valid()) info.peer = stats->peer;
        }
        info.queuedControlBytes = uint32_t(p.queued());
        if (m_role == NetworkRole::Authority && p.lastInputAt >= 0) info.inputAge = impl.now - p.lastInputAt;
        if (m_role == NetworkRole::Client && impl.lastSnapshotAt >= 0)
            info.snapshotAge = impl.now - impl.lastSnapshotAt;
        out.push_back(info);
        if (m_role == NetworkRole::Client) break;   // [0] is the host
    }
    std::stable_sort(out.begin(), out.end(), [](const NetworkPeerInfo& a, const NetworkPeerInfo& b) {
        return (a.slot ? a.slot : 0xFFFF) < (b.slot ? b.slot : 0xFFFF);
    });
}

void NetworkWorld::setState(NetworkState state) {
    if (state == m_state) return;
    m_state = state;
    push(
        {NetworkEventKind::StateChanged, state, {}, 0, state == NetworkState::Failed ? m_failure : ReasonCode::Normal});
}

void NetworkWorld::push(const NetworkEvent& event) {
    if (m_closed) return;
    m_events[m_eventSequence % kMaxEvents] = event;
    ++m_eventSequence;
    if (m_eventSequence - m_pollCursor > kMaxEvents) {
        m_pollCursor = m_eventSequence - kMaxEvents;
        ++m_impl->stats.eventsDropped;
    }
}

bool NetworkWorld::pollEvent(NetworkEvent& out) { return readEvent(m_pollCursor, out); }

bool NetworkWorld::readEvent(uint64_t& cursor, NetworkEvent& out) const {
    if (cursor >= m_eventSequence) return false;
    if (m_eventSequence - cursor > kMaxEvents) cursor = m_eventSequence - kMaxEvents;
    out = m_events[cursor % kMaxEvents];
    ++cursor;
    return true;
}

NetResult<void> NetworkWorld::begin() {
    if (m_role == NetworkRole::Offline) return {};
    if (m_closed || m_state != NetworkState::Offline) return NetError{NetErrorCode::InvalidState, "already started"};
    auto failStart = [&](ReasonCode reason, NetError error) -> NetResult<void> {
        m_failure = reason;
        m_failureText = error.message;
        release(reason);
        setState(NetworkState::Failed);
        SW_LOG(LogNetwork, Warning, "Network play didn't start: {}", error.message);
        return error;
    };
    setState(NetworkState::Starting);
    if (!m_start.authenticate && m_start.address.kind != TransportAddress::Loopback)
        return failStart(
            ReasonCode::AuthUnavailable,
            {NetErrorCode::InvalidArgument, "authentication can only be off for loopback connections"});
    if (!m_services)
        return failStart(ReasonCode::ServiceUnavailable, {NetErrorCode::Unavailable, "no online services"});
    if (m_start.authenticate && !m_services->available())
        return failStart(
            ReasonCode::AuthUnavailable,
            {NetErrorCode::Unavailable, "online services aren't available, so peers can't authenticate"});
    Impl& impl = *m_impl;
    // The scene's network objects are numbered (host) or indexed by key (client) before
    // any connection exists.
    if (auto replication = impl.beginReplication(); !replication)
        return failStart(ReasonCode::ContentMismatch, replication.error());
    if (!secureRandomBytes(m_role == NetworkRole::Authority ? impl.session : impl.clientNonce))
        return failStart(ReasonCode::ServiceUnavailable, {NetErrorCode::Io, "the system random source failed"});
    auto transport = m_services->createTransport();
    if (!transport) return failStart(ReasonCode::ServiceUnavailable, transport.error());
    m_transport = std::move(transport.value());
    if (m_start.capture && m_start.capture->isOpen())
        m_transport = makeCapturingTransport(std::move(m_transport), *m_start.capture);
    if (m_role == NetworkRole::Authority) {
        TransportLimits limits;
        limits.maxConnections = std::min(kMaxTransportConnections, impl.maxClients() + kMaxPendingBeyondCapacity);
        limits.maxMessageBytes = kMaxReliableMessageBytes;
        if (auto listening = m_transport->listen(m_start.address, limits); !listening)
            return failStart(ReasonCode::ServiceUnavailable, listening.error());
        impl.endpointGeneration = m_transport->endpointGeneration();
        m_localSlot = 1;
        setState(NetworkState::Active);
        SW_LOG(
            LogNetwork, Info, "Hosting for up to {} players, {} network objects", impl.maxClients() + 1,
            impl.objects.size());
        return {};
    }
    impl.endpointGeneration = m_transport->endpointGeneration();
    auto connection = m_transport->connect(m_start.address);
    if (!connection) return failStart(ReasonCode::ServiceUnavailable, connection.error());
    impl.peers[0] = Peer{};
    impl.peers[0].connection = connection.value();
    impl.enter(impl.peers[0], Peer::Phase::AwaitingHello);
    setState(NetworkState::Connecting);
    return {};
}

void NetworkWorld::pump(double nowSeconds) {
    if (m_closed || !m_transport) return;
    Impl& impl = *m_impl;
    const WorkTimer timer(impl.stats.workNanos);
    impl.now = nowSeconds;
    if (!impl.haveClock) {
        impl.haveClock = true;
        for (Peer& p : impl.peers)
            if (p.phase != Peer::Phase::Free && p.since < 0) p.since = nowSeconds;
    }
    impl.poll();
    if (m_transport) impl.checkTimeouts();
    if (!m_transport) return;
    if (impl.host()) {
        impl.sendHostPings();
        impl.flushAll();
    } else {
        impl.expireHeld();
        if (m_transport) impl.sendPings();
    }
    if (impl.failPending) impl.finishFail();
}

void NetworkWorld::drain(uint32_t tick) {
    m_impl->tick = tick;
    if (m_closed || !m_transport) return;
    const WorkTimer timer(m_impl->stats.workNanos);
    m_impl->poll();
}

void NetworkWorld::endTick(uint32_t tick) {
    Impl& impl = *m_impl;
    impl.tick = tick;
    if (m_closed || !m_transport || !impl.host()) return;
    const WorkTimer timer(impl.stats.workNanos);
    impl.sweepDestroyed();
    impl.recordCombat();   // where everyone is after this tick, for lag compensation
    impl.sendSnapshots();
    impl.flushAll();
    if (impl.failPending) impl.finishFail();
}

bool NetworkWorld::handleOnlineEvent(const OnlineEvent& event) {
    if (m_closed || !m_transport || m_role == NetworkRole::Offline) return false;
    Impl& impl = *m_impl;
    const size_t n = impl.host() ? impl.peers.size() : 1;
    for (size_t i = 0; i < n; ++i) {
        Peer& p = impl.peers[i];
        if (p.phase == Peer::Phase::Free) continue;
        switch (event.kind) {
        case OnlineEventKind::AuthTicketReady:
        case OnlineEventKind::RequestFailed: {
            if (!p.ticketRequest.valid() || !(event.request == p.ticketRequest)) continue;
            auto bytes = event.kind == OnlineEventKind::AuthTicketReady
                             ? m_services->authTicket(p.ticketRequest)
                             : NetResult<std::span<const uint8_t>>(event.error);
            AuthTicket ticket;
            if (bytes) ticket.ticket = bytes.value();
            if (!bytes || !impl.send(p.connection, ticket)) {
                if (impl.host())
                    impl.reject(p, ReasonCode::AuthUnavailable, "couldn't send a session ticket");
                else
                    impl.failClient(ReasonCode::AuthUnavailable, "couldn't send a session ticket");
                return true;
            }
            p.ticketSent = true;
            if (impl.host())
                impl.maybeAdmit(p);
            else
                impl.maybeSynchronized();
            return true;
        }
        case OnlineEventKind::AuthSucceeded:
        case OnlineEventKind::AuthFailed: {
            if (!p.authBegun || !(event.peer == p.peer)) continue;
            if (p.validated && event.kind == OnlineEventKind::AuthSucceeded) continue;
            if (event.kind == OnlineEventKind::AuthFailed) {
                const ReasonCode reason =
                    event.reason == ReasonCode::Normal ? ReasonCode::AuthenticationFailed : event.reason;
                if (p.validated) {   // revoked later: a ban or a lost licence, even mid-match
                    if (impl.host())
                        impl.reject(p, reason, "authentication revoked");
                    else
                        impl.failClient(reason, "the host's authentication was revoked");
                    return true;
                }
                if (impl.host())
                    impl.reject(p, reason, "authentication failed");
                else
                    impl.failClient(reason, "the host's ticket didn't validate");
                return true;
            }
            p.validated = true;
            if (impl.host())
                impl.maybeAdmit(p);
            else
                impl.maybeSynchronized();
            return true;
        }
        default: break;
        }
    }
    return false;
}

void NetworkWorld::fail(ReasonCode reason, const char* why) {
    if (m_closed || m_role == NetworkRole::Offline || m_state == NetworkState::Failed || m_impl->failPending) return;
    m_failure = reason;
    m_failureText = why ? why : "";
    m_impl->failPending = true;
    if (!m_impl->polling) m_impl->finishFail();
}

void NetworkWorld::release(ReasonCode reason) {
    Impl& impl = *m_impl;
    if (impl.released) return;
    impl.released = true;
    const bool host = m_role == NetworkRole::Authority;
    // Joins stop first, so nobody arrives while the rest shuts down.
    if (host && m_start.lobby.valid() && m_services) (void)m_services->setLobbyJoinable(m_start.lobby, false);
    if (m_transport) {
        Goodbye goodbye;
        goodbye.reason = reason;
        for (Peer& p : impl.peers)
            if (p.phase != Peer::Phase::Free && p.connection.valid() && (host || p.connected))
                impl.send(p.connection, goodbye);
        m_transport->closeAll(reason);
        impl.discarding = true;
        m_transport->poll(impl, {});
        impl.discarding = false;
    }
    for (Peer& p : impl.peers) {
        if (p.phase == Peer::Phase::Free) continue;
        if (p.authBegun && m_services) m_services->endAuthSession(p.peer);
        if (p.ticketRequest.valid() && m_services) m_services->cancel(p.ticketRequest);
        p = Peer{};
    }
    if (m_start.lobby.valid() && m_services) (void)m_services->leaveLobby(m_start.lobby);
    m_transport.reset();
    // The world's entities stay as they are (PlaySession restores the authored scene);
    // the session's maps, queues and held snapshots go.
    impl.clearReplication();
}

void NetworkWorld::close(ReasonCode reason) {
    if (m_closed) return;
    if (m_role != NetworkRole::Offline && m_state != NetworkState::Offline) {
        const bool failed = m_state == NetworkState::Failed;
        setState(NetworkState::Closing);
        m_closed = true;   // nothing is reported from here on
        ++m_generation;
        release(reason);
        m_state = failed ? NetworkState::Failed : NetworkState::Offline;
    }
    m_closed = true;
    m_pollCursor = m_eventSequence;
}

}   // namespace shockwave::net
