// Campaign saves (S06 part 1, docs/SaveGames.md, section 14 of the shipping plan), without
// a window. One program, three ctest names:
//   save_travel     game.campaign() across travel, New game and Restart; a save from
//                   another level travels there (LoadSave) and restores before its first
//                   tick, with the right GameMode; wrong game and profile; a load that
//                   finishes after its level ended; restore while paused.
//   save_migration  format 1 saves still load; the game's migrations run in order on
//                   detached data; newer formats and schemas, and failed migrations, are
//                   refused; --save-migrate's text rewrite keeps the entities as they were.
//   save_durability checksums catch damaged and cut-short files; the backup is used when
//                   the slot is damaged; a failure at each write step leaves the old save
//                   whole; two writers on one slot end in Conflict, not a lost save; at
//                   most two saves in flight, written in order.
#include <Assets/Assets.h>
#include <Engine/Components.h>
#include <Engine/Levels.h>
#include <GameFramework/GameInstance.h>
#include <GameFramework/LevelHost.h>
#include <GameFramework/PlaySession.h>
#include <GameFramework/SaveGame.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            std::fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #x);                                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

fs::path tempFolder(const char* name) {
    const auto folder =
        fs::temp_directory_path() / (std::string("shockwave-") + name + "-" +
                                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(folder);
    return folder;
}

std::string readFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeFile(const fs::path& file, const std::string& text) { std::ofstream(file, std::ios::binary) << text; }

bool hasTempFiles(const fs::path& folder) {
    for (const auto& entry : fs::directory_iterator(folder))
        if (entry.path().extension() == ".tmp") return true;
    return false;
}

// ---- Levels -------------------------------------------------------------------------------

std::map<std::string, int> begun;
std::map<std::string, StartReason> reasons;
int afterLoads = 0;

std::string levelOf(const World& world) {
    std::string id;
    const_cast<World&>(world).each<const Name>([&](Entity, const Name& name) {
        if (name.value.rfind("level:", 0) == 0) id = name.value.substr(6);
    });
    return id;
}

class Mode : public GameMode {
public:
    explicit Mode(std::string level) : m_level(std::move(level)) {}
    void beginPlay(GameContext& game) override {
        ++begun[m_level];
        reasons[m_level] = game.startReason;
    }
    void afterLoad(GameContext&) override { ++afterLoads; }

private:
    std::string m_level;
};

class Instance : public GameInstance {
public:
    std::unique_ptr<GameMode> createGameMode(const World& scene) override {
        return std::make_unique<Mode>(levelOf(scene));
    }
};

// A level: its marker, a spawn "start", and a crate at x = 0 the tests move.
void buildLevel(World& world, const std::string& id) {
    world.add<Name>(world.create(), Name{"level:" + id});
    const Entity spawn = world.create();
    world.add<Transform>(spawn, Transform{{1, 0, 0}});
    world.add<LevelSpawn>(spawn, LevelSpawn{"start"});
    const Entity crate = world.create();
    world.add<Name>(crate, Name{"crate"});
    world.add<Transform>(crate, Transform{{0, 0, 0}});
}

float crateX(World& world) {
    float x = -1;
    world.each<const Name, const Transform>([&](Entity, const Name& name, const Transform& t) {
        if (name.value == "crate") x = t.position.x;
    });
    return x;
}

void moveCrate(World& world, float x) {
    world.each<const Name, Transform>([&](Entity, const Name& name, Transform& t) {
        if (name.value == "crate") t.position.x = x;
    });
}

SaveIdentity identity(std::string profile = "p1", std::string application = "test.campaign") {
    SaveIdentity id;
    id.application = std::move(application);
    id.profile = std::move(profile);
    id.schema = 1;
    id.content = "1.0.0";
    return id;
}

LevelHost::Hooks hooks(const fs::path& folder, const SaveIdentity& id) {
    LevelHost::Hooks h;
    h.resolve = [](TravelRequest& request, TravelError& code, std::string& error) {
        if (request.level == "menu" || request.level == "a" || request.level == "b") return true;
        code = TravelError::UnknownLevel;
        error = "no level \"" + request.level + "\"";
        return false;
    };
    h.load = [](const LevelRequest& request, World& into, std::string&) {
        buildLevel(into, request.level);
        return true;
    };
    h.configure = [folder, id](PlaySession& session, const LevelRequest& request) {
        session.setSaveFolder(folder, "Scenes/" + request.level + ".swscene");
        session.setSaveIdentity(id);
    };
    return h;
}

TravelRequest to(std::string level) {
    TravelRequest r;
    r.level = std::move(level);
    r.spawn = "start";
    return r;
}

void tick(LevelHost& host) {
    if (host.session()) host.session()->update(1.0 / 60.0);
}

// A load's result goes in at the next frame or tick: a frame, then the travel it asks for.
void finishLoad(LevelHost& host) {
    host.session()->saves()->wait();
    host.session()->frame(1.0 / 60.0);
    host.process();
}

// ---- save_travel --------------------------------------------------------------------------

void testTravel() {
    Assets assets;
    const fs::path folder = tempFolder("save-travel");
    const SaveIdentity id = identity();
    {
        World world;
        JobSystem jobs(2);
        Instance instance;
        LevelHost host(world, jobs, instance, nullptr, hooks(folder, id));
        host.setMenuLevel("menu");
        buildLevel(world, "a");
        host.start({"a", "start"});
        tick(host);
        CampaignState& campaign = host.session()->context().campaign();
        CHECK(&campaign == &host.campaign());   // game.campaign() is the host's

        // Campaign values survive travel; a level's own values don't.
        campaign.set("keys", 1);
        host.session()->saves()->set("local", "a1");
        moveCrate(world, 7);
        CHECK(host.session()->saves()->save("slot1", "In A"));
        host.session()->saves()->wait();
        CHECK(host.session()->saves()->lastError().empty());
        SaveInfo info;
        std::string error;
        CHECK(readSaveInfo(folder / "slot1.swsave", info, error));
        CHECK(info.format == 2 && info.level == "a" && info.spawn == "start" && info.application == id.application);
        CHECK(info.profile == "p1" && info.revision == 1 && info.checksum && info.campaign["keys"] == "1");
        CHECK(info.values["local"] == "a1" && info.content == "1.0.0" && info.schema == 1);

        CHECK(host.request(to("b")));
        host.process();
        CHECK(host.currentLevel().level == "b" && host.session()->context().campaign().getInt("keys") == 1);
        CHECK(!host.session()->saves()->has("local"));
        tick(host);

        // Loading A's save from B travels to A, with the loading screen's ticket, and the
        // save is in before A's first tick: its crate, values and campaign.
        campaign.set("keys", 5);
        const int aBefore = begun["a"];
        const int loadsBefore = afterLoads;
        CHECK(host.session()->saves()->load("slot1"));
        finishLoad(host);
        CHECK(host.currentLevel().level == "a" && begun["a"] == aBefore + 1);
        CHECK(reasons["a"] == StartReason::RestoreSave && afterLoads == loadsBefore + 1);
        CHECK(host.session()->ticks() == 0 && crateX(world) == 7);
        CHECK(host.session()->saves()->get("local") == "a1" && campaign.getInt("keys") == 1);
        CHECK(!host.results().empty() && host.results().back().status == TravelStatus::Succeeded);
        CHECK(world.levelEpoch() == host.epoch());   // the restored world belongs to the new level
        tick(host);

        // Restart level puts the campaign back as it was on entering: no reward twice.
        campaign.set("reward", true);
        CHECK(host.restartLevel());
        host.process();
        CHECK(!campaign.has("reward") && campaign.getInt("keys") == 1);
        tick(host);

        // A same-level save loads in place (no travel); while paused it still goes in.
        moveCrate(world, 2);
        CHECK(host.session()->saves()->save("slot2"));
        host.session()->saves()->wait();
        moveCrate(world, 3);
        const uint64_t epoch = host.epoch();
        CHECK(host.session()->pause());
        CHECK(host.session()->saves()->load("slot2"));
        finishLoad(host);
        CHECK(host.epoch() == epoch && crateX(world) == 2 && host.session()->state() == PlaySession::State::Paused);
        host.session()->start();   // resume
        tick(host);

        // A load still reading when its level ends is dropped with that level.
        CHECK(host.session()->saves()->load("slot1"));
        CHECK(host.request(to("b")));
        host.process();
        tick(host);
        host.session()->frame(1.0 / 60.0);
        host.process();
        CHECK(host.currentLevel().level == "b" && !host.transitioning());

        // Another game's or profile's save is refused; the level plays on untouched.
        {
            SaveRestore restore;
            CHECK(readSaveRestore(folder / "slot1.swsave", assets, id, restore, error));
            restore.world = World{};
            buildLevel(restore.world, "a");
            restore.info.application = "other.game";
            restore.info.slot = "foreign";
            std::ostringstream text;
            writeSaveGame(text, restore.world, assets, restore.info);
            writeFile(folder / "foreign.swsave", text.str());
            restore.info.application = id.application;
            restore.info.profile = "someone";
            restore.info.slot = "theirs";
            std::ostringstream theirs;
            writeSaveGame(theirs, restore.world, assets, restore.info);
            writeFile(folder / "theirs.swsave", theirs.str());
        }
        CHECK(host.session()->saves()->load("foreign"));
        finishLoad(host);
        CHECK(saveErrorOf(host.session()->saves()->lastError()) == SaveError::WrongGame);
        CHECK(host.session()->saves()->load("theirs"));
        finishLoad(host);
        CHECK(saveErrorOf(host.session()->saves()->lastError()) == SaveError::WrongProfile);
        CHECK(host.currentLevel().level == "b" && host.playing() && !host.transitioning());

        // Continue skips them: the newest this game and profile can load is slot2... or
        // slot1, both A's. New game, confirmed, clears the campaign.
        std::string why;
        const auto newest = newestCompatibleSave(folder, id, &why);
        CHECK(newest && (newest->slot == "slot1" || newest->slot == "slot2"));
        TravelRequest fresh;
        fresh.level = "a";
        fresh.reason = TravelReason::NewGame;
        fresh.resetCampaign = true;
        CHECK(host.request(fresh));
        host.process();
        CHECK(campaign.empty());
        host.stop();
    }

    // The next launch: a save made in B, continued from the menu, plays in B.
    {
        World world;
        JobSystem jobs(2);
        Instance instance;
        LevelHost host(world, jobs, instance, nullptr, hooks(folder, id));
        buildLevel(world, "b");
        host.start({"b", "start"});
        host.campaign().set("chapter", 2);
        moveCrate(world, 9);
        CHECK(host.session()->saves()->save("inB"));
        host.session()->saves()->wait();
        host.stop();

        World next;
        LevelHost later(next, jobs, instance, nullptr, hooks(folder, id));
        buildLevel(next, "menu");
        later.start({"menu"});
        CHECK(later.campaign().empty());
        const auto newest = newestCompatibleSave(folder, id);
        CHECK(newest && newest->slot == "inB");
        auto restore = std::make_shared<SaveRestore>();
        std::string error;
        CHECK(newest && readSaveRestore(newest->file, assets, id, *restore, error));
        TravelRequest request;
        request.level = restore->info.level;
        request.spawn = restore->info.spawn;
        request.reason = TravelReason::LoadSave;
        request.restore = restore;
        CHECK(later.request(request));
        later.process();
        CHECK(later.currentLevel().level == "b" && crateX(next) == 9 && later.campaign().getInt("chapter") == 2);
        CHECK(reasons["b"] == StartReason::RestoreSave);
        // The ticket only describes the request: the save's world isn't kept in history.
        const auto status = later.status(later.results().back().id);
        CHECK(status && !status->request.restore && status->request.reason == TravelReason::LoadSave);
        later.stop();
    }
    fs::remove_all(folder);
}

// ---- save_migration -----------------------------------------------------------------------

const char* kFormat1 = "# Shockwave scene\nshockwave_scene 2\nsave_game 1\ntitle \"Old\"\nscene \"Scenes/a.swscene\"\n"
                       "saved_at \"2026-01-01 00:00:00\"\nplay_time 3.000\nvalue coins \"12\"\n\n"
                       "entity \"level:a\"\nend\nentity \"crate\"\n  Transform\n    position 4 0 0\nend\n";

void testMigration() {
    Assets assets;
    std::string error;
    const fs::path folder = tempFolder("save-migration");
    writeFile(folder / "old.swsave", kFormat1);

    // Format 1 (before S06) still reads: no identity, schema 1, no checksum.
    SaveInfo info;
    CHECK(readSaveInfo(folder / "old.swsave", info, error));
    CHECK(info.format == 1 && info.schema == 1 && !info.checksum && info.level.empty() && info.values["coins"] == "12");
    SaveRestore restore;
    CHECK(readSaveRestore(folder / "old.swsave", assets, identity(), restore, error));
    CHECK(restore.world.entityCount() == 2 && restore.info.values["coins"] == "12");

    // Migrations run oldest to newest, one step at a time, on the detached save.
    SaveIdentity v3 = identity();
    v3.schema = 3;
    std::vector<int> steps;
    v3.migrate = [&](int from, SaveInfo& save, std::string&) {
        steps.push_back(from);
        if (from == 1) {   // coins became gold
            save.values["gold"] = save.values["coins"];
            save.values.erase("coins");
        }
        if (from == 2) save.campaign["migrated"] = "yes";
        return true;
    };
    CHECK(readSaveRestore(folder / "old.swsave", assets, v3, restore, error));
    CHECK(steps == (std::vector<int>{1, 2}) && restore.info.schema == 3);
    CHECK(restore.info.values["gold"] == "12" && !restore.info.values.count("coins"));
    CHECK(restore.info.campaign["migrated"] == "yes");
    CHECK(readFile(folder / "old.swsave") == kFormat1);   // the original stays as it was

    // A migration that refuses, a newer schema, a newer format.
    SaveIdentity refusing = v3;
    refusing.migrate = [](int from, SaveInfo&, std::string& why) {
        why = "no way to convert the old inventory";
        return from != 2;
    };
    CHECK(!readSaveRestore(folder / "old.swsave", assets, refusing, restore, error));
    CHECK(saveErrorOf(error) == SaveError::MigrationFailed && error.find("old inventory") != std::string::npos);
    std::string newer = kFormat1;
    newer.replace(newer.find("save_game 1"), 11, "save_game 2\nschema 4");
    writeFile(folder / "newer.swsave", newer);
    CHECK(!readSaveRestore(folder / "newer.swsave", assets, v3, restore, error));
    CHECK(saveErrorOf(error) == SaveError::NewerSchema);
    std::string future = kFormat1;
    future.replace(future.find("save_game 1"), 11, "save_game 3");
    writeFile(folder / "future.swsave", future);
    CHECK(!readSaveInfo(folder / "future.swsave", info, error) && saveErrorOf(error) == SaveError::NewerFormat);

    // Bounded: a value past 64 KiB is refused before anything is built.
    std::string huge = kFormat1;
    huge.replace(huge.find("value coins \"12\""), 16, "value coins \"" + std::string(70000, 'x') + "\"");
    writeFile(folder / "huge.swsave", huge);
    CHECK(!readSaveInfo(folder / "huge.swsave", info, error) && saveErrorOf(error) == SaveError::TooLarge);

    // --save-migrate's rewrite: format 2 at the current schema, migrated values, a valid
    // checksum, the level filled in, the entities byte for byte.
    std::string text = kFormat1;
    SaveInfo result;
    CHECK(migrateSaveText(text, "old.swsave", v3, [](SaveInfo& save) { save.level = "a"; }, result, error));
    CHECK(result.format == 2 && result.schema == 3 && result.level == "a" && result.application == "test.campaign");
    writeFile(folder / "migrated.swsave", text);
    CHECK(readSaveInfo(folder / "migrated.swsave", info, error) && info.checksum && info.schema == 3);
    CHECK(info.values["gold"] == "12" && info.level == "a" && info.title == "Old" && info.playTime == 3.0);
    const std::string body = std::string(kFormat1).substr(std::string(kFormat1).find("entity \"level:a\""));
    CHECK(text.size() >= body.size() && text.compare(text.size() - body.size(), body.size(), body) == 0);
    CHECK(readSaveRestore(folder / "migrated.swsave", assets, v3, restore, error) && restore.world.entityCount() == 2);

    // In a session, a format 1 save loads in place, as it always did, and keeps the
    // campaign the game has (it carried none).
    {
        World world;
        JobSystem jobs(1);
        Instance instance;
        buildLevel(world, "a");
        LevelHost host(world, jobs, instance, nullptr, hooks(folder, identity()));
        host.start({"a"});
        host.campaign().set("kept", 1);
        CHECK(host.session()->saves()->load("old"));
        finishLoad(host);
        CHECK(host.currentLevel().level == "a" && crateX(world) == 4 && host.campaign().getInt("kept") == 1);
        CHECK(host.session()->saves()->getInt("coins") == 12);
        host.stop();
    }
    fs::remove_all(folder);
}

// ---- save_durability ----------------------------------------------------------------------

void testDurability() {
    Assets assets;
    std::string error;
    const fs::path folder = tempFolder("save-durability");
    JobSystem jobs(2);
    World world;
    buildLevel(world, "a");
    SaveGames saves(world, jobs, nullptr, folder);
    saves.setIdentity(identity());
    moveCrate(world, 1);
    CHECK(saves.save("slot"));
    saves.wait();
    const fs::path file = folder / "slot.swsave";
    const std::string good = readFile(file);
    SaveInfo info;
    CHECK(readSaveInfo(file, info, error) && info.checksum && info.revision == 1);

    // Damaged and cut short: the checksum says so. Without the checksum line, a hand
    // edit loads.
    std::string damaged = good;
    damaged[damaged.find("position") + 9] = '5';
    writeFile(folder / "damaged.swsave", damaged);
    CHECK(!readSaveInfo(folder / "damaged.swsave", info, error) && saveErrorOf(error) == SaveError::Corrupt);
    writeFile(folder / "short.swsave", good.substr(0, good.size() - 20));
    CHECK(!readSaveInfo(folder / "short.swsave", info, error) && saveErrorOf(error) == SaveError::Corrupt);
    std::string edited = damaged;
    edited.erase(edited.find("checksum "), edited.find('\n', edited.find("checksum ")) - edited.find("checksum ") + 1);
    writeFile(folder / "edited.swsave", edited);
    CHECK(readSaveInfo(folder / "edited.swsave", info, error) && !info.checksum);
    const auto listed = listSaveGames(folder, true);
    size_t broken = 0;
    for (const SaveInfo& s : listed) broken += !s.problem.empty();
    CHECK(broken == 2 && listSaveGames(folder).size() == listed.size() - 2);
    for (const char* name : {"damaged", "short", "edited"}) fs::remove(folder / (std::string(name) + ".swsave"));

    // A second save keeps the first as the backup; a damaged slot loads its backup.
    moveCrate(world, 2);
    CHECK(saves.save("slot"));
    saves.wait();
    CHECK(fs::exists(file.string() + ".bak") && readFile(file.string() + ".bak") == good);
    CHECK(readSaveInfo(file, info, error) && info.revision == 2);
    const std::string second = readFile(file);
    writeFile(file, second.substr(0, second.size() / 2));
    SaveRestore restore;
    std::vector<std::string> warnings;
    CHECK(readSaveRestore(file, assets, identity(), restore, error, &warnings));
    CHECK(crateX(restore.world) == 1 && warnings.size() == 1 && warnings[0].find("backup") != std::string::npos);
    writeFile(file, second);

    // A failure at each write step leaves the old save whole and no temporary file.
    for (const char* step : {"open", "write", "flush", "backup", "replace"}) {
        const std::string wanted = step;
        setSaveFaultForTesting([wanted](std::string_view at) { return at == wanted; });
        moveCrate(world, 3);
        CHECK(saves.save("slot"));
        saves.wait();
        CHECK(saveErrorOf(saves.lastError()) == SaveError::WriteFailed);
        if (saveErrorOf(saves.lastError()) != SaveError::WriteFailed) std::fprintf(stderr, "  at step %s\n", step);
        CHECK(readFile(file) == second && !hasTempFiles(folder));
    }
    // After the atomic replace the save is in, even if the folder can't be flushed.
    setSaveFaultForTesting([](std::string_view at) { return at == "directory"; });
    CHECK(saves.save("slot"));
    saves.wait();
    CHECK(saves.lastError().empty() && readSaveInfo(file, info, error) && info.revision == 3);
    setSaveFaultForTesting({});

    // Two writers on one slot (two copies of the game): the stale one gets Conflict,
    // and both saves are kept.
    World other;
    buildLevel(other, "a");
    SaveGames second2(other, jobs, nullptr, folder);
    second2.setIdentity(identity());
    CHECK(second2.load("slot"));
    second2.wait();
    CHECK(second2.applyLoaded());
    moveCrate(world, 4);
    CHECK(saves.save("slot"));   // revision 4
    saves.wait();
    moveCrate(other, 5);
    CHECK(second2.save("slot"));
    second2.wait();
    CHECK(saveErrorOf(second2.lastError()) == SaveError::Conflict);
    CHECK(readSaveInfo(file, info, error) && info.revision == 4);
    SaveInfo aside;
    CHECK(readSaveInfo(file.string() + ".conflict", aside, error) && crateX(other) == 5);
    CHECK(second2.load("slot"));   // after reading it again, it may write
    second2.wait();
    CHECK(second2.applyLoaded() && second2.save("slot"));
    second2.wait();
    CHECK(second2.lastError().empty() && readSaveInfo(file, info, error) && info.revision == 5);

    // At most two saves in flight, written in order.
    std::atomic<bool> hold{true};
    setSaveFaultForTesting([&hold](std::string_view at) {
        if (at == "write")
            while (hold) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return false;
    });
    CHECK(saves.load("slot"));
    saves.wait();
    CHECK(saves.applyLoaded());
    moveCrate(world, 6);
    CHECK(saves.save("slot"));
    moveCrate(world, 7);
    CHECK(saves.save("slot"));
    CHECK(!saves.save("slot") && saveErrorOf(saves.lastError()) == SaveError::Busy);
    hold = false;
    saves.wait();
    setSaveFaultForTesting({});
    CHECK(saveErrorOf(saves.lastError()) == SaveError::Busy);   // still the last thing that went wrong
    CHECK(readSaveInfo(file, info, error) && info.revision == 7);
    CHECK(readSaveRestore(file, assets, identity(), restore, error) && crateX(restore.world) == 7);

    // Removing a slot removes its backup too.
    CHECK(saves.remove("slot") && !fs::exists(file) && !fs::exists(file.string() + ".bak"));
    CHECK(!hasTempFiles(folder));
    fs::remove_all(folder);
}

}   // namespace

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "all";
    if (which == "travel" || which == "all") testTravel();
    if (which == "migration" || which == "all") testMigration();
    if (which == "durability" || which == "all") testDurability();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("campaign saves (%s): all checks passed\n", which.c_str());
    return 0;
}
