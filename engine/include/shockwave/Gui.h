#pragma once

#include <imgui.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

// Helpers for Dear ImGui inside Shockwave (EngineConfig::gui, Game::onGui).
//
// The window's swapchain is sRGB, so colors written by the GUI are treated as linear and
// encoded on output. ImGui colors are authored in sRGB, so every color handed to ImGui
// must go through gui::color() first, or it comes out washed out. Themes do this for you.
//
// Threading: ImGui is main-thread only. Call it from Game::onGui.
namespace shockwave::gui {

// sRGB color (0..1 components, as a color picker shows them) to what ImGui needs here.
ImVec4 color(float r, float g, float b, float a = 1.0f);
ImU32 colorU32(float r, float g, float b, float a = 1.0f);
// 0xRRGGBB as an sRGB ImVec4 (not yet converted; Theme fields are sRGB).
constexpr ImVec4 hex(uint32_t rgb, float alpha = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, alpha);
}

// Shared engineering-panel palette and density: square controls, crisp borders,
// graphite panels and an indigo accent. Geometry stays consistent across themes.
//
// Usually you don't touch this in code: the engine loads themes/default.swtheme at
// startup (EngineConfig::guiTheme) and reloads it whenever the file is saved, so the UI
// can be restyled while the editor runs. Every key in that file maps to a field here.
// All colors are sRGB, exactly as a color picker or a design tool shows them.
struct Theme {
    // ---- Palette ----
    ImVec4 background = hex(0x111113);       // window and panel backgrounds
    ImVec4 surface = hex(0x18181b);          // menu bar, title bars, tab bars, popups
    ImVec4 frame = hex(0x18181b);            // buttons, text fields, sliders, checkboxes
    ImVec4 frameHover = hex(0x222226);
    ImVec4 frameActive = hex(0x27272a);
    ImVec4 border = hex(0x3f3f46);           // the 1px lines around panels and controls
    ImVec4 header = hex(0x1d1d21);           // collapsing headers, selected list rows
    ImVec4 headerHover = hex(0x26262b);
    ImVec4 text = hex(0xededef);
    ImVec4 textDisabled = hex(0x71717a);
    ImVec4 accent = hex(0x6366f1);           // active tools, tabs, ticked checkboxes, sliders
    ImVec4 accentHover = hex(0x818cf8);
    ImVec4 accentActive = hex(0x4f46e5);
    ImVec4 selection = hex(0x6366f1, 0.22f); // selected text, the row being clicked
    ImVec4 scrollbar = hex(0x27272a);
    ImVec4 scrollbarHover = hex(0x3f3f46);

    // Semantic colors for custom controls and scene overlays (also theme-file keys).
    ImVec4 button = hex(0x18181b);
    ImVec4 buttonHover = hex(0x26262b);
    ImVec4 buttonActive = hex(0x4f46e5);
    ImVec4 onAccent = hex(0xffffff);
    ImVec4 warning = hex(0xfbbf24);
    ImVec4 error = hex(0xf87171);

    // ---- Shape and spacing, in pixels at 100% display scale ----
    float windowRounding = 0.0f;
    float childRounding = 0.0f;
    float frameRounding = 0.0f;
    float popupRounding = 0.0f;
    float tabRounding = 0.0f;
    float grabRounding = 0.0f;
    float scrollbarRounding = 0.0f;
    ImVec2 windowPadding{8.0f, 8.0f};
    ImVec2 framePadding{8.0f, 4.0f};
    ImVec2 itemSpacing{6.0f, 4.0f};
    ImVec2 itemInnerSpacing{4.0f, 4.0f};
    ImVec2 cellPadding{6.0f, 4.0f};
    float indentSpacing = 16.0f;
    float scrollbarSize = 10.0f;
    float grabMinSize = 10.0f;
    float windowBorderSize = 1.0f;
    float frameBorderSize = 1.0f;
    float popupBorderSize = 1.0f;
    float tabBorderSize = 1.0f;
    float dockingSeparatorSize = 1.0f;

    // ---- Text ----
    float fontSize = 15.0f;
    std::string font = "fonts/Roboto-Medium.ttf";   // a .ttf/.otf; relative to the working folder or the executable

    // Escape hatch: any individual ImGui color by its ImGuiCol_ index, applied last.
    std::vector<std::pair<int, ImVec4>> colorOverrides;
};

// Reads a .swtheme file on top of `theme`: keys the file sets replace the field, the rest
// keep their value. Returns false with a message (file and line) when it can't be read.
bool loadTheme(const std::string& path, Theme& theme, std::string& error);

// Applies a theme to ImGui's style. dpiScale: SDL_GetWindowDisplayScale. The engine does
// this at startup and on every theme file change; the font itself is loaded by the engine.
void applyTheme(const Theme& theme, float dpiScale = 1.0f);

// The theme most recently applied.
const Theme& currentTheme();

// Shared controls: use these for new editor actions instead of local style overrides.
// Standard ImGui widgets inherit the same global geometry and palette.
bool button(const char* label, const ImVec2& size = ImVec2(0, 0));
bool toolButton(const char* label, bool selected, const char* tooltip = nullptr);
// Call before the next toolbar item; wraps rather than overlapping at narrow widths.
void toolbarNext(float nextWidth);
float buttonWidth(const char* label);
float controlGroupWidth(std::initializer_list<const char*> labels);
// Right-align a trailing action group, moving to another row if it cannot fit.
void alignTrailing(float width);
int toolbarRows(float width, std::initializer_list<const char*> labels);
// Two-column engineering grid. Call EndTable only if beginPropertyGrid returns true.
bool beginPropertyGrid(const char* id);
void propertyLabel(const char* label);
ImVec4 semanticColor(const ImVec4& srgb);
ImU32 semanticColorU32(const ImVec4& srgb);

// The current accent color, ready for ImGui (already converted).
ImVec4 accent();

} // namespace shockwave::gui
