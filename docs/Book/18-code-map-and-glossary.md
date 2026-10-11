# 18. Map of the code, and a glossary

Keep this page handy. The first half says where everything lives; the second half
explains every word the book used.

## The folders

```text
Source/
  Runtime/     the engine modules, one folder each (below)
  Plugins/     code loaded while running: RendererVulkan, RendererNull
  Editor/      the editor
  Programs/    Player (the game and its tools), Sandbox
Games/Sample/  the sample project and its game code
Tests/         automated tests
themes/        editor themes (.swtheme)
docs/          the guides, and this book
cmake/         build helpers: modules, plugins, shaders, third-party libraries
```

## The engine modules

Each folder in `Source/Runtime` is one module, built as its own library. A module may
only use the ones above it in this table, so lower layers never depend on higher ones.

| Module | Owns | Book chapter |
|---|---|---|
| **Core** | the job system, logging (`SW_LOG`), frame pacing, profiling, plugin loading | 6, 7, 14 |
| **Platform** | the window and OS events (SDL3) | 7 |
| **Input** | keys, mouse and controllers behind named actions | 5 |
| **RHI** | the Vulkan device, swapchain, pipelines and render graph; only the Vulkan plugin uses it | 13 |
| **Renderer** | `Renderer`: meshes, textures, lights, the frame; swaps backend plugins | 13 |
| **Assets** | importing images and models, compression, LODs, reloading changed files | 17 |
| **UI** | the editor's look: `gui::Theme`, `.swtheme` files, shared controls | 15 |
| **Projects** | reading `.swproject` manifests | 3 |
| **Packaging** | turning a project into a folder for players | 17 |
| **Engine** | the World, components, `Schedule`, hierarchy, scene files, prefabs, and the main loop | 4, 6, 7, 9 |
| **Audio** | mixing and spatial sound (SDL3, Steam Audio) | 10 |
| **Physics** | bodies, characters, raycasts (Jolt) | 8 |
| **GameUI** | menus and HUDs from `.rml`/`.rcss` (RmlUi) | 11 |
| **Navigation** | navmeshes, paths and crowds (Recast & Detour) | |
| **Effects** | particle effects (Effekseer) | |
| **Animation** | skeletal animation (ozz-animation) | |
| **GameFramework** | `GameModule`, `GameInstance`, `GameMode`, `PlaySession`, save games | 5, 12 |

Each module has the same shape: `Public/<Module>/` holds the headers other code may
include (as `#include <Engine/World.h>`), and `Private/` holds everything else.

## Where to look when you want to...

| ...understand or change | Look in |
|---|---|
| the main loop | `Source/Runtime/Engine/Private/Engine.cpp`, `Engine::run` |
| Play, Stop and the tick order | `Source/Runtime/GameFramework/Private/PlaySession.cpp` |
| how systems run in parallel | `Source/Runtime/Engine/Public/Engine/Schedule.h` |
| what game code can reach | `GameContext` in `GameFramework/GameMode.h` |
| the built-in components | `Source/Runtime/Engine/Public/Engine/Components.h` |
| the GPU frame | `Source/Plugins/RendererVulkan/Private/RendererFrame.cpp` |
| the editor's panels | `Source/Editor/Editor/Private/*Panel.cpp` |
| the editor's frame and dock layout | `Source/Editor/Editor/Private/Workspace.cpp` |
| the editor's theme | `Source/Runtime/UI/Public/UI/Gui.h`, `themes/default.swtheme` |
| the command-line tools | `Source/Programs/Player/Private/` |
| the rules for contributors and AI agents | `AGENTS.md` |

## Where the libraries come from

`CMakeLists.txt` downloads every library during the first configure, pinned to exact
versions, so nothing needs installing by hand: SDL3, the Vulkan headers, volk, VMA,
vk-bootstrap, GLM, stb_image, Assimp, Jolt, Dear ImGui (docking branch) and ImGuizmo,
RmlUi, Recast & Detour, Steam Audio, ozz-animation and Effekseer. Tracy, the profiler,
is only fetched when you turn on `SHOCKWAVE_PROFILE`.

## Glossary

| Word | Meaning |
|---|---|
| **Accumulator** | The bucket of unspent time the loop fills each frame and empties one tick at a time. |
| **Action** | A named input, like `jump`, bound to keys and buttons in `Input.swinput`. |
| **Alpha** | How far between the last two ticks a frame is drawn, 0 to 1. Used to blend motion. |
| **Asset** | A file the game loads: an image, model, sound, prefab or UI document. Named by its path inside `Content/`. |
| **Backend** | The plugin behind `Renderer` that actually draws: Vulkan or null. |
| **Barrier** | A GPU sync point: "finish writing this image before reading it". The render graph places them. |
| **Command buffer** | In a system, a queue of entity changes applied after all systems finish (`ctx.commands()`). |
| **Component** | Plain data attached to an entity. |
| **Dockspace** | The area panels dock into. The whole editor window is one. |
| **ECS** | Entity component system: data in components, behavior in systems. |
| **Engine** | The object that owns the window, renderer, input and job system, and runs the loop. |
| **Entity** | An ID for a thing in the world. It's nothing until components are attached. |
| **EntityRef** | An entity plus its World, with handy `get`, `add`, `has` and `destroy`. |
| **Fixed tick** | One step of the simulation, always the same length (1/60 s by default). |
| **Frame** | One pass of the loop, ending with a picture on screen. |
| **GameContext** | What every GameMode hook gets: the world, input, audio, physics, UI, saves. |
| **GameInstance** | Lives for the whole run; makes a GameMode for each Play. |
| **GameMode** | The rules for one Play: systems and hooks. |
| **Game module** | Your compiled game code, a plugin the editor and player load. |
| **HDR** | High dynamic range: colors brighter than white, before tonemapping. |
| **Hot reload** | Replacing code (game or renderer) while the program keeps running. |
| **Immediate mode** | UI that's redrawn from your data every frame, with no widget objects (Dear ImGui). |
| **Job system** | The pool of worker threads, one per CPU core, that systems and drawing run on. |
| **LOD** | Level of detail: a simpler version of a mesh for when it's far away. |
| **Manifest** | The `.swproject` file. |
| **Module** | One engine library, like Engine or Physics. |
| **Pass** | One step of drawing a frame on the GPU, such as shadows or tonemap. |
| **PlaySession** | Runs one Play: snapshots the World, ticks it, restores it on Stop. |
| **Plugin** | A library loaded while running, that can be swapped or reloaded. |
| **Prefab** | A reusable template of entities, in a `.swprefab` file. |
| **Primary camera** | The camera the game looks through. Every scene needs one. |
| **Render graph** | The list of passes for a frame, with the images each uses. |
| **Scene** | A World saved as a `.swscene` text file. |
| **Schedule** | Runs systems in parallel stages, based on what each reads and writes. |
| **Subsystem** | A service you find by type, like physics or audio. |
| **System** | A function that runs on every entity with certain components. |
| **Theme** | The editor's colors, borders, spacing and font, from `gui::Theme` and a `.swtheme` file. |
| **Tonemapper** | Squeezes HDR colors into what a screen can show. |
| **World** | The container of all entities and components. |

Back to [the contents](README.md).
