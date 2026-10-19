#pragma once

// IOnlineServices: the platform service (Steam) behind multiplayer: whether it's there,
// who we are, lobbies, and making transports. The Player or Editor owns one for the open
// project; it outlives every transport and play session it makes.
//
// Everything asynchronous returns a RequestId at once and reports later through
// pollEvent(), on the main thread, after pump(). A request id carries the service's
// generation, and cancel() means its result is never reported, even if the platform
// finishes it later. A service that isn't available says so (NetErrorCode::Unavailable)
// for every operation; it never pretends one worked.
//
// The Steam implementation is the SteamServices module; makeUnavailableOnlineServices()
// is what builds without Steam use.

#include <Network/NetworkTypes.h>
#include <Network/Transport.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace shockwave::net {

struct RequestId {
    uint64_t value = 0;
    uint32_t serviceGeneration = 0;
    bool valid() const { return value != 0; }
    friend bool operator==(RequestId, RequestId) = default;
};

struct LobbyId {
    uint64_t value = 0;   // decimal strings in JSON and UI, never doubles
    bool valid() const { return value != 0; }
    friend bool operator==(LobbyId, LobbyId) = default;
};

enum class LobbyVisibility : uint8_t { Private, Friends, Public };

struct LobbyCreateRequest {
    LobbyVisibility visibility = LobbyVisibility::Friends;
    uint32_t maxMembers = 12;   // including the host
};

struct LobbyQuery {
    std::vector<std::pair<std::string, std::string>> requiredMetadata;   // e.g. sw_game, sw_protocol
    uint32_t maxResults = 50;
};

struct LobbySummary {
    LobbyId id;
    PeerId owner;
    uint32_t members = 0;
    uint32_t capacity = 0;
    std::vector<std::pair<std::string, std::string>> metadata;
};

enum class OnlineEventKind : uint8_t {
    LobbyCreated,
    LobbyJoined,
    LobbyList,
    LobbyMembersChanged,
    LobbyLeft,
    JoinRequested,   // an invite was accepted, or the game was launched to join a lobby
    RequestFailed,
    AuthTicketReady,   // requestAuthTicket's ticket can be sent: read it with authTicket()
    AuthSucceeded,   // beginAuthSession's peer is who the transport says, and may play
    AuthFailed,   // invalid, revoked or timed out: disconnect the peer (reason says which)
};

struct OnlineEvent {
    OnlineEventKind kind = OnlineEventKind::RequestFailed;
    RequestId request;
    LobbyId lobby;
    NetError error;   // RequestFailed only
    PeerId peer;   // Auth* only
    ReasonCode reason = ReasonCode::Normal;   // AuthFailed: AuthenticationFailed or AuthUnavailable
    std::vector<LobbySummary> lobbies;   // LobbyList only
};

struct OnlineCapabilities {
    bool steamworksBuilt = false;   // this build has the Steamworks adapter
    bool available = false;   // and it's running, signed in
};

class IOnlineServices {
public:
    virtual ~IOnlineServices() = default;

    virtual OnlineCapabilities capabilities() const = 0;
    bool available() const { return capabilities().available; }
    virtual PeerId localPeer() const = 0;

    // Main thread, once per frame, in menus and editing too.
    virtual void pump() = 0;
    // The next finished request or platform event, in order.
    virtual bool pollEvent(OnlineEvent& out) = 0;
    virtual void cancel(RequestId request) = 0;

    virtual NetResult<RequestId> createLobby(const LobbyCreateRequest& request) = 0;
    virtual NetResult<RequestId> findLobbies(const LobbyQuery& query) = 0;
    virtual NetResult<RequestId> joinLobby(LobbyId lobby) = 0;
    virtual NetResult<void> leaveLobby(LobbyId lobby) = 0;
    virtual NetResult<void> inviteFriend(LobbyId lobby) = 0;
    virtual NetResult<void> setLobbyJoinable(LobbyId lobby, bool joinable) = 0;
    virtual NetResult<void> setLobbyMetadata(LobbyId lobby, const std::string& key, const std::string& value) = 0;
    virtual NetResult<std::vector<PeerId>> lobbyMembers(LobbyId lobby) const = 0;
    virtual NetResult<std::unique_ptr<ITransport>> createTransport() = 0;

    // Authentication, both ways: each side asks for a session ticket for the other's
    // identity, sends it over the connected transport, and the receiver passes it to
    // beginAuthSession. A peer is admitted only after AuthSucceeded; a validation that
    // doesn't finish in time fails closed with AuthUnavailable. Tickets are secrets: never
    // log or capture them. cancel() on a ticket request cancels the ticket.
    virtual NetResult<RequestId> requestAuthTicket(PeerId remote) = 0;
    // The ticket's bytes, once AuthTicketReady; valid until it's cancelled.
    virtual NetResult<std::span<const uint8_t>> authTicket(RequestId ticket) const = 0;
    virtual NetResult<void> beginAuthSession(PeerId remote, std::span<const uint8_t> ticket) = 0;
    // On disconnect. Later results for that session are ignored.
    virtual void endAuthSession(PeerId remote) = 0;
};

// Every operation fails with Unavailable and `reason`.
std::unique_ptr<IOnlineServices>
makeUnavailableOnlineServices(const char* reason = "this build has no online service (Steamworks is off)");

}   // namespace shockwave::net
