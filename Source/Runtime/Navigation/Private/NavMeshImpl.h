#pragma once

// NavMesh's insides, shared by the files of the Navigation module.

#include <Navigation/NavMesh.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;

namespace shockwave {

struct NavMesh::Impl {
    dtNavMesh* nav = nullptr;
    NavMeshSettings settings;
    NavMeshStats stats;
    uint64_t inputHash = 0;
    // Tiles can only be reused by a bake with the same grid and settings.
    uint64_t layoutHash = 0;
    // The hash of the triangles each tile was baked from, by tile (x, z), so the next
    // bake can tell which tiles changed.
    std::unordered_map<uint64_t, uint64_t> tileHashes;
    // Detour queries aren't thread-safe, so each caller borrows one.
    mutable std::mutex poolMutex;
    mutable std::vector<dtNavMeshQuery*> pool;
    mutable std::once_flag debugOnce;
    mutable MeshData debug;

    Impl() = default;
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

// A query object borrowed from a mesh's pool for the length of one call.
class NavQueryLease {
public:
    explicit NavQueryLease(const NavMesh::Impl& owner);
    ~NavQueryLease();
    NavQueryLease(const NavQueryLease&) = delete;
    NavQueryLease& operator=(const NavQueryLease&) = delete;
    explicit operator bool() const { return m_query != nullptr; }
    dtNavMeshQuery* operator->() const { return m_query; }
    dtNavMeshQuery* get() const { return m_query; }

private:
    const NavMesh::Impl& m_owner;
    dtNavMeshQuery* m_query = nullptr;
};

}   // namespace shockwave
