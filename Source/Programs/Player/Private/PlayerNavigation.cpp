// player --bake-navmesh: bakes each scene's navigation mesh the way Play would, saves it
// next to the scene, and reports its size, the bake time and what the scene's agents
// cost per fixed tick.
#include "Player.h"

#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/SceneFile.h>
#include <GameFramework/PlaySession.h>
#include <Navigation/Navigation.h>

#include <algorithm>
#include <cstdio>

namespace player {

std::vector<fs::path> Player::projectScenes() const {
    std::vector<fs::path> scenes = m_tool.scenes;
    if (!scenes.empty()) return scenes;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(m_project.sceneFolder, ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".swscene") scenes.push_back(entry.path());
    if (std::find(scenes.begin(), scenes.end(), m_project.startupScene) == scenes.end())
        scenes.push_back(m_project.startupScene);
    std::sort(scenes.begin(), scenes.end());
    return scenes;
}

int Player::bakeNavMeshes() {
    int failures = 0;
    for (const fs::path& scene : projectScenes()) {
        const std::string shown = m_project.relative(scene);
        shockwave::World world;
        std::string error;
        shockwave::SceneFileHooks hooks;
        hooks.readHeader = [](const std::string&, std::istream&) {};
        if (!shockwave::loadScene(world, engine().assets(), scene.string(), error, hooks)) {
            std::printf("%s: %s\n", shown.c_str(), error.c_str());
            ++failures;
            continue;
        }
        if (!shockwave::sceneUsesNavigation(world)) {
            std::printf("%s: no NavMeshSettings or NavAgent; skipped\n", shown.c_str());
            continue;
        }
        // Baked from scratch, so the time is a full bake's.
        const shockwave::NavMeshSettings settings = shockwave::findNavMeshSettings(world);
        const shockwave::NavGeometry geometry = shockwave::collectNavGeometry(world, &engine().assets(), settings);
        const auto mesh = shockwave::bakeNavMesh(geometry, settings, engine().jobs(), error);
        if (!mesh) {
            std::printf("%s: bake failed: %s\n", shown.c_str(), error.c_str());
            ++failures;
            continue;
        }
        const shockwave::NavMeshStats& s = mesh->stats();
        std::printf(
            "%s: %d tiles, %d polygons, %d vertices from %zu triangles (%zu objects) in %.2f ms on %u threads, %.1f "
            "KB\n",
            shown.c_str(), s.tiles, s.polygons, s.vertices, s.inputTriangles, geometry.sources, s.bakeMs,
            engine().jobs().threadCount(), static_cast<double>(s.bytes) / 1024.0);
        const fs::path file = shockwave::navMeshPathFor(scene);
        if (mesh->save(file, error)) {
            std::printf("  wrote %s\n", m_project.relative(file).c_str());
        } else {
            std::printf("  %s\n", error.c_str());
            ++failures;
        }

        // Play it headless for 10 simulated seconds to measure the crowd, and say where
        // each agent ended up.
        try {
            shockwave::PlaySession session(world, engine().jobs(), *m_instance, &engine());
            session.start();
            shockwave::NavigationWorld& navigation = *session.navigation();
            for (const auto& d : navigation.diagnostics()) {
                const auto* name = world.get<shockwave::Name>(d.entity);
                std::printf("  problem: %s%s\n", name ? ("'" + name->value + "': ").c_str() : "", d.message.c_str());
            }
            const double dt = 1.0 / m_project.tickRate;
            const int ticks = static_cast<int>(m_project.tickRate * 10.0);
            double total = 0.0, worst = 0.0;
            for (int i = 0; i < ticks; ++i) {
                session.update(dt);
                total += navigation.lastStepMs();
                worst = std::max(worst, navigation.lastStepMs());
            }
            std::printf(
                "  crowd: %u agents, %.4f ms per tick on average, %.4f ms at worst, over %d ticks\n",
                navigation.agentCount(), total / ticks, worst, ticks);
            world.each<const shockwave::NavAgent>([&](shockwave::Entity e, const shockwave::NavAgent& agent) {
                const auto* name = world.get<shockwave::Name>(e);
                const glm::vec3 p = world.has<shockwave::Transform>(e) ? glm::vec3(shockwave::worldMatrix(world, e)[3])
                                                                       : glm::vec3(0.0f);
                std::printf(
                    "  agent '%s': at (%.2f, %.2f, %.2f), %s, %.2f from its goal\n", name ? name->value.c_str() : "?",
                    p.x, p.y, p.z,
                    !agent.onNavMesh ? "off the mesh"
                    : agent.arrived  ? "arrived"
                                     : "walking",
                    agent.remainingDistance);
            });
            session.stop();
        } catch (const std::exception& e) {
            std::printf("  gameplay failed to start: %s\n", e.what());
            ++failures;
        }
    }
    return failures ? 1 : 0;
}

}   // namespace player
