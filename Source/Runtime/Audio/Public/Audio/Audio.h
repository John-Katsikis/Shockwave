#pragma once
#include <Audio/SpatialAudio.h>

#include <Assets/AudioClip.h>
#include <Engine/EntityRef.h>
#include <Engine/Subsystem.h>
#include <Engine/World.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {
class Assets;
struct AudioSource {
    std::string clip{};
    float volume = 1.0f, pitch = 1.0f;
    bool loop = false, playOnStart = true, spatial = true;
    float minDistance = 1.0f, maxDistance = 30.0f;
    // Spatial sound (docs/SpatialAudio.md); ignored when spatial is off.
    AudioRolloff rolloff = AudioRolloff::Linear;
    bool airAbsorption = true;   // distant sounds lose their highs
    float directivity = 0.0f;   // 0 everywhere alike .. 1 loudest in front (-Z), silent behind
    float directivityPower = 1.0f;   // higher narrows the beam
    bool occlusion = false;   // rays against physics colliders muffle the sound behind walls
    float occlusionRadius = 0.5f;   // source size for occlusion rays; 0 casts one ray
    float transmission = 0.1f;   // how much gets through a wall (low frequencies more)
};
void registerAudioComponents();
std::vector<std::string> validateAudioSource(const AudioSource& source, const Assets& assets);
// Linear distance falloff and equal-power pan. right is listener's world +X.
glm::vec2
spatialAudioGains(glm::vec3 source, glm::vec3 listener, glm::vec3 right, float minDistance, float maxDistance);
using AudioVoice = uint64_t;   // 0 means unavailable/full command queue
struct AudioPlayback {
    float volume = 1.0f, pitch = 1.0f;
    bool loop = false, spatial = false;   // spatial: mixed to mono, then `gains`
    glm::vec2 gains{1.0f};
    // Rendered from `position` by Steam Audio (HRTF or panning) when it's loaded and a
    // spatial slot is free; otherwise `gains` apply as for spatial.
    bool positional = false;
    AudioSpatial position{};
};
// What the mixer is doing, for the Stats window and player --audio-mix.
struct AudioMixerStats {
    float mixMicroseconds = 0;   // average time to mix one 256-frame block (5.3 ms of sound)
    float peakMicroseconds = 0;   // the slowest block recently
    int voices = 0;   // playing now
    int spatialVoices = 0;   // of those, rendered by Steam Audio
    int spatialSlots = 0;   // Steam Audio voices this mixer can render at once
};
// SDL's device callback owns mixing. Transport posts bounded commands, safe from any
// thread; decoded clips stay resident. No World access or decoding on the audio thread.
class AudioMixer {
public:
    explicit AudioMixer(bool openDevice = true);
    ~AudioMixer();
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;
    AudioVoice play(std::shared_ptr<const AudioClip> clip, AudioPlayback options = {});
    bool stop(AudioVoice voice);
    bool update(AudioVoice voice, std::shared_ptr<const AudioClip> clip, AudioPlayback options);
    // Moves a playing voice: only gains and position change. Cheaper than update.
    bool place(AudioVoice voice, glm::vec2 gains, const AudioSpatial& position);
    bool setVolume(AudioVoice voice, float volume);
    bool isPlaying(AudioVoice voice) const;
    // The mixer has started this voice and it has since ended (or was replaced).
    bool finished(AudioVoice voice) const;
    AudioMixerStats stats() const;
    void setMasterVolume(float volume);
    float masterVolume() const;
    void pause(bool paused);
    bool available() const;
    const std::string& error() const;
    // Offline mixer only (openDevice=false), for deterministic PCM tests/tools.
    void render(std::span<float> stereo);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
// A blocker found by an occlusion ray.
struct AudioRayHit {
    Entity entity{};
    float distance = 0;
};
// Casts a ray, ignoring one entity; PlaySession plugs in the physics world's raycast.
using AudioRaycast =
    std::function<std::optional<AudioRayHit>(glm::vec3 origin, glm::vec3 direction, float maxDistance, Entity ignore)>;
// One spatial sound as heard, for player --audio-mix and the editor.
struct AudioSourceReport {
    Entity entity{};   // null for a sound from playAt/playOn
    std::string clip{};
    bool playing = false, spatial = false;
    AudioSpatial heard;
};
class AudioWorld final : public Subsystem {
public:
    AudioWorld(World& world, const Assets* assets, bool openDevice = true);
    ~AudioWorld() override;
    void synchronize();   // main thread: listener/source snapshots, new play-on-start sources
    // Main thread, once per fixed tick before synchronize: casts occlusion rays and
    // smooths the result, so walls fade sounds in and out instead of cutting them.
    void update(double dt);
    bool play(Entity source);   // any thread, after synchronize
    bool stop(Entity source);
    bool setVolume(Entity source, float volume);
    AudioVoice playClip(std::string_view clip, AudioPlayback options = {});
    // One-off 3D sounds, from any thread. settings gives volume, pitch, loop, distances,
    // rolloff and the rest (its clip is ignored). playOn follows the entity until the sound
    // ends; a looping one stops when the entity is destroyed. Up to 128 at once.
    //   game.audio().playAt("Audio/boom.wav", hit.point);
    //   game.audio().playOn("Audio/engine.wav", car, {.loop = true, .maxDistance = 60});
    AudioVoice playAt(std::string_view clip, glm::vec3 position, const AudioSource& settings = {});
    AudioVoice playOn(std::string_view clip, Entity entity, const AudioSource& settings = {});
    AudioVoice playOn(std::string_view clip, const EntityRef& entity, const AudioSource& settings = {}) {
        return playOn(clip, entity.entity(), settings);
    }
    bool stopVoice(AudioVoice voice) { return mixer().stop(voice); }
    void setRaycast(AudioRaycast raycast);   // main thread, before Play's first tick
    Entity listener() const;   // as of the last synchronize
    std::vector<AudioSourceReport> report() const;   // main thread; allocates, for tools
    AudioMixer& mixer();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}   // namespace shockwave
