// Scene files: new, open, save and autosave, the file dialogs, and actions that wait
// for unsaved changes to be dealt with first.
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

namespace {

const SDL_DialogFileFilter kSceneFilter[] = {{"Shockwave scenes", "swscene"}};
const SDL_DialogFileFilter kProjectFilter[] = {{"Shockwave projects", "swproject"}};
const SDL_DialogFileFilter kImportFilter[] = {
    {"Images and models", "png;jpg;jpeg;tga;bmp;psd;gif;hdr;pic;pnm;glb;gltf;fbx;obj;dae;stl;ply;blend"},
    {"All files", "*"}};

}   // namespace

// ---- Scene files --------------------------------------------------------------------------

std::string Editor::sceneDisplayName() const {
    return m_scenePath.empty() ? "Untitled" : m_scenePath.filename().string();
}

void Editor::newScene(SceneTemplate kind) {
    stop();
    rememberCamera();
    makeScene(kind, m_world, engine().assets());
    m_scenePath.clear();
    const EditorCamera camera;
    m_camera.position = camera.position;
    m_camera.yaw = camera.yaw;
    m_camera.pitch = camera.pitch;
    m_undo.clear();
    m_redo.clear();
    select(kNullEntity);
    m_dirty = false;
    m_viewThroughCamera = false;
    log(kind == SceneTemplate::Basic ? "New scene from the Basic template" : "New empty scene");
}

bool Editor::openScene(const fs::path& path) {
    stop();
    shockwave::World loaded;
    std::string err;
    std::vector<std::string> warnings;
    // Scenes saved by older editors carry the camera in the file.
    EditorCamera fileCamera;
    bool hasFileCamera = false;
    shockwave::SceneFileHooks hooks;
    hooks.readHeader = [&](const std::string& key, std::istream& in) {
        if (key == "editor_camera" || key == "camera") {
            in >> fileCamera.position.x >> fileCamera.position.y >> fileCamera.position.z >> fileCamera.yaw >>
                fileCamera.pitch;
            hasFileCamera = true;
        }
    };
    if (!shockwave::loadScene(loaded, engine().assets(), path.string(), err, hooks, &warnings)) {
        error("Couldn't open scene: " + err);
        return false;
    }
    rememberCamera();
    for (const std::string& w : warnings) warn(w);
    m_world = std::move(loaded);
    m_scenePath = fs::absolute(path).lexically_normal();
    const std::string relative = m_project.relative(m_scenePath);
    EditorCamera camera = hasFileCamera ? fileCamera : EditorCamera();
    if (const auto it = m_sceneCameras.find(relative); it != m_sceneCameras.end()) camera = it->second;
    m_camera.position = camera.position;
    m_camera.yaw = camera.yaw;
    m_camera.pitch = camera.pitch;
    m_undo.clear();
    m_redo.clear();
    select(kNullEntity);
    m_dirty = false;
    m_viewThroughCamera = false;
    m_autosaveTimer = 0.0;
    m_lastScene = relative;
    m_prefs.addRecentScene(m_scenePath.string());
    m_prefsDirty = true;
    saveEditorState();
    log("Opened " + relative + " (" + std::to_string(m_world.entityCount()) + " entities)");

    // An autosave newer than the file means the editor stopped with unsaved changes.
    std::error_code ec;
    const fs::path autosaved = autosavePath();
    if (fs::exists(autosaved, ec) && fs::last_write_time(autosaved, ec) > fs::last_write_time(m_scenePath, ec)) {
        m_recoveryPath = autosaved;
        m_openRecoveryModal = true;
    }
    return true;
}

bool Editor::saveScene() {
    if (!m_hasProject) return false;
    if (playing()) {
        warn("Stop playing before saving: the scene is mid-simulation");
        return false;
    }
    if (m_scenePath.empty()) {
        showSaveSceneDialog();
        return false;
    }
    return saveSceneAs(m_scenePath);
}

bool Editor::saveSceneAs(fs::path path) {
    if (playing()) {
        warn("Stop playing before saving: the scene is mid-simulation");
        return false;
    }
    if (path.extension() != ".swscene") path += ".swscene";
    path = fs::absolute(path).lexically_normal();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path oldAutosave = autosavePath();
    std::string err;
    if (!shockwave::saveScene(m_world, engine().assets(), path.string(), err)) {
        error("Couldn't save: " + err);
        return false;
    }
    fs::remove(oldAutosave, ec);
    m_scenePath = path;
    fs::remove(autosavePath(), ec);
    m_dirty = false;
    m_autosaveTimer = 0.0;
    const std::string relative = m_project.relative(m_scenePath);
    if (fs::path(relative).is_absolute())
        warn(relative + " is outside the project folder; other machines won't find it");
    m_lastScene = relative;
    rememberCamera();
    saveEditorState();
    m_prefs.addRecentScene(m_scenePath.string());
    m_prefsDirty = true;
    scanScenes();
    log("Saved " + relative);

    // A project without a working startup scene gets this one.
    if (!fs::is_regular_file(m_project.startupScene, ec)) {
        m_project.startupScene = m_scenePath;
        m_projectEdit.startupScene = m_scenePath;
        if (shockwave::saveProject(m_project, err)) log("Startup scene set to " + relative);
    }
    return true;
}

void Editor::autosave(double frameDt) {
    if (m_prefs.autosaveMinutes <= 0 || !m_dirty || playing()) return;
    m_autosaveTimer += frameDt;
    if (m_autosaveTimer < m_prefs.autosaveMinutes * 60.0) return;
    m_autosaveTimer = 0.0;
    const fs::path path = autosavePath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::string err;
    if (shockwave::saveScene(m_world, engine().assets(), path.string(), err)) {
        log("Autosaved " + sceneDisplayName());
    } else {
        warn("Autosave failed: " + err);
    }
}

// SDL calls back from another thread (or later on this one); results are picked up in
// handleDialogResults at the start of a frame.
namespace {

struct PendingDialog {
    std::mutex* mutex;
    void* results;   // std::vector<Editor::DialogResult>*, type-erased to keep it private
    int kind;
    std::string* error;
};

}   // namespace

#define SW_DIALOG_CALLBACK                                                                                             \
    [](void* user, const char* const* files, int) {                                                                    \
        auto* pending = static_cast<PendingDialog*>(user);                                                             \
        {                                                                                                              \
            std::lock_guard lock(*pending->mutex);                                                                     \
            if (!files) {                                                                                              \
                *pending->error = SDL_GetError();                                                                      \
            } else if (files[0]) {                                                                                     \
                std::vector<std::string> list;                                                                         \
                for (const char* const* f = files; *f; ++f) list.emplace_back(*f);                                     \
                static_cast<std::vector<DialogResult>*>(pending->results)                                              \
                    ->push_back({static_cast<DialogKind>(pending->kind), std::move(list)});                            \
            }                                                                                                          \
        }                                                                                                              \
        delete pending;                                                                                                \
    }

void Editor::showOpenSceneDialog() {
    auto* pending =
        new PendingDialog{&m_dialogMutex, &m_dialogResults, static_cast<int>(DialogKind::OpenScene), &m_dialogError};
    const std::string folder = m_project.sceneFolder.string();
    SDL_ShowOpenFileDialog(
        SW_DIALOG_CALLBACK, pending, engine().window().handle(), kSceneFilter, 1, folder.c_str(), false);
}

void Editor::showSaveSceneDialog() {
    if (!m_hasProject) return;
    auto* pending =
        new PendingDialog{&m_dialogMutex, &m_dialogResults, static_cast<int>(DialogKind::SaveScene), &m_dialogError};
    const std::string location =
        m_scenePath.empty() ? (m_project.sceneFolder / "untitled.swscene").string() : m_scenePath.string();
    SDL_ShowSaveFileDialog(SW_DIALOG_CALLBACK, pending, engine().window().handle(), kSceneFilter, 1, location.c_str());
}

void Editor::showOpenProjectDialog() {
    auto* pending =
        new PendingDialog{&m_dialogMutex, &m_dialogResults, static_cast<int>(DialogKind::OpenProject), &m_dialogError};
    SDL_ShowOpenFileDialog(
        SW_DIALOG_CALLBACK, pending, engine().window().handle(), kProjectFilter, 1, m_newProjectLocation.c_str(),
        false);
}

void Editor::showNewProjectFolderDialog() {
    auto* pending = new PendingDialog{
        &m_dialogMutex, &m_dialogResults, static_cast<int>(DialogKind::NewProjectFolder), &m_dialogError};
    SDL_ShowOpenFolderDialog(
        SW_DIALOG_CALLBACK, pending, engine().window().handle(), m_newProjectLocation.c_str(), false);
}

void Editor::showImportDialog(const fs::path& folder) {
    m_importFolder = folder;
    auto* pending =
        new PendingDialog{&m_dialogMutex, &m_dialogResults, static_cast<int>(DialogKind::Import), &m_dialogError};
    SDL_ShowOpenFileDialog(SW_DIALOG_CALLBACK, pending, engine().window().handle(), kImportFilter, 2, nullptr, true);
}

#undef SW_DIALOG_CALLBACK

void Editor::handleDialogResults() {
    std::vector<DialogResult> results;
    std::string err;
    {
        std::lock_guard lock(m_dialogMutex);
        results.swap(m_dialogResults);
        err.swap(m_dialogError);
    }
    if (!err.empty()) error("File dialog unavailable (" + err + "). It needs xdg-desktop-portal or zenity.");
    for (const DialogResult& r : results) {
        switch (r.kind) {
        case DialogKind::OpenScene: openScene(r.files[0]); break;   // unsaved changes were already confirmed
        case DialogKind::SaveScene: saveSceneAs(r.files[0]); break;
        case DialogKind::OpenProject: requestAction(PendingAction::OpenProject, r.files[0]); break;
        case DialogKind::NewProjectFolder: m_newProjectLocation = r.files[0]; break;
        case DialogKind::Import: {
            std::error_code ec;
            fs::create_directories(m_importFolder, ec);
            for (const std::string& file : r.files) {
                const fs::path target = m_importFolder / fs::path(file).filename();
                fs::copy_file(file, target, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    error("Couldn't import " + file + ": " + ec.message());
                } else {
                    log("Copied " + fs::path(file).filename().string() + " into " + m_project.relative(m_importFolder));
                }
            }
            logAssetChanges(m_assetFolder->refresh());
            break;
        }
        }
    }
}

void Editor::requestAction(PendingAction action, const std::string& path) {
    if (!m_dirty) {
        runAction(action, path);
        return;
    }
    m_pendingAction = action;
    m_pendingPath = path;
    m_openUnsavedModal = true;
}

void Editor::runAction(PendingAction action, const std::string& path) {
    switch (action) {
    case PendingAction::NewBasicScene: newScene(SceneTemplate::Basic); break;
    case PendingAction::NewEmptyScene: newScene(SceneTemplate::Empty); break;
    case PendingAction::OpenSceneDialog: showOpenSceneDialog(); break;
    case PendingAction::OpenScene: openScene(path); break;
    case PendingAction::OpenProject: {
        const auto file = shockwave::findProjectFile(path);
        if (file) {
            m_dirty = false;   // confirmed
            openProject(*file);
        } else {
            error("No Shockwave project at " + path);
        }
        break;
    }
    case PendingAction::CloseProject:
        m_dirty = false;
        closeProject();
        break;
    case PendingAction::Quit: engine().quit(); break;
    case PendingAction::None: break;
    }
}

void Editor::updateTitle() {
    std::string title = "Shockwave Editor";
    if (m_hasProject) {
        title = m_project.name + " - " + sceneDisplayName() + (m_dirty ? " *" : "") + (playing() ? " [Playing]" : "") +
                " - Shockwave Editor";
    }
    if (title != m_title) {
        engine().window().setTitle(title);
        m_title = title;
    }
}

}   // namespace editor
