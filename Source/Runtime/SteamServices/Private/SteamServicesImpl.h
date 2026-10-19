#pragma once

// SteamServices' state. SteamRuntime.cpp holds the lifecycle and callback routing,
// SteamLobbies.cpp the lobbies, SteamAuth.cpp tickets and auth sessions.

#include "SteamTransport.h"

#include <Network/Messages.h>
#include <Network/OnlineServices.h>
#include <SteamServices/SteamApi.h>
#include <SteamServices/SteamServices.h>

#include <array>
#include <cstdint>
#include <memory>

namespace shockwave::net {

// The SDK backend (SteamSdk.cpp), or null in builds without the SDK (SteamUnavailable.cpp).
std::unique_ptr<SteamApi> makeSdkSteamApi();

class SteamServicesImpl final : public SteamCallbacks {
public:
    explicit SteamServicesImpl(std::unique_ptr<SteamApi> api);
    ~SteamServicesImpl() override;

    using Adapter = SteamNetworkAdapter::SteamNetworkAdapterImpl;

    NetResult<void> initialize(SteamMode mode, const SteamServicesSettings& settings, const SteamServerParams& server);
    void framePass();
    void shutdown();
    void push(OnlineEvent&& event);
    RequestId newRequest() { return {m_nextRequest++, m_generation}; }
    bool current(RequestId request) const { return request.valid() && request.serviceGeneration == m_generation; }
    NetError notRunning() const;
    NetError noLobbies() const;   // not running, or a dedicated server
    void unregisterAdapter(Adapter* adapter);
    NetResult<std::unique_ptr<SteamNetworkAdapter>> createAdapter();
    void dropQueued(RequestId request);

    // SteamCallbacks: everything but connection changes is deferred until runCallbacks()
    // returns, so no handler runs inside Steam's dispatch.
    void onConnectionChanged(const SteamConnectionChange& change) override;
    void onAuthTicketReady(uint32_t ticket, bool ok) override;
    void onAuthValidated(uint64_t steamId, uint64_t ownerSteamId, SteamAuthResponse response) override;
    void onLobbyCreated(uint64_t call, bool ok, uint64_t lobby) override;
    void onLobbyList(uint64_t call, bool ok, uint32_t count) override;
    void onLobbyEntered(uint64_t call, bool ok, uint64_t lobby) override;
    void onLobbyMembersChanged(uint64_t lobby) override;
    void onLobbyJoinRequested(uint64_t lobby) override;
    void onServersConnected(bool connected) override;

    // SteamLobbies.cpp.
    NetResult<RequestId> createLobby(const LobbyCreateRequest& request);
    NetResult<RequestId> findLobbies(const LobbyQuery& query);
    NetResult<RequestId> joinLobby(LobbyId lobby);
    NetResult<void> leaveLobby(LobbyId lobby);
    NetResult<void> inviteFriend(LobbyId lobby);
    NetResult<void> setLobbyJoinable(LobbyId lobby, bool joinable);
    NetResult<void> setLobbyMetadata(LobbyId lobby, const std::string& key, const std::string& value);
    NetResult<std::vector<PeerId>> lobbyMembers(LobbyId lobby) const;
    void cancelCall(RequestId request);
    void lobbyCreated(uint64_t call, bool ok, uint64_t lobby);
    void lobbyList(uint64_t call, bool ok, uint32_t count);
    void lobbyEntered(uint64_t call, bool ok, uint64_t lobby);
    void leaveAllLobbies();

    // SteamAuth.cpp.
    NetResult<RequestId> requestAuthTicket(PeerId remote);
    NetResult<std::span<const uint8_t>> authTicket(RequestId ticket) const;
    bool cancelTicket(RequestId ticket);
    NetResult<void> beginAuthSession(PeerId remote, std::span<const uint8_t> ticket);
    void endAuthSession(PeerId remote);
    void ticketReady(uint32_t ticket, bool ok);
    void authValidated(uint64_t steamId, SteamAuthResponse response);
    void expireAuthSessions();
    void endAllAuth();

    std::unique_ptr<SteamApi> api;
    SteamServiceState state = SteamServiceState::Off;
    SteamServicesSettings settings;
    bool loggedOn = false;
    std::array<char, 256> lastError{};

    std::array<Adapter*, 4> adapters{};

    // Finished requests and platform events, oldest first. Fixed, so an idle framePass
    // allocates nothing; moving an OnlineEvent in and out doesn't either.
    static constexpr uint32_t kEventQueue = 256;
    std::array<OnlineEvent, kEventQueue> events{};
    uint32_t eventHead = 0, eventCount = 0;
    uint64_t droppedEvents = 0;

    // Callbacks other than connection changes, until runCallbacks() returns.
    struct Deferred {
        enum Kind : uint8_t {
            TicketReady,
            AuthValidated,
            LobbyCreated,
            LobbyList,
            LobbyEntered,
            MembersChanged,
            JoinRequested,
            ServersConnected,
            CloseOrphan
        };
        Kind kind = TicketReady;
        bool ok = false;
        uint32_t u = 0;
        uint64_t a = 0, b = 0;
        SteamAuthResponse response = SteamAuthResponse::Ok;
    };
    static constexpr uint32_t kDeferredQueue = 256;
    std::array<Deferred, kDeferredQueue> deferred{};
    uint32_t deferredCount = 0;
    void defer(const Deferred& d);

    // Steam API calls in flight (lobbies). A cancelled call keeps its slot until Steam
    // answers, so a late answer is recognised and undone (a created or joined lobby is left).
    enum class CallKind : uint8_t { None, CreateLobby, FindLobbies, JoinLobby };
    struct Call {
        CallKind kind = CallKind::None;
        uint64_t call = 0;
        RequestId request;
        bool cancelled = false;
    };
    std::array<Call, 32> calls{};
    std::array<uint64_t, 4> joinedLobbies{};

    struct Ticket {
        uint32_t handle = 0;   // 0: free
        RequestId request;
        PeerId remote;
        bool ready = false;
        uint32_t size = 0;
        std::array<uint8_t, kMaxAuthTicketBytes> bytes{};
    };
    std::array<Ticket, kMaxTransportConnections + 4> tickets{};

    struct AuthSession {
        PeerId peer;   // invalid: free
        uint64_t deadlineUs = 0;
        bool validated = false;
    };
    std::array<AuthSession, kMaxTransportConnections + 4> sessions{};

private:
    uint64_t m_nextRequest = 1;
    uint32_t m_generation = 0;

    friend class SteamServices;
};

}   // namespace shockwave::net
