// Foliage in the editor (W2b, docs/Foliage.md): Window > Foliage makes a foliage asset, adds
// and tunes its types, and paints or erases instances in the viewport. A stroke edits a copy
// of the instances and publishes it to the asset registry each frame; when it ends it becomes
// one undo step in the same journal as terrain strokes. Painted instances drop onto whatever
// collides below them (terrain or any collider), through a physics copy of the scene.
#include "Editor.h"

#include <Assets/FoliageData.h>
#include <Assets/TerrainData.h>
#include <Engine/Foliage.h>
#include <Engine/Hierarchy.h>
#include <Engine/Terrain.h>
#include <UI/Gui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace editor {

namespace gui = shockwave::gui;
namespace fs = std::filesystem;
using shockwave::Entity;
using shockwave::FoliageData;

namespace {

constexpr int kPaint = 0, kErase = 1;
constexpr float kGroundHeight = 1000.0f;   // metres above a point the drop ray starts

void markUnsaved(std::vector<std::string>& unsaved, const std::string& asset) {
    if (std::find(unsaved.begin(), unsaved.end(), asset) == unsaved.end()) unsaved.push_back(asset);
}

}   // namespace

Entity Editor::foliageTarget() const {
    const Entity chosen = selected();
    if (chosen.valid() && m_world.has<shockwave::Foliage>(chosen)) return chosen;
    Entity first = shockwave::kNullEntity;
    m_world.forEachEntity([&](Entity e) {
        if (!first.valid() && m_world.has<shockwave::Foliage>(e)) first = e;
    });
    return first;
}

void Editor::publishFoliage(const std::string& asset, std::shared_ptr<const FoliageData> data) {
    engine().assets().addFoliage(asset, std::move(data));
}

void Editor::rebuildFoliageGround() {
    m_foliage.ground.reset();
    m_foliage.groundWorld = std::make_unique<shockwave::World>(m_world);
    m_foliage.ground =
        std::make_unique<shockwave::PhysicsWorld>(*m_foliage.groundWorld, engine().jobs(), &engine().assets());
    m_foliage.ground->synchronize();
}

bool Editor::foliageGroundAt(glm::vec3 from, glm::vec3& point, glm::vec3& normal) const {
    if (!m_foliage.ground) return false;
    const auto hit = m_foliage.ground->raycast(from, glm::vec3(0.0f, -1.0f, 0.0f), kGroundHeight * 2.0f);
    if (!hit) return false;
    point = hit->point;
    normal = hit->normal;
    return true;
}

bool Editor::beginFoliageStroke(Entity target) {
    const auto* component = target.valid() ? m_world.get<shockwave::Foliage>(target) : nullptr;
    const auto data = component ? engine().assets().foliage(component->asset) : nullptr;
    if (!data || data->types.empty() || m_foliage.brush < 0) return false;
    m_foliage.type = std::clamp(m_foliage.type, 0, int(data->types.size()) - 1);
    m_foliage.stroking = true;
    m_foliage.strokeAsset = component->asset;
    m_foliage.strokeEntity = target;
    m_foliage.strokeBefore = data;
    m_foliage.working = std::make_shared<FoliageData>(*data);
    ++m_foliage.seed;
    m_foliage.dabs = 0;
    rebuildFoliageGround();
    return true;
}

void Editor::foliageDab(glm::vec2 centre) {
    if (!m_foliage.stroking || !m_foliage.working || !m_world.alive(m_foliage.strokeEntity)) return;
    FoliageData& working = *m_foliage.working;
    const glm::mat4 placed = shockwave::worldMatrix(m_world, m_foliage.strokeEntity);
    const glm::mat4 toLocal = glm::inverse(placed);
    const auto type = static_cast<uint16_t>(std::clamp(m_foliage.type, 0, int(working.types.size()) - 1));
    size_t changed = 0;
    if (m_foliage.brush == kPaint) {
        shockwave::FoliagePaint paint;
        paint.centre = centre;
        paint.radius = m_foliage.radius;
        paint.density = std::clamp(m_foliage.density, 0.01f, 20.0f);
        paint.type = type;
        paint.seed = m_foliage.seed * 7919u + m_foliage.dabs++;
        changed = shockwave::scatterFoliage(working, paint, [&](glm::vec2 xz, glm::vec3& point, glm::vec3& normal) {
            const glm::vec3 above =
                glm::vec3(placed * glm::vec4(xz.x, 0.0f, xz.y, 1.0f)) + glm::vec3(0.0f, kGroundHeight, 0.0f);
            glm::vec3 hit;
            if (!foliageGroundAt(above, hit, normal)) return false;
            point = glm::vec3(toLocal * glm::vec4(hit, 1.0f));
            return true;
        });
    } else if (m_foliage.brush == kErase) {
        changed = shockwave::eraseFoliage(working, centre, m_foliage.radius, type);
    }
    if (changed == 0) return;
    shockwave::finishFoliageEdit(working);
    publishFoliage(m_foliage.strokeAsset, std::make_shared<const FoliageData>(working));
}

void Editor::endFoliageStroke() {
    if (!m_foliage.stroking) return;
    m_foliage.stroking = false;
    const auto after = engine().assets().foliage(m_foliage.strokeAsset);
    if (after && m_foliage.strokeBefore) {
        shockwave::FoliageEdit edit =
            shockwave::makeFoliageEdit(m_foliage.strokeAsset, *m_foliage.strokeBefore, *after);
        if (!edit.added.empty() || !edit.removed.empty() || edit.layout) {
            m_foliage.status =
                (edit.added.empty() ? "" : "Painted " + std::to_string(edit.added.size()) + " instances") +
                (edit.removed.empty() ? "" : "Erased " + std::to_string(edit.removed.size()) + " instances");
            m_foliageUndo.push_back(std::move(edit));
            m_undoOrder.push_back(UndoKind::Foliage);
            m_foliageRedo.clear();
            m_terrainRedo.clear();
            m_redo.clear();
            m_redoOrder.clear();
            markUnsaved(m_foliage.unsaved, m_foliage.strokeAsset);
            m_dirty = true;
        }
    }
    m_foliage.strokeBefore.reset();
    m_foliage.working.reset();
}

void Editor::commitFoliageChange(const std::string& asset, const FoliageData& before, FoliageData after) {
    shockwave::finishFoliageEdit(after);
    m_foliageUndo.push_back(shockwave::makeFoliageEdit(asset, before, after));
    m_undoOrder.push_back(UndoKind::Foliage);
    m_foliageRedo.clear();
    m_terrainRedo.clear();
    m_redo.clear();
    m_redoOrder.clear();
    publishFoliage(asset, std::make_shared<const FoliageData>(std::move(after)));
    markUnsaved(m_foliage.unsaved, asset);
    m_dirty = true;
}

void Editor::stepFoliageUndo(bool redo) {
    auto& from = redo ? m_foliageRedo : m_foliageUndo;
    auto& to = redo ? m_foliageUndo : m_foliageRedo;
    if (from.empty()) return;
    shockwave::FoliageEdit edit = std::move(from.back());
    from.pop_back();
    if (const auto data = engine().assets().foliage(edit.asset)) {
        auto next = std::make_shared<FoliageData>(*data);
        shockwave::applyFoliageEdit(*next, edit, redo);
        publishFoliage(edit.asset, std::move(next));
        markUnsaved(m_foliage.unsaved, edit.asset);
    }
    to.push_back(std::move(edit));
}

void Editor::clearFoliageUndo() {
    m_foliageUndo.clear();
    m_foliageRedo.clear();
    m_foliage.stroking = false;
    m_foliage.working.reset();
    m_foliage.strokeBefore.reset();
    m_foliage.ground.reset();
    m_foliage.groundWorld.reset();
}

bool Editor::saveFoliage() {
    if (!m_assetFolder) return true;
    bool ok = true;
    std::vector<std::string> still;
    for (const std::string& name : m_foliage.unsaved) {
        const auto data = engine().assets().foliage(name);
        if (!data) continue;
        std::string why;
        if (!shockwave::writeFoliage(m_assetFolder->root() / name, m_assetFolder->root(), *data, why)) {
            error("Couldn't save " + name + ": " + why);
            still.push_back(name);
            ok = false;
            continue;
        }
        log("Saved " + name + " (" + std::to_string(data->instances.size()) + " instances)");
    }
    m_foliage.unsaved = std::move(still);
    return ok;
}

void Editor::redropFoliageOnTerrain(
    const std::string& terrainAsset, const shockwave::TerrainData& before, const shockwave::TerrainData& after,
    glm::ivec4 rect) {
    // Where the terrain is: its first entity with this asset (only its position counts).
    Entity terrain = shockwave::kNullEntity;
    m_world.forEachEntity([&](Entity e) {
        const auto* t = m_world.get<shockwave::Terrain>(e);
        if (!terrain.valid() && t && t->asset == terrainAsset) terrain = e;
    });
    if (!terrain.valid()) return;
    const glm::vec3 corner(shockwave::worldMatrix(m_world, terrain)[3]);
    const float size = before.size();
    // The changed points, in metres in the world, one spacing wider for the slopes around them.
    const glm::vec2 lowWorld = glm::vec2(corner.x, corner.z) + glm::vec2(rect.x - 1, rect.y - 1) * before.spacing;
    const glm::vec2 highWorld = glm::vec2(corner.x, corner.z) + glm::vec2(rect.z + 1, rect.w + 1) * before.spacing;
    std::vector<std::string> done;
    m_world.forEachEntity([&](Entity e) {
        const auto* component = m_world.get<shockwave::Foliage>(e);
        if (!component || std::find(done.begin(), done.end(), component->asset) != done.end()) return;
        const auto data = engine().assets().foliage(component->asset);
        if (!data) return;
        done.push_back(component->asset);
        const glm::mat4 placed = shockwave::worldMatrix(m_world, e);
        const glm::mat4 toLocal = glm::inverse(placed);
        // The rectangle in the foliage's space.
        glm::vec2 low(1e30f), high(-1e30f);
        for (int i = 0; i < 4; ++i) {
            const glm::vec2 c((i & 1) ? highWorld.x : lowWorld.x, (i & 2) ? highWorld.y : lowWorld.y);
            const glm::vec3 local = glm::vec3(toLocal * glm::vec4(c.x, 0.0f, c.y, 1.0f));
            low = glm::min(low, glm::vec2(local.x, local.z));
            high = glm::max(high, glm::vec2(local.x, local.z));
        }
        auto height = [&](const shockwave::TerrainData& ground) {
            return [&, ground = &ground](glm::vec2 xz, float& y) {
                const glm::vec3 world = glm::vec3(placed * glm::vec4(xz.x, 0.0f, xz.y, 1.0f));
                const float tx = world.x - corner.x, tz = world.z - corner.z;
                if (tx < 0.0f || tz < 0.0f || tx > size || tz > size) return false;
                const float h = corner.y + shockwave::terrainHeight(*ground, tx, tz);
                y = (toLocal * glm::vec4(world.x, h, world.z, 1.0f)).y;
                return true;
            };
        };
        auto next = std::make_shared<FoliageData>(*data);
        if (shockwave::redropFoliage(*next, low, high, height(before), height(after)) == 0) return;
        shockwave::sortFoliage(*next);
        shockwave::renumberFoliage(*next);
        publishFoliage(component->asset, std::move(next));
        markUnsaved(m_foliage.unsaved, component->asset);
    });
}

Entity Editor::createFoliage() {
    if (!m_assetFolder || !m_hasProject) {
        warn("Open a project first: foliage is saved in its Content folder");
        return shockwave::kNullEntity;
    }
    std::string base = m_foliage.newName;
    if (base.empty()) base = "foliage/foliage";
    if (fs::path(base).extension() == ".swfoliage") base = fs::path(base).replace_extension().generic_string();
    std::string name = base + ".swfoliage";
    for (int n = 2; fs::exists(m_assetFolder->root() / name); ++n) name = base + "_" + std::to_string(n) + ".swfoliage";
    FoliageData data;
    data.version = 2;
    data.instancesFile = fs::path(name).replace_extension(".instances").generic_string();
    if (m_foliage.newTypeModel[0]) {
        shockwave::FoliageType type;
        type.model = m_foliage.newTypeModel;
        data.types.push_back(type);
    }
    std::string why;
    if (!shockwave::writeFoliage(m_assetFolder->root() / name, m_assetFolder->root(), data, why)) {
        error("Couldn't create " + name + ": " + why);
        return shockwave::kNullEntity;
    }
    logAssetChanges(m_assetFolder->refresh());
    pushUndo(m_world);
    const Entity e = m_world.create();
    m_world.add<shockwave::Name>(e, {uniqueName("Foliage")});
    m_world.add<shockwave::Transform>(e, {});
    m_world.add<shockwave::Foliage>(e, {name});
    select(e);
    log("Created " + name + ". Add a type in Window > Foliage, then pick Paint");
    return e;
}

bool Editor::updateFoliageBrush(const ImVec2& pos, const ImVec2& size, bool hovered) {
    if (!m_foliage.open || m_foliage.brush < 0 || playing() || m_terrain.brush >= 0) {
        if (m_foliage.stroking) endFoliageStroke();
        return false;
    }
    const Entity target = foliageTarget();
    if (!target.valid()) return false;
    if (!m_foliage.ground) rebuildFoliageGround();
    const ImGuiIO& io = ImGui::GetIO();
    glm::vec3 origin, dir;
    mouseRay(io.MousePos, pos, size, origin, dir);
    std::optional<shockwave::RayHit> hit;
    if (hovered && !m_looking && m_foliage.ground) hit = m_foliage.ground->raycast(origin, dir, 5000.0f);
    const bool over = hit.has_value();
    if (over) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        constexpr int kSegments = 48;
        ImVec2 points[kSegments];
        int visible = 0;
        for (int i = 0; i < kSegments; ++i) {
            const float a = 6.2831853f * float(i) / kSegments;
            glm::vec3 p = hit->point + glm::vec3(std::cos(a), 0.0f, std::sin(a)) * m_foliage.radius;
            glm::vec3 ground, normal;
            if (foliageGroundAt(p + glm::vec3(0.0f, kGroundHeight, 0.0f), ground, normal)) p = ground;
            if (worldToScreen(p, pos, size, points[visible])) ++visible;
        }
        const auto color = m_foliage.brush == kErase ? gui::currentTheme().error : gui::currentTheme().accent;
        if (visible > 2) draw->AddPolyline(points, visible, gui::semanticColorU32(color), ImDrawFlags_Closed, 2.0f);
    }
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (!m_foliage.stroking && over && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered() &&
        !ImGuizmo::IsOver())
        beginFoliageStroke(target);
    if (m_foliage.stroking && down && over) {
        const glm::mat4 toLocal = glm::inverse(shockwave::worldMatrix(m_world, m_foliage.strokeEntity));
        const glm::vec3 local = glm::vec3(toLocal * glm::vec4(hit->point, 1.0f));
        foliageDab({local.x, local.z});
    }
    if (m_foliage.stroking && !down) endFoliageStroke();
    return over || m_foliage.stroking;
}

void Editor::drawFoliageWindow() {
    if (!m_foliage.open) return;
    ImGui::SetNextWindowSize(ImVec2(360, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Foliage", &m_foliage.open)) {
        ImGui::End();
        return;
    }
    const Entity target = foliageTarget();
    const auto* component = target.valid() ? m_world.get<shockwave::Foliage>(target) : nullptr;
    const auto data = component ? engine().assets().foliage(component->asset) : nullptr;

    if (ImGui::CollapsingHeader("Create", data ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText("Name", m_foliage.newName, sizeof(m_foliage.newName));
        ImGui::SetItemTooltip("Content path, without .swfoliage; the instances go beside it");
        ImGui::InputText("First model", m_foliage.newTypeModel, sizeof(m_foliage.newTypeModel));
        ImGui::SetItemTooltip("A model in Content (.glb, .gltf, ...) for the first type; optional");
        ImGui::BeginDisabled(playing() || !m_hasProject);
        if (ImGui::Button("Create foliage")) createFoliage();
        ImGui::EndDisabled();
    }

    if (!data) {
        ImGui::TextDisabled(
            component ? "The selected foliage's asset isn't loaded" : "No foliage in the scene: create one above");
        m_foliage.brush = -1;
    } else {
        const std::string& asset = component->asset;
        ImGui::Text("%s", asset.c_str());
        ImGui::TextDisabled(
            "%zu instances, %zu types, %zu cells", data->instances.size(), data->types.size(), data->cells.size());

        ImGui::SeparatorText("Types");
        // Edits show straight away; the undo step is made when the edit ends.
        static std::shared_ptr<const FoliageData> editBefore;
        for (size_t t = 0; t < data->types.size(); ++t) {
            ImGui::PushID(int(t));
            shockwave::FoliageType type = data->types[t];
            const std::string label = std::to_string(t) + ": " + type.model;
            if (ImGui::RadioButton(label.c_str(), m_foliage.type == int(t))) m_foliage.type = int(t);
            if (!engine().assets().findModel(type.model).valid()) {
                ImGui::SameLine();
                ImGui::TextColored(gui::currentTheme().warning, "(not loaded)");
            }
            if (ImGui::TreeNode("Settings")) {
                bool changed = false, ended = false;
                auto track = [&](bool edited) {
                    if (ImGui::IsItemActivated()) editBefore = data;
                    changed |= edited;
                    ended |= ImGui::IsItemDeactivatedAfterEdit();
                };
                track(ImGui::DragFloat(
                    "Distance", &type.distance, 1.0f, 1.0f, 5000.0f, "%.0f m", ImGuiSliderFlags_AlwaysClamp));
                track(ImGui::DragFloat(
                    "Fade", &type.fade, 0.5f, 0.0f, type.distance, "%.1f m", ImGuiSliderFlags_AlwaysClamp));
                track(ImGui::SliderFloat("Wind", &type.wind, 0.0f, 1.0f, "%.2f"));
                track(ImGui::SliderFloat("Max slope", &type.maxSlope, 0.0f, 90.0f, "%.0f deg"));
                track(ImGui::DragFloat(
                    "Scale min", &type.scaleMin, 0.01f, 0.01f, type.scaleMax, "%.2f", ImGuiSliderFlags_AlwaysClamp));
                track(ImGui::DragFloat(
                    "Scale max", &type.scaleMax, 0.01f, type.scaleMin, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp));
                bool shadows = type.shadows;
                if (ImGui::Checkbox("Casts shadows", &shadows)) {
                    FoliageData after = *data;
                    after.types[t].shadows = shadows;
                    commitFoliageChange(asset, *data, std::move(after));
                } else if (changed) {
                    type.fade = std::min(type.fade, type.distance);
                    auto after = std::make_shared<FoliageData>(*data);
                    after->types[t] = type;
                    shockwave::finishFoliageEdit(*after);
                    publishFoliage(asset, after);
                    markUnsaved(m_foliage.unsaved, asset);
                }
                if (ended && editBefore) {
                    if (const auto now = engine().assets().foliage(asset)) {
                        m_foliageUndo.push_back(shockwave::makeFoliageEdit(asset, *editBefore, *now));
                        m_undoOrder.push_back(UndoKind::Foliage);
                        m_foliageRedo.clear();
                        m_redo.clear();
                        m_redoOrder.clear();
                        m_dirty = true;
                    }
                    editBefore.reset();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::InputText("Model", m_foliage.newTypeModel, sizeof(m_foliage.newTypeModel));
        ImGui::SetItemTooltip("The new type's model, a Content path");
        ImGui::BeginDisabled(
            playing() || !m_foliage.newTypeModel[0] || data->types.size() >= shockwave::kFoliageMaxTypes);
        if (ImGui::Button("Add type")) {
            FoliageData after = *data;
            shockwave::FoliageType type;
            type.model = m_foliage.newTypeModel;
            after.types.push_back(type);
            commitFoliageChange(asset, *data, std::move(after));
            m_foliage.type = int(data->types.size());
            m_foliage.status = "Added type " + std::to_string(m_foliage.type) + " (" + type.model + ")";
        }
        ImGui::EndDisabled();

        ImGui::SeparatorText("Brush");
        ImGui::BeginDisabled(data->types.empty() || playing());
        static const char* kBrushes[] = {"Paint", "Erase"};
        static const char* kTips[] = {
            "Scatters the chosen type in the circle up to the density, onto whatever is below",
            "Removes the chosen type's instances in the circle",
        };
        for (int b = 0; b < 2; ++b) {
            if (b) ImGui::SameLine();
            const bool active = m_foliage.brush == b;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(kBrushes[b])) {
                m_foliage.brush = active ? -1 : b;
                if (m_foliage.brush >= 0) {
                    m_terrain.brush = -1;   // one brush at a time
                    rebuildFoliageGround();
                }
            }
            if (active) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", kTips[b]);
        }
        ImGui::EndDisabled();
        if (data->types.empty()) m_foliage.brush = -1;
        ImGui::DragFloat("Radius", &m_foliage.radius, 0.1f, 0.5f, 100.0f, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Density", &m_foliage.density, 0.01f, 20.0f, "%.2f per m2", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Instances per square metre that Paint fills the circle up to");
        if (m_foliage.brush >= 0) ImGui::TextDisabled("Paint in the viewport with the left mouse button");
    }

    ImGui::Separator();
    ImGui::BeginDisabled(m_foliage.unsaved.empty() || playing());
    if (ImGui::Button("Save foliage")) saveFoliage();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Writes every painted foliage's .swfoliage and instances (Save scene does too)");
    if (!m_foliage.unsaved.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%zu unsaved", m_foliage.unsaved.size());
    }
    if (!m_foliage.status.empty()) ImGui::TextWrapped("%s", m_foliage.status.c_str());
    ImGui::End();
}

// ---- --foliage-stroke ------------------------------------------------------------------

void Editor::runFoliageStroke() {
    static int frame = 0;
    if (++frame < 3) return;   // the scene and its assets are loaded
    m_options.foliageStroke = false;
    std::vector<std::string> problems;
    auto count = [&](const std::string& asset) {
        const auto data = engine().assets().foliage(asset);
        return data ? data->instances.size() : size_t(0);
    };
    const Entity target = foliageTarget();
    const auto* component = target.valid() ? m_world.get<shockwave::Foliage>(target) : nullptr;
    size_t painted = 0, erased = 0;
    if (!component || !engine().assets().foliage(component->asset)) {
        problems.push_back("the scene has no Foliage with a loaded asset");
    } else {
        const std::string asset = component->asset;
        const size_t start = count(asset);
        m_foliage.type = 0;
        m_foliage.radius = 4.0f;
        m_foliage.density = 2.0f;
        m_foliage.brush = kPaint;
        if (!beginFoliageStroke(target)) problems.push_back("the paint stroke didn't start");
        for (int i = 0; i < 3; ++i) foliageDab({float(i), 0.0f});
        endFoliageStroke();
        const size_t afterPaint = count(asset);
        painted = afterPaint - std::min(afterPaint, start);
        if (painted == 0) problems.push_back("painting added nothing");
        undo();
        if (count(asset) != start) problems.push_back("undo left " + std::to_string(count(asset)) + " instances");
        redo();
        if (count(asset) != afterPaint) problems.push_back("redo left " + std::to_string(count(asset)) + " instances");
        m_foliage.brush = kErase;
        m_foliage.radius = 2.0f;
        beginFoliageStroke(target);
        foliageDab({0.0f, 0.0f});
        endFoliageStroke();
        erased = afterPaint - std::min(afterPaint, count(asset));
        if (erased == 0) problems.push_back("erasing removed nothing");
        undo();
        if (count(asset) != afterPaint) problems.push_back("undoing the erase left " + std::to_string(count(asset)));
        m_foliage.brush = -1;
        if (!saveFoliage()) problems.push_back("saving failed");
        FoliageData saved;
        std::string why;
        const auto now = engine().assets().foliage(asset);
        if (!shockwave::readFoliage(m_assetFolder->root() / asset, m_assetFolder->root(), saved, why))
            problems.push_back("the saved file doesn't read back: " + why);
        else if (!now || saved.instances != now->instances || saved.types != now->types)
            problems.push_back("the saved file differs from the editor's foliage");
    }
    for (const auto& p : problems) std::fprintf(stderr, "foliage stroke: %s\n", p.c_str());
    std::printf(
        "foliage stroke: painted %zu, erased %zu; %s\n", painted, erased,
        problems.empty() ? "undo, redo and save agree" : "FAILED");
    m_exitCode = problems.empty() ? 0 : 1;
    engine().quit();
}

}   // namespace editor
