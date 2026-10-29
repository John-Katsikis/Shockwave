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
| `graphics_fixtures_render` | yes | every scene renders (not black); two `--fixed-step` runs are identical; frame 10 and 40 differ; `--graphics-report` has memory and settings, and every draw has a stable instance key |
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

## Known limits

- Synchronization validation (`VK_LAYER_KHRONOS_validation` with sync checks) was not run
  for G00: the layer isn't installed on the machine that recorded the baseline. The render
  graph's barriers are covered by `render_graph` instead. Run it where the layer exists.
- Baseline images and timings are llvmpipe only.
