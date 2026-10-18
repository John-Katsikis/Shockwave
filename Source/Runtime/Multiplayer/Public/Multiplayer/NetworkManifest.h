#pragma once

// The network manifest (Config/NetworkManifest.swnetmanifest): the scenes a session can
// play and the prefabs it can spawn, each with a stable explicit ID, and the
// game_protocol name that C++ gameplay changes bump.
//
//   shockwave_network_manifest 1
//   game_protocol "arena.v1"
//   scene 1 "Scenes/arena-mp.swscene"
//   prefab 1 "Prefabs/Net Player.swprefab" "arena.player.v1"
//
// Scene paths are relative to the project folder; prefab paths are inside Content, the
// way Prefab components name them. IDs are 1..65535 and never come from file order. A
// peer only ever names a scene or prefab by ID, never by path.
//
// Two compatibility digests decide whether builds may play together: the schema digest
// (ReplicationRegistry::schemaDigest) and the content digest below.

#include <Multiplayer/NetworkSettings.h>

#include <Network/Messages.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {
struct Project;
}

namespace shockwave::net {

class ReplicationRegistry;

struct ManifestScene {
    uint16_t id = 0;
    std::string path;   // project-relative
    int line = 0;   // where it was in the file, 0 if not read from one
};

struct ManifestPrefab {
    uint16_t id = 0;
    std::string path;   // inside Content
    std::string profile;   // the root NetworkObject's replication profile
    int line = 0;
};

struct NetworkManifest {
    std::string gameProtocol;
    int gameProtocolLine = 0;
    std::vector<ManifestScene> scenes;
    std::vector<ManifestPrefab> prefabs;
    const ManifestScene* scene(uint16_t id) const;
    const ManifestPrefab* prefab(uint16_t id) const;
};

// Syntax only (the first error, as "<shown>:<line>: ..."); the rest is
// validateNetworkManifest's. False leaves out unchanged.
bool parseNetworkManifest(std::string_view text, std::string_view shown, NetworkManifest& out, std::string& error);
bool readNetworkManifest(const std::filesystem::path& file, NetworkManifest& out, std::string& error);
// Scenes then prefabs, each sorted by ID.
std::string networkManifestText(const NetworkManifest& manifest);
bool writeNetworkManifest(const std::filesystem::path& file, const NetworkManifest& manifest, std::string& error);

// IDs and paths unique, paths inside the project and present, profiles registered (when a
// frozen registry is given), game_protocol set. `shown` names the file in problems.
void validateNetworkManifest(
    const NetworkManifest& manifest, const Project& project, const ReplicationRegistry* registry,
    std::string_view shown, std::vector<NetProblem>& problems);

// ---- Content digest -----------------------------------------------------------------------
//
// SHA-256 of the files gameplay depends on, so peers with different maps, prefabs,
// circuits or collision models can't join each other. In v1 that's conservatively every
// file in Content (a cosmetic-only change, like a texture, also makes builds
// incompatible), plus the manifest's scenes and the manifest itself. Files starting with
// "." are skipped; symbolic links are an error. Text files (.swscene, .swprefab, .rml...)
// have CRLF read as LF, so a Windows checkout matches a Linux one. Game DLLs aren't
// hashed (they differ by platform): bump game_protocol when C++ gameplay changes.
//
// The bytes hashed: "shockwave_content_digest 1\n", "game_protocol <name>\n", then for each
// file in byte order of its path: the path, a zero byte, its size as a little-endian u64
// and its (normalised) bytes.

struct ContentFile {
    std::string path;   // project-relative, forward slashes
    std::string bytes;
};

// The digest of these files (sorted here; give them in any order).
Digest contentDigestOf(std::string_view gameProtocol, std::vector<ContentFile> files);
// True for the text formats whose CRLF line ends are normalised.
bool contentTextFile(std::string_view path);

struct ContentDigest {
    Digest digest{};
    std::vector<std::string> files;   // what was hashed, in order
    uint64_t bytes = 0;
};

// Reads and hashes the project's content. Unreadable files and links are problems; the
// digest is only valid when it returns true.
bool computeContentDigest(
    const Project& project, const NetworkManifest& manifest, std::string_view manifestPath, ContentDigest& out,
    std::vector<NetProblem>& problems);

}   // namespace shockwave::net
