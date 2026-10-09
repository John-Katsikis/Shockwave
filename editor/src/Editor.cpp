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

} // namespace

Editor::Editor(EditorOptions options) : m_options(std::move(options)) {}

// ---- Startup ------------------------------------------------------------------------------

void Editor::onStart() {
    ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;   // dragging in the viewport is for the scene
    // ImGuizmo's colors are sRGB like ImGui's; convert them for the sRGB swapchain too.
    for (ImVec4& c : ImGuizmo::GetStyle().Colors) c = gui::color(c.x, c.y, c.z, c.w);
    createAssets();
    shockwave::FlyCamera::bindDefaultControls(engine().input());

    if (std::filesystem::exists(m_options.scenePath)) {
        if (!openScene(m_options.scenePath)) newScene();
    } else {
        // First run: start from the sample scene and write it out, so there's a file to edit.
        m_scene = makeDefaultScene();
        m_scenePath = m_options.scenePath;
        m_camera.position = m_scene.camera.position;
        m_camera.yaw = m_scene.camera.yaw;
        m_camera.pitch = m_scene.camera.pitch;
        if (saveScene()) log("Created " + m_scenePath + " from the sample scene");
    }
    if (m_options.selectOnStart >= 0) select(m_options.selectOnStart);
    log("Hold the right mouse button in the viewport to fly; Help > Controls lists the shortcuts");
}

void Editor::createAssets() {
    shockwave::Renderer& renderer = engine().renderer();
    m_meshes = {
        {"cube", renderer.createMesh(shockwave::makeCube(1.0f)), Shape::Box},
        {"sphere", renderer.createMesh(shockwave::makeSphere(0.5f)), Shape::Sphere},
        {"plane", renderer.createMesh(shockwave::makePlane(1.0f, 1.0f)), Shape::Plane},
        // A plane that tiles its texture 40 times: one tile per unit at the default 40 x 40.
        {"floor", renderer.createMesh(shockwave::makePlane(1.0f, 40.0f)), Shape::Plane},
    };
    const auto checker = makeChecker(256, 8, {0.80f, 0.80f, 0.82f}, {0.55f, 0.56f, 0.6f});
    const auto panel = makePanel(256);
    const auto grid = makeGrid(256);
    m_textures = {
        {"white", shockwave::TextureHandle{}},
        {"checker", renderer.createTexture(256, 256, checker)},
        {"panel", renderer.createTexture(256, 256, panel)},
        {"grid", renderer.createTexture(256, 256, grid)},
    };
}

const Editor::MeshAsset* Editor::findMesh(const std::string& name) const {
    for (const MeshAsset& m : m_meshes) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

shockwave::TextureHandle Editor::findTexture(const std::string& name) const {
    for (const TextureAsset& t : m_textures) {
        if (t.name == name) return t.handle;
    }
    return {};
}

// ---- Per frame ----------------------------------------------------------------------------

void Editor::onGui() {
    ImGuizmo::BeginFrame();
    m_frameTimes[m_frameTimeIndex] = ImGui::GetIO().DeltaTime * 1000.0f;
    m_frameTimeIndex = (m_frameTimeIndex + 1) % m_frameTimes.size();

    handleDialogResults();
    handleShortcuts();
    drawMenuBar();
    drawDockspace();
    drawViewport();
    if (m_showHierarchy) drawHierarchy();
    if (m_showInspector) drawInspector();
    if (m_showRenderer) drawRendererPanel();
    if (m_showLog) drawLog();
    if (m_showControls) drawControlsWindow();
    if (m_showDemo) ImGui::ShowDemoWindow(&m_showDemo);
    drawUnsavedChangesModal();
    updateTitle();
}

void Editor::onRender(shockwave::Renderer& renderer, double alpha) {
    (void)alpha;   // the editor scene is static; play mode will interpolate like a game
    renderer.setClearColor({m_scene.skyColor.r, m_scene.skyColor.g, m_scene.skyColor.b, 1.0f});
    renderer.setCamera(m_camera.view(), projection(renderer.aspectRatio()));

    // Draw submission fans out across the job system; drawMesh is thread-safe.
    engine().jobs().parallelFor(m_scene.entities.size(), 256, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const Entity& e = m_scene.entities[i];
            if (!e.visible) continue;
            const MeshAsset* mesh = findMesh(e.mesh);
            if (!mesh) continue;
            renderer.drawMesh(mesh->handle, e.matrix(), findTexture(e.texture), e.tint);
        }
    });
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
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !m_undo.empty())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !m_redo.empty())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, selected() != nullptr)) duplicateSelected();
        if (ImGui::MenuItem("Delete", "Del", false, selected() != nullptr)) deleteSelected();
        if (ImGui::MenuItem("Focus", "F", false, selected() != nullptr)) focusSelected();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
        if (ImGui::MenuItem("Cube")) addEntity("Cube", "cube", "panel");
        if (ImGui::MenuItem("Sphere")) addEntity("Sphere", "sphere", "checker");
        if (ImGui::MenuItem("Plane")) addEntity("Plane", "plane", "grid");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Hierarchy", nullptr, &m_showHierarchy);
        ImGui::MenuItem("Inspector", nullptr, &m_showInspector);
        ImGui::MenuItem("Renderer", nullptr, &m_showRenderer);
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

void Editor::drawDockspace() {
    const ImGuiID dockspace = ImGui::GetID("ShockwaveDockspace");
    if (m_resetLayout || !ImGui::DockBuilderGetNode(dockspace)) {
        buildDefaultLayout(dockspace);
        m_showHierarchy = m_showInspector = m_showRenderer = m_showLog = true;
        m_resetLayout = false;
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
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, nullptr, &center);
    const ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Renderer", rightBottom);
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
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 1.0f), std::max(ImGui::GetContentRegionAvail().y, 1.0f));

    // The engine renders the scene at exactly this panel's size, so it stays sharp and
    // undistorted however the panel is docked or resized.
    renderer.setViewport(static_cast<uint32_t>(size.x * io.DisplayFramebufferScale.x),
                         static_cast<uint32_t>(size.y * io.DisplayFramebufferScale.y));
    if (renderer.sceneTexture()) ImGui::Image(static_cast<ImTextureID>(renderer.sceneTexture()), size);
    const bool imageHovered = ImGui::IsItemHovered();

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
    if (Entity* e = selected()) {
        view = m_camera.view();
        const glm::mat4 proj = gizmoProjection(aspect);
        glm::mat4 model = e->matrix();
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
        if (using_ && !m_gizmoWasUsing) m_editStart = m_scene;
        if (changed) e->setMatrix(model);
        if (!using_ && m_gizmoWasUsing) pushUndo(m_editStart);
        m_gizmoWasUsing = using_;
    } else {
        m_gizmoWasUsing = false;
    }

    drawViewportToolbar(pos, size);

    // Click to select. Clicks on the toolbar, gizmo or view cube don't count.
    if (imageHovered && !m_looking && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered() &&
        !ImGuizmo::IsOver() && !ImGuizmo::IsUsingAny() && !ImGuizmo::IsViewManipulateHovered()) {
        select(pick(io.MousePos, pos, size));
    }

    // Status line in the bottom-left corner.
    char status[160];
    std::snprintf(status, sizeof(status), "%ux%u  |  %u draws  |  speed %.1f", renderer.renderWidth(),
                  renderer.renderHeight(), renderer.drawCount(), m_camera.moveSpeed);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 textPos(pos.x + 10.0f, pos.y + size.y - ImGui::GetTextLineHeight() - 8.0f);
    draw->AddText(ImVec2(textPos.x + 1, textPos.y + 1), IM_COL32(0, 0, 0, 160), status);
    draw->AddText(textPos, gui::colorU32(0.9f, 0.92f, 0.95f), status);

    ImGui::End();
}

void Editor::drawViewportToolbar(const ImVec2& pos, const ImVec2& size) {
    (void)size;
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 8.0f, pos.y + 8.0f));
    const ImVec4 accent = gui::accent();
    auto toolButton = [&](const char* label, bool active, const char* tooltip) {
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, accent);
        const bool pressed = ImGui::Button(label);
        if (active) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tooltip);
        return pressed;
    };

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
    if (toolButton("Move", m_gizmoOperation == ImGuizmo::TRANSLATE, "Move (W)")) m_gizmoOperation = ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (toolButton("Rotate", m_gizmoOperation == ImGuizmo::ROTATE, "Rotate (E)")) m_gizmoOperation = ImGuizmo::ROTATE;
    ImGui::SameLine();
    if (toolButton("Scale", m_gizmoOperation == ImGuizmo::SCALE, "Scale (R)")) m_gizmoOperation = ImGuizmo::SCALE;
    ImGui::SameLine(0.0f, 14.0f);
    if (toolButton(m_gizmoMode == ImGuizmo::WORLD ? "World" : "Local", false,
                   "Gizmo axes: world or the entity's own (T)")) {
        m_gizmoMode = m_gizmoMode == ImGuizmo::WORLD ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    }
    ImGui::SameLine();
    if (toolButton("Snap", m_snap, "Snap to 0.5 units, 15 degrees, 0.1 scale (hold Ctrl to flip)")) m_snap = !m_snap;
    ImGui::PopStyleVar();
}

void Editor::drawSelectionOutline(const ImVec2& pos, const ImVec2& size) {
    const Entity* e = selected();
    if (!e || !e->visible) return;
    const MeshAsset* mesh = findMesh(e->mesh);
    if (!mesh) return;

    const glm::vec3 lo = mesh->shape == Shape::Plane ? glm::vec3(-0.5f, 0.0f, -0.5f) : glm::vec3(-0.5f);
    const glm::vec3 hi = mesh->shape == Shape::Plane ? glm::vec3(0.5f, 0.0f, 0.5f) : glm::vec3(0.5f);
    const glm::mat4 mvp = projection(size.x / size.y) * m_camera.view() * e->matrix();

    glm::vec4 clip[8];
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        clip[i] = mvp * glm::vec4(corner, 1.0f);
    }
    auto toScreen = [&](const glm::vec4& c) {
        return ImVec2(pos.x + (c.x / c.w * 0.5f + 0.5f) * size.x, pos.y + (0.5f - c.y / c.w * 0.5f) * size.y);
    };

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 color = gui::colorU32(1.0f, 0.62f, 0.15f);
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

int Editor::pick(const ImVec2& mouse, const ImVec2& pos, const ImVec2& size) const {
    const glm::vec2 ndc(2.0f * (mouse.x - pos.x) / size.x - 1.0f, 1.0f - 2.0f * (mouse.y - pos.y) / size.y);
    const glm::mat4 invViewProj = glm::inverse(projection(size.x / size.y) * m_camera.view());
    glm::vec4 nearPoint = invViewProj * glm::vec4(ndc, 1.0f, 1.0f);   // reverse-Z: depth 1 is the near plane
    nearPoint /= nearPoint.w;
    const glm::vec3 origin = m_camera.position;
    const glm::vec3 dir = glm::normalize(glm::vec3(nearPoint) - origin);

    int best = -1;
    float bestDistance = std::numeric_limits<float>::max();
    for (size_t i = 0; i < m_scene.entities.size(); ++i) {
        const Entity& e = m_scene.entities[i];
        const MeshAsset* mesh = findMesh(e.mesh);
        if (!e.visible || !mesh) continue;
        const glm::mat4 model = e.matrix();
        const glm::mat4 inv = glm::inverse(model);
        const glm::vec3 lo = glm::vec3(inv * glm::vec4(origin, 1.0f));
        const glm::vec3 ld = glm::vec3(inv * glm::vec4(dir, 0.0f));

        float t = 0.0f;
        bool hit = false;
        switch (mesh->shape) {
        case Shape::Box: hit = rayBox(lo, ld, glm::vec3(-0.5f), glm::vec3(0.5f), t); break;
        case Shape::Sphere: hit = raySphere(lo, ld, 0.5f, t); break;
        case Shape::Plane: hit = rayBox(lo, ld, {-0.5f, -0.001f, -0.5f}, {0.5f, 0.001f, 0.5f}, t); break;
        }
        if (!hit) continue;
        const glm::vec3 worldHit = glm::vec3(model * glm::vec4(lo + ld * t, 1.0f));
        const float distance = glm::length(worldHit - origin);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void Editor::focusSelected() {
    const Entity* e = selected();
    if (!e) return;
    const float radius = std::max({e->scale.x, e->scale.y, e->scale.z}) * 0.75f;
    const float distance = std::clamp(radius * 3.0f, 2.5f, 60.0f);
    m_camera.position = e->position - m_camera.forward() * distance;
}

// ---- Panels -------------------------------------------------------------------------------

void Editor::drawHierarchy() {
    if (!ImGui::Begin("Hierarchy", &m_showHierarchy)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("+ Add")) ImGui::OpenPopup("AddEntity");
    if (ImGui::BeginPopup("AddEntity")) {
        if (ImGui::MenuItem("Cube")) addEntity("Cube", "cube", "panel");
        if (ImGui::MenuItem("Sphere")) addEntity("Sphere", "sphere", "checker");
        if (ImGui::MenuItem("Plane")) addEntity("Plane", "plane", "grid");
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "Filter", &m_hierarchyFilter);
    ImGui::Separator();

    ImGui::BeginChild("entities");
    for (size_t i = 0; i < m_scene.entities.size(); ++i) {
        Entity& e = m_scene.entities[i];
        if (!m_hierarchyFilter.empty()) {
            auto lower = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
                return s;
            };
            if (lower(e.name).find(lower(m_hierarchyFilter)) == std::string::npos) continue;
        }

        ImGui::PushID(static_cast<int>(i));
        ImGui::Checkbox("##visible", &e.visible);
        trackEdit();
        ImGui::SetItemTooltip("Visible");
        ImGui::SameLine();
        if (!e.visible) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(e.name.c_str(), m_selected == static_cast<int>(i))) select(static_cast<int>(i));
        if (!e.visible) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) focusSelected();
        if (ImGui::BeginPopupContextItem()) {
            select(static_cast<int>(i));
            if (ImGui::MenuItem("Focus", "F")) focusSelected();
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicateSelected();
            if (ImGui::MenuItem("Delete", "Del")) deleteSelected();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    // Clicking empty space clears the selection.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
        select(-1);
    }
    ImGui::EndChild();
    ImGui::End();
}

void Editor::drawInspector() {
    if (!ImGui::Begin("Inspector", &m_showInspector)) {
        ImGui::End();
        return;
    }
    Entity* e = selected();
    if (!e) {
        ImGui::TextDisabled("Select an entity in the viewport or the hierarchy.");
        ImGui::End();
        return;
    }

    ImGui::PushItemWidth(-90.0f * ImGui::GetStyle().FontScaleDpi);
    ImGui::InputText("Name", &e->name);
    trackEdit();
    ImGui::Checkbox("Visible", &e->visible);
    trackEdit();

    ImGui::SeparatorText("Transform");
    ImGui::DragFloat3("Position", &e->position.x, 0.05f, 0.0f, 0.0f, "%.2f");
    trackEdit();
    ImGui::DragFloat3("Rotation", &e->rotation.x, 0.5f, 0.0f, 0.0f, "%.1f\xc2\xb0");
    trackEdit();
    ImGui::DragFloat3("Scale", &e->scale.x, 0.02f, 0.001f, 1000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    trackEdit();
    if (ImGui::Button("Reset transform")) {
        pushUndo(m_scene);
        e = selected();
        e->position = glm::vec3(0.0f);
        e->rotation = glm::vec3(0.0f);
        e->scale = glm::vec3(1.0f);
    }

    ImGui::SeparatorText("Look");
    if (ImGui::BeginCombo("Mesh", e->mesh.c_str())) {
        for (const MeshAsset& m : m_meshes) {
            const bool current = m.name == e->mesh;
            if (ImGui::Selectable(m.name.c_str(), current) && !current) {
                pushUndo(m_scene);
                e = selected();
                e->mesh = m.name;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginCombo("Texture", e->texture.c_str())) {
        for (const TextureAsset& t : m_textures) {
            const bool current = t.name == e->texture;
            if (ImGui::Selectable(t.name.c_str(), current) && !current) {
                pushUndo(m_scene);
                e = selected();
                e->texture = t.name;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::ColorEdit4("Tint", &e->tint.x, ImGuiColorEditFlags_Float);
    trackEdit();
    ImGui::PopItemWidth();
    ImGui::End();
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

    ImGui::PushItemWidth(-110.0f * ImGui::GetStyle().FontScaleDpi);
    ImGui::SeparatorText("Output");
    float scale = renderer.renderScale();
    if (ImGui::SliderFloat("Render scale", &scale, 0.25f, 2.0f, "%.2fx")) renderer.setRenderScale(scale);
    ImGui::SetItemTooltip("Below 1 renders fewer pixels and scales up; above 1 supersamples");
    bool vsync = renderer.vsync();
    if (ImGui::Checkbox("VSync", &vsync)) renderer.setVsync(vsync);
    ImGui::SetItemTooltip("Off renders as fast as the GPU can go");

    ImGui::SeparatorText("Scene");
    ImGui::ColorEdit3("Sky color", &m_scene.skyColor.r, ImGuiColorEditFlags_Float);
    trackEdit();

    ImGui::SeparatorText("Camera");
    float fovDegrees = glm::degrees(m_camera.fovY);
    if (ImGui::SliderFloat("Field of view", &fovDegrees, 30.0f, 120.0f, "%.0f\xc2\xb0")) {
        m_camera.fovY = glm::radians(fovDegrees);
    }
    ImGui::SliderFloat("Move speed", &m_camera.moveSpeed, 0.5f, 500.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::PopItemWidth();
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
        {"Ctrl+D / Del", "Duplicate / delete"},
        {"Ctrl+Z / Ctrl+Y", "Undo / redo"},
        {"Ctrl+N / O / S", "New / open / save scene"},
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
    if (ImGui::Button("Save", ImVec2(110, 0))) {
        if (saveScene()) {
            ImGui::CloseCurrentPopup();
            runAction(m_pendingAction, m_pendingPath);
            m_pendingAction = PendingAction::None;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't save", ImVec2(110, 0))) {
        ImGui::CloseCurrentPopup();
        runAction(m_pendingAction, m_pendingPath);
        m_pendingAction = PendingAction::None;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        m_pendingAction = PendingAction::None;
    }
    ImGui::EndPopup();
}

// ---- Editing ------------------------------------------------------------------------------

void Editor::select(int index) {
    m_selected = (index >= 0 && index < static_cast<int>(m_scene.entities.size())) ? index : -1;
}

Entity* Editor::selected() {
    return m_selected >= 0 && m_selected < static_cast<int>(m_scene.entities.size()) ? &m_scene.entities[m_selected]
                                                                                       : nullptr;
}

std::string Editor::uniqueName(const std::string& base) const {
    auto taken = [&](const std::string& name) {
        return std::any_of(m_scene.entities.begin(), m_scene.entities.end(),
                           [&](const Entity& e) { return e.name == name; });
    };
    if (!taken(base)) return base;
    for (int n = 2;; ++n) {
        const std::string name = base + " " + std::to_string(n);
        if (!taken(name)) return name;
    }
}

void Editor::addEntity(const char* name, const char* mesh, const char* texture) {
    pushUndo(m_scene);
    Entity e;
    e.name = uniqueName(name);
    e.mesh = mesh;
    e.texture = texture;
    // Drop it a few units in front of the camera, resting on the ground.
    const glm::vec3 ahead = m_camera.position + m_camera.forward() * 6.0f;
    e.position = {ahead.x, std::string(mesh) == "plane" ? 0.01f : 0.5f, ahead.z};
    m_scene.entities.push_back(e);
    select(static_cast<int>(m_scene.entities.size()) - 1);
    log("Added " + e.name);
}

void Editor::duplicateSelected() {
    const Entity* e = selected();
    if (!e) return;
    pushUndo(m_scene);
    Entity copy = *selected();
    copy.name = uniqueName(copy.name);
    copy.position.x += 1.0f;
    m_scene.entities.push_back(copy);
    select(static_cast<int>(m_scene.entities.size()) - 1);
    log("Duplicated as " + copy.name);
}

void Editor::deleteSelected() {
    if (!selected()) return;
    pushUndo(m_scene);
    const std::string name = m_scene.entities[m_selected].name;
    m_scene.entities.erase(m_scene.entities.begin() + m_selected);
    select(-1);
    log("Deleted " + name);
}

void Editor::trackEdit() {
    // Snapshot when a widget starts editing and commit one undo step when it finishes,
    // so a whole drag is a single undo instead of one per frame.
    if (ImGui::IsItemActivated()) m_editStart = m_scene;
    if (ImGui::IsItemDeactivatedAfterEdit()) pushUndo(m_editStart);
}

void Editor::pushUndo(Scene before) {
    constexpr size_t kMaxUndo = 200;
    m_undo.push_back(std::move(before));
    if (m_undo.size() > kMaxUndo) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_dirty = true;
}

void Editor::undo() {
    if (m_undo.empty()) return;
    m_redo.push_back(std::move(m_scene));
    m_scene = std::move(m_undo.back());
    m_undo.pop_back();
    select(m_selected);
    m_dirty = true;
}

void Editor::redo() {
    if (m_redo.empty()) return;
    m_undo.push_back(std::move(m_scene));
    m_scene = std::move(m_redo.back());
    m_redo.pop_back();
    select(m_selected);
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
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) select(-1);
}

// ---- Files --------------------------------------------------------------------------------

void Editor::newScene() {
    m_scene = Scene();
    Entity floor;
    floor.name = "Floor";
    floor.mesh = "floor";
    floor.texture = "grid";
    floor.scale = {40.0f, 1.0f, 40.0f};
    m_scene.entities.push_back(floor);
    m_scenePath = (std::filesystem::path(m_scenePath).parent_path() / "untitled.swscene").string();
    m_undo.clear();
    m_redo.clear();
    select(-1);
    m_dirty = false;
    log("New scene");
}

bool Editor::openScene(const std::string& path) {
    Scene loaded;
    std::string error;
    if (!loadScene(loaded, path, error)) {
        log("Couldn't open scene: " + error);
        return false;
    }
    m_scene = std::move(loaded);
    m_scenePath = path;
    m_camera.position = m_scene.camera.position;
    m_camera.yaw = m_scene.camera.yaw;
    m_camera.pitch = m_scene.camera.pitch;
    m_undo.clear();
    m_redo.clear();
    select(-1);
    m_dirty = false;
    log("Opened " + path + " (" + std::to_string(m_scene.entities.size()) + " entities)");
    return true;
}

bool Editor::saveScene() {
    // The camera is saved with the scene so it reopens where you left off.
    m_scene.camera.position = m_camera.position;
    m_scene.camera.yaw = m_camera.yaw;
    m_scene.camera.pitch = m_camera.pitch;
    std::string error;
    if (!editor::saveScene(m_scene, m_scenePath, error)) {
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
    const std::string title = "Shockwave Editor - " + fileName(m_scenePath) + (m_dirty ? " *" : "");
    if (title != m_title) {
        engine().window().setTitle(title);
        m_title = title;
    }
}

void Editor::log(const std::string& message) {
    std::printf("Editor: %s\n", message.c_str());
    m_log.push_back(message);
    if (m_log.size() > 500) m_log.erase(m_log.begin());
    m_logScrollToBottom = true;
}

} // namespace editor
