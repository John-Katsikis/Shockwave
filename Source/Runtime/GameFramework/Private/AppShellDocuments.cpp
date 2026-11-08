// The shell's built-in screens (AppShell.h, docs/AppShell.md). They use nothing but the
// engine's own font (Noto Sans, shipped beside the program): no images, stylesheets or
// project files, so the error screen still shows when the project's files are broken.
// A project replaces any of them with Content/Shell/<screen>.rml; these are the reference.
#include <GameFramework/AppShell.h>

#include <Engine/GraphicsSettings.h>
#include <Engine/Preferences.h>

#include <string>

namespace shockwave {

namespace {

const char* kStyle = R"(
body {
    font-family: "Noto Sans";
    font-size: 18dp;
    color: #f2f4f8;
    width: 100%;
    height: 100%;
    background-color: #0b0e12e0;
}
.panel {
    position: absolute;
    left: 50%;
    top: 50%;
    width: 380dp;
    margin-left: -190dp;
    margin-top: -220dp;
    padding: 22dp 26dp;
    background-color: #1a1f27;
    border-top: 3dp #ff8c33;
}
h1 { display: block; font-size: 22dp; font-weight: bold; color: #ff8c33; margin-bottom: 16dp; }
h2 { display: block; font-size: 14dp; font-weight: bold; color: #aab3c0; margin: 4dp 0 10dp; }
p { display: block; margin-bottom: 8dp; }
div { display: block; }
button {
    display: block;
    margin-bottom: 8dp;
    padding: 8dp 14dp;
    font-size: 17dp;
    font-weight: bold;
    background-color: #2c3440;
    border: 2dp transparent;
    cursor: pointer;
}
button:hover { background-color: #36404e; }
button:focus-visible { border-color: #ff8c33; }
button.disabled { color: #7d8693; background-color: #232932; }
.tabs button { display: inline-block; margin: 0 4dp 8dp 0; padding: 5dp 9dp; font-size: 14dp; }
.tabs button.selected { color: #ff8c33; }
.row { display: block; margin-bottom: 6dp; }
.row .label { display: inline-block; width: 150dp; font-size: 15dp; }
.row button { display: inline-block; margin: 0; padding: 3dp 9dp; font-size: 15dp; }
.row .value { display: inline-block; width: 110dp; text-align: center; font-size: 15dp; }
.actions button { display: inline-block; margin-right: 6dp; }
body.high_contrast { background-color: #000000f4; }
body.high_contrast .panel { background-color: #000000; border: 2dp #ffffff; }
body.high_contrast button { background-color: #000000; border: 2dp #ffffff; }
body.high_contrast button:focus-visible { border-color: #ffd23f; }
body.high_contrast .reason, body.high_contrast .code { color: #e6eaf0; }
.reason { font-size: 13dp; color: #8a94a3; margin: -4dp 0 10dp 4dp; }
.message { font-size: 14dp; color: #ffb27a; margin-top: 8dp; }
.code { font-size: 13dp; color: #8a94a3; }
.bar { height: 10dp; background-color: #2c3440; margin: 14dp 0 10dp; }
.fill { height: 10dp; background-color: #ff8c33; }
)";

std::string screenDocument(const char* title, const char* back, const std::string& body) {
    return std::string("<rml><head><title>") + title + "</title><style>" + kStyle +
           "</style></head><body data-model=\"flow\" data-back=\"" + back + "\"><div class=\"panel\">" + body +
           "<p class=\"message\" data-if=\"message != ''\">{{message}}</p></div></body></rml>";
}

// The Settings page: rows of < value > buttons, so a gamepad and arrow keys drive it as
// well as a mouse. The rows come from preferenceInfo() (AppShellSettings.cpp).
const char* kSettings = R"(
<div data-if="page == 'settings'">
  <h2>{{text.shell.settings_title}}</h2>
  <div data-if="!display_pending">
    <div class="tabs">
      <button data-for="tab : settings_tabs" data-attr-id="'tab-' + tab.id" onclick="settings_tab" data-class-selected="tab.id == settings_tab">{{tab.label}}</button>
    </div>
    <div class="row" data-for="row : settings_rows">
      <span class="label">{{row.label}}</span>
      <button data-attr-id="'prev-' + row.key" onclick="pref_prev" data-class-disabled="!row.can_change">&lt;</button>
      <span class="value">{{row.value}}</span>
      <button data-attr-id="'next-' + row.key" onclick="pref_next" data-class-disabled="!row.can_change">&gt;</button>
      <p class="reason" data-if="!row.can_change">{{row.reason}}</p>
    </div>
    <div class="actions">
      <button id="apply_settings" onclick="apply_settings" data-class-disabled="!settings_changed">{{text.shell.apply}}</button>
      <button id="reset_settings" onclick="reset_settings">{{text.shell.defaults}}</button>
      <button id="settings_back" onclick="cancel_settings">{{text.shell.back}}</button>
    </div>
  </div>
  <div data-if="display_pending">
    <p>{{text.shell.keep_display_title}}</p>
    <p class="reason">{{display_countdown}}</p>
    <button id="keep_display" onclick="keep_display" autofocus>{{text.shell.keep}}</button>
    <button id="revert_display" onclick="revert_display">{{text.shell.revert}}</button>
  </div>
</div>)";

// "Settings" off or unavailable: the page says why.
const char* kSettingsOff = R"(
<div data-if="page == 'settings'">
  <h2>{{text.shell.settings_title}}</h2>
  <p class="reason">{{settings_reason}}</p>
  <button id="settings_closed_back" onclick="back">{{text.shell.back}}</button>
</div>)";

std::string settingsPage() {
    return std::string("<div data-if=\"can_settings\">") + kSettings + "</div><div data-if=\"!can_settings\">" +
           kSettingsOff + "</div>";
}

}   // namespace

std::vector<std::pair<std::string, std::string>> AppShell::builtinText() {
    std::vector<std::pair<std::string, std::string>> text = {
        {"shell.new_game", "New game"},
        {"shell.continue", "Continue"},
        {"shell.level_select", "Level select"},
        {"shell.settings", "Settings"},
        {"shell.credits", "Credits"},
        {"shell.quit", "Quit"},
        {"shell.back", "Back"},
        {"shell.level_select_title", "LEVEL SELECT"},
        {"shell.confirm_title", "START A NEW GAME?"},
        {"shell.confirm_text", "Your current progress will be lost."},
        {"shell.start_over", "Start over"},
        {"shell.keep_playing", "Keep playing"},
        {"shell.credits_title", "CREDITS"},
        {"shell.credits_default", "Made with Shockwave"},
        {"shell.paused", "PAUSED"},
        {"shell.resume", "Resume"},
        {"shell.restart_checkpoint", "Restart checkpoint"},
        {"shell.restart_level", "Restart level"},
        {"shell.main_menu", "Main menu"},
        {"shell.loading", "LOADING"},
        {"shell.cancel", "Cancel"},
        {"shell.error_title", "SOMETHING WENT WRONG"},
        {"shell.error_detail", "{code} · level {level}"},
        {"shell.retry", "Retry"},
        {"shell.dismiss", "Back to the level"},
        {"shell.no_new_game", "This game has no first level to start in"},
        {"shell.no_continue", "No saved game to continue yet"},
        {"shell.no_checkpoint", "No checkpoint reached yet"},
        // The Settings page.
        {"shell.settings_title", "SETTINGS"},
        {"shell.apply", "Apply"},
        {"shell.defaults", "Defaults"},
        {"shell.keep_display_title", "Keep this display?"},
        {"shell.keep_display_countdown", "Going back to the last one in {seconds} seconds"},
        {"shell.keep", "Keep"},
        {"shell.revert", "Go back"},
        {"shell.settings_off", "This game has no settings page"},
        {"shell.settings_unavailable", "Settings aren't available here"},
        {"shell.settings_saved", "Settings saved"},
        {"shell.one_language", "This game has one language"},
        {"shell.settings_not_saved", "Applied for now, but not saved: {why}"},
        {"shell.settings_failed", "Nothing changed: {why}"},
        {"shell.tab.display", "Display"},
        {"shell.tab.graphics", "Graphics"},
        {"shell.tab.audio", "Audio"},
        {"shell.tab.controls", "Controls"},
        {"shell.tab.accessibility", "Accessibility"},
        {"shell.tab.language", "Language"},
        {"shell.value.on", "On"},
        {"shell.value.off", "Off"},
        {"shell.value.default", "Game default"},
        {"shell.value.custom", "Custom"},
        {"shell.value.unlimited", "Unlimited"},
        {"shell.value.windowed", "Windowed"},
        {"shell.value.borderless", "Borderless"},
        {"shell.value.fullscreen", "Fullscreen"},
        {"shell.value.low", "Low"},
        {"shell.value.medium", "Medium"},
        {"shell.value.high", "High"},
        {"shell.value.ultra", "Ultra"},
        {"shell.pref.resolution", "Resolution"},
        {"shell.pref.graphics.preset", "Quality"},
        {"shell.pref.graphics.render_scale", "Render scale"},
        {"shell.pref.graphics.vsync", "VSync"},
        {"shell.pref.graphics.frame_limit", "Frame cap"},
    };
    // Every preference's label, from the one table: "shell.pref.master_volume".
    for (const PreferenceInfo& info : preferenceInfo())
        text.emplace_back(std::string("shell.pref.") + info.key, info.label);
    return text;
}

std::string AppShell::builtinDocument(ShellScreen screen) {
    switch (screen) {
    case ShellScreen::Menu:
        return screenDocument("Main menu", "back", std::string(R"(
<h1>{{title}}</h1>
<div data-if="page == 'main'">
  <button id="new_game" onclick="new_game" data-class-disabled="!can_new_game" autofocus>{{text.shell.new_game}}</button>
  <p class="reason" data-if="!can_new_game">{{new_game_reason}}</p>
  <button id="continue" onclick="continue" data-class-disabled="!can_continue">{{text.shell.continue}}</button>
  <p class="reason" data-if="!can_continue">{{continue_reason}}</p>
  <button id="level_select" onclick="level_select" data-if="can_level_select">{{text.shell.level_select}}</button>
  <button id="settings" onclick="settings">{{text.shell.settings}}</button>
  <button id="credits" onclick="credits">{{text.shell.credits}}</button>
  <button id="quit" onclick="quit">{{text.shell.quit}}</button>
</div>
<div data-if="page == 'levels'">
  <h2>{{text.shell.level_select_title}}</h2>
  <button data-for="entry : levels" data-attr-id="'level-' + entry.id" onclick="play_level">{{entry.title}}</button>
  <button id="levels_back" onclick="back">{{text.shell.back}}</button>
</div>
<div data-if="page == 'confirm'">
  <h2>{{text.shell.confirm_title}}</h2>
  <p class="reason">{{text.shell.confirm_text}}</p>
  <button id="confirm_new_game" onclick="confirm_new_game">{{text.shell.start_over}}</button>
  <button id="confirm_back" onclick="back">{{text.shell.keep_playing}}</button>
</div>
<div data-if="page == 'credits'">
  <h2>{{text.shell.credits_title}}</h2>
  <p>{{credits}}</p>
  <button id="credits_back" onclick="back">{{text.shell.back}}</button>
</div>)") + settingsPage());
    case ShellScreen::Pause:
        return screenDocument("Paused", "resume", std::string(R"(
<h1>{{text.shell.paused}}</h1>
<div data-if="page == 'main'">
  <button id="resume" onclick="resume" autofocus>{{text.shell.resume}}</button>
  <button id="restart_checkpoint" onclick="restart_checkpoint" data-class-disabled="!can_restart_checkpoint">{{text.shell.restart_checkpoint}}</button>
  <p class="reason" data-if="!can_restart_checkpoint">{{checkpoint_reason}}</p>
  <button id="restart_level" onclick="restart_level">{{text.shell.restart_level}}</button>
  <button id="settings" onclick="settings">{{text.shell.settings}}</button>
  <button id="main_menu" onclick="main_menu">{{text.shell.main_menu}}</button>
  <button id="quit" onclick="quit">{{text.shell.quit}}</button>
</div>)") + settingsPage());
    case ShellScreen::Loading:
        return screenDocument("Loading", "", R"(
<h1>{{text.shell.loading}}</h1>
<p>{{destination}}</p>
<div class="bar"><div class="fill" data-style-width="has_progress ? progress_width : '0%'"></div></div>
<p class="code">{{phase}} · {{percent}}%</p>
<button id="cancel_travel" onclick="cancel_travel" data-if="can_cancel">{{text.shell.cancel}}</button>)");
    case ShellScreen::Error:
        return screenDocument("Error", "", R"(
<h1>{{text.shell.error_title}}</h1>
<p>{{error_message}}</p>
<p class="code">{{error_detail}}</p>
<button id="retry" onclick="retry" autofocus>{{text.shell.retry}}</button>
<button id="dismiss" onclick="dismiss" data-if="can_dismiss">{{text.shell.dismiss}}</button>
<button id="main_menu" onclick="main_menu">{{text.shell.main_menu}}</button>
<button id="quit" onclick="quit">{{text.shell.quit}}</button>)");
    case ShellScreen::None: break;
    }
    return {};
}

}   // namespace shockwave
