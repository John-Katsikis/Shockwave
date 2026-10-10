#include <Audio/Audio.h>

#include <Assets/Assets.h>
#include <Assets/AssetFolder.h>
#include <Engine/Components.h>
#include <Engine/SceneFile.h>
#include <GameFramework/GameInstance.h>
#include <GameFramework/PlaySession.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace shockwave;
namespace fs = std::filesystem;
namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool near(float a, float b) { return std::abs(a - b) < 1e-5f; }
std::shared_ptr<AudioClip> constant(float left = .2f, float right = .4f, size_t frames = 64) {
    auto clip = std::make_shared<AudioClip>();
    clip->samples.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        clip->samples[i * 2] = left;
        clip->samples[i * 2 + 1] = right;
    }
    return clip;
}
void loading(const fs::path& root) {
    AudioClip wav, ogg;
    std::string error;
    check(loadAudioClip(root / "Games/Sample/Content/Audio/pulse.wav", wav, error), "WAV loads");
    check(wav.frames() == 8640 && near(float(wav.duration()), .18f), "WAV duration/sample conversion");
    check(loadAudioClip(root / "Tests/fixtures/audio/pulse.ogg", ogg, error), "Ogg loads");
    check(std::abs(double(ogg.frames()) - wav.frames()) < 64, "Ogg duration");
    check(
        std::all_of(ogg.samples.begin(), ogg.samples.end(), [](float f) { return std::isfinite(f); }),
        "Ogg finite PCM");
    float energy = 0;
    for (float sample : ogg.samples) energy += sample * sample;
    check(energy > 1, "Ogg is audible PCM");
    check(
        !loadAudioClip(root / "Tests/fixtures/audio/missing.wav", wav, error) && !error.empty(), "missing clip fails");
    const auto bad = fs::temp_directory_path() / "shockwave-audio-invalid.ogg";
    {
        std::ofstream file(bad);
        file << "not an ogg";
    }
    check(!loadAudioClip(bad, ogg, error), "corrupt Ogg rejected");
    fs::remove(bad);
    check(isAudioFile("SOUND.WAV") && isAudioFile("sound.Ogg") && !isAudioFile("sound.mp3"), "extensions");
}
void reload(const fs::path& root) {
    const auto folder = fs::temp_directory_path() / "shockwave-audio-reload";
    fs::remove_all(folder);
    fs::create_directories(folder);
    const auto path = folder / "tone.wav";
    fs::copy_file(root / "Games/Sample/Content/Audio/pulse.wav", path);
    Assets assets;
    JobSystem jobs(2);
    AssetFolder files(assets, jobs);
    const auto first = files.open(folder);
    check(first.size() == 1 && assets.audioClip("tone.wav"), "audio-only asset tools load clips");
    const auto before = assets.audioClip("tone.wav");
    fs::copy_file(root / "Games/Sample/Content/Audio/ambient.wav", path, fs::copy_options::overwrite_existing);
    files.refresh();
    check(assets.audioClip("tone.wav") == before, "reload keeps old PCM while decoding");
    for (int i = 0; i < 200 && assets.audioClip("tone.wav") == before; ++i) {
        SDL_Delay(5);
        files.refresh();
    }
    const auto replaced = assets.audioClip("tone.wav");
    check(replaced != before && replaced->duration() > 1, "completed decode replaces clip atomically");
    {
        std::ofstream bad(path, std::ios::trunc);
        bad << "bad wav";
    }
    files.refresh();
    for (int i = 0; i < 200 && files.find("tone.wav")->error.empty(); ++i) {
        SDL_Delay(5);
        files.refresh();
    }
    check(
        !files.find("tone.wav")->error.empty() && assets.audioClip("tone.wav") == replaced,
        "failed reload retains last-good clip");
    fs::remove(path);
    files.refresh();
    check(!assets.audioClip("tone.wav"), "removed clip invalidates its asset name");
    fs::remove_all(folder);
}

void roundTrip() {
    registerAudioComponents();
    Assets assets;
    assets.addAudioClip("Audio/test.wav", constant());
    World world;
    const auto entity = world.create();
    world.add<Name>(entity, {"Sound"});
    AudioSource authored{"Audio/test.wav", .35f, 1.5f, true, false, true, 2, 12};
    world.add<AudioSource>(entity, authored);
    const auto path = fs::temp_directory_path() / "shockwave-audio-roundtrip.swscene";
    std::string error;
    check(saveScene(world, assets, path.string(), error), "save audio scene");
    World restored;
    check(loadScene(restored, assets, path.string(), error), "load audio scene");
    const auto* source = restored.get<AudioSource>(restored.entityAt(0));
    check(
        source && source->clip == authored.clip && near(source->volume, .35f) && near(source->pitch, 1.5f) &&
            source->loop && !source->playOnStart && source->spatial && near(source->minDistance, 2) &&
            near(source->maxDistance, 12),
        "all AudioSource fields round-trip");
    std::ostringstream schema;
    writeSceneSchema(schema, assets);
    check(
        schema.str().find("AudioSource") != std::string::npos &&
            schema.str().find("Audio/test.wav") != std::string::npos,
        "schema lists audio fields/assets");
    fs::remove(path);
    check(validateAudioSource(authored, assets).empty(), "valid source passes checker");
    auto bad = authored;
    bad.clip = "missing.wav";
    bad.volume = 1.1f;
    bad.maxDistance = 1;
    const auto problems = validateAudioSource(bad, assets);
    check(problems.size() == 3, "missing clip, volume and reversed distances flagged by --check validator");
    bad = authored;
    bad.pitch = 0;
    check(validateAudioSource(bad, assets).size() == 1, "zero pitch rejected");
    bad = authored;
    bad.volume = std::numeric_limits<float>::quiet_NaN();
    check(!validateAudioSource(bad, assets).empty(), "NaN rejected");
}
void spatial() {
    auto gains = spatialAudioGains({0, 0, -1}, {}, {1, 0, 0}, 1, 11);
    check(near(gains.x, std::sqrt(.5f)) && near(gains.y, gains.x), "equal power center");
    gains = spatialAudioGains({6, 0, 0}, {}, {1, 0, 0}, 1, 11);
    check(near(gains.x, 0) && near(gains.y, .5f), "right pan and half-distance attenuation");
    gains = spatialAudioGains({-1, 0, 0}, {}, {1, 0, 0}, 1, 11);
    check(near(gains.x, 1) && near(gains.y, 0), "left pan");
    check(spatialAudioGains({12, 0, 0}, {}, {1, 0, 0}, 1, 11) == glm::vec2(0), "outside max distance silent");
    check(spatialAudioGains({2, 0, 0}, {}, {1, 0, 0}, 1, 1) == glm::vec2(0), "equal distances handled");
    gains = spatialAudioGains({6, 0, 0}, {}, {-1, 0, 0}, 1, 11);
    check(near(gains.x, .5f) && near(gains.y, 0), "listener rotation reverses pan");
}
void mixing() {
    AudioMixer mixer(false);
    std::array<float, 16> buffer{};
    auto clip = constant();
    const auto voice = mixer.play(clip, {.volume = .5f, .loop = true});
    check(voice != 0, "play queued");
    mixer.render(buffer);
    check(
        mixer.isPlaying(voice) && near(buffer[0], .1f) && near(buffer[1], .2f),
        "stereo mix preserves channels and gain");
    mixer.pause(true);
    mixer.render(buffer);
    check(buffer[0] == 0, "pause is silent");
    mixer.pause(false);
    check(mixer.setVolume(voice, .25f), "volume command accepted");
    mixer.setMasterVolume(.5f);
    mixer.render(buffer);
    check(near(buffer[0], .025f), "voice and master volume multiply");
    check(mixer.stop(voice), "stop queued");
    mixer.render(buffer);
    check(!mixer.isPlaying(voice) && buffer[0] == 0, "stop consumes voice");
    auto ramp = std::make_shared<AudioClip>();
    for (int i = 0; i < 32; ++i) {
        ramp->samples.push_back(float(i) / 32);
        ramp->samples.push_back(float(i) / 32);
    }
    mixer.setMasterVolume(1);
    const auto pitch = mixer.play(ramp, {.pitch = 2});
    mixer.render(buffer);
    check(near(buffer[2], 2.0f / 32), "pitch changes sample progression");
    mixer.render(buffer);
    mixer.render(buffer);
    check(!mixer.isPlaying(pitch), "one shot completes");
    const auto loop = mixer.play(constant(.2f, .4f, 2), {.loop = true, .spatial = true, .gains = {0, 1}});
    mixer.render(buffer);
    check(near(buffer[0], 0) && near(buffer[1], .3f) && mixer.isPlaying(loop), "spatial stereo downmix and looping");
    auto replacement = constant(.8f, .8f, 2);
    mixer.update(loop, replacement, {.loop = true});
    mixer.render(buffer);
    check(near(buffer[0], .8f), "active voice hot reload");
    mixer.stop(loop);
    mixer.render(buffer);
    check(
        !mixer.play(nullptr) && !mixer.play(clip, {.pitch = 0}) && !mixer.setVolume(voice, -1),
        "invalid transport rejected");
    std::array<std::thread, 4> producers;
    for (auto& thread : producers)
        thread = std::thread([&] {
            for (int i = 0; i < 60; ++i) {
                auto id = mixer.play(clip);
                mixer.setVolume(id, .5f);
                mixer.stop(id);
            }
        });
    for (auto& thread : producers) thread.join();
    mixer.render(buffer);
    check(
        std::all_of(buffer.begin(), buffer.end(), [](float f) { return std::isfinite(f); }),
        "worker commands mix safely");
}
void worldAudio() {
    Assets assets;
    assets.addAudioClip("test.wav", constant(.4f, .4f));
    World world;
    const auto camera = world.create();
    world.add<Transform>(camera);
    world.add<Camera>(camera);
    const auto source = world.create();
    world.add<Transform>(source, {{1, 0, 0}});
    world.add<AudioSource>(source, {.clip = "test.wav", .loop = true});
    AudioWorld audio(world, &assets, false);
    std::array<float, 16> buffer{};
    audio.synchronize();
    audio.mixer().render(buffer);
    check(near(buffer[0], 0) && near(buffer[1], .4f), "listener follows primary Camera");
    check(audio.setVolume(source, .5f), "world volume command");
    audio.synchronize();
    audio.mixer().render(buffer);
    check(near(buffer[1], .2f), "runtime volume survives synchronization");
    world.get<Transform>(camera)->position = {2, 0, 0};
    audio.synchronize();
    audio.mixer().render(buffer);
    check(near(buffer[0], .2f) && near(buffer[1], 0), "moving listener updates pan");
    audio.stop(source);
    audio.synchronize();
    audio.mixer().render(buffer);
    check(buffer[0] == 0, "stop does not replay play-on-start");
    check(audio.play(source), "source can replay");
    audio.mixer().render(buffer);
    check(buffer[0] > 0, "replay audible");
    for (int i = 0; i < 1024; ++i) check(audio.mixer().setVolume(999999, .5f), "queue accepts bounded commands");
    check(!audio.stop(source), "full queue reports failure without losing source handle");
    audio.mixer().render(buffer);
    check(audio.stop(source), "stop retries after queue drains");
    audio.mixer().render(buffer);
    check(buffer[0] == 0, "retried stop silences looping source");
    check(audio.play(source), "source replays after queue pressure");
    audio.mixer().render(buffer);
    world.destroy(source);
    audio.synchronize();
    audio.mixer().render(buffer);
    check(buffer[0] == 0, "destroyed source stops");
    JobSystem jobs(2);
    SimpleGameInstance<GameMode> game;
    PlaySession session(world, jobs, game);
    session.start();
    check(session.audio() != nullptr, "Play owns audio subsystem");
    session.pause();
    session.start();
    session.stop();
    check(session.audio() == nullptr, "Stop releases audio subsystem");
}
void device() {
    AudioMixer mixer;
    check(mixer.available(), "dummy device opens");
    const auto voice = mixer.play(constant(), {.loop = true});
    for (int i = 0; i < 200 && !mixer.isPlaying(voice); ++i) SDL_Delay(5);
    check(mixer.isPlaying(voice), "SDL callback consumes commands off main thread");
    mixer.stop(voice);
    for (int i = 0; i < 200 && mixer.isPlaying(voice); ++i) SDL_Delay(5);
    check(!mixer.isPlaying(voice), "SDL callback stops voice");
}
}   // namespace
int main(int argc, char** argv) {
    try {
        check(argc == 2, "source root argument");
        loading(argv[1]);
        reload(argv[1]);
        roundTrip();
        spatial();
        mixing();
        worldAudio();
        device();
        std::cout << "Audio tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Audio test: " << e.what() << '\n';
        return 1;
    }
}
