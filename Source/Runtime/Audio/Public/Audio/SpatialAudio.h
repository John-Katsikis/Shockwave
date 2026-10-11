#pragma once

// Spatial (3D) audio: where a sound is heard from, how distance, air, the source's
// facing and walls change it, and who is listening. The math here runs on the main
// thread; the mixer only applies its results. See docs/SpatialAudio.md.
//
// Rendering uses Steam Audio (libphonon, loaded at runtime from next to the executable)
// for HRTF binaural or speaker panning. Without the library, spatial voices fall back to
// equal-power stereo panning with distance attenuation, so nothing breaks.

#include <Engine/World.h>

#include <glm/glm.hpp>

#include <array>
#include <string>
#include <vector>

namespace shockwave {

class Assets;
struct AudioSource;

// How volume falls between min and max distance. All reach silence at max distance.
enum class AudioRolloff : int {
    Linear,   // a straight line from full volume to silence
    Inverse,   // like real sound (1/distance), rescaled to reach silence at max; needs min > 0
    None,   // full volume up to max distance, then silence
};

// Marks the entity that hears spatial sound, looking down its -Z axis. Without one, the
// scene's primary Camera listens. With several, the first one wins.
struct AudioListener {};

// Global spatial audio settings, from the project (audio_hrtf, audio_hrtf_quality).
struct AudioSettings {
    bool hrtf = true;   // headphones: HRTF binaural; false: speaker panning
    bool hrtfBilinear = true;   // HRTF quality: true high (bilinear), false low (nearest)
    bool steamAudio = true;   // false forces the simple built-in panner
};
// Thread-safe. Every mixer picks the change up at its next audio block.
void setAudioSettings(const AudioSettings& settings);
AudioSettings audioSettings();
// Loads Steam Audio on first use (main thread, about 20 ms for the HRTF). Thread-safe.
bool steamAudioAvailable();
// What renders spatial sound right now: "Steam Audio HRTF", "Steam Audio panning",
// "simple panning", or "simple panning (Steam Audio not found)".
std::string spatialBackendName();
// Why Steam Audio is missing, or where it was loaded from.
const std::string& steamAudioStatus();

// One spatial voice as heard by the listener. Made by spatialize() on the main thread.
struct AudioSpatial {
    glm::vec3 direction{0, 0, -1};   // listener space, unit: +X right, +Y up, -Z ahead
    float distance = 0;
    float distanceGain = 1;   // from the rolloff curve
    float directivity = 1;   // 1 facing the listener .. 0 for a dipole facing away
    float occlusion = 1;   // share of rays that reach the listener: 1 open .. 0 blocked
    std::array<float, 3> air{1, 1, 1};   // air absorption per band: low, mid, high
    std::array<float, 3> transmission{1, 1, 1};   // what passes through blockers, per band
    bool operator==(const AudioSpatial&) const = default;
};

struct AudioListenerPose {
    glm::vec3 position{0}, right{1, 0, 0}, up{0, 1, 0}, ahead{0, 0, -1};
};
AudioListenerPose listenerPose(const glm::mat4& world);
// The first AudioListener with a Transform, else the primary Camera if it has one.
Entity audioListener(const World& world);

float audioRolloffGain(AudioRolloff rolloff, float distance, float minDistance, float maxDistance);
// occlusion: 1 when nothing blocks (or the source doesn't use occlusion).
AudioSpatial spatialize(
    const AudioListenerPose& listener, glm::vec3 sourcePosition, glm::vec3 sourceAhead, const AudioSource& source,
    float occlusion = 1);
// The simple panner's left/right gains: equal-power pan, rolloff, directivity and
// occlusion, without air absorption or HRTF. The fallback when Steam Audio is missing.
glm::vec2 simplePanGains(const AudioSpatial& spatial);

// Every audio problem in a scene, for player --check: invalid sources, spatial sources
// without a Transform or a listener, extra listeners. Each names its entity.
std::vector<std::string> checkAudioScene(const World& world, const Assets& assets);

}   // namespace shockwave
