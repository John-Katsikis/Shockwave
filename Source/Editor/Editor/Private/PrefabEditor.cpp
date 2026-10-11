// Prefabs in the editor (Engine/Prefab.h): making one from an entity, placing copies,
// editing a prefab file the way a scene is edited, applying a copy's changes back to its
// prefab and unpacking a copy into ordinary entities.
#include "Editor.h"

#include <Engine/Hierarchy.h>
#include <Engine/Prefab.h>
#include <UI/Gui.h>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::Entity;
using shockwave::kNullEntity;
using shockwave::PrefabInstance;
using shockwave::Transform;

namespace {

void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

}   // namespace

bool Editor::isPrefabPart(Entity e) const { return m_world.has<shockwave::PrefabPart>(e); }

// Parts are remade from their prefab, so moving, renaming or deleting one on its own
// would be lost; say so instead.
bool Editor::refusePrefabParts(const char* action) {
    for (Entity e : selectionRoots()) {
        if (!isPrefabPart(e)) continue;
        const Entity root = shockwave::prefabRootOf(m_world, e);
        warn(
            "Can't " + std::string(action) + " " + displayName(e) + ": it's part of the prefab copy " +
            displayName(root) + ". Edit the prefab, or unpack the copy first");
        return true;
    }
    return false;
}

std::string Editor::newPrefabPath(const std::string& name) const {
    std::string base;
    for (const char c : name) base += std::string("/\\:*?\"<>|").find(c) == std::string::npos ? c : '_';
    if (base.empty()) base = "Prefab";
    std::error_code ec;
    for (int n = 1;; ++n) {
        const std::string path = "Prefabs/" + base + (n > 1 ? " " + std::to_string(n) : "") + ".swprefab";
        if (!fs::exists(m_project.assetFolder / path, ec)) return path;
    }
}

void Editor::createPrefab(Entity e) {
    if (!m_world.alive(e) || refusePrefabParts("make a prefab from")) return;
    const std::string path = newPrefabPath(displayName(e));
    std::string err;
    if (!shockwave::savePrefab(m_world, e, engine().assets(), m_project.assetFolder / path, err)) {
        error("Couldn't make a prefab: " + err);
        return;
    }
    shockwave::prefabs().reload(path);
    // e becomes a copy of the new prefab where it stands, keeping its place in the
    // hierarchy: what was under it is made again from the prefab, as parts.
    pushUndo(m_world);
    const std::vector<Entity> below = shockwave::subtree(m_world, e);
    for (size_t i = 1; i < below.size(); ++i) m_world.destroy(below[i]);
    m_world.add<PrefabInstance>(e, {path, nullptr});
    shockwave::updatePrefabInstances(m_world);
    select(e);
    log("Made the prefab " + m_project.relative(m_project.assetFolder / path) + "; " + displayName(e) +
        " is now a copy of it");
}

Entity Editor::placePrefab(const std::string& path, const glm::vec3& position) {
    std::string err;
    const auto prefab = shockwave::prefabs().load(path, &err);
    if (!prefab) {
        warn("Can't place " + path + ": " + err);
        return kNullEntity;
    }
    pushUndo(m_world);
    const Transform* own = prefab->world.get<Transform>(prefab->root);
    Transform at = own ? *own : Transform();
    at.position = position;
    const Entity root = shockwave::spawnPrefab(m_world, prefab, &at);
    const shockwave::Name* name = m_world.get<shockwave::Name>(root);
    m_world.add<shockwave::Name>(root, {uniqueName(name ? name->value : fs::path(path).stem().string())});
    select(root);
    log("Placed a copy of " + path);
    return root;
}

void Editor::applyPrefab(Entity root) {
    std::string err;
    if (!shockwave::applyPrefabInstance(m_world, root, engine().assets(), err)) {
        error("Couldn't apply to the prefab: " + err);
        return;
    }
    const std::string path = m_world.get<PrefabInstance>(root)->path;
    const size_t updated = shockwave::updatePrefabInstances(m_world);
    log("Applied " + displayName(root) + " to " + path + "; " + std::to_string(updated) + " cop" +
        (updated == 1 ? "y" : "ies") + " updated");
}

void Editor::unpackPrefabCopy(Entity root) {
    if (!m_world.has<PrefabInstance>(root) || isPrefabPart(root)) return;
    pushUndo(m_world);
    shockwave::unpackPrefab(m_world, root);
    log("Unpacked " + displayName(root) + "; its parts are ordinary entities now");
}

void Editor::editPrefab(const std::string& path) {
    if (!isEditingPrefab()) m_prefabReturnScene = m_scenePath;
    requestAction(PendingAction::OpenScene, (m_project.assetFolder / path).string());
}

bool Editor::isEditingPrefab() const { return m_scenePath.extension() == ".swprefab"; }

// Above the hierarchy while a prefab file is open: what saving it does, and the way back.
void Editor::drawPrefabEditingNote() {
    if (!isEditingPrefab()) return;
    ImGui::TextColored(gui::accent(), "Editing a prefab");
    ImGui::SetItemTooltip("The first entity is the root. Saving updates every copy");
    if (!m_prefabReturnScene.empty()) {
        const std::string back = "Back to " + m_prefabReturnScene.filename().string();
        gui::alignTrailing(gui::buttonWidth(back.c_str()));
        if (gui::button(back.c_str())) requestAction(PendingAction::OpenScene, m_prefabReturnScene.string());
    }
    ImGui::Separator();
}

// Under the entity's name in the Inspector. Returns true for a part, whose components
// are shown but can't be edited.
bool Editor::drawPrefabInspector(Entity e) {
    const bool part = isPrefabPart(e);
    const Entity root = part ? shockwave::prefabRootOf(m_world, e) : e;
    const PrefabInstance* instance = m_world.get<PrefabInstance>(root);
    if (!instance || (!part && instance->path.empty())) return part;
    // A part belongs to the nearest copy above it: that prefab file holds it.
    const Entity owner = part ? shockwave::prefabOwnerOf(m_world, e) : root;
    const PrefabInstance* holder = m_world.get<PrefabInstance>(owner);
    const std::string path = holder ? holder->path : instance->path;
    if (part) {
        ImGui::TextColored(gui::accent(), "Part of %s", displayName(root).c_str());
        hint(("From " + path + ". Edit the prefab to change it, or unpack the copy.").c_str());
        if (gui::button("Select copy")) select(root);
        gui::toolbarNext(gui::buttonWidth("Edit prefab"));
        if (gui::button("Edit prefab")) editPrefab(path);
    } else {
        ImGui::TextColored(gui::accent(), "Copy of %s", path.c_str());
        if (!instance->source) {
            ImGui::TextColored(gui::semanticColor(gui::currentTheme().error), "The prefab can't be loaded");
        } else {
            hint("Changes here are this copy's own. Apply sends them to every copy.");
        }
        ImGui::BeginDisabled(!instance->source);
        if (gui::button("Edit prefab")) editPrefab(path);
        gui::toolbarNext(gui::buttonWidth("Apply"));
        if (gui::button("Apply")) applyPrefab(root);
        ImGui::SetItemTooltip("Save this copy's components and added children into the prefab");
        ImGui::EndDisabled();
        gui::toolbarNext(gui::buttonWidth("Unpack"));
        if (gui::button("Unpack")) unpackPrefabCopy(root);
        ImGui::SetItemTooltip("Turn this copy into ordinary entities");
    }
    ImGui::Separator();
    return part;
}

void Editor::drawPrefabMenuItems(Entity e) {
    const bool single = selection().size() == 1;
    const bool part = isPrefabPart(e);
    const Entity root = part ? shockwave::prefabRootOf(m_world, e) : e;
    const PrefabInstance* instance = m_world.get<PrefabInstance>(root);
    if (ImGui::MenuItem("Make prefab", nullptr, false, single && !part)) createPrefab(e);
    if (!instance) return;
    if (part) {
        if (ImGui::MenuItem("Select prefab copy")) select(root);
        const PrefabInstance* holder = m_world.get<PrefabInstance>(shockwave::prefabOwnerOf(m_world, e));
        if (ImGui::MenuItem("Edit prefab", nullptr, false, holder != nullptr)) editPrefab(holder->path);
        return;
    }
    if (ImGui::MenuItem("Edit prefab", nullptr, false, single && instance->source != nullptr))
        editPrefab(instance->path);
    if (ImGui::MenuItem("Apply to prefab", nullptr, false, single && instance->source != nullptr)) applyPrefab(e);
    if (ImGui::MenuItem("Unpack prefab", nullptr, false, single)) unpackPrefabCopy(e);
}

}   // namespace editor
