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
shockwave_release 1
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
```

Every key is required, so a release never inherits a default nobody chose. `platform` and
`mode` repeat, once per value. The parser rejects all of these, giving the file and line:

- unknown keys;
- repeated keys, and the same platform or mode listed twice;
- values out of range, including NaN, infinity and numbers too large to hold;
- a contract version newer than the build reads.

Nothing is clamped. A failed read changes nothing, and saving goes through a temporary
file.

- **`application_id`** names the game permanently. It's reverse-domain style, in lowercase.
  Saves and settings will be found by it (S01), so it never changes, even when the game's
  display name does.
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
- whether an online Steam game has its own AppID.

The gates for later batches stay `unverified` until their evidence exists. That covers user
folders, the level catalog, level travel, saves, settings, cooking, performance, online
lifecycle, crash recovery and each platform. So for now no project qualifies, and the
report says exactly why.

Exit codes follow the plan's convention:

- 0: the report was written, whether or not the project qualifies;
- 1: the contract has errors;
- 2: the command line is wrong, for example `--release-info` combined with another mode.

The JSON report (format 1) carries every gate and every diagnostic. Each diagnostic has a
stable `code` (listed in ReleaseSchema.md), a `severity`, a `file`, a `line`, an `object`
(the key) and a `message`. Unknown values are `null`, never a made-up 0.

## In the editor

**Project > Release readiness** edits the contract and shows every gate, worked out the
same way as `--release-info`. A project without a contract gets a **Create release
contract** button, which fills one in from the project's own settings: its package version,
this platform, its network player count and its tick rate. Nothing is written until you
press **Save**. Save is disabled while the contract has errors, and during Play.

The window follows the file: when a tool or another editor changes it, and nothing is being
edited, it shows the new one.

**Help > Shipping implementation guide** opens the plan itself.

## Code

- `Projects/ReleaseSettings.h`: the format, its one descriptor table, the rules and the
  gates. Projects depends only on Core. The caller reads the other settings into
  `ReleaseInputs`; Player and the editor both do this.
- `Projects/Diagnostic.h`: the machine-readable diagnostic with a stable code.
- Tests:
  - `release` (`Tests/ReleaseTest.cpp`) covers parsing, ranges, rules and gates;
  - `release_cli` (`Tests/ReleaseCheck.cmake`) covers `--check`, `--release-info` and
    `--release-schema` on a copy of Arena.
