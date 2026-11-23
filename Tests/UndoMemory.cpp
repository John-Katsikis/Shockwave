// What the editor's undo history costs on a big scene (E1 in the upgrade plan): grows a
// scene to 10,000 entities, makes 50 edits the way the editor does (a whole World copy per
// step, Editor::pushUndo) and prints the memory the copies hold and the time per edit.
// Not a ctest: a measurement to run by hand.
//   build/bin/undo_memory [scene] [entities] [edits]
#include <Engine/Components.h>
#include <Engine/SceneFile.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace shockwave;

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "Games/Sample/Scenes/physics-stress-boxes-8000.swscene";
    const size_t entities = argc > 2 ? std::stoul(argv[2]) : 10000;
    const int edits = argc > 3 ? std::stoi(argv[3]) : 50;
    Assets assets;   // the names the stress scenes use; others load with a warning
    assets.addMesh("cube", MeshHandle{1});
    assets.addMesh("sphere", MeshHandle{2});
    assets.addTexture("grid", TextureHandle{1});
    assets.addTexture("panel", TextureHandle{2});
    World world;
    std::string error;
    std::vector<std::string> warnings;
    if (!loadScene(world, assets, path, error, {}, &warnings)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const size_t authored = world.entityCount();
    for (size_t i = 0; world.entityCount() < entities; ++i) world.clone(world.entityAt(i % authored));
    std::printf(
        "%s: %zu entities (%zu authored), %.1f MB\n", path.c_str(), world.entityCount(), authored,
        static_cast<double>(world.bytes()) / 1048576.0);

    std::vector<World> undo;
    double worst = 0, total = 0;
    for (int edit = 0; edit < edits; ++edit) {
        const auto start = std::chrono::steady_clock::now();
        undo.push_back(world);
        if (Transform* t = world.get<Transform>(world.entityAt(size_t(edit) % world.entityCount())))
            t->position.y += 1.0f;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        worst = std::max(worst, ms);
        total += ms;
    }
    size_t bytes = 0;
    for (const World& step : undo) bytes += step.bytes();
    const double mb = static_cast<double>(bytes) / 1048576.0;
    std::printf(
        "%d undo steps: %.1f MB (%.2f MB a step); an edit takes %.2f ms on average, %.2f ms at worst\n", edits, mb,
        mb / edits, total / edits, worst);
}
