// The player's tools: --check validates a project and its scenes, --schema writes the
// scene format reference.
#include "Player.h"

#include <Assets/AssetFolder.h>
#include <Assets/BuiltinAssets.h>
#include <Engine/Engine.h>
#include <Engine/SceneFile.h>
#include <Engine/Systems.h>
#include <GameFramework/GameModule.h>
#include <GameFramework/PlaySession.h>
#include <Physics/Physics.h>
#include <Projects/Project.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace player {

int Player::check(const std::vector<std::string>& importProblems) {
    std::vector<std::string> problems = importProblems;
    std::vector<fs::path> scenes = m_tool.scenes;
    if (scenes.empty()) {
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(m_project.sceneFolder, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".swscene") scenes.push_back(entry.path());
        }
        if (std::find(scenes.begin(), scenes.end(), m_project.startupScene) == scenes.end())
            scenes.push_back(m_project.startupScene);
        std::sort(scenes.begin(), scenes.end());
    }
    for (const fs::path& scene : scenes) {
        const std::string shown = m_project.relative(scene);
        const size_t before = problems.size();
        // Problems name the file as the project does ("Scenes/main.swscene:12: ...").
        auto report = [&](std::string problem) {
            if (problem.rfind(scene.string(), 0) == 0) problem.replace(0, scene.string().size(), shown);
            problems.push_back(std::move(problem));
        };
        shockwave::World world;
        std::string error;
        std::vector<std::string> warnings;
        shockwave::SceneFileHooks hooks;
        hooks.readHeader = [](const std::string&, std::istream&) {};   // old editor_camera lines
        if (!shockwave::loadScene(world, engine().assets(), scene.string(), error, hooks, &warnings)) {
            report(error);
            continue;
        }
        for (const std::string& warning : warnings) report(warning);

        int primaries = 0;
        world.each<const shockwave::Camera>([&](const shockwave::Camera& camera) { primaries += camera.primary; });
        if (primaries == 0) report(scene.string() + ": no entity has a primary Camera; the game has no view");
        if (primaries > 1) report(scene.string() + ": " + std::to_string(primaries) + " primary Cameras; keep one");

        // Lighting: a light needs a Transform to be placed, the renderer has a light
        // budget, and only the first Environment is used.
        auto entityName = [&](shockwave::Entity e) {
            const shockwave::Name* name = world.get<shockwave::Name>(e);
            return std::string(name ? name->value : "?");
        };
        size_t lights = 0;
        world.each<const shockwave::Light>([&](shockwave::Entity e, const shockwave::Light& light) {
            if (!world.get<shockwave::Transform>(e))
                report(scene.string() + ": entity '" + entityName(e) + "': Light has no Transform; it won't shine");
            else if (light.enabled && light.intensity > 0.0f)
                ++lights;
        });
        if (lights > shockwave::Renderer::kMaxLights)
            report(
                scene.string() + ": " + std::to_string(lights) + " enabled lights; only " +
                std::to_string(shockwave::Renderer::kMaxLights) + " are drawn");
        int environments = 0;
        world.each<const shockwave::Environment>([&](shockwave::Entity e, const shockwave::Environment&) {
            if (++environments == 2)
                report(
                    scene.string() + ": entity '" + entityName(e) +
                    "': more than one Environment; only the first is used");
        });

        // Start the game on it, as Play does, to catch physics setup problems.
        try {
            shockwave::PlaySession session(world, engine().jobs(), *m_instance, &engine());
            session.start();
            for (const auto& diagnostic : session.physics()->diagnostics()) {
                const shockwave::Name* name = world.get<shockwave::Name>(diagnostic.entity);
                report(scene.string() + ": entity '" + (name ? name->value : "?") + "': " + diagnostic.message);
            }
            session.stop();
        } catch (const std::exception& e) {
            report(scene.string() + ": gameplay failed to start: " + e.what());
        }
        if (problems.size() == before) std::printf("ok %s (%zu entities)\n", shown.c_str(), world.entityCount());
    }
    for (const std::string& problem : problems) std::printf("%s\n", problem.c_str());
    std::printf(
        "%zu problem%s in %zu scene%s\n", problems.size(), problems.size() == 1 ? "" : "s", scenes.size(),
        scenes.size() == 1 ? "" : "s");
    return problems.empty() ? 0 : 1;
}

int Player::schema() {
    std::ostringstream out;
    shockwave::writeSceneSchema(out, engine().assets());
    out << "\n## Project manifest\n\n"
        << "`<Name>.swproject` sits at the project root; paths are relative to it and quoted.\n\n"
        << "```text\n"
        << "shockwave_project 1\n"
        << "name \"My Game\"\n"
        << "game_module \"" << m_project.gameModule << "\"      # must match the compiled gameplay library\n"
        << "startup_scene \"Scenes/main.swscene\"   # must exist\n"
        << "assets \"Content\"                      # optional, default Content\n"
        << "scenes \"Scenes\"                       # optional, default Scenes\n"
        << "theme \"Config/Editor.swtheme\"         # optional editor theme for this project\n"
        << "tick_rate 60                           # simulation updates per second\n"
        << "window_size 1280 720\n"
        << "vsync true\n"
        << "```\n\n"
        << "Unknown or repeated keys are errors. `player --check` validates the manifest too.\n";
    if (m_tool.schemaOut == "-") {
        std::fputs(out.str().c_str(), stdout);
        return 0;
    }
    std::ofstream file(m_tool.schemaOut, std::ios::trunc);
    if (!(file << out.str())) {
        std::fprintf(stderr, "Player: can't write %s\n", m_tool.schemaOut.string().c_str());
        return 1;
    }
    std::printf("Wrote %s\n", m_tool.schemaOut.string().c_str());
    return 0;
}

}   // namespace player
