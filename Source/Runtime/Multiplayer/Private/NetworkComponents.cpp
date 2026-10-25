#include <Multiplayer/NetworkComponents.h>

#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Network/Messages.h>
#include <Network/NetworkTypes.h>
#include <Network/Sha256.h>

#include <algorithm>
#include <array>
#include <unordered_set>
#include <vector>

namespace shockwave::net {

void registerNetworkComponents() {
    registerComponent<NetworkObject>(
        "NetworkObject",
        {
            field("key", &NetworkObject::key),
            field("prefab", &NetworkObject::prefab),
            field("profile", &NetworkObject::profile),
            enumField("mode", &NetworkObject::mode, {"Static", "Interpolated", "PredictedCharacter"}),
            field("always_relevant", &NetworkObject::alwaysRelevant),
        },
        "Multiplayer");
    registerComponent<NetworkSpawnPoint>(
        "NetworkSpawnPoint",
        {
            field("team", &NetworkSpawnPoint::team, 1.0f, 0.0f, 31.0f),
            field("order", &NetworkSpawnPoint::order, 1.0f, 0.0f, 4095.0f),
        },
        "Multiplayer");
}

bool validNetworkKey(std::string_view key) {
    if (key.empty() || key.size() > kMaxAuthoredKeyBytes) return false;
    for (const char c : key) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == ':' || c == '-';
        if (!ok) return false;
    }
    return true;
}

namespace {

std::string uuidText(std::array<uint8_t, 16> bytes) {
    bytes[6] = uint8_t((bytes[6] & 0x0F) | 0x40);   // version 4: random
    bytes[8] = uint8_t((bytes[8] & 0x3F) | 0x80);   // RFC 4122 variant
    static const char* digits = "0123456789abcdef";
    std::string key;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) key += '-';
        key += digits[bytes[i] >> 4];
        key += digits[bytes[i] & 15];
    }
    return key;
}

}   // namespace

std::string newNetworkKey() {
    std::array<uint8_t, 16> bytes{};
    if (!secureRandomBytes(bytes)) return {};
    return uuidText(bytes);
}

int renewNetworkKeys(World& world, std::span<const Entity> roots) {
    int changed = 0;
    std::vector<Entity> pending(roots.begin(), roots.end());
    while (!pending.empty()) {
        const Entity e = pending.back();
        pending.pop_back();
        if (!world.alive(e)) continue;
        if (NetworkObject* object = world.get<NetworkObject>(e)) {
            std::string key = newNetworkKey();
            if (!key.empty()) {
                object->key = std::move(key);
                ++changed;
            }
        }
        for (const Entity child : childrenOf(world, e)) pending.push_back(child);
    }
    return changed;
}

std::vector<NetworkKeyChange> assignNetworkKeys(World& world, const uint64_t* seed) {
    std::vector<Entity> entities;
    world.each<const NetworkObject>([&](Entity e, const NetworkObject&) { entities.push_back(e); });
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.index < b.index; });
    std::unordered_set<std::string> used;
    for (const Entity e : entities)
        if (const auto& key = world.get<NetworkObject>(e)->key; validNetworkKey(key)) used.insert(key);
    std::unordered_set<std::string> seen;
    uint64_t counter = 0;
    auto next = [&] {
        for (;;) {
            std::string key;
            if (!seed) {
                key = newNetworkKey();
            } else {
                uint8_t input[16];
                for (int i = 0; i < 8; ++i) input[i] = uint8_t(*seed >> (8 * i));
                for (int i = 0; i < 8; ++i) input[8 + i] = uint8_t(counter >> (8 * i));
                ++counter;
                const Sha256Digest digest = sha256(std::span<const uint8_t>(input, sizeof input));
                std::array<uint8_t, 16> bytes{};
                std::copy_n(digest.begin(), bytes.size(), bytes.begin());
                key = uuidText(bytes);
            }
            if (key.empty() || used.insert(key).second) return key;
        }
    };
    std::vector<NetworkKeyChange> changes;
    for (const Entity e : entities) {
        NetworkObject& object = *world.get<NetworkObject>(e);
        if (validNetworkKey(object.key) && seen.insert(object.key).second) continue;
        std::string key = next();
        if (key.empty()) break;   // no secure random source: change nothing more
        changes.push_back({e, object.key, key});
        seen.insert(key);
        object.key = std::move(key);
    }
    return changes;
}

bool assignMissingNetworkKey(World& world, Entity entity) {
    NetworkObject* object = world.get<NetworkObject>(entity);
    if (!object || !object->key.empty()) return false;
    object->key = newNetworkKey();
    return !object->key.empty();
}

}   // namespace shockwave::net
