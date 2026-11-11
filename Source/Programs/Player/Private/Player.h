#pragma once

// The player: runs a project like the shipped game, or one of the command-line tools
// (--check, --schema, --input-schema, --devices, --screenshot, --benchmark, --bake-navmesh) that people, CI and AI
// agents use to verify it.

#include <Assets/AssetFolder.h>
#include <Engine/BakedLighting.h>
#include <Engine/ReflectionProbes.h>
#include <Engine/Engine.h>
#include <GameFramework/AppShell.h>
#include <GameFramework/FlowTest.h>
#include <GameFramework/GameModule.h>
#include <GameFramework/LevelAssets.h>
#include <GameFramework/LevelHost.h>
#include <GameFramework/PlaySession.h>
#include <Effects/Effects.h>
#include <Engine/PostProcess.h>
#include <GameUI/GameUI.h>
#include <Multiplayer/Replication.h>
#include <Network/Loopback.h>
#include <Network/LoopbackServices.h>
#include <Multiplayer/OnlineMatch.h>
#include <Network/NetworkCapture.h>
#include <Network/OnlineServices.h>
#include <SteamServices/SteamServices.h>
#include <Navigation/Navigation.h>
#include <Platform/UserPaths.h>
#include <Packaging/Cook.h>
#include <Projects/LevelCatalog.h>
#include <Projects/Project.h>
#include <Projects/ReleaseSettings.h>

#include <array>
#include <map>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace player {

namespace fs = std::filesystem;

// What the player was asked to do. Tools run in a hidden window and quit before the
// first frame; they need the renderer only to import assets the way the game does.
struct Tool {
    enum class Mode {
        Play,
        Check,
        Schema,
        BakeNavMesh,
        AudioMix,
        Animations,
        NodeSchema,
        CircuitRun,
        CircuitDump,
        NetworkManifest,
        AssignNetworkKeys,
        LightingInfo,
        LightingAssignIds,
        BakeLighting,
        Levels,   // --levels: the level catalog and every level's links (docs/Levels.md)
        ReleaseInfo,   // --release-info, which loads every level to follow its links
        AssetDependencies,   // --asset-dependencies [--level id] [--json out] (docs/AssetResidency.md)
        Cook   // --cook [--target t] [--release] [--json report] [--output file] (docs/Cooking.md)
    } mode = Mode::Play;
    // Cooking (PlayerCook.cpp): --target, and --release for --cook and --check.
    std::string cookTarget;
    bool release = false;
    std::vector<fs::path> scenes;   // --check, --bake-navmesh: empty means every scene in the project
    fs::path schemaOut;   // --schema, --node-schema: "-" is standard output
    std::string circuitMessage;
    int circuitTicks = 1, circuitSteps = 0;
    std::string circuitBreakpoint;
    std::string circuit;   // --circuit file --dump: a .swcircuit, inside Content or a path to one
    std::vector<std::string> animationModels;   // --animations: models inside Content; empty: all
    // Save games (docs/SaveGames.md).
    bool listSaves = false;   // --saves, with --json for a file
    std::string load;   // --load: a slot name or a .swsave file, loaded as Play starts
    std::string save;   // --save: play --frames N (default 30), save to this slot, quit
    int saveFrame = 30;
    // Playing, these make runs reproducible and checkable without a person watching.
    fs::path scene;   // --scene: instead of the project's startup scene
    std::string level;   // --level: start in this catalog level (docs/Levels.md)
    fs::path screenshot;   // --screenshot: save frame N as a PNG, then quit
    bool optimise = false;   // --optimise: compress every texture now, report (with --json), quit
    bool lodColors = false;   // --lod-colors: tint each level of detail its own colour
    int screenshotFrame = 30;   // --frames with --screenshot
    std::string camera;   // --camera: look through this named Camera entity instead of the primary one
    bool fixedStep = false;   // --fixed-step: every frame advances exactly one tick (reproducible motion)
    shockwave::DebugView view = shockwave::DebugView::Lit;   // --view
    bool showNavMesh = false;   // --view navmesh: the lit scene with the navigation mesh and agents' paths
    shockwave::GraphicsSettings graphics, graphicsDefaults;
    fs::path graphicsFile;
    // Player preferences (docs/Preferences.md): the profile's file (empty: not read or
    // saved) and --pref overrides for this run.
    fs::path preferencesFile;
    std::vector<std::pair<std::string, std::string>> preferenceSets;
    // Where this run writes (PlayerUserPaths.cpp, docs/UserPaths.md): saves, settings,
    // caches. Empty when no per-user folder could be found; tools then only read.
    std::optional<shockwave::UserPaths> userPaths;
    int shadows = -1;   // --shadows: a ShadowQuality; -1 uses the project's shadow_quality
    bool printFrame = false;   // --print-frame: the render graph, at exit
    double benchmarkSeconds = 0;   // --benchmark: measure this long after a warm-up, report, quit
    fs::path benchmarkJson;   // --json with --benchmark
    int graphicsReportFrame = 0;   // --graphics-report: report after this many frames, then quit
    bool uiPreview = false;   // --ui-preview: unset {{values}} show UIDocument preview values
    std::vector<shockwave::UINav> uiNav;   // --ui-nav: menu steps, one a frame from frame 5
    fs::path audioMix;   // --audio-mix: render the scene's sound offline to this .wav
    double audioSeconds = 3;   // --seconds with --audio-mix
    int audioHrtf = -1;   // --audio-hrtf on|off; -1 uses the project's audio_hrtf
    // --net-preview [preset]: play the scene as a client of a host in this process, over
    // a loopback hub with the fault preset's latency, jitter and loss (default poor).
    std::string netPreview;
    // Network tools: --network-manifest --output file; --assign-network-keys --scene file
    // --output file [--seed N] (a seed makes the new keys the same on every run).
    fs::path output;
    bool keySeedGiven = false;
    uint64_t keySeed = 0;
    // Online play (PlayerNetwork.cpp, docs/Networking.md): --host, --join-lobby id,
    // --join-steam id --dev-direct-join, --host --dev-direct-join; +connect_lobby id is a
    // Steam invite's cold launch. --net-capture records the session's messages.
    enum class Online { None, Host, HostDirect, JoinLobby, JoinDirect } online = Online::None;
    uint64_t onlineId = 0;   // the lobby, or the host's Steam ID
    uint64_t connectLobby = 0;
    fs::path netCapture;
    // Baked lighting (PlayerLighting.cpp, docs/Lighting.md): --bake-lighting --quality
    // preview|final [--seed N], --lighting-info and --lighting-assign-ids, for --scene or
    // the startup scene.
    std::string bakeQuality = "preview";
    // --capture-reflections (PlayerLighting.cpp): the scene's reflection probes, captured on
    // the GPU in a hidden window, then quit.
    bool captureReflections = false;
    // Asset residency (PlayerAssets.cpp, docs/AssetResidency.md): --no-asset-residency
    // turns level leases off; --asset-budget-cpu/-gpu MiB refuse a level that won't fit;
    // --load-report out.json writes each level load's phases and assets at exit.
    bool assetResidency = true;
    shockwave::AssetBudget assetBudget;
    fs::path loadReport;
    // --asset-loading all|levels: the project's asset_loading, overridden. Empty: the project's.
    std::string assetLoading;
    // --instant-travel: the project's level_travel as instant (docs/LevelLifetime.md).
    bool instantTravel = false;
    // --app-shell off|all|menu pause ...: the project's app_shell, overridden (docs/AppShell.md).
    std::string appShell;
    // --flow-test scenario.json [--json report]: run the scenario through the shell and the
    // levels, write the report, exit 1 at the first failed step (docs/FlowTest.md).
    fs::path flowTest;
    // --perf-run scenario.json [--laps N] [--perf-record "machine"] [--cold-cache] [--json
    // report]: the scenario in real time, measured; play and loading apart (S10,
    // PlayerPerformance.cpp, docs/Performance.md). Sets flowTest too.
    bool perfRun = false;
    uint32_t perfLaps = 1;
    std::string perfRecord;   // the machine's name; writes Release/Performance/<name>.swperf
    fs::path perfRecordFolder;   // --output with --perf-record; empty: the project's Release/Performance
    bool coldCache = false;   // --cold-cache: the pipeline cache was emptied before the run
};

// Bad command-line arguments: the player exits with code 2 (1 is a failure at run time).
struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Tools that need no project or window. Both return the process exit code.
int writeInputSchema(const fs::path& out);   // docs/InputSchema.md; "-" for stdout
// Lists connected controllers, then for `seconds` prints each input pressed as the
// binding text that would bind it ("pad south", "joy \"G29\" axis 2 full").
int listDevices(double seconds);
// --saves: the project's save games, newest first, damaged ones too, with why; json, when
// set, gets them as JSON too.
int listSaves(
    const shockwave::Project& project, const fs::path& folder, const shockwave::SaveIdentity& identity,
    const fs::path& json);
// The game and profile this run's saves belong to (docs/SaveGames.md#compatibility),
// without the game's migrations.
shockwave::SaveIdentity saveIdentityFor(const shockwave::Project& project, const shockwave::UserPaths* paths);
// --save-info: one save's envelope, and whether this game and profile can load it.
int saveInfo(
    const shockwave::Project& project, const fs::path& file, const shockwave::SaveIdentity& identity,
    const fs::path& json);
// --save-migrate: a copy of the save at the format and schema this game writes now, with
// the game's migrations run. Never writes over the original.
int saveMigrate(
    const shockwave::Project& project, const fs::path& file, const fs::path& output,
    const shockwave::SaveIdentity& identity);

// --package: makes a game folder for players (Packaging/Package.h). --output, --no-build
// and --no-archive override Config/Package.swpackage for this run.
struct PackageCommand {
    bool requested = false;
    fs::path output;
    bool noBuild = false, noArchive = false;
    bool development = false;   // --dev-package: a test build (allows Steam AppID 480)
    bool release = false;   // --release: only what the cook says the game needs (docs/Cooking.md)
};
int packageGame(const shockwave::Project& project, const PackageCommand& command);
// In a packaged game, the project it ships with; empty otherwise.
fs::path packagedProjectFile();
// --check: problems in Config/Package.swpackage, when the project has one.
void checkPackageSettings(const shockwave::Project& project, std::vector<std::string>& problems);
// Per-user folders (PlayerUserPaths.cpp, docs/UserPaths.md). The application ID is the
// release contract's application_id, or "shockwave.<game module>" without one. An installed
// game writes to the OS's per-user folder; a project run from source writes to its own
// Saved/ folder; --user-dir picks a folder for this run (tests and CI always do).
struct UserPathsCommand {
    fs::path userDir;   // --user-dir
    std::string profile = "default";   // --profile
    bool info = false;   // --user-paths [--json file]
    fs::path importLegacy;   // --import-legacy-saves folder
    bool preview = false;   // --preview: what an import would do, copying nothing
};
std::string applicationIdFor(const shockwave::Project& project);
bool resolveUserPaths(
    const shockwave::Project& project, const UserPathsCommand& command, shockwave::UserPaths& out, std::string& error);
int userPathsInfo(
    const shockwave::Project& project, const UserPathsCommand& command, const shockwave::UserPaths* paths,
    const std::string& error, const fs::path& json);
// Copies legacy save games (a project's old Saved/SaveGames, say) into the profile's saves:
// readable .swsave files only, never over a slot that's already there. Exit 1 if any failed.
int importLegacySaves(const fs::path& from, const shockwave::UserPaths& paths, bool preview);
// The release contract (PlayerRelease.cpp, docs/Release.md). --release-info prints the
// contract and every release gate worked out from the project (with --json, as JSON); exit 1
// when the contract has errors. --release-schema writes docs/ReleaseSchema.md ("-" for
// stdout). --check reports the contract's problems, against the project's other settings.
// The release inputs from the project's files; `levelFacts` (each level's scene, read by
// Player::levelFacts) lets the level_catalog gate follow the links between levels too.
shockwave::ReleaseInputs releaseInputs(
    const shockwave::Project& project, const std::map<std::string, shockwave::LevelSceneFacts>* levelFacts = nullptr);
int releaseInfo(
    const shockwave::Project& project, const fs::path& json,
    const std::map<std::string, shockwave::LevelSceneFacts>* levelFacts = nullptr);
// --release-tier [--json]: the graphics release tier (docs/ReleaseTier.md); 1 when it has errors.
int releaseTierInfo(const shockwave::Project& project, const fs::path& json);
int releaseSchema(const fs::path& file);
// The game's build now, as a cook names it (16 hex digits): a package's Cook.swcook, or the
// project's latest cook for this platform. "" without one. Performance records name it.
std::string currentBuildId(const shockwave::Project& project);
// --cook-info file [--verify folder] [--json out]: a cook manifest, and its files checked.
int cookInfo(const fs::path& file, const fs::path& verifyRoot, const fs::path& json);
int levelSchema(const fs::path& file);
// --preferences: every player preference, its value and the layer it came from
// (PlayerPreferences.cpp, docs/Preferences.md); with --json, as JSON.
int preferencesInfo(
    const shockwave::Project& project, const fs::path& profile,
    const std::vector<std::pair<std::string, std::string>>& session, const fs::path& json);
// --check: the project's Config/Preferences.swprefs.
void checkPreferences(const shockwave::Project& project, std::vector<std::string>& problems);
// --check: Content/Localization's catalogs, and every {{text.key}} the documents show.
void checkLocalization(const shockwave::Project& project, std::vector<std::string>& problems);
// --level: the catalog level's scene becomes the startup scene. Throws with the levels
// there are when the id isn't one.
void applyStartLevel(shockwave::Project& project, const std::string& level);
void checkRelease(
    const shockwave::Project& project, std::vector<std::string>& problems,
    const std::map<std::string, shockwave::LevelSceneFacts>* levelFacts = nullptr);
// Multiplayer (PlayerNetwork.cpp, docs/Networking.md). --network-schema writes
// docs/NetworkSchema.md for the game's registered types ("-" for stdout); --network-info
// prints the project's network settings, digests and problems (with --json, as JSON).
// Neither starts Steam or opens a socket.
int writeNetworkSchema(const shockwave::GameModule& module, const fs::path& out);
int networkInfo(const shockwave::Project& project, const shockwave::GameModule& module, const fs::path& json);
// --steam-status [client|server]: start Steam for the project's steam_app_id as a client
// (or a dedicated server), wait for sign-in and relay network access, print one line, and
// shut Steam down. Exit 1 when Steam isn't available, as in builds without the SDK.
int steamStatus(const shockwave::Project& project, bool server);
// --net-test --scenario name [--seed N] [--preset name] [--clients N] [--json file]: the
// session handshake between NetworkWorlds over an in-process loopback hub
// (Multiplayer/NetTest.h), with the project's network settings when it has them. No
// Steam, sockets, window or game code. Exit 1 when the scenario fails.
struct NetTestCommand {
    bool requested = false;
    std::string scenario = "handshake", preset = "cafe";
    uint64_t seed = 1;
    uint32_t clients = 3;
    uint32_t rotations = 3;   // map_rotation
    // --net-set key=value: network settings for this run over the project's (or the
    // defaults), such as max_players 64 for a 63-client match.
    std::vector<std::string> sets;
};
int netTest(const shockwave::Project& project, const NetTestCommand& command, const fs::path& json);
// --net-dump capture.swnetcap [--json out.json]: the capture decoded, every record with its
// message's fields (stdout without --json). --net-encode records.json --output out.swnetcap:
// the reverse, refusing redacted payloads. Neither needs a project.
int netDump(const fs::path& capture, const fs::path& json);
int netEncode(const fs::path& json, const fs::path& output);
// --image-stats file.png [--json out]: size and mean colour. --image-diff reference.png
// candidate.png [--tolerance rmse] [--json out]: how far apart two screenshots are (RMSE in
// 0..255 steps, PSNR, largest channel difference, share of visibly changed pixels). Exit 1
// when the sizes differ or the RMSE is over the tolerance (default 1). No project needed.
// region: x, y, width, height as fractions of the image (0..1); the whole image by default.
int imageStats(const fs::path& image, const fs::path& json, const std::array<double, 4>& region = {0, 0, 1, 1});
// region: compare only that part of both (fractions x, y, width, height), as --image-stats.
int imageDiff(
    const fs::path& reference, const fs::path& candidate, double tolerance, const fs::path& json,
    const std::array<double, 4>& region = {0, 0, 1, 1});
// --material-info, --texture-info: an asset's material slots (UV sets, transforms,
// samplers) or its textures' levels and size at each texture_quality, as JSON. No GPU.
int materialInfo(const fs::path& asset, const fs::path& json);
// --effect-info (PlayerEffects.cpp): an effect file's parts and how each is drawn (A04).
int effectInfo(const fs::path& effect, const fs::path& json);
// Post-processing (PlayerPostProcess.cpp, A05/A06), no project needed: --postprocess-schema
// writes docs/PostProcessSchema.md ("-": stdout); --postprocess-info reports a profile with
// its parents applied and its LUT (JSON with --json); --lut-export-neutral writes an
// identity .cube of 17, 33 or 65 entries a side.
int postProcessSchema(const fs::path& file);
int postProcessInfo(const fs::path& profile, const fs::path& json);
int lutExportNeutral(uint32_t size, const fs::path& output);
// --material-schema: the .swmaterial reference, docs/MaterialSchema.md ("-": stdout).
int materialSchema(const fs::path& file);
int textureInfo(const fs::path& asset, const fs::path& json);
// Baked lighting payloads (PlayerLighting.cpp), no project needed: --lighting-schema writes
// docs/LightingSchema.md ("-": stdout); --lighting-export turns a .ktx2 lightmap, .swbake
// or .swprobe into JSON; --lighting-import encodes such JSON back, by --output's extension.
int lightingSchema(const fs::path& file);
int lightingExport(const fs::path& asset, const fs::path& output);
int lightingImport(const fs::path& json, const fs::path& output);
// --graphics-info --json: the settings this run would use, the matching preset, and every
// setting's description (key, label, values, how it applies), for tools and menus.
std::string graphicsInfoJson(const shockwave::GraphicsSettings& settings, const fs::path& userFile);
// A packaged Steam game launched outside Steam: asks Steam to relaunch it (and the player
// exits). False for everything else: tools, the editor, development builds.
bool relaunchThroughSteam(const shockwave::Project& project);

class Player final : public shockwave::Application {
public:
    // Both in PlayerPerformance.cpp, where PerfState is complete.
    Player(shockwave::Project project, Tool tool, const shockwave::GameModule& module);
    ~Player() override;
    int exitCode() const { return m_exitCode; }

    void onStart() override;
    void onPump(double rawFrameDt) override;
    void onUpdate(double dt) override;
    void onFrame(double dt) override;
    void onRender(shockwave::Renderer& renderer, double alpha) override;
    void onShutdown() override;

private:
    // Asks for frame N, then reports and quits once the renderer has written it.
    void takeScreenshot();
    // Asset optimisation (docs/Optimisation.md): the project's settings, before importing;
    // and --optimise, which compresses every texture into the cache and reports.
    void applyOptimisationSettings();
    int optimise();
    // --benchmark: warms up, measures, prints the frame statistics and quits.
    void benchmark(double dt);
    void reportBenchmark();
    // --save: saves to the slot, waits for the file, reports and quits.
    void saveAndQuit();
    // --graphics-report: capabilities, requested and effective settings, memory by category.
    void graphicsReport();
    fs::path saveFolder() const { return m_tool.userPaths ? m_tool.userPaths->saves() : fs::path(); }
    // The game and profile its saves belong to, its save schema and migrations.
    shockwave::SaveIdentity saveIdentity();

    // Loads each scene the way the game would and reports every problem as file:line.
    int check(const std::vector<std::string>& importProblems);
    int levels();   // --levels [--json]
    // Asset dependencies and residency (PlayerAssets.cpp, docs/AssetResidency.md).
    shockwave::AssetGraph& assetGraph();
    int assetDependencies();   // --asset-dependencies [--level id] [--json out]
    void checkAssetGraph(const std::vector<fs::path>& scenes, std::vector<std::string>& problems);
    shockwave::LevelHost::Hooks withAssetLeases(shockwave::LevelHost::Hooks hooks);
    void recordLoad(const shockwave::LoadReport& report);
    // asset_loading levels (docs/AssetResidency.md): each level's assets load when it needs them.
    bool loadAssetsOnDemand() const;
    // On demand: a lease on what a scene (or the game circuit) needs, loaded, so the scene
    // can be read before its level takes its own lease. Empty without on-demand loading.
    shockwave::AssetResidency::Lease sceneAssets(const std::string& owner, const fs::path& file);
    void writeLoadReport();   // --load-report
    int releaseInfoTool();   // --release-info [--json]
    // Cooking (PlayerCook.cpp, docs/Cooking.md).
    bool cookManifest(
        bool release, shockwave::CookManifest& manifest, std::vector<std::string>& errors,
        std::vector<std::string>& warnings);
    int cook();   // --cook
    void checkCookRelease(std::vector<std::string>& problems);   // --check --release
    // Loads each level's scene once, in an isolated world, and reads what it says.
    std::map<std::string, shockwave::LevelSceneFacts> levelFacts(const shockwave::LevelCatalog& catalog);

    int schema();
    // --check for prefabs: problems loading them, and parts that don't belong in one.
    void checkPrefabs(std::vector<std::string> prefabs, std::vector<std::string>& problems);
    // --animations and --check for skeletal animation (PlayerAnimation.cpp).
    int listAnimations();
    void checkAnimators(shockwave::World& world, const std::string& scene, std::vector<std::string>& problems);
    // --check for game UI: a scene's UIDocuments, then every .rml in Content.
    void checkUIDocuments(
        shockwave::World& world, const std::string& scene, std::vector<std::string>& problems,
        std::vector<std::string>& checkedDocuments);
    // --bake-navmesh: bakes each scene's navigation mesh, saves it next to the scene and
    // prints its size, bake time and what its agents cost per tick (PlayerNavigation.cpp).
    int bakeNavMeshes();
    std::vector<fs::path> projectScenes() const;
    // --audio-mix: plays the scene with an offline mixer, writes a .wav and prints levels
    // per channel and what each spatial source sounds like (PlayerAudio.cpp).
    int audioMix();

    // Circuits (PlayerCircuits.cpp): --check compiles every .swcircuit in Content and checks
    // the scenes' Circuit components; --node-schema writes the node reference; --circuit
    // file --dump prints a circuit's bytecode.
    void checkCircuits(
        shockwave::World& world, const std::string& scene, std::vector<std::string>& problems,
        std::vector<std::string>& usedCircuits);
    // With the level catalog, a Level.RequestTravel's literal level (and, with the levels'
    // scene facts, its spawn) must be one there.
    void checkAllCircuits(
        const std::vector<std::string>& usedCircuits, std::vector<std::string>& problems,
        const shockwave::LevelCatalog* catalog = nullptr,
        const std::map<std::string, shockwave::LevelSceneFacts>* levelFacts = nullptr);
    int nodeSchema();
    int dumpCircuit();
    int runCircuit();

    // --check for particle effects (PlayerEffects.cpp): a scene's ParticleEffects, then
    // every .efkefc in Content. Warnings (parts that aren't drawn yet) don't fail it.
    void checkParticleEffects(
        shockwave::World& world, const std::string& scene, std::vector<std::string>& problems,
        std::vector<std::string>& checkedEffects);
    void checkUnusedEffects(const std::vector<std::string>& checkedEffects, std::vector<std::string>& problems);
    // --check for materials (PlayerMaterials.cpp, A02): each ModelRenderer's per-part
    // overrides name parts the model has; every .swmaterial resolves (the import already
    // reported those that don't), and its warnings are printed without failing the check.
    void checkMaterialOverrides(shockwave::World& world, const std::string& scene, std::vector<std::string>& problems);
    void checkMaterialFiles();
    // --check for colour grading, exposure and fog (PlayerPostProcess.cpp, A05/A06): each
    // PostProcessVolume's profile and LUT resolve, then every .swpostprocess in Content.
    void checkPostProcessVolumes(
        shockwave::World& world, const std::string& shown, std::vector<std::string>& problems,
        std::vector<std::string>& checkedProfiles);
    void checkPostProcessFiles(const std::vector<std::string>& checkedProfiles, std::vector<std::string>& problems);
    // Baked lighting (PlayerLighting.cpp, G05/G06): --check's id and bake rules, and the
    // scene tools. A damaged bake fails the check; a missing or out-of-date one warns.
    void checkLighting(const shockwave::World& world, const std::string& scene, std::vector<std::string>& problems);
    fs::path lightingScene() const;   // --scene, else the startup scene
    int lightingInfo();
    int lightingAssignIds();
    int bakeLightingTool();
    // --capture-reflections: starts the capture job on the loaded scene; each frame drives
    // it, and it reports and quits when it's done.
    void startReflectionCapture();
    bool driveReflectionCapture(shockwave::Renderer& renderer);

    // --check for multiplayer (PlayerNetwork.cpp): Config/Network.swnetwork, the network
    // manifest, its prefabs and the registered types, then each scene's network objects.
    void checkNetworkProject(std::vector<std::string>& problems);
    void checkNetworkScene(const shockwave::World& world, const std::string& scene, std::vector<std::string>& problems);
    // --net-preview (PlayerNetwork.cpp): makes the in-process host and starts it, and
    // sets the session up as its client. Throws when the project can't play online.
    void startNetPreview();
    // Online play (PlayerNetwork.cpp): the match setup and its services, the command
    // line's request, and each frame what the match asks for: play its scene as host or
    // client, or go back to the offline scene when it ends.
    void setUpOnline();
    void updateOnline();
    void playScene(const fs::path& scene, const shockwave::net::NetworkStart* start);
    // ---- Levels (docs/LevelLifetime.md) ----
    // The level that's in m_world starts playing as `level`.
    void startLevels(const std::string& level, shockwave::StartReason reason);
    shockwave::LevelHost::Hooks levelHooks();
    // The catalog's level for a scene ("startup" for the implicit one, or a scene the
    // catalog doesn't list, from --scene).
    std::string levelForScene(const fs::path& scene);
    void loadLevelCatalog();   // once
    fs::path sceneForLevel(const std::string& level) const;
    // After the frame: carries out travel; a level that fails to start goes back to the entry
    // (or, with the shell's error screen, stays on it with Retry and Main menu).
    void processTravel();
    // The application's screens over the levels (docs/AppShell.md), once levels have started.
    void startShell();
    // The shell's Continue: why it can't ("" when it can), and the request it sends.
    std::string continueReason();
    bool continueRequest(shockwave::TravelRequest& request, std::string& why);
    // --flow-test: loads the scenario (throws a UsageError when it's bad), then each frame
    // runs it; writes the report and quits when it ends.
    void startFlowTest();
    void driveFlowTest();
    void endFlowTest();
    // --perf-run (PlayerPerformance.cpp): each frame files the last frame's sample under
    // loading, warm-up or a level's play; at the end prints the report and writes the
    // JSON and the record.
    void startPerformance(size_t lapSteps);
    void samplePerformance();
    void endPerformance(bool passed);
    struct PerfState;
    std::unique_ptr<PerfState> m_perf;
    shockwave::PlaySession* session() const { return m_levels ? m_levels->session() : nullptr; }
    shockwave::net::IOnlineServices* startSteam(std::string& error);
    shockwave::net::IOnlineServices* services() const {
        return m_steam ? static_cast<shockwave::net::IOnlineServices*>(m_steam.get()) : m_services.get();
    }
    // --network-manifest and --assign-network-keys (PlayerNetwork.cpp).
    int networkManifest();
    int assignNetworkKeysTool();

    shockwave::Project m_project;
    Tool m_tool;
    const shockwave::GameModule& m_module;
    int m_exitCode = 0;
    int m_frame = 0;
    size_t m_uiNavFrame = 0;
    bool m_screenshotRequested = false;
    double m_benchmarkTime = 0;
    int m_saveFrames = 0;   // frames played, for --save
    int m_reportFrames = 0;   // frames played, for --graphics-report
    // Pipelines the renderer had made before the first frame drew (S10): any made after
    // were built while drawing. -1 until that frame.
    int64_t m_pipelinesBeforeFrames = -1;
    uint64_t m_warmedBeforeFrames = 0;
    bool m_benchmarkMeasuring = false;
    shockwave::FrameSummary m_benchmarkWarmup;   // the warm-up second, reported apart
    uint64_t m_benchmarkFrames = 0, m_benchmarkCpuPeak = 0, m_benchmarkGpuPeak = 0;
    double m_uiUpdateMs = 0, m_uiLayoutMs = 0;   // game UI CPU time summed over the measured frames
    size_t m_uiFrames = 0;
    double m_effectsUpdateMs = 0, m_effectsBuildMs = 0;   // particle effects, the same way
    // Each particle stage on its own (docs/Effects.md, "What it costs"), averaged per frame.
    struct ParticleTotals {
        double waitMs = 0, uploadMs = 0, gpuMs = 0, uploadBytes = 0, particles = 0;
        uint32_t peakParticles = 0;
    } m_particles;
    shockwave::World m_world;
    shockwave::FlyCamera m_camera;
    shockwave::Entity m_toolCamera;   // --camera: the Camera entity to look through
    shockwave::RenderViewId m_view;   // what the renderer keeps history for
    shockwave::Entity m_viewCamera;   // the camera m_view looks through
    std::unique_ptr<shockwave::AssetFolder> m_assets;
    std::unique_ptr<shockwave::GameInstance> m_instance;
    // Online services, pumped every frame; they outlive the session. Unavailable ones
    // until someone asks to play online (--host, a join, a menu's Host or Find), which
    // starts Steam (m_steam) for the project's steam_app_id.
    std::unique_ptr<shockwave::net::IOnlineServices> m_services = shockwave::net::makeUnavailableOnlineServices();
    std::unique_ptr<shockwave::net::SteamServices> m_steam;
    // Hosting and joining (Multiplayer/OnlineMatch.h), shared with the game's menus as
    // game.onlineMatch when the project has a network scene to play.
    shockwave::net::OnlineMatch m_match;
    std::unique_ptr<shockwave::net::ReplicationRegistry> m_matchRegistry;
    std::unique_ptr<shockwave::net::CaptureWriter> m_capture;   // --net-capture
    fs::path m_offlineScene;   // where a match that ends goes back to
    bool m_onlineFromCommandLine = false;   // its failure ends the player with exit 1
    // --net-preview: the hub and both sides' services outlive both sessions; the host's
    // session plays its own copy of the scene, pumped and ticked with ours.
    std::unique_ptr<shockwave::net::LoopbackHub> m_previewHub;
    std::unique_ptr<shockwave::net::LoopbackOnlineServices> m_previewHostServices, m_previewClientServices;
    std::unique_ptr<shockwave::net::ReplicationRegistry> m_previewRegistry;
    std::unique_ptr<shockwave::World> m_previewWorld;
    std::unique_ptr<shockwave::PlaySession> m_previewHost;
    std::unique_ptr<shockwave::GameUI> m_ui;   // before the session, whose UIWorld uses it
    std::unique_ptr<shockwave::Effects> m_effects;   // the same, for EffectsWorld
    // Postprocess profiles and LUTs (A05, A06), and the last frame's time for adaptation.
    std::unique_ptr<shockwave::PostProcessLibrary> m_postProcess;
    double m_frameDt = 1.0 / 60.0;
    // What levels hold resident and the graph that says what they need (docs/AssetResidency.md);
    // before m_levels, which holds a lease on it.
    // On demand, what loads and unloads m_assets for it; before it, so leases can use it.
    std::unique_ptr<shockwave::AssetFolderLoader> m_assetLoader;
    shockwave::AssetResidency m_residency;
    std::unique_ptr<shockwave::AssetGraph> m_assetGraph;
    std::vector<shockwave::LoadReport> m_loadReports;   // --load-report
    // On demand: the game circuit's assets, for the whole run, and the scene in m_world
    // until its level's lease holds them (startLevels lets go).
    shockwave::AssetResidency::Lease m_applicationAssets;
    shockwave::AssetResidency::Lease m_sceneAssets;
    // The active runtime level and its session (docs/LevelLifetime.md), in m_world.
    std::unique_ptr<shockwave::LevelHost> m_levels;
    std::optional<shockwave::SaveIdentity> m_saveIdentity;   // saveIdentity(), worked out once
    // After m_levels, so they go first: the shell holds a subscription to it.
    std::unique_ptr<shockwave::AppShell> m_shell;
    std::unique_ptr<shockwave::FlowTest> m_flowTest;
    shockwave::LevelCatalog m_catalog;   // where travel can go; the implicit "startup" without one
    bool m_catalogLoaded = false;
    // How the next level's session goes online (host, client, net preview), or offline.
    std::optional<shockwave::net::NetworkStart> m_levelNetworkStart;
    shockwave::net::IOnlineServices* m_levelServices = nullptr;
    shockwave::NavMeshOverlay m_navOverlay;
    shockwave::BakedLightingRuntime m_bakedLighting;   // the scene's lightmaps and probes (G05, G06)
    double m_bakedLightingTime = 0;
    shockwave::ReflectionProbesRuntime m_reflections;   // the scene's captured reflection probes (G07)
    std::unique_ptr<shockwave::ReflectionCapture> m_reflectionCapture;   // --capture-reflections
    std::string m_reflectionStage;   // the last progress line printed
    std::unique_ptr<shockwave::net::ReplicationRegistry> m_networkRegistry;   // --check: frozen, or null
    bool m_predictedSpawns = false;   // --check: the manifest spawns a PredictedCharacter
};

}   // namespace player
