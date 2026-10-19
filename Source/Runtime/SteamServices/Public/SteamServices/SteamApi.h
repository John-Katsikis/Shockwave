#pragma once

// SteamApi: every Steamworks call SteamServices makes, as one interface with no SDK types.
//
// SteamServices and SteamNetworkAdapter hold all the logic (state tables, generations,
// budgets, release rules, timeouts) and talk to Steam only through this. Two things
// implement it:
// - the SDK backend, compiled only when the build has SHOCKWAVE_ENABLE_STEAM (configure
//   with -DSHOCKWAVE_STEAMWORKS=ON); the only code that includes Steam headers;
// - MockSteamApi (SteamServices/MockSteamApi.h), an in-process stand-in for tests and
//   for builds without the SDK.
//
// Handles are the SDK's own numbers (HSteamNetConnection, HSteamListenSocket,
// HSteamNetPollGroup, SteamAPICall_t, HAuthTicket), carried as plain integers; 0 is never
// a valid one. Every function is called on the main thread. Callbacks arrive through
// SteamCallbacks, only from inside runCallbacks().

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace shockwave::net {

enum class SteamMode : uint8_t {
    Client,   // SteamAPI_Init: a player signed in to the Steam client
    DedicatedServer,   // SteamGameServer_Init: a headless server, signed in anonymously
};

struct SteamServerParams {
    uint32_t ipv4 = 0;   // 0: any
    uint16_t gamePort = 27015;
    uint16_t queryPort = 27016;
    std::string_view version = "1.0.0.0";   // shown in the server browser
};

// What a Steam API call reported, collapsed to what SteamServices acts on.
enum class SteamResult : uint8_t {
    Ok,
    NoConnection,   // the handle is gone or closed
    InvalidParam,
    InvalidState,   // e.g. not connected yet
    LimitExceeded,   // a send queue is full
    Fail,
};

// ESteamNetworkingConnectionState, the states the adapter acts on.
enum class SteamConnectionState : uint8_t {
    None,
    Connecting,
    FindingRoute,
    Connected,
    ClosedByPeer,
    ProblemDetectedLocally,
};

// SteamNetConnectionStatusChangedCallback_t.
struct SteamConnectionChange {
    uint32_t connection = 0;
    uint32_t listenSocket = 0;   // the listener it arrived on; 0 for our own connect()
    SteamConnectionState state = SteamConnectionState::None;
    SteamConnectionState oldState = SteamConnectionState::None;
    uint64_t remoteSteamId = 0;   // the identity Steam authenticated, never one a peer claims
    int32_t endReason = 0;   // ESteamNetConnectionEnd
};

// SteamNetConnectionRealTimeStatus_t.
struct SteamConnectionStatus {
    SteamConnectionState state = SteamConnectionState::None;
    int32_t pingMs = -1;
    uint32_t pendingReliableBytes = 0;
    uint32_t pendingUnreliableBytes = 0;
};

// A received SteamNetworkingMessage_t, borrowed until releaseMessage(handle).
struct SteamMessage {
    void* handle = nullptr;
    uint32_t connection = 0;
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    bool reliable = false;
    int64_t receivedUs = 0;
};

// Send flags, as SteamNetworkAdapter chooses them. The SDK backend maps them to
// k_nSteamNetworkingSend_ReliableNoNagle and k_nSteamNetworkingSend_UnreliableNoNagle.
enum class SteamSend : uint8_t { ReliableNoNagle, UnreliableNoNagle };

// ESteamNetworkingAvailability for relay network access, collapsed.
enum class SteamRelayStatus : uint8_t { Unknown, Waiting, Current, Failed };

// k_ESteamNetConnectionEnd_App_Min: end reasons from here up are the application's own.
// SteamNetworkAdapter sends ReasonCode n as kSteamAppEndReasonBase + n.
inline constexpr int32_t kSteamAppEndReasonBase = 1000;

// EBeginAuthSessionResult, collapsed.
enum class SteamBeginAuth : uint8_t { Ok, InvalidTicket, Duplicate, InvalidVersion, GameMismatch, Expired };

// EAuthSessionResponse, collapsed.
enum class SteamAuthResponse : uint8_t {
    Ok,
    UserNotConnectedToSteam,
    NoLicense,
    VacBanned,
    LoggedInElsewhere,
    VacCheckTimedOut,
    TicketCanceled,
    TicketInvalidAlreadyUsed,
    TicketInvalid,
    PublisherIssuedBan,
    NetworkIdentityFailure,
};

enum class SteamLobbyType : uint8_t { Private, FriendsOnly, Public };

// Callbacks from runCallbacks(). Implementations queue what they're told and act on it
// after runCallbacks() returns; nothing here may call back into SteamApi.
class SteamCallbacks {
public:
    virtual ~SteamCallbacks() = default;
    virtual void onConnectionChanged(const SteamConnectionChange& change) = 0;
    virtual void onAuthTicketReady(uint32_t ticket, bool ok) = 0;
    virtual void onAuthValidated(uint64_t steamId, uint64_t ownerSteamId, SteamAuthResponse response) = 0;
    virtual void onLobbyCreated(uint64_t call, bool ok, uint64_t lobby) = 0;
    virtual void onLobbyList(uint64_t call, bool ok, uint32_t count) = 0;   // read with lobbyByIndex
    virtual void onLobbyEntered(uint64_t call, bool ok, uint64_t lobby) = 0;
    virtual void onLobbyMembersChanged(uint64_t lobby) = 0;
    virtual void onLobbyJoinRequested(uint64_t lobby) = 0;
    virtual void onServersConnected(bool connected) = 0;
};

class SteamApi {
public:
    virtual ~SteamApi() = default;

    // True for the SDK backend: at most one may be initialised per process.
    virtual bool isSdk() const = 0;
    virtual const char* sdkVersion() const = 0;   // "unknown" when the build doesn't know it

    // SteamAPI_Init or SteamGameServer_Init (then LogOnAnonymous), and then
    // InitRelayNetworkAccess. On failure `error` holds the SDK's message (never a ticket or
    // credential) and nothing needs shutting down.
    virtual bool init(
        SteamMode mode, uint32_t appId, const SteamServerParams& server, SteamCallbacks& callbacks,
        std::span<char> error) = 0;
    // SteamAPI_RunCallbacks or SteamGameServer_RunCallbacks.
    virtual void runCallbacks() = 0;
    // Unregisters every callback, then SteamAPI_Shutdown or SteamGameServer_Shutdown.
    virtual void shutdown() = 0;
    virtual uint64_t localSteamId() const = 0;   // 0 until signed in
    virtual bool loggedOn() const = 0;
    virtual SteamRelayStatus relayStatus() const = 0;
    // A steady clock in microseconds (the mock's is virtual).
    virtual uint64_t nowUs() const = 0;

    // ISteamNetworkingSockets.
    virtual uint32_t createListenSocketP2P(int virtualPort) = 0;
    virtual void closeListenSocket(uint32_t socket) = 0;
    virtual uint32_t createPollGroup() = 0;
    virtual void destroyPollGroup(uint32_t group) = 0;
    virtual uint32_t connectP2P(uint64_t steamId, int virtualPort) = 0;
    virtual SteamResult acceptConnection(uint32_t connection) = 0;
    virtual bool closeConnection(uint32_t connection, int32_t endReason, bool linger) = 0;
    virtual bool setConnectionPollGroup(uint32_t connection, uint32_t group) = 0;
    // Fills `out` with up to out.size() messages; each must be released exactly once.
    virtual int receiveMessagesOnPollGroup(uint32_t group, std::span<SteamMessage> out) = 0;
    virtual void releaseMessage(void* handle) = 0;
    virtual SteamResult sendMessage(uint32_t connection, std::span<const uint8_t> bytes, SteamSend flags) = 0;
    virtual bool connectionStatus(uint32_t connection, SteamConnectionStatus& out) const = 0;

    // ISteamUser / ISteamGameServer authentication.
    // GetAuthSessionTicket for `remoteSteamId`: fills `out` and returns the ticket handle,
    // or 0. The ticket is usable once onAuthTicketReady reports it.
    virtual uint32_t getAuthSessionTicket(uint64_t remoteSteamId, std::span<uint8_t> out, uint32_t& size) = 0;
    virtual void cancelAuthTicket(uint32_t ticket) = 0;
    virtual SteamBeginAuth beginAuthSession(std::span<const uint8_t> ticket, uint64_t steamId) = 0;
    virtual void endAuthSession(uint64_t steamId) = 0;

    // ISteamMatchmaking and ISteamFriends (clients only). Calls return a SteamAPICall_t,
    // 0 if it couldn't start.
    virtual uint64_t createLobby(SteamLobbyType type, uint32_t maxMembers) = 0;
    virtual void addLobbyListFilter(std::string_view key, std::string_view value) = 0;
    virtual uint64_t requestLobbyList(uint32_t maxResults) = 0;
    virtual uint64_t lobbyByIndex(uint32_t index) const = 0;
    virtual uint64_t joinLobby(uint64_t lobby) = 0;
    virtual void leaveLobby(uint64_t lobby) = 0;
    virtual bool setLobbyData(uint64_t lobby, std::string_view key, std::string_view value) = 0;
    virtual bool setLobbyJoinable(uint64_t lobby, bool joinable) = 0;
    virtual uint64_t lobbyOwner(uint64_t lobby) const = 0;
    virtual uint32_t lobbyMemberCount(uint64_t lobby) const = 0;
    virtual uint64_t lobbyMember(uint64_t lobby, uint32_t index) const = 0;
    virtual uint32_t lobbyMemberLimit(uint64_t lobby) const = 0;
    virtual uint32_t lobbyDataCount(uint64_t lobby) const = 0;
    // Copies key and value (each up to 256 bytes, null-terminated) into the buffers.
    virtual bool lobbyDataByIndex(uint64_t lobby, uint32_t index, std::span<char> key, std::span<char> value) const = 0;
    virtual void inviteToLobby(uint64_t lobby) = 0;   // the Steam overlay's invite dialog
};

}   // namespace shockwave::net
