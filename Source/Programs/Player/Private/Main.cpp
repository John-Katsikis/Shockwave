// player: the standalone game executable and its command-line tools; see Player.h.
#include "Player.h"

#include <Assets/AssetFolder.h>
#include <Assets/BuiltinAssets.h>
#include <Engine/Engine.h>
#include <Engine/SceneFile.h>
#include <Engine/Systems.h>
#include <GameFramework/GameModule.h>
#include <GameFramework/PlaySession.h>
#include <Physics/Physics.h>
#include <Projects/Project.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using player::Player;
using player::Tool;

int main(int argc, char** argv) {
    try {
        std::filesystem::path manifest;
        int frames = 0;
        Tool tool;
        std::pair<int, int> size{0, 0};
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--project" && i + 1 < argc)
                manifest = argv[++i];
            else if (arg == "--frames" && i + 1 < argc) {
                const std::string value = argv[++i];
                const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), frames);
                if (ec != std::errc{} || end != value.data() + value.size() || frames <= 0)
                    throw std::runtime_error("--frames requires a positive integer");
            } else if (arg == "--check") {
                tool.mode = Tool::Mode::Check;
            } else if (arg == "--schema" && i + 1 < argc) {
                tool.mode = Tool::Mode::Schema;
                tool.schemaOut = argv[++i];
            } else if (arg == "--scene" && i + 1 < argc) {
                tool.scene = std::filesystem::absolute(argv[++i]).lexically_normal();
            } else if (arg == "--screenshot" && i + 1 < argc) {
                tool.screenshot = argv[++i];
            } else if (arg == "--view" && i + 1 < argc) {
                const std::string view = argv[++i];
                if (view == "lit")
                    tool.view = shockwave::DebugView::Lit;
                else if (view == "unlit")
                    tool.view = shockwave::DebugView::Unlit;
                else if (view == "normals")
                    tool.view = shockwave::DebugView::Normals;
                else if (view == "lighting")
                    tool.view = shockwave::DebugView::Lighting;
                else
                    throw std::runtime_error("--view takes lit, unlit, normals or lighting");
            } else if (arg == "--size" && i + 1 < argc) {
                const std::string value = argv[++i];
                const size_t x = value.find('x');
                int w = 0, h = 0;
                if (x == std::string::npos || std::from_chars(value.data(), value.data() + x, w).ec != std::errc{} ||
                    std::from_chars(value.data() + x + 1, value.data() + value.size(), h).ec != std::errc{} || w < 16 ||
                    h < 16)
                    throw std::runtime_error("--size takes WIDTHxHEIGHT, like 1280x720");
                size = {w, h};
            } else if (arg == "--print-frame") {
                tool.printFrame = true;
            } else if (arg == "--help") {
                std::puts("player [project.swproject] [--project path] [--scene file.swscene] [--frames N]\n"
                          "       [--size WxH] [--view lit|unlit|normals|lighting] [--print-frame]\n"
                          "player --screenshot out.png [--scene file] [--frames N] [--size WxH] [--view ...]\n"
                          "                                    play N frames (default 30), save the last, quit\n"
                          "player --check [scene.swscene...]   validate the project and scenes; exit 1 on any problem\n"
                          "player --schema file.md             write the scene format reference (- for stdout)");
                return 0;
            } else if (!arg.empty() && arg[0] != '-') {
                if (tool.mode == Tool::Mode::Check && std::filesystem::path(arg).extension() == ".swscene")
                    tool.scenes.push_back(std::filesystem::absolute(arg).lexically_normal());
                else
                    manifest = arg;
            } else
                throw std::runtime_error("Unknown or incomplete option: " + arg);
        }
        if (manifest.empty()) {
            // The project around the scene given, else the working folder's.
            const std::filesystem::path near = !tool.scenes.empty()  ? tool.scenes.front()
                                               : !tool.scene.empty() ? tool.scene
                                                                     : std::filesystem::current_path();
            const auto found = shockwave::findProjectFile(near);
            // Nothing around: the project whose game module this player was built with.
            manifest = found ? *found : std::filesystem::path(SHOCKWAVE_GAME_PROJECT);
        }
        shockwave::Project project;
        std::string error;
        if (!shockwave::loadProject(manifest, project, error)) throw std::runtime_error(error);
        if (project.gameModule != shockwave::projectGameModule().id)
            throw std::runtime_error("Project game_module does not match the compiled gameplay library");
        shockwave::EngineConfig config;
        config.window = {project.name, project.width, project.height, project.vsync};
        config.tickRate = project.tickRate;
        config.maxFrames = frames;
        if (size.first) config.window.width = size.first, config.window.height = size.second;
        if (!tool.screenshot.empty()) {
            // Runs hidden until the frame is saved; maxFrames is only a safety net.
            if (frames) tool.screenshotFrame = frames;
            config.maxFrames = tool.screenshotFrame + 120;
            config.window.hidden = true;
            config.window.vsync = false;
        }
        if (tool.mode != Tool::Mode::Play) {
            config.window.hidden = true;
            config.window.vsync = false;
        }
        shockwave::Engine engine(config);
        Player player(std::move(project), std::move(tool));
        engine.run(player);
        if (player.exitCode() != 0) return player.exitCode();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Player: %s\n", e.what());
        return 1;
    }
    return 0;
}
