// The Viewport panel: the scene rendered at the panel's size, the editor grid, camera
// controls, picking, the transform gizmo, camera and light icons, shading views,
// screenshots and drops from the Content panel.
#include "Editor.h"

#include <Engine/Prefab.h>
#include <UI/Gui.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <span>

namespace editor {

namespace gui = shockwave::gui;
using shockwave::Entity;
using shockwave::kNullEntity;
using shockwave::MeshRenderer;
using shockwave::Transform;

namespace {

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

// Text on a strip of panel background, readable over any scene.
void overlayText(ImDrawList* draw, ImVec2 at, ImU32 color, const char* text) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    draw->AddRectFilled(
        ImVec2(at.x - 4, at.y - 2), ImVec2(at.x + size.x + 4, at.y + size.y + 2),
        ImGui::GetColorU32(ImGuiCol_WindowBg, 0.85f));
    draw->AddText(at, color, text);
}

ImU32 accentU32(float alpha = 1.0f) {
    ImVec4 c = shockwave::gui::currentTheme().accent;
    c.w *= alpha;
    return shockwave::gui::semanticColorU32(c);
}

}   // namespace

glm::mat4 Editor::projection(float aspect) const { return m_camera.projection(aspect); }

glm::mat4 Editor::gizmoProjection(float aspect) const {
    // ImGuizmo only needs to place handles on screen; it expects a conventional
    // finite projection rather than reverse-Z with an infinite far plane.
    return glm::perspective(m_camera.fovY, aspect, m_camera.nearPlane, 2000.0f);
}

// The box round what an entity draws, in its own space: its mesh's or its model's.
bool Editor::entityBounds(Entity e, shockwave::Bounds& out) {
    if (const MeshRenderer* r = m_world.get<MeshRenderer>(e); r && r->visible) {
        if (const shockwave::Bounds* b = meshBounds(r->mesh)) {
            out = *b;
            return true;
        }
    }
    if (const auto* m = m_world.get<shockwave::ModelRenderer>(e); m && m->visible) {
        if (const auto model = engine().assets().model(m->model)) {
            out = model->data.bounds;
            return true;
        }
    }
    return false;
}

const shockwave::Bounds* Editor::meshBounds(shockwave::MeshHandle mesh) {
    if (!mesh.valid()) return nullptr;
    if (const auto it = m_meshBounds.find(mesh.id); it != m_meshBounds.end()) return &it->second;
    const auto data = engine().assets().meshData(mesh);
    if (!data) return nullptr;
    return &(m_meshBounds[mesh.id] = shockwave::computeBounds(*data));
}

void Editor::drawViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open =
        ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }
    drawViewportToolbar();

    shockwave::Renderer& renderer = engine().renderer();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(
        std::max(ImGui::GetContentRegionAvail().x, 1.0f), std::max(ImGui::GetContentRegionAvail().y, 1.0f));

    // The engine renders the scene at exactly this panel's size, so it stays sharp and
    // undistorted however the panel is docked or resized.
    renderer.setViewport(
        static_cast<uint32_t>(size.x * io.DisplayFramebufferScale.x),
        static_cast<uint32_t>(size.y * io.DisplayFramebufferScale.y));
    if (renderer.sceneTexture()) ImGui::Image(static_cast<ImTextureID>(renderer.sceneTexture()), size);
    const bool imageHovered = ImGui::IsItemHovered();
    const bool gameView = lookingThroughSceneCamera();
    feedGameUIPointer(pos, size, imageHovered);
    acceptViewportDrop(pos, size);
    updateCamera(imageHovered);

    if (!gameView) {
        // Outlines: the primary selection brighter than the rest.
        const Entity primary = selected();
        for (Entity e : selection()) {
            drawOutline(e, pos, size, e == primary ? accentU32() : accentU32(0.55f));
        }
        if (m_prefs.showCameraIcons) {
            drawCameraIcons(pos, size);
            drawLightIcons(pos, size);
            drawAudioIcons(pos, size);
        }
        drawSkeletons(pos, size);

        // View cube in the top-right corner: click a face to snap the camera to that axis.
        glm::mat4 view = m_camera.view();
        const glm::mat4 before = view;
        const float cubeSize = 96.0f * ImGui::GetStyle().FontScaleDpi;
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(pos.x, pos.y, size.x, size.y);
        ImGuizmo::SetGizmoSizeClipSpace(0.14f);
        ImGuizmo::ViewManipulate(
            &view[0][0], 8.0f, ImVec2(pos.x + size.x - cubeSize - 8.0f, pos.y + 8.0f), ImVec2(cubeSize, cubeSize),
            IM_COL32(0, 0, 0, 0));
        if (std::memcmp(&view, &before, sizeof(view)) != 0) {
            const glm::mat4 inv = glm::inverse(view);
            const glm::vec3 forward = -glm::vec3(inv[2]);
            m_camera.position = glm::vec3(inv[3]);
            m_camera.pitch = std::asin(std::clamp(forward.y, -0.999f, 0.999f));
            m_camera.yaw = std::atan2(-forward.x, -forward.z);
        }
        drawGizmo(pos, size);
    } else {
        m_gizmoWasUsing = false;
    }

    drawViewportOverlay(pos, size);

    // Click to select (Ctrl toggles, Shift adds). Clicks on the overlay, gizmo or view
    // cube don't count.
    if (!gameView && imageHovered && !m_looking && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !m_gameUI->wantsMouse() && !ImGui::IsAnyItemHovered() && !ImGuizmo::IsOver() && !ImGuizmo::IsUsingAny() &&
        !ImGuizmo::IsViewManipulateHovered()) {
        Entity hit = m_prefs.showCameraIcons ? cameraIconAt(io.MousePos, pos, size) : kNullEntity;
        if (!hit.valid()) hit = pick(io.MousePos, pos, size);
        // A click on a prefab copy picks the whole copy; a second click, the part.
        const Entity copy = shockwave::prefabRootOf(m_world, hit);
        if (copy.valid() && !isSelected(hit) && !isSelected(copy)) hit = copy;
        if (io.KeyCtrl) {
            toggleSelected(hit);
        } else if (io.KeyShift) {
            if (hit.valid() && !isSelected(hit)) toggleSelected(hit);
        } else {
            select(hit);
        }
    }

    if (!m_screenshotPath.empty() && !engine().renderer().screenshotPending()) {
        const std::string& result = engine().renderer().screenshotResult();
        if (result.empty())
            log("Saved " + (m_hasProject ? m_project.relative(m_screenshotPath) : m_screenshotPath.string()));
        else
            error("Screenshot failed: " + result);
        m_screenshotPath.clear();
    }

    // A coloured frame round the viewport while playing, so play-mode edits (which are
    // thrown away on Stop) are hard to mistake for real ones.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (playing()) {
        const gui::Theme& theme = gui::currentTheme();
        draw->AddRect(
            pos, ImVec2(pos.x + size.x, pos.y + size.y),
            gui::semanticColorU32(playState() == PlayState::Playing ? theme.accent : theme.warning), 0.0f, 0, 3.0f);
    }
    ImGui::End();
}

// Above the scene, in its own strip: transform tools on the left, view settings on the
// right (docs/UI.md), so nothing covers what you're looking at.
void Editor::drawViewportToolbar() {
    shockwave::Renderer& renderer = engine().renderer();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float dpi = style.FontScaleDpi;
    const char* axes = m_gizmoMode == ImGuizmo::WORLD ? "World" : "Local";
    const char* cameraLabel = lookingThroughSceneCamera() ? "Game camera" : "Editor camera";
    const float toolsWidth = gui::controlGroupWidth({"Move", "Rotate", "Scale"}) + style.ItemSpacing.x * 3.0f +
                             gui::controlGroupWidth({axes, "Snap"});
    const float viewWidth = gui::controlGroupWidth({"Grid", cameraLabel, "View"});
    const float inner = std::max(ImGui::GetContentRegionAvail().x - style.WindowPadding.x * 2.0f, 1.0f);
    const int rows = toolsWidth + style.ItemSpacing.x * 3.0f + viewWidth <= inner ? 1 : 2;
    const float height =
        rows * ImGui::GetFrameHeight() + (rows - 1) * style.ItemSpacing.y + style.WindowPadding.y * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * dpi, 4.0f * dpi));
    ImGui::BeginChild(
        "ViewportToolbar", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    const float rowY = ImGui::GetCursorPosY();
    if (gui::toolButton("Move", m_gizmoOperation == ImGuizmo::TRANSLATE, "Move (W)"))
        m_gizmoOperation = ImGuizmo::TRANSLATE;
    gui::toolbarNext(gui::buttonWidth("Rotate"));
    if (gui::toolButton("Rotate", m_gizmoOperation == ImGuizmo::ROTATE, "Rotate (E)"))
        m_gizmoOperation = ImGuizmo::ROTATE;
    gui::toolbarNext(gui::buttonWidth("Scale"));
    if (gui::toolButton("Scale", m_gizmoOperation == ImGuizmo::SCALE, "Scale (R)")) m_gizmoOperation = ImGuizmo::SCALE;
    ImGui::SameLine(0.0f, style.ItemSpacing.x * 3.0f);
    if (gui::toolButton(axes, false, "Gizmo axes: world or the entity's own (T)")) {
        m_gizmoMode = m_gizmoMode == ImGuizmo::WORLD ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    }
    gui::toolbarNext(gui::buttonWidth("Snap"));
    if (gui::toolButton("Snap", m_prefs.snap, "Snap while dragging; Ctrl flips it. Right-click for step sizes")) {
        m_prefs.snap = !m_prefs.snap;
        m_prefsDirty = true;
    }
    if (ImGui::BeginPopupContextItem("SnapSettings")) {
        if (ImGui::Checkbox("Snap", &m_prefs.snap)) m_prefsDirty = true;
        ImGui::SetNextItemWidth(120.0f * dpi);
        if (ImGui::DragFloat("Move", &m_prefs.snapMove, 0.01f, 0.01f, 100.0f, "%.2f")) m_prefsDirty = true;
        ImGui::SetNextItemWidth(120.0f * dpi);
        if (ImGui::DragFloat("Rotate", &m_prefs.snapRotate, 0.5f, 1.0f, 180.0f, "%.0f degrees")) m_prefsDirty = true;
        ImGui::SetNextItemWidth(120.0f * dpi);
        if (ImGui::DragFloat("Scale", &m_prefs.snapScale, 0.01f, 0.01f, 10.0f, "%.2f")) m_prefsDirty = true;
        ImGui::EndPopup();
    }

    // View settings, right-aligned (or on their own row when the panel is narrow).
    if (rows == 1) {
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - viewWidth, rowY));
    } else {
        ImGui::SetCursorPosX(style.WindowPadding.x);
    }
    if (gui::toolButton("Grid", m_prefs.showGrid, "Ground grid (G)")) {
        m_prefs.showGrid = !m_prefs.showGrid;
        m_prefsDirty = true;
    }
    gui::toolbarNext(gui::buttonWidth(cameraLabel));
    if (gui::toolButton(
            cameraLabel, lookingThroughSceneCamera(),
            "Look through the scene's primary Camera, or the editor's free camera (F8)")) {
        m_viewThroughCamera = !m_viewThroughCamera;
        if (m_viewThroughCamera && !shockwave::primaryCamera(m_world).valid()) {
            warn("The scene has no Camera to look through. Add one with Create > Camera.");
            m_viewThroughCamera = false;
        }
    }
    gui::toolbarNext(gui::buttonWidth("View"));
    if (gui::button("View")) ImGui::OpenPopup("ViewOptions");
    ImGui::SetItemTooltip("Viewport options");
    if (ImGui::BeginPopup("ViewOptions")) {
        ImGui::SeparatorText("Show");
        if (ImGui::MenuItem("Grid", "G", &m_prefs.showGrid)) m_prefsDirty = true;
        if (ImGui::MenuItem("Camera and light icons", nullptr, &m_prefs.showCameraIcons)) m_prefsDirty = true;
        ImGui::MenuItem("Game UI", nullptr, &m_showGameUI);
        ImGui::SetItemTooltip("The scene's UIDocuments, drawn over it as the game shows them");
        ImGui::MenuItem("Navigation mesh", nullptr, &m_showNavMesh);
        ImGui::SetItemTooltip("Where agents can walk, and during Play the path each one is taking");
        ImGui::MenuItem("Particle effects", nullptr, &m_showEffects);
        ImGui::SetItemTooltip("Play the scene's ParticleEffects in the viewport");
        ImGui::MenuItem("Preview animations", nullptr, &m_previewAnimation);
        ImGui::SetItemTooltip("Play the scene's Animators in the viewport without starting the game");
        ImGui::MenuItem("Skeletons", nullptr, &m_showSkeletons);
        ImGui::SetItemTooltip("Draw the joints of the selected animated models");
        if (ImGui::MenuItem("Stats", nullptr, &m_prefs.showStats)) m_prefsDirty = true;
        ImGui::SeparatorText("Editor camera");
        ImGui::SetNextItemWidth(160.0f * dpi);
        float fov = m_prefs.fieldOfView;
        if (ImGui::SliderFloat("Field of view", &fov, 20.0f, 140.0f, "%.0f degrees")) {
            m_prefs.fieldOfView = fov;
            m_camera.fovY = glm::radians(fov);
            m_prefsDirty = true;
        }
        ImGui::SetNextItemWidth(160.0f * dpi);
        if (ImGui::SliderFloat("Fly speed", &m_camera.moveSpeed, 0.5f, 500.0f, "%.1f", ImGuiSliderFlags_Logarithmic)) {
            m_prefs.cameraSpeed = m_camera.moveSpeed;
            m_prefsDirty = true;
        }
        ImGui::SeparatorText("Rendering");
        ImGui::SetNextItemWidth(160.0f * dpi);
        float scale = renderer.renderScale();
        if (ImGui::SliderFloat("Render scale", &scale, 0.25f, 2.0f, "%.2fx")) {
            auto settings = engine().graphics().requested();
            settings.renderScale = scale;
            std::string error;
            engine().graphics().apply(settings, error);
            m_prefs.renderScale = scale;
            m_prefsDirty = true;
        }
        // What the scene shows: the lit result or a view for checking content. The same
        // views are available to tools as player --view.
        static const char* views[] = {"Lit", "Unlit (albedo)", "Normals", "Lighting only", "Shadow cascades",
                                      "Ambient occlusion"};
        int view = static_cast<int>(renderer.debugView());
        ImGui::SetNextItemWidth(160.0f * dpi);
        if (ImGui::Combo("Shading", &view, views, IM_ARRAYSIZE(views))) {
            renderer.setDebugView(static_cast<shockwave::DebugView>(view));
        }
        int shadows = static_cast<int>(renderer.shadowQuality());
        ImGui::SetNextItemWidth(160.0f * dpi);
        if (ImGui::Combo("Shadows", &shadows, kShadowQualityNames, IM_ARRAYSIZE(kShadowQualityNames))) {
            auto settings = engine().graphics().requested();
            settings.shadows = static_cast<shockwave::ShadowQuality>(shadows);
            std::string error;
            engine().graphics().apply(settings, error);
            m_prefs.shadowQuality = shadows;
            m_prefsDirty = true;
        }
        ImGui::SeparatorText("Capture");
        if (gui::button("Save screenshot", ImVec2(-FLT_MIN, 0.0f))) {
            ImGui::CloseCurrentPopup();
            saveViewportScreenshot();
        }
        ImGui::SetItemTooltip("Saves the viewport, without editor overlays, to the project's screenshots folder");
        ImGui::EndPopup();
    }
    ImGui::EndChild();
}

// Over the scene, top-left: which camera this is when it isn't the editor's, and stats.
void Editor::drawViewportOverlay(const ImVec2& pos, const ImVec2& size) {
    shockwave::Renderer& renderer = engine().renderer();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float y = pos.y + 8.0f;
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    if (lookingThroughSceneCamera()) {
        const std::string label =
            "Looking through " + displayName(shockwave::primaryCamera(m_world)) + " (F8: editor camera)";
        overlayText(draw, ImVec2(pos.x + 10.0f, y), text, label.c_str());
        y += ImGui::GetTextLineHeightWithSpacing() + 4.0f;
    }
    if (m_prefs.showStats) {
        const double fps = engine().fps();
        char lines[4][96];
        std::snprintf(lines[0], sizeof(lines[0]), "%.0f fps  %.2f ms", fps, fps > 0.0 ? 1000.0 / fps : 0.0);
        std::snprintf(
            lines[1], sizeof(lines[1]), "%u x %u (%.2fx)", renderer.renderWidth(), renderer.renderHeight(),
            renderer.renderScale());
        std::snprintf(
            lines[2], sizeof(lines[2]), "%u draws, %u culled", renderer.drawCount() - renderer.culledDraws(),
            renderer.culledDraws());
        std::snprintf(lines[3], sizeof(lines[3]), "%zu entities", m_world.entityCount());
        for (const char* line : lines) {
            overlayText(draw, ImVec2(pos.x + 10.0f, y), text, line);
            y += ImGui::GetTextLineHeight() + 4.0f;
        }
    }
    (void)size;
}

void Editor::drawGizmo(const ImVec2& pos, const ImVec2& size) {
    const Entity primary = selected();
    Transform* t = m_world.get<Transform>(primary);
    if (!t || isPrefabPart(primary)) {   // parts follow their prefab
        m_gizmoWasUsing = false;
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const glm::mat4 view = m_camera.view();
    const glm::mat4 proj = gizmoProjection(size.x / size.y);
    // The gizmo works in world space; a child's Transform is relative to its parent.
    const Entity parent = shockwave::parentOf(m_world, primary);
    const glm::mat4 parentWorld = parent.valid() ? shockwave::worldMatrix(m_world, parent) : glm::mat4(1.0f);
    const glm::mat4 before = parentWorld * t->matrix();
    glm::mat4 model = before;
    const bool snap = m_prefs.snap != io.KeyCtrl;   // Ctrl flips snapping while held
    const float snapMove[3] = {m_prefs.snapMove, m_prefs.snapMove, m_prefs.snapMove};
    const float snapRotate[3] = {m_prefs.snapRotate, m_prefs.snapRotate, m_prefs.snapRotate};
    const float snapScale[3] = {m_prefs.snapScale, m_prefs.snapScale, m_prefs.snapScale};
    const float* snapValues = m_gizmoOperation == ImGuizmo::ROTATE  ? snapRotate
                              : m_gizmoOperation == ImGuizmo::SCALE ? snapScale
                                                                    : snapMove;
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(pos.x, pos.y, size.x, size.y);
    const bool changed = ImGuizmo::Manipulate(
        &view[0][0], &proj[0][0], m_gizmoOperation, m_gizmoMode, &model[0][0], nullptr, snap ? snapValues : nullptr);
    const bool using_ = ImGuizmo::IsUsing();
    if (using_ && !m_gizmoWasUsing) m_editStart = m_world;
    if (changed) {
        // The same change, in world space, for every other selected entity: they move,
        // turn or scale about the primary one as a group.
        const glm::mat4 delta = model * glm::inverse(before);
        auto apply = [&](Transform& target, const Transform& edited) {
            switch (m_gizmoOperation) {
            case ImGuizmo::ROTATE:
                target.rotation = edited.rotation;
                target.position = edited.position;
                break;
            case ImGuizmo::SCALE:
                target.scale = edited.scale;
                target.position = edited.position;
                break;
            default: target.position = edited.position; break;
            }
        };
        for (Entity e : selectionRoots()) {
            if (e == primary) continue;
            Transform* other = m_world.get<Transform>(e);
            if (!other) continue;
            const Entity p = shockwave::parentOf(m_world, e);
            const glm::mat4 pw = p.valid() ? shockwave::worldMatrix(m_world, p) : glm::mat4(1.0f);
            apply(*other, Transform::fromMatrix(glm::inverse(pw) * delta * pw * other->matrix()));
        }
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
}

bool Editor::worldToScreen(const glm::vec3& p, const ImVec2& pos, const ImVec2& size, ImVec2& out) const {
    const glm::vec4 clip = projection(size.x / size.y) * m_camera.view() * glm::vec4(p, 1.0f);
    if (clip.w <= m_camera.nearPlane) return false;   // behind the camera
    out = ImVec2(pos.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x, pos.y + (0.5f - clip.y / clip.w * 0.5f) * size.y);
    return true;
}

// A wireframe box round an entity's mesh or model.
void Editor::drawOutline(Entity e, const ImVec2& pos, const ImVec2& size, ImU32 color) {
    shockwave::Bounds box;
    if (!m_world.has<Transform>(e) || !entityBounds(e, box)) return;
    const shockwave::Bounds* b = &box;
    const glm::mat4 model = shockwave::worldMatrix(m_world, e);
    ImVec2 screen[8];
    bool visible[8];
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner(
            (i & 1) ? b->max.x : b->min.x, (i & 2) ? b->max.y : b->min.y, (i & 4) ? b->max.z : b->min.z);
        visible[i] = worldToScreen(glm::vec3(model * glm::vec4(corner, 1.0f)), pos, size, screen[i]);
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < 8; ++i) {
        for (int bit = 1; bit < 8; bit <<= 1) {
            const int j = i | bit;
            if (j == i || !visible[i] || !visible[j]) continue;
            draw->AddLine(screen[i], screen[j], color, 1.5f);
        }
    }
}

// Cameras have no mesh, so they get a small clickable icon, and a frustum when selected.
void Editor::drawCameraIcons(const ImVec2& pos, const ImVec2& size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const Entity primaryCamera = shockwave::primaryCamera(m_world);
    m_world.each<const shockwave::Camera, const Transform>([&](Entity e, const shockwave::Camera& camera,
                                                               const Transform&) {
        glm::mat4 view, proj;
        if (!shockwave::cameraMatrices(m_world, e, size.x / size.y, view, proj)) return;
        const glm::mat4 world = glm::inverse(view);
        const bool chosen = isSelected(e);
        const ImU32 color =
            chosen ? accentU32() : ImGui::GetColorU32(e == primaryCamera ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        if (chosen) {
            // What it sees, out to a couple of units.
            const float d = 2.0f;
            const float h = std::tan(glm::radians(camera.fov) * 0.5f) * d;
            const float w = h * size.x / size.y;
            const glm::vec3 apex(world[3]);
            const glm::vec3 local[4] = {{-w, -h, -d}, {w, -h, -d}, {w, h, -d}, {-w, h, -d}};
            ImVec2 corners[4];
            ImVec2 apexScreen;
            bool ok = worldToScreen(apex, pos, size, apexScreen);
            for (int i = 0; i < 4; ++i)
                ok = worldToScreen(glm::vec3(world * glm::vec4(local[i], 1.0f)), pos, size, corners[i]) && ok;
            if (ok) {
                for (int i = 0; i < 4; ++i) {
                    draw->AddLine(apexScreen, corners[i], color, 1.2f);
                    draw->AddLine(corners[i], corners[(i + 1) % 4], color, 1.2f);
                }
            }
        }
        ImVec2 c;
        if (!worldToScreen(glm::vec3(world[3]), pos, size, c)) return;
        // A little film camera: body and lens.
        const float s = 7.0f * dpi;
        draw->AddRectFilled(
            ImVec2(c.x - s, c.y - s * 0.7f), ImVec2(c.x + s * 0.5f, c.y + s * 0.7f),
            ImGui::GetColorU32(ImGuiCol_WindowBg, 0.6f));
        draw->AddRect(ImVec2(c.x - s, c.y - s * 0.7f), ImVec2(c.x + s * 0.5f, c.y + s * 0.7f), color, 0.0f, 0, 1.5f);
        draw->AddTriangleFilled(
            ImVec2(c.x + s * 0.5f, c.y), ImVec2(c.x + s * 1.2f, c.y - s * 0.6f), ImVec2(c.x + s * 1.2f, c.y + s * 0.6f),
            color);
    });
}

// The camera or light icon under the mouse, if any.
Entity Editor::cameraIconAt(const ImVec2& mouse, const ImVec2& pos, const ImVec2& size) const {
    const float radius = 11.0f * ImGui::GetStyle().FontScaleDpi;
    Entity best = kNullEntity;
    float bestDistance = radius * radius;
    auto consider = [&](std::span<const Entity> entities) {
        for (Entity e : entities) {
            if (!m_world.has<Transform>(e)) continue;
            ImVec2 c;
            if (!worldToScreen(glm::vec3(shockwave::worldMatrix(m_world, e)[3]), pos, size, c)) continue;
            const float dx = c.x - mouse.x, dy = c.y - mouse.y;
            if (dx * dx + dy * dy < bestDistance) {
                bestDistance = dx * dx + dy * dy;
                best = e;
            }
        }
    };
    if (const auto* cameras = m_world.pool<shockwave::Camera>()) consider(cameras->entities());
    if (const auto* lights = m_world.pool<shockwave::Light>()) consider(lights->entities());
    if (const auto* sounds = m_world.pool<shockwave::AudioSource>()) consider(sounds->entities());
    if (const auto* listeners = m_world.pool<shockwave::AudioListener>()) consider(listeners->entities());
    return best;
}

// Lights have no mesh either: an icon in the light's own color, and when selected, where
// it reaches (a range ring for point lights, the cone for spots, the direction for suns).
void Editor::drawLightIcons(const ImVec2& pos, const ImVec2& size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    m_world.each<const shockwave::Light, const Transform>(
        [&](Entity e, const shockwave::Light& light, const Transform&) {
            const glm::mat4 world = shockwave::worldMatrix(m_world, e);
            const glm::vec3 origin(world[3]);
            const glm::vec3 forward = glm::normalize(-glm::vec3(world[2]));
            const bool chosen = isSelected(e);
            const ImU32 outline =
                chosen ? accentU32() : ImGui::GetColorU32(light.enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            auto line = [&](const glm::vec3& a, const glm::vec3& b) {
                ImVec2 sa, sb;
                if (worldToScreen(a, pos, size, sa) && worldToScreen(b, pos, size, sb))
                    draw->AddLine(sa, sb, outline, 1.2f);
            };
            if (chosen) {
                using Type = shockwave::Light::Type;
                if (light.type == Type::Directional) {
                    line(origin, origin + forward * 3.0f);
                } else {
                    // A ring (point) or the cone's rim (spot), as 32 segments.
                    glm::vec3 side = glm::normalize(
                        glm::cross(forward, std::abs(forward.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
                    glm::vec3 up = glm::cross(side, forward);
                    glm::vec3 center = origin;
                    float radius = light.range;
                    if (light.type == Type::Spot) {
                        const float angle = glm::radians(std::clamp(light.spotAngle, 1.0f, 89.0f));
                        center = origin + forward * (light.range * std::cos(angle));
                        radius = light.range * std::sin(angle);
                    } else {
                        side = glm::vec3(1, 0, 0);   // point lights: a horizontal ring
                        up = glm::vec3(0, 0, 1);
                    }
                    glm::vec3 previous = center + side * radius;
                    for (int i = 1; i <= 32; ++i) {
                        const float a = glm::two_pi<float>() * static_cast<float>(i) / 32.0f;
                        const glm::vec3 next = center + (side * std::cos(a) + up * std::sin(a)) * radius;
                        line(previous, next);
                        if (light.type == Type::Spot && i % 8 == 0) line(origin, next);
                        previous = next;
                    }
                }
            }
            ImVec2 c;
            if (!worldToScreen(origin, pos, size, c)) return;
            const float s = 6.0f * dpi;
            const glm::vec3 tint = glm::clamp(glm::vec3(light.color), 0.0f, 1.0f);
            const ImU32 fill = ImGui::GetColorU32(ImVec4(tint.r, tint.g, tint.b, light.enabled ? 1.0f : 0.35f));
            draw->AddCircleFilled(c, s, fill);
            draw->AddCircle(c, s, outline, 0, 1.5f);
            if (light.type == shockwave::Light::Type::Directional) {
                // Rays round the disc: the sun.
                for (int i = 0; i < 8; ++i) {
                    const float a = glm::two_pi<float>() * static_cast<float>(i) / 8.0f;
                    const ImVec2 d(std::cos(a), std::sin(a));
                    draw->AddLine(
                        ImVec2(c.x + d.x * s * 1.4f, c.y + d.y * s * 1.4f),
                        ImVec2(c.x + d.x * s * 2.0f, c.y + d.y * s * 2.0f), outline, 1.5f);
                }
            }
        });
}

// The viewport as rendered, tonemapped, without icons or gizmos (the grid stays if shown).
void Editor::saveViewportScreenshot() {
    const std::string scene = m_scenePath.empty() ? std::string("untitled") : m_scenePath.stem().string();
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    namespace fs = std::filesystem;
    const fs::path folder = (m_hasProject ? m_project.root : fs::current_path()) / "Saved" / "Screenshots";
    std::error_code ec;
    fs::create_directories(folder, ec);
    m_screenshotPath = folder / (scene + "-" + stamp + ".png");
    engine().renderer().requestScreenshot(m_screenshotPath.string());
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
        // Rest it on the ground rather than half under it.
        const shockwave::Bounds* b = meshBounds(engine().assets().findMesh(mesh));
        at.y = b ? std::max(-b->min.y, 0.0f) : 0.0f;
        spawnMesh(mesh, at);
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kUIDocumentPayload))
        createUIDocument(static_cast<const char*>(payload->Data));
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPrefabPayload)) {
        glm::vec3 origin, dir;
        mouseRay(mouse, pos, size, origin, dir);
        glm::vec3 at = origin + dir * 6.0f;
        if (dir.y < -1e-4f) {
            const float t = -origin.y / dir.y;
            if (t > 0.0f && t < 200.0f) at = origin + dir * t;
        }
        placePrefab(static_cast<const char*>(payload->Data), at);
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAudioPayload)) {
        glm::vec3 origin, direction;
        mouseRay(mouse, pos, size, origin, direction);
        auto position = origin + direction * 6.0f;
        if (direction.y < -1e-4f) {
            const float distance = -origin.y / direction.y;
            if (distance > 0 && distance < 200) position = origin + direction * distance;
        }
        spawnAudio(static_cast<const char*>(payload->Data), position);
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kModelPayload)) {
        const std::string name = static_cast<const char*>(payload->Data);
        glm::vec3 origin, dir;
        mouseRay(mouse, pos, size, origin, dir);
        glm::vec3 at = origin + dir * 6.0f;
        if (dir.y < -1e-4f) {
            const float t = -origin.y / dir.y;
            if (t > 0.0f && t < 200.0f) at = origin + dir * t;
        }
        const auto model = engine().assets().model(engine().assets().findModel(name));
        at.y = model ? -model->data.bounds.min.y : 0.0f;
        spawnModel(name, at);
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
            kCircuitPayload, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
        const Entity target = pick(mouse, pos, size);
        if (payload->IsDelivery())
            attachCircuit(target, static_cast<const char*>(payload->Data));
        else
            drawOutline(target, pos, size, accentU32());
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
            kMaterialPayload, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
        // Onto the object under the cursor; on a model, onto the part under it.
        std::string part;
        const Entity target = pick(mouse, pos, size, &part);
        const std::string material = static_cast<const char*>(payload->Data);
        if (payload->IsDelivery()) {
            applyMaterial(target, material, part);
        } else {
            drawOutline(target, pos, size, accentU32());
            if (m_world.alive(target))
                ImGui::SetTooltip(
                    "%s on %s%s%s", material.c_str(), displayName(target).c_str(), part.empty() ? "" : ", part ",
                    part.c_str());
        }
    }
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
            kTexturePayload, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
        const Entity target = pick(mouse, pos, size);
        if (payload->IsDelivery()) {
            applyTexture(target, static_cast<const char*>(payload->Data));
        } else {
            drawOutline(target, pos, size, accentU32());
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
        if (m_camera.moveSpeed != m_prefs.cameraSpeed) {   // the wheel changes speed while flying
            m_prefs.cameraSpeed = m_camera.moveSpeed;
            m_prefsDirty = true;
        }
    } else {
        input.setMouseCaptured(false);
        // A gamepad flies the editor camera whenever the viewport has focus or the mouse.
        if ((hovered || ImGui::IsWindowFocused()) && !lookingThroughSceneCamera())
            m_camera.update(input, io.DeltaTime, shockwave::Gamepads);
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

void Editor::mouseRay(
    const ImVec2& mouse, const ImVec2& pos, const ImVec2& size, glm::vec3& origin, glm::vec3& dir) const {
    const glm::vec2 ndc(2.0f * (mouse.x - pos.x) / size.x - 1.0f, 1.0f - 2.0f * (mouse.y - pos.y) / size.y);
    const glm::mat4 invViewProj = glm::inverse(projection(size.x / size.y) * m_camera.view());
    glm::vec4 nearPoint = invViewProj * glm::vec4(ndc, 1.0f, 1.0f);   // reverse-Z: depth 1 is the near plane
    nearPoint /= nearPoint.w;
    origin = m_camera.position;
    dir = glm::normalize(glm::vec3(nearPoint) - origin);
}

// The nearest visible mesh under the mouse, tested against its actual triangles.
Entity Editor::pick(const ImVec2& mouse, const ImVec2& pos, const ImVec2& size, std::string* part) {
    glm::vec3 origin, dir;
    mouseRay(mouse, pos, size, origin, dir);

    Entity best = kNullEntity;
    float bestDistance = std::numeric_limits<float>::max();
    const shockwave::Assets& assets = engine().assets();
    m_world.each<const Transform, const MeshRenderer>([&](Entity e, const Transform&, const MeshRenderer& r) {
        if (!r.visible) return;
        const shockwave::Bounds* b = meshBounds(r.mesh);
        const auto data = assets.meshData(r.mesh);
        if (!b || !data) return;
        const glm::mat4 model = shockwave::worldMatrix(m_world, e);
        const glm::mat4 inv = glm::inverse(model);
        const glm::vec3 lo = glm::vec3(inv * glm::vec4(origin, 1.0f));
        const glm::vec3 ld = glm::vec3(inv * glm::vec4(dir, 0.0f));
        // A flat mesh has a zero-thickness box; pad it so the ray still finds it.
        const glm::vec3 pad(1e-3f);
        float t = 0.0f;
        if (!rayBox(lo, ld, b->min - pad, b->max + pad, t)) return;
        if (!shockwave::raycast(*data, lo, ld, t)) return;
        const glm::vec3 worldHit = glm::vec3(model * glm::vec4(lo + ld * t, 1.0f));
        const float distance = glm::length(worldHit - origin);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = e;
        }
    });
    m_world.each<const Transform, const shockwave::ModelRenderer>(
        [&](Entity e, const Transform&, const shockwave::ModelRenderer& r) {
            if (!r.visible) return;
            const auto model = assets.model(r.model);
            if (!model) return;
            const glm::mat4 matrix = shockwave::worldMatrix(m_world, e);
            if (std::abs(glm::determinant(matrix)) < 1e-10f) return;
            const glm::mat4 inv = glm::inverse(matrix);
            const glm::vec3 lo = glm::vec3(inv * glm::vec4(origin, 1.0f));
            const glm::vec3 ld = glm::vec3(inv * glm::vec4(dir, 0.0f));
            float t = 0.0f;
            uint32_t material = 0;
            if (!shockwave::intersectModel(model->data, lo, ld, t, &material)) return;
            const float distance = glm::length(glm::vec3(matrix * glm::vec4(lo + ld * t, 1.0f)) - origin);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = e;
                if (part) *part = material < model->data.materials.size() ? model->data.materials[material].name : "";
            }
        });
    if (part && !m_world.has<shockwave::ModelRenderer>(best)) part->clear();
    return best;
}

}   // namespace editor
