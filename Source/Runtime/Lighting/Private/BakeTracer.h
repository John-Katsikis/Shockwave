#pragma once

// The baker's ray tracing (G05, G06): a binned-SAH BVH over the static scene's world-space
// triangles, closest-hit and occlusion queries that both honour alpha masks, and the
// path estimator of docs/GraphicsImplementation.md 9.7.

#include <Engine/BakedLighting.h>

#include <cstdint>
#include <vector>

namespace shockwave::bake {

// Counter-based randomness: the same key always gives the same numbers, whichever thread
// or order computes them.
inline uint64_t mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
struct Random {
    uint64_t state;
    explicit Random(uint64_t seed) : state(mix64(seed)) {}
    float next() {
        state = mix64(state);
        return float(state >> 40) * (1.0f / 16777216.0f);   // 24 bits: [0, 1)
    }
};

glm::vec3 cosineDirection(const glm::vec3& n, float u1, float u2);
glm::vec3 sphereDirection(uint32_t i, uint32_t count);   // Fibonacci sphere, unit
float radicalInverse(uint32_t bits);   // van der Corput, base 2

struct Hit {
    float t = 0;
    uint32_t triangle = 0;
    float u = 0, v = 0;   // barycentrics of vertices 1 and 2
};

// What the path estimator needs to know about a hit.
struct SurfacePoint {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};   // shading normal, facing the incoming ray's side
    glm::vec3 geometric{0.0f, 1.0f, 0.0f};   // facing the same side as normal
    bool backface = false;   // hit the back of a one-sided surface
    glm::vec3 reflectance{0.0f};
    glm::vec3 emissive{0.0f};
};

class BakeScene {
public:
    explicit BakeScene(const BakeSceneDescription& description);
    size_t triangleCount() const { return m_triangles.size(); }
    // Closest opaque hit within (0, tMax), alpha-masked texels skipped.
    bool intersect(const glm::vec3& origin, const glm::vec3& direction, float tMax, Hit& hit) const;
    bool occluded(const glm::vec3& origin, const glm::vec3& direction, float tMax) const;
    SurfacePoint surface(const Hit& hit, const glm::vec3& direction) const;
    // Baked lights arriving at a point facing n, with visibility, in the renderer's scale
    // (a white surface facing a unit light gets 1).
    glm::vec3 direct(const glm::vec3& position, const glm::vec3& normal) const;
    // Radiance arriving at origin from direction, allowing bounces diffuse reflections
    // (9.7's traceIncoming, without recursion). firstHit reports the first hit's distance
    // (or tMax-free infinity), backface whether it was the back of a one-sided surface.
    glm::vec3 incoming(
        const glm::vec3& origin, glm::vec3 direction, uint32_t bounces, Random& random, float* firstHit = nullptr,
        bool* backface = nullptr) const;
    // An origin just off a surface: scale-aware, so large worlds and thin walls both work.
    static glm::vec3 offset(const glm::vec3& position, const glm::vec3& normal);
    const BakeSceneDescription& description() const { return m_description; }

private:
    struct Triangle {
        glm::vec3 v0, e1, e2;
        uint32_t surface;
        uint32_t first;   // first index of the triangle in the surface's mesh
    };
    struct Node {
        glm::vec3 min, max;
        uint32_t start, count;   // leaf: triangles start..start+count; inner: count 0, children start, start+1
    };
    template <class Accept>
    bool traverse(
        const glm::vec3& origin, const glm::vec3& direction, float tMax, bool anyHit, Hit& hit, Accept&& accept) const;
    bool opaque(const Triangle& tri, float u, float v) const;
    glm::vec4 baseColor(const BakeSurface& s, uint32_t first, float u, float v) const;
    void build();

    const BakeSceneDescription& m_description;
    std::vector<Triangle> m_triangles;
    std::vector<Node> m_nodes;
    std::vector<glm::mat3> m_normalMatrices;   // per surface
    std::vector<float> m_handedness;   // per surface: -1 where the transform mirrors
};

}   // namespace shockwave::bake
