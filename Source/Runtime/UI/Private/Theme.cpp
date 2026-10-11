// The Shockwave UI theme: how a Theme turns into ImGui's style, and the .swtheme reader.
// The default values live in Theme (shockwave/Gui.h) and themes/default.swtheme.
#include <UI/Gui.h>

#include <cctype>
#include <fstream>
#include <sstream>
#include <variant>

namespace shockwave::gui {

namespace {

Theme g_current;

ImVec4 lin(const ImVec4& c) { return color(c.x, c.y, c.z, c.w); }
ImVec4 withAlpha(ImVec4 c, float a) {
    c.w = a;
    return c;
}

// Every key a .swtheme file can set, and the field it sets.
struct Key {
    const char* name;
    std::variant<ImVec4 Theme::*, float Theme::*, ImVec2 Theme::*, std::string Theme::*> member;
};
const Key kKeys[] = {
    {"button", &Theme::button},
    {"button_hover", &Theme::buttonHover},
    {"button_active", &Theme::buttonActive},
    {"on_accent", &Theme::onAccent},
    {"warning", &Theme::warning},
    {"error", &Theme::error},
    {"circuit_exec", &Theme::circuitExec},
    {"circuit_bool", &Theme::circuitBool},
    {"circuit_number", &Theme::circuitNumber},
    {"circuit_vector", &Theme::circuitVector},
    {"circuit_rotation", &Theme::circuitRotation},
    {"circuit_text", &Theme::circuitText},
    {"circuit_entity", &Theme::circuitEntity},
    {"circuit_asset", &Theme::circuitAsset},
    {"circuit_event", &Theme::circuitEvent},
    {"background", &Theme::background},
    {"panel", &Theme::panel},
    {"surface", &Theme::surface},
    {"frame", &Theme::frame},
    {"frame_hover", &Theme::frameHover},
    {"frame_active", &Theme::frameActive},
    {"border", &Theme::border},
    {"header", &Theme::header},
    {"header_hover", &Theme::headerHover},
    {"text", &Theme::text},
    {"text_disabled", &Theme::textDisabled},
    {"accent", &Theme::accent},
    {"accent_hover", &Theme::accentHover},
    {"accent_active", &Theme::accentActive},
    {"secondary", &Theme::secondary},
    {"selection", &Theme::selection},
    {"scrollbar", &Theme::scrollbar},
    {"scrollbar_hover", &Theme::scrollbarHover},
    {"window_rounding", &Theme::windowRounding},
    {"child_rounding", &Theme::childRounding},
    {"frame_rounding", &Theme::frameRounding},
    {"popup_rounding", &Theme::popupRounding},
    {"tab_rounding", &Theme::tabRounding},
    {"grab_rounding", &Theme::grabRounding},
    {"scrollbar_rounding", &Theme::scrollbarRounding},
    {"window_padding", &Theme::windowPadding},
    {"frame_padding", &Theme::framePadding},
    {"item_spacing", &Theme::itemSpacing},
    {"item_inner_spacing", &Theme::itemInnerSpacing},
    {"cell_padding", &Theme::cellPadding},
    {"indent_spacing", &Theme::indentSpacing},
    {"scrollbar_size", &Theme::scrollbarSize},
    {"grab_min_size", &Theme::grabMinSize},
    {"window_border", &Theme::windowBorderSize},
    {"frame_border", &Theme::frameBorderSize},
    {"popup_border", &Theme::popupBorderSize},
    {"tab_border", &Theme::tabBorderSize},
    {"docking_separator", &Theme::dockingSeparatorSize},
    {"font_size", &Theme::fontSize},
    {"font", &Theme::font},
};

// #rrggbb or #rrggbbaa
bool parseColor(const std::string& s, ImVec4& out) {
    if (s.size() != 7 && s.size() != 9) return false;
    if (s[0] != '#') return false;
    for (size_t i = 1; i < s.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    auto byte = [&](size_t i) { return std::stoi(s.substr(i, 2), nullptr, 16) / 255.0f; };
    out = ImVec4(byte(1), byte(3), byte(5), s.size() == 9 ? byte(7) : 1.0f);
    return true;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

}   // namespace

bool loadTheme(const std::string& path, Theme& theme, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "can't open " + path;
        return false;
    }
    Theme result = theme;
    std::string line;
    int lineNumber = 0;
    auto fail = [&](const std::string& what) {
        error = path + ":" + std::to_string(lineNumber) + ": " + what;
        return false;
    };

    while (std::getline(in, line)) {
        ++lineNumber;
        // Comments start with '#' followed by a space or the end of the line, so colors
        // like #6366f1 survive.
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '#' && (i + 1 == line.size() || line[i + 1] == ' ' || line[i + 1] == '\t')) {
                line.resize(i);
                break;
            }
        }
        line = trim(line);
        if (line.empty() || line[0] == '[') continue;   // blank, or a [section] heading (just for reading)
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return fail("expected key = value");
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));

        const Key* found = nullptr;
        for (const Key& k : kKeys) {
            if (key == k.name) found = &k;
        }
        if (!found) {
            // Any ImGui color by name ("PlotLines", "TableRowBgAlt"...), for full control.
            for (int i = 0; i < ImGuiCol_COUNT; ++i) {
                if (key == ImGui::GetStyleColorName(i)) {
                    ImVec4 c;
                    if (!parseColor(value, c)) return fail("'" + value + "' isn't a color (#rrggbb or #rrggbbaa)");
                    result.colorOverrides.emplace_back(i, c);
                    found = &kKeys[0];   // handled
                    break;
                }
            }
            if (!found) return fail("unknown key '" + key + "'");
            continue;
        }

        std::istringstream vs(value);
        const bool ok = std::visit(
            [&](auto member) {
                using T = std::remove_reference_t<decltype(result.*member)>;
                if constexpr (std::is_same_v<T, ImVec4>) {
                    return parseColor(value, result.*member);
                } else if constexpr (std::is_same_v<T, float>) {
                    return static_cast<bool>(vs >> (result.*member));
                } else if constexpr (std::is_same_v<T, ImVec2>) {
                    ImVec2& v = result.*member;
                    if (!(vs >> v.x)) return false;
                    if (!(vs >> v.y)) v.y = v.x;   // one number sets both
                    return true;
                } else {
                    result.*member = value;
                    return !value.empty();
                }
            },
            found->member);
        if (!ok) return fail("bad value for " + key + ": '" + value + "'");
    }
    theme = std::move(result);
    return true;
}

const Theme& currentTheme() { return g_current; }

void applyTheme(const Theme& t, float dpiScale) {
    g_current = t;
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();

    // Tool geometry is invariant across palettes; legacy shape keys remain readable.
    style.WindowPadding = t.windowPadding;
    style.FramePadding = t.framePadding;
    style.ItemSpacing = t.itemSpacing;
    style.ItemInnerSpacing = t.itemInnerSpacing;
    style.CellPadding = t.cellPadding;
    style.IndentSpacing = t.indentSpacing;
    style.ScrollbarSize = t.scrollbarSize;
    style.GrabMinSize = t.grabMinSize;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.DockingSeparatorSize = 1.0f;
    style.TreeLinesRounding = 0.0f;
    style.TreeLinesSize = 1.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = 2.0f;   // the lit bar over the open tab
    style.SeparatorTextPadding = t.framePadding;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SeparatorTextBorderSize = 1.0f;
    style.ScaleAllSizes(dpiScale);
    style.FontSizeBase = t.fontSize;
    style.FontScaleDpi = dpiScale;

    // Palette -> every ImGui color. Converted for the sRGB swapchain on the way.
    const ImVec4 bg = lin(t.background), surface = lin(t.surface), frame = lin(t.frame);
    const ImVec4 frameHover = lin(t.frameHover), frameActive = lin(t.frameActive), border = lin(t.border);
    const ImVec4 accentColor = lin(t.accent), accentHover = lin(t.accentHover);
    const ImVec4 panel = lin(t.panel), secondary = lin(t.secondary);
    const ImVec4 clear(0, 0, 0, 0);
    ImVec4* c = style.Colors;

    c[ImGuiCol_Text] = lin(t.text);
    c[ImGuiCol_TextDisabled] = lin(t.textDisabled);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = withAlpha(surface, 0.98f);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = clear;
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = frameHover;
    c[ImGuiCol_FrameBgActive] = frameActive;
    c[ImGuiCol_TitleBg] = surface;
    c[ImGuiCol_TitleBgActive] = surface;
    c[ImGuiCol_TitleBgCollapsed] = surface;
    c[ImGuiCol_MenuBarBg] = surface;
    c[ImGuiCol_ScrollbarBg] = clear;
    c[ImGuiCol_ScrollbarGrab] = lin(t.scrollbar);
    c[ImGuiCol_ScrollbarGrabHovered] = lin(t.scrollbarHover);
    c[ImGuiCol_ScrollbarGrabActive] = accentColor;
    c[ImGuiCol_CheckMark] = lin(t.onAccent);   // white tick on an accent-filled box
    c[ImGuiCol_CheckboxSelectedBg] = accentColor;
    c[ImGuiCol_SliderGrab] = accentColor;
    c[ImGuiCol_SliderGrabActive] = accentHover;
    c[ImGuiCol_Button] = lin(t.button);
    c[ImGuiCol_ButtonHovered] = lin(t.buttonHover);
    c[ImGuiCol_ButtonActive] = lin(t.buttonActive);
    c[ImGuiCol_Header] = lin(t.header);
    c[ImGuiCol_HeaderHovered] = lin(t.headerHover);
    c[ImGuiCol_HeaderActive] = lin(t.selection);
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_SeparatorHovered] = accentHover;
    c[ImGuiCol_SeparatorActive] = accentColor;
    c[ImGuiCol_ResizeGrip] = clear;
    c[ImGuiCol_ResizeGripHovered] = withAlpha(accentColor, 0.6f);
    c[ImGuiCol_ResizeGripActive] = accentColor;
    c[ImGuiCol_InputTextCursor] = c[ImGuiCol_Text];
    c[ImGuiCol_Tab] = surface;
    c[ImGuiCol_TabHovered] = frameActive;
    c[ImGuiCol_TabSelected] = bg;
    c[ImGuiCol_TabSelectedOverline] = accentColor;
    c[ImGuiCol_TabDimmed] = surface;
    c[ImGuiCol_TabDimmedSelected] = bg;
    c[ImGuiCol_TabDimmedSelectedOverline] = withAlpha(accentColor, 0.35f);
    c[ImGuiCol_DockingPreview] = withAlpha(accentColor, 0.4f);
    c[ImGuiCol_DockingEmptyBg] = bg;
    c[ImGuiCol_PlotLines] = accentColor;
    c[ImGuiCol_PlotLinesHovered] = secondary;
    c[ImGuiCol_PlotHistogram] = secondary;
    c[ImGuiCol_PlotHistogramHovered] = accentColor;
    c[ImGuiCol_TableHeaderBg] = panel;
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = withAlpha(border, 0.6f);
    c[ImGuiCol_TableRowBg] = clear;
    c[ImGuiCol_TableRowBgAlt] = withAlpha(panel, 0.65f);
    c[ImGuiCol_TextLink] = accentColor;
    c[ImGuiCol_TextSelectedBg] = lin(t.selection);
    c[ImGuiCol_TreeLines] = border;
    c[ImGuiCol_DragDropTarget] = accentHover;
    c[ImGuiCol_DragDropTargetBg] = withAlpha(accentColor, 0.12f);
    c[ImGuiCol_UnsavedMarker] = lin(t.text);
    c[ImGuiCol_NavCursor] = accentColor;
    c[ImGuiCol_NavWindowingHighlight] = withAlpha(lin(t.text), 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.5f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);

    for (const auto& [index, value] : t.colorOverrides) {
        if (index >= 0 && index < ImGuiCol_COUNT) c[index] = lin(value);
    }
}

}   // namespace shockwave::gui
