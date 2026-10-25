#include <Multiplayer/NetworkSettings.h>

#include <Multiplayer/NetworkProtocol.h>

#include <Network/Wire.h>
#include <Projects/Project.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace shockwave::net {

namespace {

constexpr const char* kTransportNames[] = {"steam", "loopback"};
constexpr const char* kVisibilityNames[] = {"private", "friends", "public"};

using S = NetworkSettings;

const NetworkSettingField kFields[] = {
    {"enabled",
     SettingKind::Bool,
     &S::enabled,
     0,
     0,
     {},
     "Multiplayer",
     "1 lets the game host and join sessions; 0 is offline only"},
    {"transport", SettingKind::Choice, &S::transport, 0, 0, kTransportNames, "Transport",
     "steam (Steam networking, needs a Steamworks build) or loopback (in-process tests)"},
    {"steam_app_id",
     SettingKind::Integer,
     &S::steamAppId,
     0,
     4294967295.0,
     {},
     "Steam AppID",
     "The game's Steam AppID; required when enabled with the steam transport. 480 is Valve's test app"},
    {"steam_required",
     SettingKind::Bool,
     &S::steamRequired,
     0,
     0,
     {},
     "Require Steam",
     "1 refuses to start without Steam running; 0 falls back to offline play"},
    {"lobby_visibility", SettingKind::Choice, &S::lobbyVisibility, 0, 0, kVisibilityNames, "Lobby visibility",
     "Who can find a hosted lobby: private (invites only), friends or public"},
    {"virtual_port",
     SettingKind::Integer,
     &S::virtualPort,
     0,
     999,
     {},
     "Virtual port",
     "Steam virtual port the host listens on"},
    {"max_players",
     SettingKind::Integer,
     &S::maxPlayers,
     2,
     kMaxPlayers,
     {},
     "Max players",
     "Players in a session, the listen host included: 2 (1v1) to 64 (32v32)"},
    {"tick_rate",
     SettingKind::Integer,
     &S::tickRate,
     kMinTickRate,
     kMaxTickRate,
     {},
     "Tick rate",
     "Simulation ticks per second, 60 to 120; must equal the project's tick_rate"},
    {"snapshot_rate",
     SettingKind::Integer,
     &S::snapshotRate,
     10,
     120,
     {},
     "Snapshot rate",
     "State snapshots per second: 10, 15, 20, 30, 60 or 120, dividing tick_rate"},
    {"input_rate",
     SettingKind::Integer,
     &S::inputRate,
     kMinTickRate,
     kMaxTickRate,
     {},
     "Input rate",
     "Input commands per second; equals tick_rate in protocol v1"},
    {"interpolation_ms",
     SettingKind::Integer,
     &S::interpolationMs,
     25,
     200,
     {},
     "Interpolation",
     "How far behind remote objects are shown, in milliseconds"},
    {"max_extrapolation_ms",
     SettingKind::Integer,
     &S::maxExtrapolationMs,
     0,
     100,
     {},
     "Max extrapolation",
     "How long a remote object keeps moving past its last snapshot, in milliseconds"},
    {"max_rewind_ms",
     SettingKind::Integer,
     &S::maxRewindMs,
     0,
     250,
     {},
     "Max rewind",
     "How far back the host rewinds players to check a shot, in milliseconds"},
    {"connect_timeout_seconds",
     SettingKind::Seconds,
     &S::connectTimeoutSeconds,
     1,
     120,
     {},
     "Connect timeout",
     "Seconds a connection may take to open"},
    {"handshake_timeout_seconds",
     SettingKind::Seconds,
     &S::handshakeTimeoutSeconds,
     1,
     120,
     {},
     "Handshake timeout",
     "Seconds from connecting to being admitted to the session"},
    {"idle_timeout_seconds",
     SettingKind::Seconds,
     &S::idleTimeoutSeconds,
     1,
     120,
     {},
     "Idle timeout",
     "Seconds without traffic before a peer counts as gone"},
    {"max_replicated_entities",
     SettingKind::Integer,
     &S::maxReplicatedEntities,
     12,
     kMaxNetworkObjects,
     {},
     "Max replicated entities",
     "Network objects a session may hold; at least max_players"},
    {"max_snapshot_bytes",
     SettingKind::Integer,
     &S::maxSnapshotBytes,
     512,
     kMaxUnreliableMessageBytes,
     {},
     "Max snapshot bytes",
     "Largest snapshot message, protocol header included"},
    {"outbound_bytes_per_second",
     SettingKind::Integer,
     &S::outboundBytesPerSecond,
     16384,
     262144,
     {},
     "Outbound bytes/s",
     "Bandwidth the host spends on each remote peer"},
    {"max_pending_reliable_bytes",
     SettingKind::Integer,
     &S::maxPendingReliableBytes,
     16384,
     262144,
     {},
     "Max pending reliable bytes",
     "Reliable bytes queued for one peer before it counts as too slow"},
    {"max_receive_bytes_per_pump",
     SettingKind::Integer,
     &S::maxReceiveBytesPerPump,
     16384,
     1048576,
     {},
     "Max receive bytes/pump",
     "Bytes an endpoint reads per frame, all peers together"},
    {"max_receive_messages_per_pump",
     SettingKind::Integer,
     &S::maxReceiveMessagesPerPump,
     16,
     1024,
     {},
     "Max receive messages/pump",
     "Messages an endpoint reads per frame, all peers together"},
    {"manifest",
     SettingKind::Path,
     &S::manifest,
     0,
     0,
     {},
     "Manifest",
     "The network manifest (.swnetmanifest), relative to the project folder"},
    {"match_scene",
     SettingKind::Path,
     &S::matchScene,
     0,
     0,
     {},
     "Match scene",
     "The manifest scene Host plays (.swscene, relative to the project folder); empty: the manifest's first"},
};

constexpr size_t kFieldCount = std::size(kFields);

std::string formatNumber(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return out.str();
}

}   // namespace

bool validRelativeNetworkPath(std::string_view path, std::string& why) {
    if (path.empty()) return why = "is empty", false;
    if (path.size() > 260) return why = "is longer than 260 bytes", false;
    if (!validUtf8(path)) return why = "isn't UTF-8", false;
    for (const char c : path)
        if (static_cast<unsigned char>(c) < 0x20) return why = "has a control character", false;
    if (path.find('\\') != std::string_view::npos) return why = "must use / between folders", false;
    if (path.front() == '/' || path.find(':') != std::string_view::npos)
        return why = "must be relative to the project folder", false;
    for (size_t at = 0; at <= path.size();) {
        const size_t slash = std::min(path.find('/', at), path.size());
        const std::string_view part = path.substr(at, slash - at);
        if (part.empty() || part == "." || part == "..")
            return why = "must not have empty, . or .. parts (it must stay inside the project)", false;
        at = slash + 1;
    }
    return true;
}

std::span<const NetworkSettingField> networkSettingFields() { return kFields; }

const NetworkSettingField* findNetworkSettingField(std::string_view key) {
    for (const NetworkSettingField& field : kFields)
        if (key == field.key) return &field;
    return nullptr;
}

std::string settingText(const NetworkSettings& settings, const NetworkSettingField& field) {
    return std::visit(
        [&](auto member) -> std::string {
            using M = std::remove_cvref_t<decltype(settings.*member)>;
            const M& value = settings.*member;
            if constexpr (std::is_same_v<M, bool>)
                return value ? "1" : "0";
            else if constexpr (std::is_same_v<M, uint32_t>)
                return std::to_string(value);
            else if constexpr (std::is_same_v<M, double>)
                return formatNumber(value);
            else if constexpr (std::is_same_v<M, std::string>) {
                std::ostringstream out;
                out << std::quoted(value);
                return out.str();
            } else {
                const size_t index = static_cast<size_t>(value);
                return index < field.choices.size() ? field.choices[index] : std::to_string(index);
            }
        },
        field.member);
}

bool setSettingText(
    NetworkSettings& settings, const NetworkSettingField& field, std::string_view text, std::string& why) {
    const std::string key = field.key;
    return std::visit(
        [&](auto member) -> bool {
            using M = std::remove_cvref_t<decltype(settings.*member)>;
            if constexpr (std::is_same_v<M, bool>) {
                if (text != "0" && text != "1") return why = key + " must be 0 or 1", false;
                settings.*member = text == "1";
            } else if constexpr (std::is_same_v<M, uint32_t>) {
                uint64_t value = 0;
                const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
                if (ec != std::errc{} || end != text.data() + text.size() || value < field.min || value > field.max)
                    return why = key + " must be a whole number from " + formatNumber(field.min) + " to " +
                                 formatNumber(field.max),
                           false;
                settings.*member = static_cast<uint32_t>(value);
            } else if constexpr (std::is_same_v<M, double>) {
                double value = 0;
                const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
                if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value) ||
                    value < field.min || value > field.max)
                    return why = key + " must be a number of seconds from " + formatNumber(field.min) + " to " +
                                 formatNumber(field.max),
                           false;
                settings.*member = value;
            } else if constexpr (std::is_same_v<M, std::string>) {
                std::string problem;
                const bool scene = key == "match_scene";
                if (scene && text.empty()) {
                    settings.*member = std::string();   // the manifest's first scene
                    return true;
                }
                if (!validRelativeNetworkPath(text, problem))
                    return why = key + " \"" + std::string(text) + "\" " + problem, false;
                if (!scene && !text.ends_with(".swnetmanifest"))
                    return why = key + " must name a .swnetmanifest file", false;
                if (scene && !text.ends_with(".swscene")) return why = key + " must name a .swscene file", false;
                settings.*member = std::string(text);
            } else {
                for (size_t i = 0; i < field.choices.size(); ++i)
                    if (text == field.choices[i]) {
                        settings.*member = static_cast<M>(i);
                        return true;
                    }
                std::string names;
                for (size_t i = 0; i < field.choices.size(); ++i)
                    names += std::string(
                                 i == 0                          ? ""
                                 : i + 1 == field.choices.size() ? " or "
                                                                 : ", ") +
                             field.choices[i];
                return why = key + " must be " + names, false;
            }
            return true;
        },
        field.member);
}

int NetworkSettingsFile::line(std::string_view key) const {
    for (size_t i = 0; i < kFieldCount && i < lines.size(); ++i)
        if (key == kFields[i].key) return lines[i];
    return 0;
}

void validateNetworkSettings(
    const NetworkSettings& s, double projectTickRate, std::string_view shown, const NetworkSettingsFile* file,
    std::vector<NetProblem>& problems) {
    auto at = [&](std::string_view key) {
        const int line = file ? file->line(key) : 0;
        return std::string(shown) + (line > 0 ? ":" + std::to_string(line) : "") + ": ";
    };
    auto error = [&](std::string_view key, const std::string& message) {
        problems.push_back({false, at(key) + message});
    };
    auto warn = [&](std::string_view key, const std::string& message) {
        problems.push_back({true, at(key) + message});
    };

    // Field ranges, for settings that didn't come from the parser (the editor).
    for (const NetworkSettingField& field : kFields) {
        NetworkSettings copy = s;
        std::string why, text = settingText(s, field);
        if (field.kind == SettingKind::Path) text = s.*std::get<std::string NetworkSettings::*>(field.member);
        if (!setSettingText(copy, field, text, why)) error(field.key, why);
    }
    if (projectTickRate > 0 && double(s.tickRate) != projectTickRate)
        error(
            "tick_rate", "tick_rate " + std::to_string(s.tickRate) + " doesn't match the project's tick_rate " +
                             formatNumber(projectTickRate) + "; they must be equal");
    if (std::find(kSnapshotRates.begin(), kSnapshotRates.end(), s.snapshotRate) == kSnapshotRates.end())
        error("snapshot_rate", "snapshot_rate must be 10, 15, 20, 30, 60 or 120");
    else if (s.tickRate % s.snapshotRate != 0)
        error(
            "snapshot_rate", "snapshot_rate " + std::to_string(s.snapshotRate) + " doesn't divide tick_rate " +
                                 std::to_string(s.tickRate));
    if (s.inputRate != s.tickRate) error("input_rate", "input_rate must equal tick_rate in protocol v1");
    if (s.transport == NetTransportKind::Steam && s.enabled && s.steamAppId == 0)
        error("steam_app_id", "steam_app_id is required when multiplayer is enabled with the steam transport");
    if (s.steamAppId == 480)
        warn("steam_app_id", "steam_app_id 480 is Valve's Spacewar test app: fine for development, not for release");
    if (s.steamRequired && s.transport != NetTransportKind::Steam)
        error("steam_required", "steam_required needs transport steam");
    if (s.maxReplicatedEntities < s.maxPlayers)
        error(
            "max_replicated_entities", "max_replicated_entities " + std::to_string(s.maxReplicatedEntities) +
                                           " is less than max_players " + std::to_string(s.maxPlayers) +
                                           ": every player needs a network object");
    // Remote objects are shown interpolation_ms in the past; with less than two snapshot
    // intervals of samples ahead, one lost snapshot leaves nothing to move towards.
    if (s.snapshotRate > 0 && uint64_t(s.interpolationMs) * s.snapshotRate < 2000)
        warn(
            "interpolation_ms", "interpolation_ms " + std::to_string(s.interpolationMs) +
                                    " is less than two snapshot intervals at snapshot_rate " +
                                    std::to_string(s.snapshotRate) + " (" +
                                    std::to_string((2000 + s.snapshotRate - 1) / s.snapshotRate) +
                                    " ms): one lost snapshot would make remote objects stop or jump");
    if (s.maxRewindMs < s.interpolationMs)
        warn(
            "max_rewind_ms", "max_rewind_ms " + std::to_string(s.maxRewindMs) + " is less than interpolation_ms " +
                                 std::to_string(s.interpolationMs) +
                                 ": shots at players shown that far back can't be checked");
}

bool hasErrors(std::span<const NetProblem> problems) {
    return std::any_of(problems.begin(), problems.end(), [](const NetProblem& p) { return !p.warning; });
}

bool parseNetworkSettings(std::string_view text, std::string_view shown, NetworkSettingsFile& out, std::string& error) {
    NetworkSettingsFile result;
    result.lines.assign(kFieldCount, 0);
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    std::string line;
    int lineNumber = 0;
    bool header = false;
    auto fail = [&](const std::string& message) {
        error = std::string(shown) + ":" + std::to_string(lineNumber) + ": " + message;
        return false;
    };
    while (std::getline(in, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // A # outside quotes starts a comment.
        bool quoted = false;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\' && quoted) {
                ++i;
                continue;
            }
            if (line[i] == '"') quoted = !quoted;
            if (line[i] == '#' && !quoted) {
                line.erase(i);
                break;
            }
        }
        std::istringstream row(line);
        std::string key;
        if (!(row >> key)) continue;
        if (!header) {
            std::string version, extra;
            if (key != "shockwave_network" || !(row >> version) || (row >> extra))
                return fail("expected shockwave_network 1 as the first line");
            if (version != "1")
                return fail("unsupported network settings version " + version + " (this build reads 1)");
            header = true;
            continue;
        }
        const NetworkSettingField* field = findNetworkSettingField(key);
        if (!field) return fail("unknown setting " + key);
        const size_t index = size_t(field - kFields);
        if (result.lines[index] != 0)
            return fail("duplicate setting " + key + " (first on line " + std::to_string(result.lines[index]) + ")");
        std::string value, extra;
        row >> std::ws;
        if (field->kind == SettingKind::Path) {
            if (row.peek() != '"' || !(row >> std::quoted(value))) return fail(key + " takes a quoted path");
        } else if (!(row >> value)) {
            return fail(key + " needs a value");
        }
        if (row >> extra) return fail(key + " takes one value");
        std::string why;
        if (!setSettingText(result.settings, *field, value, why)) return fail(why);
        result.lines[index] = lineNumber;
    }
    if (!header) return fail("expected shockwave_network 1 as the first line");
    out = std::move(result);
    error.clear();
    return true;
}

bool readNetworkSettings(const std::filesystem::path& file, NetworkSettingsFile& out, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = file.string() + ": can't open network settings";
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) {
        error = file.string() + ": failed reading network settings";
        return false;
    }
    return parseNetworkSettings(text.str(), file.string(), out, error);
}

std::string networkSettingsText(const NetworkSettings& settings) {
    std::string out = "shockwave_network 1\n";
    for (const NetworkSettingField& field : kFields)
        out += std::string(field.key) + " " + settingText(settings, field) + "\n";
    return out;
}

bool writeNetworkSettings(const std::filesystem::path& file, const NetworkSettings& settings, std::string& error) {
    std::vector<NetProblem> problems;
    validateNetworkSettings(settings, 0, file.string(), nullptr, problems);
    for (const NetProblem& problem : problems)
        if (!problem.warning) {
            error = problem.text;
            return false;
        }
    return writeFileAtomically(file, networkSettingsText(settings), error);
}

std::filesystem::path networkSettingsPath(const Project& project) {
    return project.root / "Config" / "Network.swnetwork";
}

bool writeFileAtomically(const std::filesystem::path& file, std::string_view text, std::string& error) {
    std::error_code ec;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        return false;
    }
    const std::filesystem::path temp =
        file.string() + "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), std::streamsize(text.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(temp, ec);
            error = file.string() + ": couldn't write it";
            return false;
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
    error.clear();
    return true;
}

}   // namespace shockwave::net
