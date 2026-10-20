// The player's network tools (docs/Networking.md): --check's network rules, --network-info
// and --network-schema, which never start Steam or open a socket, and --steam-status, the
// one that does: it starts Steam, reports, and shuts it down.
#include "Player.h"

#include <Engine/Prefab.h>
#include <GameFramework/GameNetwork.h>
#include <Multiplayer/NetTest.h>
#include <Multiplayer/NetworkComponents.h>
#include <Multiplayer/NetworkSchema.h>
#include <Multiplayer/NetworkValidation.h>
#include <SteamServices/SteamServices.h>

#include <chrono>
#include <stdexcept>
#include <cstdio>
#include <fstream>
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
    m_session->setNetworkStart(start, m_previewClientServices.get());
    const net::FaultProfile& f = preset->profile;
    std::printf(
        "net preview: playing %s as a client of a host in this process, %s network (%.0f ms one way, %.0f ms "
        "jitter, %.0f%% loss)\n",
        scene.c_str(), preset->name, f.latencyUs / 1000.0, f.jitterUs / 1000.0, f.unreliableLoss * 100.0);
}

}   // namespace player
