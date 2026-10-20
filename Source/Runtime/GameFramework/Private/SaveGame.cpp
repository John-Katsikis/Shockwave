#include <GameFramework/SaveGame.h>

#include <Assets/Assets.h>
#include <Core/Jobs.h>
#include <Core/Log.h>
#include <Engine/SceneFile.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>

namespace shockwave {

namespace fs = std::filesystem;

namespace {

constexpr int kSaveVersion = 1;
constexpr size_t kIdsPerLine = 32;

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
    auto fail = [&](const std::string& what) {
        error = path + ":" + std::to_string(lineNumber) + ": " + what;
        return false;
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
            continue;
        }
        if (inEntities) continue;
        if (key == "save_game") {
            int version = 0;
            if (!(ls >> version) || version < 1 || version > kSaveVersion) return fail("unsupported save_game version");
            isSave = true;
        } else if (key == "title") {
            ls >> std::quoted(info.title);
        } else if (key == "scene") {
            ls >> std::quoted(info.scene);
        } else if (key == "saved_at") {
            ls >> std::quoted(info.savedAt);
        } else if (key == "play_time") {
            if (!(ls >> info.playTime) || info.playTime < 0) return fail("play_time takes seconds");
        } else if (key == "entities") {
            ls >> info.entities;
        } else if (key == "value") {
            std::string name, value;
            if (!(ls >> name >> std::quoted(value)) || !validSaveKey(name))
                return fail("value takes a key (letters, digits, _ - .) and a quoted text");
            info.values[name] = value;
        } else if (key == "entity_ids" && ids) {
            std::string word;
            while (ls >> word) {
                Entity e;
                if (!parseId(word, e)) return fail("bad entity id '" + word + "' (index or index:generation)");
                ids->push_back(e);
            }
        }
    }
    if (!isSave) {
        lineNumber = 1;
        return fail("not a save game (no save_game line)");
    }
    return true;
}

}   // namespace

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

void writeSaveGame(std::ostream& out, const World& world, const Assets& assets, const SaveInfo& info) {
    SceneFileHooks hooks;
    // Every entity in full, prefab parts included: a save holds the game as it is, and
    // entity_ids needs one block per entity.
    hooks.exact = true;
    hooks.writeHeader = [&](std::ostream& o) {
        o << "save_game " << kSaveVersion << "\n";
        if (!info.title.empty()) o << "title " << std::quoted(info.title) << "\n";
        if (!info.scene.empty()) o << "scene " << std::quoted(info.scene) << "\n";
        if (!info.savedAt.empty()) o << "saved_at " << std::quoted(info.savedAt) << "\n";
        o << "play_time " << std::fixed << std::setprecision(3) << info.playTime << std::defaultfloat
          << std::setprecision(6) << "\n";
        o << "entities " << world.entityCount() << "\n";
        for (const auto& [key, value] : info.values) o << "value " << key << ' ' << std::quoted(value) << "\n";
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
    writeScene(out, world, assets, hooks);
}

bool readSaveGame(
    std::istream& in, const std::string& path, const Assets& assets, World& world, SaveInfo& info, std::string& error,
    std::vector<std::string>* warnings) {
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    SaveInfo header;
    std::vector<Entity> ids;
    size_t blocks = 0;
    {
        std::istringstream scan(text);
        if (!readHeader(scan, path, header, &ids, blocks, error)) return false;
    }
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
    std::ifstream in(file);
    if (!in) {
        error = "can't open " + file.string();
        return false;
    }
    SaveInfo result;
    size_t blocks = 0;
    if (!readHeader(in, file.string(), result, nullptr, blocks, error)) return false;
    result.slot = file.stem().string();
    result.file = file;
    result.entities = blocks;
    info = std::move(result);
    return true;
}

std::vector<SaveInfo> listSaveGames(const fs::path& folder) {
    std::vector<SaveInfo> saves;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".swsave") continue;
        SaveInfo info;
        std::string error;
        if (readSaveInfo(entry.path(), info, error)) saves.push_back(std::move(info));
    }
    // Newest first; saved_at sorts as text. Ties (or hand-written files) by name.
    std::sort(saves.begin(), saves.end(), [](const SaveInfo& a, const SaveInfo& b) {
        return a.savedAt != b.savedAt ? a.savedAt > b.savedAt : a.slot < b.slot;
    });
    return saves;
}

// ---- SaveGames ----------------------------------------------------------------------------

// What workers share with the main thread. Held by shared_ptr so a job never outlives it.
struct SaveJobs {
    JobCounter counter;
    std::mutex mutex;   // guards everything below
    std::mutex fileMutex;   // one file written at a time
    struct Loaded {
        World world;
        SaveInfo info;
    };
    std::optional<Loaded> loaded;
    uint64_t latestLoad = 0;   // only the newest load request is applied
    std::string lastError;
};

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

bool SaveGames::save(std::string_view slot, std::string_view title) {
    if (m_networkPlay) {
        fail(kUnavailableDuringNetworkPlay);
        return false;
    }
    if (m_folder.empty()) {
        fail("saving is off here (no save folder)");
        return false;
    }
    if (!validSaveSlot(slot)) {
        fail("'" + std::string(slot) + "' isn't a slot name: use letters, digits, spaces, - and _");
        return false;
    }
    if (beforeSave) beforeSave();
    SaveInfo info;
    info.slot = slot;
    info.file = fileFor(slot);
    info.title = title;
    info.scene = m_scene;
    info.savedAt = nowUtc();
    info.playTime = m_playTime;
    info.entities = m_world.entityCount();
    info.values = m_values;
    // The copy is the only part on this thread; it's what makes the save consistent.
    auto snapshot = std::make_shared<const World>(m_world);
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->lastError.clear();
    }
    m_jobs.run(
        [shared = m_shared, snapshot, info = std::move(info), assets = m_assets] {
            std::ostringstream text;
            writeSaveGame(text, *snapshot, *assets, info);
            std::lock_guard files(shared->fileMutex);
            std::error_code ec;
            fs::create_directories(info.file.parent_path(), ec);
            // Through a temporary file, so a crash mid-write never eats the old save.
            const fs::path temp = info.file.string() + ".tmp";
            bool ok = false;
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                out << text.str();
                ok = static_cast<bool>(out.flush());
            }
            if (ok) fs::rename(temp, info.file, ec);
            if (ok && !ec) {
                SW_LOG(LogGame, Info, "Saved {} ({} entities)", info.file.string(), info.entities);
                return;
            }
            fs::remove(temp, ec);
            const std::string message = "couldn't write " + info.file.string();
            SW_LOG(LogGame, Warning, "Save games: {}", message);
            std::lock_guard lock(shared->mutex);
            shared->lastError = message;
        },
        &m_shared->counter);
    return true;
}

bool SaveGames::load(std::string_view slot) {
    if (m_networkPlay) {
        fail(kUnavailableDuringNetworkPlay);
        return false;
    }
    if (!validSaveSlot(slot)) {
        fail("'" + std::string(slot) + "' isn't a slot name");
        return false;
    }
    if (m_folder.empty()) {
        fail("saving is off here (no save folder)");
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
        [shared = m_shared, file, id, assets = m_assets] {
            std::string error;
            std::vector<std::string> warnings;
            SaveJobs::Loaded loaded;
            std::ifstream in(file, std::ios::binary);
            const bool ok = in && readSaveGame(in, file.string(), *assets, loaded.world, loaded.info, error, &warnings);
            for (const std::string& warning : warnings) SW_LOG(LogGame, Warning, "{}", warning);
            loaded.info.slot = file.stem().string();
            loaded.info.file = file;
            std::lock_guard lock(shared->mutex);
            if (!ok) {
                shared->lastError = error.empty() ? "can't open " + file.string() : error;
                SW_LOG(LogGame, Warning, "Save games: {}", shared->lastError);
                return;
            }
            if (id == shared->latestLoad) shared->loaded = std::move(loaded);
        },
        &m_shared->counter);
    return true;
}

bool SaveGames::applyLoaded() {
    std::optional<SaveJobs::Loaded> loaded;
    {
        std::lock_guard lock(m_shared->mutex);
        if (!m_shared->loaded) return false;
        loaded.swap(m_shared->loaded);
    }
    m_world = std::move(loaded->world);
    m_values = std::move(loaded->info.values);
    m_playTime = loaded->info.playTime;
    SW_LOG(LogGame, Info, "Loaded {} ({} entities)", loaded->info.file.string(), m_world.entityCount());
    return true;
}

bool SaveGames::remove(std::string_view slot) {
    if (m_networkPlay) {
        fail(kUnavailableDuringNetworkPlay);
        return false;
    }
    if (m_folder.empty() || !validSaveSlot(slot)) return false;
    std::lock_guard files(m_shared->fileMutex);
    std::error_code ec;
    return fs::remove(fileFor(slot), ec);
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

void SaveGames::set(std::string_view key, std::string_view value) {
    if (!validSaveKey(key)) {
        fail("'" + std::string(key) + "' isn't a value key: use letters, digits, _ - and .");
        return;
    }
    m_values[std::string(key)] = value;
}
void SaveGames::set(std::string_view key, int value) { set(key, std::string_view(std::to_string(value))); }
void SaveGames::set(std::string_view key, double value) {
    char text[32];
    const auto [end, ec] = std::to_chars(text, text + sizeof(text), value);
    set(key, std::string_view(text, ec == std::errc{} ? size_t(end - text) : 0));
}
void SaveGames::set(std::string_view key, bool value) { set(key, std::string_view(value ? "true" : "false")); }

bool SaveGames::has(std::string_view key) const { return m_values.find(std::string(key)) != m_values.end(); }

std::string SaveGames::get(std::string_view key, std::string_view fallback) const {
    const auto found = m_values.find(std::string(key));
    return found != m_values.end() ? found->second : std::string(fallback);
}

int SaveGames::getInt(std::string_view key, int fallback) const {
    const std::string text = get(key);
    int value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec == std::errc{} && end == text.data() + text.size() && !text.empty()) return value;
    const double number = getNumber(key, fallback);   // "3.0" still reads as 3
    return static_cast<int>(number);
}

double SaveGames::getNumber(std::string_view key, double fallback) const {
    const std::string text = get(key);
    double value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size() && !text.empty() ? value : fallback;
}

bool SaveGames::getBool(std::string_view key, bool fallback) const {
    const std::string text = get(key);
    if (text == "true" || text == "1" || text == "yes") return true;
    if (text == "false" || text == "0" || text == "no") return false;
    return fallback;
}

}   // namespace shockwave
