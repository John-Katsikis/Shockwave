# The graphics release tier

What a release draws with, written down and checked (S10). Three things live here:

- **Presets at native resolution.** No preset lowers the resolution.
- **The release tier file**, `Config/ReleaseTier.swtier`: the lowest preset the game
  supports and the effects its look can't do without.
- **Pipeline warm-up**, so no shader pipeline is compiled in the middle of play, and a
  count that proves it.

Measuring speed is the other half of S10: see [Performance](Performance.md).

## Presets draw at native resolution

`low`, `medium`, `high` and `ultra` all set `render_scale 1`. Low and Medium used to draw
at 67% and 85% of the output and stretch the picture, which read as pixelated. They now
save their time in shadows, ambient occlusion, bloom, fog, particles and textures.

`render_scale` is still a setting. A player, a project's `Config/Graphics.swgraphics` or
`--graphics-set render_scale=0.75` can lower it; no preset depends on it. Choosing a
preset sets it back to 1, like every other quality setting a preset owns.

## Config/ReleaseTier.swtier

Optional. A project without it has decided nothing, and nothing changes.

```
swtier 1
# The lowest preset the game supports.
fallback low

# An effect the art direction needs, the lowest value that will do, and why.
mandatory shadow_quality low "the sun's shadows are how the rooms read as lit spaces"

# Optional is the default; listing one only records that it was considered.
optional bloom_quality
```

| Line | Meaning |
| --- | --- |
| `swtier 1` | The first line. The version this build reads. |
| `fallback <preset>` | Required, once. `low`, `medium`, `high` or `ultra`: the lowest preset the release supports. Minimum-tier pictures and the fallback column use it. |
| `mandatory <setting> <value> "<reason>"` | The setting must be at least that value on every supported tier. The reason is required: an effect isn't mandatory because another engine has it. |
| `optional <setting>` | Ships, and players may turn it down or off. This is what an unlisted setting is. |

A setting is any graphics setting with named quality values
([Graphics settings](GraphicsSchema.md)): `shadow_quality`, `ambient_occlusion`,
`bloom_quality`, `fog_quality`, `baked_lighting` and so on. Numbers (`render_scale`),
display settings (`vsync`) and settings that never change the picture (`instancing`,
`occlusion_culling`, `light_culling`) aren't tier decisions and are refused.

Values rank in the order the schema lists them, lowest first; for `true`/`false`
settings, `false` is the lowest. A minimum that is the lowest value is refused, because
that's `optional`.

The file is strict, like every text format here: an unknown word, setting or value is an
error with its line.

### What the check compares

`player --check --release`, `player --release-tier` and the editor all run the same
rules:

| Code | Meaning |
| --- | --- |
| `ReleaseTierSyntax` | The file doesn't read. |
| `ReleaseTierFallbackTooLow` | The fallback preset draws a mandatory effect below its minimum. Raise the fallback or lower the minimum. |
| `ReleaseTierDefaultTooLow` | The project's own defaults (`Config/Graphics.swgraphics`) draw a mandatory effect below its minimum. |
| `ReleaseTierNoDefaults` | The project's graphics settings don't read, so nothing can be compared. |

The release gate `release_tier` ([Release](Release.md)) is `unverified` without the file,
`failed` on any of these, and `failed` when the contract's `graphics_tier` (the preset
the minimum hardware is qualified at) is below the fallback. Otherwise it passes.

The tier is a recorded decision that tools check. It doesn't stop a player from turning
a mandatory effect off in a menu, and the memory budget
([Graphics budget](GraphicsBudget.md)) may still turn one down on a GPU that's too small.

### Commands

```
player --project <game> --release-tier [--json file]
```

Prints the summary, then every tier setting with its use, its minimum, and its value at
the fallback and at each preset. Exit 1 when the tier has errors, 0 otherwise (including
when there's no file).

```
player --project <game> --graphics-preset fallback ...
```

Draws with the fallback tier: the fallback preset over the project's own defaults. It
goes anywhere `--graphics-preset low` does (`--screenshot`, `--benchmark`,
`--perf-run`). It needs a tier file that reads.

### In the editor

Project > Release readiness shows a **Graphics tier** section: the fallback, each
mandatory effect with its minimum, what the fallback draws and why, and any problem.
**Create graphics tier** writes a file whose fallback is `low`. The file has reasons in
it, so it's edited as text. Below it, **Performance records** lists each machine's run:
what it drew at, p50, p95 and p99, pipelines built in play, and whether it meets the
contract.

Window > Stats shows the contract's p95 and p99 budgets beside the measured ones, and
how many pipelines have been built.

## Pipeline warm-up

Almost every pipeline is built when the renderer starts. Two groups weren't: glass
(4 pipelines) and skin (6) were built the first time such a material was drawn, which is
a stall in the middle of play.

Now, when a glass or skin material loads, its pipelines are built as the next frame
begins. With staged level travel that frame is the loading screen's. Turning
`transmission_quality` or `skin_quality` on in a menu builds them there too.

An effect that's off still gets no pipelines: warm-up only builds what the current
settings would draw.

| Switch | Effect |
| --- | --- |
| project key `pipeline_warmup false` | Off for the game: pipelines are built at first draw, as before. |
| `player --no-pipeline-warmup` | Off for one run. |

The picture is identical either way; only when the compile happens differs.

### Counting

The renderer counts every pipeline it creates.

- `player --graphics-report` prints `pipelines: N built, W of them by warm-up while
  loading (on), D while drawing`, and its JSON has
  `"pipelines": {"created", "warmed", "warmup", "while_drawing"}`. `while_drawing` is
  what a draw had to wait for.
- `player --perf-run` counts the pipelines built off the loading screen (in warm-up and
  play frames) and writes it to the record as `pipelines_in_play`. The performance gate
  fails a record with any. The null renderer has no pipelines, so its records say
  unknown.
- Window > Stats shows the totals while you work.

Driver pipeline caches ([Drawing](Drawing.md)) still make each build cheaper on a second
start. They belong to one machine and driver, and are never shipped.

## Off really means off

Every optional effect has an off path with no pass and no memory. Starting with an
effect off is checked by the graphics fixtures. Turning one off while running is checked
by `effects_off_vulkan`: bloom, FXAA, TAA, ambient occlusion and shadows are each turned
on and off again, and GPU memory must return exactly to where it started.

## Tests

| Test | Label | What it proves |
| --- | --- | --- |
| `release_tier` | unit | The file format, value ranks, the checks, the gate, and every preset at `render_scale 1`. |
| `release_tier_cli` | integration | `--release-tier`, `--check --release` and `--graphics-preset fallback` on a copy of Reactor Run, with good, unmet and unreadable tiers, and the `pipeline_warmup` key. |
| `effects_off_vulkan` | gpu | Effects turned off mid-run give back their pass and memory. |
| `graphics_release_tier_render` | gpu | Warm-up builds glass and skin pipelines before any draw; without it a draw builds them; off builds none; the pixels are identical. Reactor Run's factory at the fallback tier isn't empty, is repeatable, has its mandatory shadows and differs from High. |

CI runs these on a software GPU. That proves the logic, never the speed.

## What's left out

- Stored reference pictures for the fallback tier. Pictures only compare across runs on
  the driver that recorded them, so CI checks properties of the picture instead.
- Orin-specific fields (power mode, clocks) in performance records. `unified_memory` is
  recorded; the rest waits for a device to test on.
- Nothing enforces a mandatory effect at run time; see above.
