// Particles (A04; docs/Effects.md): the soft particle fade, worked out by hand against
// Effekseer's formula, and the three particle settings with their presets. No GPU.
#include <Engine/GraphicsSettings.h>
#include <Renderer/Particles.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace shockwave;

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}
bool near(float a, float b) { return std::abs(a - b) <= 1e-5f; }

void testFade() {
    const float inf = std::numeric_limits<float>::infinity();
    // Effekseer: min((zs - zp) / far, (zp - nearOffset) / near), clamped to 0..1.
    check(near(softParticleFade(0.0f, 5.0f, 1.0f, 0.0f, 0.0f), 0.0f), "touching the scene: gone");
    check(near(softParticleFade(0.25f, 5.0f, 1.0f, 0.0f, 0.0f), 0.25f), "a quarter of the distance: a quarter");
    check(near(softParticleFade(2.0f, 5.0f, 1.0f, 0.0f, 0.0f), 1.0f), "past the distance: whole");
    check(near(softParticleFade(inf, 5.0f, 1.0f, 0.0f, 0.0f), 1.0f), "over the sky: whole");
    check(near(softParticleFade(-0.5f, 5.0f, 1.0f, 0.0f, 0.0f), 0.0f), "behind the scene: gone");
    // The near fade: over 2 m from 0.5 m.
    check(near(softParticleFade(inf, 0.5f, 0.0f, 2.0f, 0.5f), 0.0f), "at the near offset: gone");
    check(near(softParticleFade(inf, 1.5f, 0.0f, 2.0f, 0.5f), 0.5f), "halfway in: half");
    check(near(softParticleFade(inf, 0.2f, 0.0f, 2.0f, 0.5f), 0.0f), "nearer than the offset: gone");
    // Both: the smaller wins.
    check(near(softParticleFade(0.3f, 1.5f, 1.0f, 2.0f, 0.5f), 0.3f), "both fades: the smaller");
    // Distance 0 is off, whatever the gap.
    check(near(softParticleFade(0.0f, 0.1f, 0.0f, 0.0f, 0.0f), 1.0f), "both off: whole");
    check(near(softParticleFade(0.0f, 5.0f, 0.0f, 2.0f, 0.5f), 1.0f), "intersection fade off: near fade only");
    // Metres, not pixels: nothing here depends on resolution, so neither does the shader's.
}

void testSettings() {
    GraphicsSettings s;
    check(s.softParticles && s.particleDistortion && s.particleLighting == ParticleLighting::Scene, "defaults");
    check(
        std::string(particleLightingName(ParticleLighting::Simple)) == "simple" &&
            std::string(particleLightingName(ParticleLighting::Scene)) == "scene",
        "particle_lighting names");
    std::string error;
    for (const char* key : {"soft_particles", "particle_distortion", "particle_lighting"}) {
        const GraphicsSettingInfo* info = findGraphicsSetting(key);
        check(info && info->preset, std::string(key) + " is a preset setting");
    }
    const GraphicsSettingInfo* soft = findGraphicsSetting("soft_particles");
    const GraphicsSettingInfo* distortion = findGraphicsSetting("particle_distortion");
    const GraphicsSettingInfo* lighting = findGraphicsSetting("particle_lighting");
    if (!soft || !distortion || !lighting) return;
    check(soft->set(s, "false", error) && !s.softParticles && soft->get(s) == "false", "soft_particles false");
    check(distortion->set(s, "false", error) && !s.particleDistortion, "particle_distortion false");
    check(
        lighting->set(s, "simple", error) && s.particleLighting == ParticleLighting::Simple,
        "particle_lighting simple");
    check(
        !lighting->set(s, "full", error) && error.find("particle_lighting") != std::string::npos, "bad values refused");
    check(!soft->set(s, "maybe", error), "soft_particles takes true or false");
    // Low turns all three down; the others turn them up.
    const GraphicsSettings low = graphicsPreset(GraphicsPreset::Low, s);
    check(!low.softParticles && !low.particleDistortion && low.particleLighting == ParticleLighting::Simple, "Low");
    for (GraphicsPreset p : {GraphicsPreset::Medium, GraphicsPreset::High, GraphicsPreset::Ultra}) {
        const GraphicsSettings g = graphicsPreset(p, low);
        check(
            g.softParticles && g.particleDistortion && g.particleLighting == ParticleLighting::Scene,
            std::string(graphicsPresetName(p)));
    }
}
}   // namespace

int main() {
    testFade();
    testSettings();
    if (failures) {
        std::fprintf(stderr, "%d particle check(s) failed\n", failures);
        return 1;
    }
    std::printf("particles: all checks passed\n");
    return 0;
}
