# Flow tests

A flow test is a scripted run through a game's screens and levels. It clicks the
application shell's buttons ([AppShell.md](AppShell.md)), asks for travel, waits for
tickets, and checks where it ended up. CI and agents use it to show the menu, the
levels and the loading and error screens work together, without a person playing.

```
player --project Game.swproject --flow-test scenario.json --json report.json
player --project Game.swproject --flow-test scenario.json --renderer null   # no GPU
```

It runs in a hidden window with a fixed time step. The exit codes are:

- 0 when every step passed;
- 1 at the first step that failed, with the step and the reason on stderr;
- 2 when the scenario itself is wrong.

Use `--app-shell all` when the project doesn't turn on the screens the scenario clicks.

## A scenario

```json
{
  "format": 1,
  "name": "menu, a, b, restart b, menu",
  "timeout": 60,
  "steps": [
    {"assert_value": "flow.screen", "equals": "menu"},
    {"click": "new_game"},
    {"assert_value": "flow.screen", "equals": "loading"},
    {"wait_phase": "succeeded"},
    {"assert_level": "a"},
    {"travel": "b", "spawn": "entrance"},
    {"wait_phase": "succeeded"},
    {"press": "pause"},
    {"click": "restart_level"},
    {"wait_phase": "succeeded"},
    {"press": "pause"},
    {"click": "main_menu"},
    {"wait_phase": "succeeded"},
    {"assert_level": "menu"},
    {"quit": true}
  ]
}
```

| Step | What it does |
| --- | --- |
| `{"click": "id"}` | clicks the element with that id on the shell's screen, as the mouse would. It waits for the element to show. |
| `{"click": "id", "document": "PauseMenu"}` | the same, in one of the level's own UI documents (by its entity name) |
| `{"press": "pause"}` | what the pause key does: opens or closes the pause screen. `pause` is the only action v1 presses. |
| `{"travel": "level", "spawn": "id"}` | a request, as the application makes one. Refused fails the step. |
| `{"travel": "level", "expect": "UnknownLevel"}` | a request that must be refused with that code |
| `{"wait_phase": "succeeded"}` | waits for the latest ticket to reach that phase. The phases are `awaiting_boundary`, `failed`, `cancelled`, and the others in [LevelLifetime.md](LevelLifetime.md#travel-tickets-game-flow). A ticket that ends another way fails the step. |
| `{"assert_level": "a"}` | the level playing now is `a` |
| `{"assert_value": "flow.screen", "equals": "pause"}` | a value of the shell's `flow` model, or of the level's `game` model (`game.score`), as text |
| `{"save": "slot"}`, `{"load": "slot"}` | saves or loads a slot of the level playing, waiting for the file |
| `{"hold": "key W", "until": "game.near", "equals": "Console"}` | holds a key, mouse button, gamepad button or stick until a value is that, then lets go |
| `{"hold": "pad_axis left_y +", "value": 0.8, "seconds": 1.5}` | holds it, this far (0 to 1), for that many seconds of game time |
| `{"tap": "key E"}` | presses it for one frame, then lets go |
| `{"look": [120, -20], "frames": 4}` | moves the mouse that many pixels, spread over frames (default 1) |
| `{"unplug": "gamepad"}` | pulls the virtual gamepad's cable: its held buttons are let go and it's gone until a gamepad binding is pressed again |
| `{"wait": 0.5}` | waits that many seconds of game time |
| `{"wait_value": "game.objective", "equals": "done"}` | waits until a value is that |
| `{"quit": true}` | ends the run, passed |

### Real input

`hold`, `tap` and `look` are real input. They go in at the device boundary
(`Input::inject` and `Input::injectMouseMotion`), named the way
[a bindings file](InputSchema.md) names them, so the game reads them through its
bindings exactly as it reads the keyboard, the mouse or a pad. Rebinding an action
changes what a scenario has to press, as it would for a player.

- A `pad` or `pad_axis` binding plugs in a virtual gamepad on first use, reported as an
  Xbox pad. Joysticks can't be simulated.
- A hold always lets go when its step ends, passed, failed or timed out.
- A held key carries across travel; the next level ignores it until it's released, as
  it does for a real key (S05's hold-until-released).

A scenario that plays a whole game this way is the proof the game can be finished. A
test may still use `travel` to skip ahead, but nothing in a scenario can set a game's
success flag: only playing does. `Games/ShippingDemo/Tests/flow` plays Reactor Run from
the menu to the credits on a keyboard and mouse and on a gamepad, and saves, quits and
continues across two runs.

Any step can have `"timeout"`, in seconds. The default is 10. The scenario's own
`timeout` (default 60) bounds the whole run.

A scenario has no other step kinds, no shell commands and no scripts. The reader is
strict: an unknown key, a wrong type, two step kinds in one step or an unknown phase or
code is an error, and the error names the step.

Steps run in order:

- A click, a press, a travel, a load, a hold, a tap or a look lets one frame run before
  the next step, because travel happens after the frame and screens show a frame after
  what opens them.
- An assertion doesn't wait.
- The latest ticket is the last one a step, a click or the level itself started.

## The report

`--json report.json` (`-` for stdout) writes:

- `ok`, `state`, `failed_step` and `message`;
- every step run, with its milliseconds, frames and message;
- `now`: where things stood when it ended (the level, its epoch, whether it plays, the
  active ticket and phase, the shell's screen and page, and the error it shows);
- `travel`: every ticket that ended, with its status, code and duration.

## What it proves, and what it doesn't

With `--renderer null` it tests the flow's logic: which buttons do what, the order of
phases, refusals, failures and recovery. That can't show anything was drawn right. With
the Vulkan renderer the same scenario draws every screen. Even then, only a screenshot
or a person shows how it looks.

`Tests/fixtures/flow` holds the scenarios the tests run against `Tests/fixtures/levels`:

- `game_flow_cli` runs them headless;
- `game_flow_vulkan` runs the milestone on the GPU.
