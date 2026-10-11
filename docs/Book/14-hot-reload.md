# 14. Changing code while it runs

**In this chapter:** reloading game code and the renderer without restarting, what
survives, and how plugins work.

## Try it first

Open Orb Hunt in the editor and press Play. Leave it running. In `OrbHunt.cpp`, make
the player faster and the arena busier:

```cpp
    float speed = 9.0f;   // meters per second
```

```cpp
        for (int i = 0; i < 30; ++i) spawnOrb(game);
```

Save, go back to the editor and press **Ctrl+Alt+B** (Build > Build and reload game
code). The Console shows the build, then "Reloaded game code". Play restarts with the
new code: a faster player and 30 orbs. (The Player gets its Runner in `beginPlay`,
so the new default speed applies as soon as Play restarts.)

You can build from anywhere: your IDE, a terminal running
`cmake --build build --target game_orb_hunt`, or an AI agent. The editor notices the new
library on disk and reloads it. If the code doesn't compile, nothing changes: the old
code keeps running and the errors show in the Console.

## What survives a reload

The editor keeps:

- the open scene, including unsaved changes;
- your selection;
- the whole undo and redo history.

Your components come back too, as the *new* code's types. A field you added starts at
its default; a field you removed is dropped. If Play was running, it stops before the
reload and starts again after.

What doesn't survive is anything stored outside the World: a static variable, or state
kept only in your GameInstance.

## How it works

Your game module is a shared library: `build/bin/modules/game_orb_hunt.so`. When it
changes on disk, the editor:

1. stops Play;
2. writes the scene and every undo step out as scene text, the same format as a
   `.swscene` file;
3. loads the new library *next to* the old one. If that fails, it stops here and
   nothing is lost;
4. clears the worlds while the old code is still loaded, so the old code can free its
   own components, then unloads the old library;
5. reads the worlds back in, now using the new code's component types;
6. makes a new GameInstance and starts Play again.

Step 2 is why ECS and plain-text scenes matter so much. Because every piece of game
state is a plain struct with a registered list of fields, "save everything and read it
back" is a reliable operation. That same machinery powers scene files, undo, save
games, prefabs and this reload.

## Reloading the renderer

The renderer is a plugin too. Rebuild it, or just a shader:

```sh
cmake --build build --target renderer_vulkan
```

The editor reloads it by itself (or press **Ctrl+Alt+R**, Build > Reload renderer).
The `Renderer` keeps a copy of every mesh and texture, hands them to the new backend
under the same IDs, and replays every setting. If the new one fails to start, the old
one starts again.

**Build > Renderer** switches to a different renderer while running, and
`player --renderers` lists the ones available.

## Modules and plugins

It helps to know the two kinds of library in `build/bin`:

| Kind | Example | Loaded | Can reload? |
|---|---|---|---|
| Engine module | `libShockwaveEngine.so` | when the program starts, like any library | no |
| Plugin | `plugins/renderer_vulkan.so`, `modules/game_orb_hunt.so` | while running, by name | yes |

Engine modules being separate libraries (the default on Linux, `SHOCKWAVE_MODULAR`)
means a plugin and the editor share the one copy of the engine, so they agree about
every component and the one Dear ImGui context.

> **Under the hood.** Plugin loading is in `Core/Plugin.h`. A plugin declares itself
> with `SHOCKWAVE_PLUGIN(desc)`, naming its kind (`renderer`) and name (`vulkan`). The
> editor's reload logic is in `Source/Editor/Editor/Private/EditorCode.cpp`;
> `Engine::applyRendererChange` handles the renderer swap between frames. On GCC, plugins
> are built with `-fno-gnu-unique`, or the system loader would refuse to unload them.

## Recap

- Ctrl+Alt+B rebuilds and reloads game code; Ctrl+Alt+R reloads the renderer.
  Rebuilding from anywhere works.
- The scene, selection and undo history survive, because everything can be written
  out as scene text and read back.
- A failed build never replaces working code.

Next: [Inside the editor: panels, docking and themes](15-inside-the-editor.md).
