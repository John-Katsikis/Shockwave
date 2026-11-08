// Config/Release.swrelease: the release contract (see ReleaseSettings.h, docs/Release.md).
#include <Projects/ReleaseSettings.h>

#include <Projects/Project.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace shockwave {

namespace {

constexpr const char* kPlatformNames[] = {
    "linux-x86_64", "linux-arm64", "windows-x86_64", "macos-x86_64", "macos-arm64"};
constexpr const char* kModeNames[] = {"offline", "online"};
constexpr const char* kTierNames[] = {"low", "medium", "high", "ultra"};

constexpr uint32_t kMaxBudgetMib = 1048576;   // 1 TiB: a typo, not a machine
constexpr uint32_t kMaxOnlinePlayers = 64;   // the engine's limit (32v32)

using R = ReleaseSettings;

const ReleaseField kFields[] = {
    {"application_id",
     ReleaseFieldKind::Id,
     &R::applicationId,
     0,
     0,
     {},
     "Application ID",
     "The game's permanent ID, reverse-domain style (com.studio.game): lowercase letters, digits, - and _ in "
     "dot-separated "
     "parts. Saves and settings are found by it, so it never changes, whatever the game is called"},
    {"release_version",
     ReleaseFieldKind::Version,
     &R::releaseVersion,
     0,
     0,
     {},
     "Release version",
     "The version being released, like 1.0.0 or 1.2.0-beta1; must equal the package version "
     "(Config/Package.swpackage)"},
    {"platform", ReleaseFieldKind::Platforms, &R::platforms, 0, 0, kPlatformNames, "Platforms",
     "A platform the release is qualified for; one line each, at least one"},
    {"mode", ReleaseFieldKind::Modes, &R::modes, 0, 0, kModeNames, "Modes",
     "offline (single player) or online (multiplayer); one line each, at least one"},
    {"max_players",
     ReleaseFieldKind::Integer,
     &R::maxPlayers,
     1,
     kMaxOnlinePlayers,
     {},
     "Max players",
     "Players in one game: 1 for an offline-only game, an even 2 (1v1) to 64 (32v32) with online; equals the network "
     "settings' max_players"},
    {"target_fps",
     ReleaseFieldKind::Integer,
     &R::targetFps,
     30,
     360,
     {},
     "Target frame rate",
     "Frames per second the release promises on its minimum hardware; the frame budget is 1000/target_fps ms"},
    {"simulation_hz",
     ReleaseFieldKind::Integer,
     &R::simulationHz,
     1,
     1000,
     {},
     "Simulation rate",
     "Fixed simulation ticks per second; equals the project's tick_rate"},
    {"graphics_tier", ReleaseFieldKind::Choice, &R::graphicsTier, 0, 0, kTierNames, "Graphics tier",
     "The graphics preset the minimum hardware is qualified at: low, medium, high or ultra"},
    {"cpu_budget_mib",
     ReleaseFieldKind::Integer,
     &R::cpuBudgetMib,
     64,
     kMaxBudgetMib,
     {},
     "CPU memory budget",
     "Most system memory the game may use, in MiB"},
    {"gpu_budget_mib",
     ReleaseFieldKind::Integer,
     &R::gpuBudgetMib,
     64,
     kMaxBudgetMib,
     {},
     "GPU memory budget",
     "Most GPU memory the game may use, in MiB; at least the project's graphics_memory_budget_mb"},
    {"shared_budget_mib",
     ReleaseFieldKind::Integer,
     &R::sharedBudgetMib,
     0,
     kMaxBudgetMib,
     {},
     "Shared memory budget",
     "For unified memory (CPU and GPU share it), the whole pool in MiB, holding both budgets; 0 means separate "
     "memories (a discrete GPU)"},
    {"loading_budget_mib",
     ReleaseFieldKind::Integer,
     &R::loadingBudgetMib,
     16,
     kMaxBudgetMib,
     {},
     "Loading budget",
     "Most extra memory a level change may hold while it loads, in MiB; within cpu_budget_mib"},
    {"transition_timeout_seconds",
     ReleaseFieldKind::Seconds,
     &R::transitionTimeoutSeconds,
     1,
     600,
     {},
     "Transition timeout",
     "Seconds a level change may take before it fails with LoadTimeout; never waits forever"},
    {"acceptance_scenario",
     ReleaseFieldKind::Path,
     &R::acceptanceScenario,
     0,
     0,
     {},
     "Acceptance scenario",
     "The flow-test scenario (.json, relative to the project folder) that plays the game from start to finish"},
};

constexpr size_t kFieldCount = std::size(kFields);

// Codes for rules a contract shares with other settings (the project_settings gate).
constexpr std::string_view kSharedCodes[] = {
    "ReleaseTickRate",         "ReleaseVersionMismatch",   "ReleaseNetworkMissing",
    "ReleaseNetworkInvalid",   "ReleaseNetworkUnexpected", "ReleaseNetworkPlayers",
    "ReleaseDevelopmentAppId", "ReleaseGraphicsBudget",    "ReleaseScenarioMissing"};

bool sharedCode(std::string_view code) {
    return std::find(std::begin(kSharedCodes), std::end(kSharedCodes), code) != std::end(kSharedCodes);
}

std::string formatNumber(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return out.str();
}

std::string quoted(const std::string& text) {
    std::ostringstream out;
    out << std::quoted(text);
    return out.str();
}

std::string names(std::span<const char* const> choices) {
    std::string out;
    for (size_t i = 0; i < choices.size(); ++i)
        out += std::string(i == 0 ? "" : i + 1 == choices.size() ? " or " : ", ") + choices[i];
    return out;
}

// Reverse-domain: two or more dot-separated parts, each a lowercase letter then lowercase
// letters, digits, '-' or '_'. Lowercase only, so it names the same folder everywhere.
bool validApplicationId(std::string_view id, std::string& why) {
    if (id.empty()) return why = "is empty", false;
    if (id.size() > 128) return why = "is longer than 128 characters", false;
    int parts = 0;
    for (size_t at = 0; at <= id.size();) {
        const size_t dot = std::min(id.find('.', at), id.size());
        const std::string_view part = id.substr(at, dot - at);
        if (part.empty()) return why = "has an empty part (two dots, or a dot at an end)", false;
        if (part[0] < 'a' || part[0] > 'z') return why = "has a part that doesn't start with a lowercase letter", false;
        for (const char c : part)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
                return why = "may only hold lowercase letters, digits, '-', '_' and '.'", false;
        ++parts;
        at = dot + 1;
    }
    if (parts < 2) return why = "needs at least two parts, like com.studio.game", false;
    return true;
}

// 1 to 4 dot-separated numbers, then optionally '-' and a label of letters, digits and dots.
bool validVersion(std::string_view version, std::string& why) {
    why = "must be like 1.0.0 or 1.2.0-beta1 (numbers separated by dots, then optionally - and a label)";
    if (version.empty() || version.size() > 32) return false;
    const size_t dash = version.find('-');
    const std::string_view numbers = version.substr(0, dash);
    int parts = 0;
    for (size_t at = 0; at <= numbers.size();) {
        const size_t dot = std::min(numbers.find('.', at), numbers.size());
        const std::string_view part = numbers.substr(at, dot - at);
        if (part.empty() || part.size() > 9 ||
            !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        ++parts;
        at = dot + 1;
    }
    if (parts > 4) return false;
    if (dash != std::string_view::npos) {
        const std::string_view label = version.substr(dash + 1);
        if (label.empty() || !std::all_of(label.begin(), label.end(), [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '.';
            }))
            return false;
    }
    return true;
}

// Relative to the project, inside it, the same on every platform: forward slashes, no
// empty, . or .. parts, no drive or leading /.
bool validRelativePath(std::string_view path, std::string& why) {
    if (path.empty()) return why = "is empty", false;
    if (path.size() > 260) return why = "is longer than 260 bytes", false;
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

template <class E>
bool choose(std::span<const char* const> choices, std::string_view text, E& out) {
    for (size_t i = 0; i < choices.size(); ++i)
        if (text == choices[i]) {
            out = static_cast<E>(i);
            return true;
        }
    return false;
}

// Parses one value of a field into settings (a list field gains one entry). False with a
// reason, settings unchanged, otherwise.
bool setFieldText(ReleaseSettings& settings, const ReleaseField& field, std::string_view text, std::string& why) {
    const std::string key = field.key;
    switch (field.kind) {
    case ReleaseFieldKind::Id: {
        std::string problem;
        if (!validApplicationId(text, problem)) return why = key + " \"" + std::string(text) + "\" " + problem, false;
        settings.*std::get<std::string R::*>(field.member) = std::string(text);
        return true;
    }
    case ReleaseFieldKind::Version: {
        std::string problem;
        if (!validVersion(text, problem)) return why = key + " \"" + std::string(text) + "\" " + problem, false;
        settings.*std::get<std::string R::*>(field.member) = std::string(text);
        return true;
    }
    case ReleaseFieldKind::Path: {
        std::string problem;
        if (!validRelativePath(text, problem)) return why = key + " \"" + std::string(text) + "\" " + problem, false;
        if (!text.ends_with(".json")) return why = key + " must name a .json flow-test scenario", false;
        settings.*std::get<std::string R::*>(field.member) = std::string(text);
        return true;
    }
    case ReleaseFieldKind::Platforms: {
        auto& list = settings.*std::get<std::vector<std::string> R::*>(field.member);
        if (std::find(field.choices.begin(), field.choices.end(), text) == field.choices.end())
            return why = key + " must be " + names(field.choices), false;
        if (std::find(list.begin(), list.end(), text) != list.end())
            return why = key + " " + std::string(text) + " is listed twice", false;
        list.emplace_back(text);
        return true;
    }
    case ReleaseFieldKind::Modes: {
        auto& list = settings.*std::get<std::vector<ReleaseMode> R::*>(field.member);
        ReleaseMode mode{};
        if (!choose(field.choices, text, mode)) return why = key + " must be " + names(field.choices), false;
        if (std::find(list.begin(), list.end(), mode) != list.end())
            return why = key + " " + std::string(text) + " is listed twice", false;
        list.push_back(mode);
        return true;
    }
    case ReleaseFieldKind::Choice: {
        ReleaseGraphicsTier tier{};
        if (!choose(field.choices, text, tier)) return why = key + " must be " + names(field.choices), false;
        settings.*std::get<ReleaseGraphicsTier R::*>(field.member) = tier;
        return true;
    }
    case ReleaseFieldKind::Integer: {
        uint64_t value = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || end != text.data() + text.size() || value < field.min || value > field.max)
            return why = key + " must be a whole number from " + formatNumber(field.min) + " to " +
                         formatNumber(field.max),
                   false;
        settings.*std::get<uint32_t R::*>(field.member) = static_cast<uint32_t>(value);
        return true;
    }
    case ReleaseFieldKind::Seconds: {
        double value = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value) || value < field.min ||
            value > field.max)
            return why = key + " must be a number of seconds from " + formatNumber(field.min) + " to " +
                         formatNumber(field.max),
                   false;
        settings.*std::get<double R::*>(field.member) = value;
        return true;
    }
    }
    return why = "unknown field kind", false;
}

bool listField(const ReleaseField& field) {
    return field.kind == ReleaseFieldKind::Platforms || field.kind == ReleaseFieldKind::Modes;
}
bool quotedField(const ReleaseField& field) {
    return field.kind == ReleaseFieldKind::Id || field.kind == ReleaseFieldKind::Version ||
           field.kind == ReleaseFieldKind::Path;
}

}   // namespace

// ---- Diagnostic -------------------------------------------------------------------------

std::string Diagnostic::text() const {
    std::string out = file;
    if (!file.empty() && line > 0) out += ":" + std::to_string(line);
    if (!out.empty()) out += ": ";
    return out + message;
}

const char* diagnosticSeverityName(DiagnosticSeverity severity) {
    return severity == DiagnosticSeverity::Error ? "error" : "warning";
}

const char* problemKindName(ProblemKind kind) {
    switch (kind) {
    case ProblemKind::Content: return "content";
    case ProblemKind::Navigation: return "navigation";
    case ProblemKind::Circuit: return "circuit";
    case ProblemKind::Physics: return "physics";
    case ProblemKind::Gameplay: return "gameplay";
    case ProblemKind::Settings: return "settings";
    }
    return "content";
}

const char* problemKindTitle(ProblemKind kind) {
    switch (kind) {
    case ProblemKind::Content: return "Missing or broken content";
    case ProblemKind::Navigation: return "Navigation";
    case ProblemKind::Circuit: return "Circuit errors";
    case ProblemKind::Physics: return "Physics setup";
    case ProblemKind::Gameplay: return "Gameplay code and rules";
    case ProblemKind::Settings: return "Project settings";
    }
    return "Missing or broken content";
}

ProblemKind problemKindOf(const Diagnostic& diagnostic) {
    const std::string_view code = diagnostic.code;
    auto starts = [&](std::string_view prefix) { return code.substr(0, prefix.size()) == prefix; };
    if (starts("Circuit")) return ProblemKind::Circuit;
    if (starts("Navigation")) return ProblemKind::Navigation;
    if (starts("Physics")) return ProblemKind::Physics;
    if (starts("Game")) return ProblemKind::Gameplay;
    if (starts("Release") || starts("Package") || starts("Network") || starts("Input")) return ProblemKind::Settings;
    return ProblemKind::Content;
}

bool hasErrors(std::span<const Diagnostic> diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& d) { return d.error(); });
}

std::string diagnosticJsonString(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

std::string diagnosticJson(const Diagnostic& d) {
    auto orNull = [](const std::string& text) {
        return text.empty() ? std::string("null") : diagnosticJsonString(text);
    };
    return "{\"code\": " + diagnosticJsonString(d.code) + ", \"severity\": \"" + diagnosticSeverityName(d.severity) +
           "\", \"file\": " + orNull(d.file) + ", \"line\": " + (d.line > 0 ? std::to_string(d.line) : "null") +
           ", \"object\": " + orNull(d.object) + ", \"message\": " + diagnosticJsonString(d.message) + "}";
}

// ---- The table --------------------------------------------------------------------------

bool ReleaseSettings::hasMode(ReleaseMode mode) const {
    return std::find(modes.begin(), modes.end(), mode) != modes.end();
}

std::span<const ReleaseField> releaseFields() { return kFields; }

const ReleaseField* findReleaseField(std::string_view key) {
    for (const ReleaseField& field : kFields)
        if (key == field.key) return &field;
    return nullptr;
}

std::span<const char* const> releasePlatformNames() { return kPlatformNames; }
const char* releaseModeName(ReleaseMode mode) { return kModeNames[std::clamp(int(mode), 0, 1)]; }
const char* releaseGraphicsTierName(ReleaseGraphicsTier tier) { return kTierNames[std::clamp(int(tier), 0, 3)]; }

std::vector<std::string> releaseFieldText(const ReleaseSettings& settings, const ReleaseField& field) {
    switch (field.kind) {
    case ReleaseFieldKind::Id:
    case ReleaseFieldKind::Version:
    case ReleaseFieldKind::Path: return {quoted(settings.*std::get<std::string R::*>(field.member))};
    case ReleaseFieldKind::Platforms: return settings.*std::get<std::vector<std::string> R::*>(field.member);
    case ReleaseFieldKind::Modes: {
        std::vector<std::string> out;
        for (const ReleaseMode mode : settings.*std::get<std::vector<ReleaseMode> R::*>(field.member))
            out.push_back(releaseModeName(mode));
        return out;
    }
    case ReleaseFieldKind::Choice:
        return {releaseGraphicsTierName(settings.*std::get<ReleaseGraphicsTier R::*>(field.member))};
    case ReleaseFieldKind::Integer: return {std::to_string(settings.*std::get<uint32_t R::*>(field.member))};
    case ReleaseFieldKind::Seconds: return {formatNumber(settings.*std::get<double R::*>(field.member))};
    }
    return {};
}

// ---- Files ------------------------------------------------------------------------------

int ReleaseFile::line(std::string_view key) const {
    for (size_t i = 0; i < kFieldCount && i < lines.size(); ++i)
        if (key == kFields[i].key) return lines[i];
    return 0;
}

bool parseRelease(std::string_view text, std::string_view shown, ReleaseFile& out, Diagnostic& error) {
    ReleaseFile result;
    result.settings = {};
    result.settings.platforms.clear();
    result.settings.modes.clear();
    result.lines.assign(kFieldCount, 0);
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    std::string line;
    int lineNumber = 0;
    bool header = false;
    auto fail = [&](const char* code, const std::string& object, const std::string& message) {
        error = {code, DiagnosticSeverity::Error, std::string(shown), lineNumber, object, message};
        return false;
    };
    while (std::getline(in, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // A # outside quotes starts a comment.
        bool inQuotes = false;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\' && inQuotes) {
                ++i;
                continue;
            }
            if (line[i] == '"') inQuotes = !inQuotes;
            if (line[i] == '#' && !inQuotes) {
                line.erase(i);
                break;
            }
        }
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        std::string key;
        if (!(row >> key)) continue;
        if (!header) {
            std::string version, extra;
            if (key != "shockwave_release" || !(row >> version) || (row >> extra))
                return fail("ReleaseHeader", "", "expected shockwave_release 1 as the first line");
            if (version != "1")
                return fail(
                    "ReleaseUnsupportedVersion", "",
                    "unsupported release contract version " + version + " (this build reads 1)");
            header = true;
            continue;
        }
        const ReleaseField* field = findReleaseField(key);
        if (!field) return fail("ReleaseUnknownKey", key, "unknown key " + key);
        const size_t index = size_t(field - kFields);
        if (result.lines[index] != 0 && !listField(*field))
            return fail(
                "ReleaseDuplicateKey", key,
                "duplicate key " + key + " (first on line " + std::to_string(result.lines[index]) + ")");
        std::string value, extra;
        row >> std::ws;
        if (quotedField(*field)) {
            if (row.peek() != '"' || !(row >> std::quoted(value)))
                return fail("ReleaseInvalidValue", key, key + " takes a quoted value");
        } else if (!(row >> value)) {
            return fail("ReleaseInvalidValue", key, key + " needs a value");
        }
        if (row >> extra) return fail("ReleaseInvalidValue", key, key + " takes one value");
        std::string why;
        if (!setFieldText(result.settings, *field, value, why)) return fail("ReleaseInvalidValue", key, why);
        if (result.lines[index] == 0) result.lines[index] = lineNumber;
    }
    if (!header) return fail("ReleaseHeader", "", "expected shockwave_release 1 as the first line");
    for (size_t i = 0; i < kFieldCount; ++i)
        if (result.lines[i] == 0) {
            lineNumber = 0;
            return fail(
                "ReleaseMissingKey", kFields[i].key,
                std::string("missing ") + kFields[i].key + ": every key of a release contract is required");
        }
    out = std::move(result);
    return true;
}

bool readRelease(const std::filesystem::path& file, std::string_view shown, ReleaseFile& out, Diagnostic& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = {"ReleaseUnreadable", DiagnosticSeverity::Error, std::string(shown), 0, "", "can't open it"};
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) {
        error = {"ReleaseUnreadable", DiagnosticSeverity::Error, std::string(shown), 0, "", "failed reading it"};
        return false;
    }
    return parseRelease(text.str(), shown, out, error);
}

std::string releaseText(const ReleaseSettings& settings) {
    std::string out = "# What this project promises to ship (docs/Release.md). Edited in Project > Release readiness.\n"
                      "shockwave_release 1\n";
    for (const ReleaseField& field : kFields)
        for (const std::string& value : releaseFieldText(settings, field))
            out += std::string(field.key) + " " + value + "\n";
    return out;
}

bool writeRelease(const std::filesystem::path& file, const ReleaseSettings& settings, std::string& error) {
    std::vector<Diagnostic> problems;
    validateRelease(settings, nullptr, file.string(), nullptr, problems);
    for (const Diagnostic& problem : problems)
        if (problem.error()) {
            error = problem.text();
            return false;
        }
    std::error_code ec;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        return false;
    }
    const std::filesystem::path temp =
        file.string() + "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
    {
        const std::string text = releaseText(settings);
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

void levelReleaseInputs(
    const Project& project, ReleaseInputs& inputs, const std::map<std::string, LevelSceneFacts>* facts) {
    LevelCatalog catalog;
    std::vector<Diagnostic> diagnostics;
    const std::string shown = project.levelCatalog.empty() ? std::string() : project.relative(project.levelCatalog);
    inputs.levelCatalogChecked = true;
    inputs.levelLinksChecked = false;
    inputs.levelCatalogProblem.clear();
    const bool read = projectLevels(project, catalog, diagnostics);
    inputs.levelCatalogImplicit = read && catalog.implicit;
    inputs.levelCount = catalog.levels.size();
    inputs.levelEntry = catalog.entry;
    if (read && !catalog.implicit) {
        validateLevelCatalog(project, catalog, shown, diagnostics);
        if (facts) {
            validateLevelLinks(catalog, *facts, shown, diagnostics);
            inputs.levelLinksChecked = true;
        }
    }
    for (const Diagnostic& d : diagnostics)
        if (d.error()) {
            inputs.levelCatalogProblem = d.text();
            break;
        }
}

std::filesystem::path releasePath(const Project& project) { return project.root / "Config" / "Release.swrelease"; }

std::string fallbackApplicationId(const Project& project) {
    std::string part;
    for (const char c : project.gameModule) {
        const char lower = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') || lower == '-' || lower == '_')
            part += lower;
    }
    return "shockwave." + (part.empty() ? std::string("game") : part);
}

std::string projectApplicationId(const Project& project) {
    const std::filesystem::path contract = releasePath(project);
    std::error_code ec;
    ReleaseFile file;
    Diagnostic error;
    if (std::filesystem::exists(contract, ec) && readRelease(contract, project.relative(contract), file, error) &&
        !file.settings.applicationId.empty())
        return file.settings.applicationId;
    return fallbackApplicationId(project);
}

// ---- Validation -------------------------------------------------------------------------

void validateRelease(
    const ReleaseSettings& s, const ReleaseInputs* inputs, std::string_view shown, const ReleaseFile* file,
    std::vector<Diagnostic>& diagnostics) {
    auto add = [&](DiagnosticSeverity severity, const char* code, std::string_view key, std::string message) {
        diagnostics.push_back(
            {code, severity, std::string(shown), file ? file->line(key) : 0, std::string(key), std::move(message)});
    };
    auto error = [&](const char* code, std::string_view key, std::string message) {
        add(DiagnosticSeverity::Error, code, key, std::move(message));
    };
    auto warn = [&](const char* code, std::string_view key, std::string message) {
        add(DiagnosticSeverity::Warning, code, key, std::move(message));
    };

    // Each field on its own, for settings that didn't come from the parser (the editor).
    for (const ReleaseField& field : kFields) {
        const std::vector<std::string> values = releaseFieldText(s, field);
        if (values.empty()) {
            error("ReleaseMissingKey", field.key, std::string("needs at least one ") + field.key);
            continue;
        }
        ReleaseSettings copy;
        copy.platforms.clear();
        copy.modes.clear();
        for (std::string value : values) {
            if (quotedField(field)) value = s.*std::get<std::string R::*>(field.member);
            std::string why;
            if (!setFieldText(copy, field, value, why)) {
                error("ReleaseInvalidValue", field.key, why);
                break;
            }
        }
    }

    // Rules between fields.
    const bool online = s.hasMode(ReleaseMode::Online);
    if (!online && s.maxPlayers != 1)
        error(
            "ReleasePlayerCount", "max_players",
            "max_players " + std::to_string(s.maxPlayers) + " needs mode online; an offline-only game has 1 player");
    if (online && (s.maxPlayers < 2 || s.maxPlayers % 2 != 0))
        error(
            "ReleasePlayerCount", "max_players",
            "max_players " + std::to_string(s.maxPlayers) +
                " must be even, from 2 (1v1) to 64 (32v32), for this team game online");
    if (s.loadingBudgetMib > s.cpuBudgetMib)
        error(
            "ReleaseLoadingBudget", "loading_budget_mib",
            "loading_budget_mib " + std::to_string(s.loadingBudgetMib) + " is more than cpu_budget_mib " +
                std::to_string(s.cpuBudgetMib) + ": loading has to fit in the game's memory");
    if (s.sharedBudgetMib > 0 && uint64_t(s.cpuBudgetMib) + s.gpuBudgetMib > s.sharedBudgetMib)
        error(
            "ReleaseMemoryBudget", "shared_budget_mib",
            "cpu_budget_mib " + std::to_string(s.cpuBudgetMib) + " and gpu_budget_mib " +
                std::to_string(s.gpuBudgetMib) + " together are more than shared_budget_mib " +
                std::to_string(s.sharedBudgetMib) + ", the memory they share");
    if (!inputs) return;

    // Rules shared with the rest of the project.
    const ReleaseInputs& in = *inputs;
    if (in.projectTickRate > 0 && double(s.simulationHz) != in.projectTickRate)
        error(
            "ReleaseTickRate", "simulation_hz",
            "simulation_hz " + std::to_string(s.simulationHz) + " doesn't match the project's tick_rate " +
                formatNumber(in.projectTickRate) + "; they must be equal");
    if (!in.packageVersion.empty() && in.packageVersion != s.releaseVersion)
        error(
            "ReleaseVersionMismatch", "release_version",
            "release_version " + s.releaseVersion + " doesn't match the package version " + in.packageVersion + " (" +
                (in.packageFile.empty() ? std::string("Config/Package.swpackage") : in.packageFile) + ")");
    if (online) {
        if (!in.networkError.empty())
            error("ReleaseNetworkInvalid", "mode", "mode online, but " + in.networkError);
        else if (!in.networkPresent)
            error(
                "ReleaseNetworkMissing", "mode",
                "mode online, but there's no " + in.networkFile + ": the project plays offline only");
        else if (!in.networkEnabled)
            error("ReleaseNetworkMissing", "mode", "mode online, but " + in.networkFile + " has multiplayer off");
        else {
            if (in.networkMaxPlayers != s.maxPlayers)
                error(
                    "ReleaseNetworkPlayers", "max_players",
                    "max_players " + std::to_string(s.maxPlayers) + " doesn't match " + in.networkFile +
                        "'s max_players " + std::to_string(in.networkMaxPlayers));
            if (in.networkSteam && in.steamAppId == 480)
                warn(
                    "ReleaseDevelopmentAppId", "mode",
                    in.networkFile +
                        " plays as Steam AppID 480, Valve's test app: the release can't qualify until it has "
                        "the game's own AppID");
        }
    } else if (in.networkPresent && in.networkEnabled) {
        error(
            "ReleaseNetworkUnexpected", "mode",
            in.networkFile + " turns multiplayer on, but the contract has no mode online");
    }
    if (in.graphicsMemoryBudgetMb > s.gpuBudgetMib)
        error(
            "ReleaseGraphicsBudget", "gpu_budget_mib",
            "gpu_budget_mib " + std::to_string(s.gpuBudgetMib) +
                " is less than the project's graphics_memory_budget_mb " + std::to_string(in.graphicsMemoryBudgetMb) +
                "; the optional effects alone may use more");
    if (!in.acceptanceScenarioExists && !s.acceptanceScenario.empty())
        warn(
            "ReleaseScenarioMissing", "acceptance_scenario",
            "acceptance_scenario " + s.acceptanceScenario +
                " doesn't exist yet, so the game's start-to-finish gate stays unverified");
}

// ---- Qualification ----------------------------------------------------------------------

const char* gateStatusName(GateStatus status) {
    switch (status) {
    case GateStatus::Passed: return "passed";
    case GateStatus::Failed: return "failed";
    case GateStatus::Unverified: return "unverified";
    case GateStatus::NotApplicable: return "not_applicable";
    }
    return "?";
}

bool ReleaseReport::qualified() const {
    if (!present || !readable || gates.empty()) return false;
    return std::all_of(gates.begin(), gates.end(), [](const ReleaseGate& gate) {
        return gate.status == GateStatus::Passed || gate.status == GateStatus::NotApplicable;
    });
}

namespace {

// Gates whose evidence later batches bring: what each needs, from the shipping plan.
struct PendingGate {
    const char* id;
    const char* batch;
    const char* needs;
    bool online;   // applies only to a release with mode online
};

constexpr PendingGate kPendingGates[] = {
    {"level_travel", "S05", "a real level change (menu, level A, level B, menu) through GameFlow", false},
    {"saves", "S06", "versioned saves that continue across levels and survive interrupted writes", false},
    {"player_settings", "S08", "display, input, audio, accessibility and language settings that persist", false},
    {"cooked_package", "S09", "a release package cooked from every level's dependencies, run read-only", false},
    {"performance", "S10", "frame-time, memory and loading measurements on named release hardware", false},
    {"online_lifecycle", "S11", "a full online match lifecycle with map rotation, on the real service", true},
    {"crash_recovery", "S12", "local crash reports, symbols and a safe restart that keeps player data", false},
};

}   // namespace

void releaseGates(ReleaseReport& report, const ReleaseInputs& inputs) {
    report.gates.clear();
    report.buildConfig = inputs.buildConfig;
    auto gate = [&](std::string id, GateStatus status, std::string batch, std::string reason) {
        report.gates.push_back({std::move(id), status, std::move(batch), std::move(reason)});
    };
    if (!report.present) {
        gate(
            "release_contract", GateStatus::Unverified, "S00",
            "no " + report.file + ": a development project, with nothing to qualify yet");
        return;
    }
    if (!report.readable) {
        gate("release_contract", GateStatus::Failed, "S00", report.file + " doesn't read; fix it first");
        return;
    }
    const ReleaseSettings& s = report.settings;
    auto firstError = [&](bool shared) -> const Diagnostic* {
        for (const Diagnostic& d : report.diagnostics)
            if (d.error() && sharedCode(d.code) == shared) return &d;
        return nullptr;
    };
    if (const Diagnostic* d = firstError(false))
        gate("release_contract", GateStatus::Failed, "S00", d->text());
    else
        gate("release_contract", GateStatus::Passed, "S00", "every key present and in range");
    if (const Diagnostic* d = firstError(true))
        gate("project_settings", GateStatus::Failed, "S00", d->text());
    else
        gate(
            "project_settings", GateStatus::Passed, "S00",
            "matches the project's tick rate, package version, network and graphics settings");

    const std::string& config = inputs.buildConfig;
    if (config == "Release" || config == "RelWithDebInfo" || config == "MinSizeRel")
        gate("release_build", GateStatus::Passed, "S00", "this engine is an optimised " + config + " build");
    else if (config.empty())
        gate(
            "release_build", GateStatus::Unverified, "S00",
            "this engine's build configuration is unknown (no CMAKE_BUILD_TYPE); configure with "
            "-DCMAKE_BUILD_TYPE=Release");
    else
        gate(
            "release_build", GateStatus::Failed, "S00",
            "this engine is a " + config + " build; releases are measured on an optimised Release build");

    const bool online = s.hasMode(ReleaseMode::Online);
    if (!online || !inputs.networkSteam)
        gate(
            "steam_app_id", GateStatus::NotApplicable, "S00",
            online ? "online play doesn't use Steam" : "offline only");
    else if (inputs.steamAppId == 0 || inputs.steamAppId == 480)
        gate(
            "steam_app_id", GateStatus::Failed, "S00",
            inputs.steamAppId == 0 ? "no steam_app_id"
                                   : "Steam AppID 480 is Valve's test app; a release needs the game's own");
    else
        gate("steam_app_id", GateStatus::Passed, "S00", "Steam AppID " + std::to_string(inputs.steamAppId));

    // Saves, settings, logs and caches in per-user folders named by the application ID,
    // never beside the game (S01).
    if (!inputs.userPathsChecked)
        gate("user_paths", GateStatus::Unverified, "S01", "the per-user folders weren't checked");
    else if (!inputs.userPathsProblem.empty())
        gate("user_paths", GateStatus::Failed, "S01", inputs.userPathsProblem);
    else if (inputs.userPathsApplicationId != s.applicationId)
        gate(
            "user_paths", GateStatus::Failed, "S01",
            "the per-user folders are named \"" + inputs.userPathsApplicationId + "\", not the contract's \"" +
                s.applicationId + "\"");
    else
        gate("user_paths", GateStatus::Passed, "S01", "installed, it writes under " + inputs.userPathsRoot);

    // The game's levels (S02): a catalog whose files exist and whose links resolve.
    auto levelCatalogGate = [&]() {
        if (!inputs.levelCatalogChecked)
            gate("level_catalog", GateStatus::Unverified, "S02", "the level catalog wasn't checked");
        else if (inputs.levelCatalogImplicit)
            gate(
                "level_catalog", GateStatus::Failed, "S02",
                "no level catalog: a release lists its levels (the project's levels key, docs/Levels.md)");
        else if (!inputs.levelCatalogProblem.empty())
            gate("level_catalog", GateStatus::Failed, "S02", inputs.levelCatalogProblem);
        else if (!inputs.levelLinksChecked)
            gate(
                "level_catalog", GateStatus::Unverified, "S02",
                std::to_string(inputs.levelCount) + (inputs.levelCount == 1 ? " level" : " levels") +
                    " read and their files exist; the links between them are checked by player --check");
        else
            gate(
                "level_catalog", GateStatus::Passed, "S02",
                std::to_string(inputs.levelCount) + (inputs.levelCount == 1 ? " level" : " levels") + " from entry " +
                    inputs.levelEntry + "; every scene loads and every exit and spawn resolves");
    };
    // In batch order, with the catalog (S02) and the acceptance scenario (S05) in their places.
    for (const PendingGate& pending : kPendingGates) {
        const std::string_view id = pending.id;
        if (id == "level_travel") levelCatalogGate();
        if (pending.online && !online)
            gate(pending.id, GateStatus::NotApplicable, pending.batch, "offline only");
        else
            gate(
                pending.id, GateStatus::Unverified, pending.batch,
                std::string("no evidence yet: needs ") + pending.needs);
        if (id == "level_travel")
            gate(
                "acceptance_scenario", GateStatus::Unverified, "S05",
                inputs.acceptanceScenarioExists
                    ? s.acceptanceScenario + " exists; running it (player --flow-test) arrives with S05"
                    : s.acceptanceScenario + " doesn't exist yet");
    }
    for (const std::string& platform : s.platforms)
        gate(
            "platform:" + platform, GateStatus::Unverified, "S13",
            "no install evidence yet: a clean-machine install, play-through and update on " + platform);
}

ReleaseReport releaseReport(const Project& project, const ReleaseInputs& inputs) {
    ReleaseReport report;
    const std::filesystem::path path = releasePath(project);
    report.file = project.relative(path);
    std::error_code ec;
    report.present = std::filesystem::exists(path, ec);
    if (report.present) {
        ReleaseFile file;
        Diagnostic error;
        if (!readRelease(path, report.file, file, error)) {
            report.diagnostics.push_back(error);
        } else {
            report.readable = true;
            report.settings = file.settings;
            ReleaseInputs withScenario = inputs;
            withScenario.acceptanceScenarioExists =
                std::filesystem::is_regular_file(project.root / file.settings.acceptanceScenario, ec);
            validateRelease(file.settings, &withScenario, report.file, &file, report.diagnostics);
            releaseGates(report, withScenario);
            return report;
        }
    }
    releaseGates(report, inputs);
    return report;
}

std::string releaseReportText(const ReleaseReport& report) {
    std::string out;
    if (!report.present) {
        out += "release none: no " + report.file + " (a development project; release qualification needs one)\n";
    } else {
        out += "release " + report.file + "\n";
        if (report.readable) {
            const std::string text = releaseText(report.settings);
            // The values, without the file's comment and header lines.
            std::istringstream lines(text);
            for (std::string line; std::getline(lines, line);)
                if (!line.empty() && line[0] != '#' && line.rfind("shockwave_release", 0) != 0)
                    out += "  " + line + "\n";
        }
    }
    out += "build " + (report.buildConfig.empty() ? std::string("unknown") : report.buildConfig) + "\n";
    size_t counts[4] = {};
    for (const ReleaseGate& gate : report.gates) {
        ++counts[int(gate.status)];
        out += "gate " + gate.id + " " + gateStatusName(gate.status) + " (" + gate.batch + "): " + gate.reason + "\n";
    }
    for (const Diagnostic& d : report.diagnostics)
        out += std::string(diagnosticSeverityName(d.severity)) + " " + d.text() + " [" + d.code + "]\n";
    out += std::string("qualified ") + (report.qualified() ? "yes" : "no") + ": " + std::to_string(counts[0]) +
           " passed, " + std::to_string(counts[1]) + " failed, " + std::to_string(counts[2]) + " unverified, " +
           std::to_string(counts[3]) + " not applicable\n";
    return out;
}

std::string releaseReportJson(const ReleaseReport& report, std::string_view projectName) {
    auto boolean = [](bool value) { return std::string(value ? "true" : "false"); };
    std::string out = "{\n  \"format\": 1,\n  \"mode\": \"release-info\",\n";
    out += "  \"project\": " + diagnosticJsonString(projectName) + ",\n";
    out += "  \"file\": " + diagnosticJsonString(report.file) + ",\n";
    out += "  \"present\": " + boolean(report.present) + ",\n";
    out += "  \"readable\": " + boolean(report.readable) + ",\n";
    out += "  \"qualified\": " + boolean(report.qualified()) + ",\n";
    out += "  \"build\": {\"config\": " +
           (report.buildConfig.empty() ? std::string("null") : diagnosticJsonString(report.buildConfig)) + "},\n";
    if (report.readable) {
        out += "  \"contract\": {";
        bool first = true;
        for (const ReleaseField& field : kFields) {
            out += std::string(first ? "" : ", ") + diagnosticJsonString(field.key) + ": ";
            first = false;
            const std::vector<std::string> values = releaseFieldText(report.settings, field);
            if (listField(field)) {
                out += "[";
                for (size_t i = 0; i < values.size(); ++i)
                    out += std::string(i ? ", " : "") + diagnosticJsonString(values[i]);
                out += "]";
            } else if (quotedField(field)) {
                out += diagnosticJsonString(report.settings.*std::get<std::string R::*>(field.member));
            } else if (field.kind == ReleaseFieldKind::Choice) {
                out += diagnosticJsonString(values.front());
            } else {
                out += values.front();
            }
        }
        out += "},\n";
    } else {
        out += "  \"contract\": null,\n";
    }
    size_t counts[4] = {};
    for (const ReleaseGate& gate : report.gates) ++counts[int(gate.status)];
    out += "  \"summary\": {\"passed\": " + std::to_string(counts[0]) + ", \"failed\": " + std::to_string(counts[1]) +
           ", \"unverified\": " + std::to_string(counts[2]) + ", \"not_applicable\": " + std::to_string(counts[3]) +
           "},\n";
    out += "  \"gates\": [";
    for (size_t i = 0; i < report.gates.size(); ++i) {
        const ReleaseGate& gate = report.gates[i];
        out += std::string(i ? "," : "") + "\n    {\"id\": " + diagnosticJsonString(gate.id) + ", \"status\": \"" +
               gateStatusName(gate.status) + "\", \"batch\": " + diagnosticJsonString(gate.batch) +
               ", \"reason\": " + diagnosticJsonString(gate.reason) + "}";
    }
    out += report.gates.empty() ? "],\n" : "\n  ],\n";
    out += "  \"diagnostics\": [";
    for (size_t i = 0; i < report.diagnostics.size(); ++i)
        out += std::string(i ? "," : "") + "\n    " + diagnosticJson(report.diagnostics[i]);
    out += report.diagnostics.empty() ? "]\n" : "\n  ]\n";
    return out + "}\n";
}

// ---- Schema -----------------------------------------------------------------------------

std::string releaseSchema() {
    std::string out =
        "# Release contract reference\n\n"
        "<!-- Generated by `player --release-schema docs/ReleaseSchema.md`; don't edit by hand. -->\n\n"
        "`Config/Release.swrelease` says what a project promises to ship. See docs/Release.md for how it's used.\n"
        "A project without it is a development project, and release qualification says so.\n\n"
        "The first line is `shockwave_release 1`. Then one key and value per line; `#` starts a comment.\n"
        "`platform` and `mode` repeat, once per value. Every key is required.\n\n"
        "| Key | Values | Meaning |\n| --- | --- | --- |\n";
    for (const ReleaseField& field : kFields) {
        std::string values;
        switch (field.kind) {
        case ReleaseFieldKind::Id: values = "quoted ID, like `\"com.studio.game\"`"; break;
        case ReleaseFieldKind::Version: values = "quoted version, like `\"1.0.0\"`"; break;
        case ReleaseFieldKind::Path: values = "quoted path to a `.json`"; break;
        case ReleaseFieldKind::Platforms:
        case ReleaseFieldKind::Modes:
        case ReleaseFieldKind::Choice:
            for (size_t i = 0; i < field.choices.size(); ++i)
                values += std::string(i ? ", " : "") + "`" + field.choices[i] + "`";
            if (listField(field)) values += " (one per line)";
            break;
        case ReleaseFieldKind::Integer:
            values = "whole number, " + formatNumber(field.min) + " to " + formatNumber(field.max);
            break;
        case ReleaseFieldKind::Seconds:
            values = "seconds, " + formatNumber(field.min) + " to " + formatNumber(field.max);
            break;
        }
        out += std::string("| `") + field.key + "` | " + values + " | " + field.meaning + " |\n";
    }
    out += "\n## Rules between keys\n\n"
           "- Without `mode online`, `max_players` is 1. With it, `max_players` is even, from 2 to 64.\n"
           "- `loading_budget_mib` is at most `cpu_budget_mib`.\n"
           "- When `shared_budget_mib` isn't 0, `cpu_budget_mib` and `gpu_budget_mib` together fit in it.\n"
           "- `simulation_hz` equals the project's `tick_rate`.\n"
           "- `release_version` equals the package version in `Config/Package.swpackage` (1.0.0 without one).\n"
           "- With `mode online`, `Config/Network.swnetwork` turns multiplayer on with the same `max_players`.\n"
           "  Without it, the network settings don't turn multiplayer on.\n"
           "- `gpu_budget_mib` is at least the project's `graphics_memory_budget_mb`.\n\n"
           "## Gates\n\n"
           "`player --release-info` works out each gate from the files and evidence it can measure; none are\n"
           "ticked by hand. A release is qualified only when every gate that applies has passed.\n\n"
           "| Gate | Batch | Passes when |\n| --- | --- | --- |\n"
           "| `release_contract` | S00 | the contract reads, with every key in range |\n"
           "| `project_settings` | S00 | it matches the project, package, network and graphics settings |\n"
           "| `release_build` | S00 | the engine is a Release, RelWithDebInfo or MinSizeRel build |\n"
           "| `steam_app_id` | S00 | online over Steam with the game's own AppID (not 480) |\n"
           "| `user_paths` | S01 | installed, saves, settings, logs and caches go to per-user folders named by "
           "`application_id`, outside the game |\n";
    for (const PendingGate& pending : kPendingGates) {
        const std::string_view id = pending.id;
        if (id == "level_travel") out += "| `level_catalog` | S02 | the level catalog checks out |\n";
        out += std::string("| `") + pending.id + "` | " + pending.batch + " | evidence of " + pending.needs +
               (pending.online ? " (online only)" : "") + " |\n";
        if (id == "level_travel")
            out += "| `acceptance_scenario` | S05 | the acceptance scenario plays the game from start to finish |\n";
    }
    out += "| `platform:<name>` | S13 | a clean-machine install, play-through and update on that platform |\n\n"
           "Gates that later batches measure stay `unverified` until their evidence exists.\n\n"
           "## Diagnostic codes\n\n"
           "Codes are stable; tools can match them. Messages may change.\n\n"
           "| Code | Severity | Meaning |\n| --- | --- | --- |\n"
           "| `ReleaseHeader` | error | the first line isn't `shockwave_release 1` |\n"
           "| `ReleaseUnsupportedVersion` | error | a contract version this build can't read |\n"
           "| `ReleaseUnreadable` | error | the file can't be opened or read |\n"
           "| `ReleaseUnknownKey` | error | a key this version doesn't have |\n"
           "| `ReleaseDuplicateKey` | error | a key other than `platform` or `mode` given twice |\n"
           "| `ReleaseMissingKey` | error | a required key is left out |\n"
           "| `ReleaseInvalidValue` | error | a value out of range, malformed or listed twice |\n"
           "| `ReleasePlayerCount` | error | `max_players` doesn't fit the modes |\n"
           "| `ReleaseLoadingBudget` | error | `loading_budget_mib` is more than `cpu_budget_mib` |\n"
           "| `ReleaseMemoryBudget` | error | CPU and GPU budgets don't fit in `shared_budget_mib` |\n"
           "| `ReleaseTickRate` | error | `simulation_hz` isn't the project's `tick_rate` |\n"
           "| `ReleaseVersionMismatch` | error | `release_version` isn't the package version |\n"
           "| `ReleaseNetworkMissing` | error | `mode online` without multiplayer settings turning it on |\n"
           "| `ReleaseNetworkInvalid` | error | `mode online`, and the network settings don't read |\n"
           "| `ReleaseNetworkUnexpected` | error | multiplayer on without `mode online` |\n"
           "| `ReleaseNetworkPlayers` | error | `max_players` isn't the network settings' `max_players` |\n"
           "| `ReleaseGraphicsBudget` | error | `gpu_budget_mib` is under `graphics_memory_budget_mb` |\n"
           "| `ReleaseDevelopmentAppId` | warning | online over Steam AppID 480, Valve's test app |\n"
           "| `ReleaseScenarioMissing` | warning | the acceptance scenario's file doesn't exist yet |\n";
    return out;
}

}   // namespace shockwave
