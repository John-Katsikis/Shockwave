#include <Renderer/Decals.h>

#include <numeric>

namespace shockwave {

const char* decalQualityName(DecalQuality quality) {
    constexpr const char* names[] = {"off", "low", "medium", "high"};
    return names[std::min<uint32_t>(static_cast<uint32_t>(quality), 3)];
}

uint32_t decalBudget(DecalQuality quality) {
    constexpr uint32_t budgets[] = {0, 64, 256, 1024};
    return budgets[std::min<uint32_t>(static_cast<uint32_t>(quality), 3)];
}

bool validDecal(const DecalDesc& decal) {
    for (int c = 0; c < 3; ++c) {
        const float size = glm::length(glm::vec3(decal.boxToWorld[c]));
        if (!std::isfinite(size) || !(size > 1e-6f)) return false;
    }
    const glm::vec3 position(decal.boxToWorld[3]);
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return false;
    const float det = glm::determinant(glm::mat3(decal.boxToWorld));
    return std::isfinite(det) && std::abs(det) > 1e-12f && std::isfinite(decal.color.a) && decal.color.a >= 0.0f &&
           decal.color.a <= 1.0f;
}

bool decalMayBeVisible(const DecalDesc& decal, const glm::mat4& viewProjection) {
    glm::vec4 clip[8];
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f);
        clip[i] = viewProjection * decal.boxToWorld * glm::vec4(corner, 1.0f);
    }
    // -w <= x, y <= w and, reverse-Z, 0 <= z (in front); the far side is left open.
    auto outside = [&](auto test) {
        for (const glm::vec4& c : clip)
            if (!test(c)) return false;
        return true;
    };
    return !(
        outside([](const glm::vec4& c) { return c.x < -c.w; }) ||
        outside([](const glm::vec4& c) { return c.x > c.w; }) ||
        outside([](const glm::vec4& c) { return c.y < -c.w; }) ||
        outside([](const glm::vec4& c) { return c.y > c.w; }) ||
        outside([](const glm::vec4& c) { return c.z > c.w; }) ||   // nearer than the near plane
        outside([](const glm::vec4& c) { return c.w <= 0.0f; }));   // behind the camera
}

std::vector<uint32_t> selectDecals(std::span<const DecalDesc> decals, const glm::vec3& camera, uint32_t budget) {
    std::vector<uint32_t> kept(decals.size());
    std::iota(kept.begin(), kept.end(), 0u);
    if (kept.size() > budget) {
        std::vector<float> distance(decals.size());
        for (size_t i = 0; i < decals.size(); ++i) {
            const glm::vec3 d = glm::vec3(decals[i].boxToWorld[3]) - camera;
            distance[i] = glm::dot(d, d);
        }
        std::sort(kept.begin(), kept.end(), [&](uint32_t a, uint32_t b) {
            if (decals[a].essential != decals[b].essential) return decals[a].essential;
            if (distance[a] != distance[b]) return distance[a] < distance[b];
            if (decals[a].order != decals[b].order) return decals[a].order < decals[b].order;
            return a < b;
        });
        kept.resize(budget);
    }
    std::sort(kept.begin(), kept.end(), [&](uint32_t a, uint32_t b) {
        if (decals[a].sortOrder != decals[b].sortOrder) return decals[a].sortOrder < decals[b].sortOrder;
        if (decals[a].order != decals[b].order) return decals[a].order < decals[b].order;
        return a < b;
    });
    return kept;
}

}   // namespace shockwave
