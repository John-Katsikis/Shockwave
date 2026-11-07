// The player's tools: --check validates a project, its scenes and its input bindings;
// --schema writes the scene format reference.
#include "Player.h"

#include <Engine/Levels.h>

#include <Assets/AssetFolder.h>
#include <Assets/BuiltinAssets.h>
#include <Audio/Audio.h>
#include <Engine/Decals.h>
#include <Engine/Engine.h>
#include <Engine/Prefab.h>
#include <Engine/SceneFile.h>
#include <Engine/Systems.h>
#include <GameFramework/GameModule.h>
#include <GameFramework/PlaySession.h>
#include <GameFramework/SaveGame.h>
#include <GameUI/GameUI.h>
#include <Input/Input.h>
#include <Input/InputBinding.h>
#include <Navigation/Navigation.h>
#include <Physics/Physics.h>
#include <Projects/Project.h>
#include <Projects/ReleaseSettings.h>

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
    // Save games named on the command line are checked further down, after the scenes.
    std::vector<fs::path> saves;
    std::erase_if(scenes, [&](const fs::path& file) {
        if (file.extension() != ".swsave") return false;
        saves.push_back(file);
        return true;
    });
    const bool onlySaves = !m_tool.scenes.empty() && scenes.empty();
    std::vector<std::string> checkedDocuments;   // .rml files a scene's UIDocument validated
    std::vector<std::string> checkedProfiles;   // .swpostprocess files a scene's PostProcessVolume checked
    std::vector<std::string> checkedEffects;   // .efkefc files a scene's ParticleEffect validated
    std::vector<std::string> usedPrefabs;   // by the scenes checked
    std::vector<std::string> usedCircuits;   // .swcircuit files the scenes' Circuit components name
    if (scenes.empty()) {
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(m_project.sceneFolder, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".swscene") scenes.push_back(entry.path());
        }
        if (std::find(scenes.begin(), scenes.end(), m_project.startupScene) == scenes.end())
            scenes.push_back(m_project.startupScene);
        std::sort(scenes.begin(), scenes.end());
    }
    if (onlySaves) scenes.clear();
    // The level catalog (docs/Levels.md): a whole-project check visits every level's
    // scene, wherever it lives, and then the links between them.
    shockwave::LevelCatalog catalog;
    std::vector<shockwave::Diagnostic> levelDiagnostics;
    const std::string catalogShown =
        m_project.levelCatalog.empty() ? std::string() : m_project.relative(m_project.levelCatalog);
    const bool catalogRead = shockwave::projectLevels(m_project, catalog, levelDiagnostics);
    const bool checkLevels = m_tool.scenes.empty() && catalogRead && !catalog.implicit;
    std::map<fs::path, std::vector<const shockwave::LevelEntry*>> levelsByScene;
    std::map<fs::path, shockwave::LevelSceneFacts> sceneFacts;
    if (checkLevels) {
        shockwave::validateLevelCatalog(m_project, catalog, catalogShown, levelDiagnostics);
        for (const shockwave::LevelEntry& level : catalog.levels) {
            const fs::path file = (m_project.root / level.scene).lexically_normal();
            std::error_code ec;
            if (!fs::is_regular_file(file, ec)) continue;   // reported by validateLevelCatalog
            levelsByScene[file].push_back(&level);
            if (std::find(scenes.begin(), scenes.end(), file) == scenes.end()) scenes.push_back(file);
        }
    }
    if (m_tool.scenes.empty()) {   // every save game the project has
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(saveFolder(), ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".swsave") saves.push_back(entry.path());
        }
        std::sort(saves.begin(), saves.end());
    }
    checkNetworkProject(problems);   // before the scenes, whose network objects need its registry
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
        const fs::path sceneKey = fs::absolute(scene).lexically_normal();
        const bool isLevel = levelsByScene.count(sceneKey) != 0;
        if (!shockwave::loadScene(world, engine().assets(), scene.string(), error, hooks, &warnings)) {
            report(error);
            if (isLevel) sceneFacts[sceneKey].loadError = error;
            continue;
        }
        if (isLevel) sceneFacts[sceneKey] = shockwave::levelSceneFacts(world);
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
        // A catalog level's camera is checked by its kind below: a menu may be UI only.
        if (primaries == 0 && !isLevel)
            report(scene.string() + ": no entity has a primary Camera; the game has no view");
        if (primaries > 1) report(scene.string() + ": " + std::to_string(primaries) + " primary Cameras; keep one");

        // Material bindings (G02): a map read through TEXCOORD_1 on a mesh that has none
        // falls back to UV0, which is rarely what the artist meant.
        world.each<const shockwave::ModelRenderer>([&](shockwave::Entity e, const shockwave::ModelRenderer& m) {
            const auto asset = engine().assets().model(m.model);
            if (!asset) return;
            for (const auto& mesh : asset->data.meshes) {
                if (mesh.data.indices.empty() || !mesh.data.uv1.empty()) continue;
                const auto& material = asset->data.materials[mesh.material];
                const auto textures = shockwave::materialTextures(material);
                for (size_t slot = 0; slot < shockwave::kMaterialSlots; ++slot)
                    if (textures[slot] >= 0 && material.bindings[slot].uvSet == 1) {
                        const auto* name = world.get<shockwave::Name>(e);
                        report(
                            scene.string() + ": entity '" + (name ? name->value : "?") + "': model " +
                            engine().assets().modelName(m.model) + ", mesh '" + mesh.name + "': material '" +
                            material.name + "' reads its " +
                            shockwave::materialSlotName(shockwave::MaterialSlot(slot)) +
                            " map from UV1, which the mesh doesn't have (UV0 is used)");
                    }
            }
        });

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
        world.each<const shockwave::Environment>([&](shockwave::Entity e, const shockwave::Environment& environment) {
            if (++environments == 2)
                report(
                    scene.string() + ": entity '" + entityName(e) +
                    "': more than one Environment; only the first is used");
            // An environment map is a panorama: any other picture wraps around the scene
            // stretched, and lights and reflects it wrongly.
            if (environment.environmentMap.id == 0 || !m_assets) return;
            const std::string map = engine().assets().textureName(environment.environmentMap);
            const shockwave::AssetFolder::File* file = m_assets->find(map);
            if (file && file->height && file->width != 2 * file->height)
                report(
                    scene.string() + ": entity '" + entityName(e) + "': environment_map " + map + " is " +
                    std::to_string(file->width) + " x " + std::to_string(file->height) +
                    "; it should be an equirectangular panorama, twice as wide as it is tall");
        });

        checkUIDocuments(world, scene.string(), problems, checkedDocuments);
        checkParticleEffects(world, scene.string(), problems, checkedEffects);
        checkMaterialOverrides(world, scene.string(), problems);
        checkPostProcessVolumes(world, shown, problems, checkedProfiles);
        // Decals (G08): a box that can't land, or a material that won't resolve.
        world.each<const shockwave::Decal>([&](shockwave::Entity e, const shockwave::Decal& decal) {
            const std::string where = shown + ": entity '" + entityName(e) + "': Decal ";
            if (!world.has<shockwave::Transform>(e)) problems.push_back(where + "needs a Transform to be placed");
            if (const std::string problem = shockwave::decalProblem(decal, &engine().assets()); !problem.empty())
                problems.push_back(where + problem);
            else if (decal.receiverMask == 0)
                std::printf("warning %sreceiver_mask 0 lands on nothing\n", where.c_str());
        });
        checkLighting(world, shown, problems);
        checkAnimators(world, scene.string(), problems);
        checkCircuits(world, scene.string(), problems, usedCircuits);
        checkNetworkScene(world, shown, problems);

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
    std::map<std::string, shockwave::LevelSceneFacts> levelFacts;
    if (checkLevels) {
        for (const auto& [file, levels] : levelsByScene)
            for (const shockwave::LevelEntry* level : levels) levelFacts[level->id] = sceneFacts[file];
        shockwave::validateLevelLinks(catalog, levelFacts, catalogShown, levelDiagnostics);
    }
    {
        const size_t before = problems.size();
        for (const shockwave::Diagnostic& diagnostic : levelDiagnostics) {
            if (diagnostic.error())
                problems.push_back(diagnostic.text());
            else
                std::printf("warning %s\n", diagnostic.text().c_str());
        }
        if (checkLevels && problems.size() == before)
            std::printf(
                "ok %s (%zu level%s)\n", catalogShown.c_str(), catalog.levels.size(),
                catalog.levels.size() == 1 ? "" : "s");
    }
    checkPrefabs(usedPrefabs, problems);
    checkAllCircuits(usedCircuits, problems, catalogRead ? &catalog : nullptr, checkLevels ? &levelFacts : nullptr);
    // What everything checked reaches, file by file (docs/AssetResidency.md): references
    // that don't resolve, wrong case, paths leaving the project, cycles.
    if (!onlySaves) checkAssetGraph(scenes, problems);
    // Save games: they must read back the way a load would, entity handles and all.
    for (const fs::path& file : saves) {
        const std::string shown = m_project.relative(file);
        const size_t before = problems.size();
        auto report = [&](std::string problem) {
            if (problem.rfind(file.string(), 0) == 0) problem.replace(0, file.string().size(), shown);
            problems.push_back(std::move(problem));
        };
        shockwave::World world;
        shockwave::SaveInfo info;
        std::string error;
        std::vector<std::string> warnings;
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            report(file.string() + ": can't open it");
            continue;
        }
        if (!shockwave::readSaveGame(in, file.string(), engine().assets(), world, info, error, &warnings)) {
            report(error);
            continue;
        }
        for (const std::string& warning : warnings) report(warning);
        if (!shockwave::validSaveSlot(file.stem().string()))
            report(file.string() + ": the file name isn't a slot name (letters, digits, spaces, - and _)");
        if (!info.scene.empty() && !fs::exists(m_project.root / info.scene))
            report(
                file.string() + ": scene \"" + info.scene +
                "\" doesn't exist; `player --load` starts in the startup scene");
        if (problems.size() == before) std::printf("ok %s (%zu entities)\n", shown.c_str(), world.entityCount());
    }
    // Documents no scene shows yet can still be shown by game code: check them too.
    if (m_tool.scenes.empty()) {
        // The shell's own screens (Content/Shell, docs/AppShell.md) bind its "flow" model.
        std::unique_ptr<shockwave::GameUI> shellUI;
        for (const std::string& document : m_ui->documentFiles()) {
            if (std::find(checkedDocuments.begin(), checkedDocuments.end(), document) != checkedDocuments.end())
                continue;
            const std::string shown = m_project.relative(m_project.assetFolder / document);
            const bool shell = document.rfind("Shell/", 0) == 0;
            if (shell && !shellUI) {
                shellUI = std::make_unique<shockwave::GameUI>(nullptr, shockwave::GameUIOptions{"flow", false});
                shellUI->setContentFolder(m_project.assetFolder);
            }
            const auto uiProblems = shell ? shellUI->validate(document) : m_ui->validate(document);
            for (const auto& problem : uiProblems) problems.push_back(shown + ": " + problem.message);
            if (uiProblems.empty()) std::printf("ok %s\n", shown.c_str());
        }
    }
    if (m_tool.scenes.empty()) checkUnusedEffects(checkedEffects, problems);
    if (m_tool.scenes.empty()) checkMaterialFiles();
    if (m_tool.scenes.empty()) checkPostProcessFiles(checkedProfiles, problems);
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
    checkPackageSettings(m_project, problems);
    {
        // The release gates see the links the level check followed.
        std::map<std::string, shockwave::LevelSceneFacts> facts;
        for (const auto& [file, levels] : levelsByScene)
            for (const shockwave::LevelEntry* level : levels) facts[level->id] = sceneFacts[file];
        checkRelease(m_project, problems, checkLevels ? &facts : nullptr);
    }
    for (const std::string& problem : problems) std::printf("%s\n", problem.c_str());
    std::printf(
        "%zu problem%s in %zu scene%s\n", problems.size(), problems.size() == 1 ? "" : "s", scenes.size(),
        scenes.size() == 1 ? "" : "s");
    if (!saves.empty()) std::printf("%zu save game%s checked\n", saves.size(), saves.size() == 1 ? "" : "s");
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
        << "game_circuit \"Content/Circuits/Game.swcircuit\"   # optional; runs for the whole game (docs/Circuits.md)\n"
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

namespace {

std::string jsonText(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out += c;
    }
    return out + "\"";
}

std::string jsonValues(const std::map<std::string, std::string>& values) {
    std::string out = "{";
    size_t n = 0;
    for (const auto& [key, value] : values) out += (n++ ? ", " : "") + jsonText(key) + ": " + jsonText(value);
    return out + "}";
}

// One save as a JSON object: its envelope, and whether this game can load it.
std::string
saveJson(const shockwave::Project& project, const shockwave::SaveInfo& save, const shockwave::SaveIdentity& identity) {
    std::string problem = save.problem;
    if (problem.empty()) shockwave::checkSaveCompatible(save, identity, problem);
    std::ostringstream o;
    o << "{\"slot\": " << jsonText(save.slot) << ", \"file\": " << jsonText(project.relative(save.file))
      << ", \"loadable\": " << (problem.empty() ? "true" : "false") << ", \"problem\": " << jsonText(problem)
      << ", \"code\": " << jsonText(shockwave::saveErrorName(shockwave::saveErrorOf(problem)))
      << ", \"format\": " << save.format << ", \"application\": " << jsonText(save.application)
      << ", \"schema\": " << save.schema << ", \"content\": " << jsonText(save.content)
      << ", \"profile\": " << jsonText(save.profile) << ", \"revision\": " << save.revision
      << ", \"checksum\": " << (save.checksum ? "true" : "false") << ", \"level\": " << jsonText(save.level)
      << ", \"spawn\": " << jsonText(save.spawn) << ", \"checkpoint\": " << jsonText(save.checkpoint)
      << ", \"title\": " << jsonText(save.title) << ", \"scene\": " << jsonText(save.scene)
      << ", \"saved_at\": " << jsonText(save.savedAt) << ", \"play_time\": " << save.playTime
      << ", \"entities\": " << save.entities << ", \"values\": " << jsonValues(save.values)
      << ", \"campaign\": " << jsonValues(save.campaign) << "}";
    return o.str();
}

int writeJson(const fs::path& jsonFile, const std::string& text) {
    if (jsonFile == "-") {
        std::fputs(text.c_str(), stdout);
        return 0;
    }
    std::ofstream json(jsonFile, std::ios::trunc | std::ios::binary);
    json << text;
    json.close();
    if (!json) {
        std::fprintf(stderr, "Player: can't write %s\n", jsonFile.string().c_str());
        return 1;
    }
    std::printf("Wrote %s\n", jsonFile.string().c_str());
    return 0;
}

}   // namespace

shockwave::SaveIdentity saveIdentityFor(const shockwave::Project& project, const shockwave::UserPaths* paths) {
    shockwave::SaveIdentity identity;
    identity.application = applicationIdFor(project);
    identity.profile = paths ? paths->profile() : std::string("default");
    identity.schema = project.saveSchema;
    // The release version, for information: a save from another version still loads.
    shockwave::ReleaseFile release;
    shockwave::Diagnostic ignored;
    const fs::path contract = shockwave::releasePath(project);
    std::error_code ec;
    if (fs::exists(contract, ec) && shockwave::readRelease(contract, project.relative(contract), release, ignored))
        identity.content = release.settings.releaseVersion;
    return identity;
}

int listSaves(
    const shockwave::Project& project, const fs::path& folder, const shockwave::SaveIdentity& identity,
    const fs::path& jsonFile) {
    const auto saves = folder.empty() ? std::vector<shockwave::SaveInfo>{} : shockwave::listSaveGames(folder, true);
    if (saves.empty()) std::printf("No save games in %s\n", project.relative(folder).c_str());
    for (const shockwave::SaveInfo& save : saves) {
        std::string problem = save.problem;
        if (problem.empty()) shockwave::checkSaveCompatible(save, identity, problem);
        if (!save.problem.empty()) {
            std::printf("%-16s can't load: %s\n", save.slot.c_str(), problem.c_str());
            continue;
        }
        const std::string where = save.level.empty() ? save.scene : "level " + save.level;
        std::printf(
            "%-16s %s  %8.1f s  %5zu entities  %s%s%s%s%s\n", save.slot.c_str(), save.savedAt.c_str(), save.playTime,
            save.entities, where.c_str(), save.title.empty() ? "" : "  ", save.title.c_str(),
            problem.empty() ? "" : "  can't load: ", problem.c_str());
    }
    if (jsonFile.empty()) return 0;
    std::string json = "[";
    for (size_t i = 0; i < saves.size(); ++i) json += (i ? ",\n " : "\n ") + saveJson(project, saves[i], identity);
    json += "\n]\n";
    return writeJson(jsonFile, json);
}

int saveInfo(
    const shockwave::Project& project, const fs::path& file, const shockwave::SaveIdentity& identity,
    const fs::path& jsonFile) {
    shockwave::SaveInfo info;
    std::string error;
    if (!shockwave::readSaveInfo(file, info, error)) {
        info.slot = file.stem().string();
        info.file = file;
        info.problem = error.empty() ? "can't read it" : error;
    }
    std::string problem = info.problem;
    if (problem.empty()) shockwave::checkSaveCompatible(info, identity, problem);
    if (info.problem.empty()) {
        std::printf("%s\n", project.relative(file).c_str());
        std::printf(
            "  format %d, schema %d, revision %llu, checksum %s\n", info.format, info.schema,
            static_cast<unsigned long long>(info.revision), info.checksum ? "ok" : "none (edited by hand?)");
        std::printf(
            "  application %s, profile %s, content %s\n", info.application.empty() ? "-" : info.application.c_str(),
            info.profile.empty() ? "-" : info.profile.c_str(), info.content.empty() ? "-" : info.content.c_str());
        std::printf(
            "  level %s, spawn %s, scene %s\n", info.level.empty() ? "-" : info.level.c_str(),
            info.spawn.empty() ? "-" : info.spawn.c_str(), info.scene.empty() ? "-" : info.scene.c_str());
        std::printf(
            "  saved %s, play time %.1f s, %zu entities, %zu values, %zu campaign values\n",
            info.savedAt.empty() ? "-" : info.savedAt.c_str(), info.playTime, info.entities, info.values.size(),
            info.campaign.size());
    }
    if (problem.empty())
        std::printf("loadable\n");
    else
        std::fprintf(stderr, "can't load: %s\n", problem.c_str());
    int code = problem.empty() ? 0 : 1;
    if (!jsonFile.empty() && writeJson(jsonFile, saveJson(project, info, identity) + "\n")) code = 1;
    return code;
}

int saveMigrate(
    const shockwave::Project& project, const fs::path& file, const fs::path& output,
    const shockwave::SaveIdentity& identity) {
    if (output.empty()) {
        std::fprintf(stderr, "Player: --save-migrate needs --output newfile.swsave\n");
        return 2;
    }
    std::error_code ec;
    if (fs::absolute(file).lexically_normal() == fs::absolute(output).lexically_normal() ||
        (fs::exists(output, ec) && fs::equivalent(file, output, ec))) {
        std::fprintf(stderr, "Player: --save-migrate never writes over the save it reads; pick another --output\n");
        return 2;
    }
    if (fs::exists(output, ec)) {
        std::fprintf(
            stderr, "Player: %s already exists; remove it or pick another --output\n", output.string().c_str());
        return 2;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "Player: can't read %s\n", file.string().c_str());
        return 1;
    }
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const int fromFormat = [&] {
        shockwave::SaveInfo before;
        std::string ignored;
        return shockwave::readSaveInfo(file, before, ignored) ? before.format : 0;
    }();
    // An older save names its scene, not its level: the catalog's level for that scene.
    shockwave::LevelCatalog catalog;
    std::vector<shockwave::Diagnostic> ignored;
    if (!shockwave::projectLevels(project, catalog, ignored)) catalog = shockwave::implicitLevelCatalog(project);
    auto upgrade = [&](shockwave::SaveInfo& save) {
        if (!save.level.empty() || save.scene.empty()) return;
        const fs::path wanted = fs::absolute(project.root / save.scene).lexically_normal();
        for (const shockwave::LevelEntry& level : catalog.levels)
            if (fs::absolute(project.root / level.scene).lexically_normal() == wanted) save.level = level.id;
    };
    shockwave::SaveInfo result;
    std::string error;
    if (!shockwave::migrateSaveText(text, file.string(), identity, upgrade, result, error)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    if (!shockwave::writeSaveFile(output, text, error)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    std::printf(
        "Wrote %s (format %d to %d, schema %d, level %s)\n", output.string().c_str(), fromFormat, result.format,
        result.schema, result.level.empty() ? "-" : result.level.c_str());
    return 0;
}

}   // namespace player
