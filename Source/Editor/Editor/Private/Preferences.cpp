#include "Preferences.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace editor {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxRecent = 10;

void pushRecent(std::vector<std::string>& list, const std::string& path) {
    std::erase(list, path);
    list.insert(list.begin(), path);
    if (list.size() > kMaxRecent) list.resize(kMaxRecent);
}

std::string trim(const std::string& s) {
    const size_t begin = s.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return {};
    return s.substr(begin, s.find_last_not_of(" \t\r") - begin + 1);
}

bool parseBool(const std::string& v) { return v == "true" || v == "1" || v == "yes" || v == "on"; }

}   // namespace

void Preferences::addRecentProject(const std::string& path) { pushRecent(recentProjects, path); }
void Preferences::addRecentScene(const std::string& path) { pushRecent(recentScenes, path); }

fs::path defaultPreferencesPath() {
#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA")) return fs::path(appData) / "Shockwave" / "editor.swprefs";
#endif
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return fs::path(xdg) / "shockwave" / "editor.swprefs";
    if (const char* home = std::getenv("HOME"); home && *home) {
#ifdef __APPLE__
        return fs::path(home) / "Library" / "Application Support" / "Shockwave" / "editor.swprefs";
#else
        return fs::path(home) / ".config" / "shockwave" / "editor.swprefs";
#endif
    }
    return "editor.swprefs";
}

bool loadPreferences(const fs::path& path, Preferences& prefs, std::string& error) {
    std::ifstream in(path);
    if (!in) return true;   // first run
    Preferences loaded;
    loaded.recentProjects.clear();
    loaded.recentScenes.clear();
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            error = path.string() + ":" + std::to_string(lineNumber) + ": expected key = value";
            return false;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        auto number = [&](float& out) {
            try {
                out = std::stof(value);
            } catch (...) {
                // Keep the default; a bad number shouldn't lock anyone out of the editor.
            }
        };
        if (key == "recent_project")
            loaded.recentProjects.push_back(value);
        else if (key == "recent_scene")
            loaded.recentScenes.push_back(value);
        else if (key == "reopen_last_project")
            loaded.reopenLastProject = parseBool(value);
        else if (key == "theme")
            loaded.theme = value;
        else if (key == "vsync")
            loaded.vsync = parseBool(value);
        else if (key == "frame_limit")
            number(loaded.frameLimit);
        else if (key == "low_latency")
            loaded.lowLatency = parseBool(value);
        else if (key == "master_volume")
            number(loaded.masterVolume);
        else if (key == "render_scale")
            number(loaded.renderScale);
        else if (key == "camera_speed")
            number(loaded.cameraSpeed);
        else if (key == "field_of_view")
            number(loaded.fieldOfView);
        else if (key == "show_grid")
            loaded.showGrid = parseBool(value);
        else if (key == "show_stats")
            loaded.showStats = parseBool(value);
        else if (key == "show_camera_icons")
            loaded.showCameraIcons = parseBool(value);
        else if (key == "play_from_scene_camera")
            loaded.playFromSceneCamera = parseBool(value);
        else if (key == "snap")
            loaded.snap = parseBool(value);
        else if (key == "snap_move")
            number(loaded.snapMove);
        else if (key == "snap_rotate")
            number(loaded.snapRotate);
        else if (key == "snap_scale")
            number(loaded.snapScale);
        else if (key == "autosave_minutes") {
            float minutes = static_cast<float>(loaded.autosaveMinutes);
            number(minutes);
            loaded.autosaveMinutes = std::max(0, static_cast<int>(minutes));
        }
    }
    if (loaded.recentProjects.size() > kMaxRecent) loaded.recentProjects.resize(kMaxRecent);
    if (loaded.recentScenes.size() > kMaxRecent) loaded.recentScenes.resize(kMaxRecent);
    loaded.masterVolume = std::isfinite(loaded.masterVolume) ? std::clamp(loaded.masterVolume, 0.0f, 1.0f) : 1.0f;
    prefs = std::move(loaded);
    return true;
}

bool savePreferences(const fs::path& path, const Preferences& prefs, std::string& error) {
    std::ostringstream out;
    auto flag = [](bool b) { return b ? "true" : "false"; };
    out << "# Shockwave Editor preferences. Written by the editor; safe to edit while it's closed.\n\n"
        << "reopen_last_project = " << flag(prefs.reopenLastProject) << "\n";
    for (const std::string& p : prefs.recentProjects) out << "recent_project = " << p << "\n";
    for (const std::string& p : prefs.recentScenes) out << "recent_scene = " << p << "\n";
    out << "\n# Appearance and output\n"
        << "theme = " << prefs.theme << "\n"
        << "vsync = " << flag(prefs.vsync) << "\n"
        << "frame_limit = " << prefs.frameLimit << "\n"
        << "low_latency = " << flag(prefs.lowLatency) << "\n"
        << "master_volume = " << prefs.masterVolume << "\n"
        << "render_scale = " << prefs.renderScale << "\n"
        << "\n# Viewport\n"
        << "camera_speed = " << prefs.cameraSpeed << "\n"
        << "field_of_view = " << prefs.fieldOfView << "\n"
        << "show_grid = " << flag(prefs.showGrid) << "\n"
        << "show_stats = " << flag(prefs.showStats) << "\n"
        << "show_camera_icons = " << flag(prefs.showCameraIcons) << "\n"
        << "play_from_scene_camera = " << flag(prefs.playFromSceneCamera) << "\n"
        << "\n# Editing\n"
        << "snap = " << flag(prefs.snap) << "\n"
        << "snap_move = " << prefs.snapMove << "\n"
        << "snap_rotate = " << prefs.snapRotate << "\n"
        << "snap_scale = " << prefs.snapScale << "\n"
        << "autosave_minutes = " << prefs.autosaveMinutes << "\n";

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = path.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::trunc);
        if (!(file << out.str())) {
            error = "can't write " + temp.string();
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        error = "can't replace " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

}   // namespace editor
