// Build > Package game: see PackageWindow.h and docs/Packaging.md.
#include "PackageWindow.h"

#include <Multiplayer/NetworkValidation.h>

#include <UI/Gui.h>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::PackageStep;

namespace {

double seconds() { return static_cast<double>(SDL_GetTicksNS()) * 1e-9; }

// Exclude patterns as the window shows them: one line, separated by spaces.
std::string joinPatterns(const std::vector<std::string>& patterns) {
    std::string text;
    for (const std::string& pattern : patterns) text += (text.empty() ? "" : " ") + pattern;
    return text;
}

std::vector<std::string> splitPatterns(const std::string& text) {
    std::vector<std::string> patterns;
    std::istringstream in(text);
    for (std::string pattern; in >> pattern;) patterns.push_back(pattern);
    return patterns;
}

std::string quote(const fs::path& path) {
    std::string out = "'";
    for (const char c : path.string()) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

}   // namespace

PackageWindow::~PackageWindow() {
    m_cancel = true;
    if (m_thread.joinable()) m_thread.join();
}

// ---- Settings -----------------------------------------------------------------------

void PackageWindow::load(const shockwave::Project& project) {
    m_project = project.file;
    m_settingsError.clear();
    shockwave::PackageSettings settings;
    if (!shockwave::loadPackageSettings(project, settings, m_settingsError)) settings = {};
    m_edit = m_saved = settings;
    m_excludeText = joinPatterns(settings.exclude);
    m_outputText = settings.output.generic_string();
    std::error_code ec;
    m_settingsTime = fs::last_write_time(shockwave::packageSettingsPath(project), ec);
}

// The file is shared with tools and AI agents: when it changes on disk and nothing is
// being edited here, the window shows the new settings.
void PackageWindow::watchSettingsFile(const shockwave::Project& project) {
    if (m_project != project.file) {
        load(project);
        return;
    }
    const double now = seconds();
    if (now < m_nextWatch) return;
    m_nextWatch = now + 1.0;
    std::error_code ec;
    const auto time = fs::last_write_time(shockwave::packageSettingsPath(project), ec);
    if (time != m_settingsTime && m_edit == m_saved) load(project);
}

bool PackageWindow::save(const shockwave::Project& project) {
    if (m_edit == m_saved && fs::exists(shockwave::packageSettingsPath(project))) return true;
    std::string error;
    if (!shockwave::savePackageSettings(project, m_edit, error)) {
        m_settingsError = error;
        SW_LOG(shockwave::LogPackage, Error, "{}", error);
        return false;
    }
    m_saved = m_edit;
    m_settingsError.clear();
    std::error_code ec;
    m_settingsTime = fs::last_write_time(shockwave::packageSettingsPath(project), ec);
    SW_LOG(shockwave::LogPackage, Info, "Saved {}", project.relative(shockwave::packageSettingsPath(project)));
    return true;
}

// ---- Packaging ----------------------------------------------------------------------

void PackageWindow::start(const shockwave::Project& project) {
    if (m_running || !save(project)) return;
    if (m_thread.joinable()) m_thread.join();
    {
        std::lock_guard lock(m_mutex);
        m_lines.clear();
        m_result.reset();
        m_step = PackageStep::Build;
        m_fraction = 0.0f;
    }
    m_jobProject = project;
    m_jobSettings = m_edit;
    const shockwave::net::ProjectSteamUse steam = shockwave::net::projectSteamUse(project);
    m_jobSteam = {steam.uses, steam.appId, m_development && !m_release};
    m_jobRelease = m_release;
    m_cancel = false;
    m_running = true;
    m_thread = std::thread([this] {
        shockwave::PackageOptions options;
        options.cancel = &m_cancel;
        options.steam = m_jobSteam;
        options.release = m_jobRelease;
        options.log = [this](shockwave::LogLevel level, std::string_view text) {
            // Problems also go to the Console, where the rest of the editor's are.
            if (level >= shockwave::LogLevel::Warning) shockwave::logMessage(shockwave::LogPackage, level, text);
            std::lock_guard lock(m_mutex);
            m_lines.push_back({level, std::string(text)});
            m_scrollToEnd = true;
        };
        options.progress = [this](PackageStep step, float fraction) {
            std::lock_guard lock(m_mutex);
            m_step = step;
            m_fraction = fraction;
        };
        shockwave::PackageResult result = shockwave::packageProject(m_jobProject, m_jobSettings, options);
        if (result.ok) SW_LOG(shockwave::LogPackage, Info, "Packaged {}", result.folder.string());
        std::lock_guard lock(m_mutex);
        m_result = std::move(result);
        m_running = false;
    });
}

void PackageWindow::startRelease(const shockwave::Project& project) {
    open = true;
    load(project);
    m_release = true;
    m_development = false;
    start(project);
}

std::optional<shockwave::PackageResult> PackageWindow::result() {
    if (m_running) return std::nullopt;
    std::lock_guard lock(m_mutex);
    return m_result;
}

// ---- Window -------------------------------------------------------------------------

void PackageWindow::draw(const shockwave::Project& project) {
    if (!open) return;
    watchSettingsFile(project);
    {
        std::lock_guard lock(m_browseMutex);
        if (m_browsed) {
            m_outputText = *m_browsed;
            m_edit.output = *m_browsed;
            m_browsed.reset();
        }
    }
    ImGui::SetNextWindowSize(ImVec2(560, 620), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Package game", &open)) {
        ImGui::End();
        return;
    }
    drawSettings(project);
    drawCook(project);
    ImGui::Spacing();

    const bool running = m_running;
    const auto problems = shockwave::validatePackageSettings(project, m_edit);
    ImGui::BeginDisabled(running || !problems.empty());
    if (gui::button("Package")) start(project);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        m_release ? "Save these settings, then build, check, cook, copy only what the cook lists,\n"
                    "verify every file against it and archive (the same as: player --package --release)"
                  : "Save these settings, then build, check, copy, verify and archive the game\n"
                    "(the same as: player --package)");
    ImGui::SameLine();
    if (running) {
        if (gui::button("Cancel")) m_cancel = true;
    } else {
        ImGui::BeginDisabled(m_edit == m_saved);
        if (gui::button("Save settings")) save(project);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(
            "Write %s without packaging", project.relative(shockwave::packageSettingsPath(project)).c_str());
    }
    for (const std::string& problem : problems)
        ImGui::TextColored(gui::semanticColor(gui::currentTheme().error), "%s", problem.c_str());
    if (!m_settingsError.empty())
        ImGui::TextColored(gui::semanticColor(gui::currentTheme().error), "%s", m_settingsError.c_str());

    ImGui::Spacing();
    drawProgress();
    drawResult();
    drawLog();
    ImGui::End();
}

void PackageWindow::drawSettings(const shockwave::Project& project) {
    if (!gui::beginPropertyGrid("package")) return;
    ImGui::BeginDisabled(m_running);
    gui::propertyLabel("Name");
    ImGui::InputTextWithHint("##name", project.name.c_str(), &m_edit.name);
    ImGui::SetItemTooltip("What players see; empty uses the project's name (name)");
    gui::propertyLabel("Program");
    ImGui::InputTextWithHint("##executable", m_edit.executableName(project).c_str(), &m_edit.executable);
    ImGui::SetItemTooltip("The file players run; empty uses the name without spaces (executable)");
    gui::propertyLabel("Version");
    ImGui::InputText("##version", &m_edit.version);
    ImGui::SetItemTooltip("Part of the folder name, like 1.0.0 (version)");
    gui::propertyLabel("Output folder");
    const float browse = gui::buttonWidth("Browse...");
    ImGui::SetNextItemWidth(
        std::max(ImGui::GetContentRegionAvail().x - browse - ImGui::GetStyle().ItemSpacing.x, 60.0f));
    if (ImGui::InputTextWithHint("##output", "Saved/Packages", &m_outputText)) m_edit.output = m_outputText;
    ImGui::SetItemTooltip("Where packages go: relative to the project, or a full path (output)");
    ImGui::SameLine();
    if (gui::button("Browse...")) {
        const std::string start = m_edit.outputFolder(project).string();
        SDL_ShowOpenFolderDialog(
            [](void* user, const char* const* files, int) {
                if (!files || !files[0]) return;
                auto* self = static_cast<PackageWindow*>(user);
                std::lock_guard lock(self->m_browseMutex);
                self->m_browsed = std::string(files[0]);
            },
            this, SDL_GetKeyboardFocus(), start.c_str(), false);
    }
    gui::propertyLabel("Leave out");
    if (ImGui::InputTextWithHint("##exclude", "*.blend Content/Raw", &m_excludeText))
        m_edit.exclude = splitPatterns(m_excludeText);
    ImGui::SetItemTooltip("File patterns not to ship, separated by spaces (exclude). Source/, Saved/, Tools/\n"
                          "and hidden files are always left out");
    gui::propertyLabel("Build first");
    ImGui::Checkbox("##build", &m_edit.build);
    ImGui::SetItemTooltip("Build the player and game code before packaging, so the package is never stale (build)");
    gui::propertyLabel("Make archive");
    ImGui::Checkbox("##archive", &m_edit.archive);
    ImGui::SetItemTooltip("Also make a .tar.gz of the folder, ready to upload or send (archive)");
    gui::propertyLabel("Strip symbols");
    ImGui::Checkbox("##strip", &m_edit.strip);
    ImGui::SetItemTooltip("Leave out symbol tables players don't need; makes the package much smaller (strip)");
    gui::propertyLabel("Ship C++ runtime");
    ImGui::Checkbox("##cpp_runtime", &m_edit.cppRuntime);
    ImGui::SetItemTooltip("Include this machine's C++ runtime, used only on older systems whose own is too old\n"
                          "for the game (cpp_runtime)");
    gui::propertyLabel("Release");
    if (ImGui::Checkbox("##release", &m_release) && m_release) m_development = false;
    ImGui::SetItemTooltip("Ship only what the cook says the game needs, after the release checks: an optimised\n"
                          "engine, nothing a level needs left out, no absolute paths, no test AppID, every\n"
                          "license (player --package --release, docs/Cooking.md). Never saved");
    gui::propertyLabel("Development package");
    ImGui::BeginDisabled(m_release);
    ImGui::Checkbox("##development", &m_development);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("A test build: allows Steam AppID 480 (Valve's shared test app). Never saved, so a\n"
                          "release can't ship as one by mistake (player --package --dev-package)");
    gui::propertyLabel("Target");
    ImGui::TextUnformatted(shockwave::packagePlatform());
    ImGui::SetItemTooltip("The platform this engine was built for: an engine packages and cooks for its own\n"
                          "platform only (docs/Cooking.md)");
    ImGui::EndDisabled();
    gui::propertyLabel("Makes");
    ImGui::TextWrapped(
        "%s", (m_edit.outputFolder(project) / shockwave::packageFolderName(project, m_edit)).string().c_str());
    ImGui::EndTable();
}

// ---- Cook ---------------------------------------------------------------------------

void PackageWindow::refreshCook(const shockwave::Project& project, bool verify) {
    m_cook = shockwave::cookReport(project.root, shockwave::packagePlatform(), verify);
    m_cookProject = project.file;
    std::error_code ec;
    m_cookTime = fs::last_write_time(m_cook.file, ec);
}

void PackageWindow::drawCook(const shockwave::Project& project) {
    // The manifest changes when a release package or `player --cook` runs, here or elsewhere.
    const double now = seconds();
    if (m_cookProject != project.file) {
        refreshCook(project, false);
    } else if (now >= m_nextCookCheck && !m_running) {
        m_nextCookCheck = now + 1.0;
        std::error_code ec;
        if (fs::last_write_time(m_cook.file, ec) != m_cookTime) refreshCook(project, false);
    }
    const gui::Theme& theme = gui::currentTheme();
    if (!ImGui::CollapsingHeader("Cook", m_release ? ImGuiTreeNodeFlags_DefaultOpen : 0)) return;
    if (!m_cook.error.empty()) {
        ImGui::TextColored(
            gui::semanticColor(theme.error), "%s: %s", project.relative(m_cook.file).c_str(), m_cook.error.c_str());
        return;
    }
    if (!m_cook.cooked) {
        ImGui::TextDisabled(
            "Not cooked for %s yet: a release package cooks first (or player --cook)", shockwave::packagePlatform());
        return;
    }
    const shockwave::CookManifest& m = m_cook.manifest;
    ImGui::Text(
        "%s cook: %zu levels, %zu files (%.1f MB), %zu roots", m.release ? "Release" : "Development", m.levels.size(),
        m_cook.files, static_cast<double>(m_cook.bytes) / (1024.0 * 1024.0), m.roots.size());
    ImGui::TextDisabled(
        "content %s, build %s, %s", shockwave::hex64(m.contentId).c_str(), shockwave::hex64(m.buildId).c_str(),
        m.buildConfig.c_str());
    if (ImGui::BeginTable("##cook_levels", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Level");
        ImGui::TableSetupColumn("Files");
        ImGui::TableSetupColumn("Shared");
        ImGui::TableSetupColumn("Size");
        ImGui::TableHeadersRow();
        for (const shockwave::CookReport::Level& level : m_cook.levels) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", level.id.c_str(), level.id == m.entryLevel ? " (entry)" : "");
            ImGui::SetItemTooltip("%s", level.scene.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%zu", level.assets);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", level.shared);
            ImGui::SetItemTooltip("Files other levels reach too");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f MB", static_cast<double>(level.bytes) / (1024.0 * 1024.0));
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(m_running);
    if (gui::button("Verify files")) refreshCook(project, true);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Check every file the cook lists is still there, byte for byte\n"
                          "(the same as: player --cook-info <manifest> --verify <project>)");
    if (m_cook.verified) {
        ImGui::SameLine();
        if (m_cook.changed.empty())
            ImGui::TextColored(gui::accent(), "every file matches the cook");
        else
            ImGui::TextColored(
                gui::semanticColor(theme.warning), "%zu changed since the cook: package again", m_cook.changed.size());
        for (const std::string& line : m_cook.changed)
            ImGui::TextColored(gui::semanticColor(theme.warning), "%s", line.c_str());
    }
}

void PackageWindow::drawProgress() {
    PackageStep step;
    float fraction;
    bool failed;
    {
        std::lock_guard lock(m_mutex);
        step = m_step;
        fraction = m_fraction;
        failed = m_result && !m_result->ok;
    }
    if (!m_running && !m_result) return;
    // One row of steps, then a bar for the whole run.
    const gui::Theme& theme = gui::currentTheme();
    for (int i = 0; i < shockwave::kPackageStepCount; ++i) {
        const auto each = static_cast<PackageStep>(i);
        const bool done = static_cast<int>(step) > i;
        const bool current = step == each;
        const ImVec4 color = current && failed ? gui::semanticColor(theme.error)
                             : current         ? gui::accent()
                             : done            ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                                               : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        if (i) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 4.0f);
        ImGui::TextColored(color, "%s %s", done ? "+" : current ? ">" : "-", shockwave::packageStepName(each));
    }
    const float overall =
        step == PackageStep::Done ? 1.0f : (static_cast<float>(step) + fraction) / shockwave::kPackageStepCount;
    ImGui::ProgressBar(
        overall, ImVec2(-1.0f, 0.0f),
        m_running ? shockwave::packageStepName(step)
        : failed  ? "Failed"
                  : "Done");
}

void PackageWindow::drawResult() {
    std::optional<shockwave::PackageResult> result;
    {
        std::lock_guard lock(m_mutex);
        result = m_result;
    }
    if (!result) return;
    const gui::Theme& theme = gui::currentTheme();
    if (!result->ok) {
        ImGui::TextColored(gui::semanticColor(theme.error), "%s", result->error.c_str());
        return;
    }
    ImGui::TextColored(
        gui::accent(), "Packaged in %.1f s: %.1f MB, %zu files", result->seconds,
        static_cast<double>(result->bytes) / (1024.0 * 1024.0), result->files);
    if (result->release)
        ImGui::Text(
            "Release: %zu files from the cook, every one checked against it; %zu project files left out",
            result->cookedAssets, result->leftOut);
    if (!result->minimumGlibc.empty())
        ImGui::TextDisabled("Runs on Linux with glibc %s or newer", result->minimumGlibc.c_str());
    if (!result->warnings.empty())
        ImGui::TextColored(
            gui::semanticColor(theme.warning), "%zu warning%s below", result->warnings.size(),
            result->warnings.size() == 1 ? "" : "s");
    if (gui::button("Open folder")) SDL_OpenURL(("file://" + result->folder.generic_string()).c_str());
    ImGui::SetItemTooltip("%s", result->folder.string().c_str());
    ImGui::SameLine();
    if (gui::button("Run game")) {
        // Started on its own, the way a player would, so it outlives the editor.
#if defined(_WIN32)
        const std::string command = "start \"\" \"" + result->launcher.string() + "\"";
#else
        const std::string command = quote(result->launcher) + " >/dev/null 2>&1 &";
#endif
        if (std::system(command.c_str()) != 0)
            SW_LOG(shockwave::LogPackage, Error, "Couldn't start {}", result->launcher.string());
    }
    ImGui::SetItemTooltip("%s", result->launcher.string().c_str());
}

void PackageWindow::drawLog() {
    std::lock_guard lock(m_mutex);
    if (m_lines.empty()) return;
    ImGui::SeparatorText("Output");
    if (ImGui::BeginChild(
            "##package_log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        const gui::Theme& theme = gui::currentTheme();
        for (const Line& line : m_lines) {
            if (line.level == shockwave::LogLevel::Error)
                ImGui::TextColored(gui::semanticColor(theme.error), "%s", line.text.c_str());
            else if (line.level == shockwave::LogLevel::Warning)
                ImGui::TextColored(gui::semanticColor(theme.warning), "%s", line.text.c_str());
            else
                ImGui::TextUnformatted(line.text.c_str());
        }
        if (std::exchange(m_scrollToEnd, false)) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

}   // namespace editor
