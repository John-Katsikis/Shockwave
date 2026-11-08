// Player preferences: the table, the file, the layers and applying them (Preferences.h).
#include <Engine/Preferences.h>

#include <Core/Log.h>
#include <Engine/Engine.h>
#include <Projects/Project.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

namespace shockwave {

namespace fs = std::filesystem;

namespace {

SW_LOG_CATEGORY(LogPreferences);

std::string number(float value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

bool parseFloat(const std::string& text, float min, float max, float& out, std::string& error) {
    float value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        error = "\"" + text + "\" isn't a number";
        return false;
    }
    if (value < min || value > max) {
        error = text + " is out of range (" + number(min) + " to " + number(max) + ")";
        return false;
    }
    out = value;
    return true;
}

bool parseInt(const std::string& text, int min, int max, int& out, std::string& error) {
    int value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        error = "\"" + text + "\" isn't a whole number";
        return false;
    }
    if (value < min || value > max) {
        error = text + " is out of range (" + std::to_string(min) + " to " + std::to_string(max) + ")";
        return false;
    }
    out = value;
    return true;
}

bool parseBool(const std::string& text, bool& out, std::string& error) {
    if (text == "true" || text == "on")
        out = true;
    else if (text == "false" || text == "off")
        out = false;
    else {
        error = "\"" + text + "\" isn't true or false";
        return false;
    }
    return true;
}

// A locale: "en", "pt-BR", "zh-Hans": letters, digits and '-', 2 to 35 of them.
bool validLanguage(const std::string& text) {
    if (text.empty()) return true;
    if (text.size() < 2 || text.size() > 35 || !std::isalpha(static_cast<unsigned char>(text[0]))) return false;
    return std::all_of(
        text.begin(), text.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-'; });
}

#define SW_FLOAT_PREF(member, min, max)                                                                                \
    [](const PlayerPreferences& p) { return number(p.member); },                                                       \
        [](PlayerPreferences& p, const std::string& v, std::string& e) {                                               \
            return parseFloat(v, min, max, p.member, e);                                                               \
        }
#define SW_BOOL_PREF(member)                                                                                           \
    [](const PlayerPreferences& p) { return std::string(p.member ? "true" : "false"); },                               \
        [](PlayerPreferences& p, const std::string& v, std::string& e) { return parseBool(v, p.member, e); }

std::vector<PreferenceInfo> makeInfo() {
    const std::vector<const char*> onOff = {"false", "true"};
    return {
        {"window_mode",
         "Window mode",
         "Display",
         "Windowed, borderless (the whole display at its own resolution) or fullscreen (the display switches to "
         "the resolution)",
         "windowed, borderless, fullscreen",
         {"windowed", "borderless", "fullscreen"},
         true,
         [](const PlayerPreferences& p) { return std::string(windowModeName(p.windowMode)); },
         [](PlayerPreferences& p, const std::string& v, std::string& e) {
             for (WindowMode m : {WindowMode::Windowed, WindowMode::Borderless, WindowMode::Fullscreen})
                 if (v == windowModeName(m)) {
                     p.windowMode = m;
                     return true;
                 }
             e = "\"" + v + "\" isn't windowed, borderless or fullscreen";
             return false;
         }},
        {"window_width",
         "Width",
         "Display",
         "The window's width, or fullscreen's resolution; 0 for the game's own",
         "0, or 320..16384",
         {},
         true,
         [](const PlayerPreferences& p) { return std::to_string(p.windowWidth); },
         [](PlayerPreferences& p, const std::string& v, std::string& e) {
             return parseInt(v, 0, 16384, p.windowWidth, e) &&
                    (p.windowWidth == 0 || p.windowWidth >= 320 || (e = "the width is 0 or at least 320", false));
         }},
        {"window_height",
         "Height",
         "Display",
         "The window's height, or fullscreen's resolution; 0 for the game's own",
         "0, or 240..16384",
         {},
         true,
         [](const PlayerPreferences& p) { return std::to_string(p.windowHeight); },
         [](PlayerPreferences& p, const std::string& v, std::string& e) {
             return parseInt(v, 0, 16384, p.windowHeight, e) &&
                    (p.windowHeight == 0 || p.windowHeight >= 240 || (e = "the height is 0 or at least 240", false));
         }},
        {"master_volume",
         "Master volume",
         "Audio",
         "Everything the game plays",
         "0..1",
         {},
         false,
         SW_FLOAT_PREF(masterVolume, 0, 1)},
        {"effects_volume",
         "Effects",
         "Audio",
         "Sound effects, on top of the master volume",
         "0..1",
         {},
         false,
         SW_FLOAT_PREF(effectsVolume, 0, 1)},
        {"music_volume",
         "Music",
         "Audio",
         "Music, on top of the master volume",
         "0..1",
         {},
         false,
         SW_FLOAT_PREF(musicVolume, 0, 1)},
        {"ui_volume",
         "Interface",
         "Audio",
         "Menu and HUD sounds, on top of the master volume",
         "0..1",
         {},
         false,
         SW_FLOAT_PREF(uiVolume, 0, 1)},
        {"voice_volume",
         "Voices",
         "Audio",
         "Speech, on top of the master volume",
         "0..1",
         {},
         false,
         SW_FLOAT_PREF(voiceVolume, 0, 1)},
        {"mute_when_unfocused", "Mute in the background", "Audio", "Silence while another window has focus",
         "true, false", onOff, false, SW_BOOL_PREF(muteWhenUnfocused)},
        {"look_sensitivity",
         "Look sensitivity",
         "Controls",
         "How fast the mouse and right stick turn the view",
         "0.05..5",
         {},
         false,
         SW_FLOAT_PREF(lookSensitivity, 0.05f, 5)},
        {"invert_look_y", "Invert look up and down", "Controls", "Pushing forward looks down", "true, false", onOff,
         false, SW_BOOL_PREF(invertLookY)},
        {"stick_dead_zone",
         "Stick dead zone",
         "Controls",
         "How far a stick moves before it counts; raise it if a worn stick drifts",
         "0..0.9",
         {},
         false,
         SW_FLOAT_PREF(stickDeadZone, 0, 0.9f)},
        {"toggle_hold", "Toggle instead of hold", "Controls",
         "Actions you'd hold, like sprint, toggle with one press instead", "true, false", onOff, false,
         SW_BOOL_PREF(toggleHold)},
        {"ui_scale",
         "Interface size",
         "Accessibility",
         "The menus' and HUD's size",
         "0.75..2",
         {},
         false,
         SW_FLOAT_PREF(uiScale, 0.75f, 2)},
        {"high_contrast", "High contrast", "Accessibility", "Stronger colours and outlines in menus and the HUD",
         "true, false", onOff, false, SW_BOOL_PREF(highContrast)},
        {"subtitles", "Subtitles", "Accessibility", "Text for speech", "true, false", onOff, false,
         SW_BOOL_PREF(subtitles)},
        {"subtitle_scale",
         "Subtitle size",
         "Accessibility",
         "The subtitles' size",
         "0.75..3",
         {},
         false,
         SW_FLOAT_PREF(subtitleScale, 0.75f, 3)},
        {"reduce_flashes", "Reduce flashes", "Accessibility", "Tone down full-screen flashes", "true, false", onOff,
         false, SW_BOOL_PREF(reduceFlashes)},
        {"camera_shake",
         "Camera shake",
         "Accessibility",
         "How much the camera shakes; 0 turns it off",
         "0..1",
         {},
         false,
         SW_FLOAT_PREF(cameraShake, 0, 1)},
        {"field_of_view",
         "Field of view",
         "Camera",
         "How wide the view is, in degrees from the top of the screen to the bottom; 0 for the game's own. The "
         "game may narrow the range",
         "0, or 50..120",
         {},
         false,
         [](const PlayerPreferences& p) { return number(p.fieldOfView); },
         [](PlayerPreferences& p, const std::string& v, std::string& e) {
             float value = 0;
             if (!parseFloat(v, 0, 120, value, e)) return false;
             if (value != 0 && value < 50) {
                 e = v + " is out of range (0, or 50 to 120)";
                 return false;
             }
             p.fieldOfView = value;
             return true;
         }},
        {"language",
         "Language",
         "Language",
         "The game's text; empty for the game's own default language",
         "a locale like en, fr or pt-BR",
         {},
         false,
         [](const PlayerPreferences& p) { return p.language; },
         [](PlayerPreferences& p, const std::string& v, std::string& e) {
             if (!validLanguage(v)) {
                 e = "\"" + v + "\" isn't a locale like en, fr or pt-BR";
                 return false;
             }
             p.language = v;
             return true;
         }},
    };
}

#undef SW_FLOAT_PREF
#undef SW_BOOL_PREF

std::string quoted(const std::string& value) {
    const bool plain = !value.empty() && value.find_first_of(" \t\"#") == std::string::npos;
    return plain ? value : "\"" + value + "\"";
}

bool writeFileAtomically(const fs::path& file, const std::string& text, std::string& error) {
    std::error_code ec;
    if (!file.parent_path().empty()) fs::create_directories(file.parent_path(), ec);
    const fs::path temp =
        file.string() + "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc | std::ios::binary);
        out << text;
        out.close();
        if (!out) {
            fs::remove(temp, ec);
            error = file.string() + ": couldn't write it";
            return false;
        }
    }
    fs::rename(temp, file, ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

bool readFile(const fs::path& file, std::string& text) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream all;
    all << in.rdbuf();
    text = all.str();
    return true;
}

}   // namespace

// ---- The table --------------------------------------------------------------------------

const char* preferenceLayerName(PreferenceLayer layer) {
    switch (layer) {
    case PreferenceLayer::Default: return "default";
    case PreferenceLayer::Shipped: return "shipped";
    case PreferenceLayer::Profile: return "profile";
    case PreferenceLayer::Session: return "session";
    }
    return "default";
}

const std::vector<PreferenceInfo>& preferenceInfo() {
    static const std::vector<PreferenceInfo> info = makeInfo();
    return info;
}

const PreferenceInfo* findPreference(const std::string& key) {
    for (const auto& info : preferenceInfo())
        if (key == info.key) return &info;
    return nullptr;
}

bool setPreference(PlayerPreferences& prefs, const std::string& key, const std::string& value, std::string& error) {
    const PreferenceInfo* info = findPreference(key);
    if (!info) {
        error = "unknown preference " + key;
        return false;
    }
    PlayerPreferences changed = prefs;
    std::string why;
    if (!info->set(changed, value, why)) {
        error = key + ": " + why;
        return false;
    }
    prefs = changed;
    return true;
}

bool validatePreferences(const PlayerPreferences& prefs, std::string& error) {
    // Every value through its own parser: the same ranges as the file.
    PlayerPreferences scratch;
    for (const auto& info : preferenceInfo()) {
        std::string why;
        if (!info.set(scratch, info.get(prefs), why)) {
            error = std::string(info.key) + ": " + why;
            return false;
        }
    }
    if ((prefs.windowWidth == 0) != (prefs.windowHeight == 0)) {
        error = "window_width and window_height are both 0 (the game's own size) or both set";
        return false;
    }
    error.clear();
    return true;
}

// ---- The file -----------------------------------------------------------------------------

bool parsePreferences(
    const std::string& text, const std::string& shown, std::map<std::string, std::string>& keys, std::string& error) {
    std::map<std::string, std::string> result;
    PlayerPreferences check;
    std::istringstream in(text);
    std::string line;
    int number = 0;
    bool header = false;
    auto fail = [&](const std::string& message) {
        error = shown + ":" + std::to_string(number) + ": " + message;
        return false;
    };
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // key, then one value: bare, or "quoted" (which may hold spaces and #).
        size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos || line[i] == '#') continue;
        const size_t keyEnd = line.find_first_of(" \t", i);
        const std::string key = line.substr(i, keyEnd == std::string::npos ? std::string::npos : keyEnd - i);
        std::string value;
        bool hasValue = false;
        size_t rest = keyEnd == std::string::npos ? line.size() : line.find_first_not_of(" \t", keyEnd);
        if (rest != std::string::npos && rest < line.size() && line[rest] != '#') {
            hasValue = true;
            if (line[rest] == '"') {
                const size_t close = line.find('"', rest + 1);
                if (close == std::string::npos) return fail("a quote that doesn't close");
                value = line.substr(rest + 1, close - rest - 1);
                rest = close + 1;
            } else {
                const size_t end = line.find_first_of(" \t#", rest);
                value = line.substr(rest, end == std::string::npos ? std::string::npos : end - rest);
                rest = end == std::string::npos ? line.size() : end;
            }
            const size_t after = line.find_first_not_of(" \t", rest);
            if (after != std::string::npos && line[after] != '#') return fail("expected a key and one value");
        }
        if (!hasValue) return fail("expected a key and one value");
        if (!header) {
            if (key != "swprefs") return fail("expected swprefs 1");
            if (value != "1") return fail("version " + value + " isn't supported (this engine reads swprefs 1)");
            header = true;
            continue;
        }
        if (result.count(key)) return fail("duplicate preference " + key);
        std::string why;
        if (!setPreference(check, key, value, why)) return fail(why);
        result[key] = value;
    }
    if (!header) {
        number = std::max(number, 1);
        return fail("expected swprefs 1");
    }
    keys = std::move(result);
    error.clear();
    return true;
}

std::string preferencesText(const std::map<std::string, std::string>& keys) {
    std::string text = "swprefs 1\n";
    for (const auto& info : preferenceInfo())   // in table order, whatever the map's
        if (const auto it = keys.find(info.key); it != keys.end())
            text += std::string(info.key) + " " + quoted(it->second) + "\n";
    return text;
}

std::filesystem::path projectPreferencesPath(const Project& project) {
    return project.root / "Config" / "Preferences.swprefs";
}

std::string preferencesSchema() {
    std::string text = R"(# Player preferences schema

Generated by `player --preferences-schema docs/PreferencesSchema.md`.

Files start with `swprefs 1`. One key and value per line; `#` starts a comment, and a
value with spaces is quoted. Unknown keys, duplicate keys, values out of range and NaN are
errors, with the line. Keys left out come from the layer below: the engine's defaults, the
project's `Config/Preferences.swprefs`, the player's own file, then `--pref key=value`. See
[Preferences](Preferences.md). Graphics preferences are in [GraphicsSchema](GraphicsSchema.md).

*Display* keys are kept within 15 seconds of a change, or put back.

| Key | Group | Values | Default | What it does |
|---|---|---|---|---|
)";
    const PlayerPreferences defaults;
    for (const auto& info : preferenceInfo()) {
        std::string value = info.get(defaults);
        if (value.empty()) value = "(empty)";
        text += "| `" + std::string(info.key) + "` | " + info.group + (info.display ? " *(display)*" : "") + " | " +
                info.values + " | `" + value + "` | " + info.tooltip + " |\n";
    }
    return text;
}

// ---- Layers -------------------------------------------------------------------------------

namespace {
bool loadLayer(const fs::path& file, std::map<std::string, std::string>& layer, std::string& error) {
    std::error_code ec;
    if (file.empty() || !fs::exists(file, ec)) {
        layer.clear();
        return true;
    }
    std::string text;
    if (!readFile(file, text)) {
        error = file.string() + ": can't open it";
        return false;
    }
    std::map<std::string, std::string> keys;
    if (!parsePreferences(text, file.string(), keys, error)) return false;
    layer = std::move(keys);
    return true;
}
}   // namespace

bool PreferenceStack::loadShipped(const fs::path& file, std::string& error) {
    return loadLayer(file, m_layers[1], error);
}
bool PreferenceStack::loadProfile(const fs::path& file, std::string& error) {
    return loadLayer(file, m_layers[2], error);
}

bool PreferenceStack::setSession(const std::string& key, const std::string& value, std::string& error) {
    PlayerPreferences check = effective();
    if (!setPreference(check, key, value, error)) return false;
    m_layers[3][key] = value;
    return true;
}

PlayerPreferences PreferenceStack::below(PreferenceLayer top) const {
    PlayerPreferences prefs;
    for (int layer = 1; layer < int(top); ++layer)
        for (const auto& [key, value] : m_layers[layer]) {
            std::string ignored;
            setPreference(prefs, key, value, ignored);   // validated when the layer was read
        }
    return prefs;
}

PlayerPreferences PreferenceStack::effective() const { return below(PreferenceLayer(4)); }

void PreferenceStack::setProfile(const PlayerPreferences& chosen) {
    const PlayerPreferences shipped = below(PreferenceLayer::Profile);
    m_layers[2].clear();
    for (const auto& info : preferenceInfo())
        if (const std::string value = info.get(chosen); value != info.get(shipped)) m_layers[2][info.key] = value;
}

PreferenceLayer PreferenceStack::origin(const std::string& key) const {
    for (int layer = 3; layer >= 1; --layer)
        if (m_layers[layer].count(key)) return PreferenceLayer(layer);
    return PreferenceLayer::Default;
}

// ---- Preferences: applying them ----------------------------------------------------------

Preferences::Display Preferences::displayOf(const PlayerPreferences& prefs) {
    return {prefs.windowMode, prefs.windowWidth, prefs.windowHeight};
}

double Preferences::now() const { return m_clock ? m_clock() : static_cast<double>(SDL_GetTicksNS()) * 1e-9; }

fs::path Preferences::pendingFile() const {
    return m_profile.empty() ? fs::path() : m_profile.parent_path() / "Display.pending";
}

bool Preferences::configure(
    const fs::path& shipped, const fs::path& profile, const std::vector<std::pair<std::string, std::string>>& session,
    std::string& error) {
    m_profile = profile;
    m_pending.reset();
    std::string problem;
    if (!m_stack.loadShipped(shipped, problem)) {
        error = problem;
        return false;
    }
    if (!m_stack.loadProfile(profile, problem)) {
        // A broken profile shouldn't stop the game: the shipped preferences, and say so.
        SW_LOG(LogPreferences, Warning, "Ignoring the player's preferences: {}", problem);
    }
    // A display change the last run never kept: put back the one that worked.
    const fs::path pending = pendingFile();
    std::error_code ec;
    if (!pending.empty() && fs::exists(pending, ec)) {
        std::string text, why;
        std::map<std::string, std::string> working;
        if (readFile(pending, text) && parsePreferences(text, pending.string(), working, why)) {
            auto keys = m_stack.layer(PreferenceLayer::Profile);
            for (const char* key : {"window_mode", "window_width", "window_height"}) {
                if (const auto it = working.find(key); it != working.end())
                    keys[key] = it->second;
                else
                    keys.erase(key);
            }
            m_stack.setProfileKeys(keys);
            std::string saveError;
            if (!writeFileAtomically(profile, preferencesText(keys), saveError))
                SW_LOG(LogPreferences, Warning, "Couldn't save the restored display: {}", saveError);
            SW_LOG(
                LogPreferences, Warning, "The last display change wasn't kept; went back to the display that worked");
        }
        fs::remove(pending, ec);
    }
    m_stack.clearSession();
    for (const auto& [key, value] : session)
        if (!m_stack.setSession(key, value, problem)) {
            error = "--pref " + key + "=" + value + ": " + problem;
            return false;
        }
    const PlayerPreferences start = m_stack.effective();
    m_current = PlayerPreferences{};
    m_current.windowMode = WindowMode::Windowed;
    applyNow(start);
    const Display display = displayOf(start);
    if (display.mode != WindowMode::Windowed || display.width > 0) {
        if (!applyDisplay(display, problem))
            SW_LOG(LogPreferences, Warning, "Couldn't use the saved display ({}); staying windowed", problem);
        else
            m_current.windowMode = display.mode;
    }
    const WindowMode mode = m_current.windowMode;
    m_current = start;
    m_current.windowMode = mode;
    error.clear();
    return true;
}

bool Preferences::applyDisplay(const Display& display, std::string& error) {
    if (!m_engine) return true;
    return m_engine->window().setMode(display.mode, display.width, display.height, error);
}

void Preferences::applyNow(const PlayerPreferences& prefs) {
    if (!m_engine) return;
    const bool muted = prefs.muteWhenUnfocused && !m_engine->window().hasFocus();
    m_engine->setMasterVolume(muted ? 0.0f : prefs.masterVolume);
    // AudioCategory order: effects, music, ui, voice.
    m_engine->setCategoryVolume(0, prefs.effectsVolume);
    m_engine->setCategoryVolume(1, prefs.musicVolume);
    m_engine->setCategoryVolume(2, prefs.uiVolume);
    m_engine->setCategoryVolume(3, prefs.voiceVolume);
    m_engine->input().setDeadzoneOverride(prefs.stickDeadZone);
    m_engine->localization().setLocale(prefs.language);
}

bool Preferences::apply(const PlayerPreferences& prefs, std::string& error) {
    if (!validatePreferences(prefs, error)) return false;
    const Display from = displayOf(m_current), to = displayOf(prefs);
    const bool displayChanges = from.mode != to.mode || from.width != to.width || from.height != to.height;
    if (displayChanges) {
        // What to go back to: the display that worked before any change still waiting.
        const Display working = m_pending ? m_pending->working : from;
        const fs::path pending = pendingFile();
        if (!pending.empty()) {
            std::map<std::string, std::string> keys = {
                {"window_mode", windowModeName(working.mode)},
                {"window_width", std::to_string(working.width)},
                {"window_height", std::to_string(working.height)}};
            std::string why;
            if (!writeFileAtomically(pending, preferencesText(keys), why)) {
                error = "can't change the display without somewhere to note the one that works: " + why;
                return false;
            }
        }
        std::string why;
        if (!applyDisplay(to, why)) {
            std::string ignored;
            applyDisplay(from, ignored);
            std::error_code ec;
            if (!m_pending && !pending.empty()) fs::remove(pending, ec);
            error = "the display stayed as it was: " + why;
            return false;
        }
        m_pending = Pending{working, now() + kKeepSeconds};
    }
    applyNow(prefs);
    m_current = prefs;
    error.clear();
    return true;
}

bool Preferences::save(const PlayerPreferences& prefs, std::string& error) {
    if (!validatePreferences(prefs, error)) return false;
    if (m_profile.empty()) {
        error = "No profile to save preferences in: run the standalone player";
        return false;
    }
    PlayerPreferences chosen = prefs;
    if (m_pending) {   // not kept yet: the display that works is what's saved
        chosen.windowMode = m_pending->working.mode;
        chosen.windowWidth = m_pending->working.width;
        chosen.windowHeight = m_pending->working.height;
    }
    PreferenceStack next = m_stack;
    next.setProfile(chosen);
    if (!writeFileAtomically(m_profile, preferencesText(next.layer(PreferenceLayer::Profile)), error)) return false;
    m_stack = next;
    error.clear();
    return true;
}

double Preferences::displaySecondsLeft() const { return m_pending ? std::max(0.0, m_pending->deadline - now()) : 0.0; }

void Preferences::keepDisplay() {
    if (!m_pending) return;
    m_pending.reset();
    std::error_code ec;
    if (const fs::path pending = pendingFile(); !pending.empty()) fs::remove(pending, ec);
    if (m_profile.empty()) return;
    // The kept display goes into the profile with whatever else is saved there.
    PlayerPreferences saved = m_stack.below(PreferenceLayer::Session);
    saved.windowMode = m_current.windowMode;
    saved.windowWidth = m_current.windowWidth;
    saved.windowHeight = m_current.windowHeight;
    std::string error;
    if (!save(saved, error)) SW_LOG(LogPreferences, Warning, "Couldn't save the display: {}", error);
}

void Preferences::revertDisplay() {
    if (!m_pending) return;
    const Display working = m_pending->working;
    m_pending.reset();
    std::string why;
    if (!applyDisplay(working, why)) SW_LOG(LogPreferences, Error, "Couldn't put the display back: {}", why);
    m_current.windowMode = working.mode;
    m_current.windowWidth = working.width;
    m_current.windowHeight = working.height;
    std::error_code ec;
    if (const fs::path pending = pendingFile(); !pending.empty()) fs::remove(pending, ec);
    SW_LOG(LogPreferences, Info, "The display change wasn't kept; went back to {}", windowModeName(working.mode));
}

void Preferences::update() {
    if (m_pending && now() >= m_pending->deadline) revertDisplay();
    if (m_engine && m_current.muteWhenUnfocused) {
        const float master = m_engine->window().hasFocus() ? m_current.masterVolume : 0.0f;
        if (m_engine->masterVolume() != master) m_engine->setMasterVolume(master);
    }
}

std::vector<std::pair<std::string, std::string>> Preferences::unavailable() const {
    std::vector<std::pair<std::string, std::string>> out;
    if (m_engine && m_engine->window().resolutions().empty())
        out.emplace_back("fullscreen", "This display doesn't list any fullscreen resolutions");
    return out;
}

}   // namespace shockwave
