# Measuring performance for a release

"It runs at 60 fps" is only true for one machine, one build and one set of settings. Before
a game ships, Shockwave wants that claim measured and written down: on which hardware, at
which graphics tier, with which build, and how the slowest frames did, not just the
average. This is batch S10 of [the shipping plan](ShippingImplementation.md).

There are three parts:

- **Budgets.** The [release contract](Release.md) states how long frames and level changes
  may take.
- **A measured run.** `player --perf-run` plays a scenario in real time and reports frame
  times, loading and memory.
- **A record.** `--perf-record` keeps the run as evidence, and the release's `performance`
  gate reads it.

## Budgets

A version 2 contract (`shockwave_release 2`) adds four keys:

```text
target_fps 60
frame_p95_ms 16.667
frame_p99_ms 20.833
max_hitch_ms 50
level_load_seconds 10
```

| Key | Means |
| --- | --- |
| `frame_p95_ms` | 95% of the frames of play take at most this long |
| `frame_p99_ms` | 99% of them take at most this long |
| `max_hitch_ms` | no single frame of play takes longer than this |
| `level_load_seconds` | no level change takes longer than this, cold or warm |

The frame budget for a target rate is 1000 / `target_fps` ms: 16.667 at 60, 8.333 at 120.
Half the frames must fit in it (p50), as well as the percentiles above.

Percentiles are what players feel. An average of 60 fps can hide a stutter every few
seconds; p99 and the worst frame can't.

The rules between keys:

- `frame_p95_ms` <= `frame_p99_ms` <= `max_hitch_ms`;
- `level_load_seconds` <= `transition_timeout_seconds`.

A version 1 contract still reads. It just has no budgets, so the performance gate fails
and says which keys to add. In the editor, **Project > Release readiness** has an **Add
performance budgets** button. It fills them in from the target frame rate: p95 at the frame
budget, p99 at 1.25 times it, the worst frame at 3 times it, and a level change in 10 s.
Nothing is saved until you press Save, and saving makes the contract version 2.

## A measured run

```sh
build/bin/player --project Games/ShippingDemo --perf-run Games/ShippingDemo/Tests/perf/tour.json \
    --graphics-preset medium --laps 3 --json perf.json
```

It plays a [flow-test scenario](FlowTest.md) in real time. Unlike `--flow-test`, the
window is shown and the clock is real, with the project's vsync and frame limit, because
that's what a player gets. `--vsync`, `--fps` and `--size` change them for the run.

Every frame is put in one of three places. Nothing is thrown away:

- **Loading:** while a level change is under way.
- **Warm-up:** the first second after each arrival, while first uploads and caches settle.
  It's reported on its own, so a slow start is visible but doesn't count as play.
- **Play:** everything else, for the whole run and for each level.

```text
perf run: Tests/perf/tour.json, 3 laps, warm cache
machine:  (unnamed); Linux 6.8.0-45-generic; AMD Ryzen 7 5800X (16 threads, 32005 MiB)
gpu:      NVIDIA GeForce RTX 3070 (dedicated, NVIDIA 580.95), vulkan renderer
build:    Release, build 28478d1285cbc8bf
drawn:    1920x1080 at medium (render scale 1.00), vsync off, frame limit none
timing:   intervals between frame starts (the driver doesn't report presents)
play:     2840 frames, p50 4.10 ms, p95 5.02 ms, p99 6.31 ms, worst 11.80 ms
  level menu             610 frames, ...
  level training         1120 frames, ...
  level factory          1110 frames, ...
warm-up:  540 frames, ... (the first 1 s of each level, apart)
loading:  9 changes, 27 frames, ... on the loading screen
  load training          0.40 s, peak 674 MiB
  ...
memory:   peak 1380 MiB process, 812 MiB GPU, +2 MiB from the first lap's end to the last
budgets:  every budget in Config/Release.swrelease met
```

- **timing** says what the frame times are. They're the intervals between frames reaching
  the screen when the driver reports them, otherwise between frame starts.
- **load** gives each level change's time, from the request being accepted to the new
  level playing, and the most memory the process held during it.
- **memory** gives the peaks over the run. With laps, it also says how much memory grew
  from the end of the first lap to the end of the last.
- **budgets** says what the performance gate would say of this run, when the contract has
  budgets.

The JSON (`--json file`, format 1) has the same fields, plus the flow test's own report
under `flow`. The exit code is the flow test's: 0 when the scenario passed, 1 when a step
failed, 2 for a wrong command line. Meeting the budgets is the gate's call, not the exit
code's.

### Laps

`--laps N` (1 to 100) plays the scenario N times, with its final `quit` only at the end. The
scenario has to come back to where it started: Reactor Run's tour goes menu, training,
factory, then back to the menu.

A level rotation whose memory keeps growing has something it doesn't free. The gate allows
a lap to grow by 5% of `cpu_budget_mib`, and at least 32 MiB.

### Cold and warm caches

`--cold-cache` empties the pipeline cache before the run. The first frames then build
every pipeline from scratch, as on a player's first launch. Record a cold run and a warm
run: the cold one shows what a new player sees, and the warm one what everyone sees after
that. The record says which it was. (Compressed textures ship in the package, so a
player's first launch doesn't compress them.)

`--benchmark` takes `--cold-cache` too.

## Records

```sh
build/bin/player --project Games/ShippingDemo --perf-run Games/ShippingDemo/Tests/perf/tour.json \
    --graphics-preset medium --laps 3 --perf-record "Minimum PC"
```

This writes `Release/Performance/minimum-pc.swperf` in the project. The name is the
machine's, as people know it. `--output folder` writes the record there instead.

A record is only written when the scenario passed: a run that failed measured something
else.

A record holds:

- the machine: OS, CPU, threads, RAM, GPU, its kind (dedicated, integrated, software...),
  driver and memory;
- whether memory is unified, shared by the CPU and GPU, as on integrated GPUs and Jetson
  Orin;
- the build: its type and the cook's build ID;
- the settings: renderer, the preset they match (or `custom`), the size drawn at, render
  scale, vsync, frame limit and cache;
- the results: play, warm-up, every level and every load, memory peaks and growth.

Records are text, one key per line. They're strict: an unknown key, a repeated key or a bad
value is a warning with file and line, and that record is left out. Commit the records:
they're the evidence for the release.

## The performance gate

`player --release-info` and the editor's Release readiness window read every record in
`Release/Performance`. The gate passes when one record meets everything:

- a real GPU drew it, with the Vulkan renderer. The null renderer and software GPUs like
  llvmpipe never count;
- an optimised build (Release, RelWithDebInfo or MinSizeRel);
- the game's current build, when it has a cook. A record from an older build is stale;
- at least the contract's `graphics_tier`, at 1280x720 or more, with no frame limit under
  `target_fps`;
- p50 within the frame budget, and p95, p99 and the worst frame within theirs;
- every level change within `level_load_seconds`;
- memory within `cpu_budget_mib` and `gpu_budget_mib`, and within `shared_budget_mib`
  together when memory is shared;
- memory growth over the laps within the allowance;
- no pipelines built during play (warm-up and play frames; the loading screen may build them). A record from before the count existed, or from the null renderer, says unknown,
  which doesn't fail.

| Gate | When |
| --- | --- |
| `passed` | a record meets everything; the reason names it and its numbers |
| `failed` | the contract has no budgets, or a usable record (real GPU, optimised, current build) misses a budget; the reason lists every miss |
| `unverified` | no records, or none that count yet (only software renderers, debug builds or stale builds) |

Measure with the preset you promise. Settings that match no preset are `custom`, which
doesn't count as a tier: use `--graphics-preset` for the run.

## What CI proves, and what it doesn't

CI runs on shared machines with a software GPU, so its numbers say nothing about how fast
the game is. The tests prove the accounting instead:

- `performance` (unit):
  - percentiles;
  - the contract's budgets, version 1 and 2;
  - records round-tripping and refusing what they don't know;
  - every rule of the gate;
  - the simulation clock at 30, 60, 144 and 1000 fps, for 30, 60 and 120 Hz ticks.
- `performance_cli` (integration):
  - two laps of Reactor Run's tour under the null renderer, with loading, warm-up and play
    apart;
  - the record it writes, and the gate refusing it;
  - the same record as real hardware would write it passing, then failing when p95 is
    over budget.

A performance claim needs a record from the release hardware itself.

## Jetson Orin and other unified-memory machines

Check that the game runs there before measuring anything. Then record:

- the power mode (`nvpmodel -q`) and clocks (`jetson_clocks --show`), in the machine's name
  or beside the record;
- sustained play and travel, with `--laps`, while watching memory pressure and swap.

A record on a unified-memory machine marks `unified_memory true`. Give the contract a
`shared_budget_mib` there, since the CPU and GPU budgets come out of one pool. Report it
separately from desktop minimum requirements, and never promise "any PC": publish the
hardware, driver and capability floor you measured on.

## The benchmark

`player --benchmark` ([FramePacing.md](FramePacing.md#benchmark-from-the-command-line))
measures one scene. It reports:

- p50, p95 and p99;
- the CPU split into simulation, game and submission;
- the warm-up second on its own line;
- peak process and GPU memory, and whether the cache was cold.

The editor's **Window > Stats** shows the same percentiles and CPU parts for the last
frames.
