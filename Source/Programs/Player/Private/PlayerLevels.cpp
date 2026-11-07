// The level catalog (docs/Levels.md): --levels, --level-schema, --level, and the part of
// --check that follows links between levels.
#include "Player.h"

#include <Engine/Levels.h>
#include <Engine/SceneFile.h>
#include <Projects/Diagnostic.h>

#include <cstdio>
#include <fstream>

namespace player {

namespace {

std::string json(const std::string& text) { return shockwave::diagnosticJsonString(text); }

std::string jsonList(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) out += (i ? ", " : "") + json(items[i]);
    return out + "]";
}

}   // namespace

int levelSchema(const fs::path& file) {
    const std::string schema = shockwave::levelSchema();
    if (file == "-") {
        std::fputs(schema.c_str(), stdout);
        return 0;
    }
    std::ofstream out(file, std::ios::binary);
    out << schema;
    out.close();
    if (!out) {
        std::fprintf(stderr, "Player: can't write %s\n", file.string().c_str());
        return 1;
    }
    std::printf("Wrote %s\n", file.string().c_str());
    return 0;
}

void applyStartLevel(shockwave::Project& project, const std::string& level) {
    shockwave::LevelCatalog catalog;
    std::vector<shockwave::Diagnostic> diagnostics;
    if (!shockwave::projectLevels(project, catalog, diagnostics)) {
        std::string text;
        for (const auto& d : diagnostics)
            if (d.error()) text += (text.empty() ? "" : "\n") + d.text();
        throw std::runtime_error(text);
    }
    const shockwave::LevelEntry* entry = catalog.find(level);
    if (!entry) {
        std::string have;
        for (const auto& l : catalog.levels) have += (have.empty() ? "" : ", ") + l.id;
        throw std::runtime_error("--level " + level + ": no such level (there are: " + have + ")");
    }
    project.startupScene = (project.root / entry->scene).lexically_normal();
}

std::map<std::string, shockwave::LevelSceneFacts> Player::levelFacts(const shockwave::LevelCatalog& catalog) {
    std::map<std::string, shockwave::LevelSceneFacts> byScene, byLevel;
    for (const shockwave::LevelEntry& level : catalog.levels) {
        if (level.scene.empty()) continue;
        auto found = byScene.find(level.scene);
        if (found == byScene.end()) {
            shockwave::World world;
            std::string error;
            shockwave::SceneFileHooks hooks;
            hooks.readHeader = [](const std::string&, std::istream&) {};
            const fs::path file = m_project.root / level.scene;
            shockwave::LevelSceneFacts facts;
            std::error_code ec;
            if (!fs::is_regular_file(file, ec)) {
                facts.loadError = "it doesn't exist";
            } else if (!shockwave::loadScene(world, engine().assets(), file.string(), error, hooks)) {
                if (error.rfind(file.string(), 0) == 0) error.replace(0, file.string().size(), level.scene);
                facts.loadError = error;
            } else {
                facts = shockwave::levelSceneFacts(world);
            }
            found = byScene.emplace(level.scene, std::move(facts)).first;
        }
        byLevel.emplace(level.id, found->second);
    }
    return byLevel;
}

int Player::releaseInfoTool() {
    shockwave::LevelCatalog catalog;
    std::vector<shockwave::Diagnostic> ignored;   // reported by the gate itself
    if (shockwave::projectLevels(m_project, catalog, ignored) && !catalog.implicit) {
        const auto facts = levelFacts(catalog);
        return releaseInfo(m_project, m_tool.benchmarkJson, &facts);
    }
    return releaseInfo(m_project, m_tool.benchmarkJson);
}

int Player::levels() {
    shockwave::LevelCatalog catalog;
    std::vector<shockwave::Diagnostic> diagnostics;
    const std::string shown =
        m_project.levelCatalog.empty() ? std::string() : m_project.relative(m_project.levelCatalog);
    const bool read = shockwave::projectLevels(m_project, catalog, diagnostics);
    std::map<std::string, shockwave::LevelSceneFacts> facts;
    if (read) {
        if (!catalog.implicit) shockwave::validateLevelCatalog(m_project, catalog, shown, diagnostics);
        facts = levelFacts(catalog);
        if (!catalog.implicit) shockwave::validateLevelLinks(catalog, facts, shown, diagnostics);
    }
    const auto reachable = read ? shockwave::reachableLevels(catalog, &facts) : std::vector<std::string>{};
    const bool ok = !shockwave::hasErrors(diagnostics);
    const fs::path& out = m_tool.benchmarkJson;
    if (out.empty()) {
        if (!read) {
            std::printf("levels %s doesn't read\n", shown.c_str());
        } else if (catalog.implicit) {
            std::printf(
                "levels none: no level catalog, so the startup scene is the only level\nlevel %s gameplay %s\n",
                shockwave::kImplicitLevelId, catalog.levels[0].scene.c_str());
        } else {
            std::printf(
                "levels %s: %zu level%s, entry %s\n", shown.c_str(), catalog.levels.size(),
                catalog.levels.size() == 1 ? "" : "s", catalog.entry.c_str());
            for (const shockwave::LevelEntry& level : catalog.levels) {
                const auto& f = facts[level.id];
                std::string exits;
                for (const auto& exit : f.exits)
                    exits +=
                        (exits.empty() ? "" : ", ") + exit.destination + (exit.spawn.empty() ? "" : ":" + exit.spawn);
                std::printf(
                    "level %s %s %s%s%s, %zu spawn%s, exits: %s%s\n", level.id.c_str(),
                    shockwave::levelKindName(level.kind), level.scene.c_str(), level.next.empty() ? "" : ", next ",
                    level.next.c_str(), f.spawns.size(), f.spawns.size() == 1 ? "" : "s",
                    exits.empty() ? "none" : exits.c_str(),
                    std::find(reachable.begin(), reachable.end(), level.id) == reachable.end() ? " (unreachable)" : "");
            }
        }
        for (const shockwave::Diagnostic& d : diagnostics)
            std::printf("%s %s\n", shockwave::diagnosticSeverityName(d.severity), d.text().c_str());
        size_t errors = 0;
        for (const auto& d : diagnostics) errors += d.error();
        std::printf("%s\n", ok ? "ok" : (std::to_string(errors) + " problem" + (errors == 1 ? "" : "s")).c_str());
        return ok ? 0 : 1;
    }
    std::string text = "{\n  \"format\": 1,\n  \"mode\": \"levels\",\n";
    text += "  \"project\": " + json(m_project.name) + ",\n";
    text += "  \"catalog\": " + (shown.empty() ? std::string("null") : json(shown)) + ",\n";
    text += std::string("  \"implicit\": ") + (catalog.implicit ? "true" : "false") + ",\n";
    text += "  \"entry\": " + (read ? json(catalog.entry) : std::string("null")) + ",\n";
    text += "  \"levels\": [";
    for (size_t i = 0; i < catalog.levels.size(); ++i) {
        const shockwave::LevelEntry& level = catalog.levels[i];
        const auto& f = facts[level.id];
        std::vector<std::string> spawns, checkpoints;
        for (const auto& s : f.spawns) spawns.push_back(s.id);
        for (const auto& c : f.checkpoints) checkpoints.push_back(c.id);
        std::string exits = "[";
        for (size_t j = 0; j < f.exits.size(); ++j)
            exits += std::string(j ? ", " : "") + "{\"destination\": " + json(f.exits[j].destination) +
                     ", \"spawn\": " + json(f.exits[j].spawn) +
                     ", \"enabled\": " + (f.exits[j].enabled ? "true" : "false") +
                     ", \"entity\": " + json(f.exits[j].entity) + "}";
        exits += "]";
        text += std::string(i ? ",\n    " : "\n    ") + "{\"id\": " + json(level.id) +
                ", \"scene\": " + json(level.scene) + ", \"kind\": " + json(shockwave::levelKindName(level.kind)) +
                ", \"default_spawn\": " + json(level.defaultSpawn) + ", \"next\": " + json(level.next) +
                ", \"preload\": " + jsonList(level.preload) + ", \"loaded\": " + (f.loaded ? "true" : "false") +
                ", \"reachable\": " +
                (std::find(reachable.begin(), reachable.end(), level.id) != reachable.end() ? "true" : "false") +
                ", \"spawns\": " + jsonList(spawns) + ", \"exits\": " + exits +
                ", \"checkpoints\": " + jsonList(checkpoints) +
                ", \"persistent_ids\": " + std::to_string(f.identities.size()) +
                ", \"primary_cameras\": " + std::to_string(f.primaryCameras) +
                ", \"user_interface\": " + (f.hasUserInterface ? "true" : "false") + "}";
    }
    text += catalog.levels.empty() ? "],\n" : "\n  ],\n";
    text += "  \"diagnostics\": [";
    for (size_t i = 0; i < diagnostics.size(); ++i)
        text += std::string(i ? ",\n    " : "\n    ") + shockwave::diagnosticJson(diagnostics[i]);
    text += diagnostics.empty() ? "],\n" : "\n  ],\n";
    text += std::string("  \"ok\": ") + (ok ? "true" : "false") + "\n}\n";
    if (out == "-") {
        std::fputs(text.c_str(), stdout);
    } else {
        std::ofstream file(out, std::ios::binary);
        file << text;
        file.close();
        if (!file) {
            std::fprintf(stderr, "Player: can't write %s\n", out.string().c_str());
            return 1;
        }
        std::printf("Wrote %s\n", out.string().c_str());
    }
    return ok ? 0 : 1;
}

}   // namespace player

namespace player {

// ---- Runtime levels (docs/LevelLifetime.md) ---------------------------------------------

void Player::loadLevelCatalog() {
    if (m_catalogLoaded) return;
    m_catalogLoaded = true;
    std::vector<shockwave::Diagnostic> ignored;   // --check and --levels report them
    if (!shockwave::projectLevels(m_project, m_catalog, ignored))
        m_catalog = shockwave::implicitLevelCatalog(m_project);
}

std::string Player::levelForScene(const fs::path& scene) {
    loadLevelCatalog();   // the first level is named before anything else needs the catalog
    const fs::path wanted = fs::absolute(scene).lexically_normal();
    for (const shockwave::LevelEntry& level : m_catalog.levels)
        if (fs::absolute(m_project.root / level.scene).lexically_normal() == wanted) return level.id;
    return shockwave::kImplicitLevelId;   // the implicit level, or a scene from --scene
}

fs::path Player::sceneForLevel(const std::string& level) const {
    if (const shockwave::LevelEntry* entry = m_catalog.find(level))
        return (m_project.root / entry->scene).lexically_normal();
    return {};
}

shockwave::LevelHost::Hooks Player::levelHooks() {
    shockwave::LevelHost::Hooks hooks;
    // Before a request is accepted: a level of the catalog, and its default spawn.
    hooks.resolve = [this](shockwave::TravelRequest& request, shockwave::TravelError& code, std::string& error) {
        const shockwave::LevelEntry* entry = m_catalog.find(request.level);
        if (!entry) {
            std::string have;
            for (const auto& l : m_catalog.levels) have += (have.empty() ? "" : ", ") + l.id;
            code = shockwave::TravelError::UnknownLevel;
            error = "no level \"" + request.level + "\" in the catalog (there are: " + have + ")";
            return false;
        }
        if (request.spawn.empty()) request.spawn = entry->defaultSpawn;
        return true;
    };
    hooks.load = [this](const shockwave::LevelRequest& request, shockwave::World& into, std::string& error) {
        const fs::path scene = sceneForLevel(request.level);
        if (scene.empty()) {
            std::string have;
            for (const auto& l : m_catalog.levels) have += (have.empty() ? "" : ", ") + l.id;
            error = "no level \"" + request.level + "\" in the catalog (there are: " + have + ")";
            return false;
        }
        std::vector<std::string> warnings;
        if (!shockwave::loadScene(into, engine().assets(), scene.string(), error, {}, &warnings)) return false;
        if (!warnings.empty()) {
            error = m_project.relative(scene) + " has unresolved content: " + warnings.front();
            return false;
        }
        std::string navError;
        if (shockwave::loadSceneNavMesh(scene, navError); !navError.empty())
            SW_LOG(shockwave::LogNavigation, Warning, "{}; baking instead", navError);
        return true;
    };
    hooks.configure = [this](shockwave::PlaySession& session, const shockwave::LevelRequest& request) {
        fs::path scene = sceneForLevel(request.level);
        if (scene.empty()) scene = m_project.startupScene;
        session.setSaveFolder(saveFolder(), m_project.relative(scene));
        session.setSaveIdentity(saveIdentity());
        session.setGameCircuit(m_project.gameCircuitPath());
        session.setOnlineMatch(m_match.configured() ? &m_match : nullptr);
        if (m_levelNetworkStart) session.setNetworkStart(*m_levelNetworkStart, m_levelServices);
    };
    hooks.started = [this](shockwave::PlaySession&, const shockwave::LevelRequest&) {
        // --camera named an entity of the first level; later levels use their own camera.
        if (m_levels && m_levels->epoch() > 1) m_toolCamera = shockwave::kNullEntity;
    };
    return hooks;
}

shockwave::SaveIdentity Player::saveIdentity() {
    if (!m_saveIdentity) {
        shockwave::SaveIdentity identity = saveIdentityFor(m_project, m_tool.userPaths ? &*m_tool.userPaths : nullptr);
        // On a save worker: the game's own function, given only the detached save. The
        // instance is looked up each time, since hot reload replaces it.
        identity.migrate = [this](int from, shockwave::SaveInfo& save, std::string& error) {
            return !m_instance || m_instance->migrateSave(from, save, error);
        };
        m_saveIdentity = std::move(identity);
    }
    return *m_saveIdentity;
}

void Player::startLevels(const std::string& level, shockwave::StartReason reason) {
    if (!m_levels) {
        loadLevelCatalog();
        m_levels = std::make_unique<shockwave::LevelHost>(
            m_world, engine().jobs(), *m_instance, &engine(), withAssetLeases(levelHooks()));
        // Level leases (docs/AssetResidency.md): off with --no-asset-residency.
        m_residency.setBudget(m_tool.assetBudget);
        m_levels->setResidency(m_tool.assetResidency ? &m_residency : nullptr);
        // Travel over two frames (level_travel staged), and where returnToMenu goes: the
        // catalog's first menu level, else its entry.
        m_levels->setStaged(!m_tool.instantTravel && m_project.levelTravel != "instant");
        std::string menu = m_catalog.entry;
        for (const shockwave::LevelEntry& l : m_catalog.levels)
            if (l.kind == shockwave::LevelKind::Menu) {
                menu = l.id;
                break;
            }
        m_levels->setMenuLevel(menu);
        m_levels->setRememberDestroyed(m_project.rememberDestroyed);
        m_levels->onLoadReport = [this](const shockwave::LoadReport& report) { recordLoad(report); };
        m_levels->onResult = [this](const shockwave::TravelResult& result) {
            if (result.status == shockwave::TravelStatus::Failed)
                std::fprintf(stderr, "Player: level \"%s\": %s\n", result.request.level.c_str(), result.error.c_str());
        };
    }
    const shockwave::LevelEntry* entry = m_catalog.find(level);
    m_levels->start({level, entry ? entry->defaultSpawn : std::string()}, reason);
    m_sceneAssets.release();   // on demand: the level's lease holds them now
}

void Player::processTravel() {
    if (!m_levels) return;
    m_levels->process();
    if (m_levels->playing() || m_levels->failure().empty()) return;
    // The shell's error screen shows it, with Retry, Main menu and Quit.
    if (m_shell && m_shell->failure()) return;
    // The level that failed to start has gone; go back to the entry, once.
    const std::string failed = m_levels->current().level;
    if (failed != m_catalog.entry) {
        std::string error;
        shockwave::World entry;
        const fs::path scene = sceneForLevel(m_catalog.entry);
        try {
            if (!scene.empty()) m_sceneAssets = sceneAssets("startup", scene);   // on demand
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!scene.empty() && error.empty() && shockwave::loadScene(entry, engine().assets(), scene.string(), error)) {
            m_world = std::move(entry);
            try {
                startLevels(m_catalog.entry, shockwave::StartReason::Fresh);
                SW_LOG(
                    shockwave::LogGame, Warning, "Back at level \"{}\" after \"{}\" failed", m_catalog.entry, failed);
                return;
            } catch (const std::exception& e) {
                std::fprintf(
                    stderr, "Player: level \"%s\" didn't start either: %s\n", m_catalog.entry.c_str(), e.what());
            }
        }
    }
    m_exitCode = 1;
    engine().quit();
}

}   // namespace player
