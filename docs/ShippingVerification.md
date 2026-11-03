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
| S01-S15 | not started | none |

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

- `user_paths` (S01);
- `level_catalog` (S02);
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
