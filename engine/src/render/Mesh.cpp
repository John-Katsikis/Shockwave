#include <shockwave/Mesh.h>

#include <algorithm>
#include <cmath>

namespace shockwave {

MeshData makeCube(float size) {
    const float h = size * 0.5f;
    struct Face {
        glm::vec3 normal, u, v;   // u and v span the face so u x v = normal (CCW from outside)
    };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };

    MeshData mesh;
    for (const Face& f : faces) {
        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        const glm::vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const glm::vec2& c : corners) {
            Vertex v;
            v.position = (f.normal + f.u * c.x + f.v * c.y) * h;
            v.normal = f.normal;
            v.uv = {(c.x + 1.0f) * 0.5f, 1.0f - (c.y + 1.0f) * 0.5f};
            mesh.vertices.push_back(v);
        }
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return mesh;
}

MeshData makePlane(float size, float uvRepeat) {
    const float h = size * 0.5f;
    MeshData mesh;
    const glm::vec2 corners[4] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    for (const glm::vec2& c : corners) {
        Vertex v;
        v.position = {c.x * h, 0.0f, c.y * h};
        v.normal = {0.0f, 1.0f, 0.0f};
        v.uv = {(c.x + 1.0f) * 0.5f * uvRepeat, (c.y + 1.0f) * 0.5f * uvRepeat};
        mesh.vertices.push_back(v);
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

MeshData makeSphere(float radius, uint32_t segments, uint32_t rings) {
    segments = std::max(segments, 3u);
    rings = std::max(rings, 2u);
    const float pi = 3.14159265358979f;
    MeshData mesh;
    // Rings run top to bottom, segments around Y. The seam column is duplicated so UVs wrap.
    for (uint32_t i = 0; i <= rings; ++i) {
        const float theta = pi * static_cast<float>(i) / rings;
        for (uint32_t j = 0; j <= segments; ++j) {
            const float phi = 2.0f * pi * static_cast<float>(j) / segments;
            const glm::vec3 n{std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi)};
            Vertex v;
            v.position = n * radius;
            v.normal = n;
            v.uv = {static_cast<float>(j) / segments, static_cast<float>(i) / rings};
            mesh.vertices.push_back(v);
        }
    }
    const uint32_t stride = segments + 1;
    for (uint32_t i = 0; i < rings; ++i) {
        for (uint32_t j = 0; j < segments; ++j) {
            const uint32_t a = i * stride + j, b = a + stride;
            mesh.indices.insert(mesh.indices.end(), {a, b, b + 1, a, b + 1, a + 1});
        }
    }
    return mesh;
}

} // namespace shockwave
