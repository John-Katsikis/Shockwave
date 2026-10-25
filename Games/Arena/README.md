# Arena shooter test

For the implementation walkthrough, engine issues and prototype tradeoffs, see
[How the arena shooter was built](Docs/Development.md).

A standalone local 6v6 prototype in `Games/Arena`: one human and five blue bots
against six orange bots. The compact courtyard has cover and team spawn areas. The
SW-01 automatic rifle has hitscan collision, a 30-round magazine, unlimited reserve
ammunition, reloads, muzzle flash, pooled tracers, recoil feedback and generated audio.
Health, ammunition, score, timer, eliminations and hit feedback appear in the HUD.

## Play

From the repository root:

```sh
cmake --build build --target player game_arena -j4
./build/bin/player --scene Games/Arena/Scenes/arena.swscene
```

Click the window to capture the mouse. WASD moves, mouse aims, left click fires, Shift
sprints, Space jumps, R reloads, P opens/closes the menu, and F2 restarts the match.
Escape quits the standalone player. The menu has Resume and Restart buttons.

Blue is your team; there is no friendly fire. Eliminated fighters respawn after three
seconds with one second of protection. First to 20 eliminations wins, or the higher
score at three minutes; equal scores draw. F2 starts a fresh round.

To edit or play inside the editor:

```sh
./build/bin/editor Games/Arena/Scenes/arena.swscene --play
```

Stop restores the authored scene. Select **Match** to edit `ArenaRules`, **Arena Player**
for `ArenaFighter` and `ArenaWeapon`, or a bot for its team, health and firing settings.
Those fields use the registered component inspector and are documented in
[SceneSchema](Docs/SceneSchema.md). Bots are reusable prefabs in `Content/Prefabs/`.
Edit `Content/UI/Arena/hud.rml`, `menu.rml` and `arena.rcss` for the game UI; colors live
in the stylesheet. Bindings live in `Config/Input.swinput` and Window > Input.

## Play online

Arena's multiplayer scene (`Scenes/arena-mp.swscene`, six a side) is played over Steam
with a player built with `-DSHOCKWAVE_STEAMWORKS=ON` (see
[Networking](../../docs/Networking.md#hosting-and-joining)). Turn it on in
`Config/Network.swnetwork` (`enabled 1` and your `steam_app_id`; 480 for testing, with a
`steam_appid.txt` holding 480 in the folder you start from), then press **P** for the
menu: **Host friends match**, **Find / join** a friend's match, **Invite**, and **Leave
match / play offline**. The same from the command line:

```sh
./build/bin/player --project Games/Arena/Arena.swproject --host
./build/bin/player --project Games/Arena/Arena.swproject --join-lobby <lobby ID>
```

Without Steam the menu's online section says why it's unavailable, and the game plays
offline as before.

## Verify and measure

```sh
./build/bin/player --project Games/Arena/Arena.swproject --check
ctest --test-dir build -R 'arena|hot_reload' --output-on-failure
./build/bin/player --scene Games/Arena/Scenes/arena.swscene --screenshot arena.png
./build/bin/player --scene Games/Arena/Scenes/arena-benchmark.swscene --benchmark 20 --json arena.json
```

The benchmark scene enables `ArenaRules.auto_play`, aiming and firing without input.
It uses a longer round and higher score limit. The shutdown log reports scores,
player shots, hits and reloads, so a benchmark can confirm combat actually happened.
Use the same GPU, build type, resolution and benchmark duration when comparing runs.

Tests exercise Jolt raycast damage, cover, friendly-fire rejection, spawn protection,
reloads, respawns, score limits, Play/Stop, component serialization and invalid-scene
rejection. `--check` validates field ranges and rejects an arena missing its player,
character controller, weapon, camera or rules. The shooter also exposed and now tests
safe unloading of game-defined components held in the prefab cache.

The three small original sound effects are CC0. Regenerate them with
`python3 Games/Arena/Tools/GenerateArenaAudio.py`; no downloaded assets are required.

## Prototype boundaries

This is a functional greybox for testing the engine and its multiplayer. Bots
strafe on simple patrols and shoot visible enemies; they do not navigate the map or
use tactical AI. Characters and the weapon are primitive meshes, with no skeletal
animation, ADS, weapon switching or projectile ballistics. The gun is ordinary scene
geometry and can clip into walls. Menu pause freezes the match logic; it is not a
general-purpose physics pause. Match save/load is not implemented.

The benchmark exercises this small workload, not production art, twelve networked
clients or low-end hardware compatibility. Use it as a starting point for heavier
assets and real hardware tests.

Press **P → Graphics Settings** to select quality, render scale, shadows, LOD and frame pacing. Apply saves per-player preferences; see [Graphics settings](../../docs/Graphics.md).
