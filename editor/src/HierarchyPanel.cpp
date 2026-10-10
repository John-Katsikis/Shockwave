// The Hierarchy panel: the scene's entities as a tree. Click, Ctrl+click and Shift+click
// select; drag rows onto each other to attach them; F2 or the context menu renames.
#include "Editor.h"

#include <shockwave/Gui.h>

#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>

namespace editor {

namespace gui = shockwave::gui;
using shockwave::Entity;
using shockwave::kNullEntity;
using shockwave::MeshRenderer;
using shockwave::Name;

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

} // namespace

void Editor::drawHierarchy() {
    if (!ImGui::Begin("Hierarchy", &m_showHierarchy)) {
        ImGui::End();
        return;
    }

    // Search leads the header; creating trails it (docs/UI.md).
    const float createWidth = gui::buttonWidth("+ Add");
    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - createWidth - ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##filter", "Search entities", &m_hierarchyFilter);
    gui::alignTrailing(createWidth);
    if (gui::button("+ Add")) ImGui::OpenPopup("CreateEntity");
    ImGui::SetItemTooltip("Create an entity");
    if (ImGui::BeginPopup("CreateEntity")) {
        drawCreateMenuItems(kNullEntity);
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
        // Searching shows a flat list of matches, wherever they are in the tree.
        const std::string filter = lower(m_hierarchyFilter);
        std::vector<Entity> matches;
        m_world.forEachEntity([&](Entity e) {
            if (lower(displayName(e)).find(filter) != std::string::npos ||
                lower(kindLabel(e)).find(filter) != std::string::npos) {
                matches.push_back(e);
            }
        });
        for (const Entity e : matches) {
            if (m_world.alive(e)) drawHierarchyRow(e, false);
        }
        if (matches.empty()) ImGui::TextDisabled("Nothing matches \"%s\"", m_hierarchyFilter.c_str());
    }
    m_revealSelection = false;

    // The empty space below the list: click to clear the selection, right-click to create,
    // drop an entity on it to move it back to the top level.
    const ImVec2 rest = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##background", ImVec2(std::max(rest.x, 1.0f), std::max(rest.y, ImGui::GetFrameHeight())));
    if (ImGui::IsItemClicked()) select(kNullEntity);
    if (ImGui::BeginPopupContextItem("background")) {
        drawCreateMenuItems(kNullEntity);
        ImGui::EndPopup();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            reparentSelection(*static_cast<const Entity*>(payload->Data), kNullEntity);
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

// One entity's row: visibility box, its name as a tree node (or a text field while
// renaming), and what kind of thing it is, dimmed on the right.
bool Editor::drawHierarchyRow(Entity e, bool hasChildren) {
    ImGui::PushID(static_cast<int>(e.index));
    m_hierarchyOrderNext.push_back(e);
    MeshRenderer* renderer = m_world.get<MeshRenderer>(e);
    auto* model = m_world.get<shockwave::ModelRenderer>(e);
    bool* visible = renderer ? &renderer->visible : model ? &model->visible : nullptr;
    const bool hidden = visible && !*visible;
    if (visible) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
        ImGui::Checkbox("##visible", visible);
        ImGui::PopStyleVar();
        trackEdit();
        ImGui::SetItemTooltip("Visible");
    } else {
        ImGui::Dummy(ImVec2(ImGui::GetTextLineHeight() + 4.0f, ImGui::GetTextLineHeight() + 4.0f));   // keep names lined up
    }
    ImGui::SameLine(0.0f, 6.0f);

    const bool renaming = m_renaming == e;
    const std::string name = displayName(e);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_DefaultOpen |
                               ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_AllowOverlap;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
    if (isSelected(e)) flags |= ImGuiTreeNodeFlags_Selected;
    if (hidden) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
    // Open the way down to an entity picked in the viewport.
    if (hasChildren && m_revealSelection) {
        for (Entity s : m_selection) {
            if (shockwave::isAncestor(m_world, e, s)) ImGui::SetNextItemOpen(true);
        }
    }
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", renaming ? "" : name.c_str());
    ImGui::PopStyleVar();
    if (hidden) ImGui::PopStyleColor();
    const ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) {
            toggleSelected(e);
        } else if (io.KeyShift) {
            selectRange(e);
        } else {
            select(e);
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !renaming) focusSelected();
    if (!renaming && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kEntityPayload, &e, sizeof(e));
        const size_t count = isSelected(e) ? selection().size() : 1;
        if (count > 1) {
            ImGui::Text("%s and %zu more", name.c_str(), count - 1);
        } else {
            ImGui::Text("%s", name.c_str());
        }
        ImGui::TextDisabled("Drop on an entity to attach, or below the list to detach");
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            reparentSelection(*static_cast<const Entity*>(payload->Data), e);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kTexturePayload)) {
            applyTexture(e, static_cast<const char*>(payload->Data));
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("row")) {
        drawEntityContextMenu(e);
        ImGui::EndPopup();
    }

    // What it is, on the right.
    std::string kind = kindLabel(e);
    if (kind.empty() && hasChildren) kind = "Group";
    if (!kind.empty() && !renaming) {
        const float width = ImGui::CalcTextSize(kind.c_str()).x;
        const float nameEnd = rowMin.x + ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(name.c_str()).x;
        const float x = rowMax.x - width - 6.0f;
        if (x > nameEnd + 12.0f) {
            ImGui::GetWindowDrawList()->AddText(ImVec2(x, rowMin.y + (rowMax.y - rowMin.y - ImGui::GetTextLineHeight()) * 0.5f),
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled), kind.c_str());
        }
    }

    if (renaming) {
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x + ImGui::GetTreeNodeToLabelSpacing() - 4.0f, rowMin.y));
        ImGui::SetNextItemWidth(rowMax.x - ImGui::GetCursorScreenPos().x);
        if (m_focusRename) {
            ImGui::SetKeyboardFocusHere();
            m_focusRename = false;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
        const bool done = ImGui::InputText("##rename", &m_renameBuffer,
                                           ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopStyleVar();
        const bool cancelled = ImGui::IsKeyPressed(ImGuiKey_Escape);
        if (cancelled) {
            m_renaming = kNullEntity;
        } else if (done || ImGui::IsItemDeactivated()) {
            if (m_renameBuffer != name && !m_renameBuffer.empty()) {
                pushUndo(m_world);
                if (Name* n = m_world.get<Name>(e)) {
                    n->value = m_renameBuffer;
                } else {
                    m_world.add<Name>(e, {m_renameBuffer});
                }
            }
            m_renaming = kNullEntity;
        }
    }
    ImGui::PopID();
    return open;
}

void Editor::drawEntityContextMenu(Entity e) {
    if (!isSelected(e)) select(e);
    const size_t count = selection().size();
    if (count > 1) ImGui::TextDisabled("%zu selected", count);
    if (ImGui::BeginMenu("Create child", count == 1)) {
        drawCreateMenuItems(e);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Rename", "F2", false, count == 1)) beginRename(e);
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicateSelected();
    if (ImGui::MenuItem("Group", "Ctrl+G")) groupSelected();
    if (ImGui::MenuItem("Detach from parent", nullptr, false, shockwave::parentOf(m_world, e).valid())) {
        reparentSelection(e, kNullEntity);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Focus", "F")) focusSelected();
    if (ImGui::MenuItem("Move to view", "Ctrl+Shift+F", false, count == 1 && m_world.has<shockwave::Transform>(e))) {
        alignSelectedToView();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete", "Del")) deleteSelected();
}

} // namespace editor
