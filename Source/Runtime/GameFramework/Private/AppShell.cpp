// The application shell's screens and commands: see GameFramework/AppShell.h.
#include <GameFramework/AppShell.h>

#include <Core/Log.h>
#include <Engine/Components.h>
#include <Engine/Engine.h>
#include <Engine/Localization.h>
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
    m_recoveryPending = m_setup.recovery.has_value();
    registerBuiltins();
    m_input = m_setup.input ? m_setup.input : engine ? &engine->input() : nullptr;
    if (m_input) {
        m_devicesVersion = m_input->devicesVersion();
        for (const InputDevice& d : m_input->devices()) m_pads += d.kind == InputDevice::Kind::Gamepad;
    }
    // The Settings page edits the engine's preferences and graphics unless told otherwise.
    m_preferences = m_setup.preferences ? m_setup.preferences : engine ? &engine->preferences() : nullptr;
    m_graphics = m_setup.graphics ? m_setup.graphics : engine ? &engine->graphics() : nullptr;
    if (m_preferences) m_draft = m_preferences->current();
    if (m_graphics) m_draftGraphics = m_graphics->requested();
    // The screens' words: the given catalog, else the engine's, else the built-in English.
    // The built-in text goes under whichever it is, so a missing key still reads.
    if (m_setup.localization) {
        m_text = m_setup.localization;
    } else if (engine) {
        engine->localization().addBuiltin("en", builtinText());
        m_text = &engine->localization();
    } else {
        m_ownText = std::make_unique<Localization>();
        m_ownText->addBuiltin("en", builtinText());
        m_text = m_ownText.get();
    }
    m_continueReason = tr("shell.no_continue");
    m_checkpointReason = tr("shell.no_checkpoint");
    // Its own context and data model; not the engine's game.ui().
    m_ui = std::make_unique<GameUI>(engine, GameUIOptions{"flow", false});
    m_ui->setLocalization(m_text);
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
    if (m_input) bindDefaultControls(*m_input);
    choose();
    publish();
}

AppShell::~AppShell() {
    if (m_input) m_input->lowerLayer(this);
    m_subscription.reset();
    if (m_ui) m_ui->setEventListener({});
}

bool AppShell::isMenuLevel(const std::string& level) const {
    for (const Level& l : m_setup.levels)
        if (l.id == level) return l.menu;
    return false;
}

void AppShell::update(double frameDt) {
    if (m_input) {
        Input& input = *m_input;
        watchControllers();
        if (m_listen) {
            listen(frameDt);   // the press is the binding's, not the pause action's
        } else if (input.wasActionPressed(kPauseAction)) {
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
    if (m_listen && m_page != "settings") stopListening();
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
        if (next == ShellScreen::Menu) refreshContinue();   // a save may have been made since
        if (next == ShellScreen::Pause) refreshCheckpoint();
        // A screen opens on its first page, without the last one's complaint; the menu after
        // an unclean exit on the recovery page.
        if (next == ShellScreen::Menu || next == ShellScreen::Pause) m_page = "main";
        if (next == ShellScreen::Menu && m_recoveryPending) m_page = "recovery";
        if (m_screen != ShellScreen::None || next != ShellScreen::None) m_message.clear();
        m_screen = next;
    }
    // The shell's screens have the input over the level (docs/Input.md): an error over the
    // loading screen, over a menu. Changing it is a barrier, so the press that closed a
    // screen doesn't reach the game.
    if (m_input) {
        if (m_screen == ShellScreen::None)
            m_input->lowerLayer(this);
        else
            m_input->raiseLayer(
                this, m_screen == ShellScreen::Error     ? InputLayer::Modal
                      : m_screen == ShellScreen::Loading ? InputLayer::Loading
                                                         : InputLayer::Menu);
    }
    for (int s = 1; s < 5; ++s)
        if (m_documents[s].valid())
            if (UIDocument* doc = m_world.get<UIDocument>(m_documents[s])) doc->visible = int(m_screen) == s;
}

void AppShell::refreshContinue() {
    if (!m_setup.continueReason) {
        m_continueReason = tr("shell.no_continue");
        return;
    }
    try {
        m_continueReason = m_setup.continueReason();
    } catch (const std::exception& e) {
        m_continueReason = e.what();
    }
}

void AppShell::refreshCheckpoint() {
    const SaveGames* saves = m_levels.session() ? m_levels.session()->saves() : nullptr;
    if (!saves) {
        m_checkpointReason = tr("shell.no_checkpoint");
        return;
    }
    std::string why;
    m_checkpointReason = saves->hasCheckpoint(&why) ? std::string() : why;
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
    ui.setValue(
        "error_detail", std::string_view(
                            m_failure ? tr("shell.error_detail", {{"code", travelErrorName(m_failure->code)},
                                                                  {"level", m_failure->request.level}})
                                      : std::string()));
    ui.setValue("can_dismiss", m_failure && m_failure->levelPlays && m_levels.playing());
    const bool newGame = !m_setup.newGameLevel.empty();
    ui.setValue("can_new_game", newGame);
    ui.setValue("new_game_reason", std::string_view(newGame ? std::string() : tr("shell.no_new_game")));
    ui.setValue("can_continue", m_continueReason.empty());
    ui.setValue("continue_reason", std::string_view(m_continueReason));
    ui.setValue("can_restart_checkpoint", m_checkpointReason.empty());
    ui.setValue("checkpoint_reason", std::string_view(m_checkpointReason));
    // The recovery page (S12): what's known about the run that didn't end cleanly.
    {
        const Setup::Recovery* r = m_setup.recovery ? &*m_setup.recovery : nullptr;
        std::string text;
        if (r) {
            text = tr(r->deviceLost ? "shell.recovery_device_lost" : "shell.recovery_text", {{"started", r->started}});
            if (!r->level.empty()) text += " " + tr("shell.recovery_level", {{"level", r->level}});
        }
        ui.setValue("recovery", r != nullptr && m_recoveryPending);
        ui.setValue("recovery_text", std::string_view(text));
        ui.setValue(
            "recovery_report",
            std::string_view(r && !r->report.empty() ? tr("shell.recovery_report", {{"report", r->report}}) : ""));
        ui.setValue("can_safe_graphics", r && r->safeGraphics && !m_safeGraphicsOn);
        ui.setValue("safe_graphics_on", m_safeGraphicsOn);
        ui.setValue("can_open_logs", r && bool(r->openLogs));
    }
    // The last checkpoint's save, for a HUD: "", "saving", "saved" or "failed".
    const SaveGames* saves = m_levels.session() ? m_levels.session()->saves() : nullptr;
    const auto state = saves ? saves->checkpointState() : SaveGames::CheckpointState::None;
    ui.setValue(
        "checkpoint_state", std::string_view(
                                state == SaveGames::CheckpointState::Saving   ? "saving"
                                : state == SaveGames::CheckpointState::Saved  ? "saved"
                                : state == SaveGames::CheckpointState::Failed ? "failed"
                                                                              : ""));
    publishSettings();
    ui.setValue("credits", std::string_view(m_setup.credits.empty() ? tr("shell.credits_default") : m_setup.credits));
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
    // Buttons made by data-for carry their argument in their id: "level-<id>",
    // "tab-<tab>", "next-<key>", "prev-<key>", "bindkey-<action>" and "bindpad-<action>".
    auto after = [&](std::string_view prefix) {
        if (event.element.rfind(prefix, 0) == 0) argument = std::string(event.element.substr(prefix.size()));
    };
    if (event.command == "play_level") after("level-");
    if (event.command == "settings_tab") after("tab-");
    if (event.command == "pref_next") after("next-");
    if (event.command == "pref_prev") after("prev-");
    if (event.command == "bind_key") after("bindkey-");
    if (event.command == "bind_pad") after("bindpad-");
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
        if (m_setup.newGameLevel.empty()) return refuse(why, tr("shell.no_new_game"));
        if (m_setup.hasProgress && m_setup.hasProgress()) return page("confirm");
        return request(to(m_setup.newGameLevel, TravelReason::NewGame), send);
    }
    if (command == "confirm_new_game") {
        if (m_setup.newGameLevel.empty()) return refuse(why, tr("shell.no_new_game"));
        return request(to(m_setup.newGameLevel, TravelReason::NewGame, true), send);
    }
    if (command == "continue") {
        refreshContinue();
        if (!m_continueReason.empty() || !m_setup.continueGame) return refuse(why, m_continueReason);
        TravelRequest r;
        std::string reason;
        if (!m_setup.continueGame(r, reason)) {
            m_continueReason = reason.empty() ? tr("shell.no_continue") : reason;
            return refuse(why, m_continueReason);
        }
        r.reason = TravelReason::LoadSave;
        return request(std::move(r), send);
    }
    if (command == "safe_graphics" || command == "open_logs" || command == "restore_save" ||
        command == "dismiss_recovery") {
        if (!m_setup.recovery) return refuse(why, "The last session ended cleanly");
        Setup::Recovery& r = *m_setup.recovery;
        std::string reason;
        if (command == "safe_graphics") {
            if (!r.safeGraphics) return refuse(why, "Safe graphics aren't available here");
            if (!r.safeGraphics(reason)) return refuse(why, reason);
            m_safeGraphicsOn = true;
            m_message = tr("shell.safe_graphics_on");
            publish();
            return true;
        }
        if (command == "open_logs") {
            if (!r.openLogs) return refuse(why, "There's no log folder to show");
            if (!r.openLogs(reason)) return refuse(why, reason);
            return true;
        }
        if (command == "restore_save") {
            if (!this->command("continue", {}, why)) return false;   // Continue's own refusal
            m_recoveryPending = false;
            return true;
        }
        m_recoveryPending = false;
        return page("main");
    }
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
    bool handled = false;
    const bool settings = settingsCommand(command, argument, why, handled);
    if (handled) return settings;
    if (command == "credits") return page("credits");
    if (command == "back") {
        // Back from Settings drops the draft, as Cancel does; a display waiting to be kept
        // goes back to the one that worked.
        if (m_page == "settings" && m_preferences && m_preferences->displayPending()) m_preferences->revertDisplay();
        if (m_page == "settings") openSettings();
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
    if (command == "restart_checkpoint") {
        refreshCheckpoint();
        SaveGames* saves = m_levels.session() ? m_levels.session()->saves() : nullptr;
        if (!m_checkpointReason.empty() || !saves) return refuse(why, m_checkpointReason);
        if (!saves->restartCheckpoint()) return refuse(why, saves->lastError());
        // It goes in at the next frame (or travels, from another level): back to the game.
        resume();
        return true;
    }
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
