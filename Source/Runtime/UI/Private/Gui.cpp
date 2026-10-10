#include <UI/Gui.h>

#include <imgui_internal.h>   // the focused window, for focusOutline

#include <cmath>
#include <algorithm>
#include <cstring>

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

ImVec4 semanticColor(const ImVec4& c) { return color(c.x, c.y, c.z, c.w); }
ImU32 semanticColorU32(const ImVec4& c) { return ImGui::GetColorU32(semanticColor(c)); }

bool button(const char* label, const ImVec2& size) {
    return ImGui::Button(label, size);
}

bool toolButton(const char* label, bool selected, const char* tooltip) {
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, accent());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, semanticColor(currentTheme().accentHover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, semanticColor(currentTheme().accentActive));
        ImGui::PushStyleColor(ImGuiCol_Text, semanticColor(currentTheme().onAccent));
    }
    const bool pressed = button(label);
    if (selected) ImGui::PopStyleColor(4);
    if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

float buttonWidth(const char* label) {
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

void toolbarNext(float nextWidth) {
    const float end = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + nextWidth;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (end <= right) ImGui::SameLine();
}

float controlGroupWidth(std::initializer_list<const char*> labels) {
    float width = 0.0f;
    for (const char* label : labels)
        width += buttonWidth(label) + (width > 0.0f ? ImGui::GetStyle().ItemSpacing.x : 0.0f);
    return width;
}

void alignTrailing(float width) {
    const float right = ImGui::GetWindowContentRegionMax().x;
    const float left = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    if (left + ImGui::GetStyle().ItemSpacing.x + width <= right)
        ImGui::SameLine(right - width);
    else
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - width));
}

int toolbarRows(float width, std::initializer_list<const char*> labels) {
    int rows = 1;
    float used = 0.0f;
    for (const char* label : labels) {
        const float item = buttonWidth(label);
        if (used > 0.0f && used + ImGui::GetStyle().ItemSpacing.x + item > width) {
            ++rows;
            used = 0.0f;
        }
        used += item + (used > 0.0f ? ImGui::GetStyle().ItemSpacing.x : 0.0f);
    }
    return rows;
}

bool beginPropertyGrid(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, 0.38f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
    return true;
}

void propertyLabel(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1.0f);
}

void focusOutline() {
    ImGuiWindow* window = ImGui::GetCurrentContext()->NavWindow;
    if (!window) return;
    window = window->RootWindow;   // the panel, not a list or toolbar inside it
    // Popups and tooltips carry their own border; ## windows are bars and hosts, not panels.
    if (!window->Active || window->Hidden || std::strncmp(window->Name, "##", 2) == 0 ||
        (window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip))) return;
    const ImVec2 min = window->Pos;
    const ImVec2 max(min.x + window->Size.x, min.y + window->Size.y);
    ImDrawList* draw = window->DrawList;
    draw->PushClipRect(min, max, false);
    draw->AddRect(min, max, ImGui::GetColorU32(accent()), 0.0f, 0,
                  ImGui::GetStyle().WindowBorderSize);
    draw->PopClipRect();
}

ImVec4 accent() {
    const ImVec4& a = currentTheme().accent;
    return color(a.x, a.y, a.z, a.w);
}

} // namespace shockwave::gui
