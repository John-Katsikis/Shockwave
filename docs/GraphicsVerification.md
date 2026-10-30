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
| ambient-occlusion | Main Camera | a wall corner, a box on the floor, a thin pole 7 m in front of a far wall, a black glowing block and sky (G04); `-zero` has `ao_intensity 0`, `-direct` no ambient light |
| room-doorway-baked | Inside, Outside | `room-doorway` with its room as static lightmap receivers, the sun and lamp bounced, and a probe volume for the unbaked ball (G05, G06). Its bake isn't committed: the render test bakes a copy |

`Tools/generate.py` writes the textures, glTF models, material files, and the dense,
materials, material and ambient occlusion scenes with only the Python standard library; the other scenes are written by hand. `Content/LICENSE.md` covers
the content.

## Commands

```sh
# Look through a named camera, reproducibly:
build/bin/player --project Tests/fixtures/graphics/Graphics.swproject --no-user-graphics \
    --scene Tests/fixtures/graphics/Scenes/motion.swscene --camera "Fixed" \
    --fixed-step --frames 30 --screenshot /tmp/motion.png

# Numbers for an image (or part of it: x,y,width,height as fractions), and how far apart
# two are (exit 1 past the RMSE tolerance):
build/bin/player --image-stats /tmp/motion.png [--region 0.5,0,0.45,0.08]
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
| `graphics_fixtures_render` | yes | every scene renders (not black); two `--fixed-step` runs are identical; frame 10 and 40 differ; `--graphics-report` has memory and settings, and every draw has a stable instance key; FXAA brings thin-geometry closer to a 2x supersampled image, touches under 10% of pixels and keeps the brightness; `anti_aliasing none` matches the default; bloom and FXAA off leave no pass and no memory, each bloom tier has both; `materials` matches `materials-expected` in the unlit, normals and lit views; `material-instances` matches `materials` exactly in all three; `material_quality low` changes `material-layers` (the detail goes) but under 10% of it; `texture_quality low` shrinks material textures, settles, and changes the image; ambient occlusion (below) |
| `graphics` | no | every settings key round-trips; `taa` and out-of-range `bloom_scale` are refused; presets set FXAA and bloom and keep bloom strength; only the Low preset sets `material_quality low`; hand-tuned settings read as Custom; `status()` |
| `graphics_cli` | no | `--graphics-set`, `--anti-aliasing`, refused values with file and line, `--graphics-info --json` |
| `graphics_materials_info` | no | `--material-info` on materials.glb: KHR_texture_transform values as authored, UV sets (including KHR's own `texCoord`), colour spaces, wrap and filter modes; a missing UV1 warns on import and fails `--check`; `--texture-info` sizes and bytes per quality; `--material-info` on `.swmaterial` files: the parent chain, inherited and overridden values, slots, features and what low quality drops; a missing parent, a loop and a typo each fail with the right kind of problem; `--material-schema` |
| `screen_space` | no | the screen-space contract (G03): the view's corners land on UV (0, 0) and (1, 1) with v down; positions come back from reverse-Z depth at the near plane, far away, at every corner, for tall and wide views and orthographic ones; depth 0, negative and NaN depth are background; a texel centre at render scale 0.5, 1 and 2 lands within a texel; octahedral normals round-trip, also through 16 bits |
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

## G03 + G04: surface prepass and ambient occlusion

- **Coordinate contract.** `Renderer/ScreenSpace.h` (and its GLSL twin
  `Shaders/screen.glsl`): screen UV is u right, v down, (0, 0) the top-left of the image;
  `screenUv = (0.5 ndc.x + 0.5, 0.5 - 0.5 ndc.y)`. Positions come back from reverse-Z depth
  through the inverse of the camera's own projection, so perspective (infinite far
  plane), orthographic and any aspect or render scale work the same way; depth 0 is
  background and has no position. `screen_space` checks the C++ side; the GLSL side is the
  same expressions and is checked by the ambient occlusion fixtures, whose occlusion would
  land in the wrong place if either disagreed.
- **Surface prepass.** Only when ambient occlusion is on (or its debug view is shown).
  Opaque and alpha-tested surfaces are drawn with the scene's own vertex shader
  (`invariant gl_Position`) and the same alpha test (`baseAlpha`), writing reverse-Z
  depth and the geometric normal in view space, octahedral in `RG16_SNORM` (`RG16F` where
  that can't be rendered to). Blended surfaces aren't in it. The scene then shades those
  surfaces with depth compare EQUAL and no depth writes, over a read-only depth
  attachment; blended surfaces, the sky and the grid test the same depth as before. No
  receiver-flags target: nothing reads one yet.
- **Ambient occlusion.** `Shaders/ssao.comp` at half resolution: each texel stands for the
  nearest (greatest reverse-Z depth) of its four pixels; 8, 16 or 32 cosine-weighted
  directions over the normal's hemisphere with distances stratified and biased towards
  the point; a fixed rotation per pixel from a 4 x 4 Bayer tile, so a still camera sees
  still occlusion. A sample counts as blocked when the surface seen there is nearer than
  it by more than a bias that grows with distance, weighted down the further that
  surface is from the point (so a pole far in front of a wall doesn't darken the wall);
  samples off the screen don't count either way; `AO = 1 - blocked / counted`, then
  `pow(AO, ao_intensity)`. Two 9-tap bilateral blurs (across, down) weigh depth and
  normals, and `mesh.frag` upsamples with the four nearest texels weighted the same way.
  It darkens only the light from the surroundings' irradiance (diffuse, and its
  multiple-scattering share), combined with the material's occlusion map by the minimum.
  Reflections keep only the map's; direct light, shadows and emission are untouched.
- **Settings and scene fields.** `ambient_occlusion` off (default), low, medium, high;
  presets Low off, Medium low, High medium, Ultra high. Environment `ao_radius` (0.3 m)
  and `ao_intensity` (1). `player --view ao` and the editor's *Ambient occlusion* view
  show it alone, white where nothing occludes (the sky included), computed at medium when
  the setting is off. The report shows the sample count, resolution, radius, normal
  format and memory, under *surface buffers* and *ambient occlusion*.
- **Plugin ABI** is now 7 (`RenderBackend::setAmbientOcclusion`, `LightingDesc` and
  `Environment` gained their ambient occlusion fields).

`graphics_fixtures_render` now also checks, on `ambient-occlusion` at 640x360 with
ambient occlusion high: in the AO view the sky and open floor are white and the box's
contact with the floor is darker, and the far wall beside the thin pole isn't (no halo);
frames 10 and 40 are identical; render scale 0.5 stays within RMSE 4 of 1; off matches
the default exactly; `ao_intensity 0` with it on, and a scene with no ambient light with
it on, both match off exactly (RMSE 0: the prepass and EQUAL shading change no pixel by
themselves); on changes the lit image; the black glowing block is unchanged; off has no
prepass, no AO pass and no memory for either, and on has all of them.

**Measured** on llvmpipe at 640x360, ambient occlusion high: AO view sky and open floor
255, the box's contact 242, the far wall beside the pole 255; render scale 0.5 against 1,
RMSE 1.33 (0.7% of pixels changed, at edges). Lit, against off: RMSE 0.62 / 0.58 / 0.54
for low / medium / high on `ambient-occlusion` (ambient 0.8, so occlusion is a gentle
darkening of fill light), and 1.21 on `room-doorway` (Inside). At 1280x720 the report
says 32 samples at 640x360, 3.5 MiB of surface buffers and 3.5 MiB for occlusion.

Result: `capture.py --compare` with the default settings (ambient occlusion off) found
all 52 baseline images identical (8 by `--image-diff`, RMSE 0; 44 by mean colour), so the
baseline stands. The ambient occlusion scenes aren't in the capture: with the setting
off they'd only show the old frame, and `graphics_fixtures_render` covers them on.

## G05 + G06: lightmaps and diffuse probe volumes

- **Identity.** `BakeGeometry` (static receivers and occluders), `BakeLight`,
  `DiffuseProbeVolume` and `BakedLighting` components; persistent 128-bit `binding_id`s
  made when the component is added or an object duplicated, and a `bake_id` on prefab
  copies from which their parts' ids are derived (namespace `shockwave.bake.prefab.1`).
  `lighting_identity` checks that ids survive saving and reordering, that missing,
  malformed and duplicate ids are reported and only `assign` fixes them, that two copies
  of one prefab get distinct ids by the documented derivation, and that a receiver's hash
  covers mirrored and stretched transforms and its colour but not its name.
- **Files.** `Content/Lighting/<scene>.swlighting` (strict text manifest, errors with
  file:line), `Lightmap_NNN.ktx2` (standard KTX2, RGBA16F, one level),
  `Receivers.swbake` and `Probes.swprobe` (bounded binary readers); docs/LightingSchema.md
  is generated from the code. Published through a hidden folder and swapped only when
  every file reads back with its hash; `lighting_format` covers the round trips, every
  truncation, wrong magic and overflowing counts, and JSON export/import giving back the
  same bytes.
- **Bake.** xatlas per receiver at its texels_per_meter, shelf-packed into power-of-two
  atlases; a binned-SAH BVH; cosine-weighted paths with Hammersley first bounces, seeded
  from binding, texel and seed only. `lighting_bake` checks, against closed forms:
  a constant environment bakes that constant, black bakes zero, the receiver's own direct
  light is never baked, a red wall bounces red, 0/1/2 bounces order correctly, a closed
  box bakes zero, alpha-masked holes let light through, the same seed gives the same bytes
  on 1 and 4 threads, cancel leaves the last bake, and editing one receiver invalidates it
  alone while a light change invalidates all.
- **Probes.** SH9 irradiance / pi plus 8 x 8 octahedral distance moments per probe;
  probes with a quarter of their rays on backfaces are invalid. Sampling is trilinear x
  validity x Chebyshev visibility, normalised, falling back to the nearest valid visible
  probe; volumes by priority then size, with boundary blending to the environment.
  `diffuse_probes` checks interpolation, invalid and hidden probes, the wall (no leak
  through), priorities, blending, rotated volumes and a doorway walk with no jump or black
  frame.
- **Rendering.** `mesh.frag`'s `surroundingsIrradiance` reads the lightmap (level 0,
  clamped half a texel in) or the draw's probe SH instead of the irradiance cube, in the
  same units; nothing else in shading changes. Receivers draw a re-indexed copy of their
  mesh with lightmap UVs; everything else samples probes at its bounds centre. The
  `baked_lighting` setting (default true) strips both in `Renderer::drawMesh`, so false is
  the old path. `player --view indirect` and the editor's *Indirect diffuse* view show it.
- **Tools.** `player --bake-lighting`, `--lighting-info [--json]`, `--lighting-assign-ids`,
  `--lighting-schema`, `--lighting-export`/`--lighting-import`; `--check` fails on missing,
  malformed or duplicate ids and damaged bakes, and warns about missing or stale ones.
  Window > Lighting bakes a copy of the scene on a thread with progress and cancel.
- **Plugin ABI** is now 8 (`RenderBackend::setMeshLightmapUv`, `DrawMaterial::indirect`).

`graphics_fixtures_render` now also bakes a copy of `room-doorway-baked` (preview, seed 7)
twice and requires identical lightmap bytes; `--lighting-info` says missing, then ready;
`--check` passes without bake warnings; `baked_lighting false` matches `room-doorway`
exactly (RMSE 0); the bake changes the lit image (RMSE over 2); in the indirect view the
ceiling beside the red wall is at least 10 steps redder than beside the green one, and
not red unbaked; the ball (not baked) changes through the probes; and after moving the
box, `--lighting-info` says stale while the rest of the room keeps its light.

**Measured** on llvmpipe at 640x360 (Inside): baked against unbaked, RMSE 24.8 with 95%
of pixels changed (the room's inside is lit by its own bounces, not the open sky); the
ceiling beside the red wall reads red minus green 28 baked and 5 unbaked (1 beside the green wall); off against
unbaked RMSE 0. The bake takes 0.22 s preview and 1.2 s final on 4 threads: 9 receivers,
8899 texels in one 512 x 512 atlas (2 MiB), 324 probes (85 inside walls, invalid),
198 KiB.

Result: `capture.py --compare` (baked_lighting on, no baseline scene has a bake) found all
56 baseline images identical (9 by `--image-diff`, RMSE 0; 47 by mean colour), so the
baseline stands. `room-doorway-baked` isn't in the capture: its bake is made by the test.

## G07: local reflection probes

- **Contract.** `Renderer/ReflectionProbes.h` holds the CPU side of every rule and
  `Shaders/reflection.glsl` the same in GLSL: cube faces in the global environment's
  orientation, slab box projection with the unnormalised ray (no exit, a point outside or
  a zero ray gives weight 0, never a NaN), the linear fade over `blend_distance`, up to two
  probes a draw (priority, then smaller box, then binding id), and the blend (equal
  priorities share and cover min(sum, 1); a higher one goes over the lower; the rest is
  the global cube). `reflection_probes` checks all of it without a GPU, plus the cube
  `.ktx2` (round trip, deterministic bytes, wrong face or level count, truncation), JSON
  export/import back to the same bytes, capture resampling through each face camera from
  a stretched image, the prefilter (uniform stays uniform, deterministic, level 0 kept,
  rougher levels blur) and the `.swreflection` manifest (round trip, file:line errors).
- **Renderer.** `TextureShape::Cube` textures (RGBA16F, 6 faces, 6 levels) get their own
  descriptor array (binding 7, 128 slots) apart from the 2D bindless table; the probe
  table (up to 64) lives in the frame buffer; `DrawData.reflections` packs the draw's two
  probe indices, and is 0 when the setting is off, on a capture frame, or with no probes,
  which leaves `mesh.frag` on the old path. `Renderer::requestSceneCapture` reads the HDR
  scene target back after the scene pass (before particles, exposure, bloom and AA) with
  the grid skipped and Lit forced. Cubes count under *reflection probes* in
  `--graphics-report`. The null renderer refuses cubes and captures with a reason.
- **Capture.** `ReflectionCapture` (frame-driven, six frames a probe, cancellable) draws
  only static renderers, prefilters on the CPU with `environment_filter.comp`'s
  estimator, writes each cube atomically and then the manifest; the old capture stays
  until the new one has loaded completely. Each probe's hash covers its placement and
  settings; the scene hash covers captured renderers, lights, the Environment and the
  lighting bake. `player --capture-reflections`, Window > Lighting > *Capture
  reflections* (progress and cancel), `--lighting-info` (a `reflections` section, also
  in `--json`), `--check` (fails on damaged captures, warns on missing or stale ones and
  on bad probes), `--lighting-export`/`--lighting-import` for cubes, `--view
  reflections`, the editor's *Reflection probes* view, and the selected probe's box and
  capture point in the viewport.
- **Plugin ABI** is now 9 (texture shapes, `setReflectionProbes`, `setLocalReflections`,
  scene capture).

`graphics_fixtures_render` now also captures a copy of `reflection-rooms` (two rooms
through a doorway, one probe and one dynamic mirror ball each): `--lighting-info` says
missing, then ready; capturing twice gives identical cube bytes; `--check` passes without
reflection warnings; the west cube's face means put the lamp-lit ceiling on +Y, the green
floor on -Y, the red wall on -X and the blue one on -Z; `reflection_probes false` after
capturing matches the uncaptured image exactly (RMSE 0); the capture changes the image
(RMSE over 2); the west ball reflects red and the east ball doesn't; and after moving the
west probe it's stale and left out (the ball reflects the sky again).

**Measured** on llvmpipe at 640x360 (West camera): captured against off, RMSE 13.1 with
19% of pixels changed (the ball, and the glossy walls and floor, now reflect the room
instead of the sky); the west ball's red minus green is +47 with probes and -16 without.
Two 64-pixel probes capture in 2.5 s (llvmpipe; the cost is twelve frames plus the CPU
filter) and take 0.5 MiB of GPU memory, 513 KiB on disk.

Result: `capture.py --compare` (reflection_probes on; no baseline scene has probes) found
all 56 baseline images identical (9 by `--image-diff`, RMSE 0; 47 by mean colour), so the
baseline stands. `reflection-rooms` isn't in the capture: its probes are captured by the
test.

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
- The editor grid is drawn in the scene pass, so FXAA smooths it too.
- Images waiting to be freed after a change aren't counted in the memory report.
- Ambient occlusion follows geometric normals only: normal maps don't add occlusion of
  their own (their material occlusion map still does). There's no specular occlusion,
  no temporal accumulation (G10's TAA will smooth it further) and no bent normals.
- With ambient occlusion on, opaque geometry's vertices are processed twice (prepass and
  shading). Blended surfaces never receive it.
- Baked lighting: lightmaps are non-directional (one colour a texel), moving objects take
  one probe sample at their centre, the bake ignores emissive textures and samples base
  colour textures with wrap at full size, copies of one mesh each get their own lightmap
  space, and lightmaps are read at level 0 only. Preview bakes (64 paths) are visibly
  noisy; there's no denoiser.
- The AO debug view writes occlusion to the display as it is, so the sRGB encoding shows
  middling values lighter than their number (0.5 reads as about 188 of 255).

- Synchronization validation (`VK_LAYER_KHRONOS_validation` with sync checks) was not run
  for G00: the layer isn't installed on the machine that recorded the baseline. The render
  graph's barriers are covered by `render_graph` instead. Run it where the layer exists.
- Baseline images and timings are llvmpipe only.
