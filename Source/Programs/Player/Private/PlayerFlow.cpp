// The application shell (docs/AppShell.md) and --flow-test (docs/FlowTest.md).
#include "Player.h"

#include <GameUI/UIWorld.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace player {

void Player::startShell() {
    if (!m_levels) return;
    shockwave::AppShellScreens screens;
    std::string error;
    const std::string& text = m_tool.appShell.empty() ? m_project.appShell : m_tool.appShell;
    if (!shockwave::parseAppShellScreens(text, screens, error)) throw UsageError(error);
    if (!screens.any()) return;   // app_shell off
    shockwave::AppShell::Setup setup;
    setup.title = m_project.name;
    setup.screens = screens;
    setup.content = m_project.assetFolder;
    setup.quit = [this] { engine().quit(); };
    // New game goes where the menu level says next, else to the first gameplay level.
    for (const shockwave::LevelEntry& level : m_catalog.levels) {
        shockwave::AppShell::Level l;
        l.id = level.id;
        l.menu = level.kind == shockwave::LevelKind::Menu;
        l.selectable = level.kind == shockwave::LevelKind::Gameplay;
        setup.levels.push_back(l);
        if (l.menu && setup.newGameLevel.empty() && !level.next.empty()) setup.newGameLevel = level.next;
    }
    if (setup.newGameLevel.empty())
        for (const shockwave::LevelEntry& level : m_catalog.levels)
            if (level.kind == shockwave::LevelKind::Gameplay) {
                setup.newGameLevel = level.id;
                break;
            }
    m_shell = std::make_unique<shockwave::AppShell>(&engine(), *m_levels, std::move(setup));
}

void Player::startFlowTest() {
    std::ifstream in(m_tool.flowTest, std::ios::binary);
    if (!in) throw UsageError("--flow-test: can't read " + m_tool.flowTest.string());
    std::ostringstream text;
    text << in.rdbuf();
    shockwave::FlowScenario scenario;
    std::string error;
    if (!shockwave::parseFlowScenario(text.str(), scenario, error))
        throw UsageError("--flow-test " + m_tool.flowTest.string() + ": " + error);
    if (scenario.name.empty()) scenario.name = m_tool.flowTest.stem().string();
    shockwave::FlowTest::Hooks hooks;
    hooks.flow = m_levels.get();
    hooks.shell = m_shell.get();
    hooks.playing = [this] { return m_levels && m_levels->playing(); };
    hooks.clickLevel = [this](std::string_view document, std::string_view element) {
        shockwave::UIWorld* ui = session() ? session()->ui() : nullptr;
        return ui && ui->visible(document) && ui->click(document, element);
    };
    hooks.levelValue = [this](std::string_view name) -> std::optional<std::string> {
        const shockwave::UIValue& v = m_ui->value(name);
        if (v.kind() == shockwave::UIValue::Kind::Unset) return std::nullopt;
        return v.toText();
    };
    auto saves = [this](std::string& error) -> shockwave::SaveGames* {
        shockwave::SaveGames* s = session() ? session()->saves() : nullptr;
        if (!s)
            error = "nothing is playing";
        else if (m_levels->transitioning())
            error = "a level change is under way";
        return error.empty() ? s : nullptr;
    };
    hooks.save = [saves](std::string_view slot, std::string& error) {
        shockwave::SaveGames* s = saves(error);
        if (!s) return false;
        const bool ok = s->save(slot);
        s->wait();
        error = s->lastError();
        return ok && error.empty();
    };
    hooks.load = [saves](std::string_view slot, std::string& error) {
        shockwave::SaveGames* s = saves(error);
        if (!s) return false;
        const bool ok = s->load(slot);
        s->wait();
        error = s->lastError();
        return ok && error.empty();
    };
    m_flowTest = std::make_unique<shockwave::FlowTest>(std::move(scenario), std::move(hooks));
    SW_LOG(shockwave::LogGame, Info, "Flow test {}", m_project.relative(fs::absolute(m_tool.flowTest)));
}

void Player::driveFlowTest() {
    if (!m_flowTest) return;
    if (m_flowTest->frame() == shockwave::FlowTest::State::Running) return;
    endFlowTest();
    engine().quit();
}

void Player::endFlowTest() {
    if (!m_flowTest) return;
    const bool passed = m_flowTest->state() == shockwave::FlowTest::State::Passed;
    std::string message = m_flowTest->message();
    if (m_flowTest->state() == shockwave::FlowTest::State::Running) message = "the player stopped before it finished";
    const std::string report = m_flowTest->reportJson();
    if (!m_tool.benchmarkJson.empty()) {
        if (m_tool.benchmarkJson == "-") {
            std::fputs(report.c_str(), stdout);
        } else {
            std::ofstream out(m_tool.benchmarkJson, std::ios::binary);
            out << report;
            out.close();
            if (out)
                std::printf("Wrote %s\n", m_tool.benchmarkJson.string().c_str());
            else
                std::fprintf(stderr, "Player: can't write %s\n", m_tool.benchmarkJson.string().c_str());
        }
    }
    if (passed) {
        std::printf("flow test passed: %zu steps\n", m_flowTest->step());
    } else {
        std::fprintf(stderr, "flow test failed: %s\n", message.c_str());
        m_exitCode = 1;
    }
    m_flowTest.reset();
}

}   // namespace player
