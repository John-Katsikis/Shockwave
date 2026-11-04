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
