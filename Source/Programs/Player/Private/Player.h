#pragma once

// The player: runs a project like the shipped game, or one of the command-line tools
// (--check, --schema, --screenshot) that people, CI and AI agents use to verify it.

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
    bool printFrame = false;   // --print-frame: the render graph, at exit
};

class Player final : public shockwave::Game {
public:
    Player(shockwave::Project project, Tool tool) : m_project(std::move(project)), m_tool(std::move(tool)) {}
    int exitCode() const { return m_exitCode; }

    void onStart() override;
    void onUpdate(double dt) override;
    void onFrame(double dt) override;
    void onRender(shockwave::Renderer& renderer, double alpha) override;
    void onShutdown() override;

private:
    // Asks for frame N, then reports and quits once the renderer has written it.
    void takeScreenshot();

    // Loads each scene the way the game would and reports every problem as file:line.
    int check(const std::vector<std::string>& importProblems);

    int schema();

    shockwave::Project m_project;
    Tool m_tool;
    int m_exitCode = 0;
    int m_frame = 0;
    bool m_screenshotRequested = false;
    shockwave::World m_world;
    shockwave::FlyCamera m_camera;
    std::unique_ptr<shockwave::AssetFolder> m_assets;
    std::unique_ptr<shockwave::GameInstance> m_instance;
    std::unique_ptr<shockwave::PlaySession> m_session;
};

}   // namespace player
