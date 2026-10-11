// Baking and querying navigation meshes: scene geometry in, Recast tiles out (one job
// per tile), Detour queries on the result, and the .swnavmesh file format.
#include <Navigation/NavMesh.h>

#include "NavMeshImpl.h"

#include <Assets/Assets.h>
#include <Core/Jobs.h>
#include <Core/Log.h>
#include <Core/Profile.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>

#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>

namespace shockwave {

namespace {

constexpr uint32_t kFileMagic = 0x4D4E5753;   // "SWNM"
constexpr uint32_t kFileVersion = 1;
constexpr unsigned short kWalkFlag = 1;
constexpr int kMaxPathPolys = 512;

// ---- Hashing: FNV-1a over the bytes of what a bake reads -------------------------------

struct Hasher {
    uint64_t value = 1469598103934665603ull;
    void bytes(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) value = (value ^ p[i]) * 1099511628211ull;
    }
    template <class T>
    void add(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&v, sizeof(v));
    }
    void add(const std::string& s) {
        add(s.size());
        bytes(s.data(), s.size());
    }
};

void hashSettings(Hasher& h, const NavMeshSettings& s) {
    h.add(static_cast<int>(s.geometry));
    h.add(s.agentRadius), h.add(s.agentHeight), h.add(s.maxClimb), h.add(s.maxSlope);
    h.add(s.cellSize), h.add(s.cellHeight), h.add(s.tileSize), h.add(s.autoBounds);
    if (!s.autoBounds) h.add(s.boundsMin), h.add(s.boundsMax);
}

// ---- Built-in shapes for colliders, in unit size -----------------------------------------

struct Shape {
    std::vector<glm::vec3> vertices;
    std::vector<int> triangles;
};

const Shape& unitBox() {
    static const Shape box = [] {
        Shape s;
        for (int i = 0; i < 8; ++i) s.vertices.push_back({i & 1 ? 1.f : -1.f, i & 2 ? 1.f : -1.f, i & 4 ? 1.f : -1.f});
        // Counter-clockwise seen from outside, so tops face up the way Recast expects.
        const int faces[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
        for (const auto& f : faces) s.triangles.insert(s.triangles.end(), {f[0], f[1], f[2], f[0], f[2], f[3]});
        return s;
    }();
    return box;
}

// A sphere with an odd number of rings, so no ring sits on the equator and stretching
// the halves apart makes a capsule.
const Shape& unitSphere() {
    static const Shape sphere = [] {
        Shape s;
        constexpr int rings = 9, segments = 12;
        for (int r = 0; r <= rings; ++r) {
            const float phi = glm::pi<float>() * static_cast<float>(r) / rings;
            for (int g = 0; g < segments; ++g) {
                const float theta = glm::two_pi<float>() * static_cast<float>(g) / segments;
                s.vertices.push_back({std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)});
            }
        }
        for (int r = 0; r < rings; ++r)
            for (int g = 0; g < segments; ++g) {
                const int a = r * segments + g, b = r * segments + (g + 1) % segments;
                const int c = a + segments, d = b + segments;
                s.triangles.insert(s.triangles.end(), {a, b, d, a, d, c});
            }
        return s;
    }();
    return sphere;
}

// ---- Which entities a bake reads ---------------------------------------------------------

struct Source {
    enum class Kind { Box, Sphere, Capsule, Mesh, Model };
    Entity entity;
    Kind kind = Kind::Box;
    glm::mat4 matrix{1.0f};
    const Collider* collider = nullptr;
    std::shared_ptr<const MeshData> mesh;
    std::shared_ptr<const ModelAsset> model;
    std::string assetName;
    bool walkable = true;
};

// Moving things aren't baked: anything under a non-static body, a character or an agent.
bool moving(const World& world, Entity e) {
    for (int depth = 0; e.valid() && depth < kMaxHierarchyDepth; ++depth) {
        if (world.has<NavAgent>(e) || world.has<CharacterController>(e)) return true;
        if (const RigidBody* body = world.get<RigidBody>(e);
            body && body->enabled && body->motion != RigidBody::Motion::Static)
            return true;
        e = parentOf(world, e);
    }
    return false;
}

template <class Fn>
void forEachSource(const World& world, const Assets* assets, const NavMeshSettings& settings, Fn&& fn) {
    world.forEachEntity([&](Entity e) {
        if (!world.has<Transform>(e)) return;
        const NavModifier* modifier = world.get<NavModifier>(e);
        if (modifier && modifier->mode == NavModifier::Mode::Ignore) return;
        const Collider* collider = world.get<Collider>(e);
        const MeshRenderer* meshRenderer = world.get<MeshRenderer>(e);
        const ModelRenderer* modelRenderer = world.get<ModelRenderer>(e);
        if (!collider && !meshRenderer && !modelRenderer) return;
        if (moving(world, e)) return;
        const bool force = modifier && modifier->mode == NavModifier::Mode::Walkable;
        const bool useColliders = force || settings.geometry != NavMeshSettings::Geometry::RenderMeshes;
        const bool useMeshes = force || settings.geometry != NavMeshSettings::Geometry::StaticColliders;
        Source source;
        source.entity = e;
        source.walkable = !(modifier && modifier->mode == NavModifier::Mode::NotWalkable);
        source.matrix = worldMatrix(world, e);
        bool found = false;
        if (useColliders && collider && collider->enabled) {
            const RigidBody* body = world.get<RigidBody>(e);
            if (!body || (body->enabled && !body->sensor)) {
                source.collider = collider;
                switch (collider->shape) {
                case Collider::Shape::Box: source.kind = Source::Kind::Box, found = true; break;
                case Collider::Shape::Sphere: source.kind = Source::Kind::Sphere, found = true; break;
                case Collider::Shape::Capsule: source.kind = Source::Kind::Capsule, found = true; break;
                case Collider::Shape::ConvexModel:
                case Collider::Shape::TriangleModel:
                    if (assets && modelRenderer) source.model = assets->model(modelRenderer->model);
                    if (source.model) {
                        source.kind = Source::Kind::Model;
                        source.assetName = assets->modelName(modelRenderer->model);
                        found = true;
                    }
                    break;
                }
            }
        }
        if (!found && useMeshes && assets) {
            if (meshRenderer && meshRenderer->visible) {
                source.mesh = assets->meshData(meshRenderer->mesh);
                if (source.mesh) {
                    source.kind = Source::Kind::Mesh;
                    source.assetName = assets->meshName(meshRenderer->mesh);
                    found = true;
                }
            }
            if (!found && modelRenderer && modelRenderer->visible) {
                source.model = assets->model(modelRenderer->model);
                if (source.model) {
                    source.kind = Source::Kind::Model;
                    source.assetName = assets->modelName(modelRenderer->model);
                    found = true;
                }
            }
        }
        if (found) fn(source);
    });
}

// Appends shape's triangles under matrix, flipping them when the matrix mirrors.
void appendTriangles(
    NavGeometry& out, const glm::mat4& matrix, const glm::vec3* vertices, size_t vertexCount, const uint32_t* indices32,
    const int* indices, size_t indexCount, bool walkable) {
    const int base = static_cast<int>(out.vertices.size() / 3);
    for (size_t i = 0; i < vertexCount; ++i) {
        const glm::vec3 p = glm::vec3(matrix * glm::vec4(vertices[i], 1.0f));
        out.vertices.insert(out.vertices.end(), {p.x, p.y, p.z});
    }
    const bool mirrored = glm::determinant(glm::mat3(matrix)) < 0.0f;
    for (size_t i = 0; i + 2 < indexCount; i += 3) {
        int a = indices ? indices[i] : static_cast<int>(indices32[i]);
        int b = indices ? indices[i + 1] : static_cast<int>(indices32[i + 1]);
        int c = indices ? indices[i + 2] : static_cast<int>(indices32[i + 2]);
        if (mirrored) std::swap(b, c);
        out.triangles.insert(out.triangles.end(), {base + a, base + b, base + c});
        out.walkable.push_back(walkable ? 1 : 0);
    }
}

void appendMesh(NavGeometry& out, const glm::mat4& matrix, const MeshData& mesh, bool walkable) {
    // Positions are the first member of Vertex; copy them out once.
    thread_local std::vector<glm::vec3> positions;
    positions.clear();
    for (const Vertex& v : mesh.vertices) positions.push_back(v.position);
    appendTriangles(
        out, matrix, positions.data(), positions.size(), mesh.indices.data(), nullptr, mesh.indices.size(), walkable);
}

void appendShape(NavGeometry& out, const glm::mat4& matrix, const Shape& shape, bool walkable) {
    appendTriangles(
        out, matrix, shape.vertices.data(), shape.vertices.size(), nullptr, shape.triangles.data(),
        shape.triangles.size(), walkable);
}

// ---- Tiles ---------------------------------------------------------------------------------

struct Layout {
    float origin[3]{};
    float yMax = 0.0f;
    int tilesX = 0, tilesZ = 0;
    float tileWorld = 0.0f;
    int borderCells = 0;
    rcConfig config{};
    uint64_t hash = 0;
};

uint64_t tileKey(int x, int z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(z);
}

struct TileResult {
    unsigned char* data = nullptr;
    int size = 0;
    std::string error;
};

// One tile, the way RecastDemo's Sample_TileMesh builds it. Runs on any thread: every
// Recast object here is its own.
TileResult buildTile(
    const NavGeometry& geometry, const std::vector<int>& tris, const Layout& layout, const NavMeshSettings& s, int tx,
    int tz) {
    TileResult result;
    rcContext ctx(false);
    rcConfig cfg = layout.config;
    cfg.bmin[0] =
        layout.origin[0] + static_cast<float>(tx) * layout.tileWorld - static_cast<float>(cfg.borderSize) * cfg.cs;
    cfg.bmin[1] = layout.origin[1];
    cfg.bmin[2] =
        layout.origin[2] + static_cast<float>(tz) * layout.tileWorld - static_cast<float>(cfg.borderSize) * cfg.cs;
    cfg.bmax[0] =
        layout.origin[0] + static_cast<float>(tx + 1) * layout.tileWorld + static_cast<float>(cfg.borderSize) * cfg.cs;
    cfg.bmax[1] = layout.yMax;
    cfg.bmax[2] =
        layout.origin[2] + static_cast<float>(tz + 1) * layout.tileWorld + static_cast<float>(cfg.borderSize) * cfg.cs;

    struct Scratch {
        rcHeightfield* solid = nullptr;
        rcCompactHeightfield* chf = nullptr;
        rcContourSet* cset = nullptr;
        rcPolyMesh* pmesh = nullptr;
        rcPolyMeshDetail* dmesh = nullptr;
        ~Scratch() {
            rcFreeHeightField(solid);
            rcFreeCompactHeightfield(chf);
            rcFreeContourSet(cset);
            rcFreePolyMesh(pmesh);
            rcFreePolyMeshDetail(dmesh);
        }
    } scratch;

    const float* verts = geometry.vertices.data();
    const int nverts = static_cast<int>(geometry.vertices.size() / 3);
    const int ntris = static_cast<int>(tris.size());
    std::vector<int> indices(static_cast<size_t>(ntris) * 3);
    std::vector<unsigned char> areas(static_cast<size_t>(ntris), RC_NULL_AREA);
    for (int i = 0; i < ntris; ++i) std::memcpy(&indices[i * 3], &geometry.triangles[tris[i] * 3], sizeof(int) * 3);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts, nverts, indices.data(), ntris, areas.data());
    for (int i = 0; i < ntris; ++i)
        if (!geometry.walkable[tris[i]]) areas[i] = RC_NULL_AREA;

    auto fail = [&](const char* what) {
        result.error = std::string(what) + " (tile " + std::to_string(tx) + ", " + std::to_string(tz) + ")";
        return std::move(result);
    };
    scratch.solid = rcAllocHeightfield();
    if (!scratch.solid ||
        !rcCreateHeightfield(&ctx, *scratch.solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
        return fail("out of memory for the heightfield");
    if (!rcRasterizeTriangles(
            &ctx, verts, nverts, indices.data(), areas.data(), ntris, *scratch.solid, cfg.walkableClimb))
        return fail("couldn't rasterize the triangles");
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *scratch.solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *scratch.solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *scratch.solid);
    scratch.chf = rcAllocCompactHeightfield();
    if (!scratch.chf ||
        !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *scratch.solid, *scratch.chf))
        return fail("couldn't build the compact heightfield");
    rcFreeHeightField(scratch.solid);
    scratch.solid = nullptr;
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *scratch.chf)) return fail("couldn't erode by the agent radius");
    // Watershed regions give the cleanest polygons; tiles keep its cost per tile small.
    if (!rcBuildDistanceField(&ctx, *scratch.chf)) return fail("couldn't build the distance field");
    if (!rcBuildRegions(&ctx, *scratch.chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
        return fail("couldn't build regions");
    scratch.cset = rcAllocContourSet();
    if (!scratch.cset ||
        !rcBuildContours(&ctx, *scratch.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *scratch.cset))
        return fail("couldn't build contours");
    if (scratch.cset->nconts == 0) return result;   // nothing walkable in this tile
    scratch.pmesh = rcAllocPolyMesh();
    if (!scratch.pmesh || !rcBuildPolyMesh(&ctx, *scratch.cset, cfg.maxVertsPerPoly, *scratch.pmesh))
        return fail("couldn't triangulate the contours");
    scratch.dmesh = rcAllocPolyMeshDetail();
    if (!scratch.dmesh ||
        !rcBuildPolyMeshDetail(
            &ctx, *scratch.pmesh, *scratch.chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *scratch.dmesh))
        return fail("couldn't build the detail mesh");
    if (scratch.pmesh->npolys == 0) return result;
    if (scratch.pmesh->nverts >= 0xffff) return fail("too many vertices in one tile; lower tile_size");
    for (int i = 0; i < scratch.pmesh->npolys; ++i) {
        if (scratch.pmesh->areas[i] == RC_WALKABLE_AREA) scratch.pmesh->areas[i] = 0;
        scratch.pmesh->flags[i] = kWalkFlag;
    }
    dtNavMeshCreateParams params{};
    params.verts = scratch.pmesh->verts;
    params.vertCount = scratch.pmesh->nverts;
    params.polys = scratch.pmesh->polys;
    params.polyAreas = scratch.pmesh->areas;
    params.polyFlags = scratch.pmesh->flags;
    params.polyCount = scratch.pmesh->npolys;
    params.nvp = scratch.pmesh->nvp;
    params.detailMeshes = scratch.dmesh->meshes;
    params.detailVerts = scratch.dmesh->verts;
    params.detailVertsCount = scratch.dmesh->nverts;
    params.detailTris = scratch.dmesh->tris;
    params.detailTriCount = scratch.dmesh->ntris;
    params.walkableHeight = s.agentHeight;
    params.walkableRadius = s.agentRadius;
    params.walkableClimb = s.maxClimb;
    params.tileX = tx;
    params.tileY = tz;
    params.tileLayer = 0;
    rcVcopy(params.bmin, scratch.pmesh->bmin);
    rcVcopy(params.bmax, scratch.pmesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;
    if (!dtCreateNavMeshData(&params, &result.data, &result.size)) return fail("couldn't create the Detour tile");
    return result;
}

int ceilLog2(uint32_t v) {
    int bits = 0;
    while ((1u << bits) < v) ++bits;
    return bits;
}

}   // namespace

// ---- NavMesh ---------------------------------------------------------------------------------

NavMesh::Impl::~Impl() {
    for (dtNavMeshQuery* query : pool) dtFreeNavMeshQuery(query);
    dtFreeNavMesh(nav);
}

NavQueryLease::NavQueryLease(const NavMesh::Impl& owner) : m_owner(owner) {
    {
        std::lock_guard lock(owner.poolMutex);
        if (!owner.pool.empty()) {
            m_query = owner.pool.back();
            owner.pool.pop_back();
        }
    }
    if (!m_query) {
        m_query = dtAllocNavMeshQuery();
        if (m_query && dtStatusFailed(m_query->init(owner.nav, 2048))) {
            dtFreeNavMeshQuery(m_query);
            m_query = nullptr;
        }
    }
}

NavQueryLease::~NavQueryLease() {
    if (!m_query) return;
    std::lock_guard lock(m_owner.poolMutex);
    m_owner.pool.push_back(m_query);
}

NavMesh::NavMesh(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
NavMesh::~NavMesh() = default;

uint64_t NavMesh::inputHash() const { return m_impl->inputHash; }
const NavMeshSettings& NavMesh::settings() const { return m_impl->settings; }
const NavMeshStats& NavMesh::stats() const { return m_impl->stats; }
bool NavMesh::empty() const { return m_impl->stats.polygons == 0; }
dtNavMesh* NavMesh::detour() const { return m_impl->nav; }

bool NavMesh::findPath(
    const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& points, bool* partial) const {
    points.clear();
    if (partial) *partial = false;
    if (empty()) return false;
    NavQueryLease query(*m_impl);
    if (!query) return false;
    const dtQueryFilter filter;
    const float extents[3] = {2.0f, 4.0f, 2.0f};
    dtPolyRef startRef = 0, endRef = 0;
    float startPos[3], endPos[3];
    query->findNearestPoly(&start.x, extents, &filter, &startRef, startPos);
    query->findNearestPoly(&end.x, extents, &filter, &endRef, endPos);
    if (!startRef || !endRef) return false;
    dtPolyRef polys[kMaxPathPolys];
    int count = 0;
    const dtStatus status = query->findPath(startRef, endRef, startPos, endPos, &filter, polys, &count, kMaxPathPolys);
    if (dtStatusFailed(status) || count == 0) return false;
    const bool incomplete = polys[count - 1] != endRef;
    if (partial) *partial = incomplete;
    float target[3];
    dtVcopy(target, endPos);
    if (incomplete) query->closestPointOnPoly(polys[count - 1], endPos, target, nullptr);
    float straight[kMaxPathPolys * 3];
    int corners = 0;
    query->findStraightPath(startPos, target, polys, count, straight, nullptr, nullptr, &corners, kMaxPathPolys);
    points.reserve(static_cast<size_t>(corners));
    for (int i = 0; i < corners; ++i) points.emplace_back(straight[i * 3], straight[i * 3 + 1], straight[i * 3 + 2]);
    return !points.empty();
}

std::optional<glm::vec3> NavMesh::nearestPoint(const glm::vec3& p, const glm::vec3& extents) const {
    if (empty()) return std::nullopt;
    NavQueryLease query(*m_impl);
    if (!query) return std::nullopt;
    const dtQueryFilter filter;
    dtPolyRef ref = 0;
    float nearest[3];
    query->findNearestPoly(&p.x, &extents.x, &filter, &ref, nearest);
    if (!ref) return std::nullopt;
    return glm::vec3(nearest[0], nearest[1], nearest[2]);
}

std::optional<glm::vec3> NavMesh::raycast(const glm::vec3& start, const glm::vec3& end) const {
    if (empty()) return std::nullopt;
    NavQueryLease query(*m_impl);
    if (!query) return std::nullopt;
    const dtQueryFilter filter;
    const float extents[3] = {2.0f, 4.0f, 2.0f};
    dtPolyRef startRef = 0;
    float startPos[3];
    query->findNearestPoly(&start.x, extents, &filter, &startRef, startPos);
    if (!startRef) return glm::vec3(start);
    float t = 0.0f, normal[3];
    dtPolyRef polys[kMaxPathPolys];
    int count = 0;
    query->raycast(startRef, startPos, &end.x, &filter, &t, normal, polys, &count, kMaxPathPolys);
    if (t == std::numeric_limits<float>::max()) return std::nullopt;
    const glm::vec3 from(startPos[0], startPos[1], startPos[2]);
    return from + (end - from) * t;
}

bool NavMesh::reachable(const glm::vec3& start, const glm::vec3& end) const {
    thread_local std::vector<glm::vec3> points;
    bool partial = false;
    return findPath(start, end, points, &partial) && !partial;
}

const MeshData& NavMesh::debugMesh() const {
    std::call_once(m_impl->debugOnce, [&] {
        MeshData& out = m_impl->debug;
        const dtNavMesh* nav = m_impl->nav;
        if (!nav) return;
        constexpr float kLift = 0.04f;
        auto vertex = [&](const float* p, float shade, float lift) {
            Vertex v;
            v.position = {p[0], p[1] + lift, p[2]};
            v.color = {shade, shade, shade, 1.0f};
            out.vertices.push_back(v);
        };
        for (int t = 0; t < nav->getMaxTiles(); ++t) {
            const dtMeshTile* tile = nav->getTile(t);
            if (!tile || !tile->header) continue;
            for (int i = 0; i < tile->header->polyCount; ++i) {
                const dtPoly& poly = tile->polys[i];
                if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
                // Neighboring polygons get slightly different shades so their shapes show.
                const uint32_t h = (static_cast<uint32_t>(i) * 2654435761u) ^ (static_cast<uint32_t>(t) * 40503u);
                const float shade = 0.55f + 0.45f * static_cast<float>(h % 97u) / 96.0f;
                const dtPolyDetail& detail = tile->detailMeshes[i];
                for (int j = 0; j < detail.triCount; ++j) {
                    const unsigned char* tri = &tile->detailTris[(detail.triBase + j) * 4];
                    const uint32_t base = static_cast<uint32_t>(out.vertices.size());
                    for (int k = 0; k < 3; ++k) {
                        const float* p = tri[k] < poly.vertCount
                                             ? &tile->verts[poly.verts[tri[k]] * 3]
                                             : &tile->detailVerts[(detail.vertBase + tri[k] - poly.vertCount) * 3];
                        vertex(p, shade, kLift);
                    }
                    out.indices.insert(out.indices.end(), {base, base + 2, base + 1});   // Detour winds clockwise
                }
                // Outer edges: a thin dark strip along every edge with nothing beyond it.
                for (int j = 0; j < poly.vertCount; ++j) {
                    bool open = poly.neis[j] == 0;
                    if (poly.neis[j] & DT_EXT_LINK) {
                        open = true;
                        for (unsigned int k = poly.firstLink; k != DT_NULL_LINK; k = tile->links[k].next)
                            if (tile->links[k].edge == j) open = false;
                    }
                    if (!open) continue;
                    const float* a = &tile->verts[poly.verts[j] * 3];
                    const float* b = &tile->verts[poly.verts[(j + 1) % poly.vertCount] * 3];
                    glm::vec3 side = glm::vec3(b[2] - a[2], 0.0f, a[0] - b[0]);
                    const float length = glm::length(side);
                    if (length < 1e-4f) continue;
                    side *= 0.05f / length;
                    const float quad[4][3] = {
                        {a[0] - side.x, a[1], a[2] - side.z},
                        {a[0] + side.x, a[1], a[2] + side.z},
                        {b[0] + side.x, b[1], b[2] + side.z},
                        {b[0] - side.x, b[1], b[2] - side.z}};
                    const uint32_t base = static_cast<uint32_t>(out.vertices.size());
                    for (const auto& q : quad) vertex(q, 0.1f, kLift + 0.01f);
                    out.indices.insert(out.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
                }
            }
        }
    });
    return m_impl->debug;
}

bool NavMesh::save(const std::filesystem::path& file, std::string& error) const {
    const Impl& m = *m_impl;
    const dtNavMesh* nav = m.nav;
    const std::filesystem::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "can't write " + temp.string();
            return false;
        }
        auto put = [&](const auto& v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
        put(kFileMagic), put(kFileVersion), put(m.inputHash), put(m.settings), put(m.layoutHash);
        put(m.stats.inputTriangles);
        put(*nav->getParams());
        uint32_t tiles = 0;
        for (int i = 0; i < nav->getMaxTiles(); ++i)
            if (const dtMeshTile* t = nav->getTile(i); t && t->header && t->dataSize) ++tiles;
        put(tiles);
        for (int i = 0; i < nav->getMaxTiles(); ++i) {
            const dtMeshTile* t = nav->getTile(i);
            if (!t || !t->header || !t->dataSize) continue;
            const auto found = m.tileHashes.find(tileKey(t->header->x, t->header->y));
            put(t->header->x), put(t->header->y), put(found != m.tileHashes.end() ? found->second : uint64_t(0));
            put(t->dataSize);
            out.write(reinterpret_cast<const char*>(t->data), t->dataSize);
        }
        if (!out) {
            error = "couldn't write " + temp.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        error = "couldn't replace " + file.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::shared_ptr<const NavMesh> NavMesh::load(const std::filesystem::path& file, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "can't open " + file.string();
        return nullptr;
    }
    auto get = [&](auto& v) { return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof(v))); };
    uint32_t magic = 0, version = 0;
    if (!get(magic) || !get(version) || magic != kFileMagic || version != kFileVersion) {
        error = file.string() + " isn't a navigation mesh this version of Shockwave can read; bake it again";
        return nullptr;
    }
    auto impl = std::make_unique<NavMesh::Impl>();
    dtNavMeshParams params{};
    uint32_t tiles = 0;
    if (!get(impl->inputHash) || !get(impl->settings) || !get(impl->layoutHash) || !get(impl->stats.inputTriangles) ||
        !get(params) || !get(tiles)) {
        error = file.string() + " is cut short";
        return nullptr;
    }
    impl->nav = dtAllocNavMesh();
    if (!impl->nav || dtStatusFailed(impl->nav->init(&params))) {
        error = file.string() + ": bad navigation mesh parameters";
        return nullptr;
    }
    for (uint32_t i = 0; i < tiles; ++i) {
        int x = 0, y = 0, size = 0;
        uint64_t hash = 0;
        if (!get(x) || !get(y) || !get(hash) || !get(size) || size <= 0 || size > (64 << 20)) {
            error = file.string() + " is cut short";
            return nullptr;
        }
        auto* data = static_cast<unsigned char*>(dtAlloc(static_cast<size_t>(size), DT_ALLOC_PERM));
        if (!data || !in.read(reinterpret_cast<char*>(data), size)) {
            dtFree(data);
            error = file.string() + " is cut short";
            return nullptr;
        }
        if (dtStatusFailed(impl->nav->addTile(data, size, DT_TILE_FREE_DATA, 0, nullptr))) {
            dtFree(data);
            error = file.string() + ": a tile doesn't fit the navigation mesh";
            return nullptr;
        }
        impl->tileHashes[tileKey(x, y)] = hash;
        const auto* header = reinterpret_cast<const dtMeshHeader*>(data);
        impl->stats.tiles++;
        impl->stats.polygons += header->polyCount;
        impl->stats.vertices += header->vertCount;
        impl->stats.bytes += static_cast<size_t>(size);
    }
    return std::make_shared<const NavMesh>(std::move(impl));
}

// ---- Scene inputs -----------------------------------------------------------------------------

NavMeshSettings findNavMeshSettings(const World& world) {
    const auto* pool = world.pool<NavMeshSettings>();
    if (pool)
        for (Entity e : pool->entities())
            if (world.alive(e)) return *pool->find(e.index);
    return {};
}

bool sceneUsesNavigation(const World& world) {
    const auto* settings = world.pool<NavMeshSettings>();
    const auto* agents = world.pool<NavAgent>();
    return (settings && settings->size() > 0) || (agents && agents->size() > 0);
}

uint64_t navInputHash(const World& world, const Assets* assets, const NavMeshSettings& settings) {
    Hasher h;
    h.add(kFileVersion);
    hashSettings(h, settings);
    forEachSource(world, assets, settings, [&](const Source& s) {
        h.add(static_cast<int>(s.kind));
        h.add(s.matrix);
        h.add(s.walkable);
        if (s.collider)
            h.add(s.collider->halfExtents), h.add(s.collider->radius), h.add(s.collider->halfHeight),
                h.add(s.collider->offset);
        h.add(s.assetName);
        if (s.mesh) {
            h.add(s.mesh->vertices.size()), h.add(s.mesh->indices.size());
            const Bounds b = computeBounds(*s.mesh);
            h.add(b.min), h.add(b.max);
        }
        if (s.model) h.add(s.model->data.triangles), h.add(s.model->data.bounds.min), h.add(s.model->data.bounds.max);
    });
    return h.value;
}

NavGeometry collectNavGeometry(const World& world, const Assets* assets, const NavMeshSettings& settings) {
    SW_PROFILE_SCOPE("collectNavGeometry");
    NavGeometry out;
    out.inputHash = navInputHash(world, assets, settings);
    forEachSource(world, assets, settings, [&](const Source& s) {
        ++out.sources;
        switch (s.kind) {
        case Source::Kind::Box:
            appendShape(
                out, glm::scale(glm::translate(s.matrix, s.collider->offset), s.collider->halfExtents), unitBox(),
                s.walkable);
            break;
        case Source::Kind::Sphere:
            appendShape(
                out, glm::scale(glm::translate(s.matrix, s.collider->offset), glm::vec3(s.collider->radius)),
                unitSphere(), s.walkable);
            break;
        case Source::Kind::Capsule: {
            // The unit sphere, scaled to the radius, with its halves pulled apart.
            const Shape& sphere = unitSphere();
            Shape capsule = sphere;
            for (glm::vec3& v : capsule.vertices) {
                v *= s.collider->radius;
                v.y += v.y > 0.0f ? s.collider->halfHeight : -s.collider->halfHeight;
            }
            appendShape(out, glm::translate(s.matrix, s.collider->offset), capsule, s.walkable);
            break;
        }
        case Source::Kind::Mesh: appendMesh(out, s.matrix, *s.mesh, s.walkable); break;
        case Source::Kind::Model:
            for (const ModelNode& node : s.model->data.nodes)
                for (uint32_t index : node.meshes)
                    appendMesh(out, s.matrix * node.world, s.model->data.meshes[index].data, s.walkable);
            break;
        }
    });
    if (!out.vertices.empty()) {
        out.bounds.min = out.bounds.max = {out.vertices[0], out.vertices[1], out.vertices[2]};
        for (size_t i = 0; i < out.vertices.size(); i += 3) {
            const glm::vec3 p(out.vertices[i], out.vertices[i + 1], out.vertices[i + 2]);
            out.bounds.min = glm::min(out.bounds.min, p);
            out.bounds.max = glm::max(out.bounds.max, p);
        }
    }
    return out;
}

std::vector<std::string> validateNavMeshSettings(const NavMeshSettings& s) {
    std::vector<std::string> problems;
    auto positive = [&](float v, const char* name) {
        if (!std::isfinite(v) || v <= 0.0f)
            problems.push_back(std::string("NavMeshSettings ") + name + " must be above 0");
    };
    positive(s.agentRadius, "agent_radius");
    positive(s.agentHeight, "agent_height");
    positive(s.cellSize, "cell_size");
    positive(s.cellHeight, "cell_height");
    if (!std::isfinite(s.maxClimb) || s.maxClimb < 0.0f)
        problems.push_back("NavMeshSettings max_climb can't be negative");
    if (!std::isfinite(s.maxSlope) || s.maxSlope < 0.0f || s.maxSlope >= 90.0f)
        problems.push_back("NavMeshSettings max_slope must be in 0..89 degrees");
    if (s.tileSize < 16 || s.tileSize > 256) problems.push_back("NavMeshSettings tile_size must be 16..256 cells");
    if (s.maxAgents < 1) problems.push_back("NavMeshSettings max_agents must be at least 1");
    if (s.cellSize > 0.0f && s.agentRadius > 0.0f && s.cellSize > s.agentRadius)
        problems.push_back("NavMeshSettings cell_size is larger than agent_radius; narrow gaps will vanish. Try "
                           "agent_radius / 2");
    if (!s.autoBounds &&
        (s.boundsMin.x >= s.boundsMax.x || s.boundsMin.y >= s.boundsMax.y || s.boundsMin.z >= s.boundsMax.z))
        problems.push_back("NavMeshSettings bounds_min must be below bounds_max on every axis");
    return problems;
}

// ---- Baking -----------------------------------------------------------------------------------

std::shared_ptr<const NavMesh> bakeNavMesh(
    const NavGeometry& geometry, const NavMeshSettings& s, JobSystem& jobs, std::string& error,
    const NavMesh* previous) {
    SW_PROFILE_SCOPE("bakeNavMesh");
    const auto started = std::chrono::steady_clock::now();
    if (const auto problems = validateNavMeshSettings(s); !problems.empty()) {
        // A coarse cell size still bakes; the other problems can't.
        if (problems.size() > 1 || problems.front().find("cell_size is larger") == std::string::npos) {
            error = problems.front();
            return nullptr;
        }
    }
    if (geometry.triangles.empty()) {
        error = "there's no static geometry to walk on: add a Collider or MeshRenderer that doesn't move";
        return nullptr;
    }
    Layout layout;
    glm::vec3 lo = s.autoBounds ? geometry.bounds.min : s.boundsMin;
    glm::vec3 hi = s.autoBounds ? geometry.bounds.max : s.boundsMax;
    lo.y -= s.cellHeight;
    hi.y += s.agentHeight;
    // Tiles sit on a grid anchored at multiples of their size, so moving geometry at the
    // edge of the level doesn't shift every tile (and force a full rebake).
    layout.tileWorld = static_cast<float>(s.tileSize) * s.cellSize;
    layout.origin[0] = std::floor(lo.x / layout.tileWorld) * layout.tileWorld;
    layout.origin[2] = std::floor(lo.z / layout.tileWorld) * layout.tileWorld;
    // The height range is snapped too, so voxels line up from one bake to the next.
    const float ySnap = s.cellHeight * 10.0f;
    layout.origin[1] = std::floor(lo.y / ySnap) * ySnap;
    layout.yMax = std::ceil(hi.y / ySnap) * ySnap;
    layout.tilesX = std::max(1, static_cast<int>(std::ceil((hi.x - layout.origin[0]) / layout.tileWorld)));
    layout.tilesZ = std::max(1, static_cast<int>(std::ceil((hi.z - layout.origin[2]) / layout.tileWorld)));
    const int64_t tileCount = static_cast<int64_t>(layout.tilesX) * layout.tilesZ;
    const int tileBits = ceilLog2(static_cast<uint32_t>(std::min<int64_t>(tileCount, 1 << 20)));
    if (tileCount > (1 << 14)) {
        error = "the level needs " + std::to_string(tileCount) +
                " tiles, more than 16384; raise tile_size or cell_size, or set smaller bounds";
        return nullptr;
    }
    rcConfig& cfg = layout.config;
    cfg.cs = s.cellSize;
    cfg.ch = s.cellHeight;
    cfg.walkableSlopeAngle = s.maxSlope;
    cfg.walkableHeight = static_cast<int>(std::ceil(s.agentHeight / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(s.maxClimb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(s.agentRadius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(12.0f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
    cfg.tileSize = s.tileSize;
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    cfg.detailSampleDist = cfg.cs * 6.0f;
    cfg.detailSampleMaxError = cfg.ch * 1.0f;
    {
        Hasher h;
        hashSettings(h, s);
        h.add(layout.origin), h.add(layout.yMax);
        layout.hash = h.value;
    }

    // Which triangles each tile needs: its own square plus the border Recast looks past it.
    const float border = static_cast<float>(cfg.borderSize) * cfg.cs;
    std::vector<std::vector<int>> tileTris(static_cast<size_t>(tileCount));
    const size_t triangleCount = geometry.triangles.size() / 3;
    for (size_t t = 0; t < triangleCount; ++t) {
        glm::vec3 tmin(std::numeric_limits<float>::max()), tmax(-std::numeric_limits<float>::max());
        for (int k = 0; k < 3; ++k) {
            const float* p = &geometry.vertices[static_cast<size_t>(geometry.triangles[t * 3 + k]) * 3];
            tmin = glm::min(tmin, glm::vec3(p[0], p[1], p[2]));
            tmax = glm::max(tmax, glm::vec3(p[0], p[1], p[2]));
        }
        if (tmax.y < layout.origin[1] || tmin.y > layout.yMax) continue;
        if (!s.autoBounds && (tmax.x < lo.x || tmin.x > hi.x || tmax.z < lo.z || tmin.z > hi.z)) continue;
        const int x0 =
            std::max(0, static_cast<int>(std::floor((tmin.x - border - layout.origin[0]) / layout.tileWorld)));
        const int x1 = std::min(
            layout.tilesX - 1, static_cast<int>(std::floor((tmax.x + border - layout.origin[0]) / layout.tileWorld)));
        const int z0 =
            std::max(0, static_cast<int>(std::floor((tmin.z - border - layout.origin[2]) / layout.tileWorld)));
        const int z1 = std::min(
            layout.tilesZ - 1, static_cast<int>(std::floor((tmax.z + border - layout.origin[2]) / layout.tileWorld)));
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
                tileTris[static_cast<size_t>(z) * layout.tilesX + x].push_back(static_cast<int>(t));
    }

    auto impl = std::make_unique<NavMesh::Impl>();
    impl->settings = s;
    impl->inputHash = geometry.inputHash;
    impl->layoutHash = layout.hash;
    impl->stats.inputTriangles = triangleCount;
    const NavMesh::Impl* reuse = previous && previous->impl().layoutHash == layout.hash ? &previous->impl() : nullptr;
    struct Job {
        int x, z;
        uint64_t hash;
        const dtMeshTile* reused = nullptr;
        TileResult result;
    };
    std::vector<Job> work;
    std::vector<size_t> dirty;
    for (int z = 0; z < layout.tilesZ; ++z)
        for (int x = 0; x < layout.tilesX; ++x) {
            const auto& tris = tileTris[static_cast<size_t>(z) * layout.tilesX + x];
            if (tris.empty()) continue;
            Hasher h;
            h.add(layout.hash);
            for (int t : tris) {
                for (int k = 0; k < 3; ++k)
                    h.bytes(
                        &geometry.vertices[static_cast<size_t>(geometry.triangles[t * 3 + k]) * 3], sizeof(float) * 3);
                h.add(geometry.walkable[t]);
            }
            Job job{x, z, h.value, nullptr, {}};
            if (reuse) {
                const auto found = reuse->tileHashes.find(tileKey(x, z));
                if (found != reuse->tileHashes.end() && found->second == job.hash) {
                    job.reused = reuse->nav->getTileAt(x, z, 0);
                    if (!job.reused || !job.reused->header) job.reused = nullptr;
                }
                // A tile that was empty last time and still has the same triangles stays empty.
                if (found != reuse->tileHashes.end() && found->second == job.hash && !job.reused) {
                    impl->tileHashes[tileKey(x, z)] = job.hash;
                    continue;
                }
            }
            if (!job.reused) dirty.push_back(work.size());
            work.push_back(std::move(job));
        }
    jobs.parallelFor(dirty.size(), 1, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            Job& job = work[dirty[i]];
            job.result = buildTile(
                geometry, tileTris[static_cast<size_t>(job.z) * layout.tilesX + job.x], layout, s, job.x, job.z);
        }
    });

    dtNavMeshParams params{};
    rcVcopy(params.orig, layout.origin);
    params.tileWidth = layout.tileWorld;
    params.tileHeight = layout.tileWorld;
    params.maxTiles = 1 << tileBits;
    params.maxPolys = 1 << (22 - tileBits);
    impl->nav = dtAllocNavMesh();
    std::string firstError;
    if (!impl->nav || dtStatusFailed(impl->nav->init(&params)))
        firstError = "couldn't set up the Detour navigation mesh";
    for (Job& job : work) {
        impl->tileHashes[tileKey(job.x, job.z)] = job.hash;
        unsigned char* data = job.result.data;
        int size = job.result.size;
        if (job.reused) {
            size = job.reused->dataSize;
            data = static_cast<unsigned char*>(dtAlloc(static_cast<size_t>(size), DT_ALLOC_PERM));
            if (data) std::memcpy(data, job.reused->data, static_cast<size_t>(size));
        }
        if (!job.result.error.empty() && firstError.empty()) firstError = job.result.error;
        if (!data) continue;
        if (!firstError.empty() || dtStatusFailed(impl->nav->addTile(data, size, DT_TILE_FREE_DATA, 0, nullptr))) {
            dtFree(data);
            continue;
        }
        const auto* header = reinterpret_cast<const dtMeshHeader*>(data);
        impl->stats.tiles++;
        impl->stats.polygons += header->polyCount;
        impl->stats.vertices += header->vertCount;
        impl->stats.bytes += static_cast<size_t>(size);
    }
    if (!firstError.empty()) {
        error = firstError;
        return nullptr;
    }
    impl->stats.tilesRebuilt = static_cast<int>(dirty.size());
    impl->stats.bakeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    auto mesh = std::make_shared<const NavMesh>(std::move(impl));
    navcache::put(mesh);
    return mesh;
}

// ---- Cache and files --------------------------------------------------------------------------

namespace navcache {
namespace {
std::mutex& mutex() {
    static std::mutex m;
    return m;
}
std::vector<std::shared_ptr<const NavMesh>>& entries() {
    static std::vector<std::shared_ptr<const NavMesh>> list;
    return list;
}
}   // namespace

void put(std::shared_ptr<const NavMesh> mesh) {
    if (!mesh) return;
    std::lock_guard lock(mutex());
    auto& list = entries();
    std::erase_if(list, [&](const auto& m) { return m->inputHash() == mesh->inputHash(); });
    list.push_back(std::move(mesh));
    constexpr size_t kKeep = 8;
    if (list.size() > kKeep) list.erase(list.begin());
}

std::shared_ptr<const NavMesh> find(uint64_t inputHash) {
    std::lock_guard lock(mutex());
    for (const auto& m : entries())
        if (m->inputHash() == inputHash) return m;
    return nullptr;
}

void clear() {
    std::lock_guard lock(mutex());
    entries().clear();
}
}   // namespace navcache

std::filesystem::path navMeshPathFor(const std::filesystem::path& scene) {
    std::filesystem::path result = scene;
    result.replace_extension(".swnavmesh");
    return result;
}

std::shared_ptr<const NavMesh> loadSceneNavMesh(const std::filesystem::path& scene, std::string& error) {
    error.clear();
    const std::filesystem::path file = navMeshPathFor(scene);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return nullptr;
    auto mesh = NavMesh::load(file, error);
    if (mesh) navcache::put(mesh);
    return mesh;
}

}   // namespace shockwave
