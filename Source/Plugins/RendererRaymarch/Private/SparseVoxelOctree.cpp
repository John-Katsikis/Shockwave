#include "SparseVoxelOctree.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace shockwave::raymarch {

void SparseVoxelOctree::build(const DistanceField& field) {
    m_nodes.clear();
    m_min = field.min;
    m_cell = field.cell;
    m_size = field.size;
    if (!field.valid()) return;
    m_nodes.emplace_back();   // the root stays even when everything is empty
    const uint32_t root = buildNode(field, glm::uvec3(0), field.size);
    if (root != kNoChild && root != 0) m_nodes[0] = m_nodes[root];
}

uint32_t SparseVoxelOctree::buildNode(const DistanceField& field, glm::uvec3 origin, uint32_t span) {
    // Surface is near when some sample is closer to it than the node is big: then the
    // surface can pass through this node.
    const float reach = float(span) * m_cell * 0.87f + m_cell;   // half the node's diagonal, plus a cell
    float nearest = std::numeric_limits<float>::max();
    for (uint32_t z = origin.z; z < origin.z + span; ++z)
        for (uint32_t y = origin.y; y < origin.y + span; ++y)
            for (uint32_t x = origin.x; x < origin.x + span; ++x)
                nearest = std::min(nearest, std::abs(field.at(int(x), int(y), int(z))));
    if (nearest > reach) return kNoChild;
    const uint32_t index = static_cast<uint32_t>(m_nodes.size());
    m_nodes.emplace_back();
    if (span <= kLeafCells) {
        m_nodes[index].leaf = true;
        return index;
    }
    const uint32_t half = span / 2;
    for (uint32_t c = 0; c < 8; ++c) {
        const glm::uvec3 offset((c & 1) ? half : 0, (c & 2) ? half : 0, (c & 4) ? half : 0);
        const uint32_t child = buildNode(field, origin + offset, half);
        m_nodes[index].children[c] = child;   // m_nodes may have grown: index, not a reference
    }
    return index;
}

SparseVoxelOctree::Box SparseVoxelOctree::find(const glm::vec3& p) const {
    Box box;
    box.min = m_min;
    box.max = m_min + glm::vec3(float(m_size) * m_cell);
    if (m_nodes.empty()) return box;
    uint32_t node = 0;
    while (true) {
        if (m_nodes[node].leaf) {
            box.surface = true;
            return box;
        }
        const glm::vec3 mid = (box.min + box.max) * 0.5f;
        const uint32_t c = (p.x >= mid.x ? 1u : 0u) | (p.y >= mid.y ? 2u : 0u) | (p.z >= mid.z ? 4u : 0u);
        for (int axis = 0; axis < 3; ++axis) {
            if (c & (1u << axis))
                box.min[axis] = mid[axis];
            else
                box.max[axis] = mid[axis];
        }
        const uint32_t child = m_nodes[node].children[c];
        if (child == kNoChild) return box;   // empty: nothing to hit in here
        node = child;
    }
}

}   // namespace shockwave::raymarch
