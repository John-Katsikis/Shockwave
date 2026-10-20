# 3. Particles and transparency

[Guide index](README.md) · [Previous](02-MaterialsAndDecals.md) · [Next](04-ColorAndAtmosphere.md)

Proposed implementation: A04. Read foundation G03/G09/G10 and current
[Effects limits](../Effects.md#limits). This chapter targets the missing renderer bridge
capabilities, not replacement of Effekseer's authoring tool or simulation.

## 3.1 Current boundary and required result

The current bridge captures Effekseer sprites/ribbons/rings/tracks/models into CPU
triangles, then Shockwave renders them. Simulation already runs on a worker. Current
limitations include skipped distortion/custom materials, simplified advanced fields,
no soft particles/GPU particles, first-directional-plus-ambient lighting, and ordering
of effects as wholes. Do not claim upstream Effekseer feature parity from library presence.

Required baseline: depth-softened intersections; bounded heat distortion; per-effect
support diagnostics; useful scene lighting; defined transparent ordering; material data
preservation; TAA reactive coverage; and quality settings that preserve gameplay smoke.
GPU particle simulation and arbitrary custom shaders are separately gated sub-batches.

## 3.2 Portable particle draw contract

Extend Renderer/Particles public descriptors with fields actually consumed by supported
shaders. Version CPU/GPU layouts together and preserve resource replay. Suggested groups:

```text
geometry: vertex/index spans, world bounds, effect/instance identity
surface: blend mode, cutoff, emissive, normal/alpha/blend texture bindings
advanced: flipbook interpolation, UV distortion bindings and amplitude, falloff data
depth: depth test policy, soft intersection distance, near-camera fade distances
distortion: magnitude in documented screen or world-derived units, mask and ordering phase
lighting: unlit / simple / scene_lit, shadow policy and receive-probe policy
temporal: reactive strength, motion eligibility and validity
budget: essential/decorative class, priority, estimated screen coverage
```

Preserve semantics from the actual Effekseer version. Do not relabel an upstream parameter
with guessed units. Add fixtures authored in that version and compare against its
reference renderer/editor. Unavailable upstream data must yield a diagnostic rather
than an invented default that silently changes appearance.

Workers publish immutable draw snapshots. GPU uploads reuse bounded buffers with
completion-based retirement. No UI work or Vulkan calls from Effekseer simulation workers.
The capture bridge and GPU layout must not retain pointers into a vector that grows
before command recording completes.

## 3.3 Soft particles

Prerequisite: readable opaque depth with STORE independent of particle existence and
the reconstruction convention in G03. Keep particle depth writes disabled for ordinary
blended effects. Opaque/masked particle models can have a separate documented path.

At the particle fragment, obtain positive view depth `zp`. Sample scene depth at matching
screen UV and reconstruct positive `zs`. For ordinary surfaces:

```
gap = zs - zp
intersectionFade = clamp(gap / max(softDistance, epsilon), 0, 1)
nearFade = smoothstep(nearStart, nearEnd, zp)
fade = intersectionFade * nearFade
```

Depth test already rejects opaque surfaces in front; the fade smooths intersections
immediately behind the particle plane. Sky depth uses intersectionFade=1, not near/0.
An explicit softDistance=0 disables the intersection fade. Reject negative distances and
nearEnd<=nearStart unless near fade is explicitly disabled. Units are world meters under
the engine's agreed scene convention; changing resolution must not change fade distance.

For straight-alpha shader output multiply alpha by fade and let the blend operation
premultiply once. For premultiplied output multiply both RGB and alpha by fade. Additive
RGB must also fade; changing alpha alone does nothing under additive ONE/ONE blending.
Multiply mode must approach neutral white as fade approaches zero. Define each blend
mode's neutral contribution and test it; one formula cannot be blindly pasted into all.

With half-resolution effects, use depth-aware upsampling and foreground rejection.
Do not bilinearly average foreground/background depth and create a halo. Baseline can
remain full-resolution until this optimization passes required fixtures.

## 3.4 Distortion: a bounded scene-color consumer

Use a separate immutable scene-color snapshot or a separate accumulation/composite
phase. Never read and write the same HDR attachment without a supported feedback design.
Start with an explicit ordering model:

1. Opaque scene and background.
2. Transparent draws classified behind the distortion boundary, if any.
3. Snapshot the resulting HDR color.
4. Accumulate distortion offsets/coverage for the selected distortion group.
5. Composite distorted background to a different target.
6. Foreground particles/transparency.

This baseline is one refractive/distortion layer, not exact arbitrary interleaving.
Map Effekseer drawing priority to explicit phases and warn about unrepresentable ordering.
Do not quietly reorder every flame in front of all smoke because that is convenient.

Define decoded offset `d` from the upstream distortion texture convention. The engine
override uses pixels at reference height Href=1080, isotropic in pixels. At render size
W,H, use `deltaUV = (strength/Href) * vec2(d.x*H/W, d.y)`. This produces equal X/Y
pixel displacement for equal offset components, scaled with image height. Record this
unit in metadata and convert upstream strengths through a tested adapter; do not assume
Effekseer's raw value has the same units. Tests at 720/1080/4K and non-square aspect
ratios must demonstrate the stated behavior.

Accumulate weighted offsets in RG16F and coverage/weight in a supported channel; limit
total displacement. For overlapping patches define weighted-average offsets versus
ordered composition explicitly. Weighted average is bounded and cheap but not physical
multiple refraction. Composite with mask coverage, reject samples outside the image,
and attenuate/reject displaced lookups that cross a nearer foreground depth edge.

Off skips the copy when no other consumer needs it, accumulation, and composite.
Reactive coverage includes both the distortion mask and affected sampled region so
TAA does not retain a stale displaced background.

## 3.5 Lighting and normal maps

Provide modes:

- Unlit: emissive/fire/UI-like energy as authored, no arbitrary ambient multiplication.
- Simple: current directional-plus-ambient behavior, preserved for compatibility/Low.
- Scene-lit: dynamic diffuse probes/global fallback plus clustered local/directional
  lights, with a documented affordable particle BRDF or Lambert approximation.

Normal-mapped billboards derive a basis from billboard geometry/UVs and camera-facing
orientation. Flip handedness correctly on backfaces. Model particles use mesh tangents.
Point/spot light indices match the main immutable frame table. Transparent fragments use
valid cluster lists even when opaque tile depth does not cover them.

Shadows on particles are optional and expensive. Start with selected directional shadow
receiving in High, tested with a bounded sample count. Particles casting volumetric
shadows are outside the baseline. Do not present ordinary lit sprites as physically
multiple-scattering smoke; define the approximation honestly.

## 3.6 Transparent ordering

Gather eligible transparent meshes and particles into a common view-specific submission
description. Baseline sorting key: explicit phase, priority, conservative view-depth key,
stable identity. Alpha-over is noncommutative. Additive draws may be grouped only within
legal phases; they still interact with opaque depth and subsequent alpha-over draws.

Whole-effect sorting is retained only for effects that declare it acceptable. For an
overlapping smoke/character fixture, support per-particle or per-primitive-chunk sorting
where the bridge exposes sufficient geometry. Preserve ribbons/tracks connectivity;
sorting individual vertices destroys the effect. Explain remaining intersecting-triangle
limitations rather than promising perfect order from a single center-depth number.

Weighted blended OIT is an optional separate mode after the sorted reference works.
If implemented, document accumulation/revealage equations, weight function, supported
blend modes, and inability to exactly reproduce arbitrary refractive or strongly layered
alpha scenes. Retain sorted reference captures. Never apply OIT to every material silently.

## 3.7 Advanced Effekseer fields and support matrix

Implement a support report from the actual decoded effect nodes/materials. Status is
supported, approximated, unsupported, or disabled-by-quality; include node path, reason,
effective fallback, and required capability. Packaging can make unsupported required
features errors while permitting documented decorative approximations.

Baseline priorities:

| Feature | Required treatment |
|---|---|
| Flipbook interpolation, alpha textures/cutoff | Preserve current support; test parity |
| Falloff | Implement declared view-angle response with correct normal/view conventions |
| UV distortion | Preserve binding/strength and apply before relevant texture sampling |
| Blend textures | Preserve upstream combination modes and color-space behavior |
| Normal maps | Scene-lit/model/billboard path as above |
| Background distortion | Explicit phase path above |
| Soft particle distances | Depth fade above |
| Custom `.efkmat` | Sub-batch below; explicit unsupported until compiled and validated |
| GPU particles | Separate future sub-batch; no false claim of support |
| Embedded sounds | Existing AudioSource alternative remains; not graphics scope |

## 3.8 Custom material sub-batch

Do not execute arbitrary text pasted into a runtime shader compiler. Inspect the pinned
Effekseer MaterialFile/compiler API and prefer its supported intermediate representation
or generator, preserving license/version constraints. Build an offline asset compiler
adapter that emits Shockwave-compatible shader interfaces and reflection metadata.

First supported subset: texture/uniform parameters, constants, UV/position/normal/view
inputs, time, scalar/vector arithmetic, lerp/clamp, approved gradients, and color/emission/
opacity/normal/distortion outputs. Reject unsupported nodes with node identifiers.
Cap graph size, texture/uniform counts, recursion/cycles where applicable, generated
instruction/variant budgets, and compile time. No raw file/network access from materials.

Compiler output includes source hash, compiler/version, feature requirements, descriptor
layout, generated readable shader source, and compiled payload. Provide inspect/export
and clean rebuild. Validate reflected layouts against the particle pipeline before load;
an opaque SPIR-V blob is not proof of compatibility. Runtime chooses precompiled variants
and never compiles on the first explosion. Keep the last valid version during hot reload.

Do not claim arbitrary custom materials are fully supported until all intended node and
output semantics have reference fixtures. A bounded documented subset is an honest
milestone; rendering an unsupported material as white without warning is not.

## 3.9 Temporal behavior

All moving/translucent particle coverage feeds the reactive mask from G09. Base temporal
weight is reduced for changing opacity/emission/distortion. If valid particle motion is
available, key it by stable effect/particle generation; recycled pool indices are not
stable history identities. Do not allocate full per-particle temporal history for Low.

Opaque depth remains the main scene rejection surface. Soft particles sample that depth,
but do not overwrite it. Distortion and transparent velocity cannot be validated against
an unrelated wall's opaque velocity. Near-camera fades and rapid bright flashes are
mandatory temporal sequence fixtures.

## 3.10 Budgets and match scaling

Record active particles, generated vertices/indices, simulation time, bridge capture time,
upload bytes, material variants, visible screen coverage, distortion coverage, and GPU
pass times. A particle-count cap alone does not bound cost: one full-screen smoke quad
can cost more fragment work than many tiny sparks.

Quality controls cap decorative emitter rates, detail particles, distortion resolution,
lighting samples, and visible coverage. Essential smoke/impact cues have a minimum
representation and deterministic priority. A 32v32 scripted firefight must include
simultaneous muzzle flashes, impacts, smoke, and overlapping players; empty idle effects
do not establish the worst-case budget.

Do not move simulation to the GPU by default. First measure simulation, capture, upload,
and pixel cost separately. A GPU emitter path needs its own buffer allocator, sorting,
collision policy, event/readback semantics, and fallback; it cannot be enabled by merely
adding a compute shader to the existing CPU capture bridge.

## 3.11 UI, CLI and completion

Editor Effects inspector adds quality class, supported rendering mode, soft-distance
preview, distortion phase, and compatibility report with Reveal node/source actions.
Derived upstream properties are displayed read-only unless an explicit engine override
is stored in a `.sweffect` sidecar or registered component. Choose one source of truth;
do not modify binary `.efkefc` data behind the author's back.

Extend `--check` with the compatibility report and required-feature failures. Proposed
`--effect-info <asset> --json <file>` reports decoded features and budgets. Screenshot/
capture fixtures provide CLI verification; material compile/inspect commands are explicit
offline actions. Package all referenced effect textures/models/material payloads.

Acceptance: smoke-wall intersection with no hard seam; additive fade tends to zero;
multiply fade tends to identity; no foreground displacement leaks; correct rapid camera
motion; tested overlapping effects; reference-supported material fixtures; no compilation
or buffer-allocation spike on first use after preload; stable 64-player stress budgets.

## References

- [Effekseer advanced render settings](https://effekseer.github.io/Helps/18x/Tool/en/ToolReference/rendererCommonAd.html):
  upstream feature meanings and host support requirements.
- [Effekseer distortion tutorial](https://effekseer.github.io/Help_Tool/en/ToolTutorial/13.html):
  authoring/priority reference. The phase architecture here is Shockwave's proposed design.
