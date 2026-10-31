#include <Renderer/Particles.h>

#include <algorithm>
#include <cmath>

namespace shockwave {

const char* particleLightingName(ParticleLighting lighting) {
    return lighting == ParticleLighting::Simple ? "simple" : "scene";
}

float softParticleFade(float gap, float z, float softDistance, float softNear, float softNearOffset) {
    float fade = 1.0f;
    if (softDistance > 0.0f) fade = std::min(fade, gap / softDistance);   // gap = inf: 1
    if (softNear > 0.0f) fade = std::min(fade, (z - softNearOffset) / softNear);
    return std::isnan(fade) ? 1.0f : std::clamp(fade, 0.0f, 1.0f);
}

}   // namespace shockwave
