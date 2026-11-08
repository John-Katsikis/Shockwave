# Player preferences

The player's own choices: display, audio, controls, accessibility, camera and language.
They come in layers, the player changes them on the shell's Settings page, and games read
them every frame. Graphics quality, render scale, VSync and the frame cap have their own
file ([Graphics](Graphics.md)); the Settings page shows both.

Every key, with its values and default, is in [PreferencesSchema](PreferencesSchema.md).

## The layers

Each value comes from the highest layer that sets it:

| Layer | Where | Who writes it |
|---|---|---|
| default | the engine | nobody |
| shipped | `<project>/Config/Preferences.swprefs` | the game's makers |
| profile | `<config>/Preferences.swprefs` in the player's [user folder](UserPaths.md) | the Settings page |
| session | `player --pref key=value` | this run only |

The file is plain text:

```
swprefs 1
# Reactor Run plays quieter music by default.
music_volume 0.7
language "pt-BR"
```

Unknown keys, a key given twice, values out of range and NaN are errors, with the line. A
shipped file with an error stops the player; a broken profile is ignored with a warning,
so a player is never locked out by their own file.

The profile keeps only what differs from the shipped values, so a game's update can change
a default for everyone who never touched it.

## From the command line

```sh
player --project Games/ShippingDemo --preferences          # every value and its layer
player --project Games/ShippingDemo --preferences --json - # the same, as JSON
player --project Games/ShippingDemo --pref ui_scale=1.5 --pref language=fr
player --project Games/ShippingDemo --no-user-preferences  # ignore the profile this run
player --preferences-schema docs/PreferencesSchema.md       # write the schema
```

`--audio-volume 0.5` is the same as `--pref master_volume=0.5`. A bad `--pref` fails
before the engine starts, saying which key and why. `player --check` reads the shipped
file too.

## The Settings page

The shell's Settings page ([AppShell](AppShell.md)), from the main menu and the pause
screen, has a tab for each group: Display, Graphics, Audio, Controls, Accessibility and
Language. Each row is `< value >`: two buttons, so a mouse, the arrow keys and a gamepad
drive it the same way, with no sliders to drag.

Changes are a draft until **Apply**, which validates everything first, then uses it, then
saves it. If the save fails (a read-only folder, say) the change still applies for this
run, and the page says why it wasn't saved. **Back** drops the draft. **Defaults** puts the
shipped values in the draft, to apply or not.

Turn the page off with `AppShell::Setup::settings = false`; the Settings button then says
the game has no settings page.

### A display change has to be kept

A new window mode or resolution can leave a player looking at nothing. So after Apply the
page asks **Keep this display?** and counts down from 15 seconds. **Keep** saves it; **Go
back**, the countdown running out, or the game stopping first all put back the display that
worked. The attempt is written to `<config>/Display.pending` before the change, so after a
crash or a forced restart the next start goes back too.

Choices the platform can't do are left out: fullscreen, when the display lists no
resolutions.

### Controls

The Controls tab also rebinds the game's actions, saved to `<config>/Controls.swinput`
beside the preferences: see [Input](Input.md#the-players-own-bindings).

## In the editor

**Window > Player Settings** edits the project's shipped preferences,
`Config/Preferences.swprefs`, grouped as on the Settings page, with Save and Revert. The
file keeps only what differs from the engine's defaults; **Reset** on a row takes it out.
Game code and documents in the editor, in Play too, read these shipped values, apart from
the display and the volumes, which stay the editor's own.

The same window previews, in the viewport, what a player can change that changes how
documents look: **Interface size**, **Language** (each catalog in `Content/Localization`)
and **High contrast**. Previews are the editor's only and are never saved; **Stop
previewing** goes back to the shipped values. **Check layout** looks for text that runs
past its box or off the screen in the documents shown ([Localization](Localization.md#checks)).
The editor also reads the game's catalogs, so `{{text.*}}` shows the game's text, and
reloads them when one is saved.

```sh
editor --project Games/ShippingDemo --player-settings --preview-language fr --preview-ui-scale 1.5
```

## In game code

```cpp
#include <Shockwave/Game.h>

void frame(GameContext& game, double) override {
    const PlayerPreferences& prefs = game.preferences();
    yaw -= mouse.x * speed * prefs.lookSensitivity;
    pitch -= mouse.y * speed * prefs.lookSensitivity * (prefs.invertLookY ? -1 : 1);
    kick *= prefs.cameraShake;   // 0 turns it off
    camera.fov = prefs.fieldOfView > 0 ? prefs.fieldOfView : sceneFov;
}
```

Read them every frame, not once in `beginPlay`, so a change on the Settings page shows at
once. Without an engine (tests, tools) `game.preferences()` is the defaults.

What the engine does itself, and what's left to the game:

| Preference | Used by |
|---|---|
| volumes, `mute_when_unfocused` | the engine: the master volume and each [audio category](Audio.md#volume-by-category) |
| `stick_dead_zone` | the engine: every gamepad stick |
| `ui_scale` | the engine: every document's `dp` units, the shell's and the game's |
| `high_contrast` | the engine sets `body.high_contrast` on every document; styles say what it changes |
| `language` | the engine: [Localization](Localization.md) |
| display keys | the engine: the window |
| `look_sensitivity`, `invert_look_y`, `field_of_view`, `camera_shake`, `reduce_flashes`, `toggle_hold`, `subtitles`, `subtitle_scale` | the game |

Reactor Run uses every one it has a use for: look sensitivity and inverted look on the
mouse and stick, field of view on its camera, camera shake on the recoil kick, reduce
flashes on the hurt overlay (a faint edge instead of a red screen), and toggle instead of
hold on sprint. It has no speech, so no subtitles.

## Tests

`player_preferences` (the format, layers, apply and save, the display keep-or-revert and
after a crash), `game_flow` (the Settings page through its buttons, and its layout in
long and Greek text from 720p to 4K), `editor_player_settings` (the editor's French
preview at 1.5x, writing nothing into the project) and
`shipping_demo_flow` (Reactor Run's Settings page through the real player, the saved
profile, `--preferences` and `--pref`). See [ShippingVerification](ShippingVerification.md).
