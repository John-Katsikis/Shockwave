// player: the standalone game executable and its command-line tools; see Player.h.
#include "Player.h"
#include "PlayerFlags.h"

#include <Core/Diagnostics.h>

#include <Assets/AssetFolder.h>
#include <Assets/BuiltinAssets.h>
#include <Engine/Engine.h>
#include <Engine/ReleaseTier.h>
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

namespace {
// Ends the run's diagnostics on every way out of main() but a crash: the session marker
// goes, so the next start knows this one ended cleanly.
struct RunDiagnostics {
    bool begun = false;
    ~RunDiagnostics() {
        if (begun) player::endRunDiagnostics();
    }
};
}   // namespace

int main(int argc, char** argv) {
    player::startDiagnostics();
    player::setCommandLine(argc, argv);
    RunDiagnostics runDiagnostics;
    try {
        player::ParseState state;   // what the flags set (PlayerFlags.h)
        auto& manifest = state.manifest;
        auto& newProjectFolder = state.newProjectFolder;
        auto& newProjectName = state.newProjectName;
        auto& newProjectModule = state.newProjectModule;
        auto& frames = state.frames;
        auto& audioVolume = state.audioVolume;
        auto& tool = state.tool;
        auto& package = state.package;
        auto& size = state.size;
        auto& fpsOverride = state.fpsOverride;
        auto& lowLatency = state.lowLatency;
        auto& vsyncOverride = state.vsyncOverride;
        auto& renderer = state.renderer;
        auto& gameModuleFile = state.gameModuleFile;
        auto& graphicsFile = state.graphicsFile;
        auto& graphicsSchema = state.graphicsSchema;
        auto& materialSchema = state.materialSchema;
        auto& graphicsInfo = state.graphicsInfo;
        auto& noUserGraphics = state.noUserGraphics;
        auto& preferencesInfo = state.preferencesInfo;
        auto& noUserPreferences = state.noUserPreferences;
        auto& preferencesSchema = state.preferencesSchema;
        auto& audioVolumeSet = state.audioVolumeSet;
        auto& noPipelineCache = state.noPipelineCache;
        auto& clearPipelineCache = state.clearPipelineCache;
        auto& noPipelineWarmup = state.noPipelineWarmup;
        auto& graphicsPresetArg = state.graphicsPresetArg;
        auto& renderScaleOverride = state.renderScaleOverride;
        auto& graphicsSets = state.graphicsSets;
        auto& listGpus = state.listGpus;
        auto& networkInfo = state.networkInfo;
        auto& networkSchema = state.networkSchema;
        auto& releaseInfo = state.releaseInfo;
        auto& releaseTier = state.releaseTier;
        auto& cookInfoFile = state.cookInfoFile;
        auto& cookVerify = state.cookVerify;
        auto& userPathsCommand = state.userPathsCommand;
        auto& releaseSchema = state.releaseSchema;
        auto& levelSchema = state.levelSchema;
        auto& steamStatus = state.steamStatus;
        auto& netTest = state.netTest;
        auto& diagnostics = state.diagnostics;
        auto& install = state.install;
        auto& netDump = state.netDump;
        auto& netEncode = state.netEncode;
        auto& imageStats = state.imageStats;
        auto& imageReference = state.imageReference;
        auto& imageCandidate = state.imageCandidate;
        auto& materialInfo = state.materialInfo;
        auto& textureInfo = state.textureInfo;
        auto& effectInfo = state.effectInfo;
        auto& saveInfoFile = state.saveInfoFile;
        auto& saveMigrateFile = state.saveMigrateFile;
        auto& postProcessSchema = state.postProcessSchema;
        auto& postProcessInfo = state.postProcessInfo;
        auto& lutNeutral = state.lutNeutral;
        auto& lutNeutralSize = state.lutNeutralSize;
        auto& terrainInfo = state.terrainInfo;
        auto& terrainImport = state.terrainImport;
        auto& lightingSchema = state.lightingSchema;
        auto& lightingExport = state.lightingExport;
        auto& lightingImport = state.lightingImport;
        auto& imageTolerance = state.imageTolerance;
        auto& imageRegion = state.imageRegion;
        auto& output = state.output;
        auto& devDirectJoin = state.devDirectJoin;
        auto& joinSteam = state.joinSteam;
        auto& joinLobby = state.joinLobby;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            // A flag with the values it needs is its row in the table (PlayerFlags*.cpp).
            if (const player::Flag* flag = player::findFlag(arg, argc - 1 - i)) {
                player::FlagArgs args(arg, i, argc, argv);
                if (const std::optional<int> exit = flag->apply(state, args)) return *exit;
                continue;
            }
            if (tool.mode == Tool::Mode::Check && std::filesystem::path(arg).extension() == ".swsave") {
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
                "--bake-lighting",
                "--levels",
                "--release-info",
                "--asset-dependencies",
                "--cook"};
            mode(tool.mode != Tool::Mode::Play, toolNames[int(tool.mode)]);
            mode(package.requested, "--package");
            mode(!tool.screenshot.empty(), "--screenshot");
            mode(tool.captureReflections, "--capture-reflections");
            mode(tool.benchmarkSeconds > 0, "--benchmark");
            mode(tool.perfRun, "--perf-run");
            mode(tool.graphicsReportFrame > 0, "--graphics-report");
            mode(tool.optimise, "--optimise");
            mode(!tool.save.empty(), "--save");
            mode(tool.listSaves, "--saves");
            mode(!saveInfoFile.empty(), "--save-info");
            mode(!saveMigrateFile.empty(), "--save-migrate");
            mode(networkInfo, "--network-info");
            mode(!networkSchema.empty(), "--network-schema");
            mode(releaseInfo, "--release-info");
            mode(releaseTier, "--release-tier");
            mode(!cookInfoFile.empty(), "--cook-info");
            mode(userPathsCommand.info, "--user-paths");
            mode(!userPathsCommand.importLegacy.empty(), "--import-legacy-saves");
            mode(!releaseSchema.empty(), "--release-schema");
            mode(!levelSchema.empty(), "--level-schema");
            mode(steamStatus != 0, "--steam-status");
            mode(netTest.requested, "--net-test");
            mode(diagnostics.info, "--diagnostics");
            mode(!diagnostics.symbolize.empty(), "--symbolize");
            mode(!install.package.empty(), "--verify-install");
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
                "--package",
                "--network-info",
                "--network-schema",
                "--steam-status",
                "--net-test",
                "--net-dump",
                "--net-encode",
                "--net-preview",
                "--host",
                "--join-lobby",
                "--join-steam",
                "--run-circuit",
                "--circuit --dump",
                "--network-manifest",
                "--assign-network-keys",
                "--release-info",
                "--release-schema",
                "--user-paths",
                "--import-legacy-saves",
                "--levels",
                "--level-schema",
                "--asset-dependencies",
                "--cook",
                "--cook-info",
                "--release-tier",
                "--perf-run",
                "--diagnostics",
                "--symbolize",
                "--verify-install"};
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
            if (tool.assetLoading == "levels" && !tool.assetResidency)
                throw player::UsageError("--asset-loading levels needs level leases: drop --no-asset-residency");
            if (!tool.perfRecord.empty() && !tool.perfRun) throw player::UsageError("--perf-record goes with --perf-run");
            if (tool.perfLaps != 1 && !tool.perfRun && tool.flowTest.empty())
                throw player::UsageError("--laps goes with --flow-test or --perf-run");
            if (tool.lapToleranceMib >= 0 && (tool.perfRun || tool.perfLaps < 3))
                throw player::UsageError("--lap-tolerance goes with --flow-test and --laps 3 or more");
            if (tool.coldCache && !tool.perfRun && tool.benchmarkSeconds <= 0)
                throw player::UsageError("--cold-cache goes with --perf-run or --benchmark");
            if ((!install.machine.empty() || !install.workFolder.empty() || !install.updateFrom.empty() ||
                 install.keep || install.verbose) &&
                install.package.empty())
                throw player::UsageError("--install-record, --install-dir, --update-from, --keep-install and --verbose "
                                         "go with "
                                         "--verify-install");
            if (!install.package.empty() && !output.empty()) {
                if (install.machine.empty()) throw player::UsageError("--output goes with --install-record here");
                install.recordFolder = output;   // where the record goes instead of Release/Installs
                output.clear();
            }
            if (tool.perfRun && !tool.perfRecord.empty() && !output.empty()) {
                tool.perfRecordFolder = output;   // where the record goes instead of Release/Performance
                output.clear();
            }
            if (!output.empty() && !package.requested && netEncode.empty() && lightingExport.empty() &&
                lightingImport.empty() && saveMigrateFile.empty() && terrainImport.empty() &&
                tool.mode != Tool::Mode::NetworkManifest && tool.mode != Tool::Mode::Cook &&
                tool.mode != Tool::Mode::AssignNetworkKeys && tool.mode != Tool::Mode::LightingAssignIds)
                throw player::UsageError("--output goes with --package, --net-encode, --network-manifest, "
                                         "--assign-network-keys, --lighting-assign-ids, --lighting-export, "
                                         "--lighting-import, --save-migrate or --terrain-import");
            if ((package.development || package.noBuild || package.noArchive) && !package.requested)
                throw player::UsageError("--dev-package, --no-build and --no-archive go with --package");
            // --level picks a catalog level; guessing which wins against another start is
            // worse than saying so (LevelOptionConflict, docs/LevelSchema.md).
            if (!tool.level.empty()) {
                if (!tool.scene.empty())
                    throw player::UsageError(
                        "LevelOptionConflict: --level and --scene both choose the start; pick one");
                if (!tool.load.empty())
                    throw player::UsageError(
                        "LevelOptionConflict: --load starts where the save was made; drop --level");
                if (online || !tool.netPreview.empty())
                    throw player::UsageError(
                        "LevelOptionConflict: online play starts in the network settings' match scene; drop --level");
                // --asset-dependencies --level id: that level's closure only.
                if (tool.mode != Tool::Mode::Play && tool.mode != Tool::Mode::AssetDependencies)
                    throw player::UsageError("LevelOptionConflict: --level goes with play, not with tools");
            }
            if (userPathsCommand.preview && userPathsCommand.importLegacy.empty())
                throw player::UsageError("--preview goes with --import-legacy-saves");
            if (std::string why; !shockwave::validUserProfile(userPathsCommand.profile, why))
                throw player::UsageError("--profile: " + why);
            package.output = tool.output = output;
        }
        if (tool.release && tool.mode != Tool::Mode::Cook && tool.mode != Tool::Mode::Check && !package.requested)
            throw player::UsageError("--release goes with --cook, --check or --package");
        if (tool.release && package.development)
            throw player::UsageError(
                "--release and --dev-package don't go together: a release never ships as a test build");
        package.release = tool.release && package.requested;
        if (!tool.cookTarget.empty() && tool.mode != Tool::Mode::Cook)
            throw player::UsageError("--target goes with --cook");
        if (!cookVerify.empty() && cookInfoFile.empty()) throw player::UsageError("--verify goes with --cook-info");
        if (!install.package.empty()) {
            // A project is optional: it says where records go and which scenario to play.
            if (!manifest.empty()) {
                std::filesystem::path file = manifest;
                if (std::error_code ec; std::filesystem::is_directory(file, ec)) {
                    const auto found = shockwave::findProjectFile(file);
                    if (!found) throw std::runtime_error("no .swproject in " + file.string());
                    file = *found;
                }
                shockwave::Project project;
                std::string error;
                if (!shockwave::loadProject(file, project, error)) throw std::runtime_error(error);
                if (!install.machine.empty() && install.recordFolder.empty())
                    install.recordFolder = shockwave::installFolder(project);
                if (install.scenarios.empty()) {
                    shockwave::ReleaseFile contract;
                    shockwave::Diagnostic problem;
                    if (shockwave::readRelease(
                            shockwave::releasePath(project), "Config/Release.swrelease", contract, problem) &&
                        !contract.settings.acceptanceScenario.empty())
                        install.scenarios.push_back(project.root / contract.settings.acceptanceScenario);
                }
            }
            if (!install.machine.empty() && install.recordFolder.empty())
                throw player::UsageError("--install-record keeps the record in a project's Release/Installs: add "
                                         "--project, or --output folder");
            return player::verifyInstall(install, renderer, tool.benchmarkJson);   // the project is optional
        }
        if (!cookInfoFile.empty())
            return player::cookInfo(cookInfoFile, cookVerify, tool.benchmarkJson);   // no project
        if (!diagnostics.symbolize.empty())
            return player::symbolizeReport(diagnostics, tool.benchmarkJson);   // no project
        if (!imageStats.empty()) return player::imageStats(imageStats, tool.benchmarkJson, imageRegion);   // no project
        if (!materialInfo.empty()) return player::materialInfo(materialInfo, tool.benchmarkJson);
        if (!effectInfo.empty()) return player::effectInfo(effectInfo, tool.benchmarkJson);   // no project
        if (!postProcessSchema.empty()) return player::postProcessSchema(postProcessSchema);
        if (!postProcessInfo.empty()) return player::postProcessInfo(postProcessInfo, tool.benchmarkJson);
        if (!lutNeutral.empty()) return player::lutExportNeutral(lutNeutralSize, lutNeutral);
        if (!terrainInfo.empty()) return player::terrainInfo(terrainInfo, tool.benchmarkJson);   // no project
        if (!terrainImport.empty())
            return player::terrainImport(
                terrainImport, output, state.terrainSpacing, state.terrainLow, state.terrainHigh);   // no project
        if (!textureInfo.empty()) return player::textureInfo(textureInfo, tool.benchmarkJson);
        if (!imageReference.empty())
            return player::imageDiff(imageReference, imageCandidate, imageTolerance, tool.benchmarkJson, imageRegion);
        if (!netDump.empty()) return player::netDump(netDump, tool.benchmarkJson);   // no project
        if (!netEncode.empty()) return player::netEncode(netEncode, output);
        if (!materialSchema.empty()) return player::materialSchema(materialSchema);   // no project
        if (!releaseSchema.empty()) return player::releaseSchema(releaseSchema);
        if (!levelSchema.empty()) return player::levelSchema(levelSchema);
        if (!lightingSchema.empty()) return player::lightingSchema(lightingSchema);
        if (!lightingExport.empty()) return player::lightingExport(lightingExport, output);
        if (!lightingImport.empty()) return player::lightingImport(lightingImport, output);
        if (!preferencesSchema.empty()) {
            const auto schema = shockwave::preferencesSchema();
            if (preferencesSchema == "-")
                std::fputs(schema.c_str(), stdout);
            else {
                std::ofstream out(preferencesSchema);
                out << schema;
                out.close();
                if (!out) throw std::runtime_error("can't write " + preferencesSchema.string());
            }
            return 0;
        }
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
            std::puts("Add this folder to SHOCKWAVE_EXTRA_GAMES, then build player/editor before Play. See the project's "
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
        if (!tool.level.empty()) player::applyStartLevel(project, tool.level);
        if (!shockwave::projectGraphicsSettings(project, tool.graphicsDefaults, error)) throw std::runtime_error(error);
        tool.graphics = tool.graphicsDefaults;
        if (releaseTier) return player::releaseTierInfo(project, tool.benchmarkJson);
        {
            // Where this run writes. A run that writes (play, save) needs it; tools that
            // only read carry on without, and report it.
            shockwave::UserPaths paths;
            std::string pathsError;
            const bool resolved = player::resolveUserPaths(project, userPathsCommand, paths, pathsError);
            if (resolved) tool.userPaths = paths;
            if (userPathsCommand.info)
                return player::userPathsInfo(
                    project, userPathsCommand, resolved ? &paths : nullptr, pathsError, tool.benchmarkJson);
            if (!resolved) {
                if (!userPathsCommand.importLegacy.empty() ||
                    (tool.mode == Tool::Mode::Play && !package.requested && !tool.listSaves))
                    throw std::runtime_error(pathsError);
                SW_LOG(shockwave::LogEngine, Warning, "No per-user folder: {}", pathsError);
            }
            if (!userPathsCommand.importLegacy.empty())
                return player::importLegacySaves(userPathsCommand.importLegacy, paths, userPathsCommand.preview);
        }
        if (diagnostics.info)
            return player::diagnosticsInfo(
                project, tool.userPaths ? &*tool.userPaths : nullptr, diagnostics, tool.benchmarkJson);
        {
            // A player playing (not a tool, a measurement or an automated test) keeps a log
            // file and the session marker; every run gets crash reports.
            const bool gameRun = tool.mode == Tool::Mode::Play && !tool.optimise && tool.benchmarkSeconds == 0 &&
                                 tool.screenshot.empty() && tool.graphicsReportFrame == 0 && !package.requested &&
                                 !tool.captureReflections && !tool.listSaves && saveInfoFile.empty() &&
                                 saveMigrateFile.empty() && !netTest.requested && !networkInfo &&
                                 networkSchema.empty() && steamStatus == 0 && !releaseInfo && tool.save.empty() &&
                                 tool.flowTest.empty() && !tool.perfRun;
            player::beginRunDiagnostics(project, tool.userPaths ? &*tool.userPaths : nullptr, diagnostics, gameRun);
            runDiagnostics.begun = true;
            // A last run that died before showing a frame, or lost its GPU: offered safe
            // graphics now, before a window opens, since a menu may never be reached.
            if (gameRun && !diagnostics.safeMode && !diagnostics.noRecoveryPrompt && player::needsStartupRecovery() &&
                player::askSafeModeAtStart())
                diagnostics.safeMode = true;
        }
        tool.recoveryPrompt = !diagnostics.noRecoveryPrompt;
        tool.injectDeviceLostFrame = diagnostics.injectDeviceLost;
        if (tool.injectDeviceLostFrame && !player::packagedProjectFile().empty())
            throw player::UsageError("--inject-device-lost is a developer test; a packaged game doesn't take it");
        // Reproducible tools ignore local user preferences. Explicit --graphics still
        // loads (and --check validates) the file, without a window or GPU.
        if (!graphicsFile.empty()) {
            tool.graphicsFile = std::filesystem::absolute(graphicsFile);
            if (!shockwave::loadGraphicsSettings(tool.graphicsFile, tool.graphics, error))
                throw std::runtime_error(error);
        } else if (
            !noUserGraphics && !diagnostics.safeMode && tool.mode == Tool::Mode::Play && !tool.optimise &&
            tool.benchmarkSeconds == 0 && tool.screenshot.empty() && tool.graphicsReportFrame == 0 &&
            !package.requested && !tool.captureReflections) {
            // The player's own settings, in their profile's config folder.
            if (tool.userPaths) {
                tool.graphicsFile = tool.userPaths->config() / "Graphics.swgraphics";
                if (std::filesystem::exists(tool.graphicsFile) &&
                    !shockwave::loadGraphicsSettings(tool.graphicsFile, tool.graphics, error))
                    SW_LOG(shockwave::LogEngine, Warning, "Ignoring user graphics settings: {}", error);
            }
        }
        tool.safeMode = diagnostics.safeMode;
        if (diagnostics.safeMode) {
            // Safe graphics (docs/Diagnostics.md): the lowest tier, windowed, no pipeline
            // cache. The player's own files are neither read nor written; anything given on
            // the command line still applies on top.
            tool.graphics = player::safeGraphics(project, tool.graphics);
            tool.graphicsFile.clear();
            tool.preferenceSets.emplace_back("window_mode", "windowed");
            noPipelineCache = true;
            noPipelineWarmup = true;
            SW_LOG(shockwave::LogEngine, Warning, "Safe mode: lowest graphics, windowed, no pipeline cache");
        }
        if (!graphicsPresetArg.empty()) {
            // Before the other overrides, so --graphics-set can change one setting of a preset.
            bool found = false;
            for (auto preset :
                 {shockwave::GraphicsPreset::Low, shockwave::GraphicsPreset::Medium, shockwave::GraphicsPreset::High,
                  shockwave::GraphicsPreset::Ultra})
                if (graphicsPresetArg == shockwave::graphicsPresetName(preset)) {
                    tool.graphics = shockwave::graphicsPreset(preset, tool.graphics);
                    found = true;
                }
            if (graphicsPresetArg == "fallback") {
                // The lowest tier the game supports (Config/ReleaseTier.swtier).
                const shockwave::ReleaseTierReport tier = shockwave::releaseTierReport(project);
                if (!tier.readable)
                    throw std::runtime_error(
                        "--graphics-preset fallback needs a " + tier.file + " that reads (docs/ReleaseTier.md)");
                tool.graphics = shockwave::fallbackTierSettings(tier.tier, tool.graphics);
                found = true;
            }
            if (!found) throw std::runtime_error("--graphics-preset takes low, medium, high, ultra or fallback");
        }
        if (fpsOverride >= 0) tool.graphics.frameLimit = fpsOverride;
        if (vsyncOverride >= 0) tool.graphics.vsync = vsyncOverride == 1;
        if (lowLatency) tool.graphics.lowLatency = true;
        if (tool.shadows >= 0) tool.graphics.shadows = static_cast<shockwave::ShadowQuality>(tool.shadows);
        if (renderScaleOverride >= 0) tool.graphics.renderScale = renderScaleOverride;
        for (const auto& [key, value] : graphicsSets)
            if (!shockwave::setGraphicsSetting(tool.graphics, key, value, error))
                throw std::runtime_error("--graphics-set: " + error);
        {
            const auto preset = shockwave::matchingGraphicsPreset(tool.graphics);
            shockwave::diagnostics::setContext("graphics", preset ? shockwave::graphicsPresetName(*preset) : "custom");
        }
        // The player's preferences file, under the same rule as the graphics one: tools and
        // reproducible runs leave it alone.
        if (!noUserPreferences && tool.mode == Tool::Mode::Play && !tool.optimise && tool.benchmarkSeconds == 0 &&
            tool.screenshot.empty() && tool.graphicsReportFrame == 0 && !package.requested &&
            !tool.captureReflections && tool.userPaths)
            tool.preferencesFile = tool.userPaths->config() / "Preferences.swprefs";
        if (audioVolumeSet) tool.preferenceSets.emplace_back("master_volume", std::to_string(audioVolume));
        if (preferencesInfo) {
            // Every preference, its value and the layer it came from (docs/Preferences.md).
            if (!tool.userPaths) throw std::runtime_error("--preferences needs a profile folder (--user-dir)");
            const auto profile =
                noUserPreferences ? std::filesystem::path() : tool.userPaths->config() / "Preferences.swprefs";
            return player::preferencesInfo(project, profile, tool.preferenceSets, tool.benchmarkJson);
        }
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
        if (tool.listSaves || !saveInfoFile.empty()) {   // only read files
            const auto identity = player::saveIdentityFor(project, tool.userPaths ? &*tool.userPaths : nullptr);
            if (!saveInfoFile.empty()) return player::saveInfo(project, saveInfoFile, identity, tool.benchmarkJson);
            return player::listSaves(
                project, tool.userPaths ? tool.userPaths->saves() : std::filesystem::path(), identity,
                tool.benchmarkJson);
        }
        // --release-info loads every level's scene to follow the links between them, so it
        // runs with the game module, like --check (no window or GPU).
        if (releaseInfo) tool.mode = Tool::Mode::ReleaseInfo;
        // The project's game code, before any window: a missing module fails fast. It
        // outlives the engine and the player, which hold what it made.
        if (package.requested) return player::packageGame(project, package);
        auto gameModule = gameModuleFile.empty() ? shockwave::GameModuleLibrary::load(project.gameModule, error)
                                                 : shockwave::GameModuleLibrary::loadFile(gameModuleFile, error);
        if (!gameModule)
            throw std::runtime_error(
                "the project's game module \"" + project.gameModule + "\" couldn't load: " + error +
                (!player::packagedProjectFile().empty()
                     ? std::string(". The game's files are incomplete: install it again")   // players can't build
                     : ". Build it with: cmake --build build --target game_" + project.gameModule));
        if (gameModule->id() != project.gameModule)
            throw std::runtime_error(
                "the game module is \"" + std::string(gameModule->id()) + "\" but the project wants \"" +
                project.gameModule + "\"");
        // Multiplayer tools read files and the game's registered types; no window or GPU.
        if (networkInfo) return player::networkInfo(project, gameModule->module(), tool.benchmarkJson);
        if (!networkSchema.empty()) return player::writeNetworkSchema(gameModule->module(), networkSchema);
        if (!saveMigrateFile.empty()) {
            // The game's own migrations: its GameInstance, without a window or Play.
            const auto instance = shockwave::makeGameInstance(&gameModule->module());
            auto identity = player::saveIdentityFor(project, tool.userPaths ? &*tool.userPaths : nullptr);
            identity.migrate = [&instance](int from, shockwave::SaveInfo& save, std::string& why) {
                return instance->migrateSave(from, save, why);
            };
            return player::saveMigrate(project, saveMigrateFile, output, identity);
        }
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
        if (!tool.flowTest.empty() && !tool.perfRun) {
            // Hidden and reproducible; the scenario's timeouts end it.
            config.window.hidden = true;
            config.window.vsync = false;
            config.simulationTime = shockwave::SimulationTime::Fixed;
        }
        // --perf-run measures what a player gets: a shown window in real time, with the
        // project's vsync and frame limit unless --vsync or --fps says otherwise.
        if (tool.mode != Tool::Mode::Play || tool.optimise) {
            config.window.hidden = true;
            config.window.vsync = false;
        }
        // Tools only read files, so they need no GPU unless asked for one.
        config.renderer = !renderer.empty()                                ? renderer
                          : tool.mode != Tool::Mode::Play || tool.optimise ? std::string("null")
                                                                           : project.renderer;
        config.pipelineCache = !noPipelineCache;
        config.pipelineWarmup = project.pipelineWarmup && !noPipelineWarmup;
        if (tool.userPaths) {
            config.pipelineCacheFolder = (tool.userPaths->cache() / "pipeline-cache").string() + "/";
            config.guiSettingsFile = (tool.userPaths->config() / "imgui.ini").string();
        }
        if (clearPipelineCache) {
            const std::string folder = !config.pipelineCacheFolder.empty()
                                           ? config.pipelineCacheFolder
                                           : shockwave::Engine::pipelineCacheFolder(config.window.title);
            std::error_code ec;
            if (!folder.empty()) std::filesystem::remove_all(folder, ec);
            if (ec) throw std::runtime_error("couldn't delete " + folder + ": " + ec.message());
        }
        shockwave::Engine engine(config);
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
