// The Steamworks SDK backend of SteamApi: the only file in the engine that includes Steam
// headers, compiled only when the build has SHOCKWAVE_ENABLE_STEAM
// (-DSHOCKWAVE_STEAMWORKS=ON with a developer-supplied SDK, cmake/Steamworks.cmake). Each
// function is a direct call into the SDK; the logic around them is in SteamRuntime.cpp,
// SteamTransport.cpp, SteamLobbies.cpp and SteamAuth.cpp, and is tested with MockSteamApi.
//
// Written against the Steamworks SDK's documented API; the SDK version it was first
// compiled with is recorded in docs/NetworkingVerification.md. It needs an SDK new enough
// to have GetAuthSessionTicket with a remote identity and GetConnectionRealTimeStatus.
#ifdef SHOCKWAVE_ENABLE_STEAM

#include "SteamServicesImpl.h"

#include <Core/Log.h>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steam_api.h>
#include <steam/steam_gameserver.h>
#include <steam/steamnetworkingtypes.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#ifndef SHOCKWAVE_STEAMWORKS_VERSION
#define SHOCKWAVE_STEAMWORKS_VERSION "unknown"
#endif

namespace shockwave::net {

namespace {

// uint64_t is unsigned long on Linux but the SDK's uint64 is unsigned long long, so
// CSteamID(uint64_t) is ambiguous there.
CSteamID steamIdFrom(uint64_t id) { return CSteamID(uint64(id)); }

SteamConnectionState stateFrom(ESteamNetworkingConnectionState state) {
    switch (state) {
    case k_ESteamNetworkingConnectionState_Connecting: return SteamConnectionState::Connecting;
    case k_ESteamNetworkingConnectionState_FindingRoute: return SteamConnectionState::FindingRoute;
    case k_ESteamNetworkingConnectionState_Connected: return SteamConnectionState::Connected;
    case k_ESteamNetworkingConnectionState_ClosedByPeer: return SteamConnectionState::ClosedByPeer;
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: return SteamConnectionState::ProblemDetectedLocally;
    default: return SteamConnectionState::None;
    }
}

SteamResult resultFrom(EResult result) {
    switch (result) {
    case k_EResultOK: return SteamResult::Ok;
    case k_EResultNoConnection: return SteamResult::NoConnection;
    case k_EResultInvalidParam: return SteamResult::InvalidParam;
    case k_EResultInvalidState: return SteamResult::InvalidState;
    case k_EResultLimitExceeded: return SteamResult::LimitExceeded;
    default: return SteamResult::Fail;
    }
}

SteamAuthResponse responseFrom(EAuthSessionResponse response) {
    switch (response) {
    case k_EAuthSessionResponseOK: return SteamAuthResponse::Ok;
    case k_EAuthSessionResponseUserNotConnectedToSteam: return SteamAuthResponse::UserNotConnectedToSteam;
    case k_EAuthSessionResponseNoLicenseOrExpired: return SteamAuthResponse::NoLicense;
    case k_EAuthSessionResponseVACBanned: return SteamAuthResponse::VacBanned;
    case k_EAuthSessionResponseLoggedInElseWhere: return SteamAuthResponse::LoggedInElsewhere;
    case k_EAuthSessionResponseVACCheckTimedOut: return SteamAuthResponse::VacCheckTimedOut;
    case k_EAuthSessionResponseAuthTicketCanceled: return SteamAuthResponse::TicketCanceled;
    case k_EAuthSessionResponseAuthTicketInvalidAlreadyUsed: return SteamAuthResponse::TicketInvalidAlreadyUsed;
    case k_EAuthSessionResponseAuthTicketInvalid: return SteamAuthResponse::TicketInvalid;
    case k_EAuthSessionResponsePublisherIssuedBan: return SteamAuthResponse::PublisherIssuedBan;
    case k_EAuthSessionResponseAuthTicketNetworkIdentityFailure: return SteamAuthResponse::NetworkIdentityFailure;
    }
    return SteamAuthResponse::TicketInvalid;
}

void copyError(std::span<char> out, const char* text) {
    if (out.empty()) return;
    std::snprintf(out.data(), out.size(), "%s", text);
}

// SDK debug output arrives on Steam's own threads: keep a few lines for the main thread.
struct DebugLines {
    std::mutex mutex;
    std::array<std::array<char, 256>, 16> lines{};
    uint32_t count = 0;
    uint32_t dropped = 0;
};
DebugLines g_debug;

void debugOutput(ESteamNetworkingSocketsDebugOutputType, const char* text) {
    std::lock_guard lock(g_debug.mutex);
    if (g_debug.count == g_debug.lines.size()) {
        ++g_debug.dropped;
        return;
    }
    std::snprintf(g_debug.lines[g_debug.count].data(), g_debug.lines[g_debug.count].size(), "%s", text ? text : "");
    ++g_debug.count;
}

class SdkSteamApi;

// One Steam API call in flight, with the id SteamServices knows it by.
template <class Result>
struct CallHolder {
    SdkSteamApi* owner = nullptr;
    uint64_t call = 0;
    CCallResult<CallHolder, Result> result;
    void onResult(Result* data, bool ioFailure);
};

class SdkSteamApi final : public SteamApi {
public:
    bool isSdk() const override { return true; }
    const char* sdkVersion() const override { return SHOCKWAVE_STEAMWORKS_VERSION; }

    bool init(
        SteamMode mode, uint32_t appId, const SteamServerParams& server, SteamCallbacks& callbacks,
        std::span<char> error) override {
        m_server = mode == SteamMode::DedicatedServer;
        if (m_server) {
            const std::string version(server.version);
            if (!SteamGameServer_Init(
                    server.ipv4, server.gamePort, server.queryPort, eServerModeAuthenticationAndSecure,
                    version.c_str())) {
                copyError(error, "SteamGameServer_Init failed: is steam_appid.txt there, and the ports free?");
                return false;
            }
            if (SteamGameServerUtils()->GetAppID() != appId) {
                copyError(error, "the Steam game server started for a different AppID than steam_app_id");
                SteamGameServer_Shutdown();
                return false;
            }
            SteamGameServer()->SetProduct(std::to_string(appId).c_str());
            SteamGameServer()->SetGameDescription("Shockwave dedicated server");
            SteamGameServer()->SetDedicatedServer(true);
            SteamGameServer()->LogOnAnonymous();
            m_sockets = SteamGameServerNetworkingSockets();
        } else {
            // Development needs a steam_appid.txt holding the AppID in the working folder;
            // nothing here writes one.
            if (!SteamAPI_Init()) {
                copyError(error, "SteamAPI_Init failed: is Steam running and signed in, with steam_appid.txt present?");
                return false;
            }
            if (SteamUtils()->GetAppID() != appId) {
                copyError(error, "Steam started for a different AppID than steam_app_id (check steam_appid.txt)");
                SteamAPI_Shutdown();
                return false;
            }
            m_sockets = SteamNetworkingSockets();
        }
        if (!m_sockets) {
            copyError(error, "Steam networking sockets are unavailable");
            shutdownSdk();
            return false;
        }
        m_callbacks = &callbacks;
        registerCallbacks();
        SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Warning, debugOutput);
        SteamNetworkingUtils()->InitRelayNetworkAccess();
        return true;
    }

    void runCallbacks() override {
        if (m_server)
            SteamGameServer_RunCallbacks();
        else
            SteamAPI_RunCallbacks();
        std::array<std::array<char, 256>, 16> lines;
        uint32_t count = 0, dropped = 0;
        {
            std::lock_guard lock(g_debug.mutex);
            lines = g_debug.lines;
            count = g_debug.count;
            dropped = g_debug.dropped;
            g_debug.count = g_debug.dropped = 0;
        }
        for (uint32_t i = 0; i < count; ++i) SW_LOG(LogNetwork, Warning, "Steam networking: {}", lines[i].data());
        if (dropped) SW_LOG(LogNetwork, Warning, "Steam networking: {} more lines dropped", dropped);
    }

    void shutdown() override {
        if (!m_callbacks) return;
        SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_None, nullptr);
        unregisterCallbacks();
        m_callbacks = nullptr;
        m_sockets = nullptr;
        if (m_server) SteamGameServer()->LogOff();
        shutdownSdk();
    }

    uint64_t localSteamId() const override {
        if (!m_callbacks) return 0;
        return (m_server ? SteamGameServer()->GetSteamID() : SteamUser()->GetSteamID()).ConvertToUint64();
    }
    bool loggedOn() const override {
        if (!m_callbacks) return false;
        return m_server ? SteamGameServer()->BLoggedOn() : SteamUser()->BLoggedOn();
    }
    SteamRelayStatus relayStatus() const override {
        if (!m_callbacks) return SteamRelayStatus::Unknown;
        const ESteamNetworkingAvailability a = SteamNetworkingUtils()->GetRelayNetworkStatus(nullptr);
        if (a == k_ESteamNetworkingAvailability_Current) return SteamRelayStatus::Current;
        if (a == k_ESteamNetworkingAvailability_Retrying || a == k_ESteamNetworkingAvailability_NeverTried ||
            a == k_ESteamNetworkingAvailability_Waiting || a == k_ESteamNetworkingAvailability_Attempting)
            return SteamRelayStatus::Waiting;
        if (a == k_ESteamNetworkingAvailability_Unknown) return SteamRelayStatus::Unknown;
        return SteamRelayStatus::Failed;
    }
    uint64_t nowUs() const override { return uint64_t(SteamNetworkingUtils()->GetLocalTimestamp()); }

    uint32_t createListenSocketP2P(int virtualPort) override {
        return m_sockets->CreateListenSocketP2P(virtualPort, 0, nullptr);
    }
    void closeListenSocket(uint32_t socket) override { m_sockets->CloseListenSocket(socket); }
    uint32_t createPollGroup() override { return m_sockets->CreatePollGroup(); }
    void destroyPollGroup(uint32_t group) override { m_sockets->DestroyPollGroup(group); }
    uint32_t connectP2P(uint64_t steamId, int virtualPort) override {
        SteamNetworkingIdentity identity;
        identity.SetSteamID64(steamId);
        return m_sockets->ConnectP2P(identity, virtualPort, 0, nullptr);
    }
    SteamResult acceptConnection(uint32_t connection) override {
        return resultFrom(m_sockets->AcceptConnection(connection));
    }
    bool closeConnection(uint32_t connection, int32_t endReason, bool linger) override {
        return m_sockets->CloseConnection(connection, endReason, nullptr, linger);
    }
    bool setConnectionPollGroup(uint32_t connection, uint32_t group) override {
        return m_sockets->SetConnectionPollGroup(connection, group);
    }
    int receiveMessagesOnPollGroup(uint32_t group, std::span<SteamMessage> out) override {
        std::array<SteamNetworkingMessage_t*, 32> messages{};
        const int max = int(std::min(out.size(), messages.size()));
        const int n = m_sockets->ReceiveMessagesOnPollGroup(group, messages.data(), max);
        for (int i = 0; i < n; ++i) {
            SteamNetworkingMessage_t* m = messages[size_t(i)];
            SteamMessage& o = out[size_t(i)];
            o.handle = m;
            o.connection = m->m_conn;
            o.data = static_cast<const uint8_t*>(m->m_pData);
            o.size = m->m_cbSize > 0 ? uint32_t(m->m_cbSize) : 0;
            o.reliable = (m->m_nFlags & k_nSteamNetworkingSend_Reliable) != 0;
            o.receivedUs = m->m_usecTimeReceived;
        }
        return std::max(n, 0);
    }
    void releaseMessage(void* handle) override { static_cast<SteamNetworkingMessage_t*>(handle)->Release(); }
    SteamResult sendMessage(uint32_t connection, std::span<const uint8_t> bytes, SteamSend flags) override {
        const int sendFlags = flags == SteamSend::ReliableNoNagle ? k_nSteamNetworkingSend_ReliableNoNagle
                                                                  : k_nSteamNetworkingSend_UnreliableNoNagle;
        return resultFrom(
            m_sockets->SendMessageToConnection(connection, bytes.data(), uint32(bytes.size()), sendFlags, nullptr));
    }
    bool connectionStatus(uint32_t connection, SteamConnectionStatus& out) const override {
        SteamNetConnectionRealTimeStatus_t status;
        if (m_sockets->GetConnectionRealTimeStatus(connection, &status, 0, nullptr) != k_EResultOK) return false;
        out.state = stateFrom(status.m_eState);
        out.pingMs = status.m_nPing;
        out.pendingReliableBytes = uint32_t(std::max(0, status.m_cbPendingReliable + status.m_cbSentUnackedReliable));
        out.pendingUnreliableBytes = uint32_t(std::max(0, status.m_cbPendingUnreliable));
        return true;
    }

    uint32_t getAuthSessionTicket(uint64_t remoteSteamId, std::span<uint8_t> out, uint32_t& size) override {
        SteamNetworkingIdentity identity;
        identity.SetSteamID64(remoteSteamId);
        uint32 written = 0;
        const HAuthTicket ticket =
            m_server ? SteamGameServer()->GetAuthSessionTicket(out.data(), int(out.size()), &written, &identity)
                     : SteamUser()->GetAuthSessionTicket(out.data(), int(out.size()), &written, &identity);
        size = written;
        return ticket == k_HAuthTicketInvalid ? 0 : ticket;
    }
    void cancelAuthTicket(uint32_t ticket) override {
        if (m_server)
            SteamGameServer()->CancelAuthTicket(ticket);
        else
            SteamUser()->CancelAuthTicket(ticket);
    }
    SteamBeginAuth beginAuthSession(std::span<const uint8_t> ticket, uint64_t steamId) override {
        const EBeginAuthSessionResult result =
            m_server ? SteamGameServer()->BeginAuthSession(ticket.data(), int(ticket.size()), steamIdFrom(steamId))
                     : SteamUser()->BeginAuthSession(ticket.data(), int(ticket.size()), steamIdFrom(steamId));
        switch (result) {
        case k_EBeginAuthSessionResultOK: return SteamBeginAuth::Ok;
        case k_EBeginAuthSessionResultInvalidTicket: return SteamBeginAuth::InvalidTicket;
        case k_EBeginAuthSessionResultDuplicateRequest: return SteamBeginAuth::Duplicate;
        case k_EBeginAuthSessionResultInvalidVersion: return SteamBeginAuth::InvalidVersion;
        case k_EBeginAuthSessionResultGameMismatch: return SteamBeginAuth::GameMismatch;
        case k_EBeginAuthSessionResultExpiredTicket: return SteamBeginAuth::Expired;
        }
        return SteamBeginAuth::InvalidTicket;
    }
    void endAuthSession(uint64_t steamId) override {
        if (m_server)
            SteamGameServer()->EndAuthSession(steamIdFrom(steamId));
        else
            SteamUser()->EndAuthSession(steamIdFrom(steamId));
    }

    uint64_t createLobby(SteamLobbyType type, uint32_t maxMembers) override {
        const ELobbyType t = type == SteamLobbyType::Public        ? k_ELobbyTypePublic
                             : type == SteamLobbyType::FriendsOnly ? k_ELobbyTypeFriendsOnly
                                                                   : k_ELobbyTypePrivate;
        return track(m_created, SteamMatchmaking()->CreateLobby(t, int(maxMembers)));
    }
    void addLobbyListFilter(std::string_view key, std::string_view value) override {
        SteamMatchmaking()->AddRequestLobbyListStringFilter(
            std::string(key).c_str(), std::string(value).c_str(), k_ELobbyComparisonEqual);
    }
    uint64_t requestLobbyList(uint32_t maxResults) override {
        SteamMatchmaking()->AddRequestLobbyListFilterSlotsAvailable(1);
        SteamMatchmaking()->AddRequestLobbyListResultCountFilter(int(maxResults));
        return track(m_lists, SteamMatchmaking()->RequestLobbyList());
    }
    uint64_t lobbyByIndex(uint32_t index) const override {
        return SteamMatchmaking()->GetLobbyByIndex(int(index)).ConvertToUint64();
    }
    uint64_t joinLobby(uint64_t lobby) override {
        return track(m_entered, SteamMatchmaking()->JoinLobby(steamIdFrom(lobby)));
    }
    void leaveLobby(uint64_t lobby) override { SteamMatchmaking()->LeaveLobby(steamIdFrom(lobby)); }
    bool setLobbyData(uint64_t lobby, std::string_view key, std::string_view value) override {
        return SteamMatchmaking()->SetLobbyData(
            steamIdFrom(lobby), std::string(key).c_str(), std::string(value).c_str());
    }
    bool setLobbyJoinable(uint64_t lobby, bool joinable) override {
        return SteamMatchmaking()->SetLobbyJoinable(steamIdFrom(lobby), joinable);
    }
    uint64_t lobbyOwner(uint64_t lobby) const override {
        return SteamMatchmaking()->GetLobbyOwner(steamIdFrom(lobby)).ConvertToUint64();
    }
    uint32_t lobbyMemberCount(uint64_t lobby) const override {
        return uint32_t(std::max(0, SteamMatchmaking()->GetNumLobbyMembers(steamIdFrom(lobby))));
    }
    uint64_t lobbyMember(uint64_t lobby, uint32_t index) const override {
        return SteamMatchmaking()->GetLobbyMemberByIndex(steamIdFrom(lobby), int(index)).ConvertToUint64();
    }
    uint32_t lobbyMemberLimit(uint64_t lobby) const override {
        return uint32_t(std::max(0, SteamMatchmaking()->GetLobbyMemberLimit(steamIdFrom(lobby))));
    }
    uint32_t lobbyDataCount(uint64_t lobby) const override {
        return uint32_t(std::max(0, SteamMatchmaking()->GetLobbyDataCount(steamIdFrom(lobby))));
    }
    bool lobbyDataByIndex(uint64_t lobby, uint32_t index, std::span<char> key, std::span<char> value) const override {
        return SteamMatchmaking()->GetLobbyDataByIndex(
            steamIdFrom(lobby), int(index), key.data(), int(key.size()), value.data(), int(value.size()));
    }
    void inviteToLobby(uint64_t lobby) override { SteamFriends()->ActivateGameOverlayInviteDialog(steamIdFrom(lobby)); }
    std::string personaName(uint64_t steamId) const override {
        if (!SteamFriends()) return {};
        const char* name = SteamFriends()->GetFriendPersonaName(steamIdFrom(steamId));
        return name ? std::string(name) : std::string();
    }
    bool restartAppIfNecessary(uint32_t appId) override { return SteamAPI_RestartAppIfNecessary(appId); }

    // CallHolder results.
    void finish(uint64_t call, LobbyCreated_t* data, bool ioFailure) {
        const bool ok = !ioFailure && data->m_eResult == k_EResultOK;
        if (m_callbacks) m_callbacks->onLobbyCreated(call, ok, ok ? data->m_ulSteamIDLobby : 0);
    }
    void finish(uint64_t call, LobbyMatchList_t* data, bool ioFailure) {
        if (m_callbacks) m_callbacks->onLobbyList(call, !ioFailure, ioFailure ? 0 : data->m_nLobbiesMatching);
    }
    void finish(uint64_t call, LobbyEnter_t* data, bool ioFailure) {
        const bool ok = !ioFailure && data->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess;
        if (m_callbacks) m_callbacks->onLobbyEntered(call, ok, data->m_ulSteamIDLobby);
    }

private:
    template <class Result, size_t N>
    uint64_t track(std::array<CallHolder<Result>, N>& pool, SteamAPICall_t call) {
        if (call == k_uAPICallInvalid) return 0;
        for (CallHolder<Result>& holder : pool)
            if (holder.call == 0) {
                holder.owner = this;
                holder.call = call;
                holder.result.Set(call, &holder, &CallHolder<Result>::onResult);
                return call;
            }
        return 0;   // more than N in flight: SteamServices never asks for that many
    }

    void onConnectionChanged(SteamNetConnectionStatusChangedCallback_t* data) {
        SteamConnectionChange change;
        change.connection = data->m_hConn;
        change.listenSocket = data->m_info.m_hListenSocket;
        change.state = stateFrom(data->m_info.m_eState);
        change.oldState = stateFrom(data->m_eOldState);
        change.remoteSteamId = data->m_info.m_identityRemote.GetSteamID64();
        change.endReason = data->m_info.m_eEndReason;
        m_callbacks->onConnectionChanged(change);
    }
    void onTicket(GetAuthSessionTicketResponse_t* data) {
        m_callbacks->onAuthTicketReady(data->m_hAuthTicket, data->m_eResult == k_EResultOK);
    }
    void onValidate(ValidateAuthTicketResponse_t* data) {
        m_callbacks->onAuthValidated(
            data->m_SteamID.ConvertToUint64(), data->m_OwnerSteamID.ConvertToUint64(),
            responseFrom(data->m_eAuthSessionResponse));
    }
    void onChatUpdate(LobbyChatUpdate_t* data) { m_callbacks->onLobbyMembersChanged(data->m_ulSteamIDLobby); }
    void onJoinRequested(GameLobbyJoinRequested_t* data) {
        m_callbacks->onLobbyJoinRequested(data->m_steamIDLobby.ConvertToUint64());
    }
    void onConnected(SteamServersConnected_t*) { m_callbacks->onServersConnected(true); }
    void onDisconnected(SteamServersDisconnected_t*) { m_callbacks->onServersConnected(false); }

    template <class Callback>
    void add(Callback& callback) {
        if (m_server) callback.SetGameserverFlag();
    }

    void registerCallbacks() {
        add(m_connectionChanged);
        add(m_ticket);
        add(m_validate);
        add(m_connected);
        add(m_disconnected);
        m_connectionChanged.Register(this, &SdkSteamApi::onConnectionChanged);
        m_ticket.Register(this, &SdkSteamApi::onTicket);
        m_validate.Register(this, &SdkSteamApi::onValidate);
        m_connected.Register(this, &SdkSteamApi::onConnected);
        m_disconnected.Register(this, &SdkSteamApi::onDisconnected);
        if (!m_server) {   // lobbies are the Steam client's
            m_chatUpdate.Register(this, &SdkSteamApi::onChatUpdate);
            m_joinRequested.Register(this, &SdkSteamApi::onJoinRequested);
        }
    }

    void unregisterCallbacks() {
        m_connectionChanged.Unregister();
        m_ticket.Unregister();
        m_validate.Unregister();
        m_connected.Unregister();
        m_disconnected.Unregister();
        m_chatUpdate.Unregister();
        m_joinRequested.Unregister();
        for (auto& h : m_created) cancel(h);
        for (auto& h : m_lists) cancel(h);
        for (auto& h : m_entered) cancel(h);
    }

    template <class Holder>
    static void cancel(Holder& holder) {
        holder.result.Cancel();
        holder.call = 0;
    }

    void shutdownSdk() {
        if (m_server)
            SteamGameServer_Shutdown();
        else
            SteamAPI_Shutdown();
    }

    bool m_server = false;
    SteamCallbacks* m_callbacks = nullptr;
    ISteamNetworkingSockets* m_sockets = nullptr;
    CCallbackManual<SdkSteamApi, SteamNetConnectionStatusChangedCallback_t> m_connectionChanged;
    CCallbackManual<SdkSteamApi, GetAuthSessionTicketResponse_t> m_ticket;
    CCallbackManual<SdkSteamApi, ValidateAuthTicketResponse_t> m_validate;
    CCallbackManual<SdkSteamApi, SteamServersConnected_t> m_connected;
    CCallbackManual<SdkSteamApi, SteamServersDisconnected_t> m_disconnected;
    CCallbackManual<SdkSteamApi, LobbyChatUpdate_t> m_chatUpdate;
    CCallbackManual<SdkSteamApi, GameLobbyJoinRequested_t> m_joinRequested;
    std::array<CallHolder<LobbyCreated_t>, 32> m_created;
    std::array<CallHolder<LobbyMatchList_t>, 32> m_lists;
    std::array<CallHolder<LobbyEnter_t>, 32> m_entered;
};

template <class Result>
void CallHolder<Result>::onResult(Result* data, bool ioFailure) {
    owner->finish(std::exchange(call, 0), data, ioFailure);
}

}   // namespace

std::unique_ptr<SteamApi> makeSdkSteamApi() { return std::make_unique<SdkSteamApi>(); }

SteamBuild steamBuild() { return {true, SHOCKWAVE_STEAMWORKS_VERSION}; }

}   // namespace shockwave::net

#endif   // SHOCKWAVE_ENABLE_STEAM
