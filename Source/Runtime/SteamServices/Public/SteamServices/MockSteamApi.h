#pragma once

// MockSteamApi: an in-process stand-in for Steam behind SteamApi, so SteamServices and
// SteamNetworkAdapter run and are tested without the Steamworks SDK, a Steam client or an
// AppID (headless CI). Several mocks share one MockSteamNetwork, which plays Steam's
// backend: identities, P2P listen sockets and connections, a virtual clock, session
// tickets and lobbies. Callbacks are queued and delivered by runCallbacks(), like
// Steam's.
//
// Messages travel through a fixed pool with no delay, so once connected, sending,
// receiving and releasing allocate nothing. Faults (a failed init, listen socket, poll
// group, send, ticket or validation; held lobby results; stale callbacks) are switched
// on per mock. Counts let tests check every message is released exactly once and every
// Steam object is closed.
//
// It isn't Steam: it doesn't model loss, latency, relays or NAT. docs/Networking.md lists
// what still needs the manual two-account test.

#include <SteamServices/SteamApi.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace shockwave::net {

struct MockSteamConfig {
    uint32_t messagePool = 1024;   // messages in flight across every mock
    uint32_t messageBytes = 16384;   // the largest one
};

struct MockSteamFaults {
    bool failInit = false;
    bool loggedOn = true;
    bool failListen = false;
    bool failPollGroup = false;
    bool failSetPollGroup = false;
    bool failConnect = false;
    bool failAccept = false;
    std::optional<SteamResult> sendResult;   // every send returns this instead of sending
    SteamRelayStatus relay = SteamRelayStatus::Current;
    int32_t pingMs = 25;
    uint32_t pendingReliableBytes = 0, pendingUnreliableBytes = 0;   // as connectionStatus reports
    bool failTickets = false;   // tickets are made but reported as failed
    bool holdTickets = false;   // tickets are never reported ready
    SteamBeginAuth beginAuth = SteamBeginAuth::Ok;
    std::optional<SteamAuthResponse> validation = SteamAuthResponse::Ok;   // nullopt: Steam never answers
    bool holdLobbyResults = false;   // until releaseHeldResults()
};

struct MockSteamCounts {
    uint32_t inits = 0, shutdowns = 0;
    uint64_t runCallbacks = 0;
    uint32_t outstandingMessages = 0;   // handed out by receive, not yet released
    uint64_t receivedMessages = 0, releasedMessages = 0, badReleases = 0;
    uint32_t openConnections = 0, listenSockets = 0, pollGroups = 0;
    uint32_t ticketsIssued = 0, ticketsCancelled = 0, authSessionsBegun = 0, authSessionsEnded = 0;
    uint32_t lobbiesLeft = 0, invites = 0;
    uint64_t sends = 0;
};

class MockSteamApi;

class MockSteamNetwork {
public:
    explicit MockSteamNetwork(const MockSteamConfig& config = {});
    ~MockSteamNetwork();
    MockSteamNetwork(const MockSteamNetwork&) = delete;
    MockSteamNetwork& operator=(const MockSteamNetwork&) = delete;

    // A mock signed in as `steamId`. The network must outlive it.
    std::unique_ptr<MockSteamApi> makeApi(uint64_t steamId);
    void advance(uint64_t microseconds);
    uint64_t nowUs() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

class MockSteamApi final : public SteamApi {
public:
    ~MockSteamApi() override;

    MockSteamFaults& faults();
    const MockSteamCounts& counts() const;
    // Delivers `change` at the next runCallbacks(), as a stale or foreign callback would arrive.
    void injectConnectionChange(const SteamConnectionChange& change);
    // Steam reports `response` for an auth session with `steamId` at the next
    // runCallbacks(), as a revoked ticket or ban would.
    void injectValidation(uint64_t steamId, SteamAuthResponse response);
    // Lobby results held by faults().holdLobbyResults, delivered at the next runCallbacks().
    void releaseHeldResults();
    // A friend's invite accepted in the Steam overlay, at the next runCallbacks().
    void injectJoinRequest(uint64_t lobby);
    // This mock's open connection to `steamId` (the newest), or 0.
    uint32_t connectionTo(uint64_t steamId) const;

    bool isSdk() const override { return false; }
    const char* sdkVersion() const override { return "mock"; }
    bool init(
        SteamMode mode, uint32_t appId, const SteamServerParams& server, SteamCallbacks& callbacks,
        std::span<char> error) override;
    void runCallbacks() override;
    void shutdown() override;
    uint64_t localSteamId() const override;
    bool loggedOn() const override;
    SteamRelayStatus relayStatus() const override;
    uint64_t nowUs() const override;

    uint32_t createListenSocketP2P(int virtualPort) override;
    void closeListenSocket(uint32_t socket) override;
    uint32_t createPollGroup() override;
    void destroyPollGroup(uint32_t group) override;
    uint32_t connectP2P(uint64_t steamId, int virtualPort) override;
    SteamResult acceptConnection(uint32_t connection) override;
    bool closeConnection(uint32_t connection, int32_t endReason, bool linger) override;
    bool setConnectionPollGroup(uint32_t connection, uint32_t group) override;
    int receiveMessagesOnPollGroup(uint32_t group, std::span<SteamMessage> out) override;
    void releaseMessage(void* handle) override;
    SteamResult sendMessage(uint32_t connection, std::span<const uint8_t> bytes, SteamSend flags) override;
    bool connectionStatus(uint32_t connection, SteamConnectionStatus& out) const override;

    uint32_t getAuthSessionTicket(uint64_t remoteSteamId, std::span<uint8_t> out, uint32_t& size) override;
    void cancelAuthTicket(uint32_t ticket) override;
    SteamBeginAuth beginAuthSession(std::span<const uint8_t> ticket, uint64_t steamId) override;
    void endAuthSession(uint64_t steamId) override;

    uint64_t createLobby(SteamLobbyType type, uint32_t maxMembers) override;
    void addLobbyListFilter(std::string_view key, std::string_view value) override;
    uint64_t requestLobbyList(uint32_t maxResults) override;
    uint64_t lobbyByIndex(uint32_t index) const override;
    uint64_t joinLobby(uint64_t lobby) override;
    void leaveLobby(uint64_t lobby) override;
    bool setLobbyData(uint64_t lobby, std::string_view key, std::string_view value) override;
    bool setLobbyJoinable(uint64_t lobby, bool joinable) override;
    uint64_t lobbyOwner(uint64_t lobby) const override;
    uint32_t lobbyMemberCount(uint64_t lobby) const override;
    uint64_t lobbyMember(uint64_t lobby, uint32_t index) const override;
    uint32_t lobbyMemberLimit(uint64_t lobby) const override;
    uint32_t lobbyDataCount(uint64_t lobby) const override;
    bool lobbyDataByIndex(uint64_t lobby, uint32_t index, std::span<char> key, std::span<char> value) const override;
    void inviteToLobby(uint64_t lobby) override;
    std::string personaName(uint64_t steamId) const override { return "mock " + std::to_string(steamId % 1000); }

    struct State;

private:
    friend class MockSteamNetwork;
    MockSteamApi(MockSteamNetwork::Impl& network, uint64_t steamId);
    MockSteamNetwork::Impl& m_network;
    std::unique_ptr<State> m_state;
};

}   // namespace shockwave::net
