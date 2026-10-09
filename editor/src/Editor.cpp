#include "Editor.h"

#include <shockwave/Gui.h>

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui_internal.h>   // DockBuilder, for the default layout
#include <imgui_stdlib.h>     // InputText with std::string

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>

namespace editor {

namespace gui = shockwave::gui;
using shockwave::ComponentInfo;
using shockwave::Entity;
using shockwave::Field;
using shockwave::FieldType;
using shockwave::kNullEntity;
using shockwave::MeshRenderer;
using shockwave::Name;
using shockwave::Parent;
using shockwave::Transform;

namespace {

// ---- Built-in textures, generated until asset import lands ----------------------------

std::vector<uint8_t> makeChecker(uint32_t size, uint32_t cells, glm::vec3 a, glm::vec3 b) {
    std::vector<uint8_t> pixels(size_t(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const bool odd = ((x * cells / size) + (y * cells / size)) % 2;
            const glm::vec3 c = odd ? a : b;
            uint8_t* p = &pixels[(size_t(y) * size + x) * 4];
            p[0] = uint8_t(c.r * 255.0f);
            p[1] = uint8_t(c.g * 255.0f);
            p[2] = uint8_t(c.b * 255.0f);
            p[3] = 255;
        }
    }
    return pixels;
}

std::vector<uint8_t> makePanel(uint32_t size) {
    std::vector<uint8_t> pixels(size_t(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            const float edge = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
            const float border = edge < 0.06f ? 0.35f : 1.0f;
            const float rivet = (std::abs(u - 0.15f) < 0.03f || std::abs(u - 0.85f) < 0.03f) &&
                                        (std::abs(v - 0.15f) < 0.03f || std::abs(v - 0.85f) < 0.03f)
                                    ? 0.5f
                                    : 1.0f;
            const float shade = 0.85f + 0.15f * std::sin(u * 40.0f) * std::sin(v * 3.0f);
            const uint8_t g = uint8_t(255.0f * border * rivet * shade);
            uint8_t* p = &pixels[(size_t(y) * size + x) * 4];
            p[0] = p[1] = p[2] = g;
            p[3] = 255;
        }
    }
    return pixels;
}

// One-unit tile with a bright edge and faint quarter lines: a measuring grid when the
// floor repeats it once per unit.
std::vector<uint8_t> makeGrid(uint32_t size) {
    std::vector<uint8_t> pixels(size_t(size) * size * 4);
    const uint32_t quarter = size / 4;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float g = 0.34f;
            if (x % quarter < 1 || y % quarter < 1) g = 0.40f;
            if (x < 2 || y < 2 || x >= size - 1 || y >= size - 1) g = 0.55f;
            uint8_t* p = &pixels[(size_t(y) * size + x) * 4];
            p[0] = uint8_t(g * 255.0f);
            p[1] = uint8_t(g * 1.02f * 255.0f);
            p[2] = uint8_t(g * 1.06f * 255.0f);
            p[3] = 255;
        }
    }
    return pixels;
}

// ---- Ray casts in an entity's local space ----------------------------------------------

bool rayBox(const glm::vec3& o, const glm::vec3& d, const glm::vec3& lo, const glm::vec3& hi, float& t) {
    float tmin = -std::numeric_limits<float>::max();
    float tmax = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-8f) {
            if (o[i] < lo[i] || o[i] > hi[i]) return false;
            continue;
        }
        float t1 = (lo[i] - o[i]) / d[i], t2 = (hi[i] - o[i]) / d[i];
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
    }
    if (tmax < std::max(tmin, 0.0f)) return false;
    t = tmin > 0.0f ? tmin : tmax;
    return true;
}

bool raySphere(const glm::vec3& o, const glm::vec3& d, float radius, float& t) {
    const float a = glm::dot(d, d);
    const float b = 2.0f * glm::dot(o, d);
    const float c = glm::dot(o, o) - radius * radius;
    const float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) return false;
    const float s = std::sqrt(disc);
    const float t0 = (-b - s) / (2.0f * a), t1 = (-b + s) / (2.0f * a);
    if (t1 < 0.0f) return false;
    t = t0 > 0.0f ? t0 : t1;
    return true;
}

std::string fileName(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

const SDL_DialogFileFilter kSceneFilter[] = {{"Shockwave scenes", "swscene"}};

// "sky_color" -> "Sky color"
std::string prettyName(const std::string& name) {
    std::string s = name;
    for (char& c : s) {
        if (c == '_') c = ' ';
    }
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

} // namespace

Editor::Editor(EditorOptions options) : m_options(std::move(options)) {}

// ---- Startup ------------------------------------------------------------------------------

void Editor::onStart() {
    ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;   // dragging in the viewport is for the scene
    // ImGuizmo's colors are sRGB like ImGui's; convert them for the sRGB swapchain too.
    for (ImVec4& c : ImGuizmo::GetStyle().Colors) c = gui::color(c.x, c.y, c.z, c.w);
    createAssets();
    // Import the project's assets before the scene, so the scene finds them by name.
    m_assetFolder = std::make_unique<shockwave::AssetFolder>(engine().renderer(), engine().assets(), engine().jobs());
    logAssetChanges(m_assetFolder->open(m_options.assetFolder));
    shockwave::addBuiltinSystems(m_schedule);
    shockwave::FlyCamera::bindDefaultControls(engine().input());

    if (std::filesystem::exists(m_options.scenePath)) {
        if (!openScene(m_options.scenePath)) newScene();
    } else {
        // First run: start from the sample scene and write it out, so there's a file to edit.
        makeDefaultScene(m_world, engine().assets());
        m_scenePath = m_options.scenePath;
        const EditorCamera camera;
        m_camera.position = camera.position;
        m_camera.yaw = camera.yaw;
        m_camera.pitch = camera.pitch;
        if (saveScene()) log("Created " + m_scenePath + " from the sample scene");
    }
    if (m_options.selectOnStart >= 0) select(m_world.entityAt(static_cast<size_t>(m_options.selectOnStart)));
    if (m_options.playOnStart) play();
    log("Hold the right mouse button in the viewport to fly; Help > Controls lists the shortcuts");
}

// Built-in meshes and textures, registered by name in the engine's Assets so scene files
// and the inspector can refer to them.
void Editor::createAssets() {
    shockwave::Renderer& renderer = engine().renderer();
    shockwave::Assets& assets = engine().assets();
    m_meshes = {
        {"cube", renderer.createMesh(shockwave::makeCube(1.0f)), Shape::Box},
        {"sphere", renderer.createMesh(shockwave::makeSphere(0.5f)), Shape::Sphere},
        {"plane", renderer.createMesh(shockwave::makePlane(1.0f, 1.0f)), Shape::Plane},
        // A plane that tiles its texture 40 times: one tile per unit at the default 40 x 40.
        {"floor", renderer.createMesh(shockwave::makePlane(1.0f, 40.0f)), Shape::Plane},
    };
    for (const MeshAsset& m : m_meshes) assets.addMesh(m.name, m.handle);
    const auto checker = makeChecker(256, 8, {0.80f, 0.80f, 0.82f}, {0.55f, 0.56f, 0.6f});
    const auto panel = makePanel(256);
    const auto grid = makeGrid(256);
    assets.addTexture("checker", renderer.createTexture(256, 256, checker));
    assets.addTexture("panel", renderer.createTexture(256, 256, panel));
    assets.addTexture("grid", renderer.createTexture(256, 256, grid));
}

const Editor::MeshAsset* Editor::findMesh(shockwave::MeshHandle handle) const {
    for (const MeshAsset& m : m_meshes) {
        if (m.handle.id == handle.id) return &m;
    }
    return nullptr;
}

// ---- Per frame ----------------------------------------------------------------------------

void Editor::onGui() {
    ImGuizmo::BeginFrame();
    m_frameTimes[m_frameTimeIndex] = ImGui::GetIO().DeltaTime * 1000.0f;
    m_frameTimeIndex = (m_frameTimeIndex + 1) % m_frameTimes.size();

    handleDialogResults();
    logAssetChanges(m_assetFolder->poll(ImGui::GetIO().DeltaTime));
    handleShortcuts();
    drawMenuBar();
    drawCommandBar();
    drawDockspace();
    drawViewport();
    if (m_showHierarchy) drawHierarchy();
    if (m_showInspector) drawInspector();
    if (m_showRenderer) drawRendererPanel();
    if (m_showLog) drawLog();
    if (m_showAssets) drawAssetBrowser();
    if (m_showControls) drawControlsWindow();
    if (m_showDemo) ImGui::ShowDemoWindow(&m_showDemo);
    drawUnsavedChangesModal();
    updateTitle();
}

// Play mode runs the game's systems at the fixed tick rate, like a shipped game would.
void Editor::onUpdate(double dt) {
    if (m_playState != PlayState::Playing) return;
    shockwave::storePreviousTransforms(m_world, engine().jobs());
    m_schedule.run(m_world, engine().jobs(), dt);
}

void Editor::onRender(shockwave::Renderer& renderer, double alpha) {
    renderer.setClearColor({0.0f, 0.0f, 0.0f, 1.0f});   // a scene without an Environment
    renderer.setCamera(m_camera.view(), projection(renderer.aspectRatio()));
    // While editing (or paused) nothing moves on its own, so draw exactly what's there.
    shockwave::renderWorld(m_world, renderer, engine().jobs(), m_playState == PlayState::Playing ? alpha : 1.0);
}

bool Editor::onQuitRequested() {
    if (!m_dirty) return true;
    requestAction(PendingAction::Quit);
    return false;
}

// ---- Layout -------------------------------------------------------------------------------

void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New scene", "Ctrl+N")) requestAction(PendingAction::New);
        if (ImGui::MenuItem("Open...", "Ctrl+O")) requestAction(PendingAction::Open);
        if (ImGui::MenuItem("Save", "Ctrl+S")) saveScene();
        if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S")) showSaveDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) requestAction(PendingAction::Quit);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const bool hasSelection = selected().valid();
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !m_undo.empty() && !playing())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !m_redo.empty() && !playing())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSelection)) duplicateSelected();
        if (ImGui::MenuItem("Delete", "Del", false, hasSelection)) deleteSelected();
        if (ImGui::MenuItem("Focus", "F", false, hasSelection)) focusSelected();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
        if (ImGui::MenuItem("Cube")) addEntity("Cube", "cube", "panel");
        if (ImGui::MenuItem("Sphere")) addEntity("Sphere", "sphere", "checker");
        if (ImGui::MenuItem("Plane")) addEntity("Plane", "plane", "grid");
        ImGui::Separator();
        if (ImGui::MenuItem("Empty entity")) addEmptyEntity();
        if (ImGui::BeginMenu("Child of selection", selected().valid())) {
            if (ImGui::MenuItem("Cube")) addChild("Cube", "cube", "panel");
            if (ImGui::MenuItem("Sphere")) addChild("Sphere", "sphere", "checker");
            if (ImGui::MenuItem("Empty entity")) addChild("Entity", nullptr, nullptr);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Play")) {
        if (ImGui::MenuItem(m_playState == PlayState::Playing ? "Stop" : "Play", "Ctrl+P")) {
            m_playState == PlayState::Playing ? stop() : play();
        }
        if (ImGui::MenuItem("Pause", "Ctrl+Shift+P", m_playState == PlayState::Paused, playing())) pause();
        if (ImGui::MenuItem("Stop", nullptr, false, playing())) stop();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Hierarchy", nullptr, &m_showHierarchy);
        ImGui::MenuItem("Inspector", nullptr, &m_showInspector);
        ImGui::MenuItem("Renderer", nullptr, &m_showRenderer);
        ImGui::MenuItem("Assets", nullptr, &m_showAssets);
        ImGui::MenuItem("Log", nullptr, &m_showLog);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) m_resetLayout = true;
        ImGui::MenuItem("Dear ImGui demo", nullptr, &m_showDemo);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("Controls", "F1", &m_showControls);
        ImGui::EndMenu();
    }

    // Frame rate on the right of the menu bar.
    char fps[64];
    std::snprintf(fps, sizeof(fps), "%.0f fps", engine().fps());
    const float width = ImGui::CalcTextSize(fps).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - width - ImGui::GetStyle().WindowPadding.x * 2.0f);
    ImGui::TextDisabled("%s", fps);
    ImGui::EndMainMenuBar();
}

void Editor::drawCommandBar() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginViewportSideBar("##WorkspaceCommands", ImGui::GetMainViewport(), ImGuiDir_Up, height, flags)) {
        const char* playLabel = m_playState == PlayState::Paused ? "Resume" : "Play";
        const float transportWidth = gui::controlGroupWidth({playLabel, "Pause", "Stop"});
        const float centerX = (ImGui::GetWindowWidth() - transportWidth) * 0.5f;
        const float rowY = ImGui::GetCursorPosY();
        // Scene actions have their own scope; the transport always occupies the center.
        ImGui::AlignTextToFramePadding();
        if (centerX > ImGui::CalcTextSize("SCENE").x + style.WindowPadding.x + style.ItemSpacing.x)
            ImGui::TextDisabled("SCENE");
        else
            ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
        const std::string scene = fileName(m_scenePath) + (m_dirty ? " *" : "");
        const float nameWidth = ImGui::CalcTextSize(scene.c_str()).x;
        if (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + nameWidth + gui::buttonWidth("Save") + style.ItemSpacing.x * 3.0f < centerX) {
            ImGui::SameLine();
            ImGui::TextUnformatted(scene.c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(playing() || !m_dirty);
            if (gui::button("Save")) saveScene();
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Save scene (Ctrl+S)");
        }
        ImGui::SetCursorPos(ImVec2(std::max(centerX, style.WindowPadding.x), rowY));
        if (gui::toolButton(playLabel, playing(), "Play / stop; resume when paused (Ctrl+P)")) {
            if (m_playState == PlayState::Playing) stop();
            else play();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!playing());
        if (gui::toolButton("Pause", m_playState == PlayState::Paused, "Pause / resume (Ctrl+Shift+P)")) pause();
        ImGui::SameLine();
        if (gui::toolButton("Stop", false, "Stop and restore the scene before Play (Ctrl+P)")) stop();
        ImGui::EndDisabled();
        const char* state = m_playState == PlayState::Playing ? "SIMULATION RUNNING" : m_playState == PlayState::Paused ? "SIMULATION PAUSED" : "EDIT MODE";
        const float stateWidth = ImGui::CalcTextSize(state).x;
        const float right = ImGui::GetWindowWidth() - style.WindowPadding.x;
        if (right - stateWidth > centerX + transportWidth + style.ItemSpacing.x * 2.0f) {
            ImGui::SetCursorPos(ImVec2(right - stateWidth, rowY));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(playing() ? gui::semanticColor(m_playState == PlayState::Paused ? gui::currentTheme().warning : gui::currentTheme().accent) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", state);
        }
    }
    ImGui::End();
}

void Editor::drawDockspace() {
    const ImGuiID dockspace = ImGui::GetID("ShockwaveDockspace");
    if (m_resetLayout || !ImGui::DockBuilderGetNode(dockspace)) {
        buildDefaultLayout(dockspace);
        m_showHierarchy = m_showInspector = m_showRenderer = m_showLog = m_showAssets = true;
        m_resetLayout = false;
        m_checkedAssetsDock = true;
    }
    // Layouts saved before the asset browser existed: give it a tab next to the Log
    // instead of letting it float over the viewport.
    if (!m_checkedAssetsDock) {
        m_checkedAssetsDock = true;
        const ImGuiWindowSettings* log = ImGui::FindWindowSettingsByID(ImHashStr("Log"));
        if (!ImGui::FindWindowSettingsByID(ImHashStr("Assets")) && log && log->DockId) {
            ImGui::DockBuilderDockWindow("Assets", log->DockId);
        }
    }
    ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport());
}

void Editor::buildDefaultLayout(ImGuiID dockspace) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);

    ImGuiID center = dockspace;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    const ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Renderer", rightBottom);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    ImGui::DockBuilderDockWindow("Log", bottom);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(dockspace);
}

// ---- Viewport -----------------------------------------------------------------------------

glm::mat4 Editor::projection(float aspect) const {
    return m_camera.projection(aspect);
}

glm::mat4 Editor::gizmoProjection(float aspect) const {
    // ImGuizmo only needs to place handles on screen; it expects a conventional
    // finite projection rather than reverse-Z with an infinite far plane.
    return glm::perspective(m_camera.fovY, aspect, m_camera.nearPlane, 2000.0f);
}

void Editor::drawViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }

    shockwave::Renderer& renderer = engine().renderer();
    const ImGuiIO& io = ImGui::GetIO();
    const float toolbarWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float innerWidth = std::max(toolbarWidth - style.WindowPadding.x * 2.0f - style.ChildBorderSize * 2.0f, 1.0f);
    const char* axes = m_gizmoMode == ImGuizmo::WORLD ? "World" : "Local";
    const float groupGap = style.ItemSpacing.x * 3.0f;
    const bool oneRow = gui::controlGroupWidth({"Move", "Rotate", "Scale"}) + groupGap + gui::controlGroupWidth({axes, "Snap"}) <= innerWidth;
    const int rows = oneRow ? 1 : gui::toolbarRows(innerWidth, {"Move", "Rotate", "Scale"}) + gui::toolbarRows(innerWidth, {axes, "Snap"});
    const float toolbarHeight = rows * ImGui::GetFrameHeight() + (rows - 1) * style.ItemSpacing.y + style.WindowPadding.y * 2.0f + style.ChildBorderSize * 2.0f;
    ImGui::BeginChild("ViewportToolbar", ImVec2(0, toolbarHeight), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    drawViewportToolbar(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail());
    ImGui::EndChild();

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 1.0f), std::max(ImGui::GetContentRegionAvail().y, 1.0f));

    // The engine renders the scene at exactly this panel's size, so it stays sharp and
    // undistorted however the panel is docked or resized.
    renderer.setViewport(static_cast<uint32_t>(size.x * io.DisplayFramebufferScale.x),
                         static_cast<uint32_t>(size.y * io.DisplayFramebufferScale.y));
    if (renderer.sceneTexture()) ImGui::Image(static_cast<ImTextureID>(renderer.sceneTexture()), size);
    const bool imageHovered = ImGui::IsItemHovered();
    acceptViewportDrop(pos, size);

    updateCamera(imageHovered);
    drawSelectionOutline(pos, size);

    // View cube in the top-right corner: click a face to snap the camera to that axis.
    const float aspect = size.x / size.y;
    glm::mat4 view = m_camera.view();
    const glm::mat4 before = view;
    const float cubeSize = 96.0f * ImGui::GetStyle().FontScaleDpi;
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(pos.x, pos.y, size.x, size.y);
    ImGuizmo::SetGizmoSizeClipSpace(0.14f);
    ImGuizmo::ViewManipulate(&view[0][0], 8.0f, ImVec2(pos.x + size.x - cubeSize - 8.0f, pos.y + 8.0f),
                             ImVec2(cubeSize, cubeSize), IM_COL32(0, 0, 0, 0));
    if (std::memcmp(&view, &before, sizeof(view)) != 0) {
        const glm::mat4 inv = glm::inverse(view);
        const glm::vec3 forward = -glm::vec3(inv[2]);
        m_camera.position = glm::vec3(inv[3]);
        m_camera.pitch = std::asin(std::clamp(forward.y, -0.999f, 0.999f));
        m_camera.yaw = std::atan2(-forward.x, -forward.z);
    }

    // Transform gizmo on the selected entity.
    if (Transform* t = m_world.get<Transform>(selected())) {
        view = m_camera.view();
        const glm::mat4 proj = gizmoProjection(aspect);
        // The gizmo works in world space; a child's Transform is relative to its parent.
        const Entity parent = shockwave::parentOf(m_world, selected());
        const glm::mat4 parentWorld = parent.valid() ? shockwave::worldMatrix(m_world, parent) : glm::mat4(1.0f);
        glm::mat4 model = parentWorld * t->matrix();
        const bool snap = m_snap != io.KeyCtrl;   // Ctrl flips snapping while held
        const float snapTranslate[3] = {0.5f, 0.5f, 0.5f};
        const float snapRotate[3] = {15.0f, 15.0f, 15.0f};
        const float snapScale[3] = {0.1f, 0.1f, 0.1f};
        const float* snapValues = m_gizmoOperation == ImGuizmo::ROTATE ? snapRotate
                                  : m_gizmoOperation == ImGuizmo::SCALE ? snapScale
                                                                        : snapTranslate;
        const bool changed = ImGuizmo::Manipulate(&view[0][0], &proj[0][0], m_gizmoOperation, m_gizmoMode,
                                                  &model[0][0], nullptr, snap ? snapValues : nullptr);
        const bool using_ = ImGuizmo::IsUsing();
        if (using_ && !m_gizmoWasUsing) m_editStart = m_world;
        if (changed) {
            // Take only the part being edited, so the others don't pick up float drift.
            const Transform edited = Transform::fromMatrix(glm::inverse(parentWorld) * model);
            switch (m_gizmoOperation) {
            case ImGuizmo::ROTATE: t->rotation = edited.rotation; break;
            case ImGuizmo::SCALE: t->scale = edited.scale; break;
            default: t->position = edited.position; break;
            }
        }
        if (!using_ && m_gizmoWasUsing) pushUndo(m_editStart);
        m_gizmoWasUsing = using_;
    } else {
        m_gizmoWasUsing = false;
    }


    // Click to select. Clicks on the toolbar, gizmo or view cube don't count.
    if (imageHovered && !m_looking && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered() &&
        !ImGuizmo::IsOver() && !ImGuizmo::IsUsingAny() && !ImGuizmo::IsViewManipulateHovered()) {
        select(pick(io.MousePos, pos, size));
    }

    // Status line in the bottom-left corner.
    char status[192];
    std::snprintf(status, sizeof(status), "%s%ux%u  |  %zu entities  |  %u draws  |  speed %.1f",
                  m_playState == PlayState::Playing ? "PLAYING  |  " : m_playState == PlayState::Paused ? "PAUSED  |  " : "",
                  renderer.renderWidth(), renderer.renderHeight(), m_world.entityCount(), renderer.drawCount(),
                  m_camera.moveSpeed);
    if (ImGui::CalcTextSize(status).x > size.x - 20.0f) {
        std::snprintf(status, sizeof(status), "%s%ux%u",
                      playing() ? (m_playState == PlayState::Playing ? "PLAY | " : "PAUSE | ") : "",
                      renderer.renderWidth(), renderer.renderHeight());
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // A coloured frame round the viewport while playing, so play-mode edits (which are
    // thrown away on Stop) are hard to mistake for real ones.
    if (playing()) {
        draw->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                      m_playState == PlayState::Playing ? gui::semanticColorU32(gui::currentTheme().accent) : gui::semanticColorU32(gui::currentTheme().warning),
                      0.0f, 0, 3.0f);
    }
    const ImVec2 textPos(pos.x + 10.0f, pos.y + size.y - ImGui::GetTextLineHeight() - 8.0f);
    draw->AddRectFilled(ImVec2(pos.x, textPos.y - 4.0f), ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(ImGuiCol_WindowBg));
    draw->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), status);

    ImGui::End();
}

void Editor::drawViewportToolbar(const ImVec2&, const ImVec2& size) {
    const char* axes = m_gizmoMode == ImGuizmo::WORLD ? "World" : "Local";
    const float transformWidth = gui::controlGroupWidth({"Move", "Rotate", "Scale"});
    const float settingsWidth = gui::controlGroupWidth({axes, "Snap"});
    const bool oneRow = transformWidth + ImGui::GetStyle().ItemSpacing.x * 3.0f + settingsWidth <= size.x;
    const float rowY = ImGui::GetCursorPosY();
    if (gui::toolButton("Move", m_gizmoOperation == ImGuizmo::TRANSLATE, "Move (W)")) m_gizmoOperation = ImGuizmo::TRANSLATE;
    gui::toolbarNext(gui::buttonWidth("Rotate"));
    if (gui::toolButton("Rotate", m_gizmoOperation == ImGuizmo::ROTATE, "Rotate (E)")) m_gizmoOperation = ImGuizmo::ROTATE;
    gui::toolbarNext(gui::buttonWidth("Scale"));
    if (gui::toolButton("Scale", m_gizmoOperation == ImGuizmo::SCALE, "Scale (R)")) m_gizmoOperation = ImGuizmo::SCALE;
    if (oneRow) {
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - settingsWidth, rowY));
    } else {
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
    }
    if (gui::toolButton(axes, false, "World or local axes (T)"))
        m_gizmoMode = m_gizmoMode == ImGuizmo::WORLD ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    gui::toolbarNext(gui::buttonWidth("Snap"));
    if (gui::toolButton("Snap", m_snap, "Snap: 0.5 units, 15 degrees, 0.1 scale; Ctrl flips")) m_snap = !m_snap;
}

void Editor::drawSelectionOutline(const ImVec2& pos, const ImVec2& size) {
    drawOutline(selected(), pos, size, gui::semanticColorU32(gui::currentTheme().accent));
}

// A wireframe box round an entity's mesh.
void Editor::drawOutline(Entity e, const ImVec2& pos, const ImVec2& size, ImU32 color) {
    const Transform* t = m_world.get<Transform>(e);
    const MeshRenderer* renderer = m_world.get<MeshRenderer>(e);
    if (!t || !renderer || !renderer->visible) return;
    const MeshAsset* mesh = findMesh(renderer->mesh);
    if (!mesh) return;

    const glm::vec3 lo = mesh->shape == Shape::Plane ? glm::vec3(-0.5f, 0.0f, -0.5f) : glm::vec3(-0.5f);
    const glm::vec3 hi = mesh->shape == Shape::Plane ? glm::vec3(0.5f, 0.0f, 0.5f) : glm::vec3(0.5f);
    const glm::mat4 mvp = projection(size.x / size.y) * m_camera.view() * shockwave::worldMatrix(m_world, e);

    glm::vec4 clip[8];
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        clip[i] = mvp * glm::vec4(corner, 1.0f);
    }
    auto toScreen = [&](const glm::vec4& c) {
        return ImVec2(pos.x + (c.x / c.w * 0.5f + 0.5f) * size.x, pos.y + (0.5f - c.y / c.w * 0.5f) * size.y);
    };

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float nearW = m_camera.nearPlane;
    for (int i = 0; i < 8; ++i) {
        for (int bit = 1; bit < 8; bit <<= 1) {
            const int j = i | bit;
            if (j == i) continue;
            if (clip[i].w <= nearW || clip[j].w <= nearW) continue;   // behind the camera
            draw->AddLine(toScreen(clip[i]), toScreen(clip[j]), color, 1.5f);
        }
    }
}

// Assets dropped on the viewport: a mesh lands on the ground under the cursor, a texture
// goes onto whatever is under the cursor (outlined while you aim).
void Editor::acceptViewportDrop(const ImVec2& pos, const ImVec2& size) {
    if (!ImGui::BeginDragDropTarget()) return;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMeshPayload)) {
        const std::string mesh = static_cast<const char*>(payload->Data);
        glm::vec3 origin, dir;
        mouseRay(mouse, pos, size, origin, dir);
        glm::vec3 at = origin + dir * 6.0f;
        if (dir.y < -1e-4f) {
            const float t = -origin.y / dir.y;   // where the ray meets the ground (y = 0)
            if (t > 0.0f && t < 200.0f) at = origin + dir * t;
        }
        const MeshAsset* asset = findMesh(engine().assets().findMesh(mesh));
        at.y = asset && asset->shape == Shape::Plane ? 0.01f : 0.5f;
        spawnMesh(mesh, at);
    }
    if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(kTexturePayload, ImGuiDragDropFlags_AcceptBeforeDelivery |
                                                              ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
        const Entity target = pick(mouse, pos, size);
        if (payload->IsDelivery()) {
            applyTexture(target, static_cast<const char*>(payload->Data));
        } else {
            drawOutline(target, pos, size, ImGui::ColorConvertFloat4ToU32(gui::accent()));
        }
    }
    ImGui::EndDragDropTarget();
}

void Editor::updateCamera(bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    shockwave::Input& input = engine().input();

    // Unreal-style: hold the right mouse button in the viewport to fly.
    if (!m_looking && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_looking = true;
        ImGui::SetWindowFocus();   // so WASD doesn't also type into a text field
    }
    if (m_looking && !input.isMouseDown(shockwave::MouseButton::Right)) m_looking = false;

    if (m_looking) {
        m_camera.update(input, io.DeltaTime);   // captures the mouse while the button is held
    } else {
        input.setMouseCaptured(false);
        if (hovered && io.MouseWheel != 0.0f) {
            m_camera.position += m_camera.forward() * io.MouseWheel * std::max(0.5f, m_camera.moveSpeed * 0.15f);
        }
        if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
            const glm::vec3 right = m_camera.right();
            const glm::vec3 up = glm::cross(right, m_camera.forward());
            const float k = 0.0025f * std::max(1.0f, m_camera.moveSpeed);
            m_camera.position += (-right * io.MouseDelta.x + up * io.MouseDelta.y) * k;
        }
    }

    // While flying, ImGui stays out of the way of the mouse and keyboard.
    const ImGuiConfigFlags flyFlags =
        ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
    if (m_looking) {
        io.ConfigFlags |= flyFlags;
    } else {
        io.ConfigFlags &= ~flyFlags;
    }
}

void Editor::mouseRay(const ImVec2& mouse, const ImVec2& pos, const ImVec2& size, glm::vec3& origin,
                      glm::vec3& dir) const {
    const glm::vec2 ndc(2.0f * (mouse.x - pos.x) / size.x - 1.0f, 1.0f - 2.0f * (mouse.y - pos.y) / size.y);
    const glm::mat4 invViewProj = glm::inverse(projection(size.x / size.y) * m_camera.view());
    glm::vec4 nearPoint = invViewProj * glm::vec4(ndc, 1.0f, 1.0f);   // reverse-Z: depth 1 is the near plane
    nearPoint /= nearPoint.w;
    origin = m_camera.position;
    dir = glm::normalize(glm::vec3(nearPoint) - origin);
}

Entity Editor::pick(const ImVec2& mouse, const ImVec2& pos, const ImVec2& size) {
    glm::vec3 origin, dir;
    mouseRay(mouse, pos, size, origin, dir);

    Entity best = kNullEntity;
    float bestDistance = std::numeric_limits<float>::max();
    m_world.each<const Transform, const MeshRenderer>([&](Entity e, const Transform&, const MeshRenderer& r) {
        const MeshAsset* mesh = findMesh(r.mesh);
        if (!r.visible || !mesh) return;
        const glm::mat4 model = shockwave::worldMatrix(m_world, e);
        const glm::mat4 inv = glm::inverse(model);
        const glm::vec3 lo = glm::vec3(inv * glm::vec4(origin, 1.0f));
        const glm::vec3 ld = glm::vec3(inv * glm::vec4(dir, 0.0f));

        float hitT = 0.0f;
        bool hit = false;
        switch (mesh->shape) {
        case Shape::Box: hit = rayBox(lo, ld, glm::vec3(-0.5f), glm::vec3(0.5f), hitT); break;
        case Shape::Sphere: hit = raySphere(lo, ld, 0.5f, hitT); break;
        case Shape::Plane: hit = rayBox(lo, ld, {-0.5f, -0.001f, -0.5f}, {0.5f, 0.001f, 0.5f}, hitT); break;
        }
        if (!hit) return;
        const glm::vec3 worldHit = glm::vec3(model * glm::vec4(lo + ld * hitT, 1.0f));
        const float distance = glm::length(worldHit - origin);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = e;
        }
    });
    return best;
}

void Editor::focusSelected() {
    if (!m_world.has<Transform>(selected())) return;
    const glm::mat4 m = shockwave::worldMatrix(m_world, selected());
    const float scale = std::max({glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))});
    const float distance = std::clamp(scale * 0.75f * 3.0f, 2.5f, 60.0f);
    m_camera.position = glm::vec3(m[3]) - m_camera.forward() * distance;
}

// ---- Panels -------------------------------------------------------------------------------

void Editor::drawHierarchy() {
    if (!ImGui::Begin("Hierarchy", &m_showHierarchy)) {
        ImGui::End();
        return;
    }

    const float createWidth = gui::buttonWidth("+ Add");
    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - createWidth - ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##filter", "Search entities", &m_hierarchyFilter);
    gui::alignTrailing(createWidth);
    if (gui::button("+ Add")) ImGui::OpenPopup("AddEntity");
    if (ImGui::BeginPopup("AddEntity")) {
        if (ImGui::MenuItem("Cube")) addEntity("Cube", "cube", "panel");
        if (ImGui::MenuItem("Sphere")) addEntity("Sphere", "sphere", "checker");
        if (ImGui::MenuItem("Plane")) addEntity("Plane", "plane", "grid");
        ImGui::Separator();
        if (ImGui::MenuItem("Empty entity")) addEmptyEntity();
        ImGui::EndPopup();
    }
    ImGui::Separator();

    ImGui::BeginChild("entities");
    // Who hangs under whom, worked out once per frame. Entities whose parent is gone
    // count as top level, the same as the renderer treats them.
    ChildMap children;
    std::vector<Entity> roots;
    m_world.forEachEntity([&](Entity e) {
        const Entity parent = shockwave::parentOf(m_world, e);
        if (parent.valid()) {
            children[parent.index].push_back(e);
        } else {
            roots.push_back(e);
        }
    });

    if (m_hierarchyFilter.empty()) {
        for (const Entity e : roots) drawHierarchyNode(e, children, 0);
    } else {
        // Filtering shows a flat list of matches, wherever they are in the tree.
        auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
            return text;
        };
        const std::string filter = lower(m_hierarchyFilter);
        std::vector<Entity> matches;
        m_world.forEachEntity([&](Entity e) {
            if (lower(displayName(e)).find(filter) != std::string::npos) matches.push_back(e);
        });
        for (const Entity e : matches) {
            if (m_world.alive(e)) drawHierarchyRow(e, false);
        }
    }
    m_revealSelection = false;

    // The empty space below the list: click to clear the selection, drop an entity on it
    // to move it back to the top level.
    const ImVec2 rest = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##background", ImVec2(std::max(rest.x, 1.0f), std::max(rest.y, ImGui::GetFrameHeight())));
    if (ImGui::IsItemClicked()) select(kNullEntity);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            reparent(*static_cast<const Entity*>(payload->Data), kNullEntity);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();
    ImGui::End();
}

void Editor::drawHierarchyNode(Entity e, const ChildMap& children, int depth) {
    if (!m_world.alive(e)) return;   // deleted from a context menu earlier this frame
    const auto it = children.find(e.index);
    const bool hasChildren = it != children.end() && depth < shockwave::kMaxHierarchyDepth;
    const bool open = drawHierarchyRow(e, hasChildren);
    if (hasChildren && open) {
        ImGui::TreePush(reinterpret_cast<const void*>(static_cast<uintptr_t>(e.index)));
        const std::vector<Entity> list = it->second;   // copy: rows may edit the world
        for (const Entity child : list) drawHierarchyNode(child, children, depth + 1);
        ImGui::TreePop();
    }
}

// One entity's row: visibility box, then its name as a tree node that can be dragged
// onto another row (to attach it there) or onto the Inspector's entity fields.
bool Editor::drawHierarchyRow(Entity e, bool hasChildren) {
    ImGui::PushID(static_cast<int>(e.index));
    MeshRenderer* renderer = m_world.get<MeshRenderer>(e);
    const bool hidden = renderer && !renderer->visible;
    if (renderer) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
        ImGui::Checkbox("##visible", &renderer->visible);
        ImGui::PopStyleVar();
        trackEdit();
        ImGui::SetItemTooltip("Visible");
    } else {
        ImGui::Dummy(ImVec2(ImGui::GetTextLineHeight() + 4.0f, ImGui::GetTextLineHeight() + 4.0f));   // keep names lined up
    }
    ImGui::SameLine(0.0f, 6.0f);

    const std::string name = displayName(e);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_FramePadding |
                               ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
    if (m_selected == e) flags |= ImGuiTreeNodeFlags_Selected;
    if (hidden) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
    // Open the way down to an entity picked in the viewport.
    if (hasChildren && m_revealSelection && shockwave::isAncestor(m_world, e, selected())) ImGui::SetNextItemOpen(true);
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", name.c_str());
    ImGui::PopStyleVar();
    if (hidden) ImGui::PopStyleColor();

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) select(e);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) focusSelected();
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kEntityPayload, &e, sizeof(e));
        ImGui::Text("%s", name.c_str());
        ImGui::TextDisabled("Drop on an entity to attach it, or below the list to detach");
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            reparent(*static_cast<const Entity*>(payload->Data), e);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kTexturePayload)) {
            applyTexture(e, static_cast<const char*>(payload->Data));
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("row")) {
        select(e);
        if (ImGui::MenuItem("Focus", "F")) focusSelected();
        if (ImGui::BeginMenu("Add child")) {
            if (ImGui::MenuItem("Cube")) addChild("Cube", "cube", "panel");
            if (ImGui::MenuItem("Sphere")) addChild("Sphere", "sphere", "checker");
            if (ImGui::MenuItem("Empty entity")) addChild("Entity", nullptr, nullptr);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Detach from parent", nullptr, false, shockwave::parentOf(m_world, e).valid())) {
            reparent(e, kNullEntity);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicateSelected();
        if (ImGui::MenuItem("Delete", "Del")) deleteSelected();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return open;
}

// The inspector knows no component types: it draws whatever is registered, from the
// fields each component declared (see shockwave/Components.h). A game's own components
// show up here as soon as they're registered.
void Editor::drawInspector() {
    if (!ImGui::Begin("Inspector", &m_showInspector)) {
        ImGui::End();
        return;
    }
    const Entity e = selected();
    if (!e.valid()) {
        ImGui::TextDisabled("Select an entity in the viewport or the hierarchy.");
        ImGui::End();
        return;
    }

    ImGui::PushItemWidth(-100.0f * ImGui::GetStyle().FontScaleDpi);
    if (Name* name = m_world.get<Name>(e); name && gui::beginPropertyGrid("identity")) {
        gui::propertyLabel("Name");
        ImGui::InputText("##name", &name->value);
        trackEdit();
        ImGui::EndTable();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Entity %u (generation %u)", e.index, e.generation);

    gui::alignTrailing(gui::buttonWidth("+ Component"));
    if (gui::button("+ Component")) ImGui::OpenPopup("AddComponent");
    if (ImGui::BeginPopup("AddComponent")) {
        bool any = false;
        for (const ComponentInfo& info : shockwave::registeredComponents()) {
            if (info.has(m_world, e)) continue;
            any = true;
            if (ImGui::MenuItem(info.name.c_str())) {
                pushUndo(m_world);
                info.add(m_world, e);
                log("Added " + info.name + " to " + displayName(e));
            }
        }
        if (!any) ImGui::TextDisabled("It has every component already");
        ImGui::EndPopup();
    }
    ImGui::Separator();
    const uint32_t nameId = shockwave::componentId<Name>();
    for (const ComponentInfo& info : shockwave::registeredComponents()) {
        if (info.id == nameId || !info.has(m_world, e)) continue;
        bool removed = false;
        drawComponent(info, e, removed);
    }

    ImGui::PopItemWidth();
    ImGui::End();
}

void Editor::drawComponent(const ComponentInfo& info, Entity e, bool& removed) {
    ImGui::PushID(info.name.c_str());
    ImGui::Spacing();
    const bool open = ImGui::CollapsingHeader(info.name.c_str(),
                                              ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
    const float optionsWidth = ImGui::CalcTextSize("...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - optionsWidth);
    if (ImGui::SmallButton("...")) ImGui::OpenPopup("ComponentOptions");
    ImGui::SetItemTooltip("%s options", info.name.c_str());
    if (ImGui::BeginPopup("ComponentOptions")) {
        if (ImGui::MenuItem("Remove component")) {
            pushUndo(m_world);
            if (info.id == shockwave::componentId<Parent>()) {
                shockwave::setParent(m_world, e, kNullEntity);
            } else {
                info.remove(m_world, e);
            }
            log("Removed " + info.name + " from " + displayName(e));
            removed = true;
        }
        ImGui::EndPopup();
    }
    if (open && !removed) {
        char* data = static_cast<char*>(info.get(m_world, e));
        if (info.fields.empty()) ImGui::TextDisabled("No settings");
        if (gui::beginPropertyGrid("fields")) {
            for (const Field& f : info.fields) drawField(f, data + f.offset, e);
            ImGui::EndTable();
        }
    }
    ImGui::PopID();
}

void Editor::drawField(const Field& f, void* data, Entity e) {
    const std::string displayLabel = prettyName(f.name);
    gui::propertyLabel(displayLabel.c_str());
    const std::string label = "##value";
    const bool limited = f.min < f.max;
    const ImGuiSliderFlags clamp = limited ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
    auto speed = [&](float fallback) { return f.speed > 0.0f ? f.speed : fallback; };
    ImGui::PushID(f.name.c_str());

    // Picking a different asset is one undo step.
    auto assetCombo = [&](const auto& list, uint32_t currentId, auto assign) {
        std::string preview = "none";
        for (const auto& [n, h] : list) {
            if (h.id == currentId) preview = n;
        }
        if (ImGui::BeginCombo(label.c_str(), preview.c_str())) {
            for (const auto& [n, h] : list) {
                const bool current = h.id == currentId;
                if (ImGui::Selectable(n.c_str(), current) && !current) {
                    pushUndo(m_world);
                    assign(h);
                }
            }
            ImGui::EndCombo();
        }
    };

    switch (f.type) {
    case FieldType::Bool:
        ImGui::Checkbox(label.c_str(), static_cast<bool*>(data));
        trackEdit();
        break;
    case FieldType::Int:
        ImGui::DragInt(label.c_str(), static_cast<int*>(data), speed(0.2f), static_cast<int>(f.min),
                       static_cast<int>(f.max), "%d", clamp);
        trackEdit();
        break;
    case FieldType::Float:
        ImGui::DragFloat(label.c_str(), static_cast<float*>(data), speed(0.05f), f.min, f.max, "%.2f", clamp);
        trackEdit();
        break;
    case FieldType::Vec3:
        ImGui::DragFloat3(label.c_str(), static_cast<float*>(data), speed(0.05f), f.min, f.max, "%.2f", clamp);
        trackEdit();
        break;
    case FieldType::Color:
        ImGui::ColorEdit4(label.c_str(), static_cast<float*>(data), ImGuiColorEditFlags_Float);
        trackEdit();
        break;
    case FieldType::Rotation: {
        glm::quat& q = *static_cast<glm::quat*>(data);
        if (!(m_euler.entity == e) || m_euler.rotation != q) {
            m_euler = {e, q, shockwave::eulerDegrees(q)};
        }
        if (ImGui::DragFloat3(label.c_str(), &m_euler.degrees.x, speed(0.5f), 0.0f, 0.0f, "%.1f\xc2\xb0")) {
            q = shockwave::fromEulerDegrees(m_euler.degrees);
            m_euler.rotation = q;
        }
        trackEdit();
        break;
    }
    case FieldType::String:
        ImGui::InputText(label.c_str(), static_cast<std::string*>(data));
        trackEdit();
        break;
    case FieldType::Mesh: {
        auto* mesh = static_cast<shockwave::MeshHandle*>(data);
        assetCombo(engine().assets().meshes(), mesh->id, [&](shockwave::MeshHandle h) { *mesh = h; });
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMeshPayload)) {
                pushUndo(m_world);
                *mesh = engine().assets().findMesh(static_cast<const char*>(payload->Data));
            }
            ImGui::EndDragDropTarget();
        }
        break;
    }
    case FieldType::Texture: {
        auto* texture = static_cast<shockwave::TextureHandle*>(data);
        assetCombo(engine().assets().textures(), texture->id, [&](shockwave::TextureHandle h) { *texture = h; });
        if (ImGui::BeginItemTooltip()) {
            const float side = 128.0f * ImGui::GetStyle().FontScaleDpi;
            if (const uint64_t id = engine().renderer().guiTexture(*texture)) {
                ImGui::Image(static_cast<ImTextureID>(id), ImVec2(side, side));
            }
            ImGui::TextDisabled("Drop a texture from Assets here");
            ImGui::EndTooltip();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kTexturePayload)) {
                if (const auto h = engine().assets().findTexture(static_cast<const char*>(payload->Data))) {
                    pushUndo(m_world);
                    *texture = *h;
                }
            }
            ImGui::EndDragDropTarget();
        }
        break;
    }
    case FieldType::Entity: {
        auto* target = static_cast<Entity*>(data);
        const Parent* parent = m_world.get<Parent>(e);
        drawEntityField(label, target, e, parent && &parent->entity == target);
        break;
    }
    }
    ImGui::PopID();
}

// A combo of every other entity, which also takes an entity dragged from the Hierarchy.
// Setting a Parent goes through reparent, so the entity stays put and loops are refused.
void Editor::drawEntityField(const std::string& label, Entity* value, Entity owner, bool isParent) {
    auto assign = [&](Entity target) {
        if (target == *value || target == owner) return;
        if (isParent) {
            reparent(owner, target);
        } else {
            pushUndo(m_world);
            *value = target;
        }
    };
    const std::string preview = m_world.alive(*value) ? displayName(*value) : "none";
    if (ImGui::BeginCombo(label.c_str(), preview.c_str())) {
        if (ImGui::Selectable("none", !m_world.alive(*value))) assign(kNullEntity);
        m_world.forEachEntity([&](Entity candidate) {
            if (candidate == owner) return;
            ImGui::PushID(static_cast<int>(candidate.index));
            if (ImGui::Selectable(displayName(candidate).c_str(), candidate == *value)) assign(candidate);
            ImGui::PopID();
        });
        ImGui::EndCombo();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            assign(*static_cast<const Entity*>(payload->Data));
        }
        ImGui::EndDragDropTarget();
    }
}

void Editor::drawRendererPanel() {
    if (!ImGui::Begin("Renderer", &m_showRenderer)) {
        ImGui::End();
        return;
    }
    shockwave::Renderer& renderer = engine().renderer();
    const double fps = engine().fps();

    ImGui::TextDisabled("%s", renderer.gpuName());
    ImGui::Text("%.0f fps  (%.2f ms)", fps, fps > 0.0 ? 1000.0 / fps : 0.0);
    float maxTime = 0.0f;
    for (float t : m_frameTimes) maxTime = std::max(maxTime, t);
    ImGui::PlotLines("##frametimes", m_frameTimes.data(), static_cast<int>(m_frameTimes.size()),
                     static_cast<int>(m_frameTimeIndex), "frame time (ms)", 0.0f, std::max(maxTime * 1.2f, 1.0f),
                     ImVec2(-1.0f, 56.0f * ImGui::GetStyle().FontScaleDpi));
    ImGui::Text("Scene %u x %u, %u draws", renderer.renderWidth(), renderer.renderHeight(), renderer.drawCount());
    ImGui::Text("%u threads in the job system", engine().jobs().threadCount());

    ImGui::SeparatorText("Output");
    if (gui::beginPropertyGrid("output")) {
        gui::propertyLabel("Render scale");
        float scale = renderer.renderScale();
        if (ImGui::SliderFloat("##scale", &scale, 0.25f, 2.0f, "%.2fx")) renderer.setRenderScale(scale);
        ImGui::SetItemTooltip("Below 1 renders fewer pixels and scales up; above 1 supersamples");
        gui::propertyLabel("VSync");
        bool vsync = renderer.vsync();
        if (ImGui::Checkbox("##vsync", &vsync)) renderer.setVsync(vsync);
        ImGui::SetItemTooltip("Off renders as fast as the GPU can go");
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Camera");
    if (gui::beginPropertyGrid("camera")) {
        gui::propertyLabel("Field of view");
        float fovDegrees = glm::degrees(m_camera.fovY);
        if (ImGui::SliderFloat("##fov", &fovDegrees, 30.0f, 120.0f, "%.0f degrees"))
            m_camera.fovY = glm::radians(fovDegrees);
        gui::propertyLabel("Move speed");
        ImGui::SliderFloat("##speed", &m_camera.moveSpeed, 0.5f, 500.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        ImGui::EndTable();
    }

    // How the scheduler laid out the game's systems: each stage runs after the one
    // above it, and systems sharing a stage run at the same time.
    ImGui::SeparatorText("Systems");
    const auto stages = m_schedule.stages();
    for (size_t i = 0; i < stages.size(); ++i) {
        std::string row;
        for (const std::string& name : stages[i]) row += (row.empty() ? "" : ", ") + name;
        ImGui::BulletText("Stage %zu: %s", i + 1, row.c_str());
    }
    ImGui::TextDisabled("Runs in play mode, %.0f ticks per second", engine().tickRate());
    ImGui::End();
}

void Editor::drawLog() {
    if (!ImGui::Begin("Log", &m_showLog)) {
        ImGui::End();
        return;
    }
    if (ImGui::SmallButton("Clear")) m_log.clear();
    ImGui::Separator();
    ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    for (const std::string& line : m_log) ImGui::TextUnformatted(line.c_str());
    if (m_logScrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        m_logScrollToBottom = false;
    }
    ImGui::EndChild();
    ImGui::End();
}

void Editor::drawControlsWindow() {
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Controls", &m_showControls)) {
        ImGui::End();
        return;
    }
    static const char* rows[][2] = {
        {"Right mouse (hold)", "Fly: look around"},
        {"W A S D", "Fly: move (while holding right mouse)"},
        {"E / Q, Space / Ctrl", "Fly: up and down"},
        {"Shift", "Fly faster"},
        {"Mouse wheel", "Dolly; while flying, change speed"},
        {"Middle mouse drag", "Pan"},
        {"Left click", "Select"},
        {"W / E / R", "Move / rotate / scale gizmo"},
        {"T", "Gizmo axes: world or local"},
        {"Ctrl (while dragging)", "Toggle snapping"},
        {"F, or double-click", "Focus the selection"},
        {"Drag in Hierarchy", "Attach to another entity; drop below the list to detach"},
        {"Drag from Assets", "Mesh onto the viewport places it; texture onto an object applies it"},
        {"Ctrl+D / Del", "Duplicate / delete"},
        {"Ctrl+Z / Ctrl+Y", "Undo / redo"},
        {"Ctrl+N / O / S", "New / open / save scene"},
        {"Ctrl+P", "Play / stop"},
        {"Ctrl+Shift+P", "Pause / resume"},
        {"Esc", "Clear the selection"},
    };
    if (ImGui::BeginTable("controls", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        for (const auto& row : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row[0]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row[1]);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void Editor::drawUnsavedChangesModal() {
    if (m_openUnsavedModal) {
        ImGui::OpenPopup("Unsaved changes");
        m_openUnsavedModal = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("Save changes to %s first?", fileName(m_scenePath).c_str());
    ImGui::Spacing();
    if (gui::button("Save", ImVec2(110, 0))) {
        if (saveScene()) {
            ImGui::CloseCurrentPopup();
            runAction(m_pendingAction, m_pendingPath);
            m_pendingAction = PendingAction::None;
        }
    }
    ImGui::SameLine();
    if (gui::button("Don't save", ImVec2(110, 0))) {
        ImGui::CloseCurrentPopup();
        runAction(m_pendingAction, m_pendingPath);
        m_pendingAction = PendingAction::None;
    }
    ImGui::SameLine();
    if (gui::button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        m_pendingAction = PendingAction::None;
    }
    ImGui::EndPopup();
}

// ---- Play mode ----------------------------------------------------------------------------

void Editor::play() {
    if (m_playState == PlayState::Paused) {
        m_playState = PlayState::Playing;
        log("Resumed");
        return;
    }
    if (m_playState == PlayState::Playing) return;
    // Systems run on the live world; the copy is what Stop puts back.
    m_editWorld = m_world;
    shockwave::storePreviousTransforms(m_world, engine().jobs());
    m_playState = PlayState::Playing;
    log("Playing. Changes made now are undone by Stop.");
}

void Editor::pause() {
    if (m_playState == PlayState::Playing) {
        m_playState = PlayState::Paused;
        log("Paused");
    } else if (m_playState == PlayState::Paused) {
        play();
    }
}

void Editor::stop() {
    if (!playing()) return;
    m_world = std::move(m_editWorld);
    m_editWorld = shockwave::World();
    m_playState = PlayState::Editing;
    m_gizmoWasUsing = false;
    log("Stopped");
}

// ---- Editing ------------------------------------------------------------------------------

void Editor::select(Entity e) {
    const Entity next = m_world.alive(e) ? e : kNullEntity;
    if (!(next == m_selected)) m_revealSelection = true;
    m_selected = next;
}

Entity Editor::selected() const {
    return m_world.alive(m_selected) ? m_selected : kNullEntity;
}

std::string Editor::displayName(Entity e) const {
    const Name* name = m_world.get<Name>(e);
    return name && !name->value.empty() ? name->value : "Entity " + std::to_string(e.index);
}

std::string Editor::uniqueName(const std::string& base) const {
    auto taken = [&](const std::string& name) {
        bool found = false;
        if (const auto* names = m_world.pool<Name>()) {
            for (const Name& n : names->components()) found |= n.value == name;
        }
        return found;
    };
    if (!taken(base)) return base;
    for (int n = 2;; ++n) {
        const std::string name = base + " " + std::to_string(n);
        if (!taken(name)) return name;
    }
}

void Editor::addEntity(const char* name, const char* mesh, const char* texture) {
    pushUndo(m_world);
    const Entity e = m_world.create();
    m_world.add<Name>(e, {uniqueName(name)});
    // Drop it a few units in front of the camera, resting on the ground.
    const glm::vec3 ahead = m_camera.position + m_camera.forward() * 6.0f;
    m_world.add<Transform>(e, {{ahead.x, std::string(mesh) == "plane" ? 0.01f : 0.5f, ahead.z}});
    const shockwave::Assets& assets = engine().assets();
    m_world.add<MeshRenderer>(e, {assets.findMesh(mesh), assets.findTexture(texture).value_or(shockwave::TextureHandle{})});
    select(e);
    log("Added " + displayName(e));
}

void Editor::addEmptyEntity() {
    pushUndo(m_world);
    const Entity e = m_world.create();
    m_world.add<Name>(e, {uniqueName("Entity")});
    m_world.add<Transform>(e, {m_camera.position + m_camera.forward() * 6.0f});
    select(e);
    log("Added " + displayName(e) + "; give it components in the Inspector");
}

void Editor::addChild(const char* name, const char* mesh, const char* texture) {
    const Entity parent = selected();
    if (!parent.valid()) return;
    pushUndo(m_world);
    const Entity e = m_world.create();
    m_world.add<Name>(e, {uniqueName(name)});
    // A step to the side of the parent, in its own space, so it doesn't hide inside it.
    m_world.add<Transform>(e, {{1.5f, 0.0f, 0.0f}});
    if (mesh) {
        const shockwave::Assets& assets = engine().assets();
        m_world.add<MeshRenderer>(e, {assets.findMesh(mesh), assets.findTexture(texture).value_or(shockwave::TextureHandle{})});
    }
    m_world.add<Parent>(e, {parent});
    select(e);
    log("Added " + displayName(e) + " under " + displayName(parent));
}

Entity Editor::spawnMesh(const std::string& mesh, const glm::vec3& position) {
    pushUndo(m_world);
    const Entity e = m_world.create();
    std::string base = mesh;
    if (!base.empty()) base[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(base[0])));
    m_world.add<Name>(e, {uniqueName(base)});
    m_world.add<Transform>(e, {position});
    m_world.add<MeshRenderer>(e, {engine().assets().findMesh(mesh), shockwave::TextureHandle{}});
    select(e);
    log("Added " + displayName(e));
    return e;
}

bool Editor::reparent(Entity child, Entity parent) {
    if (!m_world.alive(child) || shockwave::parentOf(m_world, child) == parent) return false;
    if (parent.valid() && (parent == child || shockwave::isAncestor(m_world, child, parent))) {
        log("Can't put " + displayName(child) + " under " + displayName(parent) + ": it's inside it");
        return false;
    }
    shockwave::World before = m_world;
    // Keep it where it is on screen; its Transform is recalculated relative to the new parent.
    if (!shockwave::setParent(m_world, child, parent)) return false;
    pushUndo(std::move(before));
    log(parent.valid() ? "Attached " + displayName(child) + " to " + displayName(parent)
                       : "Detached " + displayName(child));
    return true;
}

bool Editor::applyTexture(Entity e, const std::string& texture) {
    MeshRenderer* renderer = m_world.get<MeshRenderer>(e);
    const auto handle = engine().assets().findTexture(texture);
    if (!renderer || !handle) {
        if (m_world.alive(e)) log(displayName(e) + " has no MeshRenderer to show " + texture + " on");
        return false;
    }
    pushUndo(m_world);
    renderer->texture = *handle;
    log("Gave " + displayName(e) + " the texture " + texture);
    return true;
}

void Editor::duplicateSelected() {
    const Entity e = selected();
    if (!e.valid()) return;
    pushUndo(m_world);
    const Entity copy = shockwave::cloneWithChildren(m_world, e);
    if (Name* name = m_world.get<Name>(copy)) name->value = uniqueName(name->value);
    if (Transform* t = m_world.get<Transform>(copy)) t->position.x += 1.0f;
    select(copy);
    log("Duplicated as " + displayName(copy));
}

void Editor::deleteSelected() {
    const Entity e = selected();
    if (!e.valid()) return;
    pushUndo(m_world);
    const std::string name = displayName(e);
    const size_t count = shockwave::subtree(m_world, e).size();
    shockwave::destroyWithChildren(m_world, e);
    select(kNullEntity);
    log(count > 1 ? "Deleted " + name + " and " + std::to_string(count - 1) + " under it" : "Deleted " + name);
}

void Editor::trackEdit() {
    // Snapshot when a widget starts editing and commit one undo step when it finishes,
    // so a whole drag is a single undo instead of one per frame.
    if (ImGui::IsItemActivated()) m_editStart = m_world;
    if (ImGui::IsItemDeactivatedAfterEdit()) pushUndo(m_editStart);
}

void Editor::pushUndo(shockwave::World before) {
    if (playing()) return;   // play-mode changes are thrown away on Stop anyway
    constexpr size_t kMaxUndo = 200;
    m_undo.push_back(std::move(before));
    if (m_undo.size() > kMaxUndo) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_dirty = true;
}

void Editor::undo() {
    if (m_undo.empty() || playing()) return;
    m_redo.push_back(std::move(m_world));
    m_world = std::move(m_undo.back());
    m_undo.pop_back();
    m_dirty = true;
}

void Editor::redo() {
    if (m_redo.empty() || playing()) return;
    m_undo.push_back(std::move(m_world));
    m_world = std::move(m_redo.back());
    m_redo.pop_back();
    m_dirty = true;
}

void Editor::handleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, global)) requestAction(PendingAction::New);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, global)) requestAction(PendingAction::Open);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global)) saveScene();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, global)) showSaveDialog();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, global)) undo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, global) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, global)) {
        redo();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, global)) duplicateSelected();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, global)) m_playState == PlayState::Playing ? stop() : play();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P, global)) pause();

    // Single keys only when nobody is typing and the camera isn't flying.
    if (io.WantTextInput || m_looking || io.KeyCtrl || io.KeyAlt) return;
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_gizmoOperation = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_gizmoOperation = ImGuizmo::ROTATE;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_gizmoOperation = ImGuizmo::SCALE;
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        m_gizmoMode = m_gizmoMode == ImGuizmo::WORLD ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) focusSelected();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) deleteSelected();
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) m_showControls = !m_showControls;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        select(kNullEntity);
    }
}

// ---- Files --------------------------------------------------------------------------------

void Editor::newScene() {
    stop();
    makeEmptyScene(m_world, engine().assets());
    m_scenePath = (std::filesystem::path(m_scenePath).parent_path() / "untitled.swscene").string();
    m_undo.clear();
    m_redo.clear();
    select(kNullEntity);
    m_dirty = false;
    log("New scene");
}

bool Editor::openScene(const std::string& path) {
    stop();
    shockwave::World loaded;
    std::string error;
    std::vector<std::string> warnings;
    EditorCamera camera;
    shockwave::SceneFileHooks hooks;
    hooks.readHeader = [&](const std::string& key, std::istream& in) {
        // "camera" is what version 1 files called it.
        if (key == "editor_camera" || key == "camera") {
            in >> camera.position.x >> camera.position.y >> camera.position.z >> camera.yaw >> camera.pitch;
        }
    };
    if (!shockwave::loadScene(loaded, engine().assets(), path, error, hooks, &warnings)) {
        log("Couldn't open scene: " + error);
        return false;
    }
    for (const std::string& w : warnings) log("Warning: " + w);
    m_world = std::move(loaded);
    m_scenePath = path;
    m_camera.position = camera.position;
    m_camera.yaw = camera.yaw;
    m_camera.pitch = camera.pitch;
    m_undo.clear();
    m_redo.clear();
    select(kNullEntity);
    m_dirty = false;
    log("Opened " + path + " (" + std::to_string(m_world.entityCount()) + " entities)");
    return true;
}

bool Editor::saveScene() {
    if (playing()) {
        log("Stop playing before saving: the scene is mid-simulation");
        return false;
    }
    // The camera is saved with the scene so it reopens where you left off.
    shockwave::SceneFileHooks hooks;
    hooks.writeHeader = [&](std::ostream& out) {
        out << "editor_camera " << m_camera.position.x << ' ' << m_camera.position.y << ' ' << m_camera.position.z
            << ' ' << m_camera.yaw << ' ' << m_camera.pitch << "\n";
    };
    std::string error;
    if (!shockwave::saveScene(m_world, engine().assets(), m_scenePath, error, hooks)) {
        log("Couldn't save: " + error);
        return false;
    }
    m_dirty = false;
    log("Saved " + m_scenePath);
    return true;
}

void Editor::showOpenDialog() {
    // SDL calls back from another thread (or later on this one); results are picked up
    // in handleDialogResults at the start of a frame.
    const std::string folder = std::filesystem::absolute(m_scenePath).parent_path().string();
    SDL_ShowOpenFileDialog(
        [](void* user, const char* const* files, int) {
            Editor* self = static_cast<Editor*>(user);
            std::lock_guard lock(self->m_dialogMutex);
            if (!files) {
                self->m_dialogError = SDL_GetError();
            } else if (files[0]) {
                self->m_dialogOpenResult = files[0];
            }
        },
        this, engine().window().handle(), kSceneFilter, 1, folder.c_str(), false);
}

void Editor::showSaveDialog() {
    const std::string location = std::filesystem::absolute(m_scenePath).string();
    SDL_ShowSaveFileDialog(
        [](void* user, const char* const* files, int) {
            Editor* self = static_cast<Editor*>(user);
            std::lock_guard lock(self->m_dialogMutex);
            if (!files) {
                self->m_dialogError = SDL_GetError();
            } else if (files[0]) {
                self->m_dialogSaveResult = files[0];
            }
        },
        this, engine().window().handle(), kSceneFilter, 1, location.c_str());
}

void Editor::handleDialogResults() {
    std::string openPath, savePath, error;
    {
        std::lock_guard lock(m_dialogMutex);
        openPath.swap(m_dialogOpenResult);
        savePath.swap(m_dialogSaveResult);
        error.swap(m_dialogError);
    }
    if (!error.empty()) log("File dialog unavailable (" + error + "). It needs xdg-desktop-portal or zenity.");
    if (!openPath.empty()) openScene(openPath);   // unsaved changes were already confirmed
    if (!savePath.empty()) {
        if (std::filesystem::path(savePath).extension() != ".swscene") savePath += ".swscene";
        m_scenePath = savePath;
        saveScene();
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
    case PendingAction::New: newScene(); break;
    case PendingAction::Open: showOpenDialog(); break;
    case PendingAction::OpenPath: openScene(path); break;
    case PendingAction::Quit: engine().quit(); break;
    case PendingAction::None: break;
    }
}

void Editor::updateTitle() {
    const std::string title = "Shockwave Editor - " + fileName(m_scenePath) + (m_dirty ? " *" : "") +
                              (playing() ? " [playing]" : "");
    if (title != m_title) {
        engine().window().setTitle(title);
        m_title = title;
    }
}

void Editor::logAssetChanges(const std::vector<shockwave::AssetFolder::Change>& changes) {
    using What = shockwave::AssetFolder::Change::What;
    for (const auto& change : changes) {
        switch (change.what) {
        case What::Added:
            if (const auto* file = m_assetFolder->find(change.name); file && file->imported) log("Imported " + change.name);
            break;
        case What::Reloaded: log("Reloaded " + change.name); break;
        case What::Failed: log("Couldn't import " + change.name + ": " + change.error); break;
        case What::Removed: log(change.name + " was removed; anything using it keeps the last version"); break;
        }
    }
}

void Editor::log(const std::string& message) {
    std::printf("Editor: %s\n", message.c_str());
    m_log.push_back(message);
    if (m_log.size() > 500) m_log.erase(m_log.begin());
    m_logScrollToBottom = true;
}

} // namespace editor
