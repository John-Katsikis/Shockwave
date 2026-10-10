#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace editor {

// The editor's own settings, per user rather than per project: recent projects, the UI
// theme, viewport and snapping choices. Kept in the user's config folder
// ($XDG_CONFIG_HOME/shockwave/editor.swprefs, usually ~/.config/shockwave) as commented
// key = value lines, so it can be read and edited by hand.
struct Preferences {
    std::vector<std::string> recentProjects;   // .swproject paths, most recent first
    std::vector<std::string> recentScenes;   // .swscene paths, most recent first
    bool reopenLastProject = true;

    std::string theme;   // a .swtheme path; empty uses the project's theme
    bool detachablePanels = true;   // panels can be dragged out of the main window (applies at startup)
    bool vsync = true;
    float frameLimit = 0.0f;   // the editor's own frame cap, fps; 0 = none
    bool lowLatency = false;
    float renderScale = 1.0f;
    float masterVolume = 1.0f;

    float cameraSpeed = 8.0f;
    float fieldOfView = 70.0f;   // degrees
    bool showGrid = true;
    bool showStats = false;
    bool showCameraIcons = true;
    bool playFromSceneCamera = true;   // Play looks through the scene's Camera

    bool snap = false;
    float snapMove = 0.5f;   // units
    float snapRotate = 15.0f;   // degrees
    float snapScale = 0.1f;

    int autosaveMinutes = 5;   // 0 turns autosave off

    void addRecentProject(const std::string& path);
    void addRecentScene(const std::string& path);
};

// Where preferences live on this machine.
std::filesystem::path defaultPreferencesPath();
// A missing file isn't an error: the defaults stay.
bool loadPreferences(const std::filesystem::path& path, Preferences& prefs, std::string& error);
bool savePreferences(const std::filesystem::path& path, const Preferences& prefs, std::string& error);

}   // namespace editor
