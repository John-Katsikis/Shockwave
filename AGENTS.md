# Shockwave contributor instructions

Preserve existing user changes. Follow `docs/UI.md` when adding or changing engine
or editor UI. Use the shared `shockwave::gui` controls and global ImGui style so new
options inherit square corners, crisp borders, dense spacing, and theme palettes.
Keep colors in `.swtheme`/`gui::Theme`, never in individual tool panels. Extend
registered component metadata for inspector fields rather than writing duplicate UI.
Build affected targets and run the relevant CTest checks; visually inspect editor UI
changes where a display is available. Report any verification limitation honestly.
