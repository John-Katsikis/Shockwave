// Level authoring in the scene (S07): the Create menu's level spawn, exit and checkpoint,
// the catalog's ids for an exit's destination, and the scene checks the Levels window's
// Validate runs, each labelled with the kind of fix it needs (docs/Levels.md#checks).
#include "Editor.h"

#include <Circuits/Circuit.h>
#include <Circuits/CircuitProgram.h>
#include <Engine/Levels.h>
#include <Engine/Prefab.h>
#include <Engine/SceneFile.h>
#include <Navigation/NavMesh.h>
#include <Navigation/Navigation.h>
#include <Physics/Physics.h>
#include <Projects/LevelCatalog.h>
#include <SDL3/SDL.h>

#include <algorithm>

namespace editor {

namespace fs = std::filesystem;
using shockwave::Entity;
using shockwave::kNullEntity;

namespace {

// An id no entity's component in the world uses yet: base, base-2, ...
template <class T>
std::string freshComponentId(shockwave::World& world, const std::string& base) {
    auto taken = [&](const std::string& id) {
        bool found = false;
        world.each<const T>([&](Entity, const T& c) { found = found || c.id == id; });
        return found;
    };
    if (!taken(base)) return base;
    for (int n = 2;; ++n)
        if (const std::string id = base + "-" + std::to_string(n); !taken(id)) return id;
}

}   // namespace

Entity Editor::placeLevelEntity(const std::string& name, Entity parent) {
    pushUndo(m_world);
    const Entity e = m_world.create();
    m_world.add<shockwave::Name>(e, {uniqueName(name)});
    m_world.add<shockwave::Transform>(e, {placementPoint()});
    if (parent.valid()) shockwave::setParent(m_world, e, parent);
    select(e);
    return e;
}

Entity Editor::createLevelSpawn(Entity parent) {
    const bool hasStart = shockwave::findLevelSpawn(m_world, "start").valid();
    const Entity e = placeLevelEntity("Spawn", parent);
    m_world.add<shockwave::LevelSpawn>(
        e, {hasStart ? freshComponentId<shockwave::LevelSpawn>(m_world, "spawn") : std::string("start")});
    return e;
}

Entity Editor::createLevelExit(Entity parent) {
    shockwave::LevelExit exit;
    // The catalog's next level for this scene, when it has one: the usual way out.
    if (!m_project.levelCatalog.empty() && !m_scenePath.empty()) {
        shockwave::LevelCatalog catalog;
        std::vector<shockwave::Diagnostic> ignored;
        if (shockwave::projectLevels(m_project, catalog, ignored))
            for (const auto& level : catalog.levels)
                if (fs::absolute(m_project.root / level.scene).lexically_normal() ==
                    fs::absolute(m_scenePath).lexically_normal())
                    exit.destination = level.next;
    }
    const Entity e = placeLevelEntity("Exit", parent);
    m_world.add<shockwave::LevelExit>(e, exit);
    return e;
}

Entity Editor::createCheckpoint(Entity parent) {
    // A checkpoint restarts at a spawn of its own, on the same entity.
    const std::string id = freshComponentId<shockwave::Checkpoint>(m_world, "checkpoint");
    const std::string spawn = freshComponentId<shockwave::LevelSpawn>(m_world, id);
    const Entity e = placeLevelEntity("Checkpoint", parent);
    m_world.add<shockwave::LevelSpawn>(e, {spawn});
    m_world.add<shockwave::Checkpoint>(e, {id, spawn, true});
    return e;
}

void Editor::drawLevelCreateItems(Entity parent) {
    ImGui::SeparatorText("Level");
    if (ImGui::MenuItem("Level spawn")) createLevelSpawn(parent);
    ImGui::SetItemTooltip("Where players arrive; the first one is \"start\", the level's default");
    if (ImGui::MenuItem("Level exit")) createLevelExit(parent);
    ImGui::SetItemTooltip(
        "A way out to another level, to the catalog's next level at first. What triggers it is the game's: a trigger "
        "and a circuit's Level.UseExit, or game code");
    if (ImGui::MenuItem("Checkpoint")) createCheckpoint(parent);
    ImGui::SetItemTooltip("A place to restart from, with its own spawn");
}

const std::vector<std::string>& Editor::catalogLevelIds() {
    const double now = double(SDL_GetTicksNS()) * 1e-9;
    if (now - m_catalogChecked < 1.0) return m_catalogIds;
    m_catalogChecked = now;
    std::error_code ec;
    const auto time =
        m_project.levelCatalog.empty() ? fs::file_time_type{} : fs::last_write_time(m_project.levelCatalog, ec);
    if (time == m_catalogTime && !m_catalogIds.empty()) return m_catalogIds;
    m_catalogTime = time;
    m_catalogIds.clear();
    shockwave::LevelCatalog catalog;
    std::vector<shockwave::Diagnostic> ignored;
    if (!m_project.levelCatalog.empty() && shockwave::projectLevels(m_project, catalog, ignored))
        for (const auto& level : catalog.levels) m_catalogIds.push_back(level.id);
    return m_catalogIds;
}

std::vector<shockwave::Diagnostic> Editor::levelSceneProblems(const fs::path& scene) {
    std::vector<shockwave::Diagnostic> out;
    const std::string shown = m_project.relative(scene);
    auto add = [&](const char* code, std::string message, bool error = true) {
        shockwave::Diagnostic d;
        d.code = code;
        d.severity = error ? shockwave::DiagnosticSeverity::Error : shockwave::DiagnosticSeverity::Warning;
        d.file = shown;
        d.message = std::move(message);
        out.push_back(std::move(d));
    };
    shockwave::World world;
    std::string error;
    shockwave::SceneFileHooks fileHooks;
    fileHooks.readHeader = [](const std::string&, std::istream&) {};
    std::vector<std::string> warnings;
    if (!shockwave::loadScene(world, engine().assets(), scene.string(), error, fileHooks, &warnings))
        return out;   // the level's facts already say it didn't load
    // Content: what loaded with a problem (a name that matches no entity, an unknown asset).
    for (std::string& warning : warnings) {
        if (warning.rfind(scene.string(), 0) == 0) warning.erase(0, scene.string().size());
        if (warning.rfind(':', 0) == 0) {   // ":12: message" -> line 12
            const size_t end = warning.find(": ", 1);
            const std::string number = warning.substr(1, end == std::string::npos ? 0 : end - 1);
            if (!number.empty() && number.find_first_not_of("0123456789") == std::string::npos) {
                add("SceneReference", warning.substr(end + 2));
                out.back().line = std::stoi(number);
                continue;
            }
        }
        add("SceneReference", warning.rfind(": ", 0) == 0 ? warning.substr(2) : warning);
    }
    auto named = [&](Entity e) {
        const auto* name = world.get<shockwave::Name>(e);
        return "entity '" + std::string(name ? name->value : "?") + "': ";
    };

    // Gameplay: the game's own rules for its content.
    std::vector<std::string> gameProblems;
    if (m_gameInstance) gameProblems = m_gameInstance->checkScene(world);
    for (const std::string& problem : gameProblems) add("GameContent", problem);

    // Circuits: each one exists and compiles.
    world.each<const shockwave::Circuit>([&](Entity e, const shockwave::Circuit& circuit) {
        if (world.has<shockwave::PrefabPart>(e)) return;
        if (circuit.circuit.empty()) {
            add("CircuitMissing", named(e) + "Circuit names no circuit");
            return;
        }
        std::error_code ec;
        if (!fs::is_regular_file(shockwave::circuitLibrary().file(circuit.circuit), ec)) {
            add("CircuitMissing", named(e) + "Circuit: " + circuit.circuit + " doesn't exist");
            return;
        }
        std::vector<std::string> found;
        shockwave::circuitLibrary().load(circuit.circuit, shockwave::CircuitKind::Entity, &found);
        for (const std::string& problem : found)
            add("CircuitError", named(e) + problem, problem.find(": warning: ") == std::string::npos);
    });

    // Navigation: a saved mesh that's missing or stale is baked again on every Play.
    std::string navError;
    const auto saved = shockwave::loadSceneNavMesh(scene, navError);
    if (!navError.empty()) add("NavigationUnreadable", navError);
    if (shockwave::sceneUsesNavigation(world) &&
        (!saved || saved->inputHash() !=
                       shockwave::navInputHash(world, &engine().assets(), shockwave::findNavMeshSettings(world))))
        add("NavigationOutOfDate",
            std::string(saved ? "the navigation mesh is out of date" : "the navigation mesh isn't baked") +
                ", so Play bakes it again; Bake it in the Navigation window",
            false);

    // Physics and navigation setup, and the game starting: a Play that isn't shown. Not
    // while the editor is playing, which owns the engine's play state.
    if (playing() || !m_gameInstance) return out;
    try {
        shockwave::PlaySession session(world, engine().jobs(), *m_gameInstance, &engine());
        session.start();
        for (const auto& d : session.physics()->diagnostics())
            add("PhysicsSetup", (d.entity.valid() ? named(d.entity) : std::string()) + d.message);
        for (const auto& d : session.navigation()->diagnostics())
            add("NavigationSetup", (d.entity.valid() ? named(d.entity) : std::string()) + d.message);
        session.stop();
    } catch (const std::exception& e) {
        // The game refusing content its own check already listed says nothing new.
        if (gameProblems.empty()) add("GameStartFailed", std::string("gameplay failed to start: ") + e.what());
    }
    return out;
}

}   // namespace editor

namespace editor {

// ---- --level-authoring -----------------------------------------------------------------

void Editor::runLevelAuthoring() {
    m_options.levelAuthoring = false;
    std::vector<std::string> failures;
    auto step = [&](bool ok, const std::string& what) {
        if (!ok) failures.push_back(what + (m_levels.message().empty() ? "" : ": " + m_levels.message()));
        return ok;
    };
    const LevelsPanel::Hooks hooks = levelsHooks();
    m_levels.load(m_project);
    if (step(!m_levels.edited().levels.empty(), "the project has no level catalog")) {
        // A new level, furnished with a spawn, a checkpoint and an exit back to the entry.
        m_levels.newLevel(m_project, hooks);
        const shockwave::LevelEntry added = m_levels.edited().levels.back();
        if (step(!m_levels.messageIsError(), "New level") && step(m_levels.save(m_project), "Save")) {
            std::printf("level authoring: new level \"%s\" (%s)\n", added.id.c_str(), added.scene.c_str());
            if (step(openScene(m_project.root / added.scene), "opening " + added.scene)) {
                const Entity spawn = createLevelSpawn(kNullEntity);
                const Entity checkpoint = createCheckpoint(kNullEntity);
                const Entity exit = createLevelExit(kNullEntity);
                m_world.get<shockwave::LevelExit>(exit)->destination = m_levels.edited().entry;
                step(saveScene(), "saving " + added.scene);
                std::printf(
                    "level authoring: placed spawn \"%s\", checkpoint \"%s\" and an exit to \"%s\"\n",
                    m_world.get<shockwave::LevelSpawn>(spawn)->id.c_str(),
                    m_world.get<shockwave::Checkpoint>(checkpoint)->id.c_str(),
                    m_world.get<shockwave::LevelExit>(exit)->destination.c_str());
            }
        }
        // A copy of the level after the entry: its scene and its catalog entry.
        const auto* entry = m_levels.edited().find(m_levels.edited().entry);
        const std::string source = entry && !entry->next.empty() ? entry->next : m_levels.edited().entry;
        if (step(m_levels.selectLevel(source), "selecting " + source)) {
            const size_t before = m_levels.edited().levels.size();
            m_levels.duplicateLevel(m_project);
            if (step(!m_levels.messageIsError() && m_levels.edited().levels.size() == before + 1, "Duplicate level") &&
                step(m_levels.save(m_project), "Save"))
                std::printf("level authoring: duplicated \"%s\": %s\n", source.c_str(), m_levels.message().c_str());
        }
    }
    // Validate, and the findings by kind, the way the window lists them.
    m_levels.validate(m_project, hooks);
    size_t errors = 0;
    for (int kind = 0; kind < shockwave::kProblemKinds; ++kind)
        for (const auto& d : m_levels.diagnostics()) {
            if (int(shockwave::problemKindOf(d)) != kind) continue;
            errors += d.error();
            std::printf(
                "level authoring: %s %s: %s [%s]\n", shockwave::problemKindName(shockwave::ProblemKind(kind)),
                shockwave::diagnosticSeverityName(d.severity), d.text().c_str(), d.code.c_str());
        }
    for (const std::string& failure : failures) std::fprintf(stderr, "level authoring: %s\n", failure.c_str());
    std::printf(
        "level authoring: %zu level%s, %zu error%s, %s\n", m_levels.edited().levels.size(),
        m_levels.edited().levels.size() == 1 ? "" : "s", errors, errors == 1 ? "" : "s",
        failures.empty() ? "every step worked" : "FAILED");
    m_exitCode = failures.empty() && errors == 0 ? 0 : 1;
    engine().quit();
}

}   // namespace editor
