// The shell's Settings page (AppShell.h, docs/Preferences.md): tabs of rows built from
// preferenceInfo() and a few graphics settings, edited as a draft that Apply uses and saves.
// Every row is "< value >": buttons a mouse, the arrow keys and a gamepad all drive the same
// way, with no sliders to drag.
#include <GameFramework/AppShell.h>

#include <Core/Log.h>
#include <Engine/Engine.h>
#include <Engine/Localization.h>
#include <Input/Input.h>
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
    m_draftBindings.clear();
    if (m_input)
        for (const auto& [action, bindings] : m_input->actions())
            if (rebindable(action)) m_draftBindings[action] = bindings;
    stopListening();
    m_tab = "display";
}

// ---- Controls: rebinding ------------------------------------------------------------------

bool AppShell::rebindable(std::string_view action) {
    return action.rfind("ui_", 0) != 0 && action != kPauseAction && !action.empty();
}

std::filesystem::path AppShell::controlsFile() const {
    if (!m_setup.controlsFile.empty()) return m_setup.controlsFile;
    if (m_preferences && !m_preferences->profileFile().empty())
        return m_preferences->profileFile().parent_path() / "Controls.swinput";
    return {};
}

void AppShell::stopListening() {
    if (!m_listen) return;
    m_listen.reset();
    m_ui->setInputEnabled(m_engine != nullptr);
}

// Another action the same press would trigger: the game's (in the draft) or the menus'.
std::string AppShell::conflictOf(const std::string& action, const InputBinding& binding) const {
    for (const auto& [other, bindings] : m_draftBindings)
        if (other != action && std::find(bindings.begin(), bindings.end(), binding) != bindings.end()) return other;
    if (m_input)
        for (const auto& [other, bindings] : m_input->actions())
            if (!rebindable(other) && other.rfind("ui_", 0) != 0 &&
                std::find(bindings.begin(), bindings.end(), binding) != bindings.end())
                return other;
    return {};
}

std::string AppShell::actionLabel(std::string_view action) const {
    const std::string key = "action." + std::string(action);
    if (m_text->has(key)) return tr(key);
    std::string label(action);   // "move_forward" -> "Move forward"
    std::replace(label.begin(), label.end(), '_', ' ');
    if (!label.empty()) label[0] = char(std::toupper((unsigned char)label[0]));
    return label;
}

void AppShell::listen(double frameDt) {
    Input& input = *m_input;
    Listen& l = *m_listen;
    l.secondsLeft -= frameDt;
    if (l.secondsLeft <= 0) {
        stopListening();
        m_message = tr("shell.listen_timed_out");
        return;
    }
    // The press that asked to listen was this frame's UI update, after this: it's never
    // taken for the binding.
    const auto& pressed = input.lastPressed();
    if (!pressed) return;
    using S = InputBinding::Source;
    const InputBinding escape{.source = S::Key, .code = uint16_t(Key::Escape)};
    const InputBinding start{.source = S::GamepadButton, .code = uint16_t(GamepadButton::Start)};
    if (*pressed == escape || *pressed == start) {   // these always cancel; they can't be bound
        stopListening();
        input.holdUntilReleased();
        m_message.clear();
        return;
    }
    const bool pad = bindingIsGamepad(*pressed);
    const bool keyboard = pressed->source == S::Key || pressed->source == S::Mouse;
    if ((l.gamepad && !pad) || (!l.gamepad && !keyboard)) return;   // the other device: keep listening
    // The new one replaces the action's bindings on that device; others stay.
    InputBinding binding = *pressed;
    if (binding.source == S::GamepadAxis && binding.range == InputBinding::Range::Full)
        binding.range = InputBinding::Range::Positive;
    auto& list = m_draftBindings[l.action];
    std::erase_if(list, [&](const InputBinding& b) {
        return l.gamepad ? bindingIsGamepad(b) : (b.source == S::Key || b.source == S::Mouse);
    });
    list.push_back(binding);
    const std::string action = l.action;
    stopListening();
    input.holdUntilReleased();   // the press doesn't also act on the page or the game
    const std::string other = conflictOf(action, binding);
    m_message = other.empty()
                    ? std::string()
                    : tr("shell.binding_conflict", {{"binding", bindingLabel(binding)}, {"other", actionLabel(other)}});
}

// ---- A controller unplugged ---------------------------------------------------------------

void AppShell::watchControllers() {
    Input& input = *m_input;
    if (input.devicesVersion() == m_devicesVersion) return;
    m_devicesVersion = input.devicesVersion();
    int pads = 0;
    for (const InputDevice& d : input.devices()) pads += d.kind == InputDevice::Kind::Gamepad;
    const bool lost = pads < m_pads && input.lastUsedGamepad();
    const bool back = pads > m_pads && m_padLost;
    m_pads = pads;
    if (lost && m_setup.pauseOnControllerLoss) {
        // Mid-level, offline: pause, so nothing happens to a player who can't play. Online
        // the screen still shows; the match goes on.
        if (m_screen == ShellScreen::None) pause();
        if (m_screen != ShellScreen::None) {
            m_padLost = true;
            m_message = tr("shell.controller_lost");
            SW_LOG(LogGame, Info, "Shell: the controller in use was unplugged");
        }
    } else if (back) {
        m_padLost = false;
        m_message = tr("shell.controller_back");
    }
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
                       command == "keep_display" || command == "revert_display" || command == "bind_key" ||
                       command == "bind_pad" || command == "cancel_listen" || command == "reset_bindings";
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
    if (command == "cancel_listen") {
        if (!m_listen) return refuse(why, "Nothing is waiting for a press");
        stopListening();
        return done();
    }
    if (m_listen) return refuse(why, tr("shell.listening_busy"));
    if (command == "bind_key" || command == "bind_pad") {
        if (!m_input || !m_draftBindings.count(std::string(argument)))
            return refuse(why, "\"" + std::string(argument) + "\" isn't a control here");
        m_listen = Listen{std::string(argument), command == "bind_pad"};
        // The page waits for the press; its buttons don't take it.
        m_ui->setInputEnabled(false);
        return done();
    }
    if (command == "reset_bindings") {
        if (!m_input) return refuse(why, "No controls here");
        const auto project = m_input->projectActions();
        for (auto& [action, bindings] : m_draftBindings) {
            const auto it = project.find(action);
            bindings = it == project.end() ? std::vector<InputBinding>{} : it->second;
        }
        return done();
    }
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
    // The player's bindings: the actions whose bindings differ from the project's.
    InputBindings controls;
    bool controlsChanged = false;
    if (m_input) {
        const auto project = m_input->projectActions();
        controls.deadzone = m_input->currentBindings().deadzone;
        for (const auto& [action, bindings] : m_draftBindings) {
            const auto it = project.find(action);
            if (it != project.end() && it->second == bindings) continue;
            for (const InputBinding& b : bindings) controls.binds.emplace_back(action, b);
        }
        controlsChanged = !(m_input->playerBindings().binds == controls.binds);
        if (controlsChanged) m_input->setPlayerBindings(controls);
    }
    std::string saveError;
    m_preferences->save(m_draft, saveError);
    if (m_graphics && saveError.empty() && !m_graphics->file().empty()) m_graphics->save(m_draftGraphics, saveError);
    if (controlsChanged && saveError.empty()) {
        const std::filesystem::path file = controlsFile();
        std::error_code ec;
        if (file.empty())
            saveError = "No profile to save controls in";
        else if (controls.binds.empty())
            std::filesystem::remove(file, ec);
        else
            saveInputBindings(file, controls, saveError);
    }
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
    bool bindingsChanged = false;
    UIValue bindings = UIValue::list();
    if (reason.empty() && m_input) {
        const auto& now = m_input->actions();
        const InputDevice* pad = nullptr;
        for (const InputDevice& d : m_input->devices())
            if (d.kind == InputDevice::Kind::Gamepad) {
                pad = &d;
                break;
            }
        for (const auto& [action, list] : m_draftBindings) {
            const auto it = now.find(action);
            bindingsChanged |= it == now.end() || it->second != list;
            std::string keyboard, gamepad, conflict;
            for (const InputBinding& b : list) {
                std::string& into = bindingIsGamepad(b) ? gamepad : keyboard;
                if (b.source == InputBinding::Source::JoystickAxis ||
                    b.source == InputBinding::Source::JoystickButton || b.source == InputBinding::Source::JoystickHat)
                    continue;
                into += (into.empty() ? "" : ", ") + bindingLabel(b, pad);
                if (conflict.empty())
                    if (const std::string other = conflictOf(action, b); !other.empty())
                        conflict =
                            tr("shell.binding_conflict",
                               {{"binding", bindingLabel(b, pad)}, {"other", actionLabel(other)}});
            }
            bindings.push(UIValue::record(
                {{"action", action},
                 {"label", actionLabel(action)},
                 {"keyboard", keyboard.empty() ? tr("shell.unbound") : keyboard},
                 {"gamepad", gamepad.empty() ? tr("shell.unbound") : gamepad},
                 {"conflict", conflict}}));
        }
    }
    ui.setValue("settings_bindings", std::move(bindings));
    ui.setValue("listening", m_listen.has_value());
    ui.setValue(
        "listening_text", std::string_view(
                              m_listen ? tr(m_listen->gamepad ? "shell.listen_pad" : "shell.listen_key",
                                            {{"action", actionLabel(m_listen->action)},
                                             {"seconds", std::to_string(int(std::ceil(m_listen->secondsLeft)))}})
                                       : std::string()));
    const bool changed = reason.empty() && (!(m_draft == m_preferences->current()) || bindingsChanged ||
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
