#pragma once

#include "Preferences.h"
#include "SceneTemplates.h"

#include <Assets/AssetFolder.h>
#include <Audio/Audio.h>
#include <Core/Log.h>
#include <Engine/Engine.h>
#include <GameFramework/GameModule.h>
#include <GameFramework/GameModuleLibrary.h>
#include <GameFramework/PlaySession.h>
#include <GameUI/GameUI.h>
#include <Projects/Project.h>

#include <imgui.h>
#include <ImGuizmo.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace editor {

SW_LOG_CATEGORY(LogEditor);

// Drag and drop payloads between panels. Meshes and textures travel by asset name.
inline constexpr const char* kEntityPayload = "SW_ENTITY";
inline constexpr const char* kMeshPayload = "SW_MESH";
inline constexpr const char* kModelPayload = "SW_MODEL";
inline constexpr const char* kAudioPayload = "SW_AUDIO";
inline constexpr const char* kTexturePayload = "SW_TEXTURE";
inline constexpr const char* kUIDocumentPayload = "SW_UI_DOCUMENT";
// Labels for shockwave::ShadowQuality, indexed by its value.
inline constexpr const char* kShadowQualityNames[] = {"Off", "Low", "Medium", "High", "Ultra"};

struct EditorOptions {
    std::string path;   // project folder, .swproject or .swscene; empty: last project
    Preferences prefs;
    std::filesystem::path prefsPath;   // where prefs are saved back
    std::string assetFolder;   // --assets: imported instead of the project's assets
    std::string contentFolder;   // --content-folder: inside project Content
    bool showPreferences = false;   // --preferences: open settings on startup
    bool focusContent = false;   // startup tab selection for CLI previews
    std::string theme;   // --theme: wins over the project's and the preferences' theme
    int selectOnStart = -1;   // entity to select at startup, in scene order (screenshots, tests)
    bool playOnStart = false;   // press Play straight away (tests)
};

// The Shockwave Editor: projects, scenes and a live viewport in a docked Dear ImGui UI.
//
//   Commands   the scene and save state, Play / Pause / Stop, the mode
//   Viewport   transform tools above the scene, an editor grid and gizmos; looks through the scene's
//              Camera while playing (F8 switches between that and the editor camera)
//   Hierarchy  the scene's entities as a tree, with multi-select, rename and drag to parent
//   Inspector  the selected entity's components, generated from their registered fields
//   Content    the project's assets and scenes, plus the engine's built-in assets
//   Console    what the editor did, with warnings and errors called out
//   Input      controllers plugged in, live, and the project's action bindings (Window > Input)
//   Status bar scene, save state, play state, selection and frame time at a glance
//
// Everything belongs to a project (shockwave/Project.h): with none open, the editor shows
// the project hub. Per-user settings live in Preferences; per-user, per-project state
// (camera positions, autosaves) in the project's Saved folder.
//
// The scene is an engine World, the same thing a game runs. Play starts a PlaySession:
// the project's gameplay and Jolt physics run on it each tick, and Stop puts back the
// scene as authored. Gameplay is the project's game module, loaded as a plugin: rebuild
// it and the editor reloads it in place, scene and undo history intact (EditorCode.cpp).
// The renderer is a plugin too, and can be swapped or reloaded the same way.
//
// The UI runs on the main thread (ImGui is single-threaded); systems and drawing fan out
// across the job system like any game.
class Editor : public shockwave::Application {
public:
    explicit Editor(EditorOptions options);
    ~Editor() override { disconnectConsole(); }

    void onStart() override;
    void onUpdate(double dt) override;
    void onFrame(double dt) override;
    void onGui() override;
    void onRender(shockwave::Renderer& renderer, double alpha) override;
    bool onQuitRequested() override;
    void onShutdown() override;

private:
    // Something that would throw away unsaved changes, waiting on the user's answer.
    enum class PendingAction {
        None,
        NewBasicScene,
        NewEmptyScene,
        OpenSceneDialog,
        OpenScene,
        OpenProject,
        CloseProject,
        Quit
    };
    using PlayState = shockwave::PlaySession::State;
    enum class LogLevel { Info, Warning, Error };
    struct LogEntry {
        std::string time;   // HH:MM:SS
        LogLevel level;
        std::string text;
    };
    enum class ContentRoot { Assets, Scenes, Engine };
    enum class CreateKind { Empty, Cube, Sphere, Plane, Camera, Environment, DirectionalLight, PointLight, SpotLight };

    // ---- Projects (Editor.cpp) ----
    bool openProject(const std::filesystem::path& path, const std::filesystem::path& sceneToOpen = {});
    void closeProject();
    bool createNewProject(const std::filesystem::path& folder, const std::string& name);
    void loadEditorState();
    void saveEditorState();
    void rememberCamera();
    std::filesystem::path autosavePath() const;
    void scanScenes();   // and the asset folder tree, for the Content panel

    // ---- Panels and windows ----
    // Workspace.cpp: the frame around the panels.
    void drawMenuBar();
    void drawCommandBar();
    void drawStatusBar();
    void drawDockspace();
    void buildDefaultLayout(ImGuiID dockspace);
    void drawCreateMenuItems(shockwave::Entity parent);
    void drawStatsWindow();
    void drawFrameTimeGraph();
    void drawPreferencesWindow();
    void previewAudio(const std::string& clip, shockwave::AudioPlayback options = {});
    void stopAudioPreview();
    shockwave::Entity spawnAudio(const std::string& clip, const glm::vec3& position);
    void drawProjectSettingsWindow();
    void drawControlsWindow();
    void drawAboutWindow();
    void drawUnsavedChangesModal();
    void drawRecoveryModal();
    // ProjectHub.cpp: shown when no project is open.
    void drawProjectHub();
    // ViewportPanel.cpp
    void drawViewport();
    void drawViewportToolbar();
    void drawViewportOverlay(const ImVec2& pos, const ImVec2& size);
    void drawGizmo(const ImVec2& pos, const ImVec2& size);
    void drawOutline(shockwave::Entity e, const ImVec2& pos, const ImVec2& size, ImU32 color);
    void drawCameraIcons(const ImVec2& pos, const ImVec2& size);
    shockwave::Entity cameraIconAt(const ImVec2& mouse, const ImVec2& pos, const ImVec2& size) const;
    void drawLightIcons(const ImVec2& pos, const ImVec2& size);
    void saveViewportScreenshot();
    void updateCamera(bool hovered);
    shockwave::Entity pick(const ImVec2& mouse, const ImVec2& pos, const ImVec2& size);
    void mouseRay(const ImVec2& mouse, const ImVec2& pos, const ImVec2& size, glm::vec3& origin, glm::vec3& dir) const;
    bool worldToScreen(const glm::vec3& p, const ImVec2& pos, const ImVec2& size, ImVec2& out) const;
    void acceptViewportDrop(const ImVec2& pos, const ImVec2& size);
    const shockwave::Bounds* meshBounds(shockwave::MeshHandle mesh);
    bool entityBounds(shockwave::Entity e, shockwave::Bounds& out);   // local space
    glm::mat4 projection(float aspect) const;   // reverse-Z, what the renderer uses
    glm::mat4 gizmoProjection(float aspect) const;   // conventional depth, for ImGuizmo
    bool lookingThroughSceneCamera() const;
    // HierarchyPanel.cpp
    void drawHierarchy();
    using ChildMap = std::unordered_map<uint32_t, std::vector<shockwave::Entity>>;
    void drawHierarchyNode(shockwave::Entity e, const ChildMap& children, int depth);
    bool drawHierarchyRow(shockwave::Entity e, bool hasChildren);   // true if open
    void drawEntityContextMenu(shockwave::Entity e);
    // InspectorPanel.cpp
    void drawInspector();
    void drawComponent(const shockwave::ComponentInfo& info, shockwave::Entity e);
    void drawField(const shockwave::Field& field, void* data, shockwave::Entity e);
    void drawEntityField(const std::string& label, shockwave::Entity* value, shockwave::Entity owner, bool isParent);
    void drawAddComponentPopup(shockwave::Entity e);
    void drawComponentNotes(const shockwave::ComponentInfo& info, shockwave::Entity e);
    // AssetBrowser.cpp: the Content panel.
    void drawContentBrowser();
    void drawContentTree();
    void drawContentItems();
    // ConsolePanel.cpp
    void drawConsole();
    // GameUIPanel.cpp: game UI preview, input during Play, Inspector notes, stats.
    void updateGameUI(double dt);
    void feedGameUIPointer(const ImVec2& pos, const ImVec2& size, bool hovered);
    void drawUIDocumentNotes(shockwave::Entity e);
    void drawGameUIStats();
    // Adds an entity showing the document; writes a starter document first if it's missing.
    shockwave::Entity createUIDocument(const std::string& document);
    std::string newUIDocumentPath() const;   // "UI/hud.rml", or the first free "UI/hud2.rml"...
    // InputPanel.cpp: controllers and the project's bindings (Config/Input.swinput).
    void drawInputWindow();
    void loadProjectInput();   // on project open and Revert
    void applyInputEdit();   // m_inputEdit goes live, unsaved
    void saveInputBindings();

    // ---- Selection (Editor.cpp) ----
    void select(shockwave::Entity e);   // replaces the selection; null clears it
    void toggleSelected(shockwave::Entity e);   // Ctrl+click
    void selectRange(shockwave::Entity to);   // Shift+click, in hierarchy order
    void selectAll();
    bool isSelected(shockwave::Entity e) const;
    shockwave::Entity selected() const;   // the primary (last clicked) entity, or null
    std::vector<shockwave::Entity> selection() const;   // live entities, primary last
    std::vector<shockwave::Entity> selectionRoots() const;   // without ones inside other selected ones

    // ---- Editing (Editor.cpp) ----
    std::string displayName(shockwave::Entity e) const;
    std::string kindLabel(shockwave::Entity e);   // "Mesh", "Camera", ... for the hierarchy
    shockwave::Entity createEntity(CreateKind kind, shockwave::Entity parent);
    shockwave::Entity spawnMesh(const std::string& mesh, const glm::vec3& position);
    shockwave::Entity spawnModel(const std::string& model, const glm::vec3& position);
    glm::vec3 placementPoint() const;   // where new things go: in front of the camera
    bool reparent(shockwave::Entity child, shockwave::Entity parent);
    bool reparentSelection(shockwave::Entity dragged, shockwave::Entity parent);
    bool applyTexture(shockwave::Entity e, const std::string& texture);
    void duplicateSelected();
    void deleteSelected();
    void groupSelected();
    void alignSelectedToView();
    void focusSelected();
    void beginRename(shockwave::Entity e);
    std::string uniqueName(const std::string& base) const;
    void trackEdit();   // call after a widget that edits the scene
    void pushUndo(shockwave::World before);
    void undo();
    void redo();
    void handleShortcuts();

    // ---- Game code and plugins (EditorCode.cpp) ----
    // Loads the project's game module (unloading another project's first). Without it the
    // project still opens for editing, with Play off until the module is built.
    void loadGameModule(const std::string& id);
    void unloadGameModule();
    // Reloads the game module from disk, keeping the scene, selection and undo history.
    void reloadGameCode();
    // Runs CMake on the editor's build folder for the module in the background, then
    // reloads it if the build succeeded.
    void buildGameCode();
    bool canBuildGameCode() const;
    void pollGameCode();   // once per frame: finished builds, rebuilt modules
    void createGameInstance();
    void drawBuildMenu();

    // ---- Play mode (Editor.cpp) ----
    void play();
    void pause();
    void stop();
    PlayState playState() const { return m_session ? m_session->state() : PlayState::Editing; }
    bool playing() const { return playState() != PlayState::Editing; }
    void applyProjectTheme();

    // ---- Scene files (Editor.cpp) ----
    void newScene(SceneTemplate kind);
    bool openScene(const std::filesystem::path& path);
    bool saveScene();
    bool saveSceneAs(std::filesystem::path path);
    void showOpenSceneDialog();
    void showSaveSceneDialog();
    void showOpenProjectDialog();
    void showNewProjectFolderDialog();
    void showImportDialog(const std::filesystem::path& folder);
    void handleDialogResults();
    void requestAction(PendingAction action, const std::string& path = {});
    void runAction(PendingAction action, const std::string& path);
    void autosave(double frameDt);
    void updateTitle();
    std::string sceneDisplayName() const;

    // ---- Console (ConsolePanel.cpp) ----
    void log(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);
    void addLog(LogLevel level, const std::string& message);
    void connectConsole();
    void disconnectConsole();
    void drainConsole();   // main thread, once per frame
    void logAssetChanges(const std::vector<shockwave::AssetFolder::Change>& changes);

    void savePreferences();
    void applyViewPreferences();

    // ---- State ----
    EditorOptions m_options;
    Preferences& m_prefs;   // m_options.prefs
    bool m_prefsDirty = false;

    // The project's game code. Declared before everything that can hold its types (the
    // world, undo history, the GameInstance), so it's unloaded after them.
    std::unique_ptr<shockwave::GameModuleLibrary> m_gameModule;
    shockwave::RebuildWatcher m_gameModuleWatcher;
    struct GameCodeBuild {
        std::thread thread;
        std::atomic<bool> running{false};
        std::atomic<bool> finished{false};
        int exitCode = 0;
        std::string target;
        double started = 0.0;
    } m_build;
    bool m_reloadGameCodeRequested = false;   // done between frames, not mid-GUI

    shockwave::Project m_project;
    bool m_hasProject = false;
    std::map<std::string, EditorCamera> m_sceneCameras;   // project-relative scene path -> view
    std::string m_lastScene;   // project-relative, reopened next time
    std::vector<std::filesystem::path> m_scenes;   // .swscene files in the project
    std::vector<std::string> m_assetFolders;   // folders inside Content/, forward slashes
    double m_sceneScanTimer = 0.0;

    shockwave::World m_world;
    std::filesystem::path m_scenePath;   // empty for a scene that was never saved
    bool m_dirty = false;
    std::vector<shockwave::Entity> m_selection;   // primary last
    shockwave::Entity m_rangeAnchor;
    std::vector<shockwave::Entity> m_hierarchyOrder;   // rows as drawn last frame, for Shift+click
    std::vector<shockwave::Entity> m_hierarchyOrderNext;

    std::unique_ptr<shockwave::AssetFolder> m_assetFolder;
    bool m_retryAssetImports = false;
    std::unordered_map<uint32_t, shockwave::Bounds> m_meshBounds;   // by mesh id
    ContentRoot m_contentRoot = ContentRoot::Assets;
    std::string m_contentFolder;   // inside the root, forward slashes, "" for the top
    std::string m_contentFilter;
    std::string m_selectedContent;
    float m_tileSize = 84.0f;

    // Play: the project's gameplay and physics on the live world; Stop restores the
    // authored scene. Recreated per project.
    std::unique_ptr<shockwave::GameInstance> m_gameInstance;   // for the editor's whole run
    // Game UI: previewed in the viewport while editing, live during Play. Before the
    // session, whose UIWorld uses it.
    std::unique_ptr<shockwave::GameUI> m_gameUI;
    bool m_showGameUI = true;
    bool m_viewportActive = false;   // the viewport has the mouse or keyboard focus
    std::unique_ptr<shockwave::PlaySession> m_session;
    bool m_gameModuleMatches = false;   // the project's game module is loaded, so Play can run it
    bool m_viewThroughCamera = false;   // render from the scene's Camera instead of the editor's
    bool m_viewBeforePlay = false;
    std::filesystem::path m_screenshotPath;   // waiting for the renderer to write it

    shockwave::FlyCamera m_camera;
    bool m_looking = false;

    // Undo history: whole-world snapshots. Scenes are small, and snapshots can't get
    // out of sync the way hand-written inverse operations can. Entity handles survive
    // undo, so the selection does too.
    std::vector<shockwave::World> m_undo, m_redo;
    shockwave::World m_editStart;

    // The inspector edits rotations as Euler angles. Converting the quaternion back every
    // frame would make the numbers jump (several angle sets give one rotation), so the
    // angles being edited are kept while the rotation they produced is unchanged.
    struct EulerCache {
        shockwave::Entity entity;
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 degrees{0.0f};
    } m_euler;

    ImGuizmo::OPERATION m_gizmoOperation = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE m_gizmoMode = ImGuizmo::WORLD;
    bool m_gizmoWasUsing = false;

    shockwave::Entity m_renaming;
    std::string m_renameBuffer;
    bool m_focusRename = false;
    bool m_revealSelection = false;   // open the selection's parents in the hierarchy next frame
    std::string m_hierarchyFilter;
    std::string m_componentFilter;

    bool m_showHierarchy = true, m_showInspector = true, m_showContent = true, m_showConsole = true;
    std::unique_ptr<shockwave::AudioMixer> m_audioPreview;
    shockwave::AudioVoice m_previewVoice = 0;
    std::string m_previewClip;
    bool m_showStats = false, m_showPreferences = false, m_showProjectSettings = false;
    bool m_showControls = false, m_showAbout = false, m_showDemo = false, m_showMetrics = false;
    bool m_resetLayout = false;
    bool m_focusConsole = false;

    // Input window
    bool m_showInput = false;
    shockwave::InputBindings m_inputEdit;
    uint32_t m_inputSeenVersion = 0;
    bool m_inputDirty = false;
    std::string m_inputListenAction;   // the action waiting for an input, empty when not listening
    int m_inputListenFrames = 0;   // frames to ignore after starting, so the click isn't recorded
    std::string m_inputNewAction, m_inputTyped, m_inputTypedError;
    glm::vec3 m_padLight{0.0f, 0.9f, 1.0f};

    std::vector<LogEntry> m_log;
    std::mutex m_pendingLogMutex;
    std::vector<LogEntry> m_pendingLog;   // from any thread, moved into m_log each frame
    int m_logSink = 0;
    bool m_logScrollToBottom = false;
    bool m_logShowInfo = true, m_logShowWarnings = true, m_logShowErrors = true;
    std::string m_logFilter;
    size_t m_warningCount = 0, m_errorCount = 0;

    PendingAction m_pendingAction = PendingAction::None;
    std::string m_pendingPath;
    bool m_openUnsavedModal = false;

    std::filesystem::path m_recoveryPath;   // an autosave newer than the scene, offered on open
    bool m_openRecoveryModal = false;
    double m_autosaveTimer = 0.0;

    // Project hub form
    std::string m_newProjectName = "My Game";
    std::string m_newProjectLocation;
    std::string m_hubError;

    // Project settings form: an edited copy, applied on Save.
    shockwave::Project m_projectEdit;

    // File dialogs answer from another thread.
    enum class DialogKind { OpenScene, SaveScene, OpenProject, NewProjectFolder, Import };
    struct DialogResult {
        DialogKind kind;
        std::vector<std::string> files;
    };
    std::mutex m_dialogMutex;
    std::vector<DialogResult> m_dialogResults;
    std::string m_dialogError;
    std::filesystem::path m_importFolder;

    std::string m_title;
    std::string m_iniPath;   // ImGui layout, next to the preferences
};

}   // namespace editor
