// Cooking (S09, docs/Cooking.md): `player --cook` works out everything a release of the
// game needs, from every level in the catalog and the files no level names, and writes it
// to Saved/Cook/<target>/Cook.swcook. `--release` holds it to the release policy;
// `--check --release` runs the same policy without writing; `--cook-info` reads a manifest
// and, with --verify, checks files against it.
#include "Player.h"

#include <Assets/AssetGraph.h>
#include <Core/Plugin.h>
#include <Multiplayer/NetworkValidation.h>
#include <Packaging/Cook.h>
#include <Packaging/Package.h>
#include <Projects/Diagnostic.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace player {

namespace {

bool readText(const fs::path& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream all;
    all << in.rdbuf();
    out = all.str();
    return true;
}

bool writeText(const fs::path& out, const std::string& text) {
    if (out == "-") {
        std::fputs(text.c_str(), stdout);
        return true;
    }
    std::ofstream file(out, std::ios::binary);
    file << text;
    file.close();
    if (!file) {
        std::fprintf(stderr, "Player: can't write %s\n", out.string().c_str());
        return false;
    }
    return true;
}

// '*' is any run of characters but '/', '**' any run at all, '?' one character.
bool includeMatch(std::string_view pattern, std::string_view text) {
    if (pattern.empty()) return text.empty();
    if (pattern.substr(0, 2) == "**") {
        for (size_t i = 0; i <= text.size(); ++i)
            if (includeMatch(pattern.substr(2), text.substr(i))) return true;
        return false;
    }
    if (pattern[0] == '*') {
        for (size_t i = 0; i <= text.size(); ++i) {
            if (includeMatch(pattern.substr(1), text.substr(i))) return true;
            if (i < text.size() && text[i] == '/') break;
        }
        return false;
    }
    if (text.empty()) return false;
    if (pattern[0] == '?' ? text[0] == '/' : pattern[0] != text[0]) return false;
    return includeMatch(pattern.substr(1), text.substr(1));
}

// Files whose text names other files: an absolute path in one is a path that only works
// on the machine it was made on.
bool textFile(std::string_view id) {
    static const char* extensions[] = {".swscene",    ".swprefab",    ".swmat",     ".rml",       ".rcss",
                                       ".swcircuit",  ".swpost",      ".swstrings", ".swinput",   ".swprefs",
                                       ".swlevels",   ".swgraphics",  ".swnetwork", ".swrelease", ".swproject",
                                       ".swlighting", ".swreflection"};
    const std::string ext = fs::path(id).extension().string();
    return std::find(std::begin(extensions), std::end(extensions), ext) != std::end(extensions);
}

// An absolute path a developer's machine has: their home folder, a drive letter, the
// project's own folder. Returns what it found, or empty.
std::string absolutePathIn(const std::string& text, const std::string& projectRoot) {
    if (!projectRoot.empty() && text.find(projectRoot) != std::string::npos) return projectRoot;
    for (const char* home : {"/home/", "/Users/", "C:\\Users\\", "C:/Users/", "/root/"}) {
        const size_t at = text.find(home);
        if (at == std::string::npos) continue;
        // Only inside a quoted value or after a space: "/home/" alone in prose isn't a path.
        const size_t end = text.find_first_of("\"\n ", at);
        return text.substr(at, std::min<size_t>(end == std::string::npos ? 40 : end - at, 60));
    }
    return {};
}

}   // namespace

// Works out the manifest. Errors stop a cook; with release, the release policy adds its own.
bool Player::cookManifest(
    bool release, shockwave::CookManifest& manifest, std::vector<std::string>& errors,
    std::vector<std::string>& warnings) {
    using shockwave::AssetGraph;
    using shockwave::AssetKind;
    AssetGraph& graph = assetGraph();
    shockwave::CookManifest m;
    m.application = m_project.name;
    m.gameModule = m_project.gameModule;
    m.target = m_tool.cookTarget.empty() ? shockwave::packagePlatform() : m_tool.cookTarget;
    m.buildConfig = shockwave::engineBuildConfig();
    m.engineVersion = shockwave::engineVersion();
    m.pluginAbi = shockwave::kPluginAbiVersion;
    m.release = release;

    const std::vector<std::string> targets = shockwave::cookTargets();
    if (std::find(targets.begin(), targets.end(), m.target) == targets.end()) {
        errors.push_back(
            "can't cook for " + m.target + ": this engine is built for " + shockwave::packagePlatform() +
            ", and a cook records the engine and the modules it ships with. Cook on a " + m.target +
            " machine, with an engine built there");
        return false;
    }
    shockwave::PackageSettings settings;
    std::string why;
    if (!shockwave::loadPackageSettings(m_project, settings, why)) errors.push_back(why);

    // ---- Every level in the catalog, not only the startup scene ----
    shockwave::LevelCatalog catalog;
    std::vector<shockwave::Diagnostic> catalogProblems;
    if (!shockwave::projectLevels(m_project, catalog, catalogProblems))
        catalog = shockwave::implicitLevelCatalog(m_project);
    for (const auto& d : catalogProblems)
        if (d.error()) errors.push_back(d.text() + " (" + d.code + ")");
    m.entryLevel = catalog.entry;
    std::map<std::string, std::set<std::string>> reachedBy;   // asset -> levels
    std::vector<shockwave::Diagnostic> found;
    auto collect = [&](const std::string& root) {
        for (auto& d : graph.diagnostics(root))
            if (std::none_of(found.begin(), found.end(), [&](const shockwave::Diagnostic& o) {
                    return o.code == d.code && o.file == d.file && o.line == d.line && o.message == d.message;
                }))
                found.push_back(std::move(d));
    };
    for (const shockwave::LevelEntry& level : catalog.levels) {
        const std::string scene = graph.idFor(m_project.root / level.scene);
        if (scene.empty()) {
            errors.push_back("level " + level.id + ": its scene " + level.scene + " isn't in the project");
            continue;
        }
        const shockwave::AssetNode& node = graph.add(scene);
        m.levels.push_back({level.id, scene, node.closureHash});
        for (const std::string& id : graph.closure(scene)) reachedBy[id].insert(level.id);
        collect(scene);
    }

    // ---- What no level names, but the game needs ----
    std::map<std::string, std::string> roots;   // id -> why
    auto root = [&](const std::string& id, const std::string& reason) {
        if (id.empty() || roots.count(id) || reachedBy.count(id)) return;
        roots[id] = reason;
        graph.add(id);
        collect(id);
    };
    root(graph.idFor(m_project.file), "the project");
    if (!m_project.gameCircuit.empty()) root(graph.idFor(m_project.gameCircuit), "the game circuit");
    std::error_code ec;
    for (const char* config :
         {"Input.swinput", "Preferences.swprefs", "Levels.swlevels", "Graphics.swgraphics", "Network.swnetwork",
          "Release.swrelease"})
        if (fs::is_regular_file(m_project.root / "Config" / config, ec))
            root("Config/" + std::string(config), "settings the game reads at start");
    if (!m_project.levelCatalog.empty() && fs::is_regular_file(m_project.levelCatalog, ec))
        root(graph.idFor(m_project.levelCatalog), "the level catalog");
    // Everything under Content the engine reads without a reference: text catalogs, fonts
    // (every document may use any), the shell's own screens, and the include patterns.
    for (auto it = fs::recursive_directory_iterator(m_project.assetFolder, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string id = graph.idFor(it->path());
        const std::string name = graph.contentName(id);
        const std::string ext = it->path().extension().string();
        if (name.rfind("Localization/", 0) == 0 && ext == ".swstrings")
            root(id, "localization catalog");
        else if (ext == ".ttf" || ext == ".otf")
            root(id, "font");
        else if (name.rfind("Shell/", 0) == 0 && ext == ".rml")
            root(id, "the shell's own screen");
        else if (name == "Shell/credits.txt")
            root(id, "the shell's credits");
        else
            for (const std::string& pattern : settings.include)
                if (includeMatch(pattern, id) || includeMatch(pattern, name)) {
                    root(id, "include \"" + pattern + "\" in Config/Package.swpackage");
                    break;
                }
    }
    for (const std::string& pattern : settings.include) {
        const bool matched = std::any_of(roots.begin(), roots.end(), [&](const auto& r) {
            return r.second == "include \"" + pattern + "\" in Config/Package.swpackage";
        });
        if (!matched) warnings.push_back("Config/Package.swpackage: include \"" + pattern + "\" matches no file");
    }
    for (const auto& [id, reason] : roots) {
        m.roots.push_back({id, reason});
        for (const std::string& reached : graph.closure(id)) reachedBy[reached];   // no level: a root's
    }

    // ---- The assets ----
    const std::string projectRoot = fs::absolute(m_project.root).lexically_normal().generic_string();
    for (const auto& [id, levels] : reachedBy) {
        const shockwave::AssetNode* node = graph.find(id);
        if (!node) continue;
        if (!node->exists && !node->builtin) continue;   // a diagnostic says so
        shockwave::CookAsset asset;
        asset.id = id;
        asset.kind = node->builtin ? "builtin" : shockwave::assetKindName(node->kind);
        asset.bytes = node->bytes;
        asset.hash = node->hash;
        asset.closure = node->closureHash;
        asset.levels.assign(levels.begin(), levels.end());
        const std::string needs = levels.empty()
                                      ? std::string("the game")
                                      : "level " + *levels.begin() + (levels.size() > 1 ? " and others" : "");
        if (!node->builtin && shockwave::packageExcludes(settings, id))
            errors.push_back(
                id + ": excluded by Config/Package.swpackage, but " + needs + " needs it; take it out of exclude");
        if (release && !node->builtin && fs::path(id).extension() == ".blend")
            errors.push_back(
                id + ": a .blend file needs Blender to load; " + needs +
                " uses it, so export it to .glb for a release (docs/Cooking.md#what-a-release-may-not-do)");
        if (release && textFile(id)) {
            std::string text;
            if (readText(graph.pathOf(id), text))
                if (const std::string path = absolutePathIn(text, projectRoot); !path.empty())
                    errors.push_back(
                        id + ": names " + path +
                        ", an absolute path that only works on this machine; name it "
                        "relative to Content");
        }
        m.assets.push_back(std::move(asset));
    }

    // ---- What the graph found ----
    for (const shockwave::Diagnostic& d : found) {
        // A name only code registers can't be followed: a release must say where it is,
        // so an include pattern covers it or it's an error.
        if (d.error() || (release && d.code == "AssetUnknownName"))
            errors.push_back(
                d.text() + " (" + d.code + ")" +
                (d.code == "AssetUnknownName" ? "; if game code makes it, fine; if it's a file, add an include pattern"
                                              : ""));
        else
            warnings.push_back(d.text() + " (" + d.code + ")");
    }

    // ---- The engine's licenses: they ship with every package ----
    const fs::path licenses = shockwave::executableDir() / "licenses";
    for (auto it = fs::directory_iterator(licenses, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) m.licenses.push_back(it->path().filename().generic_string());
    if (m.licenses.empty())
        errors.push_back("the engine's licenses aren't in " + licenses.generic_string() + ": rebuild it");

    // ---- The release policy ----
    if (release) {
        if (!shockwave::engineBuildOptimised())
            errors.push_back(
                std::string("this engine is ") +
                (m.buildConfig.empty() ? "built without a CMake configuration, so without optimisation"
                                       : "a " + m.buildConfig + " build") +
                ": a release runs many times slower than it should. Build it with "
                "cmake -B build -DCMAKE_BUILD_TYPE=Release");
        const shockwave::net::ProjectSteamUse steam = shockwave::net::projectSteamUse(m_project);
        if (steam.uses && steam.appId == 480)
            errors.push_back("Config/Network.swnetwork: steam_app_id 480 is Valve's shared test app; a release needs "
                             "the game's own");
        // The game module is loaded by now (the player won't start without it), so its
        // plugin ABI matches this engine's: the manifest records that ABI.
    }
    m.computeIds();
    manifest = std::move(m);
    return errors.empty();
}

int Player::cook() {
    shockwave::CookManifest manifest;
    std::vector<std::string> errors, warnings;
    const bool ok = cookManifest(m_tool.release, manifest, errors, warnings);
    for (const std::string& w : warnings) std::printf("warning: %s\n", w.c_str());
    for (const std::string& e : errors) std::printf("error: %s\n", e.c_str());
    const fs::path file = m_tool.output.empty() ? shockwave::cookManifestPath(m_project.root, manifest.target)
                                                : fs::absolute(m_tool.output);
    if (!m_tool.benchmarkJson.empty()) {
        // The report: the manifest's JSON with what the cook found, even when it failed.
        std::string json = shockwave::cookManifestJson(manifest);
        json.erase(json.rfind('}'));
        json += ",  \"ok\": " + std::string(ok ? "true" : "false") + ",\n  \"errors\": [";
        for (size_t i = 0; i < errors.size(); ++i) json += (i ? ", " : "") + shockwave::diagnosticJsonString(errors[i]);
        json += "],\n  \"warnings\": [";
        for (size_t i = 0; i < warnings.size(); ++i)
            json += (i ? ", " : "") + shockwave::diagnosticJsonString(warnings[i]);
        json += "]\n}\n";
        if (!writeText(m_tool.benchmarkJson, json)) return 1;
    }
    if (!ok) {
        // A failed cook leaves the last good manifest where it was.
        std::printf("Cook failed: %zu problem%s\n", errors.size(), errors.size() == 1 ? "" : "s");
        return 1;
    }
    std::string why;
    if (!shockwave::saveCookManifest(file, manifest, why)) {
        std::fprintf(stderr, "Player: %s\n", why.c_str());
        return 1;
    }
    uint64_t bytes = 0;
    for (const auto& a : manifest.assets) bytes += a.bytes;
    std::printf(
        "Cooked %s for %s%s: %zu levels, %zu assets (%.1f MB), %zu roots\n", manifest.application.c_str(),
        manifest.target.c_str(), manifest.release ? " (release)" : "", manifest.levels.size(), manifest.assets.size(),
        double(bytes) / (1024.0 * 1024.0), manifest.roots.size());
    std::printf(
        "content %s build %s\n", shockwave::hex64(manifest.contentId).c_str(),
        shockwave::hex64(manifest.buildId).c_str());
    std::printf("manifest %s\n", file.generic_string().c_str());
    return 0;
}

void Player::checkCookRelease(std::vector<std::string>& problems) {
    shockwave::CookManifest manifest;
    std::vector<std::string> errors, warnings;
    cookManifest(true, manifest, errors, warnings);
    for (const std::string& e : errors) problems.push_back("release: " + e);
    if (errors.empty())
        std::printf(
            "ok release policy (%zu levels, %zu assets, content %s)\n", manifest.levels.size(), manifest.assets.size(),
            shockwave::hex64(manifest.contentId).c_str());
}

int cookInfo(const fs::path& file, const fs::path& verifyRoot, const fs::path& json) {
    shockwave::CookManifest m;
    std::string error;
    if (!shockwave::loadCookManifest(file, m, error)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    if (!json.empty()) return writeText(json, shockwave::cookManifestJson(m)) ? 0 : 1;
    uint64_t bytes = 0;
    std::map<std::string, std::pair<size_t, uint64_t>> kinds;
    for (const auto& a : m.assets) {
        bytes += a.bytes;
        kinds[a.kind].first++;
        kinds[a.kind].second += a.bytes;
    }
    std::printf(
        "%s (%s), %s %s, engine %s, plugin ABI %u%s\n", m.application.c_str(),
        m.gameModule.empty() ? "no game module" : m.gameModule.c_str(), m.target.c_str(),
        m.buildConfig.empty() ? "(no configuration)" : m.buildConfig.c_str(), m.engineVersion.c_str(), m.pluginAbi,
        m.release ? ", release" : "");
    std::printf("content %s build %s\n", shockwave::hex64(m.contentId).c_str(), shockwave::hex64(m.buildId).c_str());
    std::printf("entry level %s\n", m.entryLevel.c_str());
    for (const auto& l : m.levels) {
        size_t count = 0;
        for (const auto& a : m.assets) count += std::binary_search(a.levels.begin(), a.levels.end(), l.id);
        std::printf(
            "level %s: %s, %zu assets, closure %s\n", l.id.c_str(), l.scene.c_str(), count,
            shockwave::hex64(l.closure).c_str());
    }
    for (const auto& r : m.roots) std::printf("root %s (%s)\n", r.id.c_str(), r.why.c_str());
    for (const auto& [kind, count] : kinds)
        std::printf("%zu %s, %.2f MB\n", count.first, kind.c_str(), double(count.second) / (1024.0 * 1024.0));
    std::printf(
        "%zu assets, %.2f MB; %zu licenses\n", m.assets.size(), double(bytes) / (1024.0 * 1024.0), m.licenses.size());
    if (verifyRoot.empty()) return 0;
    const auto problems = shockwave::verifyCookedFiles(m, verifyRoot);
    for (const std::string& p : problems) std::printf("error: %s\n", p.c_str());
    if (!problems.empty()) {
        std::printf("%zu file%s don't match the cook\n", problems.size(), problems.size() == 1 ? "" : "s");
        return 1;
    }
    std::printf("ok every file matches the cook\n");
    return 0;
}

}   // namespace player
