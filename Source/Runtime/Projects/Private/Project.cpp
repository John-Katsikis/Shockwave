#include <Projects/Project.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>

namespace shockwave {
namespace {
constexpr const char* kShadowQualities[] = {"off", "low", "medium", "high", "ultra"};
}

const char* shadowQualityName(int quality) { return kShadowQualities[std::clamp(quality, 0, 4)]; }

int parseShadowQuality(const std::string& name) {
    for (int i = 0; i < 5; ++i) {
        if (name == kShadowQualities[i]) return i;
    }
    return -1;
}

bool loadProject(const std::filesystem::path& path, Project& project, std::string& error) {
    error.clear();
    std::ifstream in(path);
    if (!in) {
        error = "can't open project " + path.string();
        return false;
    }
    Project result;
    std::error_code ec;
    result.file = std::filesystem::absolute(path, ec).lexically_normal();
    if (ec) {
        error = "can't resolve project path: " + ec.message();
        return false;
    }
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
            if (key == "name" || key == "game_module" || key == "renderer") {
                auto& value = key == "name" ? result.name : key == "renderer" ? result.renderer : result.gameModule;
                if (!(row >> std::quoted(value)) || value.empty()) return fail("empty or invalid " + key);
            } else if (key == "startup_scene" || key == "assets" || key == "scenes" || key == "theme") {
                std::string value;
                if (!(row >> std::quoted(value)) || value.empty()) return fail("empty or invalid " + key);
                auto& target = key == "startup_scene" ? result.startupScene
                               : key == "assets"      ? result.assetFolder
                               : key == "scenes"      ? result.sceneFolder
                                                      : result.theme;
                target = (result.root / value).lexically_normal();
            } else if (key == "tick_rate") {
                if (!(row >> result.tickRate) || !std::isfinite(result.tickRate) || result.tickRate < 1 ||
                    result.tickRate > 1000)
                    return fail("tick_rate must be between 1 and 1000");
            } else if (key == "window_size") {
                if (!(row >> result.width >> result.height) || result.width < 320 || result.height < 240 ||
                    result.width > 16384 || result.height > 16384)
                    return fail("invalid window_size (minimum 320 x 240)");
            } else if (key == "vsync" || key == "low_latency") {
                std::string value;
                row >> value;
                if (value != "true" && value != "false") return fail(key + " must be true or false");
                (key == "vsync" ? result.vsync : result.lowLatency) = value == "true";
            } else if (key == "texture_compression") {
                row >> result.textureCompression;
                if (result.textureCompression != "high" && result.textureCompression != "small" &&
                    result.textureCompression != "off")
                    return fail("texture_compression must be high, small or off");
            } else if (key == "mesh_lods") {
                if (!(row >> result.meshLods) || result.meshLods < 0 || result.meshLods > 7)
                    return fail("mesh_lods must be a whole number from 0 to 7");
            } else if (key == "lod_error_pixels") {
                if (!(row >> result.lodErrorPixels) || !std::isfinite(result.lodErrorPixels) ||
                    result.lodErrorPixels < 0 || result.lodErrorPixels > 100)
                    return fail("lod_error_pixels must be between 0 (always full detail) and 100");
            } else if (key == "shadow_quality") {
                std::string value;
                row >> value;
                result.shadowQuality = parseShadowQuality(value);
                if (result.shadowQuality < 0) return fail("shadow_quality must be off, low, medium, high or ultra");
            } else if (key == "audio_hrtf") {
                std::string value;
                row >> value;
                if (value != "true" && value != "false") return fail("audio_hrtf must be true or false");
                result.audioHrtf = value == "true";
            } else if (key == "audio_hrtf_quality") {
                std::string value;
                row >> value;
                if (value != "low" && value != "high") return fail("audio_hrtf_quality must be low or high");
                result.audioHrtfHigh = value == "high";
            } else if (key == "frame_limit") {
                if (!(row >> result.frameLimit) || !std::isfinite(result.frameLimit) || result.frameLimit < 0 ||
                    (result.frameLimit > 0 && result.frameLimit < 10) || result.frameLimit > 1000)
                    return fail("frame_limit must be 0 (no limit) or between 10 and 1000");
            } else
                return fail("unknown key '" + key + "'");
        }
        row >> std::ws;
        if (row.peek() != EOF && row.peek() != '#') return fail("unexpected trailing value");
    }
    if (in.bad()) return fail("error reading project");
    if (!versionSeen || result.name.empty() || result.gameModule.empty() || result.startupScene.empty())
        return fail("project requires version, name, game_module and startup_scene");
    if (result.assetFolder.empty()) result.assetFolder = result.root / "Content";
    if (result.sceneFolder.empty()) result.sceneFolder = result.root / "Scenes";
    if (!std::filesystem::is_regular_file(result.startupScene, ec))
        return fail("startup scene does not exist: " + result.startupScene.string());
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
    auto quotedPath = [&](const fs::path& p) {
        std::ostringstream q;
        q << std::quoted(project.relative(p));
        return q.str();
    };
    std::ostringstream out;
    out << "# Paths are relative to this file. Both editor and player load this manifest.\n"
        << "shockwave_project 1\n"
        << "name " << std::quoted(project.name) << "\n"
        << "game_module " << std::quoted(project.gameModule) << "\n"
        << "startup_scene " << quotedPath(project.startupScene) << "\n"
        << "assets " << quotedPath(project.assetFolder) << "\n";
    if (!project.sceneFolder.empty() && project.sceneFolder != project.root / "Scenes")
        out << "scenes " << quotedPath(project.sceneFolder) << "\n";
    if (!project.theme.empty()) out << "theme " << quotedPath(project.theme) << "\n";
    out << "tick_rate " << project.tickRate << "\n"
        << "window_size " << project.width << " " << project.height << "\n"
        << "vsync " << (project.vsync ? "true" : "false") << "\n";
    if (project.frameLimit > 0) out << "frame_limit " << project.frameLimit << "\n";
    if (project.lowLatency) out << "low_latency true\n";
    if (project.textureCompression != "high") out << "texture_compression " << project.textureCompression << "\n";
    if (project.meshLods != 4) out << "mesh_lods " << project.meshLods << "\n";
    if (project.lodErrorPixels != 1.0) out << "lod_error_pixels " << project.lodErrorPixels << "\n";
    if (project.shadowQuality != 3) out << "shadow_quality " << shadowQualityName(project.shadowQuality) << "\n";
    if (project.renderer != "vulkan") out << "renderer " << std::quoted(project.renderer) << "\n";
    if (!project.audioHrtf) out << "audio_hrtf false\n";
    if (!project.audioHrtfHigh) out << "audio_hrtf_quality low\n";
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
        std::optional<fs::path> found;
        for (const auto& entry : fs::directory_iterator(folder, ec)) {
            if (entry.path().extension() == ".swproject" && entry.is_regular_file(ec) &&
                (!found || entry.path() < *found))
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

bool createProject(
    const std::filesystem::path& folder, const std::string& name, const std::string& gameModule, Project& project,
    std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (name.empty()) {
        error = "the project needs a name";
        return false;
    }
    const fs::path root = fs::absolute(folder, ec).lexically_normal();
    if (ec) {
        error = "can't resolve " + folder.string() + ": " + ec.message();
        return false;
    }
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
    std::string fileName;
    for (const char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') fileName += c;
    result.file = root / ((fileName.empty() ? std::string("Game") : fileName) + ".swproject");
    result.name = name;
    result.gameModule = gameModule;
    result.startupScene = root / "Scenes" / "main.swscene";
    result.assetFolder = root / "Content";
    result.sceneFolder = root / "Scenes";
    for (const fs::path& dir : {root, result.assetFolder, result.sceneFolder}) {
        fs::create_directories(dir, ec);
        if (ec) {
            error = "can't create " + dir.string() + ": " + ec.message();
            return false;
        }
    }
    if (!fs::exists(root / ".gitignore", ec)) {
        std::ofstream ignore(root / ".gitignore");
        ignore << "# Per-user editor state, autosaves and screenshots\nSaved/\n";
    }
    if (!saveProject(result, error)) return false;
    project = std::move(result);
    return true;
}
}   // namespace shockwave
