#pragma once

// SteamServices: Steam for the Player and Editor (docs/Networking.md). It starts and stops
// the Steam runtime as a game client or as a headless dedicated server, pumps its
// callbacks, and implements IOnlineServices on top: sign-in identity, lobbies, session
// ticket authentication, and transports over Steam Datagram Relay (SteamNetworkAdapter).
//
// - Nothing starts Steam until initializeClient() or initializeDedicatedServer() is
//   called for an online action; --check, schemas, tools and tests never do.
// - framePass() runs Steam's callbacks. Call it once a frame on the main thread, in menus
//   and editing too, before polling its transports.
// - shutdown() closes every transport, cancels tickets, ends auth sessions, leaves
//   lobbies, unregisters callbacks and shuts Steam down, once. The destructor calls it.
// - At most one Steam runtime runs per process.
//
// Steam itself is behind SteamApi (SteamServices/SteamApi.h). Builds configured without
// the Steamworks SDK have no SDK backend: create() still works, and initialising fails
// with Unavailable. Tests pass a MockSteamApi instead.

#include <Network/OnlineServices.h>
#include <SteamServices/SteamApi.h>
#include <SteamServices/SteamNetworkAdapter.h>

#include <cstdint>
#include <memory>

namespace shockwave::net {

struct SteamBuild {
    bool sdk = false;   // built with SHOCKWAVE_ENABLE_STEAM (-DSHOCKWAVE_STEAMWORKS=ON)
    const char* sdkVersion = "";   // the SDK's version as the build found it, or "unknown"
};
SteamBuild steamBuild();

// What a session needs from Config/Network.swnetwork (NetworkSettings):
// steam_app_id, virtual_port, max_snapshot_bytes, auth_timeout_seconds.
struct SteamServicesSettings {
    uint32_t appId = 0;
    uint16_t virtualPort = 0;
    uint32_t maxUnreliableBytes = 1152;   // NetworkSettings::maxSnapshotBytes; at most 1152
    uint32_t maxPendingSendBytes = 32768;   // unreliable sends drop above this much queued
    double authTimeoutSeconds = 15.0;   // fail closed with AuthUnavailable after this
};

enum class SteamServiceState : uint8_t { Off, Client, DedicatedServer };
const char* steamServiceStateName(SteamServiceState state);
const char* steamRelayStatusName(SteamRelayStatus status);

class SteamServicesImpl;

class SteamServices final : public IOnlineServices {
public:
    // The SDK backend when the build has one; otherwise one whose initialize*() fail with
    // Unavailable.
    static std::unique_ptr<SteamServices> create();
    // Any backend; tests pass a MockSteamApi. A null api behaves like a build without the SDK.
    explicit SteamServices(std::unique_ptr<SteamApi> api);
    ~SteamServices() override;
    SteamServices(const SteamServices&) = delete;
    SteamServices& operator=(const SteamServices&) = delete;

    // SteamAPI_Init, then relay network access. Fails with Unavailable (no SDK, Steam not
    // running or not signed in), InvalidArgument (AppID 0) or InvalidState (already
    // running, or another Steam runtime is running in this process).
    NetResult<void> initializeClient(const SteamServicesSettings& settings);
    // SteamGameServer_Init and an anonymous logon, then relay network access. A dedicated
    // server has no lobbies; it listens and authenticates clients.
    NetResult<void> initializeDedicatedServer(const SteamServicesSettings& settings, const SteamServerParams& server);
    // Runs Steam's callbacks and acts on them: transport state changes, finished
    // requests, authentication results and timeouts. Allocates nothing unless a lobby
    // result arrives.
    void framePass();
    void shutdown();

    SteamServiceState state() const;
    // Changes on every initialisation; RequestIds from an earlier one are ignored.
    uint32_t generation() const;
    SteamRelayStatus relayStatus() const;
    // The last initialisation failure as Steam described it, or "".
    const char* lastError() const;
    // A transport with the adapter's extras (broadcast, counters).
    NetResult<std::unique_ptr<SteamNetworkAdapter>> createSteamTransport();

    // IOnlineServices.
    OnlineCapabilities capabilities() const override;
    PeerId localPeer() const override;
    void pump() override { framePass(); }
    bool pollEvent(OnlineEvent& out) override;
    void cancel(RequestId request) override;
    NetResult<RequestId> createLobby(const LobbyCreateRequest& request) override;
    NetResult<RequestId> findLobbies(const LobbyQuery& query) override;
    NetResult<RequestId> joinLobby(LobbyId lobby) override;
    NetResult<void> leaveLobby(LobbyId lobby) override;
    NetResult<void> inviteFriend(LobbyId lobby) override;
    NetResult<void> setLobbyJoinable(LobbyId lobby, bool joinable) override;
    NetResult<void> setLobbyMetadata(LobbyId lobby, const std::string& key, const std::string& value) override;
    NetResult<std::vector<PeerId>> lobbyMembers(LobbyId lobby) const override;
    NetResult<std::unique_ptr<ITransport>> createTransport() override;
    NetResult<RequestId> requestAuthTicket(PeerId remote) override;
    NetResult<std::span<const uint8_t>> authTicket(RequestId ticket) const override;
    NetResult<void> beginAuthSession(PeerId remote, std::span<const uint8_t> ticket) override;
    void endAuthSession(PeerId remote) override;

private:
    std::unique_ptr<SteamServicesImpl> m_impl;
};

}   // namespace shockwave::net
