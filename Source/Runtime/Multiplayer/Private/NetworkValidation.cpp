#include <Multiplayer/NetworkValidation.h>

#include <Multiplayer/NetworkComponents.h>
#include <Multiplayer/NetworkProtocol.h>

#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/PhysicsComponents.h>
#include <Engine/Prefab.h>
#include <Projects/Project.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace shockwave::net {

namespace fs = std::filesystem;

bool loadNetworkProject(const Project& project, NetworkProject& out, std::vector<NetProblem>& problems) {
    out = {};
    const fs::path file = networkSettingsPath(project);
    out.settingsShown = project.relative(file);
    std::error_code ec;
    if (!fs::exists(file, ec)) return true;   // offline only
    out.present = true;
    std::string error;
    if (!readNetworkSettings(file, out.settings, error)) {
        if (error.rfind(file.string(), 0) == 0) error.replace(0, file.string().size(), out.settingsShown);
        problems.push_back({false, error});
        return false;
    }
    const std::string& manifest = out.settings.settings.manifest;
    out.manifestShown = manifest;
    const fs::path manifestFile = project.root / manifest;
    if (!fs::exists(manifestFile, ec)) {
        // Only a problem once networking is on: an offline project may not have one yet.
        if (out.settings.settings.enabled)
            problems.push_back(
                {false, out.settingsShown + ":" + std::to_string(out.settings.line("manifest")) + ": manifest " +
                            manifest + " doesn't exist"});
        return true;
    }
    NetworkManifest parsed;
    if (!readNetworkManifest(manifestFile, parsed, error)) {
        if (error.rfind(manifestFile.string(), 0) == 0) error.replace(0, manifestFile.string().size(), manifest);
        problems.push_back({false, error});
        return true;
    }
    out.manifest = std::move(parsed);
    return true;
}

uint64_t estimatePlayerSnapshotBytesPerSecond(const NetworkSettings& settings, const ReplicationRegistry& registry) {
    uint32_t record = 0;
    for (const ReplicationProfile& profile : registry.profiles())
        if (profile.perPlayer) record = std::max<uint32_t>(record, kStateRecordHeaderBytes + profile.encodedBytes);
    if (record == 0 || settings.maxSnapshotBytes <= kSnapshotOverheadBytes) return 0;
    const uint32_t perPart = std::max<uint32_t>(1, (settings.maxSnapshotBytes - kSnapshotOverheadBytes) / record);
    const uint64_t parts = (settings.maxPlayers + perPart - 1) / perPart;
    const uint64_t perSnapshot = uint64_t(settings.maxPlayers) * record + parts * kSnapshotOverheadBytes;
    return perSnapshot * settings.snapshotRate;
}

void validateNetworkProject(
    const Project& project, const NetworkProject& network, const ReplicationRegistry& registry,
    std::vector<NetProblem>& problems) {
    if (!network.present) return;
    const NetworkSettings& s = network.settings.settings;
    validateNetworkSettings(s, project.tickRate, network.settingsShown, &network.settings, problems);
    if (!registry.frozen()) {
        problems.push_back({false, network.settingsShown + ": the replication registry isn't frozen"});
        return;
    }
    const uint64_t estimate = estimatePlayerSnapshotBytesPerSecond(s, registry);
    const uint64_t budget = s.outboundBytesPerSecond > kReservedControlBytesPerSecond
                                ? s.outboundBytesPerSecond - kReservedControlBytesPerSecond
                                : 0;
    if (estimate > budget) {
        int line = network.settings.line("outbound_bytes_per_second");
        if (!line) line = network.settings.line("max_players");
        problems.push_back(
            {false,
             network.settingsShown + (line ? ":" + std::to_string(line) : "") + ": " + std::to_string(s.maxPlayers) +
                 " players at snapshot_rate " + std::to_string(s.snapshotRate) + " need about " +
                 std::to_string(estimate) + " bytes/s per peer for player state, but outbound_bytes_per_second " +
                 std::to_string(s.outboundBytesPerSecond) + " leaves " + std::to_string(budget) + " after " +
                 std::to_string(kReservedControlBytesPerSecond) + " for events; raise it to at least " +
                 std::to_string(estimate + kReservedControlBytesPerSecond) + " or lower snapshot_rate or max_players"});
    }
    if (network.manifest) {
        validateNetworkManifest(*network.manifest, project, &registry, network.manifestShown, problems);
        if (!s.matchScene.empty() && std::none_of(
                                         network.manifest->scenes.begin(), network.manifest->scenes.end(),
                                         [&](const ManifestScene& scene) { return scene.path == s.matchScene; })) {
            const int line = network.settings.line("match_scene");
            problems.push_back(
                {false, network.settingsShown + (line ? ":" + std::to_string(line) : "") + ": match_scene \"" +
                            s.matchScene + "\" isn't a scene in " + network.manifestShown});
        }
    }
}

void validateNetworkWorld(
    const World& constWorld, const ReplicationRegistry& registry, std::string_view shown, bool prefab,
    std::vector<NetProblem>& problems) {
    // each() only reads with const component types.
    World& world = const_cast<World&>(constWorld);
    auto name = [&](Entity e) {
        const Name* n = world.get<Name>(e);
        return std::string(n ? n->value : "?");
    };
    auto error = [&](Entity e, const std::string& message) {
        problems.push_back({false, std::string(shown) + ": entity '" + name(e) + "': " + message});
    };
    std::unordered_map<std::string, Entity> keys;
    world.each<const NetworkObject>([&](Entity e, const NetworkObject& object) {
        const bool part = world.has<PrefabPart>(e);
        if (part) {
            // Parts come from a prefab; the prefab's own check reports what's wrong in it.
            if (prefab) return;
            error(e, "a prefab part can't be a NetworkObject in v1; replicate it through its root");
            return;
        }
        if (object.key.empty()) {
            if (!prefab) error(e, "NetworkObject has no key; give it a unique one (the editor makes one)");
        } else if (!validNetworkKey(object.key)) {
            error(e, "NetworkObject key \"" + object.key + "\" must be 1..96 letters, digits and . _ : -");
        } else if (auto [it, added] = keys.emplace(object.key, e); !added) {
            error(e, "NetworkObject key \"" + object.key + "\" is also used by '" + name(it->second) + "'");
        }
        const ReplicationProfile* profile = registry.profile(object.profile);
        if (object.profile.empty())
            error(e, "NetworkObject has no profile");
        else if (!profile)
            error(e, "NetworkObject profile \"" + object.profile + "\" isn't registered");
        else
            for (const std::string& component : profile->requiredComponents) {
                const ComponentInfo* info = findComponent(component);
                if (!info || !info->has(world, e))
                    error(e, "profile " + profile->name + " needs a " + component + " component");
            }
        if (!object.alwaysRelevant) error(e, "always_relevant must be 1 in v1 (interest management comes later)");
        if (parentOf(world, e).valid()) error(e, "a NetworkObject must be a root (no Parent) in v1");
        if (const Transform* t = world.get<Transform>(e);
            t && (std::fabs(t->scale.x - 1.0f) > 1e-4f || std::fabs(t->scale.y - 1.0f) > 1e-4f ||
                  std::fabs(t->scale.z - 1.0f) > 1e-4f))
            error(e, "a NetworkObject's Transform must have scale 1 1 1 in v1; scale its children instead");
        if (object.mode == NetworkObject::Mode::PredictedCharacter) {
            if (!world.has<CharacterController>(e)) error(e, "PredictedCharacter needs a CharacterController");
            if (world.has<RigidBody>(e) || world.has<Collider>(e))
                error(
                    e,
                    "PredictedCharacter can't also have a RigidBody or Collider; the CharacterController is its body");
        }
        if (object.mode == NetworkObject::Mode::Static && world.has<RigidBody>(e))
            if (const RigidBody* body = world.get<RigidBody>(e); body->motion != RigidBody::Motion::Static)
                error(e, "a Static NetworkObject can't have a moving RigidBody");
        // Network children are only allowed under a prefab root as parts; anything else
        // under a network root would need its own replication.
        for (const Entity child : childrenOf(world, e))
            if (world.has<NetworkObject>(child) && !world.has<PrefabPart>(child))
                error(child, "a NetworkObject can't be under another one in v1");
    });
    // Moving bodies other peers would see need replication; without it each peer
    // simulates its own copy and they drift apart.
    if (!keys.empty())
        world.each<const RigidBody>([&](Entity e, const RigidBody& body) {
            if (body.motion == RigidBody::Motion::Static || body.sensor || world.has<NetworkObject>(e)) return;
            if (world.has<PrefabPart>(e) && prefab) return;
            error(e, "a moving RigidBody in a network scene needs a NetworkObject (profile engine.rigidbody.v1)");
        });
    std::vector<std::pair<std::pair<int, int>, Entity>> spawns;
    world.each<const NetworkSpawnPoint>([&](Entity e, const NetworkSpawnPoint& spawn) {
        if (spawn.team < 0 || spawn.team > 31) error(e, "NetworkSpawnPoint team must be 0..31");
        if (spawn.order < 0) error(e, "NetworkSpawnPoint order can't be negative");
        if (!world.has<Transform>(e)) error(e, "NetworkSpawnPoint needs a Transform to place players");
        for (const auto& [teamOrder, other] : spawns)
            if (teamOrder == std::pair(spawn.team, spawn.order))
                error(
                    e, "NetworkSpawnPoint team " + std::to_string(spawn.team) + " order " +
                           std::to_string(spawn.order) + " is also '" + name(other) + "'");
        spawns.push_back({{spawn.team, spawn.order}, e});
    });
}

void validatePredictionWorld(
    const World& constWorld, std::string_view shown, bool predictedSpawns, std::vector<NetProblem>& problems) {
    World& world = const_cast<World&>(constWorld);   // each() only reads with const component types
    // Only network scenes (with network objects or spawn points) have players in them.
    bool predicted = false, network = world.count<NetworkSpawnPoint>() > 0;
    world.each<const NetworkObject>([&](Entity, const NetworkObject& object) {
        network = true;
        predicted = predicted || object.mode == NetworkObject::Mode::PredictedCharacter;
    });
    if (!predicted && !(network && predictedSpawns)) return;
    world.each<const RigidBody>([&](Entity e, const RigidBody& body) {
        if (body.motion == RigidBody::Motion::Static || body.sensor || !body.enabled) return;
        const Name* n = world.get<Name>(e);
        const std::string where = std::string(shown) + ": entity '" + (n ? n->value : "?") + "': ";
        if (body.motion == RigidBody::Motion::Kinematic)
            problems.push_back(
                {false, where + "a moving kinematic body can't share a scene with predicted characters in v1: they "
                                "collide with static bodies only, so they'd fall through or be left behind by it"});
        else
            problems.push_back(
                {true, where + "predicted characters collide with static bodies only, so they pass through this "
                               "dynamic body (it can still hit them through game code)"});
    });
}

bool predictedPrefab(const World& prefabWorld, Entity root) {
    const NetworkObject* object = prefabWorld.alive(root) ? prefabWorld.get<NetworkObject>(root) : nullptr;
    return object && object->mode == NetworkObject::Mode::PredictedCharacter;
}

bool readyForSession(
    const Project& project, const NetworkProject& network, const ReplicationRegistry& registry, uint16_t sceneId,
    std::vector<NetProblem>& problems) {
    const size_t before = problems.size();
    if (!network.present) {
        problems.push_back(
            {false, network.settingsShown + ": there are no network settings; the game is offline only"});
        return false;
    }
    if (!network.settings.settings.enabled)
        problems.push_back({false, network.settingsShown + ": multiplayer isn't enabled (enabled 1)"});
    if (!registry.frozen())
        problems.push_back({false, "the replication registry must be frozen before a session starts"});
    std::vector<NetProblem> found;
    if (registry.frozen()) validateNetworkProject(project, network, registry, found);
    for (NetProblem& p : found)
        if (!p.warning) problems.push_back(std::move(p));
    if (!network.manifest)
        problems.push_back({false, network.settingsShown + ": the network manifest is missing or unreadable"});
    else if (!network.manifest->scene(sceneId))
        problems.push_back(
            {false, network.manifestShown + ": scene ID " + std::to_string(sceneId) + " isn't in the manifest"});
    return problems.size() == before;
}

}   // namespace shockwave::net
