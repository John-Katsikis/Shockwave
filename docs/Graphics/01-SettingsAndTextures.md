# 1. Graphics settings and texture residency

[Guide index](README.md) · [Next: materials](02-MaterialsAndDecals.md)

Proposed implementation: A00–A01. Read foundation G00/G02/G13 first. No rendering
features described in this chapter are implemented merely by publishing this file.

## 1.1 Outcome and ownership

A game developer can build a normal graphics menu using one engine API. Players can
disable optional effects, lower texture memory, and see the effective configuration.
Quality choices survive restart without editing shipped scenes or importing assets again.
Changing a setting cannot temporarily invalidate descriptors or stall every queue while
an asset is decoded. Low-quality presets preserve essential gameplay information.

Owners: Engine/GraphicsSettings for preference metadata and layering; Renderer for
portable capability/settings/resource requests; Assets for decoded mip data and caching;
RendererVulkan for uploads and actual allocations; GameUI/Editor for presentation.
Keep all Vulkan types below the Renderer boundary. RendererNull accepts/report settings
without creating images; it must not report graphical capability it does not implement.

## 1.2 One authoritative settings descriptor

Extend existing metadata rather than maintaining four lists in parsers, schemas, menus,
and renderer code. A descriptor records key, type, default, range/enum, user label,
tooltip, category, apply policy, and capability condition. Apply policies:

- `uniform`: next frame's immutable settings snapshot;
- `pipeline`: warm required variant, then publish at a frame boundary;
- `targets`: allocate replacement targets, then publish and retire old generation;
- `residency`: enqueue background work, retain valid old textures until replacements;
- `restart`: only where genuinely unavoidable, explicitly reported before applying.

Portable proposed API contracts, adapt naming to current GraphicsSettings API:

```cpp
GraphicsCapabilities graphicsCapabilities() const;
GraphicsSettings requestedGraphicsSettings() const;
EffectiveGraphicsSettings effectiveGraphicsSettings() const;
GraphicsApplyResult requestGraphicsSettings(const GraphicsSettings& requested);
GraphicsApplyStatus graphicsApplyStatus(GraphicsRequestId id) const;
```

Results distinguish invalid, pending, applied, applied-with-fallback, and failed. A failed
transition preserves the previous coherent configuration. Fields include generation,
reason code, requested/effective value, progress where useful, and restart requirement.
Call sites cannot modify a renderer-owned settings structure in place from another thread.
Publish a validated snapshot/command through the existing threading model.

Preferences layer on top of project defaults using current precedence. A game menu's Save
writes the user preference layer. Reset restores the lower layer, not hardcoded values
that happen to match today's defaults. Preset application sets a defined group of keys;
changing an individual setting marks the preset Custom and does not reset unrelated keys.

## 1.3 Complete player-facing control matrix

Proposed additional fields; expose only implemented choices in generated schema:

| Key | Values | Effect and off contract |
|---|---|---|
| `texture_quality` | low, medium, high, ultra | Initial mip floor; actual allocation changes |
| `texture_budget_mb` | 64..16384 MiB, default project policy | Residency budget; not a promise that device memory exists |
| `anisotropy` | 1, 2, 4, 8, 16 | Clamp to device support; 1 disables anisotropic filtering |
| `bloom_quality` | off, low, medium, high | Off skips passes and releases dedicated chain after retirement |
| `bloom_scale` | 0..1 | Personal multiplier on authored intensity; zero is effective off |
| `decal_quality` | off, low, medium, high | Optional decorative decals; essential indicators have separate policy |
| `particle_quality` | off, low, medium, high | Off removes decorative particles; essential effects retain their game-approved representation |
| `soft_particles` | false, true | Off removes depth-fade work; no hidden full-resolution copy |
| `distortion` | off, low, high | Off removes scene-copy/accumulation work used only for distortion |
| `fog_quality` | off, analytic, low, medium, high | Optional atmosphere, subject to gameplay visibility policy |
| `color_grading` | false, true | Off bypasses optional creative LUT; output transfer and basic exposure remain |
| `auto_exposure` | false, true | Personal opt-out where game policy allows; fixed authored exposure fallback |
| `material_quality` | low, medium, high | Documented approximation per material family, not random missing textures |
| `subsurface_quality` | off, low, high | Off uses the material's specified opaque diffuse fallback |
| `transmission_quality` | off, low, high | Off uses authored simplified transparent fallback, not an invisible wall |
| `hair_quality` | low, medium, high | Card/lighting/AA budget; character silhouette retained |

Retain foundation `anti_aliasing`, `ambient_occlusion`, `local_reflections`, clustering,
and culling keys. Retain current `shadow_quality`, render scale, LOD, frame cap, VSync,
and low-latency controls. Validate every float for finiteness before clamping; reject
invalid authoring rather than coercing NaN into a shader constant.

Settings changing competition-relevant visibility need project rules. Decorative fog
can be disabled. A gameplay smoke grenade cannot become transparent on Low. The engine
provides essential/decorative classification and permitted preference ranges; the game
defines fair visibility. Never trust a client graphics toggle as the authoritative test
for whether a server-side hit or visibility rule is valid.

## 1.4 Genuine off paths and dependencies

Compute a feature dependency set each settings generation. A shared depth prepass stays
if AO is off but decals/TAA require it. Release only resources with no remaining consumers.
Do not report an entire shared prepass as "AO still running"; expose ownership in Stats.

An off path must satisfy all four properties:

1. No dedicated draw/dispatch for the disabled effect.
2. No dedicated per-frame CPU gathering/encoding beyond trivial dependency evaluation.
3. No dedicated persistent image/buffer residency after in-flight work retires.
4. No synchronous queue/device idle just to toggle it.

Shared pipelines/descriptors can remain cached within bounded cache policy. Distinguish
small reusable pipeline metadata from large effect targets. Reactivation warms resources
asynchronously and reports pending state instead of flickering between partial passes.

## 1.5 Bloom controls without changing the lighting authoring model

Keep Environment's `bloom_intensity` as the artist's scene value. Compute
`effectiveIntensity = authoredIntensity * playerBloomScale` only if quality is not off.
At zero, omit bloom. Preserve the existing energy-redistributing mix in tone mapping;
do not accidentally replace it with an additive glow while adding preferences.

Initial policy: Low starts at quarter render resolution with four levels; Medium starts
at half with five; High starts at half with six. Clamp dimensions to at least 1 and stop
when no useful next mip exists. These are proposed workload tiers, not measured defaults.
Changing filter resolution must preserve approximately the same perceived angular radius
and brightness; use impulses, flat colors, and bright patches to tune/test.

Changing bloom must not alter the stored Environment component or scene file. Inspector
shows authored intensity and, optionally, the current effective preview intensity with
the responsible user setting. A screenshot command can explicitly choose user preferences
or deterministic project defaults; the report records which layer was used.

## 1.6 Shadow settings

Current Off/Low/Medium/High/Ultra tiers already exist. Preserve their meaning during the
first settings integration. Expose them through the shared game-facing API and show
effective atlas dimensions, filter taps, active cascades, dropped shadow lights, and bytes.
Do not allocate Ultra atlases while Low is selected merely to make toggles instantaneous.

Artist controls remain per-light shadow casting/bias/softness and Environment distance/
cascades. Player quality may cap distance/cascades only through a documented policy.
Add no separate untracked "shadow override" global variable. Off skips maps and sampling
while leaving direct lighting intact. A lower tier never disables collision or light
entities. Compare silhouettes/acne/peter-panning at the same camera and light positions.

## 1.7 Texture quality is residency, not import compression

Existing `texture_compression` prepares data. It can reimport assets and is not suitable
as a player texture-quality switch. Preserve a full mip pyramid in the cooked/cache
artifact, then choose which mips to load. All supported quality levels must be available
without the original PNG/JPEG or an encoder on the player's computer.

Initial global mip floors: Ultra 0, High 1, Medium 2, Low 3 relative to the asset's cooked
top level. These are transparent initial definitions, not names tied to 4K assumptions.
Clamp to the final available mip. Allow asset categories to override policy: UI/font
atlases must remain readable; small lookup tables and shadow/depth resources are not
ordinary material textures; normal/color/mask maps may need coordinated quality.

For each texture track source dimensions, cooked top mip, requested first mip, resident
first mip, resident level count, format, actual allocation bytes, generation, and pending
upload state. A shader sampler LOD bias alone does **not** reduce allocated memory.

Baseline implementation uses replacement images containing only the resident mip tail:

```
source 4096 -> mips 4096, 2048, 1024, ..., 1
Low firstMip=3 -> GPU image base 512, then 256, ..., 1
```

The image's local mip zero corresponds to source mip three. Ordinary implicit derivative
LOD uses the smaller image dimensions correctly. Any explicit LOD shader API must map
source LOD to resident-local LOD with `max(0, sourceLOD-residentFirstMip)`. Do not apply
the quality mip bias a second time in the sampler. Keep logical texture handles stable
while changing physical images; include generation in descriptor safety/retirement.

## 1.8 Residency state machine

Proposed states: Unloaded, Reading, Decoding, UploadQueued, Uploading, Resident,
ReplacementPending, Failed. Each operation carries asset ID, content generation, and
request generation. A completion publishes only if those generations still match.
Changing from Ultra to Low while an Ultra upload is running must not later resurrect
Ultra accidentally. Cancel obsolete work cooperatively or discard its result safely.

Algorithm:

1. Determine quality floor and projected demand, bounded by asset policy and capabilities.
2. Estimate exact block-rounded bytes for candidate resident tails.
3. Reserve budget including temporary overlap of old/new resources and staging.
4. Read/decode on workers; validate sizes and hashes before allocation/upload.
5. Queue transfer on the renderer's supported upload path; declare dependencies.
6. Publish descriptor generation only after upload completion is guaranteed for readers.
7. Retire old image after all referencing frames finish; then release its reservation.

Avoid deadlock when replacement overlap exceeds budget: reserve a transition allowance,
evict other eligible resources, or temporarily publish a valid small fallback before
retiring/replacing an oversized texture. Never wait on the same queue from a worker
holding a resource lock needed by the render thread. Do not introduce a Vulkan sparse
residency requirement for this baseline.

## 1.9 Demand and eviction policy

Start with whole-level quality tails before adding streaming. An optional next phase
uses last-visible frame, screen size, category priority, and estimated texel density.
Normal-map demand may differ from color demand; initial coupled residency is acceptable
and must be reported. Use a deterministic tie-break by asset ID.

Add hysteresis: require sustained demand before increasing resolution, and a longer
invisibility window before eviction. Cap upload bytes and number of publications per
frame. A 64-player firefight must not cause every weapon texture to thrash simultaneously.
Prewarm essential character/weapon variants during match loading.

UI textures and essential fallback textures cannot be evicted without replacement.
Pinned does not mean exempt from total budget reporting. If essential data exceeds the
budget, report a minimum required size and apply a defined supported fallback; do not
oscillate forever trying to meet an impossible budget.

## 1.10 Formats, mips, and filtering

Color mips are filtered in linear light then encoded as appropriate. Do not average
sRGB bytes directly. Normal mips need vector averaging/renormalization and a documented
roughness/variance policy to avoid excessive sparkling. Masked textures need coverage-
preserving mip generation relative to cutoff; this cannot be replaced by treating alpha
as ordinary opaque color. Keep both source and optimized dependency hashes.

Query sampled/filter/blend/storage format capabilities independently. Desktop BC7 is
not guaranteed on every Vulkan device. Provide supported transcode/cooked alternatives
or RGBA fallback with honest memory accounting. Do not claim RGBA fallback costs the
same as BC7. Reuse the existing texture optimization/cache machinery where possible.

Sampler caching key includes wrap/filter/mip-filter/anisotropy. User anisotropy caps the
asset request; it does not change repeat into clamp or nearest pixel art into smooth
filtering. Device limits cap the effective value. Never create samplers every draw.

## 1.11 Memory math

For an uncompressed texture, sum actual mip widths/heights times bytes per texel.
For block formats, each mip size is `ceil(w/blockW)*ceil(h/blockH)*blockBytes`, including
the tiny mips. A geometric 4/3 estimate is useful for planning, not exact file validation.

At dimensions where the geometric approximation applies, dropping k top mips reduces
resident bytes by about `4^-k`. A full 4096-square BC7 chain is approximately 21.33 MiB;
starting at 512 is approximately 0.333 MiB. File headers, alignment, small blocks, staging,
and retained CPU copies add overhead. See the derivation in chapter 7.

Track device allocated bytes, resident payload bytes, upload staging, retained decoded
CPU pixels, and disk cache separately. On shared-memory machines these still compete
for system memory; double-count neither physical unified allocations nor overlook
distinct CPU copies. Report both categories and platform measurements where available.

## 1.12 UI, CLI, diagnostics and acceptance

Editor Preferences > Graphics and the game settings API expose A00 fields. Asset inspector
shows source/cooked/resident dimensions and category overrides. Window > Optimisation
shows requested/resident bytes, pending work, evictions, upload budget, and failure reasons.
Controls use existing gui properties/theme and undo for authored changes only.

Extend `--graphics-schema`, `--graphics-info --json`, `--optimise`, and `--check` as
described in the foundation. Proposed `--texture-info <asset> --json <file>` reports mip
metadata and dependency hashes without creating Vulkan. Runtime residency diagnostics
require an explicit renderer/session and must be labeled accordingly.

Tests: low-quality allocation actually smaller; all quality transitions; rapid toggles;
failed upload rollback; generation cancellation; missing mip rejection; exact BC block
sizes; color-space mip fixture; masked coverage; explicit LOD mapping; backend replay;
load/package without original sources; sustained memory under repeated level reload.

Completion gate: UI/CLI/serialized preferences agree, no renderer restart for ordinary
changes, no full-device idle on transitions, no first-use reimport on player machines,
and measured memory savings with documented visual tradeoffs. No fixed FPS gain is
promised: texture quality can improve memory pressure without changing a shader-bound
scene's average FPS.
