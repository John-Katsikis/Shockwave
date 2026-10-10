# Game projects and shared gameplay

A `.swproject` file connects a game name, startup scene, asset folder, runtime settings,
and compiled gameplay module. The included `shockwave.swproject` opens the existing
sample scene. It does not modify that scene file. Paths resolve relative to the
manifest, so launching from another working directory works:

```sh
./build/bin/editor --project shockwave.swproject
./build/bin/editor --project shockwave.swproject --play
./build/bin/player --project shockwave.swproject
```

For a launch from another folder, give an absolute manifest path. The player needs
no editor UI; it loads project assets, the startup scene, and project gameplay. The
existing sandbox remains an independent renderer/ECS demo.

## Manifest

```text
shockwave_project 1
name "My Game"
game_module "my_game"
startup_scene "scenes/main.swscene"
assets "assets"
theme "themes/default.swtheme"
tick_rate 60
window_size 1280 720
vsync true
```

The version, name, module ID, and existing startup scene are required. Assets, scenes
and theme paths default to `assets`, `scenes` and `themes/default.swtheme` (an optional
`scenes "levels"` line moves where the editor lists and saves scenes); runtime defaults are 60 Hz,
1280 x 720 and VSync enabled. Names and paths containing spaces must be quoted. Whole
line and trailing `#` comments are accepted. Unknown/duplicate keys and invalid values
are errors, with file and line diagnostics. The editor uses its own workspace size
and VSync by default, but shares the project's simulation tick rate. Explicit editor
scene, theme, and asset-folder CLI options override project defaults.

When launched from the project root, the editor discovers `shockwave.swproject`; a scene
path finds its project by searching parent folders. With no project it opens the project
hub, which creates new projects (manifest, `assets/`, `scenes/`, `.gitignore` and a
starter scene with a camera, ground and light). Project > Project settings edits the
manifest in place. `--frames N` bounds smoke tests.

Per-user editor state (last open scene, editor camera per scene, autosaves) lives in
`.shockwave/` next to the manifest and is ignored by git. Scenes no longer store the
editor camera.

## C++ gameplay

`examples/game/Game.cpp` is the included module. It defines a registered ColorPulse
component and a scheduled system. On Play, the example Crate gets that component and
its tint pulses. Stop removes runtime additions and restores its original tint. The
component also appears in the inspector's Add Component menu and can be authored and
serialized like built-in components.

Define `shockwave::projectGameModule()` in your game's source:

```cpp
#include <shockwave/Gameplay.h>

class MyGameplay : public shockwave::Gameplay {
public:
    void onStart(shockwave::GameplayContext& ctx) override {
        // Spawn runtime entities and initialize session-owned state.
    }
    void configureSystems(shockwave::GameplayContext& ctx,
                          shockwave::Schedule& schedule) override {
        // Register read/write-declared fixed-tick systems here.
    }
    void onStop(shockwave::GameplayContext& ctx) override {
        // Release any non-ECS resources owned by this session.
    }
};

const shockwave::GameModule& shockwave::projectGameModule() {
    static const shockwave::GameModule module{
        "my_game",
        [] { return std::make_unique<MyGameplay>(); },
        [] { /* Register reflected types before any scene is loaded. */ },
        [](shockwave::Engine& engine) { /* Register persistent named assets. */ }
    };
    return module;
}
```

Select that source at configure time and rebuild both hosts:

```sh
cmake -S . -B build -DSHOCKWAVE_GAME_SOURCE=/absolute/path/to/Game.cpp
cmake --build build -j
```

The same static `shockwave_game` library links into editor and player. The manifest's
`game_module` must match its module ID; a mismatch fails before creating a window.
This is a compiled C++ workflow: one selected module per build. Runtime loading or
hot reloading game libraries is a later feature. Add any extra source files to the
`shockwave_game` CMake target as your game grows.

## Lifecycle

Both hosts register module types, create the same starter assets, run the persistent
module asset setup callback, import project images, and load the scene. Starting a
session snapshots the authored World, creates a fresh Gameplay instance, installs
built-in and game systems, calls `onStart`, and initializes transform interpolation.
`configureSystems` runs before `onStart`; system closures can refer to instance state
which `onStart` initializes before the first tick.

Updates run on the engine's fixed tick: scheduled gameplay systems, Jolt simulation,
then `onPhysicsStep`. `ctx.physics` is available from `onStart` through `onStop`. `onFrame` runs once per rendered frame only
while playing. `GameplayContext` gives access to World, jobs, and Engine (non-null in
both hosts; CPU-only tests may omit it). Gameplay code uses `ctx.engine->input()` and
`ctx.engine->assets()` when needed. Schedule workers must follow the existing ECS
threading rules and use command buffers for structural mutations.

Pause freezes gameplay without creating another instance. Resume continues that
instance. Stop calls `onStop`, restores the authored World, discards the schedule, and
destroys the instance. Restart creates a fresh instance. Start/stop failures also
restore the authored World; cleanup errors are reported. ECS snapshots cannot undo
external effects such as files or audio: game-owned resources must be cleaned up by
`onStop`. Persistent module asset setup is separate and runs once per host startup.

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
2. Write or edit the `.swscene` (and `shockwave.swproject` if needed). Refer to other
   entities by unique name (`entity "Stack"`) and to options by name (`motion Kinematic`).
   Leave out fields that keep their defaults.
3. Run `build/bin/player --check` (every scene in the project) or
   `build/bin/player --check scenes/level.swscene`. It loads the manifest, imports assets,
   loads each scene and starts the gameplay and physics on it as Play would. Every
   problem prints as `scenes/level.swscene:14: no entity named 'Grond'; left empty`, and
   the exit code is 1 until there are none. It also flags scenes without exactly one
   primary Camera, lights without a Transform, more lights than the renderer draws and
   extra Environments.
4. Look at the result: `build/bin/player --scene scenes/level.swscene --screenshot
   level.png` renders the scene in a hidden window and saves the frame. Add
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
