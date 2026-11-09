# Reactor Run

The shipping plan's finishable reference game (S07). It's small on purpose, but it's a
whole game: a main menu, a training level, a factory level with a checkpoint, results,
an ending with credits, and back to the menu to start again. It runs on Shockwave's own
level travel, application shell, save games and circuits, so it's also the proof that a
game made with them can be finished.

## Play

From the repository root:

```sh
cmake --build build --target player game_shippingdemo
./build/bin/player --project Games/ShippingDemo/ShippingDemo.swproject
```

Choose **New game** on the main menu.

Or package it and play it the way a tester gets it, from outside the source tree:

```sh
./build/bin/player --package --project Games/ShippingDemo/ShippingDemo.swproject
```

`Config/Package.swpackage` names the game Reactor Run and its program `ReactorRun`, and
leaves the docs and tests out. The command prints where the package is and how to run it.

| Action | Keyboard and mouse | Gamepad |
| --- | --- | --- |
| Move | W A S D | left stick |
| Look | mouse | right stick |
| Fire | left mouse button | right trigger |
| Reload | R | West (X on Xbox, Square on PlayStation) |
| Use | E | South (A on Xbox, Cross on PlayStation) |
| Sprint | Left Shift | left stick click |
| Pause | Escape | Start |

The bindings live in `Config/Input.swinput`, and the HUD's prompts name whatever is bound
there, on the device you used last.

- **Training** teaches the controls: walk to the firing line, shoot three targets, pick
  up the ammo and open the gate at the console. Walking through the exit beyond the
  gate finishes training and autosaves.
- **The factory**: take out the two sentries, which opens the security door. Pass the
  checkpoint, restart the reactor at its console and hold the reactor room for eight
  seconds while a guard attacks. Then use the exit to see your results and go on.
- **The ending** shows the credits, with a button back to the main menu.

If you go down, you can restart from the last checkpoint, restart the level, or go to
the main menu. **Continue** on the main menu resumes from your newest save (the
checkpoint, or the autosave between levels). Health pickups only go when you're hurt,
and ammo only when your reserve isn't full.

## What's where

| Path | What it is |
| --- | --- |
| `ShippingDemo.swproject` | The project: shell on, levels load their own assets, destroyed things stay gone. |
| `Config/Levels.swlevels` | menu, training, factory, ending |
| `Scenes/` | The four levels |
| `Content/Prefabs/` | Player, Target, Enemy, Console, Ammo pickup, Health pickup |
| `Content/UI/` | HUD, results, down and credits screens, per-level badges, `demo.rcss` |
| `Content/Circuits/TrainingExit.swcircuit` | Training's exit, made with a circuit instead of C++ |
| `Content/Shell/credits.txt` | The shell's Credits page |
| `Source/DemoComponents.*` | The game's components and their saved fields |
| `Source/DemoRules.*` | One fixed tick of the rules, and the content checks |
| `Source/DemoGame.*` | The GameMode: input, HUD, sounds, checkpoints, results, travel |
| `Tests/DemoTest.cpp` | `shipping_demo`: the rules on the real scenes |
| `Tests/flow/*.json` | `shipping_demo_flow`: the whole game with real input |
| `Tools/GenerateDemoAssets.py` | Makes every model, sound and image (CC0) |
| `Docs/Playtest.md` | The playtest script for a person |

## Making levels with it

The level content is all components, so it's edited in the Inspector:

- **`ObjectiveProgress`** is one step of a level. Steps are done in `order`.
  - `Reach`: come within `radius` of `target`.
  - `Destroy`: things with `Health.group` equal to `group` are destroyed (`required`, or
    all of them).
  - `Collect`: pickups of `group` are collected.
  - `Use`: the console `target` is used.
  - `Hold`: stay within `radius` of `target` for `seconds`.

  Destroyed things and taken pickups stay gone when a level is entered again. `Destroy`
  and `Collect` count them as already done: the game keeps each one's group in the
  campaign (`demo.killed.<id>`, `demo.collected.<id>`).
- **`InteractionTarget`**: `Use` (a console), `Pickup` (`item` is `ammo` or `health`,
  with an `amount`), or `Exit` (leaves once every objective is done).
- **`DemoGate`** opens when its `objective` completes.
- **`CampaignProgress`**, one per level: the completion it records (`complete_id`), the
  level its exit leads to (`next`), and whether results show first.
- **`DemoEnemy`** with `Health` and a `Collider`: it walks straight at the player once it
  sees them, and fires on an interval.

A level whose content doesn't add up doesn't start: the error lists each problem, such
as an objective counting a group nothing has, or a gate waiting for an objective that
isn't there. `player --check` and the editor's **Project > Levels > Validate** list the
same problems under "Gameplay code and rules", apart from missing files, circuits and
navigation (the game's `GameInstance::checkScene`, see
[Checks](../../docs/Levels.md#checks)).

To make a new level in the editor: **Project > Levels > New level** (or **Duplicate
level** on training or the factory to start from one), then the **Create > Level** menu
for its spawn, checkpoint and exit, and the components above for its objectives. Set the
exit's destination from the arrow beside it, then **Validate**. See
[Authoring in the editor](../../docs/Levels.md#authoring-in-the-editor).

## How it plays by itself

```sh
ctest --test-dir build -R shipping_demo
```

`shipping_demo_flow` plays the whole game through the player with real input: keys,
mouse movement and a virtual gamepad go in at the device boundary and are read through
the bindings. No step can set the game's success flag; the scenarios win by playing.
See [Flow tests](../../docs/FlowTest.md#real-input). It then packages the game and plays the whole run, and save,
quit and Continue, again from the package. `shipping_demo_authoring` makes, furnishes and
duplicates levels in the editor and checks that broken content is reported under the
right kind.
