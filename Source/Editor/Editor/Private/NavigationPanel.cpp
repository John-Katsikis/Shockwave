// Navigation in the editor: Create menu entries, a Bake button and the mesh's state in
// the Inspector, the navigation mesh and agents' paths in the viewport, and a Navigation
// section in Window > Stats. While editing, the viewport shows the mesh the editor last
// baked or loaded; during Play, the session's mesh and where each agent is walking.
// See docs/Navigation.md.
#include "Editor.h"

#include <Engine/Components.h>
#include <Navigation/Navigation.h>
#include <UI/Gui.h>

#include <cmath>
#include <format>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::Entity;
using shockwave::kNullEntity;

namespace {

// How often the editor checks whether the scene still matches the baked mesh. The
// check hashes the scene's static geometry, so it isn't done every frame.
constexpr double kStaleCheckSeconds = 0.5;

glm::vec4 themeColor(const ImVec4& c, float alpha) {
    // Theme colors are sRGB; the renderer wants linear.
    auto linear = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return {linear(c.x), linear(c.y), linear(c.z), alpha};
}

}   // namespace

void Editor::updateNavigation(double dt) {
    if (!m_hasProject) return;
    // A different scene: show its saved mesh, if it has one.
    if (m_navScene != m_scenePath) {
        m_navScene = m_scenePath;
        m_navMesh.reset();
        m_navStale = false;
        m_navCheckTimer = 0.0;
        if (!m_scenePath.empty()) {
            std::string err;
            m_navMesh = shockwave::loadSceneNavMesh(m_scenePath, err);
            if (!err.empty()) warn(err);
        }
    }
    if (playing()) return;
    m_navCheckTimer -= dt;
    if (m_navCheckTimer > 0.0) return;
    m_navCheckTimer = kStaleCheckSeconds;
    m_navUsed = shockwave::sceneUsesNavigation(m_world);
    if (!m_navUsed) {
        m_navStale = false;
        return;
    }
    const auto settings = shockwave::findNavMeshSettings(m_world);
    const uint64_t hash = shockwave::navInputHash(m_world, &engine().assets(), settings);
    m_navStale = !m_navMesh || m_navMesh->inputHash() != hash;
    // Baked this run with the same inputs (undo, or a scene reopened): use that one.
    if (m_navStale) {
        if (auto cached = shockwave::navcache::find(hash)) {
            m_navMesh = std::move(cached);
            m_navStale = false;
        }
    }
}

bool Editor::bakeNavigation() {
    if (playing()) {
        warn("Stop playing before baking: the scene is mid-simulation");
        return false;
    }
    const auto settings = shockwave::findNavMeshSettings(m_world);
    // On the job system, so it uses every core; tiles that didn't change are reused.
    const shockwave::NavGeometry geometry = shockwave::collectNavGeometry(m_world, &engine().assets(), settings);
    std::string err;
    auto mesh = shockwave::bakeNavMesh(geometry, settings, engine().jobs(), err, m_navMesh.get());
    if (!mesh) {
        error("Navigation bake failed: " + err);
        return false;
    }
    m_navMesh = std::move(mesh);
    m_navStale = false;
    m_navCheckTimer = kStaleCheckSeconds;
    m_showNavMesh = true;
    const shockwave::NavMeshStats& s = m_navMesh->stats();
    log(std::format(
        "Baked the navigation mesh: {} polygons in {} tiles ({} rebuilt) from {} triangles, {:.1f} ms", s.polygons,
        s.tiles, s.tilesRebuilt, s.inputTriangles, s.bakeMs));
    if (m_navMesh->empty())
        warn("The navigation mesh has no walkable polygons. Check agent_radius and max_slope, and that the ground is "
             "static (a Collider without a dynamic RigidBody, or a mesh).");
    // Saved next to the scene so Play and the game load it instead of baking.
    if (m_scenePath.empty()) {
        log("Save the scene to keep the navigation mesh next to it");
    } else if (const fs::path file = shockwave::navMeshPathFor(m_scenePath); m_navMesh->save(file, err)) {
        log("Saved " + m_project.relative(file));
    } else {
        error(err);
    }
    return true;
}

void Editor::renderNavigation(shockwave::Renderer& renderer) {
    // Also shown while a navigation entity is selected, as you'd want when editing one.
    const Entity e = selected();
    const bool editingNavigation =
        e.valid() && (m_world.has<shockwave::NavMeshSettings>(e) || m_world.has<shockwave::NavAgent>(e));
    if (!m_showNavMesh && !editingNavigation) return;
    shockwave::NavMeshOverlay::Style style;
    style.mesh = themeColor(gui::currentTheme().accent, 0.4f);
    style.path = themeColor(gui::currentTheme().warning, 1.0f);
    const shockwave::NavigationWorld* navigation = m_session ? m_session->navigation() : nullptr;
    if (navigation && playing()) {
        m_navOverlay.drawMesh(renderer, navigation->sharedNavMesh(), style);
        m_navOverlay.drawPaths(renderer, engine().assets(), *navigation, m_world, style);
    } else {
        m_navOverlay.drawMesh(renderer, m_navMesh, style);
    }
}

void Editor::reportNavigationProblems() {
    const shockwave::NavigationWorld* navigation = m_session ? m_session->navigation() : nullptr;
    if (!navigation) return;
    for (const auto& problem : navigation->diagnostics())
        warn((problem.entity.valid() ? displayName(problem.entity) + ": " : "Navigation: ") + problem.message);
    if (navigation->baked())
        log("Baked the navigation mesh for Play; Bake on NavMeshSettings saves one so Play starts faster");
}

void Editor::drawNavigationCreateItems() {
    ImGui::SeparatorText("Navigation");
    bool hasSettings = false;
    m_world.each<const shockwave::NavMeshSettings>([&](const shockwave::NavMeshSettings&) { hasSettings = true; });
    if (ImGui::MenuItem("Navigation settings", nullptr, false, !hasSettings)) {
        pushUndo(m_world);
        const Entity e = m_world.create();
        m_world.add<shockwave::Name>(e, {uniqueName("Navigation")});
        m_world.add<shockwave::NavMeshSettings>(e, {});
        select(e);
        bakeNavigation();
    }
    ImGui::SetItemTooltip(
        hasSettings ? "The scene already has NavMeshSettings" : "How the walkable area is baked; one per scene");
    if (ImGui::MenuItem("Nav agent")) {
        pushUndo(m_world);
        const Entity e = m_world.create();
        glm::vec3 at = placementPoint();
        if (m_navMesh)
            if (auto snapped = m_navMesh->nearestPoint(at, {4.0f, 8.0f, 4.0f})) at = *snapped;
        m_world.add<shockwave::Name>(e, {uniqueName("Agent")});
        m_world.add<shockwave::Transform>(e, {at});
        // A body to see: a sphere stretched to the agent's default height, standing on the ground.
        const Entity body = m_world.create();
        m_world.add<shockwave::Name>(body, {uniqueName("Body")});
        shockwave::Transform t{glm::vec3(0.0f, 0.9f, 0.0f)};
        t.scale = {0.8f, 1.8f, 0.8f};
        m_world.add<shockwave::Transform>(body, t);
        shockwave::MeshRenderer mesh;
        mesh.mesh = engine().assets().findMesh("sphere");
        m_world.add<shockwave::MeshRenderer>(body, mesh);
        m_world.add<shockwave::Parent>(body, {e});
        m_world.add<shockwave::NavAgent>(e, {});
        select(e);
        log("Created " + displayName(e) +
            ". Give it a destination or a target in the Inspector, or from game code "
            "with game.navigation()");
    }
    ImGui::SetItemTooltip("Walks the navigation mesh during Play, steering round walls and other agents");
}

void Editor::drawNavigationNotes(const shockwave::ComponentInfo& info, Entity e) {
    const bool settings = info.id == shockwave::componentId<shockwave::NavMeshSettings>();
    const bool agent = info.id == shockwave::componentId<shockwave::NavAgent>();
    if (!settings && !agent) return;
    const shockwave::NavigationWorld* navigation = m_session && playing() ? m_session->navigation() : nullptr;
    const auto errorText = [](const std::string& text) {
        ImGui::PushStyleColor(ImGuiCol_Text, gui::semanticColor(gui::currentTheme().error));
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
    };
    if (navigation) {
        for (const auto& problem : navigation->diagnostics())
            if (problem.entity == e || (settings && !problem.entity.valid())) errorText(problem.message);
    }
    if (agent) {
        const auto& a = *m_world.get<shockwave::NavAgent>(e);
        if (navigation) {
            ImGui::TextDisabled(
                "%s, %.2f from its goal, %.2f m/s",
                !a.onNavMesh ? "Off the mesh"
                : a.arrived  ? "Arrived"
                             : "Walking",
                static_cast<double>(a.remainingDistance), static_cast<double>(glm::length(a.velocity)));
        } else if (m_navMesh && m_world.has<shockwave::Transform>(e)) {
            const glm::vec3 p(shockwave::worldMatrix(m_world, e)[3]);
            if (!m_navMesh->nearestPoint(
                    p, {std::max(a.radius * 2.0f, 0.5f), std::max(a.height, 1.0f), std::max(a.radius * 2.0f, 0.5f)}))
                errorText("Off the navigation mesh, so it can't move. Place it on walkable ground.");
        }
        return;
    }
    for (const std::string& problem : shockwave::validateNavMeshSettings(*m_world.get<shockwave::NavMeshSettings>(e)))
        errorText(problem);
    ImGui::BeginDisabled(playing());
    if (gui::button(m_navStale ? "Bake (out of date)" : "Bake")) bakeNavigation();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Bake the walkable area from the scene's static geometry and save it next to the scene");
    gui::toolbarNext(gui::buttonWidth("Show mesh"));
    if (gui::toolButton(
            "Show mesh", m_showNavMesh,
            "Keep the navigation mesh in the viewport when no navigation entity is selected"))
        m_showNavMesh = !m_showNavMesh;
    if (!m_navMesh) {
        ImGui::TextDisabled("Not baked yet. Play bakes it if needed.");
        return;
    }
    const shockwave::NavMeshStats& s = m_navMesh->stats();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(
        "%d polygons, %d tiles, %.1f KB, baked in %.1f ms", s.polygons, s.tiles, static_cast<double>(s.bytes) / 1024.0,
        s.bakeMs);
    if (m_navStale) ImGui::TextWrapped("The scene changed since this bake; Play bakes it again until you do.");
    ImGui::PopStyleColor();
}

void Editor::drawNavigationStats() {
    ImGui::SeparatorText("Navigation (Recast & Detour)");
    const shockwave::NavigationWorld* navigation = m_session && playing() ? m_session->navigation() : nullptr;
    const shockwave::NavMesh* mesh = navigation ? navigation->navMesh() : m_navMesh.get();
    if (!mesh) {
        ImGui::TextDisabled(m_navUsed ? "Not baked yet" : "No NavMeshSettings or NavAgent in this scene");
        return;
    }
    if (gui::beginPropertyGrid("navigation")) {
        const shockwave::NavMeshStats& s = mesh->stats();
        gui::propertyLabel("Mesh");
        ImGui::Text("%d polygons, %d tiles, %.1f KB", s.polygons, s.tiles, static_cast<double>(s.bytes) / 1024.0);
        gui::propertyLabel("Last bake");
        ImGui::Text("%.1f ms on %u threads", s.bakeMs, engine().jobs().threadCount());
        if (navigation) {
            gui::propertyLabel("Agents");
            ImGui::Text("%u", navigation->agentCount());
            gui::propertyLabel("CPU per tick");
            ImGui::Text("%.3f ms (%.3f ms average)", navigation->lastStepMs(), navigation->averageStepMs());
        }
        ImGui::EndTable();
    }
    if (!navigation) ImGui::TextDisabled("Agents move during Play");
}

}   // namespace editor
