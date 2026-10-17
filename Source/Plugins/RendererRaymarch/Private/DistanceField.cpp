#include "DistanceField.h"

#include <Core/Jobs.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace shockwave::raymarch {

namespace {

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection, 5.1.5).
glm::vec3 closestOnTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

}   // namespace

DistanceField DistanceField::bake(
    std::span<const Vertex> vertices, std::span<const uint32_t> indices, uint32_t size, JobSystem* jobs) {
    DistanceField field;
    if (vertices.empty() || indices.size() < 3) return field;
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    glm::vec3 colour(0.0f);
    for (const Vertex& v : vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
        colour += glm::vec3(v.color);
    }
    field.colour = colour / float(vertices.size());
    // A cube around the mesh with a margin, so rays start outside the surface and flat
    // meshes still get a few cells of thickness.
    const glm::vec3 centre = (lo + hi) * 0.5f;
    const float extent = std::max(glm::length(hi - lo) * 0.5f, 1e-3f) * 1.1f;
    field.size = size;
    field.min = centre - glm::vec3(extent);
    field.max = centre + glm::vec3(extent);
    field.cell = 2.0f * extent / float(size);
    field.distances.assign(size_t(size) * size * size, 0.0f);

    struct Triangle {
        glm::vec3 a, b, c, normal;
    };
    std::vector<Triangle> triangles;
    triangles.reserve(indices.size() / 3);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        Triangle t{
            vertices[indices[i]].position, vertices[indices[i + 1]].position, vertices[indices[i + 2]].position, {}};
        const glm::vec3 n = glm::cross(t.b - t.a, t.c - t.a);
        const float length = glm::length(n);
        if (length < 1e-12f) continue;
        t.normal = n / length;
        triangles.push_back(t);
    }
    const auto slices = [&](size_t begin, size_t end) {
        for (size_t z = begin; z < end; ++z)
            for (uint32_t y = 0; y < size; ++y)
                for (uint32_t x = 0; x < size; ++x) {
                    const glm::vec3 p = field.min + (glm::vec3(x, y, z) + 0.5f) * field.cell;
                    float best = std::numeric_limits<float>::max();
                    float sign = 1.0f;
                    for (const Triangle& t : triangles) {
                        const glm::vec3 q = closestOnTriangle(p, t.a, t.b, t.c);
                        const float d = glm::dot(p - q, p - q);
                        if (d < best) {
                            best = d;
                            sign = glm::dot(p - q, t.normal) >= 0.0f ? 1.0f : -1.0f;
                        }
                    }
                    field.distances[(z * size + y) * size + x] = sign * std::sqrt(best);
                }
    };
    if (jobs && JobSystem::threadIndex() != JobSystem::kForeignThread)
        jobs->parallelFor(size, 1, slices);
    else
        slices(0, size);
    return field;
}

float DistanceField::at(int x, int y, int z) const {
    const int n = int(size) - 1;
    x = std::clamp(x, 0, n);
    y = std::clamp(y, 0, n);
    z = std::clamp(z, 0, n);
    return distances[(size_t(z) * size + size_t(y)) * size + size_t(x)];
}

float DistanceField::sample(const glm::vec3& p) const {
    // Outside the box, both the distance to the box and (by the triangle inequality) the
    // distance at the box's edge minus the way there are lower bounds: safe steps.
    const glm::vec3 clamped = glm::clamp(p, min, max);
    const float outside = glm::length(p - clamped);
    const glm::vec3 g = (clamped - min) / cell - 0.5f;
    const glm::ivec3 i = glm::ivec3(glm::floor(g));
    const glm::vec3 f = g - glm::vec3(i);
    const float c00 = glm::mix(at(i.x, i.y, i.z), at(i.x + 1, i.y, i.z), f.x);
    const float c10 = glm::mix(at(i.x, i.y + 1, i.z), at(i.x + 1, i.y + 1, i.z), f.x);
    const float c01 = glm::mix(at(i.x, i.y, i.z + 1), at(i.x + 1, i.y, i.z + 1), f.x);
    const float c11 = glm::mix(at(i.x, i.y + 1, i.z + 1), at(i.x + 1, i.y + 1, i.z + 1), f.x);
    const float inside = glm::mix(glm::mix(c00, c10, f.y), glm::mix(c01, c11, f.y), f.z);
    return outside > 0.0f ? std::max(outside, inside - outside) : inside;
}

glm::vec3 DistanceField::gradient(const glm::vec3& p) const {
    const float h = cell * 0.5f;
    const glm::vec3 g(
        sample(p + glm::vec3(h, 0, 0)) - sample(p - glm::vec3(h, 0, 0)),
        sample(p + glm::vec3(0, h, 0)) - sample(p - glm::vec3(0, h, 0)),
        sample(p + glm::vec3(0, 0, h)) - sample(p - glm::vec3(0, 0, h)));
    const float length = glm::length(g);
    return length > 1e-12f ? g / length : glm::vec3(0, 1, 0);
}

}   // namespace shockwave::raymarch
