# Graphics verification

How renderer changes are measured: a fixed set of scenes, a recorded baseline, and
commands that compare a new build against it. Batch G00 of
[GraphicsImplementation.md](GraphicsImplementation.md) set this up; every later G and A
batch adds its evidence here.

## Fixtures

`Tests/fixtures/graphics` is a small project (game module `sample`) built for looking at
rendering, not for playing. Every scene is deterministic: motion comes from circuits that
rotate at a fixed rate, and `player --fixed-step` advances exactly one tick per frame, so
frame 30 is the same moment on every run.

| Scene | Cameras | What it's for |
|---|---|---|
| thin-geometry | Main Camera, Grazing | fences, wires and a fine checker floor at grazing angles (aliasing, shimmer) |
| room-doorway | Inside, Outside | a closed room with one opening (light leaks, AO, probes) |
| material-spheres | Main Camera | roughness/metallic rows and textured spheres (material fidelity) |
| transparency | Main Camera, Side | blended and alpha-tested panels over a checker floor |
| lights-overlap | Main Camera | many overlapping point and spot lights (clustered lighting) |
| motion | Orbit camera, Fixed | a moving camera, spinning, orbiting and skinned objects (motion vectors, TAA) |
| decal-wall | Main Camera | a wall and floor for projected decals |
| dense-occluder | Main Camera, Overhead | 1215 entities behind a wall (instancing, HZB culling) |

`Tools/generate.py` writes the textures, glTF models and the dense scene with only the
Python standard library; the other scenes are written by hand. `Content/LICENSE.md` covers
the content.

## Commands

```sh
# Look through a named camera, reproducibly:
build/bin/player --project Tests/fixtures/graphics/Graphics.swproject --no-user-graphics \
    --scene Tests/fixtures/graphics/Scenes/motion.swscene --camera "Fixed" \
    --fixed-step --frames 30 --screenshot /tmp/motion.png

# Numbers for an image, and how far apart two are (exit 1 past the RMSE tolerance):
build/bin/player --image-stats /tmp/motion.png
build/bin/player --image-diff reference.png candidate.png --tolerance 1.0 [--json out.json]

# Every scene, camera, size and render scale, plus paced and unpaced benchmarks:
python3 Tests/fixtures/graphics/Tools/capture.py --out /tmp/capture
# ...compared with the committed baseline:
python3 Tests/fixtures/graphics/Tools/capture.py --out /tmp/capture --compare
# ...or recorded as the new baseline (only for an intended change; say why in the commit):
python3 Tests/fixtures/graphics/Tools/capture.py --out /tmp/capture --record

# What the GPU supports, each setting as asked for and as running, GPU memory by category:
build/bin/player --project Tests/fixtures/graphics/Graphics.swproject --graphics-report [--json out.json]
```

The editor shows the same report in **Stats**: *Graphics settings* (opens by itself when a
setting fell back, with the reason in the tooltip), *GPU capabilities* and *GPU memory*.
`player --benchmark --json` now includes `gpu_memory_bytes`.

On a machine without a display, run these under `xvfb-run -a` with a software Vulkan
driver (llvmpipe), or set `SDL_VIDEO_DRIVER=offscreen`.

## Tests

| ctest | Needs a GPU | Checks |
|---|---|---|
| `render_graph` | no | barriers for mip chains, compute to fragment/vertex/indirect, reads in several stages, read-only depth, history ping-pong; contradictory uses in one pass are refused |
| `graphics_fixtures` | no | every fixture scene passes `--check` |
| `graphics_fixtures_render` | yes | every scene renders (not black); two `--fixed-step` runs are identical; frame 10 and 40 differ; `--graphics-report` has memory and settings, and every draw has a stable instance key; FXAA brings thin-geometry closer to a 2x supersampled image, touches under 10% of pixels and keeps the brightness; `anti_aliasing none` matches the default; bloom and FXAA off leave no pass and no memory, each bloom tier has both |
| `graphics` | no | every settings key round-trips; `taa` and out-of-range `bloom_scale` are refused; presets set FXAA and bloom and keep bloom strength; hand-tuned settings read as Custom; `status()` |
| `graphics_cli` | no | `--graphics-set`, `--anti-aliasing`, refused values with file and line, `--graphics-info --json` |
| `graphics_report_null` | no (runs under Xvfb, so Linux only, like the other headless play tests) | the report works on the null renderer |
| `network_session` | no | includes the fixed-step clock: one tick a frame whatever the real time |

## Baseline (G00)

`Tests/fixtures/graphics/Reference` holds `baseline.json` (every image's statistics and
SHA-256, and the benchmarks) and the 1280x720, render scale 1, first-camera image of each
scene. It was captured from commit 656d447, before any G00 renderer change, on llvmpipe
(LLVM 20.1.2, Mesa 25.2.8, Vulkan 1.4). Software and hardware drivers rasterize
differently: compare only captures made on the same driver, which `baseline.json` records.

Benchmarks at 1280x720 on llvmpipe (4 CPU cores), average frame time:

| Scene | Unpaced | Paced at 60 fps | Draws |
|---|---|---|---|
| thin-geometry | 319 ms | 336 ms | 20 |
| room-doorway | 162 ms | 165 ms | 11 |
| material-spheres | 124 ms | 125 ms | 16 |
| transparency | 395 ms | 391 ms | 6 |
| lights-overlap | 156 ms | 141 ms | 3 |
| motion | 106 ms | 110 ms | 4 |
| decal-wall | 117 ms | 106 ms | 3 |
| dense-occluder | 163 ms | 173 ms | 1211 |

These say how a software rasterizer copes, not how a GPU will: use them to spot large
regressions on this machine, and record hardware numbers alongside when a GPU is available.

## G00 groundwork

- **Render graph** (`RHI/RenderGraph.h`): storage image reads and writes, read-only and
  sampled depth, buffer uses (compute, vertex shader, index, indirect, transfer), and
  per-mip/per-layer tracking. A pass that declares two layouts for the same subresource
  throws. Barriers for consecutive mips and layers are merged. The skinning pass's output
  is now a graph buffer, so its barrier to the shadow and scene passes comes from the graph.
- **Identity**: `RenderViewId` (one per camera view: player, sandbox, editor fly camera and
  editor scene camera) and `RenderInstanceKey` (world lifetime, entity, generation, model
  part, mesh generation). Every draw from `renderWorld` carries one; Stats and
  `--graphics-report` count draws without one.
- **Capabilities and fallbacks**: `Renderer::capabilities()`, `effectiveSettings()` (a
  fallback is logged once each time it changes, never silently) and `memoryReport()`.
- **Plugin ABI** is now 3: renderer plugins must be rebuilt.

Result: `capture.py --compare` after these changes found all 52 images pixel-identical to
the baseline (8 by `--image-diff`, RMSE 0; the other 44 by SHA-256), so G00 changed no
output, as the plan requires.

## A00 + G01: settings table and FXAA

- **One table** (`graphicsSettingInfo()`) describes every setting: files, the schema,
  `--graphics-set key=value`, editor Preferences and Arena's menu all read it. Presets and
  "Custom" come from its `preset` flag.
- **Off means free.** Bloom off and anti-aliasing `none` add no frame-graph pass and hold no
  GPU memory (checked by `graphics_fixtures_render`). Bloom memory at 1280x720: off 0, low
  0.59 MiB, medium and high 2.35 MiB; the FXAA input image is 3.52 MiB.
- **No GPU idle on a change.** Bloom, FXAA and shadow-map images are rebuilt at the frame
  boundary into spare bindless slots; the old ones are freed once the frames using them
  finish.
- **FXAA** runs after tone mapping, before game UI, on display-encoded values (read and
  written through UNORM views, so nothing is encoded twice). On thin-geometry (640x360)
  against a render-scale-2 reference: RMSE 4.95 without, 4.84 with; 1.1% of pixels change
  and mean luma moves 0.14 of 255. On edges alone (1280x720) the error falls from 11.03 to
  10.43. Cost on llvmpipe at 1280x720: about 40-60 ms on a 550-600 ms frame; measure on a
  GPU before drawing conclusions.
- **Plugin ABI** is now 4.

Result: `capture.py --compare` found all 52 images identical to the G00 baseline: the
defaults (`anti_aliasing none`, `bloom_quality high`, `bloom_scale 1`) reproduce the old
output exactly.

## Known limits

- Anisotropic filtering and texture quality, listed with A00's settings, arrive with A01.
- The editor grid is drawn in the scene pass, so FXAA smooths it too.
- Images waiting to be freed after a change aren't counted in the memory report.

- Synchronization validation (`VK_LAYER_KHRONOS_validation` with sync checks) was not run
  for G00: the layer isn't installed on the machine that recorded the baseline. The render
  graph's barriers are covered by `render_graph` instead. Run it where the layer exists.
- Baseline images and timings are llvmpipe only.
