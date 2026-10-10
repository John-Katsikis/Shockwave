#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace shockwave {
// Immutable once published in Assets. Interleaved stereo float PCM at 48 kHz.
struct AudioClip {
    static constexpr int sampleRate = 48000;
    std::vector<float> samples;
    size_t frames() const { return samples.size() / 2; }
    double duration() const { return double(frames()) / sampleRate; }
};
bool isAudioFile(const std::filesystem::path& path);
bool loadAudioClip(const std::filesystem::path& path, AudioClip& clip, std::string& error);
}   // namespace shockwave
