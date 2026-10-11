# Architecture

This page shows how Shockwave's code is organized, so you know where to find things and
where new code should go. The layout follows Unreal Engine, so it will feel familiar if
you come from there.

Underneath, Shockwave uses an ECS (entity component system) instead of Unreal's object
model. Your game's data lives in small components, and systems process them in parallel,
so game code uses all CPU cores by default.

If you just want to make a game, start with
[Tutorial: your first game in Shockwave](Tutorial.md) and [Projects](Projects.md).

## Where things go

The most common changes, and where they belong:

| You want to add | Put it here |
|---|---|
| Game code | The project's `Source/`, next to its `.swproject`. It's built by `shockwave_game_module(<id> ...)`. See [Projects](Projects.md). |
| A new component | A struct in the module that owns the data. Register it with `registerComponent` in engine code, or `SW_COMPONENT` in a game module. Then regenerate `docs/SceneSchema.md`, and add a `player --check` rule if mistakes are likely. |
| A new engine service | A Subsystem in its own module. |
| A new editor panel | A `*Panel.cpp` in `Source/Editor/Editor/Private`, built from the `shockwave::gui` controls. See [UI](UI.md). |
| A new render pass | The frame graph in `Source/Plugins/RendererVulkan/Private/RendererFrame.cpp`. Declare which images it uses. |
| A new renderer | A plugin that implements `RenderBackend` (`Renderer/RenderBackend.h`). `Source/Plugins/RendererNull` is the smallest example. See [Plugins](Plugins.md). |

To regenerate the scene schema after adding a component:

```sh
build/bin/player --schema docs/SceneSchema.md
```

To try a new render pass without restarting the editor, build `renderer_vulkan`, then use
Build > Reload renderer:

```sh
cmake --build build --target renderer_vulkan
```

## Find your way around the repository

```text
Source/
  Runtime/     modules that ship with games
  Plugins/     code loaded at runtime: the renderers (Vulkan, headless)
  Editor/      the editor (Shockwave's UnrealEd)
  Programs/    executables: Player (the game and its CLI tools), Sandbox
Games/Sample/  the sample project: Sample.swproject, Content/, Scenes/, Source/ (its game module)
Tests/         CTest suites and fixtures
themes/        editor UI themes (.swtheme)
docs/          guides; SceneSchema.md and InputSchema.md are generated
cmake/         dependency, shader and module helpers
```

## Gameplay model

Shockwave borrows Unreal's names, but builds them on the ECS. Here is how the terms map:

| Shockwave | Unreal | Unity | What it is |
|---|---|---|---|
| Entity with a `Transform` | Actor | GameObject | A thing in the scene. |
| Component | Component | Component | Plain data, with no behavior of its own. |
| System | Tick | `Update` | A function that runs on every entity with certain components. |
| GameModule | Primary game module | | How a project's C++ plugs in. |
| GameInstance | GameInstance | | Lives for the whole run. |
| GameMode | GameMode | | The rules for one Play. |
| Subsystem | Subsystem | | A service you look up by type. |
| PlaySession | Play In Editor | Play mode | Runs a scene and restores it on Stop. |

In more detail:

- **Entities and components.** Components are plain structs, registered once with
  `registerComponent`, or `SW_COMPONENT` in game code. That registration drives the
  Inspector, scene files, `player --schema` and `player --check`.
- **Systems** do the work. A `Schedule` runs them in parallel stages. It works out the
  stages from the components each system reads and writes. There's no per-entity
  virtual `Tick`.
- **GameModule** registers the game's components, creates its assets and makes its
  GameInstance. It's a plugin the editor loads for the open project, and reloads when
  you rebuild it.
- **GameInstance** lives for the whole run of the editor or player. It picks the
  GameMode for each scene.
- **GameMode** sets the rules of one Play. It adds systems and gets `beginPlay`,
  `fixedUpdate`, `frame` and `endPlay` calls.
- **Subsystems** are services you find by type with `ctx.subsystem<T>()`. Physics and
  Audio are session services. Each Play session has its own set, and the GameInstance
  has one that lasts the whole run.
- **PlaySession** runs a scene the way Play In Editor does. It takes a snapshot of the
  World, plays it, and restores it exactly on Stop. The editor and the player use the
  same class.

## Modules

Each folder in `Source/Runtime` is one module, built as its own library named
`Shockwave::<Module>`. The CMake function `shockwave_module()` in
`cmake/ShockwaveModule.cmake` builds it.

A module looks like this:

```text
Source/Runtime/Renderer/
  CMakeLists.txt          shockwave_module(Renderer PUBLIC_DEPS ... PRIVATE_DEPS ...)
  Public/Renderer/*.h     the API, included as <Renderer/Renderer.h>
  Private/*.cpp           the implementation
```

A module may only depend on modules above it in the table below. CMake enforces this: a
module can only see the headers of modules it depends on, directly or through them.

| Module | What it holds | Depends on | Unreal counterpart |
|---|---|---|---|
| Core | job system, logging, frame pacing, profiling macros (Tracy), plugin loading | GLM | Core |
| Platform | window and OS events (SDL3) | Core | ApplicationCore |
| Input | keyboard, mouse, gamepads and joysticks behind named actions; `.swinput` bindings | Platform | InputCore, Enhanced Input |
| RHI | Vulkan device, swapchain, pipelines, render graph (static, inside the Vulkan plugin) | Platform | RHI, VulkanRHI |
| Renderer | meshes, textures, lights, the frame; forwards to a renderer plugin it can swap | Core, Platform | Renderer |
| Assets | asset registry, image decode, model import, hot reload | Renderer | AssetRegistry |
| UI | `shockwave::gui` controls, themes (Dear ImGui) | Core | Slate |
| Projects | `.swproject` manifest | Core | Projects |
| Engine | World, components, systems, hierarchy, scene files, subsystems, engine loop | everything above | Engine |
| Audio | SDL3 mixing, spatial sources, session playback | Engine | AudioMixer |
| Physics | Jolt bodies, characters, queries | Engine | PhysicsCore, Chaos |
| GameUI | menus and HUDs from `.rml`/`.rcss` (RmlUi, FreeType, HarfBuzz), `UIDocument`, `game.ui()` | Engine | UMG, Slate |
| Animation | skeletal animation (ozz-animation): `Animator`, clips, cross-fades, `game.animation()` | Engine | AnimGraph, Animation Blueprint |
| GameFramework | GameModule, GameInstance, GameMode, PlaySession | Engine, Physics, Audio, GameUI, Animation | Engine's GameFramework |

Vulkan, the graphics API, never reaches game or editor code. Only the Vulkan renderer
plugin links RHI.

In modular builds, each module is a shared library in `build/bin`. Modular builds are
the default on Linux and macOS, controlled by `SHOCKWAVE_MODULAR`. Otherwise modules are
static libraries, linked into each program.

## Plugins

Some code is loaded while the program runs instead of being built in. You can then swap
it, or rebuild and reload it while the editor runs. [Plugins](Plugins.md) has the
details.

| Plugin | Built as | What it is |
|---|---|---|
| `renderer_vulkan` | `bin/plugins/renderer_vulkan.so` (`Source/Plugins/RendererVulkan`) | the renderer: forward PBR, shadows, render graph, Dear ImGui |
| `renderer_null` | `bin/plugins/renderer_null.so` (`Source/Plugins/RendererNull`) | headless: draws nothing, for servers and tools |
| `game_<id>` | `bin/modules/game_<id>.so` (a project's `Source/`) | a project's game module |

Games use `Renderer`, which stays the same. A `RenderBackend` plugin behind it does the
drawing.

## How a frame runs

This section and the next are for the curious.

`Engine::run` drives a fixed simulation tick and a render loop. The render loop runs as
fast as it can, unless VSync or a frame limit slows it down:

1. **Events.** Window events and input are handled, along with the UI, on the main
   thread.
2. **Fixed tick.** This runs zero or more times per frame, at the project's `tick_rate`:
   - the GameMode's systems run on the job system;
   - then the physics step;
   - then `GameMode::fixedUpdate`;
   - then audio takes a snapshot of the updated listener and sources for the SDL mixing
     thread.
3. **Render.** `renderWorld` gathers what to draw and the lights, in parallel. It blends
   positions between ticks so motion looks smooth. The Renderer then builds a render
   graph:
   - the scene pass, in HDR (high dynamic range);
   - tonemapping, which maps HDR colors to the screen;
   - the game UI, drawn at the output's own resolution (see [Game UI](GameUI.md));
   - the editor UI, or a copy to the window;
   - present, which shows the frame.

   The render graph places all the Vulkan barriers, the sync points between GPU passes.

## Threads

- The main thread owns the window, the UI, scene loading, and starting and stopping
  Play.
- Systems, draw gathering, asset decoding and physics run on the job system
  (`Core/Jobs.h`). A system may only touch the components it declares.
- `Renderer::drawMesh`, `Renderer::addLight` and `SW_LOG` are safe to call from any
  thread.
