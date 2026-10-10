#include <Audio/Audio.h>

#include <Assets/Assets.h>
#include <Core/Log.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/Systems.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace shockwave {
namespace {
SW_LOG_CATEGORY(LogAudio);
constexpr size_t kVoices = 128, kCommands = 1024;
bool valid(const AudioPlayback& p) {
    return std::isfinite(p.volume) && p.volume >= 0 && p.volume <= 1 && std::isfinite(p.pitch) && p.pitch >= .125f &&
           p.pitch <= 8 && std::isfinite(p.gains.x) && std::isfinite(p.gains.y) && p.gains.x >= 0 && p.gains.y >= 0;
}
uint64_t key(Entity e) { return (uint64_t(e.generation) << 32) | e.index; }
AudioPlayback playback(const AudioSource& s, glm::vec2 gains) { return {s.volume, s.pitch, s.loop, s.spatial, gains}; }
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
         field("max_distance", &AudioSource::maxDistance, .1f, 0, 10000)},
        "Audio");
}
std::vector<std::string> validateAudioSource(const AudioSource& s, const Assets& assets) {
    std::vector<std::string> errors;
    if (s.clip.empty() || !assets.audioClip(s.clip)) errors.push_back("AudioSource clip is missing: '" + s.clip + "'");
    if (!std::isfinite(s.volume) || s.volume < 0 || s.volume > 1)
        errors.push_back("AudioSource volume must be in 0..1");
    if (!std::isfinite(s.pitch) || s.pitch < .125f || s.pitch > 8)
        errors.push_back("AudioSource pitch must be in 0.125..8");
    if (!std::isfinite(s.minDistance) || !std::isfinite(s.maxDistance) || s.minDistance < 0 ||
        s.maxDistance < s.minDistance)
        errors.push_back("AudioSource max distance must be at least min distance, both finite and nonnegative");
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
struct AudioMixer::Impl {
    enum class Op { Play, Stop, Update, Volume };
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
    };
    std::mutex commandsMutex;
    std::array<Command, kCommands> commands;
    size_t read = 0, count = 0;
    std::array<Voice, kVoices> voices;
    std::array<std::atomic<AudioVoice>, kVoices> playing{};
    std::atomic<AudioVoice> next{1};
    std::atomic<float> master{1};
    std::atomic<bool> paused{false};
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
    bool post(Command command) {
        if (!offline && !stream) return false;
        std::lock_guard lock(commandsMutex);
        if (count == kCommands) return false;
        commands[(read + count) % kCommands] = std::move(command);
        ++count;
        return true;
    }
    void consume() {
        // The callback never waits for a producer; commands can wait one audio block.
        std::array<Command, kCommands> pending;
        size_t size = 0;
        {
            std::unique_lock lock(commandsMutex, std::try_to_lock);
            if (!lock.owns_lock()) return;
            size = count;
            for (size_t i = 0; i < size; ++i) pending[i] = std::move(commands[(read + i) % kCommands]);
            read = (read + size) % kCommands;
            count = 0;
        }
        for (size_t i = 0; i < size; ++i) {
            auto& cmd = pending[i];
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
                voices[slot] = {cmd.id, std::move(cmd.clip), cmd.options, 0};
                playing[slot].store(cmd.id);
            } else if (slot != kVoices) {
                auto& voice = voices[slot];
                if (cmd.op == Op::Stop) {
                    voice.id = 0;
                    playing[slot].store(0);
                } else if (cmd.op == Op::Update) {
                    voice.clip = std::move(cmd.clip);
                    voice.options = cmd.options;
                } else
                    voice.options.volume = cmd.options.volume;
            }
        }
    }
    void mix(std::span<float> output) {
        consume();
        std::fill(output.begin(), output.end(), 0);
        if (paused.load()) return;
        const size_t frames = output.size() / 2;
        const float volume = master.load();
        for (size_t v = 0; v < kVoices; ++v) {
            auto& voice = voices[v];
            if (!voice.id || !voice.clip || !voice.clip->frames()) continue;
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
                if (p.spatial) value = glm::vec2((value.x + value.y) * .5f);
                output[f * 2] += value.x * gain.x;
                output[f * 2 + 1] += value.y * gain.y;
                voice.cursor += p.pitch;
            }
        }
        for (auto& sample : output) sample = std::clamp(sample, -1.0f, 1.0f);
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
    const auto id = m_impl->next.fetch_add(1);
    return m_impl->post({Impl::Op::Play, id, std::move(clip), p}) ? id : 0;
}
bool AudioMixer::stop(AudioVoice id) { return id && m_impl->post({Impl::Op::Stop, id, {}, {}}); }
bool AudioMixer::update(AudioVoice id, std::shared_ptr<const AudioClip> clip, AudioPlayback p) {
    return id && clip && clip->frames() && valid(p) && m_impl->post({Impl::Op::Update, id, std::move(clip), p});
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
struct AudioWorld::Impl {
    struct Source {
        AudioSource authored;
        AudioPlayback options;
        std::shared_ptr<const AudioClip> clip;
        AudioVoice voice = 0;
        float overrideVolume = -1;
        uint64_t seen = 0;
        bool started = false;
    };
    World& world;
    const Assets* assets;
    AudioMixer mixer;
    std::mutex mutex;
    std::unordered_map<uint64_t, Source> sources;
    uint64_t epoch = 0;
    Impl(World& w, const Assets* a, bool device) : world(w), assets(a), mixer(device && a) {}
};
AudioWorld::AudioWorld(World& world, const Assets* assets, bool device)
    : m_impl(std::make_unique<Impl>(world, assets, device)) {}
AudioWorld::~AudioWorld() = default;
AudioMixer& AudioWorld::mixer() { return m_impl->mixer; }
void AudioWorld::synchronize() {
    auto& p = *m_impl;
    if (!p.assets) return;
    glm::vec3 position{}, right{1, 0, 0};
    const auto camera = primaryCamera(p.world);
    const bool listener = camera.valid() && p.world.has<Transform>(camera);
    if (listener) {
        const auto matrix = worldMatrix(p.world, camera);
        position = matrix[3];
        right = glm::vec3(matrix[0]);
    }
    std::lock_guard lock(p.mutex);
    ++p.epoch;
    p.world.each<const AudioSource>([&](Entity entity, const AudioSource& source) {
        const auto id = key(entity);
        auto& state = p.sources[id];
        state.seen = p.epoch;
        auto clip = p.assets->audioClip(source.clip);
        glm::vec2 gains{1};
        if (source.spatial)
            gains = listener && p.world.has<Transform>(entity)
                        ? spatialAudioGains(
                              glm::vec3(worldMatrix(p.world, entity)[3]), position, right, source.minDistance,
                              source.maxDistance)
                        : glm::vec2(0);
        const auto oldOptions = state.options;
        const auto oldClip = state.clip;
        if (state.authored.volume != source.volume) state.overrideVolume = -1;
        state.authored = source;
        state.clip = clip;
        state.options = playback(source, gains);
        if (state.overrideVolume >= 0) state.options.volume = state.overrideVolume;
        if (!clip || !valid(state.options) || !std::isfinite(source.minDistance) ||
            !std::isfinite(source.maxDistance) || source.minDistance < 0 || source.maxDistance < source.minDistance) {
            if (!state.voice || p.mixer.stop(state.voice)) state.voice = 0;
            return;
        }
        if (!state.started && source.playOnStart) {
            state.voice = p.mixer.play(clip, state.options);
            state.started = state.voice != 0;
        } else if (
            state.voice && (oldClip != clip || oldOptions.volume != state.options.volume ||
                            oldOptions.pitch != state.options.pitch || oldOptions.loop != state.options.loop ||
                            oldOptions.spatial != state.options.spatial || oldOptions.gains != state.options.gains))
            if (!p.mixer.update(state.voice, clip, state.options)) {
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
}   // namespace shockwave
