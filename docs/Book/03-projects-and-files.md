# 3. Projects and their files

**In this chapter:** what a project is, what each folder in it holds, and how to start
Orb Hunt.

## A project is a folder with a manifest

Every Shockwave game is a **project**: a folder with one `.swproject` file in it. That
file is the *manifest*. It's a short text file that tells the engine where everything
is. Here is the sample's:

```text
# Paths are relative to this file. Both editor and player load this manifest.
shockwave_project 1
name "Shockwave Sample"
game_module "sample"
startup_scene "Scenes/main.swscene"
assets "Content"
tick_rate 60
window_size 1280 720
vsync true
```

Line by line:

| Line | Means |
|---|---|
| `shockwave_project 1` | "This is a Shockwave project, format version 1." |
| `name` | The game's name, as players see it. |
| `game_module` | Which compiled C++ holds this game's code. Chapter 5 makes one. |
| `startup_scene` | The level the game opens on. |
| `assets` | The folder holding models, textures and sounds. |
| `tick_rate` | How many times a second the game world updates. Chapter 7 explains why this is separate from the frame rate. |
| `window_size`, `vsync` | How the player's window starts. |

[Projects](../Projects.md#change-project-settings) lists every other key, such as shadow
quality and texture compression.

## What goes where

```text
Games/OrbHunt/
  OrbHunt.swproject   the manifest
  Scenes/             levels, as .swscene text files
  Content/            everything the game loads: models, textures, sounds, UI, prefabs
  Config/             settings files: Input.swinput (controls), Package.swpackage (shipping)
  Source/             your C++ game code
  CMakeLists.txt      one line that tells the build about Source/
  Saved/              your own editor state, autosaves and screenshots (not in git)
```

Two rules make this easy to live with:

- **Names inside Content are paths.** A sound at `Content/Audio/chime.wav` is called
  `Audio/chime.wav` everywhere: in scenes, in code, in the editor.
- **Everything is plain text** except images, models and sounds. Scenes, prefabs,
  controls and settings can all be read, diffed in git, and written by hand, by a
  script or by an AI tool.

`Saved/` is the exception you never commit. It holds things that are about *you*, not
the game: where your editor camera was in each scene, autosaves, screenshots, and the
cache of compressed textures.

## Start Orb Hunt

Make the folders, from the Shockwave folder:

```sh
mkdir -p Games/OrbHunt/Scenes Games/OrbHunt/Content Games/OrbHunt/Config Games/OrbHunt/Source
```

Then create `Games/OrbHunt/OrbHunt.swproject`:

```text
shockwave_project 1
name "Orb Hunt"
game_module "orb_hunt"
startup_scene "Scenes/arena.swscene"
assets "Content"
tick_rate 60
```

The editor can also do this for you: **File > New project...** opens the project hub,
and **Create project** writes the manifest, the folders, a `.gitignore` and a starter
scene. Doing it by hand once shows you there's no magic in it.

The scene and the game code come in the next two chapters.

> **Under the hood.** The manifest is read by `Source/Runtime/Projects`. It's strict on
> purpose: an unknown key, a repeated key or a bad value is an error that names the file
> and line, instead of being silently ignored. The same rule runs through the whole
> engine. Scenes, prefabs, input files and theme files all report mistakes with
> `file:line`.

## Recap

- A project is a folder plus a `.swproject` manifest.
- `Scenes/` holds levels, `Content/` holds assets, `Config/` holds settings, `Source/`
  holds code. `Saved/` is personal and stays out of git.
- Assets are named by their path inside `Content/`.

Next: [Entities and components](04-entities-and-components.md).
