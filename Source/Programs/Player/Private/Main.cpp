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
#include <cmath>
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
        float audioVolume = 1;
        Tool tool;
        std::pair<int, int> size{0, 0};
        double fpsOverride = -1;   // --fps
        bool lowLatency = false;   // --low-latency
        int vsyncOverride = -1;   // --vsync on|off
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--project" && i + 1 < argc)
                manifest = argv[++i];
            else if (arg == "--frames" && i + 1 < argc) {
                const std::string value = argv[++i];
                const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), frames);
                if (ec != std::errc{} || end != value.data() + value.size() || frames <= 0)
                    throw std::runtime_error("--frames requires a positive integer");
            } else if (arg == "--audio-volume" && i + 1 < argc) {
                const std::string value = argv[++i];
                const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), audioVolume);
                if (ec != std::errc{} || end != value.data() + value.size() || !std::isfinite(audioVolume) ||
                    audioVolume < 0 || audioVolume > 1)
                    throw std::runtime_error("--audio-volume requires a number in 0..1");
            } else if (arg == "--check") {
                tool.mode = Tool::Mode::Check;
            } else if (arg == "--schema" && i + 1 < argc) {
                tool.mode = Tool::Mode::Schema;
                tool.schemaOut = argv[++i];
            } else if (arg == "--input-schema" && i + 1 < argc) {
                return player::writeInputSchema(argv[i + 1]);
            } else if (arg == "--devices") {
                double seconds = 0.0;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    const std::string value = argv[i + 1];
                    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), seconds);
                    if (ec != std::errc{} || end != value.data() + value.size() || seconds < 0.0)
                        throw std::runtime_error("--devices takes a number of seconds to listen for");
                }
                return player::listDevices(seconds);
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
                else if (view == "cascades")
                    tool.view = shockwave::DebugView::ShadowCascades;
                else
                    throw std::runtime_error("--view takes lit, unlit, normals, lighting or cascades");
            } else if (arg == "--shadows" && i + 1 < argc) {
                tool.shadows = shockwave::parseShadowQuality(argv[++i]);
                if (tool.shadows < 0) throw std::runtime_error("--shadows takes off, low, medium, high or ultra");
            } else if (arg == "--size" && i + 1 < argc) {
                const std::string value = argv[++i];
                const size_t x = value.find('x');
                int w = 0, h = 0;
                if (x == std::string::npos || std::from_chars(value.data(), value.data() + x, w).ec != std::errc{} ||
                    std::from_chars(value.data() + x + 1, value.data() + value.size(), h).ec != std::errc{} || w < 16 ||
                    h < 16)
                    throw std::runtime_error("--size takes WIDTHxHEIGHT, like 1280x720");
                size = {w, h};
            } else if (arg == "--fps" && i + 1 < argc) {
                const std::string value = argv[++i];
                double fps = -1;
                const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), fps);
                if (ec != std::errc{} || end != value.data() + value.size() || fps < 0 || (fps > 0 && fps < 10) ||
                    fps > 1000)
                    throw std::runtime_error("--fps takes 0 (no limit) or a frame rate from 10 to 1000");
                fpsOverride = fps;
            } else if (arg == "--low-latency") {
                lowLatency = true;
            } else if (arg == "--vsync" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value != "on" && value != "off") throw std::runtime_error("--vsync takes on or off");
                vsyncOverride = value == "on" ? 1 : 0;
            } else if (arg == "--benchmark") {
                tool.benchmarkSeconds = 10;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    const std::string value = argv[++i];
                    const auto [end, ec] =
                        std::from_chars(value.data(), value.data() + value.size(), tool.benchmarkSeconds);
                    if (ec != std::errc{} || end != value.data() + value.size() || tool.benchmarkSeconds <= 0)
                        throw std::runtime_error("--benchmark takes a number of seconds");
                }
            } else if (arg == "--json" && i + 1 < argc) {
                tool.benchmarkJson = argv[++i];
            } else if (arg == "--print-frame") {
                tool.printFrame = true;
            } else if (arg == "--help") {
                std::puts("player [project.swproject] [--project path] [--scene file.swscene] [--frames N]\n"
                          "       [--size WxH] [--view lit|unlit|normals|lighting|cascades] [--print-frame]\n"
                          "       [--shadows off|low|medium|high|ultra] [--fps N] [--low-latency] [--vsync on|off]\n"
                          "player --screenshot out.png [--scene file] [--frames N] [--size WxH] [--view ...]\n"
                          "                                    play N frames (default 30), save the last, quit\n"
                          "player --benchmark [seconds] [--json file]\n"
                          "                                    run the scene for that long (default 10) and report\n"
                          "                                    frame times, 1%/0.1% lows and GPU time per pass\n"
                          "player --check [scene.swscene...]   validate the project and scenes; exit 1 on any problem\n"
                          "player --schema file.md             write the scene format reference (- for stdout)\n"
                          "player --input-schema file.md       write the input bindings reference (- for stdout)\n"
                          "player --audio-volume 0..1          master playback volume (default 1)\n"
                          "player --devices [seconds]          list controllers; then print each input pressed\n"
                          "                                    as binding text, for that many seconds");
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
        config.frameLimit = fpsOverride >= 0 ? fpsOverride : project.frameLimit;
        config.lowLatency = lowLatency || project.lowLatency;
        if (vsyncOverride >= 0) config.window.vsync = vsyncOverride == 1;
        config.maxFrames = frames;
        if (size.first) config.window.width = size.first, config.window.height = size.second;
        if (!tool.screenshot.empty()) {
            // Runs hidden until the frame is saved; maxFrames is only a safety net.
            if (frames) tool.screenshotFrame = frames;
            config.maxFrames = tool.screenshotFrame + 120;
            config.window.hidden = true;
            config.window.vsync = false;
        }
        if (tool.benchmarkSeconds > 0) {
            // Measured in a hidden window, uncapped unless asked: --fps, --vsync on or the
            // project's frame_limit say how it should be paced.
            config.window.hidden = true;
            if (vsyncOverride < 0) config.window.vsync = false;
        }
        if (tool.mode != Tool::Mode::Play) {
            config.window.hidden = true;
            config.window.vsync = false;
        }
        shockwave::Engine engine(config);
        engine.setMasterVolume(audioVolume);
        Player player(std::move(project), std::move(tool));
        engine.run(player);
        if (player.exitCode() != 0) return player.exitCode();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Player: %s\n", e.what());
        return 1;
    }
    return 0;
}
