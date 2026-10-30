#pragma once

// Local reflection probes in a scene (G07; docs/Lighting.md "Reflection probes"): what a
// probe captures and depends on, the runtime that hands the renderer a scene's captured
// cubes, what `--check` and `--lighting-info` say about them, and the capture job that makes
// them on the GPU. The files are Assets/ReflectionData.h; the per-pixel contract is
// Renderer/ReflectionProbes.h.

#include <Assets/ReflectionData.h>
#include <Engine/Components.h>
#include <Renderer/ReflectionProbes.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace shockwave {

class Assets;
class BakedLightingRuntime;
class JobSystem;
class Renderer;

// ---- What a capture sees ----

// Whether an object belongs in a reflection capture: visible renderers that stay where they
// are. Left out: anything marked BakeGeometry dynamic, physics bodies that aren't static,
// characters, animated models, Spin and Bob, and anything parented under one of those.
bool reflectionCaptureIncludes(const World& world, Entity e);

// One enabled probe as the scene has it now.
struct ReflectionProbeSource {
    Entity entity;
    BindingId binding;
    ReflectionProbe probe;
    ReflectionProbeDesc desc;   // the box in metres (the entity's scale applied), its capture point
    uint64_t hash = 0;   // placement and settings: what its cube depends on of its own
};
struct ReflectionSceneDescription {
    std::vector<ReflectionProbeSource> probes;   // in binding order
    // Everything static a capture sees: captured objects, lights, surroundings and the
    // scene's lighting bake (its manifest's bytes).
    uint64_t sceneHash = 0;
    std::vector<std::string> warnings;   // probes left out, and why
    uint64_t budgetBytes = 0;   // GPU bytes of every probe's cube at its resolution
};
// The scene's probes. sceneHash is only computed with withSceneHash (it walks every object).
ReflectionSceneDescription describeReflectionProbes(
    const World& world, const Assets& assets, const std::filesystem::path& contentRoot, bool withSceneHash = true);
// The scene's BakedLighting reflections, else "Lighting/<scene stem>.swreflection".
std::string reflectionManifestPath(const World& world, const std::string& scenePath);
// Where a probe's cube goes: beside the manifest, in "<manifest>_reflections/".
std::string reflectionCubePath(const std::string& manifest, const BindingId& binding);

// ---- Drawing with captured probes ----

// Loads a scene's captured probes and hands the renderer those still where and as they
// were captured. A new capture is uploaded completely before it replaces the old one, and a
// capture that fails to load keeps the last good one. Without cube support (the null
// renderer) nothing is uploaded and the status says why.
class ReflectionProbesRuntime {
public:
    ReflectionProbesRuntime();
    ReflectionProbesRuntime(const ReflectionProbesRuntime&) = delete;
    ReflectionProbesRuntime& operator=(const ReflectionProbesRuntime&) = delete;
    ~ReflectionProbesRuntime();

    // Main thread, between frames, before the frame's draws: follows the scene's
    // BakedLighting reflections, reloads it when the manifest changes on disk (checking at
    // most every interval seconds), and checks each probe still matches its capture.
    void update(const World& world, const Assets& assets, Renderer& renderer, double dt = 0.0, double interval = 0.5);
    void release();   // frees its cubes and clears the renderer's probes; before the renderer goes
    void reload() { m_forceReload = true; }

    enum class State { None, Missing, Invalid, Stale, Ready, Unsupported };
    struct Status {
        State state = State::None;
        std::string asset;
        std::string reason;
        uint32_t probes = 0;   // in the scene, enabled
        uint32_t captured = 0;   // in the manifest
        uint32_t bound = 0;   // drawn with their cube
        uint64_t gpuBytes = 0;
        uint64_t generation = 0;   // counts successful loads
    };
    const Status& status() const { return m_status; }
    static const char* stateName(State state);   // "none", "missing", ...

private:
    struct Loaded;
    void load(const Assets& assets, Renderer& renderer, const std::string& asset);
    void free(Loaded& loaded);

    Renderer* m_renderer = nullptr;
    std::unique_ptr<Loaded> m_loaded;
    std::vector<ReflectionProbeDesc> m_bound;   // what the renderer has
    State m_loadState = State::None;
    std::string m_loadError;
    Status m_status;
    std::string m_asset;
    std::filesystem::file_time_type m_stamp{};
    double m_poll = 0.0;
    bool m_staleScene = false;   // as of the last poll
    bool m_forceReload = false;
    std::string m_lastReport;
};

// What `player --lighting-info` and `--check` say about a scene's probes, without a GPU.
struct ReflectionInfo {
    using State = ReflectionProbesRuntime::State;
    State state = State::None;
    std::string asset;   // the manifest the scene names; empty: none named
    std::string reason;
    ReflectionManifest manifest;
    uint64_t payloadBytes = 0;
    uint32_t probes = 0;   // enabled in the scene
    uint32_t currentProbes = 0;   // captured as they are now
    std::vector<std::string> changedProbes;   // moved or edited since: "Hall probe"
    std::vector<std::string> uncapturedProbes;   // in the scene, not in the capture
    bool sceneChanged = false;   // something static changed: every probe is out of date
    bool broken = false;   // the manifest or a cube is damaged: capture again
    uint64_t budgetBytes = 0;
    std::vector<std::string> warnings;
};
ReflectionInfo
inspectReflectionProbes(const World& world, const Assets& assets, const std::filesystem::path& contentRoot);

// ---- Capturing ----

// The capture job: six frames per probe, each drawing the scene's static objects from the
// probe's capture point into a linear HDR readback (Renderer::requestSceneCapture), then a
// CPU prefilter, then the cube file, and once every probe is done the manifest. It drives
// frames rather than owning a thread: call render() from Application::onRender each frame
// in place of the usual camera and draws. Cancelling leaves the last capture as it was.
class ReflectionCapture {
public:
    // Collects the scene's enabled probes (all of them, or those in only) and checks the
    // renderer can capture. False, with the reason, when it can't start.
    bool start(
        const World& world, const Assets& assets, Renderer& renderer, const std::filesystem::path& contentRoot,
        const std::string& scenePath, std::string& error, const std::vector<BindingId>& only = {});
    // This frame's part of the job; true while it draws the frame (the caller then draws
    // nothing else into the scene), false once finished, failed or cancelled.
    bool
    render(World& world, Renderer& renderer, JobSystem& jobs, const Assets& assets, const BakedLightingRuntime* baked);
    void cancel() { m_cancelled = true; }
    bool cancelled() const { return m_cancelled; }

    bool running() const { return m_running; }
    bool finished() const { return m_finished; }   // ok or failed; not while running
    bool ok() const { return m_ok; }
    const std::string& error() const { return m_error; }
    float fraction() const;
    std::string stage() const;
    // After finishing: the manifest written, and a summary line per probe.
    const std::string& manifest() const { return m_manifestPath; }
    std::string report() const;

private:
    void fail(const std::string& message);
    bool finishProbe();
    bool writeManifest();

    std::filesystem::path m_contentRoot;
    std::string m_scene, m_manifestPath;
    ReflectionManifest m_manifest;   // the old one's probes that aren't captured again are kept
    std::vector<ReflectionProbeSource> m_probes;
    uint64_t m_sceneHash = 0;
    size_t m_probe = 0;
    uint32_t m_face = 0;
    bool m_waiting = false;
    std::vector<float> m_faces;   // level 0 of the probe being captured
    std::vector<std::string> m_lines;
    uint64_t m_bytes = 0;
    bool m_running = false, m_finished = false, m_ok = false;
    std::atomic<bool> m_cancelled{false};
    std::string m_error;
    std::chrono::steady_clock::time_point m_started{};
};

}   // namespace shockwave
