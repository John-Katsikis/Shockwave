# Shockwave contributor instructions

These are the rules for AI coding agents working on Shockwave. Follow all of them. Read
`docs/Architecture.md` before you change engine code.

## Ground rules

- Preserve existing user changes.
- Every feature needs both an editor UI for people and a text or command-line path for
  tools. That means:
  - a field in the schema;
  - a `--check` rule where mistakes are likely;
  - a way to verify it from the command line.

## Verify your work

- Build the affected targets and run the relevant CTest checks.
- Visually inspect editor UI changes where a display is available.
- Report any verification limitation honestly.

## Edit scenes and projects

When you write or edit `.swscene` or `.swproject` files:

1. Follow `docs/SceneSchema.md`. After adding components or assets, regenerate it:

   ```sh
   build/bin/player --schema docs/SceneSchema.md
   ```

2. Run the checker, and fix what it reports until it reports no problems:

   ```sh
   build/bin/player --check
   ```

3. To see a scene, render it to an image:

   ```sh
   build/bin/player --scene <file> --screenshot out.png
   ```

`docs/Projects.md` (section "Authoring with tools and AI") explains this workflow in
more detail.

## Edit controls

Controls live in `Config/Input.swinput`. Follow `docs/InputSchema.md`, and regenerate it
when it needs updating:

```sh
build/bin/player --input-schema docs/InputSchema.md
```

To see which controllers are plugged in and how their inputs are named:

```sh
build/bin/player --devices
```

## Optimise assets

Texture compression and levels of detail are set by `texture_compression`, `mesh_lods`
and `lod_error_pixels` in the `.swproject`, and per asset by a `<asset>.swimport` file
next to it. Check them with:

```sh
build/bin/player --check
build/bin/player --optimise --renderer vulkan --json out.json
```

See `docs/Optimisation.md`.

## Measure performance

Run the benchmark before and after a change, and compare:

```sh
build/bin/player --benchmark 5 --json out.json
```

It reports frame rate, 1% and 0.1% lows, jitter, and GPU time per pass. See
`docs/FramePacing.md`.

## Engine and editor UI

- Follow `docs/UI.md` when adding or changing engine or editor UI.
- Use the shared `shockwave::gui` controls and the global ImGui style. New options then
  inherit square corners, crisp borders, dense spacing and theme palettes.
- Keep colors in `.swtheme` files and `gui::Theme`, never in individual tool panels.
- For inspector fields, extend the registered component metadata rather than writing
  duplicate UI.

## Code layout

The engine is split into modules the way Unreal's is. `docs/Architecture.md` explains
them.

### Where files go

| Code | Folder |
|---|---|
| Engine modules | `Source/Runtime/<Module>/` |
| A module's API, included as `<Module/Header.h>` | `Source/Runtime/<Module>/Public/<Module>/` |
| Everything else in a module | `Source/Runtime/<Module>/Private/` |
| Editor | `Source/Editor/` |
| Executables | `Source/Programs/` |
| Sample game | `Games/Sample/` |

- New files go in the module they belong to.
- A new module gets `shockwave_module()`.

### Module dependencies

- A module only depends on modules listed before it in `Source/Runtime/CMakeLists.txt`.
  It declares them in its own `CMakeLists.txt`.
- Never include another module's `Private/` headers.
- Vulkan stays inside RHI and the Vulkan renderer plugin
  (`Source/Plugins/RendererVulkan`). Everything else uses `Renderer`.

### Plugins and game code

- Renderers are plugins, built with `shockwave_plugin()`.
- Game code is a game module, built with `shockwave_game_module()`.
- Both hot reload. See `docs/Plugins.md`.
- Game code uses GameMode, GameInstance and Subsystems on top of the ECS. Components
  hold data, and systems in a Schedule do the work in parallel. No per-entity virtual
  Tick. `docs/Projects.md` shows how to write game code.

## Code style

- Log with `SW_LOG(Category, Level, "...")` from `Core/Log.h`, not printf.
- Tool results that scripts read stay on standard output. Examples: `player --check`
  and `player --schema -`.
- Format with `clang-format -i`, using the repo's `.clang-format`.
- Put includes in groups, in this order:
  1. own header;
  2. local headers;
  3. Shockwave modules;
  4. third-party;
  5. standard library.
