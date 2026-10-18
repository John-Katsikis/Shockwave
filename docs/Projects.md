# Game projects and gameplay code

A Shockwave game is a project: a `.swproject` file plus the folders next to it. The
project names your game, says which scene to start in, and points to your content and
your C++ game code. Read this when you start a new game or write gameplay code.

New to Shockwave? Start with
[Tutorial: your first game in Shockwave](Tutorial.md), then come back here.

## Open a project

The sample project lives in `Games/Sample`. Open it in the editor, play it, or run it
in the standalone player:

```sh
./build/bin/editor Games/Sample
./build/bin/editor Games/Sample --play
./build/bin/player --project Games/Sample/Sample.swproject
```

The first command opens the editor. The second opens it and starts Play right away.
The third runs the game without any editor UI.

Both the editor and the player take the project as their first argument. You can pass
the folder or the `.swproject` file. `--project path` does the same thing. Paths are
read relative to the `.swproject` file, so you can launch from any working folder.

If you give the editor no path, it opens, in this order:

1. the `.swproject` in the working folder or a folder above it;
2. the last project you had open;
3. the project hub.

If you give it a scene path instead, it finds the project by searching the folders
above that scene.

If you give the player no project, it uses the one in or above the working folder.
If there is none, it uses the project its game module was built for (`Games/Sample`
by default).

The sandbox (`Source/Programs/Sandbox`) is a separate renderer and ECS demo. It doesn't
use projects.

## What's in a project folder

```text
Games/Sample/
  Sample.swproject      the manifest
  Content/              images and models; scenes name them relative to here
  Scenes/               .swscene files
  Source/SampleGame.cpp the game module
  Saved/                per-user editor state, autosaves, screenshots; not in git
```

`Saved/` holds your own editor state: the last open scene, the editor camera for each
scene, autosaves, and editor screenshots. Git ignores it. Scenes don't store the editor
camera.

## Create a new project

The project hub offers the sample project and can create a new one. A new project gets:

- `<Name>.swproject`, with spaces and punctuation removed from the name;
- `Content/`, `Scenes/`, `Config/` and `Source/` folders;
- its own C++ game module and `CMakeLists.txt`, without inheriting the open game's module;
- input settings and a README with build instructions;
- a `.gitignore` that ignores `Saved/`;
- a starter scene with a camera, ground and a light.

The same starter is available without opening the editor:

```sh
./build/bin/player --new-project /absolute/path/to/MyGame --name "My Game"
```

Register the new folder in `SHOCKWAVE_GAMES` and build its module before Play.
[Starting projects](StartingProjects.md) gives the complete workflow, including
projects outside the engine repository and the distinction between Sample and Arena.

To change a project's settings later, use File > Project settings... in the editor. It
edits the manifest in place.

## Change project settings

The manifest is a short text file. Here is a full example:

```text
shockwave_project 1
name "My Game"
game_module "my_game"
startup_scene "Scenes/main.swscene"
assets "Content"
theme "Config/Editor.swtheme"
game_circuit "Content/Circuits/Game.swcircuit"  # optional: a circuit for the whole game
tick_rate 60
window_size 1280 720
vsync true
frame_limit 0      # optional: cap in frames per second, 0 = none
low_latency false  # optional: wait for each frame to reach the screen before the next
shadow_quality high  # optional: off, low, medium, high or ultra
texture_compression high  # optional: high, small or off (docs/Optimisation.md)
mesh_lods 4        # optional: simpler levels of detail per mesh, 0 to 7
lod_error_pixels 1 # optional: how far a level may stray on screen, in pixels
renderer "vulkan"    # optional: the renderer plugin the player starts with
```

| Key | Required | Default | What it does |
|---|---|---|---|
| `shockwave_project` | yes | | The file format version. Use `1`. |
| `name` | yes | | Your game's name. |
| `game_module` | yes | | The id of your compiled game code. See [Write gameplay code](#write-gameplay-code). |
| `startup_scene` | yes | | The scene to start in. The file must exist. |
| `assets` | no | `Content` | The content folder. Scenes name assets relative to it. |
| `scenes` | no | `Scenes` | Where the editor lists and saves scenes, for example `scenes "levels"`. |
| `theme` | no | | An editor theme. Used when neither `--theme` nor Preferences picks one. |
| `game_circuit` | no | | A `.swcircuit` inside the content folder that runs for the whole game, next to the GameMode. The file must exist. See [Circuits](Circuits.md). |
| `tick_rate` | no | `60` | Simulation ticks per second, 1 to 1000. |
| `window_size` | no | `1280 720` | The player's window size. At least 320 x 240. |
| `vsync` | no | `true` | Wait for the screen's refresh before showing a frame. |
| `frame_limit` | no | `0` | Cap on frames per second: `0` for none, or 10 to 1000. |
| `low_latency` | no | `false` | Wait for each frame to reach the screen before starting the next. |
| `shadow_quality` | no | `high` | `off`, `low`, `medium`, `high` or `ultra`. |
| `texture_compression` | no | `high` | `high`, `small` or `off`. See [Optimisation](Optimisation.md). |
| `mesh_lods` | no | `4` | Simpler levels of detail per model mesh, `0` to `7`. |
| `lod_error_pixels` | no | `1` | How far, in pixels, a level of detail may stray on screen, `0` to `100`. |
| `renderer` | no | | The renderer plugin the player starts with. |

[Frame pacing](FramePacing.md) explains the frame settings, and [Shadows](Shadows.md)
explains the shadow qualities.

A few rules for the file:

- Put names and paths that contain spaces in quotes.
- `#` starts a comment, on its own line or at the end of one.
- Unknown keys, repeated keys and invalid values are errors. The message names the file
  and line.

The editor uses its own window size and VSync setting, but it shares the project's tick
rate. Scene, theme and asset folder options you pass to the editor on the command line
override the project's values.

## Write gameplay code

Your game's C++ lives in a game module: a library the editor and player load for your
project. A whole game module can be one file. Put it anywhere under your project's
`Source/` folder:

```cpp
#include <Shockwave/Game.h>   // everything game code normally needs

using namespace shockwave;

// A component: plain data. The fields listed in SW_COMPONENT are saved in scenes and
// shown in the Inspector (with the editor's limits); the rest is runtime state.
struct Health {
    float current = 100;
    float max = 100;
    float hurtTimer = 0;   // runtime only
};
SW_COMPONENT(Health, SW_FIELD(current, 0, 1000), SW_FIELD(max, 1, 1000))

class MyGameMode final : public GameMode {
public:
    // Systems run every fixed tick, in parallel, on every entity with the components.
    void configureSystems(GameContext&, Schedule& schedule) override {
        schedule.each<Health>("Regenerate", [](const SystemContext& ctx, Health& health) {
            health.current = std::min(health.max, health.current + 5.0f * ctx.dt);
        });
    }
    // The scene is loaded: find things by name, add components, spawn.
    void beginPlay(GameContext& game) override {
        if (EntityRef player = game.find("Player")) player.add(Health{.max = 150});
    }
    // Main thread, once per rendered frame: input, audio, one-off events.
    void frame(GameContext& game, double) override {
        if (game.input().wasActionPressed("jump")) game.audio().playClip("Audio/jump.wav");
    }
};

SW_GAME("my_game", MyGameMode)   // the id the .swproject's game_module names
```

A component is a plain struct of data that you attach to entities. A system is a
function that runs on every entity that has certain components. If you know Unity or
Unreal, think of entities as GameObjects or Actors, but with no per-object `Update` or
`Tick`. [Architecture](Architecture.md#gameplay-model) maps the terms.

### The building blocks

| Piece | What it does |
|---|---|
| `SW_COMPONENT(Type, fields...)` | Registers a component when the module loads. List each saved field once. |
| `SW_FIELD(name)` or `SW_FIELD(name, min, max)` | A saved field, with optional limits for the Inspector. Scene files spell it in snake_case: `SW_FIELD(jumpSpeed)` is saved as `jump_speed`. |
| `SW_ENUM_FIELD(name, "First", "Second")` | A saved int-backed enum, with a name for each value. |
| `SW_COMPONENT_IN("Category", Type, ...)` | Like `SW_COMPONENT`, but listed under another heading in the Add Component menu. |
| `SW_GAME("id", Type)` | Declares the module. Put it at global scope, outside any namespace. |
| `GameContext` | Your way into the running game (`game` in the example). |
| `EntityRef` | An entity together with its world, returned by `game.find`. |

Registered components work everywhere built-in ones do: scene files, the Inspector,
undo, `player --schema` and `player --check`.

In `SW_GAME`, `Type` is either the `GameMode` every scene plays with, or your own
`GameInstance` (see [Use different rules per scene](#use-different-rules-per-scene)).

`GameContext` gives you:

| Member | What it is |
|---|---|
| `world` | The scene's entities and components. |
| `find("Name")` | The entity with that name, as an `EntityRef`. |
| `spawn("Prefabs/X.swprefab")` | A new copy of a prefab, as an `EntityRef`. See [Prefabs](Prefabs.md). |
| `input()` | The engine's input. It works without a window too, where nothing is ever pressed. |
| `audio()` | The session's audio. |
| `physics()` | The session's physics. |
| `saves()` | Saving and loading the game. See [Save games](SaveGames.md). |
| `ui()` | The session's menus and HUDs. See [Game UI](GameUI.md). |
| `effects()` | The session's particle effects. See [Particle effects](Effects.md). |
| `animation()` | Switch a character's animation clip, find its joints. See [Animation](Animation.md). |
| `engine` | The engine. Null in tools and tests that run without a window. |
| `jobs` | The job system that runs work across threads. |
| `subsystems`, `subsystem<T>()` | This Play session's services. See [Lifecycle](#lifecycle). |

`EntityRef` has `has<T>()`, `get<T>()`, `find<T>()` (null when the component is
missing), `add(T{...})`, `remove<T>()` and `destroy()`. It converts to false when
nothing was found. To work on many entities at once, use `world.each` or a system.

### GameMode hooks

Override these in your `GameMode`. The hooks run on the main thread. The systems you
add run on worker threads.

| Hook | When it runs |
|---|---|
| `configureSystems(game, schedule)` | When Play starts. Add your systems here. |
| `beginPlay(game)` | Once the scene is loaded and physics exists. Spawn things, add subsystems. |
| `fixedUpdate(game, dt)` | Every fixed tick, after the systems and physics. |
| `frame(game, frameDt)` | Once per rendered frame, only while playing. |
| `endPlay(game)` | When Play stops. Clean up anything you created outside the world. |

### The sample game

`Games/Sample/Source/SampleGame.cpp` is the sample's module. It defines a ColorPulse
component and a `GameMode` that schedules a system for it. When you press Play, the
sample's Crate gets that component and its tint pulses. Stop removes what Play added
and restores the original tint.

ColorPulse also appears in the Inspector's Add Component menu. You can add it and save
it in scenes like a built-in component.

## Build your game module

A game module is a folder with a one-line `CMakeLists.txt` that names the module's id:

```cmake
# MyGame/CMakeLists.txt
shockwave_game_module(my_game)
```

Every `.cpp` under the folder's `Source/` is built. New files are picked up on the next
build without touching CMake. To list the files yourself instead, pass `SOURCES`.

Add the folder to `SHOCKWAVE_GAMES` when you configure. It's a `;`-separated list, and
the sample is in it by default:

```sh
cmake -S . -B build "-DSHOCKWAVE_GAMES=$PWD/Games/Sample;/absolute/path/to/MyGame" \
      -DSHOCKWAVE_GAME_PROJECT=/absolute/path/to/MyGame.swproject
cmake --build build -j
```

This builds `build/bin/modules/game_my_game.so`. `SHOCKWAVE_GAME_PROJECT` is the project
the player runs when it can't find one.

The editor and player load the module named by the project's `game_module`. So one
editor can open any project whose module is built. A project whose module isn't built
still opens for editing. Play is turned off, and the Console shows the build command.
The editor loads the module as soon as it appears.

## Reload your code while the editor runs

Rebuild the module in any of these ways, and the editor reloads it in place. The scene
and undo history are kept.

- Build > Build and reload game code in the editor.
- Press Ctrl+Alt+B.
- Run this from anywhere:

```sh
cmake --build build --target game_my_game
```

[Plugins](Plugins.md) explains what survives a reload.

## Use different rules per scene

`SW_GAME("id", MyGameMode)` uses `SimpleGameInstance<MyGameMode>`, which plays every
scene with the same `GameMode`. For more control, derive your own class from
`GameInstance` and pass that to `SW_GAME` instead. You can override:

| Member | Use it to |
|---|---|
| `createGameMode(scene)` | Pick a `GameMode` for each scene. |
| `init(engine)` | Set things up once for the whole run. `engine` is null in tools and tests without a window. |
| `shutdown()` | Undo what `init` did. |

Keep services that last the whole run in the instance's `subsystems()`.

## Take full control of the module

Instead of `SW_GAME`, you can define `shockwave::projectGameModule()` yourself. This also
lets you add:

- `createAssets`: makes persistent named assets once the engine exists;
- `registerTypes`: runs after the `SW_COMPONENT`s, if you'd rather register by hand.

```cpp
namespace shockwave {
const GameModule& projectGameModule() {
    static const GameModule module{
        .id = "my_game",
        .registerTypes = [] { /* registerComponent<T>(...) by hand, if you prefer */ },
        .createAssets = [](Engine& engine) { /* create persistent named assets */ },
        .createGameInstance = [] { return std::make_unique<SimpleGameInstance<MyGameMode>>(); },
    };
    return module;
}
}   // namespace shockwave
```

If you leave `createGameInstance` empty, you get a plain `GameInstance`. It plays with a
`GameMode` that runs only the engine's own systems.

## Authoring with tools and AI

You can generate projects and scenes with scripts or an AI agent, not only edit them by
hand. Follow these steps.

**1. Read the scene format.** [SceneSchema.md](SceneSchema.md) lists every component and
field. It's generated from the engine and your project's assets, so it always matches
the build. Regenerate it with:

```sh
build/bin/player --schema docs/SceneSchema.md
```

**2. Write or edit the scene.** Edit the `.swscene`, and the `.swproject` if needed.
Refer to other entities by their unique name (`entity "Stack"`) and to options by name
(`motion Kinematic`). Leave out fields that keep their default values.

**3. Check it for mistakes.** This checks every scene in the project around the working
folder, or the sample if there is none:

```sh
build/bin/player --check
```

Or check one scene:

```sh
build/bin/player --check Games/MyGame/Scenes/level.swscene
```

It loads the manifest, imports assets, loads each scene, and starts gameplay and physics
on it the way Play would. Each problem prints with its file and line, like this:

```text
Scenes/level.swscene:14: no entity named 'Grond'; left empty
```

The exit code is 1 until there are no problems. It also flags:

- scenes that don't have exactly one primary Camera;
- lights without a Transform;
- more lights than the renderer draws;
- extra Environments.

**4. Look at the result.** This renders the scene in a hidden window and saves the
frame as an image:

```sh
build/bin/player --scene Games/MyGame/Scenes/level.swscene --screenshot level.png
```

| Option | What it does |
|---|---|
| `--view lighting` | Show only the lighting. |
| `--view normals` | Show only the geometry's surface directions. |
| `--size 1280x720` | Pick the image size. |

An agent that can read images can compare the screenshot with what it meant to build.

### What the loader warns about

The loader warns about anything it has to skip or guess:

- unknown components or fields;
- unknown asset names;
- broken or ambiguous links to entities;
- extra values, and values outside a field's range.

The editor shows the same warnings in its Console when it opens a scene. When it saves,
it writes names rather than numbers, so files stay stable when you add or reorder
entities. Older files with numeric links and option numbers still load.

## Test your changes

Run the automated tests:

```sh
ctest --test-dir build --output-on-failure
```

These check manifest validation and relative paths, fixed-tick gameplay, pause and
resume, restarting, restoring the world, and cleanup when starting or stopping fails.

For a quick test on the GPU, run the editor and the player against the same project for
120 frames each:

```sh
./build/bin/editor Games/Sample --play --frames 120
./build/bin/player --project Games/Sample/Sample.swproject --frames 120
```

Both should report the same sample gameplay starting and shutting down, with simulation
ticks run. `--frames N` stops after N frames, which keeps smoke tests short.

## Good to know

Menus and HUDs, save games and packaging have their own guides:
[Game UI](GameUI.md), [Save games](SaveGames.md) and [Packaging your game](Packaging.md).
A few things about how projects run:

- The player renders through the scene's primary Camera entity. Play in the editor
  looks through the same camera, and F8 switches to the editor camera.
- In the player, a scene without a camera falls back to a fly camera.
- Both hosts share the same Jolt physics. See [Physics](Physics.md).
- The player refuses to start if a scene or asset fails to load, rather than running
  with missing content.
- Scenes and assets must stay readable on disk while the game runs.

## How it works

The rest of this page is for the curious. You don't need it to make a game.

### The gameplay classes

Game code is organized like Unreal's, on top of entities and systems. The headers are in
`Source/Runtime/GameFramework/Public/GameFramework/`.

| Class | What it is |
|---|---|
| `GameModule` | Plugs the game's C++ into the editor and the player. |
| `GameInstance` | Lives for the whole run of the editor or player. It outlives Play sessions, keeps state between them, and picks the `GameMode` for each scene with `createGameMode(scene)`. |
| `GameMode` | The rules for one Play of a scene. A fresh one is made for every Play. |
| `PlaySession` | Runs one Play. The editor's Play button and the player both use it. |

### Lifecycle

When the editor or player starts, it:

1. registers the module's types;
2. creates the same starter assets, then runs the module's `createAssets`;
3. creates the `GameInstance` and calls its `init`;
4. imports project content and loads the scene.

When Play starts, the session:

1. takes a snapshot of the scene as you authored it;
2. adds its `PhysicsWorld` and `AudioWorld` subsystems;
3. asks the `GameInstance` for a fresh `GameMode`;
4. installs the built-in systems and yours, with `configureSystems`;
5. calls `beginPlay`;
6. creates the physics bodies and sets up smoothing between ticks.

Because `beginPlay` runs before the first tick, your systems can use `GameMode` state
that `beginPlay` sets up.

Each fixed tick runs your scheduled systems, then the Jolt physics step, then
`fixedUpdate`. `frame` runs once per rendered frame, only while playing.

A subsystem is a service you find by type. `game.physics()` returns the `PhysicsWorld`
from `configureSystems` through `endPlay`. `game.subsystem<T>()` finds any other
subsystem, or returns null. To add your own, derive a class from `Subsystem`
(`Engine/Subsystem.h`) and call `game.subsystems.add<T>(...)`. Session subsystems are
destroyed newest first when Play stops.

`engine` is set in both the editor and the player. Only CPU-only tests may leave it out.
When you need the engine's input or assets, use `game.input()` or
`game.engine->assets()`.

Systems run on worker threads. They must follow the ECS threading rules, and use command
buffers to add or remove entities and components.

### Pause and Stop

Pause freezes gameplay without making a new `GameMode`, and Resume carries on. Stop:

1. calls `endPlay`;
2. destroys the `GameMode` and the session's subsystems;
3. restores the scene as you authored it;
4. throws away the schedule.

Playing again makes a fresh `GameMode`. If starting or stopping fails, the authored
scene is restored too, and cleanup errors are logged.

The snapshot can't undo effects outside the world, like files or sounds. Clean up
anything your game owns in `endPlay`. The `GameInstance` and the assets from
`createAssets` are separate and last for the whole run.

Game menus can access quality and frame pacing through `game.graphics()`. See [Graphics settings](Graphics.md).
