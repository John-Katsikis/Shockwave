#include <Audio/Audio.h>

#include "SteamAudio.h"

#include <Assets/Assets.h>
#include <Core/Log.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/Systems.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace shockwave {
namespace {
SW_LOG_CATEGORY(LogAudio);
constexpr size_t kVoices = 128, kCommands = 1024, kEmitters = 128;
// Clips the audio thread lets go of wait here, so their memory is freed by the next
// thread that posts a command instead of by the audio thread.
constexpr size_t kRetired = 256, kRetireBatch = 16;
constexpr size_t kBlock = steam::kBlock;
bool finite(glm::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool valid(const AudioSpatial& s) {
    bool ok = finite(s.direction) && std::isfinite(s.distance) && std::isfinite(s.distanceGain) &&
              std::isfinite(s.directivity) && std::isfinite(s.occlusion);
    for (int band = 0; band < 3; ++band) ok = ok && std::isfinite(s.air[band]) && std::isfinite(s.transmission[band]);
    return ok;
}
bool valid(const AudioPlayback& p) {
    return std::isfinite(p.volume) && p.volume >= 0 && p.volume <= 1 && std::isfinite(p.pitch) && p.pitch >= .125f &&
           p.pitch <= 8 && std::isfinite(p.gains.x) && std::isfinite(p.gains.y) && p.gains.x >= 0 && p.gains.y >= 0 &&
           (!p.positional || valid(p.position));
}
bool validDistances(const AudioSource& s) {
    return std::isfinite(s.minDistance) && std::isfinite(s.maxDistance) && s.minDistance >= 0 &&
           s.maxDistance >= s.minDistance;
}
uint64_t key(Entity e) { return (uint64_t(e.generation) << 32) | e.index; }
AudioPlayback playback(const AudioSource& s, const AudioSpatial& heard) {
    AudioPlayback p;
    p.volume = s.volume;
    p.pitch = s.pitch;
    p.loop = s.loop;
    p.spatial = s.spatial;
    p.gains = s.spatial ? simplePanGains(heard) : glm::vec2(1);
    p.positional = s.spatial;
    if (s.spatial) p.position = heard;
    return p;
}
}   // namespace

void registerAudioComponents() {
    auto clipField = field("clip", &AudioSource::clip);
    clipField.type = FieldType::AudioClip;
    registerComponent<AudioSource>(
        "AudioSource",
        {clipField, field("volume", &AudioSource::volume, .01f, 0, 1),
         field("pitch", &AudioSource::pitch, .01f, .125f, 8), field("loop", &AudioSource::loop),
         field("play_on_start", &AudioSource::playOnStart), field("spatial", &AudioSource::spatial),
         field("min_distance", &AudioSource::minDistance, .1f, 0, 10000),
         field("max_distance", &AudioSource::maxDistance, .1f, 0, 10000),
         enumField("rolloff", &AudioSource::rolloff, {"Linear", "Inverse", "None"}),
         field("air_absorption", &AudioSource::airAbsorption),
         field("directivity", &AudioSource::directivity, .01f, 0, 1),
         field("directivity_power", &AudioSource::directivityPower, .05f, 0, 16),
         field("occlusion", &AudioSource::occlusion),
         field("occlusion_radius", &AudioSource::occlusionRadius, .01f, 0, 10),
         field("transmission", &AudioSource::transmission, .01f, 0, 1)},
        "Audio");
    registerComponent<AudioListener>("AudioListener", {}, "Audio");
}
std::vector<std::string> validateAudioSource(const AudioSource& s, const Assets& assets) {
    std::vector<std::string> errors;
    if (s.clip.empty() || !assets.audioClip(s.clip)) errors.push_back("AudioSource clip is missing: '" + s.clip + "'");
    if (!std::isfinite(s.volume) || s.volume < 0 || s.volume > 1)
        errors.push_back("AudioSource volume must be in 0..1");
    if (!std::isfinite(s.pitch) || s.pitch < .125f || s.pitch > 8)
        errors.push_back("AudioSource pitch must be in 0.125..8");
    if (!validDistances(s))
        errors.push_back("AudioSource max distance must be at least min distance, both finite and nonnegative");
    const int rolloff = static_cast<int>(s.rolloff);
    if (rolloff < 0 || rolloff > static_cast<int>(AudioRolloff::None))
        errors.push_back("AudioSource rolloff must be Linear, Inverse or None");
    else if (s.spatial && s.rolloff == AudioRolloff::Inverse && !(s.minDistance > 0))
        errors.push_back("AudioSource inverse rolloff needs a min distance above 0");
    if (!std::isfinite(s.directivity) || s.directivity < 0 || s.directivity > 1)
        errors.push_back("AudioSource directivity must be in 0..1");
    if (!std::isfinite(s.directivityPower) || s.directivityPower < 0 || s.directivityPower > 16)
        errors.push_back("AudioSource directivity power must be in 0..16");
    if (!std::isfinite(s.occlusionRadius) || s.occlusionRadius < 0 || s.occlusionRadius > 10)
        errors.push_back("AudioSource occlusion radius must be in 0..10");
    if (!std::isfinite(s.transmission) || s.transmission < 0 || s.transmission > 1)
        errors.push_back("AudioSource transmission must be in 0..1");
    return errors;
}
glm::vec2 spatialAudioGains(glm::vec3 source, glm::vec3 listener, glm::vec3 right, float near, float far) {
    const auto delta = source - listener;
    const float distance = glm::length(delta);
    if (!std::isfinite(distance) || !std::isfinite(near) || !std::isfinite(far) || near < 0 || far < near) return {};
    const float gain = distance <= near ? 1 : distance >= far ? 0 : 1 - (distance - near) / (far - near);
    float pan = 0;
    const float rightLength = glm::length(right);
    if (distance > 1e-6f && std::isfinite(rightLength) && rightLength > 1e-6f)
        pan = std::clamp(glm::dot(delta / distance, right / rightLength), -1.0f, 1.0f);
    return gain * glm::vec2(std::sqrt((1 - pan) * .5f), std::sqrt((1 + pan) * .5f));
}

// ---- Mixer ------------------------------------------------------------------------------
// Two paths share the voices. Plain and simple-panned voices mix sample by sample, so
// commands land within one SDL callback. Positional voices that got a Steam Audio slot
// render in fixed 256-frame blocks into a small bus that output then drains.
struct AudioMixer::Impl {
    enum class Op { Play, Stop, Update, Volume, Place };
    struct Command {
        Op op{};
        AudioVoice id = 0;
        std::shared_ptr<const AudioClip> clip;
        AudioPlayback options;
    };
    struct Voice {
        AudioVoice id = 0;
        std::shared_ptr<const AudioClip> clip;
        AudioPlayback options;
        double cursor = 0;
        int slot = -1;   // Steam Audio slot, or -1 for the sample-by-sample path
    };
    // Guarded by commandsMutex. The audio thread only ever try-locks it.
    std::mutex commandsMutex;
    std::array<Command, kCommands> commands;
    size_t read = 0, count = 0;
    AudioVoice next = 1;
    std::array<std::shared_ptr<const AudioClip>, kRetired> retired;
    size_t retiredCount = 0;

    std::array<Voice, kVoices> voices;
    std::array<std::atomic<AudioVoice>, kVoices> playing{};
    std::atomic<AudioVoice> consumed{0};   // the newest voice id the audio thread has seen
    std::atomic<float> master{1};
    std::atomic<bool> paused{false};
    // Steam Audio bus: effects made here, before the device can call back.
    steam::Renderer spatial;
    std::array<float, kBlock> busLeft{}, busRight{}, mono{};
    size_t busRead = kBlock;   // kBlock: the bus is drained
    std::atomic<float> mixMicros{0}, peakMicros{0};
    std::atomic<int> voiceCount{0}, spatialCount{0};

    SDL_AudioStream* stream = nullptr;
    bool initialized = false, offline = false;
    std::string problem;
    explicit Impl(bool device) : offline(!device) {
        if (!device) return;
        if (!(initialized = SDL_InitSubSystem(SDL_INIT_AUDIO))) {
            problem = SDL_GetError();
            return;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, AudioClip::sampleRate};
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, callback, this);
        if (!stream) {
            problem = SDL_GetError();
            return;
        }
        if (!SDL_ResumeAudioStreamDevice(stream)) {
            problem = SDL_GetError();
            SDL_DestroyAudioStream(stream);
            stream = nullptr;
        }
    }
    ~Impl() {
        if (stream) SDL_DestroyAudioStream(stream);
        if (initialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    // Any thread. Play gets its id here, under the lock, so ids reach the audio thread in
    // order and finished() can trust `consumed`.
    AudioVoice post(Command command) {
        if (!offline && !stream) return 0;
        std::array<std::shared_ptr<const AudioClip>, kRetireBatch> release;
        AudioVoice id = command.id;
        {
            std::lock_guard lock(commandsMutex);
            if (count == kCommands) return 0;
            if (command.op == Op::Play) id = command.id = next++;
            commands[(read + count) % kCommands] = std::move(command);
            ++count;
            for (size_t i = 0; i < release.size() && retiredCount; ++i) release[i] = std::move(retired[--retiredCount]);
        }
        return id;   // `release` frees retired clips here, outside the lock
    }
    // Audio thread, holding commandsMutex.
    void retire(std::shared_ptr<const AudioClip>& clip) {
        if (!clip) return;
        if (retiredCount < kRetired)
            retired[retiredCount++] = std::move(clip);
        else
            clip.reset();   // only when no producer has posted for a long time
    }
    void freeSlot(Voice& voice) {
        spatial.release(voice.slot);
        voice.slot = -1;
    }
    void consume() {
        // The callback never waits for a producer; commands can wait one callback.
        std::unique_lock lock(commandsMutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        for (size_t i = 0; i < count; ++i) {
            auto& cmd = commands[(read + i) % kCommands];
            size_t slot = kVoices;
            for (size_t v = 0; v < kVoices; ++v)
                if (voices[v].id == cmd.id) {
                    slot = v;
                    break;
                }
            if (cmd.op == Op::Play) {
                if (slot == kVoices) {
                    for (size_t v = 0; v < kVoices; ++v)
                        if (!voices[v].id) {
                            slot = v;
                            break;
                        }
                    if (slot == kVoices)
                        slot = size_t(
                            std::min_element(
                                voices.begin(), voices.end(), [](auto& a, auto& b) { return a.id < b.id; }) -
                            voices.begin());
                }
                auto& voice = voices[slot];
                freeSlot(voice);
                retire(voice.clip);
                voice.id = cmd.id;
                voice.clip = std::move(cmd.clip);
                voice.options = cmd.options;
                voice.cursor = 0;
                playing[slot].store(cmd.id);
                consumed.store(cmd.id);
            } else if (slot != kVoices) {
                auto& voice = voices[slot];
                if (cmd.op == Op::Stop) {
                    voice.id = 0;
                    playing[slot].store(0);
                    freeSlot(voice);
                } else if (cmd.op == Op::Update) {
                    retire(voice.clip);
                    voice.clip = std::move(cmd.clip);
                    voice.options = cmd.options;
                } else if (cmd.op == Op::Place) {
                    voice.options.gains = cmd.options.gains;
                    voice.options.position = cmd.options.position;
                } else
                    voice.options.volume = cmd.options.volume;
            }
            retire(cmd.clip);   // a command for a voice that already ended
        }
        read = (read + count) % kCommands;
        count = 0;
    }
    // Gives positional voices a Steam Audio slot while it's on, and takes them back when
    // it's turned off. Returns how many voices play.
    int assignSlots(bool steamAudio, int& spatialVoices) {
        int active = 0;
        spatialVoices = 0;
        for (auto& voice : voices) {
            if (!voice.id) continue;
            ++active;
            if (steamAudio && voice.options.positional) {
                if (voice.slot < 0) voice.slot = spatial.claim();
            } else if (voice.slot >= 0)
                freeSlot(voice);
            spatialVoices += voice.slot >= 0;
        }
        return active;
    }
    // One block of every Steam Audio voice into the bus. No allocation.
    void renderBus(bool hrtf, bool bilinear) {
        busLeft.fill(0);
        busRight.fill(0);
        for (size_t v = 0; v < kVoices; ++v) {
            auto& voice = voices[v];
            if (!voice.id || voice.slot < 0 || !voice.clip || !voice.clip->frames()) continue;
            const auto& samples = voice.clip->samples;
            const auto n = voice.clip->frames();
            const auto& p = voice.options;
            bool ended = false;
            size_t f = 0;
            for (; f < kBlock; ++f) {
                if (voice.cursor >= n) {
                    if (!p.loop) {
                        ended = true;
                        break;
                    }
                    voice.cursor = std::fmod(voice.cursor, double(n));
                }
                const size_t a = size_t(voice.cursor), b = a + 1 < n ? a + 1 : p.loop ? 0 : a;
                const float fraction = float(voice.cursor - a);
                mono[f] = .5f * (std::lerp(samples[a * 2], samples[b * 2], fraction) +
                                 std::lerp(samples[a * 2 + 1], samples[b * 2 + 1], fraction));
                voice.cursor += p.pitch;
            }
            std::fill(mono.begin() + f, mono.end(), 0.0f);
            spatial.process(
                voice.slot, mono.data(), p.position, p.volume, hrtf, bilinear, busLeft.data(), busRight.data());
            if (ended) {
                voice.id = 0;
                playing[v].store(0);
                freeSlot(voice);
            }
        }
    }
    void mix(std::span<float> output) {
        const auto start = std::chrono::steady_clock::now();
        consume();
        std::fill(output.begin(), output.end(), 0);
        const size_t frames = output.size() / 2;
        if (paused.load() || !frames) return;
        const float volume = master.load();
        const auto settings = audioSettings();
        int spatialVoices = 0;
        const int active = assignSlots(spatial.ready() && settings.steamAudio, spatialVoices);
        for (size_t v = 0; v < kVoices; ++v) {
            auto& voice = voices[v];
            if (!voice.id || voice.slot >= 0 || !voice.clip || !voice.clip->frames()) continue;
            const auto& samples = voice.clip->samples;
            const auto n = voice.clip->frames();
            const auto& p = voice.options;
            const auto gain = p.gains * (p.volume * volume);
            for (size_t f = 0; f < frames; ++f) {
                if (voice.cursor >= n) {
                    if (p.loop)
                        voice.cursor = std::fmod(voice.cursor, double(n));
                    else {
                        voice.id = 0;
                        playing[v].store(0);
                        break;
                    }
                }
                const size_t a = size_t(voice.cursor), b = a + 1 < n ? a + 1 : p.loop ? 0 : a;
                const float fraction = float(voice.cursor - a);
                glm::vec2 value{
                    std::lerp(samples[a * 2], samples[b * 2], fraction),
                    std::lerp(samples[a * 2 + 1], samples[b * 2 + 1], fraction)};
                if (p.spatial || p.positional) value = glm::vec2((value.x + value.y) * .5f);
                output[f * 2] += value.x * gain.x;
                output[f * 2 + 1] += value.y * gain.y;
                voice.cursor += p.pitch;
            }
        }
        if (spatialVoices > 0 || busRead < kBlock) {
            for (size_t done = 0; done < frames;) {
                if (busRead == kBlock) {
                    renderBus(settings.hrtf, settings.hrtfBilinear);
                    busRead = 0;
                }
                const size_t n = std::min(frames - done, kBlock - busRead);
                for (size_t i = 0; i < n; ++i) {
                    output[(done + i) * 2] += busLeft[busRead + i] * volume;
                    output[(done + i) * 2 + 1] += busRight[busRead + i] * volume;
                }
                busRead += n;
                done += n;
            }
        }
        for (auto& sample : output) sample = std::clamp(sample, -1.0f, 1.0f);
        // Cost per 256 frames, so callbacks of any size compare.
        const float micros =
            std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - start).count() * float(kBlock) /
            float(frames);
        mixMicros.store(mixMicros.load() * .95f + micros * .05f);
        peakMicros.store(std::max(micros, peakMicros.load() * .999f));
        voiceCount.store(active);
        spatialCount.store(spatialVoices);
    }
    static void SDLCALL callback(void* userdata, SDL_AudioStream* stream, int bytes, int) {
        auto& self = *static_cast<Impl*>(userdata);
        std::array<float, 1024> buffer;
        while (bytes > 0) {
            const size_t frames = std::min<size_t>((size_t(bytes) + 7) / 8, buffer.size() / 2);
            self.mix(std::span(buffer.data(), frames * 2));
            if (!SDL_PutAudioStreamData(stream, buffer.data(), int(frames * 8))) break;
            bytes -= int(frames * 8);
        }
    }
};
AudioMixer::AudioMixer(bool device) : m_impl(std::make_unique<Impl>(device)) {
    if (!m_impl->problem.empty()) SW_LOG(LogAudio, Warning, "audio unavailable: {}", m_impl->problem);
}
AudioMixer::~AudioMixer() = default;
AudioVoice AudioMixer::play(std::shared_ptr<const AudioClip> clip, AudioPlayback p) {
    if (!clip || !clip->frames() || !valid(p)) return 0;
    return m_impl->post({Impl::Op::Play, 0, std::move(clip), p});
}
bool AudioMixer::stop(AudioVoice id) { return id && m_impl->post({Impl::Op::Stop, id, {}, {}}); }
bool AudioMixer::update(AudioVoice id, std::shared_ptr<const AudioClip> clip, AudioPlayback p) {
    return id && clip && clip->frames() && valid(p) && m_impl->post({Impl::Op::Update, id, std::move(clip), p});
}
bool AudioMixer::place(AudioVoice id, glm::vec2 gains, const AudioSpatial& position) {
    AudioPlayback p;
    p.gains = gains;
    p.positional = true;
    p.position = position;
    return id && valid(p) && m_impl->post({Impl::Op::Place, id, {}, p});
}
bool AudioMixer::setVolume(AudioVoice id, float volume) {
    if (!std::isfinite(volume) || volume < 0 || volume > 1) return false;
    AudioPlayback p;
    p.volume = volume;
    return id && m_impl->post({Impl::Op::Volume, id, {}, p});
}
bool AudioMixer::isPlaying(AudioVoice id) const {
    if (!id) return false;
    for (const auto& voice : m_impl->playing)
        if (voice.load() == id) return true;
    return false;
}
bool AudioMixer::finished(AudioVoice id) const { return id && id <= m_impl->consumed.load() && !isPlaying(id); }
AudioMixerStats AudioMixer::stats() const {
    return {
        m_impl->mixMicros.load(), m_impl->peakMicros.load(), m_impl->voiceCount.load(), m_impl->spatialCount.load(),
        m_impl->spatial.ready() ? steam::kSlots : 0};
}
void AudioMixer::setMasterVolume(float value) {
    if (std::isfinite(value)) m_impl->master.store(std::clamp(value, 0.0f, 1.0f));
}
float AudioMixer::masterVolume() const { return m_impl->master.load(); }
void AudioMixer::pause(bool value) { m_impl->paused.store(value); }
bool AudioMixer::available() const { return m_impl->offline || m_impl->stream; }
const std::string& AudioMixer::error() const { return m_impl->problem; }
void AudioMixer::render(std::span<float> output) {
    if (!m_impl->offline || output.size() % 2)
        throw std::logic_error("render needs an offline mixer and interleaved stereo buffer");
    m_impl->mix(output);
}

// ---- World ------------------------------------------------------------------------------
struct AudioWorld::Impl {
    struct Source {
        AudioSource authored;
        AudioPlayback options;
        std::shared_ptr<const AudioClip> clip;
        AudioVoice voice = 0;
        float overrideVolume = -1;
        uint64_t seen = 0;
        bool started = false;
        float occlusion = 1, occlusionTarget = 1;
    };
    // A sound from playAt/playOn.
    struct Emitter {
        AudioVoice voice = 0;
        std::string clip;
        Entity entity = kNullEntity;   // followed by playOn
        glm::vec3 position{0}, ahead{0, 0, -1};
        AudioSource settings;
        AudioPlayback options;
        float occlusion = 1, occlusionTarget = 1;
    };
    World& world;
    const Assets* assets;
    AudioMixer mixer;
    mutable std::mutex mutex;
    std::unordered_map<uint64_t, Source> sources;
    std::array<Emitter, kEmitters> emitters;
    uint64_t epoch = 0;
    AudioRaycast raycast;
    Entity listener = kNullEntity;
    AudioListenerPose pose;
    Impl(World& w, const Assets* a, bool device) : world(w), assets(a), mixer(device && a) {}

    AudioSpatial hear(glm::vec3 position, glm::vec3 ahead, const AudioSource& settings, float occlusion) const {
        if (!listener.valid()) return AudioSpatial{.distanceGain = 0};
        return spatialize(pose, position, ahead, settings, occlusion);
    }
    // Share of rays from around the source that reach the listener unblocked.
    float openness(glm::vec3 from, Entity ignore, float radius) const {
        auto blocked = [&](glm::vec3 origin) {
            const glm::vec3 delta = pose.position - origin;
            const float length = glm::length(delta);
            if (!(length > 1e-3f)) return false;
            const glm::vec3 direction = delta / length;
            auto listenersOwn = [&](Entity hit) {
                return hit == listener || isAncestor(world, hit, listener) || isAncestor(world, listener, hit);
            };
            auto hit = raycast(origin, direction, length, ignore);
            if (!hit || listenersOwn(hit->entity)) return false;
            if (ignore.valid() && isAncestor(world, hit->entity, ignore)) {
                // The source sits inside its parent's collider: look again from beyond it.
                const float skip = hit->distance + 0.01f;
                if (skip >= length) return false;
                hit = raycast(origin + direction * skip, direction, length - skip, hit->entity);
                return hit && !listenersOwn(hit->entity);
            }
            return true;
        };
        int rays = 1, open = blocked(from) ? 0 : 1;
        if (radius > 0) {
            const glm::vec3 direction = glm::normalize(pose.position - from + glm::vec3(0, 0, 1e-6f));
            const glm::vec3 side = glm::normalize(
                glm::cross(direction, std::abs(direction.y) < .99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
            const glm::vec3 up = glm::cross(side, direction);
            for (const glm::vec3& offset : {side, -side, up, -up}) {
                ++rays;
                open += blocked(from + offset * radius) ? 0 : 1;
            }
        }
        return float(open) / float(rays);
    }
    bool occludes(const AudioSource& s, glm::vec3 position) const {
        return raycast && listener.valid() && s.spatial && s.occlusion &&
               glm::length(position - pose.position) < s.maxDistance;
    }
};
AudioWorld::AudioWorld(World& world, const Assets* assets, bool device)
    : m_impl(std::make_unique<Impl>(world, assets, device)) {}
AudioWorld::~AudioWorld() = default;
AudioMixer& AudioWorld::mixer() { return m_impl->mixer; }
void AudioWorld::setRaycast(AudioRaycast raycast) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->raycast = std::move(raycast);
}
Entity AudioWorld::listener() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->listener;
}
void AudioWorld::update(double dt) {
    auto& p = *m_impl;
    std::lock_guard lock(p.mutex);
    const float blend = std::clamp(float(dt) * 8.0f, 0.0f, 1.0f);   // about 1/8 s to settle
    for (auto& [id, source] : p.sources) {
        const Entity entity{uint32_t(id & 0xffffffffu), uint32_t(id >> 32)};
        source.occlusionTarget = 1;
        if (source.authored.occlusion && p.raycast && p.world.alive(entity) && p.world.has<Transform>(entity)) {
            const glm::vec3 position(worldMatrix(p.world, entity)[3]);
            if (p.occludes(source.authored, position))
                source.occlusionTarget = p.openness(position, entity, source.authored.occlusionRadius);
        }
        source.occlusion += (source.occlusionTarget - source.occlusion) * blend;
    }
    for (auto& e : p.emitters) {
        if (!e.voice) continue;
        e.occlusionTarget =
            p.occludes(e.settings, e.position) ? p.openness(e.position, e.entity, e.settings.occlusionRadius) : 1.0f;
        e.occlusion += (e.occlusionTarget - e.occlusion) * blend;
    }
}
void AudioWorld::synchronize() {
    auto& p = *m_impl;
    if (!p.assets) return;
    std::lock_guard lock(p.mutex);
    p.listener = audioListener(p.world);
    p.pose = p.listener.valid() ? listenerPose(worldMatrix(p.world, p.listener)) : AudioListenerPose{};
    ++p.epoch;
    p.world.each<const AudioSource>([&](Entity entity, const AudioSource& source) {
        const auto id = key(entity);
        auto& state = p.sources[id];
        state.seen = p.epoch;
        auto clip = p.assets->audioClip(source.clip);
        AudioSpatial heard{.distanceGain = 0};
        if (source.spatial && p.world.has<Transform>(entity)) {
            const auto matrix = worldMatrix(p.world, entity);
            heard = p.hear(glm::vec3(matrix[3]), -glm::vec3(matrix[2]), source, state.occlusion);
        }
        const auto oldOptions = state.options;
        const auto oldClip = state.clip;
        if (state.authored.volume != source.volume) state.overrideVolume = -1;
        state.authored = source;
        state.clip = clip;
        state.options = playback(source, heard);
        if (state.overrideVolume >= 0) state.options.volume = state.overrideVolume;
        if (!clip || !valid(state.options) || !validDistances(source)) {
            if (!state.voice || p.mixer.stop(state.voice)) state.voice = 0;
            return;
        }
        if (!state.started && source.playOnStart) {
            state.voice = p.mixer.play(clip, state.options);
            state.started = state.voice != 0;
            return;
        }
        if (!state.voice) return;
        const auto& o = state.options;
        bool posted = true;
        if (oldClip != clip || oldOptions.volume != o.volume || oldOptions.pitch != o.pitch ||
            oldOptions.loop != o.loop || oldOptions.spatial != o.spatial || oldOptions.positional != o.positional)
            posted = p.mixer.update(state.voice, clip, o);
        else if (oldOptions.gains != o.gains || !(oldOptions.position == o.position))
            posted = p.mixer.place(state.voice, o.gains, o.position);
        if (!posted) {
            state.clip = oldClip;
            state.options = oldOptions;   // retry next synchronization
        }
    });
    for (auto it = p.sources.begin(); it != p.sources.end();) {
        if (it->second.seen != p.epoch) {
            if (!it->second.voice || p.mixer.stop(it->second.voice))
                it = p.sources.erase(it);
            else
                ++it;   // a full queue must not orphan a looping voice
        } else
            ++it;
    }
    for (auto& e : p.emitters) {
        if (!e.voice) continue;
        if (p.mixer.finished(e.voice)) {
            e.voice = 0;
            continue;
        }
        if (e.entity.valid()) {
            if (p.world.alive(e.entity) && p.world.has<Transform>(e.entity)) {
                const auto matrix = worldMatrix(p.world, e.entity);
                e.position = glm::vec3(matrix[3]);
                e.ahead = -glm::vec3(matrix[2]);
            } else if (e.settings.loop) {
                if (p.mixer.stop(e.voice)) e.voice = 0;   // its entity is gone
                continue;
            }
        }
        const auto heard = p.hear(e.position, e.ahead, e.settings, e.occlusion);
        const auto gains = simplePanGains(heard);
        if (gains != e.options.gains || !(heard == e.options.position)) {
            if (p.mixer.place(e.voice, gains, heard)) {
                e.options.gains = gains;
                e.options.position = heard;
            }
        }
    }
}
bool AudioWorld::play(Entity e) {
    auto& p = *m_impl;
    std::lock_guard lock(p.mutex);
    const auto it = p.sources.find(key(e));
    if (it == p.sources.end()) return false;
    auto& source = it->second;
    source.started = true;
    if (source.voice && !p.mixer.stop(source.voice)) return false;
    source.voice = p.mixer.play(source.clip, source.options);
    return source.voice != 0;
}
bool AudioWorld::stop(Entity e) {
    auto& p = *m_impl;
    std::lock_guard lock(p.mutex);
    const auto it = p.sources.find(key(e));
    if (it == p.sources.end()) return false;
    it->second.started = true;
    const auto voice = it->second.voice;
    if (voice && !p.mixer.stop(voice)) return false;
    it->second.voice = 0;
    return true;
}
bool AudioWorld::setVolume(Entity e, float volume) {
    if (!std::isfinite(volume) || volume < 0 || volume > 1) return false;
    auto& p = *m_impl;
    std::lock_guard lock(p.mutex);
    const auto it = p.sources.find(key(e));
    if (it == p.sources.end()) return false;
    auto& source = it->second;
    if (source.voice && !p.mixer.setVolume(source.voice, volume)) return false;
    source.overrideVolume = volume;
    source.options.volume = volume;
    return true;
}
AudioVoice AudioWorld::playClip(std::string_view name, AudioPlayback options) {
    return m_impl->assets ? m_impl->mixer.play(m_impl->assets->audioClip(name), options) : 0;
}
AudioVoice AudioWorld::playAt(std::string_view clip, glm::vec3 position, const AudioSource& settings) {
    auto& p = *m_impl;
    if (!p.assets || !finite(position)) return 0;
    auto data = p.assets->audioClip(clip);
    if (!data) return 0;
    AudioSource shape = settings;
    shape.clip.assign(clip);
    shape.spatial = true;
    if (!validateAudioSource(shape, *p.assets).empty()) return 0;
    shape.clip.clear();
    std::lock_guard lock(p.mutex);
    for (auto& e : p.emitters) {
        if (e.voice) continue;
        e.clip.assign(clip);
        e.entity = kNullEntity;
        e.position = position;
        e.ahead = {0, 0, -1};
        e.settings = shape;
        e.occlusion = e.occlusionTarget =
            p.occludes(shape, position) ? p.openness(position, kNullEntity, shape.occlusionRadius) : 1.0f;
        const auto heard = p.hear(position, e.ahead, shape, e.occlusion);
        e.options = playback(shape, heard);
        e.voice = p.mixer.play(data, e.options);
        return e.voice;
    }
    SW_LOG(LogAudio, Warning, "playAt: all {} one-off 3D sounds are busy", kEmitters);
    return 0;
}
AudioVoice AudioWorld::playOn(std::string_view clip, Entity entity, const AudioSource& settings) {
    auto& p = *m_impl;
    glm::vec3 position{0}, ahead{0, 0, -1};
    {
        std::lock_guard lock(p.mutex);
        if (!p.world.alive(entity) || !p.world.has<Transform>(entity)) return 0;
        const auto matrix = worldMatrix(p.world, entity);
        position = glm::vec3(matrix[3]);
        ahead = -glm::vec3(matrix[2]);
    }
    const auto voice = playAt(clip, position, settings);
    if (!voice) return 0;
    std::lock_guard lock(p.mutex);
    for (auto& e : p.emitters)
        if (e.voice == voice) {
            e.entity = entity;
            e.ahead = ahead;
        }
    return voice;
}
std::vector<AudioSourceReport> AudioWorld::report() const {
    auto& p = *m_impl;
    std::lock_guard lock(p.mutex);
    std::vector<AudioSourceReport> out;
    for (const auto& [id, source] : p.sources) {
        const Entity entity{uint32_t(id & 0xffffffffu), uint32_t(id >> 32)};
        out.push_back(
            {entity, source.authored.clip, p.mixer.isPlaying(source.voice), source.authored.spatial,
             source.options.position});
    }
    for (const auto& e : p.emitters)
        if (e.voice) out.push_back({kNullEntity, e.clip, p.mixer.isPlaying(e.voice), true, e.options.position});
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.entity.index < b.entity.index; });
    return out;
}
}   // namespace shockwave
