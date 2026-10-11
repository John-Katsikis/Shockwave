// Spatial audio without a GPU or speakers: the distance, air, directivity and occlusion
// math, the checker's rules, Steam Audio rendering (or the simple panner when libphonon
// is missing), occlusion rays, one-off 3D sounds, listeners, and the cost per voice.
// Run as `spatial_audio_test steam` (libphonon must load) or `spatial_audio_test simple`
// (SHOCKWAVE_PHONON=none: it must not).
#include <Audio/Audio.h>

#include <Assets/Assets.h>
#include <Engine/Components.h>
#include <Engine/SceneFile.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace shockwave;
namespace fs = std::filesystem;
namespace {
void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
bool near(float a, float b, float tolerance = 1e-4f) { return std::abs(a - b) < tolerance; }
bool contains(const std::vector<std::string>& problems, const std::string& text) {
    return std::any_of(
        problems.begin(), problems.end(), [&](const std::string& p) { return p.find(text) != std::string::npos; });
}
// A second of broadband noise: HRTFs and filters need more than a constant to show.
std::shared_ptr<AudioClip> noise(size_t frames = 48000) {
    auto clip = std::make_shared<AudioClip>();
    clip->samples.resize(frames * 2);
    uint32_t state = 12345;
    for (size_t i = 0; i < frames; ++i) {
        state = state * 1664525u + 1013904223u;
        const float value = float(state >> 8) / float(1 << 24) * 2 - 1;
        clip->samples[i * 2] = clip->samples[i * 2 + 1] = value * .5f;
    }
    return clip;
}
struct Energy {
    double left = 0, right = 0;
};
Energy render(AudioMixer& mixer, size_t frames) {
    std::vector<float> buffer(frames * 2);
    mixer.render(buffer);
    Energy e;
    for (size_t i = 0; i < frames; ++i) {
        e.left += double(buffer[i * 2]) * buffer[i * 2];
        e.right += double(buffer[i * 2 + 1]) * buffer[i * 2 + 1];
    }
    return e;
}
AudioPlayback at(glm::vec3 direction, float distance = 2) {
    AudioSource source;
    AudioListenerPose listener;
    AudioPlayback p;
    p.loop = true;
    p.spatial = true;
    p.positional = true;
    p.position = spatialize(listener, glm::normalize(direction) * distance, {0, 0, -1}, source);
    p.gains = simplePanGains(p.position);
    return p;
}

void math() {
    AudioSource s;
    check(near(audioRolloffGain(AudioRolloff::Linear, 15.5f, 1, 30), .5f), "linear rolloff halfway");
    check(audioRolloffGain(AudioRolloff::Linear, .5f, 1, 30) == 1, "full volume inside min distance");
    check(audioRolloffGain(AudioRolloff::Linear, 31, 1, 30) == 0, "silent past max distance");
    check(near(audioRolloffGain(AudioRolloff::Inverse, 2, 1, 1e6f), .5f, 1e-3f), "inverse rolloff halves at 2x min");
    check(audioRolloffGain(AudioRolloff::Inverse, 29.9f, 1, 30) < .01f, "inverse rolloff reaches silence at max");
    check(audioRolloffGain(AudioRolloff::None, 29, 1, 30) == 1, "no rolloff keeps full volume");
    check(audioRolloffGain(AudioRolloff::Linear, 5, 10, 2) == 0, "reversed distances are silent");

    // The listener looks down -Z; sounds to its right come out on +X in listener space.
    AudioListenerPose listener;
    auto heard = spatialize(listener, {3, 0, 0}, {0, 0, -1}, s);
    check(near(heard.direction.x, 1) && near(heard.distance, 3), "right of the listener");
    heard = spatialize(listener, {0, 0, -4}, {0, 0, -1}, s);
    check(near(heard.direction.z, -1), "ahead is -Z");
    // Turned to face +X (yaw -90 degrees), the same sound is now ahead.
    listener.right = {0, 0, 1};
    listener.ahead = {1, 0, 0};
    heard = spatialize(listener, {3, 0, 0}, {0, 0, -1}, s);
    check(near(heard.direction.z, -1) && near(heard.direction.x, 0), "listener rotation");
    const glm::mat4 turned =
        glm::mat4(glm::vec4(0, 0, 1, 0), glm::vec4(0, 1, 0, 0), glm::vec4(-1, 0, 0, 0), glm::vec4(5, 0, 0, 1));
    const auto pose = listenerPose(turned);
    check(near(pose.ahead.x, 1) && near(pose.right.z, 1) && near(pose.position.x, 5), "pose from a world matrix");

    listener = {};
    const auto nearSound = spatialize(listener, {0, 0, -2}, {0, 0, 1}, s);
    const auto farSound = spatialize(listener, {0, 0, -25}, {0, 0, 1}, s);
    check(farSound.air[2] < nearSound.air[2] && farSound.air[2] < farSound.air[0], "air absorbs highs with distance");
    s.airAbsorption = false;
    check(spatialize(listener, {0, 0, -25}, {0, 0, 1}, s).air[2] == 1, "air absorption can be turned off");

    s.directivity = 1;
    check(near(spatialize(listener, {0, 0, -5}, {0, 0, 1}, s).directivity, 1), "facing the listener: full");
    check(spatialize(listener, {0, 0, -5}, {0, 0, -1}, s).directivity < 1e-4f, "facing away: silent dipole");
    s.directivity = .5f;
    s.directivityPower = 2;
    check(near(spatialize(listener, {0, 0, -5}, {0, 0, -1}, s).directivity, 0), "cardioid null behind");
    s.directivity = 0;

    s.occlusion = true;
    s.transmission = .2f;
    const auto blocked = spatialize(listener, {4, 0, 0}, {0, 0, -1}, s, 0);
    check(
        blocked.occlusion == 0 && near(blocked.transmission[0], .2f) && blocked.transmission[2] < .1f,
        "transmission lets lows through");
    const auto open = spatialize(listener, {4, 0, 0}, {0, 0, -1}, s, 1);
    check(simplePanGains(blocked).y < simplePanGains(open).y * .2f, "occlusion muffles the simple panner too");
    // With nothing in the way the simple panner is exactly the classic panner.
    s = {.maxDistance = 11};
    const auto classic = spatialAudioGains({6, 0, 0}, {}, {1, 0, 0}, 1, 11);
    const auto simple = simplePanGains(spatialize({}, {6, 0, 0}, {0, 0, -1}, s));
    check(near(classic.x, simple.x) && near(classic.y, simple.y), "simple panner matches spatialAudioGains");
}

void checks(Assets& assets) {
    AudioSource s{.clip = "noise.wav"};
    check(validateAudioSource(s, assets).empty(), "default source is valid");
    s.rolloff = AudioRolloff::Inverse;
    s.minDistance = 0;
    check(contains(validateAudioSource(s, assets), "inverse rolloff needs"), "inverse rolloff with min 0");
    s = {.clip = "noise.wav"};
    s.directivity = 2;
    s.transmission = -1;
    s.occlusionRadius = 20;
    const auto problems = validateAudioSource(s, assets);
    check(problems.size() == 3, "directivity, transmission and occlusion radius ranges");

    World world;
    const auto floating = world.create();
    world.add<Name>(floating, {"Floating"});
    world.add<AudioSource>(floating, {.clip = "noise.wav"});
    const auto flat = world.create();
    world.add<Name>(flat, {"Flat"});
    world.add<AudioSource>(flat, {.clip = "noise.wav", .spatial = false, .occlusion = true});
    auto found = checkAudioScene(world, assets);
    check(contains(found, "entity 'Floating': spatial AudioSource has no Transform"), "spatial source needs Transform");
    check(contains(found, "entity 'Flat': AudioSource occlusion only works"), "occlusion on a nonspatial source");
    check(contains(found, "no AudioListener or primary Camera"), "spatial sound with no listener");
    const auto ear = world.create();
    world.add<Name>(ear, {"Ear"});
    world.add<AudioListener>(ear);
    const auto ear2 = world.create();
    world.add<Name>(ear2, {"Ear 2"});
    world.add<Transform>(ear2);
    world.add<AudioListener>(ear2);
    found = checkAudioScene(world, assets);
    check(contains(found, "entity 'Ear': AudioListener has no Transform"), "listener needs Transform");
    check(contains(found, "entity 'Ear 2': more than one AudioListener"), "one listener per scene");
    check(!contains(found, "no AudioListener"), "a listener with a Transform hears");
    check(audioListener(world) == ear2, "the AudioListener with a Transform listens");

    // Every field and the listener survive a save and load.
    registerAudioComponents();
    World scene;
    const auto e = scene.create();
    scene.add<Name>(e, {"Hum"});
    scene.add<Transform>(e);
    scene.add<AudioSource>(
        e, {.clip = "noise.wav",
            .rolloff = AudioRolloff::Inverse,
            .airAbsorption = false,
            .directivity = .5f,
            .directivityPower = 2,
            .occlusion = true,
            .occlusionRadius = .25f,
            .transmission = .3f});
    scene.add<AudioListener>(e);
    const auto path = fs::temp_directory_path() / "shockwave-spatial-audio.swscene";
    std::string error;
    check(saveScene(scene, assets, path.string(), error), "save spatial scene: " + error);
    World loaded;
    check(loadScene(loaded, assets, path.string(), error), "load spatial scene: " + error);
    fs::remove(path);
    const auto* back = loaded.get<AudioSource>(loaded.entityAt(0));
    check(
        back && back->rolloff == AudioRolloff::Inverse && !back->airAbsorption && near(back->directivity, .5f) &&
            near(back->directivityPower, 2) && back->occlusion && near(back->occlusionRadius, .25f) &&
            near(back->transmission, .3f) && loaded.has<AudioListener>(loaded.entityAt(0)),
        "spatial fields and AudioListener round-trip");
}

void rendering(bool expectSteam) {
    check(steamAudioAvailable() == expectSteam, "Steam Audio available: " + steamAudioStatus());
    std::printf("backend: %s (%s)\n", spatialBackendName().c_str(), steamAudioStatus().c_str());
    const auto clip = noise();
    for (const bool hrtf : {true, false}) {
        setAudioSettings({.hrtf = hrtf});
        AudioMixer mixer(false);
        const auto right = mixer.play(clip, at({1, 0, 0}));
        const auto e1 = render(mixer, 9600);
        check(
            e1.right > e1.left * 4, std::string(hrtf ? "HRTF" : "panning") + ": a sound on the right is louder right");
        if (expectSteam) check(mixer.stats().spatialVoices == 1, "the voice took a Steam Audio slot");
        mixer.stop(right);
        render(mixer, 4800);   // let the bus drain
        mixer.play(clip, at({-1, 0, 0}));
        const auto e2 = render(mixer, 9600);
        check(e2.left > e2.right * 4, "a sound on the left is louder left");
    }
    setAudioSettings({});
    // Occlusion with little transmission takes most of the energy away.
    AudioMixer mixer(false);
    auto open = at({0, 0, -1});
    const auto voice = mixer.play(clip, open);
    render(mixer, 4800);
    const auto loud = render(mixer, 9600);
    AudioSource walled{.occlusion = true, .transmission = .05f};
    auto blocked = spatialize({}, {0, 0, -2}, {0, 0, -1}, walled, 0);
    mixer.place(voice, simplePanGains(blocked), blocked);
    render(mixer, 4800);
    const auto quiet = render(mixer, 9600);
    check(quiet.left + quiet.right < (loud.left + loud.right) * .1, "occlusion muffles the voice");
    // The simple panner, forced: hard right is right only.
    setAudioSettings({.steamAudio = false});
    AudioMixer simple(false);
    simple.play(clip, at({1, 0, 0}));
    const auto e3 = render(simple, 4800);
    check(e3.left < 1e-9 && e3.right > 0 && simple.stats().spatialVoices == 0, "simple panner fallback");
    setAudioSettings({});
}

void cost() {
    // Mixing cost per voice: 32 looping voices around the listener, 200 blocks.
    const auto clip = noise();
    struct Mode {
        const char* name;
        AudioSettings settings;
    } modes[] = {
        {"HRTF (high)", {}},
        {"HRTF (low)", {.hrtfBilinear = false}},
        {"speaker panning", {.hrtf = false}},
        {"simple panning", {.steamAudio = false}},
    };
    constexpr int kVoices = 32, kBlocks = 200;
    std::vector<float> block(256 * 2);
    for (const auto& mode : modes) {
        if (mode.settings.steamAudio && !steamAudioAvailable()) continue;
        setAudioSettings(mode.settings);
        AudioMixer mixer(false);
        for (int v = 0; v < kVoices; ++v) {
            const float angle = 6.2831853f * float(v) / kVoices;
            mixer.play(clip, at({std::cos(angle), .2f, std::sin(angle)}, 3));
        }
        mixer.render(block);   // claims slots
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kBlocks; ++i) mixer.render(block);
        const double us =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kBlocks;
        std::printf(
            "cost %-16s %7.1f us per 256-frame block for %d voices = %.2f us per voice (%.3f%% of a core each)\n",
            mode.name, us, kVoices, us / kVoices, us / kVoices / (256.0 / 48000.0 * 1e6) * 100);
    }
    setAudioSettings({});
}

void world(Assets& assets) {
    World world;
    const auto camera = world.create();
    world.add<Transform>(camera);
    world.add<Camera>(camera);
    const auto wall = world.create();   // stands in for a collider; the fake raycast hits it
    const auto source = world.create();
    world.add<Transform>(source, {{0, 0, -5}});
    world.add<AudioSource>(source, {.clip = "noise.wav", .loop = true, .occlusion = true, .occlusionRadius = 0});
    AudioWorld audio(world, &assets, false);
    bool wallUp = false;
    int rays = 0;
    audio.setRaycast([&](glm::vec3, glm::vec3, float distance, Entity) -> std::optional<AudioRayHit> {
        ++rays;
        if (wallUp) return AudioRayHit{wall, distance * .5f};
        return std::nullopt;
    });
    audio.synchronize();
    check(audio.listener() == camera, "the primary Camera listens by default");
    for (int i = 0; i < 10; ++i) audio.update(1.0 / 60), audio.synchronize();
    auto report = audio.report();
    check(report.size() == 1 && near(report[0].heard.occlusion, 1) && rays == 10, "open path, one ray per tick");
    wallUp = true;
    for (int i = 0; i < 60; ++i) audio.update(1.0 / 60), audio.synchronize();
    report = audio.report();
    check(report[0].heard.occlusion < .01f, "a wall fades the sound out over ticks");
    // The listener's own body never blocks it.
    audio.setRaycast([&](glm::vec3, glm::vec3, float distance, Entity) -> std::optional<AudioRayHit> {
        return AudioRayHit{camera, distance};
    });
    for (int i = 0; i < 60; ++i) audio.update(1.0 / 60), audio.synchronize();
    check(audio.report()[0].heard.occlusion > .99f, "the listener's own collider doesn't occlude");

    // An AudioListener takes over from the camera.
    const auto ear = world.create();
    world.add<Transform>(ear, {{0, 0, -10}});
    world.add<AudioListener>(ear);
    audio.synchronize();
    check(audio.listener() == ear && near(audio.report()[0].heard.distance, 5), "AudioListener wins over the camera");
    world.destroy(ear);

    // One-off 3D sounds: at a point, and following an entity until it's gone.
    setAudioSettings({.steamAudio = false});   // exact left/right checks
    world.destroy(source);
    audio.synchronize();
    render(audio.mixer(), 512);
    const auto boom = audio.playAt("noise.wav", {4, 0, 0});
    check(boom != 0, "playAt plays");
    auto e = render(audio.mixer(), 2048);
    check(e.right > 0 && e.left < 1e-9, "playAt is heard from its position");
    audio.stopVoice(boom);
    const auto car = world.create();
    world.add<Transform>(car, {{4, 0, 0}});
    const auto engine = audio.playOn("noise.wav", EntityRef(world, car), {.loop = true});
    check(engine != 0, "playOn plays");
    audio.synchronize();
    render(audio.mixer(), 512);
    world.get<Transform>(car)->position = {-4, 0, 0};
    audio.synchronize();
    e = render(audio.mixer(), 2048);
    check(e.left > 0 && e.right < 1e-9, "playOn follows its entity");
    world.destroy(car);
    audio.synchronize();
    render(audio.mixer(), 512);
    check(!audio.mixer().isPlaying(engine), "a looping playOn stops with its entity");
    check(
        !audio.playAt("missing.wav", {}) && !audio.playAt("noise.wav", {}, {.minDistance = 5, .maxDistance = 1}),
        "playAt rejects missing clips and bad settings");
    setAudioSettings({});
}
}   // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "usage: spatial_audio_test steam|simple");
        Assets assets;
        assets.addAudioClip("noise.wav", noise());
        math();
        checks(assets);
        rendering(std::string(argv[1]) == "steam");
        world(assets);
        cost();
        std::cout << "Spatial audio tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Spatial audio test: " << e.what() << '\n';
        return 1;
    }
}
