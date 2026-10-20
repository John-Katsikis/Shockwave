# 5. Character and specialized shading

[Guide index](README.md) · [Previous](04-ColorAndAtmosphere.md) · [Next](06-Verification.md)

Proposed implementation: A07–A09. Implement only content-backed families, with explicit
fallbacks, after standard materials and lighting work. These features do not fix bad
normals, missing roughness maps, flat lighting, or poor animation.

## 5.1 Common shader-family contract

Extend `.swmaterial` metadata with a bounded family enum and validated compatible
extensions. Initial families: standard, skin, hair_card, transmission; standard can
optionally enable clearcoat. Eye assets combine existing family instances rather than
introducing a universal eye shader before a real asset requires it.

CPU and GPU descriptors contain explicit optional parameters with known defaults.
Missing extension parameters reproduce the current standard BRDF. Preserve source
extension metadata and warn when the importer cannot represent it. Do not claim glTF
extension support just because Assimp returns a mesh and base-color texture.

Each family supplies: parameter schema, texture interpretation, shader requirements,
alpha/depth/shadow policy, bake participation, temporal/reactive behavior, low-quality
fallback, material preview fixture, and package dependencies. Test the actual exported
source asset as well as hand-authored engine materials.

## 5.2 Clearcoat: A07a

Use a separate dielectric specular layer for coated paint/varnish. Proposed fields:
clearcoat factor [0,1], perceptual roughness [0,1], optional coat normal texture/scale,
with fixed IOR 1.5 initially. Keep coat normal distinct from base normal where authored.
The normal-incidence air/coat Fresnel value is `((1.5-1)/(1.5+1))²=0.04`.

Evaluate a normalized coat microfacet lobe and attenuate the base contribution by the
energy transmitted through the coat. A useful initial approximation is:

```
Fcoat(theta) = F0 + (1-F0)*(1-cos(theta))^5
baseWeight = (1-c*Fcoat(thetaIncoming)) * (1-c*Fcoat(thetaOutgoing))
f = baseWeight * f_base + c * f_coat
```

This is an approximation to a layered BSDF, not a proof of exact transport through a
rough coat. Decide whether to account for internal reflection, changed base interface,
and roughness coupling in a later tested model. Do not add a full-strength coat highlight
to an unchanged base and claim energy conservation.

Use consistent direct-light and IBL evaluation. If the coat uses a different normal or
roughness, its reflected direction/prefiltered mip and BRDF lookup must reflect that.
Do not use the base roughness for both lobes accidentally. AO policy remains explicit:
screen AO is not an arbitrary multiplier on all coat highlights.

Low material quality can fold coat into a documented single-lobe approximation or use
the authored standard fallback. It must not change a surface into a different alpha mode.
White-furnace integration, roughness sweeps, grazing views, and coat factor zero identity
are mandatory. Any integrated energy above tolerance is a bug or documented approximation
requiring refinement, not something to fix by globally dimming the scene.

## 5.3 Transmission and glass: A07b

Ordinary alpha blending models coverage/compositing and is not equivalent to light
transmission through a dielectric. Proposed fields: transmission [0,1], IOR [1,2.5],
thickness in meters or supported thickness texture, attenuation color [0,1], attenuation
distance >0, roughness, and thin/solid mode. A single refractive surface layer is the
initial rendering limit; multiple nested media are not silently supported.

For incident propagation vector I pointing into the surface and outward normal N,
`cosI=-dot(N,I)` and `eta=etaIncident/etaTransmitted`. With both normalized:

```
k = 1 - eta*eta*(1-cosI*cosI)
if k < 0: total internal reflection
else transmittedDirection = eta*I + (eta*cosI-sqrt(k))*N
```

Select the correct interface/normal for entering versus exiting. Clamp only tiny negative
k attributable to rounding; genuine total internal reflection must not become an invalid
sqrt. Fresnel partitions reflection/transmission. Implement exact dielectric Fresnel or
a documented approximation; both have normal-incidence and grazing reference tests.

For absorption, `sigma_a=-ln(attenuationColor)/attenuationDistance`, channel-wise, and
`T=exp(-sigma_a*pathLength)`. Clamp source color for finite internal arithmetic with a
documented error bound or special-case zero attenuation channels. At zero path length T=1;
at the authored attenuation distance T equals the color, within chosen clamping tolerance.
Track path length in world units under scaled instances. Do not multiply attenuation
distance by scale twice or treat thickness texture values as already world-space.

## 5.4 Screen-space transmission path

Retain an immutable unfogged opaque HDR snapshot and depth pyramid. Trace a bounded
refracted ray against visible depth for an approximate background hit; sample roughness-
filtered background mips. Terminate after a fixed quality-dependent step budget. Bound
thickness/ray distance, guard sky and near-plane intersections, and validate hit confidence.

Fallback on off-screen/missing hits is local/global reflection/environment information
or an authored approximation, explicitly reported by debug view. It cannot reconstruct
hidden geometry. A background color copy must not be read from the same image being
written. Distortion and transmission share snapshot infrastructure only when their
ordering/contents match; otherwise use separate declared generations.

Thin glass and solid glass use different thickness assumptions. Do not sample a wall's
depth and pretend it is the glass object's backface. A thickness texture or second-face
pass is an approximation that must be specified. Solid volume authoring requires closed
consistent geometry where that assumption is used; `--check` reports unsupported cases.

Compose reflected/transmitted/diffuse terms once with declared energy weights. Transmission
factor zero reproduces standard material; IOR one removes interface bending/Fresnel but
can retain medium absorption. Fog front/background segments follow chapter 4 once each.
Off quality uses an authored simplified glass material, not invisible geometry or opaque
white by default. High may increase trace steps/mip quality, not remove correctness checks.

Temporal handling: mark changing/refraction coverage reactive; use valid surface motion
for the front interface. Background reprojection is not automatically the same as front
surface motion. Inspect camera-pan sequences and moving objects behind glass.

## 5.5 Skin diffusion: A08

Implement an optional screen-space diffusion approximation for close visible skin.
Prerequisites: dynamic-object indirect lighting, correct normals/materials, stable motion,
and an explicit diffuse/specular split in the forward shading outputs. Skin quality does
not replace good albedo/normal/roughness assets, facial geometry, or animation.

Proposed skin profile asset: per-channel diffusion radius in millimeters, nonnegative
kernel weights or a small normalized Gaussian-mixture definition, strength [0,1], maximum
screen radius, thickness/backlighting parameter policy, and low fallback. Profile IDs
and receiver IDs are bounded integer masks, with zero reserved for non-skin.

Store diffuse illumination before the receiver's high-frequency albedo modulation where
the BRDF factorization permits; preserve the original specular/emission separately.
Blur only the designated diffuse term, then recombine with albedo and unchanged specular.
Document this approximation; it is not a general BSSRDF solution and cannot model all
multiple scattering through arbitrary geometry. Do not blur the final HDR image, which
would smear eyes, specular highlights, hair and weapon edges.

Projection radius for perspective, using positive view distance z and vertical focal
scale fy in pixels: `radiusPixels = radiusMeters * fy / max(z,epsilon)`.
For orthographic cameras use the actual meters-to-pixels scale. Clamp to the profile's
maximum and report clamping in debug stats. Unit conversion mm->m occurs once.

Use separable bilateral diffusion as an initial approximation. For each channel c:

```
weight_i = spatialKernel_c(offset_i) * depthCompatibility * normalCompatibility
           * sameSkinReceiverAndProfile
blurred_c = sum(weight_i * diffuse_i.c) / sum(weight_i)
```

Keep a nonzero center contribution; if all neighbors reject, return center. Never mix
different players' skin across a narrow screen gap. Depth rejection uses reconstructed
world/view distance, not reverse-Z differences. Renormalize after rejecting samples so
a constant field remains constant. Vertical/horizontal bilateral filtering is an
approximation; measure silhouette and thin-feature artifacts.

Thickness-dependent backlighting is a separately defined term, with authored thickness
and bounded energy. Do not add a red glow whenever a light is behind a head and call it
subsurface scattering. If physical units are not matched to current light intensity,
keep a clearly documented artistic approximation and test against a controlled fixture.

Low/off uses the authored opaque skin BRDF without diffusion. At small projected size,
fade the diffusion smoothly and skip work where no skin needs it. Avoid reserving an
entire extra full-resolution G-buffer when no visible skin profile is active.

## 5.6 Eyes

Use a controlled asset contract: opaque iris/sclera plus optional thin corneal transmission/
coat layer, with correct normals and scale. Start with the standard/coat approximation
for Low and transmission for close High. Avoid two coplanar independently depth-writing
surfaces. Iris depth/parallax and tear-line geometry are authoring features with explicit
material assignments, not guessed from texture names.

Tests: neutral gaze, grazing view, flashlight reflection, bright/dark transition, close
camera, and rapid head movement. No persistent double highlight, incorrect iris inversion,
or cross-eye blur from SSS. Eye brightness must not be faked with uncontrolled emissive
to hide missing lighting.

## 5.7 Hair cards: A09

Baseline hair geometry is cards with authored tangent/root-to-tip direction, alpha mask,
normal, and optional roughness/color variation. Full strand simulation/rendering is a
separate workload and not required here. Validate tangent data and fallback direction
when degenerate; don't infer fiber direction from world up.

Use anisotropic GGX on cards as the first fully specified approximation. It models an
anisotropic dielectric surface, not cylindrical fiber transport; do not advertise it as
a Marschner/strand-scattering model. Let T be authored root-to-tip tangent orthogonalized
against N and B the consistent bitangent. For perceptual roughness r and anisotropy a:

```
alpha = max(r*r, 0.0025)
aspect = sqrt(1 - 0.9*abs(a))                 // a in [-1,1]
alphaT = alpha/aspect; alphaB = alpha*aspect
if a < 0: swap(alphaT,alphaB)
h = normalize(incomingTowardLight + outgoingTowardCamera)
D(h) = 1 / (pi*alphaT*alphaB *
       ((dot(h,T)/alphaT)^2 + (dot(h,B)/alphaB)^2 + dot(h,N)^2)^2)
Lambda(w) = (sqrt(1 + ((alphaT*dot(w,T))^2 + (alphaB*dot(w,B))^2)
                          / dot(w,N)^2) - 1) / 2
G = 1 / (1 + Lambda(wi) + Lambda(wo))
fSpec = D*G*Fresnel / (4*dot(N,wi)*dot(N,wo))
```

Only evaluate this reflection lobe when both normal cosines are positive. Use numerically
stable visibility evaluation at grazing angles instead of dividing by an unguarded zero;
reject a zero-length half vector. F0 starts at 0.04; conserve the corresponding diffuse
share using the existing dielectric material convention. Convert into the engine's direct-
light intensity convention consistently, not by adding a second pi factor arbitrarily.
The anisotropic NDF normalization is derived in chapter 7.

When a=0, the NDF is isotropic GGX. Match the existing shader's isotropic visibility and
energy-compensation conventions where possible; compare exact outputs before accepting
the extension. High-quality anisotropic environment reflection needs an explicitly
validated approximation or preintegration because the existing isotropic cube mip is
not a full anisotropic convolution. The initial fallback uses effective roughness with
that limitation reported. A later fiber-specific model requires its own pinned formula,
normalization, absorption parameters, and reference fixtures.

Choose coverage strategy per quality: stable alpha mask for Low; coverage-preserving
mips for all; optional temporally jittered coverage only with validated TAA; alpha-to-
coverage only if MSAA exists and is correctly integrated. There is no MSAA assumption
in the current forward pipeline. Sorted alpha blend is possible but needs chapter 3's
ordering and overdraw budget. Avoid a mode that looks acceptable only in a still frame.

Hair receives appropriate direct and probe lighting. Shadow pass uses the same alpha
coverage policy or a tested shadow-specific approximation with explicit limits. Cards
must not produce a solid helmet shadow because the depth shader omitted alpha testing.
Motion uses actual skinned geometry and valid history, not only head/world transform.

## 5.8 Asset/UI/API requirements

Material inspector groups family-specific fields, shows units, hides inapplicable groups,
and displays fallback/effective quality. Preview presets: white furnace, studio key/rim,
dark room, strong grazing light, and authored environment. The furnace is diagnostic
linear output, not a tone-mapped glamour preview.

Register profile/material fields in schema; ordinary fields use shared gui metadata.
Content browser previews assets and dependency chains. Proposed `--material-info` includes
family, supported extensions, required buffers/passes, effective fallback, and estimated
variant/resource cost. `--check` validates unsupported combinations and payloads.
Game APIs select material instances/profile assets without exposing Vulkan pipelines.

Package all profile textures and variants; keep source plus inspectable optimized formats.
Run without the author's DCC application or shader compiler. Backend reload reconstructs
portable material/profile state and invalidates affected temporal history.

## 5.9 Performance and acceptance gates

Measure one close character and 64 representative distant/midrange characters separately.
Screen coverage and material taps matter more than entity count alone. Use projected
size to choose expensive diffusion/hair detail within explicit quality limits. Do not
make player identity/team classification depend on a dropped cosmetic material layer.

Required tests: coat zero identity/white furnace; Snell direction/unit length/TIR; absorption
distance/zero-thickness; refraction fallback; SSS constant-field identity and mask isolation;
skin radius scaling; hair alpha mip coverage and moving-camera sequences; eye close-up;
all Off/Low fallbacks; resize/hot reload/package; no first-use shader compilation.

## References

- [Khronos transmission extension](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_transmission/README.md)
  and [volume extension](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_volume/README.md):
  source asset semantics; screen-space limitations here remain engine-specific.
- [Khronos clearcoat extension](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_clearcoat/README.md):
  imported parameter/texture conventions.
- [Filament material documentation](https://google.github.io/filament/Filament.md.html):
  reference for reviewed material models and energy tests. Do not copy isolated formulas
  without their interface and normalization assumptions.
