// The shell's Settings page (AppShell.h, docs/Preferences.md): tabs of rows built from
// preferenceInfo() and a few graphics settings, edited as a draft that Apply uses and saves.
// Every row is "< value >": buttons a mouse, the arrow keys and a gamepad all drive the same
// way, with no sliders to drag.
#include <GameFramework/AppShell.h>

#include <Core/Log.h>
#include <Engine/Engine.h>
#include <Engine/Localization.h>
#include <Platform/Window.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace shockwave {

namespace {

const char* kTabs[] = {"display", "graphics", "audio", "controls", "accessibility", "language"};
constexpr const char* kGraphicsPrefix = "graphics.";
constexpr int kFrameLimits[] = {0, 30, 60, 120, 144, 165, 240};

// Which tab a preference shows on: its group, with Camera folded into Controls.
std::string tabOf(const PreferenceInfo& info) {
    std::string group = info.group;
    std::transform(group.begin(), group.end(), group.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return group == "camera" ? "controls" : group;
}

bool rangeOf(const char* values, float& lo, float& hi) {
    const std::string text = values;
    const size_t dots = text.find("..");
    if (dots == std::string::npos) return false;
    size_t start = dots;
    while (start > 0 && (std::isdigit((unsigned char)text[start - 1]) || text[start - 1] == '.')) --start;
    try {
        lo = std::stof(text.substr(start, dots - start));
        hi = std::stof(text.substr(dots + 2));
    } catch (...) {
        return false;
    }
    return true;
}

float stepOf(std::string_view key) {
    if (key == "stick_dead_zone") return 0.05f;
    if (key == "field_of_view") return 5;
    if (key == "ui_scale" || key == "subtitle_scale") return 0.25f;
    return 0.1f;
}

std::string numberText(double value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.4g", value);
    return text;
}

std::string percent(double value) { return std::to_string(int(std::lround(value * 100))) + "%"; }

// A number one step along: snapped to the step from the bottom of its range, then clamped.
float stepped(float value, float lo, float hi, float step, int direction) {
    const float steps = std::round((value - lo) / step) + float(direction);
    return std::clamp(lo + steps * step, lo, hi);
}

// The resolutions the Resolution row cycles through: the game's own, then the display's.
std::vector<std::pair<int, int>> resolutionChoices(Engine* engine, int width, int height) {
    std::vector<std::pair<int, int>> out = {{0, 0}};
    if (engine)
        for (const DisplayResolution& r : engine->window().resolutions())
            if (std::find(out.begin(), out.end(), std::pair{r.width, r.height}) == out.end())
                out.emplace_back(r.width, r.height);
    if (std::find(out.begin(), out.end(), std::pair{width, height}) == out.end()) out.emplace_back(width, height);
    return out;
}

template <class T>
int cycle(const std::vector<T>& choices, const T& now, int direction) {
    const auto it = std::find(choices.begin(), choices.end(), now);
    const int n = int(choices.size());
    const int at = it == choices.end() ? (direction > 0 ? -1 : 0) : int(it - choices.begin());
    return ((at + direction) % n + n) % n;
}

}   // namespace

std::string AppShell::tr(std::string_view key) const { return m_text->text(key); }

std::string AppShell::tr(std::string_view key, const std::vector<std::pair<std::string, std::string>>& values) const {
    return m_text->format(key, values);
}

// The game's own default (""), then every other locale with a catalog.
std::vector<std::string> AppShell::languageChoices() const {
    std::vector<std::string> choices = {""};
    for (const std::string& locale : m_text->locales())
        if (locale != m_text->defaultLanguage()) choices.push_back(locale);
    return choices;
}

std::string AppShell::settingsReason() const {
    if (!m_setup.settings) return tr("shell.settings_off");
    if (!m_preferences) return tr("shell.settings_unavailable");
    return {};
}

void AppShell::openSettings() {
    if (m_preferences) m_draft = m_preferences->current();
    if (m_graphics) m_draftGraphics = m_graphics->requested();
    m_tab = "display";
}

std::vector<AppShell::SettingRow> AppShell::settingRows(std::string_view tab) const {
    std::vector<SettingRow> rows;
    auto add = [&](std::string key) {
        SettingRow row;
        row.label = tr("shell.pref." + key);
        if (const PreferenceInfo* info = findPreference(key)) row.tooltip = info->tooltip;
        row.key = std::move(key);
        rows.push_back(std::move(row));
    };
    if (tab == "graphics") {
        if (!m_graphics) return rows;
        for (const char* key : {"preset", "render_scale", "vsync", "frame_limit"})
            add(std::string(kGraphicsPrefix) + key);
        return rows;
    }
    for (const PreferenceInfo& info : preferenceInfo()) {
        const std::string key = info.key;
        if (key == "window_height" || tabOf(info) != tab) continue;
        if (key == "window_width") {
            add("resolution");
            rows.back().tooltip = "The window's size, or fullscreen's resolution";
            continue;
        }
        add(key);
        if (key == "language" && languageChoices().size() < 2) {
            rows.back().canChange = false;
            rows.back().reason = tr("shell.one_language");
        }
    }
    return rows;
}

std::string AppShell::settingValue(std::string_view key) const {
    if (key == "resolution") {
        if (m_draft.windowWidth <= 0 || m_draft.windowHeight <= 0) return tr("shell.value.default");
        return std::to_string(m_draft.windowWidth) + " x " + std::to_string(m_draft.windowHeight);
    }
    if (key.rfind(kGraphicsPrefix, 0) == 0) {
        const std::string_view g = key.substr(std::string_view(kGraphicsPrefix).size());
        if (g == "preset") {
            const auto preset = matchingGraphicsPreset(m_draftGraphics);
            return tr(std::string("shell.value.") + (preset ? graphicsPresetName(*preset) : "custom"));
        }
        if (g == "render_scale") return percent(m_draftGraphics.renderScale);
        if (g == "vsync") return tr(m_draftGraphics.vsync ? "shell.value.on" : "shell.value.off");
        if (g == "frame_limit")
            return m_draftGraphics.frameLimit <= 0 ? tr("shell.value.unlimited")
                                                   : numberText(m_draftGraphics.frameLimit) + " fps";
        return {};
    }
    const PreferenceInfo* info = findPreference(std::string(key));
    if (!info) return {};
    const std::string value = info->get(m_draft);
    if (key == "language") return value.empty() ? tr("shell.value.default") : value;
    if (!info->choices.empty()) {
        if (value == "true") return tr("shell.value.on");
        if (value == "false") return tr("shell.value.off");
        const std::string name = "shell.value." + value;
        return m_text->has(name) ? tr(name) : value;
    }
    const float number = std::stof(value);
    if (key == "field_of_view") return number == 0 ? tr("shell.value.default") : numberText(number) + "°";
    if (key == "look_sensitivity") return numberText(number);
    return percent(number);
}

bool AppShell::stepSetting(std::string_view key, int direction, std::string* why) {
    std::string error;
    if (key == "resolution") {
        const auto choices = resolutionChoices(m_engine, m_draft.windowWidth, m_draft.windowHeight);
        const auto& next = choices[cycle(choices, std::pair{m_draft.windowWidth, m_draft.windowHeight}, direction)];
        m_draft.windowWidth = next.first;
        m_draft.windowHeight = next.second;
        return true;
    }
    if (key.rfind(kGraphicsPrefix, 0) == 0) {
        if (!m_graphics) return refuse(why, "\"" + std::string(key) + "\" isn't a setting here");
        const std::string g(key.substr(std::string_view(kGraphicsPrefix).size()));
        GraphicsSettings next = m_draftGraphics;
        if (g == "preset") {
            const std::vector<int> presets = {0, 1, 2, 3};
            const auto now = matchingGraphicsPreset(next);
            next = graphicsPreset(GraphicsPreset(presets[cycle(presets, now ? int(*now) : -1, direction)]), next);
        } else if (g == "render_scale") {
            next.renderScale = stepped(next.renderScale, 0.5f, 2, 0.1f, direction);
        } else if (g == "vsync") {
            next.vsync = !next.vsync;
        } else if (g == "frame_limit") {
            const std::vector<int> limits(std::begin(kFrameLimits), std::end(kFrameLimits));
            next.frameLimit = limits[cycle(limits, int(std::lround(next.frameLimit)), direction)];
        } else {
            return refuse(why, "\"" + std::string(key) + "\" isn't a setting here");
        }
        if (!validateGraphicsSettings(next, error)) return refuse(why, error);
        m_draftGraphics = next;
        return true;
    }
    const PreferenceInfo* info = findPreference(std::string(key));
    if (!info || key == "window_width" || key == "window_height")
        return refuse(why, "\"" + std::string(key) + "\" isn't a setting here");
    PlayerPreferences next = m_draft;
    std::string value;
    if (key == "language") {
        const std::vector<std::string> choices = languageChoices();
        value = choices[cycle(choices, m_draft.language, direction)];
    } else if (!info->choices.empty()) {
        std::vector<std::string> choices;
        std::vector<std::pair<std::string, std::string>> unavailable;
        if (m_preferences) unavailable = m_preferences->unavailable();
        for (const char* choice : info->choices) {
            const bool off =
                std::any_of(unavailable.begin(), unavailable.end(), [&](const auto& u) { return u.first == choice; });
            if (!off || info->get(m_draft) == choice) choices.emplace_back(choice);
        }
        value = choices[cycle(choices, info->get(m_draft), direction)];
    } else {
        float lo = 0, hi = 1;
        rangeOf(info->values, lo, hi);
        const float now = std::stof(info->get(m_draft));
        float next = 0;
        if (key == "field_of_view") {   // 0 (the game's own), then 50..120
            next = now == 0 ? (direction > 0 ? lo : 0) : stepped(now, lo, hi, stepOf(key), direction);
            if (now == lo && direction < 0) next = 0;
        } else {
            next = stepped(now, lo, hi, stepOf(key), direction);
        }
        value = numberText(next);
    }
    if (!info->set(next, value, error)) return refuse(why, error);
    m_draft = next;
    return true;
}

bool AppShell::settingsCommand(std::string_view command, std::string_view argument, std::string* why, bool& handled) {
    handled = true;
    auto done = [this] {
        m_message.clear();
        publish();
        return true;
    };
    if (command == "settings") {
        openSettings();
        m_page = "settings";
        return done();
    }
    const bool known = command == "settings_tab" || command == "pref_next" || command == "pref_prev" ||
                       command == "apply_settings" || command == "cancel_settings" || command == "reset_settings" ||
                       command == "keep_display" || command == "revert_display";
    if (!known) {
        handled = false;
        return false;
    }
    if (m_page != "settings") return refuse(why, "The Settings page isn't open");
    if (const std::string reason = settingsReason(); !reason.empty()) return refuse(why, reason);
    if (command == "keep_display" || command == "revert_display") {
        if (!m_preferences->displayPending()) return refuse(why, "No display change is waiting");
        if (command == "keep_display") {
            m_preferences->keepDisplay();
        } else {
            m_preferences->revertDisplay();
        }
        m_draft = m_preferences->current();
        return done();
    }
    if (m_preferences->displayPending()) return refuse(why, tr("shell.keep_display_title"));
    if (command == "settings_tab") {
        for (const char* tab : kTabs)
            if (argument == tab && (argument != "graphics" || m_graphics)) {
                m_tab = tab;
                return done();
            }
        return refuse(why, "No \"" + std::string(argument) + "\" tab");
    }
    if (command == "pref_next" || command == "pref_prev") {
        if (!stepSetting(argument, command == "pref_next" ? 1 : -1, why)) return false;
        return done();
    }
    if (command == "cancel_settings") {
        openSettings();
        m_page = "main";
        return done();
    }
    if (command == "reset_settings") {
        m_draft = m_preferences->defaults();
        if (m_graphics) m_draftGraphics = m_graphics->defaults();
        return done();
    }
    // apply_settings: everything validates before anything changes; then each is saved.
    std::string error;
    if (!validatePreferences(m_draft, error) || (m_graphics && !validateGraphicsSettings(m_draftGraphics, error)))
        return refuse(why, tr("shell.settings_failed", {{"why", error}}));
    if (m_graphics && !(m_draftGraphics == m_graphics->requested()) && !m_graphics->apply(m_draftGraphics, error))
        return refuse(why, tr("shell.settings_failed", {{"why", error}}));
    if (!m_preferences->apply(m_draft, error)) {
        m_draft = m_preferences->current();
        return refuse(why, tr("shell.settings_failed", {{"why", error}}));
    }
    std::string saveError;
    m_preferences->save(m_draft, saveError);
    if (m_graphics && saveError.empty() && !m_graphics->file().empty()) m_graphics->save(m_draftGraphics, saveError);
    m_message = saveError.empty() ? tr("shell.settings_saved") : tr("shell.settings_not_saved", {{"why", saveError}});
    if (!saveError.empty()) SW_LOG(LogGame, Warning, "Settings: {}", m_message);
    publish();
    return true;
}

void AppShell::publishSettings() {
    GameUI& ui = *m_ui;
    const std::string reason = settingsReason();
    ui.setValue("can_settings", reason.empty());
    ui.setValue("settings_reason", std::string_view(reason));
    ui.setValue("settings_tab", std::string_view(m_tab));
    UIValue tabs = UIValue::list();
    for (const char* tab : kTabs)
        if (std::string_view(tab) != "graphics" || m_graphics)
            tabs.push(UIValue::record({{"id", std::string(tab)}, {"label", tr(std::string("shell.tab.") + tab)}}));
    ui.setValue("settings_tabs", std::move(tabs));
    UIValue rows = UIValue::list();
    if (reason.empty())
        for (const SettingRow& row : settingRows(m_tab))
            rows.push(UIValue::record(
                {{"key", row.key},
                 {"label", row.label},
                 {"value", settingValue(row.key)},
                 {"tooltip", row.tooltip},
                 {"can_change", row.canChange},
                 {"reason", row.reason}}));
    ui.setValue("settings_rows", std::move(rows));
    const bool changed = reason.empty() && (!(m_draft == m_preferences->current()) ||
                                            (m_graphics && !(m_draftGraphics == m_graphics->requested())));
    ui.setValue("settings_changed", changed);
    const bool pending = reason.empty() && m_preferences->displayPending();
    ui.setValue("display_pending", pending);
    const int seconds = pending ? int(std::ceil(m_preferences->displaySecondsLeft())) : 0;
    ui.setValue(
        "display_countdown",
        std::string_view(pending ? tr("shell.keep_display_countdown", {{"seconds", std::to_string(seconds)}}) : ""));
}

}   // namespace shockwave
