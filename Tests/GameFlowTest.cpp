// Game code and circuits travelling (S05, docs/LevelLifetime.md) without a window: the
// game.flow() facade, game.arrival, and the Level.* circuit nodes in a game circuit:
//   - Level.RequestTravel's accepted and refused outputs, with the ticket and code;
//   - Level.OnTravelFinished runs once, in the level playing afterwards, with
//     Level.TravelStatus reading the ended ticket;
//   - a cancel before the boundary reaches the level that stays;
//   - Level.UseExit, Level.RestartLevel and Level.ReturnToMenu;
//   - a flow kept from an ended level, and a context without one, refuse;
//   - the application shell (AppShell): menu, pause, loading and error screens, their
//     buttons, a button pressed twice, cancel, failures before and after the boundary,
//     a broken project screen replaced by the built-in one, and screens switched off;
//   - flow test scenarios: parsing, and a run through the shell.
#include <Assets/Assets.h>
#include <Circuits/Circuit.h>
#include <Circuits/CircuitWorld.h>
#include <Engine/Components.h>
#include <Engine/Levels.h>
#include <Engine/Prefab.h>
#include <GameFramework/AppShell.h>
#include <Engine/Localization.h>
#include <Engine/Preferences.h>
#include <GameFramework/FlowTest.h>
#include <GameFramework/GameInstance.h>
#include <GameFramework/LevelHost.h>
#include <GameFramework/PlaySession.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            std::fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #x);                                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

const char* kGameCircuit = R"(shockwave_circuit 1
var finished Int 0
var arrivedLevel String ""
var arrivedFrom String ""
var lastCode String ""
var lastPhase String ""
var ticket Int 0
node done Level.OnTravelFinished
node count Set.finished
  exec done.then
  in value = inc.result
node inc Math.AddInt
  in a = finished
  in b = 1
node level Set.arrivedLevel
  exec count.then
  in value = done.level
node from Set.arrivedFrom
  exec level.then
  in value = done.from
node code Set.lastCode
  exec from.then
  in value = done.code
node status Level.TravelStatus
  in ticket = done.ticket
node phase Set.lastPhase
  exec code.then
  in value = status.phase
node go Event.Message
  in message = "go"
node travel Level.RequestTravel
  exec go.then
  in level = go.value
node keep Set.ticket
  exec travel.accepted
  in value = travel.ticket
node refused Set.lastCode
  exec travel.refused
  in value = travel.code
node toB Event.Message
  in message = "b"
node travelB Level.RequestTravel
  exec toB.then
  in level = "b"
  in spawn = "east"
node keepB Set.ticket
  exec travelB.accepted
  in value = travelB.ticket
node leave Event.Message
  in message = "exit"
node useExit Level.UseExit
  exec leave.then
  in exit = "exit0"
node restartMessage Event.Message
  in message = "restart"
node restart Level.RestartLevel
  exec restartMessage.then
node menuMessage Event.Message
  in message = "menu"
node menu Level.ReturnToMenu
  exec menuMessage.then
)";

std::string levelOf(const World& world) {
    std::string id;
    const_cast<World&>(world).each<const Name>([&](Entity, const Name& name) {
        if (name.value.rfind("level:", 0) == 0) id = name.value.substr(6);
    });
    return id;
}

LevelArrival lastArrival;
std::string flowLevel;
LevelFlow kept;   // from the first level, to try once it has ended

class Mode : public GameMode {
public:
    void beginPlay(GameContext& game) override {
        lastArrival = game.arrival;
        flowLevel = game.flow().level();
        CHECK(game.flow().available() && game.flow().epoch() == game.levelEpoch);
        if (!kept.available()) kept = game.flow();
        if (flowLevel == "boom") throw std::runtime_error("beginPlay of boom threw");
    }
};

class Instance : public GameInstance {
public:
    std::unique_ptr<GameMode> createGameMode(const World&) override { return std::make_unique<Mode>(); }
};

void buildLevel(World& world, const std::string& id) {
    world.add<Name>(world.create(), Name{"level:" + id});
    for (const auto& [spawn, x] : std::vector<std::pair<std::string, float>>{{"start", 1.0f}, {"east", 5.0f}}) {
        const Entity e = world.create();
        world.add<Transform>(e, Transform{{x, 0, 0}});
        world.add<LevelSpawn>(e, LevelSpawn{spawn});
    }
    const Entity exit = world.create();
    world.add<Name>(exit, Name{"exit0"});
    world.add<LevelExit>(exit, LevelExit{"b", "east", true, true});
}

const Value* var(LevelHost& host, const char* name) {
    CircuitWorld* circuits = host.session() ? host.session()->circuits() : nullptr;
    return circuits ? circuits->variable(kNullEntity, name) : nullptr;
}
int intVar(LevelHost& host, const char* name) {
    const Value* v = var(host, name);
    return v ? v->i : -1;
}
std::string textVar(LevelHost& host, const char* name) {
    const Value* v = var(host, name);
    return v ? v->text : std::string("<none>");
}

// A message to the game circuit, run by the next tick.
void say(LevelHost& host, const std::string& message, const std::string& value = {}) {
    CHECK(host.session()->circuits()->send(kNullEntity, message, kNullEntity, 0.0f, value));
    host.session()->update(1.0 / 60.0);
}

void across(LevelHost& host) {
    host.process();   // prepared, at the boundary
    host.process();   // across
}

void testCircuits(const fs::path& content) {
    {
        fs::create_directories(content / "Circuits");
        std::ofstream(content / "Circuits/game.swcircuit", std::ios::binary) << kGameCircuit;
    }
    World world;
    JobSystem jobs(1);
    Instance instance;
    LevelHost::Hooks hooks;
    hooks.resolve = [](TravelRequest& request, TravelError& code, std::string& error) {
        if (request.level == "a" || request.level == "b" || request.level == "menu") return true;
        code = TravelError::UnknownLevel;
        error = "no level \"" + request.level + "\"";
        return false;
    };
    hooks.load = [](const LevelRequest& request, World& into, std::string&) {
        buildLevel(into, request.level);
        return true;
    };
    hooks.configure = [](PlaySession& session, const LevelRequest&) {
        session.setGameCircuit("Circuits/game.swcircuit");
    };
    LevelHost host(world, jobs, instance, nullptr, hooks);
    host.setStaged(true);
    host.setMenuLevel("menu");
    buildLevel(world, "a");
    host.start({"a"});
    const std::vector<std::string> problems = host.session()->circuits()->diagnostics();
    CHECK(problems.empty());
    for (const auto& p : problems) std::fprintf(stderr, "  %s\n", p.c_str());
    CHECK(flowLevel == "a" && lastArrival.found && lastArrival.spawn == "start");
    host.session()->update(1.0 / 60.0);

    // Accepted: the ticket comes out; the level that asked never hears how it ended.
    say(host, "go", "b");
    const int ticket = intVar(host, "ticket");
    CHECK(ticket > 0 && uint64_t(ticket) == host.activeTicket());
    host.process();
    CHECK(intVar(host, "finished") == 0);   // waiting at the boundary: nothing delivered
    host.process();
    CHECK(levelOf(world) == "b" && flowLevel == "b");
    CHECK(intVar(host, "finished") == 1 && textVar(host, "arrivedLevel") == "b" && textVar(host, "arrivedFrom") == "a");
    CHECK(textVar(host, "lastCode") == "None" && textVar(host, "lastPhase") == "succeeded");

    // Refused: the refused output, with the code; nothing is delivered.
    host.session()->update(1.0 / 60.0);
    say(host, "go", "nowhere");
    CHECK(textVar(host, "lastCode") == "UnknownLevel" && intVar(host, "finished") == 1 && !host.activeTicket());

    // A literal level and spawn.
    say(host, "b");
    across(host);
    CHECK(lastArrival.spawn == "east" && lastArrival.found && intVar(host, "finished") == 1);

    // Cancelled at the boundary: the level that stays hears it, once.
    host.session()->update(1.0 / 60.0);
    say(host, "go", "a");
    const int cancelled = intVar(host, "ticket");
    host.process();
    CHECK(host.cancel(uint64_t(cancelled)) == TravelError::None);
    CHECK(intVar(host, "finished") == 2 && textVar(host, "lastCode") == "Cancelled");
    CHECK(textVar(host, "lastPhase") == "cancelled" && levelOf(world) == "b");

    // An exit, a restart and the menu, from circuits.
    say(host, "exit");
    CHECK(host.activeTicket() != 0);
    across(host);
    CHECK(levelOf(world) == "b" && lastArrival.spawn == "east" && textVar(host, "arrivedFrom") == "b");
    host.session()->update(1.0 / 60.0);
    say(host, "restart");
    across(host);
    CHECK(levelOf(world) == "b" && lastArrival.spawn == "east" && intVar(host, "finished") == 1);
    host.session()->update(1.0 / 60.0);
    say(host, "menu");
    across(host);
    CHECK(levelOf(world) == "menu" && textVar(host, "arrivedLevel") == "menu");

    // A flow kept from the first level is stale now; no flow at all can't travel.
    CHECK(kept.request("a").error == TravelError::StaleLevel);
    CHECK(kept.restartLevel().error == TravelError::StaleLevel);
    CHECK(LevelFlow().request("a").error == TravelError::NotPlaying && !LevelFlow().available());
    host.stop();
}

// ---- The application shell ---------------------------------------------------------------

TravelRequest to_request(const char* level) {
    TravelRequest r;
    r.level = level;
    return r;
}

struct ShellRig {
    World world;
    JobSystem jobs{1};
    Instance instance;
    std::unique_ptr<LevelHost> host;
    std::unique_ptr<AppShell> shell;
    std::map<uint64_t, int> heard;   // completions per ticket
    GameFlow::Subscription subscription;

    ShellRig(const fs::path& content, AppShellScreens screens = AppShellScreens{true, true, true, true, true}) {
        LevelHost::Hooks hooks;
        hooks.resolve = [](TravelRequest& request, TravelError& code, std::string& error) {
            for (const char* known : {"menu", "a", "b", "broken", "boom"})
                if (request.level == known) return true;
            code = TravelError::UnknownLevel;
            error = "no level \"" + request.level + "\"";
            return false;
        };
        hooks.load = [](const LevelRequest& request, World& into, std::string& error) {
            if (request.level == "broken") {
                error = "Scenes/broken.swscene doesn't parse";
                return false;
            }
            buildLevel(into, request.level);
            return true;
        };
        saveFolder = content.parent_path() / "ShellSaves";
        fs::remove_all(saveFolder);
        hooks.configure = [this](PlaySession& session, const LevelRequest&) { session.setSaveFolder(saveFolder); };
        host = std::make_unique<LevelHost>(world, jobs, instance, nullptr, hooks);
        host->setStaged(true);
        host->setMenuLevel("menu");
        subscription = host->subscribe([this](const TravelCompletion& c) { ++heard[c.ticket]; });
        buildLevel(world, "menu");
        host->start({"menu"});
        AppShell::Setup setup;
        setup.title = "Shell test";
        setup.screens = screens;
        setup.content = content;
        setup.newGameLevel = "a";
        setup.levels = {
            {"menu", "Menu", true, false},
            {"a", "Level A", false, true},
            {"b", "", false, true},
            {"broken", "", false, true},
            {"boom", "", false, true}};
        setup.quit = [this] { quit = true; };
        setup.hasProgress = [this] { return progress; };
        setup.continueReason = [this] { return continueReason; };
        setup.continueGame = [this](TravelRequest& r, std::string& why) {
            if (!continueReason.empty()) return (why = continueReason), false;
            r.level = "b";
            r.restore = std::make_shared<SaveRestore>();
            buildLevel(r.restore->world, "b");
            r.restore->world.add<Name>(r.restore->world.create(), Name{"restored"});
            r.restore->info.format = 2;
            r.restore->info.campaign["chapter"] = "2";
            return true;
        };
        // The Settings page edits these, with no engine.
        std::string error;
        fs::remove_all(content.parent_path() / "ShellConfig");
        fs::create_directories(content.parent_path() / "ShellConfig");
        preferences.setClock(&ShellRig::clock);
        CHECK(preferences.configure({}, content.parent_path() / "ShellConfig" / "Preferences.swprefs", {}, error));
        setup.preferences = &preferences;
        setup.graphics = &graphics;
        text.addBuiltin("en", AppShell::builtinText());
        setup.localization = &text;
        shell = std::make_unique<AppShell>(nullptr, *host, setup);
        frame();
    }
    static inline double now = 100;
    static double clock() { return now; }
    Preferences preferences;
    Graphics graphics;
    Localization text;
    ~ShellRig() {
        shell.reset();
        host->stop();
    }
    // One frame of the player: the shell, the level, then travel.
    void frame() {
        shell->update(1.0 / 60.0);
        if (host->session()) host->session()->update(1.0 / 60.0);
        host->process();
    }
    std::string value(const char* name) const { return shell->value(name).toText(); }
    bool quit = false;
    bool progress = false;
    std::string continueReason = "No saved game to continue yet";
    fs::path saveFolder;
};

void testShellScreens(const fs::path& content) {
    AppShellScreens s;
    std::string error;
    CHECK(parseAppShellScreens("off", s, error) && !s.any() && appShellScreensText(s) == "off");
    CHECK(
        parseAppShellScreens("all", s, error) && s.menu && s.pause && s.levelSelect && appShellScreensText(s) == "all");
    CHECK(parseAppShellScreens("menu loading", s, error) && s.menu && s.loading && !s.pause && !s.error);
    CHECK(appShellScreensText(s) == "menu loading");
    CHECK(appShellScreensText(AppShellScreens{}) == "loading error");
    CHECK(!parseAppShellScreens("menu shop", s, error) && error.find("shop") != std::string::npos);
    CHECK(!parseAppShellScreens("off menu", s, error));
    CHECK(!parseAppShellScreens("", s, error));

    // The built-in screens lay out cleanly, with nothing but the engine's font.
    ShellRig rig(content);
    for (const ShellScreen screen : {ShellScreen::Menu, ShellScreen::Pause, ShellScreen::Loading, ShellScreen::Error}) {
        const std::string name = std::string("builtin:shell/") + shellScreenName(screen);
        const auto problems = rig.shell->ui().validate(name);
        CHECK(problems.empty());
        for (const auto& p : problems) std::fprintf(stderr, "  %s: %s\n", name.c_str(), p.message.c_str());
    }
    CHECK(!rig.shell->ui().validate("builtin:shell/nothing").empty());
    CHECK(rig.shell->problems().empty());
}

void testShellFlow(const fs::path& content) {
    ShellRig rig(content);
    AppShell& shell = *rig.shell;
    LevelHost& host = *rig.host;

    // The menu shows over the menu level, its disabled buttons saying why.
    CHECK(shell.screen() == ShellScreen::Menu && shell.capturesInput() && rig.value("screen") == "menu");
    CHECK(rig.value("title") == "Shell test" && rig.value("can_continue") == "false");
    CHECK(!rig.value("continue_reason").empty() && rig.value("can_level_select") == "true");
    std::string why;
    CHECK(!shell.command("continue", {}, &why) && !why.empty());
    CHECK(!shell.pause());   // the menu level has the menu instead
    CHECK(shell.command("credits") && shell.page() == "credits" && shell.command("back") && shell.page() == "main");
    CHECK(shell.command("level_select") && shell.page() == "levels" && shell.command("back"));

    // New game, pressed twice: one travel; the second is Busy and the first carries on.
    CHECK(shell.click("new_game"));
    const uint64_t first = shell.lastTicket();
    CHECK(first != 0 && host.activeTicket() == first);
    CHECK(!shell.command("new_game", {}, &why) && why.find("under way") != std::string::npos);
    CHECK(!shell.failure());   // Busy isn't an error
    rig.frame();   // prepared, at the boundary
    CHECK(shell.screen() == ShellScreen::Loading && host.status(first)->phase == TravelPhase::AwaitingBoundary);
    shell.update(1.0 / 60.0);   // the frame that renders between the two
    CHECK(shell.screen() == ShellScreen::Loading && rig.value("phase") == "awaiting_boundary");
    CHECK(rig.value("can_cancel") == "true" && rig.value("destination") == "a" && rig.value("percent") == "45");
    CHECK(rig.value("progress_width") == "45%");
    rig.frame();   // across
    rig.frame();
    CHECK(host.current().level == "a" && host.playing() && shell.screen() == ShellScreen::None);
    CHECK(!shell.capturesInput() && rig.heard[first] == 1);

    // Pause: the session pauses; Resume plays on.
    CHECK(shell.pause() && shell.paused() && host.session()->state() == PlaySession::State::Paused);
    rig.frame();
    CHECK(shell.screen() == ShellScreen::Pause && rig.value("can_restart_checkpoint") == "false");
    CHECK(shell.click("resume") && !shell.paused() && host.session()->state() == PlaySession::State::Playing);
    rig.frame();
    CHECK(shell.screen() == ShellScreen::None);

    // Restart level from the pause screen, cancelled at the boundary: back to the pause
    // screen, still paused, same level.
    const uint64_t epoch = host.epoch();
    CHECK(shell.pause());
    rig.frame();
    CHECK(shell.click("restart_level"));
    const uint64_t restart = shell.lastTicket();
    rig.frame();
    shell.update(1.0 / 60.0);
    CHECK(shell.screen() == ShellScreen::Loading);
    CHECK(shell.click("cancel_travel") && host.status(restart)->phase == TravelPhase::Cancelled);
    rig.frame();
    CHECK(shell.screen() == ShellScreen::Pause && host.epoch() == epoch);
    CHECK(host.session()->state() == PlaySession::State::Paused && rig.heard[restart] == 1);
    CHECK(!shell.command("cancel_travel", {}, &why));   // nothing loading now

    // Restart for real: a fresh, unpaused level, the pause screen gone.
    CHECK(shell.click("restart_level"));
    for (int i = 0; i < 3; ++i) rig.frame();
    CHECK(host.epoch() > epoch && host.current().level == "a" && !shell.paused());
    CHECK(host.session()->state() == PlaySession::State::Playing && shell.screen() == ShellScreen::None);

    // A level that doesn't load: the error screen, and the level plays on behind it.
    CHECK(shell.pause());
    rig.frame();
    CHECK(shell.click("main_menu"));
    for (int i = 0; i < 3; ++i) rig.frame();
    CHECK(host.current().level == "menu" && shell.screen() == ShellScreen::Menu);
    CHECK(shell.command("play_level", "broken"));
    for (int i = 0; i < 3; ++i) rig.frame();
    CHECK(shell.screen() == ShellScreen::Error && shell.failure() && shell.failure()->code == TravelError::LoadFailed);
    CHECK(rig.value("error_code") == "LoadFailed" && rig.value("error_level") == "broken");
    CHECK(rig.value("error_message").find("doesn't parse") != std::string::npos && rig.value("can_dismiss") == "true");
    CHECK(shell.click("retry"));   // fails the same way
    for (int i = 0; i < 3; ++i) rig.frame();
    CHECK(shell.screen() == ShellScreen::Error && host.current().level == "menu" && host.playing());
    CHECK(shell.click("dismiss") && !shell.failure());
    rig.frame();
    CHECK(shell.screen() == ShellScreen::Menu);
    CHECK(!shell.command("play_level", "menu", &why));   // not in Level select

    // A level that doesn't start: nothing plays; Retry and Main menu still travel.
    CHECK(shell.command("play_level", "boom"));
    for (int i = 0; i < 3; ++i) rig.frame();
    CHECK(!host.playing() && shell.screen() == ShellScreen::Error && shell.failure()->code == TravelError::StartFailed);
    CHECK(rig.value("can_dismiss") == "false" && !shell.command("dismiss", {}, &why));
    CHECK(!shell.pause());
    CHECK(shell.click("main_menu"));
    for (int i = 0; i < 3; ++i) rig.frame();
    CHECK(host.playing() && host.current().level == "menu" && shell.screen() == ShellScreen::Menu && !shell.failure());

    // New game over progress asks first; confirming resets the campaign.
    rig.progress = true;
    CHECK(shell.command("new_game") && shell.page() == "confirm" && host.activeTicket() == 0);
    CHECK(shell.command("back") && shell.page() == "main");
    CHECK(shell.command("new_game") && shell.click("confirm_new_game"));
    const auto reset = host.status(shell.lastTicket());
    CHECK(reset && reset->request.resetCampaign && reset->request.reason == TravelReason::NewGame);
    for (int i = 0; i < 3; ++i) rig.frame();
    CHECK(host.current().level == "a" && shell.screen() == ShellScreen::None);

    // Continue (S06): once there's a save, the menu offers it; it travels with the save
    // (LoadSave), which is in before the level's first tick, campaign and all.
    rig.continueReason.clear();
    CHECK(shell.command("main_menu"));
    for (int i = 0; i < 3; ++i) rig.frame();
    CHECK(shell.screen() == ShellScreen::Menu && rig.value("can_continue") == "true");
    CHECK(shell.click("continue"));
    const auto resumed = host.status(shell.lastTicket());
    CHECK(resumed && resumed->request.reason == TravelReason::LoadSave && !resumed->request.restore);
    for (int i = 0; i < 3; ++i) rig.frame();
    CHECK(host.current().level == "b" && host.session()->context().startReason == StartReason::RestoreSave);
    CHECK(findEntity(host.session()->world(), "restored") && host.campaign().get("chapter") == "2");

    // Restart checkpoint (S06 part 2): off until a checkpoint is written, then it takes the
    // level back to it and closes the pause screen.
    rig.frame();
    CHECK(shell.pause() && rig.value("can_restart_checkpoint") == "false");
    CHECK(!shell.command("restart_checkpoint") && shell.screen() == ShellScreen::Pause);
    CHECK(shell.command("resume"));
    World& level = host.session()->world();
    const Entity spawnPoint = findLevelSpawn(level, "start");
    level.add<Checkpoint>(spawnPoint, Checkpoint{"cp", "start", true});
    CHECK(host.session()->saves()->checkpoint("cp"));
    host.session()->saves()->wait();
    rig.frame();
    CHECK(rig.value("checkpoint_state") == "saved");
    level.get<Transform>(spawnPoint)->position.x = 42;
    CHECK(shell.pause() && rig.value("can_restart_checkpoint") == "true");
    CHECK(shell.click("restart_checkpoint") && shell.screen() == ShellScreen::None);
    host.session()->saves()->wait();
    rig.frame();
    CHECK(host.current().level == "b" && host.session()->world().get<Transform>(spawnPoint)->position.x == 1);

    // Quit.
    CHECK(!shell.click("quit"));   // no screen shows in a playing level
    CHECK(shell.command("quit") && rig.quit);
    // Every ticket ended exactly once.
    for (const auto& [ticket, n] : rig.heard) CHECK(n == 1);
    CHECK(!shell.command("dance", {}, &why) && why.find("Unknown") != std::string::npos);
}

// The Settings page: tabs and rows from the preference table, a draft that Apply uses and
// saves, Back that drops it, Defaults, the display keep-or-revert, and translated text.
void testShellSettings(const fs::path& content) {
    ShellRig rig(content);
    AppShell& shell = *rig.shell;
    std::string why;
    CHECK(rig.value("can_settings") == "true");
    CHECK(shell.value("text").field("shell") && shell.value("text").field("shell")->field("new_game"));
    CHECK(!shell.command("pref_next", "master_volume", &why));   // the page isn't open
    CHECK(shell.click("settings") && shell.page() == "settings" && rig.value("settings_tab") == "display");
    rig.frame();
    // Display first: window mode and one Resolution row (width and height together).
    const UIValue& rows = shell.value("settings_rows");
    CHECK(rows.size() == 2 && rows[1].field("key")->text() == "resolution");
    CHECK(shell.value("settings_tabs").size() == 6);

    // Audio: < and > step the volume, as the mouse would click them.
    CHECK(shell.click("tab-audio") && rig.value("settings_tab") == "audio");
    CHECK(shell.settingValue("master_volume") == "100%" && rig.value("settings_changed") == "false");
    CHECK(shell.click("prev-master_volume"));
    CHECK(shell.click("prev-master_volume"));
    CHECK(shell.settingValue("master_volume") == "80%" && rig.value("settings_changed") == "true");
    CHECK(rig.preferences.current().masterVolume == 1);   // only the draft so far
    CHECK(shell.command("pref_next", "mute_when_unfocused") && shell.settingValue("mute_when_unfocused") == "On");
    CHECK(!shell.command("pref_next", "no_such_thing", &why) && why.find("no_such_thing") != std::string::npos);
    // Back drops the draft.
    CHECK(shell.command("back") && shell.page() == "main" && rig.preferences.current().masterVolume == 1);
    CHECK(shell.command("settings") && shell.settingValue("master_volume") == "100%");
    // Apply uses it and saves it to the profile.
    CHECK(shell.command("settings_tab", "audio") && shell.command("pref_prev", "music_volume"));
    CHECK(shell.command("apply_settings", {}, &why));
    CHECK(std::abs(rig.preferences.current().musicVolume - 0.9f) < 1e-4f && rig.value("message") == "Settings saved");
    CHECK(fs::exists(rig.preferences.profileFile()));
    CHECK(rig.preferences.stack().layer(PreferenceLayer::Profile).count("music_volume") == 1);
    CHECK(rig.value("settings_changed") == "false");
    // Steps stop at the ends of the range.
    for (int i = 0; i < 15; ++i) shell.command("pref_next", "music_volume");
    CHECK(shell.settingValue("music_volume") == "100%");
    // Field of view: the game's own, then 50..120.
    CHECK(shell.command("settings_tab", "controls") && shell.settingValue("field_of_view") == "Game default");
    CHECK(shell.command("pref_next", "field_of_view") && shell.draftPreferences().fieldOfView == 50);
    CHECK(shell.command("pref_prev", "field_of_view") && shell.draftPreferences().fieldOfView == 0);
    // Graphics: presets cycle, and the frame cap through its list.
    CHECK(shell.command("settings_tab", "graphics"));
    CHECK(shell.command("pref_next", "graphics.frame_limit") && shell.draftGraphics().frameLimit == 30);
    CHECK(shell.settingValue("graphics.frame_limit") == "30 fps");
    CHECK(shell.command("pref_next", "graphics.preset"));
    CHECK(!shell.command("settings_tab", "shop", &why));
    // Defaults: the shipped values, still a draft.
    CHECK(shell.command("reset_settings") && shell.draftPreferences().musicVolume == 1);
    CHECK(shell.command("cancel_settings") && shell.page() == "main");

    // A display change waits to be kept; the page counts down, then goes back by itself.
    CHECK(shell.command("settings") && shell.command("pref_next", "window_mode"));
    CHECK(shell.draftPreferences().windowMode == WindowMode::Borderless);
    CHECK(shell.command("apply_settings", {}, &why) && rig.preferences.displayPending());
    rig.frame();
    CHECK(rig.value("display_pending") == "true" && rig.value("display_countdown").find("15") != std::string::npos);
    CHECK(!shell.command("pref_next", "master_volume", &why));   // keep or go back first
    // The saved profile has the display that worked until it's kept.
    CHECK(rig.preferences.stack().layer(PreferenceLayer::Profile).count("window_mode") == 0);
    CHECK(shell.command("keep_display") && !rig.preferences.displayPending());
    CHECK(rig.preferences.stack().layer(PreferenceLayer::Profile).at("window_mode") == "borderless");
    CHECK(shell.command("pref_prev", "window_mode") && shell.command("apply_settings"));
    ShellRig::now += Preferences::kKeepSeconds + 1;
    rig.preferences.update();
    rig.frame();
    CHECK(!rig.preferences.displayPending() && rig.preferences.current().windowMode == WindowMode::Borderless);
    CHECK(rig.value("display_pending") == "false");

    // The shell's words come from the catalog: a French one replaces the built-in English.
    const fs::path folder = content.parent_path() / "ShellText";
    fs::create_directories(folder);
    {
        std::ofstream(folder / "fr.swstrings")
            << "swstrings 1\nshell.new_game \"Nouvelle partie\"\nshell.no_new_game \"Pas de premier niveau\"\n";
    }
    rig.text.load(folder, "en");
    rig.text.setLocale("fr");
    rig.frame();
    CHECK(shell.value("text").field("shell")->field("new_game")->text() == "Nouvelle partie");
    CHECK(shell.value("text").field("shell")->field("resume")->text() == "Resume");   // falls back
    CHECK(shell.command("settings") && shell.command("settings_tab", "language"));
    CHECK(shell.command("pref_next", "language") && shell.draftPreferences().language == "fr");
}

void testShellOverrides(const fs::path& content) {
    // A project screen that loads replaces the built-in one; a broken one is replaced by it.
    const fs::path project = content.parent_path() / "ShellContent";
    fs::create_directories(project / "Shell");
    std::ofstream(project / "Shell/menu.rml", std::ios::binary)
        << "<rml><head><style>body { font-family: \"Noto Sans\"; } button { display: block; }</style></head>"
           "<body data-model=\"flow\"><button id=\"go\" onclick=\"new_game\">Play {{title}}</button></body></rml>";
    std::ofstream(project / "Shell/error.rml", std::ios::binary)
        << "<rml><head><link type=\"text/rcss\" href=\"missing.rcss\"/></head>"
           "<body data-model=\"flow\"><p>{{error_message}}</p></body></rml>";
    {
        ShellRig rig(project);
        CHECK(rig.shell->problems().size() == 1);
        CHECK(!rig.shell->problems().empty() && rig.shell->problems()[0].find("Shell/error.rml") == 0);
        CHECK(rig.shell->click("go") && rig.shell->lastTicket() != 0);   // the project's own menu
        for (int i = 0; i < 3; ++i) rig.frame();
        CHECK(rig.host->current().level == "a");
        CHECK(rig.shell->command("play_level", "broken"));
        for (int i = 0; i < 3; ++i) rig.frame();
        CHECK(rig.shell->screen() == ShellScreen::Error && rig.shell->click("retry"));   // the built-in one
    }
    // Screens switched off don't show; what's left still works.
    {
        AppShellScreens screens;   // loading and error
        ShellRig rig(project, screens);
        CHECK(rig.shell->screen() == ShellScreen::None && !rig.shell->pause());
        CHECK(rig.host->request(to_request("a")).id != 0);
        rig.frame();
        rig.shell->update(1.0 / 60.0);
        CHECK(rig.shell->screen() == ShellScreen::Loading);
        rig.frame();
        CHECK(rig.shell->screen() == ShellScreen::None && rig.host->current().level == "a");
    }
    {
        ShellRig rig(project, AppShellScreens{false, false, false, false, false});
        CHECK(rig.host->request(to_request("broken")).id != 0);
        for (int i = 0; i < 3; ++i) rig.frame();
        CHECK(rig.shell->screen() == ShellScreen::None && !rig.shell->failure());
    }
}

// ---- Flow test scenarios -------------------------------------------------------------------

void testScenarios(const fs::path& content) {
    FlowScenario scenario;
    std::string error;
    CHECK(parseFlowScenario(
        R"({"format": 1, "steps": [{"click": "new_game", "timeout": 2}, {"quit": true}]})", scenario, error));
    CHECK(scenario.steps.size() == 2 && scenario.steps[0].timeout == 2 && scenario.timeout == 60);
    auto refused = [&](const char* text, const char* says) {
        FlowScenario ignored;
        std::string why;
        const bool ok = !parseFlowScenario(text, ignored, why) && why.find(says) != std::string::npos;
        if (!ok) std::fprintf(stderr, "  scenario %s: \"%s\"\n", text, why.c_str());
        return ok;
    };
    CHECK(refused(R"({"steps": []})", "format"));
    CHECK(refused(R"({"format": 2, "steps": []})", "format must be 1"));
    CHECK(refused(R"({"format": 1, "steps": [{"click": "a", "travel": "b"}]})", "only one"));
    CHECK(refused(R"({"format": 1, "steps": [{"shell": "rm -rf /"}]})", "it needs one of"));
    CHECK(refused(R"({"format": 1, "steps": [{"click": "a", "script": "x"}]})", "click has no \"script\""));
    CHECK(refused(R"({"format": 1, "steps": [{"wait_phase": "done"}]})", "isn't a travel phase"));
    CHECK(refused(R"({"format": 1, "steps": [{"travel": "a", "expect": "Nope"}]})", "isn't a travel code"));
    CHECK(refused(R"({"format": 1, "steps": [{"assert_value": "score", "equals": "1"}]})", "flow.<value>"));
    CHECK(refused(R"({"format": 1, "steps": [{"assert_value": "flow.screen"}]})", "needs \"equals\""));
    CHECK(refused(R"({"format": 1, "timeout": 0, "steps": []})", "timeout"));
    CHECK(refused(R"({"format": 1, "steps": [{"press": "jump"}]})", "press takes"));
    CHECK(refused(R"({"format": 1, "steps": [{"quit": true}]} x)", "after"));
    CHECK(refused(R"({"format": 1, "format": 1, "steps": []})", "twice"));
    // S07: real input steps.
    CHECK(parseFlowScenario(
        R"({"format": 1, "steps": [
            {"hold": "key W", "until": "game.zone", "equals": "range"},
            {"hold": "pad_axis left_y +", "value": 0.5, "seconds": 1.5},
            {"tap": "pad south"}, {"look": [120, -20], "frames": 4}, {"wait": 0.5},
            {"wait_value": "flow.screen", "equals": "none"}]})",
        scenario, error));
    CHECK(scenario.steps.size() == 6 && scenario.steps[0].kind == FlowStep::Kind::Hold);
    CHECK(scenario.steps[1].binding.source == InputBinding::Source::GamepadAxis && scenario.steps[1].value == 0.5f);
    CHECK(scenario.steps[1].seconds == 1.5 && scenario.steps[1].timeout >= 11.5);
    CHECK(scenario.steps[3].lookX == 120 && scenario.steps[3].lookY == -20 && scenario.steps[3].frames == 4);
    CHECK(refused(R"({"format": 1, "steps": [{"hold": "key Nope", "seconds": 1}]})", "isn't a binding"));
    CHECK(refused(R"({"format": 1, "steps": [{"hold": "key W"}]})", "either \"seconds\""));
    CHECK(refused(
        R"({"format": 1, "steps": [{"hold": "key W", "seconds": 1, "until": "game.a", "equals": "b"}]})", "either"));
    CHECK(refused(R"({"format": 1, "steps": [{"hold": "key W", "until": "zone", "equals": "b"}]})", "until names"));
    CHECK(refused(R"({"format": 1, "steps": [{"tap": "joy * axis 0 +"}]})", "not joysticks"));
    CHECK(refused(R"({"format": 1, "steps": [{"look": [1]}]})", "look is written"));
    CHECK(refused(R"({"format": 1, "steps": [{"wait": -1}]})", "wait is a number"));
    CHECK(refused(R"({"format": 1, "steps": [{"wait_value": "game.a"}]})", "needs \"equals\""));
    CHECK(refused(R"({"format": 1, "steps": [{"tap": "key E", "value": 1}]})", "tap has no \"value\""));

    // A run: through the menu to a, a refusal, then a failing assertion stops it.
    ShellRig rig(content);
    CHECK(parseFlowScenario(
        R"({"format": 1, "name": "rig", "steps": [
            {"assert_value": "flow.screen", "equals": "menu"},
            {"click": "new_game"},
            {"assert_value": "flow.screen", "equals": "loading"},
            {"wait_phase": "succeeded"},
            {"assert_level": "a"},
            {"travel": "nowhere", "expect": "UnknownLevel"},
            {"press": "pause"},
            {"assert_value": "flow.screen", "equals": "pause"},
            {"assert_level": "b"},
            {"quit": true}]})",
        scenario, error));
    FlowTest::Hooks hooks;
    hooks.flow = rig.host.get();
    hooks.shell = rig.shell.get();
    hooks.playing = [&] { return rig.host->playing(); };
    FlowTest test(scenario, hooks);
    int frames = 0;
    while (test.frame() == FlowTest::State::Running && frames++ < 100) rig.frame();
    CHECK(test.state() == FlowTest::State::Failed && test.step() == 8);
    CHECK(test.message().find("level \"a\" is playing") != std::string::npos);
    const std::string report = test.reportJson();
    CHECK(report.find("\"failed_step\": 9") != std::string::npos);
    CHECK(report.find("\"screen\": \"pause\"") != std::string::npos);
    CHECK(report.find("\"status\": \"succeeded\"") != std::string::npos);
    if (test.state() != FlowTest::State::Failed) std::fprintf(stderr, "%s", report.c_str());
}

}   // namespace

int main() {
    registerCircuitComponents();
    registerGameUIComponents();
    const fs::path root =
        fs::temp_directory_path() /
        ("shockwave-game-flow-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path content = root / "Content";
    fs::create_directories(content);
    Assets assets;
    prefabs().setup(content, &assets);
    testCircuits(content);
    testShellScreens(content);
    testShellFlow(content);
    testShellSettings(content);
    testShellOverrides(content);
    testScenarios(content);
    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures) {
        std::fprintf(stderr, "game_flow: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    std::printf("game_flow: all checks passed\n");
    return 0;
}
