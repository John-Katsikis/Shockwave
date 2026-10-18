#include <Engine/GraphicsSettings.h>

#include <Engine/Engine.h>
#include <Projects/Project.h>

#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>

namespace shockwave {
GraphicsSettings graphicsPreset(GraphicsPreset preset, GraphicsSettings base) {
    switch (preset) {
    case GraphicsPreset::Low:
        base.renderScale = 0.67f;
        base.shadows = ShadowQuality::Low;
        base.lodErrorPixels = 2;
        break;
    case GraphicsPreset::Medium:
        base.renderScale = 0.85f;
        base.shadows = ShadowQuality::Medium;
        base.lodErrorPixels = 1.5f;
        break;
    case GraphicsPreset::High:
        base.renderScale = 1;
        base.shadows = ShadowQuality::High;
        base.lodErrorPixels = 1;
        break;
    case GraphicsPreset::Ultra:
        base.renderScale = 1;
        base.shadows = ShadowQuality::Ultra;
        base.lodErrorPixels = 0.5f;
        break;
    }
    return base;
}
bool validateGraphicsSettings(const GraphicsSettings& s, std::string& error) {
    error.clear();
    if (!std::isfinite(s.renderScale) || s.renderScale < 0.25f || s.renderScale > 2)
        error = "render_scale must be in 0.25..2";
    else if (static_cast<uint32_t>(s.shadows) > 4)
        error = "shadow_quality must be off, low, medium, high or ultra";
    else if (!std::isfinite(s.lodErrorPixels) || s.lodErrorPixels < 0 || s.lodErrorPixels > 100)
        error = "lod_error_pixels must be in 0..100";
    else if (
        !std::isfinite(s.frameLimit) || s.frameLimit < 0 || (s.frameLimit > 0 && s.frameLimit < 10) ||
        s.frameLimit > 1000)
        error = "frame_limit must be 0 (uncapped) or in 10..1000";
    return error.empty();
}
namespace {
template <class T>
bool number(const std::string& text, T& value) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size() && std::isfinite(value);
}
}   // namespace
bool loadGraphicsSettings(const std::filesystem::path& file, GraphicsSettings& out, std::string& error) {
    error.clear();
    std::ifstream in(file);
    if (!in) {
        error = file.string() + ": can't open graphics settings";
        return false;
    }
    GraphicsSettings result = out;
    std::string line;
    int lineNumber = 0;
    bool header = false;
    std::set<std::string> seen;
    auto fail = [&](const std::string& message) {
        error = file.string() + ":" + std::to_string(lineNumber) + ": " + message;
        return false;
    };
    while (std::getline(in, line)) {
        ++lineNumber;
        if (auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream row(line);
        std::string key, value, extra;
        if (!(row >> key)) continue;
        if (!(row >> value) || (row >> extra)) return fail("expected a key and one value");
        if (!header) {
            if (key != "swgraphics" || value != "1") return fail("expected swgraphics 1");
            header = true;
            continue;
        }
        if (!seen.insert(key).second) return fail("duplicate setting " + key);
        if (key == "render_scale") {
            if (!number(value, result.renderScale)) return fail("invalid render_scale");
        } else if (key == "lod_error_pixels") {
            if (!number(value, result.lodErrorPixels)) return fail("invalid lod_error_pixels");
        } else if (key == "frame_limit") {
            if (!number(value, result.frameLimit)) return fail("invalid frame_limit");
        } else if (key == "shadow_quality")
            result.shadows = static_cast<ShadowQuality>(parseShadowQuality(value));
        else if (key == "vsync" || key == "low_latency") {
            if (value != "true" && value != "false") return fail(key + " must be true or false");
            (key == "vsync" ? result.vsync : result.lowLatency) = value == "true";
        } else
            return fail("unknown graphics setting " + key);
        std::string problem;
        if (!validateGraphicsSettings(result, problem)) return fail(problem);
    }
    if (!header) return fail("expected swgraphics 1");
    if (in.bad()) return fail("failed reading graphics settings");
    if (!validateGraphicsSettings(result, error)) return false;
    out = result;
    return true;
}
std::string graphicsSettingsText(const GraphicsSettings& s) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << "swgraphics 1\nrender_scale "
        << s.renderScale << "\nshadow_quality " << shadowQualityName(static_cast<int>(s.shadows))
        << "\nlod_error_pixels " << s.lodErrorPixels << "\nvsync " << (s.vsync ? "true" : "false") << "\nframe_limit "
        << s.frameLimit << "\nlow_latency " << (s.lowLatency ? "true" : "false") << '\n';
    return out.str();
}
bool saveGraphicsSettings(const std::filesystem::path& file, const GraphicsSettings& s, std::string& error) {
    if (!validateGraphicsSettings(s, error)) return false;
    if (file.empty()) {
        error = "No user settings file: run the standalone player to save preferences";
        return false;
    }
    std::error_code ec;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        return false;
    }
    const auto temp = std::filesystem::path(
        file.string() + "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp");
    {
        std::ofstream out(temp, std::ios::trunc);
        out << graphicsSettingsText(s);
        out.close();
        if (!out) {
            std::filesystem::remove(temp, ec);
            error = file.string() + ": couldn't write graphics settings";
            return false;
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
    error.clear();
    return true;
}
std::string graphicsSettingsSchema() {
    return R"(# Graphics settings schema

Generated by `player --graphics-schema docs/GraphicsSchema.md`.

Files start with `swgraphics 1`. One key/value per line; `#` starts a comment.
Unknown keys, duplicate keys, non-finite numbers and unsupported versions are errors.
Omitted keys inherit the preceding layer: project manifest, Config/Graphics.swgraphics,
user preferences, then command-line overrides. See [Graphics](Graphics.md).

| Key | Values | Built-in default | Meaning |
|---|---|---|---|
| render_scale | 0.25..2 | 1 | Scene resolution multiplier; game UI stays at output resolution. |
| shadow_quality | off, low, medium, high, ultra | high | Shadow map quality. |
| lod_error_pixels | 0..100 | 1 | Mesh LOD error in rendered pixels; 0 forces full detail. Needs imported LODs. |
| vsync | true, false | true | Synchronize presentation with the display. |
| frame_limit | 0 or 10..1000 | 0 | FPS cap; 0 means uncapped except for VSync. |
| low_latency | true, false | false | Wait for presentation before polling the next frame's input. |
)";
}
bool projectGraphicsSettings(const Project& project, GraphicsSettings& out, std::string& error) {
    GraphicsSettings settings{
        1,
        static_cast<ShadowQuality>(project.shadowQuality),
        static_cast<float>(project.lodErrorPixels),
        project.vsync,
        project.frameLimit,
        project.lowLatency};
    const auto file = project.root / "Config" / "Graphics.swgraphics";
    std::error_code ec;
    const bool exists = std::filesystem::exists(file, ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        return false;
    }
    if ((exists && !loadGraphicsSettings(file, settings, error)) || !validateGraphicsSettings(settings, error))
        return false;
    out = settings;
    return true;
}
GraphicsSettings Graphics::current() const {
    if (!m_engine) return m_headless;
    const auto& r = m_engine->renderer();
    return {r.renderScale(), r.shadowQuality(),      r.lodSettings().maxErrorPixels,
            r.vsync(),       m_engine->frameLimit(), m_engine->lowLatency()};
}
GraphicsSettings Graphics::requested() const { return m_pending.value_or(current()); }
bool Graphics::apply(const GraphicsSettings& s, std::string& error) {
    if (!validateGraphicsSettings(s, error)) return false;
    if (m_engine)
        m_pending = s;
    else
        m_headless = s;
    return true;
}
bool Graphics::configure(const GraphicsSettings& defaults, std::filesystem::path file, std::string& error) {
    if (!validateGraphicsSettings(defaults, error)) return false;
    m_defaults = defaults;
    m_file = std::move(file);
    return true;
}
bool Graphics::save(const GraphicsSettings& s, std::string& error) const {
    return saveGraphicsSettings(m_file, s, error);
}
void Graphics::commit() {
    if (!m_pending || !m_engine) return;
    const auto s = *m_pending;
    m_pending.reset();
    const auto previous = current();
    auto& r = m_engine->renderer();
    if (s.renderScale != previous.renderScale) r.setRenderScale(s.renderScale);
    if (s.shadows != previous.shadows) r.setShadowQuality(s.shadows);
    if (s.lodErrorPixels != previous.lodErrorPixels) {
        auto lod = r.lodSettings();
        lod.maxErrorPixels = s.lodErrorPixels;
        r.setLodSettings(lod);
    }
    if (s.vsync != previous.vsync) r.setVsync(s.vsync);
    // Refresh the smoother's period when VSync changes too.
    if (s.frameLimit != previous.frameLimit || s.vsync != previous.vsync) m_engine->setFrameLimit(s.frameLimit);
    if (s.lowLatency != previous.lowLatency) m_engine->setLowLatency(s.lowLatency);
}
}   // namespace shockwave
