// Selection and editing: creating, parenting, duplicating, deleting and grouping
// entities, undo/redo and the keyboard shortcuts.
#include "Editor.h"

#include <Engine/PostProcess.h>

#include <Assets/BuiltinAssets.h>
#include <Engine/BakedLighting.h>
#include <Engine/Decals.h>
#include <Engine/Prefab.h>
#include <Multiplayer/NetworkComponents.h>
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

// ---- Selection ----------------------------------------------------------------------------

void Editor::select(Entity e) {
    const bool alive = m_world.alive(e);
    const bool changed = !(m_selection.size() == 1 && m_selection[0] == e) && !(m_selection.empty() && !alive);
    m_selection.clear();
    if (alive) m_selection.push_back(e);
    m_rangeAnchor = alive ? e : kNullEntity;
    if (changed) m_revealSelection = true;
}

void Editor::toggleSelected(Entity e) {
    if (!m_world.alive(e)) return;
    if (isSelected(e)) {
        std::erase(m_selection, e);
    } else {
        m_selection.push_back(e);
        m_revealSelection = true;
    }
    m_rangeAnchor = e;
}

void Editor::selectRange(Entity to) {
    const auto a = std::find(m_hierarchyOrder.begin(), m_hierarchyOrder.end(), m_rangeAnchor);
    const auto b = std::find(m_hierarchyOrder.begin(), m_hierarchyOrder.end(), to);
    if (a == m_hierarchyOrder.end() || b == m_hierarchyOrder.end()) {
        select(to);
        return;
    }
    const Entity anchor = m_rangeAnchor;
    m_selection.assign(std::min(a, b), std::max(a, b) + 1);
    std::erase(m_selection, to);
    m_selection.push_back(to);   // the clicked row is the primary
    m_rangeAnchor = anchor;
}

void Editor::selectAll() {
    m_selection.clear();
    m_world.forEachEntity([&](Entity e) { m_selection.push_back(e); });
}

bool Editor::isSelected(Entity e) const {
    return std::find(m_selection.begin(), m_selection.end(), e) != m_selection.end();
}

Entity Editor::selected() const {
    for (auto it = m_selection.rbegin(); it != m_selection.rend(); ++it) {
        if (m_world.alive(*it)) return *it;
    }
    return kNullEntity;
}

std::vector<Entity> Editor::selection() const {
    std::vector<Entity> live;
    for (Entity e : m_selection) {
        if (m_world.alive(e)) live.push_back(e);
    }
    return live;
}

std::vector<Entity> Editor::selectionRoots() const {
    const std::vector<Entity> all = selection();
    std::vector<Entity> roots;
    for (Entity e : all) {
        const bool inside = std::any_of(all.begin(), all.end(), [&](Entity other) {
            return !(other == e) && shockwave::isAncestor(m_world, other, e);
        });
        if (!inside) roots.push_back(e);
    }
    return roots;
}

// ---- Editing ------------------------------------------------------------------------------

std::string Editor::displayName(Entity e) const {
    const Name* name = m_world.get<Name>(e);
    return name && !name->value.empty() ? name->value : "Entity " + std::to_string(e.index);
}

std::string Editor::kindLabel(Entity e) {
    if (m_world.has<shockwave::PrefabInstance>(e)) return "Prefab";
    if (m_world.has<shockwave::Camera>(e)) return "Camera";
    if (m_world.has<shockwave::Environment>(e)) return "Environment";
    if (const auto* light = m_world.get<shockwave::Light>(e)) {
        static const char* kinds[] = {"Directional light", "Point light", "Spot light"};
        const int type = static_cast<int>(light->type);
        return type >= 0 && type < 3 ? kinds[type] : "Light";
    }
    if (const MeshRenderer* r = m_world.get<MeshRenderer>(e)) {
        const std::string mesh = engine().assets().meshName(r->mesh);
        return mesh.empty() ? "Mesh" : fs::path(mesh).stem().string();
    }
    if (const auto* m = m_world.get<shockwave::ModelRenderer>(e)) {
        const std::string model = engine().assets().modelName(m->model);
        return model.empty() ? "Model" : fs::path(model).stem().string();
    }
    if (m_world.has<shockwave::ParticleEffect>(e)) return "Particle effect";
    if (m_world.has<shockwave::PostProcessVolume>(e)) return "Postprocess volume";
    if (m_world.has<shockwave::CharacterController>(e)) return "Character";
    if (m_world.has<shockwave::PhysicsSettings>(e)) return "Physics";
    if (m_world.has<shockwave::Collider>(e)) {
        const auto* body = m_world.get<shockwave::RigidBody>(e);
        return body && body->sensor ? "Trigger" : "Collider";
    }
    return {};
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

glm::vec3 Editor::placementPoint() const {
    // Where the view meets the ground, if that's reasonably close; otherwise a few units
    // ahead, dropped to the ground.
    const glm::vec3 forward = m_camera.forward();
    glm::vec3 at = m_camera.position + forward * 6.0f;
    if (forward.y < -0.05f) {
        const float t = -m_camera.position.y / forward.y;
        if (t > 0.0f && t < 40.0f) at = m_camera.position + forward * t;
    }
    at.y = 0.0f;
    if (m_prefs.snap && m_prefs.snapMove > 0.0f) {
        at.x = std::round(at.x / m_prefs.snapMove) * m_prefs.snapMove;
        at.z = std::round(at.z / m_prefs.snapMove) * m_prefs.snapMove;
    }
    return at;
}

Entity Editor::createEntity(CreateKind kind, Entity parent) {
    if (parent.valid() && !m_world.alive(parent)) parent = kNullEntity;
    pushUndo(m_world);
    const Entity e = m_world.create();
    const shockwave::Assets& assets = engine().assets();
    const glm::vec3 ground = placementPoint();
    auto addMesh = [&](const char* name, const char* mesh, float lift) {
        m_world.add<Name>(e, {uniqueName(name)});
        m_world.add<Transform>(e, {parent.valid() ? glm::vec3(0.0f) : ground + glm::vec3(0.0f, lift, 0.0f)});
        MeshRenderer renderer;
        renderer.mesh = assets.findMesh(mesh);
        m_world.add<MeshRenderer>(e, renderer);
    };
    switch (kind) {
    case CreateKind::Empty:
        m_world.add<Name>(e, {uniqueName("Entity")});
        m_world.add<Transform>(e, {parent.valid() ? glm::vec3(0.0f) : ground});
        break;
    case CreateKind::Cube: addMesh("Cube", "cube", 0.5f); break;
    case CreateKind::Sphere: addMesh("Sphere", "sphere", 0.5f); break;
    case CreateKind::Plane: addMesh("Plane", "plane", 0.0f); break;
    case CreateKind::Camera: {
        // Starts out seeing what the viewport sees.
        m_world.add<Name>(e, {uniqueName("Camera")});
        Transform t = Transform::fromMatrix(glm::inverse(m_camera.view()));
        if (parent.valid())
            t = Transform::fromMatrix(glm::inverse(shockwave::worldMatrix(m_world, parent)) * t.matrix());
        m_world.add<Transform>(e, t);
        shockwave::Camera camera;
        camera.primary = !shockwave::primaryCamera(m_world).valid();
        m_world.add<shockwave::Camera>(e, camera);
        break;
    }
    case CreateKind::DirectionalLight:
    case CreateKind::PointLight:
    case CreateKind::SpotLight: {
        shockwave::Light light;
        Transform t;
        if (kind == CreateKind::DirectionalLight) {
            // Only the direction matters; slanted like an afternoon sun, above the scene.
            m_world.add<Name>(e, {uniqueName("Sun")});
            light.type = shockwave::Light::Type::Directional;
            light.intensity = 2.5f;
            t.position = glm::vec3(0.0f, 10.0f, 0.0f);
            t.rotation = shockwave::fromEulerDegrees({-50.0f, 35.0f, 0.0f});
        } else if (kind == CreateKind::PointLight) {
            m_world.add<Name>(e, {uniqueName("Point light")});
            light.type = shockwave::Light::Type::Point;
            light.intensity = 4.0f;
            light.range = 8.0f;
            t.position = placementPoint() + glm::vec3(0.0f, 2.0f, 0.0f);
        } else {
            // Above where the camera looks, pointing down at it.
            m_world.add<Name>(e, {uniqueName("Spot light")});
            light.type = shockwave::Light::Type::Spot;
            light.intensity = 10.0f;
            light.range = 12.0f;
            t.position = placementPoint() + glm::vec3(0.0f, 4.0f, 0.0f);
            t.rotation = shockwave::fromEulerDegrees({-90.0f, 0.0f, 0.0f});
        }
        if (parent.valid())
            t = Transform::fromMatrix(glm::inverse(shockwave::worldMatrix(m_world, parent)) * t.matrix());
        m_world.add<Transform>(e, t);
        m_world.add<shockwave::Light>(e, light);
        break;
    }
    case CreateKind::Environment:
        m_world.add<Name>(e, {uniqueName("Environment")});
        m_world.add<shockwave::Environment>(e);
        if (m_world.count<shockwave::Environment>() > 1) warn("The scene now has two Environments; only one is used");
        break;
    }
    if (parent.valid() && m_world.has<Transform>(e)) m_world.add<Parent>(e, {parent});
    select(e);
    log("Created " + displayName(e) + (parent.valid() ? " under " + displayName(parent) : ""));
    return e;
}

Entity Editor::spawnMesh(const std::string& mesh, const glm::vec3& position) {
    pushUndo(m_world);
    const Entity e = m_world.create();
    std::string base = fs::path(mesh).stem().string();
    if (!base.empty()) base[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(base[0])));
    m_world.add<Name>(e, {uniqueName(base)});
    m_world.add<Transform>(e, {position});
    MeshRenderer renderer;
    renderer.mesh = engine().assets().findMesh(mesh);
    m_world.add<MeshRenderer>(e, renderer);
    select(e);
    log("Placed " + displayName(e));
    return e;
}

Entity Editor::spawnModel(const std::string& model, const glm::vec3& position) {
    const auto handle = engine().assets().findModel(model);
    if (!handle.valid()) {
        warn(model + " hasn't been imported");
        return kNullEntity;
    }
    pushUndo(m_world);
    const Entity e = m_world.create();
    m_world.add<Name>(e, {uniqueName(fs::path(model).stem().string())});
    m_world.add<Transform>(e, {position});
    m_world.add<shockwave::ModelRenderer>(e, {handle});
    addAnimatorIfAnimated(e);
    select(e);
    log("Placed " + displayName(e));
    return e;
}

bool Editor::reparent(Entity child, Entity parent) {
    if (!m_world.alive(child) || shockwave::parentOf(m_world, child) == parent) return false;
    if (isPrefabPart(child)) {
        warn("Can't move " + displayName(child) + " out of its prefab copy; edit the prefab, or unpack the copy");
        return false;
    }
    if (parent.valid() && (parent == child || shockwave::isAncestor(m_world, child, parent))) {
        warn("Can't put " + displayName(child) + " under " + displayName(parent) + ": it's inside it");
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

// Dragging one of several selected rows moves the whole selection.
bool Editor::reparentSelection(Entity dragged, Entity parent) {
    if (!isSelected(dragged) || selection().size() < 2) return reparent(dragged, parent);
    if (refusePrefabParts("move")) return false;
    shockwave::World before = m_world;
    int moved = 0;
    for (Entity e : selectionRoots()) {
        if (e == parent || shockwave::parentOf(m_world, e) == parent) continue;
        if (parent.valid() && shockwave::isAncestor(m_world, e, parent)) continue;
        moved += shockwave::setParent(m_world, e, parent) ? 1 : 0;
    }
    if (moved == 0) return false;
    pushUndo(std::move(before));
    log(
        (parent.valid() ? "Attached " + std::to_string(moved) + " entities to " + displayName(parent)
                        : "Detached " + std::to_string(moved) + " entities"));
    return true;
}

bool Editor::applyTexture(Entity e, const std::string& texture) {
    MeshRenderer* renderer = m_world.get<MeshRenderer>(e);
    const auto handle = engine().assets().findTexture(texture);
    if (!renderer || !handle) {
        if (m_world.alive(e)) warn(displayName(e) + " has no MeshRenderer to show " + texture + " on");
        return false;
    }
    pushUndo(m_world);
    renderer->texture = *handle;
    log("Gave " + displayName(e) + " the texture " + texture);
    return true;
}

Entity Editor::placeDecal(const std::string& asset, bool material, const glm::vec3& point, const glm::vec3& normal) {
    shockwave::Decal decal;
    if (material) {
        decal.material = engine().assets().findMaterial(asset);
        if (!decal.material.valid()) {
            warn(asset + " isn't loaded yet");
            return kNullEntity;
        }
    } else {
        const auto handle = engine().assets().findTexture(asset);
        if (!handle) {
            warn(asset + " isn't loaded yet");
            return kNullEntity;
        }
        decal.texture = *handle;
    }
    // A metre square, reaching 10 cm either side of the surface: enough to cover small
    // bumps without going through the wall to the room behind.
    decal.size = {1.0f, 1.0f, 0.2f};
    pushUndo(m_world);
    const Entity e = m_world.create();
    m_world.add<Name>(e, {uniqueName(std::filesystem::path(asset).stem().string() + " decal")});
    m_world.add<Transform>(e, {point, shockwave::impactDecalRotation(normal, 0), glm::vec3(1.0f)});
    m_world.add<shockwave::Decal>(e, decal);
    select(e);
    log("Placed a decal of " + asset);
    return e;
}

void Editor::duplicateSelected() {
    const std::vector<Entity> roots = selectionRoots();
    if (roots.empty() || refusePrefabParts("duplicate")) return;
    pushUndo(m_world);
    std::vector<Entity> copies;
    for (Entity e : roots) {
        const Entity copy = shockwave::cloneWithChildren(m_world, e);
        if (Name* name = m_world.get<Name>(copy)) name->value = uniqueName(name->value);
        copies.push_back(copy);
    }
    // Copies of network objects are new network objects (docs/Networking.md).
    if (!isEditingPrefab()) shockwave::net::renewNetworkKeys(m_world, copies);
    // And copies of baked objects are new bake participants, with their own lighting.
    for (Entity copy : copies) shockwave::refreshLightingIds(m_world, copy);
    m_selection = copies;
    m_rangeAnchor = copies.back();
    log(copies.size() == 1 ? "Duplicated as " + displayName(copies[0])
                           : "Duplicated " + std::to_string(copies.size()) + " entities");
}

void Editor::deleteSelected() {
    const std::vector<Entity> roots = selectionRoots();
    if (roots.empty() || refusePrefabParts("delete")) return;
    pushUndo(m_world);
    size_t count = 0;
    const std::string first = displayName(roots[0]);
    for (Entity e : roots) {
        count += shockwave::subtree(m_world, e).size();
        shockwave::destroyWithChildren(m_world, e);
    }
    select(kNullEntity);
    log(count == 1 ? "Deleted " + first : "Deleted " + std::to_string(count) + " entities");
}

// Puts the selection under a new empty entity at their centre, without moving anything.
void Editor::groupSelected() {
    const std::vector<Entity> roots = selectionRoots();
    if (roots.empty() || refusePrefabParts("group")) return;
    pushUndo(m_world);
    Entity commonParent = shockwave::parentOf(m_world, roots[0]);
    glm::vec3 centre(0.0f);
    for (Entity e : roots) {
        if (!(shockwave::parentOf(m_world, e) == commonParent)) commonParent = kNullEntity;
        centre += glm::vec3(shockwave::worldMatrix(m_world, e)[3]);
    }
    centre /= static_cast<float>(roots.size());
    const Entity group = m_world.create();
    m_world.add<Name>(group, {uniqueName("Group")});
    const glm::mat4 parentWorld =
        commonParent.valid() ? shockwave::worldMatrix(m_world, commonParent) : glm::mat4(1.0f);
    m_world.add<Transform>(group, {glm::vec3(glm::inverse(parentWorld) * glm::vec4(centre, 1.0f))});
    if (commonParent.valid()) m_world.add<Parent>(group, {commonParent});
    for (Entity e : roots) shockwave::setParent(m_world, e, group);
    select(group);
    log("Grouped " + std::to_string(roots.size()) + " entities under " + displayName(group));
}

// Moves the selection to the editor camera: the quick way to set up a game camera.
void Editor::alignSelectedToView() {
    const Entity e = selected();
    Transform* t = m_world.get<Transform>(e);
    if (!t) return;
    pushUndo(m_world);
    const Entity parent = shockwave::parentOf(m_world, e);
    const glm::mat4 parentWorld = parent.valid() ? shockwave::worldMatrix(m_world, parent) : glm::mat4(1.0f);
    const Transform local = Transform::fromMatrix(glm::inverse(parentWorld) * glm::inverse(m_camera.view()));
    t->position = local.position;
    t->rotation = local.rotation;
    log("Moved " + displayName(e) + " to the view");
}

// Frames everything selected: the world-space box round their meshes (or positions).
void Editor::focusSelected() {
    const std::vector<Entity> all = selection();
    bool any = false;
    glm::vec3 lo(0.0f), hi(0.0f);
    auto grow = [&](const glm::vec3& p) {
        lo = any ? glm::min(lo, p) : p;
        hi = any ? glm::max(hi, p) : p;
        any = true;
    };
    for (Entity e : all) {
        if (!m_world.has<Transform>(e)) continue;
        const glm::mat4 m = shockwave::worldMatrix(m_world, e);
        shockwave::Bounds box;
        if (!entityBounds(e, box)) {
            grow(glm::vec3(m[3]));
            continue;
        }
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 corner(
                (i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y, (i & 4) ? box.max.z : box.min.z);
            grow(glm::vec3(m * glm::vec4(corner, 1.0f)));
        }
    }
    if (!any) return;
    const glm::vec3 centre = (lo + hi) * 0.5f;
    const float radius = std::max(glm::length(hi - lo) * 0.5f, 0.5f);
    const float distance = std::clamp(radius / std::tan(m_camera.fovY * 0.5f) * 1.1f, 1.5f, 2000.0f);
    m_camera.position = centre - m_camera.forward() * distance;
}

void Editor::beginRename(Entity e) {
    if (!m_world.alive(e) || isPrefabPart(e)) return;
    m_renaming = e;
    m_renameBuffer = displayName(e);
    m_focusRename = true;
    m_revealSelection = true;
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
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, global)) requestAction(PendingAction::NewBasicScene);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, global)) requestAction(PendingAction::OpenSceneDialog);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global)) saveScene();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, global)) showSaveSceneDialog();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, global)) undo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, global) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, global)) {
        redo();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, global)) playState() == PlayState::Playing ? stop() : play();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P, global)) pause();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Comma, global)) m_showPreferences = true;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_B, global)) buildGameCode();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_R, global) && engine().renderer().swappable())
        engine().reloadRenderer();

    // The rest act on the scene, so not while typing.
    if (io.WantTextInput) return;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, global)) duplicateSelected();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A, global)) selectAll();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_G, global)) groupSelected();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F, global)) alignSelectedToView();

    // Single keys only when the camera isn't flying.
    if (m_looking || io.KeyCtrl || io.KeyAlt) return;
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_gizmoOperation = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_gizmoOperation = ImGuizmo::ROTATE;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_gizmoOperation = ImGuizmo::SCALE;
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        m_gizmoMode = m_gizmoMode == ImGuizmo::WORLD ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_G, false) && !io.KeyShift) {
        m_prefs.showGrid = !m_prefs.showGrid;
        m_prefsDirty = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) focusSelected();
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) beginRename(selected());
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) m_viewThroughCamera = !m_viewThroughCamera;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) deleteSelected();
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) m_showControls = !m_showControls;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        select(kNullEntity);
    }
}

}   // namespace editor
