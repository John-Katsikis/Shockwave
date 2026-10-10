#pragma once
#include <Assets/AudioClip.h>
#include <Engine/Subsystem.h>
#include <Engine/World.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {
class Assets;
struct AudioSource {
    std::string clip;
    float volume = 1.0f, pitch = 1.0f;
    bool loop = false, playOnStart = true, spatial = true;
    float minDistance = 1.0f, maxDistance = 30.0f;
};
void registerAudioComponents();
std::vector<std::string> validateAudioSource(const AudioSource& source, const Assets& assets);
// Linear distance falloff and equal-power pan. right is listener's world +X.
glm::vec2
spatialAudioGains(glm::vec3 source, glm::vec3 listener, glm::vec3 right, float minDistance, float maxDistance);
using AudioVoice = uint64_t;   // 0 means unavailable/full command queue
struct AudioPlayback {
    float volume = 1.0f, pitch = 1.0f;
    bool loop = false, spatial = false;
    glm::vec2 gains{1.0f};
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
    bool setVolume(AudioVoice voice, float volume);
    bool isPlaying(AudioVoice voice) const;
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
class AudioWorld final : public Subsystem {
public:
    AudioWorld(World& world, const Assets* assets, bool openDevice = true);
    ~AudioWorld() override;
    void synchronize();   // main thread: listener/source snapshots, new play-on-start sources
    bool play(Entity source);   // any thread, after synchronize
    bool stop(Entity source);
    bool setVolume(Entity source, float volume);
    AudioVoice playClip(std::string_view clip, AudioPlayback options = {});
    AudioMixer& mixer();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}   // namespace shockwave
