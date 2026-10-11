// player --audio-mix: hear a scene without speakers. It plays the scene like the game
// does, mixes offline (the same mixer, no audio device), writes a 16-bit stereo WAV and
// prints levels per channel, so a tool or an agent can check where sounds sit.
#include "Player.h"

#include <Audio/Audio.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/SceneFile.h>
#include <GameFramework/PlaySession.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <span>
#include <vector>

namespace player {
namespace {
constexpr int kRate = 48000;

bool writeWav(const fs::path& path, std::span<const float> stereo) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = uint32_t(stereo.size() * 2);
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);   // PCM
    u16(2);
    u32(kRate);
    u32(kRate * 4);
    u16(4);
    u16(16);
    out.write("data", 4);
    u32(bytes);
    std::vector<int16_t> samples(stereo.size());
    for (size_t i = 0; i < stereo.size(); ++i)
        samples[i] = int16_t(std::lround(std::clamp(stereo[i], -1.0f, 1.0f) * 32767.0f));
    out.write(reinterpret_cast<const char*>(samples.data()), std::streamsize(samples.size() * 2));
    return bool(out);
}
double decibels(double rms) { return rms > 1e-9 ? 20.0 * std::log10(rms) : -180.0; }
}   // namespace

int Player::audioMix() {
    const fs::path scenePath = m_tool.scene.empty() ? m_project.startupScene : m_tool.scene;
    std::string error;
    std::vector<std::string> warnings;
    shockwave::SceneFileHooks hooks;
    hooks.readHeader = [](const std::string&, std::istream&) {};
    if (!shockwave::loadScene(m_world, engine().assets(), scenePath.string(), error, hooks, &warnings)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    for (const auto& warning : warnings) std::printf("warning: %s\n", warning.c_str());
    for (const auto& problem : shockwave::checkAudioScene(m_world, engine().assets()))
        std::printf("problem: %s\n", problem.c_str());
    auto name = [&](shockwave::Entity e) {
        const auto* n = m_world.get<shockwave::Name>(e);
        return std::string(n ? n->value : "?");
    };

    shockwave::PlaySession session(m_world, engine().jobs(), *m_instance, &engine());
    session.setAudioDevice(false);
    session.start();
    shockwave::AudioWorld& audio = *session.audio();
    const double tick = 1.0 / engine().tickRate();
    const size_t total = size_t(std::llround(m_tool.audioSeconds * kRate));
    std::vector<float> mix(total * 2);
    size_t done = 0;
    double clock = 0, mixSeconds = 0;
    int peakSpatial = 0, peakVoices = 0;
    while (done < total) {
        session.update(tick);
        session.frame(tick);
        clock += tick;
        const size_t until = std::min(total, size_t(std::llround(clock * kRate)));
        const auto start = std::chrono::steady_clock::now();
        audio.mixer().render(std::span(mix.data() + done * 2, (until - done) * 2));
        mixSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        peakVoices = std::max(peakVoices, audio.mixer().stats().voices);
        peakSpatial = std::max(peakSpatial, audio.mixer().stats().spatialVoices);
        done = until;
    }
    const auto sources = audio.report();
    const shockwave::Entity listener = audio.listener();
    const shockwave::AudioMixerStats stats = audio.mixer().stats();
    std::printf(
        "audio mix: %s, %.2f s, %d Hz stereo, %s\n", m_project.relative(scenePath).c_str(), m_tool.audioSeconds, kRate,
        shockwave::spatialBackendName().c_str());
    if (listener.valid()) {
        const glm::vec3 at(shockwave::worldMatrix(m_world, listener)[3]);
        std::printf(
            "listener: '%s' (%s) at %.2f %.2f %.2f\n", name(listener).c_str(),
            m_world.has<shockwave::AudioListener>(listener) ? "AudioListener" : "primary Camera",
            static_cast<double>(at.x), static_cast<double>(at.y), static_cast<double>(at.z));
    } else {
        std::printf("listener: none; spatial sounds are silent\n");
    }
    for (const auto& s : sources) {
        const std::string who = s.entity.valid() ? "'" + name(s.entity) + "'" : "one-off";
        if (!s.spatial) {
            std::printf(
                "source %s (%s): not spatial, %s\n", who.c_str(), s.clip.c_str(), s.playing ? "playing" : "stopped");
            continue;
        }
        const auto& h = s.heard;
        const double azimuth = glm::degrees(std::atan2(h.direction.x, -h.direction.z));
        const double elevation = glm::degrees(std::asin(std::clamp(h.direction.y, -1.0f, 1.0f)));
        std::printf(
            "source %s (%s): %.2f m, azimuth %+.0f deg (%s), elevation %+.0f deg, gain %.3f, air high %.3f, "
            "occlusion %.2f, %s\n",
            who.c_str(), s.clip.c_str(), static_cast<double>(h.distance), azimuth,
            std::abs(azimuth) < 5     ? "ahead"
            : std::abs(azimuth) > 175 ? "behind"
            : azimuth > 0             ? "right"
                                      : "left",
            elevation, static_cast<double>(h.distanceGain * h.directivity), static_cast<double>(h.air[2]),
            static_cast<double>(h.occlusion), s.playing ? "playing" : "stopped");
    }
    session.stop();

    double sum[2] = {0, 0}, peak[2] = {0, 0};
    for (size_t i = 0; i < total; ++i)
        for (int c = 0; c < 2; ++c) {
            const double v = mix[i * 2 + c];
            sum[c] += v * v;
            peak[c] = std::max(peak[c], std::abs(v));
        }
    const double rms[2] = {
        std::sqrt(sum[0] / double(std::max<size_t>(total, 1))), std::sqrt(sum[1] / double(std::max<size_t>(total, 1)))};
    std::printf("channel  rms       peak      rms dBFS\n");
    const char* channels[2] = {"left ", "right"};
    for (int c = 0; c < 2; ++c)
        std::printf("%s    %.5f   %.5f   %.1f\n", channels[c], rms[c], peak[c], decibels(rms[c]));
    const double balance = decibels(rms[1]) - decibels(rms[0]);
    if (rms[0] < 1e-6 && rms[1] < 1e-6)
        std::printf("balance: silent\n");
    else if (std::abs(balance) < 1)
        std::printf("balance: centered (%+.1f dB right)\n", balance);
    else
        std::printf("balance: %s louder by %.1f dB\n", balance > 0 ? "right" : "left", std::abs(balance));
    const double blocks = double(total) / 256.0;
    std::printf(
        "mix cost: %.1f us per 256-frame block (%.2f%% of one core), peak %.1f us; up to %d voices, %d through "
        "Steam Audio\n",
        mixSeconds * 1e6 / blocks, mixSeconds / m_tool.audioSeconds * 100.0,
        static_cast<double>(stats.peakMicroseconds), peakVoices, peakSpatial);
    if (!writeWav(m_tool.audioMix, mix)) {
        std::fprintf(stderr, "Player: can't write %s\n", m_tool.audioMix.string().c_str());
        return 1;
    }
    std::printf("Wrote %s\n", m_tool.audioMix.string().c_str());
    return 0;
}

}   // namespace player
