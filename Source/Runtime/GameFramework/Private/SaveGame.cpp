#include <GameFramework/SaveGame.h>

#include <Assets/Assets.h>
#include <Core/Jobs.h>
#include <Core/Log.h>
#include <Engine/Levels.h>
#include <Engine/SceneFile.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace shockwave {

namespace fs = std::filesystem;

namespace {

constexpr int kSaveVersion = 2;
constexpr size_t kIdsPerLine = 32;
constexpr const char* kChecksumPlaceholder = "checksum 0000000000000000\n";

bool isWordChar(char c, bool dot) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           (dot && c == '.');
}

std::string nowUtc() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &utc);
    return text;
}

std::string coded(SaveError code, const std::string& message) {
    return std::string(saveErrorName(code)) + ": " + message;
}

// FNV-1a, 64 bits: catches a damaged or cut-short file. Not a signature: anyone can
// recompute it, and offline saves are meant to stay editable.
uint64_t fnv(std::string_view text, uint64_t hash = 14695981039346656037ull) {
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex16(uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

// Where the header's checksum line is: [begin, end) including its newline, or npos. Only
// above the first entity block.
std::pair<size_t, size_t> findChecksumLine(std::string_view text) {
    size_t at = 0;
    while (at < text.size()) {
        const size_t eol = text.find('\n', at);
        const size_t next = eol == std::string_view::npos ? text.size() : eol + 1;
        const std::string_view line = text.substr(at, next - at);
        if (line.rfind("entity", 0) == 0 && (line.size() == 6 || line[6] == ' ' || line[6] == '\n')) break;
        if (line.rfind("checksum ", 0) == 0) return {at, next};
        at = next;
    }
    return {std::string_view::npos, std::string_view::npos};
}

uint64_t checksumWithout(std::string_view text, std::pair<size_t, size_t> line) {
    if (line.first == std::string_view::npos) return fnv(text);
    return fnv(text.substr(line.second), fnv(text.substr(0, line.first)));
}

// The checksum line, if there is one, against the rest of the file.
bool verifyChecksum(std::string_view text, const std::string& path, bool& had, std::string& error) {
    had = false;
    const auto line = findChecksumLine(text);
    if (line.first == std::string_view::npos) return true;
    std::string_view value = text.substr(line.first + 9, line.second - line.first - 9);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' '))
        value.remove_suffix(1);
    uint64_t stored = 0;
    const auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), stored, 16);
    if (ec != std::errc{} || p != value.data() + value.size()) {
        error = coded(SaveError::Corrupt, path + ": the checksum line isn't 16 hex digits");
        return false;
    }
    if (stored != checksumWithout(text, line)) {
        error = coded(
            SaveError::Corrupt, path + ": the checksum doesn't match: the file is damaged, cut short or was edited "
                                       "(after editing a save by hand, delete its checksum line)");
        return false;
    }
    had = true;
    return true;
}

// "5" for {5, 0}, "5:2" for {5, 2}.
bool parseId(const std::string& word, Entity& e) {
    const size_t colon = word.find(':');
    const char* end = word.data() + (colon == std::string::npos ? word.size() : colon);
    auto [p, ec] = std::from_chars(word.data(), end, e.index);
    if (ec != std::errc{} || p != end || e.index == UINT32_MAX) return false;
    e.generation = 0;
    if (colon == std::string::npos) return true;
    const char* last = word.data() + word.size();
    auto [q, ec2] = std::from_chars(word.data() + colon + 1, last, e.generation);
    return ec2 == std::errc{} && q == last;
}

// The header lines of a save (everything before the first entity block), and how many
// entity blocks follow. Shared by the full load and the quick info read.
bool readHeader(
    std::istream& in, const std::string& path, SaveInfo& info, std::vector<Entity>* ids, size_t& blocks,
    std::string& error) {
    std::string line;
    int lineNumber = 0;
    bool isSave = false, inEntities = false;
    blocks = 0;
    auto fail = [&](const std::string& what, SaveError code = SaveError::Invalid) {
        error = coded(code, path + ":" + std::to_string(lineNumber) + ": " + what);
        return false;
    };
    auto quotedValue = [&](std::istringstream& ls, std::map<std::string, std::string>& into, const char* what) {
        std::string name, value;
        if (!(ls >> name >> std::quoted(value)) || !validSaveKey(name))
            return fail(std::string(what) + " takes a key (letters, digits, _ - .) and a quoted text");
        if (value.size() > kMaxSaveValueBytes) return fail(name + " is longer than 64 KiB", SaveError::TooLarge);
        if (into.size() >= kMaxSaveValues && !into.count(name))
            return fail("more than " + std::to_string(kMaxSaveValues) + " " + what + "s", SaveError::TooLarge);
        into[name] = value;
        return true;
    };
    while (std::getline(in, line)) {
        ++lineNumber;
        if (line.empty() || line[0] == ' ' || line[0] == '\t') continue;
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key) || key[0] == '#') continue;
        if (key == "entity") {
            inEntities = true;
            ++blocks;
            if (!ids) {   // the quick read only needs the count: skip the parsing below
                while (std::getline(in, line))
                    if (line.rfind("entity", 0) == 0 && (line.size() == 6 || line[6] == ' ')) ++blocks;
                break;
            }
            if (blocks > kMaxSaveEntities)
                return fail("more than " + std::to_string(kMaxSaveEntities) + " entities", SaveError::TooLarge);
            continue;
        }
        if (inEntities) continue;
        if (key == "save_game") {
            int version = 0;
            if (!(ls >> version) || version < 1) return fail("save_game takes a version number", SaveError::NotASave);
            if (version > kSaveVersion)
                return fail(
                    "save_game " + std::to_string(version) + " is newer than this build reads (up to " +
                        std::to_string(kSaveVersion) + ")",
                    SaveError::NewerFormat);
            info.format = version;
            isSave = true;
        } else if (key == "application") {
            ls >> std::quoted(info.application);
        } else if (key == "schema") {
            if (!(ls >> info.schema) || info.schema < 1) return fail("schema takes a version from 1");
        } else if (key == "content") {
            ls >> std::quoted(info.content);
        } else if (key == "level") {
            ls >> std::quoted(info.level);
        } else if (key == "spawn") {
            ls >> std::quoted(info.spawn);
        } else if (key == "checkpoint") {
            ls >> std::quoted(info.checkpoint);
        } else if (key == "profile") {
            ls >> std::quoted(info.profile);
        } else if (key == "fresh") {
            info.fresh = true;
        } else if (key == "campaign_id") {
            ls >> std::quoted(info.campaignId);
        } else if (key == "completed") {
            std::string id;
            if (!(ls >> std::quoted(id)) || !validSaveKey(id))
                return fail("completed takes an id (letters, digits, _ - .)");
            if (info.completed.size() >= kMaxSaveValues)
                return fail("more than " + std::to_string(kMaxSaveValues) + " completions", SaveError::TooLarge);
            info.completed.insert(id);
        } else if (key == "destroyed") {
            std::string level, id;
            if (!(ls >> level)) return fail("destroyed takes a level and PersistentIdentity ids");
            auto& ids = info.destroyed[level];
            while (ls >> id) {
                if (ids.size() >= kMaxSaveEntities)
                    return fail("more than " + std::to_string(kMaxSaveEntities) + " destroyed", SaveError::TooLarge);
                ids.insert(id);
            }
        } else if (key == "revision") {
            if (!(ls >> info.revision)) return fail("revision takes a count");
        } else if (key == "title") {
            ls >> std::quoted(info.title);
        } else if (key == "scene") {
            ls >> std::quoted(info.scene);
        } else if (key == "saved_at") {
            ls >> std::quoted(info.savedAt);
        } else if (key == "play_time") {
            if (!(ls >> info.playTime) || !std::isfinite(info.playTime) || info.playTime < 0)
                return fail("play_time takes seconds");
        } else if (key == "entities") {
            ls >> info.entities;
            if (info.entities > kMaxSaveEntities)
                return fail("more than " + std::to_string(kMaxSaveEntities) + " entities", SaveError::TooLarge);
        } else if (key == "value") {
            if (!quotedValue(ls, info.values, "value")) return false;
        } else if (key == "campaign") {
            if (!quotedValue(ls, info.campaign, "campaign value")) return false;
        } else if (key == "entity_ids" && ids) {
            std::string word;
            while (ls >> word) {
                Entity e;
                if (!parseId(word, e)) return fail("bad entity id '" + word + "' (index or index:generation)");
                if (ids->size() >= kMaxSaveEntities)
                    return fail("more than " + std::to_string(kMaxSaveEntities) + " entities", SaveError::TooLarge);
                ids->push_back(e);
            }
        }
    }
    if (!isSave) {
        lineNumber = 1;
        return fail("not a save game (no save_game line)", SaveError::NotASave);
    }
    if (info.format < 2 && info.schema == 0) info.schema = 1;   // older saves: the game's first schema
    return true;
}

bool readText(const fs::path& file, std::string& text, std::string& error) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) {
        error = "can't open " + file.string();
        return false;
    }
    if (size > kMaxSaveBytes) {
        error = coded(SaveError::TooLarge, file.string() + " is bigger than 256 MiB");
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "can't open " + file.string();
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::mutex g_faultMutex;
std::function<bool(std::string_view)> g_fault;

bool faulted(std::string_view step) {
    std::lock_guard lock(g_faultMutex);
    return g_fault && g_fault(step);
}

// Pushes a file's (or folder's) data to the disk, where the platform has a call for it.
bool syncFile(std::FILE* f) {
    if (std::fflush(f) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return ::fsync(::fileno(f)) == 0;
#endif
}

bool syncFolder(const fs::path& folder) {
#ifdef _WIN32
    (void)folder;   // NTFS: the rename is journaled; there's no folder handle to flush
    return true;
#else
    const int fd = ::open(folder.c_str(), O_RDONLY);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#endif
}

std::string uniqueSuffix() {
    static std::atomic<uint64_t> counter{0};
    const uint64_t now = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    std::random_device random;
    return hex16(fnv(std::to_string(now) + ":" + std::to_string(random()) + ":" + std::to_string(++counter)))
        .substr(0, 12);
}

}   // namespace

const char* saveErrorName(SaveError error) {
    switch (error) {
    case SaveError::None: return "None";
    case SaveError::NotASave: return "NotASave";
    case SaveError::Corrupt: return "Corrupt";
    case SaveError::TooLarge: return "TooLarge";
    case SaveError::NewerFormat: return "NewerFormat";
    case SaveError::NewerSchema: return "NewerSchema";
    case SaveError::WrongGame: return "WrongGame";
    case SaveError::WrongProfile: return "WrongProfile";
    case SaveError::MigrationFailed: return "MigrationFailed";
    case SaveError::Conflict: return "Conflict";
    case SaveError::WriteFailed: return "WriteFailed";
    case SaveError::Busy: return "Busy";
    case SaveError::NoCheckpoint: return "NoCheckpoint";
    case SaveError::UnavailableDuringNetworkPlay: return "UnavailableDuringNetworkPlay";
    case SaveError::Invalid: return "Invalid";
    }
    return "Invalid";
}

SaveError saveErrorOf(std::string_view message) {
    for (int i = 1; i <= int(SaveError::Invalid); ++i) {
        const std::string_view name = saveErrorName(SaveError(i));
        if (message.size() > name.size() && message.substr(0, name.size()) == name && message[name.size()] == ':')
            return SaveError(i);
    }
    return SaveError::None;
}

bool validSaveSlot(std::string_view slot) {
    if (slot.empty() || slot.size() > 64 || slot.front() == ' ') return false;
    for (char c : slot)
        if (!isWordChar(c, false) && c != ' ') return false;
    return true;
}

bool validSaveKey(std::string_view key) {
    if (key.empty() || key.size() > 64) return false;
    for (char c : key)
        if (!isWordChar(c, true)) return false;
    return true;
}

bool checkSaveCompatible(const SaveInfo& save, const SaveIdentity& identity, std::string& error) {
    const std::string what = save.slot.empty() ? std::string("the save") : "save \"" + save.slot + "\"";
    if (save.format >= 2 && !identity.application.empty() && !save.application.empty() &&
        save.application != identity.application) {
        error = coded(SaveError::WrongGame, what + " belongs to " + save.application + ", not " + identity.application);
        return false;
    }
    if (save.format >= 2 && !identity.profile.empty() && !save.profile.empty() && save.profile != identity.profile) {
        error = coded(
            SaveError::WrongProfile,
            what + " belongs to profile \"" + save.profile + "\", not \"" + identity.profile + "\"");
        return false;
    }
    if (save.schema > identity.schema) {
        error = coded(
            SaveError::NewerSchema, what + " was written by a newer version of the game (save schema " +
                                        std::to_string(save.schema) + "; this one reads up to " +
                                        std::to_string(identity.schema) + ")");
        return false;
    }
    return true;
}

bool migrateSave(SaveInfo& save, const SaveIdentity& identity, std::string& error) {
    if (save.schema > identity.schema) return checkSaveCompatible(save, identity, error);
    while (save.schema < identity.schema) {
        std::string why;
        bool ok = true;
        if (identity.migrate) {
            try {
                ok = identity.migrate(save.schema, save, why);
            } catch (const std::exception& e) {
                ok = false;
                why = e.what();
            }
        }
        if (!ok) {
            error = coded(
                SaveError::MigrationFailed, "the game's migration from save schema " + std::to_string(save.schema) +
                                                " refused it" + (why.empty() ? std::string() : ": " + why));
            return false;
        }
        ++save.schema;
    }
    return true;
}

namespace {

// Every header line but entity_ids, in the order writeSaveGame writes them.
void writeHeaderLines(std::ostream& o, const SaveInfo& info, size_t entities) {
    o << "save_game " << kSaveVersion << "\n";
    o << kChecksumPlaceholder;   // filled in once the rest is known
    if (!info.application.empty()) o << "application " << std::quoted(info.application) << "\n";
    o << "schema " << std::max(info.schema, 1) << "\n";
    if (!info.content.empty()) o << "content " << std::quoted(info.content) << "\n";
    if (!info.profile.empty()) o << "profile " << std::quoted(info.profile) << "\n";
    o << "revision " << info.revision << "\n";
    if (!info.level.empty()) o << "level " << std::quoted(info.level) << "\n";
    if (!info.spawn.empty()) o << "spawn " << std::quoted(info.spawn) << "\n";
    if (!info.checkpoint.empty()) o << "checkpoint " << std::quoted(info.checkpoint) << "\n";
    if (info.fresh) o << "fresh\n";
    if (!info.title.empty()) o << "title " << std::quoted(info.title) << "\n";
    if (!info.scene.empty()) o << "scene " << std::quoted(info.scene) << "\n";
    if (!info.savedAt.empty()) o << "saved_at " << std::quoted(info.savedAt) << "\n";
    o << "play_time " << std::fixed << std::setprecision(3) << info.playTime << std::defaultfloat
      << std::setprecision(6) << "\n";
    o << "entities " << entities << "\n";
    if (!info.campaignId.empty()) o << "campaign_id " << std::quoted(info.campaignId) << "\n";
    for (const auto& [key, value] : info.campaign) o << "campaign " << key << ' ' << std::quoted(value) << "\n";
    for (const std::string& id : info.completed) o << "completed " << std::quoted(id) << "\n";
    for (const auto& [level, ids] : info.destroyed) {
        size_t n = 0;
        for (const std::string& id : ids) {   // wrapped like entity_ids
            if (n % kIdsPerLine == 0) o << (n ? "\n" : "") << "destroyed " << level;
            o << ' ' << id;
            ++n;
        }
        if (n) o << "\n";
    }
    for (const auto& [key, value] : info.values) o << "value " << key << ' ' << std::quoted(value) << "\n";
}

void fillChecksum(std::string& text) {
    const auto line = findChecksumLine(text);
    if (line.first != std::string::npos)
        text.replace(line.first, line.second - line.first, "checksum " + hex16(checksumWithout(text, line)) + "\n");
}

}   // namespace

void writeSaveGame(std::ostream& out, const World& world, const Assets& assets, const SaveInfo& info) {
    SceneFileHooks hooks;
    // Every entity in full, prefab parts included: a save holds the game as it is, and
    // entity_ids needs one block per entity.
    hooks.exact = true;
    hooks.writeHeader = [&](std::ostream& o) {
        writeHeaderLines(o, info, world.entityCount());
        // The handle of each entity block below, in order, so references game code
        // holds still point at the same things after loading.
        size_t n = 0;
        world.forEachEntity([&](Entity e) {
            o << (n % kIdsPerLine == 0 ? (n ? "\nentity_ids" : "entity_ids") : "") << ' ' << e.index;
            if (e.generation) o << ':' << e.generation;
            ++n;
        });
        if (n) o << "\n";
    };
    std::ostringstream body;
    writeScene(body, world, assets, hooks);
    std::string text = std::move(body).str();
    fillChecksum(text);
    out << text;
}

bool migrateSaveText(
    std::string& text, const std::string& path, const SaveIdentity& identity,
    const std::function<void(SaveInfo&)>& upgrade, SaveInfo& result, std::string& error) {
    if (text.size() > kMaxSaveBytes) {
        error = coded(SaveError::TooLarge, path + " is bigger than 256 MiB");
        return false;
    }
    SaveInfo info;
    size_t blocks = 0;
    {
        std::istringstream in(text);
        if (!readHeader(in, path, info, nullptr, blocks, error)) return false;
    }
    if (!verifyChecksum(text, path, info.checksum, error)) return false;
    if (!checkSaveCompatible(info, identity, error) || !migrateSave(info, identity, error)) return false;
    if (upgrade) upgrade(info);
    if (info.application.empty()) info.application = identity.application;
    if (info.profile.empty()) info.profile = identity.profile;
    if (!identity.content.empty()) info.content = identity.content;
    info.format = kSaveVersion;
    // The entities and anything else above them stay as they were; only the save's own
    // header lines are replaced.
    static const std::set<std::string> ownKeys = {
        "save_game", "checksum", "application", "schema", "content",     "profile",   "revision",
        "level",     "spawn",    "checkpoint",  "title",  "scene",       "saved_at",  "play_time",
        "entities",  "value",    "campaign",    "fresh",  "campaign_id", "completed", "destroyed"};
    std::ostringstream header;
    writeHeaderLines(header, info, blocks);
    std::string out;
    out.reserve(text.size() + 256);
    bool placed = false, inEntities = false;
    size_t at = 0;
    while (at < text.size()) {
        const size_t eol = text.find('\n', at);
        const size_t next = eol == std::string::npos ? text.size() : eol + 1;
        const std::string_view line(text.data() + at, next - at);
        at = next;
        if (!inEntities && line.rfind("entity", 0) == 0 && (line.size() == 6 || line[6] == ' ' || line[6] == '\n'))
            inEntities = true;
        if (!inEntities && !line.empty() && line[0] != ' ' && line[0] != '\t') {
            const size_t end = line.find_first_of(" \t\r\n");
            const std::string key(line.substr(0, end == std::string_view::npos ? line.size() : end));
            if (ownKeys.count(key)) continue;
            if (!placed && (key == "shockwave_scene" || key == "entity_ids")) {
                if (key == "entity_ids") out += header.str();
                out += line;
                if (line.back() != '\n') out += '\n';
                if (key == "shockwave_scene") out += header.str();
                placed = true;
                continue;
            }
        }
        if (!placed && inEntities) {
            out += header.str();
            placed = true;
        }
        out += line;
    }
    if (!placed) out += header.str();
    fillChecksum(out);
    text = std::move(out);
    info.entities = blocks;
    info.checksum = true;
    result = std::move(info);
    return true;
}

bool writeSaveFile(const fs::path& file, std::string_view text, std::string& error) {
    auto failed = [&](const std::string& what) {
        error = coded(SaveError::WriteFailed, "couldn't write " + file.string() + ": " + what);
        return false;
    };
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    // Unique, so two writers never share a temporary file.
    const fs::path temp = file.string() + "." + uniqueSuffix() + ".tmp";
    {
        const bool refused = faulted("open");
        std::FILE* f = nullptr;
        if (!refused) {
#ifdef _WIN32
            f = _wfopen(temp.c_str(), L"wb");   // the wide path: user folders can have any name
#else
            f = std::fopen(temp.c_str(), "wb");
#endif
        }
        if (!f)
            return failed(
                "can't create a file there" +
                (refused ? std::string() : " (" + std::string(std::strerror(errno)) + ")"));
        const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size() && !faulted("write");
        const bool synced = written && syncFile(f) && !faulted("flush");
        const bool closed = std::fclose(f) == 0;
        if (!written || !synced || !closed) {
            fs::remove(temp, ec);
            return failed(!written ? "the disk is full or refused the data" : "the data didn't reach the disk");
        }
    }
    // The last known good save, kept before it's replaced.
    if (fs::is_regular_file(file, ec)) {
        const fs::path backup = file.string() + ".bak";
        const fs::path backupTemp = backup.string() + "." + uniqueSuffix() + ".tmp";
        bool ok = !faulted("backup") && fs::copy_file(file, backupTemp, fs::copy_options::overwrite_existing, ec);
        if (ok) {
            fs::rename(backupTemp, backup, ec);
            ok = !ec;
        }
        if (!ok) {
            fs::remove(backupTemp, ec);
            fs::remove(temp, ec);
            return failed("couldn't keep a backup of the old save first");
        }
    }
    // The atomic step: the new file replaces the old one whole, or not at all.
    if (faulted("replace")) {
        fs::remove(temp, ec);
        return failed("couldn't replace the old file");
    }
    fs::rename(temp, file, ec);
    if (ec) {
        const std::string why = ec.message();
        fs::remove(temp, ec);
        return failed("couldn't replace the old file (" + why + ")");
    }
    // Past this point the save is in: a failure to flush the folder is only a warning.
    if (faulted("directory") || !syncFolder(file.parent_path().empty() ? fs::path(".") : file.parent_path()))
        SW_LOG(LogGame, Warning, "Save games: {} is written, but its folder couldn't be flushed", file.string());
    return true;
}

void setSaveFaultForTesting(std::function<bool(std::string_view step)> fault) {
    std::lock_guard lock(g_faultMutex);
    g_fault = std::move(fault);
}

bool readSaveGame(
    std::istream& in, const std::string& path, const Assets& assets, World& world, SaveInfo& info, std::string& error,
    std::vector<std::string>* warnings) {
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (text.size() > kMaxSaveBytes) {
        error = coded(SaveError::TooLarge, path + " is bigger than 256 MiB");
        return false;
    }
    SaveInfo header;
    std::vector<Entity> ids;
    size_t blocks = 0;
    {
        std::istringstream scan(text);
        if (!readHeader(scan, path, header, &ids, blocks, error)) return false;
    }
    if (!verifyChecksum(text, path, header.checksum, error)) return false;
    SceneFileHooks hooks;
    hooks.readHeader = [](const std::string&, std::istream&) {};   // read above
    // writeSaveGame writes prefab parts out in full; a hand-written save that only names
    // its prefabs gets them made the way a scene does.
    hooks.exact = text.find("\n  PrefabPart\n") != std::string::npos;
    World skeleton;
    if (!ids.empty()) {
        if (ids.size() != blocks) {
            error = path + ": entity_ids lists " + std::to_string(ids.size()) + " entities but the file has " +
                    std::to_string(blocks) + "; remove the entity_ids lines to load with new handles";
            return false;
        }
        skeleton = World::withEntities(ids);
        if (skeleton.entityCount() != ids.size()) {
            error = path + ": entity_ids lists an entity twice";
            return false;
        }
        hooks.entities = &skeleton;
    }
    std::istringstream body(text);
    World result;
    if (!readScene(result, assets, body, path, error, hooks, warnings)) return false;
    header.entities = result.entityCount();
    world = std::move(result);
    info = std::move(header);
    return true;
}

bool readSaveInfo(const fs::path& file, SaveInfo& info, std::string& error) {
    std::string text;
    if (!readText(file, text, error)) return false;
    SaveInfo result;
    size_t blocks = 0;
    std::istringstream in(text);
    if (!readHeader(in, file.string(), result, nullptr, blocks, error)) return false;
    if (!verifyChecksum(text, file.string(), result.checksum, error)) return false;
    result.slot = file.stem().string();
    result.file = file;
    result.entities = blocks;
    info = std::move(result);
    return true;
}

std::vector<SaveInfo> listSaveGames(const fs::path& folder, bool broken) {
    std::vector<SaveInfo> saves;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".swsave") continue;
        SaveInfo info;
        std::string error;
        if (readSaveInfo(entry.path(), info, error)) {
            saves.push_back(std::move(info));
        } else if (broken) {
            SaveInfo bad;
            bad.slot = entry.path().stem().string();
            bad.file = entry.path();
            bad.problem = error.empty() ? "can't read it" : error;
            saves.push_back(std::move(bad));
        }
    }
    // Newest first; saved_at sorts as text. Ties (or hand-written files) by name.
    std::sort(saves.begin(), saves.end(), [](const SaveInfo& a, const SaveInfo& b) {
        return a.savedAt != b.savedAt ? a.savedAt > b.savedAt : a.slot < b.slot;
    });
    return saves;
}

std::optional<SaveInfo> newestCompatibleSave(const fs::path& folder, const SaveIdentity& identity, std::string* why) {
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return std::nullopt;
    // Damaged files have no saved_at to sort by: any of them is reported as passed over.
    std::string passed;
    for (const SaveInfo& save : listSaveGames(folder, true)) {
        std::string error = save.problem;
        if (error.empty() && checkSaveCompatible(save, identity, error)) {
            if (why) *why = passed;
            return save;
        }
        if (passed.empty()) passed = "\"" + save.slot + "\" can't be loaded: " + error;
    }
    if (why) *why = passed;
    return std::nullopt;
}

bool readSaveRestore(
    const fs::path& file, const Assets& assets, const SaveIdentity& identity, SaveRestore& out, std::string& error,
    std::vector<std::string>* warnings) {
    auto read = [&](const fs::path& from, SaveRestore& into, std::string& why) {
        std::string text;
        if (!readText(from, text, why)) return false;
        std::istringstream in(text);
        return readSaveGame(in, from.string(), assets, into.world, into.info, why, warnings);
    };
    SaveRestore result;
    if (!read(file, result, error)) {
        // A damaged slot falls back to the backup its last write kept, saying so.
        const fs::path backup = file.string() + ".bak";
        std::error_code ec;
        std::string backupError;
        SaveRestore older;
        if (saveErrorOf(error) != SaveError::Corrupt || !fs::is_regular_file(backup, ec) ||
            !read(backup, older, backupError))
            return false;
        const std::string note = error + "; loaded the backup from before its last save instead";
        SW_LOG(LogGame, Warning, "Save games: {}", note);
        if (warnings) warnings->push_back(note);
        result = std::move(older);
        error.clear();
    }
    result.info.slot = file.stem().string();
    result.info.file = file;
    if (!checkSaveCompatible(result.info, identity, error) || !migrateSave(result.info, identity, error)) return false;
    out = std::move(result);
    return true;
}

// ---- SaveGames ----------------------------------------------------------------------------

// What workers share with the main thread. Held by shared_ptr so a job never outlives it.
struct SaveJobs {
    JobCounter counter;
    std::mutex mutex;   // guards everything below
    std::optional<SaveRestore> loaded;
    uint64_t latestLoad = 0;   // only the newest load request is applied
    std::string lastError;
    // Saves waiting to be written, in order, by one writer job at a time.
    struct Write {
        std::shared_ptr<const World> snapshot;
        SaveInfo info;
        const Assets* assets = nullptr;
        bool checkpoint = false;   // the checkpoint's state follows it
    };
    std::deque<Write> writes;
    bool writing = false;
    size_t inFlight = 0;   // queued and being written
    // The revision each slot had when this session last read or wrote it.
    std::map<std::string, uint64_t> revisions;
    SaveGames::CheckpointState checkpoint = SaveGames::CheckpointState::None;
    std::string checkpointId;
};

namespace {

constexpr size_t kMaxSavesInFlight = 2;

// One save: the slot's revision is checked against what this session last saw, then the
// file is written durably. Called by the writer job, one at a time.
bool writeOne(SaveJobs& shared, SaveJobs::Write& job) {
    SaveInfo& info = job.info;
    std::error_code ec;
    std::optional<uint64_t> expected;
    {
        std::lock_guard lock(shared.mutex);
        if (const auto it = shared.revisions.find(info.slot); it != shared.revisions.end()) expected = it->second;
    }
    SaveInfo disk;
    std::string ignored;
    const bool exists = fs::is_regular_file(info.file, ec);
    const bool readable = exists && readSaveInfo(info.file, disk, ignored);
    info.revision = (readable ? disk.revision : 0) + 1;
    std::ostringstream text;
    writeSaveGame(text, *job.snapshot, *job.assets, info);
    std::string error;
    if (readable && expected && disk.revision != *expected) {
        // Someone else saved the slot since: keep both, overwrite neither.
        const fs::path aside = info.file.string() + ".conflict";
        std::string asideError;
        const bool kept = writeSaveFile(aside, text.str(), asideError);
        error = coded(
            SaveError::Conflict, "slot \"" + info.slot + "\" was saved by another writer (revision " +
                                     std::to_string(disk.revision) + ", this game last saw " +
                                     std::to_string(*expected) + "); " +
                                     (kept ? "this save is kept as " + aside.string() : "and " + asideError));
    } else if (writeSaveFile(info.file, text.str(), error)) {
        SW_LOG(LogGame, Info, "Saved {} ({} entities, revision {})", info.file.string(), info.entities, info.revision);
        std::lock_guard lock(shared.mutex);
        shared.revisions[info.slot] = info.revision;
        return true;
    }
    SW_LOG(LogGame, Warning, "Save games: {}", error);
    std::lock_guard lock(shared.mutex);
    shared.lastError = error;
    return false;
}

}   // namespace

SaveGames::SaveGames(World& world, JobSystem& jobs, const Assets* assets, fs::path folder, std::string scene)
    : m_world(world), m_jobs(jobs), m_ownAssets(assets ? nullptr : std::make_unique<Assets>()),
      m_assets(assets ? assets : m_ownAssets.get()), m_folder(std::move(folder)), m_scene(std::move(scene)),
      m_shared(std::make_shared<SaveJobs>()) {}

SaveGames::~SaveGames() { wait(); }

void SaveGames::fail(std::string message) {
    SW_LOG(LogGame, Warning, "Save games: {}", message);
    std::lock_guard lock(m_shared->mutex);
    m_shared->lastError = std::move(message);
}

fs::path SaveGames::fileFor(std::string_view slot) const { return m_folder / (std::string(slot) + ".swsave"); }

bool SaveGames::canSave(std::string_view slot) {
    if (m_networkPlay) {
        fail(kUnavailableDuringNetworkPlay);
        return false;
    }
    if (m_folder.empty()) {
        fail(coded(SaveError::Invalid, "saving is off here (no save folder)"));
        return false;
    }
    if (!validSaveSlot(slot)) {
        fail(coded(
            SaveError::Invalid, "'" + std::string(slot) + "' isn't a slot name: use letters, digits, spaces, - and _"));
        return false;
    }
    std::lock_guard lock(m_shared->mutex);
    if (m_shared->inFlight >= kMaxSavesInFlight) {
        m_shared->lastError = coded(SaveError::Busy, "two saves are still being written; try again shortly");
        SW_LOG(LogGame, Warning, "Save games: {}", m_shared->lastError);
        return false;
    }
    return true;
}

SaveInfo SaveGames::envelope(std::string_view slot, std::string_view title) const {
    SaveInfo info;
    info.slot = slot;
    info.file = fileFor(slot);
    info.format = 2;
    info.application = m_identity.application;
    info.schema = std::max(m_identity.schema, 1);
    info.content = m_identity.content;
    info.profile = m_identity.profile;
    info.level = m_level;
    info.spawn = m_spawn;
    info.title = title;
    info.scene = m_scene;
    info.savedAt = nowUtc();
    info.playTime = m_playTime;
    info.values = m_values;
    if (m_campaign) m_campaign->write(info);
    return info;
}

void SaveGames::enqueue(SaveInfo info, std::shared_ptr<const World> snapshot, bool checkpoint) {
    info.entities = snapshot->entityCount();
    bool start = false;
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->lastError.clear();
        if (checkpoint) {
            m_shared->checkpoint = CheckpointState::Saving;
            m_shared->checkpointId = info.checkpoint;
        }
        m_shared->writes.push_back({std::move(snapshot), std::move(info), m_assets, checkpoint});
        ++m_shared->inFlight;
        start = !m_shared->writing;
        m_shared->writing = true;
    }
    if (start)
        m_jobs.run(
            [shared = m_shared] {
                for (;;) {
                    SaveJobs::Write job;
                    {
                        std::lock_guard lock(shared->mutex);
                        if (shared->writes.empty()) {
                            shared->writing = false;
                            return;
                        }
                        job = std::move(shared->writes.front());
                        shared->writes.pop_front();
                    }
                    const bool ok = writeOne(*shared, job);
                    std::lock_guard lock(shared->mutex);
                    --shared->inFlight;
                    // Only once it's on disk is it the place Restart checkpoint goes back to.
                    if (job.checkpoint && shared->checkpointId == job.info.checkpoint)
                        shared->checkpoint = ok ? CheckpointState::Saved : CheckpointState::Failed;
                }
            },
            &m_shared->counter);
}

bool SaveGames::save(std::string_view slot, std::string_view title) {
    if (!canSave(slot)) return false;
    if (beforeSave) beforeSave();
    SaveInfo info = envelope(slot, title);
    // The copy is the only part on this thread; it's what makes the save consistent.
    enqueue(std::move(info), std::make_shared<const World>(m_world), false);
    return true;
}

bool SaveGames::checkpoint(std::string_view id) {
    Entity found = kNullEntity;
    const Checkpoint* point = nullptr;
    m_world.each<const Checkpoint>([&](Entity e, const Checkpoint& c) {
        if (c.id == id && !point) {
            found = e;
            point = &c;
        }
    });
    if (!point || !point->enabled) {
        fail(coded(
            SaveError::Invalid, std::string(point ? "the Checkpoint \"" : "this level has no Checkpoint \"") +
                                    std::string(id) + (point ? "\" is disabled" : "\"")));
        return false;
    }
    (void)found;
    if (!canSave(kCheckpointSlot)) return false;
    if (beforeSave) beforeSave();
    SaveInfo info = envelope(kCheckpointSlot, "Checkpoint");
    info.checkpoint = id;
    if (!point->spawn.empty()) info.spawn = point->spawn;
    enqueue(std::move(info), std::make_shared<const World>(m_world), true);
    return true;
}

SaveGames::CheckpointState SaveGames::checkpointState() const {
    std::lock_guard lock(m_shared->mutex);
    return m_shared->checkpoint;
}

std::string SaveGames::checkpointId() const {
    std::lock_guard lock(m_shared->mutex);
    return m_shared->checkpointId;
}

bool SaveGames::hasCheckpoint(std::string* why) const {
    auto no = [&](std::string reason) {
        if (why) *why = std::move(reason);
        return false;
    };
    if (m_folder.empty()) return no("Saving is off here");
    SaveInfo info;
    std::string error;
    std::error_code ec;
    if (!fs::is_regular_file(fileFor(kCheckpointSlot), ec)) return no("No checkpoint reached yet");
    if (!readSaveInfo(fileFor(kCheckpointSlot), info, error)) return no("The checkpoint can't be read: " + error);
    if (!checkSaveCompatible(info, m_identity, error)) return no("The checkpoint can't be loaded: " + error);
    if (info.checkpoint.empty() || (m_campaign && info.campaignId != m_campaign->id()))
        return no("No checkpoint reached in this game yet");
    if (why) why->clear();
    return true;
}

bool SaveGames::restartCheckpoint() {
    std::string why;
    if (!hasCheckpoint(&why)) {
        fail(coded(SaveError::NoCheckpoint, why));
        return false;
    }
    return loadFile(fileFor(kCheckpointSlot));
}

bool SaveGames::saveTransition(std::string_view slot, std::string level, std::string spawn) {
    if (!canSave(slot)) return false;
    if (beforeSave) beforeSave();
    SaveInfo info = envelope(slot, "Autosave");
    info.level = std::move(level);
    info.spawn = std::move(spawn);
    info.fresh = true;   // the next level as authored: nothing of this one's world
    info.values.clear();
    info.scene.clear();
    info.playTime = m_playTime;
    enqueue(std::move(info), std::make_shared<const World>(), false);
    return true;
}

bool SaveGames::load(std::string_view slot) {
    if (m_networkPlay) {
        fail(kUnavailableDuringNetworkPlay);
        return false;
    }
    if (!validSaveSlot(slot)) {
        fail(coded(SaveError::Invalid, "'" + std::string(slot) + "' isn't a slot name"));
        return false;
    }
    if (m_folder.empty()) {
        fail(coded(SaveError::Invalid, "saving is off here (no save folder)"));
        return false;
    }
    return loadFile(fileFor(slot));
}

bool SaveGames::loadFile(const fs::path& file) {
    if (m_networkPlay) {
        fail(kUnavailableDuringNetworkPlay);
        return false;
    }
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        fail("there's no save " + file.string());
        return false;
    }
    uint64_t id;
    {
        std::lock_guard lock(m_shared->mutex);
        id = ++m_shared->latestLoad;
        m_shared->lastError.clear();
    }
    m_jobs.run(
        [shared = m_shared, file, id, assets = m_assets, identity = m_identity] {
            std::string error;
            std::vector<std::string> warnings;
            SaveRestore loaded;
            const bool ok = readSaveRestore(file, *assets, identity, loaded, error, &warnings);
            for (const std::string& warning : warnings) SW_LOG(LogGame, Warning, "{}", warning);
            std::lock_guard lock(shared->mutex);
            if (!ok) {
                shared->lastError = error.empty() ? "can't open " + file.string() : error;
                SW_LOG(LogGame, Warning, "Save games: {}", shared->lastError);
                return;
            }
            shared->revisions[loaded.info.slot] = loaded.info.revision;
            if (id == shared->latestLoad) shared->loaded = std::move(loaded);
        },
        &m_shared->counter);
    return true;
}

std::optional<SaveRestore> SaveGames::takeLoaded() {
    std::optional<SaveRestore> loaded;
    std::lock_guard lock(m_shared->mutex);
    loaded.swap(m_shared->loaded);
    return loaded;
}

void SaveGames::apply(SaveRestore restore) {
    if (!restore.info.fresh) {   // a fresh save keeps the level as authored
        restore.world.setLevelEpoch(m_world.levelEpoch());   // the same level, loaded: refs stay valid
        m_world = std::move(restore.world);
    }
    m_values = std::move(restore.info.values);
    m_playTime = restore.info.playTime;
    // Older saves carry no campaign: what the game has now stays.
    if (m_campaign && restore.info.format >= 2) m_campaign->read(restore.info);
    {
        std::lock_guard lock(m_shared->mutex);
        if (!restore.info.slot.empty()) m_shared->revisions[restore.info.slot] = restore.info.revision;
    }
    SW_LOG(LogGame, Info, "Loaded {} ({} entities)", restore.info.file.string(), m_world.entityCount());
}

bool SaveGames::applyLoaded() {
    std::optional<SaveRestore> loaded = takeLoaded();
    if (!loaded) return false;
    apply(std::move(*loaded));
    return true;
}

bool SaveGames::remove(std::string_view slot) {
    if (m_networkPlay) {
        fail(kUnavailableDuringNetworkPlay);
        return false;
    }
    if (m_folder.empty() || !validSaveSlot(slot)) return false;
    wait();   // not under a write
    std::error_code ec;
    const bool removed = fs::remove(fileFor(slot), ec);
    fs::remove(fileFor(slot).string() + ".bak", ec);
    std::lock_guard lock(m_shared->mutex);
    m_shared->revisions.erase(std::string(slot));
    return removed;
}

bool SaveGames::exists(std::string_view slot) const {
    std::error_code ec;
    return !m_folder.empty() && validSaveSlot(slot) && fs::is_regular_file(fileFor(slot), ec);
}

std::vector<SaveInfo> SaveGames::list() const {
    return m_folder.empty() ? std::vector<SaveInfo>{} : listSaveGames(m_folder);
}

bool SaveGames::busy() const { return !m_shared->counter.done(); }

void SaveGames::wait() { m_jobs.wait(m_shared->counter); }

std::string SaveGames::lastError() const {
    std::lock_guard lock(m_shared->mutex);
    return m_shared->lastError;
}

// ---- Values ----

void SaveValues::set(std::string_view key, std::string_view value) {
    if (!validSaveKey(key)) {
        badKey(
            coded(SaveError::Invalid, "'" + std::string(key) + "' isn't a value key: use letters, digits, _ - and ."));
        return;
    }
    m_values[std::string(key)] = value;
}
void SaveValues::set(std::string_view key, int value) { set(key, std::string_view(std::to_string(value))); }
void SaveValues::set(std::string_view key, double value) {
    char text[32];
    const auto [end, ec] = std::to_chars(text, text + sizeof(text), value);
    set(key, std::string_view(text, ec == std::errc{} ? size_t(end - text) : 0));
}
void SaveValues::set(std::string_view key, bool value) { set(key, std::string_view(value ? "true" : "false")); }

bool SaveValues::has(std::string_view key) const { return m_values.find(std::string(key)) != m_values.end(); }

bool SaveValues::erase(std::string_view key) { return m_values.erase(std::string(key)) > 0; }

std::string SaveValues::get(std::string_view key, std::string_view fallback) const {
    const auto found = m_values.find(std::string(key));
    return found != m_values.end() ? found->second : std::string(fallback);
}

int SaveValues::getInt(std::string_view key, int fallback) const {
    const std::string text = get(key);
    int value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec == std::errc{} && end == text.data() + text.size() && !text.empty()) return value;
    const double number = getNumber(key, fallback);   // "3.0" still reads as 3
    return static_cast<int>(number);
}

double SaveValues::getNumber(std::string_view key, double fallback) const {
    const std::string text = get(key);
    double value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size() && !text.empty() ? value : fallback;
}

bool SaveValues::getBool(std::string_view key, bool fallback) const {
    const std::string text = get(key);
    if (text == "true" || text == "1" || text == "yes") return true;
    if (text == "false" || text == "0" || text == "no") return false;
    return fallback;
}

void CampaignState::badKey(std::string message) { SW_LOG(LogGame, Warning, "Campaign: {}", message); }

CampaignState::CampaignState() : m_id(newPersistentId()) {}

bool CampaignState::complete(std::string_view id) {
    if (!validSaveKey(id)) {
        badKey(coded(
            SaveError::Invalid, "'" + std::string(id) + "' isn't a completion id: use letters, digits, _ - and ."));
        return false;
    }
    return m_completed.insert(std::string(id)).second;
}

const std::set<std::string>& CampaignState::destroyed(const std::string& level) const {
    static const std::set<std::string> none;
    const auto it = m_destroyed.find(level);
    return it != m_destroyed.end() ? it->second : none;
}

void CampaignState::addDestroyed(const std::string& level, const std::set<std::string>& ids) {
    if (!ids.empty()) m_destroyed[level].insert(ids.begin(), ids.end());
}

void CampaignState::clear() {
    m_values.clear();
    m_completed.clear();
    m_destroyed.clear();
    m_id = newPersistentId();
}

void CampaignState::write(SaveInfo& save) const {
    save.campaign = m_values;
    save.campaignId = m_id;
    save.completed = m_completed;
    save.destroyed = m_destroyed;
}

void CampaignState::read(const SaveInfo& save) {
    m_values = save.campaign;
    m_completed = save.completed;
    m_destroyed = save.destroyed;
    if (!save.campaignId.empty()) m_id = save.campaignId;
}

bool importSaveGames(
    const std::filesystem::path& from, const std::filesystem::path& to, bool preview, SaveImport& result,
    std::string& error) {
    namespace fs = std::filesystem;
    result = {};
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return error = from.string() + " isn't a folder of save games", false;
    if (fs::equivalent(from, to, ec)) return error = from.string() + " is already the profile's save folder", false;
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(from, ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".swsave") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    if (!preview && !files.empty()) {
        fs::create_directories(to, ec);
        if (!fs::is_directory(to, ec))
            return error = "can't create " + to.string() + (ec ? ": " + ec.message() : std::string()), false;
    }
    for (const fs::path& file : files) {
        SaveImport::Entry entry;
        entry.slot = file.stem().string();
        SaveInfo info;
        std::string why;
        const fs::path target = to / file.filename();
        if (!validSaveSlot(entry.slot)) {
            entry.action = SaveImport::Action::Fail;
            entry.reason = "not a slot name";
        } else if (!readSaveInfo(file, info, why)) {
            entry.action = SaveImport::Action::Fail;
            entry.reason = why;
        } else if (fs::exists(target, ec)) {
            // Never over a slot already in the profile: it may well be newer.
            entry.action = SaveImport::Action::Keep;
            entry.reason = "the profile already has this slot";
        } else if (!preview) {
            // Copied, never moved: the original stays where it was.
            fs::copy_file(file, target, fs::copy_options::none, ec);
            if (ec) {
                entry.action = SaveImport::Action::Fail;
                entry.reason = ec.message();
            }
        }
        ++(entry.action == SaveImport::Action::Copy   ? result.copied
           : entry.action == SaveImport::Action::Keep ? result.kept
                                                      : result.failed);
        result.entries.push_back(std::move(entry));
    }
    error.clear();
    return true;
}

}   // namespace shockwave
