// Save game tests: the file format round trip, header reads, slot rules, and saving and
// loading during a PlaySession. No GPU or window needed: ctest --test-dir build
#include <Assets/Assets.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/PhysicsComponents.h>
#include <Engine/Prefab.h>
#include <Engine/SceneFile.h>
#include <GameFramework/PlaySession.h>
#include <GameFramework/SaveGame.h>
#include <Physics/Physics.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {

int failures = 0;
void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

fs::path tempFolder(const char* name) {
    const auto folder =
        fs::temp_directory_path() / (std::string("shockwave-") + name + "-" +
                                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(folder);
    return folder;
}

// The file format on its own: what goes in comes back, with the same entity handles.
void roundTrip() {
    Assets assets;
    World world;
    const Entity a = world.create();
    const Entity gone = world.create();
    const Entity b = world.create();
    world.destroy(gone);
    const Entity c = world.create();   // index 3: freed indices aren't reused right away
    world.add<Name>(a, {"Player"});
    world.add<Transform>(a, {{1, 2, 3}});
    world.add<RigidBody>(a, {.linearVelocity = {0, 5, 0}});
    world.add<Name>(b, {"Child"});
    world.add<Transform>(b, {{0, 1, 0}});
    world.add<Parent>(b, {a});
    world.add<Name>(c, {"Two words"});

    SaveInfo info;
    info.title = "Chapter \"2\"";
    info.scene = "Scenes/main.swscene";
    info.savedAt = "2026-10-11 10:30:00";
    info.playTime = 12.5;
    info.values = {{"coins", "12"}, {"name", "Yianni's game"}};
    std::stringstream text;
    writeSaveGame(text, world, assets, info);
    const std::string written = text.str();
    check(written.find("shockwave_scene") != std::string::npos, "A save game is a scene file");
    check(written.find("save_game 1") != std::string::npos, "It says it's a save game");

    World loaded;
    SaveInfo back;
    std::string error;
    std::vector<std::string> warnings;
    check(readSaveGame(text, "test.swsave", assets, loaded, back, error, &warnings), "A save game reads back");
    check(error.empty() && warnings.empty(), "...without problems");
    check(loaded.entityCount() == 3, "Every entity comes back");
    check(loaded.alive(a) && loaded.alive(b) && loaded.alive(c), "Entities keep their handles");
    check(!loaded.alive(gone), "Destroyed entities stay destroyed");
    check(loaded.get<Transform>(a) && loaded.get<Transform>(a)->position == glm::vec3(1, 2, 3), "Components come back");
    check(
        loaded.get<RigidBody>(a) && loaded.get<RigidBody>(a)->linearVelocity == glm::vec3(0, 5, 0),
        "Velocities come back");
    check(loaded.get<Parent>(b) && loaded.get<Parent>(b)->entity == a, "Links between entities come back");
    check(back.title == info.title && back.scene == info.scene && back.savedAt == info.savedAt, "Header comes back");
    check(back.playTime == 12.5 && back.values == info.values, "Play time and values come back");
    const Entity next = loaded.create();
    check(next != a && next != b && next != c, "New entities after a load don't reuse live handles");

    // A save loads as a plain scene too (the editor and `player --scene` open them).
    World asScene;
    std::istringstream again(written);
    check(readScene(asScene, assets, again, "test.swsave", error), "A save game opens as a scene");
    check(asScene.entityCount() == 3, "...with every entity");
}

void headersAndRules() {
    Assets assets;
    std::string error;
    World world;
    SaveInfo info;

    // Written by hand (or by a tool): no entity_ids, so entities get new handles.
    std::istringstream byHand("shockwave_scene 2\nsave_game 1\nvalue level \"3\"\n\nentity \"Crate\"\n  Transform\n    "
                              "position 0 4 0\nend\n");
    check(readSaveGame(byHand, "hand.swsave", assets, world, info, error), "A save without entity_ids loads");
    check(world.entityCount() == 1 && info.values["level"] == "3", "...with its entities and values");

    std::istringstream wrongCount("shockwave_scene 2\nsave_game 1\nentity_ids 0 1\n\nentity \"A\"\nend\n");
    check(!readSaveGame(wrongCount, "bad.swsave", assets, world, info, error), "entity_ids must match the blocks");
    check(error.find("entity_ids lists 2") != std::string::npos, "...and says so");

    std::istringstream badId("shockwave_scene 2\nsave_game 1\nentity_ids x\n\nentity \"A\"\nend\n");
    check(!readSaveGame(badId, "bad.swsave", assets, world, info, error), "Bad entity ids are an error");

    std::istringstream notSave("shockwave_scene 2\n\nentity \"A\"\nend\n");
    check(!readSaveGame(notSave, "scene.swscene", assets, world, info, error), "A plain scene isn't a save game");
    check(error.find("not a save game") != std::string::npos, "...and says so");

    check(validSaveSlot("slot1") && validSaveSlot("Quick save") && validSaveSlot("auto_2-b"), "Good slot names");
    check(!validSaveSlot("") && !validSaveSlot("../x") && !validSaveSlot("a.b") && !validSaveSlot(" x"), "Bad slots");
    check(!validSaveSlot(std::string(65, 'a')), "Slot names have a length limit");
    check(validSaveKey("player.coins") && !validSaveKey("two words") && !validSaveKey(""), "Value key rules");

    // Lists read only headers, newest first, and skip files that aren't saves.
    const fs::path folder = tempFolder("save-list");
    auto write = [&](const char* name, const char* when) {
        std::ofstream(folder / name) << "shockwave_scene 2\nsave_game 1\nsaved_at \"" << when
                                     << "\"\n\nentity \"A\"\nend\nentity \"B\"\nend\n";
    };
    write("old.swsave", "2026-01-01 00:00:00");
    write("new.swsave", "2026-10-11 09:00:00");
    std::ofstream(folder / "junk.swsave") << "hello\n";
    std::ofstream(folder / "other.txt") << "shockwave_scene 2\nsave_game 1\n";
    const auto saves = listSaveGames(folder);
    check(saves.size() == 2, "Lists only save games");
    check(saves.size() == 2 && saves[0].slot == "new" && saves[1].slot == "old", "Newest first");
    check(saves.size() == 2 && saves[0].entities == 2, "The list counts entities");
    fs::remove_all(folder);
}

// A GameMode that keeps an EntityRef and counts the save hooks.
struct Counters {
    int beforeSave = 0, afterLoad = 0;
    int coinsAfterLoad = -1;
    Entity player;
};
class SavingMode final : public GameMode {
public:
    explicit SavingMode(Counters& counters) : c(counters) {}
    void beginPlay(GameContext& game) override {
        c.player = game.find("Player").entity();
        game.saves().set("coins", 0);
    }
    void beforeSave(GameContext&) override { ++c.beforeSave; }
    void afterLoad(GameContext& game) override {
        ++c.afterLoad;
        c.coinsAfterLoad = game.saves().getInt("coins", -1);
    }

private:
    Counters& c;
};
struct TestInstance final : GameInstance {
    std::function<std::unique_ptr<GameMode>()> make;
    explicit TestInstance(std::function<std::unique_ptr<GameMode>()> f) : make(std::move(f)) {}
    std::unique_ptr<GameMode> createGameMode(const World&) override { return make(); }
};

void duringPlay() {
    const fs::path folder = tempFolder("save-play");
    World world;
    JobSystem jobs(2);
    const Entity ground = world.create();
    world.add<Name>(ground, {"Ground"});
    world.add<Transform>(ground);
    world.add<Collider>(ground, {.halfExtents = {10, 0.5f, 10}});
    world.add<RigidBody>(ground, {.motion = RigidBody::Motion::Static});
    const Entity player = world.create();
    world.add<Name>(player, {"Player"});
    world.add<Transform>(player, {{0, 5, 0}});
    world.add<Collider>(player);
    world.add<RigidBody>(player);

    Counters c;
    TestInstance instance([&] { return std::make_unique<SavingMode>(c); });
    PlaySession session(world, jobs, instance);
    session.setSaveFolder(folder, "Scenes/test.swscene");
    session.start();
    SaveGames& saves = *session.saves();
    for (int i = 0; i < 10; ++i) session.update(1.0 / 60);
    const glm::vec3 savedAt = world.get<Transform>(player)->position;
    saves.set("coins", 7);
    check(saves.save("slot1", "First"), "Saving starts");
    // The snapshot was taken when save() was called: changes after it aren't in the file.
    world.get<Transform>(player)->position.x = 100;
    saves.wait();
    check(saves.lastError().empty() && fs::exists(folder / "slot1.swsave"), "The save is written");
    check(c.beforeSave == 1, "GameMode::beforeSave runs once per save");
    check(saves.exists("slot1") && saves.list().size() == 1, "The slot is listed");
    check(saves.list().size() == 1 && saves.list()[0].title == "First", "...with its title");
    check(saves.list().size() == 1 && saves.list()[0].scene == "Scenes/test.swscene", "...and its scene");
    check(saves.list().size() == 1 && saves.list()[0].playTime > 0.15, "...and its play time");

    // Change everything, then load.
    saves.set("coins", 99);
    for (int i = 0; i < 30; ++i) session.update(1.0 / 60);
    const Entity extra = world.create();
    world.add<Name>(extra, {"Spawned later"});
    world.destroy(ground);
    check(saves.load("slot1"), "Loading starts");
    saves.wait();
    check(c.afterLoad == 0, "Nothing changes until the next tick");
    session.update(1.0 / 60);
    check(c.afterLoad == 1 && c.coinsAfterLoad == 7, "GameMode::afterLoad runs, with the saved values");
    check(world.alive(c.player) && world.alive(ground) && !world.alive(extra), "The saved entities are back");
    const Transform* t = world.get<Transform>(c.player);
    check(t && std::abs(t->position.x - savedAt.x) < 0.01f, "The world is as it was when saved");
    check(session.physics() && session.physics()->bodyCount() == 2, "Physics is rebuilt for the loaded world");
    check(t && t->position.y < savedAt.y, "...and keeps simulating");

    // While paused, a load still goes in (pause menus load games).
    session.pause();
    world.get<Transform>(player)->position.x = 50;
    saves.load("slot1");
    saves.wait();
    session.frame(1.0 / 60);
    check(c.afterLoad == 2 && std::abs(world.get<Transform>(player)->position.x) < 1, "Loads apply while paused");

    check(!saves.load("missing"), "Loading a slot that doesn't exist fails");
    check(!saves.save("../escape"), "Bad slot names are refused");
    check(!saves.lastError().empty(), "...with a reason");
    check(saves.remove("slot1") && !saves.exists("slot1"), "Slots can be deleted");

    session.stop();
    check(world.entityCount() == 2 && world.get<Transform>(player)->position == glm::vec3(0, 5, 0), "Stop restores");

    // Without a folder (tools, tests) saving is off but the game still runs.
    PlaySession offline(world, jobs, instance);
    offline.start();
    check(!offline.saves()->save("slot1") && !offline.saves()->lastError().empty(), "No folder: saving is off");
    offline.stop();
    fs::remove_all(folder);
}

// Prefab copies: their parts are saved as they are in play, not remade from the prefab.
void prefabCopies() {
    const fs::path content = tempFolder("save-prefabs");
    Assets assets;
    prefabs().setup(content, &assets);
    World source;
    const Entity crate = source.create();
    source.add<Name>(crate, {"Crate"});
    source.add<Transform>(crate);
    const Entity lid = source.create();
    source.add<Name>(lid, {"Lid"});
    source.add<Transform>(lid, {{0, 1, 0}});
    source.add<Parent>(lid, {crate});
    std::string error;
    check(savePrefab(source, crate, assets, content / "Prefabs/Crate.swprefab", error), "A prefab saves");

    World world;
    const Entity copy = spawnPrefab(world, "Prefabs/Crate.swprefab");
    check(copy.valid() && world.entityCount() == 2, "A prefab copy has its parts");
    Entity part;
    world.each<const Name, const PrefabPart>([&](Entity e, const Name& name, const PrefabPart&) {
        if (name.value == "Lid") part = e;
    });
    check(part.valid(), "...including the lid");
    if (part.valid()) world.get<Transform>(part)->position = {5, 6, 7};   // knocked off during play

    std::stringstream text;
    writeSaveGame(text, world, assets, {});
    World loaded;
    SaveInfo back;
    std::vector<std::string> warnings;
    check(readSaveGame(text, "prefabs.swsave", assets, loaded, back, error, &warnings), "A save with prefabs loads");
    check(error.empty(), error.c_str());
    check(loaded.entityCount() == 2, "Prefab copies come back once, parts included");
    check(
        part.valid() && loaded.alive(part) && loaded.get<Transform>(part) &&
            loaded.get<Transform>(part)->position == glm::vec3(5, 6, 7),
        "A moved prefab part keeps where it was");
    prefabs().setup({}, nullptr);
    fs::remove_all(content);
}

}   // namespace

int main() {
    roundTrip();
    headersAndRules();
    duringPlay();
    prefabCopies();
    if (failures) {
        std::fprintf(stderr, "%d save game check(s) failed\n", failures);
        return 1;
    }
    std::puts("save game tests passed");
    return 0;
}
