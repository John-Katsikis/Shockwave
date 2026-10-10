# Architecture

Shockwave is laid out like Unreal Engine, so the structure feels familiar if you come from
there. Underneath, it uses an ECS rather than Unreal's object model, so game code runs
across all cores by default.

## Repository

```
Source/
  Runtime/     modules that ship with games
  Editor/      the editor (Shockwave's UnrealEd)
  Programs/    executables: Player (the game and its CLI tools), Sandbox
Games/Sample/  the sample project: Sample.swproject, Content/, Scenes/, Source/
Tests/         CTest suites and fixtures
themes/        editor UI themes (.swtheme)
docs/          guides; SceneSchema.md is generated
cmake/         dependency, shader and module helpers
```

## Modules

Each folder in `Source/Runtime` is one module, built as its own static library
(`Shockwave::<Module>`) by `shockwave_module()` in `cmake/ShockwaveModule.cmake`:

```
Source/Runtime/Renderer/
  CMakeLists.txt          shockwave_module(Renderer PUBLIC_DEPS ... PRIVATE_DEPS ...)
  Public/Renderer/*.h     the API, included as <Renderer/Renderer.h>
  Private/*.cpp           the implementation
```

Modules depend only on those above them in the table below. CMake enforces this: a
module sees only the headers of modules it depends on, directly or through them.

| Module | What it holds | Depends on | Unreal counterpart |
|---|---|---|---|
| Core | job system, logging | GLM | Core |
| Platform | window and OS events (SDL3) | Core | ApplicationCore |
| Input | keyboard and mouse actions | Platform | InputCore |
| RHI | Vulkan device, swapchain, pipelines, render graph | Platform | RHI, VulkanRHI |
| Renderer | meshes, textures, lights, the frame | Core, Platform; RHI privately | Renderer |
| Assets | asset registry, image decode, model import, hot reload | Renderer | AssetRegistry |
| UI | `shockwave::gui` controls, themes (Dear ImGui) | Core | Slate |
| Projects | `.swproject` manifest | Core | Projects |
| Engine | World, components, systems, hierarchy, scene files, subsystems, engine loop | everything above | Engine |
| Physics | Jolt bodies, characters, queries | Engine | PhysicsCore, Chaos |
| GameFramework | GameModule, GameInstance, GameMode, PlaySession | Engine, Physics | Engine's GameFramework |

Vulkan never reaches game or editor code, because only the Renderer links RHI.

## Gameplay model

This mirrors Unreal's vocabulary, built on the ECS:

- **Entities and components** take the place of Actors and their components. An entity
  with a `Transform` is what Unreal calls an Actor. Components are plain structs,
  registered once (`registerComponent`). That registration drives the inspector, the
  scene files, `player --schema` and `player --check`.
- **Systems** do the work. A `Schedule` runs them in parallel stages worked out from the
  components each system reads and writes. There's no per-entity virtual `Tick`.
- **GameModule** is how a project's C++ plugs in, like Unreal's primary game module.
  It registers the game's components, creates its assets and makes its GameInstance.
- **GameInstance** lives for the whole run of the editor or player. It picks the
  GameMode for each scene.
- **GameMode** sets the rules of one Play. It adds systems and gets `beginPlay`,
  `fixedUpdate`, `frame` and `endPlay`.
- **Subsystems** are services found by type (`ctx.subsystem<T>()`). Physics is the
  first one. Each Play session has its own collection, and the GameInstance has one
  that lasts the whole run.
- **PlaySession** runs a scene the way Play In Editor does. It snapshots the World,
  plays it, and restores it exactly on Stop. The editor and the player use the same
  class.

## A frame

`Engine::run` drives a fixed simulation tick and an uncapped render loop:

1. **Events.** Window events and input are handled, along with the UI on the main
   thread.
2. **Fixed tick** (project `tick_rate`), zero or more times per frame:
   - the GameMode's systems run on the job system;
   - then the physics step;
   - then `GameMode::fixedUpdate`.
3. **Render.** `renderWorld` gathers draws and lights in parallel and interpolates
   between ticks. The Renderer then builds a render graph:
   - the scene pass (HDR);
   - tonemapping;
   - the editor UI, or a copy to the window;
   - present.

   The graph places all the Vulkan barriers.

## Threads

- The main thread owns the window, the UI, scene loading, and starting and stopping
  Play.
- Systems, draw gathering, asset decoding and physics run on the job system
  (`Core/Jobs.h`). A system may only touch the components it declares.
- `Renderer::drawMesh`, `Renderer::addLight` and `SW_LOG` are safe from any thread.

## Where things go

- **A new component:** a struct in the module that owns the data, registered with
  `registerComponent`. Then regenerate `docs/SceneSchema.md` and add a `player --check`
  rule if mistakes are likely.
- **A new engine service:** a Subsystem in its own module.
- **A new render pass:** add it to the frame graph in
  `Source/Runtime/Renderer/Private/RendererFrame.cpp` and declare its image uses.
- **A new editor panel:** a `*Panel.cpp` in `Source/Editor/Editor/Private`, built
  from the `shockwave::gui` controls (`docs/UI.md`).
- **Game code:** goes in the project's `Source/`, next to its `.swproject`.
