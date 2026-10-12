// player: the standalone game executable and its command-line tools; see Player.h.
#include "Player.h"

#include <Assets/AssetFolder.h>
#include <Assets/BuiltinAssets.h>
#include <Engine/Engine.h>
#include <Engine/SceneFile.h>
#include <Engine/Systems.h>
#include <GameFramework/GameModule.h>
#include <GameFramework/GameModuleLibrary.h>
#include <GameFramework/PlaySession.h>
#include <GameFramework/SaveGame.h>
#include <Physics/Physics.h>
#include <Projects/Project.h>
#include <Renderer/GpuSelection.h>

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
        player::PackageCommand package;   // --package
        std::pair<int, int> size{0, 0};
        double fpsOverride = -1;   // --fps
        bool lowLatency = false;   // --low-latency
        int vsyncOverride = -1;   // --vsync on|off
        std::string renderer;   // --renderer
        std::filesystem::path gameModuleFile;   // --game-module
        bool listGpus = false;   // --gpus
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
            } else if (arg == "--renderer" && i + 1 < argc) {
                renderer = argv[++i];
            } else if (arg == "--renderers") {
                for (const std::string& name : shockwave::Renderer::availableBackends()) std::puts(name.c_str());
                return 0;
            } else if (arg == "--gpu" && i + 1 < argc) {
                shockwave::setRequestedGpu(argv[++i]);
            } else if (arg == "--gpus") {
                listGpus = true;
            } else if (arg == "--game-module" && i + 1 < argc) {
                gameModuleFile = argv[++i];
            } else if (arg == "--package") {
                package.requested = true;
            } else if (arg == "--output" && i + 1 < argc) {
                package.output = argv[++i];
            } else if (arg == "--no-build") {
                package.noBuild = true;
            } else if (arg == "--no-archive") {
                package.noArchive = true;
            } else if (arg == "--check") {
                tool.mode = Tool::Mode::Check;
            } else if (arg == "--bake-navmesh") {
                tool.mode = Tool::Mode::BakeNavMesh;
            } else if (arg == "--schema" && i + 1 < argc) {
                tool.mode = Tool::Mode::Schema;
                tool.schemaOut = argv[++i];
            } else if (arg == "--animations") {
                tool.mode = Tool::Mode::Animations;
                while (i + 1 < argc && argv[i + 1][0] != '-') tool.animationModels.push_back(argv[++i]);
            } else if (arg == "--node-schema" && i + 1 < argc) {
                tool.mode = Tool::Mode::NodeSchema;
                tool.schemaOut = argv[++i];
            } else if (arg == "--circuit" && i + 1 < argc) {
                tool.circuit = argv[++i];
                if (tool.mode == Tool::Mode::Play) tool.mode = Tool::Mode::CircuitDump;
            } else if (arg == "--run-circuit") {
                tool.mode = Tool::Mode::CircuitRun;
            } else if ((arg == "--circuit-ticks" || arg == "--circuit-steps") && i + 1 < argc) {
                int count = 0;
                const std::string value = argv[++i];
                const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), count);
                if (ec != std::errc{} || end != value.data() + value.size() || count < 0 || count > 10000)
                    throw std::runtime_error(arg + " takes an integer from 0 to 10000");
                if (arg == "--circuit-ticks")
                    tool.circuitTicks = count;
                else
                    tool.circuitSteps = count;
            } else if (arg == "--break-node" && i + 1 < argc) {
                tool.circuitBreakpoint = argv[++i];
            } else if (arg == "--dump") {
                tool.mode = Tool::Mode::CircuitDump;
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
                else if (view == "navmesh")
                    tool.showNavMesh = true;
                else
                    throw std::runtime_error("--view takes lit, unlit, normals, lighting, cascades or navmesh");
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
            } else if (arg == "--optimise" || arg == "--optimize") {
                tool.optimise = true;
            } else if (arg == "--lod-colors") {
                tool.lodColors = true;
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
            } else if (arg == "--ui-preview") {
                tool.uiPreview = true;
            } else if (arg == "--ui-nav" && i + 1 < argc) {
                // "down,down,confirm": what the arrow keys or d-pad, Enter and Back would do.
                std::string steps = argv[++i];
                for (size_t at = 0; at <= steps.size();) {
                    const size_t comma = std::min(steps.find(',', at), steps.size());
                    const std::string step = steps.substr(at, comma - at);
                    at = comma + 1;
                    if (step == "up")
                        tool.uiNav.push_back(shockwave::UINav::Up);
                    else if (step == "down")
                        tool.uiNav.push_back(shockwave::UINav::Down);
                    else if (step == "left")
                        tool.uiNav.push_back(shockwave::UINav::Left);
                    else if (step == "right")
                        tool.uiNav.push_back(shockwave::UINav::Right);
                    else if (step == "confirm")
                        tool.uiNav.push_back(shockwave::UINav::Confirm);
                    else if (step == "back")
                        tool.uiNav.push_back(shockwave::UINav::Back);
                    else
                        throw std::runtime_error("--ui-nav takes steps like up,down,left,right,confirm,back");
                }
            } else if (arg == "--audio-mix" && i + 1 < argc) {
                tool.mode = Tool::Mode::AudioMix;
                tool.audioMix = argv[++i];
            } else if (arg == "--seconds" && i + 1 < argc) {
                const std::string value = argv[++i];
                const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), tool.audioSeconds);
                if (ec != std::errc{} || end != value.data() + value.size() || !(tool.audioSeconds > 0) ||
                    tool.audioSeconds > 600)
                    throw std::runtime_error("--seconds takes a number of seconds up to 600");
            } else if (arg == "--audio-hrtf" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value != "on" && value != "off") throw std::runtime_error("--audio-hrtf takes on or off");
                tool.audioHrtf = value == "on" ? 1 : 0;
            } else if (arg == "--print-frame") {
                tool.printFrame = true;
            } else if (arg == "--saves") {
                tool.listSaves = true;
            } else if (arg == "--load" && i + 1 < argc) {
                tool.load = argv[++i];
            } else if (arg == "--save" && i + 1 < argc) {
                tool.save = argv[++i];
                if (!shockwave::validSaveSlot(tool.save))
                    throw std::runtime_error("--save takes a slot name: letters, digits, spaces, - and _");
            } else if (arg == "--help") {
                std::puts(
                    "player --package [--output folder] [--no-build] [--no-archive]\n"
                    "                                    make a game folder for players (docs/Packaging.md)");
                std::puts(
                    "player [project.swproject] [--project path] [--scene file.swscene] [--frames N]\n"
                    "       [--size WxH] [--view lit|unlit|normals|lighting|cascades|navmesh] [--print-frame]\n"
                    "       [--shadows off|low|medium|high|ultra] [--fps N] [--low-latency] [--vsync on|off]\n"
                    "       [--renderer vulkan|null] [--gpu name|number] [--game-module file.so]\n"
                    "player --gpus [--json file]          list the GPUs, which one renders and why, and any\n"
                    "                                    warnings (like rendering on a GPU the screen isn't on)\n"
                    "player --renderers                  list the renderer plugins that can be loaded\n"
                    "player --screenshot out.png [--scene file] [--frames N] [--size WxH] [--view ...]\n"
                    "                                    play N frames (default 30), save the last, quit\n"
                    "       [--ui-preview] [--ui-nav down,down,confirm]\n"
                    "                                    show UI preview values; step through menus first\n"
                    "player --benchmark [seconds] [--json file]\n"
                    "                                    run the scene for that long (default 10) and report\n"
                    "                                    frame times, 1%/0.1% lows and GPU time per pass\n"
                    "player --optimise [--json file]     compress every texture into the cache now and report\n"
                    "                                    texture memory and mesh levels of detail\n"
                    "player --lod-colors                 tint each level of detail its own colour\n"
                    "player --check [scene.swscene...]   validate the project and scenes; exit 1 on any problem\n"
                    "player --bake-navmesh [scene.swscene...]\n"
                    "                                    bake and save each scene's navigation mesh; print its\n"
                    "                                    tiles, polygons, bake time and crowd cost per tick\n"
                    "player --schema file.md             write the scene format reference (- for stdout)\n"
                    "player --node-schema file.md        write the circuit node reference (- for stdout)\n"
                    "player --circuit file --run-circuit [--circuit-ticks N] [--break-node name] [--circuit-steps N]\n"
                    "player --circuit file --dump        compile a .swcircuit and print its bytecode\n"
                    "player --saves [--json file]        list the project's save games (Saved/SaveGames)\n"
                    "player --save slot [--frames N]     play N frames (default 30), save to the slot, quit\n"
                    "player --load slot|file.swsave      start Play from a save game (add --screenshot to see it)\n"
                    "player --check file.swsave          check a save game; --check alone checks them all\n"
                    "player --input-schema file.md       write the input bindings reference (- for stdout)\n"
                    "player --animations [model...]      list models' skeletons and animation clips\n"
                    "player --audio-volume 0..1          master playback volume (default 1)\n"
                    "player --audio-hrtf on|off          headphone (HRTF) or speaker spatial audio\n"
                    "player --audio-mix out.wav [--seconds N] [--scene file]\n"
                    "                                    play the scene N seconds (default 3) with no audio\n"
                    "                                    device, write the mix and print per-channel levels\n"
                    "player --devices [seconds]          list controllers; then print each input pressed\n"
                    "                                    as binding text, for that many seconds");
                return 0;
            } else if (tool.mode == Tool::Mode::Check && std::filesystem::path(arg).extension() == ".swsave") {
                tool.scenes.push_back(std::filesystem::absolute(arg).lexically_normal());
            } else if (!arg.empty() && arg[0] != '-') {
                if ((tool.mode == Tool::Mode::Check || tool.mode == Tool::Mode::BakeNavMesh) &&
                    std::filesystem::path(arg).extension() == ".swscene")
                    tool.scenes.push_back(std::filesystem::absolute(arg).lexically_normal());
                else
                    manifest = arg;
            } else
                throw std::runtime_error("Unknown or incomplete option: " + arg);
        }
        // A packaged game runs the project it ships with, wherever it's started from.
        if (manifest.empty()) manifest = player::packagedProjectFile();
        if (manifest.empty()) {
            // The project around the scene given, else the working folder's.
            const std::filesystem::path near = !tool.scenes.empty()  ? tool.scenes.front()
                                               : !tool.scene.empty() ? tool.scene
                                                                     : std::filesystem::current_path();
            const auto found = shockwave::findProjectFile(near);
            // Nothing around: the project whose game module this player was built with.
            manifest = found ? *found : std::filesystem::path(SHOCKWAVE_GAME_PROJECT);
        } else if (std::error_code ec; std::filesystem::is_directory(manifest, ec)) {
            // A project folder: the .swproject in it.
            const auto found = shockwave::findProjectFile(manifest);
            if (!found) throw std::runtime_error("no .swproject in " + manifest.string());
            manifest = *found;
        }
        if (listGpus) {
            // Starts the Vulkan renderer in a hidden window, just to see which GPU it takes.
            shockwave::EngineConfig config;
            config.window = {"Shockwave GPUs", 320, 240, false};
            config.window.hidden = true;
            config.renderer = "vulkan";
            shockwave::Engine engine(config);
            const shockwave::GpuChoice* choice = engine.renderer().gpuChoice();
            if (!choice) throw std::runtime_error("the renderer reports no GPU");
            std::fputs(shockwave::describeGpuChoice(*choice).c_str(), stdout);
            if (!tool.benchmarkJson.empty()) {
                std::ofstream(tool.benchmarkJson) << shockwave::gpuChoiceJson(*choice);
                std::printf("Wrote %s\n", tool.benchmarkJson.string().c_str());
            }
            return choice->chosen >= 0 ? 0 : 1;
        }
        shockwave::Project project;
        std::string error;
        if (!shockwave::loadProject(manifest, project, error)) throw std::runtime_error(error);
        if (tool.listSaves) return player::listSaves(project, tool.benchmarkJson);   // only reads files
        // The project's game code, before any window: a missing module fails fast. It
        // outlives the engine and the player, which hold what it made.
        if (package.requested) return player::packageGame(project, package);
        auto gameModule = gameModuleFile.empty() ? shockwave::GameModuleLibrary::load(project.gameModule, error)
                                                 : shockwave::GameModuleLibrary::loadFile(gameModuleFile, error);
        if (!gameModule)
            throw std::runtime_error(
                "the project's game module \"" + project.gameModule + "\" couldn't load: " + error +
                ". Build it with: cmake --build build --target game_" + project.gameModule);
        if (gameModule->id() != project.gameModule)
            throw std::runtime_error(
                "the game module is \"" + std::string(gameModule->id()) + "\" but the project wants \"" +
                project.gameModule + "\"");
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
        if (!tool.save.empty()) {
            // Like a screenshot: hidden, and quits once the save is written.
            if (frames) tool.saveFrame = frames;
            config.maxFrames = std::max(config.maxFrames, tool.saveFrame + 120);
            config.window.hidden = true;
            config.window.vsync = false;
        }
        if (tool.benchmarkSeconds > 0) {
            // Measured in a hidden window, uncapped unless asked: --fps, --vsync on or the
            // project's frame_limit say how it should be paced.
            config.window.hidden = true;
            if (vsyncOverride < 0) config.window.vsync = false;
        }
        if (tool.mode != Tool::Mode::Play || tool.optimise) {
            config.window.hidden = true;
            config.window.vsync = false;
        }
        // Tools only read files, so they need no GPU unless asked for one.
        config.renderer = !renderer.empty()                                ? renderer
                          : tool.mode != Tool::Mode::Play || tool.optimise ? std::string("null")
                                                                           : project.renderer;
        shockwave::Engine engine(config);
        engine.setMasterVolume(audioVolume);
        Player player(std::move(project), std::move(tool), gameModule->module());
        engine.run(player);
        if (player.exitCode() != 0) return player.exitCode();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Player: %s\n", e.what());
        return 1;
    }
    return 0;
}
