#pragma once

// Save games: the running world, written as a scene file with a few extra header lines.
//
//     game.saves().save("slot1", "Chapter 2");   // snapshot now, written on a worker
//     game.saves().load("slot1");                // swapped in at the start of the next tick
//     game.saves().set("coins", coins);          // extra values, saved with the world
//     coins = game.saves().getInt("coins");      // ...and back after a load
//
// What's saved: every entity with every registered component (SW_COMPONENT fields), so
// a component's saved fields are its save data. Fields not listed in SW_COMPONENT aren't
// saved. Entities come back with the same handles, so EntityRefs game code holds stay
// valid after a load. Physics bodies are rebuilt from the loaded components (velocities
// included); GameMode::afterLoad runs once the new world is in.
//
// Files are <project>/Saved/SaveGames/<slot>.swsave. They're ordinary .swscene text that
// starts with save_game lines, so they open in the editor and `player --scene`, diff
// cleanly, and `player --check` validates them. docs/SaveGames.md has the format.
//
// Threading: save, load, remove and the values belong to the main thread (GameMode
// hooks, UI handlers), never systems. The slow parts, turning the world into text, file
// writes, reading and parsing, run on job workers, so a save doesn't hitch the frame.

#include <Engine/Subsystem.h>
#include <Engine/World.h>

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

class Assets;
struct SaveJobs;

// What a save file says about itself: everything above its first entity.
struct SaveInfo {
    std::string slot;   // the file name without .swsave
    std::filesystem::path file;
    std::string title;   // what the game called it, "" when nothing
    std::string scene;   // the scene Play started from, project-relative
    std::string savedAt;   // UTC, "2026-10-11 10:30:00"
    double playTime = 0;   // seconds of Play when it was saved
    size_t entities = 0;
    std::map<std::string, std::string> values;   // SaveGames::set
};

// Slot names: 1 to 64 letters, digits, spaces, '-' and '_', not starting with a space.
bool validSaveSlot(std::string_view slot);
// Value keys: 1 to 64 letters, digits, '_', '-' and '.'.
bool validSaveKey(std::string_view key);

// Writes world as a save file. Any thread, as long as nobody changes world meanwhile.
void writeSaveGame(std::ostream& out, const World& world, const Assets& assets, const SaveInfo& info);
// Reads a save file into world (entities keep their saved handles) and info. Any thread.
// warnings collects things that loaded with a problem; path only names it in messages.
bool readSaveGame(
    std::istream& in, const std::string& path, const Assets& assets, World& world, SaveInfo& info, std::string& error,
    std::vector<std::string>* warnings = nullptr);
// Only the header: fast, for lists of saves. False if the file isn't a save game.
bool readSaveInfo(const std::filesystem::path& file, SaveInfo& info, std::string& error);
// Every .swsave in folder, newest first. Files that aren't save games are left out.
std::vector<SaveInfo> listSaveGames(const std::filesystem::path& folder);

// What save(), load(), loadFile() and remove() fail with during network play.
inline constexpr const char* kUnavailableDuringNetworkPlay =
    "UnavailableDuringNetworkPlay: saving and loading are off during network play";

// The save game service of one Play session, as game.saves(). PlaySession creates it.
class SaveGames final : public Subsystem {
public:
    // folder: where slots live; empty means saving is off (tools, tests), and save() and
    // load() fail with a message. assets null: a built-in set with only "white".
    SaveGames(
        World& world, JobSystem& jobs, const Assets* assets, std::filesystem::path folder, std::string scene = {});
    ~SaveGames() override;   // waits for saves still being written

    // Snapshots the world now and writes it to the slot on a worker. False (and a
    // logged reason) when the name is bad or saving is off.
    bool save(std::string_view slot, std::string_view title = {});
    // Reads the slot on a worker; the world is replaced at the start of the next tick
    // (or frame, while paused). False when the slot doesn't exist.
    bool load(std::string_view slot);
    bool loadFile(const std::filesystem::path& file);   // any .swsave, by path
    bool remove(std::string_view slot);
    bool exists(std::string_view slot) const;
    std::vector<SaveInfo> list() const;   // newest first
    std::filesystem::path fileFor(std::string_view slot) const;
    const std::filesystem::path& folder() const { return m_folder; }

    // Something is still being written or read.
    bool busy() const;
    // Blocks until everything in flight is done (tools and tests).
    void wait();
    // The last thing that went wrong, "" when the last save and load worked.
    std::string lastError() const;

    // ---- Values saved alongside the world ----
    // For game state that isn't in components: a score, unlocked levels. They go into
    // every save from now on and are replaced by the saved ones on load.
    void set(std::string_view key, std::string_view value);
    void set(std::string_view key, const char* value) { set(key, std::string_view(value)); }
    void set(std::string_view key, int value);
    void set(std::string_view key, double value);
    void set(std::string_view key, float value) { set(key, double(value)); }
    void set(std::string_view key, bool value);
    bool has(std::string_view key) const;
    std::string get(std::string_view key, std::string_view fallback = {}) const;
    int getInt(std::string_view key, int fallback = 0) const;
    double getNumber(std::string_view key, double fallback = 0) const;
    bool getBool(std::string_view key, bool fallback = false) const;
    const std::map<std::string, std::string>& values() const { return m_values; }

    // Seconds of Play so far, carried over from the loaded save.
    double playTime() const { return m_playTime; }

    // ---- For PlaySession ----
    void addPlayTime(double dt) { m_playTime += dt; }
    // Network play: save and load fail with kUnavailableDuringNetworkPlay before they
    // touch a file or the world.
    void setNetworkPlay(bool online) { m_networkPlay = online; }
    bool networkPlay() const { return m_networkPlay; }
    // Called on the main thread just before a save snapshots the world.
    std::function<void()> beforeSave;
    // Swaps a finished load into the world. True when it did; the session then rebuilds
    // physics and calls GameMode::afterLoad.
    bool applyLoaded();

private:
    void fail(std::string message);

    World& m_world;
    JobSystem& m_jobs;
    std::unique_ptr<Assets> m_ownAssets;
    const Assets* m_assets;
    std::filesystem::path m_folder;
    std::string m_scene;
    std::map<std::string, std::string> m_values;
    bool m_networkPlay = false;
    double m_playTime = 0;

    // Workers hand results back through these.
    std::shared_ptr<SaveJobs> m_shared;
};

}   // namespace shockwave
