// The Material window: one material file (.swmaterial, A02, docs/MaterialSchema.md) at a
// time, with what it inherits and what it changes. Values it sets are shown in the accent
// colour with an Inherit button that removes them; the rest show the parent's value, and
// editing one makes it an override. Every edit is saved straight to the file (canonical
// order, comments dropped), and the asset folder reloads it, so the viewport updates on
// the next frame. Also: new materials, putting them on objects and model parts.
#include "Editor.h"

#include <Assets/Material.h>
#include <UI/Gui.h>

#include <imgui_stdlib.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cstring>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::MaterialFile;
using shockwave::MaterialParamInfo;
using shockwave::MaterialSlot;
using shockwave::MaterialValueType;

namespace {

std::string pretty(std::string text) {
    std::replace(text.begin(), text.end(), '_', ' ');
    if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

template <class T>
T& member(shockwave::MaterialParams& p, const MaterialParamInfo& info) {
    return *reinterpret_cast<T*>(reinterpret_cast<char*>(&p) + info.offset);
}
template <class T>
const T& member(const shockwave::MaterialParams& p, const MaterialParamInfo& info) {
    return *reinterpret_cast<const T*>(reinterpret_cast<const char*>(&p) + info.offset);
}

// A row's label, in the accent colour when this file sets the value itself.
void overrideLabel(const char* label, bool overridden) {
    if (overridden) ImGui::PushStyleColor(ImGuiCol_Text, gui::accent());
    gui::propertyLabel(overridden ? (std::string(label) + " *").c_str() : label);
    if (overridden) ImGui::PopStyleColor();
}

const char* const kWrapNames[] = {"repeat", "clamp", "mirror"};
const char* const kFilterNames[] = {"linear", "nearest"};
const char* const kMipNames[] = {"none", "nearest", "linear"};

}   // namespace

void Editor::openMaterial(const std::string& path) {
    m_materialPath = path;
    m_showMaterial = true;
    m_materialLoadError.clear();
    if (!shockwave::loadMaterialFile(m_project.assetFolder / path, m_materialEdit, m_materialLoadError))
        m_materialEdit = {};
}

std::string Editor::newMaterial(
    const std::string& folder, const std::string& base, const std::string& parent, const std::string& parentMaterial) {
    const std::string dir = folder.empty() ? "Materials/" : folder + "/";
    std::string path = dir + base + ".swmaterial";
    for (int n = 2; fs::exists(m_project.assetFolder / path); ++n)
        path = dir + base + " " + std::to_string(n) + ".swmaterial";
    MaterialFile file;
    file.parent = parent;
    file.parentMaterial = parentMaterial;
    if (parent.empty()) {
        // A material of its own starts with the values people change first, written out.
        for (const char* key : {"base_color", "metallic", "roughness"}) file.setValue(key, true);
    }
    std::error_code ec;
    fs::create_directories((m_project.assetFolder / path).parent_path(), ec);
    std::string problem;
    if (!shockwave::saveMaterialFile(m_project.assetFolder / path, file, problem)) {
        error("Couldn't create " + path + ": " + problem);
        return {};
    }
    log("Created " + path + (parent.empty() ? "" : ", starting from " + parent));
    logAssetChanges(m_assetFolder->refresh());
    openMaterial(path);
    return path;
}

bool Editor::applyMaterial(shockwave::Entity e, const std::string& material, const std::string& part) {
    const shockwave::MaterialHandle handle = engine().assets().findMaterial(material);
    if (!handle.valid()) {
        warn(material + " isn't loaded yet");
        return false;
    }
    if (auto* renderer = m_world.get<shockwave::MeshRenderer>(e)) {
        pushUndo(m_world);
        renderer->material = handle;
        log("Gave " + displayName(e) + " the material " + material);
        return true;
    }
    auto* instance = m_world.get<shockwave::ModelRenderer>(e);
    const auto model = instance ? engine().assets().model(instance->model) : nullptr;
    if (!model) {
        if (m_world.alive(e)) warn(displayName(e) + " has no MeshRenderer or ModelRenderer to put " + material + " on");
        return false;
    }
    const auto replaces = [&](const shockwave::ModelMaterial& source) {
        return !source.name.empty() && (part.empty() || source.name == part);
    };
    if (std::none_of(model->data.materials.begin(), model->data.materials.end(), replaces)) {
        warn(
            "No named material on " + displayName(e) + " to replace" + (part.empty() ? "" : " (" + part + ")") +
            "; unnamed materials can't be overridden, name them in the modelling tool");
        return false;
    }
    pushUndo(m_world);
    for (const auto& source : model->data.materials) {
        if (!replaces(source)) continue;
        auto& overrides = instance->materials;
        const auto it = std::find_if(overrides.begin(), overrides.end(), [&](const shockwave::MaterialOverride& o) {
            return o.part == source.name;
        });
        if (it != overrides.end())
            it->material = handle;
        else
            overrides.push_back({source.name, handle});
    }
    log("Gave " + displayName(e) + (part.empty() ? "" : "'s " + part) + " the material " + material);
    return true;
}

void Editor::drawModelMaterials(shockwave::Entity e) {
    auto* instance = m_world.get<shockwave::ModelRenderer>(e);
    const auto asset = instance ? engine().assets().model(instance->model) : nullptr;
    if (!asset) return;
    ImGui::SeparatorText("Materials");
    const std::string modelName = engine().assets().modelName(instance->model);
    for (size_t i = 0; i < asset->data.materials.size(); ++i) {
        const auto& material = asset->data.materials[i];
        ImGui::PushID(static_cast<int>(i));
        const shockwave::MaterialOverride* o = shockwave::findMaterialOverride(instance->materials, material.name);
        const std::string replacement = o ? engine().assets().materialName(o->material) : std::string();
        const std::string title = (material.name.empty() ? std::string("Unnamed material") : material.name) +
                                  (replacement.empty() ? "" : "  ->  " + replacement);
        const bool open = ImGui::TreeNode("##part", "%s", title.c_str());
        // Drop a material on the row to replace this part's.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMaterialPayload))
                applyMaterial(e, static_cast<const char*>(payload->Data), material.name);
            ImGui::EndDragDropTarget();
        }
        if (open) {
            if (material.name.empty()) {
                ImGui::TextDisabled("Name it in the modelling tool to replace it here.");
            } else {
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##replace", replacement.empty() ? "its own" : replacement.c_str())) {
                    if (ImGui::Selectable("its own", !o) && o) {
                        pushUndo(m_world);
                        std::erase_if(instance->materials, [&](const auto& x) { return x.part == material.name; });
                    }
                    for (const auto& [name, handle] : engine().assets().materials())
                        if (ImGui::Selectable(name.c_str(), name == replacement) && name != replacement)
                            applyMaterial(e, name, material.name);
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("Draw this part with a material from Content instead; drop one here");
                if (gui::button("Make instance")) {
                    // A material that starts as exactly this one, ready to change.
                    const std::string path = newMaterial("Materials", material.name, modelName, material.name);
                    if (!path.empty()) applyMaterial(e, path, material.name);
                }
                ImGui::SetItemTooltip("A new .swmaterial inheriting this part's material, put on this part");
                if (o) {
                    gui::toolbarNext(gui::buttonWidth("Edit") + gui::buttonWidth("Reset to source"));
                    if (gui::button("Edit")) openMaterial(replacement);
                    ImGui::SameLine();
                    if (gui::button("Reset to source")) {
                        pushUndo(m_world);
                        std::erase_if(instance->materials, [&](const auto& x) { return x.part == material.name; });
                    }
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void Editor::drawMaterialWindow() {
    ImGui::SetNextWindowSize(ImVec2(560, 680), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Material", &m_showMaterial)) {
        ImGui::End();
        return;
    }
    if (!m_hasProject || m_materialPath.empty()) {
        ImGui::TextDisabled("Double-click a material in Content to edit it.\nRight-click Content for New material.");
        ImGui::End();
        return;
    }
    const auto& theme = gui::currentTheme();
    const shockwave::AssetFolder::File* file = m_assetFolder->find(m_materialPath);
    const auto asset = file ? engine().assets().material(file->material) : nullptr;
    ImGui::TextUnformatted(m_materialPath.c_str());
    if (!m_materialLoadError.empty()) {
        ImGui::TextColored(gui::semanticColor(theme.error), "%s", m_materialLoadError.c_str());
        if (gui::button("Reload")) openMaterial(m_materialPath);
        ImGui::End();
        return;
    }
    if (file && !file->error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, gui::semanticColor(theme.error));
        ImGui::TextWrapped("%s", file->error.c_str());
        ImGui::PopStyleColor();
    }
    if (file)
        for (const std::string& warning : file->warnings) {
            ImGui::PushStyleColor(ImGuiCol_Text, gui::semanticColor(theme.warning));
            ImGui::TextWrapped("%s", warning.c_str());
            ImGui::PopStyleColor();
        }
    if (asset && !asset->fallback) {
        std::string chain;
        for (size_t i = 1; i < asset->desc.chain.size(); ++i) chain += (i > 1 ? "  <-  " : "") + asset->desc.chain[i];
        ImGui::TextDisabled("Inherits from: %s", chain.empty() ? "nothing (the defaults)" : chain.c_str());
        std::string features;
        for (uint32_t bit = 0; bit < shockwave::kMaterialFeatureCount; ++bit)
            if (asset->desc.features & (1u << bit))
                features += (features.empty() ? "" : ", ") + std::string(shockwave::materialFeatureName(1u << bit));
        ImGui::TextDisabled("Features: %s", features.empty() ? "plain" : features.c_str());
        const std::string draws =
            shockwave::materialEffectiveShading(asset->desc, engine().renderer().shadingSettings());
        ImGui::TextDisabled("Draws as: %s", draws.c_str());
        const auto omitted = shockwave::materialLowQualityOmissions(asset->desc);
        if (!omitted.empty()) {
            std::string list;
            for (const auto& name : omitted) list += (list.empty() ? "" : ", ") + name;
            ImGui::TextDisabled("Material quality low leaves out: %s", list.c_str());
        }
    }
    const shockwave::MaterialParams defaults;
    const shockwave::MaterialParams& effective = asset && !asset->fallback ? asset->desc.params : defaults;
    MaterialFile& edit = m_materialEdit;
    bool commit = false;   // save now
    auto settled = [&](bool changed) {
        // Drags save when let go; everything else as soon as it changes.
        if (ImGui::IsItemDeactivatedAfterEdit() || (changed && !ImGui::IsItemActive())) commit = true;
    };
    const float inheritWidth = gui::buttonWidth("Inherit") + ImGui::GetStyle().ItemSpacing.x;

    ImGui::SeparatorText("Parent");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##parent", "none: starts from the defaults", &edit.parent);
    settled(false);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMaterialPayload)) {
            edit.parent = static_cast<const char*>(payload->Data);
            edit.parentMaterial.clear();
            commit = true;
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kModelPayload)) {
            edit.parent = static_cast<const char*>(payload->Data);
            commit = true;
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SetItemTooltip("Drop a material or a model from Content here");
    if (!edit.parent.empty() && !shockwave::isMaterialFile(edit.parent)) {
        // A model parent: which of its materials.
        const auto model = engine().assets().model(engine().assets().findModel(edit.parent));
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo(
                "##source", edit.parentMaterial.empty() ? "pick its material" : edit.parentMaterial.c_str())) {
            if (model)
                for (const auto& source : model->data.materials)
                    if (!source.name.empty() &&
                        ImGui::Selectable(source.name.c_str(), source.name == edit.parentMaterial)) {
                        edit.parentMaterial = source.name;
                        commit = true;
                    }
            ImGui::EndCombo();
        }
    }

    // Values, by group, in schema order.
    const std::pair<const char*, const char*> groups[] = {{"surface", "Surface"},     {"alpha", "Alpha"},
                                                          {"detail", "Detail layer"}, {"layer", "Second layer"},
                                                          {"clearcoat", "Clearcoat"}, {"transmission", "Transmission"},
                                                          {"skin", "Skin"},           {"hair", "Hair"}};
    // The family's own groups only (docs/Shading.md): another family's values do nothing.
    const int family = edit.isSet("family") ? edit.values.family : effective.family;
    for (const auto& [group, title] : groups) {
        if (!shockwave::materialFamilyUses(family, group)) continue;
        ImGui::SeparatorText(title);
        if (!gui::beginPropertyGrid(group)) continue;
        for (const MaterialParamInfo& info : shockwave::materialParams()) {
            if (std::string_view(info.group) != group) continue;
            ImGui::PushID(info.key);
            const bool set = edit.isSet(info.key);
            overrideLabel(pretty(info.key).c_str(), set);
            ImGui::SetNextItemWidth(set ? -inheritWidth : -1.0f);
            // Inherited values start from what the parent gives.
            shockwave::MaterialParams& values = edit.values;
            if (!set)
                std::memcpy(
                    reinterpret_cast<char*>(&values) + info.offset,
                    reinterpret_cast<const char*>(&effective) + info.offset,
                    info.type == MaterialValueType::Bool     ? sizeof(bool)
                    : info.type == MaterialValueType::Enum   ? sizeof(int)
                    : info.type == MaterialValueType::Vec2   ? sizeof(float) * 2
                    : info.type == MaterialValueType::Color3 ? sizeof(float) * 3
                    : info.type == MaterialValueType::Color4 ? sizeof(float) * 4
                                                             : sizeof(float));
            bool changed = false;
            const float lo = info.min, hi = info.max;
            switch (info.type) {
            case MaterialValueType::Float:
                changed = ImGui::DragFloat(
                    "##v", &member<float>(values, info), 0.005f * (hi - lo), lo, hi, "%.3f",
                    ImGuiSliderFlags_AlwaysClamp);
                break;
            case MaterialValueType::Vec2:
                changed = ImGui::DragFloat2(
                    "##v", &member<float>(values, info), 0.1f, lo, hi, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                break;
            case MaterialValueType::Color3:
                changed = ImGui::ColorEdit3(
                    "##v", &member<float>(values, info), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                break;
            case MaterialValueType::Color4:
                changed = ImGui::ColorEdit4(
                    "##v", &member<float>(values, info), ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_AlphaBar);
                break;
            case MaterialValueType::Bool: changed = ImGui::Checkbox("##v", &member<bool>(values, info)); break;
            case MaterialValueType::Enum: {
                int& value = member<int>(values, info);
                if (ImGui::BeginCombo(
                        "##v", info.options[size_t(std::clamp(value, 0, int(info.options.size()) - 1))])) {
                    for (size_t i = 0; i < info.options.size(); ++i)
                        if (ImGui::Selectable(info.options[i], value == int(i)) && value != int(i)) {
                            value = int(i);
                            changed = true;
                        }
                    ImGui::EndCombo();
                }
                break;
            }
            }
            ImGui::SetItemTooltip("%s", info.description);
            if (changed) edit.setValue(info.key, true);
            settled(changed);
            if (set) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Inherit")) {
                    edit.setValue(info.key, false);
                    commit = true;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Texture slots: which texture, and how it's read.
    ImGui::SeparatorText("Textures");
    std::vector<std::string> textures;
    for (const auto& f : m_assetFolder->files())
        if (f.kind == shockwave::AssetFolder::Kind::Texture) textures.push_back(f.name);
    if (gui::beginPropertyGrid("material_slots")) {
        for (size_t s = 0; s < shockwave::kAllMaterialSlots; ++s) {
            const auto slot = MaterialSlot(s);
            if (!shockwave::materialFamilyUses(family, shockwave::materialSlotInfo(slot).group)) continue;
            ImGui::PushID(static_cast<int>(s));
            shockwave::MaterialSlotOverride& o = edit.slots[s];
            const shockwave::ResolvedSlot inherited =
                asset && !asset->fallback ? asset->desc.slots[s] : shockwave::ResolvedSlot{};
            overrideLabel(pretty(shockwave::materialSlotName(slot)).c_str(), !o.empty());
            std::string preview;
            if (o.hasTexture)
                preview = o.texture.empty() ? "none (cleared)" : o.texture;
            else if (!inherited.texture.empty())
                preview = (inherited.texture.modelTexture >= 0 ? inherited.texture.asset + "'s own"
                                                               : inherited.texture.asset) +
                          " (inherited)";
            else
                preview = "none";
            ImGui::SetNextItemWidth(o.hasTexture ? -inheritWidth : -1.0f);
            if (ImGui::BeginCombo("##texture", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable("none (clear)", o.hasTexture && o.texture.empty())) {
                    o.hasTexture = true;
                    o.texture.clear();
                    commit = true;
                }
                for (const std::string& name : textures)
                    if (ImGui::Selectable(name.c_str(), o.hasTexture && o.texture == name)) {
                        o.hasTexture = true;
                        o.texture = name;
                        commit = true;
                    }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip(
                "%s\nDrop a texture from Content here", shockwave::materialSlotInfo(slot).description);
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kTexturePayload)) {
                    o.hasTexture = true;
                    o.texture = static_cast<const char*>(payload->Data);
                    commit = true;
                }
                ImGui::EndDragDropTarget();
            }
            if (o.hasTexture) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Inherit")) {
                    o.hasTexture = false;
                    o.texture.clear();
                    commit = true;
                }
            }
            // How it's read, for slots with a texture.
            if ((o.hasTexture && !o.texture.empty()) || (!o.hasTexture && !inherited.texture.empty()) || o.hasUvSet ||
                o.hasTransform || o.hasSampler) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                if (ImGui::TreeNode(
                        "##reading", "UV and sampler%s", o.hasUvSet || o.hasTransform || o.hasSampler ? " *" : "")) {
                    int uvSet = int(o.hasUvSet ? o.uvSet : inherited.binding.uvSet);
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
                    if (ImGui::Combo(
                            "UV set", &uvSet,
                            "0\0"
                            "1\0")) {
                        o.hasUvSet = true;
                        o.uvSet = uint32_t(uvSet);
                        commit = true;
                    }
                    if (o.hasUvSet) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Inherit##uv")) o.hasUvSet = false, commit = true;
                    }
                    shockwave::UvTransform t = o.hasTransform ? o.transform : inherited.binding.transform;
                    float degrees = glm::degrees(t.rotation);
                    bool moved = ImGui::DragFloat2("Offset", &t.offset.x, 0.01f);
                    settled(moved);
                    bool scaled = ImGui::DragFloat2("Scale", &t.scale.x, 0.01f);
                    settled(scaled);
                    bool turned = ImGui::DragFloat("Rotation", &degrees, 0.5f, -360.0f, 360.0f, "%.1f deg");
                    settled(turned);
                    if (moved || scaled || turned) {
                        t.rotation = glm::radians(degrees);
                        o.hasTransform = true;
                        o.transform = t;
                    }
                    if (o.hasTransform && ImGui::SmallButton("Inherit##transform"))
                        o.hasTransform = false, commit = true;
                    shockwave::SamplerDesc d = o.hasSampler ? o.sampler : inherited.binding.sampler;
                    int wrapU = int(d.wrapU), wrapV = int(d.wrapV), mag = int(d.magFilter), min = int(d.minFilter);
                    int mip = d.mipmaps ? (d.mipFilter == shockwave::TextureFilter::Nearest ? 1 : 2) : 0;
                    int aniso = d.maxAnisotropy;
                    bool sampled = false;
                    sampled |= ImGui::Combo("Wrap U", &wrapU, kWrapNames, 3);
                    sampled |= ImGui::Combo("Wrap V", &wrapV, kWrapNames, 3);
                    sampled |= ImGui::Combo("Min filter", &min, kFilterNames, 2);
                    sampled |= ImGui::Combo("Mag filter", &mag, kFilterNames, 2);
                    sampled |= ImGui::Combo("Mip filter", &mip, kMipNames, 3);
                    const bool dragged = ImGui::SliderInt("Anisotropy", &aniso, 1, 16);
                    settled(dragged);
                    if (sampled || dragged) {
                        d.wrapU = shockwave::TextureWrap(wrapU);
                        d.wrapV = shockwave::TextureWrap(wrapV);
                        d.minFilter = shockwave::TextureFilter(min);
                        d.magFilter = shockwave::TextureFilter(mag);
                        d.mipmaps = mip != 0;
                        d.mipFilter = mip == 1 ? shockwave::TextureFilter::Nearest : shockwave::TextureFilter::Linear;
                        d.maxAnisotropy = uint8_t(aniso);
                        o.hasSampler = true;
                        o.sampler = d;
                        commit |= sampled;
                    }
                    if (o.hasSampler && ImGui::SmallButton("Inherit##sampler")) o.hasSampler = false, commit = true;
                    ImGui::TreePop();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Saved as you edit. The file is rewritten in a standard order, so comments in it aren't kept.");

    if (commit) {
        std::string problem;
        if (shockwave::saveMaterialFile(m_project.assetFolder / m_materialPath, edit, problem))
            logAssetChanges(m_assetFolder->refresh());
        else
            error("Couldn't save " + m_materialPath + ": " + problem);
    }
    ImGui::End();
}

}   // namespace editor
