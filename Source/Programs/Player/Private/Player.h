#pragma once

// The player: runs a project like the shipped game, or one of the command-line tools
// (--check, --schema, --input-schema, --devices, --screenshot, --benchmark) that people, CI and AI agents use to verify it.

#include <Assets/AssetFolder.h>
#include <Engine/Engine.h>
#include <GameFramework/GameModule.h>
#include <GameFramework/PlaySession.h>
#include <Projects/Project.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace player {

namespace fs = std::filesystem;

// What the player was asked to do. Tools run in a hidden window and quit before the
// first frame; they need the renderer only to import assets the way the game does.
struct Tool {
    enum class Mode { Play, Check, Schema } mode = Mode::Play;
    std::vector<fs::path> scenes;   // --check: empty means every scene in the project
    fs::path schemaOut;   // --schema: "-" is standard output
    // Playing, these make runs reproducible and checkable without a person watching.
    fs::path scene;   // --scene: instead of the project's startup scene
    fs::path screenshot;   // --screenshot: save frame N as a PNG, then quit
    int screenshotFrame = 30;   // --frames with --screenshot
    shockwave::DebugView view = shockwave::DebugView::Lit;   // --view
    int shadows = -1;   // --shadows: a ShadowQuality; -1 uses the project's shadow_quality
    bool printFrame = false;   // --print-frame: the render graph, at exit
    double benchmarkSeconds = 0;   // --benchmark: measure this long after a warm-up, report, quit
    fs::path benchmarkJson;   // --json with --benchmark
};

// Tools that need no project or window. Both return the process exit code.
int writeInputSchema(const fs::path& out);   // docs/InputSchema.md; "-" for stdout
// Lists connected controllers, then for `seconds` prints each input pressed as the
// binding text that would bind it ("pad south", "joy \"G29\" axis 2 full").
int listDevices(double seconds);

class Player final : public shockwave::Application {
public:
    Player(shockwave::Project project, Tool tool, const shockwave::GameModule& module)
        : m_project(std::move(project)), m_tool(std::move(tool)), m_module(module) {}
    int exitCode() const { return m_exitCode; }

    void onStart() override;
    void onUpdate(double dt) override;
    void onFrame(double dt) override;
    void onRender(shockwave::Renderer& renderer, double alpha) override;
    void onShutdown() override;

private:
    // Asks for frame N, then reports and quits once the renderer has written it.
    void takeScreenshot();
    // --benchmark: warms up, measures, prints the frame statistics and quits.
    void benchmark(double dt);
    void reportBenchmark();

    // Loads each scene the way the game would and reports every problem as file:line.
    int check(const std::vector<std::string>& importProblems);

    int schema();

    shockwave::Project m_project;
    Tool m_tool;
    const shockwave::GameModule& m_module;
    int m_exitCode = 0;
    int m_frame = 0;
    bool m_screenshotRequested = false;
    double m_benchmarkTime = 0;
    bool m_benchmarkMeasuring = false;
    shockwave::World m_world;
    shockwave::FlyCamera m_camera;
    std::unique_ptr<shockwave::AssetFolder> m_assets;
    std::unique_ptr<shockwave::GameInstance> m_instance;
    std::unique_ptr<shockwave::PlaySession> m_session;
};

}   // namespace player
