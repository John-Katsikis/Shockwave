#include "SteamAudio.h"

#include <Audio/Audio.h>
#include <Audio/SpatialAudio.h>

#include <Assets/Assets.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/Systems.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace shockwave {
namespace {
std::atomic<bool> g_hrtf{true}, g_bilinear{true}, g_steamAudio{true};
// Steam Audio's default air absorption, per meter, for its low, mid and high bands.
constexpr std::array<float, 3> kAirPerMeter{0.0002f, 0.0017f, 0.0182f};
// A wall lets lows through more than highs, like Steam Audio's generic material.
constexpr std::array<float, 3> kTransmissionShape{1.0f, 0.5f, 0.3f};
glm::vec3 unit(glm::vec3 v, glm::vec3 fallback) {
    const float length = glm::length(v);
    return std::isfinite(length) && length > 1e-6f ? v / length : fallback;
}
// Read-only iteration in entity order, so problems come out in scene file order.
template <class T, class Fn>
void forEach(const World& world, Fn&& fn) {
    const auto* pool = world.pool<T>();
    if (!pool) return;
    const auto entities = pool->entities();
    const auto components = pool->components();
    std::vector<size_t> order(entities.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return entities[a].index < entities[b].index; });
    for (size_t i : order) fn(entities[i], components[i]);
}
std::string entityName(const World& world, Entity e) {
    const auto* name = world.get<Name>(e);
    return name ? name->value : "?";
}
}   // namespace

void setAudioSettings(const AudioSettings& settings) {
    g_hrtf.store(settings.hrtf);
    g_bilinear.store(settings.hrtfBilinear);
    g_steamAudio.store(settings.steamAudio);
}
AudioSettings audioSettings() { return {g_hrtf.load(), g_bilinear.load(), g_steamAudio.load()}; }
bool steamAudioAvailable() { return steam::available(); }
const std::string& steamAudioStatus() { return steam::status(); }
std::string spatialBackendName() {
    const auto settings = audioSettings();
    if (!settings.steamAudio) return "simple panning";
    if (!steam::available()) return "simple panning (Steam Audio not found)";
    if (!settings.hrtf) return "Steam Audio panning";
    return settings.hrtfBilinear ? "Steam Audio HRTF" : "Steam Audio HRTF (low quality)";
}

AudioListenerPose listenerPose(const glm::mat4& world) {
    AudioListenerPose pose;
    pose.position = glm::vec3(world[3]);
    pose.right = unit(glm::vec3(world[0]), {1, 0, 0});
    pose.up = unit(glm::vec3(world[1]), {0, 1, 0});
    pose.ahead = -unit(glm::vec3(world[2]), {0, 0, 1});
    return pose;
}

Entity audioListener(const World& world) {
    Entity found = kNullEntity;
    if (const auto* listeners = world.pool<AudioListener>())
        for (const Entity e : listeners->entities())   // lowest index first, as scene files list them
            if (world.has<Transform>(e) && (!found.valid() || e.index < found.index)) found = e;
    return found.valid() ? found : primaryCamera(world);
}

float audioRolloffGain(AudioRolloff rolloff, float distance, float near, float far) {
    if (!std::isfinite(distance) || !std::isfinite(near) || !std::isfinite(far) || near < 0 || far < near) return 0;
    if (distance <= near) return 1;
    if (distance >= far) return 0;
    switch (rolloff) {
    case AudioRolloff::Inverse: {
        if (near <= 0) return 0;
        // 1/distance, shifted and scaled to go from 1 at min to exactly 0 at max.
        const float floor = near / far;
        return std::clamp((near / distance - floor) / (1 - floor), 0.0f, 1.0f);
    }
    case AudioRolloff::None: return 1;
    case AudioRolloff::Linear:
    default: return 1 - (distance - near) / (far - near);
    }
}

AudioSpatial spatialize(
    const AudioListenerPose& listener, glm::vec3 sourcePosition, glm::vec3 sourceAhead, const AudioSource& source,
    float occlusion) {
    AudioSpatial s;
    const glm::vec3 delta = sourcePosition - listener.position;
    s.distance = glm::length(delta);
    if (!std::isfinite(s.distance)) return AudioSpatial{.distanceGain = 0};
    const glm::vec3 toSource = unit(delta, listener.ahead);
    if (s.distance > 1e-4f)
        s.direction = unit(
            glm::vec3(
                glm::dot(toSource, listener.right), glm::dot(toSource, listener.up),
                -glm::dot(toSource, listener.ahead)),
            {0, 0, -1});
    s.distanceGain = audioRolloffGain(source.rolloff, s.distance, source.minDistance, source.maxDistance);
    if (source.directivity > 0 && s.distance > 1e-4f) {
        // Steam Audio's directivity model, a blend of everywhere-alike and a cosine lobe,
        // without its rear lobe: 0.5 is a cardioid, 1 is silent behind the source.
        const float facing = glm::dot(unit(sourceAhead, {0, 0, -1}), -toSource);
        const float weight = std::clamp(source.directivity, 0.0f, 1.0f);
        s.directivity =
            std::pow(std::max((1 - weight) + weight * facing, 0.0f), std::max(source.directivityPower, 0.0f));
    }
    if (source.airAbsorption)
        for (int band = 0; band < 3; ++band) s.air[band] = std::exp(-kAirPerMeter[band] * s.distance);
    if (source.occlusion) {
        s.occlusion = std::clamp(occlusion, 0.0f, 1.0f);
        const float through = std::clamp(source.transmission, 0.0f, 1.0f);
        for (int band = 0; band < 3; ++band) s.transmission[band] = through * kTransmissionShape[band];
    }
    return s;
}

glm::vec2 simplePanGains(const AudioSpatial& s) {
    const float pan = std::clamp(s.direction.x, -1.0f, 1.0f);
    const float blocked = s.occlusion + (1 - s.occlusion) * s.transmission[1];
    const float gain = std::max(s.distanceGain * s.directivity * blocked, 0.0f);
    return gain * glm::vec2(std::sqrt((1 - pan) * .5f), std::sqrt((1 + pan) * .5f));
}

std::vector<std::string> checkAudioScene(const World& world, const Assets& assets) {
    std::vector<std::string> problems;
    bool anySpatial = false;
    forEach<AudioSource>(world, [&](Entity e, const AudioSource& source) {
        const std::string who = "entity '" + entityName(world, e) + "': ";
        for (const auto& problem : validateAudioSource(source, assets)) problems.push_back(who + problem);
        if (source.spatial && !world.has<Transform>(e))
            problems.push_back(who + "spatial AudioSource has no Transform");
        if (!source.spatial && source.occlusion)
            problems.push_back(who + "AudioSource occlusion only works when spatial is on");
        anySpatial = anySpatial || source.spatial;
    });
    int listeners = 0;
    forEach<AudioListener>(world, [&](Entity e, const AudioListener&) {
        const std::string who = "entity '" + entityName(world, e) + "': ";
        if (!world.has<Transform>(e)) problems.push_back(who + "AudioListener has no Transform; it can't hear");
        if (++listeners == 2) problems.push_back(who + "more than one AudioListener; only the first is used");
    });
    if (anySpatial && !audioListener(world).valid())
        problems.push_back("spatial sounds are silent: no AudioListener or primary Camera with a Transform");
    return problems;
}

}   // namespace shockwave
