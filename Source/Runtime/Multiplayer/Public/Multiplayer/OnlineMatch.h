#pragma once

// OnlineMatch: hosting, finding, joining, inviting and leaving, the one implementation the
// player's command line, the editor's Network window and a game's menus all call
// (docs/Networking.md, "Hosting and joining"). It runs the lobby side of section 4 of the
// plan over IOnlineServices; it never touches a transport, and it never starts Play itself.
// It hands the application a NetworkStart, and the application plays the match scene with
// it:
//
//   host():   create a lobby (friends only, max_players members) -> set its metadata ->
//             Starting: the application takes the start and plays the scene as host ->
//             the session is Active -> the lobby becomes joinable -> Hosting
//   join(id): join the lobby -> read its metadata: protocol, game, schema, content and
//             scene must match ours, and the advertised host must be the lobby's owner ->
//             Starting: the application plays the scene as a client of the owner ->
//             the session is Active -> Joined
//   find():   a bounded search for lobbies of this game and protocol (at most 50)
//   leave():  leaves the lobby; the application stops Play and goes back to a safe menu
//
// A session that fails or ends (the host left, a timeout, a rejection) ends the match:
// Failed with the reason, until the next request. An invite accepted in Steam, or a cold
// launch with +connect_lobby, is a pending join: taken at once when idle, otherwise held
// until the player leaves the current match or declines it; a callback never replaces the
// match being played.
//
// The services come from a provider the application sets, so Steam starts only when
// someone asks to play online (never for tools or tests). Everything runs on the main
// thread; nothing here blocks.

#include <Multiplayer/NetworkSettings.h>
#include <Multiplayer/NetworkWorld.h>
#include <Network/OnlineServices.h>

#include <functional>
#include <string>
#include <vector>

namespace shockwave {
struct Project;
}

namespace shockwave::net {

class ReplicationRegistry;

// What every match of this project needs, made once by the application
// (prepareMatchSetup) before anyone hosts or joins.
struct MatchSetup {
    std::string game;   // sw_game: the project's game module ID
    NetworkSettings settings;
    NetworkCompatibility compatibility;   // protocol, schema and content digests, scene ID
    std::string scenePath;   // the scene to play, inside the project ("Scenes/arena-mp.swscene")
    const ReplicationRegistry* registry = nullptr;   // frozen; must outlive the match
    std::vector<NetworkPrefab> prefabs;
    TransportAddress::Kind addressKind = TransportAddress::SteamPeer;   // Loopback in tests
    LobbyVisibility visibility = LobbyVisibility::Friends;
};

// Reads the project's network settings and manifest and fills setup for its network
// scene: `scene` (inside the project) when given, else match_scene, else the manifest's
// first scene. False,
// with every reason in problems, when the project can't play online (multiplayer off,
// errors in the settings or manifest, the scene isn't a network scene). Reads every
// content file for the content digest; opens no socket and starts nothing.
bool prepareMatchSetup(
    const Project& project, const std::string& gameModule, const ReplicationRegistry& registry,
    const std::string& scene, MatchSetup& setup, std::vector<std::string>& problems);

enum class MatchState : uint8_t {
    Idle,   // offline, nothing in flight
    CreatingLobby,   // host: waiting for the platform's lobby
    JoiningLobby,   // client: waiting to enter the lobby
    Starting,   // a NetworkStart is ready (takeStart), or the session is connecting
    Hosting,   // host: the session is Active and the lobby open to joins
    Joined,   // client: the session is Active
    Failed,   // see error(); the next request starts over
};
const char* matchStateName(MatchState state);

// A lobby found by find(), checked against our setup.
struct LobbyListing {
    LobbyId id;
    PeerId host;
    std::string hostName;   // untrusted display text, at most 64 bytes
    uint32_t players = 0, capacity = 0;
    std::string scene;   // sw_scene, as advertised
    std::string state;   // sw_state: waiting or playing
    bool compatible = false;
    std::string problem;   // why not, when it isn't
};

class OnlineMatch {
public:
    // Returns the services to use, starting them if needed, or null with error set.
    using ServiceProvider = std::function<IOnlineServices*(std::string& error)>;

    OnlineMatch() = default;
    OnlineMatch(const OnlineMatch&) = delete;
    OnlineMatch& operator=(const OnlineMatch&) = delete;

    void setSetup(MatchSetup setup) { m_setup = std::move(setup); }
    const MatchSetup& setup() const { return m_setup; }
    bool configured() const { return !m_setup.scenePath.empty(); }
    void setServiceProvider(ServiceProvider provider) { m_provider = std::move(provider); }
    // The services in use once a request needed them; null before.
    IOnlineServices* services() const { return m_services; }

    // ---- Requests (false, with error(), when it can't begin) ----
    bool host();
    bool find();
    bool join(LobbyId lobby);
    // Developer direct join to a host's Steam ID, outside any lobby. The host must have
    // started with hostDirect(): a host with a lobby admits only its members.
    bool joinDirect(PeerId host);
    // Developer host with no lobby, which admits direct joins (still authenticated).
    bool hostDirect();
    // Opens the platform's invite dialog for our lobby.
    bool invite();
    // Leaves the lobby and forgets the match; the application stops Play. Cancels a
    // request still in flight.
    void leave();

    // ---- The application, every frame ----
    // A platform event (from the services' pollEvent). True when it was the match's.
    bool handleOnlineEvent(const OnlineEvent& event);
    // Once a start is ready: the NetworkStart to play setup().scenePath with, replacing any
    // Play (so a pending takeEnded() is dropped). Taken once.
    bool takeStart(NetworkStart& out);
    // The session the start made (null when Play stopped): its state moves the match on,
    // and its end ends the match. Call after the session pumped.
    void update(const NetworkWorld* session);
    // True once when a match the application started Play for has ended (failed, host
    // left, left): stop Play and go back to a safe menu or the offline scene.
    bool takeEnded();

    // ---- Pending joins from invites and +connect_lobby ----
    LobbyId pendingJoin() const { return m_pendingJoin; }
    void requestJoin(LobbyId lobby);   // what JoinRequested and +connect_lobby do
    bool acceptPendingJoin();   // leaves the current match, then joins
    void declinePendingJoin() { m_pendingJoin = {}; }

    // ---- What the UI shows ----
    MatchState state() const { return m_state; }
    NetworkRole role() const { return m_role; }
    LobbyId lobby() const { return m_lobby; }
    bool busy() const;   // a request in flight or a match on
    bool inMatch() const;   // Starting, Hosting or Joined
    const std::string& status() const { return m_status; }   // one line for the UI
    const std::string& error() const { return m_error; }
    bool searching() const { return m_findRequest.valid(); }
    const std::vector<LobbyListing>& lobbies() const { return m_lobbies; }
    bool directOnly() const { return m_direct; }

    static constexpr uint32_t kMaxLobbyResults = 50;
    static constexpr const char* kProtocol = "1";

private:
    bool begin(const char* what);
    bool failRequest(std::string why);
    void fail(std::string why);
    void finish(MatchState state, std::string status);
    bool applyMetadata();
    bool checkLobby(const LobbySummary& lobby, std::string& problem) const;
    NetworkStart makeStart(NetworkRole role, PeerId host, uint16_t port) const;
    void leaveLobby();

    MatchSetup m_setup;
    ServiceProvider m_provider;
    IOnlineServices* m_services = nullptr;
    MatchState m_state = MatchState::Idle;
    NetworkRole m_role = NetworkRole::Offline;
    RequestId m_request;   // create or join in flight
    RequestId m_findRequest;
    LobbyId m_lobby;
    LobbyId m_pendingJoin;
    bool m_direct = false;
    bool m_startReady = false, m_started = false;   // a start to take; Play was started with it
    bool m_ended = false;
    bool m_sessionSeen = false;   // the started session existed at the last update
    uint32_t m_advertisedPlayers = 0;
    NetworkStart m_start;
    std::string m_status = "offline", m_error;
    std::vector<LobbyListing> m_lobbies;
};

}   // namespace shockwave::net
