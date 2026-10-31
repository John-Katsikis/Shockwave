#pragma once

// The player: runs a project like the shipped game, or one of the command-line tools
// (--check, --schema, --input-schema, --devices, --screenshot, --benchmark, --bake-navmesh) that people, CI and AI
// agents use to verify it.

#include <Assets/AssetFolder.h>
#include <Engine/BakedLighting.h>
#include <Engine/ReflectionProbes.h>
#include <Engine/Engine.h>
#include <GameFramework/GameModule.h>
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
#include <Projects/Project.h>

#include <array>
#include <filesystem>
#include <memory>
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
        BakeLighting
    } mode = Mode::Play;
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
// --saves: the project's save games, newest first; json, when set, gets them as JSON too.
int listSaves(const shockwave::Project& project, const fs::path& json);

// --package: makes a game folder for players (Packaging/Package.h). --output, --no-build
// and --no-archive override Config/Package.swpackage for this run.
struct PackageCommand {
    bool requested = false;
    fs::path output;
    bool noBuild = false, noArchive = false;
    bool development = false;   // --dev-package: a test build (allows Steam AppID 480)
};
int packageGame(const shockwave::Project& project, const PackageCommand& command);
// In a packaged game, the project it ships with; empty otherwise.
fs::path packagedProjectFile();
// --check: problems in Config/Package.swpackage, when the project has one.
void checkPackageSettings(const shockwave::Project& project, std::vector<std::string>& problems);
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
    Player(shockwave::Project project, Tool tool, const shockwave::GameModule& module)
        : m_project(std::move(project)), m_tool(std::move(tool)), m_module(module) {}
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
    fs::path saveFolder() const { return m_project.statePath() / "SaveGames"; }

    // Loads each scene the way the game would and reports every problem as file:line.
    int check(const std::vector<std::string>& importProblems);

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
    void checkAllCircuits(const std::vector<std::string>& usedCircuits, std::vector<std::string>& problems);
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
    bool m_benchmarkMeasuring = false;
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
    std::unique_ptr<shockwave::PlaySession> m_session;
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
