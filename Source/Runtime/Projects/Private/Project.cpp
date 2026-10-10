#include <Projects/Project.h>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>

namespace shockwave {
bool loadProject(const std::filesystem::path& path, Project& project, std::string& error) {
    error.clear();
    std::ifstream in(path);
    if (!in) { error = "can't open project " + path.string(); return false; }
    Project result;
    std::error_code ec;
    result.file = std::filesystem::absolute(path, ec).lexically_normal();
    if (ec) { error = "can't resolve project path: " + ec.message(); return false; }
    result.root = result.file.parent_path();
    std::set<std::string> seen;
    bool versionSeen = false;
    std::string line;
    int number = 0;
    auto fail = [&](const std::string& message) {
        error = path.string() + ":" + std::to_string(number) + ": " + message;
        return false;
    };
    while (std::getline(in, line)) {
        ++number;
        std::istringstream row(line);
        std::string key;
        if (!(row >> key) || key[0] == '#') continue;
        if (!seen.insert(key).second) return fail("duplicate key '" + key + "'");
        if (key == "shockwave_project") {
            int version = 0;
            if (!(row >> version) || version != 1) return fail("unsupported project version");
            versionSeen = true;
        } else {
            if (!versionSeen) return fail("expected shockwave_project 1 first");
            if (key == "name" || key == "game_module") {
                auto& value = key == "name" ? result.name : result.gameModule;
                if (!(row >> std::quoted(value)) || value.empty()) return fail("empty or invalid " + key);
            } else if (key == "startup_scene" || key == "assets" || key == "scenes" || key == "theme") {
                std::string value;
                if (!(row >> std::quoted(value)) || value.empty()) return fail("empty or invalid " + key);
                auto& target = key == "startup_scene" ? result.startupScene : key == "assets" ? result.assetFolder
                             : key == "scenes" ? result.sceneFolder : result.theme;
                target = (result.root / value).lexically_normal();
            } else if (key == "tick_rate") {
                if (!(row >> result.tickRate) || !std::isfinite(result.tickRate) || result.tickRate < 1 || result.tickRate > 1000)
                    return fail("tick_rate must be between 1 and 1000");
            } else if (key == "window_size") {
                if (!(row >> result.width >> result.height) || result.width < 320 || result.height < 240 || result.width > 16384 || result.height > 16384)
                    return fail("invalid window_size (minimum 320 x 240)");
            } else if (key == "vsync") {
                std::string value;
                row >> value;
                if (value != "true" && value != "false") return fail("vsync must be true or false");
                result.vsync = value == "true";
            } else return fail("unknown key '" + key + "'");
        }
        row >> std::ws;
        if (row.peek() != EOF && row.peek() != '#') return fail("unexpected trailing value");
    }
    if (in.bad()) return fail("error reading project");
    if (!versionSeen || result.name.empty() || result.gameModule.empty() || result.startupScene.empty())
        return fail("project requires version, name, game_module and startup_scene");
    if (result.assetFolder.empty()) result.assetFolder = result.root / "assets";
    if (result.sceneFolder.empty()) result.sceneFolder = result.root / "scenes";
    if (result.theme.empty()) result.theme = result.root / "themes/default.swtheme";
    if (!std::filesystem::is_regular_file(result.startupScene, ec)) return fail("startup scene does not exist: " + result.startupScene.string());
    project = std::move(result);
    return true;
}

std::string Project::relative(const std::filesystem::path& path) const {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec).lexically_normal();
    const auto rel = absolute.lexically_relative(root);
    if (ec || rel.empty() || *rel.begin() == "..") return absolute.generic_string();
    return rel.generic_string();
}

bool saveProject(const Project& project, std::string& error) {
    namespace fs = std::filesystem;
    auto quotedPath = [&](const fs::path& p) { std::ostringstream q; q << std::quoted(project.relative(p)); return q.str(); };
    std::ostringstream out;
    out << "# Paths are relative to this file. Both editor and player load this manifest.\n"
        << "shockwave_project 1\n"
        << "name " << std::quoted(project.name) << "\n"
        << "game_module " << std::quoted(project.gameModule) << "\n"
        << "startup_scene " << quotedPath(project.startupScene) << "\n"
        << "assets " << quotedPath(project.assetFolder) << "\n";
    if (!project.sceneFolder.empty() && project.sceneFolder != project.root / "scenes")
        out << "scenes " << quotedPath(project.sceneFolder) << "\n";
    if (!project.theme.empty()) out << "theme " << quotedPath(project.theme) << "\n";
    out << "tick_rate " << project.tickRate << "\n"
        << "window_size " << project.width << " " << project.height << "\n"
        << "vsync " << (project.vsync ? "true" : "false") << "\n";
    const fs::path temp = project.file.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!(file << out.str()) || !file.flush()) {
            error = "can't write " + temp.string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, project.file, ec);
    if (ec) {
        error = "can't replace " + project.file.string() + ": " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

std::optional<std::filesystem::path> findProjectFile(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto inFolder = [&](const fs::path& folder) -> std::optional<fs::path> {
        if (fs::is_regular_file(folder / "shockwave.swproject", ec)) return folder / "shockwave.swproject";
        std::optional<fs::path> found;
        for (const auto& entry : fs::directory_iterator(folder, ec)) {
            if (entry.path().extension() == ".swproject" && entry.is_regular_file(ec) && (!found || entry.path() < *found))
                found = entry.path();
        }
        return found;
    };
    fs::path at = fs::absolute(path, ec).lexically_normal();
    if (ec) return std::nullopt;
    if (at.extension() == ".swproject") {
        if (fs::is_regular_file(at, ec)) return at;
        return std::nullopt;
    }
    if (!fs::is_directory(at, ec)) at = at.parent_path();
    for (int depth = 0; depth < 32 && !at.empty(); ++depth) {
        if (auto found = inFolder(at)) return found;
        if (at == at.parent_path()) break;
        at = at.parent_path();
    }
    return std::nullopt;
}

bool createProject(const std::filesystem::path& folder, const std::string& name, const std::string& gameModule,
                   Project& project, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (name.empty()) { error = "the project needs a name"; return false; }
    const fs::path root = fs::absolute(folder, ec).lexically_normal();
    if (ec) { error = "can't resolve " + folder.string() + ": " + ec.message(); return false; }
    if (fs::is_directory(root, ec)) {
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (entry.path().extension() == ".swproject") {
                error = root.string() + " already holds a project";
                return false;
            }
        }
    }
    Project result;
    result.root = root;
    result.file = root / "shockwave.swproject";
    result.name = name;
    result.gameModule = gameModule;
    result.startupScene = root / "scenes" / "main.swscene";
    result.assetFolder = root / "assets";
    result.sceneFolder = root / "scenes";
    for (const fs::path& dir : {root, result.assetFolder, result.sceneFolder}) {
        fs::create_directories(dir, ec);
        if (ec) { error = "can't create " + dir.string() + ": " + ec.message(); return false; }
    }
    if (!fs::exists(root / ".gitignore", ec)) {
        std::ofstream ignore(root / ".gitignore");
        ignore << "# Per-user editor state: camera positions, autosaves\n.shockwave/\n";
    }
    if (!saveProject(result, error)) return false;
    project = std::move(result);
    return true;
}
} // namespace shockwave
