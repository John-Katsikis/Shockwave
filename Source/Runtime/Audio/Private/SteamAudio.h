#pragma once

// Steam Audio (libphonon) behind a small interface. The library is opened at runtime,
// so builds without it, and machines without it, still run: available() is then false
// and the mixer uses its simple panner. No phonon headers leak out of this file.

#include <Audio/SpatialAudio.h>

#include <array>
#include <cstdint>
#include <string>

namespace shockwave::steam {

constexpr int kBlock = 256;   // frames per Steam Audio block: 5.3 ms at 48 kHz
constexpr int kSlots = 64;   // spatial voices one mixer renders at once

// Loads libphonon and makes the context and HRTF once per process. Thread-safe.
bool available();
const std::string& status();

// The effects for kSlots voices, made up front on the main thread, so the audio
// thread never allocates. Not thread-safe: the mixer's audio thread owns it.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    bool ready() const { return m_ready; }
    // Audio thread. Claims a free slot (and clears its filters), or returns -1.
    int claim();
    void release(int slot);
    // Audio thread, no allocation: filters mono[kBlock] by distance, air, directivity
    // and occlusion, spatializes it with the HRTF or the speaker panner, and adds it to
    // left and right. gain is the voice volume.
    void process(
        int slot, const float* mono, const AudioSpatial& spatial, float gain, bool hrtf, bool bilinear, float* left,
        float* right);

private:
    struct Slot {
        void* direct = nullptr;
        void* binaural = nullptr;
        void* panning = nullptr;
    };
    std::array<Slot, kSlots> m_slots{};
    uint64_t m_free = 0;   // bit i set: slot i is free
    bool m_ready = false;
    std::array<float, kBlock> m_filtered{}, m_left{}, m_right{};
};

}   // namespace shockwave::steam
