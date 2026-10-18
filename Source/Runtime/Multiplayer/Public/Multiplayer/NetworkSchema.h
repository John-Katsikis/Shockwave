#pragma once

// Generated references and reports for tools: the network schema (docs/NetworkSchema.md,
// from `player --network-schema`) and `player --network-info`.

#include <Multiplayer/NetworkSettings.h>
#include <Multiplayer/NetworkValidation.h>
#include <Multiplayer/Replication.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave::net {

// What this build can do, for --network-info and the editor.
struct NetworkBuild {
    bool steamworks = false;   // a Steamworks SDK adapter is compiled in
    bool loopback = true;   // the in-process test transport
};
NetworkBuild networkBuild();

// docs/NetworkSchema.md: settings keys, components, manifest format, the registry's
// profiles, commands and events. NetworkObject and NetworkSpawnPoint must be registered.
std::string networkSchemaMarkdown(const ReplicationRegistry& registry, std::string_view gameName);

struct NetworkInfo {
    std::string project;   // the .swproject, as given
    NetworkProject network;
    std::vector<NetProblem> problems;
    std::optional<Digest> schemaDigest;
    std::optional<ContentDigest> content;
    uint64_t playerSnapshotBytesPerSecond = 0;
    const ReplicationRegistry* registry = nullptr;
};

// Everything in the report must already be computed; these only format it. IDs and the
// AppID are decimal strings in JSON, so 64-bit values survive JSON readers.
std::string networkInfoJson(const NetworkInfo& info);
std::string networkInfoText(const NetworkInfo& info);

// A JSON string literal, escaped.
std::string jsonQuote(std::string_view text);
// A u64 as a JSON string ("18446744073709551615"), never a number.
std::string jsonU64(uint64_t value);

}   // namespace shockwave::net
