# Input

Games read named **actions**, never keys: `input.isActionDown("fire")`, not "is the left
mouse button down". The project binds actions to keys, mouse buttons, gamepad buttons and
sticks in `Config/Input.swinput` (every field is in [InputSchema](InputSchema.md)), the
editor's Input window edits that file, and players rebind them on the shell's Settings
page.

## Who has the input

Something drawn over the game can take the input from it. Each owner raises a layer, and
the highest one has the input:

| Layer | Raised by |
|---|---|
| `gameplay` | nothing: the game has it |
| `hud` | a HUD that wants input but leaves the game playing |
| `menu` | the shell's menu and pause screens; any game UI with keyboard focus |
| `loading` | the shell's loading screen |
| `modal` | an error or a question that must be answered |

```cpp
input.raiseLayer(this, InputLayer::Menu);   // my inventory screen is open
input.lowerLayer(this);                     // closed
if (input.gameplayFocused()) fire();        // nothing above the HUD has the input
```

Read `gameplayFocused()` before acting on gameplay actions. Online games keep simulating
under a menu, but send neutral input while it's false. Reactor Run does both: its
character only moves, looks and shoots while `gameplayFocused()`.

### No stray presses

The press that closes a menu must not also fire the gun. So when the top layer changes to
or from one above `hud`, every action held at that moment reads as released (value 0,
never pressed) until it is let go: a **barrier**. A barrier also comes when:

- the window loses focus (Alt+Tab with W held doesn't leave the player walking);
- a controller is plugged in (its buttons may report held as it connects);
- a new level starts ([LevelLifetime](LevelLifetime.md)).

Unplugging a controller isn't a barrier: the actions it held simply let go.

## The player's own bindings

The Settings page's Controls tab lists every action except the `ui_*` ones and `pause`,
with its keyboard and gamepad binding. Choosing one listens for 10 seconds for a press on
that kind of device. Escape (keyboard) or Start (gamepad) cancels and can't be bound. A
binding another action already uses is shown on both: "Left mouse also does Fire".

**Apply** saves the actions that differ from the project's to `<config>/Controls.swinput`
in the player's [user folder](UserPaths.md); **Default controls** puts the project's back.
The player loads that file at start. The player's bindings replace the project's for each
action they name, so a game update that adds or renames actions keeps the rest.

```cpp
input.setPlayerBindings(bindings);   // what the Settings page does on Apply
input.projectActions();               // the actions without the player's changes
bindingLabel(binding, pad);           // "Left mouse", "Cross" on a PlayStation pad
```

## Controllers coming and going

`input.devices()` lists what's connected and `devicesVersion()` changes whenever that
list does. `lastUsedGamepad()` says whether the player last pressed something on a pad,
so prompts can show pad buttons.

When the pad in use is unplugged during a level, the shell pauses it (offline) and says
"Controller disconnected. Plug it back in, or carry on with the keyboard and mouse."
Plugging it back says so, and any key or button carries on. Turn this off with
`AppShell::Setup::pauseOnControllerLoss = false`. A [flow test](FlowTest.md) unplugs the
pad with `{"unplug": "gamepad"}`.

## Tests

`input` (layers, the barrier after a menu and on focus loss, the player's bindings over
the project's), `game_flow` (the shell's layers, rebinding through the Controls tab, the pad
going away), and `shipping_demo_flow` (Reactor Run rebinding fire to F, cancelling a
listen, unplugging the pad mid-level, and the saved `Controls.swinput`).
