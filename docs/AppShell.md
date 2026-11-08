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
  confirm page. That's when the campaign (`game.campaign()`) holds something; confirming
  clears it.
- **Continue** loads the newest save this game and profile can load, travelling to its
  level ([Save games](SaveGames.md#saves-from-another-level)). Without one it's disabled,
  saying why ("No saved game to continue yet", or why the newest can't load).
- **Restart checkpoint** loads the last checkpoint reached in this campaign
  ([Save games](SaveGames.md#checkpoints)), and returns to the game. Until one is
  written it's disabled, saying why.
- **Settings** opens the Settings page: display, graphics, audio, controls, accessibility
  and language, with Apply, Defaults and Back ([Preferences](Preferences.md#the-settings-page)).
  A display change asks to be kept within 15 seconds.
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

The shell raises an [input layer](Input.md#who-has-the-input) while a screen shows (`menu`,
`loading`, or `modal` for an error), so the game's `gameplayFocused()` is false under it.

The Settings page's Controls tab rebinds the game's actions
([Input](Input.md#the-players-own-bindings)): `bind_key` and `bind_pad` with the action
listen for a press, `cancel_listen` stops, and `reset_bindings` puts the project's back.
Its values are `settings_bindings` (each with `action`, `label`, `keyboard`, `gamepad`
and `conflict`), `listening` and `listening_text`.

When the gamepad in use is unplugged during a level, the shell pauses it and says so
([Input](Input.md#controllers-coming-and-going)); `Setup::pauseOnControllerLoss = false`
turns that off. `Setup::input` and `Setup::controlsFile` say whose input it reads and
where the player's bindings go (the engine's, and beside the preferences, by default).

A page too tall for the screen (a long Controls list at 720p, a large Interface size)
scrolls inside its panel, and moving the focus scrolls it into view.

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
| `settings_tab` (on `id="tab-<tab>"`), `pref_next` and `pref_prev` (on `id="next-<key>"`, `id="prev-<key>"`; graphics keys as `graphics.<key>`), `apply_settings`, `cancel_settings`, `reset_settings`, `keep_display`, `revert_display` | the Settings page, from the menu and pause |
| `cancel_travel` | loading |
| `retry`, `dismiss`, `main_menu`, `quit` | error |

| `flow` value | What it is |
| --- | --- |
| `screen`, `page` | the screen showing (`none`, `menu`, `pause`, `loading`, `error`) and the menu's page (`main`, `levels`, `settings`, `credits`, `confirm`) |
| `title`, `level`, `credits` | the project's name, the level playing, the credits text (the project's `Content/Shell/credits.txt` when it has one, else "Made with Shockwave") |
| `destination`, `phase`, `progress`, `percent`, `progress_width`, `has_progress`, `can_cancel` | the travel under way: where to, its phase, 0..1, 0..100, `"45%"` for a bar's width |
| `error_code`, `error_message`, `error_level`, `can_dismiss` | the failure: its stable code (`LoadFailed`, ...), why, which level |
| `checkpoint_state` | the last checkpoint's save: `saving`, `saved`, `failed`, or empty |
| `can_new_game`, `new_game_reason`, `can_continue`, `continue_reason`, `can_restart_checkpoint`, `checkpoint_reason`, `can_level_select`, `levels` | what the buttons can do now, and why not |
| `can_settings`, `settings_reason`, `settings_tab`, `settings_tabs` (`id`, `label`), `settings_rows` (`key`, `label`, `value`, `tooltip`, `can_change`, `reason`), `settings_changed`, `display_pending`, `display_countdown` | the Settings page |
| `error_detail` | the error's code and level, as one sentence |
| `text` | the screens' words, by key: `{{text.shell.new_game}}` ([Localization](Localization.md)) |
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
