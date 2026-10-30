#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/type_precision.hpp>

#include <cstdint>
#include <vector>

namespace shockwave {

// The engine's standard vertex. Shaders read it straight from GPU memory (vertex pulling),
// so its layout here is the layout in the shader: tightly packed, 64 bytes.
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 color{1.0f};
    // For normal maps: xyz points along +u, w is +1 or -1 so that cross(normal, xyz) * w
    // points up the texture (glTF's convention). Model import fills it in.
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
};
static_assert(sizeof(Vertex) == 64, "Vertex must match the shader's scalar layout");

// Which joints move a vertex of a skinned mesh, and how much (Renderer::createSkinnedMesh).
// Joints index the mesh's own joint list, the matrices given to drawSkinnedMesh. Weights
// add up to 1; unused slots have weight 0.
struct VertexSkin {
    glm::u16vec4 joints{0};
    glm::vec4 weights{1.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(VertexSkin) == 24, "VertexSkin must match the skinning shader's scalar layout");

// CPU-side mesh, ready to upload with Renderer::createMesh.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;   // counter-clockwise triangles are front faces
    // Simpler levels of detail (optimiseMesh in Assets/MeshOptimise.h), each a range of
    // lodIndices, most detailed first. indices stays the full mesh, which is all that
    // picking, physics and navigation read. Renderer::setMeshLods sends them to the GPU.
    std::vector<uint32_t> lodIndices{};
    std::vector<struct MeshLod> lods{};
    // A second set of texture coordinates (glTF's TEXCOORD_1), one per vertex, or empty.
    // Kept beside the vertices rather than in Vertex, so meshes without it cost nothing;
    // materials pick it with TextureBinding::uvSet. Renderer::setMeshUv1 uploads it.
    std::vector<glm::vec2> uv1{};
    // Where each vertex is in a baked lightmap atlas (G05), one per vertex, or empty. Only
    // meshes made for baked receivers have it; Renderer::setMeshLightmapUv uploads it.
    std::vector<glm::vec2> lightmapUv{};
};

// Built-in shapes, centred on the origin.
MeshData makeCube(float size = 1.0f);
MeshData makePlane(float size = 1.0f, float uvRepeat = 1.0f);   // in XZ, facing +Y
MeshData makeSphere(float radius = 0.5f, uint32_t segments = 48, uint32_t rings = 24);

struct Bounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};
Bounds computeBounds(const MeshData& mesh);

// Nearest hit of a ray (origin, direction, both in the mesh's own space) with the mesh's
// triangles, from either side. t is the distance along direction (in its units).
bool raycast(const MeshData& mesh, const glm::vec3& origin, const glm::vec3& direction, float& t);

// A simpler version of a mesh, drawn instead of it when it's far enough away that nobody
// can tell. Every level uses the mesh's vertices; only the triangles differ.
struct MeshLod {
    uint32_t firstIndex = 0;   // into MeshData::lodIndices
    uint32_t indexCount = 0;
    float error = 0.0f;   // how far this level strays from the full mesh, in the mesh's own units
};
constexpr uint32_t kMaxMeshLods = 7;   // simpler levels a mesh can have, besides itself

// How the renderer picks levels of detail (Renderer::setLodSettings).
struct LodSettings {
    // The largest difference from the full mesh allowed on screen, in pixels at the render
    // resolution. 0 always draws full detail. 1 is invisible in practice.
    float maxErrorPixels = 1.0f;
    bool showLevels = false;   // tint each level its own colour, to see where they switch
};
// What levels of detail saved last frame.
struct LodStats {
    uint64_t triangles = 0;   // triangles drawn
    uint64_t fullTriangles = 0;   // what they would have been with full detail everywhere
    uint32_t reducedDraws = 0;   // draws that used a simpler level
};

}   // namespace shockwave
