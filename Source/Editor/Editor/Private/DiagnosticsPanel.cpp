// Window > Diagnostics (S12, docs/Diagnostics.md): this editor's log file and whether its
// last session ended cleanly; the project's player runs (Saved/Logs, Saved/Crashes) with
// each crash report named frame by frame, the way `player --symbolize` does.
#include "Editor.h"

#include <Core/Diagnostics.h>
#include <Projects/Project.h>
#include <UI/Gui.h>

#include <SDL3/SDL.h>

#include <cstdio>
#include <string>

namespace editor {

namespace {

namespace diag = shockwave::diagnostics;
namespace gui = shockwave::gui;
namespace fs = std::filesystem;

std::string describe(const diag::CrashReport& report) {
    std::string text = report.program + " crashed: " + report.signal + "\n";
    for (const diag::Frame& frame : report.frames) {
        char line[64];
        std::snprintf(line, sizeof(line), "  #%-2d ", frame.index);
        text += line;
        text += frame.function.empty() ? "??" : frame.function;
        if (!frame.module.empty()) {
            std::snprintf(line, sizeof(line), " +0x%llx", static_cast<unsigned long long>(frame.offset));
            text += "  (" + fs::path(frame.module).filename().string() + line + ")";
        }
        text += "\n";
    }
    if (!report.context.empty()) {
        text += "\nContext:\n";
        for (const auto& [key, value] : report.context) text += "  " + key + ": " + value + "\n";
    }
    if (!report.recentLog.empty()) {
        text += "\nLast log lines:\n";
        const size_t from = report.recentLog.size() > 40 ? report.recentLog.size() - 40 : 0;
        for (size_t i = from; i < report.recentLog.size(); ++i) text += "  " + report.recentLog[i] + "\n";
    }
    return text;
}

void openFolder(const fs::path& folder) {
    std::error_code ec;
    if (fs::is_directory(folder, ec)) SDL_OpenURL(("file://" + fs::absolute(folder).generic_string()).c_str());
}

}   // namespace

void Editor::drawDiagnosticsWindow() {
    ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Diagnostics", &m_showDiagnostics)) {
        ImGui::End();
        return;
    }
    const gui::Theme& theme = gui::currentTheme();
    const auto showSession = [&](const char* who, const diag::PreviousSession& session) {
        if (!session.found) {
            ImGui::Text("%s's last session ended cleanly.", who);
        } else if (session.stillRunning) {
            ImGui::Text("Another %s is running (started %s).", who, session.startedAt.c_str());
        } else {
            std::string level;
            for (const auto& [key, value] : session.context)
                if (key == "level") level = ", level " + value;
            ImGui::TextColored(
                theme.warning, "%s's last session (started %s%s) didn't end cleanly.", who, session.startedAt.c_str(),
                level.c_str());
        }
    };

    // ---- This editor ---------------------------------------------------------------------
    ImGui::SeparatorText("Editor");
    showSession("The editor", diag::previousSession());
    const diag::LogFileStatus log = diag::logFileStatus();
    const fs::path logPath = diag::logFilePath();
    if (log.open)
        ImGui::Text(
            "Log file: %s (%llu KiB)", diag::redact(logPath.generic_string()).c_str(),
            static_cast<unsigned long long>(log.bytes / 1024));
    else
        ImGui::Text("Log file: off%s", log.stopped ? (" (" + log.stopReason + ")").c_str() : "");
    ImGui::Text("Crash reports: %s", diag::crashHandlerInstalled() ? "on" : "off");
    ImGui::SetItemTooltip("A crash writes a report beside the editor's preferences (Crashes/), with the session's\n"
                          "context and its last log lines. Nothing is ever uploaded. --no-crash-handler turns it off.");
    if (!m_options.prefsPath.empty()) {
        const fs::path folder = m_options.prefsPath.parent_path();
        ImGui::SameLine();
        if (gui::button("Open editor logs")) openFolder(folder / "Logs");
        if (!diag::previousSession().crashReport.empty()) {
            ImGui::SameLine();
            if (gui::button("Show the last crash")) m_diagnosticsReport = diag::previousSession().crashReport;
        }
    }

    // ---- The project's player ------------------------------------------------------------
    ImGui::SeparatorText("Player (this project, run from source)");
    if (m_project.root.empty()) {
        ImGui::TextDisabled("Open a project to see its player runs.");
        ImGui::End();
        return;
    }
    // A project run from source writes to its own Saved/ folder (docs/UserPaths.md).
    const fs::path saved = m_project.root / "Saved";
    const fs::path logs = saved / "Logs", crashes = saved / "Crashes";
    showSession("The player", diag::inspectSession(logs, crashes, "player"));
    if (gui::button("Open player logs")) openFolder(logs);
    ImGui::SameLine();
    if (gui::button("Open crash reports")) openFolder(crashes);
    ImGui::SetItemTooltip("An installed game keeps these in the player's own folders instead; player --diagnostics\n"
                          "says where, and player --symbolize <report> names a report's frames.");

    std::vector<fs::path> reports = diag::crashReports(crashes);
    if (!m_options.prefsPath.empty())
        for (const fs::path& report : diag::crashReports(m_options.prefsPath.parent_path() / "Crashes"))
            reports.push_back(report);
    ImGui::Text("%zu crash report%s", reports.size(), reports.size() == 1 ? "" : "s");
    if (ImGui::BeginChild("##reports", ImVec2(260, 0), ImGuiChildFlags_Borders)) {
        for (const fs::path& report : reports) {
            const std::string name = report.filename().string();
            if (ImGui::Selectable(name.c_str(), report == m_diagnosticsReport)) m_diagnosticsReport = report;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##report", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        if (m_diagnosticsReport != m_diagnosticsDescribed) {
            m_diagnosticsDescribed = m_diagnosticsReport;
            m_diagnosticsText.clear();
            if (!m_diagnosticsDescribed.empty()) {
                diag::CrashReport report;
                std::string error;
                m_diagnosticsText =
                    diag::symbolizeCrashReport(m_diagnosticsDescribed, {}, report, error) ? describe(report) : error;
            }
        }
        if (m_diagnosticsText.empty()) {
            ImGui::TextDisabled("Pick a report to see where it crashed.");
        } else {
            if (gui::button("Copy")) ImGui::SetClipboardText(m_diagnosticsText.c_str());
            ImGui::TextUnformatted(m_diagnosticsText.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

}   // namespace editor
