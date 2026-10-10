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
Every feature needs both an editor UI for people and a text/CLI path for tools: a field in
the schema, a `--check` rule where mistakes are likely, and a way to verify it from the
command line.
