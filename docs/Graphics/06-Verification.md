# 6. Implementation sequence, verification and delivery

[Guide index](README.md) · [Previous](05-CharacterShading.md) · [Next](07-MathematicalContracts.md)

This is the execution contract for the foundation and A00–A10. It specifies future
implementation verification; documentation/UI verification performed while writing this
guide is recorded separately at the end of this chapter.

## 6.1 Dependency-respecting sequence

| Step | Work | Do not advance until |
|---|---|---|
| 1 | G00 baseline fixtures, graph resource states, capabilities, lifetimes | Existing output unchanged; synchronization/reference tests pass |
| 2 | A00 shared settings, G01 FXAA | User/CLI/editor settings agree; off paths skip work |
| 3 | G02 import/UV/sampler contracts, A01 residency | Mips/materials survive reload and packaging; memory savings measured |
| 4 | A02 instances/detail/layers | Imported and authored material fixtures match declared semantics |
| 5 | G03 depth/normal prepass, G04 SSAO | Coordinate reconstruction and depth consumers correct |
| 6 | G05 static diffuse bake, G06 dynamic probes | Normalization, identity, leakage, stale data and cancellation tested |
| 7 | G07 reflections | Capture, parallax, blending, memory and fallback correct |
| 8 | G08 plus A03 decals | Runtime/editor authoring, masks, ordering, budgets and package complete |
| 9 | G09 motion, G10 TAA | Camera/rigid/skinned/viewmodel histories correct in sequences |
| 10 | G11 clusters, A04 particles | Local-light reference parity, overflow, transparency and particle fixtures |
| 11 | A05 color/exposure, A06 fog | Neutral operations identity; volume math and composition pass |
| 12 | A07 coating/transmission, A08 skin/eyes, A09 hair | Content-specific numerical/visual gates and fallbacks |
| 13 | G12 visibility/batching/upload improvements | No missing geometry; measured CPU/GPU/pacing improvement |
| 14 | G13/A10 integrated presets and game slice | Representative match loads, packaged runs and documented limitations |

Independent authoring work may move earlier, but do not merge a dependent renderer pass
that only "works" by disabling its prerequisite tests. If another agent owns the renderer,
agree on files/APIs first. Do not infer authorization to merge or push from this guide.

## 6.2 Concrete code ownership and extension points

Existing owners to read before implementation:

| Area | Existing path |
|---|---|
| Settings | `Source/Runtime/Engine/Public/Engine/GraphicsSettings.h` and matching Private implementation |
| View/draw/resources | `Source/Runtime/Renderer/Public/Renderer/Renderer.h`, `RenderBackend.h`, `RenderTypes.h`, `TextureData.h` |
| CPU replay | `Source/Runtime/Renderer/Private/Renderer.cpp` |
| Materials/import | `Source/Runtime/Assets/Public/Assets/Model.h`; `Source/Runtime/Assets/Private/Model.cpp`, `AssetFolder.cpp` |
| Component schema | `Source/Runtime/Engine/Private/Components.cpp`, `SceneFile.cpp`; module registration for new module types |
| Frame graph | `Source/Plugins/RendererVulkan/Private/RendererFrame.cpp`; `Source/Runtime/RHI/Public/RHI/RenderGraph.h` |
| Shader resources | `Source/Plugins/RendererVulkan/Private/RendererResources.cpp`, `RendererPipelines.cpp`, `RendererSetup.cpp` |
| Particle bridge | `Source/Runtime/Effects/Private/EffekseerBridge.cpp`, `Effects.cpp`; `Source/Runtime/Renderer/Public/Renderer/Particles.h` |
| Particle draw | `Source/Plugins/RendererVulkan/Private/RendererParticles.cpp`, `Shaders/particle.frag` |
| Existing tone/bloom | `Source/Plugins/RendererVulkan/Shaders/tonemap.frag`; `Private/RendererBloom.cpp` |
| Game/editor UI | Existing GameUI API; `Source/Editor/Editor/Private/Preferences.cpp`, `InspectorPanel.cpp`, `ContentBrowserPanel.cpp` |
| CLI | `Source/Programs/Player/Private/Main.cpp`, `PlayerTools.cpp`, `PlayerOptimise.cpp`, `Player.h` |

Proposed additions should be small domain files in those modules: MaterialAsset/MaterialParser
in Assets, texture-residency coordination in Assets/Renderer, dedicated Vulkan pass files
for fog/SSS/distortion, and Player tools grouped by feature. Do not place engine services
in Arena game code or include another module's Private headers.

Lighting is the proposed new module from the foundation. Color/atmosphere do not require
a second graphics engine module just to own a handful of descriptors. If later scope
justifies a module, define dependency direction with `shockwave_module()` before moving
types. Engine cannot depend on a module placed after it merely to read one component.

## 6.3 Frame/resource contract review

Before each new pass, write a table of inputs, outputs, dimensions, format, lifetime,
initial contents, stage/access/layout, history identity, and maximum bytes. Review all
consumers that share scene depth/color. Do not rely on a comment claiming "only tonemap
reads this" after adding another reader.

Distinguish resource lifetimes:

- persistent asset: texture/material/probe, retained across frames;
- per-view history: previous HDR/depth/volume history, reset by view generation;
- per-frame transient: AO/distortion/skin intermediates;
- staging: CPU/GPU transfer buffers awaiting completion;
- immutable shared lookup: BRDF table, kernel/LUT data with explicit generation.

GPU completion determines retirement. C++ object lifetime or end of a frame function
alone does not. Changing a descriptor referenced by submitted commands needs the backend's
legal update/indirection strategy. Old generation metadata must stay paired with old images.

Single graphics queue first. Copying between buffers/images still requires correct
barriers. Optional async compute later needs queue-family ownership and semaphore scopes,
not just moving a dispatch to another command pool.

## 6.4 Editor routes that must exist when each feature ships

These are proposed feature routes; the Help guide route already exists as documentation UI.

| Feature | Human authoring | Player preference | Tool path |
|---|---|---|---|
| Lighting bake | Lighting panel, BakeGeometry inspector, progress/cancel | Baked lighting retained in low preset | bake/info/schema/check commands from foundation |
| Probes | Create, gizmos, inspector, capture/rebake | Local reflection quality | lighting-info/capture/check |
| AA/AO | Scene AO radius; preview/debug views | AA mode, AO quality | graphics schema/info/overrides/capture |
| Textures | Asset category/import policy, residency report | Texture quality/budget/filtering | texture-info/optimise/check |
| Materials | Content create/instance, slots and override provenance | Material quality | material schema/info/bake/check |
| Decals | Surface placement, material, receiver and lifetime controls | Decorative decal quality | scene schema/check/capture |
| Effects | Compatibility report, renderer overrides, preview | Particle/distortion/soft-particle quality | effect-info/check/capture |
| Grading/exposure | Profile editor, volumes, LUT import/export, histogram | Allowed grade/adaptation choices | postprocess schema/info/check |
| Fog | Environment/volume density and lighting | Allowed fog quality | profile/scene schema/check/debug capture |
| Skin/hair/glass | Material family/profile preview and fallback | Family quality | material schema/info/check/capture |

Inspector fields use metadata; special previews/progress use shared gui controls. Every
authored change is undoable where the editor already supports it. Previewing a quality
fallback does not rewrite the source material. Missing references show a named error and
an action to locate/replace the dependency, not an empty unlabeled picker.

## 6.5 Schemas and validation contracts

Regenerate authoritative references through player after implementation:

```sh
build/bin/player --schema docs/SceneSchema.md
build/bin/player --graphics-schema docs/GraphicsSchema.md
build/bin/player --node-schema docs/CircuitNodes.md
```

Proposed material/postprocess/lighting schemas are added by their batches. Do not fake
generated docs before the implementation can emit them. Parser errors include file,
line, key, value, and stable code. Runtime capability fallback has a separate diagnostic
category from malformed source. Unknown/duplicate fields and non-finite numbers fail.

Use overflow-safe arithmetic before every allocation/count multiplication. Check mip
dimensions, layers, byte offsets, index bounds, material inheritance depth, LUT sample
count, particle vertices, froxel counts, and texture budget. Restrict output/import paths
according to existing project rules. Tools do not execute arbitrary material code or
shell strings from asset contents.

## 6.6 Reference scenes and generated assets

Use `Tests/fixtures/graphics/` as specified by the foundation. Keep every required test
asset redistributable and available from source/generator. Record generator seed/license.
Never require the user's external asset-library mount to run CI or reproduce a result.

Scene inventory:

1. Coordinate laboratory: reverse-Z, near-plane, orthographic and jitter fixtures.
2. Neutral material room: white/black furnace, roughness/metallic/coat sweeps, UV grids.
3. Lighting room: sealed chamber, doorway, colored bounce wall, moving neutral character.
4. Reflection room: two differently colored boxes, rough/metal surfaces, moving camera.
5. Texture stress: repeated high-resolution assets, pixel art, masks, normal detail.
6. Impact corridor: decals on walls and moving door, masks/priorities, persistent/transient.
7. Effects arena: smoke-wall intersections, additive/multiply fades, heat haze, overlaps.
8. Color stage: neutral ramps, LUT axes, exposure transitions, bright flashes, UI reference.
9. Fog stage: homogeneous analytic reference, height layer, flashlight, glass/smoke overlap.
10. Character stage: skin/eyes/hair close-up, distant cards, fast joint motion and viewmodel.
11. Combat load: seeded 1v1, 6v6, 16v16, 32v32 actors with representative firing/effects.

Use actual game-like content for the final slice in addition to synthetic math scenes.
A triangle-budget fixture cannot verify artistic skin quality; a beautiful room cannot
prove a conservative occlusion algorithm never hides an object.

## 6.7 Numerical tests

Implement pure CPU reference tests for chapter 7. Do not call the same production helper
on both sides and label the comparison independent. Use known closed forms, double
precision numerical integration, or independently coded reference transforms.

Recommended error categories: exact discrete identity (IDs/counts); float relative/absolute
tolerance; Monte Carlo confidence interval for bake estimates; image metric tolerance;
conservative containment (no false-negative geometry). Document chosen tolerances and
why they match format precision/sampling, instead of expanding them until tests pass.

GPU readback tests run outside the gameplay path on tiny fixtures. Compare linear
pre-tonemap values when testing transport/color arithmetic. Record format/capabilities
and skip only unsupported optional GPU cases with explicit reasons. A skipped test is
not evidence that the feature works on that hardware.

## 6.8 Temporal and visual tests

Fixed-step capture sequences specify seed, camera path, simulation time, resolution,
render scale, exposure, jitter phase, warmup, and quality. Required events: spawn/despawn,
entity index reuse, teleport, scene cut, LOD switch, hot reload, resize, backend swap,
Play/Stop, projectile/effect spawn, and rapid viewmodel animation.

Inspect at playback speed and slowed playback. Metrics include static-region variance,
trailing residual behind moving masks, reference edge disagreement, and reactive/rejection
maps. No single aggregate SSIM score proves absence of a small but severe weapon ghost.
Provide diagnostic masks and representative frames with every failing sequence.

UI verification: default and alternate theme; narrow panel; high DPI; project hub and
open project; selected entity; playing/paused; detached viewport if supported. Verify
click/keyboard navigation, errors, undo, profile hot reload, and effective setting display.

## 6.9 Performance protocol

Measure release-equivalent builds on the same machine/driver/configuration. Separate
validation-mode runs. Record CPU capture/submission, GPU passes, memory, uploaded bytes,
pipeline compile counts, frame intervals, 1%/0.1% lows, and longest hitch. Distinguish
cold load, first-use warmup, sustained play, reload, and teardown.

Use paired before/after runs with identical content and settings. Repeat enough to
identify noise/thermal effects; do not promise a single benchmark number is universal.
Five seconds is a smoke measurement; a sustained combat test should cover repeated
effects, residency turnover, and scene transitions. Include CPU/GPU bottleneck labels.

64-player test means 64 representative animated/rendered actors plus weapons, shadows,
effects and decals. Networking's microsecond codec measurements do not establish this
rendering cost. If the scene is intentionally worst-case full visibility, label it.
Use both realistic camera paths and worst-case coverage.

Every feature can be individually disabled for profiling. Capture requested/effective
settings and dependency-kept passes. Report bytes freed after retirement. On the 8 GB
Orin shared-memory device, also record CPU/process/system memory and thermal/power mode;
do not compare an uncooled throttled run with a desktop and blame shader architecture.

## 6.10 Packaging and migration

Traverse material parents, profiles, LUTs, effect material graphs, probe/bake payloads,
all required mip tails, fallback textures, and precompiled variants. Avoid shipping
developer caches wholesale. Keep source format/optimized format versions explicit.
Games run without the engine repository, source texture originals, DCC tools, internet,
or a shader compiler. No first-launch bake or compression task hidden in a loading screen.

Defaults preserve old scenes. Add fields with known defaults; reject unsupported future
versions. Migrations write through explicit tools or editor actions with reviewable output.
User quality preferences do not alter network content identity of authored assets; inspect
existing multiplayer digest policy before adding derived-cache files to its input.

For gameplay-relevant visibility, game/session policy may constrain effects and fallback
behavior. Do not use client-only fog density as an authoritative anti-cheat mechanism.
Document which authored properties are visual and which the game separately uses in rules.

## 6.11 Delivery checklist per batch

- Source audit and prerequisites recorded.
- Portable API, data ownership, lifetime and units specified.
- Editor UI, asset/scene schema and CLI implemented together.
- Null/backend fallback and requested/effective settings behave honestly.
- No silent unsupported material/effect feature.
- Parser bounds and numerical edge cases tested.
- Graph hazards, resource retirement and hot reload exercised.
- Source and binary inspect/export/import round trips checked where applicable.
- All dependencies package and run outside source tree.
- Visual/temporal fixtures inspected; performance and memory measured.
- Limitations documented with exact missing evidence.
- Touched C++ clang-formatted; affected targets/relevant CTest checks pass.

## 6.12 Implementing-agent handoff format

For every batch report: commit/revision; files/APIs; before/after behavior; implemented
and excluded features; commands and exit results; hardware/capabilities; screenshots/
sequences; timing/memory comparisons; schema/package results; unresolved issues; next
dependency-ready batch. Do not describe a prototype as production complete because it
renders one screenshot.

## 6.13 Documentation delivery verification

The guide itself adds a small editor Help window and two CLI flags. It does not implement
A00–A10 or G00–G13. The editor uses the existing style and opens Markdown externally;
it is not a new Markdown renderer. Documentation-only changes are copied by `editor_docs`
when building editor. A developer can read the same source with any text viewer.

Verification for this delivery is recorded after final checks below. Keep this entry
separate from future graphics implementation evidence; passing a help-window test does
not validate the rendering algorithms described in the chapters.

Delivery checks on 2026-10-20:

- Built `editor`, `ui_theme_test`, and `graphics_test` successfully with the current
  configured build. Dependencies emitted existing CMake deprecation warnings.
- CTest `ui_theme`, `graphics`, and `graphics_cli` passed (3/3).
- `editor --graphics-guide-path` resolved the live source index without initializing
  the engine; `--help` lists both new flags.
- Inspected the running Vulkan editor on the RTX 3080: guide open from startup in the
  project hub/default theme, and a narrow guide with the Sample project, selected entity,
  and Ember theme. Guide uses shared style and wrapped descriptions/paths.
- Checked Markdown navigation/anchors, table widths, fences, explicit source paths,
  bundled document contents, and whitespace.
- Independently checked the appendix's exact mip counts, fog/exposure partition identities,
  phase normalization, and anisotropic NDF quadrature in Python. These are document
  reference checks, not tests of graphics features that have not yet been implemented.
- External Markdown application launch, clipboard behavior, high-DPI transitions, and
  Play/Pause interaction were not independently exercised. The UI uses existing SDL/ImGui
  launch and clipboard APIs; users need an associated Markdown application for Open.
- No rendering feature or performance change is claimed by this documentation delivery.
