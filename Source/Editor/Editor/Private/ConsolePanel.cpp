// The Console panel: everything the editor reports, with warnings and errors called out,
// filters per level, search and copy. The newest message also shows in the status bar.
#include "Editor.h"

#include <UI/Gui.h>

#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace editor {

namespace gui = shockwave::gui;

namespace {

std::string clockTime() {
    const std::time_t now = std::time(nullptr);
    char text[16];
    std::strftime(text, sizeof(text), "%H:%M:%S", std::localtime(&now));
    return text;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

}   // namespace

void Editor::log(const std::string& message) { addLog(LogLevel::Info, message); }
void Editor::warn(const std::string& message) { addLog(LogLevel::Warning, message); }
void Editor::error(const std::string& message) { addLog(LogLevel::Error, message); }

void Editor::addLog(LogLevel level, const std::string& message) {
    const char* prefix = level == LogLevel::Error ? "error: " : level == LogLevel::Warning ? "warning: " : "";
    std::printf("Editor: %s%s\n", prefix, message.c_str());
    m_log.push_back({clockTime(), level, message});
    if (level == LogLevel::Warning) ++m_warningCount;
    if (level == LogLevel::Error) ++m_errorCount;
    constexpr size_t kMaxLines = 2000;
    if (m_log.size() > kMaxLines) {
        const LogEntry& dropped = m_log.front();
        if (dropped.level == LogLevel::Warning) --m_warningCount;
        if (dropped.level == LogLevel::Error) --m_errorCount;
        m_log.erase(m_log.begin());
    }
    m_logScrollToBottom = true;
}

void Editor::logAssetChanges(const std::vector<shockwave::AssetFolder::Change>& changes) {
    using What = shockwave::AssetFolder::Change::What;
    for (const auto& change : changes) {
        switch (change.what) {
        case What::Added:
            if (const auto* file = m_assetFolder->find(change.name); file && file->imported)
                log("Imported " + change.name);
            break;
        case What::Reloaded: log("Reloaded " + change.name); break;
        case What::Failed: error("Couldn't import " + change.name + ": " + change.error); break;
        case What::Removed: warn(change.name + " was removed; anything using it keeps the last version"); break;
        }
    }
}

void Editor::drawConsole() {
    if (m_focusConsole) {
        ImGui::SetNextWindowFocus();
        m_focusConsole = false;
    }
    if (!ImGui::Begin("Console", &m_showConsole)) {
        ImGui::End();
        return;
    }
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    if (gui::button("Clear")) {
        m_log.clear();
        m_warningCount = m_errorCount = 0;
    }
    ImGui::SameLine();
    const size_t infoCount = m_log.size() - m_warningCount - m_errorCount;
    auto levelToggle = [&](const char* name, size_t count, bool& show, ImVec4 color) {
        char label[64];
        std::snprintf(label, sizeof(label), "%s %zu###%s", name, count, name);
        if (!show)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        else
            ImGui::PushStyleColor(ImGuiCol_Text, color);
        if (gui::button(label)) show = !show;
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip(show ? "Hide these" : "Show these");
        ImGui::SameLine();
    };
    levelToggle("Info", infoCount, m_logShowInfo, ImGui::GetStyleColorVec4(ImGuiCol_Text));
    levelToggle("Warnings", m_warningCount, m_logShowWarnings, gui::semanticColor(gui::currentTheme().warning));
    levelToggle("Errors", m_errorCount, m_logShowErrors, gui::semanticColor(gui::currentTheme().error));
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x, 120.0f * dpi));
    ImGui::InputTextWithHint("##search", "Search", &m_logFilter);
    ImGui::Separator();

    const std::string filter = lower(m_logFilter);
    ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec4 warnColor = gui::semanticColor(gui::currentTheme().warning);
    const ImVec4 errorColor = gui::semanticColor(gui::currentTheme().error);
    for (size_t i = 0; i < m_log.size(); ++i) {
        const LogEntry& entry = m_log[i];
        if ((entry.level == LogLevel::Info && !m_logShowInfo) ||
            (entry.level == LogLevel::Warning && !m_logShowWarnings) ||
            (entry.level == LogLevel::Error && !m_logShowErrors)) {
            continue;
        }
        if (!filter.empty() && lower(entry.text).find(filter) == std::string::npos) continue;
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextDisabled("%s", entry.time.c_str());
        ImGui::SameLine();
        if (entry.level == LogLevel::Info) {
            ImGui::TextUnformatted(entry.text.c_str());
        } else {
            ImGui::TextColored(entry.level == LogLevel::Error ? errorColor : warnColor, "%s", entry.text.c_str());
        }
        if (ImGui::BeginPopupContextItem("line")) {
            if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(entry.text.c_str());
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (m_logScrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        m_logScrollToBottom = false;
    }
    ImGui::EndChild();
    ImGui::End();
}

}   // namespace editor
