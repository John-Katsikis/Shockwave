// The level catalog: see LevelCatalog.h and docs/Levels.md.
#include <Projects/LevelCatalog.h>

#include <Projects/Project.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>

namespace shockwave {

namespace fs = std::filesystem;

namespace {

constexpr const char* kKinds[] = {"menu", "gameplay", "ending"};

std::string quoted(const std::string& text) {
    std::ostringstream out;
    out << std::quoted(text);
    return out.str();
}

std::string lower(std::string text) {
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

Diagnostic make(
    const char* code, DiagnosticSeverity severity, const std::string& file, int line, std::string object,
    std::string message) {
    return Diagnostic{code, severity, file, line, std::move(object), std::move(message)};
}

// A project-relative path as the catalog stores it: normal, forward slashes, inside the
// project. Empty `why` when it's fine.
std::string normalPath(const std::string& text, std::string& why) {
    why.clear();
    if (text.empty()) return why = "is empty", std::string();
    const fs::path path(text);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return why = "must be relative to the project, not absolute", std::string();
    const fs::path normal = path.lexically_normal();
    if (normal.empty() || normal == "." || *normal.begin() == "..")
        return why = "leaves the project folder", std::string();
    return normal.generic_string();
}

// The levels reachable along `next` and enabled exits.
std::set<std::string> reachable(const LevelCatalog& catalog, const std::map<std::string, LevelSceneFacts>* facts) {
    std::set<std::string> seen;
    std::vector<std::string> todo;
    if (catalog.find(catalog.entry)) todo.push_back(catalog.entry);
    while (!todo.empty()) {
        const std::string id = todo.back();
        todo.pop_back();
        if (!seen.insert(id).second) continue;
        const LevelEntry* level = catalog.find(id);
        if (!level) continue;
        if (!level->next.empty() && catalog.find(level->next)) todo.push_back(level->next);
        if (!facts) continue;
        const auto it = facts->find(id);
        if (it == facts->end()) continue;
        for (const auto& exit : it->second.exits)
            if (exit.enabled && catalog.find(exit.destination)) todo.push_back(exit.destination);
    }
    return seen;
}

}   // namespace

const char* levelKindName(LevelKind kind) { return kKinds[std::clamp(int(kind), 0, 2)]; }

const LevelEntry* LevelCatalog::find(std::string_view id) const {
    for (const LevelEntry& level : levels)
        if (level.id == id) return &level;
    return nullptr;
}

LevelEntry* LevelCatalog::find(std::string_view id) {
    for (LevelEntry& level : levels)
        if (level.id == id) return &level;
    return nullptr;
}

bool validLevelId(std::string_view id) {
    if (id.empty() || id.size() > 64 || id[0] < 'a' || id[0] > 'z') return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

bool validPersistentId(std::string_view id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

// ---- Parsing -------------------------------------------------------------------------

bool parseLevelCatalog(
    std::string_view text, const std::string& shown, LevelCatalog& out, std::vector<Diagnostic>& diagnostics) {
    const size_t firstDiagnostic = diagnostics.size();
    auto error = [&](const char* code, int line, std::string object, std::string message) {
        diagnostics.push_back(
            make(code, DiagnosticSeverity::Error, shown, line, std::move(object), std::move(message)));
    };
    LevelCatalog result;
    std::istringstream in{std::string(text)};
    std::string line;
    int number = 0;
    bool header = false;
    LevelEntry* open = nullptr;   // the level block being read
    std::set<std::string> topKeys, blockKeys;
    auto finishBlock = [&](int endLine) {
        if (!open) return;
        if (open->scene.empty() && !open->lines.count("scene"))
            error(
                "LevelMissingKey", endLine ? endLine : open->line, open->id, "level \"" + open->id + "\" has no scene");
        if (!open->lines.count("kind"))
            error(
                "LevelMissingKey", endLine ? endLine : open->line, open->id, "level \"" + open->id + "\" has no kind");
        open = nullptr;
    };
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream row(line);
        std::string key;
        if (!(row >> key) || key[0] == '#') continue;
        if (!header) {
            int version = 0;
            if (key != "shockwave_levels" || !(row >> version)) {
                error("LevelHeader", number, {}, "the first line must be `shockwave_levels 1`");
                return false;
            }
            if (version != 1) {
                error(
                    "LevelUnsupportedVersion", number, {},
                    "catalog version " + std::to_string(version) + " is newer than this build reads (1)");
                return false;
            }
            header = true;
            continue;
        }
        // A quoted value, or a bare word for kind.
        auto quotedValue = [&](std::string& value) -> bool {
            row >> std::ws;
            if (row.peek() != '"' || !(row >> std::quoted(value))) {
                error("LevelInvalidValue", number, key, key + " takes a quoted value");
                return false;
            }
            return true;
        };
        auto trailing = [&]() {
            row >> std::ws;
            if (row.peek() != EOF && row.peek() != '#')
                error("LevelInvalidValue", number, key, "unexpected text after " + key);
        };
        if (key == "level") {
            if (open) {
                error("LevelStructure", number, open->id, "level \"" + open->id + "\" isn't closed with `end`");
                finishBlock(0);
            }
            std::string id;
            if (!quotedValue(id)) continue;
            trailing();
            if (!validLevelId(id))
                error(
                    "LevelInvalidValue", number, id,
                    "level id \"" + id +
                        "\" must be 1 to 64 lowercase letters, digits, '-' and '_', starting with a letter");
            if (result.find(id)) error("LevelDuplicateId", number, id, "level \"" + id + "\" is listed twice");
            result.levels.push_back({});
            open = &result.levels.back();
            open->id = id;
            open->line = number;
            blockKeys.clear();
            continue;
        }
        if (key == "end") {
            trailing();
            if (!open) {
                error("LevelStructure", number, {}, "`end` without a `level`");
                continue;
            }
            finishBlock(number);
            continue;
        }
        if (!open) {
            if (key != "entry") {
                const bool levelKey =
                    key == "scene" || key == "kind" || key == "default_spawn" || key == "next" || key == "preload";
                error(
                    levelKey ? "LevelStructure" : "LevelUnknownKey", number, key,
                    levelKey ? key + " belongs inside a `level` block" : "unknown key '" + key + "'");
                continue;
            }
            if (!topKeys.insert(key).second) {
                error("LevelDuplicateKey", number, key, "entry is given twice");
                continue;
            }
            if (!quotedValue(result.entry)) continue;
            trailing();
            result.entryLine = number;
            continue;
        }
        const bool repeatable = key == "preload";
        if (key != "scene" && key != "kind" && key != "default_spawn" && key != "next" && !repeatable) {
            error("LevelUnknownKey", number, key, "unknown key '" + key + "' in level \"" + open->id + "\"");
            continue;
        }
        if (!repeatable && !blockKeys.insert(key).second) {
            error("LevelDuplicateKey", number, key, key + " is given twice in level \"" + open->id + "\"");
            continue;
        }
        open->lines.emplace(key, number);
        if (key == "kind") {
            std::string kind;
            row >> kind;
            trailing();
            const auto* found = std::find(std::begin(kKinds), std::end(kKinds), kind);
            if (found == std::end(kKinds)) {
                error("LevelInvalidValue", number, key, "kind \"" + kind + "\" isn't one of menu, gameplay, ending");
                continue;
            }
            open->kind = LevelKind(found - std::begin(kKinds));
            continue;
        }
        std::string value;
        if (!quotedValue(value)) continue;
        trailing();
        if (key == "scene" || key == "preload") {
            std::string why;
            const std::string path = normalPath(value, why);
            if (!why.empty()) {
                error("LevelPathOutside", number, key, key + " \"" + value + "\" " + why);
                continue;
            }
            if (key == "scene") {
                if (fs::path(path).extension() != ".swscene")
                    error("LevelInvalidValue", number, key, "scene \"" + value + "\" isn't a .swscene file");
                open->scene = path;
            } else {
                if (std::find(open->preload.begin(), open->preload.end(), path) != open->preload.end())
                    error("LevelInvalidValue", number, key, "preload \"" + value + "\" is listed twice");
                open->preload.push_back(path);
            }
        } else if (key == "default_spawn") {
            if (!validLevelId(value))
                error("LevelInvalidValue", number, key, "default_spawn \"" + value + "\" isn't a valid spawn id");
            open->defaultSpawn = value;
        } else {   // next
            if (!validLevelId(value))
                error("LevelInvalidValue", number, key, "next \"" + value + "\" isn't a valid level id");
            open->next = value;
        }
    }
    if (in.bad()) {
        error("LevelUnreadable", number, {}, "error reading the catalog");
        return false;
    }
    if (!header) {
        error("LevelHeader", 1, {}, "the first line must be `shockwave_levels 1`");
        return false;
    }
    if (open) {
        error("LevelStructure", open->line, open->id, "level \"" + open->id + "\" isn't closed with `end`");
        finishBlock(0);
    }
    if (result.levels.empty()) error("LevelMissingKey", number, {}, "the catalog lists no levels");
    if (result.entry.empty())
        error("LevelNoEntry", 0, "entry", "no entry level: add `entry \"<level id>\"`");
    else if (!result.find(result.entry))
        error("LevelUnknownEntry", result.entryLine, "entry", "entry \"" + result.entry + "\" isn't a level here");
    for (const LevelEntry& level : result.levels)
        if (!level.next.empty() && !result.find(level.next))
            error(
                "LevelUnknownNext", level.lines.count("next") ? level.lines.at("next") : level.line, level.id,
                "level \"" + level.id + "\": next \"" + level.next + "\" isn't a level here");
    if (hasErrors(std::span<const Diagnostic>(diagnostics).subspan(firstDiagnostic))) return false;
    out = std::move(result);
    return true;
}

bool readLevelCatalog(
    const fs::path& file, const std::string& shown, LevelCatalog& out, std::vector<Diagnostic>& diagnostics) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        diagnostics.push_back(
            make("LevelUnreadable", DiagnosticSeverity::Error, shown, 0, {}, "can't open the level catalog"));
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) {
        diagnostics.push_back(make("LevelUnreadable", DiagnosticSeverity::Error, shown, 0, {}, "can't read it"));
        return false;
    }
    return parseLevelCatalog(text.str(), shown, out, diagnostics);
}

std::string levelCatalogText(const LevelCatalog& catalog) {
    std::string out = "shockwave_levels 1\n";
    out += "entry " + quoted(catalog.entry) + "\n";
    for (const LevelEntry& level : catalog.levels) {
        out += "level " + quoted(level.id) + "\n";
        out += "  scene " + quoted(level.scene) + "\n";
        out += std::string("  kind ") + levelKindName(level.kind) + "\n";
        if (!level.defaultSpawn.empty()) out += "  default_spawn " + quoted(level.defaultSpawn) + "\n";
        if (!level.next.empty()) out += "  next " + quoted(level.next) + "\n";
        for (const std::string& file : level.preload) out += "  preload " + quoted(file) + "\n";
        out += "end\n";
    }
    return out;
}

bool writeLevelCatalog(const fs::path& file, const LevelCatalog& catalog, std::string& error) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    const fs::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!(out << levelCatalogText(catalog)) || !out.flush()) {
            error = "can't write " + temp.string();
            out.close();
            fs::remove(temp, ec);
            return false;
        }
    }
    fs::rename(temp, file, ec);
    if (ec) {
        error = "can't replace " + file.string() + ": " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    error.clear();
    return true;
}

// ---- The project's levels -------------------------------------------------------------

fs::path levelCatalogPath(const Project& project) { return project.levelCatalog; }

LevelCatalog implicitLevelCatalog(const Project& project) {
    LevelCatalog catalog;
    catalog.implicit = true;
    catalog.entry = kImplicitLevelId;
    LevelEntry level;
    level.id = kImplicitLevelId;
    level.scene = project.relative(project.startupScene);
    level.kind = LevelKind::Gameplay;
    catalog.levels.push_back(std::move(level));
    return catalog;
}

bool projectLevels(const Project& project, LevelCatalog& out, std::vector<Diagnostic>& diagnostics) {
    if (project.levelCatalog.empty()) {
        out = implicitLevelCatalog(project);
        return true;
    }
    return readLevelCatalog(project.levelCatalog, project.relative(project.levelCatalog), out, diagnostics);
}

void validateLevelCatalog(
    const Project& project, const LevelCatalog& catalog, const std::string& shown,
    std::vector<Diagnostic>& diagnostics) {
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(project.root, ec);
    auto lineOf = [](const LevelEntry& level, const char* key) {
        const auto it = level.lines.find(key);
        return it == level.lines.end() ? level.line : it->second;
    };
    // A file the catalog names: inside the project once links are followed, there, and
    // spelt the way the disk spells it (a case-insensitive disk would hide the mistake
    // until the game runs somewhere that isn't).
    auto checkFile = [&](const LevelEntry& level, const char* key, const std::string& file, const char* missingCode) {
        const fs::path path = project.root / file;
        const int line = lineOf(level, key);
        const fs::path real = fs::weakly_canonical(path, ec);
        const fs::path rel = real.lexically_relative(root);
        if (!ec && (rel.empty() || *rel.begin() == ".."))
            return diagnostics.push_back(make(
                "LevelPathOutside", DiagnosticSeverity::Error, shown, line, level.id,
                std::string(key) + " \"" + file + "\" leads outside the project (through a link)"));
        fs::path here = project.root;
        for (const fs::path& part : fs::path(file)) {
            const fs::path next = here / part;
            // Compare against the directory's own spelling: on a case-insensitive disk
            // exists() would accept any case, and the mistake would only show elsewhere.
            if (part != "." && part != "..") {
                bool exact = false;
                std::string other;
                for (const auto& entry : fs::directory_iterator(here, ec)) {
                    const std::string name = entry.path().filename().string();
                    if (name == part.string())
                        exact = true;
                    else if (lower(name) == lower(part.string()))
                        other = name;
                }
                if (!exact && !other.empty())
                    return diagnostics.push_back(make(
                        "LevelPathCase", DiagnosticSeverity::Error, shown, line, level.id,
                        std::string(key) + " \"" + file + "\": the disk spells \"" + part.string() + "\" as \"" +
                            other + "\""));
            }
            if (!fs::exists(next, ec))
                return diagnostics.push_back(make(
                    missingCode, DiagnosticSeverity::Error, shown, line, level.id,
                    std::string(key) + " \"" + file + "\" doesn't exist"));
            here = next;
        }
        if (std::string(key) == "scene" && !fs::is_regular_file(path, ec))
            diagnostics.push_back(make(
                missingCode, DiagnosticSeverity::Error, shown, line, level.id, "scene \"" + file + "\" isn't a file"));
    };
    std::map<std::string, const LevelEntry*> byLowerScene;
    for (const LevelEntry& level : catalog.levels) {
        if (!level.scene.empty()) {
            checkFile(level, "scene", level.scene, "LevelSceneMissing");
            const auto [it, added] = byLowerScene.emplace(lower(level.scene), &level);
            if (!added && it->second->scene != level.scene)
                diagnostics.push_back(make(
                    "LevelPathCase", DiagnosticSeverity::Error, shown, lineOf(level, "scene"), level.id,
                    "scene \"" + level.scene + "\" differs only by case from level \"" + it->second->id + "\"'s \"" +
                        it->second->scene + "\""));
        }
        for (const std::string& file : level.preload) checkFile(level, "preload", file, "LevelPreloadMissing");
    }
}

// ---- Links between levels -------------------------------------------------------------

std::vector<std::string>
reachableLevels(const LevelCatalog& catalog, const std::map<std::string, LevelSceneFacts>* facts) {
    const std::set<std::string> seen = reachable(catalog, facts);
    std::vector<std::string> out;
    for (const LevelEntry& level : catalog.levels)
        if (seen.count(level.id)) out.push_back(level.id);
    return out;
}

void validateLevelLinks(
    const LevelCatalog& catalog, const std::map<std::string, LevelSceneFacts>& facts, const std::string& shown,
    std::vector<Diagnostic>& diagnostics) {
    for (const LevelEntry& level : catalog.levels) {
        const auto it = facts.find(level.id);
        if (it == facts.end()) continue;
        const LevelSceneFacts& f = it->second;
        const std::string& scene = level.scene;
        auto sceneError = [&](const char* code, const std::string& object, std::string message) {
            diagnostics.push_back(make(code, DiagnosticSeverity::Error, scene, 0, object, std::move(message)));
        };
        if (!f.loaded) {
            diagnostics.push_back(make(
                "LevelSceneBroken", DiagnosticSeverity::Error, shown, level.line, level.id,
                "level \"" + level.id + "\": " + scene + " doesn't load: " + f.loadError));
            continue;
        }
        std::set<std::string> spawns;
        for (const auto& spawn : f.spawns) {
            if (!validLevelId(spawn.id))
                sceneError(
                    "LevelInvalidValue", spawn.entity,
                    "entity '" + spawn.entity + "': LevelSpawn id \"" + spawn.id + "\" isn't a valid spawn id");
            else if (!spawns.insert(spawn.id).second)
                sceneError(
                    "LevelDuplicateSpawn", spawn.entity,
                    "entity '" + spawn.entity + "': another LevelSpawn already has id \"" + spawn.id + "\"");
        }
        if (!level.defaultSpawn.empty() && !spawns.count(level.defaultSpawn))
            diagnostics.push_back(make(
                "LevelSpawnMissing", DiagnosticSeverity::Error, shown,
                level.lines.count("default_spawn") ? level.lines.at("default_spawn") : level.line, level.id,
                "level \"" + level.id + "\": default_spawn \"" + level.defaultSpawn + "\" isn't a LevelSpawn in " +
                    scene));
        for (const auto& exit : f.exits) {
            const LevelEntry* destination = catalog.find(exit.destination);
            if (exit.destination.empty() || !destination) {
                sceneError(
                    "LevelExitDestination", exit.entity,
                    "entity '" + exit.entity + "': LevelExit destination \"" + exit.destination +
                        "\" isn't a level in " + shown);
                continue;
            }
            if (exit.spawn.empty()) continue;   // the destination's default spawn
            const auto there = facts.find(destination->id);
            if (there == facts.end() || !there->second.loaded) continue;
            const auto& theirs = there->second.spawns;
            if (std::none_of(theirs.begin(), theirs.end(), [&](const auto& s) { return s.id == exit.spawn; }))
                sceneError(
                    "LevelExitSpawn", exit.entity,
                    "entity '" + exit.entity + "': LevelExit spawn \"" + exit.spawn + "\" isn't a LevelSpawn in " +
                        destination->scene + " (level \"" + destination->id + "\")");
        }
        std::set<std::string> checkpoints;
        for (const auto& checkpoint : f.checkpoints) {
            if (!validLevelId(checkpoint.id))
                sceneError(
                    "LevelCheckpoint", checkpoint.entity,
                    "entity '" + checkpoint.entity + "': Checkpoint id \"" + checkpoint.id + "\" isn't a valid id");
            else if (!checkpoints.insert(checkpoint.id).second)
                sceneError(
                    "LevelCheckpoint", checkpoint.entity,
                    "entity '" + checkpoint.entity + "': another Checkpoint already has id \"" + checkpoint.id + "\"");
            if (checkpoint.spawn.empty() || !spawns.count(checkpoint.spawn))
                sceneError(
                    "LevelCheckpointSpawn", checkpoint.entity,
                    "entity '" + checkpoint.entity + "': Checkpoint spawn \"" + checkpoint.spawn +
                        "\" isn't a LevelSpawn in this scene");
        }
        std::map<std::string, std::string> identities;
        for (const auto& identity : f.identities) {
            if (!identity.problem.empty()) {
                sceneError(
                    "LevelPersistentId", identity.entity, "entity '" + identity.entity + "': " + identity.problem);
                continue;
            }
            const auto [found, added] = identities.emplace(identity.id, identity.entity);
            if (!added)
                sceneError(
                    "LevelDuplicatePersistentId", identity.entity,
                    "entity '" + identity.entity + "': PersistentIdentity \"" + identity.id + "\" is also on '" +
                        found->second + "'");
        }
        // A gameplay level needs a view; a menu or ending can be drawn by its UI alone.
        if (f.primaryCameras == 0 && (level.kind == LevelKind::Gameplay || !f.hasUserInterface))
            diagnostics.push_back(make(
                "LevelNoCamera", DiagnosticSeverity::Error, scene, 0, level.id,
                "level \"" + level.id + "\" (" + levelKindName(level.kind) + ") has no primary Camera" +
                    (level.kind == LevelKind::Gameplay ? std::string() : std::string(" and no UIDocument"))));
    }
    const std::set<std::string> seen = reachable(catalog, &facts);
    for (const LevelEntry& level : catalog.levels)
        if (!seen.count(level.id))
            diagnostics.push_back(make(
                "LevelUnreachable", DiagnosticSeverity::Warning, shown, level.line, level.id,
                "level \"" + level.id + "\" can't be reached from entry \"" + catalog.entry +
                    "\" by next or any exit"));
}

// ---- Schema ---------------------------------------------------------------------------

std::string levelSchema() {
    return "# Level catalog reference\n\n"
           "<!-- Generated by `player --level-schema docs/LevelSchema.md`; don't edit by hand. -->\n\n"
           "The level catalog lists a game's levels. The project's `levels` key names it, by convention\n"
           "`Config/Levels.swlevels`. See docs/Levels.md for how it's used. A project without one has a\n"
           "single implicit level, `startup`: its startup scene.\n\n"
           "## Format\n\n"
           "The first line is `shockwave_levels 1`. `#` starts a comment. Values are quoted, except `kind`.\n\n"
           "| Key | Where | Values | Meaning |\n| --- | --- | --- | --- |\n"
           "| `entry` | top, once | level id | the level the game starts in |\n"
           "| `level` | top, repeats | level id | starts a level block, closed by `end` |\n"
           "| `scene` | level, required | project-relative `.swscene` | the level's scene |\n"
           "| `kind` | level, required | `menu`, `gameplay`, `ending` | what the level is for |\n"
           "| `default_spawn` | level, optional | spawn id | the LevelSpawn players arrive at |\n"
           "| `next` | level, optional | level id | the default destination; never automatic |\n"
           "| `preload` | level, repeats | project-relative file | loaded before the level starts |\n\n"
           "Level and spawn ids are 1 to 64 lowercase letters, digits, `-` and `_`, starting with a letter.\n"
           "Paths stay inside the project. Cycles through `next` are allowed (replays, rotations).\n\n"
           "## Components\n\n"
           "| Component | Fields | Rules |\n| --- | --- | --- |\n"
           "| `LevelSpawn` | `id` (\"start\"), `team` (-1: any) | id unique in the scene |\n"
           "| `LevelExit` | `destination`, `spawn`, `enabled` (true), `one_shot` (true) | destination is a "
           "catalog level; spawn, if given, is a LevelSpawn there |\n"
           "| `Checkpoint` | `id`, `spawn`, `enabled` (true) | id unique in the scene; spawn is a LevelSpawn in "
           "it |\n"
           "| `PersistentIdentity` | `id` | lowercase UUID, unique in the level; inside a prefab copy it "
           "combines with the copy's own |\n\n"
           "## Diagnostic codes\n\n"
           "Codes are stable; tools can match them. Messages may change.\n\n"
           "| Code | Severity | Meaning |\n| --- | --- | --- |\n"
           "| `LevelHeader` | error | the first line isn't `shockwave_levels 1` |\n"
           "| `LevelUnsupportedVersion` | error | a catalog version this build can't read |\n"
           "| `LevelUnreadable` | error | the file can't be opened or read |\n"
           "| `LevelUnknownKey` | error | a key this version doesn't have |\n"
           "| `LevelDuplicateKey` | error | a key given twice (other than `preload`) |\n"
           "| `LevelStructure` | error | a level key outside a block, an `end` without a `level`, or an unclosed "
           "block |\n"
           "| `LevelMissingKey` | error | a level without `scene` or `kind`, or a catalog without levels |\n"
           "| `LevelInvalidValue` | error | a malformed id, kind, value or repeated preload |\n"
           "| `LevelDuplicateId` | error | two levels with one id |\n"
           "| `LevelNoEntry` | error | no `entry` |\n"
           "| `LevelUnknownEntry` | error | `entry` names no level |\n"
           "| `LevelUnknownNext` | error | `next` names no level |\n"
           "| `LevelPathOutside` | error | a path that's absolute or leaves the project, by `..` or a link |\n"
           "| `LevelPathCase` | error | a path spelt differently from the disk, or two scenes differing only by "
           "case |\n"
           "| `LevelSceneMissing` | error | a scene that doesn't exist |\n"
           "| `LevelPreloadMissing` | error | a preload that doesn't exist |\n"
           "| `LevelSceneBroken` | error | a level's scene, or a prefab in it, doesn't load |\n"
           "| `LevelSpawnMissing` | error | `default_spawn` isn't a LevelSpawn in the scene |\n"
           "| `LevelDuplicateSpawn` | error | two LevelSpawns with one id in a scene |\n"
           "| `LevelExitDestination` | error | a LevelExit whose destination isn't a level |\n"
           "| `LevelExitSpawn` | error | a LevelExit spawn that isn't a LevelSpawn in the destination |\n"
           "| `LevelCheckpoint` | error | a Checkpoint id that's malformed or used twice |\n"
           "| `LevelCheckpointSpawn` | error | a Checkpoint whose spawn isn't a LevelSpawn in its scene |\n"
           "| `LevelPersistentId` | error | a PersistentIdentity that isn't a valid UUID, or a prefab copy "
           "without one |\n"
           "| `LevelDuplicatePersistentId` | error | two entities with one persistent identity in a level |\n"
           "| `LevelNoCamera` | error | a gameplay level with no primary Camera, or a menu/ending with neither "
           "a Camera nor a UIDocument |\n"
           "| `LevelUnreachable` | warning | nothing leads to the level from the entry |\n"
           "| `LevelOptionConflict` | error | `--level` combined with `--scene`, `--load` or an online option |\n";
}

}   // namespace shockwave
