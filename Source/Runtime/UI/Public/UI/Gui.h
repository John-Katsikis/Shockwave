#pragma once

#include <imgui.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

// Helpers for Dear ImGui inside Shockwave (EngineConfig::gui, Application::onGui).
//
// The window's swapchain is sRGB, so colors written by the GUI are treated as linear and
// encoded on output. ImGui colors are authored in sRGB, so every color handed to ImGui
// must go through gui::color() first, or it comes out washed out. Themes do this for you.
//
// Threading: ImGui is main-thread only. Call it from Application::onGui.
namespace shockwave::gui {

// sRGB color (0..1 components, as a color picker shows them) to what ImGui needs here.
ImVec4 color(float r, float g, float b, float a = 1.0f);
ImU32 colorU32(float r, float g, float b, float a = 1.0f);
// 0xRRGGBB as an sRGB ImVec4 (not yet converted; Theme fields are sRGB).
constexpr ImVec4 hex(uint32_t rgb, float alpha = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, alpha);
}

// Shared telemetry-console palette and density: square controls, crisp 1px borders, an
// obsidian void with matte slate inner panels, electric cyan for what's active and amber
// for secondary data. Geometry stays consistent across themes.
//
// Usually you don't touch this in code: the engine loads themes/default.swtheme at
// startup (EngineConfig::guiTheme) and reloads it whenever the file is saved, so the UI
// can be restyled while the editor runs. Every key in that file maps to a field here.
// All colors are sRGB, exactly as a color picker or a design tool shows them.
struct Theme {
    // ---- Palette ----
    ImVec4 background = hex(0x06080a);   // window backgrounds: the obsidian void
    ImVec4 panel = hex(0x0d161c);   // inner panels: child regions, lists, table rows
    ImVec4 surface = hex(0x0d1616);   // menu bar, title bars, tab bars, popups
    ImVec4 frame = hex(0x0d1c22);   // buttons, text fields, sliders, checkboxes
    ImVec4 frameHover = hex(0x16222a);
    ImVec4 frameActive = hex(0x0d3542);
    ImVec4 border = hex(0x223d49);   // the 1px lines around panels and controls
    ImVec4 header = hex(0x0d1c26);   // collapsing headers, selected list rows
    ImVec4 headerHover = hex(0x16323d);
    ImVec4 text = hex(0xd4dee6);
    ImVec4 textDisabled = hex(0x5c6875);
    ImVec4 accent = hex(0x00e5ff);   // active tools, tabs, focused panel, graph lines
    ImVec4 accentHover = hex(0x6cf3ff);
    ImVec4 accentActive = hex(0x00b4cc);
    ImVec4 secondary = hex(0xffb020);   // secondary data highlights: histograms, hovered plots
    ImVec4 selection = hex(0x00e5ff, 0.18f);   // selected text, the row being clicked
    ImVec4 scrollbar = hex(0x162632);
    ImVec4 scrollbarHover = hex(0x263d49);

    // Semantic colors for custom controls and scene overlays (also theme-file keys).
    ImVec4 button = hex(0x0d1c22);
    ImVec4 buttonHover = hex(0x16222a);
    ImVec4 buttonActive = hex(0x00b4cc);
    ImVec4 onAccent = hex(0x02090c);
    ImVec4 warning = hex(0xffd23f);
    ImVec4 error = hex(0xff3d5a);

    // ---- Shape and spacing, in pixels at 100% display scale ----
    float windowRounding = 0.0f;
    float childRounding = 0.0f;
    float frameRounding = 0.0f;
    float popupRounding = 0.0f;
    float tabRounding = 0.0f;
    float grabRounding = 0.0f;
    float scrollbarRounding = 0.0f;
    ImVec2 windowPadding{6.0f, 4.0f};
    ImVec2 framePadding{6.0f, 3.0f};
    ImVec2 itemSpacing{4.0f, 3.0f};
    ImVec2 itemInnerSpacing{3.0f, 3.0f};
    ImVec2 cellPadding{4.0f, 2.0f};
    float indentSpacing = 12.0f;
    float scrollbarSize = 8.0f;
    float grabMinSize = 4.0f;
    float windowBorderSize = 1.0f;
    float frameBorderSize = 1.0f;
    float popupBorderSize = 1.0f;
    float tabBorderSize = 1.0f;
    float dockingSeparatorSize = 1.0f;

    // ---- Text ----
    float fontSize = 14.0f;
    std::string font = "fonts/Cousine-Regular.ttf";   // a .ttf/.otf; relative to the working folder or the executable

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

// Outlines the focused panel or window in the accent color. Call once per frame, after
// the tool windows and before any modal popups.
void focusOutline();

}   // namespace shockwave::gui
