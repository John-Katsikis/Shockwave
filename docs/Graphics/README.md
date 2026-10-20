# Graphics implementation guide

Status: **specification, not a claim of implemented rendering features**. Updated
2026-10-20 after inspection of `f3e0dbd` and the current Vulkan renderer. The documentation
entry point is implemented separately from the graphics work described here.

Open this guide in Shockwave through **Help > Graphics implementation guide**. The
window lists the chapters and opens Markdown in the operating system's associated
application. It also lets you copy each path. This works from the project hub and from
an open project. From a terminal:

```sh
build/bin/editor --graphics-guide
build/bin/editor --graphics-guide-path
```

The first opens the editor with the guide window visible. The second prints the resolved
index path and exits without starting the engine. Source builds prefer the live `docs/`
tree; a copy next to the executable supports a relocated build. Rebuild the `editor`
target to refresh that bundled copy after editing documentation. Markdown remains the
authoritative editable source; no network connection or account is required.

## Read in this order

| Document | Purpose |
|---|---|
| [Foundation specification](../GraphicsImplementation.md) | Existing-code inventory; G00–G13; graph/lifetimes; FXAA/TAA; lightmaps/probes; SSAO; decals; clustering; conservative visibility |
| [1. Settings and texture residency](01-SettingsAndTextures.md) | Complete player controls, capability fallback, mip residency, budgets, bloom and shadow controls |
| [2. Material authoring and surface detail](02-MaterialsAndDecals.md) | Reusable materials, imported material fidelity, layers/detail normals, decals, authoring APIs |
| [3. Particles and transparency](03-ParticlesAndTransparency.md) | Soft particles, distortion, scene lighting, ordering, supported Effekseer features, budgets |
| [4. Color and atmosphere](04-ColorAndAtmosphere.md) | Working color spaces, exposure, white balance, LUTs, postprocess volumes, analytic/froxel fog |
| [5. Character and specialized shading](05-CharacterShading.md) | Clearcoat, transmission, skin, hair cards, eyes, quality fallback and tests |
| [6. Verification and delivery](06-Verification.md) | Integration order, code ownership, UI routes, schemas/CLI, fixture suite, performance and release gates |
| [7. Mathematical contracts](07-MathematicalContracts.md) | Derivations, reference cases, numerical boundaries, and what each argument does not prove |

The foundation remains authoritative for G00–G13 unless a chapter explicitly tightens
or extends a contract. These chapters add A00–A10, the remaining production features.
Do not implement two material formats, two preference systems, two fog passes, or two
temporal histories because two documents mention them. A06 promotes the foundation's
deferred fog item into a scoped optional feature. A04 extends its particle/reactive-mask
requirements. A01 fills the texture-quality gap; A00 fills bloom/decal/settings gaps.

## What is currently present

The inspected Vulkan path provides forward metallic/roughness PBR, UV0 textures and normal
maps, global environment IBL, cascaded and local-light shadows, bloom, tone mapping,
GPU skinning, LOD/frustum culling, and a partial Effekseer bridge. Anisotropic sampling
exists through a shared sampler; this is not yet a complete player texture-quality API.

No dedicated TAA/motion-vector, SSAO, scene bake/probe, decal, volumetric-fog, or grading-LUT
passes were found in the inspected frame graph. Imported material occlusion is not SSAO
or a baked scene lightmap. Alpha blending is not physically modeled transmission.
Effekseer availability does not imply every Effekseer material feature is supported.
The separate CPU raymarch plugin is a prototype, not the implementation target for this
Vulkan roadmap; preserve its build and advertise its actual capabilities.

## Implementation rules

1. Read `AGENTS.md`, [Architecture](../Architecture.md), and [UI](../UI.md). Preserve
   other agents' changes and coordinate renderer ownership before editing code.
2. Treat chapter APIs and filenames marked proposed as design contracts to implement,
   not existing symbols. Locate actual current owners before editing.
3. Implement one batch at a time. Each batch includes scene/asset metadata, applicable
   validation, editor controls, CLI access, backend/null fallback, packaging, and evidence.
4. All features are budgeted. Optional effects have a genuine off path. Keep appearance
   authored by the developer distinct from personal quality preferences.
5. Store inspectable text source and documented bulk data. Provide encode/decode/inspect
   tools for new proprietary binary formats. Never serialize native object memory.
6. Numerical derivations justify limited mathematical claims. They do not prove visual
   quality, frame rate, race freedom, or portability. Those require tests and measurements.
7. Do not replace mature dependencies or working engine subsystems without evidence.
   Do not introduce a shader compiler framework or render graph rewrite beyond the
   explicit needs of the foundation.

## Target game and target quality

The target is a bounded-map shooter with symmetric **1v1 through 32v32** configurations.
Graphics budgets must account for 64 animated players, weapons, effects, decals, and
shadows, not only 64 static mesh transforms. Different match sizes need not change
visual identity, but their workload must be represented in benchmarks.

"AAA-looking" is an artistic acceptance target, not a number of effects. The required
reference scene is one finished combat room and adjoining outdoor/indoor transition:
consistent authored assets; a first-person weapon; a moving character; rough and metallic
materials; muzzle flash, smoke, impacts; doorway lighting; and readable gameplay.
Validate it in motion. Art direction, geometry, UVs, textures, animation, camera motion,
lighting composition, sound, and performance remain separate production responsibilities.

## Batch overview

| Batch | Delivery | Required prerequisites |
|---|---|---|
| A00 | Unified graphics controls, effective settings, genuine off paths | G00; existing settings |
| A01 | Texture residency/quality and upload budgets | A00, G02 resource contracts |
| A02 | Material instances, layers, normal detail, import diagnostics | G02, A01 |
| A03 | Production decals and gameplay impact authoring | G08, A02 |
| A04 | Particle compatibility, soft particles, distortion, ordering | G03, G09 interfaces; G11 for local-light quality |
| A05 | Grading, white balance, exposure, postprocess volumes | G00, G10 integration contract |
| A06 | Analytic fog then optional froxel lighting | G03, G11, A05 |
| A07 | Clearcoat and transmission | A02, G07, A04 scene-color contract |
| A08 | Skin diffusion and eyes | A02, G06, G09; A07 for corneal option |
| A09 | Hair cards and transparency quality | A02, A04, G09/G10 |
| A10 | Representative 1v1/6v6/16v16/32v32 validation and packaging | All selected batches |

Character features are content dependent. A helmeted distant character does not need
the same shader budget as a close-up face. A batch may ship with a declared supported
subset, but cannot silently report unsupported authoring features as successful.

## Related existing documentation

- [Graphics settings](../Graphics.md) and [generated schema](../GraphicsSchema.md)
- [Lighting and bloom](../Lighting.md), [shadows](../Shadows.md), [effects limits](../Effects.md#limits)
- [Assets](../Assets.md), [optimization](../Optimisation.md), [packaging](../Packaging.md)
- [Renderer explanation in the Shockwave Book](../Book/13-rendering.md)
