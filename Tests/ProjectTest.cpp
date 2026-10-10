#include <Engine/Components.h>
#include <GameFramework/PlaySession.h>
#include <Projects/Project.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <stdexcept>

using namespace shockwave;
namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
struct Counters {
    int created = 0, started = 0, stopped = 0, frames = 0;
    bool failStart = false, failStop = false;
};
class TestGameMode : public GameMode {
public:
    explicit TestGameMode(Counters& counts) : c(counts) { ++c.created; }
    void configureSystems(GameContext&, Schedule& s) override {
        s.each<Transform>("Move", [](const SystemContext& ctx, Transform& t) { t.position.x += ctx.dt; });
    }
    void beginPlay(GameContext& ctx) override {
        ++c.started;
        const auto extra = ctx.world.create();
        ctx.world.add<Name>(extra, {"Runtime only"});
        if (c.failStart) throw std::runtime_error("start failed");
    }
    void frame(GameContext&, double) override { ++c.frames; }
    void endPlay(GameContext&) override {
        ++c.stopped;
        if (c.failStop) throw std::runtime_error("stop failed");
    }

private:
    Counters& c;
};
// A GameInstance that makes GameModes with a lambda, for tests.
struct TestInstance final : GameInstance {
    std::function<std::unique_ptr<GameMode>()> make;
    explicit TestInstance(std::function<std::unique_ptr<GameMode>()> f) : make(std::move(f)) {}
    std::unique_ptr<GameMode> createGameMode(const World&) override { return make(); }
};
void sessions() {
    World world;
    JobSystem jobs(2);
    const Entity e = world.create();
    world.add<Transform>(e, {{3, 2, 1}});
    Counters c;
    TestInstance instance([&] { return std::make_unique<TestGameMode>(c); });
    PlaySession session(world, jobs, instance);
    check(session.state() == PlaySession::State::Editing, "Session starts in editing state");
    session.update(1);
    session.frame(1);
    session.stop();
    check(c.created == 0 && world.entityCount() == 1, "Inactive callbacks do nothing");
    session.start();
    session.start();
    check(c.started == 1 && world.entityCount() == 2, "Start initializes once and may spawn runtime entities");
    session.update(0.5);
    session.frame(0.5);
    check(
        world.get<Transform>(e)->position.x == 3.5f && session.ticks() == 1 && c.frames == 1,
        "Gameplay system and frame hook run");
    session.pause();
    session.update(1);
    session.frame(1);
    check(session.ticks() == 1 && c.frames == 1, "Pause freezes simulation and frame hooks");
    session.start();
    session.update(0.5);
    check(c.started == 1 && world.get<Transform>(e)->position.x == 4, "Resume does not reinitialize");
    session.stop();
    session.stop();
    check(
        c.stopped == 1 && world.entityCount() == 1 && world.get<Transform>(e)->position.x == 3,
        "Stop cleans up once and restores authored state and handles");
    session.start();
    check(
        c.created == 2 && session.ticks() == 0 && session.schedule().size() == 3,
        "Restart uses fresh gameplay without duplicate systems");
    session.stop();
    c.failStart = true;
    try {
        session.start();
        check(false, "Failed start throws");
    } catch (const std::runtime_error&) {}
    check(!session.active() && world.entityCount() == 1 && c.stopped == 3, "Failed start cleans up and restores world");
    c.failStart = false;
    c.failStop = true;
    session.start();
    try {
        session.stop();
        check(false, "Failed cleanup throws");
    } catch (const std::runtime_error&) {}
    check(!session.active() && world.entityCount() == 1, "Cleanup failure still restores authored state");
    c.failStop = false;
    {
        PlaySession scoped(world, jobs, instance);
        scoped.start();
    }
    check(world.entityCount() == 1 && c.stopped == 5, "Destructor cleans up an active session");
}
void manifests() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("shockwave-project-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "scenes with spaces");
    const auto file = root / "game.swproject";
    {
        std::ofstream scene(root / "scenes with spaces/main.swscene");
        scene << "shockwave_scene 2\n";
    }
    const std::string valid = "shockwave_project 1\nname \"Test # Game\"\ngame_module \"test\"\nstartup_scene \"scenes "
                              "with spaces/main.swscene\"\ntick_rate 120\nwindow_size 800 600\nvsync false # comment\n";
    auto write = [&](const std::string& data) {
        std::ofstream out(file);
        out << data;
    };
    write(valid);
    Project p;
    std::string error;
    check(loadProject(file, p, error), "Quoted paths, comments and settings load");
    check(p.name == "Test # Game" && p.tickRate == 120 && !p.vsync && p.width == 800, "Manifest settings retained");
    check(
        p.startupScene == root / "scenes with spaces/main.swscene" && p.assetFolder == root / "Content",
        "Paths resolve against manifest from another working directory");
    const auto original = p.name;
    for (const auto& invalid :
         {valid + "tick_rate 30\n", valid + "unknown 1\n",
          std::string("shockwave_project 1\nname Test\ngame_module test\nstartup_scene \"scenes with "
                      "spaces/main.swscene\"\nwindow_size 1 1\n"),
          std::string("shockwave_project 99\n"), std::string("shockwave_project 1\nname \"Missing scene\"\n"),
          std::string("shockwave_project 1\nname Test\ngame_module test\nstartup_scene missing.swscene\n"),
          std::string("shockwave_project 1\nname Test\ngame_module test\nstartup_scene \"scenes with "
                      "spaces/main.swscene\"\ntick_rate 0\n")}) {
        write(invalid);
        check(
            !loadProject(file, p, error) && p.name == original && !error.empty(),
            "Invalid manifest is rejected without replacing loaded project");
    }
    std::filesystem::remove_all(root);
}
// Created projects load back, save round-trips, and scenes find their project.
void created() {
    namespace fs = std::filesystem;
    const auto root =
        fs::temp_directory_path() /
        ("shockwave-create-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Project p;
    std::string error;
    check(createProject(root / "My Game", "My Game", "test", p, error), "New project is laid out");
    check(
        p.file == root / "My Game/MyGame.swproject" && fs::is_directory(p.assetFolder) &&
            fs::is_directory(p.sceneFolder) && fs::exists(root / "My Game/.gitignore"),
        "Manifest, folders and .gitignore exist");
    check(!createProject(root / "My Game", "Again", "test", p, error), "One project per folder");
    {
        std::ofstream scene(p.startupScene);
        scene << "shockwave_scene 2\n";
    }
    Project loaded;
    check(loadProject(p.file, loaded, error), "Created manifest loads once its scene exists");
    check(
        loaded.name == "My Game" && loaded.gameModule == "test" && loaded.sceneFolder == root / "My Game/Scenes",
        "Created settings retained");
    loaded.tickRate = 90;
    loaded.startupScene = loaded.sceneFolder / "main.swscene";
    check(
        saveProject(loaded, error) && loadProject(p.file, loaded, error) && loaded.tickRate == 90, "Save round-trips");
    check(loaded.relative(loaded.sceneFolder / "a.swscene") == "Scenes/a.swscene", "Project-relative paths");
    const auto found = findProjectFile(root / "My Game/scenes/deep/x.swscene");
    check(found && *found == p.file, "A scene finds the project above it");
    check(findProjectFile(root / "My Game") == p.file, "A folder finds its manifest");
    fs::remove_all(root);
}
}   // namespace
int main() {
    sessions();
    manifests();
    created();
    return failures ? 1 : 0;
}
