# 16. Command-line tools and checks

**In this chapter:** the player as a toolbox, checking a project for mistakes, taking
screenshots without opening a window, and measuring performance.

## Why command-line tools?

Shockwave has a rule: every feature has a way to use it in the editor, *and* a way to
use it from the command line. The editor is for people. The command line is for
scripts, automated tests, continuous integration and AI agents, all of which can't
click buttons but can run a command and read its output.

All these tools are options on `build/bin/player`.

## Check for mistakes

```sh
./build/bin/player --check --project Games/OrbHunt
```

This loads the project the way the game would: it reads the manifest, imports every
asset, loads every scene and prefab, and even starts gameplay and physics on each scene.
Then it reports every problem with its file and line:

```text
Scenes/arena.swscene:42: no entity named 'Playr'; left empty
```

When everything is fine it prints `0 problems`, and exits with code 0. Otherwise it
exits with 1, so a script can tell. It catches typos in component and field names,
values out of range, missing assets, a scene without exactly one primary camera, broken
input bindings, bad package settings and more.

It uses the headless renderer, so it runs on a machine with no GPU and no screen. The
GitHub build runs it on every push.

## Look at a scene without the editor

```sh
./build/bin/player --scene Games/OrbHunt/Scenes/arena.swscene --screenshot arena.png
```

This renders the scene in a hidden window and saves the image. Add `--view lighting`,
`--view normals` or `--view unlit` to look at one part of the picture, and
`--size 1280x720` to pick the size.

## Know the format

```sh
./build/bin/player --schema docs/SceneSchema.md
./build/bin/player --input-schema docs/InputSchema.md
```

These write the reference for every component, field, default, range and asset name,
straight from the engine. Run the first after adding components, so the docs always
match the code. `player --devices` lists the controllers plugged in and what their
buttons are called.

## Measure performance

```sh
./build/bin/player Games/OrbHunt --benchmark 5 --json bench.json
```

This plays for 5 seconds and reports the frame rate, the 1% and 0.1% lows (how bad the
worst frames are), jitter, and how long each GPU pass took. Run it before and after a
change to see whether you made things faster or slower. Inside the editor, **Window >
Stats** shows the same frame-time graph live.

## The whole toolbox

| Option | What it does |
|---|---|
| `--check` | find mistakes in a project, scene or prefab |
| `--scene file --screenshot out.png` | render a scene to an image |
| `--schema`, `--input-schema` | write the scene and input references |
| `--benchmark N` | measure frame times for N seconds |
| `--optimise` | report what texture compression and LODs save (chapter 17) |
| `--package` | make a folder players can run (chapter 17) |
| `--save slot`, `--load slot`, `--saves` | save, load and list save games |
| `--bake-navmesh` | build a navigation mesh for AI pathfinding |
| `--devices`, `--gpus`, `--renderers` | list controllers, GPUs and renderer plugins |
| `--json file` | also write the results as JSON, for scripts |

`player --help` lists everything.

## Tests

The engine's own tests live in `Tests/`. Run them all with:

```sh
ctest --test-dir build --output-on-failure
```

Most need no GPU: they cover entities and scheduling, scene files, prefabs, physics,
input, save games, packaging and more.

> **Under the hood.** The tools are in `Source/Programs/Player/Private`: `PlayerTools.cpp`
> for checks and schemas, and one file per feature for the rest (`PlayerPackage.cpp`,
> `PlayerOptimise.cpp`, `PlayerNavigation.cpp`...). Results meant for scripts go to
> standard output; everything else goes through `SW_LOG`.

## Recap

- `player --check` finds mistakes and exits non-zero when it does. Run it often.
- `player --screenshot` shows a scene without opening the editor.
- `player --benchmark` measures; compare before and after.
- Every feature has an editor way and a command-line way.

Next: [Shipping your game](17-shipping.md).
