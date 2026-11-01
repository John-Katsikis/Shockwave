#include <Renderer/Culling.h>

#include <algorithm>
#include <cmath>

namespace shockwave {

const char* occlusionCullingName(OcclusionCulling culling) { return culling == OcclusionCulling::On ? "on" : "off"; }

std::vector<HzbLevel> buildHzb(const std::vector<float>& depth, uint32_t width, uint32_t height) {
    std::vector<HzbLevel> levels;
    if (width == 0 || height == 0 || depth.size() < size_t(width) * height) return levels;
    const std::vector<float>* source = &depth;
    uint32_t w = width, h = height;
    do {
        HzbLevel level;
        level.width = (w + 1) / 2;
        level.height = (h + 1) / 2;
        level.depth.resize(size_t(level.width) * level.height);
        for (uint32_t y = 0; y < level.height; ++y)
            for (uint32_t x = 0; x < level.width; ++x) {
                // Every child: two of a side, one at an odd edge.
                float least = 1.0f;
                for (uint32_t dy = 0; dy < 2; ++dy)
                    for (uint32_t dx = 0; dx < 2; ++dx) {
                        const uint32_t cx = 2 * x + dx, cy = 2 * y + dy;
                        if (cx < w && cy < h) least = std::min(least, (*source)[size_t(cy) * w + cx]);
                    }
                level.depth[size_t(y) * level.width + x] = least;
            }
        levels.push_back(std::move(level));
        source = &levels.back().depth;
        w = levels.back().width;
        h = levels.back().height;
    } while (w > 1 || h > 1);
    return levels;
}

ScreenRect projectBounds(glm::vec4 sphere, const glm::mat4& viewProjection, uint32_t width, uint32_t height) {
    ScreenRect rect;
    const glm::vec3 center(sphere);
    const float r = sphere.w;
    glm::vec2 lo(1e30f), hi(-1e30f);
    float nearest = 0.0f;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner = center + glm::vec3((i & 1) ? r : -r, (i & 2) ? r : -r, (i & 4) ? r : -r);
        const glm::vec4 clip = viewProjection * glm::vec4(corner, 1.0f);
        if (!(clip.w > 1e-6f)) return rect;   // behind the camera or on its plane
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (!(ndc.z <= 1.0f)) return rect;   // nearer than the near plane
        const glm::vec2 pixel((ndc.x * 0.5f + 0.5f) * float(width), (0.5f - ndc.y * 0.5f) * float(height));
        lo = glm::min(lo, pixel);
        hi = glm::max(hi, pixel);
        nearest = std::max(nearest, ndc.z);   // reverse-Z: larger is nearer
    }
    if (!std::isfinite(lo.x) || !std::isfinite(lo.y) || !std::isfinite(hi.x) || !std::isfinite(hi.y)) return rect;
    rect.valid = true;
    rect.min = lo - glm::vec2(1.0f);   // a pixel's margin for rasterization rounding
    rect.max = hi + glm::vec2(1.0f);
    rect.nearestDepth = nearest;
    return rect;
}

uint32_t hzbLevelFor(const ScreenRect& rect, uint32_t levels) {
    if (levels == 0) return 0;
    const float span = std::max(std::max(rect.max.x - rect.min.x, rect.max.y - rect.min.y), 1.0f);
    // A level-L texel covers 2^(L+1) pixels: span / 2^(L+1) <= 4.
    const int level = std::max(int(std::ceil(std::log2(span / 4.0f))) - 1, 0);
    return std::min(uint32_t(level), levels - 1);
}

bool hzbOccluded(const std::vector<HzbLevel>& hzb, const ScreenRect& rect, float bias) {
    if (!rect.valid || hzb.empty()) return false;
    const float width = float(hzb[0].width * 2), height = float(hzb[0].height * 2);
    const glm::vec2 lo = glm::max(rect.min, glm::vec2(0.0f));
    const glm::vec2 hi = glm::min(rect.max, glm::vec2(width - 1.0f, height - 1.0f));
    if (lo.x > hi.x || lo.y > hi.y) return false;   // off screen: not this test's call
    const uint32_t level = hzbLevelFor(rect, uint32_t(hzb.size()));
    const HzbLevel& l = hzb[level];
    const float texel = float(2u << level);
    const uint32_t x0 = std::min(uint32_t(lo.x / texel), l.width - 1),
                   x1 = std::min(uint32_t(hi.x / texel), l.width - 1);
    const uint32_t y0 = std::min(uint32_t(lo.y / texel), l.height - 1),
                   y1 = std::min(uint32_t(hi.y / texel), l.height - 1);
    float least = 1.0f;
    for (uint32_t y = y0; y <= y1; ++y)
        for (uint32_t x = x0; x <= x1; ++x) least = std::min(least, l.depth[size_t(y) * l.width + x]);
    return rect.nearestDepth < least - bias;
}

}   // namespace shockwave
