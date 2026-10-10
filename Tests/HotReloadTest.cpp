// Game code hot reload: a module is loaded, used, rebuilt (swapped for version 2 on
// disk) and reloaded, and the worlds holding its components come through with the same
// entities and their data, now as the new code's types.

#include <Assets/Assets.h>
#include <Core/Plugin.h>
#include <Engine/Components.h>
#include <Engine/World.h>
#include <GameFramework/GameModuleLibrary.h>
#include <GameFramework/PlaySession.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
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

// How many copies of the test module are mapped into this process right now.
int loadedCopies() {
    std::ifstream maps("/proc/self/maps");
    std::string line, last;
    int copies = 0;
    while (std::getline(maps, line)) {
        const size_t at = line.find("game_reload_test-");
        if (at == std::string::npos) continue;
        const std::string name = line.substr(at);
        if (name != last) ++copies;   // one library spans several consecutive mappings
        last = name;
    }
    return copies;
}

const ComponentInfo* probeInfo() { return findComponent("ReloadProbe"); }

float probeField(const World& world, Entity e, const char* field) {
    const ComponentInfo* info = probeInfo();
    if (!info) return -1.0f;
    const void* data = info->getConst(world, e);
    if (!data) return -1.0f;
    for (const Field& f : info->fields)
        if (f.name == field) return *reinterpret_cast<const float*>(static_cast<const char*>(data) + f.offset);
    return -1.0f;
}

// Plays once: the GameMode stamps its version into every probe; Stop puts the value back.
float stampedVersion(GameModuleLibrary& library, World& world, JobSystem& jobs, Entity e) {
    auto instance = makeGameInstance(&library.module());
    PlaySession session(world, jobs, *instance);
    session.start();
    const float stamped = probeField(world, e, "a");
    session.stop();
    return stamped;
}
}   // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: hot_reload_test <module v1> <module v2>\n");
        return 2;
    }
    const fs::path v1 = argv[1], v2 = argv[2];
    const fs::path dir = fs::temp_directory_path() / "shockwave-hot-reload-test";
    fs::create_directories(dir);
    const fs::path live = dir / "game_reload_test.so";
    fs::copy_file(v1, live, fs::copy_options::overwrite_existing);

    JobSystem jobs(1);
    Assets assets;
    std::string error;
    auto library = GameModuleLibrary::loadFile(live, error);
    check(library != nullptr, "version 1 loads");
    if (!library) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }
    check(library->id() == "reload_test", "the module's id comes from its code");
    check(probeInfo() && probeInfo()->fields.size() == 1, "version 1 registers its component");

    // A scene and an undo snapshot holding the module's component, with a gap in the
    // entities so handles aren't just 0, 1, 2.
    World world;
    const Entity gone = world.create();
    const Entity probe = world.create();
    world.add<Name>(probe, {"Probe"});
    world.add<Transform>(probe, {.position = {1, 2, 3}});
    probeInfo()->add(world, probe);
    *static_cast<float*>(probeInfo()->get(world, probe)) = 5.0f;
    world.destroy(gone);
    World undo = world;
    check(stampedVersion(*library, world, jobs, probe) == 100.0f, "version 1's GameMode runs");

    // A rebuild lands on disk; the watcher notices once it has settled.
    RebuildWatcher watcher(0.2);
    watcher.watch(live);
    check(!watcher.poll(0.0), "nothing to reload before a rebuild");
    fs::copy_file(v2, live, fs::copy_options::overwrite_existing);
    fs::last_write_time(live, fs::last_write_time(live) + std::chrono::seconds(2));
    check(!watcher.poll(1.0), "a fresh build waits to settle");
    check(watcher.poll(1.5), "a settled build is reported");
    check(!watcher.poll(2.0), "and only once");

    World* worlds[] = {&world, &undo};
    check(reloadGameModule(library, worlds, assets, error), "version 2 reloads");
    if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
    check(probeInfo() && probeInfo()->fields.size() == 2, "the component is now version 2's type");
    check(world.alive(probe) && !world.alive(gone), "entity handles are unchanged");
    check(probeField(world, probe, "a") == 5.0f, "component data carries over");
    check(probeField(world, probe, "b") == 7.0f, "a new field starts at its default");
    check(world.get<Transform>(probe) && world.get<Transform>(probe)->position.y == 2.0f, "engine components too");
    check(probeField(undo, probe, "a") == 5.0f, "undo history carries over");
    check(stampedVersion(*library, world, jobs, probe) == 200.0f, "version 2's GameMode runs");
#ifdef __linux__
    check(loadedCopies() == 1, "the old code is unloaded, not just replaced");
#endif

    // A broken build: the reload fails and version 2 keeps running.
    {
        std::ofstream broken(live, std::ios::binary | std::ios::trunc);
        broken << "not a library";
    }
    check(!reloadGameModule(library, worlds, assets, error), "a broken build doesn't load");
    check(
        probeInfo() && probeInfo()->fields.size() == 2 && probeField(world, probe, "a") == 5.0f,
        "the running code and the worlds are untouched");

    // Unloading takes the module's types with it.
    world = {};
    undo = {};
    library.reset();
    check(probeInfo() == nullptr, "unloading unregisters the module's components");
#ifdef __linux__
    check(loadedCopies() == 0, "and unmaps it");
#endif

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures) {
        std::fprintf(stderr, "%d hot reload check(s) failed\n", failures);
        return 1;
    }
    std::puts("hot reload ok");
    return 0;
}
