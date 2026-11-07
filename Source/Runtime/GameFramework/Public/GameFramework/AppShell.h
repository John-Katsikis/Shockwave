#pragma once

// AppShell: the application's own screens around the levels (S05, docs/AppShell.md): the
// main menu, pause, loading and error screens, over whatever level is playing.
//
//   AppShell shell(&engine, levels, setup);   // levels: the LevelHost that is the GameFlow
//   ... every frame, before the level's session runs its frame:
//   shell.update(dt);                         // pause key, clicks, what the flow is doing
//   levelUI.setInputEnabled(!shell.capturesInput());
//   ... after the level's UI has drawn:
//   shell.render(renderer);
//
// The shell has its own GameUI: its own RmlUi context, its own data model ("flow", bound
// with data-model="flow"), focus and handlers, so nothing leaks between it and the level's
// UI (game.ui(), whose data model is "game"). It draws on top and gets input first: while
// one of its screens shows, the level's UI and gameplay input wait.
//
// It reaches the flow through the LevelHost the application owns for the whole run, never
// through a level's GameContext, and follows travel with a GameFlow subscription.
//
// Each screen can be switched off (AppShellScreens, the project's app_shell key, player
// --app-shell). Its document is the project's Content/Shell/<screen>.rml when there is one,
// else one compiled into the engine. A project document that doesn't load (a missing file,
// a broken font, bad markup) is replaced by the built-in one, so the error screen shows
// even when the project's own files are what's broken.
//
// The buttons call command(): onclick="new_game" in a document, and the same command
// from --flow-test (FlowTest.h) and from code. Main thread only.

#include <Engine/World.h>
#include <GameFramework/GameFlow.h>
#include <GameUI/GameUI.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

class Engine;
class Input;
class LevelHost;
class Renderer;

// Which of the shell's screens are on. The project key app_shell is "off", "all", or the
// names of those on: "app_shell menu pause loading error level_select". Without the key,
// loading and error are on: they cover travel any game does, and draw nothing otherwise.
struct AppShellScreens {
    bool menu = false;   // on a menu-kind level: New game, Continue, Settings, Credits, Quit
    bool levelSelect = false;   // the menu's Level select (the catalog's gameplay levels)
    bool pause = false;   // the pause action: Resume, Restart checkpoint, Restart level, Settings, Main menu
    bool loading = true;   // while a travel is under way, with its honest progress and Cancel
    bool error = true;   // a travel that failed: why, with Retry, Main menu and Quit
    bool any() const { return menu || pause || loading || error; }
    bool operator==(const AppShellScreens&) const = default;
};
bool parseAppShellScreens(std::string_view text, AppShellScreens& out, std::string& error);
std::string appShellScreensText(const AppShellScreens& screens);   // the key's value: "loading error"

enum class ShellScreen { None, Menu, Pause, Loading, Error };
const char* shellScreenName(ShellScreen screen);   // "none", "menu", "pause", "loading", "error"

// The input action that opens and closes the pause screen: Escape and a gamepad's Start
// unless the project binds it (Config/Input.swinput).
inline constexpr const char* kPauseAction = "pause";

class AppShell {
public:
    struct Level {
        std::string id;
        std::string title;   // shown in Level select; the id when empty
        bool menu = false;   // a menu-kind level: the main menu shows over it
        bool selectable = false;   // listed in Level select
    };
    struct Setup {
        std::string title;   // the game's name, on the main menu
        AppShellScreens screens;
        std::vector<Level> levels;
        std::string newGameLevel;   // where New game goes; empty turns it off, with a reason
        // The project's Content folder: Shell/menu.rml (pause, loading, error) there
        // replaces the built-in screen. Empty: the built-in ones.
        std::filesystem::path content;
        std::string credits;   // the Credits page's text
        std::function<void()> quit;   // Quit
        // There is progress New game would reset: it asks first. Unset: none.
        std::function<bool()> hasProgress;
        // Continue (S06): why it can't, "" when it can. Asked each time the menu opens and
        // after a travel ends. Unset: Continue stays off.
        std::function<std::string()> continueReason;
        // The request Continue sends: the newest save this game and profile can load,
        // read, as a LoadSave travel. False, with why, when there's none.
        std::function<bool(TravelRequest& request, std::string& why)> continueGame;
    };

    // engine: where to draw and read input; null in tests (documents still load and
    // commands still work). levels must outlive the shell.
    AppShell(Engine* engine, LevelHost& levels, Setup setup);
    ~AppShell();
    AppShell(const AppShell&) = delete;
    AppShell& operator=(const AppShell&) = delete;

    // Once per frame, before the level's session runs its frame: the pause action, what
    // the flow is doing (loading, a failure), and the shell's UI and its clicks.
    void update(double frameDt);
    // Draws the screen showing, over the level and its UI.
    void render(Renderer& renderer);

    // One of the screens shows: the level's UI and gameplay input should wait.
    bool capturesInput() const { return m_screen != ShellScreen::None; }
    ShellScreen screen() const { return m_screen; }
    // The menu's page: "main", "levels", "settings", "credits" or "confirm".
    const std::string& page() const { return m_page; }
    bool paused() const { return m_paused; }

    // What a button does, by its onclick text. False when it's refused right now, with
    // why ("No checkpoint yet", "Busy: ..."). The commands:
    //   menu:    new_game, confirm_new_game, continue, level_select, play_level <id>,
    //            settings, credits, back, quit
    //   pause:   resume, restart_checkpoint, restart_level, settings, main_menu, quit
    //   loading: cancel_travel
    //   error:   retry, main_menu, dismiss (when the level still plays), quit
    bool command(std::string_view command, std::string_view argument = {}, std::string* why = nullptr);
    // Clicks a button of the screen showing, by its id, as the mouse would: what its
    // onclick says runs. False when the screen has no such element.
    bool click(std::string_view element);
    // Opens or closes the pause screen (the pause action does this).
    bool pause();
    bool resume();

    // The "flow" data model the screens show: screen, page, title, level, destination,
    // phase, progress (0..1), percent, can_cancel, error_code, error_message, error_level,
    // can_dismiss, can_new_game, new_game_reason, can_continue, continue_reason,
    // can_restart_checkpoint, checkpoint_reason, checkpoint_state, can_level_select, levels, settings_reason,
    // credits, message (the last refusal).
    const UIValue& value(std::string_view name) const;
    GameUI& ui() { return *m_ui; }
    // The last ticket a shell command got (0: none yet).
    uint64_t lastTicket() const { return m_lastTicket; }
    // The failure the error screen shows, if any.
    struct Failure {
        TravelRequest request;
        TravelError code = TravelError::None;
        std::string message;
        bool levelPlays = false;   // the level it came from still plays (dismiss goes back to it)
    };
    const std::optional<Failure>& failure() const { return m_failure; }
    // Project screens that didn't load and were replaced by the built-in ones.
    const std::vector<std::string>& problems() const { return m_problems; }

    // Binds the pause action to Escape and a gamepad's Start where nothing binds it yet.
    static void bindDefaultControls(Input& input);
    // The built-in screens' documents ("builtin:shell/menu", ...), for tools and tests.
    static std::string builtinDocument(ShellScreen screen);

private:
    void onTravelFinished(const TravelCompletion& completion);
    void onUIEvent(const UIEvent& event);
    TravelTicket travel(TravelTicket ticket);   // records the ticket, or the refusal
    void fail(TravelRequest request, TravelError code, std::string message, bool levelPlays);
    bool isMenuLevel(const std::string& level) const;
    void choose();   // which screen shows now
    void publish();   // the flow model
    void refreshContinue();   // asks Setup::continueReason
    void refreshCheckpoint();   // asks the level's SaveGames
    bool refuse(std::string* why, std::string reason);

    Engine* m_engine;
    LevelHost& m_levels;
    Setup m_setup;
    World m_world;   // the shell's UIDocuments: one entity per screen
    Entity m_documents[5] = {};   // by ShellScreen; None unused
    std::unique_ptr<GameUI> m_ui;
    GameFlow::Subscription m_subscription;
    ShellScreen m_screen = ShellScreen::None;
    std::string m_page = "main";
    bool m_paused = false;   // the pause screen is open (the session paused if it could be)
    bool m_pausedSession = false;   // and we paused the session
    uint64_t m_pausedEpoch = 0;
    uint64_t m_lastTicket = 0;
    std::optional<Failure> m_failure;
    std::string m_message;
    std::string m_continueReason = "No saved game to continue yet";
    std::string m_checkpointReason = "No checkpoint reached yet";
    std::vector<std::string> m_problems;
    // Mouse capture while a screen shows, given back when it goes in the same level.
    bool m_mouseWasCaptured = false;
    uint64_t m_captureEpoch = 0;
    bool m_releasedMouse = false;
};

}   // namespace shockwave
