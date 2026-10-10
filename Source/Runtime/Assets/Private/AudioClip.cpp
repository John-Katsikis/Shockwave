#include <Assets/AudioClip.h>

#include <SDL3/SDL.h>
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace shockwave {
namespace {
std::string extension(const std::filesystem::path& path) {
    auto value = path.extension().string();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}
struct Free {
    void operator()(void* p) const { SDL_free(p); }
};
}   // namespace
bool isAudioFile(const std::filesystem::path& path) {
    const auto ext = extension(path);
    return ext == ".wav" || ext == ".ogg";
}
bool loadAudioClip(const std::filesystem::path& path, AudioClip& clip, std::string& error) {
    clip = {};
    error.clear();
    // These are resident effects/ambient clips, not a streaming music decoder.
    constexpr size_t maxBytes = 256 * 1024 * 1024;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > maxBytes) {
        error = "audio file is missing or exceeds 256 MiB";
        return false;
    }
    SDL_AudioSpec src{};
    std::vector<float> decoded;
    std::unique_ptr<Uint8, Free> wav;
    const Uint8* data = nullptr;
    int bytes = 0;
    if (extension(path) == ".wav") {
        Uint8* buffer = nullptr;
        Uint32 length = 0;
        if (!SDL_LoadWAV(path.string().c_str(), &src, &buffer, &length)) {
            error = SDL_GetError();
            return false;
        }
        wav.reset(buffer);
        if (length > maxBytes) {
            error = "decoded audio exceeds 256 MiB";
            return false;
        }
        data = buffer;
        bytes = int(length);
    } else if (extension(path) == ".ogg") {
        int code = 0;
        auto* vorbis = stb_vorbis_open_filename(path.string().c_str(), &code, nullptr);
        if (!vorbis) {
            error = "invalid Ogg Vorbis clip (decoder error " + std::to_string(code) + ")";
            return false;
        }
        const std::unique_ptr<stb_vorbis, decltype(&stb_vorbis_close)> owned(vorbis, stb_vorbis_close);
        const auto info = stb_vorbis_get_info(vorbis);
        const auto frames = stb_vorbis_stream_length_in_samples(vorbis);
        if (info.channels < 1 || info.channels > 2 || info.sample_rate == 0 || frames == 0 ||
            uint64_t(frames) * info.channels * sizeof(float) > maxBytes) {
            error = "Ogg clips require mono/stereo, nonempty PCM within 256 MiB";
            return false;
        }
        decoded.resize(size_t(frames) * info.channels);
        size_t read = 0;
        while (read < frames) {
            const int got = stb_vorbis_get_samples_float_interleaved(
                vorbis, info.channels, decoded.data() + read * info.channels, int((frames - read) * info.channels));
            if (!got) break;
            read += size_t(got);
        }
        if (read != frames || stb_vorbis_get_error(vorbis) != VORBIS__no_error) {
            error = "truncated or corrupt Ogg Vorbis clip";
            return false;
        }
        src = {SDL_AUDIO_F32, info.channels, int(info.sample_rate)};
        data = reinterpret_cast<const Uint8*>(decoded.data());
        bytes = int(decoded.size() * sizeof(float));
    } else {
        error = "supported audio extensions are .wav and .ogg";
        return false;
    }
    if (src.freq <= 0 || src.channels <= 0 || bytes <= 0 ||
        double(bytes) * AudioClip::sampleRate / src.freq * 8 / (SDL_AUDIO_BYTESIZE(src.format) * src.channels) >
            maxBytes) {
        error = "invalid, empty or oversized decoded audio";
        return false;
    }
    const SDL_AudioSpec dst{SDL_AUDIO_F32, 2, AudioClip::sampleRate};
    Uint8* buffer = nullptr;
    int length = 0;
    if (!SDL_ConvertAudioSamples(&src, data, bytes, &dst, &buffer, &length)) {
        error = SDL_GetError();
        return false;
    }
    const std::unique_ptr<Uint8, Free> converted(buffer);
    if (length <= 0 || size_t(length) > maxBytes || length % 8) {
        error = "invalid decoded PCM";
        return false;
    }
    AudioClip result;
    result.samples.resize(size_t(length) / sizeof(float));
    std::memcpy(result.samples.data(), buffer, size_t(length));
    if (!std::all_of(result.samples.begin(), result.samples.end(), [](float x) { return std::isfinite(x); })) {
        error = "audio contains non-finite samples";
        return false;
    }
    clip = std::move(result);
    return true;
}
}   // namespace shockwave
