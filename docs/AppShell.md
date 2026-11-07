# Application shell

The application shell is the game's own set of screens around its levels: the main menu,
the pause screen, the loading screen and the error screen. The player draws them on top
of whatever level is playing. Every screen can be switched off, and a project can replace
any of them with its own document.

```
app_shell all                 # in the .swproject: every screen
app_shell menu pause loading  # or just these
app_shell off                 # none: the game draws its own
```

Without the key, `loading` and `error` are on. They only show while a travel is under way
or after one failed, so a game that never travels never sees them. `player --app-shell`
overrides the key for one run, and takes the same values (put a list in quotes).

## The screens

| Screen | Shows | Buttons |
| --- | --- | --- |
| `menu` | over a level whose catalog `kind` is `menu` | New game, Continue, Level select, Settings, Credits, Quit |
| `pause` | when the `pause` action is pressed in a gameplay level | Resume, Restart checkpoint, Restart level, Settings, Main menu, Quit |
| `loading` | while a travel ticket is active | Cancel, while cancelling is still safe |
| `error` | after a travel failed | Retry, Back to the level (when it still plays), Main menu, Quit |

`level_select` adds Level select to the menu, listing the catalog's gameplay levels.

What each button does:

- **New game** goes to the menu level's `next` level, or else to the catalog's first
  gameplay level. If the game has progress to lose, it asks first, on the menu's
  confirm page.
- **Continue** and **Restart checkpoint** are disabled for now. Each shows why ("No saved
  game to continue yet") until campaign saves arrive (S06).
- **Settings** opens a page that says player settings aren't available yet. They arrive
  with S08.
- **Restart level** goes to the same level again, at the spawn it was entered at.
- **Main menu** goes to the catalog's first menu level.
- **Cancel** cancels the travel. Until the old level starts ending, it plays on as if
  nothing happened, still paused if you came from the pause screen. After that point the
  button is hidden.
- **Retry** asks for the failed level again, as the application.
- **Back to the level** dismisses the error. It is only there when the level you were in
  is still playing, for example after a scene that didn't load or a missing spawn.

After a level fails to start, nothing plays, so the error screen has only Retry, Main
menu and Quit. Those still work: the application may travel when nothing plays, even
though a level may not.

Pressing a button twice travels once. The second press is refused as `Busy` and the first
carries on.

## Input

The pause screen opens and closes with the `pause` input action. It is bound to Escape
and a gamepad's Start unless `Config/Input.swinput` binds it. While a screen shows:

- the shell has the keyboard, the mouse and the gamepad;
- the level's own UI and its buttons don't get input;
- the pause screen pauses the level's session (online, the session keeps running).

The key press that opened a screen doesn't also count as `ui_back` on it, or as an input
in the game underneath. Its actions read as released until you let go. If the game had
captured the mouse, the shell frees it while a screen shows and captures it again when
you return to the same level.

The arrow keys, Enter and Escape (and the d-pad, South and East) move around the screens,
the same as in any game UI (see [Game UI](GameUI.md#menus-with-the-keyboard-and-gamepad)).

## Your own screens

Put `Content/Shell/menu.rml`, `pause.rml`, `loading.rml` or `error.rml` in the project,
and it replaces the built-in screen. Bind values with `data-model="flow"`. Make each
button's `onclick` one of the shell's commands:

```html
<body data-model="flow" data-back="back">
  <h1>{{title}}</h1>
  <button id="new_game" onclick="new_game">Play</button>
  <button id="quit" onclick="quit">Quit</button>
</body>
```

If a document doesn't load cleanly, the shell shows the built-in screen instead and logs
why. That includes a missing file, a broken font or stylesheet, and bad markup. The
built-in screens use only the engine's own font, with no images, stylesheets or project
files, so the error screen still shows when the project's files are what's broken.
`player --check` checks `Content/Shell` documents against the `flow` model.

| Commands | Where |
| --- | --- |
| `new_game`, `confirm_new_game`, `continue`, `level_select`, `play_level` (on an element with `id="level-<id>"`), `settings`, `credits`, `back`, `quit` | the menu |
| `resume`, `restart_checkpoint`, `restart_level`, `settings`, `main_menu`, `quit` | pause |
| `cancel_travel` | loading |
| `retry`, `dismiss`, `main_menu`, `quit` | error |

| `flow` value | What it is |
| --- | --- |
| `screen`, `page` | the screen showing (`none`, `menu`, `pause`, `loading`, `error`) and the menu's page (`main`, `levels`, `settings`, `credits`, `confirm`) |
| `title`, `level`, `credits` | the project's name, the level playing, the credits text |
| `destination`, `phase`, `progress`, `percent`, `progress_width`, `has_progress`, `can_cancel` | the travel under way: where to, its phase, 0..1, 0..100, `"45%"` for a bar's width |
| `error_code`, `error_message`, `error_level`, `can_dismiss` | the failure: its stable code (`LoadFailed`, ...), why, which level |
| `can_new_game`, `new_game_reason`, `can_continue`, `continue_reason`, `can_restart_checkpoint`, `checkpoint_reason`, `can_level_select`, `levels`, `settings_reason` | what the buttons can do now, and why not |
| `message` | the last refusal, like Busy |

## How it works

- `GameFramework/AppShell.h` holds the shell. It has its own `GameUI` (its own RmlUi
  context, a `flow` data model, and not the one `game.ui()` finds) and a small world of
  its own with one `UIDocument` per screen.
- It reaches the travel coordinator through the `LevelHost` the player owns for the whole
  run, never through a level's `GameContext`, and follows travel with a `GameFlow`
  subscription.
- Each frame the player updates the shell before the level's frame and UI. It turns the
  level UI's input off while a screen shows, and draws the shell last.
- The built-in documents are in `GameFramework/Private/AppShellDocuments.cpp`.
- `player --flow-test` ([FlowTest.md](FlowTest.md)) clicks the same buttons.

The editor's Play doesn't run the shell yet; it plays levels as before.
