#pragma once

// Network settings: Config/Network.swnetwork, a project's multiplayer configuration.
//
//   shockwave_network 1
//   enabled 1
//   transport steam
//   steam_app_id 480
//   max_players 12
//   tick_rate 60
//   manifest "Config/NetworkManifest.swnetmanifest"
//
// A project without the file plays offline only. One key per line, `#` starts a comment,
// keys left out keep their defaults. Unknown or repeated keys, values out of range and
// non-finite numbers are errors with file:line; nothing is ever clamped. Reading is
// transactional (a failed read changes nothing) and writing goes through a temporary
// file. One descriptor table (networkSettingFields) drives the parser, the writer, the
// validator, the editor's Project settings and the generated docs/NetworkSchema.md.

#include <Network/NetworkTypes.h>
#include <Network/OnlineServices.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace shockwave {
struct Project;
}

namespace shockwave::net {

enum class NetTransportKind : int { Steam, Loopback };
// LobbyVisibility (private, friends, public) is the online services' own (Network/OnlineServices.h).

struct NetworkSettings {
    bool enabled = false;
    NetTransportKind transport = NetTransportKind::Steam;
    uint32_t steamAppId = 0;
    bool steamRequired = false;
    LobbyVisibility lobbyVisibility = LobbyVisibility::Friends;
    uint32_t virtualPort = 0;
    uint32_t maxPlayers = 12;   // includes the listen host
    uint32_t tickRate = 60;
    uint32_t snapshotRate = 20;
    uint32_t inputRate = 60;
    uint32_t interpolationMs = 100;
    uint32_t maxExtrapolationMs = 100;
    uint32_t maxRewindMs = 200;
    double connectTimeoutSeconds = 15;
    double handshakeTimeoutSeconds = 15;
    double idleTimeoutSeconds = 10;
    uint32_t maxReplicatedEntities = 512;
    uint32_t maxSnapshotBytes = 1152;
    uint32_t outboundBytesPerSecond = 65536;   // per remote peer
    uint32_t maxPendingReliableBytes = 65536;   // per peer
    uint32_t maxReceiveBytesPerPump = 262144;   // whole endpoint
    uint32_t maxReceiveMessagesPerPump = 256;
    std::string manifest = "Config/NetworkManifest.swnetmanifest";   // project-relative
    std::string matchScene;   // project-relative; empty: the manifest's first scene
    bool operator==(const NetworkSettings&) const = default;
};

// ---- The descriptor table ----------------------------------------------------------------

enum class SettingKind { Bool, Choice, Integer, Seconds, Path };

struct NetworkSettingField {
    const char* key;
    SettingKind kind;
    std::variant<
        bool NetworkSettings::*, uint32_t NetworkSettings::*, double NetworkSettings::*, std::string NetworkSettings::*,
        NetTransportKind NetworkSettings::*, LobbyVisibility NetworkSettings::*>
        member;
    double min = 0, max = 0;   // Integer and Seconds
    std::span<const char* const> choices;   // Choice: names, in enum order
    const char* label;   // for people: the editor's row
    const char* meaning;   // one line for the editor's tooltip and the schema
};

// Every key, in file order.
std::span<const NetworkSettingField> networkSettingFields();
const NetworkSettingField* findNetworkSettingField(std::string_view key);

// A field's value as file text ("1", "steam", "15", "\"Config/x.swnetmanifest\"").
std::string settingText(const NetworkSettings& settings, const NetworkSettingField& field);
// Parses one value into settings; false (settings unchanged) with a reason otherwise.
bool setSettingText(
    NetworkSettings& settings, const NetworkSettingField& field, std::string_view text, std::string& why);

// ---- Validation -------------------------------------------------------------------------

// One finding from validating network files. `text` starts with the file (and line) it's
// about, the way `player --check` prints it.
struct NetProblem {
    bool warning = false;
    std::string text;
};

// A settings file as read: the values, and the line each key was on (0 if left out).
struct NetworkSettingsFile {
    NetworkSettings settings;
    std::vector<int> lines;   // one per networkSettingFields() entry
    int line(std::string_view key) const;
};

// Ranges of each field, and the rules between them. projectTickRate, when nonzero, must
// equal tick_rate. `shown` names the file in problems; `lines` (optional) adds lines.
void validateNetworkSettings(
    const NetworkSettings& settings, double projectTickRate, std::string_view shown, const NetworkSettingsFile* lines,
    std::vector<NetProblem>& problems);
bool hasErrors(std::span<const NetProblem> problems);

// ---- Files ------------------------------------------------------------------------------

// Parses the text of a .swnetwork. Syntax errors and per-field range errors stop it (the
// first goes into error as "<shown>:<line>: ..."); cross-field rules are
// validateNetworkSettings' job. False leaves out unchanged.
bool parseNetworkSettings(std::string_view text, std::string_view shown, NetworkSettingsFile& out, std::string& error);
bool readNetworkSettings(const std::filesystem::path& file, NetworkSettingsFile& out, std::string& error);
// The file text, every key in table order, so a write followed by a read is exact.
std::string networkSettingsText(const NetworkSettings& settings);
// Checks the field ranges first; writes through a temporary file and renames it.
bool writeNetworkSettings(const std::filesystem::path& file, const NetworkSettings& settings, std::string& error);

// Config/Network.swnetwork under the project root.
std::filesystem::path networkSettingsPath(const Project& project);

// A path relative to the project (or Content) that stays inside it and reads the same on
// every platform: forward slashes, no empty, . or .. parts, no drive or leading /.
bool validRelativeNetworkPath(std::string_view path, std::string& why);

// Writes a whole text file through a temporary file in the same folder, then renames it
// over the original, so a crash never leaves half a file.
bool writeFileAtomically(const std::filesystem::path& file, std::string_view text, std::string& error);

}   // namespace shockwave::net
