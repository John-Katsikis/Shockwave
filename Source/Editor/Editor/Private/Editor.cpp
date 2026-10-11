// The editor application: startup and shutdown, and what runs every frame. The rest of
// the editor core is in Editor*.cpp and PlayInEditor.cpp; panels in *Panel.cpp.
#include "Editor.h"

#include <Assets/BuiltinAssets.h>
#include <Audio/Audio.h>
#include <GameUI/GameUI.h>
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

Editor::Editor(EditorOptions options) : m_options(std::move(options)), m_prefs(m_options.prefs) {}

// ---- Startup and shutdown -----------------------------------------------------------------

void Editor::onStart() {
    shockwave::registerAudioComponents();
    shockwave::registerGameUIComponents();
    m_gameUI = std::make_unique<shockwave::GameUI>(&engine());
    m_audioPreview = std::make_unique<shockwave::AudioMixer>();
    connectConsole();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigWindowsMoveFromTitleBarOnly = true;   // dragging in the viewport is for the scene
    // The dock layout is per user, next to the preferences, not in whatever folder the
    // editor was started from.
    std::error_code ec;
    fs::create_directories(m_options.prefsPath.parent_path(), ec);
    m_iniPath = (m_options.prefsPath.parent_path() / "editor-layout.ini").string();
    io.IniFilename = m_iniPath.c_str();
    // ImGuizmo's colors are sRGB like ImGui's; convert them for the sRGB swapchain too.
    for (ImVec4& c : ImGuizmo::GetStyle().Colors) c = gui::color(c.x, c.y, c.z, c.w);

    // The starter assets the editor and the player share. Game code comes with a project
    // (loadGameModule); until then Play runs the engine's systems only.
    shockwave::createBuiltinAssets(engine().renderer(), engine().assets());
    m_assetFolder = std::make_unique<shockwave::AssetFolder>(engine().renderer(), engine().assets(), engine().jobs());
    createGameInstance();
    engine().setAutoReloadRenderer(m_prefs.autoReloadRenderer);
    shockwave::FlyCamera::bindDefaultControls(engine().input());
    applyViewPreferences();

    if (const char* home = std::getenv("HOME")) m_newProjectLocation = (fs::path(home) / "Shockwave Projects").string();

    if (!m_options.path.empty()) {
        const fs::path path = m_options.path;
        if (const auto projectFile = shockwave::findProjectFile(path)) {
            openProject(*projectFile, path.extension() == ".swscene" ? path : fs::path());
        } else {
            error("No Shockwave project at " + path.string() + ". Create one in the project hub.");
        }
    } else if (
        m_prefs.reopenLastProject && !m_prefs.recentProjects.empty() &&
        fs::exists(m_prefs.recentProjects.front(), ec)) {
        openProject(m_prefs.recentProjects.front());
    }

    m_showPreferences = m_options.showPreferences;
    if (m_hasProject && m_options.focusContent) {
        const std::filesystem::path folder = m_options.contentFolder;
        if (folder.is_absolute() ||
            std::any_of(folder.begin(), folder.end(), [](const auto& part) { return part == ".."; }))
            throw std::runtime_error("--content-folder must be a path inside Content");
        m_contentFolder = folder.lexically_normal().generic_string();
        if (m_contentFolder == ".") m_contentFolder.clear();
        if (!m_contentFolder.empty() &&
            std::find(m_assetFolders.begin(), m_assetFolders.end(), m_contentFolder) == m_assetFolders.end())
            throw std::runtime_error("Unknown Content folder: " + m_contentFolder);
    }
    if (m_hasProject && m_options.selectOnStart >= 0) {
        select(m_world.entityAt(static_cast<size_t>(m_options.selectOnStart)));
    }
    if (m_hasProject && m_options.playOnStart) play();
}

void Editor::onShutdown() {
    stopAudioPreview();
    m_audioPreview.reset();
    if (m_build.thread.joinable()) {
        if (m_build.running) log("Waiting for the game code build to finish");
        m_build.thread.join();
    }
    stop();
    m_session.reset();
    if (m_gameInstance) m_gameInstance->shutdown();
    m_gameUI.reset();
    if (m_hasProject) {
        rememberCamera();
        saveEditorState();
    }
    savePreferences();
    // The engine destroys the ImGui context after the editor is gone, and ImGui saves
    // the layout then; save it now and stop ImGui from using m_iniPath once it's freed
    // (it used to write the layout to a file with a garbage name in the working folder).
    ImGui::SaveIniSettingsToDisk(m_iniPath.c_str());
    ImGui::GetIO().IniFilename = nullptr;
    disconnectConsole();
}

bool Editor::onQuitRequested() {
    if (!m_dirty) return true;
    requestAction(PendingAction::Quit);
    return false;
}

void Editor::applyViewPreferences() {
    engine().setMasterVolume(m_prefs.masterVolume);
    if (m_audioPreview) m_audioPreview->setMasterVolume(m_prefs.masterVolume);
    m_camera.moveSpeed = m_prefs.cameraSpeed;
    m_camera.fovY = glm::radians(std::clamp(m_prefs.fieldOfView, 20.0f, 140.0f));
    engine().renderer().setVsync(m_prefs.vsync);
    engine().setFrameLimit(m_prefs.frameLimit);
    engine().setLowLatency(m_prefs.lowLatency);
    engine().renderer().setRenderScale(m_prefs.renderScale);
    engine().renderer().setShadowQuality(static_cast<shockwave::ShadowQuality>(m_prefs.shadowQuality));
}

void Editor::savePreferences() {
    m_prefsDirty = false;
    std::string err;
    if (!editor::savePreferences(m_options.prefsPath, m_prefs, err)) warn("Couldn't save preferences: " + err);
}

// ---- Per frame ----------------------------------------------------------------------------

void Editor::onGui() {
    drainConsole();
    ImGuizmo::BeginFrame();
    const float dt = ImGui::GetIO().DeltaTime;

    handleDialogResults();
    if (!m_hasProject) {
        engine().renderer().setViewport(0, 0);
        drawMenuBar();
        drawProjectHub();
        drawUnsavedChangesModal();
        if (m_showPreferences) drawPreferencesWindow();
        if (m_showAbout) drawAboutWindow();
        updateTitle();
        return;
    }

    if (m_retryAssetImports) {
        m_retryAssetImports = false;
        logAssetChanges(m_assetFolder->refresh(true));
    } else {
        logAssetChanges(m_assetFolder->poll(dt));
    }
    m_sceneScanTimer += dt;
    if (m_sceneScanTimer > 2.0) scanScenes();
    autosave(dt);
    handleShortcuts();

    drawMenuBar();
    drawCommandBar();
    drawStatusBar();
    drawDockspace();
    drawViewport();
    m_hierarchyOrderNext.clear();
    if (m_showHierarchy) drawHierarchy();
    m_hierarchyOrder.swap(m_hierarchyOrderNext);
    if (m_showInspector) drawInspector();
    if (m_showContent) drawContentBrowser();
    if (m_showConsole) drawConsole();
    if (m_showStats) drawStatsWindow();
    if (m_showInput) drawInputWindow();
    if (m_showPreferences) drawPreferencesWindow();
    if (m_showProjectSettings) drawProjectSettingsWindow();
    m_package.draw(m_project);
    if (m_showControls) drawControlsWindow();
    if (m_showAbout) drawAboutWindow();
    if (m_showDemo) ImGui::ShowDemoWindow(&m_showDemo);
    if (m_showMetrics) ImGui::ShowMetricsWindow(&m_showMetrics);
    gui::focusOutline();
    drawUnsavedChangesModal();
    drawRecoveryModal();
    updateTitle();

    // Preference changes are written once the user lets go of the widget.
    if (m_prefsDirty && !ImGui::IsAnyItemActive()) savePreferences();
}

// Play mode runs the project's gameplay and physics at the fixed tick rate, like the player.
void Editor::onUpdate(double dt) {
    if (playState() != PlayState::Playing) return;
    m_session->update(dt);
}

void Editor::onFrame(double dt) {
    pollGameCode();   // between frames, before anything looks at the world
    if (m_session) m_session->frame(dt);
    updateGameUI(dt);
}

void Editor::onRender(shockwave::Renderer& renderer, double alpha) {
    if (!m_hasProject) {
        renderer.setClearColor({0.0f, 0.0f, 0.0f, 1.0f});
        renderer.setGrid({});
        return;
    }
    renderer.setClearColor({0.0f, 0.0f, 0.0f, 1.0f});   // a scene without an Environment
    const bool sceneCamera = lookingThroughSceneCamera();
    glm::mat4 view = m_camera.view(), proj = projection(renderer.aspectRatio());
    if (sceneCamera)
        shockwave::cameraMatrices(m_world, shockwave::primaryCamera(m_world), renderer.aspectRatio(), view, proj);
    renderer.setCamera(view, proj);

    shockwave::GridSettings grid;
    grid.enabled = m_prefs.showGrid && !sceneCamera;
    grid.cellSize = 1.0f;
    grid.fadeDistance = std::clamp(m_camera.position.y * 12.0f + 60.0f, 60.0f, 600.0f);
    renderer.setGrid(grid);
    // While editing (or paused) nothing moves on its own, so draw exactly what's there.
    shockwave::renderWorld(
        m_world, renderer, engine().jobs(), playState() == PlayState::Playing ? alpha : 1.0, &engine().assets());
    if (m_showGameUI) m_gameUI->render(renderer);   // the viewport shows the game's UI too
}

bool Editor::lookingThroughSceneCamera() const {
    return m_viewThroughCamera && shockwave::primaryCamera(m_world).valid();
}

}   // namespace editor
