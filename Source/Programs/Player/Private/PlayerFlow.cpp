// The application shell (docs/AppShell.md) and --flow-test (docs/FlowTest.md).
#include "Player.h"

#include <GameFramework/SaveGame.h>
#include <GameUI/UIWorld.h>
#include <Input/Input.h>
#include <Packaging/Package.h>
#include <Physics/Physics.h>
#include <Platform/SystemInfo.h>
#include <Renderer/GpuSelection.h>
#include <Renderer/Renderer.h>

#include <algorithm>
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
    // The Credits page: the project's Content/Shell/credits.txt, when it has one.
    if (std::ifstream credits(m_project.assetFolder / "Shell" / "credits.txt", std::ios::binary); credits) {
        std::ostringstream text;
        text << credits.rdbuf();
        setup.credits = text.str();
        while (!setup.credits.empty() && (setup.credits.back() == '\n' || setup.credits.back() == '\r'))
            setup.credits.pop_back();
    }
    setup.quit = [this] { engine().quit(); };
    // S06: New game asks first when the campaign holds progress; Continue loads the newest
    // save this game and profile can load.
    setup.hasProgress = [this] { return m_levels && !m_levels->campaign().empty(); };
    setup.continueReason = [this] { return continueReason(); };
    setup.continueGame = [this](shockwave::TravelRequest& request, std::string& why) {
        return continueRequest(request, why);
    };
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
    setup.recovery = recovery();
    m_shell = std::make_unique<shockwave::AppShell>(&engine(), *m_levels, std::move(setup));
}

std::string Player::continueReason() {
    const fs::path folder = saveFolder();
    if (folder.empty()) return "Saving is off in this run";
    std::string passed;
    if (shockwave::newestCompatibleSave(folder, saveIdentity(), &passed)) return {};
    return passed.empty() ? "No saved game to continue yet" : "No save to continue: " + passed;
}

// Read here, on the main thread, while the menu shows: a save is one file, and the loading
// screen covers the rest of the travel.
bool Player::continueRequest(shockwave::TravelRequest& request, std::string& why) {
    std::string passed;
    const auto newest = shockwave::newestCompatibleSave(saveFolder(), saveIdentity(), &passed);
    if (!newest) {
        why = passed.empty() ? "No saved game to continue yet" : "No save to continue: " + passed;
        return false;
    }
    auto restore = std::make_shared<shockwave::SaveRestore>();
    std::vector<std::string> warnings;
    if (!shockwave::readSaveRestore(newest->file, engine().assets(), saveIdentity(), *restore, why, &warnings))
        return false;
    for (const std::string& warning : warnings) SW_LOG(shockwave::LogGame, Warning, "{}", warning);
    // Saves from before S06 don't name their level: the one whose scene they came from.
    request.level = restore->info.level;
    if (request.level.empty() && !restore->info.scene.empty())
        request.level = levelForScene(m_project.root / restore->info.scene);
    request.spawn = restore->info.spawn;
    request.restore = std::move(restore);
    if (!passed.empty()) SW_LOG(shockwave::LogGame, Warning, "Continue: {}", passed);
    SW_LOG(shockwave::LogGame, Info, "Continue from {}", m_project.relative(newest->file));
    return true;
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
    size_t lapSteps = scenario.steps.size();
    if (m_tool.perfLaps > 1) {
        // Laps: the scenario's steps again and again, its final quit only at the end. The
        // scenario has to come back to where it started (docs/Performance.md).
        std::vector<shockwave::FlowStep> lap = scenario.steps;
        const bool quits = !lap.empty() && lap.back().kind == shockwave::FlowStep::Kind::Quit;
        if (quits) lap.pop_back();
        lapSteps = lap.size();
        scenario.steps.clear();
        for (uint32_t i = 0; i < m_tool.perfLaps; ++i)
            scenario.steps.insert(scenario.steps.end(), lap.begin(), lap.end());
        if (quits) scenario.steps.push_back(shockwave::FlowStep{.kind = shockwave::FlowStep::Kind::Quit});
        if (scenario.steps.size() > shockwave::kMaxFlowSteps)
            throw UsageError(
                "--laps " + std::to_string(m_tool.perfLaps) + " makes " + std::to_string(scenario.steps.size()) +
                " steps, more than a scenario may have (" + std::to_string(shockwave::kMaxFlowSteps) + ")");
        scenario.timeout = std::min(scenario.timeout * m_tool.perfLaps, shockwave::kMaxFlowTimeout);
    }
    if (m_tool.perfRun) startPerformance(lapSteps);
    else if (m_tool.perfLaps > 1) {
        m_lapSteps = std::max<size_t>(lapSteps, 1);
        m_lapStart = m_levels ? m_levels->currentLevel().level : std::string();
    }
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
    // S07: real input, through the engine's Input as the devices would give it.
    hooks.inject = [this](const shockwave::InputBinding& binding, float value) {
        return engine().input().inject(binding, value);
    };
    hooks.look = [this](float x, float y) { engine().input().injectMouseMotion({x, y}); };
    hooks.unplug = [this] { engine().input().removeVirtualGamepad(); };
    // S16: a full disk for saves, as section 25 step 11 asks: every write fails until it's off.
    hooks.diskFull = [](bool full) {
        if (full)
            shockwave::setSaveFaultForTesting([](std::string_view step) { return step == "write"; });
        else
            shockwave::setSaveFaultForTesting(nullptr);
    };
    m_flowTest = std::make_unique<shockwave::FlowTest>(std::move(scenario), std::move(hooks));
    SW_LOG(shockwave::LogGame, Info, "Flow test {}", m_project.relative(fs::absolute(m_tool.flowTest)));
}

void Player::driveFlowTest() {
    if (!m_flowTest) return;
    if (m_perf) samplePerformance();
    const shockwave::FlowTest::State state = m_flowTest->frame(m_frameDt);
    if (m_lapSteps) sampleLaps();
    if (state == shockwave::FlowTest::State::Running) return;
    endFlowTest();
    engine().quit();
}

// --laps: each lap's watermarks and the verdict, for the flow report's "run".
std::string Player::lapsJson() const {
    if (!m_lapSteps) return {};
    std::string out = ", \"laps\": [";
    for (size_t i = 0; i < m_lapMarks.size(); ++i) {
        const LapMark& m = m_lapMarks[i];
        out += std::string(i ? ", " : "") + "{\"resident_bytes\": " + std::to_string(m.residentBytes) +
               ", \"entities\": " + std::to_string(m.entities) + ", \"bodies\": " + std::to_string(m.bodies) +
               ", \"assets\": " + std::to_string(m.assets) + ", \"leases\": " + std::to_string(m.leases) +
               ", \"threads\": " + std::to_string(m.threads) + "}";
    }
    const std::string verdict = lapVerdict();
    out += "], \"plateau\": " + std::string(verdict.empty() && m_lapMarks.size() >= 3 ? "true" : "false");
    std::string escaped;
    for (const char c : verdict) {
        if (c == '"' || c == '\\') escaped += '\\';
        escaped += c;
    }
    return out + ", \"plateau_problem\": \"" + escaped + "\"";
}

// What ran, for the report: a verifier (player --verify-install) checks it's the package it
// installed, and records the renderer and GPU the game really used.
std::string Player::runJson() {
    auto q = [](std::string_view text) {
        std::string out = "\"";
        for (const char c : text) {
            if (c == '"' || c == '\\') out += '\\';
            if (static_cast<unsigned char>(c) >= 0x20) out += c;
        }
        return out + "\"";
    };
    shockwave::Renderer& renderer = engine().renderer();
    std::string kind = "none", driver;
    if (const shockwave::GpuChoice* choice = renderer.gpuChoice())
        if (const shockwave::GpuCandidate* gpu = choice->chosenGpu()) {
            kind = gpuRecordKind(gpu->kind);
            driver = gpu->driver;
        }
    const fs::path packaged = packagedProjectFile();
    return std::string("{\"packaged\": ") + (packaged.empty() ? "false" : "true") +
           ", \"package_project\": " + q(packaged.empty() ? std::string() : packaged.filename().string()) +
           ", \"build_id\": " + q(currentBuildId(m_project)) +
           ", \"build_type\": " + q(shockwave::engineBuildConfig()) + ", \"renderer\": " + q(renderer.backendName()) +
           ", \"gpu\": " + q(renderer.gpuName() ? renderer.gpuName() : "") + ", \"gpu_kind\": " + q(kind) +
           ", \"gpu_driver\": " + q(driver) +
           ", \"menu_level\": " + q(m_levels ? m_levels->menuLevel() : std::string()) +
           ", \"profile_sha256\": " + q(m_profileAtStart) + lapsJson() + "}";
}

// As each lap ends (back where it began): what the process holds. Retirement is done by
// then: the old level's session, bodies and lease went before the new level started.
void Player::sampleLaps() {
    const size_t done = std::min<size_t>(m_flowTest->step() / m_lapSteps, m_tool.perfLaps);
    while (m_lapMarks.size() < done) {
        LapMark m;
        const shockwave::ProcessMemory memory = shockwave::processMemory();
        m.residentBytes = memory.residentBytes;
        m.threads = shockwave::processThreads();
        m.entities = m_world.entityCount();
        if (session())
            if (const shockwave::PhysicsWorld* physics = session()->physics()) m.bodies = physics->bodyCount();
        m.assets = m_residency.size();
        m.leases = m_residency.leases();
        m.level = m_levels ? m_levels->currentLevel().level : std::string();
        m_lapMarks.push_back(std::move(m));
    }
}

// Section 25 step 10: after the first lap (caches, first uploads, pools growing to size),
// nothing accumulates. Entities, physics bodies, resident assets and leases come back to
// the same counts at the end of every lap; threads never exceed the first lap's; resident
// memory from a tenth of the way in (at least lap 2) to the end grows by no more than the
// tolerance (--lap-tolerance, default 32 MiB).
std::string Player::lapVerdict() const {
    for (size_t i = 0; i < m_lapMarks.size(); ++i)
        if (m_lapMarks[i].level != m_lapStart)
            return "lap " + std::to_string(i + 1) + " ended in " + m_lapMarks[i].level + ", not " + m_lapStart +
                   " where it began: laps have to come back to where they started";
    if (m_lapMarks.size() < 3) return {};
    const LapMark& first = m_lapMarks.front();
    for (size_t i = 1; i < m_lapMarks.size(); ++i) {
        const LapMark& m = m_lapMarks[i];
        auto grew = [&](const char* what, uint64_t then, uint64_t now) {
            return "lap " + std::to_string(i + 1) + " ended with " + std::to_string(now) + " " + what + ", lap 1 with " +
                   std::to_string(then);
        };
        if (m.entities != first.entities) return grew("entities", first.entities, m.entities);
        if (m.bodies != first.bodies) return grew("physics bodies", first.bodies, m.bodies);
        if (m.assets != first.assets) return grew("resident assets", first.assets, m.assets);
        if (m.leases != first.leases) return grew("asset leases", first.leases, m.leases);
        if (first.threads && m.threads > first.threads) return grew("threads", first.threads, m.threads);
    }
    const double tolerance = m_tool.lapToleranceMib >= 0 ? m_tool.lapToleranceMib : 32.0;
    const size_t from = std::max<size_t>(1, m_lapMarks.size() / 10);
    const double grown = (double(m_lapMarks.back().residentBytes) - double(m_lapMarks[from].residentBytes)) / 1048576.0;
    if (m_lapMarks.back().residentBytes && grown > tolerance) {
        char text[200];
        std::snprintf(
            text, sizeof text, "resident memory grew %.1f MiB from lap %zu to lap %zu, more than the %.1f MiB tolerance",
            grown, from + 1, m_lapMarks.size(), tolerance);
        return text;
    }
    return {};
}

void Player::endFlowTest() {
    if (!m_flowTest) return;
    shockwave::setSaveFaultForTesting(nullptr);   // a disk_full step never outlives its test
    bool passed = m_flowTest->state() == shockwave::FlowTest::State::Passed;
    std::string message = m_flowTest->message();
    if (m_lapSteps && passed && m_lapMarks.size() < m_tool.perfLaps) {
        passed = false;
        message = "only " + std::to_string(m_lapMarks.size()) + " of " + std::to_string(m_tool.perfLaps) +
                  " laps finished";
    }
    if (const std::string verdict = m_lapSteps ? lapVerdict() : std::string(); passed && !verdict.empty()) {
        passed = false;
        message = verdict;
    }
    if (m_lapSteps && !m_lapMarks.empty()) {
        const LapMark& a = m_lapMarks.front();
        const LapMark& z = m_lapMarks.back();
        std::printf(
            "laps: %zu; lap 1 ended with %.1f MiB, %zu entities, %u bodies, %zu assets, %u threads; lap %zu with %.1f "
            "MiB, %zu entities, %u bodies, %zu assets, %u threads\n",
            m_lapMarks.size(), double(a.residentBytes) / 1048576.0, a.entities, a.bodies, a.assets, a.threads,
            m_lapMarks.size(), double(z.residentBytes) / 1048576.0, z.entities, z.bodies, z.assets, z.threads);
    }
    if (m_flowTest->state() == shockwave::FlowTest::State::Running) message = "the player stopped before it finished";
    const std::string report = m_flowTest->reportJson(runJson());
    if (m_perf) {
        // The perf run's own report goes to --json instead; the flow's failure still fails it.
        endPerformance(passed);
    } else if (!m_tool.benchmarkJson.empty()) {
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
