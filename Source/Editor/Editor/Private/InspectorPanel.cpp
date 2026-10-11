// The Inspector panel. It knows no component types: it draws whatever is registered, from
// the fields each component declared (see shockwave/Components.h), so a game's own
// components show up here as soon as they're registered.
#include "Editor.h"

#include <Physics/Physics.h>
#include <UI/Gui.h>

#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace editor {

namespace gui = shockwave::gui;
using shockwave::ComponentInfo;
using shockwave::Entity;
using shockwave::Field;
using shockwave::FieldType;
using shockwave::kNullEntity;
using shockwave::Name;
using shockwave::Parent;

namespace {

// "sky_color" -> "Sky color"
std::string prettyName(const std::string& name) {
    std::string s = name;
    for (char& c : s) {
        if (c == '_') c = ' ';
    }
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

}   // namespace

void Editor::drawInspector() {
    if (!ImGui::Begin("Inspector", &m_showInspector)) {
        ImGui::End();
        return;
    }
    const std::vector<Entity> all = selection();
    if (all.empty()) {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::TextDisabled("Click an entity in the viewport or the hierarchy.");
        ImGui::End();
        return;
    }
    if (all.size() > 1) {
        ImGui::Text("%zu entities selected", all.size());
        ImGui::Spacing();
        const size_t shown = std::min<size_t>(all.size(), 12);
        for (size_t i = 0; i < shown; ++i) ImGui::BulletText("%s", displayName(all[i]).c_str());
        if (all.size() > shown) ImGui::TextDisabled("and %zu more", all.size() - shown);
        ImGui::Spacing();
        const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
        if (gui::button("Group", ImVec2(width, 0))) groupSelected();
        ImGui::SameLine();
        if (gui::button("Duplicate", ImVec2(width, 0))) duplicateSelected();
        ImGui::SameLine();
        if (gui::button("Delete", ImVec2(width, 0))) deleteSelected();
        ImGui::Spacing();
        ImGui::TextDisabled("The gizmo moves them all together.");
        ImGui::End();
        return;
    }

    const Entity e = all[0];
    // Identity first, with component creation beside it (docs/UI.md).
    const float addWidth = gui::buttonWidth("+ Component");
    ImGui::SetNextItemWidth(
        std::max(1.0f, ImGui::GetContentRegionAvail().x - addWidth - ImGui::GetStyle().ItemSpacing.x));
    if (Name* name = m_world.get<Name>(e)) {
        ImGui::InputText("##name", &name->value);
        trackEdit();
    } else {
        std::string empty;
        if (ImGui::InputTextWithHint("##name", "Name", &empty, ImGuiInputTextFlags_EnterReturnsTrue) &&
            !empty.empty()) {
            pushUndo(m_world);
            m_world.add<Name>(e, {empty});
        }
    }
    gui::alignTrailing(addWidth);
    if (gui::button("+ Component")) {
        m_componentFilter.clear();
        ImGui::OpenPopup("AddComponent");
    }
    drawAddComponentPopup(e);
    const std::string kind = kindLabel(e);
    ImGui::TextDisabled("%s%sEntity %u", kind.c_str(), kind.empty() ? "" : "  |  ", e.index);
    ImGui::Separator();

    const uint32_t nameId = shockwave::componentId<Name>();
    for (const ComponentInfo& info : shockwave::registeredComponents()) {
        if (info.id == nameId || !info.has(m_world, e)) continue;
        drawComponent(info, e);
        if (!m_world.alive(e)) break;
    }
    ImGui::End();
}

// Searchable, grouped by category ("Rendering", "Motion", the game's own...).
void Editor::drawAddComponentPopup(Entity e) {
    ImGui::SetNextWindowSize(ImVec2(260.0f * ImGui::GetStyle().FontScaleDpi, 0.0f));
    if (!ImGui::BeginPopup("AddComponent")) return;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search components", &m_componentFilter);
    const std::string filter = lower(m_componentFilter);

    std::map<std::string, std::vector<const ComponentInfo*>> byCategory;
    const uint32_t nameId = shockwave::componentId<Name>();
    for (const ComponentInfo& info : shockwave::registeredComponents()) {
        if (info.id == nameId || info.has(m_world, e)) continue;
        if (!filter.empty() && lower(info.name).find(filter) == std::string::npos &&
            lower(info.category).find(filter) == std::string::npos) {
            continue;
        }
        byCategory[info.category.empty() ? "Other" : info.category].push_back(&info);
    }
    if (byCategory.empty()) ImGui::TextDisabled(filter.empty() ? "It has every component already" : "No matches");
    const ComponentInfo* chosen = nullptr;
    for (const auto& [category, list] : byCategory) {
        ImGui::SeparatorText(category.c_str());
        for (const ComponentInfo* info : list) {
            if (ImGui::Selectable(info->name.c_str())) chosen = info;
        }
    }
    // Enter takes the only match.
    if (!chosen && ImGui::IsKeyPressed(ImGuiKey_Enter) && byCategory.size() == 1 &&
        byCategory.begin()->second.size() == 1) {
        chosen = byCategory.begin()->second[0];
    }
    if (chosen) {
        pushUndo(m_world);
        if (chosen->id == shockwave::componentId<Parent>()) {
            m_world.add<Parent>(e);
        } else {
            chosen->add(m_world, e);
        }
        log("Added " + chosen->name + " to " + displayName(e));
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Editor::drawComponent(const ComponentInfo& info, Entity e) {
    ImGui::PushID(info.name.c_str());
    ImGui::Spacing();
    const bool open =
        ImGui::CollapsingHeader(info.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
    // A menu button at the right end of the header.
    const float buttonWidth = ImGui::CalcTextSize("...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth);
    if (ImGui::SmallButton("...")) ImGui::OpenPopup("ComponentMenu");
    ImGui::SetItemTooltip("%s options", info.name.c_str());
    bool removed = false;
    if (ImGui::BeginPopup("ComponentMenu")) {
        if (ImGui::MenuItem("Reset to defaults")) {
            pushUndo(m_world);
            if (info.id == shockwave::componentId<shockwave::Transform>()) {
                *m_world.get<shockwave::Transform>(e) = shockwave::Transform();
            } else if (info.id != shockwave::componentId<Parent>()) {
                info.remove(m_world, e);
                info.add(m_world, e);
            }
            log("Reset " + info.name + " on " + displayName(e));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Remove component")) {
            pushUndo(m_world);
            if (info.id == shockwave::componentId<Parent>()) {
                shockwave::setParent(m_world, e, kNullEntity);   // detach without jumping
            } else {
                info.remove(m_world, e);
            }
            log("Removed " + info.name + " from " + displayName(e));
            removed = true;
        }
        ImGui::EndPopup();
    }
    if (open && !removed && info.has(m_world, e)) {
        char* data = static_cast<char*>(info.get(m_world, e));
        if (info.fields.empty()) ImGui::TextDisabled("No settings");
        if (!info.fields.empty() && gui::beginPropertyGrid("fields")) {
            for (const Field& f : info.fields) drawField(f, data + f.offset, e);
            ImGui::EndTable();
        }
        drawComponentNotes(info, e);
    }
    ImGui::PopID();
}

// What a component's fields can't show: physics setup problems, imported materials.
void Editor::drawComponentNotes(const ComponentInfo& info, Entity e) {
    if (info.id == shockwave::componentId<shockwave::UIDocument>()) drawUIDocumentNotes(e);
    drawNavigationNotes(info, e);
    drawSpatialAudioNotes(info, e);
    if (info.id == shockwave::componentId<shockwave::ParticleEffect>()) drawParticleEffectNotes(e);
    if (info.id == shockwave::componentId<shockwave::AudioSource>()) {
        const auto& source = *m_world.get<shockwave::AudioSource>(e);
        for (const auto& problem : shockwave::validateAudioSource(source, engine().assets())) {
            ImGui::TextColored(gui::semanticColor(gui::currentTheme().error), "%s", problem.c_str());
        }
        const bool missing = !engine().assets().audioClip(source.clip);
        ImGui::BeginDisabled(missing);
        if (gui::button("Preview")) previewAudio(source.clip, {source.volume, source.pitch, source.loop});
        ImGui::EndDisabled();
        gui::toolbarNext(gui::buttonWidth("Stop preview"));
        if (gui::button("Stop preview")) stopAudioPreview();
        if (const auto clip = engine().assets().audioClip(source.clip))
            ImGui::TextDisabled("%.2f s | 48 kHz stereo", clip->duration());
    }
    const bool physicsPart =
        info.id == shockwave::componentId<shockwave::Collider>() ||
        info.id == shockwave::componentId<shockwave::CharacterController>() ||
        (info.id == shockwave::componentId<shockwave::RigidBody>() && !m_world.has<shockwave::Collider>(e));
    if (physicsPart) {
        if (const auto* physics = m_session->physics()) {
            for (const auto& problem : physics->diagnostics()) {
                if (!(problem.entity == e)) continue;
                ImGui::PushStyleColor(ImGuiCol_Text, gui::semanticColor(gui::currentTheme().error));
                ImGui::TextWrapped("%s", problem.message.c_str());
                ImGui::PopStyleColor();
            }
        }
    }
    if (info.id == shockwave::componentId<shockwave::ModelRenderer>()) {
        const auto* instance = m_world.get<shockwave::ModelRenderer>(e);
        const auto asset = instance ? engine().assets().model(instance->model) : nullptr;
        if (!asset) return;
        ImGui::SeparatorText("Imported materials");
        for (size_t i = 0; i < asset->data.materials.size(); ++i) {
            const auto& material = asset->data.materials[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::TreeNode(material.name.empty() ? "Unnamed material" : material.name.c_str())) {
                ImGui::TextDisabled(
                    "Base color %.2f %.2f %.2f %.2f", material.baseColor.r, material.baseColor.g, material.baseColor.b,
                    material.baseColor.a);
                if (material.baseColorTexture >= 0) {
                    ImGui::TextWrapped("Texture: %s", asset->data.textures[material.baseColorTexture].source.c_str());
                }
                ImGui::TextDisabled("Metallic %.2f, roughness %.2f", material.metallic, material.roughness);
                using Alpha = shockwave::ModelMaterial::Alpha;
                ImGui::TextDisabled(
                    "%s, %s",
                    material.alpha == Alpha::Mask    ? "Cutout"
                    : material.alpha == Alpha::Blend ? "Transparent"
                                                     : "Opaque",
                    material.doubleSided ? "double-sided" : "single-sided");
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TextDisabled("Edit source materials in your modeling tool.");
    }
}

void Editor::drawField(const Field& f, void* data, Entity e) {
    gui::propertyLabel(prettyName(f.name).c_str());
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
        if (ImGui::BeginCombo(label.c_str(), preview.c_str(), ImGuiComboFlags_HeightLarge)) {
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
    case FieldType::Enum: {
        int value = 0;
        std::memcpy(&value, data, sizeof(value));
        const char* preview =
            value >= 0 && size_t(value) < f.options.size() ? f.options[value].c_str() : "Invalid value";
        if (ImGui::BeginCombo(label.c_str(), preview)) {
            for (size_t i = 0; i < f.options.size(); ++i) {
                if (ImGui::Selectable(f.options[i].c_str(), value == int(i)) && value != int(i)) {
                    pushUndo(m_world);
                    value = int(i);
                    std::memcpy(data, &value, sizeof(value));
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case FieldType::Int:
        ImGui::DragInt(
            label.c_str(), static_cast<int*>(data), speed(0.2f), static_cast<int>(f.min), static_cast<int>(f.max), "%d",
            clamp);
        trackEdit();
        break;
    case FieldType::Float:
        ImGui::DragFloat(label.c_str(), static_cast<float*>(data), speed(0.05f), f.min, f.max, "%.2f", clamp);
        trackEdit();
        break;
    case FieldType::Vec2:
        ImGui::DragFloat2(label.c_str(), static_cast<float*>(data), speed(0.05f), f.min, f.max, "%.2f", clamp);
        trackEdit();
        break;
    case FieldType::Vec3:
        ImGui::DragFloat3(label.c_str(), static_cast<float*>(data), speed(0.05f), f.min, f.max, "%.2f", clamp);
        trackEdit();
        break;
    case FieldType::Color:
        ImGui::ColorEdit4(
            label.c_str(), static_cast<float*>(data), ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_AlphaBar);
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
    case FieldType::AudioClip: {
        auto& value = *static_cast<std::string*>(data);
        if (ImGui::BeginCombo(label.c_str(), value.empty() ? "none" : value.c_str())) {
            if (ImGui::Selectable("none", value.empty()) && !value.empty()) {
                pushUndo(m_world);
                value.clear();
            }
            for (const auto& clip : engine().assets().audioClips()) {
                if (ImGui::Selectable(clip.c_str(), clip == value) && clip != value) {
                    pushUndo(m_world);
                    value = clip;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload(kAudioPayload)) {
                pushUndo(m_world);
                value = static_cast<const char*>(payload->Data);
            }
            ImGui::EndDragDropTarget();
        }
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
    case FieldType::Model: {
        auto* model = static_cast<shockwave::ModelHandle*>(data);
        assetCombo(engine().assets().models(), model->id, [&](shockwave::ModelHandle h) { *model = h; });
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kModelPayload)) {
                pushUndo(m_world);
                *model = engine().assets().findModel(static_cast<const char*>(payload->Data));
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
            ImGui::TextDisabled("Drop a texture from Content here");
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
    case FieldType::UIDocument: {
        auto& value = *static_cast<std::string*>(data);
        if (ImGui::BeginCombo(label.c_str(), value.empty() ? "none" : value.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (const std::string& document : m_gameUI->documentFiles()) {
                if (ImGui::Selectable(document.c_str(), document == value) && document != value) {
                    pushUndo(m_world);
                    value = document;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("An .rml document in Content; drop one from the Content panel");
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload(kUIDocumentPayload)) {
                pushUndo(m_world);
                value = static_cast<const char*>(payload->Data);
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
    if (ImGui::BeginCombo(label.c_str(), preview.c_str(), ImGuiComboFlags_HeightLarge)) {
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

}   // namespace editor
