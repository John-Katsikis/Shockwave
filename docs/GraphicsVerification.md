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
| materials | Main Camera | nine quads, each reading its maps through one material binding (G02) |
| materials-expected | Main Camera | the same quads with every binding baked into plain UV0: must match `materials` |
| material-instances | Main Camera | the `materials` quads, each part drawn through a `.swmaterial` that only names its source: must match `materials` exactly (A02) |
| material-layers | Main Camera | stone cubes plain, with a detail normal layer, and with moss as a second layer; masked foliage near and far (A02) |

`Tools/generate.py` writes the textures, glTF models, material files, and the dense,
materials and material scenes with only the Python standard library; the other scenes are written by hand. `Content/LICENSE.md` covers
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
| `graphics_fixtures_render` | yes | every scene renders (not black); two `--fixed-step` runs are identical; frame 10 and 40 differ; `--graphics-report` has memory and settings, and every draw has a stable instance key; FXAA brings thin-geometry closer to a 2x supersampled image, touches under 10% of pixels and keeps the brightness; `anti_aliasing none` matches the default; bloom and FXAA off leave no pass and no memory, each bloom tier has both; `materials` matches `materials-expected` in the unlit, normals and lit views; `material-instances` matches `materials` exactly in all three; `material_quality low` changes `material-layers` (the detail goes) but under 10% of it; `texture_quality low` shrinks material textures, settles, and changes the image |
| `graphics` | no | every settings key round-trips; `taa` and out-of-range `bloom_scale` are refused; presets set FXAA and bloom and keep bloom strength; only the Low preset sets `material_quality low`; hand-tuned settings read as Custom; `status()` |
| `graphics_cli` | no | `--graphics-set`, `--anti-aliasing`, refused values with file and line, `--graphics-info --json` |
| `graphics_materials_info` | no | `--material-info` on materials.glb: KHR_texture_transform values as authored, UV sets (including KHR's own `texCoord`), colour spaces, wrap and filter modes; a missing UV1 warns on import and fails `--check`; `--texture-info` sizes and bytes per quality; `--material-info` on `.swmaterial` files: the parent chain, inherited and overridden values, slots, features and what low quality drops; a missing parent, a loop and a typo each fail with the right kind of problem; `--material-schema` |
| `materials` | no | the `.swmaterial` parser: a valid file, canonical writing that reads back the same, exact float round trips, and 19 kinds of mistake each rejected with its line; inheritance order, determinism, imported parents found by name after their materials are reordered, missing references, loops, the 16-file limit; features, low-quality omissions and warnings; reoriented normal mapping's identities and unit length; Toksvig widening; coverage-keeping mipmaps against plain ones; cache keys; `material` and `materials` scene fields round-trip; hot reload (a parent edit reaches the child, a half-written file keeps the last good version, a missing parent goes magenta and recovers with the same handle) |
| `texture_quality` | no | exact BC7 and RGBA8 chain bytes, the 32-pixel floor, explicit LOD mapping, CPU tails averaged in linear light, sliced chains, UV transform maths, the sampler cache; then low, a reload at low, three changes in one frame, a change while one is running, and back to ultra, on the null renderer |
| `texture_quality_vulkan` | yes | the same transitions with real images swapped in while they're drawn |
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

## G02 + A01: material bindings and texture quality

- **Bindings.** Every material slot (base colour, normal, metallic-roughness, emissive,
  occlusion) has its own UV set (0 or 1), UV transform and sampler, from import to the
  shaders. The transform is `uv' = offset + R(rotation) * (scale * uv)` with
  `R = [[cos, sin], [-sin, cos]]` applied as rows, as the Khronos glTF sample renderer and
  three.js do it (the GLSL in the KHR_texture_transform README reads the other way round
  if taken as column-major; the sample renderer is what its test models are checked
  against). An identity transform leaves UVs bit for bit, so untextured and plain scenes
  are unchanged.
- **Importer facts, checked with fixtures** (Assimp 6.0.5): texture `texCoord`, wrap modes
  and glTF filters arrive per slot. KHR_texture_transform arrives rewritten into Assimp's
  convention and then flipped by `FlipUVs`; the importer undoes both and gets the
  authored numbers back exactly. Assimp drops KHR_texture_transform's own `texCoord`, so
  the importer reads it from the glTF JSON itself. Other formats' UV transforms are
  reported and ignored. TEXCOORD_1 becomes `MeshData::uv1`, an optional stream next to the
  64-byte vertex, for static and skinned meshes.
- **Normal maps** use the vertex tangents when they follow UV0 as modelled. Read through
  UV1, rotated or mirrored, they use a tangent frame built from the screen derivatives of
  their own UVs (`mesh.frag`, `derivativeTangents`). Where those derivatives are
  degenerate (UVs that don't change across the pixel) the map is skipped and the
  interpolated normal is used, rather than a made-up frame.
- **One copy of the shared code.** `Shaders/draw.glsl` holds the draw table, vertex layout,
  slot UVs and sampling, and the alpha test; the scene and shadow passes both include it,
  so a cut-out casts exactly the shadow it shows.
- **Samplers** are cached by description (at most 64; the default is handle 0) and made
  once per material, never per draw. `anisotropy` caps them, the device caps that, and
  nearest-filtered or unmipmapped samplers never get it. Changing it builds a second bank
  and retires the first; no GPU idle.
- **Texture quality** keeps the tail of each material texture from level 0 (ultra), 1, 2 or
  3 (low), never below 32 pixels. Multi-level textures (BC7 from `.ktx2`) are sliced; a lone
  RGBA8 level is halved on the CPU in linear light, then the GPU makes the rest of the
  chain as before. Changes run on workers (8 started a frame, 16 at once), and each new
  image goes into a spare bindless slot at a frame boundary while the old one is retired.
  The logical texture id never changes. If no spare slot is free, the swap falls back to a
  device wait (and says so in the log); 256 spares make that rare.
- **Measured.** `materials` against `materials-expected` on llvmpipe at 640x360: unlit RMSE
  0.69-1.14 (the nearest-sampled quad rounds texel edges a pixel either way), normals 0.01,
  lit 0.76, with the alpha-tested shadows identical. On transparency, `texture_quality low`
  reduces 3 of its 14 material textures (the rest are 32 pixels or smaller already) and
  holds 19,072 bytes of them on the GPU against 114,560 at ultra; all are BC7 from the
  texture cache, so the CPU copies are the cached chains (114,560 bytes).
- **Plugin ABI** is now 5.

Result: `capture.py --compare` found all 52 images identical to the G00 baseline (8 by
`--image-diff`, RMSE 0; 44 by mean colour). The defaults (`texture_quality ultra`,
`anisotropy 16`, identity bindings, the default sampler) reproduce the old output exactly.

## A02: material instances, detail and layers

- **Material files.** `.swmaterial` (format in [MaterialSchema.md](MaterialSchema.md),
  generated from the same descriptors the parser reads) names a parent and the values it
  changes. Imported parents are found by material name, never index; unnamed or
  duplicate names can't be referred to, and say so. Resolving gives one immutable
  `MaterialDesc`; every material, imported or authored, is then turned into what the
  renderer draws by the same `buildDrawMaterial`, which is why an instance that changes
  nothing draws bit for bit like its source.
- **Hot reload and failure.** A parse error keeps the last good version (the error shows
  in Content and the Material window); a missing reference draws flat magenta with the
  reason. Editing a parent, model or texture reloads every material that depends on it.
  Handles never change, so scenes and undo history keep pointing at the right material.
- **Colour space per slot.** Each slot decodes its texture as its own kind of data: a PNG
  used as colour by one material and as a mask by another is decoded (and compressed and
  cached) once for each. The variants are made on workers, cached as `.ktx2` like other
  textures, and freed when no material reads them.
- **Detail layer.** Colour multiplies around neutral white; normals combine with
  reoriented normal mapping (`composeNormals`, also in `mesh.frag`), in the detail's own
  tangent frame, so mirrored or rotated detail UVs work. It fades by camera distance and
  can be masked. `material_quality low` skips it in the shader, and nothing else changes.
- **Second layer.** Its own colour, roughness, metallic and normals, mixed in by a texture
  channel or a vertex colour channel. A vertex-colour mask stops the vertex colours tinting
  (they're paint data then); vertex alpha still multiplies alpha.
- **Specular anti-aliasing.** Where a normal map's filtered mip is shorter than unit
  length, roughness widens to match (Toksvig, through the Blinn-Phong equivalence
  `s = 2/a^2 - 2`). It only acts once the map is minified (`textureQueryLod`), and allows
  for 8-bit quantisation, so close-up and magnified normal maps are unchanged.
- **Coverage-keeping mipmaps.** For a masked material's base colour, each mip's alpha is
  scaled so the same share of texels passes the cutoff as at full size (Castano's
  bisection). Done on the CPU for uncompressed textures, and before encoding for
  compressed ones; the cutoff is part of the cache key.
- **Measured** on llvmpipe at 640x360: `material-instances` against `materials`, RMSE 0 in
  the unlit, normals and lit views. `material-layers` at `material_quality low` against
  high: RMSE 1.10, 0.66% of pixels changed, all on the detailed cube. Coverage on a
  64-pixel random mask stays within 0.06 of the original down to 8 x 8 with the new
  mipmaps; plain box filtering loses more than 0.15 of it.
- **Plugin ABI** is now 6 (`DrawMaterial` gained its layers, `MeshRenderer` and
  `ModelRenderer` their material fields).

Result: `capture.py --compare` found all 52 existing images identical to the baseline (8
by `--image-diff`, RMSE 0; 44 by mean colour). Neither Toksvig nor the coverage mipmaps
changed any of them at these sizes, so the baseline stands. `material-layers` is new; its 1280x720 image and
entries were added to the baseline without re-recording the others.

## Known limits

- Normal-map mips are still box-filtered; Toksvig widens roughness from their length in
  the shader instead of baking it into a roughness map.
- A `.ktx2` texture keeps its own mipmaps, so a masked material reading one gets no
  coverage adjustment (it warns), and one stored as the wrong colour space for its slot
  is used as stored (it warns).
- Linear and coverage variants of a texture are decoded on the CPU from the source file
  the first time (then cached), which costs memory and import time for every extra use.
- No `--material-bake`, no preview sphere in the Material window, and edits in the
  Material window aren't on the editor's undo stack (each is saved straight to the file;
  scene changes such as which material a part uses are undoable). Saving rewrites the
  file in canonical order and drops comments.
- Circuits can't set or read materials yet.
- Shading families other than standard (clearcoat, skin, hair) are refused until A07-A09.
- Texture quality is whole-level only: no streaming by distance or visibility yet, and
  every material texture keeps a CPU copy while it's loaded (shown in the report and the
  Optimisation window).
- A MeshRenderer with a material ignores its own texture, roughness and metallic but still
  applies its tint and tiling.
- Generated lightmap UVs (a third stream) are G05's.
- The editor grid is drawn in the scene pass, so FXAA smooths it too.
- Images waiting to be freed after a change aren't counted in the memory report.

- Synchronization validation (`VK_LAYER_KHRONOS_validation` with sync checks) was not run
  for G00: the layer isn't installed on the machine that recorded the baseline. The render
  graph's barriers are covered by `render_graph` instead. Run it where the layer exists.
- Baseline images and timings are llvmpipe only.
