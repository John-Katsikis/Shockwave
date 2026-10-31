#include <Renderer/Clusters.h>

#include <algorithm>
#include <cmath>

namespace shockwave {

const char* lightCullingName(LightCulling culling) {
    switch (culling) {
    case LightCulling::Auto: return "auto";
    case LightCulling::Clustered: return "clustered";
    case LightCulling::BruteForce: return "brute_force";
    }
    return "auto";
}

ClusterGrid clusterGrid(uint32_t width, uint32_t height, uint64_t budgetBytes) {
    ClusterGrid grid;
    grid.slices = kClusterSlices;
    grid.tileSize = kClusterTileSize;
    for (;;) {
        grid.tilesX = std::max(1u, (width + grid.tileSize - 1) / grid.tileSize);
        grid.tilesY = std::max(1u, (height + grid.tileSize - 1) / grid.tileSize);
        if (grid.bytes() <= budgetBytes || (grid.tilesX == 1 && grid.tilesY == 1)) return grid;
        grid.tileSize *= 2;
    }
}

uint32_t clusterSlice(float z, float nearPlane, float farPlane, uint32_t slices) {
    const float t = std::log(std::max(z, 1e-6f) / nearPlane) / std::log(farPlane / nearPlane);
    const float s = std::floor(t * float(slices));
    return uint32_t(std::clamp(s, 0.0f, float(slices - 1)));
}

float clusterSliceStart(uint32_t s, float nearPlane, float farPlane, uint32_t slices) {
    return nearPlane * std::pow(farPlane / nearPlane, float(s) / float(slices));
}

ClusterBounds clusterBounds(
    const ClusterGrid& grid, uint32_t x, uint32_t y, uint32_t s, const glm::vec2& renderSize,
    const glm::mat4& inverseProjection, float nearPlane, float farPlane) {
    const glm::vec2 lo = glm::vec2(x, y) * float(grid.tileSize) - 1.0f;
    const glm::vec2 hi = glm::vec2(x + 1, y + 1) * float(grid.tileSize) + 1.0f;
    const float z0 = clusterSliceStart(s, nearPlane, farPlane, grid.slices) * 0.999f;
    const float z1 = clusterSliceStart(s + 1, nearPlane, farPlane, grid.slices) * 1.001f;
    ClusterBounds box{glm::vec3(1e30f), glm::vec3(-1e30f)};
    for (int corner = 0; corner < 4; ++corner) {
        const glm::vec2 pixel((corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y);
        const glm::vec2 ndc(2.0f * pixel.x / renderSize.x - 1.0f, 1.0f - 2.0f * pixel.y / renderSize.y);
        // On the near plane (reverse-Z depth 1), then along its ray to each slice distance.
        const glm::vec4 h = inverseProjection * glm::vec4(ndc, 1.0f, 1.0f);
        const glm::vec3 p = glm::vec3(h) / h.w;
        for (float z : {z0, z1}) {
            const glm::vec3 q = p * (z / -p.z);
            box.lo = glm::min(box.lo, q);
            box.hi = glm::max(box.hi, q);
        }
    }
    return box;
}

bool sphereTouchesBox(const glm::vec3& centre, float radius, const ClusterBounds& box) {
    const glm::vec3 nearest = glm::clamp(centre, box.lo, box.hi);
    const glm::vec3 d = centre - nearest;
    const float r = radius * 1.001f + 1e-4f;
    return glm::dot(d, d) <= r * r;
}

float projectionNearPlane(const glm::mat4& projection) {
    if (projection[2][3] == 0.0f) return 0.0f;   // w doesn't depend on z: orthographic
    const glm::vec4 h = glm::inverse(projection) * glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    if (!(std::abs(h.w) > 1e-20f)) return 0.0f;
    const float nearPlane = -h.z / h.w;
    return std::isfinite(nearPlane) && nearPlane > 0.0f ? nearPlane : 0.0f;
}

}   // namespace shockwave
