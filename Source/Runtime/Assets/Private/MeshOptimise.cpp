#include <Assets/MeshOptimise.h>

#include <Assets/Model.h>

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace shockwave {
namespace {
// A typical post-transform cache; the numbers are only used to report the improvement.
constexpr unsigned kCacheSize = 16;

float shadedPerTriangle(const std::vector<uint32_t>& indices, size_t vertexCount) {
    if (indices.empty()) return 0.0f;
    return meshopt_analyzeVertexCache(indices.data(), indices.size(), vertexCount, kCacheSize, 0, 0).acmr;
}

// Positions for the simplifier, with near-identical ones made identical. A seam (where UVs
// wrap, or normals split) is two vertices in one place; the simplifier keeps it closed only
// when their positions match bit for bit, and rounding often makes them differ slightly,
// which would open cracks in the simpler levels.
std::vector<glm::vec3> weldedPositions(const std::vector<Vertex>& vertices) {
    glm::vec3 lo = vertices[0].position, hi = lo;
    for (const Vertex& v : vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    const glm::vec3 extent = hi - lo;
    const float tolerance = std::max(std::max({extent.x, extent.y, extent.z}) * 1e-5f, 1e-12f);
    const auto cellOf = [&](const glm::vec3& p) { return glm::floor((p - lo) / tolerance); };
    const auto key = [](const glm::vec3& cell) {
        const auto part = [](float c) { return uint64_t(int64_t(c)) * 0x9e3779b97f4a7c15ull; };
        return part(cell.x) ^ (part(cell.y) >> 21 | part(cell.y) << 43) ^ (part(cell.z) >> 42 | part(cell.z) << 22);
    };
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
    std::vector<glm::vec3> welded(vertices.size());
    for (uint32_t i = 0; i < vertices.size(); ++i) {
        const glm::vec3 p = vertices[i].position, cell = cellOf(p);
        welded[i] = p;
        bool found = false;
        for (int dz = -1; dz <= 1 && !found; ++dz)
            for (int dy = -1; dy <= 1 && !found; ++dy)
                for (int dx = -1; dx <= 1 && !found; ++dx) {
                    const auto it = cells.find(key(cell + glm::vec3(dx, dy, dz)));
                    if (it == cells.end()) continue;
                    for (uint32_t other : it->second) {
                        const glm::vec3 d = welded[other] - p;
                        if (glm::dot(d, d) <= tolerance * tolerance) {
                            welded[i] = welded[other];
                            found = true;
                            break;
                        }
                    }
                }
        if (!found) cells[key(cell)].push_back(i);
    }
    return welded;
}

void add(MeshOptimiseReport& total, const MeshOptimiseReport& mesh) {
    const uint64_t before = total.levelTriangles[0], added = mesh.levelTriangles[0];
    if (before + added > 0) {
        const auto weight = [&](float a, float b) {
            return static_cast<float>((a * double(before) + b * double(added)) / double(before + added));
        };
        total.shadedPerTriangleBefore = weight(total.shadedPerTriangleBefore, mesh.shadedPerTriangleBefore);
        total.shadedPerTriangleAfter = weight(total.shadedPerTriangleAfter, mesh.shadedPerTriangleAfter);
    }
    for (size_t i = 0; i < total.levelTriangles.size(); ++i) total.levelTriangles[i] += mesh.levelTriangles[i];
    total.levels = std::max(total.levels, mesh.levels);
    total.meshes += mesh.meshes;
}
}   // namespace

MeshOptimiseReport optimiseMesh(MeshData& mesh, const MeshOptimiseSettings& settings) {
    MeshOptimiseReport report;
    mesh.lods.clear();
    mesh.lodIndices.clear();
    auto& indices = mesh.indices;
    const size_t vertexCount = mesh.vertices.size();
    if (indices.size() < 3 || vertexCount == 0) return report;
    report.meshes = 1;
    report.levels = 1;
    report.levelTriangles[0] = indices.size() / 3;
    report.shadedPerTriangleBefore = shadedPerTriangle(indices, vertexCount);

    const float* positions = &mesh.vertices[0].position.x;
    constexpr size_t stride = sizeof(Vertex);
    if (settings.reorder) {
        meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), vertexCount);
        // Sorts clusters of triangles front to back where the vertex cache allows (5% worse).
        meshopt_optimizeOverdraw(
            indices.data(), indices.data(), indices.size(), positions, vertexCount, stride, 1.05f);
    }
    report.shadedPerTriangleAfter = shadedPerTriangle(indices, vertexCount);

    const uint32_t wanted = std::min(settings.lods, kMaxMeshLods);
    if (wanted == 0 || indices.size() / 3 < settings.minTriangles) return report;
    // Errors come back relative to the mesh's size; the renderer wants its own units.
    const std::vector<glm::vec3> welded = weldedPositions(mesh.vertices);
    const float scale = meshopt_simplifyScale(&welded[0].x, vertexCount, sizeof(glm::vec3));
    const float reduction = std::clamp(settings.reduction, 0.05f, 0.95f);
    std::vector<uint32_t> level(indices.size());
    size_t previous = indices.size();
    float previousError = 0.0f;
    for (uint32_t i = 0; i < wanted; ++i) {
        // Every level is simplified from the full mesh, so its error is measured against it.
        const size_t target = std::max<size_t>(size_t(double(previous) * reduction) / 3 * 3, 3);
        float error = 0.0f;
        const size_t count = meshopt_simplify(
            level.data(), indices.data(), indices.size(), &welded[0].x, vertexCount, sizeof(glm::vec3), target, 1.0f, 0,
            &error);
        // A level must save something worth a draw switch, or there's no point in it.
        if (count == 0 || count > previous * 9 / 10) break;
        meshopt_optimizeVertexCache(level.data(), level.data(), count, vertexCount);
        MeshLod lod;
        lod.firstIndex = static_cast<uint32_t>(mesh.lodIndices.size());
        lod.indexCount = static_cast<uint32_t>(count);
        // Never below the level before, so coarser levels always need more distance.
        lod.error = std::max(error * scale, previousError);
        mesh.lodIndices.insert(mesh.lodIndices.end(), level.begin(), level.begin() + count);
        mesh.lods.push_back(lod);
        report.levelTriangles[mesh.lods.size()] = count / 3;
        previous = count;
        previousError = lod.error;
    }
    report.levels = static_cast<uint32_t>(mesh.lods.size()) + 1;
    return report;
}

MeshOptimiseReport optimiseModel(ModelData& model, const MeshOptimiseSettings& settings) {
    MeshOptimiseReport total;
    for (ModelMesh& mesh : model.meshes) add(total, optimiseMesh(mesh.data, settings));
    return total;
}

}   // namespace shockwave
