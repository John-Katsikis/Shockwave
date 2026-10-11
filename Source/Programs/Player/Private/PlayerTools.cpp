// The player's tools: --check validates a project, its scenes and its input bindings;
// --schema writes the scene format reference.
#include "Player.h"

#include <Assets/AssetFolder.h>
#include <Assets/BuiltinAssets.h>
#include <Audio/Audio.h>
#include <Engine/Engine.h>
#include <Engine/Prefab.h>
#include <Engine/SceneFile.h>
#include <Engine/Systems.h>
#include <GameFramework/GameModule.h>
#include <GameFramework/PlaySession.h>
#include <GameUI/GameUI.h>
#include <Input/Input.h>
#include <Input/InputBinding.h>
#include <Navigation/Navigation.h>
#include <Physics/Physics.h>
#include <Projects/Project.h>

#include <SDL3/SDL.h>

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
    // The project's renderer must be one the player can load.
    const auto renderers = shockwave::Renderer::availableBackends();
    if (std::find(renderers.begin(), renderers.end(), m_project.renderer) == renderers.end()) {
        std::string have;
        for (const std::string& name : renderers) have += (have.empty() ? "" : ", ") + name;
        problems.push_back(
            m_project.relative(m_project.file) + ": renderer \"" + m_project.renderer + "\" isn't available (" + have +
            ")");
    }
    std::vector<fs::path> scenes = m_tool.scenes;
    std::vector<std::string> checkedDocuments;   // .rml files a scene's UIDocument validated
    std::vector<std::string> checkedEffects;   // .efkefc files a scene's ParticleEffect validated
    std::vector<std::string> usedPrefabs;   // by the scenes checked
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
        world.each<const shockwave::PrefabInstance>([&](shockwave::Entity e, const shockwave::PrefabInstance& p) {
            if (world.has<shockwave::PrefabPart>(e)) return;
            const auto* name = world.get<shockwave::Name>(e);
            if (p.path.empty())
                report(scene.string() + ": entity '" + (name ? name->value : "?") + "': Prefab has no path");
            if (!p.source) return;
            usedPrefabs.push_back(p.source->path);
            usedPrefabs.insert(usedPrefabs.end(), p.source->uses.begin(), p.source->uses.end());
        });

        for (const auto& problem : shockwave::checkAudioScene(world, engine().assets()))
            report(scene.string() + ": " + problem);

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

        checkUIDocuments(world, scene.string(), problems, checkedDocuments);
        checkParticleEffects(world, scene.string(), problems, checkedEffects);

        // Navigation: a saved mesh that no longer matches the scene gets baked again on
        // every Play, which is slow and easy to miss.
        std::string navError;
        if (const auto saved = shockwave::loadSceneNavMesh(scene, navError);
            saved && shockwave::sceneUsesNavigation(world) &&
            saved->inputHash() !=
                shockwave::navInputHash(world, &engine().assets(), shockwave::findNavMeshSettings(world)))
            report(
                m_project.relative(shockwave::navMeshPathFor(scene)) +
                ": out of date, so Play bakes the navigation mesh again; run player --bake-navmesh or Bake in the "
                "editor");
        if (!navError.empty()) report(navError);

        // Start the game on it, as Play does, to catch physics and navigation setup problems.
        try {
            shockwave::PlaySession session(world, engine().jobs(), *m_instance, &engine());
            session.start();
            for (const auto& diagnostic : session.physics()->diagnostics()) {
                const shockwave::Name* name = world.get<shockwave::Name>(diagnostic.entity);
                report(scene.string() + ": entity '" + (name ? name->value : "?") + "': " + diagnostic.message);
            }
            for (const auto& diagnostic : session.navigation()->diagnostics())
                report(
                    scene.string() + ": " +
                    (diagnostic.entity.valid() ? "entity '" + entityName(diagnostic.entity) + "': " : "") +
                    diagnostic.message);
            session.stop();
        } catch (const std::exception& e) {
            report(scene.string() + ": gameplay failed to start: " + e.what());
        }
        if (problems.size() == before) std::printf("ok %s (%zu entities)\n", shown.c_str(), world.entityCount());
    }
    checkPrefabs(usedPrefabs, problems);
    // Documents no scene shows yet can still be shown by game code: check them too.
    if (m_tool.scenes.empty()) {
        for (const std::string& document : m_ui->documentFiles()) {
            if (std::find(checkedDocuments.begin(), checkedDocuments.end(), document) != checkedDocuments.end())
                continue;
            const std::string shown = m_project.relative(m_project.assetFolder / document);
            const auto uiProblems = m_ui->validate(document);
            for (const auto& problem : uiProblems) problems.push_back(shown + ": " + problem.message);
            if (uiProblems.empty()) std::printf("ok %s\n", shown.c_str());
        }
    }
    if (m_tool.scenes.empty()) checkUnusedEffects(checkedEffects, problems);
    // Input bindings: optional, but a broken file means broken controls.
    const fs::path inputFile = shockwave::inputBindingsPath(m_project.root);
    if (fs::exists(inputFile)) {
        const std::string shown = m_project.relative(inputFile);
        shockwave::InputBindings bindings;
        std::string error;
        if (!shockwave::loadInputBindings(inputFile, bindings, error)) {
            if (error.rfind(inputFile.string(), 0) == 0) error.replace(0, inputFile.string().size(), shown);
            problems.push_back(error);
        } else {
            const size_t before = problems.size();
            for (size_t i = 0; i < bindings.binds.size(); ++i) {
                for (size_t j = 0; j < i; ++j) {
                    if (bindings.binds[j] == bindings.binds[i]) {
                        problems.push_back(
                            shown + ": " + bindings.binds[i].first + " is bound to '" +
                            shockwave::formatBinding(bindings.binds[i].second) + "' twice");
                        break;
                    }
                }
            }
            if (problems.size() == before) std::printf("ok %s (%zu bindings)\n", shown.c_str(), bindings.binds.size());
        }
    }
    for (const std::string& problem : problems) std::printf("%s\n", problem.c_str());
    std::printf(
        "%zu problem%s in %zu scene%s\n", problems.size(), problems.size() == 1 ? "" : "s", scenes.size(),
        scenes.size() == 1 ? "" : "s");
    return problems.empty() ? 0 : 1;
}

// Every .swprefab in Content (game code can spawn any of them), or with scenes named on the
// command line, the prefabs those use.
void Player::checkPrefabs(std::vector<std::string> prefabs, std::vector<std::string>& problems) {
    const fs::path content = m_project.assetFolder;
    if (m_tool.scenes.empty()) {
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(content, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".swprefab")
                prefabs.push_back(entry.path().lexically_relative(content).generic_string());
        }
    }
    std::sort(prefabs.begin(), prefabs.end());
    prefabs.erase(std::unique(prefabs.begin(), prefabs.end()), prefabs.end());
    // Problems name the file as the project does ("Content/Prefabs/lamp.swprefab:3: ...").
    const std::string folder = m_project.relative(content) + "/";
    for (const std::string& path : prefabs) {
        const size_t before = problems.size();
        std::string error;
        const auto prefab = shockwave::prefabs().load(path, &error);
        if (!prefab) {
            problems.push_back(folder + path + ": " + error);
            continue;
        }
        for (const std::string& warning : prefab->warnings) problems.push_back(folder + warning);
        shockwave::World world = prefab->world;   // each() wants a mutable world
        auto where = [&](shockwave::Entity e) {
            const auto* name = world.get<shockwave::Name>(e);
            return folder + path + ": entity '" + (name ? name->value : "?") + "': ";
        };
        world.each<const shockwave::PrefabInstance>([&](shockwave::Entity e, const shockwave::PrefabInstance& p) {
            if (p.path.empty()) problems.push_back(where(e) + "Prefab has no path");
        });
        world.each<const shockwave::Light>([&](shockwave::Entity e, const shockwave::Light&) {
            if (!world.has<shockwave::Transform>(e))
                problems.push_back(where(e) + "Light has no Transform; it won't shine");
        });
        world.each<const shockwave::Environment>([&](shockwave::Entity e, const shockwave::Environment&) {
            problems.push_back(where(e) + "an Environment in a prefab changes the whole scene; keep it in the scene");
        });
        if (!world.has<shockwave::Transform>(prefab->root))
            problems.push_back(where(prefab->root) + "the root has no Transform, so copies can't be placed");
        if (problems.size() == before)
            std::printf("ok %s%s (%zu entities)\n", folder.c_str(), path.c_str(), world.entityCount());
    }
}

void Player::checkUIDocuments(
    shockwave::World& world, const std::string& scene, std::vector<std::string>& problems,
    std::vector<std::string>& checkedDocuments) {
    const std::string shownScene = m_project.relative(scene);
    world.each<const shockwave::UIDocument>([&](shockwave::Entity e, const shockwave::UIDocument& document) {
        const shockwave::Name* name = world.get<shockwave::Name>(e);
        const std::string where = shownScene + ": entity '" + (name ? name->value : "?") + "': UIDocument";
        if (document.document.empty()) {
            problems.push_back(where + " has no document");
            return;
        }
        checkedDocuments.push_back(document.document);
        std::string error;
        if (!document.preview.empty() && !shockwave::parseUIPreview(document.preview, &error))
            problems.push_back(where + " preview: " + error);
        for (const auto& problem : m_ui->validate(document.document))
            problems.push_back(where + " " + document.document + ": " + problem.message);
    });
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
        << "audio_hrtf true                        # optional: false pans spatial sound for speakers\n"
        << "audio_hrtf_quality high                # optional: low or high\n"
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

int writeInputSchema(const fs::path& out) {
    const std::string text = shockwave::inputReference();
    if (out == "-") {
        std::fputs(text.c_str(), stdout);
        return 0;
    }
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    if (!(file << text)) {
        std::fprintf(stderr, "Player: can't write %s\n", out.string().c_str());
        return 1;
    }
    std::printf("Wrote %s\n", out.string().c_str());
    return 0;
}

int listDevices(double seconds) {
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "Player: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    int code = 0;
    {
        shockwave::Input input;
        // Let the devices already plugged in arrive.
        auto pump = [&](double forSeconds) {
            const Uint64 end = SDL_GetTicks() + static_cast<Uint64>(forSeconds * 1000.0);
            do {
                input.beginFrame();
                SDL_Event event;
                while (SDL_PollEvent(&event)) input.handleEvent(event);
                input.update(0.01);
                if (const auto& pressed = input.lastPressed(); pressed && seconds > 0.0)
                    std::printf("pressed: %s\n", shockwave::formatBinding(*pressed).c_str()), std::fflush(stdout);
                SDL_Delay(10);
            } while (SDL_GetTicks() < end);
        };
        pump(0.3);
        const auto& devices = input.devices();
        std::printf("%zu controller%s\n", devices.size(), devices.size() == 1 ? "" : "s");
        for (const shockwave::InputDevice& d : devices) {
            static const char* types[] = {"unknown", "Xbox", "PS5", "PS4", "PS3", "Switch", "Steam", "other"};
            if (d.kind == shockwave::InputDevice::Kind::Gamepad) {
                std::printf(
                    "gamepad \"%s\": %s layout, player %d%s%s%s%s\n", d.name.c_str(), types[static_cast<int>(d.type)],
                    d.player, d.hasRumble ? ", rumble" : "", d.hasTriggerRumble ? ", trigger rumble" : "",
                    d.hasLight ? ", light" : "", d.hasGyro ? ", gyro" : "");
            } else {
                std::printf(
                    "joystick \"%s\": %zu axes, %zu buttons, %zu hats%s\n", d.name.c_str(), d.axes.size(),
                    d.buttons.size(), d.hats.size(), d.hasRumble ? ", rumble" : "");
            }
            std::printf("  axes:");
            for (float a : d.axes) std::printf(" %.2f", static_cast<double>(a));
            std::printf("\n");
        }
        if (seconds > 0.0) {
            std::printf("Press buttons or move axes for %g seconds...\n", seconds);
            std::fflush(stdout);
            pump(seconds);
        }
    }
    SDL_Quit();
    return code;
}

}   // namespace player
