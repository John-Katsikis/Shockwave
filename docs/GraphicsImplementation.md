# Graphics implementation specification for Shockwave

Status: **implementation plan; the features and commands marked proposed do not exist yet**.

Written 2026-10-19 against inspected revision
`35dec6cbdb81dec2400081c3448e55bb169ba885`. Re-read the affected source before
implementing a batch: other contributors may have completed or changed parts of it.
This document itself changes no renderer behavior.

The [graphics implementation guide](Graphics/README.md) extends this foundation with
production settings, texture residency, materials, particles, grading/fog, specialized
shading, and mathematical reference cases. Open it from the editor's **Help > Graphics
implementation guide**. Its A00–A10 batches supplement G00–G13; A06 explicitly scopes
the fog work listed as deferred below. No rendering feature is implemented by these docs.

Navigation: [current code](#1-what-exists-at-the-inspected-revision),
[architecture](#2-architecture-and-ownership),
[batch order](#3-required-implementation-batches-and-dependency-graph),
[baking](#9-g05-bake-contracts-persistent-identity-and-static-diffuse-lighting),
[temporal AA](#14-g10-temporal-anti-aliasing-without-premature-upscaling),
[budgets](#17-settings-defaults-budgets-and-observability),
[tools and packaging](#18-editor-text-formats-cli-contracts-and-packaging),
[verification](#19-verification-matrix-and-exact-evidence-expectations).

## 0. Mission, constraints, and how to use this document

Build a visually convincing, stable renderer for bounded multiplayer shooter maps,
with useful settings for inexpensive PCs. Prioritize stable edges, believable indoor
lighting, material fidelity, contact, and predictable frame cost. Do not equate visual
quality with enabling every effect, and do not assume Vulkan support implies sufficient
GPU speed or memory. An Orin Nano Super is a useful shared-memory stress platform,
but neither it nor an RTX 3080 represents every intended PC.

The six required workstreams are:

1. Anti-aliasing and temporal image stability.
2. Baked diffuse lighting for static geometry and diffuse probes for moving objects.
3. Local reflection probes, with the existing global environment as fallback.
4. Screen-space ambient occlusion for contact detail.
5. Material import fidelity and reusable projected decals.
6. Scalable local lighting, batching, and conservative visibility.

This is a specification for an implementing agent. Work through the batches in section
3, not as one enormous rewrite. Complete a batch's UI, CLI, validation, documentation,
and evidence before claiming it is done. If the user requests approval between batches,
stop at each completion gate. Do not infer permission to push, merge, or deploy from
this document. Follow the user's current branch and collaboration instructions.

### 0.1 Non-negotiable rules

- Read `AGENTS.md`, `docs/Architecture.md`, and `docs/UI.md` first.
- Preserve unrelated working-tree changes. Coordinate renderer ownership before edits.
- Public engine APIs must not contain Vulkan types. Vulkan belongs in RHI and the
  Vulkan plugin. The null renderer must still build and support validation workflows.
- Extend the existing renderer, settings, assets, registration, and packaging paths.
  Do not add a second graphics-settings system or bypass asset handles with raw paths
  inside shaders or game code.
- Every authored feature has registered metadata or a documented asset schema,
  editor controls, useful `--check` diagnostics, and a command-line verification path.
- Use shared `shockwave::gui` controls, theme colors, existing undo tracking, and
  stable widget IDs. Generated metadata drives ordinary component inspector fields.
- Asset formats remain accessible. Text manifests describe binary payloads. Every
  new proprietary binary format needs documented fields and inspect/export/import
  tooling; never serialize C++ object memory or hide the only usable copy in a cache.
- No implicit CPU ray tracing, baking, GPU readback, shader compilation, or device-wide
  idle waits on the normal gameplay frame path.
- Keep effects individually disableable and report actual supported settings.
- All numerical limits below are initial design defaults or targets, not measured
  performance claims. Record measurements before changing defaults.
- Do not replace existing shadows, PBR, bloom, skinning, or environment filtering
  merely because this plan introduces new consumers of them.

### 0.2 Scope boundaries

Required baseline: FXAA, material correctness, an optional surface prepass, SSAO,
offline diffuse lightmaps/probes, offline local reflection probes, basic projected
decals, correct motion vectors/TAA, clustered light lists, and measured batching and
occlusion improvements.

Deferred: temporal upscaling, SSR, fog, directional lightmaps, dynamic GI, ray-traced
reflections, automatic exposure, shader graphs, clouds, fluid simulation, virtualized
geometry, and virtual texturing. Section 21 states prerequisites for these. Do not
create empty systems for them during baseline work.

## 1. What exists at the inspected revision

These are source observations, not evidence that every path has passed a visual test.

| Area | Existing implementation | Consequence for this plan |
|---|---|---|
| Shading | Forward metallic/roughness PBR, normal/tangent data, vertex colors, alpha mask/blend, double-sided materials | Extend the material record and shared shader functions. |
| Environment | HDR environment conversion, diffuse irradiance, GGX-filtered specular mips, BRDF integration, multiscatter compensation | Reuse filtering and BRDF conventions for local probes. |
| Depth | Infinite-far reverse-Z, clear depth zero | Reconstruct depth correctly; use MIN for conservative reverse-Z HZB. |
| Shadows | Sun cascades and point/spot shadow atlases | Preserve quality controls and caster visibility. |
| Postprocess | HDR bloom and tone mapping; manual exposure | Put temporal resolve before bloom, and spatial AA before UI. |
| Animation | GPU skinning reused for scene/shadow draws | Previous skinning data is a new requirement, not already solved by physics history. |
| Visibility | CPU frustum culling, mesh LOD selection, opaque/transparent sorting | Optimize incrementally and retain a reference path. |
| Resolution | Render scale and output-resolution UI | Keep render, output, history, and viewport dimensions distinct. |
| Diagnostics | GPU timestamps, benchmark JSON, screenshots, debug views | Extend existing tools instead of adding an unrelated profiler. |
| Assets | Assimp import, texture optimization, generated LODs, hot reload | Bake dependencies and material bindings must participate in these systems. |
| Backend lifecycle | Renderer resource IDs and state replay on backend reload/switch | Journal new resources/settings and reset temporal state explicitly. |

At this revision, dedicated implementations of FXAA/TAA, SSAO, lightmaps, local
reflection probes, clustered lighting, and projected decals were not found in the
inspected renderer paths. Recheck before implementing; do not overwrite newer work.

### 1.1 Read these files before changing their area

Paths in this table are existing repository paths, not proposed filenames.

| Concern | Files |
|---|---|
| Render API and replay | `Source/Runtime/Renderer/Public/Renderer/Renderer.h`, `RenderBackend.h`, `RenderTypes.h`; `Source/Runtime/Renderer/Private/Renderer.cpp` |
| Mesh and texture contracts | `Source/Runtime/Renderer/Public/Renderer/Mesh.h`, `TextureData.h` |
| Frame assembly | `Source/Plugins/RendererVulkan/Private/RendererFrame.cpp`, `RendererImpl.h`, `VulkanRenderer.cpp` |
| Resource/pipeline creation | `Source/Plugins/RendererVulkan/Private/RendererResources.cpp`, `RendererSetup.cpp`, `RendererPipelines.cpp` |
| Existing lighting | `Source/Plugins/RendererVulkan/Private/RendererEnvironment.cpp`, `RendererShadows.cpp`, `RendererBloom.cpp` |
| Animation/visibility | `Source/Plugins/RendererVulkan/Private/RendererSkinning.cpp`, `RendererOptimisation.cpp` |
| Render graph | `Source/Runtime/RHI/Public/RHI/RenderGraph.h`, `Source/Runtime/RHI/Private/RenderGraph.cpp` |
| Asset import | `Source/Runtime/Assets/Public/Assets/Model.h`, `Image.h`, `ImportSettings.h`; `Source/Runtime/Assets/Private/Model.cpp`, `AssetFolder.cpp`, `MeshOptimise.cpp` |
| Render submission | `Source/Runtime/Engine/Private/Systems.cpp`, `Camera.cpp`; `Source/Runtime/Engine/Public/Engine/World.h` |
| Settings/components | `Source/Runtime/Engine/Public/Engine/GraphicsSettings.h`, `Components.h`; matching files in `Private/` |
| Scene persistence | `Source/Runtime/Engine/Public/Engine/SceneFile.h`, `Source/Runtime/Engine/Private/SceneFile.cpp` |
| CLI | `Source/Programs/Player/Private/Main.cpp`, `PlayerTools.cpp`, `PlayerOptimise.cpp`, `Player.h` |
| Editor | `Source/Editor/Editor/Private/InspectorPanel.cpp`, `ContentBrowserPanel.cpp`, `ViewportPanel.cpp`, `Preferences.cpp` |
| Plugin ABI | `Source/Runtime/Core/Public/Core/Plugin.h` |

Also read `docs/Graphics.md`, `GraphicsSchema.md`, `Lighting.md`, `Shadows.md`,
`Assets.md`, `Optimisation.md`, `SceneSchema.md`, `FramePacing.md`, `Packaging.md`, and
`Plugins.md`. Locate shaders with `rg --files Source/Plugins/RendererVulkan`; do not
guess their installed or generated locations.

### 1.2 Existing implementation traps

1. `prepareDraws()` assigns draw indices after sorting. A draw index is not a persistent
   object identity. Never index temporal history by last frame's sorted position.
2. Skinned output offsets follow gathered work. Last frame's offset is not automatically
   the same mesh instance this frame.
3. `recordScene()` can discard depth when particles do not need it. Later depth readers
   require `STORE`, independently of particle presence.
4. The graph currently tracks images, not general buffer dependencies or independent
   mip/layer states. Compute additions need explicit dependency support.
5. The scene viewport has **negative height**. Projection matrices are not Y-flipped in
   `setCamera()`. Avoid a second flip in reconstruction, jitter, or motion vectors.
6. The existing vertex layout is 64 bytes. CPU structures, skinning, and shader buffer
   loads must agree. Adding a field to one copy corrupts all later attributes.
7. Global diffuse irradiance filtering stores irradiance divided by pi. Adding a raw
   irradiance lightmap to that shader produces a brightness error.
8. Existing direct-light intensity conventions are not simply textbook radiometric
   radiance. Match existing output when implementing the offline integrator.
9. Current resource upload/destruction paths include blocking synchronization. Do not
   multiply those stalls by streaming probes or recompiling material variants in play.
10. GPU timing has a finite pass capacity (`kMaxTimedPasses` currently 31). Increase the
    capacity consistently or report overflow; do not silently leave new effects untimed.

## 2. Architecture and ownership

### 2.1 Keep the forward renderer

Use forward shading plus an optional depth/normal prepass. This provides inputs for AO,
decals, temporal rejection, and visibility without immediately replacing the renderer
with a full deferred material G-buffer. The prepass has a real geometry cost, especially
on low-end hardware; disable it when no enabled feature needs it.

Render passes consume immutable frame data. Asset decoding/baking works on copied or
owned CPU snapshots. The render thread owns command recording and GPU lifetime.
Worker tasks never mutate ImGui state or a live World while baking.

### 2.2 Module responsibilities

| Owner | Responsibility |
|---|---|
| Renderer | Portable view/draw identifiers, material/resource descriptors, feature settings/capabilities, resource replay, public statistics |
| RHI | Vulkan synchronization and resource-state machinery used by Vulkan passes |
| RendererVulkan | Actual images, buffers, descriptors, shaders, pass scheduling, GPU upload/retirement |
| Assets | Imported UVs/samplers/material slots, decoded payloads, dependency hashes, hot reload, asset references |
| Engine | Shared graphics preferences, existing scene/components/render submission, generic persistent scene identity if needed |
| **Lighting (new)** | Registered bake/probe components, immutable bake snapshots, CPU bake jobs, bake manifests, runtime binding service |
| GameFramework | Register/start the Lighting subsystem and schedule it before render submission |
| Editor | Bake controls, progress/cancel, gizmos, debug selection, preview and undo |
| Player | Parse CLI, run the same services as editor, print results and return exit status |
| Packaging | Collect and validate all referenced bake/probe/material payloads |

Create `Source/Runtime/Lighting` with `shockwave_module()`. Place it after Engine and
before its consumers in `Source/Runtime/CMakeLists.txt`; declare dependencies explicitly.
Its public headers live in `Public/Lighting/`, implementation in `Private/`. It may use
Engine, Assets, and Renderer. Engine must **not** include Lighting in return.

For render submission, use portable bindings declared in Renderer, populated by the
Lighting subsystem before the render schedule runs. The baseline integration mechanism
is a runtime-only `RenderLightingBinding` component declared in Engine, containing
portable handles and per-model-part binding entries. It is derived state: do not register
it as user-editable serialized scene data. Lighting writes it; Engine's render submission
reads it; Vulkan receives copied draw descriptors. Do not scan Lighting components from
Vulkan code and do not give Vulkan a World pointer.

Declare the schedule dependency from lighting-binding preparation to render submission.
Empty/missing entries mean the global lighting fallback. Each entry identifies the model
part, binding generation, lightmap handle/UV stream and atlas transform, selected diffuse
probe volume handles, and up to two selected reflection-probe handles. Renderer-owned
descriptors hold the immutable sampling metadata behind those handles. This avoids
copying entire probe arrays into every draw record. Initialize/clear these derived
components on scene clone and Play/Stop so cloned authoring state cannot retain another
session's GPU handles. Test removal and reload before adding baked content.

Register Lighting components before loading scenes in editor, player, and relevant tests.
Headless scene checking must not create a GPU or require an audio device. Pure bake
algorithms should be callable in tests without opening a window.

### 2.3 ABI and lifecycle

- Extend `RenderBackend`, Renderer forwarding, resource journaling, and RendererNull
  together. If a plugin ABI contract changes, bump the plugin ABI and rebuild plugins.
- Do not pass spans whose storage disappears before a backend records/copies them.
- Resource handles have generation/lifetime validation. A failed reload retains the
  last valid resource and emits a diagnostic; it never exposes a half-uploaded atlas.
- Backend reload replays meshes, auxiliary UV streams, texture shape/mips, samplers,
  lighting bindings, and graphics settings. It invalidates all temporal history.
- Scene unload, Play/Stop, project switch, and GPU shutdown detach bindings before
  freeing resources. GPU objects retire after their last submission completes.

### 2.4 Why these choices fit this engine

| Choice | Reason | Cost or limitation accepted |
|---|---|---|
| Keep forward shading | Preserves working PBR, shadows, and material integration; transparent shading uses the same light model | A prepass repeats geometry work when enabled; profile it |
| FXAA before TAA | Gives an inexpensive useful option without first requiring correct motion history | It does not stabilize subpixel geometry or specular shimmer across frames |
| Offline diffuse lighting | Bounded mostly static maps can spend authoring time to save runtime lighting work | Bake time, UVs, memory, and explicit invalidation become production concerns |
| Probes for moving objects | Avoids giving every moving character a lightmap or solving dynamic GI each frame | Approximate spatial lighting; visibility and sparse-grid leakage require care |
| Local reflections before SSR | Room-scale probes cover off-screen geometry and work without temporal reconstruction | Captures are static approximations and cannot reflect every moving actor |
| AO as a small optional effect | Adds readable contact on unbaked surfaces at a bounded resolution | Screen-space information is incomplete; AO cannot supply missing bounce light |
| Explicit material bindings before baking | Runtime and offline tools must agree about texture coordinates and alpha | Import/schema/replay changes precede the visually impressive bake result |
| Conservative culling with reference paths | A shooter cannot accept a player disappearing because a heuristic was aggressive | False-visible results cost work; they are preferable to false-hidden results |
| One queue initially | Makes synchronization and history ownership easier to prove | Some potential compute/graphics overlap is intentionally deferred |

Do not turn these tradeoffs into permanent dogma. Replace an algorithm when a controlled
comparison demonstrates better correctness, image quality, or cost within the same
public contracts. Record the reason and update the tests/specification together.

## 3. Required implementation batches and dependency graph

The order intentionally delivers a cheap AA option early, fixes asset fidelity before
baking it, and postpones temporal complexity until the renderer has explicit inputs.

| Batch | Deliverable | Dependencies | Completion gate |
|---|---|---|---|
| G00 | Baseline fixtures, graph/capability/lifetime groundwork | None | Existing output preserved; validation and baseline evidence |
| G01 | None/FXAA settings and pass | G00 | Aliasing fixture improved; UI unaffected |
| G02 | Material UV/sampler/transform fidelity | G00 | Import fixtures and all passes agree |
| G03 | Optional surface prepass and reconstruction | G02 | Depth/normals/masks correct at all supported views/scales |
| G04 | SSAO | G03 | Contact benefit without direct/emissive darkening |
| G05 | Bake identity, lightmap unwrap, CPU diffuse bake | G02 | Reference bake, invalidation, UI/CLI/package round trip |
| G06 | Diffuse probe volumes | G05 | Moving object follows baked illumination without gross leaks |
| G07 | Local reflection probes | G05, G06 infrastructure | Correct box projection/blending and global fallback |
| G08 | Projected decals | G03, G02 | Stable layering, receiver rules, runtime API and editor |
| G09 | Stable render history and motion vectors | G03 | Rigid, camera, skinned, and viewmodel motion tests |
| G10 | TAA at native render resolution | G09 | Temporal sequences pass; no UI blur or history leaks |
| G11 | Clustered local lighting | G03 infrastructure | Matches brute-force path, including overflow |
| G12 | Instancing, conservative HZB/indirect draws | G11, G09 identity | No missing objects; measured benefit on crowded fixture |
| G13 | Final budgets, presets, tool/package integration | All required batches | Reproducible end-to-end evidence |

G05 can be developed independently of SSAO after G02, but implement serially unless
the user explicitly delegates parallel work. Do not let speculative optional features
block a completed baseline.

For every batch: inspect current code; make the smallest coherent implementation;
format touched C++; build affected targets; run relevant tests; inspect screenshots/UI;
record evidence in proposed `docs/GraphicsVerification.md`; commit if authorized.
Record commit, hardware/driver, settings, commands, actual results, and limitations.

## 4. G00: baseline, render graph, capabilities, and view identity

### 4.1 Create deterministic fixtures first

Add a small self-contained graphics test project under `Tests/fixtures/graphics/`,
using generated geometry and tiny authored/generated textures. Do not depend on
`/mnt/STORAGE_/Asset Library/`, private game assets, downloaded commercial assets, or
an untracked local scene. Include a license note for generated content.

Scenes: slanted thin geometry/checkerboard; colored room with a doorway; roughness/
metalness spheres; transparent and alpha-masked panels; two overlapping light volumes;
a moving rigid object and skinned strip; decals on a wall and moving door; dense
repeated geometry with an occluding wall. Use fixed seeds and scripted camera paths.

Capture the existing renderer before changing it. Include 720p and 1080p, render scales
0.5 and 1, VSync off for throughput, and the normal paced configuration for jitter.
Store compact reference metadata and selected images, not gigabytes of captures.

### 4.2 Extend graph declarations before dependent effects

Keep the existing single graphics queue initially. Do not introduce async compute as
a prerequisite. Dispatching compute on the graphics queue can still execute on the GPU;
it simply does not promise overlap with unrelated graphics work.

Add explicit resource-use descriptors for:

- Storage image read, write, and read/write at compute stage.
- Sampled reads at fragment and compute stages.
- Buffer transfer writes, compute storage reads/writes, vertex/fragment shader storage
  reads, index reads, and indirect-command reads.
- Depth attachment read/write and read-only depth attachment plus sampled read.
- Image subresource ranges: aspect, mip base/count, layer base/count.
- Buffer ranges or, initially, a conservative whole-buffer state.

Vertex pulling reads an SSBO/device-address buffer in the **vertex shader**; declaring
only a vertex-input stage dependency is insufficient. An indirect argument buffer read
uses draw-indirect stage/access, independently of the shader's instance data reads.

Track the last relevant writes and accumulated reads. Emit read-after-write,
write-after-read, write-after-write, and layout transitions. Treat execution dependency
for read-to-write hazards explicitly even when source access has no writes. Reject
contradictory declarations for overlapping subresources in a single pass, or merge
them into a legal combined state before recording barriers.

Use read-only depth layout for a depth attachment sampled in the same rendering pass,
with depth writes disabled and both access/stage requirements declared. No sampling
from a color attachment being written without a separately supported feedback design.
Do not solve this by adding `GENERAL` to every image and hoping it synchronizes access.

State tracking for a downsample mip chain must distinguish each mip's writes and
subsequent reads. Imported history images keep state across submissions. New resources
start undefined; resources whose previous contents are needed never use discard import.
Keep pass order explicit; a new graph scheduler is not required for this work.

Add graph tests for overlapping mip ranges, compute-to-fragment reads, skinning-to-
vertex reads, compute-to-indirect, depth read+sample, and repeated history reuse. Run
Vulkan synchronization validation on supported development machines.

### 4.3 Capabilities and effective settings

Query required image format usages, blending support, descriptor limits, buffer limits,
and indirect-draw features. Expose a portable capabilities report through Renderer.
Use valid fallback formats or disable the unsupported effect with an explanation.
Vulkan 1.3 alone does not guarantee every optional format/feature combination.

The settings report must distinguish requested and effective values and why they
differ. A fallback is visible in editor and `--graphics-info`; it is not a silent
replacement with a more expensive option.

### 4.4 View and object identity

Introduce opaque `RenderViewId` and `RenderInstanceKey` value types. Suggested contents:

```
RenderViewId: unique view lifetime token, not an ImGui window pointer
RenderInstanceKey: world lifetime + entity index/generation + stable model node/part key
                  + mesh resource generation
```

An entity has index and generation in the current ECS; generation alone is not global
across Worlds. Editor and play views have independent histories. Reflection capture
views never borrow the main camera's history. Non-ECS callers supply an explicit key;
the compatibility draw API without a key renders normally but marks object history
invalid. Do not hash transforms as identities.

World persistent bake identity is a separate concern in G05. Runtime render keys need
not be serialized or stable across application restarts.

### 4.5 Resource cost and lifetime reporting

Report allocated bytes by category: base targets, surface buffers, AO, temporal history,
lightmaps, diffuse probes, reflection probes, clusters, HZB, staging, and pipeline cache.
Distinguish logical payload size from actual allocation size where available. Count
resident CPU copies separately; shared memory does not make duplicate copies free.

Do not create all optional targets at maximum resolution on startup. Allocate on enable
or view resize, with sensible reuse and deferred retirement. Log unsupported capability
or budget fallback once per transition, not once per frame.

## 5. G01: inexpensive spatial anti-aliasing

### 5.1 Public settings

Add `anti_aliasing = none | fxaa | taa` to the existing graphics settings metadata;
make `taa` unavailable with a diagnostic until G10 is implemented, rather than accepting
it as a no-op. Initially publish only implemented choices in generated schema/help.
Use `fxaa` as the proposed low-cost preset setting after measurements.

Extend `Config/Graphics.swgraphics` parsing, layering, Preferences/game settings APIs,
`--graphics-schema`, `--graphics-info`, and overrides together. Keep field spelling
consistent across these paths. Unsupported enum strings and duplicate keys are errors.

### 5.2 Pass contract

Run FXAA after tone mapping and before game UI, editor overlays, and editor GUI. It must
not blur text, crosshairs, or panel borders. If the grid is currently in the scene pass,
move editor grid/gizmos to an appropriate post-AA overlay phase with correct depth.

Do not sample and overwrite the same display image. Use a separate intermediate and
choose the final display target according to AA mode. FXAA operates at output resolution
for this baseline; render-scale upsampling still follows the existing spatial path.

Use a reviewed, license-compatible FXAA implementation or implement the published
algorithm with attribution. Declare explicitly whether luminance/edge detection uses
display-encoded or linear values. Recommended contract: tone map to linear display
RGB, encode to sRGB for FXAA filtering/luma, decode the FXAA result before writing to
an sRGB attachment. Avoid double encoding through automatic sRGB sampling/writing.
If using an UNORM intermediate for encoded RGB, document that exception at the resource.

Expose enable/mode, not dozens of internal thresholds. Keep reference threshold values
beside the shader and change them only with fixture evidence. Add color-ramp and
unchanged-flat-color tests so AA does not alter overall brightness.

### 5.3 Completion gate

- `none` reproduces baseline output within the agreed image tolerance.
- FXAA reduces edge stair-stepping on the thin-geometry fixture.
- Color ramps and flat areas remain correct; game UI pixels are unaffected.
- Resize, render-scale change, backend reload, and editor multi-view operation work.
- GPU pass time and extra memory are reported. No performance guarantee is inferred
  from another engine's measurements.

## 6. G02: material fidelity and auxiliary vertex data

### 6.1 Texture binding model

Replace implicit "one texture means UV0 and the global sampler" assumptions with an
explicit portable texture binding record for each material slot:

```
asset/texture handle
color interpretation (sRGB color or linear data)
UV set (0 or 1 initially)
UV offset vec2, scale vec2, rotation radians
sampler: wrap U/V, min/mag filter, mip filter, requested anisotropy
```

Material base color/emissive color are sRGB textures; normal, metallic/roughness, and
occlusion data are linear. Preserve glTF channel meanings: roughness G, metallic B,
occlusion R. Do not reinterpret a single source image globally when different bindings
need different views/color interpretation.

The UV transform convention is `uv' = offset + R(rotation) * (scale * uv)`, using the
glTF convention and a texCoord override when supplied. Test rotation about the origin,
negative scale, and different UV sets per slot. Preserve Assimp-provided UV indices and
sampler modes; confirm actual importer behavior with fixtures. If a source property
is not exposed by the current Assimp path, report it or extend import explicitly.
Do not silently claim support because the source file extension imports successfully.

### 6.2 Preserve the base vertex ABI deliberately

Keep the current 64-byte Vertex initially. Add optional auxiliary streams for UV1 and
generated lightmap coordinates, with portable MeshData/API descriptors and matching
CPU replay data. Missing UV1 resolves only according to an explicit import policy:
`--check` should flag materials asking for an unavailable UV set.

Generated lightmap UVs are **not** automatically the same as artist-authored UV1.
Do not overwrite a normal map's UV set to fit lightmaps. An auxiliary lightmap stream
may have an associated remapped mesh derivative when seams duplicate vertices.

Audit all shader vertex definitions and skinning copies. Auxiliary streams must follow
vertex remapping, LOD generation, and buffer lifetime. Tangents for normal mapping must
match the normal texture's UV parameterization. For transformed/alternate normal UVs,
either generate the correct tangent basis or use a documented derivative-based basis
with a stable fallback on degenerate derivatives. Simply rotating UVs while keeping an
incompatible tangent frame is incorrect.

When xatlas or another unwrap duplicates a vertex, copy position, normal, UV0, UV1,
color, tangent, and any applicable skin data through its remap. Rebuild affected index
buffers and derived LOD data. Static-only lightmap baking must reject animated/skinned
receivers rather than silently baking their rest pose as permanent lighting.

### 6.3 Samplers, shader consistency, and material authoring

Cache samplers by normalized sampler descriptor. Clamp requested anisotropy to device
limits and expose effective values. Do not create a sampler per draw or every frame.
Preserve identical alpha-mask sampling/cutoff in depth, normal, shadow, and color passes.
Put shared texture/alpha/normal helpers in shader includes instead of copied formulas.

Extend the existing material/override authoring path. If reusable material instances
need a new `.swmaterial` format, specify it before implementation: version header,
base material/reference, named slots, overrides, dependency collection, schema, and
upgrade behavior. Do not introduce this format merely to store pipeline objects.
Runtime material instances contain data; users can edit them without compiling shaders.

Editor: per-slot UV set and transform, texture preview, sampler properties, clear
inherited/overridden values, drag-and-drop, and undo. CLI: a proposed `--material-info
<asset>` inspection command reports imported slots, UV availability, color spaces,
samplers, and warnings; it loads through the same importer as gameplay.

### 6.4 Completion gate

Generated glTF fixtures cover UV0/UV1, repeat/clamp/mirror, nearest/linear, transformed
UVs, masked foliage, mirrored instances, metallic/roughness channels, and emissive.
Compare depth/shadow silhouettes to color. Test hot reload, optimized-cache reload,
backend replay, scene round trips, and packaged resource collection.

## 7. G03: surface prepass and coordinate conventions

### 7.1 Required surface data

Split the current combined scene recording into explicit opaque/masked and transparent
phases. Preserve transparent back-to-front sorting. The optional surface prepass writes:

| Resource | Initial representation | Purpose |
|---|---|---|
| Scene depth | Existing D32 float, reverse-Z | Reconstruction, depth tests, rejection |
| Geometric normal | Octahedral RG16 SNORM, or supported RGBA16F fallback | AO and decal receiver direction |
| Receiver flags | R32 UINT if needed by enabled features | Valid surface, decal mask, baked receiver flag |
| Motion payload | G09 only; separate target | Velocity and temporal validation |

Allocate only needed outputs. Flag bit assignments live in one documented contract
shared by CPU and shaders. A sky pixel has depth zero and no valid-surface flag. Do not
treat clear normal data as a real normal.

The prepass draws opaque and alpha-masked geometry with exactly the same transforms,
skinning, LOD, clipping, alpha UV/sampler, cutoff, and culling as color. It does not put
ordinary alpha-blended glass or smoke in opaque depth. Use inverse-transpose normal
transforms and handle mirrored transforms consistently with existing raster winding.

For the initial normal target, store geometric/interpolated vertex normals, not a full
normal-mapped material normal. This avoids reimplementing all material shading in the
prepass. Document that AO will not resolve subpixel normal-map cavities.

Opaque shading uses read-only depth when the prepass is active. Start with matching
geometry and `EQUAL` depth test, then retain or change to `GREATER_OR_EQUAL` only after
testing hardware and all material variants. Any offset/deformation affecting depth must
be shared across passes. No prepass means the existing opaque depth-writing path.

### 7.2 Coordinates: use these definitions everywhere

Define screen UV in image coordinates: U increases right; V increases downward. Given
an unmodified Y-up projection and the current negative-height Vulkan scene viewport:

```
clip = projection * view * vec4(worldPosition, 1)
ndc = clip.xyz / clip.w
screenUV = vec2(0.5 * ndc.x + 0.5, 0.5 - 0.5 * ndc.y)
```

To reconstruct view position at an actual sampled UV and device depth `d`:

```
ndc = vec4(2 * u - 1, 1 - 2 * v, d, 1)
h = inverse(actualProjectionUsedForThatDepth) * ndc
viewPosition = h.xyz / h.w
```

Reject sky/background before division. For the current infinite perspective projection,
positive forward distance is `near / d`; this shortcut is **not** valid for arbitrary
orthographic or future finite projections. Use inverse projection for general code.
Validate finite results and never divide by a zero sky depth. Culling uses an unjittered
frustum expanded enough to include the jitter footprint once TAA is enabled.

Add CPU and GPU tests projecting/reconstructing points at all four image corners, near
plane, large distance, and an orthographic view. Include non-square render targets,
negative-height viewport, and render scale. A visually plausible center pixel does not
prove the Y convention is correct.

### 7.3 Pass ordering after the baseline additions

Some steps below appear only after their later batch is implemented:

```
environment refresh, resource uploads, skinning
shadow passes
surface prepass                              [if a consumer requires it]
HZB build / visibility                      [G12, if enabled]
cluster construction                        [G11, if enabled]
SSAO + bilateral filter                     [G04, if enabled]
decal accumulation                          [G08, if enabled]
sky + opaque forward shading                [baked/probe bindings sampled here]
transparent forward shading + particles     [preserve ordering; reactive mask for TAA]
TAA resolve                                 [G10, linear HDR, if selected]
bloom                                       [input is resolved HDR when TAA is selected]
tone map / spatial output scaling
FXAA                                        [if selected; mutually exclusive with TAA]
depth-aware editor overlays + game UI
screenshot / editor composition / presentation (honor existing capture contract)
```

Sky rendering must not destroy prepass depth. Debug views can bypass effects, but each
mode must say what it shows. Orthographic editor views can fall back to FXAA when TAA
is unsupported initially; report that effective view setting.

Post-AA world-space editor overlays use unjittered projection for stable lines. If they
sample jittered scene depth for occlusion, map their sample positions into that depth
image using the current jitter/extent convention. Do not compare an unjittered overlay
pixel against an unrelated jittered depth pixel and accept flickering as an editor issue.

## 8. G04: screen-space ambient occlusion

### 8.1 Baseline algorithm

Implement a modest half-resolution hemisphere SSAO first. Do not import an entire
vendor effects SDK to obtain one effect. A later optimized algorithm can replace it
behind the same settings/API if evidence warrants it.

Initial quality descriptors, to be tuned with evidence:

| Mode | Resolution | Hemisphere samples | Blur |
|---|---|---|---|
| off | No resources or dispatch | 0 | None |
| low | Half each dimension | 8 | Small separable bilateral |
| medium | Half each dimension | 16 | Bilateral |
| high | Half each dimension | 32 | Bilateral |

Use a fixed deterministic sample sequence and tiled orientation pattern. Baseline AO
has no temporal history or frame-varying noise. Reconstruct position and normal from
the corresponding full-resolution surface sample. Select a foreground representative
when downsampling rather than averaging depth across a silhouette.

Generate cosine-weighted hemisphere directions in a basis around the normal; use
stratified radial distances biased toward the origin. For each sample `Q = P + R*t*dir`,
project Q, reject invalid/out-of-screen positions, sample scene depth, and reconstruct
the sampled surface's positive view distance. It occludes when the sampled surface is
closer than Q by more than bias, with a smooth range weight that approaches zero when
its view-distance separation from P exceeds radius R. Normalize by accumulated valid
sample weights. Out-of-screen samples do not count as guaranteed occlusion.

Compute `AO = clamp(1 - occludedWeight / validWeight, 0, 1)`, with AO=1 when no valid
weights exist. Use a world-unit radius, initially 0.3 m for shooter-scale contact detail;
bias should scale modestly with depth/precision and remain capped relative to radius.
Expose scene AO radius/intensity through registered environment metadata, with finite
positive bounds. Quality remains a user graphics preference.

Blur and upsample with depth and normal rejection. A background pixel must not darken
the foreground silhouette. Check very thin objects and a camera close to walls.

### 8.2 Lighting integration

Apply screen AO to **diffuse indirect lighting only** in the baseline. Do not multiply
the final shaded pixel: direct light, emissive, transparent color, and sky must not be
darkened by this pass. Leave specular occlusion as a separately tested future feature.

For unbaked surfaces, use a documented conservative combination such as
`min(materialOcclusion, screenAO)` rather than accidentally multiplying the same cavity
twice. Preserve existing material-occlusion behavior when screen AO is disabled.
When lightmaps already contain static occlusion, start with screen AO disabled on baked
receivers using the receiver flag. This sacrifices dynamic contact on those receivers
until a separate dynamic-occluder approach is implemented, but avoids pervasive double
darkening. Do not claim the initial mode solves dynamic contact on baked floors.

### 8.3 UI, checks, and acceptance

Add `ambient_occlusion = off | low | medium | high` to shared graphics settings.
Expose AO-only debug view and requested/effective resolution/sample count. Validate
radius/intensity and reject NaN/Inf. Show per-pass GPU time and allocation size.

Tests: empty background white AO, open plane near-white, corner contact, silhouette
leak prevention, disabled output unchanged, direct-lit/emissive patch unchanged,
render-scale invariance in world units, and no flicker on a static camera sequence.

## 9. G05: bake contracts, persistent identity, and static diffuse lighting

This is the largest baseline batch. Split it into independently reviewable subcommits:
identity/schema; immutable geometry snapshot; unwrap; integrator; serialization/cache;
runtime sampling; editor/CLI/package; evidence. Keep the feature off until complete.

### 9.1 Proposed registered components

Use these names consistently unless newer code already provides equivalent concepts:

| Component | Proposed fields and defaults | Meaning |
|---|---|---|
| `BakeGeometry` | `binding_id` empty until authored; `mobility dynamic`; `cast_indirect true`; `receive_lightmap false`; `texels_per_meter 16` | Explicit static geometry eligibility and per-instance bake binding |
| `BakeLight` | `include_indirect true` | On an entity with Light; its contribution to static bounce is baked, direct lighting remains runtime |
| `BakedLighting` | `asset` empty; `enabled true` | One scene binding to a bake manifest |
| `DiffuseProbeVolume` | See G06 | Dynamic-object diffuse illumination |
| `ReflectionProbe` | See G07 | Local specular illumination |

`mobility` is `static | dynamic`. A receiver must be static and have valid renderable
geometry. Dynamic geometry never becomes a bake occluder just because it currently
isn't moving. Old scenes without these components retain their existing lighting.
Do not infer staticness from absence of a Rigidbody.

`binding_id` is a persistent 128-bit identifier serialized as 32 lowercase hexadecimal
digits. Use a general persistent scene identifier if one already exists and satisfies
these requirements; otherwise implement this narrow binding field first. Names and ECS
indices are not adequate: duplicates and reordered scene text must not attach another
wall's atlas region.

Editor creation/duplication assigns fresh IDs. Prefab instances need distinct IDs: store
a stable instance ID in the scene and derive child binding IDs from instance ID plus
prefab-local ID, with a documented hash namespace. Duplicating a prefab instance changes
its instance ID, not the asset's child IDs. Scene reload preserves IDs. `--check` reports
missing/duplicate IDs on bake participants; an explicit proposed `--lighting-assign-ids`
tool may edit files, but checking and baking must never silently rewrite source scenes.

For multi-part models, the bake key is persistent binding ID plus a stable imported
node/mesh key and source geometry hash. An importer reorder invalidates the bake if
stable keys cannot be preserved. Never accept the wrong part based on vector position.

### 9.2 Authored data and generated data

Proposed layout inside a project, where `Arena` below is merely an example scene name:

```
Content/Lighting/Arena.swlighting            readable manifest and bake settings
Content/Lighting/Arena/Lightmap_000.ktx2     linear HDR atlas payload
Content/Lighting/Arena/Receivers.swbake      derived UV/mesh bindings, if needed
Content/Lighting/Arena/Probes.swprobe        G06 diffuse probe payload
Content/Lighting/Arena/Reflection_*.ktx2     G07 filtered cube payloads
```

Do not embed megabytes of texels or vertices in `.swscene`. The scene references the
manifest as an asset. Generated payloads may be checked in or reproduced offline;
project packaging must work with either policy once the artifacts exist.

Use a text manifest with version header `swlighting 1`, documented parser rules,
quoted path/string support, duplicate/unknown-key errors, and explicit resource records.
Fields include scene identity, source hash, algorithm version, color convention,
settings, receiver bindings, payload hashes/dimensions, dependencies, and tool version.
Specify the exact grammar in generated `docs/LightingSchema.md` before adding examples
to production scenes. Do not invent a second ad hoc parser if a compatible existing
asset-parser utility can be reused.

Prefer standard KTX2 payloads with uncompressed RGBA16F initially. Extend the loader's
supported formats/shapes as required; a file extension alone is not loader support.
BC6H or other HDR compression is optional later, based on format support and measured
error. BC7 is not a substitute for an HDR lightmap format.

For a proprietary `.swbake`/`.swprobe` payload, use explicit little-endian scalar fields,
magic, version, sizes, counts, and checked offsets. Document every field and alignment.
Use bounded reads and overflow-safe arithmetic. No pointers, GLM memory dumps, native
`size_t`, compiler padding, or C++ struct serialization.

Provide inspect/export/import commands in section 18. Lossless export uses a readable
JSON structure with enough significant digits for float32 round trips and finite-number
validation. Large texture previews can be PNG; HDR export must preserve HDR values,
not silently clamp them to PNG. A preview is not a lossless decompiler.

### 9.3 Invalidation and atomic replacement

Hash all dependencies that change a result: geometry and node transforms; instance
transforms; UV remap; material bindings, alpha and emissive; static light values and
transforms; environment map/intensity/rotation; probe settings; algorithm/unwrap
versions; bake quality/seed. Record logical source paths and content hashes, not absolute
developer paths or modification times alone.

Write a complete bake into a temporary sibling directory. Validate payloads and hashes,
then publish a new manifest/generation atomically. A cancelled or failed bake leaves
the last valid generation available. Retire old GPU resources only after completion.
During hot reload, decode/upload a complete replacement before switching bindings.

Stale bake: editor shows the reason and offers rebake; interactive play falls back to
global IBL for invalid receivers and reports once. `--check` treats a referenced stale
or missing required bake as an error for packaging. A project can remove/disable the
bake explicitly if it wants to ship without it. Moving a static receiver at runtime
invalidates that receiver's bake binding; never drag baked shadows along with it.
Changing baked lights/environment invalidates the scene bake, not only one receiver.

### 9.4 Immutable geometry snapshot and BVH

Create a portable CPU `LightingBakeScene` snapshot containing indexed render triangles,
world transforms, normals, material bindings/samplers, decoded relevant textures,
static light descriptors, and environment sampling data. Include primitives and all
eligible imported model parts. Use actual render geometry, not simplified physics
collision hulls. Detect degenerate triangles, invalid indices, non-finite transforms,
and singular transforms before tracing.

Build a deterministic CPU BVH over world-space triangles. A tested binned-SAH binary
BVH with small leaves is sufficient initially. Keep intersect, any-hit, and alpha-tested
visibility paths shared. Bound worker count, memory, and progress updates. Support
cancellation between tiles/sample chunks. Do not add Embree or require hardware ray
tracing unless later measurements justify another dependency.

Honor masked alpha and double-sided rules using the same UV transforms/samplers as
runtime. Baseline transparent blend surfaces are excluded from bake occlusion and
indirect transport, with a clear warning; do not bake glass as an opaque black wall.
Specify wrap/filter sampling in CPU tests. Offset ray origins by a scale-aware small
epsilon; large fixed offsets cause light leaks through thin walls.

### 9.5 Unwrap and atlas construction

Use xatlas if generated non-overlapping lightmap UVs are needed; fetch a pinned upstream
revision through a dedicated `cmake/` dependency file, preserve its license, and expose
no xatlas types in public engine APIs. First check whether another unwrap dependency
already exists. Keep generated results cached by geometry and unwrap settings.

Authored non-overlapping lightmap UVs may be selected explicitly. Validate them rather
than assuming every UV1 is suitable. Generated charts duplicate vertices as required;
apply the complete attribute remap from G02. Per-instance atlas placement is separate
from reusable mesh-local chart UVs, so two instances do not share lighting accidentally.

Initial defaults: 16 texels/meter, 1024 atlas dimension, maximum 2048 for the baseline,
and quality-controlled gutter width of at least four base-level texels. These are starting
points, not universal recommendations. Report coverage, wasted area, atlas count, and
predicted memory before starting an expensive bake. Reject exceeding the user budget
unless they explicitly change settings; never silently reduce every surface's quality.

Rasterize triangles into atlas texels with position, normal, and owner/chart identifiers.
Resolve coverage robustly and dilate valid chart edges into gutters. Generate mipmaps
without mixing unrelated charts: constrain maximum LOD to protected gutter coverage,
or use chart-aware mip generation and adequately padded layouts. Blindly generating a
full image mip chain causes distant light leaks. Test small charts and atlas boundaries.

### 9.6 Exact lighting convention and no double counting

Define stored diffuse lighting as `B(P,N) = E(P,N) / pi`, linear scene RGB, before tone
mapping and exposure. Current environment diffuse filtering uses this convention.
The stored value is incident diffuse lighting, **not** receiver albedo multiplied by it.

For the initial hybrid renderer:

- Runtime analytic lights continue to provide their existing direct diffuse/specular
  lighting and shadows.
- A lightmap replaces global diffuse environment irradiance at a valid baked receiver.
- It contains occluded environment lighting, static diffuse interreflection, and emissive
  contributions. It excludes the receiver's direct analytic-light term.
- A diffuse probe replaces global diffuse environment irradiance for a dynamic receiver.
- Local reflection probes replace/blend the global prefiltered specular source; they
  do not add another copy of it.

Adapt the current IBL shader around one selected `diffuseIrradiance` input and one selected
`prefilteredSpecular` input. Preserve the existing BRDF/multiscatter energy terms. Feed
the selected diffuse input into the existing irradiance-dependent terms rather than
adding a second Lambert term beside them. A non-directional lightmap is tied to the
baked geometric normal; normal mapping cannot correctly relight it from new directions.
Record that limitation instead of fabricating directionality from one RGB value.

### 9.7 Initial offline estimator

For each valid lightmap texel, sample a cosine-weighted hemisphere about the receiver
normal. The average of incoming radiance samples directly estimates `E/pi`:

```
B = (1 / sampleCount) * sum(traceIncoming(receiver, cosineDirection, configuredBounceCount))

traceIncoming(origin, direction, remainingDiffuseReflections):
    if ray misses static scene: return sampled linear environment radiance
    hit = first valid opaque/masked static surface
    result = hit.material.emissive
    if remainingDiffuseReflections > 0:
        result += diffuseReflectance(hit) * analyticDirectAtHitWithVisibility
        nextDirection = one cosine-weighted sample around hit.normal
        result += diffuseReflectance(hit) * traceIncoming(
            hit, nextDirection, remainingDiffuseReflections - 1)
    return result
```

The receiver's analytic direct light is absent because the first operation is a ray
into its hemisphere. Analytic illumination of a secondary hit *is* indirect light for
the receiver and must be included. Environment rays blocked by geometry do not also
receive unoccluded global IBL afterwards.

Match the engine's existing direct-light scale. Its diffuse convention makes a facing
white surface under a unit light approximately unit response, rather than inserting an
extra `1/pi`. Thus `analyticDirectAtHitWithVisibility` uses the existing light color,
attenuation, cone, and cosine convention. If converting to physical incident radiance
inside a general integrator, derive the corresponding pi factor once and test it.

Baseline bounce material is a documented diffuse approximation, for example
`rho = baseColor * (1-metallic) * 0.96`, clamped to a non-amplifying range. It does not
simulate glossy/specular caustics or transmission. Do not claim agreement with the full
runtime BRDF for those phenomena. A zero-bounce limit still includes environment misses
and first-hit emission; define the UI bounce count as the number of allowed diffuse
surface reflections and test 0/1/2 explicitly to avoid off-by-one ambiguity.

Use fixed per-texel seeds derived from binding/chart/texel/sample, independent of worker
completion order. Start with preview 64 and final 512 samples per texel, up to an explicit
user maximum; measure convergence. Tile work so cancellation and memory remain bounded.
Small bright emissive surfaces can converge poorly without importance sampling: report
this limitation and add light/emitter sampling with correct MIS only in a tested follow-up.
Avoid recursive exponential branching: one continuation direction per path/sample,
with many independent paths at the receiver.

### 9.8 Bake tests and acceptance

- Constant environment, no occluders: B equals environment RGB within sampling tolerance.
- Black environment and no lights/emission: B is zero.
- A directly lit plane with no bounce surfaces gets no extra baked direct light.
- Colored secondary wall creates a colored bounce without changing receiver albedo.
- Closed box blocks the environment; alpha-mask holes transmit visibility.
- Nonuniform transforms, repeated model instances, prefab duplication, and scene reorder
  preserve correct bindings or explicitly invalidate them.
- Cancelled jobs preserve last good artifacts; dependency edits mark them stale.
- CPU-only bake and inspect work without Vulkan or Steam initialization.
- Editor bake/progress/cancel, CLI bake, hot reload, backend replay, and packaging all work.

## 10. G06: diffuse probe volumes for moving objects

### 10.1 Component and payload

`DiffuseProbeVolume` fields: local box half-extents, spacing in world meters, blend
distance, priority integer, enabled boolean, and persistent binding ID. Transform
positions/orients the volume; reject zero/negative/singular scale. Use a regular grid
initially, with a documented maximum probe count and predicted memory before bake.

Store nine real spherical-harmonic coefficients per color channel, linear float32 in
the inspectable canonical payload. GPU storage can use float16 after error tests.
Also store validity and directional visibility data needed for interpolation rejection.
Do not clamp signed SH coefficients; clamp reconstructed negative lighting to zero.

Use a documented basis/order, for example:

```
Y00, Y1-1(y), Y10(z), Y11(x), Y2-2(xy), Y2-1(yz), Y20(3z²-1), Y21(xz), Y22(x²-y²)
```

Include normalization constants and test vectors in the format reference. This is a
mathematical basis ordering, not an instruction to change the engine's Y-up world.

Project incoming radiance using uniformly distributed sphere directions:
`L_i = (4*pi/N) * sum(L(direction)*Y_i(direction))`. Convert to coefficients of
irradiance/pi with band factors `[1, 2/3, 1/4]` for bands 0,1,2. Runtime evaluation
uses the receiver's world normal in the same basis. A constant environment must evaluate
to the same constant at every normal; this catches normalization and pi mistakes.

### 10.2 Probe placement and visibility

Do not accept probes buried in walls. Bake occupancy/near-surface tests, mark invalid
points, and visualize them. Initial automatic relocation may be omitted if authors can
edit volume bounds/spacing and invalid points are clearly reported. A grid of invalid
probes must not silently become black ambient lighting.

Trilinear interpolation alone leaks light through walls. Store a small directional
distance representation per probe (for example an octahedral map of first-hit distance
and second moment). Document resolution, units, miss distance, filtering, and whether
moments are baked from jittered rays. For a receiver direction, reduce a probe's weight
when receiver distance exceeds the visibility estimate. A moment-based bound requires
a variance floor and bias tests; do not copy constants without checking wall thickness.

Combine trilinear weight, validity, visibility weight, volume boundary blend, and
priority policy; normalize the surviving weights. If all weights vanish, use the nearest
valid visible probe within the volume if available, otherwise the global environment.
Do not divide by zero or choose a probe on the other side of a wall merely because it
is closest in Euclidean distance.

Evaluate initially per object at its bounds center for small objects, with an option
to evaluate per fragment on large dynamic objects. State the limitations of per-object
sampling; a long object spanning two rooms needs more than one illumination sample.
The GPU data layout should permit per-fragment evaluation without changing authored data.

### 10.3 Acceptance

Move a neutral object from a bright room through a doorway into a dark room. Check
smooth changes, no gross wall leaks, no sudden black frames, sensible behavior outside
volumes, and stable results on volume boundaries. Tests cover one-cell volumes, missing
neighbors, overlapping priorities, rotated volumes, invalid payloads, and constant SH.
Provide probe spheres, validity colors from theme/debug palette, and SH/visibility
inspection in editor and a machine-readable `--lighting-info` report.

## 11. G07: local reflection probes

### 11.1 Authored representation

`ReflectionProbe` fields: persistent binding ID; enabled; local box half-extents;
capture offset; blend distance; integer priority; resolution `64 | 128 | 256 | 512`;
near/far capture distances; and generated asset reference. Default to 128 and offline
capture. Transform defines the box and capture frame. Capture point must be finite and
inside its influence box for the baseline. Reject nonpositive extents and invalid planes.

Capture offline from six views using the existing forward shading and environment
conventions. Include static opaque/masked geometry, direct lighting/shadows, lightmaps,
and the global environment. Exclude local reflection sampling during capture to prevent
recursive/self-dependent results. Exclude UI, editor grid, transient particles, viewmodel,
and moving actors. Transparent capture is a later explicitly tested capability.

Reuse the existing environment prefilter/BRDF convention. Cubemap face orientation must
match environment conversion and sampling; use an axis-colored fixture to verify all
faces and seams. Capture linear HDR before exposure/tone mapping/bloom/AA. Do not bake
an editor screenshot into a reflection texture.

Reflection capture requires a GPU in this baseline; CPU diffuse baking does not. CLI
and editor must report this difference. Captures run as explicit jobs with progress and
cancellation, never automatically each game frame. Global specular remains available
until a replacement is completely loaded. Do not claim offline probes reflect moving
players or newly destroyed geometry.

### 11.2 Texture API changes

Extend portable texture descriptors with shape (`2D`, `cube`, or supported array),
layer/face counts, mip descriptions, and validated byte ranges. Add format/shape support
queries and correct Vulkan views/descriptors. Cube samplers are not interchangeable
with the existing 2D bindless table. Preserve resource IDs and replay on backend switch.

KTX2 loading and packaging must preserve all faces and mip levels. Test orientation,
roughness-to-mip mapping, final mip, truncated payloads, and face count errors. If arrays
are used for efficient binding, define how differing resolutions are pooled rather than
silently padding every probe to the largest requested resolution.

### 11.3 Box projection and blending

For world position P and reflected direction R, transform the ray into the probe box's
local space. Intersect with the box and find the first positive exit distance. Transform
the hit back to world space, then sample the cube in the direction from capture point
to that hit (converted to the capture frame). Do not normalize a transformed ray before
using a parameter whose units were derived from the unnormalized direction.

Parallel ray components need safe slab handling. If P is outside the usable influence
box, no positive exit exists, or the transformed direction is degenerate, use a defined
fallback, never NaN. This approximation improves box-shaped rooms; it does not reproduce
arbitrary geometry or mirror-accurate parallax.

Start with at most two local probes per shaded object, selected by bounds overlap,
priority, and stable tie-break ID. Compute per-fragment boundary weights within those
candidates. Define behavior for large objects intersecting more volumes; a later spatial
lookup can improve selection without changing asset data.

Example blend contract for surviving candidates with nonnegative raw weights wi:

```
sumW = sum(wi)
localCoverage = clamp(sumW, 0, 1)
local = sum(wi * sampleProbe(i)) / max(sumW, epsilon)
specularSource = localCoverage * local + (1-localCoverage) * globalEnvironment
```

When sumW is zero, skip local evaluation and use global. Select highest-priority overlap
group first, then blend equal-priority probes; document boundary behavior. Avoid adding
each probe's full lighting on top of global IBL. Blend in linear HDR before the existing
specular BRDF response. Keep roughness filtering identical across local/global sources.

### 11.4 Budget and acceptance

A 128-square RGBA16F cubemap with full mips is approximately
`6 * 128² * 8 * 4/3 = 1 MiB`; 256 is about 4 MiB; 512 about 16 MiB, before allocation
padding or extra CPU copies. This makes unbounded high-resolution probes unacceptable.
Expose count, resolution, resident bytes, and a project bake budget.

Test two differently colored rooms, roughness sweep, metallic sphere, doorway blending,
rotated box, out-of-volume fallback, capture invalidation, hot reload, and a packaged
game. Provide probe-bound/capture-position gizmos and local-probe-selection debug view.
Unbaked/stale/invalid probes must be actionable `--check` diagnostics.

## 12. G08: decals that artists and gameplay can both use

### 12.1 Baseline scope and components

Implement box-projected decals for static/rigid receivers first: bullet marks, painted
signs, grime, stains, and roughness variation. These are a high visual return for a bounded
shooter map. Skeletal attachment and normal-map decals are later extensions, not silently
claimed by this baseline.

`Decal` component fields: material/texture asset, local size vec3, opacity, tint,
integer sort order, receiver layer mask, angle fade threshold, enabled. Reuse Transform
for pose. Define projection along local -Z, local bounds [-0.5,+0.5] after applying size,
and UV orientation in the schema and editor gizmo. Reject zero/negative sizes, invalid
asset types, non-finite values, and opacity outside [0,1].

Expose a runtime data API for spawning/removing/updating decals through the normal
entity/command path. Add an optional lifetime/fade policy in the appropriate gameplay
component, not a wall-clock lifetime hidden inside Vulkan. Pool transient decal records
and impose a configurable visible count/budget. Stable IDs settle equal sort orders.

### 12.2 D-buffer contract

After depth/normal prepass, accumulate decal modifications into separate targets; opaque
forward shading samples them before lighting. This avoids pretending a bullet mark is
an already-lit transparent plane. Baseline targets:

- Albedo override: premultiplied linear RGB and alpha, RGBA8 UNORM.
- Optional roughness/metalness override: premultiplied roughness R, metalness G, unused B,
  shared coverage A, RGBA8 UNORM. Baseline material override changes both values together;
  independent coverage would need a different explicitly designed representation.
- Optional emissive override/addition: allocate an HDR target only when used; define
  compositing separately from albedo. It must not be multiplied by lighting or AO.

For albedo, compose ordered decals with premultiplied-over blending and apply once:
`surfaceAlbedo = baseAlbedo*(1-accumulatedAlpha) + accumulatedPremultipliedRGB`.
Decode sRGB decal color before accumulation. Clear targets to zero coverage each frame.
Do not sample from the same accumulation target while writing it.

Use screen bounds/scissor or box rasterization plus depth reconstruction to restrict work.
Handle the camera inside a box without dropping the decal. Reconstruct world position,
transform into decal space, reject outside-box samples, apply receiver mask, and fade
at grazing angles using the prepass normal. Do not project through the foreground onto
another room's visible wall merely because both lie inside an oversized box.

All decal targets require queried format/blend support. If independent attachment blend
state is unavailable, use legal separate passes or disable the optional channels with
a capability report. Never assume optional Vulkan blend features are enabled.

Transparent surfaces do not receive these baseline decals. A decal on a moving door
can be parented so its box follows the door; receiver filtering must prevent it affecting
nearby unrelated geometry. Precisely attached skinned bullet marks need another approach.

### 12.3 Editor and tests

Content browser: decal material preview, drag into viewport to create a Decal entity,
surface alignment, box gizmo, inspector fields, sort/mask controls, and undo. CLI:
registered scene schema/checks plus the common screenshot/debug modes.

Tests: ordering, partial alpha, linear color correctness, grazing fade, receiver masks,
camera inside volume, zero extents rejected, moving-door attachment, transparent
exclusion, no depth writes, and deterministic pruning under budget pressure. Show decal
coverage/overdraw debug view; measure overlapping decals, not only one decal on a wall.

## 13. G09: motion vectors and history ownership

Do not implement TAA first and then patch individual ghosts. Establish correct motion
and history lifetime before resolving any previous color.

### 13.1 Frame state

For each RenderViewId retain previous submitted camera view/projection, jitter, render
extent, history generation, and validity. Keep current and previous **rendered** object
poses keyed by RenderInstanceKey. Physics `PreviousTransform` is for fixed-step
interpolation; it is not necessarily last frame's interpolated rendered transform.

Capture history from submitted frames, not simulation ticks or attempted draws that
were abandoned during resize. Snapshot after world-parent transforms and render
interpolation. Do not advance one view's history when rendering another view.

Reset view history on view creation, world/scene replacement, Play/Stop, backend reload,
resolution/render-scale change, projection mode change, explicit camera cut/teleport,
and incompatible debug/AA mode changes. Mark individual object history invalid on spawn,
mesh generation change, topology change, discontinuous animation reset, or missing prior
visibility. LOD transitions may conservatively invalidate object history initially.

### 13.2 Rigid and skinned motion

For a rigid vertex, compute current and previous clip positions using current/previous
rendered world and view-projection matrices. For a skinned vertex, previous position
must use previous rendered joints plus previous world transform. Merely applying last
world transform to the current deformed vertex omits limb motion.

Possible implementation: retain previous rendered joint palettes by stable instance key
and compute current/previous skinned positions in one pass. Alternatively retain a
previous-position stream with explicit instance/vertex mapping. Bound history memory
and retire data for dead instances. Do not read prior frame's transient skinned offset
without proving identity and lifetime.

First-person weapon rendering must use the correct current/previous weapon projection
if its FOV/view differs from the world. Either represent that as a separate supported
view/layer with correct composition or generate correct motion for its actual clip
transform. Do not hide camera/weapon snapping behind TAA blur.

### 13.3 Motion payload and exact jitter convention

Baseline motion target: RGBA16F (verify format support). XY is normalized screen-UV
velocity, Z is expected previous positive view distance for depth rejection, W is a
history-valid flag. Half-float distance is sufficient only within the declared map/view
range; use a more suitable encoding/format if ranges exceed its finite/precision limits.

Store **unjittered** motion:

```
motion = currentUnjitteredUV - previousUnjitteredUV
```

Define jitter J in screen pixels, positive right/down, using centered Halton(2,3),
initially an eight-sample cycle. With the negative-height viewport and Y-up projection,
apply jitter in clip space as:

```
clip.x += ( 2 * J.x / renderWidth)  * clip.w
clip.y += (-2 * J.y / renderHeight) * clip.w
```

This produces screenUV shift `J/renderExtent`. Consequently, for current sampled UV:

```
previousHistoryUV = currentUV - motion
                  + previousJitter/previousExtent - currentJitter/currentExtent
```

This formula assumes the stored velocity is unjittered; do not also include jitter in
velocity and subtract it again. Histories reset on extent changes in the baseline.
Rasterization/reconstruction uses the jittered current projection; identity/motion
calculation also retains the unjittered matrices. Perspective interpolation of previous
clip coordinates must reproduce the previous location of the current surface sample.

Depth history stores positive linear view distance. Compare against the expected previous
distance from the same surface, not current depth: camera translation changes depth even
on a perfectly stationary wall. Samples with invalid prior clip W or outside prior view
must reject history. Sky can reject history initially; later use rotation-only reprojection
for infinite background and test it separately.

### 13.4 Transparent/reactive surfaces

Opaque velocity cannot describe smoke in front of a wall. Add a reactive mask written
by transparent/particle/emissive-changing contributions, with a defined maximum blend
rule and queried format support. Fallback to an appropriate supported format or separate
pass. Never overwrite the opaque depth history with arbitrary blended particle depth.

Initially strongly reduce or reject history where reactive coverage is high. This may
leave some particle aliasing but must prevent long ghost trails. Document how Effekseer
or other effects integration supplies reactive data; do not edit an external dependency
just to access its private buffers. If the integration cannot supply it, implement a
conservative coverage mask through its supported render hook.

### 13.5 Motion acceptance tests

Use debug captures of velocity, previous expected depth, validity, and reactive mask:
static camera/object (zero motion), camera rotation, camera translation, moving parent,
interpolated physics object, animated joint, viewmodel, newly spawned entity, destroyed
and reused ECS index, LOD change, teleport, two editor views, and renderer hot reload.
For synthetic projected vertices, compare GPU velocity against CPU reference UVs.
Do not advance to G10 while these tests are ambiguous.

## 14. G10: temporal anti-aliasing, without premature upscaling

### 14.1 Resolve inputs and resources

First implement TAA at the current internal render resolution. Render scale still uses
the existing spatial output scaler. TAA is not automatically a temporal upscaler.

Inputs: current linear HDR scene color after world transparency/particles, current depth,
motion/previous-depth payload, reactive mask, previous resolved HDR, previous linear
depth, and view validity. Output: resolved HDR and current depth history. Use separate
read/write history resources. Bloom consumes resolved HDR; UI never enters history.

Use RGBA16F history color and R32F positive-depth history initially, with invalid/sky
sentinel defined. Two history slots are sufficient on a single ordered queue **only if**
graph dependencies protect both reading and later overwriting those slots across
submissions. CPU frames in flight do not remove GPU dependencies. Alternatively use
more slots with explicit completion tracking; never reuse by swapchain index alone.

### 14.2 Baseline resolve algorithm

1. If view/history invalid, copy current color and write current depth history.
2. Reproject with the exact convention in G09. Reject out-of-range UV and invalid motion.
3. Sample previous depth and compare to expected previous depth. Initial tolerance can
   be `max(0.01 m, 0.01 * expectedDepth)`, then tune against sloped/thin surfaces. This
   is a starting point, not proof against all disocclusions.
4. Fetch previous color. Compute current 3x3 neighborhood min/max and variance in a
   luminance/chroma space such as YCoCg. Clip history toward the neighborhood's plausible
   region; document the chosen variance scale (initially 1). Preserve HDR values.
5. Compute history weight from validity, motion, local disagreement, and reactive mask.
   Initial upper bound 0.9 at rest, reduced toward 0.7 for motion and toward 0 for reactive
   pixels. Parameters are internal defaults until justified as user controls.
6. Blend in linear HDR. Write current depth, not blended depth. Guard NaN/Inf and invalid
   reprojection before they contaminate future frames.

Depth sampling/rejection near edges needs care: bilinear averaging of unrelated surfaces
can accept a foreground/background mismatch. Start with point depth and consistent
candidate selection; add better edge-aware reconstruction only with sequence tests.
Avoid sampling history color from one surface while validating another surface's depth.

The current scene is not pre-exposed, so manual display exposure changes need not rescale
linear HDR history. If a future implementation adds pre-exposure, store exposure with
history and rescale correctly or invalidate it. Large lighting/bake/probe discontinuities
should invalidate affected history or conservatively reset the view.

Optional sharpening is off initially. A strong sharpen can conceal blur while amplifying
ringing and flicker. Never stack FXAA automatically after TAA.

### 14.3 Acceptance and known limits

Compare sequences, not only stills. Include subpixel wires/fences, specular highlights,
moving silhouettes, animated characters, rapid 180-degree turns, muzzle-flash-like
emission, smoke, weapon motion, camera cuts, and indoor/outdoor transitions.

Measure edge variance on static regions and trail energy behind moving objects using
deterministic masks. Set documented tolerances from reference sequences; do not demand
bit-identical GPU screenshots across vendors. Inspect motion at actual playback speed.

TAA must be optional: some players prefer FXAA or none for competitive clarity. Accept
the batch only when no persistent ghosts, stale editor-view history, brightness drift,
UI blur, or newly visible-object smears remain in the required fixtures. Record remaining
limitations explicitly; do not market this baseline as equivalent to a mature upscaler.

## 15. G11: clustered local lighting

### 15.1 Keep a reference implementation

Retain the existing brute-force light loop behind a debug/validation setting. Clustered
shading changes which lights a fragment tests; it must not change their BRDF, attenuation,
shadow indices, or color. Directional lights remain a separate short list.

Do not increase the existing scene light limit until buffer layouts, upload, shadows,
validation, and tests support it. First make current accepted lights cheaper to shade.
Report any global limit separately from a per-cluster list limit.

### 15.2 Grid and depth slicing

Initial grid: 32x32 render-pixel tiles and 16 logarithmic positive-view-distance slices.
Use actual render extent, not window extent. Define a finite `cluster_far` in scene
settings because the camera has an infinite far plane. Initial bounded-map default:
250 meters, editable and validated greater than near plane.

For positive view distance z within range:

```
slice = clamp(floor(log(z / near) / log(clusterFar / near) * sliceCount),
              0, sliceCount - 1)
```

Compute cluster frusta/bounds using the same projection conventions as rendering.
Conservatively include jitter footprint. Orthographic views use a documented linear
depth partition or the brute-force fallback. For z beyond clusterFar, use brute-force
lighting initially; do not omit far lights or pretend clamping is always conservative.

### 15.3 Build lists and handle overflow correctly

Start with one workgroup per cluster, testing the current accepted local lights against
the cluster volume. Point lights use bounding spheres. Spot lights can initially use
conservative bounding spheres, followed by cone checks in fragment shading. A light
without a finite effective influence radius must be considered globally; do not invent
a radius that cuts off visible energy without a documented attenuation threshold.

Use fixed-capacity lists first, e.g. 64 uint32 indices per cluster plus count/overflow.
This is simple to validate. At 1920x1080 the initial grid has `60*34*16 = 32640` clusters,
so indices alone occupy about 7.97 MiB. At 4K it is about 31.88 MiB; adapt tile size or
select fallback when the configured budget is exceeded. Report effective grid dimensions.

If a cluster overflows, flag it and run the full accepted local-light loop for fragments
in that cluster. Never silently keep only the first N lights: lights would disappear or
flicker when submission order changes. A later compact list/prefix-sum design can remove
this fallback after tests, but overflow correctness remains mandatory.

Use conservative full cluster volumes for the initial lists. Restricting lists to opaque
tile min/max depth can omit lights needed by transparent surfaces in front of/behind
that depth range. Transparency must use a valid list or the reference path.

### 15.4 Integration and completion gate

Graph: light upload -> compute cluster writes -> fragment shader reads. Light/shadow
indices refer to the same immutable frame light table. Keep original identities through
sorting and packing. Do not let a culled camera light erase needed shadow data for other
views or captures.

UI/CLI: cluster debug heatmap, overflow count, list memory, accepted light count, average/
maximum candidate count, and requested/effective method. Authored setting belongs to
the shared graphics/environment system, not a renderer-only magic constant.

Compare clustered and brute-force HDR output under randomly generated lights, point
and spot lights, shadows, transparent particles, camera near/far extremes, orthographic
fallback, empty lists, and deliberate overflow. Require a measured win on a light-dense
fixture; use brute force automatically below a measured crossover if appropriate.

## 16. G12: batching, HZB, indirect drawing, and uploads

These are separate optimizations. Land them separately with before/after evidence.
More work on the GPU is useful only if it reduces the actual bottleneck.

### 16.1 Instancing first

The current draw path issues `vkCmdDrawIndexed(..., instanceCount=1, firstInstance=drawIndex)`
and indexes DrawBuffer through `gl_InstanceIndex`. Compatible repeated geometry can
use multiple instances if their DrawData records are contiguous and shader indexing
still matches. Preserve stable RenderInstanceKey separately from physical buffer order.

Group only compatible draws: pipeline/material feature variant, mesh/index buffer and
LOD, raster state including mirrored winding, and any resource binding constraints.
Per-instance transforms/tints/material table indices can differ only where the shader
already reads them from per-instance data. Do not accidentally force distinct materials
to the first instance's texture. Skinned instances with distinct deformation buffers
need an explicit supported binding strategy; rigid instancing does not solve them.

Do not reorder alpha-blended draws merely to batch them; maintain the required order or
use a separately justified transparent batching policy. Shadow batching uses each shadow
view's visibility and material-mask requirements. Camera invisibility does not imply
shadow invisibility.

Tests compare submitted/rendered instances, per-instance materials, negative scale,
LOD transitions, stable motion, and draw-call counts. Report both CPU submission time
and GPU cost; a lower draw-call count alone is not proof of faster rendering.

### 16.2 Conservative reverse-Z HZB

Build a mip pyramid from current depth after a valid occluder pass. For reverse-Z, near
depth is larger and clear/far is zero. To prove an object's nearest possible depth is
behind **all** depth samples covering its projected rectangle, each parent stores the
**minimum** child depth, including clear zeros:

```
HZBparent = min(all covered child depths)
occluded only if objectNearestDepth < minCoveredHZBDepth - conservativeBias
```

This is deliberately conservative. MAX reduction would retain the nearest occluder and
could hide an object behind a pixel that does not actually cover it. An uncovered sky
sample reduces the minimum to zero and prevents a false occlusion decision.

Handle odd dimensions by reducing all valid children. Choose a mip and sample every
texel whose footprint intersects the projected bound; one center sample is insufficient.
For a near-plane-intersecting bound, camera-inside bound, invalid projection, or uncertain
rectangle, classify visible. Expand bounds for jitter and numerical uncertainty.

Baseline uses **current-frame** depth from the full opaque prepass. This can reduce
expensive color shading and submission, but it does not avoid that prepass's geometry
cost. Say so in performance reports. An occluder-only prepass can be introduced later
with explicit conservative occluder eligibility; absent occlusion evidence means visible.
Transparent surfaces cannot serve as opaque occluders; masked surfaces need real coverage.

Do not begin with unqualified previous-frame HZB rejection. Camera motion, disocclusion,
moving walls, and new objects make stale depth unsafe. Any later temporal occlusion
scheme requires reprojection/validation or conservative retesting, explicit camera-cut
handling, and a missing-object regression suite.

### 16.3 GPU indirect commands

Write bounded indirect command arrays grouped by compatible bindings, then declare
compute-write -> indirect-read and instance-data shader-read dependencies. Query enabled
features such as multi-draw indirect, indirect first-instance, and draw-indirect count.
Provide a valid fallback: fixed-size command lists with zero instanceCount for culled
draws where supported, or the existing CPU submission path. Do not assume all Vulkan
1.3 devices enable all optional draw features.

Keep material/instance table indexing consistent with firstInstance and any draw-ID
mapping. Never use a compacted command index as the persistent history key. Do not read
visibility counts back to the CPU and wait each frame just to issue fewer calls.
Read asynchronous counters for diagnostics after completion.

Apply camera occlusion only to camera draws. Shadow cascades, point/spot faces, reflection
captures, and editor views need their own visibility decisions.

### 16.4 Upload and pipeline stutter audit

New atlases/probes can expose existing synchronous upload costs. Add or reuse a bounded
staging queue, frame-completion retirement, and atomic handle replacement. Move decode,
hashing, and compression off the render thread. Reuse staging allocations and apply
per-frame upload byte/time budgets. Prefer visible assets, but keep scheduling deterministic
enough to debug. A resource is sampleable only after transfer/layout dependencies complete.

Pre-create known pipeline variants during explicit load/warmup. Cache compiled pipeline
data with device/driver/engine/shader identity. Treat native cache payloads as disposable,
with readable metadata and inspect/clear controls; never make them an opaque authored
asset required to reconstruct a game. Use bounded variants, not every combination of
every material checkbox.

Measure cold load, warm load, first use, hot reload, and steady state separately. A
steady-state benchmark can hide the hitch a player experiences on the first muzzle flash.

### 16.5 Acceptance

Walk around/through an occluder, teleport, resize, change FOV, spawn objects, animate
doors, render masks/glass, look into sky gaps, and view objects that cast visible shadows
while off camera. Compare against forced-visible reference captures. No missing geometry
is acceptable in exchange for FPS. Disable an optimization if its cost exceeds its benefit
on the tested low-end fixture and report why.

## 17. Settings, defaults, budgets, and observability

### 17.1 Proposed preference fields

Extend the existing `swgraphics` metadata/schema and version/migration conventions.
Do not publish unimplemented enum values as working options.

| Proposed key | Values | Ownership |
|---|---|---|
| `anti_aliasing` | none, fxaa, taa | User preference |
| `ambient_occlusion` | off, low, medium, high | User preference |
| `local_reflections` | off, low, medium, high | User preference; selects resident probe policy |
| `lighting_method` | auto, brute_force, clustered | Preference/debug override |
| `occlusion_culling` | off, on | Preference/debug override |
| `graphics_memory_budget_mb` | Bounded positive integer or documented auto sentinel | User/project budget; MiB units must be explicit |

Existing render_scale, shadow_quality, lod_error_pixels, vsync, frame_limit, and low_latency
stay intact. Environment/bake artist parameters (AO radius, probe placement, texel density,
capture resolution, cluster far distance) are scene/project data, not arbitrary per-user
changes to level composition.

Setting changes must use one shared apply path from game menus, editor Preferences,
configuration files, and CLI. The game can query capabilities and effective values.
Preserve existing priority/layering rules in `GraphicsSettings`; do not make a game menu
silently overwrite the checked-in project file when saving personal preferences.

### 17.2 Initial presets, subject to measurements

| Feature | Low | Medium | High |
|---|---|---|---|
| AA | FXAA | FXAA or opt-in TAA | TAA, user override retained |
| AO | Off | Half-resolution low/medium | Half-resolution high |
| Static diffuse | Baked, budgeted | Baked | Baked |
| Dynamic diffuse | Probe sampling, low spatial cost | Probes | Probes |
| Local reflections | Few 64/128 probes or global fallback | 128 probes | Selected 256 probes |
| Decals | Lower visible count, essential channels | Moderate count | Higher budget |
| Shadows | Existing low tier | Existing medium tier | Existing high tier |
| Clustering/batching | Auto based on scene/capability | Auto | Auto |
| Clouds/SSR/fog | Not part of baseline | Not part of baseline | Not required for baseline |

Do not tie baked diffuse lighting to a costly "ultra" setting: it is a principal way
to keep convincing map lighting inexpensive at runtime. Runtime baked-light disabling
is a debug/fallback mode, not the preferred low preset.

### 17.3 Memory arithmetic

For W*H pixels, before allocator padding:

```
RGBA16F image:            W*H*8 bytes
R32F depth/history:       W*H*4 bytes
RG16 SNORM normals:       W*H*4 bytes
R32 UINT receiver flags:  W*H*4 bytes
RGBA8 decal/display:      W*H*4 bytes
2 color + 2 depth history images: W*H*24 bytes
Full single-channel 32-bit mip pyramid: approximately W*H*4*(4/3)
```

At 1080p, TAA color+depth histories alone are about 47.46 MiB; the RGBA16F motion target
adds 15.82 MiB. This excludes current HDR color, depth, normals, masks, bloom, decals,
swapchain, and resource padding. Count the actual allocations instead of describing TAA
as "just one extra texture."

Initial policy targets for additional transient graphics features: low <=32 MiB at
720p; medium <=128 MiB at 1080p; high <=256 MiB at 1080p. These are planning limits to
test and adjust, not total game memory budgets. Baked payloads, models, texture caches,
CPU retained copies, swapchain, and operating-system headroom need their own budgets.
On the 8 GB shared-memory machine, measure process/system pressure as well as GPU
allocations; CPU and GPU budgets cannot each assume they own all 8 GB.

Budget fallback order: drop optional histories/effects according to selected preset;
reduce reflection residency/resolution; reduce optional decal channels/count; retain
valid essential resources and report the effective result. Never overwrite source
assets or silently discard required geometry to fit memory.

### 17.4 Performance evidence

Use milliseconds, not only FPS. At 60 Hz the frame interval is 16.67 ms; at 120 Hz it
is 8.33 ms. Set the tested hardware/output target before judging whether an effect fits.
CPU and GPU work overlap, so do not simply add CPU and GPU timing totals as if every
stage were serial. Record frame pacing and 1%/0.1% lows alongside median pass cost.

Required reports: CPU submission time; GPU time by pass; draw/instance counts; visible/
culled counts; accepted/clustered light counts and overflows; target dimensions; resource
bytes; upload bytes/time; pipeline creation during play; history resets/rejection reasons.
Counters should be cheap, bounded, and sampled asynchronously where necessary.

Report hardware, driver, API, resolution, render scale, preset, VSync/frame limit, warmup,
capture duration, scene revision, and whether validation was enabled. Do not compare a
validation build with a release baseline and attribute all overhead to an effect.

## 18. Editor, text formats, CLI contracts, and packaging

### 18.1 Editor work required for completion

- Graphics preferences use the existing shared settings API. Show unavailable choices
  with a reason and distinguish requested from effective settings.
- Lighting toolbar/panel provides preview/final bake, capture reflections, cancel,
  stale-state reasons, progress, estimated memory, and last bake result. Commands call
  the same service as player. Only explicit actions write bake files.
- Probe and decal components use registered inspector fields, viewport gizmos, duplicate/
  delete, drag-and-drop, and undo. Gizmo colors come from the theme/debug palette.
- Content browser recognizes manifests/payloads, shows previews and dependencies, and
  offers inspect/reveal/rebake actions. Do not show binary payload gibberish as a preview.
- Material controls expose UV/sampler semantics and actual imported limitations.
- Viewport debug selector groups related modes without becoming a wall of buttons.
- Bake status/UI remains responsive. Worker progress is transferred safely to the main
  thread; cancelling and switching projects cannot call into destroyed panels.
- Validate narrow panels, high DPI, alternate themes, detached viewports, and Play/Stop.
  No fixed screen coordinates, panel-local colors, rounded custom cards, or duplicate
  handwritten inspector UI for metadata-supported fields.

### 18.2 CLI: proposed new commands

These are **specifications to implement**, not commands available at the inspected
revision. Keep existing player option conventions where practical, but implement one
unambiguous action at a time and reject conflicting modes. `--help` must label arguments.

| Proposed command | Required behavior |
|---|---|
| `--lighting-schema <file\|->` | Generate text format/component/bake-setting reference from authoritative metadata |
| `--lighting-info [--json <file>]` | Resolve scene's bake, hashes, stale reasons, receiver/probe counts, payload sizes; no bake or source writes |
| `--lighting-assign-ids` | Explicitly assign missing persistent bake IDs to selected scene; preserve valid IDs and unrelated text semantically; report changed files |
| `--bake-lighting --quality preview\|final [--seed N]` | Bake static diffuse and enabled diffuse volumes for selected scene; deterministic seed; bounded progress; atomic output |
| `--capture-reflections` | GPU capture/filter enabled probes using current valid bake; fail if required resources/capability unavailable |
| `--lighting-export <asset> --output <file>` | Export canonical inspectable representation without clamping HDR/signed coefficients |
| `--lighting-import <file> --output <asset>` | Validate and encode canonical representation, atomically; reject malformed/out-of-range input |
| `--material-info <asset> [--json <file>]` | Imported material binding, UV, sampler, color-space, and compatibility report |
| `--graphics-capture <directory> --frames N --capture-step <seconds>` | Deterministic sequence capture, fixed simulation step/seed and explicit camera path from fixture; manifest includes settings and reset events |

New actions use existing `--project` and `--scene` resolution. Output assets must stay
inside the project's allowed content/output locations under existing path policy. Resolve
symlinks/canonical paths consistently; reject unintended traversal. `--check`, info,
schema, and inspect operations do not modify authored assets or initialize Steam.

Return 0 on success, 1 on validation/bake/load/capture failure, and use existing parser
conventions for usage errors if they differ. Tool-readable results go to stdout; normal
engine logs use `SW_LOG` and must not corrupt JSON output. Use `--json <file>` for detailed
reports if that matches existing tools. Include schema version in every new JSON report.
Cancelled bakes return a non-success status and leave the previous generation intact.

`--graphics-capture` must define whether captures include UI, how warmup frames are
counted, and whether image encoding is included in timing. It is not a benchmark; do
not compare a capture loop's FPS with an uncaptured game. Add an explicit warmup option
if needed, and record actual captured frame numbers/jitter indices in the manifest.

### 18.3 Extend existing commands

- `--check`: all checks below, with source path, line where available, component/asset,
  offending value, and actionable message. Validation must not require a GPU.
- `--schema`: registered components and Environment additions.
- `--graphics-schema`: implemented settings and ranges, requested/effective semantics.
- `--graphics-info`: capabilities, requested/effective settings, fallback reasons.
- `--optimise`: derived UV/LOD/texture dependency compatibility, payload memory estimates.
- `--benchmark`: new pass times/counters and memory categories without removing old keys.
- `--view`: new debug modes with stable names; unsupported modes produce a useful error.
- `--package`: collects all reachable bake/material/probe payloads and validates hashes.

Proposed stable debug view names: `surface-normal`, `linear-depth`, `motion`,
`history-validity`, `history-rejection`, `reactive-mask`, `ao`, `lightmap`,
`probe-diffuse`, `reflection-probes`, `decal-coverage`, `light-clusters`, and `occlusion`.
Only publish a name when implemented. Document whether its output bypasses exposure.

### 18.4 Check rule inventory

| Area | Error conditions | Warning/diagnostic conditions |
|---|---|---|
| General | Unsupported version, duplicate/unknown keys, NaN/Inf, malformed enum/range | Unsupported requested GPU feature reported separately at runtime |
| Materials | Missing/wrong-type texture, unavailable UV set, invalid transform/sampler | Source importer property unsupported; approximation identified |
| Bake identity | Missing/duplicate required IDs, invalid prefab-derived binding | Old scene has no bake participants: valid, not an error |
| Bake geometry | Dynamic/skinned receiver marked static-baked, invalid mesh/transform, invalid UV charts | Excessive density or tiny/degenerate charts with usable fallback |
| Bake references | Missing manifest/payload, hash mismatch, stale required bake, invalid bounds/counts/offsets | Optional disabled bake retained but not active |
| Bake lights | BakeLight without Light, non-finite parameters | Animated light/environment will invalidate baked bounce |
| Diffuse probes | Nonpositive bounds/spacing, count overflow, invalid SH/payload | Sparse coverage, invalid probe fraction, visibility leakage risks |
| Reflection probes | Invalid near/far, resolution, extents, capture point, missing baked cube | Overlapping priority ambiguity or excessive memory |
| Decals | Invalid asset/size/opacity/mask/sort range | Unsupported receiver/channel feature explicitly identified |
| AO | Nonpositive radius, invalid intensity | Radius large enough to act as broad fake lighting |
| Cluster settings | clusterFar <= near, invalid memory/grid limits | Reference fallback/overflow counts from runtime diagnostics |
| Package | Any active unresolved required dependency | Size estimate and optional quality variants |

Warnings must not be used to hide corrupted required assets. Separate authoring errors
from optional visual-quality advisories. Include stable diagnostic codes in JSON so
tests need not match entire English sentences.

### 18.5 Packaging and load policy

Follow asset references transitively: scene -> lighting manifest -> receiver map,
lightmaps, diffuse probes, reflection cubes, and material textures. Do not include an
entire development cache or absolute paths. Verify packaged paths by running from a
different working directory with the original Content folder unavailable.

Package valid generated payloads; do not bake during first launch. For a level load,
decode metadata and stage essential lighting resources before exposing ready state.
Optional reflection quality can load later under a budget with global fallback.
Static diffuse data should not appear several seconds late as an unexplained major
lighting pop; either make it part of load readiness or implement an intentional transition.

For low-memory operation, prefer a bounded residency set aligned to the bounded map's
rooms/visibility rather than loading every quality variant. Streaming policy needs
measured evidence; a bounded-map baseline may simply load its budgeted active set.
Do not invent world streaming solely because large engines have it.

## 19. Verification matrix and exact evidence expectations

### 19.1 Unit/CPU tests (proposed test targets)

Use existing CTest conventions. Names below describe required coverage, not currently
existing targets. Keep pure math/serialization tests runnable without a display/GPU.

| Suggested target | Required cases |
|---|---|
| `graphics_settings_test` | Layering, enum/range validation, save/load, requested/effective settings |
| `graphics_coordinates_test` | Negative viewport Y, reverse-Z reconstruction, jitter/reprojection, orthographic fallback |
| `material_binding_test` | UV transforms/sets, sampler/color-space rules, remaps, import/hot reload |
| `lighting_format_test` | Manifest grammar, corrupt/truncated payloads, overflow checks, export/import round trip |
| `lighting_identity_test` | Duplicate/reordered entities, prefab instances, model parts, source invalidation |
| `lighting_bake_test` | Constant environment, zero light, direct exclusion, color bounce, alpha visibility, cancellation |
| `diffuse_probe_test` | SH normalization, interpolation, visibility weights, invalid probes and overlap |
| `reflection_probe_test` | Box slabs/parallax, weights, fallback, cube/mip metadata |
| `decal_test` | Projection bounds, masks, ordering, premultiplied compositing |
| `render_history_test` | Identity reuse, view separation, pose snapshots, reset/retirement rules |
| `cluster_lighting_test` | Slice math, sphere bounds, overflow/reference fallback, memory bounds |
| `occlusion_test` | MIN pyramid, sky gaps, odd extents, all covered texels, near-plane conservatism |
| `graphics_package_test` | Dependency collection, stale rejection, isolated packaged run |

Use seeded random/property tests where they verify conservative bounds or round-trip
invariants. Do not create tests that merely repeat a production formula and therefore
share the same error. Include hand-derived reference cases, especially pi normalization,
Y sign, jitter correction, and reverse-Z reduction.

### 19.2 GPU integration and visual tests

Run shader compilation as part of build. Run selected small Vulkan fixtures with
lavapipe in CI when its exposed capabilities permit. A skipped optional GPU test must
state why; CPU-only CI cannot establish GPU correctness. Keep software-rendered scenes
small enough to finish predictably and separate them from hardware benchmarks.

Use validation and synchronization validation in a dedicated development job. Capture
single frames for AO/material/bake/decal correctness and sequences for TAA/visibility.
Compare with tolerances and masks; preserve diagnostic output on failures. Do not make
cross-driver image equality depend on an exact floating-point bit pattern.

Required manual hardware checks before claiming the roadmap complete:

1. Discrete GPU: reference visual quality, feature interactions, normal target resolution.
2. Integrated/low-end GPU: low preset, memory/format fallback, actual pass cost.
3. 8 GB shared-memory testbed if available: cold load, peak memory, repeated level loads,
   scene unload, and long play without accumulating histories/probe copies.
4. Editor: two views, resize/dock/undock, Play/Stop, theme/DPI changes, backend hot reload.

No available machine means an explicit unverified entry, not an invented success.

### 19.3 Commands already available at the inspected revision

Run from the repository root. Build target names must be checked against current CMake;
the general build command avoids assuming new test targets already exist.

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
build/bin/player --schema docs/SceneSchema.md
build/bin/player --graphics-schema docs/GraphicsSchema.md
build/bin/player --project Games/Arena/Arena.swproject --check
build/bin/player --project Games/Arena/Arena.swproject --graphics-info
build/bin/player --project Games/Arena/Arena.swproject --benchmark 5 --json /tmp/graphics-after.json
```

Run the same benchmark before and after each performance-sensitive batch, with the same
settings and hardware, using separate JSON paths. Use the dedicated generated graphics
fixture for controlled comparisons; Arena is an additional integration check.

Existing screenshot command form, with an actual fixture path substituted after creation:

```text
build/bin/player --project <fixture.swproject> --scene <fixture.swscene> --screenshot /tmp/graphics.png --frames 32
```

For implemented components/node metadata that affects circuits, regenerate
`docs/CircuitNodes.md` with the existing `--node-schema` command as required by AGENTS.
Do not hand-edit generated schemas to make a feature appear implemented.

### 19.4 Proposed end-to-end verification script after implementation

The following is deliberately a template: fixture filenames and new command support
must exist before converting it into a checked-in executable test.

```text
check fixture source and IDs
bake-lighting preview with fixed seed
lighting-info -> assert no stale dependencies and bounded memory
lighting-export -> lighting-import -> semantic payload equality
capture-reflections -> check all faces/mips and manifest dependencies
render reference views with AA none, FXAA, and TAA
capture fixed-step temporal sequences and evaluate masks/metrics
compare brute-force and clustered HDR output including deliberate overflow
compare forced-visible and HZB visibility sequences
package fixture -> run outside source tree -> capture/compare
repeat Play/Stop, backend reload, level reload -> verify no retained-resource growth
```

### 19.5 Evidence record per batch

Use this template in `docs/GraphicsVerification.md` when implementation begins:

```text
Batch / commit:
Implemented behavior and exclusions:
Source/schema/API changes:
Build and test commands + exit results:
GPU/driver, OS, build type, validation mode:
Fixture revision, settings, resolution, warmup:
Before/after CPU, GPU pass times, frame lows, memory:
Screenshots/sequences inspected and what they demonstrate:
Editor interactions inspected:
Hot reload / package / CLI checks:
Known limitations or unavailable verification:
Completion gate: pass / incomplete, with reason:
```

## 20. Failure handling and migration rules

- Old scenes without new components continue using current rendering and global IBL.
  Adding the renderer upgrade must not require every user to bake a scene immediately.
- Keep shader/resource fallbacks deterministic. An invalid probe falls back to global
  specular; an invalid lightmap falls back to global diffuse and produces a diagnostic.
  Packaging still fails for active required invalid artifacts.
- A missing optional GPU feature does not make scene parsing fail on a headless tool.
  Separate authored validity from hardware capability.
- Asset/parser versions are explicit. Reject newer unsupported formats with the version
  and supported range. Implement migrations only when the semantic mapping is known.
- Backend reload and project switching cancel or detach jobs safely. Completed jobs
  cannot publish into a different project's asset folder by using stale references.
- Handle allocation failure by rolling back the new resource generation, keeping a
  working previous configuration if possible, and reporting an actionable failure.
- Fixed bounds apply to file counts, atlas dimensions, coefficient counts, texture mips,
  cluster indices, decal counts, and memory arithmetic. Malformed content cannot trigger
  unbounded allocations or integer overflow before validation.
- CPU/GPU structs have explicit layout assertions and shader layout tests where practical.
  Shader and CPU definitions must be updated together; no "works on my compiler" packing.
- Timeouts/cancel do not mean successful completion. A partially written bake cannot be
  recorded as valid evidence merely because the editor stayed responsive.

## 21. Deferred graphics work and entry criteria

These are future directions, not requirements to finish the baseline or excuses to
start another framework. Each needs a separate implementation specification when chosen.

### 21.1 Temporal upscaling

Prerequisites: correct motion, jitter, disocclusion, reactive coverage, stable view identity,
dynamic-resolution history policy, and TAA sequence tests. Then evaluate a maintained,
license-compatible upscaler or a separately scoped reconstruction method. Input/output
resolution, exposure, sharpness, and UI composition contracts must be explicit.
Do not rename render_scale plus TAA to an advanced upscaler.

### 21.2 Screen-space reflections

Prerequisites: usable depth hierarchy, normals/material roughness, motion, and local/global
reflection fallback. SSR traces only visible information and misses off-screen/occluded
geometry. Blend by confidence into the existing selected specular source, then apply the
BRDF once; do not add SSR on top of a full-strength probe reflection. Include edge loss,
roughness, temporal rejection, and transparent-surface policy. Keep it optional on low.

### 21.3 Volumetric fog

Prerequisites: light clustering/shadow sampling, depth, pass/resource budgets, and stable
history if temporally accumulated. A bounded froxel volume with explicit resolution,
extinction/scattering units, phase function, and near/far policy can be a useful shooter
feature. Integrate transmittance and in-scattering consistently with sky and transparent
objects. It is separate from surface AO and cannot replace indirect lighting.

### 21.4 Directional lightmaps and dynamic GI

Non-directional lightmaps do not respond correctly to changing surface normal direction.
Directional basis storage is a possible extension, but increases memory and bake/shader
complexity. Dynamic GI additionally needs update cost, invalidation, visibility, and
stability work. Reuse the selected-indirect-light interface; do not add another independent
ambient term. A raymarcher alone does not supply correct GI transport or visibility data.

### 21.5 Clouds, water, and fluid simulation

Clouds can improve outdoor scenes but are lower priority for compact indoor/urban shooter
maps. Water rendering and fluid simulation are separate systems with different budgets.
Neither is necessary to ship a convincing baseline shooter. Plan them against measured
content needs, not a feature-count comparison with Unreal.

## 22. Implementer checklist and stop conditions

Before each batch:

- [ ] Confirm current source still matches this plan; identify overlapping newer work.
- [ ] Confirm scope/branch/renderer ownership and preserve unrelated changes.
- [ ] Define public data/API, lifecycle, schema, and expected test outcomes first.
- [ ] Confirm prerequisites are implemented and verified, not merely documented.

Before calling the batch complete:

- [ ] Feature works through editor and text/CLI paths.
- [ ] Bad content produces useful checks; unsupported GPU choices are visible.
- [ ] Shared settings/theme/component registration conventions are preserved.
- [ ] Correct graph dependencies and GPU lifetimes are declared.
- [ ] CPU/GPU layout, units, coordinate conventions, and color spaces are documented.
- [ ] Hot reload, scene unload, Play/Stop, and backend replay were exercised.
- [ ] Required assets package and run without the source tree.
- [ ] Relevant builds/tests passed; visual sequences were inspected where required.
- [ ] Cost was measured with memory and frame-pacing evidence.
- [ ] Known limitations are recorded without claiming unperformed verification.
- [ ] Touched C++ is clang-formatted; generated schemas are genuinely regenerated.

Stop implementation of the dependent batch if coordinate tests, history identity,
indirect-light normalization, or GPU synchronization are unresolved. Continue investigating
the prerequisite; do not cover the failure with a compensating brightness multiplier,
forced frame wait, random motion clamp, or blanket disabling of validation.

The endpoint is a coherent graphics pipeline with useful low-end behavior, authoring
tools, accessible data, and reproducible correctness/performance evidence. A collection
of shaders that can each be toggled on is not sufficient completion.

## 23. Primary technical references

These explain algorithms or API rules; they are not instructions to copy entire external
frameworks. Pin dependency versions and preserve applicable licenses when implementing.
Most design choices above are Shockwave-specific proposals derived from the inspected
code, not promises made by these sources.

- [Khronos glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html):
  material color spaces, channels, texture coordinates, and sampler semantics.
- [KHR_texture_transform](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_texture_transform/README.md):
  per-texture transform/coordinate override semantics.
- [Khronos texture-transform sample](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/TextureTransformMultiTest/README.md):
  external reference when validating import, subject to the asset's license.
- [xatlas upstream](https://github.com/jpcy/xatlas): UV atlas generation, vertex remapping,
  API and license. Do not assume it solves bake identity or light transport.
- [NVIDIA FXAA white paper](https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf):
  spatial postprocess AA reference; confirm chosen implementation licensing.
- [Advances in Real-Time Rendering 2014](https://advances.realtimerendering.com/s2014/index.html):
  Brian Karis's temporal supersampling presentation and related course materials.
- [Vulkan synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html)
  and [synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html):
  image/buffer hazards and stage/access scope.
- [Vulkan render-pass specification](https://docs.vulkan.org/spec/latest/chapters/renderpass.html):
  attachment/read-only-depth rules, including relevant dynamic-rendering constraints.
- [Vulkan multi-draw-indirect sample](https://docs.vulkan.org/samples/latest/samples/performance/multi_draw_indirect/README.html):
  indirect-draw integration and feature considerations.
- [Clustered deferred and forward shading, original paper](https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf):
  spatial light clustering; the fixed-list/overflow policy here is a separate baseline design.
- [AMD FidelityFX CACAO documentation](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/combined-adaptive-compute-ambient-occlusion/):
  reference for later AO quality/performance comparison, not a required dependency.
