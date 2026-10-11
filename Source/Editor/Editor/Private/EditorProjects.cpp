// Projects: opening, creating and closing them, and the per-user editor state kept in
// the project's Saved folder.
#include "Editor.h"

#include <Assets/BuiltinAssets.h>
#include <Physics/Physics.h>
#include <UI/Gui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::Entity;
using shockwave::kNullEntity;
using shockwave::MeshRenderer;
using shockwave::Name;
using shockwave::Parent;
using shockwave::Transform;

// ---- Projects -----------------------------------------------------------------------------

bool Editor::openProject(const fs::path& path, const fs::path& sceneToOpen) {
    shockwave::Project project;
    std::string err;
    if (!shockwave::loadProject(path, project, err)) {
        error("Couldn't open project: " + err);
        m_hubError = err;
        return false;
    }
    stop();
    if (m_hasProject) {
        rememberCamera();
        saveEditorState();
    }
    m_project = project;
    m_projectEdit = project;
    m_hasProject = true;
    m_hubError.clear();
    m_scenePath.clear();
    m_contentRoot = ContentRoot::Assets;
    m_contentFolder.clear();
    m_prefs.addRecentProject(m_project.file.string());
    savePreferences();
    log("Opened project " + m_project.name + " (" + m_project.root.string() + ")");
    // The project's game code, before any scene: its components must be known to load one.
    loadGameModule(m_project.gameModule);
    engine().setTickRate(m_project.tickRate);
    applyAudioSettings(m_project);
    applyProjectTheme();
    loadProjectInput();
    logAssetChanges(
        m_assetFolder->open(m_options.assetFolder.empty() ? m_project.assetFolder : fs::path(m_options.assetFolder)));
    m_gameUI->setContentFolder(m_project.assetFolder);
    loadEditorState();
    scanScenes();

    // The scene asked for, else the one open last time, else the startup scene, else any.
    std::error_code ec;
    fs::path scene = sceneToOpen;
    if (scene.empty() && !m_lastScene.empty() && fs::exists(m_project.root / m_lastScene, ec)) {
        scene = m_project.root / m_lastScene;
    }
    if (scene.empty() && fs::exists(m_project.startupScene, ec)) scene = m_project.startupScene;
    if (scene.empty() && !m_scenes.empty()) scene = m_scenes.front();
    if (scene.empty() || !openScene(scene)) {
        // Give the project a first scene to work in, saved where it expects its startup scene.
        newScene(SceneTemplate::Basic);
        if (!fs::exists(m_project.startupScene, ec)) saveSceneAs(m_project.startupScene);
    }
    return true;
}

// The theme: --theme if given, else the one picked in Preferences, else the project's.
void Editor::applyProjectTheme() {
    std::error_code ec;
    std::string theme = m_options.theme;
    if (theme.empty()) theme = m_prefs.theme;
    if (theme.empty() && m_hasProject && fs::is_regular_file(m_project.theme, ec)) theme = m_project.theme.string();
    if (!theme.empty() && theme != engine().guiThemePath()) engine().setGuiTheme(theme);
}

void Editor::closeProject() {
    if (!m_hasProject) return;
    stop();
    rememberCamera();
    saveEditorState();
    log("Closed project " + m_project.name);
    m_hasProject = false;
    engine().input().clearBindingsFile();
    m_world.clear();
    m_gameUI->setContentFolder({});
    m_scenePath.clear();
    m_selection.clear();
    m_undo.clear();
    m_redo.clear();
    m_dirty = false;
    m_scenes.clear();
}

bool Editor::createNewProject(const fs::path& folder, const std::string& name) {
    shockwave::Project project;
    std::string err;
    // New projects play with the game module loaded now, if any, so Play works straight away.
    const std::string module = m_gameModule ? std::string(m_gameModule->id()) : "game";
    if (!shockwave::createProject(folder, name, module, project, err)) {
        m_hubError = err;
        error("Couldn't create the project: " + err);
        return false;
    }
    m_hubError.clear();
    log("Created project " + project.name + " in " + folder.string());
    // The manifest needs its startup scene to exist before it will load.
    shockwave::World scene;
    makeScene(SceneTemplate::Basic, scene, engine().assets());
    if (!shockwave::saveScene(scene, engine().assets(), project.startupScene.string(), err)) {
        m_hubError = err;
        error("Couldn't write the first scene: " + err);
        return false;
    }
    return openProject(project.file);
}

// Saved/editor.swstate: which scene was open and where the camera was in each scene.
// Per user, so it stays out of version control.
void Editor::loadEditorState() {
    m_sceneCameras.clear();
    m_lastScene.clear();
    std::ifstream in(m_project.statePath() / "editor.swstate");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "last_scene") {
            std::getline(ls >> std::ws, m_lastScene);
        } else if (key == "camera") {
            EditorCamera camera;
            std::string scene;
            if (ls >> camera.position.x >> camera.position.y >> camera.position.z >> camera.yaw >> camera.pitch &&
                std::getline(ls >> std::ws, scene)) {
                m_sceneCameras[scene] = camera;
            }
        }
    }
}

void Editor::saveEditorState() {
    if (!m_hasProject) return;
    std::error_code ec;
    fs::create_directories(m_project.statePath(), ec);
    std::ofstream out(m_project.statePath() / "editor.swstate", std::ios::trunc);
    out << "# Shockwave Editor state for this project, per user. Safe to delete.\n";
    if (!m_lastScene.empty()) out << "last_scene " << m_lastScene << "\n";
    for (const auto& [scene, c] : m_sceneCameras) {
        out << "camera " << c.position.x << ' ' << c.position.y << ' ' << c.position.z << ' ' << c.yaw << ' ' << c.pitch
            << ' ' << scene << "\n";
    }
}

void Editor::rememberCamera() {
    if (m_scenePath.empty()) return;
    m_sceneCameras[m_project.relative(m_scenePath)] = {m_camera.position, m_camera.yaw, m_camera.pitch};
}

fs::path Editor::autosavePath() const {
    std::string name = m_scenePath.empty() ? "untitled" : m_project.relative(m_scenePath);
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':') c = '_';
    }
    if (!name.ends_with(".swscene")) name += ".swscene";
    return m_project.statePath() / "autosave" / name;
}

void Editor::scanScenes() {
    m_sceneScanTimer = 0.0;
    m_scenes.clear();
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(m_project.sceneFolder, ec); !ec && it != fs::end(it);
         it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".swscene") m_scenes.push_back(it->path());
    }
    std::sort(m_scenes.begin(), m_scenes.end());

    m_assetFolders.clear();
    const fs::path assets = m_project.assetFolder;
    for (auto it = fs::recursive_directory_iterator(assets, ec); !ec && it != fs::end(it); it.increment(ec)) {
        if (it->path().filename().string().starts_with(".")) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(ec)) m_assetFolders.push_back(it->path().lexically_relative(assets).generic_string());
    }
    std::sort(m_assetFolders.begin(), m_assetFolders.end());
}

}   // namespace editor
