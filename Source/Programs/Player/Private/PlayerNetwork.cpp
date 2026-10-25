// The player's network tools (docs/Networking.md): --check's network rules, --network-info,
// --network-schema, --network-manifest, --assign-network-keys and the capture tools
// (--net-dump, --net-encode), which never start Steam or open a socket; --steam-status,
// which starts Steam, reports and shuts it down; and online play: --host and the joins,
// and the game's own Host / Find / Invite menus through OnlineMatch.
#include "Player.h"

#include <Core/Log.h>
#include <Engine/Prefab.h>
#include <Engine/SceneFile.h>
#include <GameFramework/GameNetwork.h>
#include <Multiplayer/NetTest.h>
#include <Multiplayer/NetworkComponents.h>
#include <Multiplayer/NetworkSchema.h>
#include <Multiplayer/NetworkValidation.h>
#include <Navigation/NavMesh.h>
#include <Network/CaptureTransport.h>
#include <Network/Sha256.h>
#include <SteamServices/SteamServices.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace player {

namespace net = shockwave::net;

namespace {

void print(const net::NetProblem& problem, std::vector<std::string>& problems) {
    if (problem.warning)
        std::printf("warning %s\n", problem.text.c_str());
    else
        problems.push_back(problem.text);
}

bool writeText(const fs::path& out, const std::string& text) {
    if (out == "-") {
        std::fputs(text.c_str(), stdout);
        return true;
    }
    std::ofstream file(out, std::ios::binary);
    file << text;
    file.close();
    if (!file) {
        std::fprintf(stderr, "Player: can't write %s\n", out.string().c_str());
        return false;
    }
    std::printf("Wrote %s\n", out.string().c_str());
    return true;
}

bool readBytes(const fs::path& file, std::vector<uint8_t>& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

bool samePath(const fs::path& a, const fs::path& b) {
    std::error_code ec1, ec2;
    const fs::path x = fs::weakly_canonical(fs::absolute(a), ec1), y = fs::weakly_canonical(fs::absolute(b), ec2);
    return !ec1 && !ec2 && x == y;
}

}   // namespace

int writeNetworkSchema(const shockwave::GameModule& module, const fs::path& out) {
    net::registerNetworkComponents();
    net::ReplicationRegistry registry;
    std::vector<std::string> problems;
    if (!shockwave::buildReplicationRegistry(&module, registry, problems)) {
        for (const std::string& problem : problems) std::fprintf(stderr, "Player: %s\n", problem.c_str());
        return 1;
    }
    return writeText(out, net::networkSchemaMarkdown(registry, module.id)) ? 0 : 1;
}

int networkInfo(const shockwave::Project& project, const shockwave::GameModule& module, const fs::path& json) {
    net::NetworkInfo info;
    info.project = project.file.string();
    net::ReplicationRegistry registry;
    std::vector<std::string> registryProblems;
    const bool frozen = shockwave::buildReplicationRegistry(&module, registry, registryProblems);
    for (const std::string& problem : registryProblems) info.problems.push_back({false, problem});
    if (frozen) {
        info.registry = &registry;
        info.schemaDigest = registry.schemaDigest();
    }
    net::loadNetworkProject(project, info.network, info.problems);
    if (frozen) net::validateNetworkProject(project, info.network, registry, info.problems);
    if (info.network.manifest) {
        net::ContentDigest content;
        if (net::computeContentDigest(
                project, *info.network.manifest, info.network.manifestShown, content, info.problems))
            info.content = std::move(content);
    }
    if (frozen && info.network.present)
        info.playerSnapshotBytesPerSecond =
            net::estimatePlayerSnapshotBytesPerSecond(info.network.settings.settings, registry);
    if (json.empty())
        std::fputs(net::networkInfoText(info).c_str(), stdout);
    else if (!writeText(json, net::networkInfoJson(info)))
        return 1;
    return net::hasErrors(info.problems) ? 1 : 0;
}

int steamStatus(const shockwave::Project& project, bool server) {
    const net::SteamBuild build = net::steamBuild();
    std::printf("build steamworks %s%s\n", build.sdk ? "yes, SDK " : "no", build.sdk ? build.sdkVersion : "");
    const fs::path path = net::networkSettingsPath(project);
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        std::fprintf(
            stderr, "Player: %s doesn't exist, so the project has no Steam AppID\n", project.relative(path).c_str());
        return 1;
    }
    net::NetworkSettingsFile file;
    std::string error;
    if (!net::readNetworkSettings(path, file, error)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    const net::NetworkSettings& settings = file.settings;
    if (settings.steamAppId == 0) {
        std::fprintf(stderr, "Player: steam_app_id is 0 in %s\n", project.relative(path).c_str());
        return 1;
    }
    net::SteamServicesSettings steam;
    steam.appId = settings.steamAppId;
    steam.virtualPort = uint16_t(settings.virtualPort);
    steam.maxUnreliableBytes = settings.maxSnapshotBytes;

    std::unique_ptr<net::SteamServices> services = net::SteamServices::create();
    const net::NetResult<void> started =
        server ? services->initializeDedicatedServer(steam, {}) : services->initializeClient(steam);
    if (!started) {
        std::printf("steam unavailable: %s\n", started.error().message);
        if (*services->lastError()) std::printf("steam said: %s\n", services->lastError());
        return 1;
    }
    // Relay network access settles within a few seconds; the UI never waits like this.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        services->framePass();
        const net::SteamRelayStatus relay = services->relayStatus();
        if (services->available() &&
            (relay == net::SteamRelayStatus::Current || relay == net::SteamRelayStatus::Failed))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const bool signedIn = services->available();
    const net::SteamRelayStatus relay = services->relayStatus();
    std::printf(
        "steam %s, AppID %u, SteamID %llu, signed in %s, relay %s\n", net::steamServiceStateName(services->state()),
        settings.steamAppId, static_cast<unsigned long long>(services->localPeer().value), signedIn ? "yes" : "no",
        net::steamRelayStatusName(relay));
    services->shutdown();
    return signedIn && relay != net::SteamRelayStatus::Failed ? 0 : 1;
}

int netTest(const shockwave::Project& project, const NetTestCommand& command, const fs::path& json) {
    net::NetTestOptions options;
    options.scenario = command.scenario;
    options.seed = command.seed;
    options.preset = command.preset;
    options.clients = command.clients;
    // The project's settings when it has them: tick rate, player count and timeouts.
    const fs::path settingsFile = net::networkSettingsPath(project);
    if (std::error_code ec; fs::is_regular_file(settingsFile, ec)) {
        net::NetworkSettingsFile file;
        std::string error;
        if (!net::readNetworkSettings(settingsFile, file, error)) {
            std::fprintf(stderr, "Player: %s\n", error.c_str());
            return 1;
        }
        options.settings = file.settings;
    }
    net::NetTestReport report;
    if (!net::runNetTest(options, report)) {
        for (const std::string& e : report.errors) std::fprintf(stderr, "Player: %s\n", e.c_str());
        return 2;
    }
    std::fputs(net::netTestText(report).c_str(), stdout);
    if (!json.empty() && !writeText(json, net::netTestJson(report))) return 1;
    return report.ok ? 0 : 1;
}

int netDump(const fs::path& capture, const fs::path& json) {
    std::vector<uint8_t> bytes;
    if (!readBytes(capture, bytes)) {
        std::fprintf(stderr, "Player: can't read %s\n", capture.string().c_str());
        return 1;
    }
    std::string text, error;
    if (!net::captureToJson(bytes, text, error)) {
        std::fprintf(stderr, "Player: %s: %s\n", capture.string().c_str(), error.c_str());
        return 1;
    }
    return writeText(json.empty() ? fs::path("-") : json, text) ? 0 : 1;
}

int netEncode(const fs::path& json, const fs::path& output) {
    std::vector<uint8_t> text;
    if (!readBytes(json, text)) {
        std::fprintf(stderr, "Player: can't read %s\n", json.string().c_str());
        return 1;
    }
    std::vector<uint8_t> capture;
    std::string error;
    if (!net::captureFromJson(
            std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), capture, error)) {
        std::fprintf(stderr, "Player: %s: %s\n", json.string().c_str(), error.c_str());
        return 1;
    }
    std::ofstream out(output, std::ios::binary);
    out.write(reinterpret_cast<const char*>(capture.data()), std::streamsize(capture.size()));
    out.close();
    if (!out) {
        std::fprintf(stderr, "Player: can't write %s\n", output.string().c_str());
        return 1;
    }
    std::printf("Wrote %s (%zu bytes)\n", output.string().c_str(), capture.size());
    return 0;
}

bool relaunchThroughSteam(const shockwave::Project& project) {
    net::NetworkSettingsFile file;
    std::string error;
    const fs::path path = net::networkSettingsPath(project);
    if (std::error_code ec; !fs::is_regular_file(path, ec) || !net::readNetworkSettings(path, file, error))
        return false;
    const net::NetworkSettings& s = file.settings;
    if (!s.enabled || s.transport != net::NetTransportKind::Steam || s.steamAppId == 0) return false;
    // A development package (AppID 480, Valve's shared test app) runs as it was started:
    // Steam would start Spacewar instead.
    if (s.steamAppId == 480) return false;
    if (!net::SteamServices::create()->restartThroughSteamIfNeeded(s.steamAppId)) return false;
    std::puts("Steam is starting the game");
    return true;
}

void Player::checkNetworkProject(std::vector<std::string>& problems) {
    m_networkRegistry = std::make_unique<net::ReplicationRegistry>();
    std::vector<std::string> registryProblems;
    if (!shockwave::buildReplicationRegistry(&m_module, *m_networkRegistry, registryProblems)) {
        for (const std::string& problem : registryProblems)
            problems.push_back(m_project.relative(m_project.file) + ": game module network types: " + problem);
        m_networkRegistry.reset();
        return;
    }
    net::NetworkProject network;
    std::vector<net::NetProblem> found;
    net::loadNetworkProject(m_project, network, found);
    net::validateNetworkProject(m_project, network, *m_networkRegistry, found);
    if (network.manifest) {
        // Each manifest prefab's root is the network object copies of it spawn as.
        for (const net::ManifestPrefab& entry : network.manifest->prefabs) {
            std::string error;
            const auto prefab = shockwave::prefabs().load(entry.path, engine().assets(), &error);
            if (!prefab) continue;   // the manifest check already reports a missing file
            const std::string shown = m_project.relative(m_project.assetFolder / entry.path);
            const auto* object = prefab->world.get<net::NetworkObject>(prefab->root);
            if (!object)
                found.push_back({false, shown + ": the manifest spawns it, but its root has no NetworkObject"});
            else if (object->profile != entry.profile)
                found.push_back(
                    {false, shown + ": its root's profile is \"" + object->profile + "\" but the manifest says \"" +
                                entry.profile + "\" (" + network.manifestShown + ":" + std::to_string(entry.line) +
                                ")"});
            net::validateNetworkWorld(prefab->world, *m_networkRegistry, shown, true, found);
            m_predictedSpawns = m_predictedSpawns || net::predictedPrefab(prefab->world, prefab->root);
        }
        net::ContentDigest content;
        net::computeContentDigest(m_project, *network.manifest, network.manifestShown, content, found);
    }
    const size_t before = problems.size();
    for (const net::NetProblem& problem : found) print(problem, problems);
    if (network.present && problems.size() == before) std::printf("ok %s\n", network.settingsShown.c_str());
}

void Player::checkNetworkScene(
    const shockwave::World& world, const std::string& scene, std::vector<std::string>& problems) {
    if (!m_networkRegistry) return;
    std::vector<net::NetProblem> found;
    net::validateNetworkWorld(world, *m_networkRegistry, scene, false, found);
    net::validatePredictionWorld(world, scene, m_predictedSpawns, found);
    for (const net::NetProblem& problem : found) print(problem, problems);
}

void Player::startNetPreview() {
    const net::FaultPreset* preset = net::findFaultPreset(m_tool.netPreview);
    if (!preset) throw std::runtime_error("--net-preview takes clean, cafe, poor or hostile");
    m_previewRegistry = std::make_unique<net::ReplicationRegistry>();
    std::vector<std::string> registryProblems;
    if (!shockwave::buildReplicationRegistry(&m_module, *m_previewRegistry, registryProblems))
        throw std::runtime_error("--net-preview: the game's network types: " + registryProblems.front());
    net::NetworkProject network;
    std::vector<net::NetProblem> problems;
    net::loadNetworkProject(m_project, network, problems);
    const std::string scene = m_project.relative(m_project.startupScene);
    uint16_t sceneId = 0;
    if (network.manifest)
        for (const net::ManifestScene& s : network.manifest->scenes)
            if (s.path == scene) sceneId = s.id;
    if (!network.manifest || sceneId == 0)
        throw std::runtime_error("--net-preview: " + scene + " isn't a scene in the network manifest");
    network.settings.settings.enabled = true;   // a preview on this machine needs no published multiplayer
    network.settings.settings.transport = net::NetTransportKind::Loopback;
    if (!net::readyForSession(m_project, network, *m_previewRegistry, sceneId, problems))
        throw std::runtime_error("--net-preview: " + problems.front().text);
    net::NetworkStart start;
    start.settings = network.settings.settings;
    start.compatibility.gameProtocol = network.manifest->gameProtocol;
    start.compatibility.schemaDigest = m_previewRegistry->schemaDigest();
    start.compatibility.sceneId = sceneId;   // one process: the content digest matches by construction
    start.address = {net::TransportAddress::Loopback, 27015, 0};
    start.registry = m_previewRegistry.get();
    for (const net::ManifestPrefab& p : network.manifest->prefabs) start.prefabs.push_back({p.id, p.path, p.profile});
    net::LoopbackHubConfig hub;
    hub.faults = preset->profile;
    m_previewHub = std::make_unique<net::LoopbackHub>(hub);
    m_previewHostServices = std::make_unique<net::LoopbackOnlineServices>(*m_previewHub, net::PeerId{1});
    m_previewClientServices = std::make_unique<net::LoopbackOnlineServices>(*m_previewHub, net::PeerId{2});
    // The host plays its own copy of the scene with no window, sound or devices.
    m_previewWorld = std::make_unique<shockwave::World>(m_world);
    m_previewHost = std::make_unique<shockwave::PlaySession>(*m_previewWorld, engine().jobs(), *m_instance);
    m_previewHost->setAudioDevice(false);
    start.role = net::NetworkRole::Authority;
    m_previewHost->setNetworkStart(start, m_previewHostServices.get());
    m_previewHost->start();
    start.role = net::NetworkRole::Client;
    start.capture = m_capture.get();   // --net-capture records the client's side
    m_session->setNetworkStart(start, m_previewClientServices.get());
    const net::FaultProfile& f = preset->profile;
    std::printf(
        "net preview: playing %s as a client of a host in this process, %s network (%.0f ms one way, %.0f ms "
        "jitter, %.0f%% loss)\n",
        scene.c_str(), preset->name, f.latencyUs / 1000.0, f.jitterUs / 1000.0, f.unreliableLoss * 100.0);
}

// ---- --network-manifest and --assign-network-keys ----------------------------------------

int Player::networkManifest() {
    net::registerNetworkComponents();
    net::ReplicationRegistry registry;
    std::vector<std::string> registryProblems;
    if (!shockwave::buildReplicationRegistry(&m_module, registry, registryProblems)) {
        for (const std::string& p : registryProblems) std::fprintf(stderr, "Player: %s\n", p.c_str());
        return 1;
    }
    net::NetworkProject network;
    std::vector<net::NetProblem> problems;
    net::loadNetworkProject(m_project, network, problems);
    const fs::path manifestFile = m_project.root / network.settings.settings.manifest;
    if (samePath(m_tool.output, manifestFile)) {
        std::fprintf(stderr, "Player: --output is the manifest itself; write a candidate elsewhere and compare\n");
        return 2;
    }
    net::NetworkManifest candidate = network.manifest.value_or(net::NetworkManifest{});
    if (candidate.gameProtocol.empty()) candidate.gameProtocol = m_project.gameModule + "-1";
    auto nextId = [](const auto& entries) {
        uint16_t id = 1;
        for (const auto& e : entries) id = std::max<uint16_t>(id, uint16_t(e.id + 1));
        return id;
    };
    std::vector<std::string> added;
    // Prefabs in Content whose root is a network object.
    std::error_code ec;
    std::vector<fs::path> prefabFiles;
    for (const auto& entry : fs::recursive_directory_iterator(m_project.assetFolder, ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".swprefab") prefabFiles.push_back(entry.path());
    std::sort(prefabFiles.begin(), prefabFiles.end());
    std::set<std::string> networkPrefabs;
    for (const fs::path& file : prefabFiles) {
        const std::string path = fs::relative(file, m_project.assetFolder).generic_string();
        std::string error;
        const auto prefab = shockwave::prefabs().load(path, engine().assets(), &error);
        if (!prefab) continue;
        const auto* object = prefab->world.get<net::NetworkObject>(prefab->root);
        if (!object) continue;
        networkPrefabs.insert(path);
        if (std::none_of(
                candidate.prefabs.begin(), candidate.prefabs.end(), [&](const auto& p) { return p.path == path; })) {
            net::ManifestPrefab entry;
            entry.id = nextId(candidate.prefabs);
            entry.path = path;
            entry.profile = object->profile;
            candidate.prefabs.push_back(entry);
            added.push_back("prefab " + std::to_string(entry.id) + " \"" + path + "\" (its root is a network object)");
        }
    }
    // Scenes with network objects or spawn points; and what their prefab copies need.
    for (const fs::path& file : projectScenes()) {
        const std::string path = m_project.relative(file);
        shockwave::World world;
        std::string error;
        if (!shockwave::loadScene(world, engine().assets(), file.string(), error)) {
            problems.push_back({false, error});
            continue;
        }
        bool networked = false;
        world.each<const net::NetworkObject>([&](shockwave::Entity, const net::NetworkObject&) { networked = true; });
        world.each<const net::NetworkSpawnPoint>(
            [&](shockwave::Entity, const net::NetworkSpawnPoint&) { networked = true; });
        const bool listed = std::any_of(
            candidate.scenes.begin(), candidate.scenes.end(), [&](const auto& s) { return s.path == path; });
        if (networked && !listed) {
            net::ManifestScene entry;
            entry.id = nextId(candidate.scenes);
            entry.path = path;
            candidate.scenes.push_back(entry);
            added.push_back("scene " + std::to_string(entry.id) + " \"" + path + "\" (it has network objects)");
        }
    }
    net::validateNetworkManifest(candidate, m_project, &registry, "the candidate manifest", problems);
    net::ContentDigest content;
    const bool digested = net::computeContentDigest(m_project, candidate, network.manifestShown, content, problems);
    std::string error;
    if (!net::writeNetworkManifest(m_tool.output, candidate, error)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    std::printf(
        "manifest %s: %zu scenes, %zu prefabs\n", network.manifest ? network.manifestShown.c_str() : "(none yet)",
        candidate.scenes.size(), candidate.prefabs.size());
    for (const std::string& line : added) std::printf("candidate adds %s\n", line.c_str());
    if (added.empty()) std::puts("candidate adds nothing: every network scene and prefab is listed");
    std::printf("schema digest %s\n", net::toHex(registry.schemaDigest()).c_str());
    if (digested)
        std::printf(
            "content digest %s (%zu files, %llu bytes)\n", net::toHex(content.digest).c_str(), content.files.size(),
            static_cast<unsigned long long>(content.bytes));
    int errors = 0;
    for (const net::NetProblem& p : problems) {
        std::printf("%s %s\n", p.warning ? "warning" : "error", p.text.c_str());
        errors += !p.warning;
    }
    std::printf("Wrote %s\n", m_tool.output.string().c_str());
    return errors ? 1 : 0;
}

int Player::assignNetworkKeysTool() {
    if (samePath(m_tool.output, m_tool.scene)) {
        std::fprintf(stderr, "Player: --output is the scene itself; write a new file and compare\n");
        return 2;
    }
    net::registerNetworkComponents();
    shockwave::World world;
    std::string error;
    std::vector<std::string> header;   // top-level lines kept as they are (editor_camera, say)
    shockwave::SceneFileHooks hooks;
    hooks.readHeader = [&](const std::string& key, std::istream& in) {
        std::string rest;
        std::getline(in, rest);
        header.push_back(key + rest);
    };
    hooks.writeHeader = [&](std::ostream& out) {
        for (const std::string& line : header) out << line << '\n';
    };
    if (!shockwave::loadScene(world, engine().assets(), m_tool.scene.string(), error, hooks)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    const std::vector<net::NetworkKeyChange> changes =
        net::assignNetworkKeys(world, m_tool.keySeedGiven ? &m_tool.keySeed : nullptr);
    for (const net::NetworkKeyChange& c : changes) {
        const auto* name = world.get<shockwave::Name>(c.entity);
        std::printf(
            "entity '%s': %s -> %s\n", name ? name->value.c_str() : "?",
            c.before.empty() ? "(no key)" : ("\"" + c.before + "\"").c_str(), c.after.c_str());
    }
    size_t objects = 0;
    world.each<const net::NetworkObject>([&](shockwave::Entity, const net::NetworkObject&) { ++objects; });
    if (!shockwave::saveScene(world, engine().assets(), m_tool.output.string(), error, hooks)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    std::printf(
        "%zu of %zu network objects got new keys; wrote %s\n", changes.size(), objects, m_tool.output.string().c_str());
    return 0;
}

// ---- Online play -------------------------------------------------------------------------

net::IOnlineServices* Player::startSteam(std::string& error) {
    if (m_steam && m_steam->state() != net::SteamServiceState::Off) return m_steam.get();
    const net::NetworkSettings& settings = m_match.setup().settings;
    if (!net::steamBuild().sdk) {
        error = "this build has no Steamworks SDK (configure with -DSHOCKWAVE_STEAMWORKS=ON)";
        return nullptr;
    }
    if (settings.steamAppId == 0) {
        error = "steam_app_id is 0 in Config/Network.swnetwork";
        return nullptr;
    }
    net::SteamServicesSettings steam;
    steam.appId = settings.steamAppId;
    steam.virtualPort = uint16_t(settings.virtualPort);
    steam.maxUnreliableBytes = settings.maxSnapshotBytes;
    m_steam = net::SteamServices::create();
    if (const auto started = m_steam->initializeClient(steam); !started) {
        error = std::string("Steam didn't start: ") + started.error().message;
        if (*m_steam->lastError()) error += std::string(" (") + m_steam->lastError() + ")";
        m_steam.reset();
        return nullptr;
    }
    return m_steam.get();
}

void Player::setUpOnline() {
    m_offlineScene = m_project.startupScene;
    const bool asked = m_tool.online != Tool::Online::None || m_tool.connectLobby;
    m_onlineFromCommandLine = m_tool.online != Tool::Online::None;
    if (!m_tool.netCapture.empty()) {
        m_capture = std::make_unique<net::CaptureWriter>();
        if (const auto opened = m_capture->open(m_tool.netCapture); !opened)
            throw std::runtime_error("--net-capture: " + std::string(opened.error().message));
        std::printf(
            "capturing network messages to %s (session tickets redacted)\n", m_tool.netCapture.string().c_str());
    }
    if (!m_tool.netPreview.empty()) return;   // the preview's loopback session is its own
    // A project with a network scene offers online play to its menus; without one only a
    // command-line request says why not.
    net::NetworkProject network;
    std::vector<net::NetProblem> ignored;
    net::loadNetworkProject(m_project, network, ignored);
    if (!network.present && !asked) return;
    m_matchRegistry = std::make_unique<net::ReplicationRegistry>();
    std::vector<std::string> problems;
    net::MatchSetup setup;
    const std::string scene = m_tool.scene.empty() || !asked ? std::string() : m_project.relative(m_tool.scene);
    if (!shockwave::buildReplicationRegistry(&m_module, *m_matchRegistry, problems) ||
        !net::prepareMatchSetup(m_project, m_project.gameModule, *m_matchRegistry, scene, setup, problems)) {
        if (asked) throw std::runtime_error("can't play online: " + problems.front());
        SW_LOG(shockwave::LogNetwork, Info, "Online play is off: {}", problems.front());
        return;
    }
    m_match.setSetup(std::move(setup));
    m_match.setServiceProvider([this](std::string& error) { return startSteam(error); });
    // A packaged game that requires Steam starts it now, and stops when it can't; one that
    // doesn't plays offline and starts Steam when someone asks to play online.
    if (!packagedProjectFile().empty() && network.settings.settings.steamRequired) {
        std::string error;
        if (!startSteam(error)) throw std::runtime_error("this game needs Steam: " + error);
    }
    bool requested = true;
    switch (m_tool.online) {
    case Tool::Online::Host: requested = m_match.host(); break;
    case Tool::Online::HostDirect: requested = m_match.hostDirect(); break;
    case Tool::Online::JoinLobby: requested = m_match.join(net::LobbyId{m_tool.onlineId}); break;
    case Tool::Online::JoinDirect: requested = m_match.joinDirect(net::PeerId{m_tool.onlineId}); break;
    case Tool::Online::None:
        if (m_tool.connectLobby) m_match.requestJoin(net::LobbyId{m_tool.connectLobby});
        break;
    }
    if (!requested) throw std::runtime_error(m_match.error());
}

void Player::playScene(const fs::path& scene, const net::NetworkStart* start) {
    if (m_session) {
        m_session->stop();
        m_session.reset();
    }
    std::string error;
    std::vector<std::string> warnings;
    shockwave::World world;
    if (!shockwave::loadScene(world, engine().assets(), scene.string(), error, {}, &warnings))
        throw std::runtime_error(error);
    if (!warnings.empty()) throw std::runtime_error("Scene has unresolved content: " + warnings.front());
    m_world = std::move(world);
    if (shockwave::loadSceneNavMesh(scene, error); !error.empty())
        SW_LOG(shockwave::LogNavigation, Warning, "{}; baking instead", error);
    m_session = std::make_unique<shockwave::PlaySession>(m_world, engine().jobs(), *m_instance, &engine());
    m_session->setSaveFolder(saveFolder(), m_project.relative(scene));
    m_session->setGameCircuit(m_project.gameCircuitPath());
    m_session->setOnlineMatch(m_match.configured() ? &m_match : nullptr);
    if (start) {
        net::NetworkStart online = *start;
        online.capture = m_capture.get();
        m_session->setNetworkStart(std::move(online), services());
    }
    m_session->start();
    SW_LOG(
        shockwave::LogEngine, Info, "playing {}{}", m_project.relative(scene),
        start ? (start->role == net::NetworkRole::Authority ? " as host" : " as a client") : "");
}

void Player::updateOnline() {
    if (!m_match.configured()) return;
    auto backOffline = [&](const std::string& why) {
        if (m_onlineFromCommandLine) {
            // A command-line match that ends is the end of the run.
            if (!why.empty()) {
                std::fprintf(stderr, "Player: %s\n", why.c_str());
                m_exitCode = 1;
            }
            engine().quit();
            return;
        }
        playScene(m_offlineScene, nullptr);   // the game's own menus say what happened
    };
    if (m_match.takeEnded()) backOffline(m_match.state() == net::MatchState::Failed ? m_match.error() : "");
    net::NetworkStart start;
    if (m_match.takeStart(start)) {
        try {
            playScene(m_project.root / m_match.setup().scenePath, &start);
        } catch (const std::exception& e) {
            m_match.leave();
            (void)m_match.takeEnded();
            const std::string why = std::string("the match couldn't start: ") + e.what();
            SW_LOG(shockwave::LogNetwork, Warning, "{}", why);
            backOffline(why);
        }
    } else if (m_onlineFromCommandLine && m_match.state() == net::MatchState::Failed && !m_session) {
        backOffline(m_match.error());   // failed before Play started (a refused join, say)
    }
}

}   // namespace player
