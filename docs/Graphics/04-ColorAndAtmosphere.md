# 4. Color, exposure, grading and atmosphere

[Guide index](README.md) · [Previous](03-ParticlesAndTransparency.md) · [Next](05-CharacterShading.md)

Proposed implementation: A05–A06. This promotes a bounded optional fog implementation
from the foundation's deferred work, without requiring clouds or dynamic GI.

## 4.1 Scope and linear-light contract

Current tone mapping already includes ACES, AgX, Reinhard, None, and manual exposure.
Preserve those curves and existing scenes by default. Add creative grading and optional
adaptation around them; do not silently switch the project's look or change stored light
intensities to match another engine's units.

Working space for this baseline: linear RGB with sRGB/Rec.709 primaries and D65 white.
HDR scene values can exceed one. Tone mapping produces bounded SDR linear display RGB;
the final sRGB attachment performs output encoding exactly once. HDR scene storage is
not HDR-monitor output. Wide-gamut/HDR-display support requires another explicit contract.

Pipeline contract:

```
linear lighting, surface effects, fog/transparent composition
temporal resolve (if selected, still unexposed scene RGB)
luminance metering (ungraded, before bloom)
bloom (existing scene-linear behavior)
white adaptation -> exposure -> existing tone mapper
creative display-linear grading LUT
output scaling / selected spatial AA with its documented encoding
game UI and editor overlays in their existing output contract
single final sRGB encoding
```

If technical constraints require scaling at another stage, document/retest it. UI never
enters the luminance histogram, bloom, history, or scene grading. Debug views explicitly
declare bypasses. Do not apply a LUT to normals/velocity merely because it is the last pass.

## 4.2 Postprocess authoring and volumes

Proposed `PostProcessVolume` registered component: enabled, unbounded flag, local box
half-extents, priority integer, blend distance in meters, weight [0,1], and profile asset.
Transform positions/orients finite volumes. Profile is a proposed `.swpostprocess` text
asset with `swpostprocess 1` header and typed named settings using the common parser.

Profile groups: manual exposure; auto-exposure limits/speeds/metering; white point;
creative LUT/reference and intensity; bloom authored overrides; optional fog parameters.
Omitted groups inherit scene defaults. Preferences limit or disable supported groups
without editing profiles. A profile cycle/missing dependency is a check error.

Evaluate volume membership at camera position initially, once per view. For finite boxes,
compute inside/boundary distance in world units even under nonuniform scale. Define
falloff as `w = authoredWeight * smoothstep(0,blendDistance,insideDistance)`; unbounded
volumes have w=authoredWeight. A zero blend distance means hard boundary with a warning
for animated cameras. Outside volumes contribute zero in this baseline.

Process priorities ascending; within equal priority normalize weights and compute a
group result, then blend that group over lower-priority result by clamped total coverage.
Use stable IDs for discrete choices. Blend exposure in stops, not multiplicative exposure
factors; blend continuous color transforms only in declared spaces. Do not average LUT
file indices. Either blend evaluated LUT outputs or preblend compatible LUTs off-thread.
Limit active LUTs to a bounded count and report approximation when exceeded.

Changing camera/view does not mutate global profile state. Postprocess volumes support
prefabs, undo, duplicate, hot reload, and debug gizmos showing priority/blend boundary.

## 4.3 White balance

Baseline authoritative input is source-white chromaticity xy; UI labels it "Illuminant
white to neutralize". This avoids ambiguous warm/cool slider conventions. Default D65:
x=0.3127, y=0.3290. Validate x>0, y>0, x+y<1 and a safe adaptation gain limit.

Compute white XYZ with Y=1: `W=(x/y,1,(1-x-y)/y)`. Bradford cone matrix:

```
M = [ 0.8951  0.2664 -0.1614
     -0.7502  1.7135  0.0367
      0.0389 -0.0685  1.0296 ]
A = inverse(M) * diag((M*W_D65)/(M*W_source)) * M
RGBadapt = XYZ_to_linear_sRGB * A * linear_sRGB_to_XYZ
```

Use a consistent published RGB/XYZ matrix pair and column-vector convention, stored in
one CPU/shader contract with reference vectors. Guard near-zero cone values and reject
unreasonable white points before inversion. Compute the matrix on CPU when parameters
change; no per-pixel matrix inverse. D65->D65 must be identity within floating tolerance.

A later Kelvin/tint UI converts through a documented color-temperature model into xy;
retain xy in authoritative data or serialize the model/version. Do not implement Kelvin
by multiplying red and blue with arbitrary constants. Creative warm/cool looks belong
to the LUT or a separately labeled artistic adjustment.

## 4.4 Creative LUTs

Support text `.cube` 3D LUTs initially. Required directives: TITLE optional, LUT_3D_SIZE,
DOMAIN_MIN/MAX optional with documented defaults, then exactly N^3 RGB entries. Initial
N choices 17,33,65; enforce memory/count limits before allocation. Reject unsupported 1D
or mixed forms rather than guessing. Preserve an explicit input/output encoding tag in
the engine sidecar because `.cube` alone does not establish a universal color pipeline.

Canonical engine LUT domain is **tone-mapped linear display RGB [0,1]**. An imported
display-encoded LUT must be converted or wrapped with decode/encode in the declared order;
do not silently treat it as linear. Document table iteration order and validate with
axis ramps, not only an identity LUT where indexing errors may be less obvious.

Upload a 3D image or supported 2D atlas fallback. For a 3D texture of N samples per axis:
`sampleUV = (color*(N-1)+0.5)/N`. Clamp addressing, use trilinear interpolation initially,
and compare with CPU reference. No repeat sampling. Do not clamp HDR before tone mapping
just to fit the LUT. Blend graded and ungraded display-linear RGB by profile intensity.

Keep editable source LUT and metadata. Any optimized binary has inspect/export/import
and source hash. A LUT loads/prewarms as an asset, never parsed every frame. Export a
neutral LUT through a proposed CLI command so artists can grade a known contract.

## 4.5 Auto exposure

Auto exposure is optional and off for existing scenes. A competitive shooter may choose
fixed exposure deliberately. Authoring fields: enabled, compensation stops, minimum/
maximum exposure stops, target gray, low/high histogram percentile, brighten/darken
adaptation times, metering mask, and reset-on-cut behavior. Validate finite ordered ranges.

Build a bounded log2-luminance histogram in compute from unexposed, ungraded scene color
before bloom/UI. Rec.709 luminance weights: (0.2126,0.7152,0.0722). Ignore invalid values;
clamp luminance to an explicit positive floor. Use e.g. 128 bins over [-12,+16] log2 units
initially. Record clamped fraction so an inappropriate range is visible. Optional masks
weight important screen regions and must have deterministic handling of zero total weight.

Trim the configured percentile tails, estimate mean log luminance, then set
`targetEV = log2(targetGray) - meanLogLuminance + compensation` within author limits.
If no valid pixels remain, retain last valid exposure or authored fixed exposure.
Histogram/reduction uses no CPU readback for the frame's display; optional diagnostics
read completed data asynchronously.

Adapt in stop space with `a = 1-exp(-dt/tau)` and
`EVnew = EVold + a*(targetEV-EVold)`, choosing documented tau for increasing/decreasing
exposure. Use actual elapsed display time, clamp pathological pauses, and reset on cuts
according to policy. Tau is a time constant, not an undocumented "speed" reciprocal.
This filter's fixed-target frame-partition invariance is derived in chapter 7.

Current TAA history is unexposed scene RGB; keep it that way initially. If pre-exposure
is later introduced, scale history consistently and include the factor in its validity
contract. UI brightness must not adapt with the scene. A muzzle flash must not cause
several seconds of unwanted darkening under the default shooter profile.

## 4.6 Analytic fog first

Define extinction sigma_t in inverse meters, scattering albedo in [0,1], and a linear
fog source color/illumination term. Distinguish extinction from an arbitrary opacity slider.
For a homogeneous medium over distance d:

```
T = exp(-sigma_t*d)
C = T*surfaceRadiance + (1-T)*fogRadiance
```

This is a constant-source approximation. It is useful, cheap, and resolution independent,
but not volumetric local-light scattering. Apply it to sky using a declared fog maximum
distance. Reconstruct actual ray distance, not just axial view depth: off-axis rays are
longer. Respect a near exclusion/start distance by integrating only the included segment.

Optional height density: `sigma(y)=sigma0*exp(-k*(y-y0))`. Along a normalized ray with
vertical component dy, optical depth is
`tau = sigma0*exp(-k*(originY-y0))*L * (-expm1(-k*dy*L)/(k*dy*L))`.
The ratio tends to 1 as k*dy*L tends to zero. Use a stable small-x branch; bound exponent
and optical depth before overflow. Under negative dy, density increases downward; do not
accidentally create negative extinction. Clamp density to an authored physical maximum.

## 4.7 Optional froxel fog

After analytic mode passes, add a view-aligned 3D grid. Initial Low 16x16 render-pixel
tiles by 48 depth slices; Medium 12x12 by 64; High 8x8 by 64. Dimensions depend on actual
render size; cap cell count/memory and report effective grid. Use finite fog distance,
log slices for perspective, and a tested orthographic policy. These are initial budgets.

For each cell, evaluate density, scattering/extinction, static/global incident illumination,
and selected clustered lights with existing shadow sampling where enabled. No private
Physics geometry queries from shaders. Fog's light lists must cover its entire volume,
not just opaque-visible depth. Density volumes are registered box/sphere components with
bounded textures/noise parameters and explicit priority/additive density rules.

Henyey–Greenstein phase for propagation-direction cosine mu:
`p(mu)=(1-g²)/(4*pi*(1+g²-2*g*mu)^(3/2))`. Here mu=dot(incoming propagation,
outgoing propagation toward camera); g>0 is forward scattering. If a light vector points
from sample toward light, incoming propagation is its negative. Document signs and test
the bright lobe; do not copy a formula using another direction convention unchanged.
Clamp g to a conservative range such as [-0.9,0.9] for stable low-resolution integration.

Accumulate front-to-back over cell segments with constant source q per meter:

```
segmentT = exp(-sigma_t*ds)
segmentS = q * (-expm1(-sigma_t*ds)) / sigma_t   // tends to q*ds at sigma_t=0
S += Tprefix * segmentS
Tprefix *= segmentT
```

q includes scattering coefficient times incident radiance/phase and any emissive medium
term; do not multiply by sigma_s twice. RGB extinction can be a later extension; baseline
scalar extinction with RGB source is simpler to validate. Early termination at small T
must have a documented remaining-radiance bound or be labeled a quality approximation.

Store integrated S and T along the view for surface sampling. Depth-aware spatial
interpolation prevents bleed across geometry; do not use opaque depth to erase fog
needed by transparent foreground. Initially no fog temporal accumulation: deterministic
sampling exposes true cost. Add history only after motion/light-change rejection and
trail tests; camera TAA history alone does not solve moving-light fog history.

## 4.8 Transparent and refractive composition

Opaque color at depth d is fogged as `C=S(d)+T(d)*L`. A straight-alpha surface at dp
over an already-fogged background uses source `S(dp)+T(dp)*Lp` in alpha-over. This
preserves shared front-segment scattering under the surface-coverage approximation.
Pure additive emission contributes `T(dp)*E` without another S term. See chapter 7.

Refraction is more complicated: do not sample an already fogged background and then
multiply it by front transmittance again. Retain an unfogged opaque color snapshot for
the single-layer transmission path; estimate background segment fog once, then apply
the front segment once. If the refraction path cannot integrate the bent segment,
declare the straight-segment approximation and test fogged glass against a reference.
No claim of correct multiple refractive layers in the baseline.

## 4.9 Public data and UI routes

Add registered postprocess/fog components and profile schemas. Keep artist density,
color, exposure, and grade in scenes/assets; keep fog quality/effect enable in preferences.
Proposed inspector fields have units in labels: meters, inverse meters, stops, seconds,
chromaticity, and linear color. Defaults reproduce old scenes.

Editor: Create > Postprocess volume / Fog volume; profile asset inspector; neutral LUT
export; histogram/exposure graph; volume gizmos; fog density/transmittance debug views;
effective-quality badge; comparison toggle that does not rewrite the scene.

CLI proposals: `--postprocess-schema <file|->`, `--postprocess-info --json <file>`,
`--lut-export-neutral --size 33 --output <file.cube>`, and existing check/capture/benchmark
paths. All authored ranges, references, count caps, and unsupported LUT encodings are
validated without a GPU. Package profiles/LUTs/noise textures transitively.

## 4.10 Tests and completion

White adaptation identity and neutralization; LUT identity/axis order/endpoints;
exposure ramps at 30/60/120 Hz; zero-valid histogram; adaptation reset on cut; grade
bypass on UI; no double gamma; black/bright/negative numerical input handling.

Fog tests: zero density identity, homogeneous closed form, partition invariance,
height horizontal-ray limit, positive extinction, phase normalization/sign, fogged
opaque/alpha/additive objects, sky limit, camera-inside-volume, overlapping volumes,
transparent object before/behind wall, moving flashlight, rapid turn, and quality changes.

GPU tests compare numerical outputs before tone mapping. Record integration approximation
error and performance/memory separately. A plausible screenshot does not validate an
incorrect phase sign or a repeated fog multiplication.

## References

- [PBRT: volume transmittance](https://pbr-book.org/4ed/Volume_Scattering/Transmittance):
  transport definitions. The discrete contracts and tests above are proposed for Shockwave.
- [PBRT: phase functions](https://pbr-book.org/4ed/Volume_Scattering/Phase_Functions):
  phase conventions; check vector directions before using a formula.
- [Filament PBR documentation](https://google.github.io/filament/Filament.md.html):
  useful implementation comparison for color/material handling; not a required dependency.
