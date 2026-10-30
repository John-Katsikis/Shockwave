#include <Renderer/Temporal.h>

#include <algorithm>
#include <cmath>

namespace shockwave {

float halton(uint32_t index, uint32_t base) {
    float result = 0.0f, fraction = 1.0f;
    while (index > 0) {
        fraction /= float(base);
        result += fraction * float(index % base);
        index /= base;
    }
    return result;
}

glm::vec2 taaJitter(uint32_t frame) {
    const uint32_t i = frame % kTaaJitterCycle + 1;
    return {halton(i, 2) - 0.5f, halton(i, 3) - 0.5f};
}

glm::mat4 jitterProjection(const glm::mat4& projection, const glm::vec2& jitter, const glm::vec2& extent) {
    // clip' = M clip, with M adding a w and b w to x and y: column 3, rows 0 and 1.
    glm::mat4 m(1.0f);
    m[3][0] = 2.0f * jitter.x / extent.x;
    m[3][1] = -2.0f * jitter.y / extent.y;
    return m * projection;
}

const char* historyResetName(HistoryReset reset) {
    constexpr const char* names[] = {"none",          "first frame", "view changed", "resized",
                                     "projection changed", "camera cut", "mode changed"};
    return names[std::min<uint32_t>(static_cast<uint32_t>(reset), 6)];
}

HistoryReset historyReset(const TemporalViewState& previous, const TemporalViewState& current, bool cut) {
    if (!previous.valid) return HistoryReset::FirstFrame;
    if (!(previous.view == current.view)) return HistoryReset::ViewChanged;
    if (previous.extent != current.extent) return HistoryReset::Resized;
    if (previous.orthographic != current.orthographic) return HistoryReset::ProjectionChanged;
    if (cut) return HistoryReset::CameraCut;
    return HistoryReset::None;
}

float taaHistoryWeight(float motionPixels, float reactive) {
    const float moving = std::clamp(motionPixels / kTaaMotionPixels, 0.0f, 1.0f);
    const float weight = kTaaRestWeight + (kTaaMotionWeight - kTaaRestWeight) * moving;
    return weight * (1.0f - std::clamp(reactive, 0.0f, 1.0f));
}

glm::vec3 rgbToYCoCg(const glm::vec3& c) {
    return {0.25f * c.r + 0.5f * c.g + 0.25f * c.b, 0.5f * c.r - 0.5f * c.b, -0.25f * c.r + 0.5f * c.g - 0.25f * c.b};
}
glm::vec3 yCoCgToRgb(const glm::vec3& c) { return {c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z}; }

glm::vec3 clipToBox(const glm::vec3& history, const glm::vec3& lo, const glm::vec3& hi) {
    const glm::vec3 centre = 0.5f * (lo + hi), extents = glm::max(0.5f * (hi - lo), glm::vec3(1e-6f));
    const glm::vec3 offset = history - centre;
    const glm::vec3 units = glm::abs(offset / extents);
    const float outside = std::max({units.x, units.y, units.z});
    return outside > 1.0f ? centre + offset / outside : history;
}

}   // namespace shockwave
