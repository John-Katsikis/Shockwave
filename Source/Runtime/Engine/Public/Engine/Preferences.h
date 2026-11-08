#pragma once

// Player preferences (S08, docs/Preferences.md): display, audio, controls, accessibility,
// camera and language, in three layers.
//
//   shipped   the engine's defaults, then the project's Config/Preferences.swprefs
//   profile   the player's own <config>/Preferences.swprefs (what Save writes)
//   session   this run only: player --pref key=value, tests
//
// Every preference is described once in preferenceInfo(): files, the schema, the command
// line, the shell's Settings page and the editor read that table. Graphics preferences
// (quality, render scale, VSync, frame cap) keep their own file and API (GraphicsSettings.h);
// the Settings page shows both.
//
// Applying and saving are separate: apply() changes what runs and save() writes the
// profile, so a failed write never changes the game and a failed change is never saved.
// An apply() is one transaction: every value is validated before any is used.
//
// A display change (window mode or size) can leave a player with nothing they can see, so
// it has to be kept: apply() makes it, then keepDisplay() within 15 seconds keeps it, and
// otherwise update() puts back the last display that worked. The attempt is written to
// <config>/Display.pending first, so a crash or a restart before it's kept also goes back.

#include <Engine/Subsystem.h>
#include <Platform/Window.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace shockwave {

class Engine;
struct Project;

struct PlayerPreferences {
    // ---- Display ----
    WindowMode windowMode = WindowMode::Windowed;
    int windowWidth = 0, windowHeight = 0;   // 0: the project's window_size
    // ---- Audio ----
    float masterVolume = 1, effectsVolume = 1, musicVolume = 1, uiVolume = 1, voiceVolume = 1;
    bool muteWhenUnfocused = false;   // silence while another window has focus
    // ---- Controls ----
    float lookSensitivity = 1;   // a factor on the game's own look speed
    bool invertLookY = false;
    float stickDeadZone = 0.15f;   // gamepad sticks: below this reads as 0
    bool toggleHold = false;   // actions a game holds (sprint, aim) toggle with one press instead
    // ---- Accessibility ----
    float uiScale = 1;   // the shell's and the game's UI
    bool highContrast = false;
    bool subtitles = true;
    float subtitleScale = 1;
    bool reduceFlashes = false;   // games tone down full-screen flashes
    float cameraShake = 1;   // a factor on the game's camera shake; 0 turns it off
    // ---- Camera ----
    float fieldOfView = 0;   // degrees; 0: the game's own
    // ---- Language ----
    std::string language;   // a locale like "fr" or "pt-BR"; empty: the project's default
    bool operator==(const PlayerPreferences&) const = default;
};

enum class PreferenceLayer { Default, Shipped, Profile, Session };
const char* preferenceLayerName(PreferenceLayer layer);   // "default", "shipped", "profile", "session"

// One preference, as files, the schema, menus and tools all see it.
struct PreferenceInfo {
    const char* key;   // as spelled in .swprefs and after --pref
    const char* label;   // for menus: "Window mode"
    const char* group;   // "Display", "Audio", "Controls", "Accessibility", "Camera", "Language"
    const char* tooltip;
    const char* values;   // for the schema: "0..1", "windowed, borderless, fullscreen"
    std::vector<const char*> choices;   // enum and true/false values in menu order; empty otherwise
    bool display;   // a display change: kept within 15 seconds or reverted
    std::string (*get)(const PlayerPreferences&);
    // Parses and range-checks one value; leaves the preferences unchanged and says why.
    bool (*set)(PlayerPreferences&, const std::string& value, std::string& error);
};
const std::vector<PreferenceInfo>& preferenceInfo();
const PreferenceInfo* findPreference(const std::string& key);
bool setPreference(PlayerPreferences& prefs, const std::string& key, const std::string& value, std::string& error);
bool validatePreferences(const PlayerPreferences& prefs, std::string& error);

// The file: "swprefs 1", then one "key value" per line ("#" starts a comment). Values with
// spaces are quoted. Unknown and duplicate keys, bad ranges and NaN are errors with a line.
// Keys read go into `keys` (key -> value text) for the layer stack.
bool parsePreferences(
    const std::string& text, const std::string& shown, std::map<std::string, std::string>& keys, std::string& error);
std::string preferencesText(const std::map<std::string, std::string>& keys);
std::string preferencesSchema();   // docs/PreferencesSchema.md
// Where the project's shipped preferences live: <project>/Config/Preferences.swprefs.
std::filesystem::path projectPreferencesPath(const Project& project);

// The three layers and where each value came from.
class PreferenceStack {
public:
    // Each replaces that layer. A layer that doesn't read leaves it as it was.
    bool loadShipped(const std::filesystem::path& file, std::string& error);   // missing file: none
    bool loadProfile(const std::filesystem::path& file, std::string& error);   // missing file: none
    bool setSession(const std::string& key, const std::string& value, std::string& error);
    void clearSession() { m_layers[3].clear(); }
    // The profile as the player chose: every key whose value differs from the layers under it.
    void setProfile(const PlayerPreferences& chosen);
    void setProfileKeys(std::map<std::string, std::string> keys) { m_layers[2] = std::move(keys); }
    const std::map<std::string, std::string>& layer(PreferenceLayer layer) const { return m_layers[int(layer)]; }

    PlayerPreferences effective() const;
    PlayerPreferences below(PreferenceLayer layer) const;   // the layers under it only
    PreferenceLayer origin(const std::string& key) const;   // the highest layer that sets it

private:
    std::map<std::string, std::string> m_layers[4];   // Default is always empty
};

// game.preferences() and the shell's Settings page: main thread.
class Preferences final : public Subsystem {
public:
    explicit Preferences(Engine* engine = nullptr) : m_engine(engine) {}

    // The host sets it up once: the project's shipped file, the player's profile file
    // (empty: nothing is saved) and --pref overrides. An unconfirmed display attempt left
    // by the last run is undone first. Applies the result.
    bool configure(
        const std::filesystem::path& shipped, const std::filesystem::path& profile,
        const std::vector<std::pair<std::string, std::string>>& session, std::string& error);

    const PlayerPreferences& current() const { return m_current; }
    const PreferenceStack& stack() const { return m_stack; }
    PlayerPreferences defaults() const { return m_stack.below(PreferenceLayer::Profile); }
    const std::filesystem::path& profileFile() const { return m_profile; }

    // Validates the whole set, then uses it: volumes, the stick dead zone and the display
    // at once, the rest read by the shell and the game. A display change starts the
    // 15-second keep-or-revert; one the platform refuses leaves the display as it was and
    // fails with why.
    bool apply(const PlayerPreferences& prefs, std::string& error);
    // Writes the profile: the values that differ from the shipped ones. Doesn't apply. A
    // display change still waiting to be kept saves the display that worked instead.
    bool save(const PlayerPreferences& prefs, std::string& error);

    // ---- Keeping a display change ----
    static constexpr double kKeepSeconds = 15.0;
    bool displayPending() const { return m_pending.has_value(); }
    double displaySecondsLeft() const;
    void keepDisplay();   // the player said keep: saves it to the profile
    void revertDisplay();   // back to the last display that worked
    // Once a frame (the engine does it): reverts a display change nobody kept in time. Reads
    // a clock; never waits.
    void update();
    // The display the platform can't do right now, with why, for the Settings page:
    // "fullscreen" when the display lists no resolutions, say. Empty: all can be chosen.
    std::vector<std::pair<std::string, std::string>> unavailable() const;
    // For tests: what time it is (seconds); the engine's clock when unset.
    void setClock(double (*now)()) { m_clock = now; }

private:
    struct Display {
        WindowMode mode = WindowMode::Windowed;
        int width = 0, height = 0;
    };
    static Display displayOf(const PlayerPreferences& prefs);
    bool applyDisplay(const Display& display, std::string& error);
    void applyNow(const PlayerPreferences& prefs);   // everything but the display
    std::filesystem::path pendingFile() const;
    double now() const;

    Engine* m_engine;
    PreferenceStack m_stack;
    PlayerPreferences m_current;
    std::filesystem::path m_profile;
    struct Pending {
        Display working;   // what to go back to
        double deadline = 0;
    };
    std::optional<Pending> m_pending;
    double (*m_clock)() = nullptr;
};

}   // namespace shockwave
