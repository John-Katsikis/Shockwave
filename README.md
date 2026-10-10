# Shockwave

[![Build](https://github.com/John-Katsikis/Shockwave/actions/workflows/build.yml/badge.svg)](https://github.com/John-Katsikis/Shockwave/actions/workflows/build.yml)

Shockwave is a free, open source 3D game engine written in C++20. It draws with Vulkan,
uses every CPU core by default, and comes with an editor for building scenes and playing
them. It's MIT licensed with no royalties.

Shockwave runs on Linux today. Windows and macOS (through MoltenVK, a layer that runs
Vulkan on Apple's Metal) are planned. If you know Unity or Unreal, much of it will feel
familiar: [Architecture](docs/Architecture.md) maps Shockwave's ideas to Unreal's.

## Get started

### 1. Install the system packages

On CachyOS or Arch (the main development platform), install the build tools and libraries:

```
sudo pacman -S --needed base-devel cmake ninja git glslang \
    vulkan-icd-loader vulkan-validation-layers vulkan-tools \
    libx11 libxext libxrandr libxcursor libxi libxtst libxss libxkbcommon \
    wayland wayland-protocols libdecor
```

You also need the Vulkan driver for your graphics card. CachyOS usually installs it already.

| GPU | Package |
|---|---|
| AMD | `vulkan-radeon` |
| Intel | `vulkan-intel` |
| NVIDIA | `nvidia-utils` |

To check that Vulkan sees your GPU, run this. It prints a short summary of your driver:

```
vulkaninfo --summary
```

A few notes:

- `glslang` is optional. Without it, the build compiles glslang from source.
- The editor's Open and Save As dialogs use `xdg-desktop-portal`, which comes with KDE and
  GNOME. If it's missing, they fall back to `zenity`.
- Other distros need the same things under their own package names. SDL keeps a list for
  each one: https://wiki.libsdl.org/SDL3/README-linux#build-dependencies

You don't need to install any other libraries. CMake downloads them during the build.

### 2. Build

From the repository folder, configure a debug build and then compile it:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The programs end up in `build/bin`.

### 3. Open the editor

This opens the editor on the included sample project:

```
./build/bin/editor Games/Sample
```

Press Ctrl+P to play the scene and Ctrl+P again to stop. Press F1 to see every control.
[Use the editor](#use-the-editor) below explains the windows and shortcuts.

### 4. Make your first game

Follow [Tutorial: your first game in Shockwave](docs/Tutorial.md). It walks you through
making a project, building a scene and writing your first game code.

### Run the tests

The unit tests need no GPU. They cover entities, scheduling, parenting and scene files:

```
ctest --test-dir build
```

## What Shockwave can do

Each item links to the guide that covers it in detail.

- **Write game code in C++.** A whole game module can be one file. You define components
  as plain structs and add systems that run across every core. See
  [Game projects](docs/Projects.md).
- **Change code while the editor runs.** Rebuild your game code, the renderer or its
  shaders, and the editor reloads them in place. Your scene and undo history are kept.
  See [Plugins and hot reload](docs/Plugins.md).
- **Light your scenes.** Directional, point and spot lights with physically based
  shading, and soft shadows from all three. See [Shadows](docs/Shadows.md).
- **Import models and textures.** Drop glTF, FBX, OBJ and other models into your
  project's `Content/` folder and place them in the editor. See [Assets](docs/Assets.md).
- **Simulate physics.** Rigid bodies, triggers, raycasts and a character controller,
  powered by Jolt. See [Physics](docs/Physics.md).
- **Play sound.** WAV and Ogg clips with 3D positioning. See [Audio](docs/Audio.md).
- **Use any controller.** Gamepads, flight sticks, wheels and more, bound to named
  actions. See [Controllers](#use-controllers) below and the
  [input schema](docs/InputSchema.md).
- **Keep frames smooth.** A precise frame limiter, a low-latency mode and frame
  statistics. See [Frame pacing](docs/FramePacing.md).
- **Write scenes as text.** Scenes are plain text, so people, scripts and AI tools can
  write them, and you can check them from the command line. See
  [Authoring with tools and AI](docs/Projects.md#authoring-with-tools-and-ai) and the
  [scene schema](docs/SceneSchema.md).
- **Restyle the tools.** The editor's look comes from plain text theme files that reload
  as you save. See [The tool UI](docs/UI.md).

## Use the editor

### Open a project

You can start the editor in three ways:

```
./build/bin/editor                                  # the project in or above the working folder
./build/bin/editor Games/Sample                     # a project folder or .swproject file
./build/bin/editor Games/Sample/Scenes/main.swscene # a scene inside a project
```

If you give no project and there's none in or above the working folder, the editor
reopens your last project. If there's no last project, it shows the project hub. The hub
lists recent projects and has two buttons:

- **Create project** writes `<Name>.swproject`, `Content/`, `Scenes/`, a `.gitignore`
  and a starter scene.
- **Open the sample project** opens `Games/Sample`.

### Find your way around

The editor is a set of docked windows around a live view of the scene, called the viewport.

| Window | What it does |
|---|---|
| Command bar | Left: the open scene, with Save, Undo and Redo. Center: Play, Pause and Stop. Right: the current mode (edit, simulation running, or paused). |
| Viewport toolbar | Left: Move, Rotate, Scale, World/Local and Snap (right-click Snap for step sizes). Right: Grid, Game/Editor camera and View options (field of view, fly speed, render scale, overlays, shading view, Save screenshot). Toolbars sit above the scene, never on top of it. |
| Hierarchy | Every entity as a tree, with its kind on the right. Search, select several, rename in place, drag to attach and detach, and press Ctrl+G to group. |
| Inspector | The entity's name and a + Component button at the top. Components are searchable and grouped by Core, Rendering, Physics, Motion and Gameplay. Each component shows as a grid of properties. Reset and Remove are in each component's "..." menu. Physics problems and imported materials show under their components. |
| Content | Your project's `Content/` and `Scenes/` folders, plus the engine's built-in meshes and textures. Search, import, make folders, drag meshes and models into the viewport, and drop textures on objects. Files saved from other programs reload straight away. |
| Console | Everything logged with `SW_LOG` by the engine and your game, with info, warning and error filters. |
| Status bar | The scene's save state, how many entities are selected, the entity count and the frame time. |

More windows live in the menus:

- **Window > Stats**: a frame time graph, render graph passes and barriers, light count,
  the system schedule and Jolt physics counters.
- **Window > Input**: every controller live, and the input bindings.
- **Edit > Preferences** and **Project > Project settings**.

### Play your scene

Every scene should have an entity with a **Camera** component. Play looks through the
main (primary) camera, and the standalone player uses the same camera, so what you frame
in the editor is what ships. Press F8 to switch between the scene camera and the editor
camera.

Play runs your project's game code and Jolt physics on the scene. Stop puts the scene
back exactly as it was, so nothing you do while playing sticks. The colored frame around
the viewport reminds you that you're playing. Play is turned off when the project's
`game_module` isn't the one compiled into the editor.

### Add lights

Lighting is part of the scene. **Create > Light** adds a directional sun, a point light
or a spot light in front of the camera. Each light shows as a colored icon you can click.
The selected light draws its direction, range or cone.

- Rotate a light to aim it. It shines along its -Z axis.
- The **Environment** entity sets the sky and ground ambient light, exposure in stops,
  and the tonemapper: ACES, AgX, Reinhard or None. A tonemapper turns the scene's bright
  HDR colors into colors your screen can show.
- **MeshRenderer** has roughness, metallic and emissive settings.
- A scene with no lights gets a default sun.

**View > Shading** switches the viewport to Unlit, Normals or Lighting only.
**View > Save screenshot** saves the viewport, without icons or gizmos, to the project's
`Saved/Screenshots/` folder.

### Editor controls

Help > Controls (or F1) lists these too.

| Action | Keys |
|---|---|
| Fly the camera | Hold the right mouse button in the viewport: mouse to look, WASD to move, E/Q up and down, Shift to go faster, mouse wheel to change speed |
| Dolly and pan | Mouse wheel dollies, middle mouse drag pans |
| Select | Left click selects. Ctrl toggles, Shift adds. Shift+click in the Hierarchy selects a range. |
| Move, rotate, scale | W, E, R switch the gizmo. T toggles world or local axes. Hold Ctrl while dragging to flip snapping. |
| Focus and rename | F (or double-click in the Hierarchy) focuses the selection. F2 renames. |
| Grid and camera | G toggles the grid. F8 switches between the scene camera and the editor camera. |
| Edit the selection | Ctrl+D duplicates, Del deletes, Ctrl+G groups, Ctrl+A selects all, Esc selects none, Ctrl+Shift+F moves the selection to the view |
| Undo and files | Ctrl+Z undo, Ctrl+Y redo, Ctrl+N new, Ctrl+O open, Ctrl+S save |
| Play | Ctrl+P plays and stops, Ctrl+Shift+P pauses |
| Preferences | Ctrl+, |

A gamepad also flies the editor camera while the viewport has focus. The left stick
moves, the right stick looks, and the triggers go up and down.

### Where your work is saved

| What | Where | In git? |
|---|---|---|
| Scenes (`.swscene`) and the project manifest | Your project folder, as plain text | Yes |
| Per-user state: the open scene, the editor camera for each scene, autosaves | The project's `Saved/` folder | No, the generated `.gitignore` leaves it out |
| Preferences: theme, autosave interval, camera speed, grid | `$XDG_CONFIG_HOME/shockwave/editor.swprefs` (usually under `~/.config`), shared by all projects | No |
| Window layout, including detached windows | `editor-layout.ini`, next to the preferences | No |

Unsaved changes are autosaved to `Saved/`. When you reopen the scene, the editor offers
to recover them. Window > Reset layout restores the default layout.

### Arrange the windows

You can dock any window anywhere, stack windows as tabs, or drag a window out of the
editor by its tab or title bar onto any monitor. Drag it back onto the editor to dock it
again.

To keep windows inside the editor, turn off Preferences > Panels can leave the main window
(`detachable_panels = false`). Wayland doesn't let programs place their own windows. So
on a Wayland desktop, while this option is on, the editor runs through XWayland. Set
`SDL_VIDEO_DRIVER=wayland` to force native Wayland, with windows kept inside the editor.

### Change the editor's look

The editor and in-game UI take their look from `themes/default.swtheme`, a dark
"telemetry console" theme: an obsidian background, matte slate panels, square controls
with 1px borders, an electric cyan accent and amber for secondary data. It's plain
`key = value` text with every setting commented. Save it while the editor runs and the UI
restyles at once.

Two more themes come with the engine. Try them like this:

```
./build/bin/editor --theme themes/ember.swtheme       # neutral graphite with a burnt orange accent
./build/bin/editor --theme themes/instrument.swtheme  # a cyan engineering-panel palette
```

Games pick their theme with `EngineConfig::guiTheme`. Run from the repository root to
edit `themes/` live. The build also copies the themes next to the programs.

Engine tools share one style: square controls, crisp one-pixel borders, compact toolbars
and aligned property grids. Themes change the colors and spacing, not that structure.
New ImGui widgets pick up the style by themselves, and `UI/Gui.h` has shared helpers for
actions, toolbars and property grids. Contributors and AI agents should follow
[the UI guide](docs/UI.md) when they add options.

### Change the frame rate in the editor

The editor runs with vsync on, since an editor mostly sits idle. Vsync waits for your
screen's refresh before showing each frame. To render as fast as possible, untick
Preferences > VSync, or start the editor with `--uncapped`:

```
./build/bin/editor Games/Sample --uncapped
```

## Run your game

A project's `.swproject` file sets the startup scene, the content folder, simulation
settings and the C++ game module. The sample's is `Games/Sample/Sample.swproject`. The
editor and the standalone player run the same compiled game code:

```
./build/bin/editor Games/Sample --play                    # open the editor and start playing
./build/bin/player --project Games/Sample/Sample.swproject  # run the game without the editor
./build/bin/player                                        # no project around the working folder: runs Games/Sample
```

Project paths work from any working folder when you give the manifest path.
[Game projects](docs/Projects.md) covers the manifest format, your own game module and
current limits.

### Write game code

Game code is a `GameMode`, the rules for one Play of a scene. A `GameInstance`, which
lives for the whole run, picks the GameMode. Both sit on top of the entities and systems
described in [How entities and components work](#how-entities-and-components-work).

The sample's GameMode, in `Games/Sample/Source/SampleGame.cpp`, adds a ColorPulse
behavior to the Crate during Play. Stop restores the scene you built.

Registering a component adds it to scene files and gives it an inspector in the editor,
with no UI code. In a game module, that's one line next to the struct:

```cpp
struct Velocity { glm::vec3 value{0}; };
SW_COMPONENT(Velocity, SW_FIELD(value))
```

[Game projects](docs/Projects.md#write-gameplay-code) shows a complete one-file game module.

### Reload code while the editor runs

Use Build > Build and reload game code (Ctrl+Alt+B), or rebuild `game_<id>` from anywhere.
The editor picks up the new code and keeps the scene and undo history. The renderer
reloads the same way (Ctrl+Alt+R, or rebuild `renderer_vulkan`), so shader edits show up
live. See [Plugins and hot reload](docs/Plugins.md).

### Use controllers

PlayStation (DualSense, DualShock 4), Xbox, Switch and Steam pads work out of the box,
with rumble, trigger rumble, light bar color and gyro. Flight sticks, throttles, wheels,
pedals and button boxes work as joysticks, bound by part of their name.

Game code reads named actions such as "jump". The bindings live in the project's
`Config/Input.swinput` file and reload when it changes.

- In the editor, **Window > Input** shows every controller live and edits the bindings.
  **+ Listen** binds the next button or axis you press.
- `player --check` checks the bindings file for mistakes.
- The [input schema](docs/InputSchema.md) lists the file format and every name.
  `player --input-schema` regenerates it.

This lists your controllers, then prints each input you press for 10 seconds as the line
that would bind it:

```
./build/bin/player --devices 10
```

### Try physics

The editor's Play and the standalone player share Jolt rigid bodies, collision shapes,
triggers, raycasts and an upright character controller. They run at the project's fixed
tick rate. You edit the components in the inspector and they save with the scene. Stop
restores the scene you built.

This opens the physics sample and starts playing:

```sh
./build/bin/editor Games/Sample/Scenes/physics.swscene --play
```

See [Physics](docs/Physics.md) for setting up components, the gameplay API, collision
masks, threading rules, and which shapes and parenting setups work.

### Import models and materials

Drop glTF/GLB, FBX, OBJ and other models into the project's `Content/` folder. Then place
them from the **Content** window. Shockwave imports them with Assimp, an open source model
loader. Meshes with several parts, the source materials, embedded or external textures
and reloading when a file changes all work the same in the editor and the player.

Modern `.blend` files are converted to GLB first, using a Blender install if you have
one. See [Assets](docs/Assets.md) for setup, supported features and limits.

### Add sound

WAV and Ogg clips in your project's `Content/` folder import and reload by themselves.
AudioSource components give you volume, pitch, looping and 3D playback heard from the
primary Camera. Preview clips in the inspector or the Content window, and set the master
volume in Preferences.

The sample loops a CC0 (public domain) ambient tone and plays a short tone on `pulse`.
See [Audio](docs/Audio.md) for the gameplay API, command-line checks and current limits.

### Measure frame pacing

Shockwave has a precise frame limiter, a low-latency mode, and present timing where the
driver supports it. Its frame statistics track the 1% and 0.1% lows, the slowest frames,
not just the average. Set these in Preferences or Project settings, or with `frame_limit`
and `low_latency` in the `.swproject`. Watch them in **Window > Stats**.

This runs a 10-second benchmark and saves the results as JSON:

```sh
./build/bin/player --benchmark 10 --json bench.json
```

To profile every thread with the Tracy profiler, build with `-DSHOCKWAVE_PROFILE=ON`.
See [Frame pacing](docs/FramePacing.md).

## Check scenes and take screenshots

Scenes are plain text, one property per line, so they diff and merge cleanly in git, and
people, scripts and AI can write them too. Links to other entities, like a Parent, use
the target's name. Options use their names too, as in `shape "Convex model"`. Scene files
from the first editor still open, and save in the new format.

[SceneSchema.md](docs/SceneSchema.md) lists every component, field, default and asset
name. `player --schema` regenerates it.

This checks a project's manifest and scenes for mistakes:

```
./build/bin/player --check
```

With no project given, it checks the project around the working folder, or else the
sample. It prints each problem as `file:line: problem` and exits with an error code if
it finds any. See [Authoring with tools and AI](docs/Projects.md#authoring-with-tools-and-ai).

The player can also render a scene with no one watching. This is how tools and AI agents
check how a scene looks:

```sh
./build/bin/player --scene Games/Sample/Scenes/main.swscene --screenshot shot.png --size 1280x720
./build/bin/player --scene Games/Sample/Scenes/main.swscene --screenshot normals.png --view normals
./build/bin/player --frames 5 --print-frame   # print the render graph's passes and barriers
```

| Option | What it does |
|---|---|
| `--screenshot file.png` | Saves a rendered frame to the file |
| `--size WxH` | Picks the resolution, such as `1280x720` |
| `--view` | Picks what to draw: `lit`, `unlit`, `normals` or `lighting` |
| `--frames N` | Picks which frame is captured (default 30) |

## Run the sandbox

The sandbox is a small test scene that shows off the renderer and the entity system.
This runs it:

```
./build/bin/sandbox
```

You'll see 10,000 cubes. They're entities moved by the engine's Spin and Bob systems. The
window title shows the frame rate, frame time, render resolution and draw count.

Controls: hold the right mouse button to look, WASD to move, E/Q (or Space/Ctrl) to go up
and down, Shift to go faster, scroll to change speed, and Esc to quit.

| Option | What it does |
|---|---|
| `--grid 100` | Sets the cube grid (100 = 10,000 cubes) |
| `--scale 0.5` | Renders at half resolution |
| `--vsync` | Caps the frame rate to the display |
| `--gui` | Shows a Dear ImGui debug overlay |
| `--frames 300` | Quits after 300 frames |

## Log messages

Engine and game code log with `SW_LOG` from `Core/Log.h`:

```cpp
SW_LOG(LogCategory, Level, "format {}", args);
```

The engine logs under `LogEngine`, `LogRender`, `LogPhysics` and `LogGame`. A game adds
its own category with `SW_LOG_CATEGORY(LogMyGame)`. Messages go to standard error, and
the editor shows them in its Console.

## Find Vulkan mistakes

Debug builds turn on the Vulkan validation layers if they're installed. These print any
misuse of the Vulkan API to the terminal. For a stricter check that also covers
synchronization, run with this environment variable:

```
VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1 ./build/bin/editor Games/Sample
```

## How it works

This part is for the curious. You don't need it to make a game.

### Design principles

- **Multithreaded from the start.** A work-stealing job system runs on every core. Engine
  APIs say whether you can call them from worker threads, and the busy ones (creating
  meshes, submitting draws) are safe.
- **Modern GPU techniques only, no legacy paths.** Shockwave uses Vulkan 1.3 with dynamic
  rendering, synchronization2, timeline semaphores, buffer device address with vertex
  pulling, a GPU draw table indexed per draw, bindless textures, and reverse-Z depth with
  an infinite far plane.
- **Entities and components, scheduled across cores.** Systems declare what they read and
  write. The scheduler runs systems that don't conflict side by side, and splits each
  one's entities across the job system. Where they conflict, the order is fixed.
- **Frame-rate independent.** The simulation ticks at a fixed rate, 60 Hz by default.
  Rendering blends between ticks, so gameplay is the same at 30 or 500 fps. The frame
  rate is uncapped by default, using mailbox presentation, so there's no tearing.
- **Resolution and aspect independent.** The scene renders into its own HDR image at a
  render scale you choose, then is scaled to whatever size and shape the window is.
- **Distro agnostic.** CMake fetches every library. Vulkan and the window system are
  found when the program starts.

### How entities and components work

Shockwave uses an entity component system, or ECS. A `World` holds entities, and an
entity is just an id. Components are plain structs. Any copyable type works, with no base
class or registration needed to store it:

```cpp
struct Velocity { glm::vec3 value; };

shockwave::World world;
shockwave::Entity e = world.create();
world.add<shockwave::Transform>(e, {{0, 1, 0}});
world.add<Velocity>(e, {{1, 0, 0}});
```

Systems go in a `Schedule`. A system lists the components it touches, and marks the ones
it only reads as `const`. The schedule works out what can run in parallel:

```cpp
shockwave::Schedule schedule;
schedule.each<shockwave::Transform, const Velocity>("Move",
    [](const shockwave::SystemContext& ctx, shockwave::Transform& t, const Velocity& v) {
        t.position += v.value * ctx.dt;
    });
// each tick (Application::onUpdate):
schedule.run(world, engine().jobs(), dt);
```

Each `each()` system splits its entities across every core. Systems that write different
components run at the same time. To create or destroy entities from inside a system, go
through `ctx.commands()`. Those changes apply when the tick ends. `renderWorld()` draws
every `MeshRenderer`, blending between ticks for smooth motion at any frame rate.

Inside a game module you'd normally use the shorter form from `<Shockwave/Game.h>`, with
`SW_COMPONENT` and `SW_GAME`. See [Game projects](docs/Projects.md#write-gameplay-code).

Built-in components include `Name`, `Transform`, `Parent`, `MeshRenderer`,
`ModelRenderer`, `Camera`, `Light`, `Environment` (sky and lighting), `Spin` and `Bob`,
plus the physics and audio components. [SceneSchema.md](docs/SceneSchema.md) lists them all.

### Parent one entity to another

An entity with a `Parent` moves, turns and scales with it: its `Transform` is relative to
the parent's. `Engine/Hierarchy.h` has the helpers:

```cpp
shockwave::setParent(world, wheel, car);         // attach without the wheel jumping
glm::mat4 placed = shockwave::worldMatrix(world, wheel);
shockwave::destroyWithChildren(world, car);      // car and wheels
```

Rendering works out each entity's world matrix on the job system by walking up its
parents. So there's no ordering to manage and nothing to keep in sync.

### How assets load

`AssetFolder` imports a folder of source files into `Assets`, named by their path inside
it, such as `textures/bricks.png`. It re-imports a file when it changes on disk. Images
(PNG, JPEG, TGA, BMP, PSD, GIF, HDR) become textures, and decoding runs in parallel on
the job system.

### How a frame is drawn

Each frame is a small render graph (`Source/Runtime/RHI/Public/RHI/RenderGraph.h`).
Passes declare which images they read and write, and the graph places the
synchronization2 barriers, the GPU's "wait for this first" markers.

The scene renders to an HDR image with forward PBR shading: GGX specular, Lambert
diffuse, up to 256 directional, point and spot lights, and hemisphere ambient light.
Then a tonemap pass applies exposure and the chosen curve, writing an sRGB image at the
window or viewport size.

The sun casts soft cascaded shadows, and spot and point lights cast shadows from a shared
atlas. See [Shadows](docs/Shadows.md). Bloom and upscaling aren't done yet.

### Repository layout

```
Shockwave/
  CMakeLists.txt        top-level build; fetches dependencies
  .clang-format         code style
  cmake/                build helpers (engine modules, shader compilation, Dear ImGui)
  Source/
    Runtime/            the engine, one library per module
      Core/               job system, logging, frame pacing, profiling
      Platform/           window
      Input/              keyboard, mouse, gamepads, joysticks and action mapping
      RHI/                Vulkan helpers: device, swapchain, pipelines, render graph
      Renderer/           the Renderer games use, the RenderBackend interface, built-in meshes
      Assets/             asset names, built-in assets, image and model import, the asset folder watcher
      UI/                 Dear ImGui helpers and the theme loader
      Projects/           .swproject manifests
      Engine/             entities and components (World), parenting, system scheduler,
                          scene files, camera, subsystems and the engine loop
      Audio/              clips, mixing and spatial sound sources
      Physics/            Jolt physics (PhysicsWorld)
      GameFramework/      GameMode, GameInstance, GameModule, PlaySession
    Plugins/            renderer plugins, loaded at runtime and hot-swappable (docs/Plugins.md)
      RendererVulkan/     the Vulkan renderer; engine shaders (GLSL) in Shaders/
      RendererNull/       a headless renderer for servers and tools
    Editor/Editor/      Shockwave Editor (Dear ImGui), built on the engine
    Programs/Player/    the standalone player and its command-line tools
    Programs/Sandbox/   a small test scene that uses the engine
  Games/Sample/         the sample project
    Sample.swproject      the manifest
    Config/               input bindings (Input.swinput)
    Content/              source files the editor imports (images, models, audio), watched for changes
    Scenes/               scene files (.swscene, plain text)
    Source/               the sample's game module (SampleGame.cpp), hot reloaded by the editor
    Saved/                per-user editor state and screenshots, not in git
  Tests/                unit tests that need no GPU (ctest --test-dir build); fixtures/ holds test files
  themes/               UI themes (.swtheme, plain text)
```

Each folder in `Source/Runtime` is a module. `Public/<Module>/` holds its API and
`Private/` holds the rest. Code includes a module's headers by module name, such as
`#include <Engine/Components.h>`.

Each module builds as the library `Shockwave<Module>`, with the alias
`Shockwave::<Module>` (`cmake/ShockwaveModule.cmake`). It's a shared library by default,
or static with `-DSHOCKWAVE_MODULAR=OFF`. A module only uses the modules listed before it
in `Source/Runtime/CMakeLists.txt`. [Architecture](docs/Architecture.md) has the details.

Format code with `clang-format -i`. Group includes in this order: the file's own header,
local headers, Shockwave modules, third-party, then the standard library.

### Tech stack

| Part | What it uses |
|---|---|
| Language and build | C++20 and CMake |
| Physics | Jolt Physics 5.6.0, for rigid bodies, triggers and character controllers |
| Windows, input, audio, timing | SDL3, on every platform |
| Graphics | Vulkan 1.3, loaded at runtime by volk and set up with vk-bootstrap |
| GPU memory and math | VMA for GPU memory, GLM for math |
| Editor and debug UI | Dear ImGui (docking branch), with ImGuizmo for gizmos |
| Shaders | GLSL, compiled to SPIR-V at build time |

## Roadmap

1. Window, Vulkan device and swapchain, fixed-timestep game loop (done)
2. Job system, mesh upload, bindless vertex pulling, HDR render target with render scale,
   uncapped interpolated rendering (done)
3. 3D basics: reverse-Z depth, back-face culling, fly camera, keyboard and mouse input
   with action mapping, bindless textures with GPU mipmaps, GPU draw table (done)
4. Gamepads, flight sticks and wheels (done). Next: GPU-driven rendering, with frustum
   culling and indirect draws generated on the GPU
5. Scene: entities and components, with systems scheduled on the job system (done)
6. Render graph, tonemapping, lighting and shadows (done), and audio (done). Next:
   upscaling, glTF assets in the asset browser
7. Hot reload of game code, renderers and shaders (done), and swappable renderer plugins
   (done)
8. Editor (first version done): Dear ImGui docking UI, viewport rendered by the engine at
   panel size, hierarchy, inspector, move/rotate/scale gizmos, click to select,
   undo/redo, plain-text scenes, editing engine entities with a generated inspector, play
   mode, hot-reloadable UI themes, entity parenting, asset browser with image import and
   hot reload, multi-select (done). Next: glTF meshes in the asset browser
9. Windows and macOS ports
