#pragma once

// A baked navigation mesh: the walkable surface of a scene as polygons, split into tiles,
// plus the queries that find paths across it. Baking turns static geometry into it with
// Recast, on the job system; Detour answers the queries. Game code normally uses it
// through NavigationWorld (game.navigation()), which owns the session's mesh.
//
// Threading: a NavMesh never changes once baked. findPath, nearestPoint, raycast and
// the other queries are safe from any thread at once; each borrows a query object from a
// small pool, so after warm-up they don't allocate. collectNavGeometry and
// navInputHash read the World, so they follow its rules (main thread, or a system that
// declares what it reads). bakeNavMesh is safe from any thread, and spreads the tiles
// over the job system.

#include <Core/Log.h>
#include <Navigation/NavComponents.h>
#include <Renderer/Mesh.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class dtNavMesh;

namespace shockwave {

SW_LOG_CATEGORY(LogNavigation);

class Assets;
class JobSystem;
class World;

// The triangles a bake starts from, in world space, gathered from the scene.
struct NavGeometry {
    std::vector<float> vertices;   // x, y, z per vertex
    std::vector<int> triangles;   // three vertex indices per triangle
    std::vector<uint8_t> walkable;   // per triangle: 1 may be walked on, 0 only blocks
    Bounds bounds;   // of every vertex
    uint64_t inputHash = 0;   // navInputHash of the scene it came from
    size_t sources = 0;   // entities that contributed
};

struct NavMeshStats {
    int tiles = 0;   // tiles that hold polygons
    int tilesRebuilt = 0;   // tiles baked this time; the rest were reused from the last bake
    int polygons = 0;
    int vertices = 0;
    size_t inputTriangles = 0;
    size_t bytes = 0;   // Detour tile data
    double bakeMs = 0.0;   // wall time of the whole bake
};

class NavMesh {
public:
    struct Impl;
    explicit NavMesh(std::unique_ptr<Impl> impl);
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // The scene inputs it was baked from (navInputHash) and with which settings.
    uint64_t inputHash() const;
    const NavMeshSettings& settings() const;
    const NavMeshStats& stats() const;
    bool empty() const;   // no walkable polygons at all

    // A walking path from start to end along the mesh, as corner points (start and end
    // included), into points (cleared first; reuse it to avoid allocating). Returns false
    // when either point is off the mesh. When end can't be reached, the path goes as near
    // as it can and partial is set.
    bool findPath(
        const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& points, bool* partial = nullptr) const;
    // The nearest point on the mesh within extents (half sizes of a search box) of p.
    std::optional<glm::vec3> nearestPoint(const glm::vec3& p, const glm::vec3& extents = {2.0f, 4.0f, 2.0f}) const;
    // Walks a straight line along the mesh from start toward end; returns where it hits a
    // wall, or nothing when the whole line is walkable.
    std::optional<glm::vec3> raycast(const glm::vec3& start, const glm::vec3& end) const;
    // True when end can be walked to from start (both snapped to the mesh first).
    bool reachable(const glm::vec3& start, const glm::vec3& end) const;

    // The mesh as triangles for drawing: each polygon a slightly varied shade of color,
    // with darker outer edges, lifted a little off the ground. Built on first use.
    const MeshData& debugMesh() const;

    // Saves the mesh next to its scene (.swnavmesh) so games don't bake it again. Detour
    // tile data is stored as is, so files are tied to the CPU's byte order.
    bool save(const std::filesystem::path& file, std::string& error) const;
    static std::shared_ptr<const NavMesh> load(const std::filesystem::path& file, std::string& error);

    // For the Navigation module itself.
    dtNavMesh* detour() const;
    Impl& impl() const { return *m_impl; }

private:
    std::unique_ptr<Impl> m_impl;
};

// The scene's NavMeshSettings, or the defaults when it has none.
NavMeshSettings findNavMeshSettings(const World& world);
// True when the scene asks for navigation: it has NavMeshSettings or a NavAgent.
bool sceneUsesNavigation(const World& world);
// A cheap fingerprint of everything a bake would read: the settings and every source
// entity's placement, shape, mesh or model. Equal hashes bake equal meshes. It hashes
// mesh names, sizes and bounds rather than every vertex.
uint64_t navInputHash(const World& world, const Assets* assets, const NavMeshSettings& settings);
// Gathers the triangles to bake, in world space.
NavGeometry collectNavGeometry(const World& world, const Assets* assets, const NavMeshSettings& settings);
// Bakes geometry into a navigation mesh, one job per tile. Tiles whose triangles match
// previous are copied from it rather than baked again. Returns null and sets error when
// the settings are invalid or nothing could be built. The result is also put in the
// cache below.
std::shared_ptr<const NavMesh> bakeNavMesh(
    const NavGeometry& geometry, const NavMeshSettings& settings, JobSystem& jobs, std::string& error,
    const NavMesh* previous = nullptr);

// Meshes baked or loaded recently, by navInputHash, so pressing Play after a bake (or
// loading a saved one) doesn't bake again. Thread-safe. Keeps the last few.
namespace navcache {
void put(std::shared_ptr<const NavMesh> mesh);
std::shared_ptr<const NavMesh> find(uint64_t inputHash);
void clear();
}   // namespace navcache

// Where a scene's saved navigation mesh lives: Scenes/level.swscene -> Scenes/level.swnavmesh.
std::filesystem::path navMeshPathFor(const std::filesystem::path& scene);
// Loads the scene's saved mesh, if there is one, into the cache. Returns it, or null
// when there's no file or it can't be read (error says why; empty when there's no file).
std::shared_ptr<const NavMesh> loadSceneNavMesh(const std::filesystem::path& scene, std::string& error);

// Problems in NavMeshSettings values, as sentences; empty when they're fine.
std::vector<std::string> validateNavMeshSettings(const NavMeshSettings& settings);

}   // namespace shockwave
