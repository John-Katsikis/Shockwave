# Shockwave

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
  assets/               source files the editor imports (images and Assimp models), watched for changes
  cmake/                build helpers (shader compilation, Dear ImGui)
  engine/               the engine, built as a static library (shockwave)
    include/shockwave/    public API games include (no Vulkan types in here)
    shaders/              engine shaders (GLSL)
    src/core/             job system, asset names, image import and the asset folder watcher
    src/input/            keyboard, mouse and action mapping
    src/render/           camera, built-in meshes
    src/scene/            entities and components (World), parenting, system scheduler, scene files
    src/ui/               Dear ImGui helpers and the theme loader
    src/vulkan/           Vulkan backend: device, swapchain, pipelines, memory
  editor/               Shockwave Editor (Dear ImGui), built on the engine
  sandbox/              a small test scene that uses the engine
  scenes/               scene files (.swscene, plain text)
  tests/                unit tests that need no GPU (ctest --test-dir build)
  themes/               UI themes (.swtheme, plain text)
```

## Build and run

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/bin/sandbox
```

The window title shows frame rate, frame time, render resolution and draw count.
The sandbox's 10,000 cubes are entities moved by the engine's Spin and Bob systems.

Run the unit tests (entities, scheduling, parenting, scene files) with `ctest --test-dir build`.

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
// each tick (Game::onUpdate):
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
the parent's. `Hierarchy.h` has the helpers:

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
./build/bin/editor                       # from a project folder: opens its shockwave.swproject
./build/bin/editor path/to/project       # a project folder or .swproject file
./build/bin/editor path/to/level.swscene # a scene inside a project
```

With no project to open, the editor starts on the project hub: recent projects, Create
project (writes `shockwave.swproject`, `assets/`, `scenes/`, a `.gitignore` and a starter
scene) and Open the sample project.

The editor is a docked workspace around a live viewport:

- **Command bar**: the open scene with Save, Undo and Redo on the left, Play, Pause and
  Stop in the center, and the current mode (edit, simulation running, paused) on the right.
- **Viewport toolbar**: Move, Rotate, Scale, World/Local and Snap (right-click for step
  sizes) on the left; Grid, Game/Editor camera and View options (fov, fly speed, render
  scale, overlays) on the right. Toolbars sit above the scene, never on top of it.
- **Hierarchy**: search, entities as a tree with their kind on the right, multi-select,
  rename in place, drag to attach and detach, Ctrl+G to group.
- **Inspector**: name and + Component at the top (searchable and grouped by Core,
  Rendering, Physics, Motion and Gameplay), then each component as a property grid.
  Reset and Remove live in each component's "..." menu. Physics diagnostics and imported
  materials show under their components.
- **Content**: the project's `assets/` and `scenes/` folders plus the engine's built-in
  meshes and textures. Search, import, new folder, drag meshes and models into the
  viewport, drop textures on objects. Files saved from other programs reload straight away.
- **Console** with info, warning and error filters, and a status bar with the scene's
  save state, selection count, entity count and frame time.
- Window > Stats (frame graph, system schedule, Jolt physics counters), Edit >
  Preferences and Project > Project settings.

Every scene should have an entity with a **Camera** component. Play looks through the
primary camera (F8 switches back to the editor camera), and the player uses the same
camera, so what you frame in the editor is what ships. Play runs the project's game
module and Jolt physics on the scene; Stop restores it exactly, so nothing done while
playing sticks (the colored frame around the viewport is the reminder). Play is disabled
when the project's `game_module` isn't the one compiled into the editor.

What gets saved where:

- Scenes (`.swscene`) and the manifest are plain text and belong in git.
- Per-user state (the open scene, the editor camera per scene, autosaves) goes in the
  project's `.shockwave/` folder, which the generated `.gitignore` excludes. Unsaved
  changes are autosaved there; reopening offers to recover them.
- Preferences (theme, autosave interval, camera speed, grid) go in
  `$XDG_CONFIG_HOME/shockwave/editor.swprefs` (usually `~/.config`), shared by all projects.
- The dock layout goes in `~/.local/share/Shockwave/Shockwave Editor/`; Window > Reset
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

Scenes are plain text, one property per line, so they diff and merge cleanly in git.
A parent is saved as the position of its entity in the file (`entity 6` is the seventh
`entity` block).
Scene files from the first editor still open, and are saved in the new format.

### UI themes

The editor and in-game UI take their look from `themes/default.swtheme`: a minimal dark
theme in the style of Linear and Vercel, with graphite panels, 1px borders and an indigo
accent. It's plain `key = value` text with every setting commented. Save it while the
editor is running and the UI restyles at once. `themes/ember.swtheme` is the same look
with a burnt orange accent: `./build/bin/editor --theme themes/ember.swtheme`.
Games pick their theme with `EngineConfig::guiTheme`.
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
4. Gamepads, and GPU-driven rendering: frustum culling and indirect draws generated on the GPU
5. Scene: entities and components, with systems scheduled on the job system (done)
6. Render graph, tonemapping and upscaling, lighting, glTF assets, audio
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
property-grid helpers are available in `shockwave/Gui.h`. Contributors and AI agents
should follow [the UI guide](docs/UI.md) when adding options.

### Game projects and player

`shockwave.swproject` defines the startup scene, project assets, simulation settings,
and C++ gameplay module. The editor and standalone player share the same compiled
gameplay code:

```
./build/bin/editor --project shockwave.swproject --play
./build/bin/player --project shockwave.swproject
```

The sample module adds a ColorPulse behavior to the Crate during Play. Stop restores
the authored scene. Project paths work from any working directory when the manifest
path is supplied. See [the project guide](docs/Projects.md) for the manifest format,
your own game module, lifecycle hooks, and current runtime limitations.

### Imported models and materials

Drop glTF/GLB, FBX, OBJ and other Assimp models into the project assets folder,
then place them from **Assets → Models**. Multipart meshes, source materials,
embedded/external textures and dependency reloads are shared by editor and player.
Modern `.blend` import uses an optional Blender executable to convert to GLB first.
See [Assets](docs/Assets.md) for setup, supported rendering features and limitations.

### Physics

Editor Play and the standalone player share Jolt rigid bodies, collision shapes,
triggers, raycasts, and an upright character controller at the project fixed tick.
Components are editable and saved in scenes; Stop restores the authored state.

```sh
./build/bin/editor scenes/physics.swscene --play
```

See [Physics](docs/Physics.md) for component setup, gameplay APIs, collision masks,
threading rules, and the supported shape and parenting constraints.
