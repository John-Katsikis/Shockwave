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

#include <SDL3/SDL.h>

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
        std::filesystem::path newProjectFolder;
        std::string newProjectName, newProjectModule;
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
        std::filesystem::path graphicsFile, graphicsSchema, materialSchema;
        bool graphicsInfo = false, noUserGraphics = false;
        float renderScaleOverride = -1;
        std::vector<std::pair<std::string, std::string>> graphicsSets;   // --graphics-set key=value, --anti-aliasing
        bool listGpus = false;   // --gpus
        bool networkInfo = false;   // --network-info
        std::filesystem::path networkSchema;   // --network-schema
        int steamStatus = 0;   // --steam-status: 1 client, 2 dedicated server
        player::NetTestCommand netTest;   // --net-test
        std::filesystem::path netDump, netEncode;   // --net-dump, --net-encode
        std::filesystem::path imageStats, imageReference, imageCandidate;   // --image-stats, --image-diff
        std::filesystem::path materialInfo, textureInfo;   // --material-info, --texture-info
        std::filesystem::path effectInfo;   // --effect-info
        std::filesystem::path postProcessSchema, postProcessInfo, lutNeutral;   // A05/A06 tools, no project
        uint32_t lutNeutralSize = 0;
        std::filesystem::path lightingSchema, lightingExport, lightingImport;   // need no project
        double imageTolerance = 1.0;   // --tolerance
        std::array<double, 4> imageRegion{0, 0, 1, 1};   // --region
        std::filesystem::path output;   // --output: --package, --net-encode and the network tools
        bool devDirectJoin = false;   // --dev-direct-join
        bool joinSteam = false, joinLobby = false;
        auto wholeNumber = [](const std::string& value, uint64_t max, const char* what) {
            uint64_t n = 0;
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
            if (ec != std::errc{} || end != value.data() + value.size() || n > max)
                throw player::UsageError(std::string(what) + " takes a whole number up to " + std::to_string(max));
            return n;
        };
        // Lobby and Steam IDs: decimal, nonzero, 64-bit.
        auto steamNumber = [](const std::string& value, const char* what) {
            uint64_t n = 0;
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
            if (ec != std::errc{} || end != value.data() + value.size() || n == 0 || value.size() > 20)
                throw player::UsageError(std::string(what) + " takes a decimal Steam ID (a nonzero 64-bit number)");
            return n;
        };
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--graphics" && i + 1 < argc)
                graphicsFile = argv[++i];
            else if (arg == "--graphics-schema" && i + 1 < argc)
                graphicsSchema = argv[++i];
            else if (arg == "--material-schema" && i + 1 < argc)
                materialSchema = argv[++i];
            else if (arg == "--graphics-info")
                graphicsInfo = true;
            else if (arg == "--no-user-graphics")
                noUserGraphics = true;
            else if (arg == "--graphics-set" && i + 1 < argc) {
                const std::string value = argv[++i];
                const auto equals = value.find('=');
                if (equals == std::string::npos || equals == 0)
                    throw std::runtime_error("--graphics-set takes key=value, like anti_aliasing=fxaa");
                graphicsSets.emplace_back(value.substr(0, equals), value.substr(equals + 1));
            } else if (arg == "--anti-aliasing" && i + 1 < argc)
                graphicsSets.emplace_back("anti_aliasing", argv[++i]);
            else if (arg == "--render-scale" && i + 1 < argc) {
                const std::string value = argv[++i];
                const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), renderScaleOverride);
                if (ec != std::errc{} || end != value.data() + value.size() || !std::isfinite(renderScaleOverride) ||
                    renderScaleOverride < 0.25f || renderScaleOverride > 2)
                    throw std::runtime_error("--render-scale takes a number in 0.25..2");
            } else if (arg == "--new-project" && i + 1 < argc)
                newProjectFolder = argv[++i];
            else if (arg == "--name" && i + 1 < argc)
                newProjectName = argv[++i];
            else if (arg == "--module" && i + 1 < argc)
                newProjectModule = argv[++i];
            else if (arg == "--project" && i + 1 < argc)
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
                output = argv[++i];
            } else if (arg == "--no-build") {
                package.noBuild = true;
            } else if (arg == "--no-archive") {
                package.noArchive = true;
            } else if (arg == "--dev-package") {
                package.development = true;
            } else if (arg == "--check") {
                tool.mode = Tool::Mode::Check;
            } else if (arg == "--bake-lighting") {
                tool.mode = Tool::Mode::BakeLighting;
            } else if (arg == "--quality" && i + 1 < argc) {
                tool.bakeQuality = argv[++i];
                if (tool.bakeQuality != "preview" && tool.bakeQuality != "final")
                    throw player::UsageError("--quality takes preview or final");
            } else if (arg == "--capture-reflections") {
                tool.captureReflections = true;
            } else if (arg == "--lighting-info") {
                tool.mode = Tool::Mode::LightingInfo;
            } else if (arg == "--lighting-assign-ids") {
                tool.mode = Tool::Mode::LightingAssignIds;
            } else if (arg == "--lighting-schema" && i + 1 < argc) {
                lightingSchema = argv[++i];
            } else if (arg == "--lighting-export" && i + 1 < argc) {
                lightingExport = argv[++i];
            } else if (arg == "--lighting-import" && i + 1 < argc) {
                lightingImport = argv[++i];
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
            } else if (arg == "--send-message" && i + 1 < argc) {
                tool.circuitMessage = argv[++i];
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
            } else if (arg == "--camera" && i + 1 < argc) {
                tool.camera = argv[++i];
            } else if (arg == "--fixed-step") {
                tool.fixedStep = true;
            } else if (arg == "--image-stats" && i + 1 < argc) {
                imageStats = argv[++i];
            } else if (arg == "--region" && i + 1 < argc) {
                // x,y,w,h as fractions of the image, for --image-stats.
                const std::string value = argv[++i];
                size_t start = 0;
                for (int k = 0; k < 4; ++k) {
                    const size_t comma = k < 3 ? value.find(',', start) : value.size();
                    const char* first = value.data() + start;
                    const char* last = value.data() + std::min(comma, value.size());
                    const auto [end, ec] = std::from_chars(first, last, imageRegion[size_t(k)]);
                    if (comma == std::string::npos || ec != std::errc{} || end != last ||
                        !(imageRegion[size_t(k)] >= 0 && imageRegion[size_t(k)] <= 1))
                        throw player::UsageError("--region takes x,y,width,height as fractions from 0 to 1");
                    start = comma + 1;
                }
                if (imageRegion[2] <= 0 || imageRegion[3] <= 0 || imageRegion[0] + imageRegion[2] > 1.000001 ||
                    imageRegion[1] + imageRegion[3] > 1.000001)
                    throw player::UsageError("--region must be a non-empty part of the image");
            } else if (arg == "--postprocess-schema" && i + 1 < argc) {
                postProcessSchema = argv[++i];
            } else if (arg == "--postprocess-info" && i + 1 < argc) {
                postProcessInfo = argv[++i];
            } else if (arg == "--lut-export-neutral" && i + 2 < argc) {
                lutNeutralSize = uint32_t(wholeNumber(argv[++i], 65, "--lut-export-neutral"));
                lutNeutral = argv[++i];
            } else if (arg == "--effect-info" && i + 1 < argc) {
                effectInfo = argv[++i];
            } else if (arg == "--material-info" && i + 1 < argc) {
                materialInfo = argv[++i];
            } else if (arg == "--texture-info" && i + 1 < argc) {
                textureInfo = argv[++i];
            } else if (arg == "--image-diff" && i + 2 < argc) {
                imageReference = argv[++i];
                imageCandidate = argv[++i];
            } else if (arg == "--tolerance" && i + 1 < argc) {
                const std::string value = argv[++i];
                const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), imageTolerance);
                if (ec != std::errc{} || end != value.data() + value.size() || !std::isfinite(imageTolerance) ||
                    imageTolerance < 0 || imageTolerance > 255)
                    throw player::UsageError("--tolerance takes an RMSE from 0 to 255");
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
                else if (view == "ao")
                    tool.view = shockwave::DebugView::AmbientOcclusion;
                else if (view == "indirect")
                    tool.view = shockwave::DebugView::IndirectDiffuse;
                else if (view == "reflections")
                    tool.view = shockwave::DebugView::Reflections;
                else if (view == "decals")
                    tool.view = shockwave::DebugView::Decals;
                else if (view == "motion")
                    tool.view = shockwave::DebugView::Motion;
                else if (view == "reactive")
                    tool.view = shockwave::DebugView::Reactive;
                else if (view == "clusters")
                    tool.view = shockwave::DebugView::Clusters;
                else if (view == "fog")
                    tool.view = shockwave::DebugView::Fog;
                else if (view == "transmission")
                    tool.view = shockwave::DebugView::Transmission;
                else if (view == "shading")
                    tool.view = shockwave::DebugView::ShadingFamilies;
                else if (view == "navmesh")
                    tool.showNavMesh = true;
                else
                    throw std::runtime_error("--view takes lit, unlit, normals, lighting, cascades, ao, indirect, "
                                             "reflections, decals, motion, reactive, clusters, fog, transmission, "
                                             "shading or navmesh");
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
                if (ec != std::errc{} || end != value.data() + value.size() || !std::isfinite(fps) || fps < 0 ||
                    (fps > 0 && fps < 10) || fps > 1000)
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
            } else if (arg == "--graphics-report") {
                tool.graphicsReportFrame = 10;
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
            } else if (arg == "--network-info") {
                networkInfo = true;
            } else if (arg == "--network-schema" && i + 1 < argc) {
                networkSchema = argv[++i];
            } else if (arg == "--steam-status") {
                steamStatus = 1;
                if (i + 1 < argc &&
                    (std::string_view(argv[i + 1]) == "client" || std::string_view(argv[i + 1]) == "server"))
                    steamStatus = std::string_view(argv[++i]) == "server" ? 2 : 1;
            } else if (arg == "--net-preview") {
                tool.netPreview = "poor";
                if (i + 1 < argc && argv[i + 1][0] != '-') tool.netPreview = argv[++i];
            } else if (arg == "--net-test") {
                netTest.requested = true;
            } else if (arg == "--scenario" && i + 1 < argc) {
                netTest.scenario = argv[++i];
            } else if (arg == "--preset" && i + 1 < argc) {
                netTest.preset = argv[++i];
            } else if (arg == "--seed" && i + 1 < argc) {
                netTest.seed = tool.keySeed = wholeNumber(argv[++i], UINT64_MAX, "--seed");
                tool.keySeedGiven = true;
            } else if (arg == "--host") {
                if (tool.online != Tool::Online::None)
                    throw player::UsageError("--host, --join-lobby and --join-steam are one each");
                tool.online = Tool::Online::Host;
            } else if (arg == "--join-lobby" && i + 1 < argc) {
                if (tool.online != Tool::Online::None)
                    throw player::UsageError("--host, --join-lobby and --join-steam are one each");
                tool.online = Tool::Online::JoinLobby;
                tool.onlineId = steamNumber(argv[++i], "--join-lobby");
                joinLobby = true;
            } else if (arg == "--join-steam" && i + 1 < argc) {
                if (tool.online != Tool::Online::None)
                    throw player::UsageError("--host, --join-lobby and --join-steam are one each");
                tool.online = Tool::Online::JoinDirect;
                tool.onlineId = steamNumber(argv[++i], "--join-steam");
                joinSteam = true;
            } else if (arg == "+connect_lobby" && i + 1 < argc) {
                tool.connectLobby = steamNumber(argv[++i], "+connect_lobby");   // Steam's own launch option
            } else if (arg == "--dev-direct-join") {
                devDirectJoin = true;
            } else if (arg == "--net-capture" && i + 1 < argc) {
                tool.netCapture = argv[++i];
            } else if (arg == "--net-dump" && i + 1 < argc) {
                netDump = argv[++i];
            } else if (arg == "--net-encode" && i + 1 < argc) {
                netEncode = argv[++i];
            } else if (arg == "--network-manifest") {
                tool.mode = Tool::Mode::NetworkManifest;
            } else if (arg == "--assign-network-keys") {
                tool.mode = Tool::Mode::AssignNetworkKeys;
            } else if (arg == "--clients" && i + 1 < argc) {
                netTest.clients = uint32_t(wholeNumber(argv[++i], 63, "--clients"));
            } else if (arg == "--saves") {
                tool.listSaves = true;
            } else if (arg == "--load" && i + 1 < argc) {
                tool.load = argv[++i];
            } else if (arg == "--save" && i + 1 < argc) {
                tool.save = argv[++i];
                if (!shockwave::validSaveSlot(tool.save))
                    throw std::runtime_error("--save takes a slot name: letters, digits, spaces, - and _");
            } else if (arg == "--help") {
                std::puts("player --package [--output folder] [--no-build] [--no-archive] [--dev-package]\n"
                          "                                    make a game folder for players (docs/Packaging.md)");
                std::puts(
                    "player [project.swproject] [--project path] [--scene file.swscene] [--frames N]\n"
                    "player --new-project folder [--name display-name] [--module module_id]\n"
                    "                                    create an independent C++ starter in an empty folder\n"
                    "       [--size WxH] [--view "
                    "lit|unlit|normals|lighting|cascades|ao|indirect|reflections|decals|motion|reactive|clusters|"
                    "navmesh] "
                    "[--print-frame]\n"
                    "       [--graphics file.swgraphics] [--no-user-graphics] [--render-scale 0.25..2]\n"
                    "       [--graphics-info [--json file]] [--graphics-schema file|-]\n"
                    "       [--graphics-set key=value]... [--anti-aliasing none|fxaa|taa]\n"
                    "                                    any setting in docs/GraphicsSchema.md, for this run\n"
                    "       [--shadows off|low|medium|high|ultra] [--fps N] [--low-latency] [--vsync on|off]\n"
                    "       [--renderer vulkan|null] [--gpu name|number] [--game-module file.so]\n"
                    "player --gpus [--json file]          list the GPUs, which one renders and why, and any\n"
                    "                                    warnings (like rendering on a GPU the screen isn't on)\n"
                    "player --renderers                  list the renderer plugins that can be loaded\n"
                    "player --screenshot out.png [--scene file] [--frames N] [--size WxH] [--view ...]\n"
                    "                                    play N frames (default 30), save the last, quit\n"
                    "       [--camera name] [--fixed-step]\n"
                    "                                    look through a named Camera entity; advance exactly\n"
                    "                                    one tick per frame so motion is reproducible\n"
                    "player --image-stats file.png [--region x,y,w,h] [--json file]\n"
                    "player --image-diff reference.png candidate.png [--tolerance rmse] [--region x,y,w,h] [--json "
                    "file]\n"
                    "                                    compare two screenshots; exit 1 over the tolerance\n"
                    "player --material-info model.glb|file.swmaterial [--json file]\n"
                    "                                    each material slot's UV set, transform and sampler;\n"
                    "                                    for a .swmaterial, also its parents, values, features,\n"
                    "                                    what material_quality low leaves out, and warnings\n"
                    "player --material-schema file|-     write the .swmaterial reference (docs/MaterialSchema.md)\n"
                    "player --effect-info file.efkefc [--json file]\n"
                    "                                    each part of a particle effect and how it's drawn:\n"
                    "                                    supported, approximated or unsupported (docs/Effects.md)\n"
                    "player --postprocess-schema file|- write the .swpostprocess reference "
                    "(docs/PostProcessSchema.md)\n"
                    "player --postprocess-info file.swpostprocess [--json file]\n"
                    "                                    a grading, exposure and fog profile with its parents\n"
                    "                                    applied, and its LUT (docs/PostProcess.md)\n"
                    "player --lut-export-neutral 17|33|65 file.cube|-\n"
                    "                                    an identity LUT to grade in another tool\n"
                    "player --bake-lighting [--scene file.swscene] [--quality preview|final] [--seed N]\n"
                    "                                    bake lightmaps and probe volumes (docs/Lighting.md)\n"
                    "player --capture-reflections [--scene file.swscene] [--size WxH]\n"
                    "                                    capture reflection probes on the GPU (docs/Lighting.md)\n"
                    "player --lighting-info [--scene file.swscene] [--json file]\n"
                    "                                    the scene's bake: state, why it's out of date, sizes\n"
                    "player --lighting-assign-ids [--scene file.swscene] [--output new.swscene]\n"
                    "                                    give bake participants missing or duplicate ids\n"
                    "player --lighting-schema file|-     write the baked lighting reference (docs/LightingSchema.md)\n"
                    "player --lighting-export payload --output file.json\n"
                    "player --lighting-import file.json --output payload\n"
                    "                                    a lightmap .ktx2, .swbake or .swprobe as JSON, and back\n"
                    "player --texture-info texture.png|model.glb [--json file]\n"
                    "                                    levels and GPU bytes at each texture_quality\n"
                    "       [--ui-preview] [--ui-nav down,down,confirm]\n"
                    "                                    show UI preview values; step through menus first\n"
                    "player --graphics-report [--frames N] [--json file]\n"
                    "                                    play N frames (default 10) hidden, then print what\n"
                    "                                    the GPU supports, each graphics setting as asked for\n"
                    "                                    and as running (with why), and GPU memory by category\n"
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
                    "player --circuit file --run-circuit [--circuit-ticks N] [--send-message name] [--break-node name] "
                    "[--circuit-steps N]\n"
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
                    "player --network-info [--json file] the project's multiplayer settings, digests and\n"
                    "                                    problems; opens no socket (docs/Networking.md)\n"
                    "player --network-schema file.md     write the multiplayer reference (- for stdout)\n"
                    "player --steam-status [client|server]\n"
                    "                                    start Steam for the project's steam_app_id, print\n"
                    "                                    sign-in and relay status, shut it down\n"
                    "player --net-test [--scenario handshake|rejections|stop-start|replication|prediction] [--seed N]\n"
                    "       [--preset clean|cafe|poor|hostile] [--clients N] [--json file]\n"
                    "                                    run the session handshake over an in-process\n"
                    "                                    loopback network; no Steam, sockets or window\n"
                    "player --net-preview [clean|cafe|poor|hostile]\n"
                    "                                    play the scene as a client of a host in this\n"
                    "                                    process over a simulated network (default poor),\n"
                    "                                    to see prediction and interpolation\n"
                    "player --host [--scene file.swscene] [--net-capture file.swnetcap]\n"
                    "                                    host a friends-only Steam match of the match scene\n"
                    "player --join-lobby ID              join a Steam lobby's match (+connect_lobby ID too)\n"
                    "player --host --dev-direct-join     developer: host with no lobby, for direct joins\n"
                    "player --join-steam STEAMID --dev-direct-join\n"
                    "                                    developer: join such a host by its Steam ID\n"
                    "player --net-dump file.swnetcap [--json file]\n"
                    "                                    decode a packet capture (no project needed)\n"
                    "player --net-encode file.json --output file.swnetcap\n"
                    "                                    encode a decoded capture back\n"
                    "player --network-manifest --output file.swnetmanifest\n"
                    "                                    write a candidate manifest and its digests\n"
                    "player --assign-network-keys --scene file.swscene --output new.swscene [--seed N]\n"
                    "                                    fill missing or duplicate network keys\n"
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
                throw player::UsageError("Unknown or incomplete option: " + arg);
        }
        // Developer direct joins say so, on both sides.
        if (joinSteam && !devDirectJoin)
            throw player::UsageError("--join-steam is a developer direct join: add --dev-direct-join (friends join "
                                     "with --join-lobby or an invite)");
        if (devDirectJoin && tool.online == Tool::Online::Host)
            tool.online = Tool::Online::HostDirect;
        else if (devDirectJoin && !joinSteam)
            throw player::UsageError("--dev-direct-join goes with --host or --join-steam");
        (void)joinLobby;
        // Each of these is a mode of its own; two at once is a mistake, never a choice.
        {
            std::vector<std::string> modes;
            auto mode = [&](bool on, const char* name) {
                if (on) modes.push_back(name);
            };
            static const char* toolNames[] = {
                "",
                "--check",
                "--schema",
                "--bake-navmesh",
                "--audio-mix",
                "--animations",
                "--node-schema",
                "--run-circuit",
                "--circuit --dump",
                "--network-manifest",
                "--assign-network-keys",
                "--lighting-info",
                "--lighting-assign-ids",
                "--bake-lighting"};
            mode(tool.mode != Tool::Mode::Play, toolNames[int(tool.mode)]);
            mode(package.requested, "--package");
            mode(!tool.screenshot.empty(), "--screenshot");
            mode(tool.captureReflections, "--capture-reflections");
            mode(tool.benchmarkSeconds > 0, "--benchmark");
            mode(tool.graphicsReportFrame > 0, "--graphics-report");
            mode(tool.optimise, "--optimise");
            mode(!tool.save.empty(), "--save");
            mode(tool.listSaves, "--saves");
            mode(networkInfo, "--network-info");
            mode(!networkSchema.empty(), "--network-schema");
            mode(steamStatus != 0, "--steam-status");
            mode(netTest.requested, "--net-test");
            mode(!netDump.empty(), "--net-dump");
            mode(!lightingSchema.empty(), "--lighting-schema");
            mode(!lightingExport.empty(), "--lighting-export");
            mode(!lightingImport.empty(), "--lighting-import");
            mode(!netEncode.empty(), "--net-encode");
            mode(!tool.netPreview.empty(), "--net-preview");
            mode(tool.online == Tool::Online::Host || tool.online == Tool::Online::HostDirect, "--host");
            mode(tool.online == Tool::Online::JoinLobby, "--join-lobby");
            mode(tool.online == Tool::Online::JoinDirect, "--join-steam");
            // Only the network and online modes are strict: older tools keep combining as before.
            static const std::vector<std::string> strict = {
                "--package",        "--network-info",     "--network-schema",     "--steam-status",
                "--net-test",       "--net-dump",         "--net-encode",         "--net-preview",
                "--host",           "--join-lobby",       "--join-steam",         "--run-circuit",
                "--circuit --dump", "--network-manifest", "--assign-network-keys"};
            const bool anyStrict = std::any_of(modes.begin(), modes.end(), [](const std::string& m) {
                return std::find(strict.begin(), strict.end(), m) != strict.end();
            });
            if (modes.size() > 1 && anyStrict) {
                std::fprintf(
                    stderr, "Player: %s and %s can't be combined: they run on their own\n", modes[0].c_str(),
                    modes[1].c_str());
                return 2;
            }
            const bool online = tool.online != Tool::Online::None || tool.connectLobby;
            if (!tool.netCapture.empty() && tool.online == Tool::Online::None && tool.netPreview.empty())
                throw player::UsageError("--net-capture records a session: add --host, a join or --net-preview");
            if (online && (!tool.load.empty() || !tool.netPreview.empty()))
                throw player::UsageError("online play doesn't go with --load or --net-preview");
            if (tool.online != Tool::Online::None && tool.connectLobby)
                throw player::UsageError("+connect_lobby is a join of its own: drop --host and the join switches");
            if (!netEncode.empty() && output.empty())
                throw player::UsageError("--net-encode needs --output file.swnetcap");
            if (tool.mode == Tool::Mode::NetworkManifest && output.empty())
                throw player::UsageError("--network-manifest needs --output file: it never rewrites the manifest");
            if (tool.mode == Tool::Mode::AssignNetworkKeys && (tool.scene.empty() || output.empty()))
                throw player::UsageError("--assign-network-keys needs --scene file.swscene and --output new-file");
            if ((!lightingExport.empty() || !lightingImport.empty()) && output.empty())
                throw player::UsageError("--lighting-export and --lighting-import need --output file");
            if (tool.bakeQuality != "preview" && tool.mode != Tool::Mode::BakeLighting)
                throw player::UsageError("--quality goes with --bake-lighting");
            if (!output.empty() && !package.requested && netEncode.empty() && lightingExport.empty() &&
                lightingImport.empty() && tool.mode != Tool::Mode::NetworkManifest &&
                tool.mode != Tool::Mode::AssignNetworkKeys && tool.mode != Tool::Mode::LightingAssignIds)
                throw player::UsageError("--output goes with --package, --net-encode, --network-manifest, "
                                         "--assign-network-keys, --lighting-assign-ids, --lighting-export or "
                                         "--lighting-import");
            if ((package.development || package.noBuild || package.noArchive) && !package.requested)
                throw player::UsageError("--dev-package, --no-build and --no-archive go with --package");
            package.output = tool.output = output;
        }
        if (!imageStats.empty()) return player::imageStats(imageStats, tool.benchmarkJson, imageRegion);   // no project
        if (!materialInfo.empty()) return player::materialInfo(materialInfo, tool.benchmarkJson);
        if (!effectInfo.empty()) return player::effectInfo(effectInfo, tool.benchmarkJson);   // no project
        if (!postProcessSchema.empty()) return player::postProcessSchema(postProcessSchema);
        if (!postProcessInfo.empty()) return player::postProcessInfo(postProcessInfo, tool.benchmarkJson);
        if (!lutNeutral.empty()) return player::lutExportNeutral(lutNeutralSize, lutNeutral);
        if (!textureInfo.empty()) return player::textureInfo(textureInfo, tool.benchmarkJson);
        if (!imageReference.empty())
            return player::imageDiff(imageReference, imageCandidate, imageTolerance, tool.benchmarkJson, imageRegion);
        if (!netDump.empty()) return player::netDump(netDump, tool.benchmarkJson);   // no project
        if (!netEncode.empty()) return player::netEncode(netEncode, output);
        if (!materialSchema.empty()) return player::materialSchema(materialSchema);   // no project
        if (!lightingSchema.empty()) return player::lightingSchema(lightingSchema);
        if (!lightingExport.empty()) return player::lightingExport(lightingExport, output);
        if (!lightingImport.empty()) return player::lightingImport(lightingImport, output);
        if (!graphicsSchema.empty()) {
            const auto schema = shockwave::graphicsSettingsSchema();
            if (graphicsSchema == "-")
                std::fputs(schema.c_str(), stdout);
            else {
                std::ofstream out(graphicsSchema);
                out << schema;
                out.close();
                if (!out) throw std::runtime_error("can't write " + graphicsSchema.string());
            }
            return 0;
        }
        if (!newProjectFolder.empty()) {
            if (newProjectName.empty()) newProjectName = newProjectFolder.lexically_normal().filename().string();
            if (newProjectModule.empty()) newProjectModule = shockwave::projectModuleId(newProjectName);
            shockwave::Project created;
            std::string error;
            const char* base = SDL_GetBasePath();
            if (!base || !shockwave::createGameProject(
                             newProjectFolder, newProjectName, newProjectModule,
                             std::filesystem::path(base) / "templates" / "Blank", created, error))
                throw std::runtime_error(error.empty() ? "can't locate project templates" : error);
            std::printf("project %s\nmodule %s\n", created.file.string().c_str(), created.gameModule.c_str());
            std::puts("Add this folder to SHOCKWAVE_GAMES, then build player/editor before Play. See the project's "
                      "README.md.");
            return 0;
        }
        if (!newProjectName.empty() || !newProjectModule.empty())
            throw std::runtime_error("--name and --module require --new-project");
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
        if (std::filesystem::is_directory(manifest)) {
            const auto found = shockwave::findProjectFile(manifest);
            if (!found) throw std::runtime_error("no .swproject in " + manifest.string());
            manifest = *found;
        }
        if (!shockwave::loadProject(manifest, project, error)) throw std::runtime_error(error);
        if (!shockwave::projectGraphicsSettings(project, tool.graphicsDefaults, error)) throw std::runtime_error(error);
        tool.graphics = tool.graphicsDefaults;
        // Reproducible tools ignore local user preferences. Explicit --graphics still
        // loads (and --check validates) the file, without a window or GPU.
        if (!graphicsFile.empty()) {
            tool.graphicsFile = std::filesystem::absolute(graphicsFile);
            if (!shockwave::loadGraphicsSettings(tool.graphicsFile, tool.graphics, error))
                throw std::runtime_error(error);
        } else if (
            !noUserGraphics && tool.mode == Tool::Mode::Play && !tool.optimise && tool.benchmarkSeconds == 0 &&
            tool.screenshot.empty() && tool.graphicsReportFrame == 0 && !package.requested &&
            !tool.captureReflections) {
            if (char* pref = SDL_GetPrefPath("Shockwave", project.gameModule.c_str())) {
                tool.graphicsFile = std::filesystem::path(pref) / "Graphics.swgraphics";
                SDL_free(pref);
                if (std::filesystem::exists(tool.graphicsFile) &&
                    !shockwave::loadGraphicsSettings(tool.graphicsFile, tool.graphics, error))
                    SW_LOG(shockwave::LogEngine, Warning, "Ignoring user graphics settings: {}", error);
            }
        }
        if (fpsOverride >= 0) tool.graphics.frameLimit = fpsOverride;
        if (vsyncOverride >= 0) tool.graphics.vsync = vsyncOverride == 1;
        if (lowLatency) tool.graphics.lowLatency = true;
        if (tool.shadows >= 0) tool.graphics.shadows = static_cast<shockwave::ShadowQuality>(tool.shadows);
        if (renderScaleOverride >= 0) tool.graphics.renderScale = renderScaleOverride;
        for (const auto& [key, value] : graphicsSets)
            if (!shockwave::setGraphicsSetting(tool.graphics, key, value, error))
                throw std::runtime_error("--graphics-set: " + error);
        if (graphicsInfo) {
            const auto preset = shockwave::matchingGraphicsPreset(tool.graphics);
            if (!tool.benchmarkJson.empty()) {
                std::ofstream(tool.benchmarkJson) << player::graphicsInfoJson(tool.graphics, tool.graphicsFile);
                std::printf("Wrote %s\n", tool.benchmarkJson.string().c_str());
                return 0;
            }
            std::printf(
                "# user file: %s\n# preset: %s\n",
                tool.graphicsFile.empty() ? "disabled" : tool.graphicsFile.string().c_str(),
                preset ? shockwave::graphicsPresetName(*preset) : "custom");
            std::fputs(shockwave::graphicsSettingsText(tool.graphics).c_str(), stdout);
            return 0;
        }
        const bool networkTool = networkInfo || !networkSchema.empty() || steamStatus || netTest.requested;
        if (networkTool && (!tool.load.empty() || tool.online != Tool::Online::None)) {
            std::fprintf(
                stderr, "Player: --network-info, --network-schema, --steam-status and --net-test run on their own\n");
            return 2;
        }
        if (!tool.netPreview.empty() &&
            (tool.mode != Tool::Mode::Play || tool.listSaves || !tool.save.empty() || !tool.load.empty())) {
            std::fprintf(stderr, "Player: --net-preview plays the scene; it doesn't go with tools, --save or --load\n");
            return 2;
        }
        // A packaged Steam game started outside Steam: Steam starts it again, its way.
        if (tool.mode == Tool::Mode::Play && !player::packagedProjectFile().empty() &&
            player::relaunchThroughSteam(project))
            return 0;
        if (netTest.requested) return player::netTest(project, netTest, tool.benchmarkJson);   // no window or game code
        if (steamStatus) return player::steamStatus(project, steamStatus == 2);   // no game module or window
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
        // Multiplayer tools read files and the game's registered types; no window or GPU.
        if (networkInfo) return player::networkInfo(project, gameModule->module(), tool.benchmarkJson);
        if (!networkSchema.empty()) return player::writeNetworkSchema(gameModule->module(), networkSchema);
        shockwave::EngineConfig config;
        config.window = {project.name, project.width, project.height, tool.graphics.vsync};
        config.tickRate = project.tickRate;
        config.frameLimit = tool.graphics.frameLimit;
        config.lowLatency = tool.graphics.lowLatency;
        if (vsyncOverride >= 0) config.window.vsync = vsyncOverride == 1;
        config.maxFrames = frames;
        if (tool.fixedStep) config.simulationTime = shockwave::SimulationTime::Fixed;
        if (size.first) config.window.width = size.first, config.window.height = size.second;
        if (!tool.screenshot.empty()) {
            // Runs hidden until the frame is saved; maxFrames is only a safety net.
            if (frames) tool.screenshotFrame = frames;
            config.maxFrames = tool.screenshotFrame + 120;
            config.window.hidden = true;
            config.window.vsync = false;
        }
        if (tool.graphicsReportFrame > 0) {
            // Like a screenshot: hidden, a few frames so everything sized on the first is counted.
            if (frames) tool.graphicsReportFrame = frames;
            config.maxFrames = tool.graphicsReportFrame + 120;
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
        if (tool.captureReflections) {
            // Hidden, on the project's GPU renderer; quits once the manifest is written.
            // maxFrames is a safety net: six frames a probe, plus a few to start.
            config.maxFrames = 0;
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
    } catch (const player::UsageError& e) {
        std::fprintf(stderr, "Player: %s\n", e.what());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Player: %s\n", e.what());
        return 1;
    }
    return 0;
}
