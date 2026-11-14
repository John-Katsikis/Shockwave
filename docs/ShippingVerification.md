# Shipping verification

This is the evidence for each batch of [the shipping plan](ShippingImplementation.md). Each
section says:

- what was built;
- what was measured, on which machine;
- what is still unverified.

`player --release-info` works out the release gates from the same evidence ([Release.md](Release.md)).

| Batch | Status | Evidence |
| --- | --- | --- |
| S00 release contract | done | [below](#s00-release-contract) |
| S01 per-user paths | done | [below](#s01-per-user-paths) |
| S02 level catalog | done | [below](#s02-level-catalog) |
| S03 level lifetime | done | [below](#s03-level-lifetime) |
| S04 assets and loading | done: dependencies, leases, budget, load report; loading on demand, deferred GPU frees | [part 1](#s04-asset-dependencies-and-residency-part-1), [part 2](#s04-loading-on-demand-part-2) |
| S05 offline flow and travel | done: travel tickets, staged travel, spawns, exits, circuit nodes; the application shell's screens, `--flow-test` | [part 1](#s05-travel-tickets-part-1), [part 2](#s05-application-shell-and-flow-tests-part-2) |
| S06 campaign saves | done: save format 2, campaign travel, migration, durable writes, Continue; checkpoints, finishing a level, things that stay gone | [part 1](#s06-campaign-saves-part-1), [part 2](#s06-checkpoints-and-lasting-progress-part-2) |
| S07 finishable reference game | done: Reactor Run, real-input flow steps, the whole game by keyboard and mouse and by gamepad, save/quit/continue, from the build and from a package; editor level authoring, failures by kind, game content checks in `--check` | [part 1](#s07-reactor-run-part-1), [part 2](#s07-reactor-run-part-2) |
| S08-S15 | not started | none |

## S00: release contract

### What was built

- `Config/Release.swrelease` is the release contract, with a parser, a writer and a
  validator. All three run from one descriptor table, in `Projects/ReleaseSettings.h`.
- `player --release-schema` generates [ReleaseSchema.md](ReleaseSchema.md).
- `player --release-info [--json]` reports the contract, the build and every gate.
- `player --check` cross-validates the contract against four sets of settings: the
  project's tick rate, the package version, the network settings and the graphics memory
  budget.
- Diagnostics are machine-readable, with a stable code, severity, file, line, object and
  message (`Projects/Diagnostic.h`). Plain `--check` output keeps its old `file:line:
  message` shape.
- In the editor, **Project > Release readiness** edits the contract and shows the gates.
  **Help > Shipping implementation guide** opens the plan.

Projects without a contract are unaffected. The `release_cli` test runs Arena as it is,
without one, and `--check` still passes with no mention of a contract.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `release` | unit | see below |
| `release_cli` | integration | see below |

`release` covers the contract on its own:

- parsing and writing, which round-trip exactly, including CRLF line endings;
- every range, plus NaN, infinity, overflow, signs, fractions and exponents;
- duplicate keys, and the same platform or mode listed twice;
- missing keys, and an unsupported version named in the error;
- the rules between keys and with the project's other settings;
- the gates, in batch order;
- a project without a contract;
- the JSON report and JSON escaping, and that the schema is stable.

`release_cli` runs on a copy of Arena:

- legacy Arena, with no contract, checks clean;
- a good offline contract checks clean, and every gate in the JSON is well formed;
- bad values fail `--check` and `--release-info` with exit 1 and file:line;
- a changed package version or tick rate fails;
- a graphics budget above `gpu_budget_mib` fails;
- online without multiplayer fails, and so does a mismatched `max_players`;
- AppID 480 gives a warning and a failed `steam_app_id` gate;
- the checked-in schema matches what the build generates;
- `--release-info` refuses to combine with other modes (exit 2).

The editor window was checked by eye under Xvfb on a copy of Arena with a contract. It
showed the property grid, the warning for the missing acceptance scenario, Save and Revert,
and the gate table.

### Baseline: the machine and build

These baselines are recorded for later batches to compare against. They are not release
measurements: this machine has no GPU, and `llvmpipe` renders on the CPU.

| | |
| --- | --- |
| CPU | Intel Xeon Processor @ 2.10GHz, 4 logical CPUs |
| Memory | 16 GB |
| GPU | llvmpipe (LLVM 20.1.2, 256 bits): software Vulkan 1.4.318, Mesa 25.2.8 |
| OS | Ubuntu 24.04.5 LTS, kernel 6.18.44 |
| Compiler | GCC 13.3.0, CMake 3.28.3, Ninja |
| Build | `CMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`), `SHOCKWAVE_MODULAR=ON`, Steam Audio on, Steamworks off |

The build type was checked in `build/CMakeCache.txt`, and `--release-info` reports it as
`build Release`. Two CI jobs differ:

- the Linux jobs in `.github/workflows/build.yml` configure with
  `-DCMAKE_BUILD_TYPE=Release`;
- the Windows job builds `--config Release` with a multi-config generator.

### Baseline: tests

`ctest --test-dir build --show-only=json-v1` lists 82 tests in this build, Steamworks off:

| Label | Tests |
| --- | --- |
| `gpu` | 14 |
| `timing` | 2 (one of them, `graphics_clusters_timing`, is also `gpu`) |
| `unit` | 1 |
| `integration` | 1 |
| none | 65 |

S00 introduced the `unit` and `integration` labels, on its two tests. Labelling the older
tests is S14's job. The full suite was run on this machine; the result is in the S00 pull
request.

### Baseline: benchmark

```sh
SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy build/bin/player --project Games/Arena/Arena.swproject \
    --size 1280x720 --benchmark 5 --json out.json
```

The run used Arena's `Scenes/arena.swscene` at 1280x720 with the project's default graphics
settings (benchmarks ignore user preferences). Presentation was uncapped mailbox, with no
frame limit, for 5 seconds per run:

| Cache | Frames | Average | 1% low | Worst | CPU | GPU | Draws (culled) | GPU memory |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| cold (pipeline cache deleted) | 50 | 100.6 ms (9.9 fps) | 8.2 fps | 122.0 ms | 100.8 ms | 97.8 ms | 94 (19) | 157.5 MiB |
| warm | 51 | 99.4 ms (10.1 fps) | 8.2 fps | 122.0 ms | 99.7 ms | 96.3 ms | 94 (19) | 157.5 MiB |

The two passes that took most of the frame were bloom (15 ms) and tonemapping (13 ms). A CPU
renderer spends its time filling pixels, so these numbers say nothing about a real GPU.
They are a regression reference for this machine only.

### Baseline: package

```sh
SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy build/bin/player --package \
    --project Games/Sample/Sample.swproject --no-build --no-archive --output <folder>
```

This made `ShockwaveSample-1.0.0-linux-x86_64`:

- 157 files were copied (139 MB before stripping), giving a 92 MB folder;
- 67 of the files are the game's own;
- 0 of 11 textures were compressed;
- it took 7.8 s with an existing build, including the package's own verification run.

### Still unverified

The S00 machine gates:

| Gate | Status |
| --- | --- |
| `release_contract` | passes for a valid contract |
| `project_settings` | passes when the contract agrees with the project |
| `release_build` | passes on this Release build |
| `steam_app_id` | passes for online Steam games with their own AppID; not applicable offline |

Every other gate is `unverified` until its batch brings the evidence:

- `user_paths` (S01, measured since S01);
- `level_catalog` (S02, measured since S02);
- `level_travel` and `acceptance_scenario` (S05);
- `saves` (S06);
- `player_settings` (S08);
- `cooked_package` (S09);
- `performance` (S10);
- `online_lifecycle` (S11);
- `crash_recovery` (S12);
- `platform:<name>` (S13).

Three things the plan allows are not in S00:

- No repository game has a contract yet. The reference game (S07) gets one. The values for
  Arena (application ID, platforms, modes and player count) are the developer's choice, so
  they are left to them.
- Device-tier overrides are not in format 1. They can be added as keyed records once there's
  measured hardware to key them by (S10).
- `--check` gained no JSON output of its own. Machine-readable diagnostics are in
  `--release-info --json`. The older check rules still produce plain text.

## S01: per-user paths

### What was built

- `Platform/UserPaths.h` works out where a game writes ([UserPaths.md](UserPaths.md)). It
  has three sources: installed (the OS per-user folder plus the application ID),
  `--user-dir`, and the project's `Saved/` when run from source. Each profile gets its own
  config and saves. Nothing is created until it's written.
- The player now sends saves, graphics settings, window layout and the pipeline cache
  there. Before, an installed game wrote saves into its own `Game/Saved/` and the rest into
  SDL's preferences folder.
- New player options:
  - `--user-paths [--json]` shows the folders;
  - `--profile` picks a player profile;
  - `--user-dir` puts everything somewhere else;
  - `--import-legacy-saves [--preview]` copies older saves in. It checks each one, copies
    rather than moves, and never writes over a slot.
- `projectApplicationId` names the folders by the contract's `application_id`, or
  `shockwave.<module>` without a contract, never by the display name.
- The `user_paths` release gate is measured now. It passes when the installed folders
  resolve for the contract's ID outside the game, and fails with the reason otherwise.
- In the editor, **Project > Release readiness** has a **Player data** section. It shows
  both locations, has an Open button for each, and has **Import development saves**.
- Every test that runs a packaged game passes `--user-dir`: `package_cli`,
  `graphics_package_render` and `package_steam`.

Run from source, nothing changes: saves stay in `<project>/Saved/SaveGames`, and
`save_games_cli` passes unchanged.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `user_paths` | unit | see below |
| `user_paths_cli` | integration | see below |
| `release` | unit | adds the `user_paths` gate (passed, wrong ID, no folder) and the application ID rules |
| `release_cli` | integration | the gate passes for a good contract, and `--user-paths` uses its ID |

`user_paths` uses an injected environment, so it never reads the real `HOME`. It checks:

- the OS base folder, with `XDG_DATA_HOME` used when absolute and ignored when relative;
- missing and relative bases fail clearly;
- every folder in the installed and development layouts;
- a second profile keeps separate config and saves;
- profile and application ID rules;
- `--user-dir` inside the install is refused, however it's spelled, but a sibling folder is
  allowed;
- `ensure()` creates only the folder asked for, works with spaces and accented names, and
  reports a file sitting where a folder should be.

`user_paths_cli` packages Sample into `Install dir é`, then runs it from an unrelated
folder with a test `HOME` (`home ü`). It checks:

- it resolves as `installed` under `HOME/.local/share/shockwave.sample` and creates nothing;
- without `HOME` it exits 1 with the reason and writes nowhere;
- a save lands in `profiles/default/Saves`, and the package's file hashes don't change;
- profile `p2` sees no saves, and a bad profile is exit 2;
- the package, once moved, still lists the save;
- `--user-dir` inside the package is refused;
- import with `--preview` copies nothing;
- import copies new slots, keeps existing ones, skips a broken file, never changes the
  original, and a second run copies 0;
- a renamed copy of the project keeps the ID `shockwave.sample`.

### Still unverified

- **Permission errors.** This machine runs tests as root, so a read-only folder can't be
  simulated. The unit test covers the error path with a file in the folder's place.
- **Windows and macOS.** The `%APPDATA%` and `Application Support` branches compile in CI
  on Windows, but no Windows install was run here. Clean-machine runs are S13.
- **Profiles from accounts.** Profiles are chosen with `--profile`. Tying a profile to a
  Steam account comes with the online work.
- **Logs and crash reports.** The folders are resolved, but nothing writes there yet. Log
  files and crash reports come with S12.
- **Old saves.** Saves from before S01 are not moved automatically, by design. The player
  imports them with `--import-legacy-saves`.

## S02: level catalog

### What was built

- `Projects/LevelCatalog.h` reads, writes and checks the level catalog
  ([Levels.md](Levels.md)): levels with a scene, a kind, a default spawn, a next level and
  preloads, plus the entry. The project's optional `levels` key names it. Without it, the
  project has one implicit level, `startup`, and nothing changes.
- The checks are pure data, so Projects still depends on no world. The caller loads each
  level's scene and hands over what it found (`LevelSceneFacts`).
- Four components in the new **Levels** category: `LevelSpawn`, `LevelExit`, `Checkpoint`
  and `PersistentIdentity`. Adding a PersistentIdentity in the editor gives it a new UUID.
- `player --check` checks the catalog and follows every link between levels. The new
  `--levels [--json]` lists them, `--level <id>` starts in one, and `--level-schema`
  writes the generated [LevelSchema.md](LevelSchema.md). Every problem has a stable code.
- The `level_catalog` release gate is measured: failed without a catalog or with an error,
  unverified when only the files were checked, passed when every scene loads and every exit
  and spawn resolves. `--release-info` now runs with the game module so it can load scenes.
- In the editor, **Project > Levels** creates and edits the catalog, with undo, validation,
  atomic saves, Open scene and Play level.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `level_catalog` | unit | see below |
| `level_check_cli` | integration | see below |
| `release_cli` | integration | the gate fails without a catalog, passes for a good one-level Arena catalog, and fails for a bad default spawn |

`level_catalog` covers:

- parsing every key, a write followed by a read giving the same catalog, and the atomic
  write;
- each format error with its code and line: header, version, unknown and repeated keys,
  structure, missing keys, bad IDs and kinds, duplicate levels, unknown entry and next;
- paths: missing files, `..` and absolute paths, a symlink out of the project, spelling
  that differs from the disk only by case;
- links: unknown exit destinations and spawns, missing default and checkpoint spawns,
  duplicate spawns, checkpoints and persistent IDs, bad UUIDs, cameras by level kind, and
  unreachable levels as warnings;
- the implicit level, the `levels` project key, and the gate's inputs with and without
  the links checked.

`level_check_cli` copies `Tests/fixtures/levels` (a menu and two gameplay levels, menu to
a to b and back) and checks:

- the good fixture passes `--check`, and `--levels` lists it as text and as JSON;
- each of 18 mistakes, made one at a time, fails `--check`, and `--levels --json` names
  its code;
- an unreachable level warns and still exits 0;
- `--level b` plays, `--level zz` exits 1 and lists the levels, and `--level` with
  `--scene` exits 2;
- `--level-schema` matches the checked-in file;
- Sample, which has no catalog, lists the implicit `startup` level.

### Still unverified

- **Travel.** Nothing moves between levels at runtime yet. Exits, spawns and checkpoints
  are checked but not used; level travel is S03 to S05.
- **Renaming across files.** Rename in the Levels panel changes the catalog only. Exits in
  scenes that use the old ID are listed by Validate but not rewritten. The plan's
  previewable rename across files is still to do.
- **Other ways in.** A level reached only from code or a menu is reported as unreachable,
  as a warning. There's no way yet to mark it as an intended second entry.
- **The editor's gate.** The Release readiness window checks the catalog's files but not
  the links, so it shows the gate as unverified until `player --check` or `--release-info`
  measures it.
- **Prefab identities.** Combining a prefab copy's identity with its entities' own, and
  reporting a copy without one, is written but no test scene has a prefab with a
  PersistentIdentity yet. The unit test covers how such a problem is reported.
- **Arena.** The reference game has no catalog yet, so its gate fails. Arena's levels are a
  choice for the reference game work (S07).

## S03: level lifetime

### What was built

- **Session policies.** `PlaySession` has `SessionWorldPolicy` and `SessionSettingsPolicy`
  ([LevelLifetime.md](LevelLifetime.md)). The defaults keep the old snapshot-and-restore
  behaviour, so existing tools, tests and games are unchanged.
- **`GameFramework/LevelHost.h`** owns the one active runtime level and moves between
  levels. Game code asks with `game.requestLevel(id, spawn)`, and the move happens after
  the frame's callbacks.
  - The next level is prepared in a separate world, so a failed load keeps the current
    level.
  - Every request ends exactly once: succeeded, failed or cancelled.
  - Requests from an old level are refused.
  - More than 8 redirects before a level plays fail as a cycle.
  - Epoch overflow fails explicitly.
- **Lifecycle context.** `game.startReason` (`Fresh`, `RestoreSave`, `OnlineBaseline`) and
  `game.levelEpoch`.
- **`World::levelEpoch`.** An `EntityRef` from an earlier level no longer resolves to the
  new level's entity with the same handle. A save loaded in place keeps its refs, as
  before.
- **The player** plays every level through the host: offline, online and the net
  preview. A level that fails to start sends it back to the entry level, and if that
  fails too, it exits 1. A whole-world copy per session is gone.
- **Editor Play** moves the authored scene aside, plays one copy, and travels inside that
  copy. Stop puts the authored scene and selection back. Undo and the editor camera are
  never touched. Hot reload waits while a request is pending.
- The owner table is in [LevelLifetime.md](LevelLifetime.md#who-owns-what).
- Plugin ABI 17: `GameContext` and `EntityRef` changed, so older game modules are refused
  and need rebuilding.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `level_lifetime` | unit | see below |
| `level_lifetime_editor` | gpu | the real editor, 100 Play, travel to `a`, Stop cycles on `Tests/fixtures/levels`: the authored scene (as text), undo, selection and camera unchanged, and 100 arrivals |

`level_lifetime` builds levels in memory and checks:

- construction, `beginPlay`, `endPlay` and destruction counted exactly once per level;
- requests from `fixedUpdate`, `frame`, a UI-style callback between frames (while paused),
  and `beginPlay`;
- a level that doesn't load keeps the current one; `endPlay` throwing still ends the level;
  `beginPlay` throwing leaves nothing playing, with one failure;
- replaced and stopped requests are each cancelled once;
- Stop during preparation cancels and moves nothing in;
- an `EntityRef` and a `requestLevel` callback kept from level A are refused in B;
- `transitioning()` while a request waits (hot reload holds off);
- a short `beginPlay` chain works, and a cycle fails after 8 with the chain in the message;
- epoch overflow fails;
- 100 editor-style Play/A/B/Stop cycles leave the authored world exactly as it was;
- both world policies, on a plain `PlaySession`.

`level_lifetime` also passes in an AddressSanitizer + UndefinedBehaviorSanitizer build
(RelWithDebInfo, `-fsanitize=address,undefined`, leak checking on), with no reports.

### Still unverified

- **Loading in the background.** Preparation runs on the main thread, inside `process()`.
  Bounded, budgeted loading and honest progress are S04, and the loading screen is S05.
- **Spawns.** A request carries its spawn, but placing the player at a LevelSpawn is S05.
- **Menus and circuits.** No circuit node or menu button travels yet (S05).
- **ThreadSanitizer.** Not run in this batch. The host is main-thread only, and the one
  background producer it meets (save loading) is waited for when its session ends.
- **Online travel.** Online map changes still restart the match's session as before,
  through the host. Map rotation with a controlled session restart is S11.

## S04: asset dependencies and residency, part 1

### What was built

- **The dependency graph** (`Assets/AssetGraph.h`, [AssetResidency.md](AssetResidency.md)).
  Providers registered per file kind read scenes, prefabs, materials, models (what their
  importer opened), RML, RCSS, effects, circuits, post-process profiles, lighting and
  reflection manifests and navigation sidecars, each with its own parser.
  - Problems with stable codes: `AssetMissing`, `AssetPathCase`, `AssetPathOutside`,
    `AssetKindConflict`, `AssetUnreadable`, `AssetCycle` and the warning
    `AssetUnknownName`.
  - Cycles are found by a depth-first search with colours and reported as the loop.
  - Each node has a closure hash over its bytes, kind, provider versions and every
    dependency's closure hash.
  - The graph re-reads only files whose size or time changed.
- **`registerAssetPathField`** (`Engine/AssetReferences.h`) marks String fields that hold
  Content paths. Engine, Circuits, Effects and Multiplayer register theirs; game modules can
  too.
- **Leases** (`Assets/AssetResidency.h`). Each level holds a lease on its closure. Shared
  assets are counted once. The next level's lease is taken before the old one goes, so
  assets both use never retire. A plan before teardown says what's shared, new and retiring.
- **The budget.** `--asset-budget-cpu` and `--asset-budget-gpu` (MiB), or the editor's
  window. A level that doesn't fit fails as `BudgetExceeded` with totals, before the
  current level ends. A first level that doesn't fit exits 1.
- **`LevelHost`**: `Hooks::assets`, `setResidency`, and a `LoadReport` per start and
  travel with milliseconds for validate, read, prepare, retire, upload and start.
- **The player**: `--asset-dependencies [--level id] [--json]`, the dependency check in
  `--check`, `--load-report out.json` and `--no-asset-residency`.
- **The editor**: Window > Resource residency, with level loads, resident assets by owner,
  the open scene's dependency check, and the lease switch and budgets.
- **Fix from S03**: the player named its first level "startup" even when a catalog listed
  the startup scene, because the catalog was read after the name. It's read first now.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `asset_dependencies` | unit | every provider's edges on a project made on disk; each problem code with file, line and object; material and prefab cycles reported as loops; closure hashes change when a texture two steps down changes, and not for unrelated files; unchanged files aren't read again |
| `asset_residency` | unit | shared assets survive A to B; unique ones retire; duplicates coalesce; a level over budget is refused before teardown and the current level plays on; a start over budget fails; leases off asks nothing; 100 A/B cycles plateau and drain at Stop |
| `asset_dependencies_cli` | integration | `--asset-dependencies` as text and JSON, `--level`, closure hashes on the command line, `--check` failing on a missing texture and a cycle, `--load-report`, `--no-asset-residency`, a budget too small, bad budget arguments |

`player --check` on Sample, Arena, `Tests/fixtures/levels` and `Tests/fixtures/graphics`
reports no new problems. Unbaked lighting stays a warning: bake references are optional.

### Still unverified

- **Lazy loading and freeing.** Done in [part 2](#s04-loading-on-demand-part-2).
- **Measured CPU cost.** A lease counts each file's size as its CPU cost (still true after
  part 2).
- **Dynamic names.** Assets named only in C++ are warned about, not followed.
  `runtime_roots` for the cook is S09.
- **Models without an AssetFolder.** A model's edges come from its importer. A tool that
  doesn't import (none today) would see models as leaves.
- **The editor window** draws during Play (`editor --residency --play --frames 60` on
  `Tests/fixtures/levels` exits cleanly), and the editor's 100 level cycles pass with leases
  on. Nobody has looked at it on screen yet in this batch.

## S04: loading on demand, part 2

### What was built

- **`asset_loading all|levels`** in the project (default `all`), `--asset-loading` in the
  player, and **Player asset loading** in the editor's Resource residency window. `levels`
  with `--no-asset-residency` is refused (exit 2). The editor itself stays eager.
- **AssetFolder on demand** (`setOnDemand`). `open()` only lists files. `load(names,
  LoadOptions)` imports in batches bounded by an estimated staging size, decoding on
  workers and publishing on the main thread only, textures before models and sounds before
  materials. `cancelled()` is asked before each batch and after its decode; a cancelled
  load unloads what it published. A failed upload unregisters the file and destroys its
  partial GPU objects. `unload()` unregisters and destroys, and materials re-resolve, so
  one reading an unloaded texture falls back. Hot reload only re-imports loaded files.
- **Assets registry**: `removeTexture`, `removeModel`, `removeMaterial`. Handles to removed
  models and materials stay invalid; their slots aren't reused.
- **Deferred GPU frees.** `Renderer::pendingFrees()` and `flushFrees()`. Vulkan's
  `destroyMesh` now retires the mesh behind the frame fences instead of waiting for the
  device to go idle. The null renderer fakes two frames in flight. Plugin ABI 18.
- **`AssetResidency::Loader`**: `load(lease)` imports what a lease holds; the last lease on
  an asset going unloads it. `AssetFolderLoader` maps `Content/` ids to the AssetFolder.
- **`LevelHost` on demand**: plan, end, swap (old assets unload), upload, read, start.
  `start()` loads the first lease too. Load reports count loaded and unloaded assets and
  pending frees.
- **The player** holds a lease on the game circuit's assets for the run and one on the
  startup scene while it starts, flushes frees before the report, and writes
  `asset_loading`, per-load `loaded`, `unloaded`, `pending_frees` and the totals.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `asset_residency` | unit | with a loader: a lease loads what isn't in, its release unloads what only it held, a failed or cancelled load keeps nothing, the on-demand travel order (plan, end, unload, load, read), 100 A/B cycles on demand plateau and drain |
| `asset_streaming` | unit | a real AssetFolder on the null renderer's fake GPU: open imports nothing; batches under a staging limit; uploads only on the main thread; a decode cancelled before publishing is never uploaded and what was published goes again; a failed model or texture upload leaves nothing registered and frees its meshes; a material falls back when its texture unloads; frees wait for frames, then drain; a saved loaded file re-imports and an unloaded one doesn't; `AssetFolderLoader` under `AssetResidency`; 100 load/unload cycles keep the registry flat and drain |
| `asset_streaming_vulkan` | gpu | the same on Vulkan, with real frame fences |
| `asset_dependencies_cli` | integration | adds `--asset-loading levels` on level b (its material and texture load, and everything unloads with no frees pending at exit), the default reporting `all`, and both usage errors |

Run here on Linux (llvmpipe for Vulkan).

### Still unverified

- **Assets named only in code** aren't loaded on demand. A game that has them should stay
  on `all` until S09's `runtime_roots` lists them.
- **Measured CPU cost and staging.** CPU bytes are file sizes and staging is estimated as
  four times them; peak staging is measured per load but not against real allocations.
- **GPU cost before first load** is 0 for a file never loaded; plans for a level whose
  assets were never loaded under-count until they have been once.
- **Loading on a real game.** Sample and Arena still default to `all`. With
  `--asset-loading levels --renderer null --frames 30` both start, load their startup
  scene's assets (9 and 6) and unload them all at exit with no frees pending, but nobody
  has played them that way on screen. The loading screen that would show progress is S05.

## S05: travel tickets, part 1

### What was built

- **`GameFlow`** (`GameFramework/GameFlow.h`), which `LevelHost` implements and game code
  reaches as `game.flow()`. A request gets a ticket with a phase, a progress that only
  goes up (1 only once it succeeded), and a stable code when it ends. `cancel`, `status`,
  `restartLevel`, `returnToMenu` and `useExit` are there too. Observers subscribe with a
  token. Completions are delivered after the change, never during it. The last 64 ended
  tickets stay queryable.
- **One travel at a time.** A second request is refused as `Busy`; a redirect from
  `beginPlay` still works. This replaces S03's "the newer request wins".
- **Staged travel.** `level_travel staged` (the default) in the project. The first frame
  prepares the next level and pauses the current one; a frame renders; the next frame
  retires and starts. Until the boundary, cancel leaves the current level playing,
  unpaused and untouched. After it, cancel returns `TooLateToCancel`. `level_travel
  instant` or `player --instant-travel` does it all in one frame. The editor follows the
  project's setting.
- **Codes** for every refusal and failure: `Busy`, `NotPlaying`, `StaleLevel`,
  `UnknownLevel`, `UnknownSpawn`, `InvalidRequest`, `AuthorityRequired`, `LoadFailed`,
  `BudgetExceeded`, `StartFailed`, `Cancelled`, `RedirectCycle`, plus `UnknownTicket`,
  `AlreadyFinished` and `TooLateToCancel` for `cancel`.
- **Spawns.** An empty spawn is the catalog's `default_spawn`. A named one that the
  destination doesn't have fails as `UnknownSpawn` before the current level ends. The new
  level sees `game.arrival`: the spawn and its world transform.
- **Exits.** `useExit` travels through a `LevelExit` and disables a one-shot exit once its
  request is accepted. A disabled exit is refused.
- **Arriving clean.** Actions held through the change read as released until they're let
  go (`Input::holdUntilReleased`), the camera history is cut, and the loading time isn't
  caught up in ticks (`Engine::discardFrameTime`).
- **Online.** A request during network play is refused (`AuthorityRequired`) instead of
  disconnecting the peers; S11 adds map rotation.
- **Circuit nodes**: `Level.RequestTravel`, `Level.RestartLevel`, `Level.ReturnToMenu`,
  `Level.UseExit` (accepted and refused outputs), `Level.TravelStatus` and
  `Level.OnTravelFinished`, which runs only in the level playing afterwards. `player
  --check` fails literal levels and spawns that don't exist. The node references are
  regenerated.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `level_travel` | unit | staged phases and progress; A to B to A; explicit and default spawns; restart and menu; every refusal code; a UI action fired twice travels once; cancel queued and at the boundary leaves the level playing with the same epoch; `TooLateToCancel` from the new level's `beginPlay`; a user's pause kept through a cancel; two exits in one frame travel once and a one-shot exit is used once; a stale `game.flow()`; a missing spawn and a broken scene fail before the boundary; a start failure after it leaves nothing playing; Stop while queued, while reading and at the boundary; each ticket completes exactly once, after the change; unsubscribing; history bounded at 64; instant mode; a redirect from `beginPlay` |
| `game_flow` | unit | a game circuit using every `Level.*` node: tickets and codes from the outputs, `Level.OnTravelFinished` once in the new level with `Level.TravelStatus` reading `succeeded`, a refusal reaching the refused output, a cancel heard by the level that stays, exits, restart and menu from circuits, stale and missing flows refusing |
| `input` | unit | a key held through a level change reads as up, with no release event, until it's let go; other actions count at once |
| `level_check_cli` | integration | `--check` passes a good `Level.RequestTravel` and fails an unknown level or spawn, with the line |
| `level_lifetime`, `asset_residency` | unit | S03 and S04 behaviour still holds, with the Busy rule replacing "the newer request wins" |
| `level_lifetime_editor` | gpu | the editor's 100 Play/travel/Stop cycles, now with staged travel |

Full local suite: 95 of 95 pass on Linux, gpu included (`test_snapshot_delta` failed once under `ctest -j8` and passed three times alone, as it has before).

### Still unverified

- **No loading screen yet.** The frame at the boundary is there for one, but the
  application's menu, pause, loading and error screens come in part 2, with
  `--flow-test`.
- **Shutdown in every phase.** Stop is tested while queued, while reading and at the
  boundary. Retiring, uploading and starting all happen inside one `process()` call, so
  there is no moment between them for Stop to arrive from outside.
- **Reading isn't on a worker.** The scene is read on the main thread, inside the first
  frame. Section 6.1 allows that until scene parsing is split from world construction.
- **Spawns on demand.** With `asset_loading levels` the scene is read after the old level
  has gone, so a missing spawn fails the start (`UnknownSpawn`, nothing plays) instead of
  being refused before.
- **Placing the player.** `game.arrival` says where; moving the player there is the
  game's `beginPlay`. Sample and Arena don't use it yet (S07's game will).

## S05: application shell and flow tests, part 2

### What was built

- **The application shell** (`GameFramework/AppShell.h`, [AppShell.md](AppShell.md)): the
  player's own main menu, pause, loading and error screens, drawn over the level.
  - The menu has New game, Continue, Level select, Settings, Credits and Quit.
  - Pause has Resume, Restart checkpoint, Restart level, Settings, Main menu and Quit.
  - Loading shows the ticket's real phase and progress, with Cancel only while
    cancelling is safe.
  - Error shows the stable code and reason, with Retry, Back to the level (only when it
    still plays), Main menu and Quit.
  - New game over existing progress asks first, and confirming sets `resetCampaign`.
- **Every screen can be switched off.** The project key `app_shell` takes `off`, `all`,
  or any of `menu level_select pause loading error`; `player --app-shell` overrides it.
  Without the key, only `loading` and `error` are on, and they only show during or after
  travel. So Arena, Sample and RenderLab look the same as before.
- **A separate UI layer.** The shell has its own `GameUI`, with its own RmlUi context, a
  `flow` data model and no `forEngine` registration (`GameUIOptions`).
  - The shell gets input first; the level's UI input is off while a screen shows.
  - The key that opens a screen doesn't also press `ui_back` on it.
  - A mouse the game captured is freed while a screen shows and captured again for the
    same level.
- **The zero-asset fallback.** `builtin:` documents are compiled into the program
  (`addBuiltinDocument`), using only the engine's font. A project's `Content/Shell/*.rml`
  replaces a built-in screen. If it doesn't load cleanly, the built-in one shows instead,
  and the reason is logged. `player --check` checks those files against the `flow` model.
- **Recovering from a failed start.** When a level doesn't start and nothing plays, the
  application can still request travel (Retry, Main menu). The failed level can't: its
  requests are `StaleLevel`. With the error screen on, the player stays on it instead of
  jumping back to the entry level.
- **Application requests are never redirects.** Before, a request made before a level's
  first tick committed at once, with no boundary frame. Now that only applies to the
  level's own requests, so the shell's buttons always get the loading screen.
- **`player --flow-test scenario.json [--json report]`** ([FlowTest.md](FlowTest.md)).
  - Steps: `click` (a shell button by id, or a level document's), `press pause`,
    `travel` (with `expect` for a refusal), `wait_phase`, `assert_level`, `assert_value`
    (`flow.*` and `game.*`), `save`, `load` and `quit`.
  - Each step has a timeout, inside the scenario's total.
  - The reader is strict, and there are no scripts.
  - It exits 1 at the first failure. The report has every step, the state when it
    ended, and every ticket.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `game_flow` | unit | `app_shell` parsing. The built-in screens validate clean. The menu shows over the menu level, its disabled buttons say why. New game pressed twice travels once (the second is `Busy`, not an error). Loading shows `awaiting_boundary` at 45% with Cancel. Pause pauses the session and Resume resumes it. Restart level cancelled at the boundary goes back to the paused pause screen, same epoch, still paused. Restart for real arrives unpaused. A level that doesn't load shows `LoadFailed` with the level still playing, Retry fails again, and Back returns to the menu. A level that doesn't start leaves nothing playing, and Main menu recovers. New game over progress asks, and confirming resets the campaign. A project screen replaces the built-in one, and a broken one falls back to it. Screens switched off don't show. Every ticket completes once. Scenario parsing refuses unknown keys, two kinds, bad phases and codes, shell-like steps and trailing text. A run stops at its first failed step, with the report. |
| `level_travel` | unit | after a failed start, the application's request recovers to a playing level; the failed level's is `StaleLevel` |
| `game_flow_cli` | integration | the milestone through the shell's buttons, headless: menu, New game, loading, A, B, Restart level from pause, Main menu (23 steps), from `--app-shell all` and from the project key. Also: refusals (unknown level, missing spawn, Continue); save, pause, load; a broken level A (error, Retry, Main menu); a broken menu scene (Main menu fails, the level plays on); a project menu screen with `--check`; a failing step's exit code and report; bad scenarios as usage errors; a click without the shell. |
| `game_flow_vulkan` | gpu | the same milestone with every screen drawn by the Vulkan renderer (lavapipe) |

Full local suite (S05 part 2): 97 of 97 pass on Linux, gpu included. Five long GPU tests failed once when the player was rebuilt while they ran; all five passed when run again on their own.

### Still unverified

- **A person at a real build.** The plan's completion asks for someone to use the buttons
  in a running build and watch for stale audio and memory growth. The milestone has run
  only as a flow test: headless, then on lavapipe. On lavapipe the screens are drawn but
  nothing compared the pictures; a screenshot of the menu was checked by eye. Audio and
  memory over a long run weren't measured.
- **The editor.** Editor Play doesn't run the shell yet; it plays levels as before.
- **Continue, Restart checkpoint and Settings** are present but disabled, each with its
  reason, until S06 (campaign saves and checkpoints) and S08 (settings).
- **A menu scene that doesn't load at startup.** The first scene is loaded before the
  levels start, so a broken entry scene still stops the player with exit 1. A broken menu
  reached by travel shows the error screen.
- **Shutdown in every phase** is as in part 1.
- **The loading screen's length.** It shows for the frames travel really takes. Reading
  is on the main thread, so that is usually two frames.
- **`press`** knows only the pause action; other input isn't scripted in v1.

## S06: campaign saves, part 1

### What was built

- **Save format 2** (`save_game 2`, [SaveGames.md](SaveGames.md#the-file-format)).
  - The envelope: application, profile, game save schema, release version, revision,
    level and spawn (checkpoint is reserved for part 2), campaign values and a checksum.
  - The checksum is FNV-1a over the file without its own line. It sits in the header, so
    a file cut short fails it.
  - Format 1 saves still load, in place, as schema 1, with the campaign left as it is.
  - Every problem has a stable code (`SaveError`) at the start of its message.
  - The reader refuses files over 256 MiB, more than 4096 values, values over 64 KiB and
    more than 4M entities before it builds a world.
- **`game.campaign()`** (`CampaignState`). It is owned by `LevelHost`, so it survives
  travel, and goes into every save.
  - New game with `resetCampaign` clears it.
  - Restart level puts back the campaign as it was on entering, so a reward isn't kept twice.
  - The shell's New game asks to confirm when the campaign holds something.
- **Loading across levels.** `TravelRequest::restore` carries a read save. A save from
  another level is a `LoadSave` travel through the usual tickets and loading screen. The
  new session starts with `StartReason::RestoreSave`, and `PlaySession::restore` puts the
  save in before the first tick or frame. Then physics is rebuilt and `afterLoad` and the
  circuits run. A same-level save still loads in place. Tickets keep the request without
  the save's world.
- **Compatibility and migrations.** `SaveIdentity` is the game, the profile, the
  project's new `save_schema` key and `GameInstance::migrateSave`.
  - Another game's or profile's save is refused, and so is a newer schema.
  - Migrations run step by step, oldest first, on the detached save, on the load worker.
    The file isn't touched.
- **Durable writes** (`writeSaveFile`):
  1. a unique temporary file, written, then flushed and synced;
  2. the old file copied to `.bak`;
  3. an atomic rename;
  4. the folder synced on POSIX (a failure here is only a warning: the save is in).

  Other properties:
  - Writes run in order, one writer job at a time, with at most two in flight (`Busy`).
  - Each slot's revision is checked against what this session last read or wrote. A
    stale writer gets `Conflict` and its save is kept as `.swsave.conflict`.
  - A damaged slot loads its `.bak`, saying so. `remove()` removes the backup too.
- **Continue** in the shell. It loads the newest save this game and profile can load,
  passing over damaged or foreign ones and saying why. A format 1 save is mapped to its
  level by its scene.
- **CLI** (all of them read files only, except `--save-migrate`, which loads the game
  module for its migrations):
  - `--save-info file [--json]`;
  - `--save-migrate file --output new`. It rewrites the header and keeps the entity body
    byte for byte. It never writes over the input or an existing file.
  - `--saves` now lists damaged saves with their problem, and its JSON has the whole
    envelope, `loadable` and `code`.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `save_travel` | unit | A save carries level, spawn, game, profile, revision, checksum and campaign. Campaign values survive A to B; level values don't. Loading A's save from B travels to A (`RestoreSave`, the right GameMode), with the save in at tick 0 (crate, values, campaign) and the level's epoch. Restart level drops a reward taken since entry. A same-level load while paused goes in place, still paused. A load still reading when its level ends is dropped. Another game's or profile's save is refused, and the level plays on. New game resets the campaign. On a "next launch", a save made in B is continued from the menu into B. Tickets don't keep the save's world. |
| `save_migration` | unit | A format 1 save reads and loads in place (campaign kept). Migrations 1 to 2 to 3 run in order on the detached save, and the original file is unchanged. A refusing migration gives `MigrationFailed`. A newer schema gives `NewerSchema`, a newer format `NewerFormat`, and a huge value `TooLarge`. The `--save-migrate` rewrite has a valid checksum, migrated values, a filled-in level and the same entity body. |
| `save_durability` | unit | A damaged byte or a cut-short file is `Corrupt`. Without its checksum line, an edit loads. `--saves`-style listing shows broken files with their problem. The second save keeps the first as `.bak`, and a damaged slot loads it with a warning. A failure at `open`, `write`, `flush`, `backup` or `replace` leaves the old save byte for byte with no temporary files, while one at `directory` still commits. Two writers: the stale one gets `Conflict`, the newer save stays, and the stale save is kept aside; after reloading, it saves. A third save while two are in flight is `Busy`, and the two write in order. Remove deletes the backup. |
| `save_games_cli` | integration | An edited file fails `--check` and `--load` with `Corrupt`. `--save-info` shows format 2 and loadable. `--save-migrate` refuses its own input and writes a loadable copy. `--saves --json` lists the slot as loadable in its level. |
| `game_flow` | unit | Continue enabled once there's a save: a `LoadSave` ticket to B, with the save in at the start and its campaign. |

Full local suite: 100 of 100 pass on Linux, gpu included.

### Still unverified

- **Real power loss.** The fault hook simulates a failure at each write step, and the
  real filesystem tests show the rename replaces the file. Neither proves anything about
  power loss. On Windows the folder isn't flushed (NTFS journals the rename).
- **Two processes at the same instant.** The revision check happens just before the
  rename. Two copies of the game saving the same slot in the same millisecond could
  still both write; there's no OS lock. The test runs the two writers one after the other.
- **Permission errors.** CI runs as root in containers, where a read-only folder can
  still be written. Disk-full and permission failures are covered only by the fault
  hook.
- **Snapshot cost.** The world is still copied on the main thread. It hasn't been
  measured on a big level.
- **Part 2:** checkpoints and Restart checkpoint, rewards granted once by completion ID,
  tombstones for destroyed authored entities, a dedicated restore hook instead of
  beginPlay then `afterLoad`, and stable IDs instead of exact handles. Until then, a
  cross-level load runs the level's `beginPlay` on its authored world, then replaces the
  world with the snapshot and calls `afterLoad`.
- **The editor's Save Games window** shows saves as before. It doesn't show
  compatibility, backups or conflicts yet.

## S06: checkpoints and lasting progress, part 2

### What was built

- **Checkpoints.** `SaveGames::checkpoint(id)` finds an enabled `Checkpoint` in the level
  and saves to the `checkpoint` slot, with the checkpoint's id, its spawn and the
  campaign id.
  - Its state is `Saving`, then `Saved` or `Failed`, set by the writer once the file is
    on disk. A failed write leaves the previous checkpoint as the restart point.
  - `hasCheckpoint` and `restartCheckpoint` accept only a checkpoint from the current
    campaign (`campaign_id`); otherwise they fail with `NoCheckpoint`.
  - A checkpoint in another level travels there with reason `Checkpoint`.
  - The shell's Restart checkpoint is enabled from that check when the pause screen
    opens, and `flow.checkpoint_state` follows the state.
- **The campaign** now carries an id, completions and destroyed identities, besides its
  values.
  - `complete(id)` is true once per campaign.
  - New game clears everything and makes a new id.
  - Restart level restores the whole campaign as it was on entry, completions included.
  - A save's campaign is in before the level's session starts.
- **Finishing a level.** `GameContext::finishLevel(next, spawn)` requests the travel. Once
  it's accepted, it writes the `autosave` slot as a `fresh` save: the campaign and the
  destination, with no world. Loading a fresh save always travels, starts the level as
  authored (`StartReason::Fresh`) and applies only the campaign.
- **Things that stay gone.** When a level is left (not restarted), `LevelHost` records its
  authored `PersistentIdentity` ids that are no longer alive. When the level is entered
  again, those entities and their children are destroyed before its session starts. It
  is switched by the project key `remember_destroyed`, which defaults to true, and
  `LevelHost::setRememberDestroyed`; the editor follows the key too.
- **Format 2 lines:** `fresh`, `campaign_id`, `completed`, and `destroyed <level> <ids>`.
  The reader bounds them, and `--save-migrate` rewrites them.
- **Circuits:** `Level.ReachCheckpoint`, `Level.RestartCheckpoint`, `Level.FinishLevel`,
  `Campaign.Complete`, `Campaign.Set` and `Campaign.Get`. The node references are
  regenerated.
- **Plugin ABI 19**, for part 1's `GameInstance::migrateSave` and `GameContext` changes.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `save_checkpoints` | unit | An unknown checkpoint is refused, and none yet gives `NoCheckpoint`. A checkpoint goes Saving to Saved and restores the level as it was. A checkpoint whose replace fails is `Failed`, and Restart still goes to the previous one. From B, Restart checkpoint travels to A with reason `Checkpoint`. `complete` is once only, and saves keep completions. Restart level undoes completions and rewards made since entry. `finishLevel` writes a fresh autosave with no entities, the completion, the reward and the next level. In a "next launch" it resumes B as authored (`Fresh`) with the reward given once. The autosave loaded from inside B travels again, to a new epoch. |
| `save_destroyed` | unit | An enemy killed in A is gone when A is entered again, and the other stays. Restart level brings back this visit's kills but not an earlier visit's. A save made in B carries A's destroyed list into a new run. New game brings everything back. With `setRememberDestroyed(false)`, A comes back as authored. |
| `game_flow` | unit | Restart checkpoint is disabled until a checkpoint is written, then `checkpoint_state` reads `saved`, and the button restores the level and closes the pause screen. |
| `circuit_check`, `network_check` | integration | the regenerated node references match the six new nodes |

Full local suite: 102 of 102 pass on Linux, gpu included.

### Still unverified

- **Restore is still the legacy exact mode.** A dedicated restore hook and stable-ID
  remapping (plan 14.1, steps 4 and 6) aren't built. A restore runs `beginPlay` with
  `RestoreSave`, then swaps the snapshot in and calls `afterLoad`; games skip fresh
  spawning by checking `startReason`.
- **Prefab copies.** Only an entity's own `PersistentIdentity` id is tracked, not the
  combined identity of a part inside a prefab copy.
- **Runtime-spawned entities** don't get tombstones; only the save snapshot holds them.
- **Quit during Saving** waits for the write (the `SaveGames` destructor drains it) with no
  timeout or on-screen "Saving..." yet.
- **The editor's Save Games window** is unchanged.

## S07: Reactor Run, part 1

### What was built

- **Games/ShippingDemo, "Reactor Run"** (module `shippingdemo`, in the default
  `SHOCKWAVE_GAMES`): a menu, a training level, a factory level and an ending, joined by
  `Config/Levels.swlevels`. The application shell is on (`app_shell all`) and levels
  load and unload their own assets (`asset_loading levels`). See its
  [README](../Games/ShippingDemo/README.md).
- **Game-owned components**, all saved: `DemoPlayer`, `Health`, `Loadout`,
  `ObjectiveProgress` (Reach, Destroy, Collect, Use, Hold), `InteractionTarget` (Use,
  Pickup, Exit), `DemoEnemy`, `DemoGate` and `CampaignProgress`. `DemoRules::validate`
  refuses bad content (unknown groups, items, objectives, duplicate ids, a Use objective
  whose target isn't usable) with one line per problem, and the GameMode won't start
  such a level ("gameplay content").
- **Rules** in `DemoRules`, one fixed tick from a `PlayerCommand`, with a stated order: a
  death wins over a victory on the same tick, one objective completes per tick, the Hold
  timer counts ticks. Enemy sight and movement is a Schedule system across cores;
  damage, pickups and objectives are applied on the main thread after physics.
- **Input** is read per frame and latched per tick; prompts name the bound key or button
  on the device used last (`bindingLabel`), so rebinding changes them.
- **Two ways out.** Training leaves through a circuit (`Circuits/TrainingExit.swcircuit`:
  a player-only sensor, `Campaign.Complete`, `Level.FinishLevel`); the factory leaves
  through C++ (`InteractionTarget` Exit, the results screen, `finishLevel`).
- **Checkpoints** are reached by walking to a `Checkpoint`; going down offers Restart
  from checkpoint (the level when there's none), Restart level and Main menu.
- **Assets**: two generated models, seven sounds and two badges, made by
  `Tools/GenerateDemoAssets.py` (CC0, recorded in `Content/PROVENANCE.md`). Training and
  the factory each have their own model, sound, UI image and scene dependency.
- **Real input in flow tests**: `hold`, `tap`, `look`, `wait` and `wait_value` steps go
  in through `Input::inject` and `Input::injectMouseMotion`, which act like the device
  through the bindings; a `pad` binding plugs in a virtual gamepad
  (`Input::removeVirtualGamepad` unplugs it). See [FlowTest.md](FlowTest.md#real-input).
- **Credits**: the shell's Credits page reads `Content/Shell/credits.txt` when a project
  has one.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `shipping_demo` | unit | On the real scenes with real Jolt: all four pass the content checks and five broken copies are refused by name; injected W walks forward through the bindings, a rebinding is followed (old key does nothing, new key walks), prompts read E, Left mouse, Right trigger and then F; a virtual pad's stick walks and unplugging it stops the player; two hits destroy a target, an empty magazine reloads itself after 72 ticks, an empty reserve dry-fires; pickups only when needed and gone after; the factory's sentries shoot back and fall, the door opens, the console only works in its turn, the exit stays shut; the 8 s hold takes 8 s at 30, 60 and 120 ticks a second; a death on the tick the last objective would complete wins; a completion is granted once. |
| `shipping_demo_flow` | integration | `--check` passes; the whole game (menu, training, factory with checkpoint and results, credits, back to the menu and a confirmed New game) played with real keyboard and mouse input, then again on a virtual gamepad, each from a fresh profile; save and quit in the factory after the checkpoint, then in a second run Continue resumes there with the sentries still gone and finishes the game. |
| `input` | unit | injected taps, holds, mouse motion, the virtual pad and its unplugging, rebinding and NaN all read as the device would |
| `game_flow` | unit | the new steps parse, and nine malformed ones are refused with the reason |

Full local suite: 104 of 104 pass on Linux, gpu included.

### Still unverified

- **Part 2** is below: editor authoring, failures by kind, the game's checks in
  `--check` and the packaged demo.
- **Drawn on a GPU.** The flow tests run with `--renderer null`; nobody has looked at the
  levels, HUD and screens on a real display yet.
- **Controller-only menus.** The gamepad run plays the levels on a pad but clicks the
  shell's buttons; menu navigation on a pad alone is S08.
- **The enemy AI** walks straight at what it sees; there's no navigation mesh, so a
  sentry can be stuck behind a crate.

## S07: Reactor Run, part 2

### What was built

- **Failures by kind.** `player --check` lists every problem under the kind of fix it
  needs: missing or broken content, navigation, circuit errors, physics setup, gameplay
  code and rules, or project settings (`ProblemKind`, `problemKindOf` from a diagnostic's
  code). The editor's Validate shows the same kinds, with a filter for each, and each
  finding's code. See [Levels.md](Levels.md#checks).
- **The game's own checks in `--check`.** `GameInstance::checkScene` (plugin ABI 21)
  returns one line per content problem; `--check` and the editor's Validate run it on
  every scene, loaded but not playing, as gameplay problems. Reactor Run returns
  `DemoRules::validate`. A game that then refuses the same content isn't reported again
  as "gameplay failed to start".
- **Editor level authoring** (Project > Levels and the Create menu):
  - **New level** writes a playable scene and adds it to the catalog; **Duplicate level**
    copies the selected level's scene and catalog entry; **Play from entry** plays the
    game from its start, beside **Play level** for the selected one.
  - **Create > Level**: a level spawn (`start` first), a level exit aimed at the catalog's
    next level, and a checkpoint with its own spawn.
  - In the Inspector, an exit's `destination` picks from the catalog's levels and a
    checkpoint's `spawn` from the scene's spawns.
  - **Validate** also runs the scene checks: the game's rules, circuits that are missing or
    don't compile, a stale or missing navigation mesh, physics and navigation setup, and
    references that didn't resolve. Double-click a finding to open its scene.
  - `editor --level-authoring` drives these actions for tests.
- **`Config/Package.swpackage`**: Reactor Run, program `ReactorRun`, version 1.0.0,
  without its docs and tests. The packaged game plays through from outside the source
  tree.
- **Playtest script** (`Games/ShippingDemo/Docs/Playtest.md`): starts from the package,
  and has a section for level makers.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `shipping_demo_flow` | integration | as in part 1, then (in modular builds off Windows) `player --package` makes `ReactorRun` without Docs, Tests or Source, and the packaged launcher plays the whole game, then save and quit in the factory and Continue in a second run, each from a fresh profile |
| `shipping_demo_authoring` | gpu | on a copy of the game, `editor --level-authoring` adds a level, places a spawn, checkpoint and exit in it, duplicates training, saves, and Validate finds no errors, and `player --check` passes on what the editor wrote; on a copy broken three ways, Validate names the missing entity under content, the missing circuit under circuit and the objective without a target under gameplay, and `--check` lists each under its heading, plus the exit to an unknown level under content, without "gameplay failed to start" |
| `level_catalog` | unit | each diagnostic code maps to its kind, and every kind has its own name and heading |

Full local suite: 105 of 105 pass on Linux, gpu included.

### Still unverified

- **The human playtest.** The script is written; nobody has played the package on a
  display with a real keyboard, mouse and gamepad yet.
- **The editor by hand.** The authoring actions are tested through
  `editor --level-authoring`, not by clicking the window; the Inspector's destination
  picker has no automated test.
- **Packaging on Windows**, like every package test (docs/Packaging.md).
- **Navigation and physics findings** in Validate are only as good as the engine's own
  diagnostics; Reactor Run's levels have no navigation mesh to go stale.

## S08: Settings and localization, part 1

### What was built

- **Player preferences** (`Engine/Preferences.h`, [Preferences](Preferences.md)): display,
  audio, controls, accessibility, camera and language, described once in
  `preferenceInfo()`. Layers: the engine's defaults, the project's
  `Config/Preferences.swprefs`, the player's profile, then `--pref key=value`, each value
  with the layer it came from. `player --preferences [--json]`, `--no-user-preferences`
  and `--preferences-schema` ([PreferencesSchema](PreferencesSchema.md)). Apply and save
  are separate, and an apply validates everything before it uses any of it.
- **Display modes**: windowed, borderless and fullscreen at a chosen resolution
  (`Window::setMode`, `Window::resolutions`). A change is kept within 15 seconds or put
  back, and the attempt is written to `Display.pending` first, so a crash goes back too.
- **Audio categories**: effects, music, UI and voice, each a volume on top of the master
  ([Audio](Audio.md#volume-by-category)). `--audio-volume` is now the master volume
  preference.
- **Localization** (`Engine/Localization.h`, [Localization](Localization.md)):
  `Content/Localization/<locale>.swstrings` catalogs, placeholders, the fallback from the
  locale to its language, the project's `default_language`, the engine's own English, then
  `[missing: key]`. Documents show `{{text.key}}`; game code calls `game.text()` and
  `game.format()`. `player --check` reports broken catalogs, stray keys, mismatched
  placeholders, documents' unknown keys, and untranslated keys as warnings.
- **The Settings page** in the shell, from the menu and pause: tabs of `< value >` rows
  built from the preference table, plus graphics quality, render scale, VSync and frame
  cap; Apply, Defaults and Back; the keep-display countdown. The shell's own words are
  `shell.*` keys, translatable like a game's. `AppShell::Setup::settings = false` turns
  the page off.
- **Interface size and high contrast** for every document: `ui_scale` scales `dp`, and
  `high_contrast` sets `body.high_contrast` (the shell and Reactor Run style it).
- **Reactor Run** uses look sensitivity, inverted look, field of view, camera shake,
  reduce flashes and toggle instead of hold, and its HUD, results and down screens read
  from `en.swstrings`, with a partial `fr.swstrings`.
- `GameContext::preferences()`, `localization()`, `text()` and `format()`. Plugin ABI 22.
- A fix in GameUI: `click` and `elementVisible` bring data bindings up to date first, so
  an element a `data-for` just made can be found.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `player_preferences` | unit | the `.swprefs` format (header, quotes, unknown and duplicate keys, ranges, NaN, the line), every key's getter reads back through its setter; layers and origins; apply without save and save without apply, the profile holding only what differs from shipped; a display change kept, timed out, reverted, and undone on the next start after a crash; a broken profile ignored; catalogs, keys, placeholders, the whole fallback order, `format()` with braces and a missing value, and every finding `check()` makes |
| `preferences_schema` | unit | `docs/PreferencesSchema.md` is what `--preferences-schema` writes |
| `game_flow` | unit | the Settings page through its buttons: tabs, rows from the table, steps clamped at the ends of a range, field of view's 0-or-50..120, the frame cap's list, Back dropping the draft, Apply using and saving it, Defaults, the display countdown, keep, and the timeout; the shell's text in French with English for the rest |
| `audio` | unit | each category's volume applies on its own, multiplies with the master, and an unknown category is refused |
| `shipping_demo_flow` | integration | as in S07, then Reactor Run's Settings page through the real player: a volume, French and borderless applied and kept, the HUD in French in the next level; the saved profile has all three; `--preferences` shows the profile's language, `--pref` adds a session value, and a bad one fails; the same Settings flow from the package (so the catalogs are packaged) |

Full local suite: 107 of 107 pass on Linux, gpu included.

### Still unverified

- **A real display.** Borderless and fullscreen are tested headless (the offscreen video
  driver); nobody has switched modes on a monitor, and the resolution list there is
  empty.
- **Reading the page by hand**: long labels, a large interface size and non-Latin text are
  part 2's layout tests (below).
- **Subtitles**: the preferences exist, but no game has speech yet.

## S08: Input, rebinding and reliability, part 2

### What was built

- **Input layers** (`InputLayer`, [Input](Input.md#who-has-the-input)): gameplay, HUD,
  menu, loading and modal, raised by their owners. The shell raises its screens' layer;
  any game UI with keyboard focus raises `menu`. `gameplayFocused()` is what gameplay
  checks; Reactor Run does.
- **No stray presses**: a change of the top layer to or from one above the HUD is a
  barrier, and so are the window losing focus and a controller being plugged in, so the
  press that closes a menu never also fires.
- **Rebinding** on the Settings page's Controls tab: keyboard and gamepad per action,
  listening for 10 seconds, Escape or Start to cancel, conflicts named on both actions,
  Default controls. Saved as `<config>/Controls.swinput` (only what differs from the
  project's), loaded at start, kept by action id across game updates.
- **Controller unplugged**: the shell pauses the level (offline) and says so, and says when
  it's back; `Setup::pauseOnControllerLoss = false` turns it off. Flow tests unplug with
  `{"unplug": "gamepad"}`.
- **Audio device recovery**: the engine retries opening a lost audio device every 2 s
  ([Audio](Audio.md#when-the-audio-device-goes-away)).
- **Editor: Window > Player Settings** ([Preferences](Preferences.md#in-the-editor)):
  edits `Config/Preferences.swprefs`, and previews Interface size, Language and High
  contrast in the viewport without saving them; Check layout. The editor now loads the
  game's catalogs, so `{{text.*}}` shows the game's text, and reloads them on save.
  `--player-settings`, `--preview-language`, `--preview-ui-scale`.
- **Layout checks**: `GameUI::layoutProblems()` (text past its box, a box out of its
  parent, text off the screen) and `missingGlyphs()`; `GameUI::setDensity()` for tests.
  `player --check` names catalog text with characters no font draws.
- **The shell's panel** is centred by flex, never larger than the screen, and scrolls
  (with a styled bar); rows are flex, so long labels wrap instead of pushing the buttons
  out. The layout tests found the Settings page running off a 720p screen on the Controls
  tab, and labels and values running out of their rows in long text.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `input` | unit | layers and their order; the barrier when a menu closes, none for the same layer again or HUD changes, the barrier on focus loss; the devices version; the last-used device; labels; the player's bindings over the project's, and actions the game dropped ignored |
| `game_flow` | unit | the shell's layers while its screens show; the Controls tab: listening, a press from the wrong device ignored, Escape cancelling, the timeout, conflicts, Apply saving only what differs, Default controls; the pad in use unplugged pausing the level; the main menu, level select, every Settings tab, listening and credits laid out in English, pseudo-German and Greek at 720p, 1080p, 1440p and 4K at 200%, and Interface sizes 1.5 and 2, with no problems, while a 320 x 200 screen has some; every character of the three catalogs has a font, CJK doesn't |
| `audio` | unit | the audio device failing to open, then reopening by itself |
| `localization_glyphs` | unit | `player --check` passes on Reactor Run and fails on a Japanese catalog it has no font for, naming the characters |
| `editor_player_settings` | gpu | the editor previewing Reactor Run in French at 1.5x reads the French catalog, and writes nothing into the project |
| `shipping_demo_flow` | integration | as in part 1, plus fire rebound to F and a pad listen cancelled, the saved `Controls.swinput` holding exactly that; the pad unplugged mid-level pausing it, the message, Enter carrying on, then pause and resume from the pad |

Full local suite: 109 of 109 pass on Linux, gpu included. The first full run caught a
focus left on the hidden loading screen holding the `menu` layer, so Reactor Run ignored
W after the first travel; a focus only counts while its element is visible.

### Still unverified

- **Real controllers**: unplugging is tested with the virtual pad; a real pad's connect
  barrier and a Bluetooth pad sleeping haven't been tried by hand.
- **A real audio device** going away: the test makes SDL's driver fail; nobody has pulled
  headphones out.
- **Fonts for other scripts**: `--check` says when one is missing, but no font for CJK,
  Arabic or Indic text ships, and right-to-left layout hasn't been tested.
- **The game's own documents**: the shell's screens are tested at every size; Reactor
  Run's HUD has only been checked in the editor with Check layout.
- **The editor window by hand**: its preview is tested from the command line; editing
  the shipped values through the window hasn't been driven by a test.

## S09: Cooking and release checks, part 1

### What was built

- **The cook manifest** ([Cooking](Cooking.md)): `player --cook` writes
  `Saved/Cook/<target>/Cook.swcook`, plain sorted text listing every level with its
  closure hash, every asset each level reaches (kind, size, hash, closure, which levels),
  the roots no level names (catalogs, fonts, settings, the shell's screens, the project)
  and the engine's licenses. A content ID hashes the content; a build ID adds the engine,
  its configuration, the target and the plugin ABI. No times, no absolute paths.
- **Files code loads by name**: `include "<pattern>"` in `Config/Package.swpackage` makes
  matching files roots. Reactor Run includes `Content/Audio/*.wav`, which its code plays.
- **`player --cook-info <manifest>`** prints a manifest, `--json` writes it as JSON, and
  `--verify <folder>` checks every file against it. A manifest whose IDs don't match its
  records is refused as edited or damaged.
- **Release policy** (`--cook --release`, `--check --release`): an optimised engine build,
  no scene or graph errors (an unknown asset name is one), nothing a level needs excluded,
  no `.blend` sources, no absolute developer paths in shipped text, no test Steam AppID,
  every license present, and a target this engine is built for. A failed cook keeps the
  last good manifest.
- **Release builds by default**: CMake configures Release when no build type is given,
  the README says so, and the packager warns about any unoptimised engine, not only
  Debug. An unoptimised build is the likely reason a packaged game ran at seconds per
  frame.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `cook_reproducibility` | integration | two cooks of a copy of Reactor Run, and one of a copy in a folder with spaces and non-ASCII letters started from another working directory, are the same bytes; nothing names where it was made; `--verify` passes; `--json` has the entry level and 4 levels |
| `cook_dependencies` | integration | a change to the stylesheet three levels share changes exactly those three levels and the content ID; a file edited after the cook and an edited manifest are caught; a missing texture fails the cook and keeps the manifest; an excluded document a level needs fails; without the include pattern the code-played sounds are gone; an absolute path fails only in release; `--check --release` passes; another target is refused |

### Still unverified

- **Packages from the cook**: `--package` still copies the project's files; shipping only
  the cooked closure is part 2.
- **Other targets**: an engine cooks only for the platform it was built for; Windows and
  macOS cooks need those builds.
- **Files code builds names for**: a name put together at run time is only found if an
  `include` pattern covers it; nothing checks that every such name is covered.

## S09: Release packages and the editor, part 2

### What was built

- **`player --package --release`** ([Cooking](Cooking.md#release-packages)): runs a release
  cook after the checks, ships only what the cook lists plus `Game/Cook.swcook`, and checks
  every file in `Game/` against the cook before the package replaces the last one.
  The marker records `release true` and the cook's content and build IDs. It refuses an
  unoptimised engine and `--dev-package`.
- **The editor's Package window** ([Cooking](Cooking.md#in-the-editor)): a Release checkbox
  (never saved), the target, and the project's cook. The cook section shows whether it's a
  release cook, its files, size and IDs, a table of each level's files, shared files and
  size, and **Verify files**. The result says how many files came from the cook and how
  many were left out. `editor --package-release` drives it in tests.
- **The shell's credits** (`Content/Shell/credits.txt`) are a cook root. The first release
  package left them out, so the Credits page would have shown "Made with Shockwave".
- **A packaged game missing its module** tells players to install it again instead of
  telling them to build it with CMake.
- **`cookReport()`** (Packaging/Cook.h): what the window shows, testable without it.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `package_release` | integration | a clean copy in a folder with spaces and non-ASCII letters, packaged from another folder, ships only the cook's files (no Docs, Tools, Source, Tests, notes or settings it never reads); the marker's build ID is the cook's; the packaged player's `--verify` passes; moved and read-only, it plays the full run, then save, quit and resume, from another working folder; a missing module says to install again; a missing library is named; an absolute path and a missing texture each fail the release and keep the last good package with nothing half-made; `--release --dev-package` is refused |
| `package` | unit | a cancelled package stops and keeps the last one; an engine folder with no player says so; the cook report counts levels, shared files and bytes, verifies on request and refuses a damaged manifest |
| `editor_package_release` | gpu | the window's Release package is made from the cook |

### Still unverified

- **Windows and macOS release packages**: packaging needs the modular build off Windows,
  and macOS packages haven't been tried by hand.
- **Clicking a missing asset** in the window doesn't open its authoring reference yet;
  problems are listed in the window's output with their files.
- **Startup work in a release**: the cook proves what ships. S10 still has to show that
  a release never imports, compresses or bakes anything on players' machines.

## S10: Measured performance, part 1

### What was built

- **Budgets in the contract** ([Performance](Performance.md#budgets)): version 2 adds
  `frame_p95_ms`, `frame_p99_ms`, `max_hitch_ms` and `level_load_seconds`, required
  together and kept in order. A version 1 contract still reads and round-trips unchanged;
  only its performance gate fails. The editor's **Add performance budgets** fills them in
  from `target_fps`.
- **Percentiles and the CPU split**: every frame sample records:
  - the fixed simulation ticks;
  - the game's own frame;
  - gathering plus submitting.

  `summarizeFrames` adds p50, p95 and p99, and present percentiles when every frame had
  one. `--benchmark` and **Window > Stats** show them. The benchmark's warm-up second is
  reported on its own instead of being cleared away, with peak process and GPU memory.
- **`player --perf-run`** ([Performance](Performance.md#a-measured-run)): a flow-test
  scenario in real time, with a shown window and the project's pacing.
  - Each frame is filed under loading, warm-up (the first second after each arrival) or
    play, for the whole run and per level. Nothing is deleted.
  - Each level change gets its time and peak memory.
  - `--laps N` repeats the scenario and reports memory growth from the first lap to the
    last.
  - `--cold-cache` empties the pipeline cache first.
  - The report says which budgets the run meets.
- **Machine information** (Platform/SystemInfo.h): OS and version, CPU model, threads and
  RAM on Linux, Windows and macOS, and ARM boards' model names (Jetson). Process memory
  comes from /proc, the working set or Mach task info.
- **Performance records** (Projects/Performance.h): `--perf-record "<machine>"` writes
  `Release/Performance/<machine>.swperf`. It's strict text, with the machine, GPU kind and
  driver, unified memory, build type and cook build ID, settings, cache, play, warm-up, each
  level and load, memory peaks and growth.
- **The performance gate**: passes on a record that meets everything; see
  [the rules](Performance.md#the-performance-gate). It fails on a usable record that misses
  a budget, with every miss listed. It stays unverified when no record counts. Software and
  null renderers, debug builds and stale builds never count.
- **Reactor Run's tour** (`Tests/perf/tour.json`): menu, training, factory and back, for
  laps.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `performance` | unit | nearest-rank percentiles; a hitch shows in the worst frame, not p95; the contract's version 1 and 2 budgets, ranges, order and the null JSON; records round-trip, refuse unknown, repeated, bad and missing keys, and name files safely; every gate rule; the simulation clock at 30, 60, 144 and 1000 fps for 30, 60 and 120 Hz ticks, and no burst of catch-up ticks after a 5 s load |
| `performance_cli` | integration | two laps of Reactor Run's tour, with loading (6 changes), warm-up, play and each level apart, in text and JSON; the record written; the gate unverified without records and with the null-renderer record; passed for the same run as real hardware reports it; failed when p95 is over budget; a damaged record is a warning; flags that don't go together are refused |
| `release`, `release_cli` | unit, integration | version 3 is refused; a version 1 contract fails the performance gate and names the keys |

### Still unverified

- **Any real performance claim**: no record from release hardware exists yet. CI's
  software GPU proves the accounting only.
- **Part 2**:
  - native-resolution presets;
  - the release tier file;
  - pipeline warm-up during loading, and counting pipelines built in play (`pipelines_in_play`
    stays -1, unknown);
  - effect-off memory checks;
  - fallback-tier pictures.
- **Orin**: not run. The record has the fields, and the docs say how to measure there.

## S11: Map rotation, part 1

Multiplayer matches change maps by **controlled session restart with lobby continuity**
(plan section 19.1). See [Networking: Map rotation](Networking.md#map-rotation).

### What was built

- **Wire protocol 2**: `MapPrepare`, `MapReady`, `MapReject`, `MapCommit` and `MapAbort`,
  and reason `MapRotation`. A level is named by its catalog ID, checked for safe characters,
  never by a path. A v1 peer is refused as `ProtocolMismatch`; lobbies advertise
  `sw_protocol 2`. `docs/NetworkProtocol.md` is regenerated.
- **NetworkWorld**: `prepareRotation`, `rotationReady`, `rejectRotation`, `abortRotation`,
  `rotation()`, `rotationHandedOver()` and `nextStart()`. The host commits when everyone is
  ready, drops (or, by setting, calls it off for) whoever isn't at the deadline, refuses
  joins while the map changes, and holds the ready players' slots in the next session. A
  client stamps its rejoin `Hello` with the announced nonce and retries while the host
  starts; a `Challenge` with another nonce is refused. The old session ends with
  `Goodbye(MapRotation)`, which is not a failure and doesn't leave the lobby.
- **LevelHost**: the host's request during network play becomes a rotation (reason
  `OnlineMap`); a client follows each announcement with its own travel; both hold the
  prepared level at the boundary until the commit (the host until its players have left
  for the next map), then start the next level with the next session. A client's own
  request is `AuthorityRequired`; a called-off change ends as `RotationAborted`.
  `game.flow().rotateMap()` goes to the next level of `map_rotation`.
- **OnlineMatch**: a `Rotating` state; the host closes the lobby to joins during the change
  and then advertises the new map (`sw_scene`) and opens it again. A join plays the map the
  lobby is on.
- **Settings and checks**: `map_rotation`, `rotation_ready_seconds`,
  `rotation_drop_unready` in `Network.swnetwork` (schema regenerated). `--check` reports
  rotation levels missing from the catalog or not playing a manifest scene.
- **Editor**: the Network window shows the map epoch, the rotation phase and who's ready.
- **Command line**: `player --net-test --scenario map_rotation [--rotations N]`.

### Evidence

- `player --net-test --scenario map_rotation`, seed 1, 3 clients, 3 rotations: ok on clean
  (longest 583 ms), cafe (967 ms), poor (1567 ms) and hostile (4733 ms); every slot
  reclaimed.
- Hostile, 11 clients, 10 rotations, seed 5: ok, 110 slots reclaimed, longest 5633 ms.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `network_map_rotation` | unit | a player never ready is dropped with `MapNotLoaded` and the rest go on in their slots; with `rotation_drop_unready 0` the change is called off and a later one works; a client that can't load the map is dropped with its reason; joins under way, during preparation and after the commit are refused with `MapRotation`; a newcomer on the next map doesn't take a held slot; host loss before the commit ends the match, after it the client times out; a next session with another nonce is refused; a raw peer's repeated ready and stale reject are ignored and an answer to an unannounced map is refused; real Play sessions through LevelHost: a client can't change maps, the host's request takes everyone to the next level in the same slots; the `map_rotation` scenario on every fault preset |
| `network_wire` | unit | golden bytes of the five messages; level IDs with paths, dots first or spaces are refused; version 2 headers |

### Still unverified

Part 2 (below) covers the player counts, degradation, kicks, outages, re-authentication
and the duplicate-message cases. What part 1 still leaves open is listed there.

## S11: Scale, degradation and match control, part 2

Plan section 19.2: measured matches at 2, 12, 32 and 64 players, queue bounds and update
degradation, kicks, and the outage and re-authentication cases. See
[Networking: Updates under load](Networking.md#updates-under-load) and
[Kicking a player](Networking.md#kicking-a-player).

### What was built

- **Update degradation** (`degrade_updates`, default on): per-client update levels 0 to 2
  (other objects every 2nd, then players every 2nd and others every 4th snapshot), raised
  after 3 pressured snapshots (records held back, queue or transport half full), lowered
  after 3 s calm, at most once a second. Players take turns within the budget too, so
  nobody at the end of a 64-player list starves.
- **Adaptive interpolation delay**: clients draw `max(interpolation_ms, 2 x snapshot gap)`
  behind, and the host's lag compensation follows the shooter's level, so degraded
  clients' shots aren't refused. Grounded samples ignore downward velocity (characters
  were drawn up to 1.6 cm into the floor).
- **Kick**: `NetworkWorld::kick(slot)`, reason `Kicked` (16, wire protocol 2, reference
  regenerated), kept for the rest of the match across map changes. The editor's Network
  window has a Kick button and an Updates column per client. Ready checks and team balance
  stay game rules (Arena already has both).
- **Fix**: a client that couldn't accept a `MapPrepare` for content reasons now sends
  `MapReject(ContentMismatch)`; before, the reject was skipped and the host waited for the
  deadline.
- **Command line**: `player --net-test --scenario match` and `--net-set key=value`.

### Evidence

`player --project Games/Arena --net-test --scenario match --clients N --preset P --net-set
max_players=64`, seed 1, on the CI container (one process, virtual clock; CPU figures are
this machine's, all machines in one process):

| Players | Preset | Host tick mean / p99 | Host up (per player) | Client down / up | Degraded | Held back | Events settle | Late join | Map change | Drawn error | Memory host / client |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | cafe | 0.036 / 0.062 ms | 4.0 KB/s | 4.0 / 7.9 KB/s | 0 | 0 | 0 ms | n/a | 933 ms | 0.5 mm | 0.94 / 1.84 MiB |
| 12 | cafe | 0.079 / 0.124 ms | 213 KB/s (19.4) | 19.2 / 7.8 KB/s | 0 | 0 | 17 ms | 350 ms | 1033 ms | 0.5 mm | 0.95 / 1.84 MiB |
| 32 | cafe | 0.179 / 0.331 ms | 1.59 MB/s (51.3) | 50.8 / 7.8 KB/s | 0 | 0 | 83 ms | 433 ms | 1083 ms | 0.5 mm | 1.01 / 1.84 MiB |
| 64 | cafe | 0.270 / 0.492 ms | 3.39 MB/s (53.8) | 53.3 / 7.8 KB/s | 63 | 391 | 117 ms | 367 ms | 1167 ms | 0.6 mm | 1.23 / 1.84 MiB |
| 2 | hostile | 0.037 / 0.066 ms | 3.9 KB/s | 3.5 / 7.8 KB/s | 0 | 0 | 0 ms | n/a | 3250 ms | 0.5 mm | 0.94 / 1.84 MiB |
| 12 | hostile | 0.063 / 0.092 ms | 198 KB/s (18.0) | 16.7 / 7.8 KB/s | 0 | 0 | 0 ms | 2433 ms | 4266 ms | 0.5 mm | 0.95 / 1.84 MiB |
| 32 | hostile | 0.182 / 0.340 ms | 1.52 MB/s (49.2) | 45.2 / 7.8 KB/s | 0 | 0 | 1117 ms | 3067 ms | 5850 ms | 0.6 mm | 1.01 / 1.84 MiB |
| 64 | hostile | 0.272 / 0.410 ms | 3.20 MB/s (50.7) | 46.5 / 7.8 KB/s | 63 | 252 | 1433 ms | 2417 ms | 6283 ms | 0.6 mm | 1.20 / 1.84 MiB |

Every run: no honest shot refused, every score event reached every client, final error
0.0000 m, nothing left open after stopping. At 64 players every client sits at level 2
and stays within its 64 KiB/s upload budget; a 64-player host needs about 3.4 MB/s
(27 Mbit/s) up, which is a home connection's limit, not the engine's. On hostile, events
arrive late (up to 1.4 s, resent through loss) but none are lost. 64 players with
`degrade_updates 0` also passes, with more records held back each snapshot.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `network_map_rotation` | unit | through a proxy: duplicated `MapPrepare`/`MapCommit` counted once, other content refused with `ContentMismatch` while the host goes on, an old session's snapshot on the new connection refused as `MalformedPacket` with nothing applied; a 4 s outage everyone survives; a 12 s outage (clients `Timeout`, host keeps hosting, they rejoin) and one across a map change (host commits at the deadline without them); a ticket refused on the next map (`AuthenticationFailed`, the rest play on); kicks refused on rejoin and on the next map; 63 clients in slots 2..64, the 65th `ServerFull`, under 8 MiB, and a rotation with all of them; `match` at 2, 12, 32 and 64 players and 64 with degradation off |
| `network_cli` | unit | `--net-test --scenario match` on poor with `--net-set`; bad `--net-set` keys and values exit 2 |
| `network_wire` | unit | reason `Kicked` in range, 17 refused; the reference is current |

### Still unverified (gates for Yianni)

These need real Steam and real machines; nothing here can run them:

- [ ] Two accounts on two machines: host, invite, join, play a round, rotate the map,
      leave, rejoin.
- [ ] Kick from the editor's Network window over Steam; the kicked player can't rejoin
      until the match ends.
- [ ] Pull the client's network cable for 4 s (stays) and for 15 s (times out cleanly, can
      rejoin), during play and during a map change.
- [ ] Three or more real machines on home connections, watching the Updates column: no
      one should sit degraded on a decent line.
- [ ] Render frame times with 12 players over Steam (the figures above are network and
      simulation only).
- **32 and 64 players over real Steam** need that many accounts; the numbers above come
  from the loopback hub.
- **The editor's own Play** still doesn't rotate maps (the player does).

## S12: Diagnostics and crashes, part 1

Plan section 20: log files, crash capture, symbols and the unclean-exit check. See
[Diagnostics](Diagnostics.md).

### What was built

- **Core/Diagnostics.h**: a 64 KiB recent-log ring; a session context the engine keeps
  current (build, project, renderer, GPU and driver, graphics preset, level, epoch,
  travel phase, last save, last error); redaction of the home folder and long hex runs;
  rotating log files that stop at a size limit or a full disk; a session marker that
  tells an unclean exit (and a second running copy).
- **Crash capture**: Linux and macOS signal handlers on their own stack using only
  signal-safe calls on prepared memory; a Windows unhandled-exception filter with a
  minidump and `abort()` caught. Reports carry the context, raw frames, the memory map
  (Linux) and the recent log. The folder is made only when a crash happens.
- **Symbols**: `player --symbolize` names frames from ELF symbol tables. Packages keep
  each binary's unstripped copy by GNU build ID in `<package>-symbols/` before stripping
  (`keep_symbols`, default on, editor checkbox).
- **Player**: `--diagnostics [--json]`, `--symbolize`, `--safe-mode` (lowest tier,
  windowed, no pipeline cache, user graphics file untouched), `--log-file`,
  `--no-log-file`, `--no-crash-handler`; a warning at start after an unclean exit.
- **Editor**: its own log, crash reports and session marker beside its preferences;
  Window > Diagnostics with both programs' sessions and reports, symbolized.
- `crash_program` is the only thing that crashes on request (a test target, never
  packaged).

### Evidence

A segfault in `crash_program`, symbolized:

```text
player crashed: SIGSEGV (11)
  #0  shockwaveTestCrashHere(char const*)  (crash_program +0x4963)
  #1  shockwaveTestCrashOuter(char const*)  (crash_program +0x497f)
  #2  main  (crash_program +0x3dea)
  #3  ??  (libc.so.6 +0x2a1ca)
  #4  __libc_start_main  (libc.so.6 +0x2a28b)
  #5  _start  (crash_program +0x4055)
5 of 6 frames named
```

Packaging Sample kept 29 binaries' symbols beside the package.

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `diagnostics` | unit | ring, context, redaction, log rotation, size limit and full disk, session markers, report order, frames named from the process's own map |
| `diagnostics_cli` | integration | real crashes (segfault, abort, trap) and their reports; symbolization, also from kept symbols after stripping; `--diagnostics` after an unclean exit; game-run logs, rotation and clean exit; safe mode; no ticket in an authentication failure's log |
| `package_cli` | gpu | symbols kept beside a Linux package, by build ID |

### Still unverified

- **Windows and macOS crash reports**: the code compiles in CI, but no crash was run
  there; Windows minidumps need opening in a debugger with the PDBs (the Release build
  makes none yet), and `--symbolize` doesn't name Windows or macOS frames.
- **Line numbers**: only function names; lines need a RelWithDebInfo build.
- **Window > Diagnostics** was built and started, not looked at on a display.
- Part 2 follows.

## S12: Diagnostics and crashes, part 2

Plan section 20 and 20.1: recovering without losing the game, child processes without a
shell, and paths from data files. See [Diagnostics](Diagnostics.md).

### What was built

- **Recovery**: after a run that didn't end cleanly, a system dialog before the window
  (when that run never showed a frame or lost its GPU) offers safe graphics; otherwise the
  main menu opens on a recovery page: Safe graphics for the session, Restore last save,
  Open logs. Nothing is deleted or reset; `--no-recovery-prompt` turns it off.
- **A lost GPU device**: `VK_ERROR_DEVICE_LOST` stops the renderer for good (nothing more
  submitted), the engine tells the application once; the player stops simulating and
  saving and offers a restart with safe graphics; the editor keeps unsaved changes in the
  autosave and closes. `--inject-device-lost N` (refused in a package) tests it.
- **Core/Process.h**: programs started with an argument vector, an explicit environment,
  a working folder, captured output, a timeout and cancel that end the whole process tree
  (process group, a Linux parent-death signal, a Windows job object). The packager, the
  editor's game-code build and Run game use it; the shell-string `popen`/`system` calls
  are gone. `runShellCommand` is a separate, explicit developer call.
- **Core/SafePath.h** and the packager: cook asset IDs and scenes must be portable relative
  paths that stay inside the game; no two package files may differ only by case; links
  are followed deliberately (outside the project stops packaging); staging folders are
  marked by the packager and only marked ones deleted; a cook manifest is size-checked
  before it's read.
- **Release readiness**: the `crash_recovery` gate is measured: failed while the game's
  crash reports are unresolved or symbols aren't kept; a Fatal errors section lists the
  reports with Mark resolved (moved to `Crashes/Resolved`).

### Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `process` | unit | hostile-looking arguments byte for byte and never run; environment, working folder, exit codes, output lines; a missing program; a timeout ending child and grandchild; a cancel; a detached start; Windows command-line encoding |
| `failure_recovery` | integration | injected device loss: exit 1, marker kept, the next run warns; the recovery page's Safe graphics (graphics file untouched) and Restore last save; switched off; after a real crash the page and its report |
| `package` | unit | escaping, absolute and case-colliding cook paths refused; an unmarked staging folder kept, a marked one taken over |

### Still unverified

- **Real device loss**: only injected; a real TDR or GPU reset is a manual check.
- **The dialogs** (safe graphics at start, restart after device loss) need a display;
  headless runs skip them, so they've only been built, not seen.
- **Windows and macOS** child-process code compiles and runs the `process` test in CI;
  nothing beyond that was run on those machines here.
- **Editor**: the device-lost autosave and the Fatal errors section were built, not used
  on a display.
- **Not done**: the editor Console doesn't link diagnostic codes to documentation and
  source files yet (plan section 20's last paragraph).
