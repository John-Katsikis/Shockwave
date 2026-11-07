// The application shell's screens and commands: see GameFramework/AppShell.h.
#include <GameFramework/AppShell.h>

#include <Core/Log.h>
#include <Engine/Components.h>
#include <Engine/Engine.h>
#include <GameFramework/LevelHost.h>
#include <Input/Input.h>

#include <cmath>
#include <sstream>

namespace shockwave {

namespace {

const char* kScreenFiles[] = {"", "Shell/menu.rml", "Shell/pause.rml", "Shell/loading.rml", "Shell/error.rml"};
const char* kBuiltinNames[] = {"", "shell/menu", "shell/pause", "shell/loading", "shell/error"};

void registerBuiltins() {
    static bool done = false;
    if (done) return;
    done = true;
    for (const ShellScreen s : {ShellScreen::Menu, ShellScreen::Pause, ShellScreen::Loading, ShellScreen::Error})
        addBuiltinDocument(kBuiltinNames[int(s)], AppShell::builtinDocument(s));
}

bool screenOn(const AppShellScreens& screens, ShellScreen s) {
    switch (s) {
    case ShellScreen::Menu: return screens.menu;
    case ShellScreen::Pause: return screens.pause;
    case ShellScreen::Loading: return screens.loading;
    case ShellScreen::Error: return screens.error;
    case ShellScreen::None: break;
    }
    return false;
}

}   // namespace

bool parseAppShellScreens(std::string_view text, AppShellScreens& out, std::string& error) {
    std::istringstream words{std::string(text)};
    std::string word;
    AppShellScreens screens;
    screens.loading = screens.error = false;
    int count = 0;
    bool off = false, all = false;
    while (words >> word) {
        ++count;
        if (word == "off") {
            off = true;
        } else if (word == "all") {
            all = true;
        } else if (word == "menu") {
            screens.menu = true;
        } else if (word == "level_select") {
            screens.levelSelect = true;
        } else if (word == "pause") {
            screens.pause = true;
        } else if (word == "loading") {
            screens.loading = true;
        } else if (word == "error") {
            screens.error = true;
        } else {
            error = "app_shell: unknown screen \"" + word +
                    "\" (it takes off, all, or any of menu, level_select, pause, loading, error)";
            return false;
        }
    }
    if (count == 0) {
        error = "app_shell takes off, all, or any of menu, level_select, pause, loading, error";
        return false;
    }
    if ((off || all) && count > 1) {
        error = std::string("app_shell ") + (off ? "off" : "all") + " goes on its own";
        return false;
    }
    if (off) screens = AppShellScreens{false, false, false, false, false};
    if (all) screens = AppShellScreens{true, true, true, true, true};
    out = screens;
    return true;
}

std::string appShellScreensText(const AppShellScreens& s) {
    if (!s.any() && !s.levelSelect) return "off";
    if (s.menu && s.levelSelect && s.pause && s.loading && s.error) return "all";
    std::string text;
    auto add = [&](bool on, const char* name) {
        if (on) text += (text.empty() ? "" : " ") + std::string(name);
    };
    add(s.menu, "menu");
    add(s.levelSelect, "level_select");
    add(s.pause, "pause");
    add(s.loading, "loading");
    add(s.error, "error");
    return text;
}

const char* shellScreenName(ShellScreen screen) {
    switch (screen) {
    case ShellScreen::None: return "none";
    case ShellScreen::Menu: return "menu";
    case ShellScreen::Pause: return "pause";
    case ShellScreen::Loading: return "loading";
    case ShellScreen::Error: return "error";
    }
    return "none";
}

void AppShell::bindDefaultControls(Input& input) {
    if (input.actions().count(kPauseAction)) return;   // the project binds it
    input.bindAction(kPauseAction, Key::Escape);
    input.bindAction(kPauseAction, GamepadButton::Start);
}

AppShell::AppShell(Engine* engine, LevelHost& levels, Setup setup)
    : m_engine(engine), m_levels(levels), m_setup(std::move(setup)) {
    registerBuiltins();
    // Its own context and data model; not the engine's game.ui().
    m_ui = std::make_unique<GameUI>(engine, GameUIOptions{"flow", false});
    if (!m_setup.content.empty()) m_ui->setContentFolder(m_setup.content);
    for (const ShellScreen s : {ShellScreen::Menu, ShellScreen::Pause, ShellScreen::Loading, ShellScreen::Error}) {
        if (!screenOn(m_setup.screens, s)) continue;
        const Entity e = m_world.create();
        m_documents[int(s)] = e;
        m_world.add<Name>(e, Name{std::string("Shell ") + shellScreenName(s)});
        UIDocument doc;
        doc.visible = false;
        doc.order = 1000 + int(s);   // the error screen over loading, over pause, over the menu
        doc.focusOnShow = true;
        const std::string builtin = std::string(kBuiltinDocumentPrefix) + kBuiltinNames[int(s)];
        std::error_code ec;
        const bool own =
            !m_setup.content.empty() && std::filesystem::is_regular_file(m_setup.content / kScreenFiles[int(s)], ec);
        doc.document = own ? kScreenFiles[int(s)] : builtin;
        m_world.add<UIDocument>(e, doc);
        if (!own) continue;
        // The project's own screen, unless it doesn't load cleanly: then the built-in one,
        // which needs none of the project's files.
        const bool loaded = m_ui->loaded(m_world, e);
        const auto& problems = m_ui->problems(e);
        if (loaded && problems.empty()) continue;
        const std::string why = problems.empty() ? std::string("it didn't load") : problems.front().message;
        m_problems.push_back(
            std::string(kScreenFiles[int(s)]) + ": " + why + "; showing the built-in " + shellScreenName(s) +
            " screen instead");
        SW_LOG(LogGame, Warning, "{}", m_problems.back());
        m_world.get<UIDocument>(e)->document = builtin;
    }
    m_ui->setEventListener([this](const UIEvent& event) { onUIEvent(event); });
    m_ui->setInputEnabled(engine != nullptr);
    m_subscription = m_levels.subscribe([this](const TravelCompletion& c) { onTravelFinished(c); });
    if (engine) bindDefaultControls(engine->input());
    choose();
    publish();
}

AppShell::~AppShell() {
    m_subscription.reset();
    if (m_ui) m_ui->setEventListener({});
}

bool AppShell::isMenuLevel(const std::string& level) const {
    for (const Level& l : m_setup.levels)
        if (l.id == level) return l.menu;
    return false;
}

void AppShell::update(double frameDt) {
    if (m_engine) {
        Input& input = m_engine->input();
        if (input.wasActionPressed(kPauseAction)) {
            const bool changed = m_paused ? (m_page == "main" ? resume() : (m_page = "main", true)) : pause();
            // The same press mustn't also reach the screen that just opened (Escape is
            // ui_back too) or the game underneath.
            if (changed) input.holdUntilReleased();
        }
    }
    choose();
    publish();
    // The mouse is the screens' while one shows; given back to the same level after.
    if (m_engine) {
        Input& input = m_engine->input();
        if (capturesInput() && !m_releasedMouse) {
            m_mouseWasCaptured = input.isMouseCaptured();
            m_captureEpoch = m_levels.epoch();
            m_releasedMouse = true;
            if (m_mouseWasCaptured) input.setMouseCaptured(false);
        } else if (!capturesInput() && m_releasedMouse) {
            m_releasedMouse = false;
            if (m_mouseWasCaptured && m_captureEpoch == m_levels.epoch()) input.setMouseCaptured(true);
        }
    }
    m_ui->update(m_world, frameDt);
    // A click may have changed what shows: the next frame's update shows it.
    choose();
    publish();
}

void AppShell::render(Renderer& renderer) { m_ui->render(renderer); }

void AppShell::choose() {
    if (m_paused && m_levels.epoch() != m_pausedEpoch) {   // a new level: nothing to resume
        m_paused = false;
        m_pausedSession = false;
    }
    ShellScreen next = ShellScreen::None;
    const uint64_t active = m_levels.activeTicket();
    const auto status = active ? m_levels.status(active) : std::nullopt;
    if (m_setup.screens.loading && status && !travelPhaseTerminal(status->phase)) {
        next = ShellScreen::Loading;
    } else if (m_failure && m_setup.screens.error) {
        next = ShellScreen::Error;
    } else if (m_paused && m_setup.screens.pause) {
        next = ShellScreen::Pause;
    } else if (m_setup.screens.menu && m_levels.playing() && isMenuLevel(m_levels.current().level)) {
        next = ShellScreen::Menu;
    }
    if (next != m_screen) {
        // A screen opens on its first page, without the last one's complaint.
        if (next == ShellScreen::Menu || next == ShellScreen::Pause) m_page = "main";
        if (m_screen != ShellScreen::None || next != ShellScreen::None) m_message.clear();
        m_screen = next;
    }
    for (int s = 1; s < 5; ++s)
        if (m_documents[s].valid())
            if (UIDocument* doc = m_world.get<UIDocument>(m_documents[s])) doc->visible = int(m_screen) == s;
}

void AppShell::publish() {
    GameUI& ui = *m_ui;
    ui.setValue("screen", std::string_view(shellScreenName(m_screen)));
    ui.setValue("page", std::string_view(m_page));
    ui.setValue("title", std::string_view(m_setup.title));
    ui.setValue("level", std::string_view(m_levels.current().level));
    ui.setValue("message", std::string_view(m_message));
    const uint64_t active = m_levels.activeTicket();
    const auto status = active ? m_levels.status(active) : std::nullopt;
    if (status) {
        const float progress = std::isfinite(status->progress) ? status->progress : 0.0f;
        const int percent = int(std::lround(progress * 100.0f));
        ui.setValue("destination", std::string_view(status->request.level));
        ui.setValue("phase", std::string_view(travelPhaseName(status->phase)));
        ui.setValue("progress", double(progress));
        ui.setValue("percent", double(percent));
        ui.setValue("progress_width", std::string_view(std::to_string(percent) + "%"));
        ui.setValue("has_progress", true);
        ui.setValue("can_cancel", status->cancellable && travelPhaseCancellable(status->phase));
    } else {
        ui.setValue("destination", std::string_view(""));
        ui.setValue("phase", std::string_view(""));
        ui.setValue("progress", 0.0);
        ui.setValue("percent", 0.0);
        ui.setValue("progress_width", std::string_view("0%"));
        ui.setValue("has_progress", false);
        ui.setValue("can_cancel", false);
    }
    ui.setValue("error_code", std::string_view(m_failure ? travelErrorName(m_failure->code) : ""));
    ui.setValue("error_message", std::string_view(m_failure ? m_failure->message : std::string()));
    ui.setValue("error_level", std::string_view(m_failure ? m_failure->request.level : std::string()));
    ui.setValue("can_dismiss", m_failure && m_failure->levelPlays && m_levels.playing());
    const bool newGame = !m_setup.newGameLevel.empty();
    ui.setValue("can_new_game", newGame);
    ui.setValue("new_game_reason", std::string_view(newGame ? "" : "This game has no first level to start in"));
    // Saves and checkpoints across levels arrive with campaign progress (S06), settings
    // with S08: the buttons are there, disabled, saying why.
    ui.setValue("can_continue", false);
    ui.setValue("continue_reason", std::string_view("No saved game to continue yet"));
    ui.setValue("can_restart_checkpoint", false);
    ui.setValue("checkpoint_reason", std::string_view("No checkpoint reached yet"));
    ui.setValue("settings_reason", std::string_view("Player settings aren't available in this build yet"));
    ui.setValue("credits", std::string_view(m_setup.credits.empty() ? "Made with Shockwave" : m_setup.credits));
    UIValue levels = UIValue::list();
    if (m_setup.screens.levelSelect)
        for (const Level& l : m_setup.levels)
            if (l.selectable) levels.push(UIValue::record({{"id", l.id}, {"title", l.title.empty() ? l.id : l.title}}));
    ui.setValue("can_level_select", levels.size() > 0);
    ui.setValue("levels", std::move(levels));
}

const UIValue& AppShell::value(std::string_view name) const { return m_ui->value(name); }

bool AppShell::refuse(std::string* why, std::string reason) {
    SW_LOG(LogGame, Info, "Shell: {}", reason);
    m_message = reason;
    if (why) *why = std::move(reason);
    publish();
    return false;
}

void AppShell::fail(TravelRequest request, TravelError code, std::string message, bool levelPlays) {
    if (!m_setup.screens.error) return;   // the application's own fallback handles it
    m_failure = Failure{std::move(request), code, std::move(message), levelPlays};
    m_paused = m_paused && levelPlays;
    m_page = "main";
}

TravelTicket AppShell::travel(TravelTicket ticket) {
    if (ticket) {
        m_lastTicket = ticket.id;
        m_message.clear();
    }
    return ticket;
}

void AppShell::onTravelFinished(const TravelCompletion& c) {
    if (c.status == TravelStatus::Succeeded) {
        m_failure.reset();
        m_message.clear();
        m_page = "main";
    } else if (c.status == TravelStatus::Failed) {
        fail(c.request, c.code, c.message, m_levels.playing());
    }
    choose();
    publish();
}

void AppShell::onUIEvent(const UIEvent& event) {
    if (event.type != "click" && event.type != "back") return;
    if (event.command.empty()) return;
    std::string argument;
    if (event.command == "play_level" && event.element.rfind("level-", 0) == 0)
        argument = std::string(event.element.substr(6));
    std::string why;
    command(event.command, argument, &why);
}

bool AppShell::pause() {
    if (!m_setup.screens.pause || m_paused) return false;
    if (!m_levels.playing() || m_levels.activeTicket() || m_failure) return false;
    if (isMenuLevel(m_levels.current().level)) return false;
    PlaySession* session = m_levels.session();
    // Online, the session keeps running (pause() refuses); the screen still shows.
    m_pausedSession = session->state() == PlaySession::State::Playing && session->pause();
    m_paused = true;
    m_pausedEpoch = m_levels.epoch();
    m_page = "main";
    choose();
    publish();
    return true;
}

bool AppShell::resume() {
    if (!m_paused) return false;
    PlaySession* session = m_levels.session();
    if (m_pausedSession && session && session->state() == PlaySession::State::Paused) {
        try {
            session->start();
        } catch (const std::exception& e) {
            SW_LOG(LogGame, Error, "The level didn't resume: {}", e.what());
        }
    }
    m_paused = false;
    m_pausedSession = false;
    choose();
    publish();
    return true;
}

bool AppShell::command(std::string_view command, std::string_view argument, std::string* why) {
    // Sends a request; a refusal other than Busy shows on the error screen, with Retry.
    auto request = [&](TravelRequest r, auto send) {
        const TravelTicket ticket = send(r);
        travel(ticket);
        if (!ticket) {
            // Busy: a button pressed twice, or while another travel runs; it carries on.
            if (ticket.error == TravelError::Busy) return refuse(why, ticket.message);
            fail(std::move(r), ticket.error, ticket.message, m_levels.playing());
            return refuse(why, std::string(travelErrorName(ticket.error)) + ": " + ticket.message);
        }
        choose();
        publish();
        return true;
    };
    auto send = [this](TravelRequest r) { return m_levels.request(std::move(r)); };
    auto to = [](std::string level, TravelReason reason, bool reset = false) {
        TravelRequest r;
        r.level = std::move(level);
        r.reason = reason;
        r.resetCampaign = reset;
        return r;
    };
    auto page = [&](const char* name) {
        m_page = name;
        m_message.clear();
        publish();
        return true;
    };
    if (command == "new_game") {
        if (m_setup.newGameLevel.empty()) return refuse(why, "This game has no first level to start in");
        if (m_setup.hasProgress && m_setup.hasProgress()) return page("confirm");
        return request(to(m_setup.newGameLevel, TravelReason::NewGame), send);
    }
    if (command == "confirm_new_game") {
        if (m_setup.newGameLevel.empty()) return refuse(why, "This game has no first level to start in");
        return request(to(m_setup.newGameLevel, TravelReason::NewGame, true), send);
    }
    if (command == "continue") return refuse(why, "No saved game to continue yet");
    if (command == "level_select") {
        if (!value("can_level_select").flag()) return refuse(why, "This game has no level select");
        return page("levels");
    }
    if (command == "play_level") {
        for (const Level& l : m_setup.levels)
            if (l.id == argument && l.selectable && m_setup.screens.levelSelect)
                return request(to(l.id, TravelReason::NewGame), send);
        return refuse(why, "\"" + std::string(argument) + "\" isn't in Level select");
    }
    if (command == "settings") return page("settings");
    if (command == "credits") return page("credits");
    if (command == "back") {
        if (m_page != "main") return page("main");
        if (m_screen == ShellScreen::Pause) return resume();
        return true;
    }
    if (command == "quit") {
        if (!m_setup.quit) return refuse(why, "Quit isn't available here");
        m_setup.quit();
        return true;
    }
    if (command == "resume") return resume() || refuse(why, "Nothing is paused");
    if (command == "restart_checkpoint") return refuse(why, "No checkpoint reached yet");
    if (command == "restart_level") {
        TravelRequest r = to(m_levels.current().level, TravelReason::Restart);
        r.spawn = m_levels.current().spawn;
        return request(std::move(r), [this](const TravelRequest&) { return m_levels.restartLevel(); });
    }
    if (command == "main_menu") {
        return request(to(m_levels.menuLevel(), TravelReason::Menu), [this](const TravelRequest&) {
            return m_levels.returnToMenu();
        });
    }
    if (command == "cancel_travel") {
        const uint64_t active = m_levels.activeTicket();
        if (!active) return refuse(why, "Nothing is loading");
        const TravelError e = m_levels.cancel(active);
        if (e != TravelError::None)
            return refuse(why, std::string(travelErrorName(e)) + ": it's too late to cancel; the level is changing");
        choose();
        publish();
        return true;
    }
    if (command == "retry") {
        if (!m_failure) return refuse(why, "Nothing to retry");
        TravelRequest r = m_failure->request;
        r.fromEpoch = 0;   // the application asks: the level that asked may have gone
        if (r.level.empty()) return refuse(why, "Nothing to retry");
        m_failure.reset();
        return request(std::move(r), send);
    }
    if (command == "dismiss") {
        if (!m_failure || !m_failure->levelPlays || !m_levels.playing()) return refuse(why, "No level to go back to");
        m_failure.reset();
        choose();
        publish();
        return true;
    }
    return refuse(why, "Unknown command \"" + std::string(command) + "\"");
}

bool AppShell::click(std::string_view element) {
    if (m_screen == ShellScreen::None) return false;
    const Entity e = m_documents[int(m_screen)];
    if (!e.valid()) return false;
    // Lay it out as it shows now, so the element is where it would be clicked.
    choose();
    m_ui->update(m_world, 0.0);
    if (!m_ui->elementVisible(e, element)) return false;
    return m_ui->click(e, element);
}

}   // namespace shockwave
