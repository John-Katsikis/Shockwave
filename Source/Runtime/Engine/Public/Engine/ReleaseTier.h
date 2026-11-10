#pragma once

// The graphics release tier (S10, docs/ReleaseTier.md): Config/ReleaseTier.swtier records
// which effects this game ships with, which ones its art direction can't do without (and
// why), and which preset is the lowest it supports. Nothing reads it while playing: it's a
// decision written down, checked against the presets and the project's default settings
// by `player --check --release`, and shown by `player --release-tier` and the editor's
// Project > Release readiness. A project without the file has made no decision yet.
//
//   swtier 1
//   fallback low
//   mandatory shadow_quality low "enemies are read by their shadows"
//   optional bloom_quality

#include <Engine/GraphicsSettings.h>
#include <Projects/Diagnostic.h>

#include <filesystem>
#include <string>
#include <vector>

namespace shockwave {
struct ReleaseInputs;

enum class TierUse {
    Optional,   // ships, and players may turn it down or off
    Mandatory,   // the art direction needs at least `minimum` on every supported tier
};
const char* tierUseName(TierUse use);   // "optional", "mandatory"

struct TierEffect {
    std::string key;   // a graphics setting with named values (graphicsSettingInfo)
    TierUse use = TierUse::Optional;
    std::string minimum;   // mandatory only: the lowest value that will do
    std::string reason;   // mandatory only: why
    int line = 0;
};

struct ReleaseTier {
    GraphicsPreset fallback = GraphicsPreset::Low;
    int fallbackLine = 0;
    std::vector<TierEffect> effects;   // in file order; a setting not listed is optional
    const TierEffect* find(const std::string& key) const;
};

std::filesystem::path releaseTierPath(const Project& project);   // Config/ReleaseTier.swtier

// How good a value of a setting is: 0 is its lowest (off, where it has one). -1 when the
// setting has no named values or doesn't have that one. true/false settings rank false 0.
int tierRank(const GraphicsSettingInfo& info, const std::string& value);
// The settings a tier may name: those with named values that players see as quality.
bool tierSetting(const GraphicsSettingInfo& info);

// Strict, like every text format here: an unknown word or setting is an error with its
// line. `shown` names the file in diagnostics.
bool readReleaseTier(const std::filesystem::path& file, const std::string& shown, ReleaseTier& out, Diagnostic& error);
bool parseReleaseTier(const std::string& text, const std::string& shown, ReleaseTier& out, Diagnostic& error);
std::string releaseTierText(const ReleaseTier& tier);

// What's wrong with a tier that reads: a mandatory effect the fallback preset or the
// project's defaults don't provide.
void validateReleaseTier(
    const ReleaseTier& tier, const GraphicsSettings& projectDefaults, const std::string& shown,
    std::vector<Diagnostic>& out);

// The fallback preset with the project's own choices kept for what presets don't set:
// what `--graphics-preset fallback` draws with.
GraphicsSettings fallbackTierSettings(const ReleaseTier& tier, const GraphicsSettings& projectDefaults);

struct ReleaseTierReport {
    std::string file;   // as the project names it
    bool present = false;
    bool readable = false;
    ReleaseTier tier;
    GraphicsSettings defaults;   // the project's own (Config/Graphics.swgraphics)
    std::vector<Diagnostic> diagnostics;
    std::string summary;   // "fallback low; 2 mandatory, 18 optional"
};
ReleaseTierReport releaseTierReport(const Project& project);
// Every tier setting with its use and its value in the fallback and each preset.
std::string releaseTierReportText(const ReleaseTierReport& report);
std::string releaseTierReportJson(const ReleaseTierReport& report);

// Fills the release_tier inputs of the release check (Projects/ReleaseSettings.h).
void releaseTierInputs(const Project& project, ReleaseInputs& inputs);

}   // namespace shockwave
