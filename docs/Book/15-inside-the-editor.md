# 15. Inside the editor: panels, docking and themes

**In this chapter:** how the editor draws itself, how the docked layout is built, how
themes work, and how to add a panel of your own.

## Immediate-mode UI

The editor is built with **Dear ImGui**, which works differently from most UI toolkits.
In a typical toolkit you create a button once, keep it around, and attach a callback.
In ImGui, there are no button objects. You *draw* the button every frame, and the call
tells you if it was clicked:

```cpp
if (ImGui::Button("Save")) saveScene();
```

That line runs 60+ times a second. Most frames it draws a button and returns false. The
frame you click it, it returns true. The UI is a function of your data, re-run every
frame, so it can never get out of sync with it. This is called **immediate-mode** UI.

A panel is just a block of these calls between `Begin` and `End`. Here's the start of
the Console panel, from `ConsolePanel.cpp`:

```cpp
void Editor::drawConsole() {
    if (!ImGui::Begin("Console", &m_showConsole)) {
        ImGui::End();
        return;
    }
    if (gui::button("Clear")) {
        m_log.clear();
        m_warningCount = m_errorCount = 0;
    }
    // ... filters, then one line of text per log entry ...
    ImGui::End();
}
```

`Begin` returns false when the panel is collapsed or hidden behind another tab, so the
panel skips drawing what nobody can see. `&m_showConsole` gives the panel a close button
that sets the flag to false.

## What the editor draws each frame

In chapter 7's loop, the engine calls `onGui()` once per frame. The editor's version is
in `Source/Editor/Editor/Private/Editor.cpp`, and reads almost like a list of what's on
screen:

```cpp
    drawMenuBar();
    drawCommandBar();
    drawStatusBar();
    drawDockspace();
    drawViewport();
    if (m_showHierarchy) drawHierarchy();
    if (m_showInspector) drawInspector();
    if (m_showContent) drawContentBrowser();
    if (m_showConsole) drawConsole();
    if (m_showStats) drawStatsWindow();
    // ... the rest of the windows ...
    gui::focusOutline();
    drawUnsavedChangesModal();
```

Each `m_show...` flag is what the **Window** menu ticks on and off. With no project
open, `onGui` draws the menu bar and the **Project Hub** instead, and stops.

Every panel has its own file in `Source/Editor/Editor/Private`: `HierarchyPanel.cpp`,
`InspectorPanel.cpp`, `ViewportPanel.cpp`, `ContentBrowserPanel.cpp` and so on. The
frame around them (menu bar, command bar, status bar, dock layout, small windows) is
`Workspace.cpp`.

## The dock layout

**Docking** lets panels snap together into one window, with draggable splitters
between them, and stack as tabs. The whole editor window is one big **dockspace**.

The default arrangement is built in code, in `Workspace.cpp`:

```cpp
void Editor::buildDefaultLayout(ImGuiID dockspace) {
    // ...
    ImGuiID center = dockspace;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Content", bottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(dockspace);
}
```

Read it as cutting a sheet of paper. Cut 17% off the left for the Hierarchy. Cut 25%
off the right of what's left for the Inspector. Cut 30% off the bottom of the middle for
Content and Console, which share it as tabs. The Viewport gets the rest.

This only runs the first time, or after **Window > Reset layout**. After that, ImGui
remembers wherever you dragged things, in `editor-layout.ini` next to your editor
preferences. Other windows (Stats, Input, Save Games, Optimisation...) open floating,
and you can dock them anywhere.

With **Preferences > detachable panels** on, you can drag a panel right out of the
editor onto another monitor; it becomes a real window of its own. On Wayland the editor
quietly switches to XWayland for this, because Wayland doesn't let programs place their
own windows.

## The look: themes

Colors, borders, spacing and the font all come from one **theme**. The current one is a
telemetry-console look: square corners, 1px borders, dense spacing, a monospaced font,
cyan for anything active and amber for secondary data.

A theme has two layers:

1. **Built-in defaults**, in the `gui::Theme` struct in
   `Source/Runtime/UI/Public/UI/Gui.h`.
2. **A `.swtheme` file** that overrides any of them. The editor loads
   `themes/default.swtheme` from the working folder (or the copy beside the program).

A theme file is easy to read:

```text
[palette]
background      = #06080a   # windows: the obsidian void
panel           = #0d161c   # inner panels: lists, child regions, table rows
border          = #223d49   # 1px lines around panels and controls
accent          = #00e5ff   # what's active: focused panel, current tool, open tab
secondary       = #ffb020   # amber: histograms and hovered graph values

[shape]
window_rounding = 0
frame_border    = 1

[spacing]
item_spacing    = 4 3

[text]
font_size = 14
font      = fonts/Cousine-Regular.ttf
```

**Save the file while the editor is running and the editor restyles within half a
second.** The engine checks the file's time twice a second and reapplies it. A typo
doesn't break anything: the Console names the file and line, and the current look stays.

To use a different theme, pick it in Preferences or start with
`editor --theme themes/ember.swtheme`. The repository ships `default`, `ember` and
`instrument`.

Two rules keep the look consistent:

- **Colors live only in themes.** A panel never writes its own hex color. It asks the
  theme: `gui::currentTheme().warning`, `gui::accent()` and so on.
- **Use the shared controls.** `gui::button`, `gui::toolButton` (a tool that can be
  selected, drawn in the accent color), `gui::toolbarNext` (wraps a toolbar onto a new
  row when the panel is narrow) and `gui::beginPropertyGrid` (label and value columns).
  New panels then inherit the look for free.

`gui::focusOutline()`, called near the end of `onGui`, draws the thin cyan outline
around whichever panel has focus.

One known quirk: ImGui's colors go through an sRGB screen buffer in 8-bit steps, so
very dark colors snap to the nearest step. That's why the theme's near-blacks are
chosen to sit exactly on those steps.

## Add a panel of your own

Here's a tiny "Orbs" panel that counts the orbs in the scene. In a new file
`Source/Editor/Editor/Private/OrbsPanel.cpp`:

```cpp
#include "Editor.h"

#include <UI/Gui.h>

namespace editor {

namespace gui = shockwave::gui;

void Editor::drawOrbsPanel() {
    if (!ImGui::Begin("Orbs", &m_showOrbs)) {
        ImGui::End();
        return;
    }
    if (gui::beginPropertyGrid("orbs")) {
        gui::propertyLabel("Entities");
        ImGui::Text("%zu", m_world.entityCount());
        ImGui::EndTable();
    }
    ImGui::End();
}

}   // namespace editor
```

Then three small additions: declare `void drawOrbsPanel();` and `bool m_showOrbs = false;`
in `Editor.h`, add `if (m_showOrbs) drawOrbsPanel();` to `onGui`, and add
`ImGui::MenuItem("Orbs", nullptr, &m_showOrbs);` to the Window menu in `Workspace.cpp`.
The build picks up the new file automatically. [UI](../UI.md) has the full rules for
editor UI.

> **Under the hood.** `Engine::initGui` (`Source/Runtime/Engine/Private/Engine.cpp`)
> creates the ImGui context, turns on docking (and multi-viewports when detachable
> panels are on), and loads the theme. The SDL3 half of ImGui lives in the Engine
> module, because it owns window events; the Vulkan half lives in the renderer plugin
> (`RendererGui.cpp`), because it owns the GPU. ImGui itself is built as a shared
> library in `cmake/ImGui.cmake`, so the editor and the renderer plugin share one UI
> context. That file also copies the fonts and themes next to the programs.

## Recap

- The editor is Dear ImGui: immediate mode, every panel redrawn every frame.
- `Editor::onGui` draws the frame, the dockspace, then each open panel.
- `buildDefaultLayout` cuts the dockspace into Hierarchy, Inspector, Content/Console
  and Viewport. Your own arrangement is remembered in `editor-layout.ini`.
- Themes are `gui::Theme` defaults plus a `.swtheme` file that reloads as you save.
- Colors come from the theme; controls come from `shockwave::gui`.

Next: [Command-line tools and checks](16-tools-and-checks.md).
