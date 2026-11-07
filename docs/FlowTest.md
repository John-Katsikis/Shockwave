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
| `{"quit": true}` | ends the run, passed |

Any step can have `"timeout"`, in seconds. The default is 10. The scenario's own
`timeout` (default 60) bounds the whole run.

A scenario has no other step kinds, no shell commands and no scripts. The reader is
strict: an unknown key, a wrong type, two step kinds in one step or an unknown phase or
code is an error, and the error names the step.

Steps run in order:

- A click, a press, a travel or a load lets one frame run before the next step, because
  travel happens after the frame and screens show a frame after what opens them.
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
