# Shockwave tool UI

The tools use a shared engineering-panel design system in `shockwave/Gui.h`.
New standard ImGui widgets automatically inherit it. Keep all ImGui work on the main
thread, inside `Game::onGui()`.

## Geometry and layout

`gui::applyTheme()` enforces zero rounding and one logical pixel borders for windows,
children, inputs, popups, tabs, and dock separators. HiDPI scales the logical pixel.
Legacy rounding/border theme keys remain readable, but cannot change this geometry.
Themes control density, typography, and colors. Avoid local geometry overrides,
drop shadows, floating cards, and hardcoded colors.

The default look is a telemetry console: an obsidian `background` void, matte slate
`panel` fill for inner regions (child windows, lists, table rows), electric cyan
`accent` for anything active and amber `secondary` for secondary data such as
histograms. The focused panel gets a 1px accent outline from `gui::focusOutline()`,
which the editor calls once per frame after its tool windows. Text uses the monospaced
Cousine font that ships with Dear ImGui (`build/bin/fonts`).

Use `gui::button()` for ordinary actions and `gui::toolButton()` for selected tools.
Selected tools use the theme accent and `on_accent` text. Use `gui::toolbarNext()`
between toolbar items so narrow panels wrap instead of overlapping.

Keep application-wide actions in the workspace command bar (scene context left,
playback centered, mode status right). Keep viewport editing tools left and viewport
settings right. Group related actions and reserve spacing between groups. Measure
with `controlGroupWidth()`/`toolbarRows()` and use `alignTrailing()` for panel actions;
never position controls with fixed screen coordinates. Search fields lead panel
headers and expand into available space; secondary actions trail them. Put component
creation beside entity identity and destructive actions in the component options
menu. On narrow panels, move a complete settings group to its own row.

Use `gui::beginPropertyGrid()` and `gui::propertyLabel()` for aligned property rows:

```cpp
if (shockwave::gui::beginPropertyGrid("settings")) {
    shockwave::gui::propertyLabel("Speed");
    ImGui::DragFloat("##speed", &speed);
    shockwave::gui::propertyLabel("Enabled");
    ImGui::Checkbox("##enabled", &enabled);
    ImGui::EndTable();
}
```

Keep IDs stable and unique; use `PushID` around repeated objects/fields. Call the
editor's existing undo tracking after editable scene widgets. Keep drag/drop and
keyboard behavior intact. Put toolbars in reserved layout space, not over scene content.

## Themes

Copy `themes/default.swtheme` and pass `--theme path/to/theme.swtheme` to the editor.
Colors are authored in sRGB hex and converted centrally for the Vulkan sRGB output.
ImGui stores them as 8-bit linear values, so near-black colors snap to a few sRGB steps
(0, 13, 22, 28, 34, 38...); keep dark tones on those steps to see them exactly.
The engine reloads saved themes automatically. Missing keys retain built-in defaults;
the ember file is a partial theme over built-in defaults, not over default.swtheme.

Semantic keys include `button`, `button_hover`, `button_active`, `on_accent`,
`accent`, `accent_hover`, `accent_active`, `secondary`, `panel`, `warning`, and `error`. Standard ImGui
color names can also override mapped colors. For custom drawing use
`ImGui::GetColorU32(ImGuiCol_...)`, or `gui::semanticColorU32()` for semantic colors.
For `TextColored` use `gui::semanticColor()`. Never pass sRGB values directly to ImGui.
If a feature needs another semantic color, add it to Theme, the loader key table,
and the example theme together. Do not embed the feature's palette in panel code.

## Verification

Build editor and tests, then run CTest. Inspect the default and an alternate theme,
a selected entity, play/pause states, and narrow panel layouts. Check labels fit,
controls do not overlap, selection stays readable, and scene editing still supports
undo. The UI theme test runs without a GPU and checks file parsing, palette changes,
geometry invariants, and repeated application at different display scales.
