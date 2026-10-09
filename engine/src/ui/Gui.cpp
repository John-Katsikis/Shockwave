#include <shockwave/Gui.h>

#include <cmath>

namespace shockwave::gui {

namespace {

float toLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace

ImVec4 color(float r, float g, float b, float a) {
    return ImVec4(toLinear(r), toLinear(g), toLinear(b), a);
}

ImU32 colorU32(float r, float g, float b, float a) {
    return ImGui::GetColorU32(color(r, g, b, a));
}

ImVec4 accent() {
    return color(0.16f, 0.56f, 1.0f);
}

void applyTheme(float dpiScale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();

    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 5);
    style.ItemInnerSpacing = ImVec2(5, 4);
    style.IndentSpacing = 14;
    style.ScrollbarSize = 12;
    style.GrabMinSize = 8;
    style.WindowBorderSize = 1;
    style.FrameBorderSize = 0;
    style.PopupBorderSize = 1;
    style.TabBorderSize = 0;
    style.WindowRounding = 4;
    style.ChildRounding = 4;
    style.FrameRounding = 3;
    style.PopupRounding = 4;
    style.ScrollbarRounding = 6;
    style.GrabRounding = 3;
    style.TabRounding = 3;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.DockingSeparatorSize = 2;
    style.ScaleAllSizes(dpiScale);
    style.FontScaleDpi = dpiScale;

    // Authored in sRGB; color() converts for the sRGB swapchain.
    ImVec4* c = style.Colors;
    const ImVec4 accentColor = accent();
    const ImVec4 accentHover = color(0.32f, 0.66f, 1.0f);
    const ImVec4 accentDim = color(0.12f, 0.36f, 0.66f);

    c[ImGuiCol_Text] = color(0.90f, 0.91f, 0.93f);
    c[ImGuiCol_TextDisabled] = color(0.48f, 0.50f, 0.54f);
    c[ImGuiCol_WindowBg] = color(0.105f, 0.11f, 0.12f);
    c[ImGuiCol_ChildBg] = color(0.105f, 0.11f, 0.12f, 0.0f);
    c[ImGuiCol_PopupBg] = color(0.12f, 0.125f, 0.14f, 0.98f);
    c[ImGuiCol_Border] = color(0.20f, 0.21f, 0.23f);
    c[ImGuiCol_BorderShadow] = color(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = color(0.16f, 0.17f, 0.185f);
    c[ImGuiCol_FrameBgHovered] = color(0.21f, 0.22f, 0.24f);
    c[ImGuiCol_FrameBgActive] = color(0.24f, 0.25f, 0.28f);
    c[ImGuiCol_TitleBg] = color(0.08f, 0.085f, 0.09f);
    c[ImGuiCol_TitleBgActive] = color(0.08f, 0.085f, 0.09f);
    c[ImGuiCol_TitleBgCollapsed] = color(0.08f, 0.085f, 0.09f);
    c[ImGuiCol_MenuBarBg] = color(0.08f, 0.085f, 0.09f);
    c[ImGuiCol_ScrollbarBg] = color(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = color(0.25f, 0.26f, 0.28f);
    c[ImGuiCol_ScrollbarGrabHovered] = color(0.32f, 0.33f, 0.36f);
    c[ImGuiCol_ScrollbarGrabActive] = color(0.38f, 0.40f, 0.43f);
    c[ImGuiCol_CheckMark] = accentColor;
    c[ImGuiCol_SliderGrab] = accentColor;
    c[ImGuiCol_SliderGrabActive] = accentHover;
    c[ImGuiCol_Button] = color(0.19f, 0.20f, 0.22f);
    c[ImGuiCol_ButtonHovered] = color(0.25f, 0.26f, 0.29f);
    c[ImGuiCol_ButtonActive] = accentDim;
    c[ImGuiCol_Header] = color(0.18f, 0.19f, 0.21f);
    c[ImGuiCol_HeaderHovered] = color(0.23f, 0.24f, 0.27f);
    c[ImGuiCol_HeaderActive] = accentDim;
    c[ImGuiCol_Separator] = color(0.20f, 0.21f, 0.23f);
    c[ImGuiCol_SeparatorHovered] = accentDim;
    c[ImGuiCol_SeparatorActive] = accentColor;
    c[ImGuiCol_ResizeGrip] = color(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = accentDim;
    c[ImGuiCol_ResizeGripActive] = accentColor;
    c[ImGuiCol_InputTextCursor] = c[ImGuiCol_Text];
    c[ImGuiCol_Tab] = color(0.08f, 0.085f, 0.09f);
    c[ImGuiCol_TabHovered] = color(0.20f, 0.21f, 0.23f);
    c[ImGuiCol_TabSelected] = color(0.105f, 0.11f, 0.12f);
    c[ImGuiCol_TabSelectedOverline] = accentColor;
    c[ImGuiCol_TabDimmed] = color(0.08f, 0.085f, 0.09f);
    c[ImGuiCol_TabDimmedSelected] = color(0.105f, 0.11f, 0.12f);
    c[ImGuiCol_TabDimmedSelectedOverline] = color(0.30f, 0.32f, 0.35f);
    c[ImGuiCol_DockingPreview] = ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.5f);
    c[ImGuiCol_DockingEmptyBg] = color(0.06f, 0.065f, 0.07f);
    c[ImGuiCol_PlotLines] = accentColor;
    c[ImGuiCol_PlotLinesHovered] = accentHover;
    c[ImGuiCol_PlotHistogram] = accentColor;
    c[ImGuiCol_PlotHistogramHovered] = accentHover;
    c[ImGuiCol_TableHeaderBg] = color(0.14f, 0.145f, 0.16f);
    c[ImGuiCol_TableBorderStrong] = color(0.20f, 0.21f, 0.23f);
    c[ImGuiCol_TableBorderLight] = color(0.16f, 0.17f, 0.185f);
    c[ImGuiCol_TableRowBg] = color(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = color(1.0f, 1.0f, 1.0f, 0.03f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(accentColor.x, accentColor.y, accentColor.z, 0.35f);
    c[ImGuiCol_DragDropTarget] = accentHover;
    c[ImGuiCol_NavCursor] = accentColor;
    c[ImGuiCol_NavWindowingHighlight] = color(1.0f, 1.0f, 1.0f, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = color(0.8f, 0.8f, 0.8f, 0.2f);
    c[ImGuiCol_ModalWindowDimBg] = color(0.0f, 0.0f, 0.0f, 0.5f);
}

} // namespace shockwave::gui
