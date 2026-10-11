# 11. A HUD with the score

**In this chapter:** in-game UI written like a web page, showing a value from game
code, and reacting to a button.

## Two kinds of UI

Shockwave has two completely separate UI systems. It's worth knowing which is which:

| | Game UI | Editor UI |
|---|---|---|
| Who sees it | your players | you, while building the game |
| Written in | `.rml` and `.rcss` files, like HTML and CSS | C++, with Dear ImGui (chapter 15) |
| Library | RmlUi | Dear ImGui |
| Lives in | your project's `Content/UI/` | the engine's `Source/Editor` |

This chapter is about game UI.

## Write the HUD

A UI screen is a **document**: an `.rml` file for the content and an `.rcss` file for
the style. Create `Games/OrbHunt/Content/UI/hud.rml`:

```html
<rml>
<head>
    <title>Orb Hunt HUD</title>
    <link type="text/rcss" href="hud.rcss"/>
</head>
<body data-model="game">
    <div class="panel">
        <span class="label">ORBS</span>
        <span id="score">{{score}}</span>
        <button onclick="reset">Reset</button>
    </div>
</body>
</rml>
```

And `Games/OrbHunt/Content/UI/hud.rcss`:

```css
body {
    font-family: "Noto Sans";
    font-size: 18dp;
    color: #e8f6ff;
    width: 100%;
    height: 100%;
    pointer-events: none;
}
.panel {
    position: absolute;
    left: 24dp;
    top: 24dp;
    padding: 12dp 16dp;
    background-color: #0a1420cc;
    border-left: 3dp #33e0ff;
    pointer-events: auto;
}
.label {
    display: block;
    font-size: 12dp;
    color: #33e0ff;
}
#score {
    display: block;
    font-size: 40dp;
    font-weight: bold;
}
```

If you've written a web page, this is familiar. Three things are specific:

- **`{{score}}`** is a placeholder. `data-model="game"` on the body connects it to values
  your game code sets.
- **`onclick="reset"`** doesn't run script. It sends a command called `reset` to your
  game code.
- **`dp`** is a size that grows with the display's scale setting, so the HUD looks the
  same on a laptop and a 4K monitor. `pointer-events: none` on the body lets clicks on
  empty space reach the game.

## Show it in the scene

A scene shows a document through an entity with a `UIDocument` component. Add this to
`arena.swscene`:

```text
entity "HUD"
  UIDocument
    document "UI/hud.rml"
    visible 1
    order 0
    preview "score=12"
end
```

`preview` gives the placeholders stand-in values while you're editing, so the viewport
shows "12" instead of an empty box. Save the `.rml` or `.rcss` and the viewport updates
straight away, even during Play.

## Connect it to the game

Two calls do it: `game.ui().set(name, value)` fills a placeholder, and
`game.ui().on(command, function)` reacts to a command.

In `beginPlay`, after spawning the orbs:

```cpp
        m_score = 0;
        game.ui().set("score", m_score);
        game.ui().on("reset", [this, &game](const UIEvent&) {
            m_score = 0;
            game.ui().set("score", m_score);
        });
```

And in `collectOrbs`, replace the log line with:

```cpp
        if (!collected.empty()) game.ui().set("score", m_score);
```

Rebuild and play. The score counts up as you collect orbs, and the Reset button sets it
back to zero.

`game.ui()` can also show and hide whole documents by the UIDocument entity's name:
`game.ui().show("PauseMenu")`, `hide`, `toggle`. The sample game uses that for its pause
menu, which you can drive with arrow keys or a gamepad. [Game UI](../GameUI.md) covers
menus, lists, focus and fonts.

> **Under the hood.** Game UI is the `GameUI` module, built on RmlUi. FreeType draws the
> letters and HarfBuzz shapes them, so text is sharp and correctly spaced. Each frame
> the UI is drawn *after* tonemapping, at the window's own resolution, so it stays crisp
> even when the 3D scene renders at a lower resolution. Values you `set` are pushed into
> RmlUi's data model, which redraws only what changed.

## Recap

- Game UI is `.rml` + `.rcss` in `Content/UI/`, shown by a `UIDocument` entity.
- `{{name}}` placeholders are filled with `game.ui().set(name, value)`.
- `onclick="cmd"` sends a command; `game.ui().on("cmd", ...)` handles it.
- Files reload as you save them.

Next: [Saving the game](12-saving.md).
