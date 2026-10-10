# Editor and tool UI

This is the guide to follow when you add a panel, button or option to the editor or the
other tools. All of them share one look, defined in `UI/Gui.h`
(`Source/Runtime/UI/Public/UI/Gui.h`). Follow the rules below so new UI matches and
keeps working on narrow panels, in other themes and on high-DPI screens.

The tools are built with Dear ImGui, an immediate-mode UI library: you draw your
widgets every frame in code. Standard ImGui widgets you add pick up the shared look
automatically.

## The basic rules

- Do all ImGui work on the main thread, inside `Application::onGui()`.
- Keep widget IDs stable and unique. Use `PushID` around repeated objects and fields.
- After a widget that edits the scene, call the editor's existing undo tracking.
- Keep drag and drop and keyboard behavior working.
- Put toolbars in space reserved in the layout, never on top of scene content.
- Never position anything at fixed screen coordinates.
- Never hardcode colors. Use theme colors (see [Use colors](#use-colors)).

## Add a button or tool

| Use | For |
|---|---|
| `gui::button()` | Ordinary actions |
| `gui::toolButton()` | Tools that can be selected. A selected tool uses the theme's accent color with `on_accent` text. |
| `gui::toolbarNext()` | Call it between toolbar items, so a narrow panel wraps them onto a new row instead of overlapping |

## Add an editable setting

Use `gui::beginPropertyGrid()` and `gui::propertyLabel()` to line up label and value
rows. On a narrow panel the labels wrap inside their column, instead of running off its
edge.

```cpp
if (shockwave::gui::beginPropertyGrid("settings")) {
    shockwave::gui::propertyLabel("Speed");
    ImGui::DragFloat("##speed", &speed);
    shockwave::gui::propertyLabel("Enabled");
    ImGui::Checkbox("##enabled", &enabled);
    ImGui::EndTable();
}
```

The `##` in a widget name hides the text and keeps only the ID, since the property grid
draws the label.

## Where controls go

Put each kind of control in its usual place:

| Control | Where |
|---|---|
| Actions for the whole application | The workspace command bar. Scene context on the left, playback in the center, mode status on the right. |
| Viewport editing tools | Left side of the viewport toolbar |
| Viewport settings | Right side of the viewport toolbar |
| Search field | First in a panel's header, stretching to fill the free space. Secondary actions come after it. |
| Adding a component | Next to the entity's name |
| Destructive actions on a component | The component's options menu |

Layout rules:

- Group related actions, and leave space between groups.
- Measure groups with `controlGroupWidth()` and `toolbarRows()`.
- Use `alignTrailing()` to push panel actions to the right edge.
- On a narrow panel, move a whole settings group to its own row. Don't split it.

## Panels outside the main window

Panels can be dragged out of the editor's main window, using Dear ImGui's
multi-viewports feature. So never assume a window is inside the main viewport:

- Position things relative to `ImGui::GetMainViewport()` or the current window.
- Never position at absolute `(0, 0)`.
- Mouse positions are in desktop coordinates, not window coordinates.

## Shape and spacing

`gui::applyTheme()` forces square corners and borders one logical pixel wide. This
covers windows, child windows, inputs, popups, tabs and dock separators. On high-DPI
screens the logical pixel scales up.

Older theme keys for rounding and borders still load, but they can't change this shape.
Themes control density, fonts and colors only.

Don't override this shape locally. Don't add drop shadows, floating cards or hardcoded
colors.

## The default look

The default theme looks like a telemetry console:

| Theme key | Color | Used for |
|---|---|---|
| `background` | Obsidian | Window backgrounds |
| `panel` | Matte slate | Inner regions: child windows, lists, table rows |
| `accent` | Electric cyan | Anything active |
| `secondary` | Amber | Secondary data, such as histograms |

The focused panel gets a 1 pixel accent outline from `gui::focusOutline()`. The editor
calls it once per frame, after drawing its tool windows.

Text uses Cousine, a monospaced font that ships with Dear ImGui. The build copies it to
`build/bin/fonts`.

## Use colors

- For custom drawing, use `ImGui::GetColorU32(ImGuiCol_...)`, or
  `gui::semanticColorU32()` for a semantic color.
- For `TextColored`, use `gui::semanticColor()`.
- Never pass sRGB values straight to ImGui.

Semantic colors are the named roles in a theme: `button`, `button_hover`,
`button_active`, `on_accent`, `accent`, `accent_hover`, `accent_active`, `secondary`,
`panel`, `warning` and `error`, among others. A theme can also override standard ImGui
color names directly.

If your feature needs a new semantic color, add it in three places together: the
`Theme` struct, the loader's key table and the example theme. Don't put the feature's
own palette in panel code.

## Make or try a theme

Copy `themes/default.swtheme`, edit it, and start the editor with it:

```sh
./build/bin/editor --theme path/to/theme.swtheme
```

The engine reloads a theme file automatically when you save it. Keys you leave out keep the built-in
defaults. The ember theme is a partial theme on top of the built-in defaults, not on top
of `default.swtheme`.

Write colors as sRGB hex, the usual web color codes. They are converted in one central
place for Vulkan's sRGB output.

ImGui stores colors as 8-bit linear values. Because of that, very dark colors snap to a
few sRGB steps: 0, 13, 22, 28, 34, 38 and so on. Keep dark tones on those steps if you
want them shown exactly.

## Check your change

Build the editor and the tests, then run CTest:

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Then look at your change in the editor:

- with the default theme and one other theme
- with an entity selected
- while playing and while paused
- with narrow panels

Check that labels fit, controls don't overlap, the selection stays readable and scene
edits can still be undone.

The UI theme test runs without a GPU. It checks theme file parsing, palette changes,
the shape rules, and applying the theme again at different display scales.
