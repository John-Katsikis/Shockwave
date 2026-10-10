# Shockwave contributor instructions

Preserve existing user changes. Follow `docs/UI.md` when adding or changing engine
or editor UI. Use the shared `shockwave::gui` controls and global ImGui style so new
options inherit square corners, crisp borders, dense spacing, and theme palettes.
Keep colors in `.swtheme`/`gui::Theme`, never in individual tool panels. Extend
registered component metadata for inspector fields rather than writing duplicate UI.
Build affected targets and run the relevant CTest checks; visually inspect editor UI
changes where a display is available. Report any verification limitation honestly.

When writing or editing `.swscene` or `.swproject` files, follow `docs/SceneSchema.md`
(regenerate it with `build/bin/player --schema docs/SceneSchema.md` after adding
components or assets) and run `build/bin/player --check` until it reports no problems.
To see a scene, render it with `build/bin/player --scene <file> --screenshot out.png`.
Controls live in `Config/Input.swinput`; follow `docs/InputSchema.md` (regenerate with
`build/bin/player --input-schema docs/InputSchema.md`), and use `player --devices` to see
what controllers are plugged in and how their inputs are named.
To measure performance, run `build/bin/player --benchmark 5 --json out.json` (frame rate,
1% and 0.1% lows, jitter, GPU time per pass; see `docs/FramePacing.md`) before and after a
change.
Every feature needs both an editor UI for people and a text/CLI path for tools: a field in
the schema, a `--check` rule where mistakes are likely, and a way to verify it from the
command line.

## Code layout

The engine is split into modules the way Unreal's is; read `docs/Architecture.md` first.

- Engine code lives in `Source/Runtime/<Module>/`. `Public/<Module>/` holds the module's
  API (included as `<Module/Header.h>`), `Private/` everything else. Editor code is in
  `Source/Editor/`, executables in `Source/Programs/`, the sample game in `Games/Sample/`.
- A module only depends on modules listed before it in `Source/Runtime/CMakeLists.txt`,
  and declares them in its `CMakeLists.txt`. Never include another module's `Private/`
  headers. Vulkan stays inside RHI and the Vulkan renderer plugin
  (`Source/Plugins/RendererVulkan`); everything else uses `Renderer`.
- Renderers are plugins (`shockwave_plugin()`), game code is a game module
  (`shockwave_game_module()`); both hot reload. See `docs/Plugins.md`.
- New files go in the module they belong to; a new module gets `shockwave_module()`.
- Game code uses GameMode/GameInstance/Subsystems on top of the ECS: components hold
  data, systems in a Schedule do the work in parallel. No per-entity virtual Tick.
- Log with `SW_LOG(Category, Level, "...")` (`Core/Log.h`), not printf. Tool results
  that scripts read (player `--check`, `--schema -`) stay on standard output.
- Format with `clang-format -i` (the repo's `.clang-format`). Includes go in groups:
  own header, local headers, Shockwave modules, third-party, standard library.
