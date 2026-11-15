# Release contract

Before a game ships, it has to say what it's promising. `Config/Release.swrelease` is that
promise:

- which game it is;
- which version is being released;
- which platforms and modes it's qualified for;
- how many players a game holds;
- the frame-rate and memory budgets;
- the scenario that plays it from start to finish.

This is batch S00 of [the shipping plan](ShippingImplementation.md). The full key
reference is [ReleaseSchema.md](ReleaseSchema.md), generated with
`player --release-schema docs/ReleaseSchema.md`.

A project without the file is a development project. Nothing changes for it: Play, `--check`
and packaging all work as before. Release qualification simply reports that there's no
contract yet.

## The file

```text
shockwave_release 2
application_id "com.studio.mygame"
release_version "1.0.0"
platform linux-x86_64
platform windows-x86_64
mode offline
mode online
max_players 12
target_fps 60
simulation_hz 60
graphics_tier medium
cpu_budget_mib 4096
gpu_budget_mib 2048
shared_budget_mib 0
loading_budget_mib 512
transition_timeout_seconds 30
acceptance_scenario "Tests/complete_game.json"
frame_p95_ms 16.667
frame_p99_ms 20.833
max_hitch_ms 50
level_load_seconds 10
```

Every key is required, so a release never inherits a default nobody chose. The last four
are the performance budgets, which arrived with version 2 (S10,
[Performance.md](Performance.md)). A version 1 contract, without them, still reads: only its
performance gate fails, saying which keys to add. `platform` and
`mode` repeat, once per value. The parser rejects all of these, giving the file and line:

- unknown keys;
- repeated keys, and the same platform or mode listed twice;
- values out of range, including NaN, infinity and numbers too large to hold;
- a contract version newer than the build reads.

Nothing is clamped. A failed read changes nothing, and saving goes through a temporary
file.

- **`application_id`** names the game permanently. It's reverse-domain style, in lowercase.
  The player's saves and settings are found by it ([UserPaths.md](UserPaths.md)), so it
  never changes, even when the game's display name does.
- **`max_players`** is 1 for an offline-only game. With `mode online`, it's an even number
  from 2 (1v1) to 64 (32v32).
- **`shared_budget_mib`** is for unified memory, where the CPU and GPU share one pool. It
  must hold both budgets. 0 means the memories are separate, as with a discrete GPU.

## Checked against the rest of the project

`player --check` reads the contract alongside the project's other settings. It reports each
disagreement as an error, with file and line:

| Contract | Must agree with |
| --- | --- |
| `simulation_hz` | the project's `tick_rate` |
| `release_version` | the package version (`Config/Package.swpackage`, 1.0.0 without one) |
| `mode online` and `max_players` | `Config/Network.swnetwork`: multiplayer on, with the same `max_players` |
| no `mode online` | the network settings mustn't turn multiplayer on |
| `gpu_budget_mib` | at least the project's `graphics_memory_budget_mb` |

There are also two warnings. Neither fails `--check`, since both are normal while a game is
in development:

- online over Steam AppID 480, Valve's test app;
- an acceptance scenario whose file doesn't exist yet.

## Gates

```sh
build/bin/player --project <file.swproject> --release-info [--json out.json]
```

This prints the contract, the engine build, and every release gate. Each gate is
`passed`, `failed`, `unverified` or `not_applicable`, with the batch that owns it and the
reason. The engine works out every gate from files and evidence it can measure; nobody
ticks a box by hand.

A release is qualified only when every gate that applies has passed. Today these are
measured:

- the contract itself;
- its agreement with the project's settings;
- whether the engine is an optimised build;
- whether an online Steam game has its own AppID;
- where the installed game writes (S01, [UserPaths.md](UserPaths.md));
- the level catalog and the links between its scenes (S02, [Levels.md](Levels.md)).
  `--release-info` loads every level's scene to measure it.
- performance (S10, [Performance.md](Performance.md)): the records in
  `Release/Performance`, from `player --perf-run --perf-record`, against the budgets. Only a
  record from real hardware, on an optimised build of the current cook, counts.
- release_tier (S10, [ReleaseTier.md](ReleaseTier.md)): `Config/ReleaseTier.swtier` names a
  fallback preset and the mandatory effects, and the presets and project defaults provide them.
- crash_recovery (S12, [Diagnostics.md](Diagnostics.md)): no unresolved crash reports, and
  packages keep their symbols.
- each `platform:<name>` (S13, [below](#platforms)): the install records in
  `Release/Installs`, from `player --verify-install --install-record`.

The gates for later batches stay `unverified` until their evidence exists. That covers
level travel, saves, settings, cooking and the online lifecycle. A platform stays
`unverified` until a record also proves an update and a reinstall, which S13 part 2 adds.
So for now no project qualifies, and the report says exactly why.

Exit codes follow the plan's convention:

- 0: the report was written, whether or not the project qualifies;
- 1: the contract has errors;
- 2: the command line is wrong, for example `--release-info` combined with another mode.

The JSON report (format 1) carries every gate and every diagnostic. Each diagnostic has a
stable `code` (listed in ReleaseSchema.md), a `severity`, a `file`, a `line`, an `object`
(the key) and a `message`. Unknown values are `null`, never a made-up 0.

## Platforms

Each platform the contract names has its own gate, and a compile pass doesn't count: the
evidence is the game installed on that platform and played there. `player --verify-install`
gathers it:

```sh
build/bin/player --verify-install Saved/Packages/SpaceAce-1.2.0-linux-x86_64 \
    --flow-test Tests/flow/save-and-quit.json --flow-test Tests/flow/continue.json \
    --update-from Saved/Packages/SpaceAce-1.1.0-linux-x86_64 \
    --install-record "Yianni's desktop" --project Games/SpaceAce
```

It installs the package the hard way, then plays it. With `--update-from <older package>`
(same platform, at least two scenarios), it first installs and plays the older version the
same way (**install previous**, then the first scenario on it, so there's a save to carry),
then installs this one over it:

1. **install**: hashes every file in the package (and its `.tar.gz`, when there is one),
   copies it into a read-only folder whose path has spaces and non-ASCII letters, and checks
   the copy is exact.
2. **check**: starts the program the package's marker names (only that program, never
   anything a report or file suggests) from another folder, with a minimal environment: a
   fresh home folder, `PATH=/usr/bin:/bin`, no `LD_LIBRARY_PATH` and none of the engine's
   settings. Only display, audio and Vulkan driver variables pass through. The game's own
   `--check` must pass.
3. **scenario**: plays each `--flow-test` scenario in order with the same player data, so
   one can save and quit and the next continue. Without any, it plays the contract's
   `acceptance_scenario`. Each report must come from this package's build.
4. **user data**: the player's saves and settings landed in the fresh home's per-user
   folders, and the install folder is still byte for byte as it was. As root, where the
   system doesn't enforce read-only, this check is what proves it.
5. **update**: every save the older version left is still there and the newer version
   continued it. A save the newer version wrote at a higher save schema must have its old
   file kept beside it, byte for byte (see [Save games](SaveGames.md#updates)). The two
   packages must be different builds. Without `--update-from` this step and the next are
   recorded as skipped.
6. **downgrade**: the older version is put back and runs `--check` and `--saves` over the
   newer data. It must not change or remove a single save file, and when the save schema
   rose it must say those saves are from a newer version. Continue then uses the copies kept
   from before the update.
7. **reinstall**: the install folder is removed and the package installed again at another
   path. The game must still find every save (by its application ID, not where it was
   installed) and load each one.

`--renderer null` plays without a GPU (CI does this). `--install-dir folder` installs there
instead of a temporary folder; it must be empty or one this tool made, and it's removed
afterwards unless you add `--keep-install`. `--verbose` shows the game's own output and
`--json report.json` writes every step. The exit code is 0 when every step passed.

`--install-record "<machine>"` keeps what it saw in
`Release/Installs/<machine>-<platform>.swinstall` (or in `--output folder`): the machine
(OS, distribution, C library, CPU), how the game drew (renderer, GPU and driver, from the
game's own report), the package (version, build type, the cook's build and content IDs,
the SHA-256 of its files and archive) and every step with its result. Unknown keys are
errors, and a record whose steps failed can't say it passed.

A platform's gate passes on a record from a release package of the current, optimised
build, installed the hard way, played with a real GPU (not the null or a software renderer),
with every step passed, an update, a downgrade and a reinstall included. When the newest record of the
current build failed, the gate fails and names the step. Otherwise it stays `unverified`
and says what's missing. A record on one machine never stands in for another platform or
architecture.

## In the editor

**Project > Release readiness** edits the contract and shows every gate, worked out the
same way as `--release-info`. A project without a contract gets a **Create release
contract** button, which fills one in from the project's own settings: its package version,
this platform, its network player count and its tick rate, with performance budgets
worked out from the target frame rate. A version 1 contract gets an **Add performance
budgets** button instead of those rows. Nothing is written until you press **Save**. Save is disabled while the contract has errors, and during Play.

The window follows the file: when a tool or another editor changes it, and nothing is being
edited, it shows the new one.

Its **Platforms** section lists each platform the contract names with the install records
behind its gate: the machine, the package and its hashes, and what the record proves or
lacks. Hover a row to see every step.

**Help > Shipping implementation guide** opens the plan itself.

## Code

- `Projects/ReleaseSettings.h`: the format, its one descriptor table, the rules and the
  gates. Projects depends only on Core. The caller reads the other settings into
  `ReleaseInputs`; Player and the editor both do this.
- `Projects/Diagnostic.h`: the machine-readable diagnostic with a stable code.
- `Projects/InstallRecord.h`: install records and the platform gates; the player's
  `PlayerInstall.cpp` is `--verify-install`.
- Tests:
  - `release` (`Tests/ReleaseTest.cpp`) covers parsing, ranges, rules and gates;
  - `release_cli` (`Tests/ReleaseCheck.cmake`) covers `--check`, `--release-info` and
    `--release-schema` on a copy of Arena;
  - `install_records` (`Tests/InstallRecordTest.cpp`) covers records and the platform gates;
  - `installed_game_flow` (`Tests/InstalledGameCheck.cmake`) verifies a release package of
    Reactor Run installed the hard way.
