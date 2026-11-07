// The shell's built-in screens (AppShell.h, docs/AppShell.md). They use nothing but the
// engine's own font (Noto Sans, shipped beside the program): no images, stylesheets or
// project files, so the error screen still shows when the project's files are broken.
// A project replaces any of them with Content/Shell/<screen>.rml; these are the reference.
#include <GameFramework/AppShell.h>

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

const char* kSettings = R"(
<div data-if="page == 'settings'">
  <h2>SETTINGS</h2>
  <p class="reason">{{settings_reason}}</p>
  <button id="settings_back" onclick="back">Back</button>
</div>)";

}   // namespace

std::string AppShell::builtinDocument(ShellScreen screen) {
    switch (screen) {
    case ShellScreen::Menu:
        return screenDocument("Main menu", "back", std::string(R"(
<h1>{{title}}</h1>
<div data-if="page == 'main'">
  <button id="new_game" onclick="new_game" data-class-disabled="!can_new_game" autofocus>New game</button>
  <p class="reason" data-if="!can_new_game">{{new_game_reason}}</p>
  <button id="continue" onclick="continue" data-class-disabled="!can_continue">Continue</button>
  <p class="reason" data-if="!can_continue">{{continue_reason}}</p>
  <button id="level_select" onclick="level_select" data-if="can_level_select">Level select</button>
  <button id="settings" onclick="settings">Settings</button>
  <button id="credits" onclick="credits">Credits</button>
  <button id="quit" onclick="quit">Quit</button>
</div>
<div data-if="page == 'levels'">
  <h2>LEVEL SELECT</h2>
  <button data-for="entry : levels" data-attr-id="'level-' + entry.id" onclick="play_level">{{entry.title}}</button>
  <button id="levels_back" onclick="back">Back</button>
</div>
<div data-if="page == 'confirm'">
  <h2>START A NEW GAME?</h2>
  <p class="reason">Your current progress will be lost.</p>
  <button id="confirm_new_game" onclick="confirm_new_game">Start over</button>
  <button id="confirm_back" onclick="back">Keep playing</button>
</div>
<div data-if="page == 'credits'">
  <h2>CREDITS</h2>
  <p>{{credits}}</p>
  <button id="credits_back" onclick="back">Back</button>
</div>)") + kSettings);
    case ShellScreen::Pause:
        return screenDocument("Paused", "resume", std::string(R"(
<h1>PAUSED</h1>
<div data-if="page == 'main'">
  <button id="resume" onclick="resume" autofocus>Resume</button>
  <button id="restart_checkpoint" onclick="restart_checkpoint" data-class-disabled="!can_restart_checkpoint">Restart checkpoint</button>
  <p class="reason" data-if="!can_restart_checkpoint">{{checkpoint_reason}}</p>
  <button id="restart_level" onclick="restart_level">Restart level</button>
  <button id="settings" onclick="settings">Settings</button>
  <button id="main_menu" onclick="main_menu">Main menu</button>
  <button id="quit" onclick="quit">Quit</button>
</div>)") + kSettings);
    case ShellScreen::Loading:
        return screenDocument("Loading", "", R"(
<h1>LOADING</h1>
<p>{{destination}}</p>
<div class="bar"><div class="fill" data-style-width="has_progress ? progress_width : '0%'"></div></div>
<p class="code">{{phase}} · {{percent}}%</p>
<button id="cancel_travel" onclick="cancel_travel" data-if="can_cancel">Cancel</button>)");
    case ShellScreen::Error:
        return screenDocument("Error", "", R"(
<h1>SOMETHING WENT WRONG</h1>
<p>{{error_message}}</p>
<p class="code">{{error_code}} · level {{error_level}}</p>
<button id="retry" onclick="retry" autofocus>Retry</button>
<button id="dismiss" onclick="dismiss" data-if="can_dismiss">Back to the level</button>
<button id="main_menu" onclick="main_menu">Main menu</button>
<button id="quit" onclick="quit">Quit</button>)");
    case ShellScreen::None: break;
    }
    return {};
}

}   // namespace shockwave
