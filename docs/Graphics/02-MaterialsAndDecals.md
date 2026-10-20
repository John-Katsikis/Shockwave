# 2. Material authoring and surface detail

[Guide index](README.md) · [Previous](01-SettingsAndTextures.md) · [Next](03-ParticlesAndTransparency.md)

Proposed implementation: A02–A03. Foundation G02 and G08 define import and D-buffer
contracts. This chapter makes their authoring path concrete and adds bounded layering.

## 2.1 Desired behavior

An artist imports a material, sees its actual supported bindings, creates a reusable
instance, changes roughness or adds detail, and assigns it without editing C++ or losing
the change on model reimport. A tool performs the same operation through text files.
Instances are small data assets; they do not duplicate megabytes of texture pixels.

Engine asset identity and source material identity remain separate. Imported base
materials are generated records keyed by source asset plus stable material identifier.
An authored override references that record and changes explicit fields. Source reorder
must preserve identity or mark the reference unresolved; never silently bind by old index.

## 2.2 Portable data model

Proposed Assets public types, adapted to existing handles:

```text
MaterialDefinition:
  stable asset identity, version, supported shading family
  scalar/vector parameters and named texture bindings
  blend/depth/cull policy, alpha cutoff
  declared feature requirements and fallback definition

MaterialInstance:
  parent definition/instance asset
  explicit parameter/slot overrides
  dependency generation and resolved immutable descriptor

TextureBinding:
  texture asset, interpretation, UV set, UV transform, sampler descriptor
```

Renderer receives a resolved immutable `MaterialDesc` and handles, not parser objects or
filesystem paths. Assets resolves inheritance, loads dependencies, and hot reloads them.
Engine's MeshRenderer/ModelRenderer binding selects the material or per-part override.
Material UI lives in Editor; Vulkan evaluates only supported shader families.

Inheritance depth is capped at 16, cycles are errors, and duplicate override keys fail
validation. A definition can be referenced by many instances. Deleting a parent produces
an actionable missing-reference error and a visible fallback material; it never dereferences
a destroyed GPU buffer. Resolve complete replacements before publishing generations.

## 2.3 Text format: proposed `.swmaterial` version 1

Use the engine's text parsing helpers where suitable. The following grammar is normative
for the new format unless an equivalent format has already been implemented:

```text
swmaterial 1
parent "Materials/SteelBase.swmaterial"
family standard
base_color 0.65 0.67 0.70 1
metallic 1
roughness 0.32
normal_scale 1
emissive 0 0 0
alpha_mode opaque
alpha_cutoff 0.5
double_sided false
texture base_color "Textures/steel_color.ktx2"
texture normal "Textures/steel_normal.ktx2"
uv_set normal 0
uv_transform normal 0 0 1 1 0
sampler normal repeat repeat linear linear linear 8
```

Records are one per line. `#` comments apply outside quotes. Strings are UTF-8 with
double-quote/backslash escaping; reject newline/NUL in a path. Scalars are finite decimal
numbers using locale-independent parsing. Reject unknown version/keys/slots, duplicate
records, trailing junk, bad ranges, and unsupported family combinations. Omitted fields
inherit parent values or the schema's defaults. `parent` is optional; `family` cannot
contradict an inherited family without an explicit compatible conversion command.

`uv_transform slot offsetU offsetV scaleU scaleV rotationRadians` follows G02.
`sampler slot wrapU wrapV minFilter magFilter mipFilter anisotropy` uses repeat/clamp/mirror,
nearest/linear min/mag, and none/nearest/linear mip choices. Define every slot's color/data
interpretation in metadata; do not permit an arbitrary checkbox to reinterpret a normal
map as sRGB without a diagnostic. Named extension slots follow the same grammar.

An explicit `clear_texture <slot>` removes an inherited binding. An explicit
`inherit <parameter-or-slot>` is an editor action that removes the corresponding override
record; it is not another serialized value competing with the parent. Preserve comments
where the existing document editor can; document canonical rewrite if it cannot.

The example illustrates the proposed syntax; it is not a currently loadable asset.
Generate a MaterialSchema reference from the parser/parameter descriptors once implemented.

## 2.4 Defaults, bounds and color conventions

Base color components [0,1], metallic [0,1], perceptual roughness [0,1] with shader-only
minimum for numerical stability, normal scale [0,4], alpha cutoff [0,1], emissive RGB
nonnegative finite with an engine-defined upper bound. Preserve zero roughness in source
even if evaluation clamps to a documented minimum. Do not rewrite artists' values on load.

Text base/emissive colors use explicitly documented linear scene RGB. UI color pickers
perform the same conversion as other engine color fields. Texture base/emissive colors
use sRGB decoding; masks, metallic/roughness, normals, and occlusion are linear data.
Imported color semantics follow the source format, not assumptions from filenames.

UI labels distinguish roughness from smoothness; if an importer converts smoothness,
record `roughness=1-smoothness` in its diagnostic report. Keep glTF's G roughness/B
metallic packing intact. Never globally switch a shared image's interpretation because
one material uses it as data and another as color.

## 2.5 UVs and tangent frames

Implement UV1 and texture transforms from G02 before claiming complete import fidelity.
Every texture slot can select a supported UV set. Missing requested data is an authoring
error; a visible fallback is allowed for interactive preview with a diagnostic.

For normal mapping, normal N transforms with inverse transpose; tangent T transforms
as a direction and is orthogonalized against N; bitangent uses handedness and the mirror
sign consistently with the mesh convention. Normalize after interpolation, guarding
degenerate length. Nonuniform negative scale is a required fixture.

For a normal map using a different/transformed UV parameterization, generate a matching
tangent basis or derive it from position/UV derivatives. Shared alpha evaluation must
use the same UV transform/filter/cutoff in depth, shadow, and color passes. Incorrect
shadow silhouettes cannot be patched with a different cutoff in just the shadow shader.

## 2.6 Detail maps and normal composition

Baseline detail layer: detail color, detail normal, optional roughness override, UV scale,
mask, strength, and distance fade. Detail color is a modulation around neutral white;
state the neutral encoding explicitly. Linear interpolation of base color with an entirely
different color is a separate layer operation, not the same as modulation.

Use a tested reoriented-normal-map composition, or a documented tangent-space slope
method with known limitations. For RNM with normalized base b and detail d:

```
t = b + (0,0,1)
u = d * (-1,-1,1)
r = normalize(t * dot(t,u) / t.z - u)
```

Require `t.z > epsilon`; provide a robust rotation-basis fallback for the degenerate
base opposite +Z. Check neutral detail `(0,0,1)` returns b and neutral base returns d.
Do not average normals as encoded RGB. Strength scales tangent slopes or uses a
documented normal-strength operation before composition; renormalize afterwards.

Fade detail by screen footprint or distance smoothly. Abruptly disabling normal detail
at a distance creates specular popping and invalidates temporal assumptions. Use the
same selected material generation and feature fade for current/previous history or mark
reactive change as necessary. Profile extra texture taps on the low preset.

## 2.7 Bounded material layers

Initial feature is at most two standard-material layers plus a mask, not an arbitrary
user shader graph. Each layer supplies base color/normal/roughness/metallic within the
same family. Mask can be a texture or vertex color channel. Evaluate the mask in its
declared space, clamp to [0,1], and blend material parameters before a single BRDF.

This is a convenient spatial material mixture approximation. It is not a physically
exact layered dielectric model. Clearcoat in chapter 5 has a separate energy-aware
surface model. Do not reuse parameter blending to claim coat physics.

For more than two layers, pre-bake an authored composite or require a later explicit
budgeted extension. Low material quality can select a generated composite retaining
appearance; if detail is dropped instead, show precisely which fields are omitted.
Do not silently drop the alpha mask that controls a fence's visibility.

## 2.8 Shader variants and hot reload

Define a bounded feature bitset and canonicalize impossible/redundant combinations.
Examples: opaque/mask/blend; normal map; detail; two-layer; skin/coat/transmission family.
Do not create a distinct shader for every scalar parameter or texture path. Scalars and
indices live in material buffers. Prewarm reachable variants during import/load/package.

Hot reload resolves/validates on workers, queues GPU resources, and swaps immutable
material generation at a frame boundary. Keep the last good generation on compile or
asset failure. A material changing alpha mode must update pipeline, depth/shadow eligibility,
and transparency sorting coherently. Invalidate bake hashes and temporal history when
its optical appearance or topology requirements change.

## 2.9 Production decal authoring

Foundation G08 supplies projected D-buffers. Extend authoring with reusable decal assets
that reference `.swmaterial` entries in a restricted decal-compatible family. Asset data:
projection size, albedo/roughness-metalness channel enable flags, opacity, angle fade,
receiver mask, sort order, optional lifetime/fade defaults. Validate supported channels.
Normal decals remain a separate extension until their tangent/world-space accumulation
is defined and tested; do not silently approximate them as color.

Editor drag-and-drop surface placement uses the actual picked world normal, a stable
reference up vector, and a small projection depth that encloses the surface without
reaching through unrelated walls. Gizmo shows projection axis and receiver volume.
Grid snapping, rotate, duplicate, prefab inclusion, and undo must work.

Runtime impact API accepts hit position, normal, parent/receiver identity, decal asset,
seed, and lifetime. It creates ordinary tracked decal data. On rigid moving receivers,
store a local anchor and validate receiver generation. Destroying/reusing an entity
cannot transfer its bullet holes to a new entity. Skeletal attachment requires triangle/
barycentric or bone-aware data; report unsupported use until that system is implemented.

Persistent decals are authored assets/entities. Transient impact marks are pooled with
bounded lifetimes and an eviction policy. Essential gameplay markers are classified
separately from decoration and retain a readable low-cost representation on Off/Low.

## 2.10 UI routes and CLI

- Content > Create > Material / Material instance / Decal material.
- Material inspector: parent/source provenance, override indicators, slots, UV transforms,
  samplers, preview sphere/plane under selectable environment, effective-quality display.
- Mesh/Model inspector: assign instance per model part, reset to source, reveal material.
- Viewport: drag material onto selected object/part; preview then undoable assignment.
- Proposed `player --material-schema <file|->` emits format reference.
- Proposed `player --material-info <asset> --json <file>` resolves full inheritance,
  source/UV/sampler metadata, feature requirements, dependencies, and fallback reasons.
- Proposed `player --material-bake <asset> --output <asset>` creates an explicit composite
  where supported; never runs implicitly during game frames.
- Existing `--check`, screenshots, optimization, and package paths cover these assets.

## 2.11 Acceptance fixtures

Include transformed UV0/UV1 glTF materials, mirrored meshes, multiple instances, hot
reimport with reordered source materials, nearest pixel art, masked foliage, chrome/
roughness grid, fine detail normals, layer mask edges, and moving-door decals.

Numerical tests: RNM neutral identities, normal unit length, color decode/mip behavior,
inheritance determinism/cycle rejection, exact descriptor reuse, and stable override
resolution. Visual tests: alpha silhouettes agree between passes; no detail popping;
clear material provenance; no UV seam introduced by remapping.

Package a material whose textures live only in packaged content and whose parent is
not directly referenced by any scene. Dependency traversal must still include it.
Inspect peak memory/variant count and cold/warm load, not just steady-state screenshots.
