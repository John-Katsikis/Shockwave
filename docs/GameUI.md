# Game UI

Game UI is the menus, HUDs and on-screen text your players see. You write each screen
as an `.rml` file (markup much like HTML) styled by an `.rcss` file (much like CSS), and
save them in your project's `Content/UI/` folder. They reload as you save, in the editor
and in the running game.

Shockwave draws UI with [RmlUi](https://github.com/mikke89/RmlUi). FreeType draws the
letters and HarfBuzz shapes the text, which means it handles kerning (letter spacing
pairs like "AV"), ligatures and scripts that join letters. UI is drawn at the window's
own resolution after the 3D scene, so text stays sharp at any window size or render
scale.

New to Shockwave? [Tutorial: your first game in Shockwave](Tutorial.md) walks through a
whole project first.

## Add a HUD to a scene

1. In the editor, use Create > UI document. This writes a starter `Content/UI/hud.rml`
   and `hud.rcss` if they don't exist yet. It also adds an entity named `Hud` with a
   `UIDocument` component.
2. The viewport shows the document over the scene straight away.
3. Open the `.rml` file in any text editor. In the Inspector, Open file does this for
   you. Save, and the viewport updates.

You can also drag an `.rml` file from the Content panel into the viewport, or
double-click it.

A scene shows a document through a `UIDocument` component. In a `.swscene` file it looks
like this:

```text
entity "HUD"
  UIDocument
    document "UI/hud.rml"
    visible 1
    order 0
    focus_on_show 0
end
```

| Setting | Text name | What it does |
|---|---|---|
| Document | `document` | The `.rml` file, as a path inside `Content`. |
| Visible | `visible` | Shows or hides the document. Game code changes this with `game.ui().show()` and `hide()`. |
| Order | `order` | Which document is on top. Higher numbers draw on top and get the mouse first. |
| Focus on show | `focus_on_show` | Gives the document keyboard focus when it's shown. Turn it on for menus, leave it off for HUDs. |

The sample project's `Scenes/main.swscene` has a small HUD that counts pulses and has a
Pulse button. Its files are `Games/Sample/Content/UI/hud.rml` and `hud.rcss`.

## Write a document

Here is a small HUD. The `.rml` file holds the content:

```html
<rml>
<head>
    <title>HUD</title>
    <link type="text/rcss" href="hud.rcss"/>
</head>
<body data-model="game">
    <div class="panel">
        <p>Score: <span id="score">{{score}}</span></p>
        <p id="hint">Find the exit</p>
        <button onclick="pause">Pause</button>
    </div>
</body>
</rml>
```

The `.rcss` file holds the style:

```css
body {
    font-family: "Noto Sans";
    font-size: 18dp;
    color: #f2f4f8;
    width: 100%;
    height: 100%;
    pointer-events: none;
}
.panel {
    position: absolute;
    left: 24dp;
    top: 24dp;
    padding: 12dp;
    background-color: #12161ccc;
    pointer-events: auto;
}
```

A few things to know:

- **Units.** `px` is one pixel of the window. `dp` grows with the display's scale
  setting, so use `dp` for sizes that should look the same on a laptop and a 4K
  monitor. `%`, `vw` and `vh` follow the window size.
- **Clicks through to the game.** `pointer-events: none` on the body lets clicks on
  empty parts of a HUD reach the game. Turn it back on with `pointer-events: auto` for
  panels and buttons.
- **Text.** Use `font-family: "Noto Sans"`, the font that comes with Shockwave, or add
  your own (see [Use your own fonts](#use-your-own-fonts)).
- **The rest.** RML and RCSS follow HTML and CSS closely: flexbox, transitions,
  animations, transforms, images, form controls and templates all work. The
  [RmlUi documentation](https://mikke89.github.io/RmlUiDoc/) covers every element and
  property.

## Show values from game code

Game code reaches the UI through `game.ui()`, the same way it reaches `game.audio()` and
`game.physics()`. Name a document by its entity's name, by its file (`"UI/hud.rml"`) or
by its file name without the extension (`"hud"`). Name an element by its `id`.

```cpp
void beginPlay(GameContext& game) override {
    game.ui().set("score", 0);                         // {{score}} in the document
    game.ui().setText("HUD", "hint", "Find the exit");  // <p id="hint">
}

void frame(GameContext& game, double) override {
    if (game.input().wasActionPressed("pause")) game.ui().toggle("PauseMenu");
}
```

Values you `set` appear wherever a document inside `data-model="game"` writes
`{{name}}`. They can be numbers, `true` or `false`, or text. Documents can use them in
conditions and attributes too, like `data-if="health < 25"` or
`data-class-low="health < 25"`. While you're editing, values that game code hasn't set
yet show as empty.

| Call | What it does |
|---|---|
| `set(name, value)` | Sets a `{{name}}` value: a number, `bool` or text. |
| `get(name)`, `number(name)` | Reads a value back, as text or as a number. |
| `show(doc)`, `hide(doc)`, `toggle(doc)`, `visible(doc)` | Shows or hides a document. This changes its `UIDocument`. |
| `setText(doc, id, text)` | Replaces an element's content with plain text. |
| `text(doc, id)` | Reads an element's text. |
| `setRml(doc, id, rml)` | Replaces an element's content with markup. |
| `setClass(doc, id, name, on)` | Adds or removes a class, to restyle an element from the `.rcss`. |
| `setProperty(doc, id, name, value)` | Sets one style property, like `setProperty("HUD", "bar", "width", "40%")`. |
| `setAttribute(doc, id, name, value)` | Sets an attribute, like `src` on an image. |
| `focus(doc, id)` | Gives an element keyboard focus. |
| `click(doc, id)` | Clicks an element as the mouse would. Handy in tests. |
| `capturesInput()` | True while the mouse is over the UI or a text field has focus. Skip gameplay input then. |

Calls that can't find the document or element return `false` and change nothing. When
Play stops, your handlers and values are dropped and every document reloads, so nothing
done while playing sticks.

## React to clicks and events

The quickest way is a command. In the document, `onclick` names it:

```html
<button onclick="resume">Resume</button>
```

In game code, `on` reacts to it from any document:

```cpp
game.ui().on("resume", [&](const UIEvent&) { game.ui().hide("PauseMenu"); });
```

To react to an element by its `id`, use `onClick`, or `onEvent` for other events. A
handler on an element also runs for clicks on anything inside it.

```cpp
game.ui().onClick("PauseMenu", "quit", [&](const UIEvent&) { game.engine->quit(); });
game.ui().onEvent("Options", "volume", "change", [&](const UIEvent& e) {
    game.engine->setMasterVolume(std::stof(std::string(e.value)) / 100.0f);
});
```

`onEvent` can listen for `click`, `dblclick`, `change`, `submit`, `mouseover`, `mouseout`,
`focus` and `blur`. Each handler gets a `UIEvent`:

| Field | What it holds |
|---|---|
| `document` | The document's entity name, or its path if the entity has no name. |
| `element` | The `id` of the element, or of its nearest parent that has one. |
| `ids` | That `id` and every parent's, innermost first. |
| `type` | The event: `"click"`, `"change"` and so on. |
| `command` | The `onclick` text, for commands. Empty otherwise. |
| `value` | A form control's value, such as a slider's or a text field's. |
| `entity` | The `UIDocument` entity. |

Handlers run on the main thread after the UI has handled the frame's input, so they can
show, hide and change documents freely.

## Use the mouse and keyboard

In the player, the UI gets the mouse, the keyboard and typed text every frame. Text
fields get the operating system's typing support, so accents and input methods work.

In the editor, the UI takes input only during Play, and only while the viewport has the
mouse or keyboard focus. Typing in other panels never reaches the game. While editing,
clicks in the viewport select entities as usual, and the UI is just a preview.

Gameplay keeps getting input too. Check `game.ui().capturesInput()` to ignore a click or
key the UI is using, as the sample does:

```cpp
if (game.input().wasActionPressed("pulse") && !game.ui().capturesInput()) pulse(game);
```

## Use your own fonts

Shockwave ships Noto Sans, regular and bold, which covers Latin, Greek and Cyrillic
text. To use another font, save its `.ttf` or `.otf` file anywhere in `Content`. It
loads by itself, under the family name inside the file:

```css
h1 { font-family: "Press Start 2P"; }
```

You can also load a font from a stylesheet with `@font-face`:

```css
@font-face {
    font-family: "Title";
    src: "fonts/Title-Regular.ttf";
}
```

For other scripts, such as Arabic, Hindi or Japanese, add a font that has their
letters. HarfBuzz then shapes them correctly. The `lang` attribute (`lang="ar"`) helps
it pick the script and direction.

Text with no matching font isn't drawn. The editor's Console and `player --check` say so
and name the missing font.

## Check documents for mistakes

`player --check` loads every document the way the game does and reports what's wrong:

```sh
build/bin/player --check
```

It checks each scene's `UIDocument`s, then every other `.rml` file in `Content`, because
game code can show those too. You'll see lines like these:

```text
Scenes/main.swscene: entity 'HUD': UIDocument UI/hud.rml: Syntax error parsing property declaration 'colr: red;' in UI/hud.rcss: 57.
Content/UI/menu.rml: Closing tag 'body' mismatched on UI/menu.rml:12 was expecting 'p'.
Content/UI/menu.rml: missing font 'Nope' [bold]: no loaded font matches its font-family, so the text isn't drawn. Use "Noto Sans" or add the font file to Content.
```

It reports these mistakes:

- a `UIDocument` with no document, a file that isn't `.rml`, or a file that's missing;
- RML markup errors, such as tags that don't match;
- RCSS errors, such as misspelled properties or bad values;
- fonts that no loaded font file matches;
- images and stylesheets that can't be found.

In the editor, the same problems appear in red in the `UIDocument`'s Inspector, and in
the Console as `LogUI` messages when a document loads or reloads.

## See the UI from the command line

Screenshots include the UI, drawn exactly as players see it:

```sh
build/bin/player --scene Games/Sample/Scenes/main.swscene --screenshot hud.png
```

`player --benchmark` reports what the UI costs: its GPU pass, named `ui`, and its CPU
time.

```sh
build/bin/player --benchmark 5 --json out.json
```

You'll see lines like these, and `ui_update_ms` and `ui_layout_ms` in the JSON:

```text
  pass ui                       0.421 ms
game ui cpu:  0.037 ms update + 0.043 ms layout (1 documents, 6 draws, 220 vertices)
```

## Reload documents

Every document and the files it uses (its stylesheets, templates and images) are watched.
Save any of them and the documents that use them reload within a quarter of a second.
The Console logs `Reloaded UI/hud.rml`. This works in the editor, during Play and in the
player.

A reload starts the document fresh. Text and classes that game code changed are reset,
but `{{values}}` come back straight away because they live in game code.

| To do this | In the editor | In a text file | On the command line |
|---|---|---|---|
| Add a document to a scene | Create > UI document, or drag an `.rml` from Content | a `UIDocument` block in the `.swscene` | |
| Pick the document | Inspector > Document, or drop an `.rml` on it | `document "UI/hud.rml"` | |
| Edit a document | Inspector > Open file, or Content > right-click > Open in text editor | the `.rml` and `.rcss` | |
| Reload everything | Inspector > Reload | | |
| Hide the UI in the viewport | View > Game UI | | |
| Check for mistakes | Inspector, Console | | `player --check` |
| See it | the viewport | | `player --screenshot out.png` |
| Measure it | Window > Stats > Game UI, and the `ui` GPU pass | | `player --benchmark 5` |

## What it costs

The UI lays out on the main thread, and draws in one GPU pass at the end of the frame.
Measured on the sample's HUD at 1280 x 720, on the software Vulkan driver (llvmpipe),
which is much slower than any real GPU:

| Part | Time per frame |
|---|---|
| GPU, `ui` pass | 0.42 ms |
| CPU, update (input, hot reload checks, showing and hiding) | 0.04 ms |
| CPU, layout and building the draw list | 0.04 ms |

With no document visible, the `ui` pass is left out of the frame, so it costs nothing.
Window > Stats shows the same numbers for your own UI while the editor runs, under Game
UI and GPU time per pass.

To keep frames steady, Shockwave's side of the UI doesn't allocate memory each frame
once it has settled. Its draw lists and the renderer's vertex and index buffers are
reused, and grow only when a frame needs more. RmlUi lays out again only what changed. Glyph atlases, the textures that hold rendered letters,
only change when new letters or sizes appear.

## Limits

- Filters, `box-shadow`, gradients and masks (`filter`, `backdrop-filter`, `mask-image`,
  `clip-path` on transformed elements) aren't drawn yet. Borders, backgrounds, images,
  transforms, opacity and clipping all work.
- `{{values}}` from game code are single numbers, `bool`s or text. Lists for `data-for`
  aren't available from game code yet. Build lists with `setRml` instead.
- UI is drawn in sRGB, like a web browser, so colors match what you see in other tools.
- Fonts load once and stay loaded until the program exits. Editing a font file needs a
  restart to show up.
- Gamepad navigation of menus isn't wired up yet. Use `focus` and your own actions.
- The UI isn't thread-safe. Call `game.ui()` only from the main thread: `beginPlay`,
  `frame`, `fixedUpdate` and `endPlay`, never from systems.

## How it works

This section is for the curious.

- **Module.** `Source/Runtime/GameUI` holds `GameUI` (one RmlUi context with its input
  and drawing), `UIWorld` (what `game.ui()` returns) and the `UIDocument` component. It
  links RmlUi, FreeType and HarfBuzz statically. The HarfBuzz font engine comes from
  RmlUi's own sample. `cmake/RmlUi.cmake` fetches all three at pinned versions.
- **Hosts.** The editor and the player each own one `GameUI`. Every frame they call
  `update()`, which matches documents to the scene's `UIDocument`s, checks for changed
  files and feeds input. Then they call `render()`, which lays the documents out and
  hands the draw list to the renderer. `UIWorld` is a Play session subsystem. It finds
  its host's `GameUI` with `GameUI::forEngine`.
- **Drawing.** The renderer gets 2D triangles with a texture, a scissor rectangle and a
  transform, through `Renderer::drawUi` and the `RenderBackend` interface. The Vulkan
  renderer copies them into per-frame buffers and draws them in a `ui` pass after
  tonemapping, straight into the display image at output resolution. The pass is
  declared in the render graph like every other. The headless renderer ignores it.
- **Input.** `Input::events()` keeps the frame's raw keyboard, mouse and text events in
  order, and `GameUI` passes them to RmlUi. The editor passes the mouse position inside
  the viewport instead.
- **Problems.** RmlUi's warnings and errors go to the log under `LogUI`. While a
  document loads, they're also recorded for that document, which is what the Inspector
  and `player --check` show.
- **Licenses.** RmlUi is MIT licensed, FreeType uses the FreeType License, and HarfBuzz
  uses the "Old MIT" license. Their license files are copied to `build/bin/licenses`.
  Noto Sans is under the SIL Open Font License 1.1. Its license sits next to the font,
  in `Source/Runtime/GameUI/Fonts/OFL.txt`, and is copied to
  `build/bin/fonts/NotoSans-OFL.txt`.
