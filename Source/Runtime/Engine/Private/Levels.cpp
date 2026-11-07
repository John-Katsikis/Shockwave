// Level components: see Engine/Levels.h and docs/Levels.md.
#include <Engine/Levels.h>

#include <Engine/Camera.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/Prefab.h>

#include <cstdio>
#include <random>

namespace shockwave {

std::string newPersistentId() {
    static thread_local std::mt19937_64 random{std::random_device{}() ^ uint64_t(std::random_device{}()) << 32};
    const uint64_t high = random(), low = random();
    uint8_t bytes[16];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = uint8_t(high >> (56 - 8 * i));
        bytes[8 + i] = uint8_t(low >> (56 - 8 * i));
    }
    bytes[6] = uint8_t((bytes[6] & 0x0f) | 0x40);   // version 4: random
    bytes[8] = uint8_t((bytes[8] & 0x3f) | 0x80);   // RFC 4122 variant
    char text[37];
    std::snprintf(
        text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1],
        bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12],
        bytes[13], bytes[14], bytes[15]);
    return text;
}

Entity findLevelSpawn(const World& constWorld, std::string_view id) {
    World& world = const_cast<World&>(constWorld);   // each() has no const form; this only reads
    Entity found = kNullEntity;
    world.each<const LevelSpawn>([&](Entity e, const LevelSpawn& spawn) {
        if (spawn.id == id && (found == kNullEntity || e.index < found.index)) found = e;
    });
    return found;
}

LevelArrival levelArrival(const World& world, const std::string& spawn) {
    LevelArrival arrival;
    arrival.entity = findLevelSpawn(world, spawn.empty() ? std::string_view("start") : std::string_view(spawn));
    if (arrival.entity != kNullEntity)
        arrival.spawn = spawn.empty() ? "start" : spawn;
    else
        arrival.spawn = spawn;
    if (arrival.entity != kNullEntity && world.get<Transform>(arrival.entity)) {
        arrival.found = true;
        arrival.transform = Transform::fromMatrix(worldMatrix(world, arrival.entity));
    }
    return arrival;
}

LevelSceneFacts levelSceneFacts(const World& constWorld) {
    // World::each has no const form; these visits only read.
    World& world = const_cast<World&>(constWorld);
    LevelSceneFacts facts;
    facts.loaded = true;
    auto nameOf = [&](Entity e) {
        const Name* name = world.get<Name>(e);
        return name && !name->value.empty() ? name->value : "#" + std::to_string(e.index);
    };
    world.each<const LevelSpawn>([&](Entity e, const LevelSpawn& s) { facts.spawns.push_back({s.id, nameOf(e)}); });
    world.each<const LevelExit>(
        [&](Entity e, const LevelExit& x) { facts.exits.push_back({x.destination, x.spawn, nameOf(e), x.enabled}); });
    world.each<const Checkpoint>(
        [&](Entity e, const Checkpoint& c) { facts.checkpoints.push_back({c.id, c.spawn, nameOf(e)}); });
    world.each<const PersistentIdentity>([&](Entity e, const PersistentIdentity& p) {
        LevelSceneFacts::Identity identity;
        identity.entity = nameOf(e);
        if (!validPersistentId(p.id)) {
            identity.problem = "PersistentIdentity \"" + p.id + "\" isn't a lowercase UUID";
            facts.identities.push_back(std::move(identity));
            return;
        }
        identity.id = p.id;
        // Inside a prefab copy: the copy's root (the nearest PrefabInstance that isn't
        // itself a part) gives the instance identity, so two copies don't collide.
        if (world.has<PrefabPart>(e)) {
            Entity root = parentOf(world, e);
            while (world.alive(root) && world.has<PrefabPart>(root)) root = parentOf(world, root);
            const PersistentIdentity* instance = world.alive(root) ? world.get<PersistentIdentity>(root) : nullptr;
            if (!instance || !validPersistentId(instance->id)) {
                identity.problem = "PersistentIdentity \"" + p.id + "\" is in a prefab copy ('" +
                                   (world.alive(root) ? nameOf(root) : std::string("?")) +
                                   "') that has no PersistentIdentity of its own";
                identity.id.clear();
            } else {
                identity.id = instance->id + "/" + p.id;
            }
        }
        facts.identities.push_back(std::move(identity));
    });
    world.each<const Camera>([&](const Camera& camera) { facts.primaryCameras += camera.primary; });
    if (const ComponentInfo* ui = findComponent("UIDocument"))
        world.forEachEntity([&](Entity e) { facts.hasUserInterface |= ui->has(world, e); });
    return facts;
}

}   // namespace shockwave
