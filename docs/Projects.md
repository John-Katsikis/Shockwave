# Game projects and shared gameplay

A `.swproject` file connects a game name, startup scene, content folder, runtime
settings, and compiled game module. The sample project lives in `Games/Sample`:

```text
Games/Sample/
  Sample.swproject      the manifest
  Content/              images and models; scenes name them relative to here
  Scenes/               .swscene files
  Source/SampleGame.cpp the game module
  Saved/                per-user editor state, autosaves, screenshots; not in git
```

Paths resolve relative to the manifest, so launching from another working directory
works:

```sh
./build/bin/editor Games/Sample
./build/bin/editor Games/Sample --play
./build/bin/player --project Games/Sample/Sample.swproject
```

The editor and player also take the project (a folder or `.swproject`) as their first
argument; `--project path` does the same. The player needs no editor UI; it loads
project content, the startup scene, and project gameplay. Given no project, it uses the
one in or above the working folder, else the project its game module was built for
(`Games/Sample`). The sandbox remains an independent renderer/ECS demo.

## Manifest

```text
shockwave_project 1
name "My Game"
game_module "my_game"
startup_scene "Scenes/main.swscene"
assets "Content"
theme "Config/Editor.swtheme"
tick_rate 60
window_size 1280 720
vsync true
```

The version, name, module ID, and existing startup scene are required. The content and
scene folders default to `Content` and `Scenes` (an optional `scenes "levels"` line
moves where the editor lists and saves scenes). `theme` is optional; the editor uses it
when neither `--theme` nor Preferences picks one. Runtime defaults are 60 Hz,
1280 x 720 and VSync enabled. Names and paths containing spaces must be quoted. Whole
line and trailing `#` comments are accepted. Unknown/duplicate keys and invalid values
are errors, with file and line diagnostics. The editor uses its own workspace size
and VSync by default, but shares the project's simulation tick rate. Explicit editor
scene, theme, and asset-folder CLI options override project defaults.

Launched with no path, the editor opens the `.swproject` in the working folder or a
folder above it, else the last project, else the project hub. A scene path finds its
project by searching parent folders. The hub offers the sample project and creates new
projects: `<Name>.swproject` (the name without spaces or punctuation), `Content/`,
`Scenes/`, a `.gitignore` for `Saved/` and a starter scene with a camera, ground and
light. Project > Project settings edits the
manifest in place. `--frames N` bounds smoke tests.

Per-user editor state (last open scene, editor camera per scene, autosaves) and editor
screenshots live in `Saved/` next to the manifest, which is ignored by git. Scenes no longer store the
editor camera.

## C++ gameplay

Game code runs on the entities and systems described in the README, organized like
Unreal:

- `GameModule` plugs the game's C++ into the editor and the player.
- `GameInstance` lives for the whole run of the editor or player. It outlives Play
  sessions, holds state that carries between them, and picks the `GameMode` for each
  scene with `createGameMode(scene)`.
- `GameMode` is the rules of one Play of a scene. A fresh one is made for every Play.
- `PlaySession` runs one Play: the editor's Play button and the player both use it.

The headers are in `Source/Runtime/GameFramework/Public/GameFramework/`.

`Games/Sample/Source/SampleGame.cpp` is the included module. It defines a registered
ColorPulse component and a `GameMode` that schedules a system for it. On Play, the
sample Crate gets that component and its tint pulses. Stop removes runtime additions
and restores its original tint. The component also appears in the inspector's Add
Component menu and can be authored and serialized like built-in components.

Define `shockwave::projectGameModule()` in your game's source:

```cpp
#include <GameFramework/GameModule.h>

namespace shockwave {
namespace {

class MyGameMode final : public GameMode {
public:
    void configureSystems(GameContext& ctx, Schedule& schedule) override {
        // Register read/write-declared fixed-tick systems here.
    }
    void beginPlay(GameContext& ctx) override {
        // Spawn runtime entities, add subsystems, initialize session-owned state.
    }
    void fixedUpdate(GameContext& ctx, double dt) override {
        // Main thread, every fixed tick after the systems and the physics step.
    }
    void endPlay(GameContext& ctx) override {
        // Release any non-ECS resources owned by this session.
    }
};

}   // namespace

const GameModule& projectGameModule() {
    static const GameModule module{
        .id = "my_game",
        .registerTypes = [] { /* registerComponent the game's types before any scene loads. */ },
        .createAssets = [](Engine& engine) { /* Create persistent named assets. */ },
        .createGameInstance = [] { return std::make_unique<SimpleGameInstance<MyGameMode>>(); },
    };
    return module;
}

}   // namespace shockwave
```

`SimpleGameInstance<TGameMode>` plays every scene with the same `GameMode`. For more,
derive from `GameInstance`: override `createGameMode(scene)` to pick a mode per scene,
`init(engine)` and `shutdown()` for run-long setup (`engine` is null in tools and tests
without a window), and keep run-long services in its `subsystems()`. Leaving
`createGameInstance` empty gives a plain `GameInstance`, which plays with a `GameMode`
that runs only the engine's systems.

Select that source and its project at configure time and rebuild both hosts:

```sh
cmake -S . -B build -DSHOCKWAVE_GAME_SOURCE=/absolute/path/to/MyGame.cpp \
      -DSHOCKWAVE_GAME_PROJECT=/absolute/path/to/MyGame.swproject
cmake --build build -j
```

The same static `ShockwaveGame` library (`Games/Sample/CMakeLists.txt`) links into
editor and player. `SHOCKWAVE_GAME_PROJECT` is the project the player runs when it
finds none. The manifest's `game_module` must match the module's `id`; a mismatch
fails in the player before creating a window, and turns Play off in the editor.
This is a compiled C++ workflow: one selected module per build. Runtime loading or
hot reloading game libraries is a later feature. Add any extra source files to the
`ShockwaveGame` CMake target as your game grows.

## Lifecycle

Both hosts register module types, create the same starter assets, run the module's
`createAssets`, create the `GameInstance` and call its `init`, import project content,
and load the scene. Starting a Play snapshots the authored World, adds the session's
`PhysicsWorld` subsystem, asks the `GameInstance` for a fresh `GameMode`, installs
built-in and game systems with `configureSystems`, calls `beginPlay`, then creates the
physics bodies and initializes transform interpolation. System closures can refer to
`GameMode` state which `beginPlay` initializes before the first tick.

Updates run on the engine's fixed tick: scheduled systems, Jolt simulation, then
`fixedUpdate`. `frame` runs once per rendered frame only while playing. `GameContext`
gives access to `world`, `jobs`, `engine` (non-null in both hosts; CPU-only tests may
omit it) and the session's `subsystems`. `ctx.physics()` returns the `PhysicsWorld`
from `configureSystems` through `endPlay`; `ctx.subsystem<T>()` finds any other
subsystem, or returns null. A game adds its own with `ctx.subsystems.add<T>(...)` (a
class derived from `Subsystem`, in `Engine/Subsystem.h`); session subsystems are
destroyed newest first when Play stops. Gameplay code uses `ctx.engine->input()` and
`ctx.engine->assets()` when needed. Schedule workers must follow the existing ECS
threading rules and use command buffers for structural mutations.

Pause freezes gameplay without creating another `GameMode`. Resume continues it. Stop
calls `endPlay`, destroys the `GameMode` and the session's subsystems, restores the
authored World and discards the schedule. Playing again creates a fresh `GameMode`.
Start/stop failures also restore the authored World; cleanup errors are logged. ECS
snapshots cannot undo external effects such as files or audio: game-owned resources
must be cleaned up by `endPlay`. The `GameInstance` and the module's `createAssets`
are separate and last for the whole run of the host.

## Current boundaries

This first step provides project configuration, shared C++ gameplay and a standalone
runtime. It is not the release exporter or a new asset pipeline. The player renders
through the scene's primary Camera entity, and the editor's Play looks through the same
camera (F8 switches to the editor camera). A scene without a camera falls back to a fly
camera in the player. Game menus, audio, and save data remain future work. Jolt physics is shared by both
hosts; see [Physics](Physics.md).
The player rejects scene/asset loading errors rather than silently launching incomplete
content. Scenes and assets must currently remain accessible on disk.

## Authoring with tools and AI

Projects and scenes are designed to be generated as well as edited by hand:

1. Read [SceneSchema.md](SceneSchema.md). It is generated from the engine's component
   registry and the project's assets, so it always matches the build:
   `build/bin/player --schema docs/SceneSchema.md`.
2. Write or edit the `.swscene` (and the `.swproject` if needed). Refer to other
   entities by unique name (`entity "Stack"`) and to options by name (`motion Kinematic`).
   Leave out fields that keep their defaults.
3. Run `build/bin/player --check` (every scene in the project around the working
   folder, else the sample) or `build/bin/player --check Games/MyGame/Scenes/level.swscene`.
   It loads the manifest, imports assets, loads each scene and starts the gameplay and
   physics on it as Play would. Every problem prints with its file and line, as
   `Scenes/level.swscene:14: no entity named 'Grond'; left empty`, and
   the exit code is 1 until there are none. It also flags scenes without exactly one
   primary Camera, lights without a Transform, more lights than the renderer draws and
   extra Environments.
4. Look at the result: `build/bin/player --scene Games/MyGame/Scenes/level.swscene
   --screenshot level.png` renders the scene in a hidden window and saves the frame. Add
   `--view lighting` or `--view normals` to see the light or the geometry on its own,
   and `--size 1280x720` to pick the resolution. An agent that can read images can
   compare this against what it meant to build.

The loader warns about anything it has to skip or guess: unknown components or fields,
unknown asset names, broken or ambiguous entity links, extra values and values outside
a field's range. The editor shows the same warnings in its Console when it opens a scene,
and saving a scene writes names rather than numbers, so files stay stable when entities
are added or reordered. Numeric links and option numbers from older files still load.

## Checks

`ctest --test-dir build --output-on-failure` checks manifest validation and relative
paths, fixed-tick gameplay, pause/resume, restart, world restoration, and lifecycle
failure cleanup. For a GPU smoke test, run editor `--play --frames 120` and player
`--frames 120` against the same project. Both should report the same sample gameplay
startup and shutdown, with simulation ticks executed.
