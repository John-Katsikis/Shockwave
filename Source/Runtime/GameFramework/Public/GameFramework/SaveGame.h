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
// A save also carries the campaign (game.campaign(), progress that outlives a level), the
// level it was made in, and who it belongs to (SaveIdentity). Loading a save from another
// level travels there first. Files are written so a crash never loses the last good save,
// and a slot written by another copy of the game since is never overwritten (Conflict).
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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

class Assets;
struct SaveJobs;

// Why a save can't be read, loaded or written: stable codes for UI, the command line and
// reports. Messages start with the name ("Corrupt: ..."); the rest may change.
enum class SaveError {
    None,
    NotASave,   // no save_game line: a scene, or something else
    Corrupt,   // the checksum doesn't match, or the file is cut short
    TooLarge,   // more than the reader's limits (kMaxSaveBytes, kMaxSaveValues, ...)
    NewerFormat,   // a save_game version this build doesn't know
    NewerSchema,   // the game's save data is newer than the game (save_schema)
    WrongGame,   // another application's save
    WrongProfile,   // another profile's save
    MigrationFailed,   // the game's migration refused it
    Conflict,   // another writer saved the slot since this one read it
    WriteFailed,   // the disk said no: full, read-only, no permission
    Busy,   // two saves are already being written
    UnavailableDuringNetworkPlay,
    Invalid,   // a bad slot name, saving off, a malformed line
};
const char* saveErrorName(SaveError error);   // "None", "Corrupt", ... as in the enum
// The code a message from this file starts with, None when it starts with none.
SaveError saveErrorOf(std::string_view message);

// The reader's limits: anything bigger is refused before it's parsed.
inline constexpr size_t kMaxSaveBytes = size_t(256) << 20;
inline constexpr size_t kMaxSaveValues = 4096;   // values and campaign values, each
inline constexpr size_t kMaxSaveValueBytes = size_t(64) << 10;
inline constexpr size_t kMaxSaveEntities = size_t(1) << 22;

// What a save file says about itself: everything above its first entity.
struct SaveInfo {
    std::string slot;   // the file name without .swsave
    std::filesystem::path file;
    int format = 0;   // save_game N: 1 for the old snapshot-only saves, 2 from S06
    std::string application;   // the game that wrote it (format 2)
    int schema = 0;   // the game's save data schema when it was written (format 2)
    std::string content;   // the release version that wrote it; for information only
    std::string level;   // the level it was saved in (format 2), "" for older saves
    std::string spawn;   // the spawn that level was entered at
    std::string checkpoint;   // the checkpoint it restarts at ("" for now: S06 part 2)
    std::string profile;   // the player profile it belongs to
    uint64_t revision = 0;   // counts the slot's writes: 1 for the first
    bool checksum = false;   // it carries a checksum, and it matched
    std::string title;   // what the game called it, "" when nothing
    std::string scene;   // the scene Play started from, project-relative
    std::string savedAt;   // UTC, "2026-10-11 10:30:00"
    double playTime = 0;   // seconds of Play when it was saved
    size_t entities = 0;
    std::map<std::string, std::string> values;   // SaveGames::set: the level's
    std::map<std::string, std::string> campaign;   // CampaignState: the whole game's
    // Only in listSaveGames(folder, true): why it can't load ("Corrupt: ..."), else "".
    std::string problem;
};

// A save read into memory, ready to replace a level's world.
struct SaveRestore {
    World world;
    SaveInfo info;
};

// Steps a save's data up from one schema to the next: fromSchema to fromSchema + 1. Runs
// on a job worker with detached data, so it must touch nothing but `save` (its values,
// campaign, title...). False, with error, refuses the save.
using SaveMigration = std::function<bool(int fromSchema, SaveInfo& save, std::string& error)>;

// Which game and player saves belong to (docs/SaveGames.md#compatibility).
struct SaveIdentity {
    std::string application;   // refuses other games' saves; "" accepts any
    std::string profile;   // refuses other profiles' saves; "" accepts any
    int schema = 1;   // the game's save data schema now (the project's save_schema)
    std::string content;   // the release version, written into saves for information
    SaveMigration migrate;   // unset: older schemas load unchanged
};

// Whether this game and profile can load the save: WrongGame, WrongProfile, NewerSchema.
// Format 1 saves carry no identity and are always accepted.
bool checkSaveCompatible(const SaveInfo& save, const SaveIdentity& identity, std::string& error);
// Runs the game's migrations on save, from its schema to identity.schema, one step at a
// time. Its schema says how far it got. Does nothing when it's already current.
bool migrateSave(SaveInfo& save, const SaveIdentity& identity, std::string& error);

// Slot names: 1 to 64 letters, digits, spaces, '-' and '_', not starting with a space.
bool validSaveSlot(std::string_view slot);
// Value keys: 1 to 64 letters, digits, '_', '-' and '.'.
bool validSaveKey(std::string_view key);

// Writes world as a save file (format 2, with a checksum). Any thread, as long as nobody
// changes world meanwhile.
void writeSaveGame(std::ostream& out, const World& world, const Assets& assets, const SaveInfo& info);
// Upgrades a save's text to format 2 and the game's current schema (player --save-migrate):
// its header is rewritten with the migrated values and a new checksum; its entities stay
// exactly as they were. upgrade, when set, fills in what an older save lacks (its level,
// from its scene). False, with the code, when it can't be read or migrated.
bool migrateSaveText(
    std::string& text, const std::string& path, const SaveIdentity& identity,
    const std::function<void(SaveInfo&)>& upgrade, SaveInfo& result, std::string& error);
// Writes text to file so a crash or a full disk never leaves a half-written save: a
// unique temporary file beside it, flushed to disk, then swapped in with an atomic
// rename. The file it replaces is kept as <file>.bak first. False, with WriteFailed.
bool writeSaveFile(const std::filesystem::path& file, std::string_view text, std::string& error);
// For tests: called at each step of writeSaveFile ("open", "write", "flush", "backup",
// "replace", "directory"); returning true makes that step fail as if the disk had.
void setSaveFaultForTesting(std::function<bool(std::string_view step)> fault);
// Reads a save file into world (entities keep their saved handles) and info. Any thread.
// warnings collects things that loaded with a problem; path only names it in messages.
bool readSaveGame(
    std::istream& in, const std::string& path, const Assets& assets, World& world, SaveInfo& info, std::string& error,
    std::vector<std::string>* warnings = nullptr);
// Only the header, and the checksum: for lists of saves. False if the file isn't a save
// game, or is damaged.
bool readSaveInfo(const std::filesystem::path& file, SaveInfo& info, std::string& error);
// Every .swsave in folder, newest first. Files that aren't save games, or are damaged,
// are left out; with broken, they're listed too, with SaveInfo::problem saying why.
std::vector<SaveInfo> listSaveGames(const std::filesystem::path& folder, bool broken = false);
// The newest save in folder this game and profile can load (Continue), or nothing. why,
// when given, says why a newer one was passed over ("Corrupt: ...").
std::optional<SaveInfo>
newestCompatibleSave(const std::filesystem::path& folder, const SaveIdentity& identity, std::string* why = nullptr);
// Reads, checks and migrates a save: what load() does on its worker, for the shell's
// Continue and tools. Any thread.
bool readSaveRestore(
    const std::filesystem::path& file, const Assets& assets, const SaveIdentity& identity, SaveRestore& out,
    std::string& error, std::vector<std::string>* warnings = nullptr);

// Copying save games from an older location into a profile (docs/UserPaths.md): each slot
// is checked first, copied (never moved), and never written over one the profile already
// has. preview reports what would happen and changes nothing.
struct SaveImport {
    enum class Action { Copy, Keep, Fail };
    struct Entry {
        std::string slot;
        Action action = Action::Copy;
        std::string reason;   // why it was kept or failed
    };
    std::vector<Entry> entries;   // by slot name
    size_t copied = 0, kept = 0, failed = 0;
};
// False, with error, only when `from` isn't a folder, is `to`, or `to` can't be made.
bool importSaveGames(
    const std::filesystem::path& from, const std::filesystem::path& to, bool preview, SaveImport& result,
    std::string& error);

// What save(), load(), loadFile() and remove() fail with during network play.
inline constexpr const char* kUnavailableDuringNetworkPlay =
    "UnavailableDuringNetworkPlay: saving and loading are off during network play";

// Named values kept as text: a save's values and the campaign's. Keys use letters,
// digits, '_', '-' and '.'.
class SaveValues {
public:
    virtual ~SaveValues() = default;
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
    bool erase(std::string_view key);
    const std::map<std::string, std::string>& values() const { return m_values; }

protected:
    virtual void badKey(std::string message) { (void)message; }
    std::map<std::string, std::string> m_values;
};

// game.campaign(): progress that belongs to the whole game, not one level: unlocked
// levels, collected keys, the story so far. It survives travel, goes into every save and
// comes back with a load. New game (after the confirm) clears it; Restart level puts it
// back as it was when the level was entered, so a reward taken since isn't kept twice.
// LevelHost owns it; a session without one has its own.
class CampaignState final : public SaveValues {
public:
    void replace(std::map<std::string, std::string> values) { m_values = std::move(values); }
    void clear() { m_values.clear(); }
    bool empty() const { return m_values.empty(); }

private:
    void badKey(std::string message) override;
};

// The save game service of one Play session, as game.saves(). PlaySession creates it.
class SaveGames final : public Subsystem, public SaveValues {
public:
    // folder: where slots live; empty means saving is off (tools, tests), and save() and
    // load() fail with a message. assets null: a built-in set with only "white".
    SaveGames(
        World& world, JobSystem& jobs, const Assets* assets, std::filesystem::path folder, std::string scene = {});
    ~SaveGames() override;   // waits for saves still being written

    // Snapshots the world now and writes it to the slot on a worker. False (and a
    // logged reason) when the name is bad, saving is off, or two saves are in flight.
    // Writes happen in order. A slot another writer saved since this one last read or
    // wrote it isn't overwritten: the save fails with Conflict, and what it would have
    // written is kept beside it as <slot>.swsave.conflict.
    bool save(std::string_view slot, std::string_view title = {});
    // Reads the slot on a worker; the world is replaced at the start of the next tick
    // (or frame, while paused). A save from another level travels there first, through
    // the loading screen (reason LoadSave). False when the slot doesn't exist.
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
    // The last thing that went wrong, "" when the last save and load worked. It starts
    // with its SaveError code ("Conflict: ...").
    std::string lastError() const;

    // ---- Values saved alongside the world ----
    // For game state that isn't in components: a score, the level's own state. They go
    // into every save from now on and are replaced by the saved ones on load. Progress
    // that should outlive the level goes in game.campaign() instead.
    // (set, get, getInt, getNumber, getBool, has: SaveValues)

    // Seconds of Play so far, carried over from the loaded save.
    double playTime() const { return m_playTime; }

    // ---- For PlaySession ----
    void addPlayTime(double dt) { m_playTime += dt; }
    // Network play: save and load fail with kUnavailableDuringNetworkPlay before they
    // touch a file or the world.
    void setNetworkPlay(bool online) { m_networkPlay = online; }
    bool networkPlay() const { return m_networkPlay; }
    // The game and profile saves belong to, and the game's migrations.
    void setIdentity(SaveIdentity identity) { m_identity = std::move(identity); }
    const SaveIdentity& identity() const { return m_identity; }
    // The level playing, and the spawn it was entered at: written into saves.
    void setLevel(std::string level, std::string spawn) {
        m_level = std::move(level);
        m_spawn = std::move(spawn);
    }
    // The campaign saves carry, and a load replaces. Null: none.
    void setCampaign(CampaignState* campaign) { m_campaign = campaign; }
    // Called on the main thread just before a save snapshots the world.
    std::function<void()> beforeSave;
    // A finished load, taken out to apply (or travel with); nothing when none is waiting.
    std::optional<SaveRestore> takeLoaded();
    // Puts a loaded save in: the world, values, play time and campaign. The session then
    // rebuilds physics and calls GameMode::afterLoad.
    void apply(SaveRestore restore);
    // takeLoaded, then apply. True when it did.
    bool applyLoaded();
    void fail(std::string message);

private:
    World& m_world;
    JobSystem& m_jobs;
    std::unique_ptr<Assets> m_ownAssets;
    const Assets* m_assets;
    std::filesystem::path m_folder;
    std::string m_scene;
    std::string m_level, m_spawn;
    SaveIdentity m_identity;
    CampaignState* m_campaign = nullptr;
    bool m_networkPlay = false;
    double m_playTime = 0;
    void badKey(std::string message) override { fail(std::move(message)); }

    // Workers hand results back through these.
    std::shared_ptr<SaveJobs> m_shared;
};

}   // namespace shockwave
