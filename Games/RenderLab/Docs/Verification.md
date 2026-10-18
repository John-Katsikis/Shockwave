# Initial verification — 18 October 2026

Built `player`, `editor` and `renderlab_import` with the optional `renderlab` game module.
All 32 CTest checks pass, including the graphics API/menu and Render Lab checks.
The project's five scenes and 16 prefabs pass `player --check`. The conversion test round-trips
known geometry through the engine importer and standard glTF. A gun's binary vertex and
index buffers were decoded back to readable JSON with `InspectGltf.py --dump`.

Gallery and close-up materials were rendered on an NVIDIA GeForce RTX 3080. The normals
view was also captured and inspected. Textures, metallic highlights, shadows and model
placement are visible. Screenshots and raw reports are in the project's ignored `Saved/`
folder. The editor also loads the project through the normal scene/content UI.

## Measured workload results

1280×720 output, static camera, VSync off, uncapped, low latency off, 2048-pixel maximum
source texture dimension, BC7 compression, four generated LODs. Each run requested five
seconds after warmup. **CMake build type was unset, not Release.** These are initial
engineering observations on this checkout, not production performance or minimum-spec
claims. The engine's rolling 2,048-frame window can cover less than the requested duration.

| Scene | Profile | Average FPS | 1% low FPS | GPU ms |
|---|---|---:|---:|---:|
| gallery | high | 432.8 | 181.6 | 0.39 |
| stress-64 | high | 149.6 | 105.8 | 0.60 |
| stress-256 | high | 85.7 | 60.3 | 1.69 |
| stress-1024 | high | 32.1 | 23.3 | 4.91 |
| stress-256 | low | 76.2 | 39.9 | 1.13 |
| stress-1024 | low | 31.3 | 21.9 | 4.99 |
| stress-256 | full-detail | 72.3 | 33.7 | 4.74 |
| stress-1024 | full-detail | 32.7 | 24.5 | 17.83 |

High uses native render scale, high shadows and one-pixel LOD error. Low uses 67% scale,
low shadows and two-pixel LOD error. Full-detail uses High with LOD error zero.

## What this exposes

- The largest grid has approximately 48 million source mesh triangles before LOD. With
  LOD disabled, GPU cost rises substantially; the LOD path is doing useful work.
- At 1,024 instances, total frame time stays around 31 ms even when reported GPU time
  is much shorter. This suggests CPU work/submission is a limiting factor in this build;
  it does not identify a specific function. Some kit models contain over 100 separate
  mesh parts. Profile a Release build before changing the renderer or deciding whether
  batching, traversal, culling or submission deserves priority.
- Lower GPU quality does not consistently increase FPS here. Single short runs are noisy
  and a CPU-limited workload may not benefit from lower render resolution.
- Reusing the same 16 assets keeps texture allocation roughly constant across grids.
  This is not a comprehensive VRAM exhaustion or streaming test. Increasing the texture
  preparation limit tests more texture pressure; a Jetson run is still needed for that
  platform's real shared-memory and driver constraints.
- The normal-map convention is assumed to be OpenGL. The preparation script supports
  flipping green for DirectX-authored maps, but no source-package specification confirmed
  which convention these gun maps use. A normals screenshot alone cannot prove the
  artistic intent of a map.
- The kit ships without texture maps, so its shiny neutral metal is a test material,
  not a reconstruction of an authored look. The helicopter is Unreal-only `.uasset`
  content and was not imported. Photo-reference images are not geometry.

The engine loads the whole selected Content folder, even for the four-model close-up.
Startup time and asset residency therefore don't scale down to only the visible models.
The reports record total process time separately from steady-state frame timing.
