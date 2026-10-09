#pragma once

#include "Scene.h"

#include <shockwave/AssetFolder.h>
#include <shockwave/Engine.h>

#include <imgui.h>
#include <ImGuizmo.h>

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace editor {

// Drag and drop payloads between panels. Meshes and textures travel by asset name.
inline constexpr const char* kEntityPayload = "SW_ENTITY";
inline constexpr const char* kMeshPayload = "SW_MESH";
inline constexpr const char* kTexturePayload = "SW_TEXTURE";

struct EditorOptions {
    std::string scenePath = "scenes/main.swscene";
    std::string assetFolder = "assets";   // imported into the asset browser, watched for changes
    int selectOnStart = -1;   // entity to select at startup, in hierarchy order (for screenshots and tests)
    bool playOnStart = false; // press Play straight away (for tests)
};

// The Shockwave Editor: a docked Dear ImGui interface around a live viewport.
//
//   Viewport   the scene, rendered by the engine at the panel's size, with transform and snapping tools
//   Hierarchy  every entity in the scene as a tree; drag one onto another to attach it
//   Inspector  the selected entity's components, generated from their registered fields
//   Assets     meshes and textures, with the project's assets/ folder imported and
//              watched; drag them onto the viewport or the inspector
//   Renderer   frame timing, resolution, vsync, camera settings and the system schedule
//   Log        what the editor did (saves, loads, errors)
//
// The scene is an engine World, the same thing a game runs. Play copies it and runs the
// game's systems on the copy each tick; Stop throws the copy away.
//
// The UI runs on the main thread (ImGui is single-threaded); systems and drawing fan out
// across the job system like any game.
class Editor : public shockwave::Game {
public:
    explicit Editor(EditorOptions options);

    void onStart() override;
    void onUpdate(double dt) override;
    void onGui() override;
    void onRender(shockwave::Renderer& renderer, double alpha) override;
    bool onQuitRequested() override;

private:
    enum class Shape { Box, Sphere, Plane };
    struct MeshAsset {
        std::string name;
        shockwave::MeshHandle handle;
        Shape shape;
    };
    // Something that would throw away unsaved changes, waiting on the user's answer.
    enum class PendingAction { None, New, Open, OpenPath, Quit };
    enum class PlayState { Editing, Playing, Paused };

    // Assets
    void createAssets();
    const MeshAsset* findMesh(shockwave::MeshHandle handle) const;

    // Panels
    void drawMenuBar();
    void drawCommandBar();
    void drawDockspace();
    void buildDefaultLayout(ImGuiID dockspace);
    void drawViewport();
    void drawViewportToolbar(const ImVec2& pos, const ImVec2& size);
    void drawSelectionOutline(const ImVec2& pos, const ImVec2& size);
    void drawOutline(shockwave::Entity e, const ImVec2& pos, const ImVec2& size, ImU32 color);
    void drawHierarchy();
    using ChildMap = std::unordered_map<uint32_t, std::vector<shockwave::Entity>>;
    void drawHierarchyNode(shockwave::Entity e, const ChildMap& children, int depth);
    bool drawHierarchyRow(shockwave::Entity e, bool hasChildren);   // true if open
    void drawAssetBrowser();
    void drawAssetTiles(const char* section, int kind);
    void drawInspector();
    void drawComponent(const shockwave::ComponentInfo& info, shockwave::Entity e, bool& removed);
    void drawField(const shockwave::Field& field, void* data, shockwave::Entity e);
    void drawEntityField(const std::string& label, shockwave::Entity* value, shockwave::Entity owner, bool isParent);
    void drawRendererPanel();
    void drawLog();
    void drawControlsWindow();
    void drawUnsavedChangesModal();

    // Viewport interaction
    void updateCamera(bool hovered);
    shockwave::Entity pick(const ImVec2& mouse, const ImVec2& pos, const ImVec2& size);
    void mouseRay(const ImVec2& mouse, const ImVec2& pos, const ImVec2& size, glm::vec3& origin, glm::vec3& dir) const;
    void acceptViewportDrop(const ImVec2& pos, const ImVec2& size);
    glm::mat4 projection(float aspect) const;          // reverse-Z, what the renderer uses
    glm::mat4 gizmoProjection(float aspect) const;     // conventional depth, for ImGuizmo
    void focusSelected();

    // Play mode
    void play();
    void pause();
    void stop();
    bool playing() const { return m_playState != PlayState::Editing; }

    // Editing
    void select(shockwave::Entity e);
    shockwave::Entity selected() const;   // kNullEntity if nothing (alive) is selected
    std::string displayName(shockwave::Entity e) const;
    void addEntity(const char* name, const char* mesh, const char* texture);
    void addEmptyEntity();
    void addChild(const char* name, const char* mesh, const char* texture);
    shockwave::Entity spawnMesh(const std::string& mesh, const glm::vec3& position);
    bool reparent(shockwave::Entity child, shockwave::Entity parent);
    bool applyTexture(shockwave::Entity e, const std::string& texture);
    void duplicateSelected();
    void deleteSelected();
    std::string uniqueName(const std::string& base) const;
    void trackEdit();                 // call after a widget that edits the scene
    void pushUndo(shockwave::World before);
    void undo();
    void redo();
    void handleShortcuts();

    // Files
    void newScene();
    bool openScene(const std::string& path);
    bool saveScene();
    void showOpenDialog();
    void showSaveDialog();
    void handleDialogResults();
    void requestAction(PendingAction action, const std::string& path = {});
    void runAction(PendingAction action, const std::string& path);
    void updateTitle();

    void log(const std::string& message);
    void logAssetChanges(const std::vector<shockwave::AssetFolder::Change>& changes);

    EditorOptions m_options;
    shockwave::World m_world;
    std::string m_scenePath;
    bool m_dirty = false;
    shockwave::Entity m_selected;

    std::vector<MeshAsset> m_meshes;
    std::unique_ptr<shockwave::AssetFolder> m_assetFolder;
    std::string m_assetFilter;
    std::string m_selectedAsset;
    float m_tileSize = 84.0f;

    shockwave::Schedule m_schedule;
    PlayState m_playState = PlayState::Editing;
    shockwave::World m_editWorld;   // the scene as it was when Play was pressed

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
    bool m_snap = false;
    bool m_gizmoWasUsing = false;

    bool m_showHierarchy = true, m_showInspector = true, m_showRenderer = true, m_showLog = true, m_showAssets = true;
    bool m_revealSelection = false;   // open the selection's parents in the hierarchy next frame
    bool m_checkedAssetsDock = false;
    bool m_showControls = false, m_showDemo = false;
    bool m_resetLayout = false;
    std::string m_hierarchyFilter;

    std::array<float, 240> m_frameTimes{};
    size_t m_frameTimeIndex = 0;

    std::vector<std::string> m_log;
    bool m_logScrollToBottom = false;

    PendingAction m_pendingAction = PendingAction::None;
    std::string m_pendingPath;
    bool m_openUnsavedModal = false;

    // File dialogs answer from another thread.
    std::mutex m_dialogMutex;
    std::string m_dialogOpenResult, m_dialogSaveResult, m_dialogError;

    std::string m_title;
};

} // namespace editor
