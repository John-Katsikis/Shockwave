// NavMeshOverlay: the navigation mesh and agents' paths, drawn through the Renderer like
// any other mesh, so it shows in the editor and in player screenshots alike, and the
// headless renderer skips it for free.
#include <Navigation/Navigation.h>

#include <Assets/Assets.h>
#include <Engine/Components.h>
#include <Renderer/Renderer.h>

#include <glm/gtc/matrix_transform.hpp>

namespace shockwave {

namespace {

DrawMaterial overlayMaterial(const glm::vec4& color) {
    DrawMaterial material;
    material.blend = color.a < 1.0f;
    material.doubleSided = true;
    material.castShadows = false;
    material.roughness = 1.0f;
    // Mostly self-lit, so it reads the same in shadow and in sunlight.
    material.emissive = glm::vec3(color) * 0.35f;
    return material;
}

}   // namespace

NavMeshOverlay::~NavMeshOverlay() = default;

void NavMeshOverlay::release(Renderer& renderer) {
    renderer.destroyMesh(m_mesh);
    m_mesh = {};
    m_uploaded.reset();
}

void NavMeshOverlay::drawMesh(Renderer& renderer, const std::shared_ptr<const NavMesh>& mesh, const Style& style) {
    if (mesh != m_uploaded) {
        // A new bake: swap the GPU copy. Rare (only after a bake), so the wait is fine.
        release(renderer);
        if (mesh && !mesh->debugMesh().indices.empty()) m_mesh = renderer.createMesh(mesh->debugMesh());
        m_uploaded = mesh;
    }
    if (!m_mesh.valid()) return;
    renderer.drawMesh(m_mesh, glm::mat4(1.0f), {}, style.mesh, overlayMaterial(style.mesh));
}

void NavMeshOverlay::drawPaths(
    Renderer& renderer, const Assets& assets, const NavigationWorld& navigation, const World& world,
    const Style& style) {
    const MeshHandle cube = assets.findMesh("cube");
    if (!cube.valid()) return;
    const DrawMaterial material = overlayMaterial(style.path);
    constexpr float kWidth = 0.06f, kLift = 0.08f;
    const auto* agents = world.pool<NavAgent>();
    if (!agents) return;
    for (Entity e : agents->entities()) {
        if (!navigation.agentPath(e, m_points)) continue;
        for (size_t i = 0; i + 1 < m_points.size(); ++i) {
            const glm::vec3 a = m_points[i] + glm::vec3(0, kLift, 0);
            const glm::vec3 b = m_points[i + 1] + glm::vec3(0, kLift, 0);
            const glm::vec3 d = b - a;
            const float length = glm::length(d);
            if (length < 1e-3f) continue;
            // A unit cube stretched along the segment: x across, y up, z along it.
            const glm::vec3 forward = d / length;
            const glm::vec3 side = glm::normalize(
                glm::cross(std::abs(forward.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0), forward));
            const glm::vec3 up = glm::cross(forward, side);
            glm::mat4 m(1.0f);
            m[0] = glm::vec4(side * kWidth, 0.0f);
            m[1] = glm::vec4(up * kWidth, 0.0f);
            m[2] = glm::vec4(forward * length, 0.0f);
            m[3] = glm::vec4((a + b) * 0.5f, 1.0f);
            renderer.drawMesh(cube, m, {}, style.path, material);
        }
        // A small post at the end of the path.
        const glm::vec3 end = m_points.back();
        const glm::mat4 post =
            glm::scale(glm::translate(glm::mat4(1.0f), end + glm::vec3(0, 0.3f, 0)), glm::vec3(0.12f, 0.6f, 0.12f));
        renderer.drawMesh(cube, post, {}, style.path, material);
    }
}

}   // namespace shockwave
