#include "ArenaGraphicsMenu.h"

#include <array>
#include <cstdio>

namespace arena {
using namespace shockwave;
namespace {
template <class T, size_t N>
T next(T current, const std::array<T, N>& choices) {
    for (T choice : choices)
        if (choice > current + T(0.001)) return choice;
    return choices.front();
}
}   // namespace
void GraphicsMenu::bind(GameContext& game) {
    auto& ui = game.ui();
    ui.on("arena_graphics", [this, &game](const UIEvent&) { open(game); });
    ui.on("graphics_cancel", [this, &game](const UIEvent&) { close(game); });
    ui.on("graphics_defaults", [this, &game](const UIEvent&) {
        m_draft = game.graphics().defaults();
        refresh(game);
    });
    for (int i = 0; i != 4; ++i) {
        ui.on("graphics_preset_" + std::to_string(i), [this, &game, i](const UIEvent&) {
            m_draft = graphicsPreset(static_cast<GraphicsPreset>(i), m_draft);
            refresh(game);
        });
    }
    ui.on("graphics_scale", [this, &game](const UIEvent&) {
        m_draft.renderScale =
            next(m_draft.renderScale, std::array{0.25f, 0.5f, 0.67f, 0.75f, 0.85f, 1.f, 1.25f, 1.5f, 2.f});
        refresh(game);
    });
    ui.on("graphics_shadows", [this, &game](const UIEvent&) {
        m_draft.shadows = static_cast<ShadowQuality>((static_cast<int>(m_draft.shadows) + 1) % 5);
        refresh(game);
    });
    ui.on("graphics_aa", [this, &game](const UIEvent&) {
        m_draft.antiAliasing = m_draft.antiAliasing == AntiAliasing::None ? AntiAliasing::Fxaa : AntiAliasing::None;
        refresh(game);
    });
    ui.on("graphics_bloom", [this, &game](const UIEvent&) {
        m_draft.bloomQuality = static_cast<BloomQuality>((static_cast<int>(m_draft.bloomQuality) + 1) % 4);
        refresh(game);
    });
    ui.on("graphics_bloom_scale", [this, &game](const UIEvent&) {
        m_draft.bloomScale = next(m_draft.bloomScale, std::array{0.f, 0.25f, 0.5f, 0.75f, 1.f});
        refresh(game);
    });
    ui.on("graphics_lod", [this, &game](const UIEvent&) {
        m_draft.lodErrorPixels = next(m_draft.lodErrorPixels, std::array{0.f, 0.5f, 1.f, 1.5f, 2.f, 4.f});
        refresh(game);
    });
    ui.on("graphics_vsync", [this, &game](const UIEvent&) {
        m_draft.vsync = !m_draft.vsync;
        refresh(game);
    });
    ui.on("graphics_fps", [this, &game](const UIEvent&) {
        m_draft.frameLimit = next(m_draft.frameLimit, std::array{0., 60., 90., 120., 144., 165., 240., 360.});
        refresh(game);
    });
    ui.on("graphics_latency", [this, &game](const UIEvent&) {
        m_draft.lowLatency = !m_draft.lowLatency;
        refresh(game);
    });
    ui.on("graphics_apply", [this, &game](const UIEvent&) {
        std::string error;
        auto& graphics = game.graphics();
        const bool persist = !graphics.file().empty();
        if (!validateGraphicsSettings(m_draft, error) || (persist && !graphics.save(m_draft, error)) ||
            !graphics.apply(m_draft, error)) {
            game.ui().setText("Arena Menu", "graphics-status", error);
            return;
        }
        game.ui().setText(
            "Arena Menu", "graphics-status",
            persist ? "Applied and saved." : "Applied for this session. Saving is disabled in this host.");
    });
    ui.on("arena_menu_back", [this, &game](const UIEvent&) {
        if (m_visible)
            close(game);
        else
            game.ui().click("Arena Menu", "resume");
    });
}
void GraphicsMenu::open(GameContext& game) {
    m_draft = game.graphics().requested();
    m_visible = true;
    game.ui().setClass("Arena Menu", "menu-panel", "hidden", true);
    game.ui().setClass("Arena Menu", "match-panel", "hidden", true);
    game.ui().setClass("Arena Menu", "graphics-panel", "hidden", false);
    refresh(game);
    game.ui().focus("Arena Menu", "preset-low");
}
void GraphicsMenu::close(GameContext& game) {
    m_visible = false;
    game.ui().setClass("Arena Menu", "graphics-panel", "hidden", true);
    game.ui().setClass("Arena Menu", "menu-panel", "hidden", false);
    game.ui().setClass("Arena Menu", "match-panel", "hidden", false);
    game.ui().focus("Arena Menu", "graphics");
}
void GraphicsMenu::refresh(GameContext& game) {
    auto label = [&](const char* id, const std::string& text) { game.ui().setText("Arena Menu", id, text); };
    char text[128];
    std::snprintf(text, sizeof(text), "Render scale: %.0f%%", m_draft.renderScale * 100);
    label("graphics-scale", text);
    label("graphics-shadows", std::string("Shadows: ") + shadowQualityName(static_cast<int>(m_draft.shadows)));
    label("graphics-aa", m_draft.antiAliasing == AntiAliasing::Fxaa ? "Anti-aliasing: FXAA" : "Anti-aliasing: none");
    label("graphics-bloom", std::string("Bloom: ") + bloomQualityName(m_draft.bloomQuality));
    std::snprintf(text, sizeof(text), "Bloom strength: %.0f%%", m_draft.bloomScale * 100);
    label("graphics-bloom-scale", text);
    const auto preset = matchingGraphicsPreset(m_draft);
    std::snprintf(
        text, sizeof(text), "Preset: %s. Select a setting to cycle its value. Apply when ready.",
        preset ? graphicsPresetName(*preset) : "custom");
    label("graphics-preset", text);
    std::snprintf(
        text, sizeof(text), "Mesh LOD error: %.2g px%s", m_draft.lodErrorPixels,
        m_draft.lodErrorPixels == 0 ? " (full detail)" : "");
    label("graphics-lod", text);
    label("graphics-vsync", m_draft.vsync ? "VSync: on" : "VSync: off");
    std::snprintf(text, sizeof(text), "Frame limit: %.0f FPS", m_draft.frameLimit);
    label("graphics-fps", m_draft.frameLimit == 0 ? "Frame limit: uncapped" : text);
    label("graphics-latency", m_draft.lowLatency ? "Low latency: on" : "Low latency: off");
    label("graphics-apply", game.graphics().file().empty() ? "APPLY" : "APPLY & SAVE");
    label("graphics-status", "Changes take effect when you apply.");
}
}   // namespace arena
