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
| S06-S15 | not started | none |

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

Full local suite: 97 of 97 pass on Linux, gpu included. Five long GPU tests failed once when the player was rebuilt while they ran; all five passed when run again on their own.

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
