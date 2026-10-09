#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace shockwave {

// The engine's standard vertex. Shaders read it straight from GPU memory (vertex pulling),
// so its layout here is the layout in the shader: tightly packed, 48 bytes.
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 color{1.0f};
};
static_assert(sizeof(Vertex) == 48, "Vertex must match the shader's scalar layout");

// CPU-side mesh, ready to upload with Renderer::createMesh.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;   // counter-clockwise triangles are front faces
};

// Built-in shapes, centred on the origin.
MeshData makeCube(float size = 1.0f);
MeshData makePlane(float size = 1.0f, float uvRepeat = 1.0f);   // in XZ, facing +Y
MeshData makeSphere(float radius = 0.5f, uint32_t segments = 48, uint32_t rings = 24);

} // namespace shockwave
