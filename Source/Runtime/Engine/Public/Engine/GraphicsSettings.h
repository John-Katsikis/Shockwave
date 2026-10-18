#pragma once

#include <Engine/Subsystem.h>
#include <Renderer/RenderTypes.h>

#include <filesystem>
#include <optional>
#include <string>

namespace shockwave {
class Engine;
struct Project;

// Player preferences, independent of scene lighting, materials and the renderer backend.
struct GraphicsSettings {
    float renderScale = 1;
    ShadowQuality shadows = ShadowQuality::High;
    float lodErrorPixels = 1;
    bool vsync = true;
    double frameLimit = 0;
    bool lowLatency = false;
    bool operator==(const GraphicsSettings&) const = default;
};
enum class GraphicsPreset { Low, Medium, High, Ultra };
// Presets change quality only; keep the player's pacing choices.
GraphicsSettings graphicsPreset(GraphicsPreset preset, GraphicsSettings base = {});
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

private:
    friend class Engine;
    struct Project;
    void commit();
    Engine* m_engine;
    GraphicsSettings m_defaults, m_headless;
    std::optional<GraphicsSettings> m_pending;
    std::filesystem::path m_file;
};
}   // namespace shockwave
