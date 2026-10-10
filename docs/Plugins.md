# Plugins and hot reload

Renderers and game code are plugins: libraries the engine loads at runtime instead of
linking in. That gives you three things:

- **Game code hot reload.** Edit your game's C++, rebuild, and the editor reloads it in
  place. The open scene, the selection and the undo history are kept, and Play restarts
  if it was running.
- **Renderer hot reload.** Rebuild the renderer (or just its shaders) and reload it
  without restarting. Meshes, textures and settings carry over.
- **Swappable renderers.** Pick a different renderer plugin while running, or start the
  player headless for servers and tools.

## Using it

| | Editor | Text | Command line |
|---|---|---|---|
| Build and reload game code | Build > Build and reload game code (Ctrl+Alt+B) | | `cmake --build build --target game_<id>` |
| Reload game code as it is on disk | Build > Reload game code | | |
| Reload game code by itself after any rebuild | Build > Reload when rebuilt; Preferences > Plugins (on by default) | `auto_reload_game_code` in the editor preferences | |
| Reload the renderer | Build > Reload renderer (Ctrl+Alt+R) | | `cmake --build build --target renderer_vulkan` |
| Reload the renderer by itself after a rebuild | Build > Reload renderer when rebuilt; Preferences > Plugins (on by default) | `auto_reload_renderer` | |
| Pick the editor's renderer | Build > Renderer; Preferences > Plugins > Renderer | `renderer` in the editor preferences | `editor --renderer vulkan` |
| Pick the player's renderer | Project settings | `renderer "vulkan"` in the `.swproject` | `player --renderer null` |
| List renderers | Build > Renderer | | `player --renderers` |
| Load a game module from a file | | | `player --game-module path/to/game_x.so` |

Because the editor reloads whatever lands on disk, a rebuild from anywhere counts: an
IDE, a terminal, or an AI agent running CMake. The Console says what was reloaded and
how long it took, or why it couldn't be. A build that fails to compile or load never
replaces the running code.

`player --check` and `player --schema` start on the headless renderer, so they need no
GPU. `--check` also reports a project whose `renderer` isn't available.

## What survives a game code reload

The editor stops Play first (it restarts once the new code is in), then:

1. writes the scene, the edit in progress and every undo and redo step as scene text;
2. loads the new library next to the old one; if that fails, it stops here and the old
   code carries on;
3. clears those worlds while the old code is still there to free its components, and
   unloads it;
4. reads the worlds back with the same entities, so handles, the selection and undo all
   still line up. Game components come back as the new code's types: fields you added
   start at their defaults, removed ones are dropped;
5. makes a new GameInstance and runs the module's `createAssets` again.

What isn't saved in scenes doesn't survive: components that aren't registered, and
state your GameInstance keeps elsewhere. Anything your module hooks into the engine
beyond `registerTypes`, `createAssets` and its GameInstance (a log sink, say) must be
undone in `GameInstance::shutdown`.

## What survives a renderer swap

The `Renderer` games and tools use stays put; only the `RenderBackend` behind it is
replaced, between frames. It replays every setting (vsync, render scale, lighting,
shadow quality, viewport...) and recreates every mesh and texture under the same handle.
To do that it keeps a copy of each in memory, which the editor turns on
(`EngineConfig::swappableRenderer`); the player doesn't, so it uses no extra memory.

If the new renderer can't start, the previous one is started again and keeps running.

## Writing a renderer

Implement `RenderBackend` (`Source/Runtime/Renderer/Public/Renderer/RenderBackend.h`)
and describe the plugin:

```cpp
std::unique_ptr<RenderBackend> create(const RenderBackendDesc& desc) {
    return std::make_unique<MyRenderer>(desc.window, desc.jobs, desc.vsync);
}
const RendererPluginApi api{.flags = kRendererDrawsGui | kRendererPresents, .create = create};
const PluginDesc plugin{.kind = "renderer", .name = "mine", .description = "My renderer", .api = &api};
SHOCKWAVE_PLUGIN(plugin)
```

Build it with `shockwave_plugin(renderer mine DEPS Shockwave::Renderer)` in a folder
under `Source/Plugins/`. `Source/Plugins/RendererNull` is the smallest complete example.
The renderer is given resource ids rather than choosing them, so they survive swaps.

## How it works

- `Core/Plugin.h` loads plugins. A library is copied to a private file before loading,
  so the build can overwrite the original at any time, and the copy is what's reloaded
  from. Libraries load with their own symbols kept private (`RTLD_LOCAL`), so a new copy
  never picks up the old one's code. GCC builds plugins with `-fno-gnu-unique`, without
  which the loader would refuse to ever unload them.
- `RebuildWatcher` notices a rebuilt library once its time stamp has stopped changing,
  so a half-written file isn't loaded, and reports each build once.
- In modular builds (`SHOCKWAVE_MODULAR`, the default except on Windows for now) every
  engine module is a shared library too, so the editor, the player, plugins and game
  modules all share one copy of the engine. Turn it off and plugins are linked into the
  executables instead: everything works the same except reloading.
- A plugin built against a different engine version is refused (`kPluginAbiVersion`).
  Rebuild it with the engine.
