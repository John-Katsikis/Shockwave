#include <shockwave/Gui.h>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace gui = shockwave::gui;
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    ImGui::CreateContext();
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "%s\n", message); ++failures; }
    };
    auto same = [](ImVec4 a, ImVec4 b) {
        return std::abs(a.x-b.x) < 0.0001f && std::abs(a.y-b.y) < 0.0001f &&
               std::abs(a.z-b.z) < 0.0001f && std::abs(a.w-b.w) < 0.0001f;
    };
    std::string error;
    gui::Theme base, ember, instrument;
    const auto root = std::filesystem::path(argv[1]);
    check(gui::loadTheme((root / "themes/default.swtheme").string(), base, error), "Default theme loads");
    check(gui::loadTheme((root / "themes/ember.swtheme").string(), ember, error), "Ember theme loads");
    check(gui::loadTheme((root / "themes/instrument.swtheme").string(), instrument, error), "Instrument theme loads");
    gui::applyTheme(base);
    const auto baseButton = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    gui::applyTheme(ember);
    check(!same(baseButton, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)), "Theme switches active button color");
    check(same(ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive), gui::semanticColor(ember.buttonActive)), "Button color is converted to linear");
    // An old theme cannot accidentally reintroduce rounded controls or thick borders.
    ember.frameRounding = 12; ember.windowBorderSize = 5;
    gui::applyTheme(ember, 2);
    auto& s = ImGui::GetStyle();
    check(s.FrameRounding == 0 && s.WindowRounding == 0 && s.ChildRounding == 0 &&
          s.PopupRounding == 0 && s.TabRounding == 0 && s.GrabRounding == 0 && s.ScrollbarRounding == 0,
          "Geometry remains square across themes");
    check(s.FrameBorderSize == 2 && s.WindowBorderSize == 2 && s.TabBorderSize == 2, "HiDPI scales one logical pixel borders");
    gui::applyTheme(base, 1);
    check(s.FramePadding.x == base.framePadding.x && s.FrameBorderSize == 1, "Theme reload does not accumulate DPI scaling");
    const auto fixture = std::filesystem::path("ui-theme-test.swtheme");
    { std::ofstream out(fixture); out << "button = #00ccff\nwarning = #ffcc00\nerror = #ff0000\npanel = #0b0f13\nsecondary = #ffb020\nButtonHovered = #ffffff\n"; }
    gui::Theme custom;
    check(gui::loadTheme(fixture.string(), custom, error), "Custom semantic and ImGui colors load");
    gui::applyTheme(custom);
    check(same(s.Colors[ImGuiCol_ButtonHovered], gui::semanticColor(gui::hex(0xffffff))), "Explicit ImGui override wins");
    check(same(s.Colors[ImGuiCol_ChildBg], gui::semanticColor(gui::hex(0x0b0f13))), "Panel color fills inner panels");
    check(same(s.Colors[ImGuiCol_PlotHistogram], gui::semanticColor(gui::hex(0xffb020))), "Secondary color drives histograms");
    const auto previous = custom.button;
    { std::ofstream out(fixture); out << "button = #ff0000\nunknown_key = 3\n"; }
    check(!gui::loadTheme(fixture.string(), custom, error) && same(previous, custom.button), "Invalid theme leaves previous palette intact");
    std::filesystem::remove(fixture);
    ImGui::DestroyContext();
    return failures ? 1 : 0;
}
