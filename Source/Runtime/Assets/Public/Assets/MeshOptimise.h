#pragma once

// Mesh optimisation with meshoptimizer (docs/Optimisation.md): triangles reordered so the
// GPU shades each vertex once and draws less of what's hidden, and simpler levels of
// detail for drawing far-away copies. Runs at import; CPU only, safe on any thread.

#include <Renderer/Mesh.h>

#include <array>
#include <cstdint>

namespace shockwave {

struct ModelData;

struct MeshOptimiseSettings {
    bool reorder = true;   // vertex cache and overdraw order
    uint32_t lods = 4;   // simpler levels to make, 0..kMaxMeshLods
    // Each level aims for this share of the one before's triangles.
    float reduction = 0.5f;
    // Meshes smaller than this get no levels: drawing them is already cheap.
    uint32_t minTriangles = 256;
};

// What optimisation did, for the editor and `player --optimise`.
struct MeshOptimiseReport {
    uint32_t meshes = 0;
    // Vertices the GPU shades per triangle, before and after reordering (lower is better;
    // 0.5 is ideal, 3 is no reuse at all). Weighted by triangles over every mesh.
    float shadedPerTriangleBefore = 0.0f, shadedPerTriangleAfter = 0.0f;
    // Triangles at full detail, then in each simpler level, summed over every mesh.
    std::array<uint64_t, kMaxMeshLods + 1> levelTriangles{};
    uint32_t levels = 0;   // the most levels any mesh got, full detail included
};

// Reorders mesh.indices (vertices stay where they are, so data kept per vertex stays
// valid) and fills mesh.lods and mesh.lodIndices. Replaces any levels it had.
MeshOptimiseReport optimiseMesh(MeshData& mesh, const MeshOptimiseSettings& settings = {});
// Every mesh of a model.
MeshOptimiseReport optimiseModel(ModelData& model, const MeshOptimiseSettings& settings = {});

}   // namespace shockwave
