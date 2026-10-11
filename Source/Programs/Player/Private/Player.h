#pragma once

// The player: runs a project like the shipped game, or one of the command-line tools
// (--check, --schema, --input-schema, --devices, --screenshot, --benchmark, --bake-navmesh) that people, CI and AI
// agents use to verify it.

#include <Assets/AssetFolder.h>
#include <Engine/Engine.h>
#include <GameFramework/GameModule.h>
#include <GameFramework/PlaySession.h>
#include <Effects/Effects.h>
#include <GameUI/GameUI.h>
#include <Navigation/Navigation.h>
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
    enum class Mode {
        Play,
        Check,
        Schema,
        BakeNavMesh,
        AudioMix,
        Animations,
        NodeSchema,
        CircuitDump
    } mode = Mode::Play;
    std::vector<fs::path> scenes;   // --check, --bake-navmesh: empty means every scene in the project
    fs::path schemaOut;   // --schema, --node-schema: "-" is standard output
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
    shockwave::DebugView view = shockwave::DebugView::Lit;   // --view
    bool showNavMesh = false;   // --view navmesh: the lit scene with the navigation mesh and agents' paths
    int shadows = -1;   // --shadows: a ShadowQuality; -1 uses the project's shadow_quality
    bool printFrame = false;   // --print-frame: the render graph, at exit
    double benchmarkSeconds = 0;   // --benchmark: measure this long after a warm-up, report, quit
    fs::path benchmarkJson;   // --json with --benchmark
    bool uiPreview = false;   // --ui-preview: unset {{values}} show UIDocument preview values
    std::vector<shockwave::UINav> uiNav;   // --ui-nav: menu steps, one a frame from frame 5
    fs::path audioMix;   // --audio-mix: render the scene's sound offline to this .wav
    double audioSeconds = 3;   // --seconds with --audio-mix
    int audioHrtf = -1;   // --audio-hrtf on|off; -1 uses the project's audio_hrtf
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
};
int packageGame(const shockwave::Project& project, const PackageCommand& command);
// In a packaged game, the project it ships with; empty otherwise.
fs::path packagedProjectFile();
// --check: problems in Config/Package.swpackage, when the project has one.
void checkPackageSettings(const shockwave::Project& project, std::vector<std::string>& problems);

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
    // Asset optimisation (docs/Optimisation.md): the project's settings, before importing;
    // and --optimise, which compresses every texture into the cache and reports.
    void applyOptimisationSettings();
    int optimise();
    // --benchmark: warms up, measures, prints the frame statistics and quits.
    void benchmark(double dt);
    void reportBenchmark();
    // --save: saves to the slot, waits for the file, reports and quits.
    void saveAndQuit();
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

    // --check for particle effects (PlayerEffects.cpp): a scene's ParticleEffects, then
    // every .efkefc in Content. Warnings (parts that aren't drawn yet) don't fail it.
    void checkParticleEffects(
        shockwave::World& world, const std::string& scene, std::vector<std::string>& problems,
        std::vector<std::string>& checkedEffects);
    void checkUnusedEffects(const std::vector<std::string>& checkedEffects, std::vector<std::string>& problems);

    shockwave::Project m_project;
    Tool m_tool;
    const shockwave::GameModule& m_module;
    int m_exitCode = 0;
    int m_frame = 0;
    size_t m_uiNavFrame = 0;
    bool m_screenshotRequested = false;
    double m_benchmarkTime = 0;
    int m_saveFrames = 0;   // frames played, for --save
    bool m_benchmarkMeasuring = false;
    double m_uiUpdateMs = 0, m_uiLayoutMs = 0;   // game UI CPU time summed over the measured frames
    size_t m_uiFrames = 0;
    double m_effectsUpdateMs = 0, m_effectsBuildMs = 0;   // particle effects, the same way
    shockwave::World m_world;
    shockwave::FlyCamera m_camera;
    std::unique_ptr<shockwave::AssetFolder> m_assets;
    std::unique_ptr<shockwave::GameInstance> m_instance;
    std::unique_ptr<shockwave::GameUI> m_ui;   // before the session, whose UIWorld uses it
    std::unique_ptr<shockwave::Effects> m_effects;   // the same, for EffectsWorld
    std::unique_ptr<shockwave::PlaySession> m_session;
    shockwave::NavMeshOverlay m_navOverlay;
};

}   // namespace player
