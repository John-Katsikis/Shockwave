# Final acceptance

Section 25 of the shipping plan (docs/ShippingImplementation.md) defines a complete,
playable reference game as fifteen steps, run by machines and by people. This page says
which steps the tests run on Reactor Run (Games/ShippingDemo), how to run them yourself, and
the checklist for the steps only a person, or real hardware, can do. Automation doesn't
replace looking at the game: a test can click a menu, it can't tell you the menu reads well.

## What the machines run

| Step | What it asks | Run by | Notes |
| --- | --- | --- | --- |
| 1 | a Release build and package from documented commands; hash, commit and contract recorded | `tutorial` (nightly), `package_release`, `installed_game_flow` | the install record keeps the package's SHA-256, build and content IDs |
| 2 | moved, run from a third folder, read-only, empty user folder | `installed_game_flow` | `--verify-install`: spaces and non-ASCII letters, a clean environment, a fresh home |
| 3 | started through the player-facing program; a usable menu | `installed_game_flow` | starts the package's entry point; every scenario asserts the menu first |
| 4 | graphics, audio and a binding changed, applied, restarted, kept; display revert | `shipping_demo_flow` (settings), `installed_game_flow` (volume, the `player_settings` proof) | the display's keep-or-revert prompt is in settings.json |
| 5 | a new campaign: loading, training, HUD, controls, an objective | `shipping_demo_flow` (full-run, gamepad-run) | |
| 6 | training's exit to the factory; nothing of training left | `shipping_demo_flow` (full-run), `acceptance_round_trips` | entities, bodies, assets and leases come back to the same counts |
| 7 | progress carried; save, quit, relaunch, Continue into the factory | `installed_game_flow` (resume-quit, resume-continue: the `saves` proof) | two processes, one user folder |
| 8 | die, restart the checkpoint, restart the level; rewards not duplicated, the destroyed stay destroyed | `save_checkpoints`, `save_destroyed`, `game_flow` | the engine's semantics; see the checklist for playing it |
| 9 | the factory finished, ending, credits, menu, a new game | `shipping_demo_flow` (full-run) | no restart, no console |
| 10 | menu, A, B, menu 100 times; memory plateaus; nothing accumulates | `acceptance_round_trips` | `player --flow-test round-trip.json --laps 100` ([FlowTest.md](FlowTest.md#laps)) |
| 11 | a missing and a corrupt level, a full disk, a cancelled change; the last save intact | `acceptance_failures` | failures.json on a copy with `factory` deleted and `ending` corrupted |
| 12 | updated to a build with a schema and content change; old save continued and kept; downgrade refused safely | `installed_game_flow` (`--update-from`) | |
| 13 | keyboard and mouse, and a controller; minimum resolution, high DPI; each platform and tier | `shipping_demo_flow` (gamepad-run, controller) | the rest needs real hardware: see the checklist |
| 14 | online: host, join, match, rotation, late join, rejoin, menu | Arena's `network_*` tests on the loopback and faults | Reactor Run is offline; a real-service run is for a person |
| 15 | a controlled crash; the report found and symbolised; recovery keeps player data | `failure_recovery`, `diagnostics_cli` | on the real package: see the checklist |

The error codes of section 24 each have a test too: [ErrorCatalog.md](ErrorCatalog.md).

## Running them

From the repository, after a Release build with Reactor Run in `SHOCKWAVE_GAMES` (the
default), with no window:

```sh
SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy ctest --test-dir build --output-on-failure \
  -R "^(installed_game_flow|acceptance_round_trips|acceptance_failures|error_catalog|shipping_demo_flow|save_checkpoints|save_destroyed|failure_recovery|level_travel|asset_residency)$"
```

The round trips on their own, with a report of every lap's watermarks:

```sh
build/bin/player --project Games/ShippingDemo --renderer null \
  --flow-test Games/ShippingDemo/Tests/flow/round-trip.json --laps 100 --json laps.json
```

With a real GPU, drop `--renderer null` (and the `SDL_VIDEO_DRIVER` setting) and the same
scenario runs through Vulkan; `--lap-tolerance <MiB>` changes the memory tolerance (32 MiB).

The release's own evidence, on your machine: package a release, verify it installed with
the three scenarios that prove the logic gates, then read the gates. On a real GPU this
record also qualifies the platform (with `--update-from` an older package, as
[Release.md](Release.md#platforms) explains).

```sh
build/bin/player --project Games/ShippingDemo --package --release --output out
build/bin/player --verify-install out/ReactorRun-<version>-linux-x86_64 --project Games/ShippingDemo \
  --install-record "My PC" \
  --flow-test Games/ShippingDemo/Tests/flow/volume.json \
  --flow-test Games/ShippingDemo/Tests/flow/resume-quit.json \
  --flow-test Games/ShippingDemo/Tests/flow/resume-continue.json
build/bin/player --project Games/ShippingDemo --release-info
```

## The checklist for people

Play the installed package, not the editor, on each platform and hardware tier the
contract names. Write down the machine, the package's file name and anything that fails.
A release rehearsal record ([ReleaseRehearsal.md](ReleaseRehearsal.md)) is where a new
tester's run of this goes.

1. **The menu (3).** It appears without a terminal, reads well at the window's size, and
   every button does what it says with the mouse, the keyboard and a controller.
2. **Settings (4).** Change the resolution and window mode, a volume and one binding.
   Apply, quit, start again: they're kept. Change the display and don't confirm: it goes
   back by itself.
3. **Training (5).** The loading screen shows progress. The HUD is legible, the controls
   respond, physics objects behave, sounds play from where they should, and the objective
   text matches what's happening.
4. **Into the factory (6).** It looks and sounds different; nothing from training is still
   playing or drawn.
5. **Death and restarts (8).** Die in the factory and restart from the checkpoint: what you
   did before the checkpoint stays done. Restart the level: it's as it was when you
   entered it. Enemies you destroyed don't come back, and rewards aren't given twice.
6. **The ending (9).** Finish the factory; the results, the ending and the credits read
   well; back at the menu, a new game starts cleanly.
7. **Hardware (13).** Play a level at the smallest window the game allows and on a high-DPI
   display: nothing is cut off or blurry. Play once with a controller only.
8. **A crash (15).** Nothing in the game crashes on request (only the test program
   `crash_program` does, [Diagnostics.md](Diagnostics.md)), so end the installed game the
   hard way while it plays: `kill -SEGV <pid>` on Linux or macOS. Start it again: the
   recovery page offers safe graphics, the crash report is where it says, `player
   --symbolize` names the functions from the symbols kept beside the package, and the saves
   are still there.
9. **Online (14), for an online release only.** Through the real service: host, join, play
   a match, let the map rotate, join late, drop out and rejoin, return to the menu. Note
   any player count, fault or performance combination the release doesn't support.
