// The Player Settings window: the defaults a game ships with (Config/Preferences.swprefs),
// edited in place, and previews of what a player can change that changes how documents
// look: interface size, language and high contrast. A preview is the editor's only and is
// never saved; with none set the viewport shows the shipped values. The editor also loads
// the game's text catalogs, so {{text.*}} reads the same as in the game and a catalog's
// edit shows when it's saved. See docs/Preferences.md and docs/Localization.md.
#include "Editor.h"

#include <Engine/Localization.h>
#include <Engine/Preferences.h>
#include <GameFramework/AppShell.h>
#include <GameUI/GameUI.h>
#include <UI/Gui.h>

#include <imgui_stdlib.h>

#include <format>
#include <fstream>
#include <sstream>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::PlayerPreferences;
using shockwave::PreferenceInfo;

namespace {

// The catalogs' sizes and times, so a save shows without reading every file each frame.
uint64_t textStamp(const fs::path& folder) {
    uint64_t stamp = 1469598103934665603ull;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return stamp;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.path().extension() != ".swstrings") continue;
        const auto size = entry.file_size(ec);
        const auto time = entry.last_write_time(ec).time_since_epoch().count();
        for (const uint64_t part :
             {std::hash<std::string>{}(entry.path().filename().string()), uint64_t(size), uint64_t(time)})
            stamp = (stamp ^ part) * 1099511628211ull;
    }
    return stamp;
}

}   // namespace

void Editor::loadProjectText() {
    shockwave::Localization& text = engine().localization();
    text.load(m_project.assetFolder / "Localization", m_project.defaultLanguage);
    text.addBuiltin("en", shockwave::AppShell::builtinText());
    for (const std::string& problem : text.problems()) warn(problem);
    m_textStamp = textStamp(m_project.assetFolder / "Localization");
    applyPlayerSettings();   // the locale again, now that its catalog may be there
}

void Editor::loadShippedPreferences() {
    m_shippedPrefs.clear();
    m_shippedPrefsEdit.clear();
    m_shippedPrefsError.clear();
    m_shippedPrefsDirty = false;
    const fs::path file = shockwave::projectPreferencesPath(m_project);
    std::error_code ec;
    if (fs::exists(file, ec)) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        if (!shockwave::parsePreferences(text.str(), m_project.relative(file), m_shippedPrefs, m_shippedPrefsError)) {
            m_shippedPrefs.clear();
            error(m_shippedPrefsError);
        }
    }
    applyPlayerSettings();
}

void Editor::applyPlayerSettings() {
    if (!m_hasProject) return;
    shockwave::Preferences& preferences = engine().preferences();
    PlayerPreferences shipped;
    std::string why;
    for (const auto& [key, value] : m_shippedPrefs) shockwave::setPreference(shipped, key, value, why);
    if (m_previewUiScale > 0) shipped.uiScale = m_previewUiScale;
    if (m_previewLanguage) shipped.language = *m_previewLanguage;
    if (m_previewContrast >= 0) shipped.highContrast = m_previewContrast == 1;
    // The editor's own window and volume stay as they are: only what documents and game
    // code read comes from the game's preferences.
    PlayerPreferences shown = preferences.current();
    const PlayerPreferences keep = shown;
    shown = shipped;
    shown.windowMode = keep.windowMode;
    shown.windowWidth = keep.windowWidth;
    shown.windowHeight = keep.windowHeight;
    shown.masterVolume = keep.masterVolume;
    shown.effectsVolume = keep.effectsVolume;
    shown.musicVolume = keep.musicVolume;
    shown.uiVolume = keep.uiVolume;
    shown.voiceVolume = keep.voiceVolume;
    shown.muteWhenUnfocused = keep.muteWhenUnfocused;
    if (!preferences.apply(shown, why)) warn("Couldn't preview the player settings: " + why);
}

void Editor::updatePlayerSettings(double dt) {
    if (!m_hasProject) return;
    m_textAge += dt;
    if (m_textAge < 1.0) return;
    m_textAge = 0;
    if (textStamp(m_project.assetFolder / "Localization") != m_textStamp) {
        loadProjectText();
        log("Reloaded the game's text");
    }
}

void Editor::drawPlayerSettingsWindow() {
    ImGui::SetNextWindowSize(ImVec2(560, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Player Settings", &m_showPlayerSettings)) {
        ImGui::End();
        return;
    }
    if (!m_hasProject) {
        ImGui::TextDisabled("Open a project to see its player settings.");
        ImGui::End();
        return;
    }
    shockwave::Localization& text = engine().localization();

    // ---- Preview: editor only ----
    ImGui::SeparatorText("Preview in the viewport");
    ImGui::TextDisabled("Only the editor sees these; they're never saved.");
    bool changed = false;
    if (gui::beginPropertyGrid("preview")) {
        gui::propertyLabel("Interface size");
        bool scaleOn = m_previewUiScale > 0;
        if (ImGui::Checkbox("##scaleon", &scaleOn)) {
            m_previewUiScale = scaleOn ? 1.0f : 0.0f;
            changed = true;
        }
        ImGui::SetItemTooltip("Off shows the shipped interface size");
        ImGui::SameLine();
        ImGui::BeginDisabled(!scaleOn);
        float scale = scaleOn ? m_previewUiScale : engine().preferences().current().uiScale;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##scale", &scale, 0.75f, 2.0f, "%.2fx") && scaleOn) {
            m_previewUiScale = scale;
            changed = true;
        }
        ImGui::EndDisabled();

        gui::propertyLabel("Language");
        const std::string shownLocale = text.locale().empty() ? text.defaultLanguage() : text.locale();
        const std::string current =
            m_previewLanguage
                ? (m_previewLanguage->empty() ? text.defaultLanguage() + " (default)" : *m_previewLanguage)
                : "Shipped (" + shownLocale + ")";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##language", current.c_str())) {
            if (ImGui::Selectable("Shipped", !m_previewLanguage)) {
                m_previewLanguage.reset();
                changed = true;
            }
            for (const std::string& locale : text.locales()) {
                const bool isDefault = locale == text.defaultLanguage();
                const std::string label = isDefault ? locale + " (default)" : locale;
                if (ImGui::Selectable(label.c_str(), m_previewLanguage && *m_previewLanguage == locale)) {
                    m_previewLanguage = locale;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The game's catalogs in Content/Localization. Text a catalog leaves out\n"
                              "shows in the default language, and a key nobody has as [missing: key].");

        gui::propertyLabel("High contrast");
        const char* contrastNames[] = {"Shipped", "Off", "On"};
        int contrast = m_previewContrast + 1;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##contrast", &contrast, contrastNames, 3)) {
            m_previewContrast = contrast - 1;
            changed = true;
        }
        ImGui::EndTable();
    }
    const bool previewing = m_previewUiScale > 0 || m_previewLanguage || m_previewContrast >= 0;
    ImGui::BeginDisabled(!previewing);
    if (gui::button("Stop previewing")) {
        m_previewUiScale = 0;
        m_previewLanguage.reset();
        m_previewContrast = -1;
        changed = true;
    }
    ImGui::EndDisabled();
    if (changed) applyPlayerSettings();
    gui::toolbarNext(gui::buttonWidth("Check layout"));
    if (gui::button("Check layout") && m_gameUI) {
        // The documents in the viewport now, at this size and in this language.
        const auto problems = m_gameUI->layoutProblems();
        for (const auto& problem : problems) warn(problem.file + ": " + problem.message);
        log(problems.empty() ? "The game's UI fits: no text runs past its box or off the screen"
                             : std::format("The game's UI: {} things don't fit (above)", problems.size()));
    }
    ImGui::SetItemTooltip("Look for text that runs past its box or off the screen in the documents shown\n"
                          "in the viewport, as previewed. Try the longest language and the largest size.");

    // ---- The catalogs' state ----
    const std::vector<std::string> checks = text.check();
    size_t errors = 0, warnings = 0;
    for (const std::string& line : checks) (line.rfind("warning: ", 0) == 0 ? warnings : errors)++;
    if (!checks.empty()) {
        ImGui::PushStyleColor(
            ImGuiCol_Text, gui::semanticColor(errors ? gui::currentTheme().error : gui::currentTheme().warning));
        const std::string summary = std::format("Text: {} problems, {} warnings###textchecks", errors, warnings);
        const bool open = ImGui::TreeNode(summary.c_str());
        ImGui::PopStyleColor();
        if (open) {
            for (const std::string& line : checks) ImGui::TextWrapped("%s", line.c_str());
            ImGui::TreePop();
        }
    }

    // ---- Shipped defaults: Config/Preferences.swprefs ----
    ImGui::SeparatorText("What the game ships with");
    const fs::path file = shockwave::projectPreferencesPath(m_project);
    ImGui::TextDisabled("%s", m_project.relative(file).c_str());
    if (!m_shippedPrefsError.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(gui::semanticColor(gui::currentTheme().error), "file has errors");
        ImGui::SetItemTooltip("%s", m_shippedPrefsError.c_str());
    } else if (m_shippedPrefsDirty) {
        ImGui::SameLine();
        ImGui::TextColored(gui::semanticColor(gui::currentTheme().warning), "unsaved");
    }
    gui::alignTrailing(gui::controlGroupWidth({"Revert", "Save"}));
    ImGui::BeginDisabled(!m_shippedPrefsDirty);
    if (gui::button("Revert")) loadShippedPreferences();
    ImGui::SameLine();
    if (gui::button("Save")) {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << shockwave::preferencesText(m_shippedPrefs);
        if (out.flush()) {
            m_shippedPrefsDirty = false;
            m_shippedPrefsError.clear();
            log("Saved " + m_project.relative(file));
        } else {
            error("Couldn't write " + m_project.relative(file));
        }
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("A player's own choices on the Settings page go on top of these.");

    const PlayerPreferences engineDefaults;
    const char* group = "";
    bool open = false;
    for (const PreferenceInfo& info : shockwave::preferenceInfo()) {
        if (std::string(group) != info.group) {
            if (open) ImGui::EndTable();
            group = info.group;
            open = ImGui::CollapsingHeader(group, ImGuiTreeNodeFlags_DefaultOpen) &&
                   gui::beginPropertyGrid((std::string("prefs-") + group).c_str());
        }
        if (!open) continue;
        ImGui::PushID(info.key);
        gui::propertyLabel(info.label);
        ImGui::SetItemTooltip("%s\n\n%s: %s", info.tooltip, info.key, info.values);
        const auto set = m_shippedPrefs.find(info.key);
        const std::string value = set != m_shippedPrefs.end() ? set->second : info.get(engineDefaults);
        const float resetWidth = gui::buttonWidth("Reset");
        ImGui::SetNextItemWidth(-resetWidth - ImGui::GetStyle().ItemSpacing.x);
        std::string newValue;
        if (!info.choices.empty()) {
            if (ImGui::BeginCombo("##v", value.c_str())) {
                for (const char* choice : info.choices)
                    if (ImGui::Selectable(choice, value == choice)) newValue = choice;
                ImGui::EndCombo();
            }
        } else {
            // What's typed, until Enter or leaving the field uses it.
            std::string& edit = m_shippedPrefsEdit.try_emplace(info.key, value).first->second;
            ImGui::InputText("##v", &edit);
            if (ImGui::IsItemDeactivatedAfterEdit()) newValue = edit;
            std::string why;
            PlayerPreferences probe;
            if (!shockwave::setPreference(probe, info.key, edit, why)) {
                ImGui::SameLine(0, 0);
                ImGui::TextColored(gui::semanticColor(gui::currentTheme().error), " !");
                ImGui::SetItemTooltip("%s", why.c_str());
            }
        }
        if (!newValue.empty()) {
            std::string why;
            PlayerPreferences probe;
            if (shockwave::setPreference(probe, info.key, newValue, why)) {
                // The engine's own value needn't be written: the file keeps what's different.
                if (newValue == info.get(engineDefaults))
                    m_shippedPrefs.erase(info.key);
                else
                    m_shippedPrefs[info.key] = newValue;
                m_shippedPrefsEdit[info.key] = newValue;
                m_shippedPrefsDirty = true;
                applyPlayerSettings();
            } else {
                warn(std::string(info.key) + ": " + why);
                m_shippedPrefsEdit[info.key] = value;   // back to what's kept
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(set == m_shippedPrefs.end());
        if (gui::button("Reset")) {
            m_shippedPrefs.erase(info.key);
            m_shippedPrefsEdit[info.key] = info.get(engineDefaults);
            m_shippedPrefsDirty = true;
            applyPlayerSettings();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Back to the engine's default (%s)", info.get(engineDefaults).c_str());
        ImGui::PopID();
    }
    if (open) ImGui::EndTable();
    ImGui::End();
}

}   // namespace editor
