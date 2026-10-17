#pragma once

// A sparse voxel octree over a distance field: it keeps only the cells near the surface,
// so rays can jump over empty space in big steps instead of sphere tracing through it.
//
// The root covers the whole field. A node is split into eight children down to leaves of
// kLeafCells^3 cells; children with no surface nearby aren't stored. find() returns the
// deepest node around a point: a surface leaf (march the field there) or an empty box
// (skip to where the ray leaves it).

#include "DistanceField.h"

#include <array>
#include <cstdint>
#include <vector>

namespace shockwave::raymarch {

class SparseVoxelOctree {
public:
    static constexpr uint32_t kLeafCells = 4;
    static constexpr uint32_t kNoChild = UINT32_MAX;

    struct Box {
        glm::vec3 min{0.0f}, max{0.0f};   // mesh space
        bool surface = false;   // a leaf with surface in or near it
    };

    void build(const DistanceField& field);
    Box find(const glm::vec3& p) const;
    size_t nodeCount() const { return m_nodes.size(); }
    bool valid() const { return !m_nodes.empty(); }

private:
    struct Node {
        std::array<uint32_t, 8> children{kNoChild, kNoChild, kNoChild, kNoChild,
                                         kNoChild, kNoChild, kNoChild, kNoChild};
        bool leaf = false;
    };
    // Builds the node covering cells [origin, origin + span) and returns its index, or
    // kNoChild when no surface is near any of them.
    uint32_t buildNode(const DistanceField& field, glm::uvec3 origin, uint32_t span);

    std::vector<Node> m_nodes;   // m_nodes[0] is the root
    glm::vec3 m_min{0.0f};
    float m_cell = 0.0f;
    uint32_t m_size = 0;
};

}   // namespace shockwave::raymarch
