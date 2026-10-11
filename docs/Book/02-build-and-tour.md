# 2. Build it and take a tour

**In this chapter:** build Shockwave, open the editor, and learn what each window is for.

## Build once

The [README](../../README.md#get-started) lists the system packages to install first. On
Arch or CachyOS that's one `pacman` line. After that, from the Shockwave folder:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The first command *configures*: it decides what to build and downloads every library
Shockwave uses (SDL3, Jolt, Dear ImGui and the rest). You don't install those yourself.
The second command *compiles*. The first build takes a while. Later builds only redo
what changed.

Everything lands in `build/bin`:

```text
build/bin/
  editor, player, sandbox   the programs
  libShockwave*.so          one library per engine module (chapter 18)
  plugins/                  the renderers, loaded while running (chapter 14)
  modules/                  game code, one library per game (chapter 5)
  shaders/ fonts/ themes/   files the programs need beside them
```

## Open the editor

```sh
./build/bin/editor Games/Sample
```

This opens the sample project. Press **Ctrl+P** to play. The orange crate pulses, and
**Space** makes it flash. Press **Ctrl+P** again to stop, and everything goes back to
how it was. Press **F1** any time for the list of controls.

## A tour of the windows

The editor is a set of windows docked around a big view of the scene.

```text
+------------------------------------------------------------------+
| File  Edit  Create  Play  Window  Build  Help      (menu bar)    |
| main.swscene  Save  Undo  Redo   [Play] [Pause] [Stop]   EDIT    |  command bar
+-------------+--------------------------------+-------------------+
| Hierarchy   |  Viewport                      | Inspector         |
|             |  (the scene, live)             |                   |
| Floor       |                                | Crate             |
| Crate       |                                |  Transform        |
| Ball        |                                |  MeshRenderer     |
| ...         |                                |  + Component      |
|             +--------------------------------+                   |
|             | Content | Console              |                   |
+-------------+--------------------------------+-------------------+
| saved  |  1 selected  |  14 entities  |  4.1 ms     (status bar)  |
+------------------------------------------------------------------+
```

| Window | What it's for |
|---|---|
| **Viewport** | The scene, drawn live. Hold the right mouse button and use WASD to fly around. W, E and R switch between move, rotate and scale. |
| **Hierarchy** | Every thing in the scene, as a tree. Drag one onto another to attach it. |
| **Inspector** | The data of whatever you selected. Change a number and the viewport updates. |
| **Content** | Your project's files: models, textures, sounds, scenes, prefabs. Drag them into the viewport. |
| **Console** | Messages from the engine and from your game code. Errors show in red. |
| **Command bar** | The open scene, Save, Undo and Redo, and Play, Pause and Stop. |
| **Status bar** | Whether the scene is saved, what's selected, and how long each frame takes. |

More windows live in the **Window** menu: Stats (frame times and the system schedule),
Input (live controllers and bindings), Save Games, Optimisation and others.

You can drag any window by its tab to dock it somewhere else, stack windows as tabs, or
drag one out onto another monitor. **Window > Reset layout** puts everything back.

## Try it

1. Click the **Ball** in the Hierarchy. The Inspector shows its `Transform`,
   `MeshRenderer` and `Bob` components.
2. Change the Bob **amplitude** to 2 and press **Ctrl+P**. The ball now bounces much
   higher.
3. Stop. The amplitude is still 2, because you changed it while editing, not while
   playing. Press **Ctrl+Z** to undo it.

## Recap

- One `cmake` configure, one `cmake --build`, and everything is in `build/bin`.
- The editor is a viewport surrounded by Hierarchy, Inspector, Content and Console.
- Ctrl+P plays and stops. Stop always restores the scene.

Next: [Projects and their files](03-projects-and-files.md).
