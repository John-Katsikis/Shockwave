# Start a game without editing the engine

Each game is a project folder. It can live outside the Shockwave checkout and have
its own Git repository. Adding gameplay does not require adding files to engine
modules or editing the engine's `CMakeLists.txt`.

## Repository boundaries

```text
Shockwave/
  Source/Runtime/       shared engine modules
  Source/Plugins/       renderer plugins
  Source/Editor/        editor application
  Source/Programs/      player and other tools
  Templates/Blank/     readable project-generation template
  Games/Sample/        engine feature showcase; game_sample
  Games/Arena/         independent shooter; game_arena
    Source/            shooter code
    Content/           shooter assets, prefabs and UI
    Scenes/            shooter levels
    Config/            shooter input/settings
    Tests/             shooter tests
    Docs/              shooter schemas and implementation notes
  Tests/               shared engine/tool tests
  docs/                shared engine documentation

YourGame/              can be anywhere, including a separate repository
  YourGame.swproject
  CMakeLists.txt
  Source/Game.cpp
  Content/
  Scenes/main.swscene
  Config/Input.swinput
  README.md
  .gitignore
  Saved/               generated/local files, ignored by Git
```

Arena's module, bindings and content are separate from Sample. Opening Sample or a
blank game no longer imports the shooter. The engine's eager import policy within
each project's Content folder is unchanged.

## Create a project

Build Shockwave once, then run this from its checkout. Choose an empty or nonexistent
destination; the generator refuses to overwrite a nonempty folder.

```sh
./build/bin/player --new-project /absolute/path/to/MyGame --name "My Game"
```

This creates `MyGame.swproject`, an independent `project_my_game` C++ module and a
basic scene with a floor, cube, sun and camera. It does not copy Sample or Arena
gameplay, assets, bindings or game circuits. No window or Vulkan device is needed to
create a project.

For an explicit module ID:

```sh
./build/bin/player --new-project /absolute/path/to/MyGame --name "My Game" --module my_game
```

IDs must start with a lowercase ASCII letter and contain only lowercase letters,
digits and underscores. Choose a unique ID among games in one engine build. The ID
is written into both the manifest and the C++ module declaration.

The editor's project hub **Create project** action uses the same generator. It opens
the new scene for editing, but the new C++ module must be registered and built before
Play becomes available. It no longer inherits the currently open game's module.

## Register and build

The current source-build workflow shares an engine build directory. Supply project
folders through the `SHOCKWAVE_GAMES` CMake cache setting; it is a semicolon-separated
list. Paths with spaces work when the complete argument is quoted.

To build just your game, from the engine checkout:

```sh
cmake -S . -B build "-DSHOCKWAVE_GAMES=/absolute/path/to/MyGame"
cmake --build build --target player editor -j4
```

Player/editor depend on the registered game modules, so this also builds your game.
To keep both bundled examples available as well (POSIX shell):

```sh
cmake -S . -B build "-DSHOCKWAVE_GAMES=$PWD/Games/Sample;$PWD/Games/Arena;/absolute/path/to/MyGame"
cmake --build build --target player editor -j4
```

`SHOCKWAVE_GAMES` replaces the configured list; it does not append to it. Fresh engine
builds default to Sample and Arena. Existing build caches retain their configured
list, so add Arena explicitly if upgrading from a Sample-only build. Paths are build
configuration, not edits to engine source. Distinct folder names are not required,
but module IDs must be distinct.

For a dedicated optimized build, use `-DCMAKE_BUILD_TYPE=Release` with a single-config
generator. Visual Studio and other multi-config generators use `--config Release`
when building. The starter is not a precompiled SDK: it uses the engine's source-build
toolchain and dependencies.

## Open, run and validate

```sh
./build/bin/player --project /absolute/path/to/MyGame --check
./build/bin/editor /absolute/path/to/MyGame
./build/bin/player --project /absolute/path/to/MyGame
```

After the initial configure/build, new `.cpp` files under the project's `Source/` are
picked up by CMake. In modular builds, use **Build > Build and reload game code** in
the editor, or build `game_project_my_game` (or `game_my_game` for the explicit ID
above). In static builds, including the default Windows configuration, rebuild and
relaunch player/editor; game-module hot reload is unavailable.

Keep project-specific schema output with the project. For example, after creating
its `Saved/` folder:

```sh
./build/bin/player --project /absolute/path/to/MyGame --schema /absolute/path/to/MyGame/Saved/SceneSchema.md
./build/bin/player --project /absolute/path/to/MyGame --node-schema /absolute/path/to/MyGame/Saved/CircuitNodes.md
```

The root `docs/SceneSchema.md` and `docs/CircuitNodes.md` describe the bundled Sample
context. Arena has its own generated references in `Games/Arena/Docs/`. Do not replace
the engine's checked-in references with another game's components.

## Run the relocated shooter

```sh
./build/bin/player --project Games/Arena/Arena.swproject --vsync off --fps 0
./build/bin/editor Games/Arena/Arena.swproject
./build/bin/player --project Games/Arena/Arena.swproject --check
```

The old `Games/Sample/Scenes/arena.swscene` path has moved to
`Games/Arena/Scenes/arena.swscene`. See the [Arena guide](../Games/Arena/README.md)
and [development walkthrough](../Games/Arena/Docs/Development.md).

## What goes where as the game grows

Put weapons, enemies, scoring, levels, game UI and game tests in your project. Use
registered components for inspector/schema access, and validate mistakes through
`player --check`. Put reusable actor groups in `.swprefab` assets and visual scripts
in `.swcircuit` assets inside Content. Use the existing [packaging](Packaging.md)
workflow when ready to distribute a game.

Only change shared engine modules when fixing an engine defect or introducing a
general-purpose capability. For example, the prefab-unload and frame-timing fixes
found while building Arena remain engine changes; its rifle and team rules do not.

For an optional asset/rendering workload project, see [Render Lab](../Games/RenderLab/README.md).
It keeps local asset-library derivatives outside Git and provides reproducible scene generators
and benchmark profiles alongside its game code.
