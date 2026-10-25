// Hosting, finding, joining, inviting and leaving over IOnlineServices (Multiplayer/OnlineMatch.h).
#include <Multiplayer/OnlineMatch.h>

#include <Core/Log.h>
#include <Multiplayer/NetworkManifest.h>
#include <Multiplayer/NetworkValidation.h>
#include <Multiplayer/Replication.h>
#include <Network/Sha256.h>
#include <Projects/Project.h>

#include <algorithm>
#include <array>
#include <charconv>

namespace shockwave::net {

namespace {

const std::string* metadata(const LobbySummary& lobby, std::string_view key) {
    for (const auto& [k, v] : lobby.metadata)
        if (k == key) return &v;
    return nullptr;
}

bool parseU64(const std::string& text, uint64_t& out) {
    if (text.empty() || text.size() > 20) return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && end == text.data() + text.size();
}

// Display text from the platform: printable, at most 64 bytes, cut on a UTF-8 boundary.
std::string displayText(std::string text) {
    std::erase_if(text, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
    if (text.size() > 64) {
        size_t cut = 64;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text.resize(cut);
    }
    return text;
}

}   // namespace

const char* matchStateName(MatchState state) {
    switch (state) {
    case MatchState::Idle: return "idle";
    case MatchState::CreatingLobby: return "creating lobby";
    case MatchState::JoiningLobby: return "joining lobby";
    case MatchState::Starting: return "starting";
    case MatchState::Hosting: return "hosting";
    case MatchState::Joined: return "joined";
    case MatchState::Failed: return "failed";
    }
    return "?";
}

bool prepareMatchSetup(
    const Project& project, const std::string& gameModule, const ReplicationRegistry& registry,
    const std::string& scene, MatchSetup& setup, std::vector<std::string>& problems) {
    const size_t before = problems.size();
    NetworkProject network;
    std::vector<NetProblem> found;
    loadNetworkProject(project, network, found);
    uint16_t sceneId = 0;
    std::string scenePath;
    if (network.manifest) {
        for (const ManifestScene& s : network.manifest->scenes)
            if (scene.empty() ? sceneId == 0 : s.path == scene) {
                sceneId = s.id;
                scenePath = s.path;
            }
        if (sceneId == 0)
            found.push_back(
                {false, network.manifestShown + ": " +
                            (scene.empty() ? std::string("there's no scene to play online")
                                           : scene + " isn't a network scene in the manifest")});
    }
    if (sceneId)
        readyForSession(project, network, registry, sceneId, found);
    else if (!network.present || !network.manifest)
        readyForSession(project, network, registry, 1, found);
    ContentDigest content;
    if (network.manifest && sceneId)
        computeContentDigest(project, *network.manifest, network.manifestShown, content, found);
    for (const NetProblem& p : found)
        if (!p.warning) problems.push_back(p.text);
    if (problems.size() != before) return false;
    setup = {};
    setup.game = gameModule;
    setup.settings = network.settings.settings;
    setup.compatibility.gameProtocol = network.manifest->gameProtocol;
    setup.compatibility.schemaDigest = registry.schemaDigest();
    setup.compatibility.contentDigest = content.digest;
    setup.compatibility.sceneId = sceneId;
    setup.scenePath = scenePath;
    setup.registry = &registry;
    for (const ManifestPrefab& p : network.manifest->prefabs) setup.prefabs.push_back({p.id, p.path, p.profile});
    return true;
}

bool OnlineMatch::busy() const {
    return m_state == MatchState::CreatingLobby || m_state == MatchState::JoiningLobby || inMatch();
}

bool OnlineMatch::inMatch() const {
    return m_state == MatchState::Starting || m_state == MatchState::Hosting || m_state == MatchState::Joined;
}

bool OnlineMatch::failRequest(std::string why) {
    m_error = std::move(why);
    m_status = m_error;
    SW_LOG(LogNetwork, Warning, "Online: {}", m_error);
    return false;
}

void OnlineMatch::fail(std::string why) {
    leaveLobby();
    if (m_request.valid() && m_services) m_services->cancel(m_request);
    m_request = {};
    m_ended = m_started;
    m_startReady = m_started = false;
    m_sessionSeen = false;
    m_role = NetworkRole::Offline;
    m_direct = false;
    m_state = MatchState::Failed;
    failRequest(std::move(why));
}

void OnlineMatch::finish(MatchState state, std::string status) {
    m_state = state;
    m_status = std::move(status);
    SW_LOG(LogNetwork, Info, "Online: {}", m_status);
}

// What every request needs: a setup, services that are signed in, and nothing else on.
bool OnlineMatch::begin(const char* what) {
    m_error.clear();
    if (!configured()) return failRequest(std::string("can't ") + what + ": this project has no network scene to play");
    if (busy()) return failRequest(std::string("can't ") + what + ": leave the current match first");
    if (!m_services) {
        std::string why;
        m_services = m_provider ? m_provider(why) : nullptr;
        if (!m_services)
            return failRequest(std::string("can't ") + what + ": " + (why.empty() ? "there's no online service" : why));
    }
    if (!m_services->available())
        return failRequest(std::string("can't ") + what + ": Steam isn't running or isn't signed in");
    return true;
}

NetworkStart OnlineMatch::makeStart(NetworkRole role, PeerId host, uint16_t port) const {
    NetworkStart start;
    start.role = role;
    start.settings = m_setup.settings;
    start.compatibility = m_setup.compatibility;
    start.address = {m_setup.addressKind, role == NetworkRole::Client ? host.value : 0, port};
    start.lobby = m_lobby;
    start.registry = m_setup.registry;
    start.prefabs = m_setup.prefabs;
    return start;
}

bool OnlineMatch::host() {
    if (!begin("host")) return false;
    LobbyCreateRequest request;
    request.visibility = m_setup.visibility;
    request.maxMembers = m_setup.settings.maxPlayers;
    const NetResult<RequestId> created = m_services->createLobby(request);
    if (!created) return failRequest(std::string("can't host: ") + created.error().message);
    m_request = created.value();
    m_role = NetworkRole::Authority;
    m_direct = false;
    finish(MatchState::CreatingLobby, "creating a lobby");
    return true;
}

bool OnlineMatch::hostDirect() {
    if (!begin("host")) return false;
    m_role = NetworkRole::Authority;
    m_direct = true;
    m_lobby = {};
    m_start = makeStart(NetworkRole::Authority, {}, uint16_t(m_setup.settings.virtualPort));
    m_startReady = true;
    finish(MatchState::Starting, "starting a direct-join host (developer)");
    return true;
}

bool OnlineMatch::join(LobbyId lobby) {
    if (!lobby.valid()) return failRequest("can't join: that isn't a lobby ID");
    if (!begin("join")) return false;
    const NetResult<RequestId> joining = m_services->joinLobby(lobby);
    if (!joining) return failRequest(std::string("can't join: ") + joining.error().message);
    m_request = joining.value();
    m_role = NetworkRole::Client;
    m_direct = false;
    if (m_pendingJoin == lobby) m_pendingJoin = {};
    finish(MatchState::JoiningLobby, "joining lobby " + std::to_string(lobby.value));
    return true;
}

bool OnlineMatch::joinDirect(PeerId host) {
    if (!host.valid()) return failRequest("can't join: that isn't a Steam ID");
    if (!begin("join")) return false;
    m_role = NetworkRole::Client;
    m_direct = true;
    m_lobby = {};
    m_start = makeStart(NetworkRole::Client, host, uint16_t(m_setup.settings.virtualPort));
    m_startReady = true;
    finish(MatchState::Starting, "connecting directly to " + std::to_string(host.value) + " (developer)");
    return true;
}

bool OnlineMatch::find() {
    m_error.clear();
    if (searching()) return true;   // one search at a time; its results replace the list
    if (!configured()) return failRequest("can't search: this project has no network scene to play");
    if (!m_services) {
        std::string why;
        m_services = m_provider ? m_provider(why) : nullptr;
        if (!m_services)
            return failRequest(std::string("can't search: ") + (why.empty() ? "there's no online service" : why));
    }
    if (!m_services->available()) return failRequest("can't search: Steam isn't running or isn't signed in");
    LobbyQuery query;
    query.requiredMetadata = {{"sw_game", m_setup.game}, {"sw_protocol", kProtocol}};
    query.maxResults = kMaxLobbyResults;
    const NetResult<RequestId> request = m_services->findLobbies(query);
    if (!request) return failRequest(std::string("can't search: ") + request.error().message);
    m_findRequest = request.value();
    if (!busy()) m_status = "searching for matches";
    return true;
}

bool OnlineMatch::invite() {
    m_error.clear();
    if (!m_services || !m_lobby.valid() || !(m_state == MatchState::Hosting || m_state == MatchState::Joined))
        return failRequest("can't invite: there's no lobby to invite friends to");
    if (const NetResult<void> invited = m_services->inviteFriend(m_lobby); !invited)
        return failRequest(std::string("can't invite: ") + invited.error().message);
    return true;
}

void OnlineMatch::leaveLobby() {
    // A host's session closes its lobby to joins and leaves it as Play stops (NetworkWorld
    // close()); before its Play started, the match leaves it here.
    const bool sessionOwnsLobby = m_role == NetworkRole::Authority && m_started;
    if (m_lobby.valid() && m_services && !sessionOwnsLobby) (void)m_services->leaveLobby(m_lobby);
    m_lobby = {};
}

void OnlineMatch::leave() {
    if (m_request.valid() && m_services) m_services->cancel(m_request);
    m_request = {};
    leaveLobby();
    m_ended = m_started;
    m_startReady = m_started = false;
    m_sessionSeen = false;
    m_role = NetworkRole::Offline;
    m_direct = false;
    m_error.clear();
    finish(MatchState::Idle, "offline");
}

void OnlineMatch::requestJoin(LobbyId lobby) {
    if (!lobby.valid()) return;
    if (!busy() && configured()) {
        join(lobby);
        return;
    }
    m_pendingJoin = lobby;   // the UI asks first: leaving ends the match being played
    m_status = "a friend's invite is waiting: leave this match to join theirs";
}

bool OnlineMatch::acceptPendingJoin() {
    const LobbyId lobby = m_pendingJoin;
    if (!lobby.valid()) return false;
    if (busy()) leave();
    m_pendingJoin = {};
    return join(lobby);
}

bool OnlineMatch::applyMetadata() {
    std::array<uint8_t, 16> nonce{};
    if (!secureRandomBytes(nonce)) return false;
    static const char* digits = "0123456789abcdef";
    std::string session;
    for (const uint8_t b : nonce) {
        session += digits[b >> 4];
        session += digits[b & 15];
    }
    const std::pair<const char*, std::string> values[] = {
        {"sw_protocol", kProtocol},
        {"sw_game", m_setup.game},
        {"sw_schema", toHex(m_setup.compatibility.schemaDigest)},
        {"sw_content", toHex(m_setup.compatibility.contentDigest)},
        {"sw_scene", std::to_string(m_setup.compatibility.sceneId)},
        {"sw_host", std::to_string(m_services->localPeer().value)},
        {"sw_session", session},
        {"sw_state", "waiting"},
        {"sw_port", std::to_string(m_setup.settings.virtualPort)},
        {"sw_players", "1"},
        {"sw_capacity", std::to_string(m_setup.settings.maxPlayers)},
    };
    for (const auto& [key, value] : values)
        if (!m_services->setLobbyMetadata(m_lobby, key, value)) return false;
    m_advertisedPlayers = 1;
    return true;
}

// Is this lobby a match we can join? The checks a join repeats once inside the lobby.
bool OnlineMatch::checkLobby(const LobbySummary& lobby, std::string& problem) const {
    auto value = [&](const char* key) { return metadata(lobby, key); };
    const std::string* protocol = value("sw_protocol");
    const std::string* game = value("sw_game");
    const std::string* host = value("sw_host");
    const std::string* schema = value("sw_schema");
    const std::string* content = value("sw_content");
    const std::string* scene = value("sw_scene");
    const std::string* port = value("sw_port");
    if (!protocol || !game || !host || !schema || !content || !scene || !port) {
        problem = "the lobby's details are incomplete";
        return false;
    }
    uint64_t hostId = 0, portNumber = 0;
    if (*protocol != kProtocol)
        problem = "it uses another network protocol (" + *protocol + ")";
    else if (*game != m_setup.game)
        problem = "it's another game";
    else if (!parseU64(*host, hostId) || PeerId{hostId} != lobby.owner)
        problem = "the advertised host isn't the lobby's owner";
    else if (*schema != toHex(m_setup.compatibility.schemaDigest))
        problem = "it runs a different build of the game (network types differ)";
    else if (*content != toHex(m_setup.compatibility.contentDigest))
        problem = "it has different content (maps, prefabs or circuits differ)";
    else if (*scene != std::to_string(m_setup.compatibility.sceneId))
        problem = "it plays a different map";
    else if (!parseU64(*port, portNumber) || portNumber != m_setup.settings.virtualPort)
        problem = "it listens on a different port";
    else
        return true;
    return false;
}

bool OnlineMatch::handleOnlineEvent(const OnlineEvent& event) {
    switch (event.kind) {
    case OnlineEventKind::LobbyCreated: {
        if (!m_request.valid() || event.request != m_request) return false;
        m_request = {};
        m_lobby = event.lobby;
        if (m_state != MatchState::CreatingLobby) {
            leaveLobby();
            return true;
        }
        // Advertise first, open to joins last (once the session listens).
        if (!m_services->setLobbyJoinable(m_lobby, false) || !applyMetadata()) {
            fail("can't host: the lobby's details couldn't be set");
            return true;
        }
        m_start = makeStart(NetworkRole::Authority, {}, uint16_t(m_setup.settings.virtualPort));
        m_startReady = true;
        finish(MatchState::Starting, "lobby " + std::to_string(m_lobby.value) + " made; starting the match");
        return true;
    }
    case OnlineEventKind::LobbyJoined: {
        if (!m_request.valid() || event.request != m_request) return false;
        m_request = {};
        m_lobby = event.lobby;
        if (m_state != MatchState::JoiningLobby) {
            leaveLobby();
            return true;
        }
        const NetResult<LobbySummary> info = m_services->lobbyInfo(m_lobby);
        std::string problem;
        if (!info) {
            fail(std::string("can't join: ") + info.error().message);
            return true;
        }
        if (!checkLobby(info.value(), problem)) {
            fail("can't join: " + problem);
            return true;
        }
        m_start = makeStart(NetworkRole::Client, info->owner, uint16_t(m_setup.settings.virtualPort));
        m_startReady = true;
        finish(MatchState::Starting, "in lobby " + std::to_string(m_lobby.value) + "; connecting to the host");
        return true;
    }
    case OnlineEventKind::LobbyList: {
        if (!m_findRequest.valid() || event.request != m_findRequest) return false;
        m_findRequest = {};
        m_lobbies.clear();
        for (const LobbySummary& lobby : event.lobbies) {
            if (m_lobbies.size() == kMaxLobbyResults) break;
            LobbyListing listing;
            listing.id = lobby.id;
            listing.host = lobby.owner;
            listing.hostName = displayText(m_services->displayName(lobby.owner));
            listing.capacity = lobby.capacity;
            uint64_t players = lobby.members;
            if (const std::string* p = metadata(lobby, "sw_players")) parseU64(*p, players);
            listing.players = uint32_t(std::min<uint64_t>(players, 64));
            if (const std::string* s = metadata(lobby, "sw_scene")) listing.scene = displayText(*s);
            if (const std::string* s = metadata(lobby, "sw_state")) listing.state = displayText(*s);
            listing.compatible = checkLobby(lobby, listing.problem);
            if (listing.compatible && listing.players >= listing.capacity) {
                listing.compatible = false;
                listing.problem = "it's full";
            }
            m_lobbies.push_back(std::move(listing));
        }
        std::stable_sort(m_lobbies.begin(), m_lobbies.end(), [](const LobbyListing& a, const LobbyListing& b) {
            return a.compatible > b.compatible;
        });
        if (!busy())
            m_status = std::to_string(m_lobbies.size()) + (m_lobbies.size() == 1 ? " match found" : " matches found");
        return true;
    }
    case OnlineEventKind::RequestFailed:
        if (m_findRequest.valid() && event.request == m_findRequest) {
            m_findRequest = {};
            failRequest(std::string("the search failed: ") + event.error.message);
            return true;
        }
        if (m_request.valid() && event.request == m_request) {
            fail(std::string(m_role == NetworkRole::Client ? "can't join: " : "can't host: ") + event.error.message);
            return true;
        }
        return false;
    case OnlineEventKind::JoinRequested: requestJoin(event.lobby); return true;
    case OnlineEventKind::LobbyMembersChanged:
    case OnlineEventKind::LobbyLeft:
        // The session itself checks membership; ours is only for the UI.
        return event.lobby.valid() && event.lobby == m_lobby && m_role != NetworkRole::Authority;
    default: return false;
    }
}

bool OnlineMatch::takeStart(NetworkStart& out) {
    if (!m_startReady) return false;
    out = m_start;
    m_startReady = false;
    m_started = true;
    m_ended = false;   // the new Play replaces the one that ended
    m_sessionSeen = false;
    return true;
}

void OnlineMatch::update(const NetworkWorld* session) {
    if (!m_started) return;
    const bool online = session && session->online();
    if (!online) {
        // Play stopped, or never went online: the match is over.
        if (m_sessionSeen || session) {
            leaveLobby();
            m_ended = true;
            m_started = false;
            m_sessionSeen = false;
            m_role = NetworkRole::Offline;
            m_direct = false;
            finish(MatchState::Idle, "offline");
        }
        return;
    }
    m_sessionSeen = true;
    if (session->state() == NetworkState::Failed) {
        const char* reason = reasonName(session->failure());
        fail(
            (m_role == NetworkRole::Client ? std::string("disconnected: ") : std::string("the match ended: ")) +
            (session->failureText().empty() ? std::string(reason ? reason : "the session failed")
                                            : session->failureText()));
        return;
    }
    if (session->state() != NetworkState::Active) return;
    if (m_state == MatchState::Starting) {
        if (m_role == NetworkRole::Authority) {
            if (m_lobby.valid() && !m_services->setLobbyJoinable(m_lobby, true)) {
                fail("can't host: the lobby couldn't be opened to joins");
                return;
            }
            finish(
                MatchState::Hosting,
                m_direct ? "hosting for direct joins (developer)" : "hosting lobby " + std::to_string(m_lobby.value));
        } else {
            finish(MatchState::Joined, "playing as player " + std::to_string(session->localPlayer()));
        }
    }
    if (m_state == MatchState::Hosting && m_lobby.valid()) {
        const uint32_t players = session->stats().admitted + 1;
        if (players != m_advertisedPlayers) {
            (void)m_services->setLobbyMetadata(m_lobby, "sw_players", std::to_string(players));
            m_advertisedPlayers = players;
        }
    }
}

bool OnlineMatch::takeEnded() { return std::exchange(m_ended, false); }

}   // namespace shockwave::net
