# Shockwave

[![Build](https://github.com/John-Katsikis/Shockwave/actions/workflows/build.yml/badge.svg)](https://github.com/John-Katsikis/Shockwave/actions/workflows/build.yml)

A modern 3D game engine in C++20, rendering natively with Vulkan. Open source (MIT),
no royalties. Linux first, with Windows and macOS (through MoltenVK) as later ports.

## Design principles

- **Multithreaded from the start.** A work-stealing job system runs on every core.
  Engine APIs say whether they are safe to call from worker threads, and the hot ones are
  (mesh creation, draw submission).
- **Modern GPU techniques only, no legacy paths.** Vulkan 1.3 with dynamic rendering,
  synchronization2, timeline semaphores, buffer device address with vertex pulling,
  a GPU draw table indexed per draw, bindless textures, and reverse-Z depth with an
  infinite far plane.
- **Entities and components, scheduled across cores.** Systems declare what they read
  and write; the scheduler runs non-conflicting systems side by side and splits each
  one's entities across the job system, with a deterministic order where they conflict.
- **Frame-rate independent.** Simulation ticks at a fixed rate (60 Hz by default) and
  rendering interpolates between ticks, so gameplay is identical at 30 or 500 fps.
  The frame rate is uncapped by default (mailbox present, no tearing).
- **Resolution and aspect independent.** The scene renders into its own HDR target at a
  configurable render scale, then is scaled to whatever size and shape the window is.
- **Distro agnostic.** Every library is fetched by CMake; Vulkan and the window system
  are found at runtime.

## Stack

- **C++20 + CMake**
- **Jolt Physics 5.6.0** for rigid bodies, triggers and character controllers
- **SDL3**: windows, input, audio and timing on every platform
- **Vulkan 1.3**, loaded at runtime by **volk**; setup through **vk-bootstrap**
- **VMA** for GPU memory, **GLM** for math
- **Dear ImGui** (docking branch) for the editor and in-game debug UI, **ImGuizmo** for gizmos
- **GLSL** shaders compiled to SPIR-V at build time

## Layout

```
Shockwave/
  CMakeLists.txt        top-level build; fetches dependencies
  .clang-format         code style
  cmake/                build helpers (engine modules, shader compilation, Dear ImGui)
  Source/
    Runtime/            the engine, one static library per module
      Core/               job system, logging
      Platform/           window
      Input/              keyboard, mouse, gamepads, joysticks and action mapping
      RHI/                Vulkan backend: device, swapchain, pipelines, render graph
      Renderer/           renderer, built-in meshes; engine shaders (GLSL) in Shaders/
      Assets/             asset names, built-in assets, image and model import, the asset folder watcher
      UI/                 Dear ImGui helpers and the theme loader
      Projects/           .swproject manifests
      Engine/             entities and components (World), parenting, system scheduler,
                          scene files, camera, subsystems and the engine loop
      Physics/            Jolt physics (PhysicsWorld)
      GameFramework/      GameMode, GameInstance, GameModule, PlaySession
    Editor/Editor/      Shockwave Editor (Dear ImGui), built on the engine
    Programs/Player/    the standalone player and its command-line tools
    Programs/Sandbox/   a small test scene that uses the engine
  Games/Sample/         the sample project
    Sample.swproject      the manifest
    Content/              source files the editor imports (images and Assimp models), watched for changes
    Scenes/               scene files (.swscene, plain text)
    Source/               the sample's game module (SampleGame.cpp)
    Saved/                per-user editor state and screenshots, not in git
  Tests/                unit tests that need no GPU (ctest --test-dir build); fixtures/ holds test files
  themes/               UI themes (.swtheme, plain text)
```

Each folder in `Source/Runtime` is a module: `Public/<Module>/` is its API and `Private/`
the rest. Code includes a module's headers by module name, such as
`#include <Engine/Components.h>`. Each module builds as the static library
`Shockwave<Module>` with the alias `Shockwave::<Module>` (`cmake/ShockwaveModule.cmake`),
and only uses the modules listed before it in `Source/Runtime/CMakeLists.txt`.
[docs/Architecture.md](docs/Architecture.md) has the details. Format code with
`clang-format -i`; includes go in groups: own header, local headers, Shockwave modules,
third-party, standard library.

## Build and run

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/bin/sandbox
```

The window title shows frame rate, frame time, render resolution and draw count.
The sandbox's 10,000 cubes are entities moved by the engine's Spin and Bob systems.

Run the unit tests (entities, scheduling, parenting, scene files) with `ctest --test-dir build`.

Engine and game code log with `SW_LOG(LogCategory, Level, "format {}", args)` from
`Core/Log.h`. The engine logs under `LogEngine`, `LogRender`, `LogPhysics` and `LogGame`;
a game adds its own with `SW_LOG_CATEGORY(LogMyGame)`. Messages go to standard error,
and the editor shows them in its Console.

## Entities and components

A `World` holds entities, and an entity is just an id. Components are plain structs; any
copyable type works, with no base class or registration needed to store it:

```cpp
struct Velocity { glm::vec3 value; };

shockwave::World world;
shockwave::Entity e = world.create();
world.add<shockwave::Transform>(e, {{0, 1, 0}});
world.add<Velocity>(e, {{1, 0, 0}});
```

Systems go in a `Schedule`. A system lists the components it touches. Ones it only reads
are marked `const`. The schedule works out what can run in parallel:

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
components run at the same time. Creating or destroying entities from inside a system
goes through `ctx.commands()`, and those changes apply when the tick ends.
`renderWorld()` draws every `MeshRenderer`, blending between ticks for smooth motion at any
frame rate.

Registering a component adds it to scene files and gives it an editor inspector, with no
UI code:

```cpp
shockwave::registerComponent<Velocity>("Velocity", {shockwave::field("value", &Velocity::value)});
```

Built-in components: `Name`, `Transform`, `Parent`, `MeshRenderer`, `Environment` (sky
color), `Spin` and `Bob`.

An entity with a `Parent` moves, turns and scales with it: its `Transform` is relative to
the parent's. `Engine/Hierarchy.h` has the helpers:

```cpp
shockwave::setParent(world, wheel, car);         // attach without the wheel jumping
glm::mat4 placed = shockwave::worldMatrix(world, wheel);
shockwave::destroyWithChildren(world, car);      // car and wheels
```

Rendering works out each entity's world matrix on the job system by walking up its
parents, so there's no ordering to manage and nothing to keep in sync.

`AssetFolder` imports a folder of source files into `Assets`, by their path inside it
(`textures/bricks.png`), and re-imports a file when it changes on disk. Images (PNG, JPEG,
TGA, BMP, PSD, GIF, HDR) become textures; decoding runs in parallel on the job system.

Controls: hold the right mouse button to look, WASD to move, E/Q (or Space/Ctrl) up and
down, Shift to go faster, scroll to change speed, Esc to quit.

Sandbox options: `--grid 100` sets the cube grid (100 = 10,000 cubes), `--scale 0.5`
renders at half resolution, `--vsync` caps to the display, `--gui` shows a Dear ImGui
debug overlay, `--frames 300` quits after 300 frames.

### Editor

```
./build/bin/editor                                  # the project in or above the working folder
./build/bin/editor Games/Sample                     # a project folder or .swproject file
./build/bin/editor Games/Sample/Scenes/main.swscene # a scene inside a project
```

With no project given and none in or above the working folder, the editor reopens the
last project, or else starts on the project hub: recent projects, Create project (writes
`<Name>.swproject`, `Content/`, `Scenes/`, a `.gitignore` and a starter scene) and Open
the sample project (`Games/Sample`).

The editor is a docked workspace around a live viewport:

- **Command bar**: the open scene with Save, Undo and Redo on the left, Play, Pause and
  Stop in the center, and the current mode (edit, simulation running, paused) on the right.
- **Viewport toolbar**: Move, Rotate, Scale, World/Local and Snap (right-click for step
  sizes) on the left; Grid, Game/Editor camera and View options (fov, fly speed, render
  scale, overlays, shading view, Save screenshot) on the right. Toolbars sit above the scene, never on top of it.
- **Hierarchy**: search, entities as a tree with their kind on the right, multi-select,
  rename in place, drag to attach and detach, Ctrl+G to group.
- **Inspector**: name and + Component at the top (searchable and grouped by Core,
  Rendering, Physics, Motion and Gameplay), then each component as a property grid.
  Reset and Remove live in each component's "..." menu. Physics diagnostics and imported
  materials show under their components.
- **Content**: the project's `Content/` and `Scenes/` folders plus the engine's built-in
  meshes and textures. Search, import, new folder, drag meshes and models into the
  viewport, drop textures on objects. Files saved from other programs reload straight away.
- **Console** with info, warning and error filters (it shows everything logged with
  `SW_LOG`, from the engine and the game), and a status bar with the scene's
  save state, selection count, entity count and frame time.
- Window > Stats (frame graph, render graph passes and barriers, light count, system
  schedule, Jolt physics counters), Edit >
  Preferences and Project > Project settings.

Every scene should have an entity with a **Camera** component. Play looks through the
primary camera (F8 switches back to the editor camera), and the player uses the same
camera, so what you frame in the editor is what ships. Play runs the project's game
module and Jolt physics on the scene; Stop restores it exactly, so nothing done while
playing sticks (the colored frame around the viewport is the reminder). Play is disabled
when the project's `game_module` isn't the one compiled into the editor.

Lighting is part of the scene. **Create > Light** adds a directional sun, a point light
or a spot light in front of the camera; each shows as a colored icon you can click, and
the selected one draws its direction, range or cone. Rotate a light to aim it (it shines
along its -Z). The **Environment** entity sets the sky and ground ambient, exposure in
stops and the tonemapper (ACES, AgX, Reinhard or None), and **MeshRenderer** has
roughness, metallic and emissive. View > Shading switches the viewport to Unlit, Normals
or Lighting only, and View > Save screenshot writes the viewport (without icons or
gizmos) to the project's `Saved/Screenshots/` folder. A scene with no lights gets a default sun.

What gets saved where:

- Scenes (`.swscene`) and the manifest are plain text and belong in git.
- Per-user state (the open scene, the editor camera per scene, autosaves) goes in the
  project's `Saved/` folder, which the generated `.gitignore` excludes. Unsaved
  changes are autosaved there; reopening offers to recover them.
- Preferences (theme, autosave interval, camera speed, grid) go in
  `$XDG_CONFIG_HOME/shockwave/editor.swprefs` (usually `~/.config`), shared by all projects.
- The dock layout goes in `editor-layout.ini` next to the preferences; Window > Reset
  layout restores the default.

Editor controls (Help > Controls, or F1, lists them too):

- Hold the right mouse button in the viewport to fly: mouse to look, WASD to move,
  E/Q up and down, Shift faster, mouse wheel changes speed
- Mouse wheel dollies, middle mouse drag pans
- Left click selects; Ctrl toggles, Shift adds; Shift+click in the Hierarchy selects a range
- W / E / R switch the gizmo to move / rotate / scale; T toggles world or local axes;
  hold Ctrl while dragging to flip snapping
- F (or double-click in the hierarchy) focuses the selection; F2 renames; G toggles the
  grid; F8 switches between the scene camera and the editor camera
- Ctrl+D duplicates, Del deletes, Ctrl+G groups, Ctrl+A / Esc select all / none,
  Ctrl+Shift+F moves the selection to the view
- Ctrl+Z / Ctrl+Y undo and redo; Ctrl+N / Ctrl+O / Ctrl+S new, open, save
- Ctrl+P plays and stops; Ctrl+Shift+P pauses; Ctrl+, opens Preferences

Scenes are plain text, one property per line, so they diff and merge cleanly in git,
and so people, scripts and AI can write them too. Links to other entities (a Parent)
use the target's name, and options use their names (`shape "Convex model"`).
[docs/SceneSchema.md](docs/SceneSchema.md) lists every component, field, default and
asset name (`player --schema` regenerates it), and `player --check` validates a
project's manifest and scenes (with no project given, the one around the working folder,
else the sample), printing `file:line: problem` and exiting non-zero on
any problem. See [Authoring with tools and AI](docs/Projects.md#authoring-with-tools-and-ai).
Scene files from the first editor still open, and are saved in the new format.

### UI themes

The editor and in-game UI take their look from `themes/default.swtheme`: a minimal dark
theme in the style of Linear and Vercel, with graphite panels, 1px borders and an indigo
accent. It's plain `key = value` text with every setting commented. Save it while the
editor is running and the UI restyles at once. `themes/ember.swtheme` is the same look
with a burnt orange accent: `./build/bin/editor --theme themes/ember.swtheme`.
Games pick their theme with `EngineConfig::guiTheme`. Run from the repository root to
edit `themes/` live; the build also copies them next to the executables.
The editor runs with vsync on (an editor mostly sits idle); untick VSync in the Renderer
panel or start it with `--uncapped` to render flat out.

Debug builds turn on the Vulkan validation layers if they are installed and print
any API misuse to the terminal. For a stricter check, run with
`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1` to enable synchronization validation too.

### System packages

CachyOS / Arch (the main development platform):

```
sudo pacman -S --needed base-devel cmake ninja git glslang \
    vulkan-icd-loader vulkan-validation-layers vulkan-tools \
    libx11 libxext libxrandr libxcursor libxi libxtst libxss libxkbcommon \
    wayland wayland-protocols libdecor
```

Plus the Vulkan driver for your GPU: `vulkan-radeon` (AMD), `vulkan-intel` (Intel)
or `nvidia-utils` (NVIDIA). CachyOS usually installs this already; `vulkaninfo --summary` shows it.

`glslang` is optional: without it the build compiles glslang from source.
The editor's Open and Save As dialogs use `xdg-desktop-portal` (installed with KDE and
GNOME) or fall back to `zenity`.
Other distros need the same things under their own names; SDL keeps a list for each:
https://wiki.libsdl.org/SDL3/README-linux#build-dependencies

## Milestones

1. Window, Vulkan device and swapchain, fixed-timestep game loop (done)
2. Job system, mesh upload, bindless vertex pulling, HDR render target with render scale,
   uncapped interpolated rendering (done)
3. 3D basics: reverse-Z depth, back-face culling, fly camera, keyboard and mouse input with
   action mapping, bindless textures with GPU mipmaps, GPU draw table (done)
4. Gamepads, flight sticks and wheels (done), and GPU-driven rendering: frustum culling and indirect draws generated on the GPU
5. Scene: entities and components, with systems scheduled on the job system (done)
6. Render graph, tonemapping and lighting (done); shadows, upscaling, glTF assets in the
   asset browser, audio
7. Hot reload of game code and shaders
8. Editor (first version done): Dear ImGui docking UI, viewport rendered by the engine at
   panel size, hierarchy, inspector, move/rotate/scale gizmos, click to select, undo/redo,
   plain-text scenes, editing engine entities with a generated inspector, play mode,
   hot-reloadable UI themes, entity parenting, asset browser with image import and hot
   reload. Next: glTF meshes in the asset browser, multi-select
9. Windows and macOS ports

### Shared tools UI

Engine tools use square controls, crisp one-pixel borders, compact toolbars, and aligned
property grids. Colors and density come from `.swtheme` files; changing a palette keeps
the same structural style. Try `./build/bin/editor --theme themes/instrument.swtheme`
for the cyan engineering-panel palette, or `themes/ember.swtheme` for orange.

New ImGui widgets inherit the global style automatically. Shared action, toolbar, and
property-grid helpers are available in `UI/Gui.h`. Contributors and AI agents
should follow [the UI guide](docs/UI.md) when adding options.

### Game projects and player

A project's `.swproject` (the sample's is `Games/Sample/Sample.swproject`) defines the
startup scene, content folder, simulation settings, and C++ game module. The editor and
standalone player share the same compiled gameplay code:

```
./build/bin/editor Games/Sample --play
./build/bin/player --project Games/Sample/Sample.swproject
./build/bin/player                # no project around the working folder: runs Games/Sample
```

Game code is a `GameMode` (the rules of one Play) picked by the project's
`GameInstance` (lives for the whole run), on top of the entities and systems above.
The sample's `GameMode` (`Games/Sample/Source/SampleGame.cpp`) adds a ColorPulse
behavior to the Crate during Play. Stop restores
the authored scene. Project paths work from any working directory when the manifest
path is supplied. See [the project guide](docs/Projects.md) for the manifest format,
your own game module, the GameMode hooks, and current runtime limitations.

### Rendering and lighting

Each frame is a small render graph (`Source/Runtime/RHI/Public/RHI/RenderGraph.h`): passes declare
which images they read and write and the graph places the synchronization2 barriers.
The scene renders to an HDR target with forward PBR shading (GGX specular, Lambert
diffuse, up to 256 directional, point and spot lights, hemisphere ambient), then a
tonemap pass applies exposure and the chosen curve into an sRGB image at the window or
viewport size. Shadows, bloom and upscaling are not done yet.

The player can render without anyone looking, which is how tools and AI agents check
how a scene looks:

```sh
./build/bin/player --scene Games/Sample/Scenes/main.swscene --screenshot shot.png --size 1280x720
./build/bin/player --scene Games/Sample/Scenes/main.swscene --screenshot normals.png --view normals
./build/bin/player --frames 5 --print-frame   # the render graph's passes and barriers
```

`--view` takes `lit`, `unlit`, `normals` or `lighting`; `--frames N` picks which frame is
captured (default 30).

### Imported models and materials

Drop glTF/GLB, FBX, OBJ and other Assimp models into the project's `Content/` folder,
then place them from the **Content** panel. Multipart meshes, source materials,
embedded/external textures and dependency reloads are shared by editor and player.
Modern `.blend` import uses an optional Blender executable to convert to GLB first.
See [Assets](docs/Assets.md) for setup, supported rendering features and limitations.

### Physics

Editor Play and the standalone player share Jolt rigid bodies, collision shapes,
triggers, raycasts, and an upright character controller at the project fixed tick.
Components are editable and saved in scenes; Stop restores the authored state.

```sh
./build/bin/editor Games/Sample/Scenes/physics.swscene --play
```

See [Physics](docs/Physics.md) for component setup, gameplay APIs, collision masks,
threading rules, and the supported shape and parenting constraints.

### Controllers

PlayStation (DualSense, DualShock 4), Xbox, Switch and Steam pads work out of the box, with
rumble, trigger rumble, light bar color and gyro. Flight sticks, throttles, wheels, pedals and
button boxes work as joysticks, bound by part of their name. Game code reads named actions;
the bindings live in the project's `Config/Input.swinput` and reload when it changes.

- In the editor, **Window > Input** shows every controller live and edits the bindings:
  **+ Listen** binds the next button or axis you press.
- `player --devices 10` lists controllers, then prints each input you press for 10 seconds as
  the line that binds it. `player --check` validates the bindings file.
- [Input schema](docs/InputSchema.md) (`player --input-schema`) lists the format and every name.

A gamepad also flies the editor camera while the viewport has focus: left stick moves, right
stick looks, triggers go up and down.

### Audio

WAV and Ogg clips in project Content import and reload automatically. AudioSource
components provide volume, pitch, looping and spatial playback through the primary
Camera. Preview clips in the inspector or Content browser; set master volume in
Preferences. The sample loops a CC0 ambient tone and plays a short tone on `pulse`.
See [Audio](docs/Audio.md) for gameplay APIs, CLI validation and current limits.
