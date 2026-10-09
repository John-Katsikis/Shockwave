#pragma once

#include "Scene.h"

#include <shockwave/Engine.h>

#include <imgui.h>
#include <ImGuizmo.h>

#include <array>
#include <mutex>
#include <string>
#include <vector>

namespace editor {

struct EditorOptions {
    std::string scenePath = "scenes/main.swscene";
    int selectOnStart = -1;   // entity to select at startup (for screenshots and tests)
};

// The Shockwave Editor: a docked Dear ImGui interface around a live viewport.
//
//   Viewport   the scene, rendered by the engine at the panel's size
//   Hierarchy  every entity in the scene
//   Inspector  the selected entity's transform and look
//   Renderer   frame timing, resolution, vsync and camera settings
//   Log        what the editor did (saves, loads, errors)
//
// Everything here runs on the main thread (ImGui is single-threaded); drawing the scene
// fans out across the job system like any game.
class Editor : public shockwave::Game {
public:
    explicit Editor(EditorOptions options);

    void onStart() override;
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
    struct TextureAsset {
        std::string name;
        shockwave::TextureHandle handle;
    };
    // Something that would throw away unsaved changes, waiting on the user's answer.
    enum class PendingAction { None, New, Open, OpenPath, Quit };

    // Assets
    void createAssets();
    const MeshAsset* findMesh(const std::string& name) const;
    shockwave::TextureHandle findTexture(const std::string& name) const;

    // Panels
    void drawMenuBar();
    void drawDockspace();
    void buildDefaultLayout(ImGuiID dockspace);
    void drawViewport();
    void drawViewportToolbar(const ImVec2& pos, const ImVec2& size);
    void drawSelectionOutline(const ImVec2& pos, const ImVec2& size);
    void drawHierarchy();
    void drawInspector();
    void drawRendererPanel();
    void drawLog();
    void drawControlsWindow();
    void drawUnsavedChangesModal();

    // Viewport interaction
    void updateCamera(bool hovered);
    int pick(const ImVec2& mouse, const ImVec2& pos, const ImVec2& size) const;
    glm::mat4 projection(float aspect) const;          // reverse-Z, what the renderer uses
    glm::mat4 gizmoProjection(float aspect) const;     // conventional depth, for ImGuizmo
    void focusSelected();

    // Editing
    void select(int index);
    Entity* selected();
    void addEntity(const char* name, const char* mesh, const char* texture);
    void duplicateSelected();
    void deleteSelected();
    std::string uniqueName(const std::string& base) const;
    void trackEdit();                 // call after a widget that edits the scene
    void pushUndo(Scene before);
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

    EditorOptions m_options;
    Scene m_scene;
    std::string m_scenePath;
    bool m_dirty = false;
    int m_selected = -1;

    std::vector<MeshAsset> m_meshes;
    std::vector<TextureAsset> m_textures;

    shockwave::FlyCamera m_camera;
    bool m_looking = false;

    // Undo history: whole-scene snapshots. Scenes are small, and snapshots can't get
    // out of sync the way hand-written inverse operations can.
    std::vector<Scene> m_undo, m_redo;
    Scene m_editStart;

    ImGuizmo::OPERATION m_gizmoOperation = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE m_gizmoMode = ImGuizmo::WORLD;
    bool m_snap = false;
    bool m_gizmoWasUsing = false;

    bool m_showHierarchy = true, m_showInspector = true, m_showRenderer = true, m_showLog = true;
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
