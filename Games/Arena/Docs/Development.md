# How the arena shooter was built, and what it exposed

This describes the implementation reviewed on 18 October 2026, including the
first-person motion fix. It is a record of the code in this checkout, not a claim
that every proposed improvement below has been implemented or merged upstream.
For launch commands and controls, see [Arena](../README.md).

## Scope and approach

The result is a local first-person greybox: one player and five allied bots against
six enemy bots. It has an automatic rifle, damage, reloads, respawns, team scores,
a round timer, HUD, menu and generated audio. There are no networked players.

The first version lived inside Sample. It has since moved to `Games/Arena`, with
its own `Arena.swproject`, `game_arena` module, content, bindings, tests and manuals.
Sample retains its original showcase behavior. Arena has no Sample game circuit or
asset-folder dependency. New games use `Templates/Blank`, rather than copying either
example. Shared engine fixes remain under `Source/Runtime`.

I authored the scene, prefab and UI text files directly. Runtime screenshots and the
editor were used to inspect the result. The map was not built through interactive
editor placement, and the bots are not imported character models.

## Source map

Links below point to the current file layout; command paths are relative to the repository root.

| File | Responsibility |
|---|---|
| [ArenaGame.h](../Source/ArenaGame.h) | Match, fighter and weapon data; presentation helper declaration |
| [ArenaGame.cpp](../Source/ArenaGame.cpp) | Registration, validation, movement, combat, respawning, presentation and HUD updates |
| [ArenaModule.cpp](../Source/ArenaModule.cpp) | Selects the arena game mode for this project |
| [Arena CMake](../CMakeLists.txt) | Builds the source files into `game_arena` with `shockwave_game_module(arena)` |
| [arena.swscene](../Scenes/arena.swscene) | Playable map, player, camera, rifle, bots and UI documents |
| [arena-benchmark.swscene](../Scenes/arena-benchmark.swscene) | Copy of the map with automatic aiming/firing and longer match settings |
| [Blue bot prefab](<../Content/Prefabs/Arena bot 0.swprefab>) / [orange bot prefab](<../Content/Prefabs/Arena bot 1.swprefab>) | Reusable bot bodies and team defaults |
| [Input.swinput](../Config/Input.swinput) | Named keyboard/mouse bindings |
| [hud.rml](../Content/UI/Arena/hud.rml), [menu.rml](../Content/UI/Arena/menu.rml), [arena.rcss](../Content/UI/Arena/arena.rcss) | Game UI markup and styling |
| [GenerateArenaAudio.py](../Tools/GenerateArenaAudio.py) | Reproducible original sound generation |
| [ArenaTest.cpp](../Tests/ArenaTest.cpp) / [ArenaCheck.cmake](../Tests/ArenaCheck.cmake) | Gameplay, presentation and CLI validation tests |

## Data and game-mode selection

Three plain ECS components hold authored settings and runtime state:

| Component | Registered, editable settings | Runtime-only state |
|---|---|---|
| `ArenaRules` | Score limit, round duration, autoplay | Team scores, remaining time, finished flag, shot/hit/reload counters |
| `ArenaFighter` | Team, player flag, maximum health, speed, fire interval, damage | Current health, spawn position, cooldowns, protection, kills/deaths |
| `ArenaWeapon` | Magazine size, damage, shot interval, reload duration, sensitivity | Ammunition and cooldown/reload timers |

`registerTypes()` calls `registerComponent` under the `Arena` inspector category.
The arena module registers that function through its `AutoComponent` registration
list. The metadata supplies the existing inspector, scene serialization, generated
scene schema and component-related circuit nodes. No custom editor panel was added.
The shooter logic itself is C++, not a circuit graph.

`ArenaInstance::createGameMode()` always creates `ArenaGame`. The module is dedicated
to this project, so a malformed arena scene cannot silently select Sample behavior.
Sample's module is again independent and does not register the arena components.

`beginPlay()` validates numeric ranges and structural requirements, records spawn
positions, initializes the round, allocates tracer entities and registers menu
callbacks. It requires one rules component, one player with a character controller
and weapon, and a root primary camera named `Arena Camera`. Fighters require root
transforms; bots require colliders. `Arena Rifle` and `Arena Muzzle` are optional
visuals found by name. Other than the camera, these names are not validated links.

## Map, movement and bot construction

The courtyard uses a 34 by 30 unit floor, four boundary walls, four cover blocks,
lane markings and colored spawn stripes. Built-in cube meshes and the white texture
provide almost all geometry; tints, roughness, a directional light and ambient
environment settings provide its appearance. Colliders on the floor, walls and
cover participate in Jolt collision and raycasts.

The player uses the existing Jolt-backed `CharacterController`. WASD becomes a
horizontal desired velocity rotated by the player's yaw. Diagonal input is
normalized; sprint multiplies speed by 1.55. Space requests a jump. The authored
player speed is 5.5 units/second. Mouse deltas control yaw and pitch; pitch is clamped.

Each bot prefab contains a collider, kinematic rigid body and primitive child meshes
for its torso, head, visor, legs and rifle. Blue and orange prefabs contain duplicated
geometry with different colors and team values. Scene copies mainly override spawn
position. Bots move sideways on a sine wave around that position; their `speed`
setting controls the oscillation rate, not a navigation speed in meters per second.

Bot targeting scans living enemies and chooses the nearest. A physics ray must then
hit that enemy before the bot fires. It does not search for an alternative visible
enemy when the nearest is behind cover. Bots have no route planning, avoidance,
tactical movement or skeletal animation. Shockwave already has navigation and
animation systems; this prototype does not exercise them.

## Frame and simulation flow

The project simulates at 60 Hz. Rendering can run much faster.

1. `configureSystems()` adds one exclusive `Arena controls` system. Before physics,
   it updates cooldowns/protection/respawns, bot patrol positions and the player's
   desired velocity. Exclusive access keeps these small-world edits straightforward;
   this sample does not demonstrate parallel combat systems.
2. The session runs navigation and Jolt physics.
3. `fixedUpdate()` advances match/reload timers, resolves player and bot firing,
   applies damage, updates scores and checks the round ending conditions.
4. `frame()` consumes mouse aim and action edges, updates presentation and sends
   values/classes to the HUD. Recoil recovery and reload lowering use frame time.

The engine polls input before fixed ticks but calls the game's `frame()` afterward.
Consequently, the arena's cached aim/fire intent can be from the preceding rendered
frame when a fixed tick fires. Movement reads actions directly in the pre-physics
system. This ordering distinction matters for future input-latency work; the camera
motion fix did not change input-command scheduling.

## Weapon, damage and round rules

The rifle is ordinary scene geometry parented under the camera. It has a cube-based
receiver, barrel, stock, magazine, sight and support hand, plus a small emissive
muzzle mesh. It is not a separate first-person render pass.

Player defaults are 30 rounds, 34 damage, a 0.11 second firing interval and a 1.5 second
reload. Intervals are checked on fixed ticks, so actual cadence is quantized to the
simulation rate. An empty magazine starts a reload while firing; R reloads manually.
Reserve ammunition is unlimited. Recoil moves the visible gun backward and recovers;
there is no aim kick, bullet spread or recoil pattern.

A shot casts a ray from the fighter's eye up to 60 units, ignoring the shooter.
The first obstruction stops the ray. Enemy fighters lose health unless protected;
friendly fighters take no damage but still block the shot. There is one body hit
volume, no headshot multiplier, penetration or projectile flight. The visible rifle
muzzle is not a second obstruction test, so a future weapon system must address
camera-versus-muzzle clearance near cover.

A ring of 24 pre-created cube entities draws short-lived tracers. Slots are reused
instead of creating entities per shot. This avoids that particular allocation path;
it does not mean the entire game loop is allocation-free. HUD strings, library calls
and other engine systems still do work each frame.

At zero health a fighter's collision and visible body parts are disabled, the
opposing score increases, and a three-second respawn timer begins. Respawning restores
health, position and one second of protection. Player ammunition is refilled. Restart
resets the round. Teleports reset transform history so respawns do not interpolate
across the map. The normal scene ends at 20 eliminations or 180 seconds.

## UI and audio

RmlUi documents display score, time, health, ammunition, kills/deaths, status,
elimination messages and a crosshair. Damage and hits toggle CSS classes. Game code
publishes named `arena_*` values through `UIWorld`; Resume and Restart call registered
callbacks. The palette and square-bordered layout live in `arena.rcss`. These are
game UI documents; the editor continues to use its existing shared controls/theme.

P sets a game-local pause flag and releases mouse capture. It freezes arena logic,
but does not pause the whole `PlaySession` or Jolt. Physics and other session systems
can continue, for example a player already in the air can keep falling.

The Python generator uses only the standard library, a seeded random generator,
sine waves and envelopes. It writes mono 16-bit, 24 kHz WAVs: a 0.14 second shot,
0.35 second reload and 0.12 second elimination tone. They are original CC0 assets.
Player sounds use `playClip`; bot shots use spatial `playAt`. There are no downloaded
gun recordings. The tone called `arena-hit.wav` currently plays on a player kill,
while ordinary hit feedback is visual.

## Bugs exposed and fixes already made

### Cached prefabs outliving game-module code — fixed in this checkout

Arena bot prefabs contain game-defined components. The global prefab cache could
retain their component pools after the game library unloaded. Destroying those
pools at process exit then called code from an unmapped library, causing a crash.
The debugger identified this path during project validation.

[GameModuleLibrary.cpp](../../../Source/Runtime/GameFramework/Private/GameModuleLibrary.cpp)
now clears the prefab cache before the plugin unloads.
[HotReloadTest.cpp](../../../Tests/HotReloadTest.cpp) tests a cached prefab containing a
module-owned component and checks that it is released on unload. This fixes the
observed cache path; it is not proof that arbitrary external references to plugin
objects are safe. Future plugin services need explicit ownership/unload contracts.

### Gun snapping relative to the camera — fixed for the arena

[cameraMatrices](../../../Source/Runtime/Engine/Private/Systems.cpp) reads the current camera
transform, whereas `renderWorld` interpolates mesh transforms and their parents.
The original arena moved the camera every frame but left its previous-tick transform
intact. The rifle therefore inherited an older camera pose than the view matrix,
causing visible slipping and snapping as the interpolation fraction reset.

[Engine](../../../Source/Runtime/Engine/Public/Engine/Engine.h) now exposes
`interpolationAlpha()` during frame/render callbacks. Arena `updateView()` interpolates
the player's feet, applies current mouse aim, and synchronizes the camera and rifle's
previous transforms with their presentation poses. This prevents a second blend.
Recoil recovery and reload lowering now run per frame. The regression test simulates
60 Hz movement at 1,300 render updates/second and verifies camera-space rifle alignment.

The shared engine camera API still has the current-versus-interpolated distinction.
Other games need to handle it; a general presentation policy remains follow-up work.

## Engine improvements justified by this exercise

These are proposed changes, not additional fixes performed while writing this document.
The source links identify observed behavior; the proposed designs are recommendations.

| Priority | Finding and evidence | Recommended fix and completion test |
|---|---|---|
| High | **Presentation state is too implicit.** Arena must directly rewrite `PreviousTransform`, despite that type being described as engine-managed. Cameras and parented meshes have different sampling behavior in [Systems.cpp](../../../Source/Runtime/Engine/Private/Systems.cpp). | Provide a shared presentation API for fixed-tick objects, frame-driven cameras, attachments and teleports. Test camera/weapon alignment and smooth movement at multiple tick/render rates, including pause and respawn. |
| High | **Validation metadata is not one complete contract.** [SceneFile.cpp](../../../Source/Runtime/Engine/Private/SceneFile.cpp) applies its generic range-warning loop to floats and vectors, not integer fields such as magazine size. Arena repeats limits in `beginPlay()`. [PlayerTools.cpp](../../../Source/Programs/Player/Private/PlayerTools.cpp) starts gameplay to discover those structural errors. | Validate all numeric metadata consistently; add pure component/scene validation hooks with entity/field diagnostics shared by inspector and CLI. Test invalid magazine, team and score values without starting audio, physics or gameplay callbacks. Keep a separate startup smoke test. |
| High | **Player asset loading is eager across the project.** [Player.cpp](../../../Source/Programs/Player/Private/Player.cpp) opens the whole Content folder before scene loading; [AssetFolder.cpp](../../../Source/Runtime/Assets/Private/AssetFolder.cpp) scans and imports supported assets. Before the project split, arena startup imported unrelated Sample models/textures. The split removes that local dependency but does not change the eager-loading policy within any project. | Separate asset discovery from residency. Load scene dependencies and declared code-only assets through bounded async loading, with memory budgets and deliberate warm-up. Verify a large unused asset does not consume player residency, and first use does not hitch. Code-only audio references must remain discoverable. |
| High | **Benchmark totals can be misleading.** [FrameStats](../../../Source/Runtime/Core/Private/FramePacing.cpp) uses a rolling buffer (default 2,048 samples), and [Player::reportBenchmark](../../../Source/Programs/Player/Private/Player.cpp) summarizes it while printing the requested run duration. | Keep full-run benchmark statistics separately from the rolling overlay. Report total measured frames, actual elapsed wall time, sampled interval and presentation/build settings. Test a run exceeding 2,048 frames and ensure early hitches remain represented in its lows. |
| Medium | **Input sampling and simulation phases need a clearer game-facing contract.** [Engine::run](../../../Source/Runtime/Engine/Private/Engine.cpp) samples input, runs fixed ticks, then calls `onFrame`; arena prepares some firing intent in that last phase. This is an integration hazard, not evidence that all engine input is delayed. | Add or document a pre-simulation input-command phase: accumulate mouse deltas, consume action edges once, and feed commands to fixed simulation separately from latest-frame camera aim. Test zero-tick and multi-tick rendered frames for lost or duplicated actions. |
| Medium | **Game-menu pause is awkward.** [PlaySession](../../../Source/Runtime/GameFramework/Private/PlaySession.cpp) has real pause, but `GameContext` does not expose session pause control and normal game `frame()` hooks stop while paused. Arena uses a local flag to keep its menu callbacks working. | Expose a deliberate pause request and a UI/unscaled update path that works while simulation is paused. Verify timers, physics and audio obey policy, while keyboard/mouse menu resume still works. |

For the low-memory shooter goal, asset residency is especially important. For making
the next game easier to author correctly, presentation and validation are the first
shared APIs to improve. Benchmark accounting should be corrected before treating the
reported lows as whole-run evidence.

## Shortcuts in this game, rather than missing engine capability

| Shortcut | Consequence and next step |
|---|---|
| Most behavior is in one `ArenaGame` class with one exclusive control system. | Good for a small test, but movement, weapons, damage, spawning, bots and HUD should become separate systems as complexity grows. The ECS already supports scheduling; I did not use that capability extensively here. |
| Camera/rifle/muzzle are found by display name; sounds and dimensions are hardcoded. | Renaming can break behavior or hide visuals. Use reflected entity/asset references and authored settings with validation. The engine already supports entity fields. |
| Normal and benchmark scenes duplicate the map; two bot prefabs duplicate geometry. | Changes can diverge. Share authored map content or introduce a small, validated scenario override rather than manually maintaining two full scenes. |
| Bot movement and aiming are simplistic. | Use existing navigation and animation systems, add sight-aware target selection and movement states, and test obstacles. Bot aim is exact and the difficulty is not balanced. |
| Gun rendering uses world geometry. | It can intersect walls; it has no independent viewmodel FOV/depth policy. Add a viewmodel approach and muzzle-clearance rules if the game needs them, with measured GPU cost. |
| Respawn visibility touches the root and immediate mesh children only. | Deeper hierarchies or imported character renderers need explicit handling. Do not assume this helper hides an arbitrary actor subtree. |
| Runtime counters/state are not registered for persistence, and there are no arena save hooks. | The engine has save games, but this match is not save/load complete. Implement `beforeSave`/`afterLoad` or explicit persistent state and test mid-reload/mid-respawn saves. |
| The menu text says “First to 20” literally. | The benchmark's 200-point limit is not reflected in that menu text. Bind text to rules. The earlier inherited Sample pulse binding/circuit was removed by the project split. |
| Tracer entities are pooled, but reused transforms do not explicitly reset their interpolation history. | A reused tracer can blend from its old segment. Apply the teleport/presentation policy to transient effects and test repeated reuse. This was found in the code review, not confirmed by a dedicated visual test. |
| Game-specific audio paths are passed as strings. | Existing asset importing makes them work, but the arena does not explicitly validate all these code-only references at startup. Declare required assets and fail usefully if one is missing. |

## Verification and the limits of the results

The game/player/editor built, the project checker reported zero problems across 13
scenes, and the full CTest run immediately after the motion fix passed all 26 tests. This is historical verification of that stage, before project scaffolding tests were added.
The arena tests cover damage, cover, friendly fire, protection, reloads, repeated
respawns, score limits, Play/Stop restoration, component round-tripping, invalid
startup and first-person presentation. The CLI test covers the valid map, an invalid
magazine and a missing player controller. These do not cover every gameplay state.

Player HUD/menu screenshots and an editor view were inspected. An automated local
match exercised actual shots, hits, kills and reloads on an RTX 3080. The player also
reported a large FPS increase after disabling VSync. That points to presentation
limiting that run; it does not diagnose every possible 110 FPS case.

Benchmark runs use a hidden window and disable VSync by default. Their settings differ
from normal visible gameplay. The local CMake build type was unset rather than Release.
At approximately 1,300 FPS, the 2,048-entry statistics buffer covers only about 1.6
seconds, even when the requested run lasts five or ten seconds. Earlier high-FPS
results therefore demonstrate a light workload, not complete-run frame pacing or
low-end readiness. No claim of a performance improvement should be made from the
small before/after differences during the motion fix.

There has been no Jetson validation, network synchronization test, twelve-human-player
test, long-duration memory soak, or production-art performance test for this arena.
The general packaging test passed; a standalone packaged arena product was not
specifically built and acceptance-tested during this work.

To reproduce the relevant checks from the repository root:

```sh
cmake --build build --target player editor game_arena arena_test -j4
ctest --test-dir build -R 'arena|hot_reload' --output-on-failure
./build/bin/player --project Games/Arena/Arena.swproject --check
./build/bin/player --scene Games/Arena/Scenes/arena.swscene --vsync off --fps 0
./build/bin/player --scene Games/Arena/Scenes/arena-benchmark.swscene --benchmark 10 --json arena.json
```

Build all targets before running the full CTest suite on a fresh checkout. Repeat
performance measurements with a controlled Release build and representative content,
and preserve the build, GPU, resolution, VSync and sampling settings with each result.
