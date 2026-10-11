#include "SteamAudio.h"

#include <Core/Log.h>

#include <SDL3/SDL.h>

#if SHOCKWAVE_HAS_STEAM_AUDIO
#include <phonon.h>
#endif

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <vector>

namespace shockwave::steam {
namespace {
SW_LOG_CATEGORY(LogSteamAudio);
std::string g_status = "Steam Audio isn't built in (SHOCKWAVE_STEAM_AUDIO off)";

#if SHOCKWAVE_HAS_STEAM_AUDIO
// The few entry points the mixer uses, looked up by name so nothing links libphonon.
struct Api {
#define SW_IPL_FUNCTIONS(X)                                                                                            \
    X(iplContextCreate)                                                                                                \
    X(iplHRTFCreate)                                                                                                   \
    X(iplDirectEffectCreate)                                                                                           \
    X(iplDirectEffectRelease)                                                                                          \
    X(iplDirectEffectReset)                                                                                            \
    X(iplDirectEffectApply)                                                                                            \
    X(iplBinauralEffectCreate)                                                                                         \
    X(iplBinauralEffectRelease)                                                                                        \
    X(iplBinauralEffectReset)                                                                                          \
    X(iplBinauralEffectApply)                                                                                          \
    X(iplPanningEffectCreate)                                                                                          \
    X(iplPanningEffectRelease)                                                                                         \
    X(iplPanningEffectReset)                                                                                           \
    X(iplPanningEffectApply)
#define SW_IPL_MEMBER(name) decltype(&::name) name = nullptr;
    SW_IPL_FUNCTIONS(SW_IPL_MEMBER)
#undef SW_IPL_MEMBER
    IPLContext context = nullptr;
    IPLHRTF hrtf = nullptr;
    IPLAudioSettings audio{48000, kBlock};
};
Api g_api;
bool g_loaded = false;

void IPLCALL logCallback(IPLLogLevel level, const char* message) {
    if (level == IPL_LOGLEVEL_ERROR)
        SW_LOG(LogSteamAudio, Error, "{}", message);
    else if (level == IPL_LOGLEVEL_WARNING)
        SW_LOG(LogSteamAudio, Warning, "{}", message);
}

SDL_SharedObject* openLibrary(std::string& tried) {
#if defined(_WIN32)
    const char* file = "phonon.dll";
#elif defined(__APPLE__)
    const char* file = "libphonon.dylib";
#else
    const char* file = "libphonon.so";
#endif
    std::vector<std::string> candidates;
    const char* env = std::getenv("SHOCKWAVE_PHONON");
    if (env && std::string_view(env) == "none") {
        tried = "none: SHOCKWAVE_PHONON=none";
        return nullptr;
    }
    if (env && *env) candidates.push_back(env);
    if (const char* base = SDL_GetBasePath()) candidates.push_back(std::string(base) + file);
    candidates.push_back(file);   // the system's search path
    for (const auto& path : candidates) {
        if (auto* library = SDL_LoadObject(path.c_str())) {
            tried = path;
            return library;
        }
        tried += (tried.empty() ? "" : ", ") + path;
    }
    return nullptr;
}

void load() {
    const auto start = std::chrono::steady_clock::now();
    std::string where;
    SDL_SharedObject* library = openLibrary(where);
    if (!library) {
        g_status = "libphonon not found (tried " + where + ")";
        SW_LOG(LogSteamAudio, Info, "{}; spatial audio uses simple panning", g_status);
        return;
    }
    Api api;
    bool complete = true;
#define SW_IPL_LOAD(name)                                                                                              \
    api.name = reinterpret_cast<decltype(&::name)>(SDL_LoadFunction(library, #name));                                  \
    complete = complete && api.name;
    SW_IPL_FUNCTIONS(SW_IPL_LOAD)
#undef SW_IPL_LOAD
    if (!complete) {
        g_status = where + " is missing Steam Audio functions";
        SW_LOG(LogSteamAudio, Warning, "{}", g_status);
        return;   // the library stays open: unloading at startup buys nothing
    }
    IPLContextSettings context{};
    context.version = STEAMAUDIO_VERSION;
    context.logCallback = logCallback;
    context.simdLevel = IPL_SIMDLEVEL_AVX2;
    if (api.iplContextCreate(&context, &api.context) != IPL_STATUS_SUCCESS) {
        g_status = where + " isn't a compatible Steam Audio " + std::to_string(STEAMAUDIO_VERSION_MAJOR) + "." +
                   std::to_string(STEAMAUDIO_VERSION_MINOR);
        SW_LOG(LogSteamAudio, Warning, "{}", g_status);
        return;
    }
    IPLHRTFSettings hrtf{};
    hrtf.type = IPL_HRTFTYPE_DEFAULT;
    hrtf.volume = 1.0f;
    hrtf.normType = IPL_HRTFNORMTYPE_NONE;
    if (api.iplHRTFCreate(api.context, &api.audio, &hrtf, &api.hrtf) != IPL_STATUS_SUCCESS) {
        g_status = "Steam Audio couldn't make its HRTF";
        SW_LOG(LogSteamAudio, Warning, "{}", g_status);
        return;
    }
    g_api = api;
    g_loaded = true;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    g_status = "Steam Audio " + std::to_string(STEAMAUDIO_VERSION_MAJOR) + "." +
               std::to_string(STEAMAUDIO_VERSION_MINOR) + "." + std::to_string(STEAMAUDIO_VERSION_PATCH) + " from " +
               where;
    SW_LOG(LogSteamAudio, Info, "{} (loaded in {:.1f} ms)", g_status, ms);
}
#endif
}   // namespace

bool available() {
#if SHOCKWAVE_HAS_STEAM_AUDIO
    static std::once_flag once;
    std::call_once(once, load);
    return g_loaded;
#else
    return false;
#endif
}

const std::string& status() {
    available();
    return g_status;
}

Renderer::Renderer() {
#if SHOCKWAVE_HAS_STEAM_AUDIO
    if (!available()) return;
    auto& api = g_api;
    IPLDirectEffectSettings direct{1};
    IPLBinauralEffectSettings binaural{api.hrtf};
    IPLPanningEffectSettings panning{};
    panning.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    for (int i = 0; i < kSlots; ++i) {
        IPLDirectEffect d = nullptr;
        IPLBinauralEffect b = nullptr;
        IPLPanningEffect p = nullptr;
        const bool ok = api.iplDirectEffectCreate(api.context, &api.audio, &direct, &d) == IPL_STATUS_SUCCESS &&
                        api.iplBinauralEffectCreate(api.context, &api.audio, &binaural, &b) == IPL_STATUS_SUCCESS &&
                        api.iplPanningEffectCreate(api.context, &api.audio, &panning, &p) == IPL_STATUS_SUCCESS;
        m_slots[i] = {d, b, p};
        if (!ok) {
            SW_LOG(LogSteamAudio, Warning, "couldn't make Steam Audio effects; using {} spatial voices", i);
            break;
        }
        m_free |= uint64_t(1) << i;
    }
    m_ready = m_free != 0;
#endif
}

Renderer::~Renderer() {
#if SHOCKWAVE_HAS_STEAM_AUDIO
    if (!g_loaded) return;
    for (auto& slot : m_slots) {
        auto d = static_cast<IPLDirectEffect>(slot.direct);
        auto b = static_cast<IPLBinauralEffect>(slot.binaural);
        auto p = static_cast<IPLPanningEffect>(slot.panning);
        if (d) g_api.iplDirectEffectRelease(&d);
        if (b) g_api.iplBinauralEffectRelease(&b);
        if (p) g_api.iplPanningEffectRelease(&p);
    }
#endif
}

int Renderer::claim() {
    if (!m_free) return -1;
    const int slot = std::countr_zero(m_free);
    m_free &= ~(uint64_t(1) << slot);
#if SHOCKWAVE_HAS_STEAM_AUDIO
    const auto& s = m_slots[slot];
    g_api.iplDirectEffectReset(static_cast<IPLDirectEffect>(s.direct));
    g_api.iplBinauralEffectReset(static_cast<IPLBinauralEffect>(s.binaural));
    g_api.iplPanningEffectReset(static_cast<IPLPanningEffect>(s.panning));
#endif
    return slot;
}

void Renderer::release(int slot) {
    if (slot >= 0 && slot < kSlots) m_free |= uint64_t(1) << slot;
}

void Renderer::process(
    int slot, const float* mono, const AudioSpatial& spatial, float gain, bool hrtf, bool bilinear, float* left,
    float* right) {
#if SHOCKWAVE_HAS_STEAM_AUDIO
    const auto& s = m_slots[slot];
    std::copy(mono, mono + kBlock, m_filtered.begin());
    float* filtered = m_filtered.data();
    IPLAudioBuffer monoBuffer{1, kBlock, &filtered};
    IPLDirectEffectParams direct{};
    direct.flags = static_cast<IPLDirectEffectFlags>(
        IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION | IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION |
        IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY | IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION |
        IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
    direct.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
    direct.distanceAttenuation = std::clamp(spatial.distanceGain * gain, 0.0f, 1.0f);
    direct.directivity = std::clamp(spatial.directivity, 0.0f, 1.0f);
    direct.occlusion = std::clamp(spatial.occlusion, 0.0f, 1.0f);
    for (int band = 0; band < 3; ++band) {
        direct.airAbsorption[band] = std::clamp(spatial.air[band], 0.0f, 1.0f);
        direct.transmission[band] = std::clamp(spatial.transmission[band], 0.0f, 1.0f);
    }
    g_api.iplDirectEffectApply(static_cast<IPLDirectEffect>(s.direct), &direct, &monoBuffer, &monoBuffer);
    float* channels[2] = {m_left.data(), m_right.data()};
    IPLAudioBuffer stereo{2, kBlock, channels};
    const IPLVector3 direction{spatial.direction.x, spatial.direction.y, spatial.direction.z};
    if (hrtf) {
        IPLBinauralEffectParams params{};
        params.direction = direction;
        params.interpolation = bilinear ? IPL_HRTFINTERPOLATION_BILINEAR : IPL_HRTFINTERPOLATION_NEAREST;
        // Fade to plain stereo for sounds inside the listener's head, where HRTFs say little.
        params.spatialBlend = std::clamp(spatial.distance / 0.25f, 0.0f, 1.0f);
        params.hrtf = g_api.hrtf;
        g_api.iplBinauralEffectApply(static_cast<IPLBinauralEffect>(s.binaural), &params, &monoBuffer, &stereo);
    } else {
        IPLPanningEffectParams params{direction};
        g_api.iplPanningEffectApply(static_cast<IPLPanningEffect>(s.panning), &params, &monoBuffer, &stereo);
    }
    for (int i = 0; i < kBlock; ++i) {
        left[i] += m_left[i];
        right[i] += m_right[i];
    }
#else
    (void)slot, (void)mono, (void)spatial, (void)gain, (void)hrtf, (void)bilinear, (void)left, (void)right;
#endif
}

}   // namespace shockwave::steam
