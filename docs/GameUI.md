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
    preview "score=1200; items=[Sword, Shield]"
end
```

| Setting | Text name | What it does |
|---|---|---|
| Document | `document` | The `.rml` file, as a path inside `Content`. |
| Visible | `visible` | Shows or hides the document. Game code changes this with `game.ui().show()` and `hide()`. |
| Order | `order` | Which document is on top. Higher numbers draw on top and get the mouse first. |
| Focus on show | `focus_on_show` | Gives the document keyboard and gamepad focus when it's shown. Turn it on for menus, leave it off for HUDs. See [Menus with the keyboard and gamepad](#menus-with-the-keyboard-and-gamepad). |
| Preview | `preview` | Stand-in `{{values}}` for the editor while you're not playing. See [Preview values while editing](#preview-values-while-editing). |

The sample project's `Scenes/main.swscene` has a small HUD in the bottom left that counts
pulses and has a Pulse button, and a pause menu (press P, or Start on a gamepad) with a
list of recent pulses. Their files are in `Games/Sample/Content/UI/`.

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
`{{name}}`. They can be numbers, `true` or `false`, text, or lists and records (see
[Fill lists from game code](#fill-lists-from-game-code)). Documents can use them in
conditions and attributes too, like `data-if="health < 25"` or
`data-class-low="health < 25"`. While you're editing, values that game code hasn't set
yet show their preview value, or their own name, like `{{score}}`. During Play they show
as empty until game code sets them.

| Call | What it does |
|---|---|
| `set(name, value)` | Sets a `{{name}}` value: a number, `bool`, text, or a `UIValue` list or record. |
| `get(name)`, `number(name)`, `value(name)` | Reads a value back, as text, as a number, or as a `UIValue`. |
| `show(doc)`, `hide(doc)`, `toggle(doc)`, `visible(doc)` | Shows or hides a document. This changes its `UIDocument`. |
| `setText(doc, id, text)` | Replaces an element's content with plain text. |
| `text(doc, id)` | Reads an element's text. |
| `setRml(doc, id, rml)` | Replaces an element's content with markup. |
| `setClass(doc, id, name, on)` | Adds or removes a class, to restyle an element from the `.rcss`. |
| `setProperty(doc, id, name, value)` | Sets one style property, like `setProperty("HUD", "bar", "width", "40%")`. |
| `setAttribute(doc, id, name, value)` | Sets an attribute, like `src` on an image. |
| `focus(doc, id)` | Gives an element keyboard focus. |
| `focused(doc)` | The `id` of the focused element in a document, or `""`. |
| `navigate(step)` | Moves around a menu as the arrow keys or d-pad would: `UINav::Up`, `Down`, `Left`, `Right`, `Confirm` or `Back`. |
| `click(doc, id)` | Clicks an element as the mouse would. Handy in tests. |
| `capturesInput()` | True while the mouse is over the UI or a text field has focus. Skip gameplay input then. |

Calls that can't find the document or element return `false` and change nothing. When
Play stops, your handlers and values are dropped and every document reloads, so nothing
done while playing sticks.

## Fill lists from game code

A document repeats an element for every item in a list with `data-for`. Each item can be
a plain value or a record with named fields:

```html
<div class="inventory">
    <p data-for="item : inventory">{{item.name}} x{{item.count}}</p>
    <p data-if="inventory.size == 0">Your bag is empty</p>
</div>
```

Game code builds the list with `UIValue` and sets it like any other value:

```cpp
UIValue items = UIValue::list();
for (const Item& item : m_inventory)
    items.push(UIValue::record({{"name", item.name}, {"count", item.count}}));
game.ui().set("inventory", items);
```

Short lists can be written in one go: `UIValue::list({"Sword", "Shield"})`, shown with
`{{it}}` (the default name for each item) or a name you pick in `data-for`. Lists and
records can hold other lists and records. `{{inventory.size}}` is the number of items,
and `{{inventory[0].name}}` reaches into one.

Set the whole list again whenever it changes. Only the parts of the document that use it
are updated, so this is cheap for the lists a menu or HUD shows.

## Preview values while editing

While you're editing, a `{{value}}` that game code hasn't set shows its own name, like
`{{score}}`, so you can see where values go. To judge a layout with realistic text, give
the document preview values in the Inspector's Preview field, or in the scene file:

```text
preview "score=1200; player=Ana; inventory=[{name=Sword, count=1}, {name=Rope, count=3}]"
```

Write one `name=value` per entry, separated by `;` or new lines. A value is written like
this:

| Value | Example |
|---|---|
| A number | `12`, `-3.5` |
| `true` or `false` | `true` |
| Text | `Ana`, or `"Text, with commas"` in quotes |
| A list | `[Sword, Shield]` |
| A record | `{name=Sword, count=1}` |

Preview values only fill in for values game code hasn't set, and they're never used
during Play or by the player, unless you ask the player for them (see
[See the UI from the command line](#see-the-ui-from-the-command-line)). The Inspector
lists the values a document uses under its Document field. A mistake in a preview shows
in red there, and `player --check` reports it too.

## Menus with the keyboard and gamepad

Turn on Focus on show (`focus_on_show 1`) for a menu. When it's shown, it takes focus:
its button with the `autofocus` attribute, or else its first button or control, gets the
focus. Players then move around it like this:

| To do this | Keyboard | Gamepad | Action |
|---|---|---|---|
| Move between buttons | Arrow keys | D-pad or left stick | `ui_up`, `ui_down`, `ui_left`, `ui_right` |
| Press the focused button | Enter | South (Cross, A) | `ui_confirm` |
| Go back | Escape or Backspace | East (Circle, B) | `ui_back` |

Holding a direction repeats after a moment. These are ordinary input actions, so you can
rebind them in `Config/Input.swinput` or the editor's Input window. In the player,
Escape still quits the game too, as it always has.

Focus moves to the nearest button or control in that direction. Buttons, inputs,
checkboxes, sliders and selects can all take focus. To make another element focusable,
give it `tab-index: auto` in the stylesheet. To send focus somewhere specific, name the
element in the stylesheet, and `player --check` reports names that don't exist:

```css
#play { nav-down: #options; }
#quit { nav-down: #play; }   /* wrap around at the bottom */
#title { nav: none; }        /* never move from here */
```

Style the focused button with `:focus-visible`, so players can see where they are:

```css
button { border: 2dp transparent; }
button:focus-visible { border-color: #ffffff; }
```

Confirm presses the focused button, just like a click, so `onclick` commands and
`onClick` handlers run. On a checkbox it toggles the box; on a slider, left and right
change the value; on a select, it opens the list.

**Back** sends a `"back"` event. Name the command it sends with `data-back` on the menu's
body or on any element inside it. The nearest one to the focused element wins:

```html
<body data-model="game" data-back="resume">
```

```cpp
game.ui().on("resume", [&](const UIEvent&) { game.ui().hide("PauseMenu"); });
```

Without `data-back`, handle it with `onEvent(doc, element, "back", ...)`.

Navigation only takes over while a menu (a shown document with Focus on show) is up, or
something in a document has focus. A HUD on its own never takes the d-pad or arrow keys
from your game. While a menu has focus, `capturesInput()` is true, so gameplay can skip
its own input. While a text field has focus, the keyboard's arrows, Enter and Backspace
edit the text, and only the gamepad navigates.

## Shadows and gradients

`box-shadow` and gradient decorators work like they do in CSS:

```css
.panel {
    box-shadow: #00000080 0 6dp 18dp 0;    /* color, x, y, blur, spread */
    decorator: linear-gradient(180deg, #1d232b, #101318);
}
.badge { decorator: radial-gradient(circle, #ffd27a, #ff8c33); }
.dial  { decorator: conic-gradient(#ff8c33, #3d7bd9, #ff8c33); }
```

`linear-gradient`, `radial-gradient` and `conic-gradient` all work, and so do their
`repeating-` versions. `box-shadow` takes several shadows and `inset` shadows too.

Shadows and gradients are worked out once, when an element first appears or changes
size, and kept as small textures. After that they draw like any image, so they cost
nothing extra per frame. Working one out takes a few milliseconds on the main thread for a
large soft shadow, with the work spread across every CPU core: the sample's pause menu
shadow (472 x 370 pixels) took about 5 ms on a slow 4-core cloud machine, so expect a
millisecond or two on a desktop. Gradients take well under a millisecond.

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
`focus`, `blur` and `back`. Each handler gets a `UIEvent`:

| Field | What it holds |
|---|---|
| `document` | The document's entity name, or its path if the entity has no name. |
| `element` | The `id` of the element, or of its nearest parent that has one. |
| `ids` | That `id` and every parent's, innermost first. |
| `type` | The event: `"click"`, `"change"`, `"back"` and so on. |
| `command` | The `onclick` text for commands, or the `data-back` text for `"back"`. Empty otherwise. |
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
- images and stylesheets that can't be found;
- `nav-up`, `nav-down`, `nav-left` and `nav-right` that name an element that isn't there;
- preview values that aren't written correctly.

In the editor, the same problems appear in red in the `UIDocument`'s Inspector, and in
the Console as `LogUI` messages when a document loads or reloads.

## See the UI from the command line

Screenshots include the UI, drawn exactly as players see it:

```sh
build/bin/player --scene Games/Sample/Scenes/main.swscene --screenshot hud.png
```

To see the preview values in a screenshot, add `--ui-preview`. To see a menu after some
keyboard or gamepad steps, add `--ui-nav` with the steps, separated by commas. They run
one a frame, from the fifth frame, before the screenshot:

```sh
build/bin/player --scene Games/Sample/Scenes/main.swscene --ui-preview --screenshot preview.png
build/bin/player --ui-nav down,confirm,down --screenshot menu.png
```

The steps are `up`, `down`, `left`, `right`, `confirm` and `back`.

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
| Make a menu take the keyboard and gamepad | Inspector > Focus on show | `focus_on_show 1` | `player --ui-nav down,confirm` |
| See values before Play | Inspector > Preview | `preview "score=12"` | `player --ui-preview` |
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

- Filters and masks (`filter`, `backdrop-filter`, `mask-image`) aren't drawn yet: the
  element shows without them, except `filter: opacity()`, which works. Rounded corners
  don't clip an element's children. Borders, backgrounds, images, gradients, shadows,
  transforms, opacity and rectangular clipping all work.
- A shadow is worked out on the CPU when its element first appears or changes size. An
  element whose size animates every frame works its shadow out every frame. Give such an
  element no shadow, or animate `transform: scale()` instead, which keeps the shadow.
- UI is drawn in sRGB, like a web browser, so colors match what you see in other tools.
- Fonts load once and stay loaded until the program exits. Editing a font file needs a
  restart to show up.
- Mouse hover doesn't move the keyboard and gamepad focus.
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
- **Navigation.** The `ui_*` actions drive RmlUi's own keyboard navigation (its `nav`
  properties, with `auto` where a stylesheet doesn't say), so the arrow keys and the
  d-pad behave the same. `GameUI::bindDefaultControls` binds them where nothing else has.
- **Values.** Every name a document's bindings use is found in its `.rml` and bound
  before RmlUi reads it, so values game code sets later appear without a reload.
  `UIValue` is the value type, with lists and records for `data-for`.
- **Shadows and gradients.** Gradients are baked into a small texture per element and
  drawn as ordinary UI triangles, so the Vulkan UI pass needs no new shader. RmlUi draws
  a `box-shadow` into an off-screen layer once and keeps it as a texture; Shockwave
  records what goes into those layers and rasterizes and blurs it on the CPU
  (`Source/Runtime/GameUI/Private/UIEffects.cpp`).
- **Problems.** RmlUi's warnings and errors go to the log under `LogUI`. While a
  document loads, they're also recorded for that document, which is what the Inspector
  and `player --check` show.
- **Licenses.** RmlUi is MIT licensed, FreeType uses the FreeType License, and HarfBuzz
  uses the "Old MIT" license. Their license files are copied to `build/bin/licenses`.
  Noto Sans is under the SIL Open Font License 1.1. Its license sits next to the font,
  in `Source/Runtime/GameUI/Fonts/OFL.txt`, and is copied to
  `build/bin/fonts/NotoSans-OFL.txt`.
