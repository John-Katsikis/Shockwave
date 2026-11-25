// Terrain in the editor (W1a, W1b, docs/Terrain.md): Window > Terrain makes a terrain, sculpts
// it with Raise, Lower, Smooth and Flatten brushes in the viewport, paints its layers (Paint,
// Add layer, Remove layer), imports heights from a 16-bit PNG or .r16 and saves. Strokes change an editable copy of the heights and publish
// it to the asset registry each frame, rebuilding only the chunks they touch; each stroke
// is one undo step in a journal beside the world snapshots.
#include "Editor.h"

#include <Assets/TerrainData.h>
#include <Engine/PhysicsComponents.h>
#include <Engine/Terrain.h>
#include <UI/Gui.h>

#include <algorithm>
#include <cmath>

namespace editor {

namespace gui = shockwave::gui;
namespace fs = std::filesystem;
using shockwave::Entity;
using shockwave::TerrainData;

namespace {

constexpr const char* kBrushNames[] = {"Raise", "Lower", "Smooth", "Flatten", "Paint"};
constexpr const char* kBrushTips[] = {
    "Lifts the ground under the circle while the left button is down",
    "Lowers the ground under the circle while the left button is down",
    "Evens out bumps under the circle",
    "Pulls the ground towards the height under the first click",
    "Paints the chosen layer under the circle, taking share from the others",
};
constexpr int kPaintBrush = int(shockwave::TerrainBrush::Paint);

}   // namespace

Entity Editor::terrainTarget() const {
    const Entity chosen = selected();
    if (chosen.valid() && m_world.has<shockwave::Terrain>(chosen)) return chosen;
    Entity first = shockwave::kNullEntity;
    m_world.forEachEntity([&](Entity e) {
        if (!first.valid() && m_world.has<shockwave::Terrain>(e)) first = e;
    });
    return first;
}

void Editor::publishTerrain(
    const std::string& asset, std::shared_ptr<const TerrainData> data, glm::ivec4 rect, uint32_t changed) {
    shockwave::Assets& assets = engine().assets();
    const auto previous = assets.terrain(asset);
    if (!previous) return;
    const std::shared_ptr<const TerrainData> before = previous->data;
    auto next =
        shockwave::updateTerrain(engine().renderer(), *previous, std::move(data), rect, m_terrain.retired, changed);
    const std::shared_ptr<const TerrainData> after = next->data;
    assets.addTerrain(asset, std::move(next));
    // Foliage standing on the old ground follows it (W2b).
    if ((changed & shockwave::kTerrainHeightsChanged) && before && after && rect.x <= rect.z)
        redropFoliageOnTerrain(asset, *before, *after, rect);
}

void Editor::commitTerrainChange(
    const std::string& asset, const TerrainData& before, std::shared_ptr<TerrainData> after, glm::ivec4 rect,
    uint32_t changed) {
    const glm::ivec4 all{0, 0, int(after->samples) - 1, int(after->samples) - 1};
    // Heights undo only within rect; a layout change keeps all of the weights itself.
    m_terrainUndo.push_back(shockwave::makeTerrainEdit(
        asset, before, *after, (changed & shockwave::kTerrainHeightsChanged) ? rect : glm::ivec4{1, 1, 0, 0}));
    m_undoOrder.push_back(UndoKind::Terrain);
    m_terrainRedo.clear();
    m_redo.clear();
    m_redoOrder.clear();
    m_terrain.working[asset] = std::make_shared<TerrainData>(*after);
    publishTerrain(asset, std::move(after), (changed & shockwave::kTerrainHeightsChanged) ? rect : all, changed);
    if (std::find(m_terrain.unsaved.begin(), m_terrain.unsaved.end(), asset) == m_terrain.unsaved.end())
        m_terrain.unsaved.push_back(asset);
    m_dirty = true;
}

bool Editor::updateTerrainBrush(const ImVec2& pos, const ImVec2& size, bool hovered) {
    // Meshes replaced last frame: this frame's draws no longer name them.
    for (const shockwave::MeshHandle mesh : m_terrain.retiring) engine().renderer().destroyMesh(mesh);
    m_terrain.retiring.swap(m_terrain.retired);
    m_terrain.retired.clear();

    if (!m_terrain.open || m_terrain.brush < 0 || playing()) {
        m_terrain.stroking = false;
        return false;
    }
    const Entity target = terrainTarget();
    const auto* component = target.valid() ? m_world.get<shockwave::Terrain>(target) : nullptr;
    const auto asset = component ? engine().assets().terrain(component->asset) : nullptr;
    if (!asset || !asset->data) return false;
    const glm::vec3 corner(shockwave::worldMatrix(m_world, target)[3]);

    const ImGuiIO& io = ImGui::GetIO();
    glm::vec3 origin, dir;
    mouseRay(io.MousePos, pos, size, origin, dir);
    float t = 0.0f;
    const bool over =
        hovered && !m_looking && shockwave::raycastTerrain(*asset->data, origin - corner, dir, 5000.0f, t);
    const glm::vec3 hit = origin - corner + dir * t;

    // The brush circle, following the ground.
    if (over) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        constexpr int kSegments = 64;
        ImVec2 points[kSegments];
        int visible = 0;
        for (int i = 0; i < kSegments; ++i) {
            const float a = 6.2831853f * float(i) / kSegments;
            const glm::vec2 p = glm::vec2(hit.x, hit.z) + glm::vec2(std::cos(a), std::sin(a)) * m_terrain.radius;
            const glm::vec3 world = corner + glm::vec3(p.x, shockwave::terrainHeight(*asset->data, p.x, p.y), p.y);
            if (worldToScreen(world, pos, size, points[visible])) ++visible;
        }
        if (visible > 2)
            draw->AddPolyline(
                points, visible, gui::semanticColorU32(gui::currentTheme().accent), ImDrawFlags_Closed, 2.0f);
    }

    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (!m_terrain.stroking && over && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered() &&
        !ImGuizmo::IsOver()) {
        m_terrain.stroking = true;
        m_terrain.strokeAsset = component->asset;
        m_terrain.strokeBefore = asset->data;
        m_terrain.strokeRect = {1, 1, 0, 0};
        m_terrain.flattenTo = shockwave::terrainHeight(*asset->data, hit.x, hit.z);
        // What's published may have changed since (a reload, an undo): start from it.
        m_terrain.working[component->asset] = std::make_shared<TerrainData>(*asset->data);
    }
    if (m_terrain.stroking && down && over && m_terrain.strokeAsset == component->asset) {
        auto& working = m_terrain.working[m_terrain.strokeAsset];
        shockwave::TerrainBrushStroke stroke;
        stroke.brush = shockwave::TerrainBrush(m_terrain.brush);
        stroke.falloff = shockwave::TerrainFalloff(m_terrain.falloff);
        stroke.centre = {hit.x, hit.z};
        stroke.radius = m_terrain.radius;
        stroke.strength = m_terrain.strength;
        stroke.dt = std::min(io.DeltaTime, 0.1f);
        stroke.flattenTo = m_terrain.flattenTo;
        stroke.layer = static_cast<uint32_t>(std::clamp(m_terrain.paintLayer, 0, 3));
        const glm::ivec4 changed = shockwave::applyTerrainBrush(*working, stroke);
        if (changed.x <= changed.z) {
            m_terrain.strokeRect = shockwave::unionRect(m_terrain.strokeRect, changed);
            // The registry gets its own copy; the working copy keeps changing.
            publishTerrain(
                m_terrain.strokeAsset, std::make_shared<const TerrainData>(*working), changed,
                m_terrain.brush == kPaintBrush ? shockwave::kTerrainWeightsChanged : shockwave::kTerrainHeightsChanged);
        }
    }
    if (m_terrain.stroking && !down) {
        m_terrain.stroking = false;
        if (m_terrain.strokeRect.x <= m_terrain.strokeRect.z && m_terrain.strokeBefore) {
            const auto after = engine().assets().terrain(m_terrain.strokeAsset);
            if (after && after->data) {
                m_terrainUndo.push_back(shockwave::makeTerrainEdit(
                    m_terrain.strokeAsset, *m_terrain.strokeBefore, *after->data, m_terrain.strokeRect));
                m_undoOrder.push_back(UndoKind::Terrain);
                m_terrainRedo.clear();
                m_redo.clear();
                m_redoOrder.clear();
                if (std::find(m_terrain.unsaved.begin(), m_terrain.unsaved.end(), m_terrain.strokeAsset) ==
                    m_terrain.unsaved.end())
                    m_terrain.unsaved.push_back(m_terrain.strokeAsset);
                m_dirty = true;
            }
        }
        m_terrain.strokeBefore.reset();
    }
    return over || m_terrain.stroking;
}

void Editor::stepTerrainUndo(bool redo) {
    auto& from = redo ? m_terrainRedo : m_terrainUndo;
    auto& to = redo ? m_terrainUndo : m_terrainRedo;
    if (from.empty()) return;
    shockwave::TerrainEdit edit = std::move(from.back());
    from.pop_back();
    const auto asset = engine().assets().terrain(edit.asset);
    if (asset && asset->data) {
        auto data = std::make_shared<TerrainData>(*asset->data);
        shockwave::applyTerrainEdit(*data, edit, redo);
        m_terrain.working[edit.asset] = std::make_shared<TerrainData>(*data);
        uint32_t changed = 0;
        if (!edit.before.empty()) changed |= shockwave::kTerrainHeightsChanged;
        if (edit.layout || !edit.weightsBefore.empty()) changed |= shockwave::kTerrainWeightsChanged;
        const glm::ivec4 all{0, 0, int(data->samples) - 1, int(data->samples) - 1};
        publishTerrain(edit.asset, data, edit.layout ? shockwave::unionRect(edit.rect, all) : edit.rect, changed);
        if (std::find(m_terrain.unsaved.begin(), m_terrain.unsaved.end(), edit.asset) == m_terrain.unsaved.end())
            m_terrain.unsaved.push_back(edit.asset);
    }
    to.push_back(std::move(edit));
}

void Editor::clearTerrainUndo() {
    m_undoOrder.clear();
    m_redoOrder.clear();
    m_terrainUndo.clear();
    m_terrainRedo.clear();
    m_terrain.working.clear();
    m_terrain.stroking = false;
}

bool Editor::saveTerrains() {
    if (!m_assetFolder) return true;
    bool ok = true;
    std::vector<std::string> still;
    for (const std::string& name : m_terrain.unsaved) {
        const auto asset = engine().assets().terrain(name);
        if (!asset || !asset->data) continue;
        std::string why;
        if (!shockwave::writeTerrain(m_assetFolder->root() / name, m_assetFolder->root(), *asset->data, why)) {
            error("Couldn't save " + name + ": " + why);
            still.push_back(name);
            ok = false;
            continue;
        }
        log("Saved " + name);
    }
    m_terrain.unsaved = std::move(still);
    return ok;
}

Entity Editor::createTerrain() {
    if (!m_assetFolder || !m_hasProject) {
        warn("Open a project first: a terrain is saved in its Content folder");
        return shockwave::kNullEntity;
    }
    std::string base = m_terrain.newName;
    if (base.empty()) base = "terrain/terrain";
    if (fs::path(base).extension() == ".swterrain") base = fs::path(base).replace_extension().generic_string();
    // Not over an existing one: terrain, terrain_2, ...
    std::string name = base + ".swterrain";
    for (int n = 2; fs::exists(m_assetFolder->root() / name); ++n) name = base + "_" + std::to_string(n) + ".swterrain";
    const uint32_t samples = (64u << std::clamp(m_terrain.newSamples, 0, 5)) + 1;
    const float low = std::min(m_terrain.newLow, m_terrain.newHigh - 1.0f), high = m_terrain.newHigh;
    TerrainData data = shockwave::flatTerrain(
        samples, std::clamp(m_terrain.newSpacing, 0.1f, 100.0f), low, high, std::clamp(0.0f, low, high));
    data.heightsFile = fs::path(name).replace_extension(".r16").generic_string();
    std::string why;
    if (!shockwave::writeTerrain(m_assetFolder->root() / name, m_assetFolder->root(), data, why)) {
        error("Couldn't create " + name + ": " + why);
        return shockwave::kNullEntity;
    }
    logAssetChanges(m_assetFolder->refresh());
    pushUndo(m_world);
    const Entity e = m_world.create();
    m_world.add<shockwave::Name>(e, {uniqueName("Terrain")});
    const glm::vec3 centre = placementPoint();
    m_world.add<shockwave::Transform>(
        e, {glm::vec3(centre.x - data.size() * 0.5f, 0.0f, centre.z - data.size() * 0.5f)});
    m_world.add<shockwave::Terrain>(e, {name});
    shockwave::RigidBody body;
    body.motion = shockwave::RigidBody::Motion::Static;
    m_world.add<shockwave::RigidBody>(e, body);
    shockwave::Collider collider;
    collider.shape = shockwave::Collider::Shape::Terrain;
    m_world.add<shockwave::Collider>(e, collider);
    select(e);
    log("Created " + name + " (" + std::to_string(samples) + " points a side, " + std::to_string(int(data.size())) +
        " m). Pick a brush in Window > Terrain to sculpt it");
    return e;
}

void Editor::drawTerrainWindow() {
    if (!m_terrain.open) return;
    ImGui::SetNextWindowSize(ImVec2(360, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Terrain", &m_terrain.open)) {
        ImGui::End();
        return;
    }
    const Entity target = terrainTarget();
    const auto* component = target.valid() ? m_world.get<shockwave::Terrain>(target) : nullptr;
    const auto asset = component ? engine().assets().terrain(component->asset) : nullptr;

    if (ImGui::CollapsingHeader("Create", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* kSizes[] = {"65", "129", "257", "513", "1025", "2049"};
        ImGui::InputText("Name", m_terrain.newName, sizeof(m_terrain.newName));
        ImGui::SetItemTooltip("Content path, without .swterrain; the heights go beside it as .r16");
        ImGui::Combo("Points a side", &m_terrain.newSamples, kSizes, IM_ARRAYSIZE(kSizes));
        ImGui::DragFloat("Spacing", &m_terrain.newSpacing, 0.05f, 0.1f, 100.0f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
        ImGui::DragFloat("Lowest", &m_terrain.newLow, 1.0f, -100000.0f, 100000.0f, "%.1f m");
        ImGui::DragFloat("Highest", &m_terrain.newHigh, 1.0f, -100000.0f, 100000.0f, "%.1f m");
        const float side = float(64u << std::clamp(m_terrain.newSamples, 0, 5)) * m_terrain.newSpacing;
        ImGui::TextDisabled("%.0f m by %.0f m, flat at 0 m", double(side), double(side));
        ImGui::BeginDisabled(playing() || !m_hasProject || !(m_terrain.newHigh > m_terrain.newLow));
        if (ImGui::Button("Create terrain")) createTerrain();
        ImGui::EndDisabled();
    }

    ImGui::SeparatorText("Sculpt");
    if (!asset || !asset->data) {
        ImGui::TextDisabled(
            component ? "The selected terrain's asset isn't loaded" : "No terrain in the scene: create one above");
        m_terrain.brush = -1;
    } else {
        const TerrainData& data = *asset->data;
        ImGui::Text("%s", component->asset.c_str());
        ImGui::TextDisabled(
            "%u points a side, %.0f m, %u chunks", data.samples, double(data.size()),
            shockwave::terrainChunkCount(data));
        for (int i = 0; i < kPaintBrush; ++i) {
            if (i) ImGui::SameLine();
            const bool active = m_terrain.brush == i;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(kBrushNames[i])) m_terrain.brush = active ? -1 : i;
            if (active) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", kBrushTips[i]);
        }
        ImGui::DragFloat("Radius", &m_terrain.radius, 0.2f, 1.0f, 200.0f, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Strength", &m_terrain.strength, 0.0f, 1.0f, "%.2f");
        static const char* kFalloff[] = {"Smooth", "Linear"};
        ImGui::Combo("Falloff", &m_terrain.falloff, kFalloff, IM_ARRAYSIZE(kFalloff));
        if (m_terrain.brush >= 0 && m_terrain.brush != kPaintBrush)
            ImGui::TextDisabled("Sculpt in the viewport with the left mouse button");

        // Painted layers (W1b): up to four materials, blended by weights.
        ImGui::SeparatorText("Paint");
        const auto layers = shockwave::terrainLayers(data);
        const uint32_t count = shockwave::terrainLayerCount(data);
        if (!data.painted())
            ImGui::TextDisabled(
                "One material (%s). Add a layer to paint more",
                data.material.empty() ? "default" : data.material.c_str());
        for (uint32_t i = 0; i < shockwave::kTerrainLayerCount; ++i) {
            if (!layers[i].used) continue;
            ImGui::PushID(int(i));
            const std::string label =
                "Layer " + std::to_string(i) + ": " +
                (layers[i].material.empty() ? std::string("default surface") : layers[i].material);
            if (ImGui::RadioButton(label.c_str(), m_terrain.paintLayer == int(i))) m_terrain.paintLayer = int(i);
            ImGui::SetItemTooltip("%g m per texture repeat", double(layers[i].uvMetres));
            if (data.painted() && count > 1) {
                ImGui::SameLine();
                ImGui::BeginDisabled(playing());
                if (ImGui::SmallButton("Remove")) {
                    auto next = std::make_shared<TerrainData>(data);
                    std::string why;
                    if (shockwave::removeTerrainLayer(*next, i, why)) {
                        commitTerrainChange(
                            component->asset, data, next, {1, 1, 0, 0}, shockwave::kTerrainWeightsChanged);
                        m_terrain.status = "Removed layer " + std::to_string(i) + "; its share went to the others";
                    } else {
                        m_terrain.status = why;
                    }
                }
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("Removes the layer, sharing its weight among the others");
            }
            ImGui::PopID();
        }
        ImGui::InputText("Material", m_terrain.newLayerMaterial, sizeof(m_terrain.newLayerMaterial));
        ImGui::SetItemTooltip("The new layer's .swmaterial, a Content path; empty for the default surface");
        ImGui::DragFloat(
            "Metres per repeat", &m_terrain.newLayerUvMetres, 0.1f, 0.01f, 10000.0f, "%.2f m",
            ImGuiSliderFlags_AlwaysClamp);
        ImGui::BeginDisabled(playing() || count >= shockwave::kTerrainLayerCount);
        if (ImGui::Button("Add layer")) {
            auto next = std::make_shared<TerrainData>(data);
            std::string why;
            const int index =
                shockwave::addTerrainLayer(*next, m_terrain.newLayerMaterial, m_terrain.newLayerUvMetres, why);
            if (index >= 0) {
                commitTerrainChange(component->asset, data, next, {1, 1, 0, 0}, shockwave::kTerrainWeightsChanged);
                m_terrain.paintLayer = index;
                m_terrain.status = "Added layer " + std::to_string(index) +
                                   (data.painted() ? "" : "; the terrain is now painted (swterrain 2)");
            } else {
                m_terrain.status = why;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        {
            const bool active = m_terrain.brush == kPaintBrush;
            ImGui::BeginDisabled(!data.painted());
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(kBrushNames[kPaintBrush])) m_terrain.brush = active ? -1 : kPaintBrush;
            if (active) ImGui::PopStyleColor();
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("%s", kBrushTips[kPaintBrush]);
        }
        if (m_terrain.brush == kPaintBrush) {
            if (!data.painted() || !layers[size_t(std::clamp(m_terrain.paintLayer, 0, 3))].used)
                m_terrain.brush = -1;
            else
                ImGui::TextDisabled("Paint layer %d in the viewport with the left mouse button", m_terrain.paintLayer);
        }

        ImGui::SeparatorText("Import heights");
        ImGui::InputText("File", m_terrain.importPath, sizeof(m_terrain.importPath));
        ImGui::SetItemTooltip("A 16-bit greyscale PNG or a raw .r16, %u by %u", data.samples, data.samples);
        ImGui::BeginDisabled(playing() || !m_terrain.importPath[0]);
        if (ImGui::Button("Import")) {
            uint32_t samples = 0;
            std::vector<uint16_t> heights;
            std::string why;
            if (!shockwave::importTerrainHeights(m_terrain.importPath, samples, heights, why)) {
                m_terrain.status = why;
            } else if (samples != data.samples) {
                m_terrain.status = "The file has " + std::to_string(samples) + " points a side; this terrain has " +
                                   std::to_string(data.samples) + ". Use player --terrain-import for a new size";
            } else {
                auto next = std::make_shared<TerrainData>(data);
                next->heights = std::move(heights);
                const glm::ivec4 all{0, 0, int(data.samples) - 1, int(data.samples) - 1};
                commitTerrainChange(component->asset, data, next, all, shockwave::kTerrainHeightsChanged);
                m_terrain.status = "Imported " + fs::path(m_terrain.importPath).filename().string();
            }
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    ImGui::BeginDisabled(m_terrain.unsaved.empty() || playing());
    if (ImGui::Button("Save terrains")) saveTerrains();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        "Writes every sculpted or painted terrain's .swterrain, heights and weights (Save scene does too)");
    if (!m_terrain.unsaved.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%zu unsaved", m_terrain.unsaved.size());
    }
    if (!m_terrain.status.empty()) ImGui::TextWrapped("%s", m_terrain.status.c_str());
    ImGui::End();
}

}   // namespace editor
