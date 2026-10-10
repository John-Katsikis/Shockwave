// The project hub: what the editor shows with no project open. Recent projects, a new
// project form, and a way to open any project (or the bundled sample).
#include "Editor.h"

#include <shockwave/Gui.h>

#include <SDL3/SDL.h>
#include <imgui_stdlib.h>

#include <filesystem>
#include <unordered_map>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;

namespace {

// The sample project at the root of the engine's source tree (shockwave.swproject).
// Looked for where you run from, above the build, then in the tree the editor was built from.
std::optional<fs::path> findSampleProject() {
    const fs::path exe = SDL_GetBasePath();
    for (const fs::path& base : {fs::current_path(), exe / ".." / "..", fs::path(SHOCKWAVE_SOURCE_DIR)}) {
        std::error_code ec;
        const fs::path file = base / "shockwave.swproject";
        if (fs::is_regular_file(file, ec)) return fs::weakly_canonical(file, ec);
    }
    return std::nullopt;
}

// What the recent list shows for a project, re-read every couple of seconds rather than
// every frame.
struct RecentInfo {
    bool exists = false, readable = false;
    std::string name, error;
};
const RecentInfo& recentInfo(const std::string& path) {
    static std::unordered_map<std::string, std::pair<double, RecentInfo>> cache;
    const double now = ImGui::GetTime();
    auto& [stamp, info] = cache[path];
    if (stamp == 0.0 || now - stamp > 2.0) {
        stamp = now;
        std::error_code ec;
        info = {};
        info.exists = fs::exists(path, ec);
        shockwave::Project project;
        info.readable = info.exists && shockwave::loadProject(path, project, info.error);
        info.name = project.name;
    }
    return info;
}

} // namespace

void Editor::drawProjectHub() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking;
    if (!ImGui::Begin("##hub", nullptr, flags)) {
        ImGui::End();
        return;
    }
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const float width = std::min(980.0f * dpi, ImGui::GetContentRegionAvail().x);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 40.0f * dpi);
    ImGui::BeginGroup();

    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted("Shockwave");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Open a project to start, or make a new one.");
    ImGui::Dummy(ImVec2(width, 18.0f * dpi));

    if (ImGui::BeginTable("hub", 2, ImGuiTableFlags_SizingStretchProp, ImVec2(width, 0))) {
        ImGui::TableSetupColumn("recent", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("new", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();

        // Recent projects.
        ImGui::TableNextColumn();
        ImGui::SeparatorText("Recent projects");
        if (m_prefs.recentProjects.empty()) ImGui::TextDisabled("None yet.");
        std::string remove;
        for (const std::string& path : m_prefs.recentProjects) {
            const RecentInfo& info = recentInfo(path);
            const bool exists = info.exists, readable = info.readable;
            const std::string& err = info.error;
            ImGui::PushID(path.c_str());
            const float rowHeight = ImGui::GetTextLineHeight() * 2.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
            const ImVec2 rowPos = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##project", false, readable ? 0 : ImGuiSelectableFlags_Disabled,
                                  ImVec2(ImGui::GetContentRegionAvail().x - 70.0f * dpi, rowHeight))) {
                openProject(path);
            }
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float pad = ImGui::GetStyle().FramePadding.y;
            draw->AddText(ImVec2(rowPos.x + 8.0f, rowPos.y + pad), ImGui::GetColorU32(readable ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                          readable ? info.name.c_str() : fs::path(path).stem().string().c_str());
            const std::string detail = readable ? fs::path(path).parent_path().string() : (exists ? err : "Missing: " + path);
            draw->AddText(ImVec2(rowPos.x + 8.0f, rowPos.y + pad + ImGui::GetTextLineHeight()),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), detail.c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowHeight - ImGui::GetFrameHeight()) * 0.5f);
            if (ImGui::SmallButton("Remove")) remove = path;
            ImGui::SetItemTooltip("Take it off this list (nothing is deleted)");
            ImGui::PopID();
        }
        if (!remove.empty()) {
            std::erase(m_prefs.recentProjects, remove);
            m_prefsDirty = true;
            savePreferences();
        }

        // New or other project.
        ImGui::TableNextColumn();
        ImGui::SeparatorText("New project");
        ImGui::PushItemWidth(-1.0f);
        ImGui::TextDisabled("Name");
        ImGui::InputText("##name", &m_newProjectName);
        ImGui::TextDisabled("Location");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f * dpi);
        ImGui::InputText("##location", &m_newProjectLocation);
        ImGui::SameLine();
        if (gui::button("Browse...", ImVec2(-1.0f, 0))) showNewProjectFolderDialog();
        ImGui::PopItemWidth();
        const fs::path folder = fs::path(m_newProjectLocation) / m_newProjectName;
        ImGui::TextDisabled("Creates %s", folder.string().c_str());
        ImGui::Spacing();
        ImGui::BeginDisabled(m_newProjectName.empty() || m_newProjectLocation.empty());
        const gui::Theme& theme = gui::currentTheme();
        ImGui::PushStyleColor(ImGuiCol_Button, gui::semanticColor(theme.accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, gui::semanticColor(theme.accentHover));
        ImGui::PushStyleColor(ImGuiCol_Text, gui::semanticColor(theme.onAccent));
        if (gui::button("Create project", ImVec2(-1.0f, 0))) createNewProject(folder, m_newProjectName);
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        if (!m_hubError.empty()) ImGui::TextColored(gui::semanticColor(gui::currentTheme().error), "%s", m_hubError.c_str());

        ImGui::Spacing();
        ImGui::SeparatorText("Open");
        if (gui::button("Open project...", ImVec2(-1.0f, 0))) showOpenProjectDialog();
        ImGui::SetItemTooltip("Pick a .swproject file");
        static const std::optional<fs::path> sample = findSampleProject();   // the disk doesn't move under us
        if (sample) {
            if (gui::button("Open the sample project", ImVec2(-1.0f, 0))) openProject(*sample);
            ImGui::SetItemTooltip("%s", sample->string().c_str());
        }
        ImGui::EndTable();
    }

    // Problems from startup (a bad path on the command line, say).
    if (m_errorCount > 0) {
        ImGui::Dummy(ImVec2(0, 12.0f * dpi));
        for (const LogEntry& entry : m_log) {
            if (entry.level == LogLevel::Error) ImGui::TextColored(gui::semanticColor(gui::currentTheme().error), "%s", entry.text.c_str());
        }
    }
    ImGui::EndGroup();
    ImGui::End();
}

} // namespace editor
