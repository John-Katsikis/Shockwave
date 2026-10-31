#pragma once

#include <Engine/Subsystem.h>
#include <Renderer/Clusters.h>
#include <Renderer/Decals.h>
#include <Renderer/RenderTypes.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace shockwave {
class Engine;
struct Project;

// Player preferences, independent of scene lighting, materials and the renderer backend.
// Every field is described once in graphicsSettingInfo(): files, schema, command line,
// editor and game menus all read that table.
struct GraphicsSettings {
    float renderScale = 1;
    ShadowQuality shadows = ShadowQuality::High;
    float lodErrorPixels = 1;
    bool vsync = true;
    double frameLimit = 0;
    bool lowLatency = false;
    AntiAliasing antiAliasing = AntiAliasing::None;
    BloomQuality bloomQuality = BloomQuality::High;
    float bloomScale = 1;   // the player's multiplier on the scene's authored bloom intensity
    TextureQuality textureQuality = TextureQuality::Ultra;   // material textures' largest resident level
    uint32_t anisotropy = 16;   // 1, 2, 4, 8 or 16: caps every sampler's anisotropic filtering
    MaterialQuality materialQuality = MaterialQuality::High;   // low: materials skip their detail layer (A02)
    // Off by default, like anti-aliasing: the presets turn it on (G04).
    AmbientOcclusion ambientOcclusion = AmbientOcclusion::Off;
    // Baked lightmaps and probe volumes (G05, G06); off draws the environment instead.
    bool bakedLighting = true;
    // Local reflection probes (G07); off reflects the global environment everywhere.
    bool reflectionProbes = true;
    // Projected decals (G08): how many a frame draws, nearest first; off draws none.
    DecalQuality decals = DecalQuality::High;
    // Clustered local lighting (G11): how pixels find their point and spot lights. The same
    // picture every way; brute force is the reference.
    LightCulling lightCulling = LightCulling::Auto;
    bool operator==(const GraphicsSettings&) const = default;
};
enum class GraphicsPreset { Low, Medium, High, Ultra };
const char* graphicsPresetName(GraphicsPreset preset);   // "low", ...
// Presets set the quality keys (GraphicsSettingInfo::preset) and keep the rest: pacing
// choices and personal ones like bloom_scale.
GraphicsSettings graphicsPreset(GraphicsPreset preset, GraphicsSettings base = {});
// The preset whose quality keys these settings match, or none: "Custom".
std::optional<GraphicsPreset> matchingGraphicsPreset(const GraphicsSettings& settings);

// How a change reaches the screen (docs/Graphics/01-SettingsAndTextures.md 1.2).
enum class GraphicsApply {
    Uniform,   // the next frame's constants
    Pipeline,   // pipelines are ready; switches at a frame boundary
    Targets,   // new images are made, then the old ones freed once their frames finish
    Restart,   // only takes effect after a restart (none today)
};
const char* graphicsApplyName(GraphicsApply apply);   // "uniform", ...

// One setting, as files, the schema, menus and tools all see it.
struct GraphicsSettingInfo {
    const char* key;   // as spelled in .swgraphics and on the command line
    const char* label;   // for menus: "Anti-aliasing"
    const char* category;   // "Quality", "Display"
    const char* tooltip;
    const char* values;   // for the schema: "0.25..2", "none, fxaa"
    std::vector<const char*> choices;   // enum and true/false values in menu order; empty for numbers
    GraphicsApply apply;
    bool preset;   // presets set it
    std::string (*get)(const GraphicsSettings&);
    // Parses and range-checks one value; leaves settings unchanged and says why on failure.
    bool (*set)(GraphicsSettings&, const std::string& value, std::string& error);
};
const std::vector<GraphicsSettingInfo>& graphicsSettingInfo();
const GraphicsSettingInfo* findGraphicsSetting(const std::string& key);
// "key=value" or key and value: for --graphics-set and the editor.
bool setGraphicsSetting(
    GraphicsSettings& settings, const std::string& key, const std::string& value, std::string& error);
bool validateGraphicsSettings(const GraphicsSettings& settings, std::string& error);
// Missing fields inherit out's values. Failed reads leave out unchanged.
bool loadGraphicsSettings(const std::filesystem::path& file, GraphicsSettings& out, std::string& error);
bool saveGraphicsSettings(const std::filesystem::path& file, const GraphicsSettings& settings, std::string& error);
std::string graphicsSettingsText(const GraphicsSettings& settings);
std::string graphicsSettingsSchema();
// Manifest defaults overlaid by optional Config/Graphics.swgraphics. No user state.
bool projectGraphicsSettings(const Project& project, GraphicsSettings& out, std::string& error);

// game.graphics(): main-thread API, including UI callbacks. apply() validates the whole
// request, then queues it for the next frame boundary. Never changes GPU resources while
// recording a frame. No file access or allocations occur in an unchanged frame.
// Without an Engine (tests/tools), the same validation and state API works in memory.
class Graphics final : public Subsystem {
public:
    explicit Graphics(Engine* engine = nullptr) : m_engine(engine) {}
    GraphicsSettings current() const;   // applied values, read from the engine (also sees editor edits)
    GraphicsSettings requested() const;   // newest queued values, otherwise current()
    bool apply(const GraphicsSettings& settings, std::string& error);
    // Defaults + user file are configured by the host, never by a game module.
    bool configure(const GraphicsSettings& defaults, std::filesystem::path userFile, std::string& error);
    const GraphicsSettings& defaults() const { return m_defaults; }
    const std::filesystem::path& file() const { return m_file; }
    // Save an explicit validated selection. Saving does not change rendering; callers
    // can save first, then apply, so a failed write leaves the live settings untouched.
    bool save(const GraphicsSettings& settings, std::string& error) const;

    // How the last apply() went: pending until the frame boundary (and while the renderer
    // is still making new images), then applied, or applied with a fallback the renderer
    // explains in Renderer::effectiveSettings.
    enum class State { Applied, Pending, AppliedWithFallback };
    struct Status {
        uint64_t generation = 0;   // counts apply() calls that passed validation
        State state = State::Applied;
        std::vector<EffectiveSetting> fallbacks;   // settings running differently, and why
    };
    Status status() const;

private:
    friend class Engine;
    struct Project;
    void commit();
    Engine* m_engine;
    GraphicsSettings m_defaults, m_headless;
    std::optional<GraphicsSettings> m_pending;
    std::filesystem::path m_file;
    uint64_t m_generation = 0;
};
}   // namespace shockwave
