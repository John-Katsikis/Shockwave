#pragma once

// A mesh as a signed distance field: a grid of distances to its surface, sampled with
// trilinear interpolation. Negative inside, positive outside (for closed meshes; open ones
// like planes get a sign from the nearest triangle's facing, which is only right near it).

#include <Renderer/Mesh.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace shockwave {

class JobSystem;

namespace raymarch {

struct DistanceField {
    glm::vec3 min{0.0f}, max{0.0f};   // the grid's box in the mesh's own space
    uint32_t size = 0;   // cells per side (a power of two, for the octree)
    float cell = 0.0f;   // cell edge, mesh units
    std::vector<float> distances;   // size^3 samples at cell centres, x fastest
    glm::vec3 colour{1.0f};   // average vertex colour, until surfaces keep their own

    // Bakes by brute force: every cell against every triangle, spread across the job
    // system. Fine for a skeleton; a BVH over the triangles is the obvious speed-up.
    static DistanceField
    bake(std::span<const Vertex> vertices, std::span<const uint32_t> indices, uint32_t size, JobSystem* jobs);

    float at(int x, int y, int z) const;
    // Distance at a point in mesh space; outside the grid, a safe underestimate.
    float sample(const glm::vec3& p) const;
    glm::vec3 gradient(const glm::vec3& p) const;   // points away from the surface
    bool valid() const { return size > 0; }
};

}   // namespace raymarch
}   // namespace shockwave
