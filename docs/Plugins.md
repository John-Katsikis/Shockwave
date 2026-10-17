# Plugins and hot reload

In Shockwave, renderers and game code are plugins. A plugin is a library the engine
loads while it runs, instead of building it into the program. This lets you change
your game code or the renderer and see the result without restarting the editor.

Plugins give you three things:

- **Game code hot reload.** Edit your game's C++, rebuild, and the editor reloads it in
  place. The open scene, the selection and the undo history are kept. If Play was
  running, it restarts.
- **Renderer hot reload.** Rebuild the renderer, or just its shaders, and reload it
  without restarting. Meshes, textures and settings carry over.
- **Swappable renderers.** Switch to a different renderer while running, or start the
  player headless (with no graphics) for servers and tools.

## Reload your game code

The quickest way is Build > Build and reload game code in the editor, or Ctrl+Alt+B. You
can also build from a terminal, with `<id>` being your project's `game_module`:

```sh
cmake --build build --target game_<id>
```

The editor reloads whatever lands on disk, so a rebuild from anywhere counts: an IDE, a
terminal, or an AI agent running CMake. The Console says what was reloaded and how long
it took, or why it couldn't. A build that fails to compile or load never replaces the
code that's running.

| To do this | In the editor | In a text file | On the command line |
|---|---|---|---|
| Build and reload game code | Build > Build and reload game code (Ctrl+Alt+B) | | `cmake --build build --target game_<id>` |
| Reload game code as it is on disk | Build > Reload game code | | |
| Reload game code by itself after any rebuild | Build > Reload when rebuilt, or Preferences > Plugins (on by default) | `auto_reload_game_code` in the editor preferences | |
| Load a game module from a file | | | `player --game-module path/to/game_x.so` |

To set up a game module in the first place, see [Projects](Projects.md).

## Reload or switch the renderer

Use Build > Reload renderer in the editor, or Ctrl+Alt+R. To rebuild the Vulkan renderer
from a terminal:

```sh
cmake --build build --target renderer_vulkan
```

To see which renderers are available:

```sh
build/bin/player --renderers
```

| To do this | In the editor | In a text file | On the command line |
|---|---|---|---|
| Reload the renderer | Build > Reload renderer (Ctrl+Alt+R) | | `cmake --build build --target renderer_vulkan` |
| Reload the renderer by itself after a rebuild | Build > Reload renderer when rebuilt, or Preferences > Plugins (on by default) | `auto_reload_renderer` in the editor preferences | |
| Pick the editor's renderer | Build > Renderer, or Preferences > Plugins > Renderer | `renderer` in the editor preferences | `editor --renderer vulkan` |
| Pick the player's renderer | Project settings | `renderer "vulkan"` in the `.swproject` | `player --renderer null` |
| List renderers | Build > Renderer | | `player --renderers` |

`player --check` and `player --schema` start on the headless renderer, so they don't
need a GPU. `--check` also reports a project whose `renderer` isn't available.

## What survives a game code reload

The editor stops Play first, and starts it again once the new code is in. Then it:

1. saves the scene, the edit in progress, and every undo and redo step as scene text;
2. loads the new library next to the old one. If that fails, it stops here and the old
   code keeps running;
3. clears those worlds while the old code is still loaded, so the old code can free its
   components. Then it unloads the old code;
4. reads the worlds back with the same entities, so handles, the selection and undo all
   still line up. Game components come back as the new code's types. Fields you added
   start at their defaults, and fields you removed are dropped;
5. makes a new GameInstance and runs the module's `createAssets` again.

Anything that isn't saved in scenes doesn't survive. That includes components that
aren't registered, and state your GameInstance keeps elsewhere.

If your module hooks into the engine in other ways than `registerTypes`, `createAssets`
and its GameInstance, undo that in `GameInstance::shutdown`. A custom log sink is one
example.

## What survives a renderer swap

Games and tools talk to the `Renderer`, and that stays in place. Only the
`RenderBackend` behind it is replaced, between two frames. The new backend gets:

- every setting replayed: vsync, render scale, lighting, shadow quality, viewport and
  so on;
- every mesh and texture recreated under the same handle.

To do this, the renderer keeps a copy of each mesh and texture in memory. The editor
turns this on (`EngineConfig::swappableRenderer`). The player doesn't, so it uses no
extra memory.

If the new renderer can't start, the previous one starts again and keeps running.

## Write a renderer

Implement `RenderBackend`
(`Source/Runtime/Renderer/Public/Renderer/RenderBackend.h`) and describe the plugin:

```cpp
std::unique_ptr<RenderBackend> create(const RenderBackendDesc& desc) {
    return std::make_unique<MyRenderer>(desc.window, desc.jobs, desc.vsync);
}
const RendererPluginApi api{.flags = kRendererDrawsGui | kRendererPresents, .create = create};
const PluginDesc plugin{.kind = "renderer", .name = "mine", .description = "My renderer", .api = &api};
SHOCKWAVE_PLUGIN(plugin)
```

Put it in a folder under `Source/Plugins/` and build it with this CMake line:

```cmake
shockwave_plugin(renderer mine DEPS Shockwave::Renderer)
```

`Source/Plugins/RendererNull` is the smallest complete example. The engine gives your
renderer resource ids instead of letting it choose them, so the ids survive swaps.

`Source/Plugins/RendererRaymarch` is a starting point for a renderer of your own that
draws without any GPU features: a software ray marcher (`player --renderer raymarch`).

- **Meshes:** each mesh is baked into a signed distance field with a sparse voxel octree
  over it when it's created.
- **Frames:** every pixel is sphere traced on the CPU, across the job system, with soft
  shadows, and shown through SDL. It draws at a quarter of the window's resolution.
- **Not yet:** textures, skinning, particles, game UI and the editor's GUI. Those places
  are marked `Next:` in the code.
- **Screenshots:** `--screenshot` saves the traced image at its own resolution.

## How it works

This section is for the curious.

- **Loading.** `Core/Plugin.h` loads plugins. It copies each library to a private file
  first and loads that copy. The build can then overwrite the original at any time, and
  a reload loads a fresh copy.
- **No mixing old and new code.** Libraries load with their symbols kept private
  (`RTLD_LOCAL`), so a new copy never picks up the old one's code.
- **Unloading on GCC.** GCC builds plugins with `-fno-gnu-unique`. Without it, the
  system loader would refuse to ever unload them.
- **Spotting a rebuild.** `RebuildWatcher` waits until a rebuilt library's time stamp
  stops changing, so it never loads a half-written file. It reports each build once.
- **Modular builds.** With `SHOCKWAVE_MODULAR` on, every engine module is a shared
  library too. The editor, the player, plugins and game modules then all share one copy
  of the engine. It's on by default everywhere except Windows, for now. Turn it off and
  plugins are built into the programs instead. Everything works the same, except
  reloading.
- **Version check.** A plugin built against a different engine version is refused
  (`kPluginAbiVersion`). Rebuild it along with the engine.
