# 7. Mathematical contracts and reference cases

[Guide index](README.md) · [Previous](06-Verification.md)

These are limited derivations for implementation invariants. They are not proofs that
the engine will look AAA, hit a frame-rate target, or synchronize GPU resources correctly.
Convert them into independent CPU/GPU tests. A derivation is useful only when the code
uses the same units, coordinate conventions, domains and assumptions.

Notation: vectors are columns; distances are meters; z is positive forward view distance;
screen UV has origin top-left; radiance/color arithmetic is linear; pi is mathematical pi;
T is transmittance unless a section explicitly uses it as a tangent/direction.

## 7.1 Why diffuse irradiance divided by pi is the right shared quantity

For incoming radiance L(w), normal n, and upper hemisphere H:

```
E(n) = integral_H L(w) * max(dot(n,w),0) dw
B(n) = E(n)/pi
```

Under constant radiance C, the angular cosine integral is pi:

```
integral_0^(2*pi) integral_0^(pi/2) cos(theta)*sin(theta) dtheta dphi
  = 2*pi * [sin(theta)^2/2]_0^(pi/2)
  = pi
therefore B=C.
```

For cosine-weighted hemisphere sampling, density `p(w)=cos(theta)/pi`. The Monte Carlo
term for B is `L(w)*cos(theta)/(pi*p(w))=L(w)`. Thus averaging incoming radiance estimates
B without an extra pi factor. This assumes the sampler actually has that distribution.
Uniform hemisphere sampling instead needs weight `2*L(w)*cos(theta)`.

Reference: constant environment RGB (0.2,0.4,0.8) with no occluders returns exactly that
constant apart from arithmetic/texture sampling. Multiplying by pi or dividing again
is a bug. A Lambert receiver with reflectance rho produces `rho*B`; the existing PBR
shader's energy terms must be preserved around its selected diffuse irradiance input.

No-double-count rule: partition incoming paths by origin/type. The lightmap contains
environment visibility and indirect paths; runtime analytic direct evaluates direct
light paths at the receiver. Adding global diffuse IBL again counts environment paths
twice. This is a path-partition argument, not proof that the approximate bounce BRDF
matches the runtime BRDF exactly. Bright sun present in both an HDR map and an analytic
light is an authoring overlap that needs an explicit policy.

## 7.2 Diffuse SH band factors

With orthonormal real SH and a rotationally symmetric cosine kernel, each band l is
scaled by `K_l = 2 * integral_0^1 mu * P_l(mu) dmu` when storing E/pi.

```
P0=1:             K0 = 2*(1/2) = 1
P1=mu:            K1 = 2*(1/3) = 2/3
P2=(3mu²-1)/2:    K2 = 3/4 - 1/2 = 1/4
```

Project full-sphere radiance using `L_i=(4*pi/N)*sum(L(w)*Y_i(w))`, then multiply by
the factor of that coefficient's band. Do not apply convolution again at runtime.
For a constant C, only coefficient 0 is nonzero: `L0=C*sqrt(4*pi)` and
`Y0=1/sqrt(4*pi)`, so evaluation is C.

Test coefficients and evaluated lighting separately. Rotating a probe basis without
rotating coefficients/directions changes the lighting. Truncation to nine coefficients
can ring and produce negative reconstructed values; clamping is an approximation and
does not prove exact positivity or visibility. Signed coefficients themselves must not
be clamped, because that destroys the basis expansion.

## 7.3 Texture mip memory

For a power-of-two square N and b bytes per texel, ignoring block minimums:

```
M = b*N² * sum_(j=0)^m (1/4)^j
  = b*N² * (1-(1/4)^(m+1))/(1-1/4)
  < (4/3)*b*N².
```

Dropping k largest mips gives approximately `M/4^k` when many levels remain. Sampling
a coarser mip from an unchanged allocation gives no such memory reduction. That is why
A01 requires resident-tail images or another real residency mechanism.

BC7 has 16-byte 4x4 blocks. Exact per-mip bytes are
`16*ceil(width/4)*ceil(height/4)`. For a 4096 square chain through 1, total payload is
22,369,648 bytes (approximately 21.3334 MiB); for a chain beginning at 512 it is 349,552
bytes (approximately 0.33336 MiB). Test these exact integers independently of production
layout code. Allocator alignment/file metadata/staging are additional quantities.

## 7.4 Reverse-Z reconstruction and conservative occlusion

For the current infinite perspective projection, `d=near/z`, hence `z=near/d` for
d>0. Near is d=1; distant geometry approaches zero. d=0 is background and must not be
divided. General/orthographic reconstruction uses inverse projection instead.

With negative-height viewport and unflipped projection:
`u=(ndcX+1)/2`, `v=(1-ndcY)/2`. Inverse: `ndcX=2u-1`, `ndcY=1-2v`.
Test the four corners; the center alone cannot detect an inverted Y.

For a candidate object, let a be an upper bound on its nearest reverse-Z depth, and let
b_i be opaque depth at every covered sample. The object is entirely behind opaque depth
if `a < b_i` for all i. This is equivalent to `a < min_i b_i`. Therefore a conservative
hierarchy for this test stores MIN, not MAX. Include clear zero samples: one uncovered
sample prevents proof of complete coverage and must keep the candidate visible.

Reference: occluder depths [0.8,0.8,0,0.8], candidate nearest 0.5. MIN=0 correctly fails
the occlusion test. MAX=0.8 would incorrectly hide it. Numerical bias makes rejection
stricter (`a < min-bias`). Every touched hierarchy texel must cover the projected bound;
sampling only its center does not establish the "for all i" condition.

This proof assumes conservative projected bounds and valid current depth. It does not
justify stale previous-frame depth, near-plane-clipped bounding boxes, or transparent
occluders. Those require separate treatment from G12.

## 7.5 Jitter and motion reprojection

Let C and P be unjittered current/previous UV of the same surface. Store motion M=C-P.
Let current/previous jitter in normalized UV be jc and jp. Current raster sample U=C+jc.
The corresponding previous history location is P+jp, so:

```
P+jp = U - M + jp - jc.
```

This derives G09's formula. If motion already contains jitter, adding jp-jc again is
wrong. For screen jitter J pixels right/down, clip offsets must be
`(+2*Jx/W*clipW, -2*Jy/H*clipW)` under the negative-height viewport.

Example: C=(0.6,0.4), P=(0.5,0.4), jc=(0.001,0), jp=(-0.001,0). U=(0.601,0.4),
M=(0.1,0), previous UV=(0.499,0.4). A static surface with changing jitter has M=0,
but needs the jitter correction to sample the same surface location.

Depth rejection compares previous-history depth to the predicted previous depth of the
same current surface. Current depth is not equivalent when the camera translates.
Object history must be actual previous rendered/skinned pose, not an arbitrary previous
physics step. Correct algebra cannot repair a wrong identity or pose input.

## 7.6 Alpha compositing and soft fades

For straight color c and opacity a, premultiplied C=a*c and T=1-a. Front layer A over B:
`Cout=Ca+Ta*Cb`, `Tout=Ta*Tb`. Composition is associative because distributing products
gives the same nested expression. It is not commutative.

Example: half-opacity red over half-opacity blue on black gives (0.5,0,0.25); reversing
the order gives (0.25,0,0.5). Sorting effects as wholes can therefore change results when
their particles interpenetrate. An OIT approximation must declare how it differs.

A fade f must move the layer toward identity `(C=0,T=1)`. For premultiplied alpha this
means `(C'=f*C,a'=f*a)`. For additive E the neutral is E=0, so multiply E by f even if
the blend state ignores alpha. For multiply factor m the neutral is 1, so use
`m'=1+f*(m-1)`. These identities produce cheap unit tests for the soft-particle path.

For soft particles, gap=sceneZ-particleZ and f=clamp(gap/distance,0,1). With distance
0.2 m, gap -0.1 ->0, gap0 ->0, gap0.1 ->0.5, gap0.2 ->1. Sky is handled separately.
This is a geometric intersection approximation, not a physical volume simulation.

## 7.7 Homogeneous fog and partition invariance

Transmittance satisfies `dT/ds=-sigma_t*T`, T(0)=1, giving `T(d)=exp(-sigma_t*d)`.
For a constant source q per meter:

```
S(d)=integral_0^d exp(-sigma_t*s)*q ds
    = q*(1-exp(-sigma_t*d))/sigma_t.
```

At sigma_t=0 the limit is q*d, using the exponential Taylor expansion. Use `-expm1(-x)`
or a stable series for tiny x; subtracting nearly equal floating-point numbers loses
precision. If q=sigma_t*C_fog, this becomes `(1-T)*C_fog`.

Two consecutive segments combine as `(S1+T1*S2,T1*T2)`. Since
`exp(-sigma*d1)*exp(-sigma*d2)=exp(-sigma*(d1+d2))`, repeated homogeneous segments
agree with the single closed form. Test 1,2,16,64 segments. Heterogeneous cell sampling
still has discretization error; partition invariance is not exact there.

Reference: sigma_t=0.1/m, d=10m -> T=exp(-1)=0.3678794412. For constant fog radiance
one and black background, output is 0.6321205588. T must stay in [0,1] for nonnegative
extinction. Negative extinction would amplify light and is rejected as invalid input.

## 7.8 Height-fog horizontal limit

Along normalized ray y(s)=oy+dy*s and density sigma0*exp(-k*(y-y0)):

```
tau = sigma0*exp(-k*(oy-y0))*integral_0^L exp(-k*dy*s) ds
    = sigmaAtOrigin*L*(1-exp(-x))/x, x=k*dy*L.
```

As x->0, `(1-exp(-x))/x ->1`, so the horizontal ray has tau=sigmaAtOrigin*L.
Series: `1-x/2+x²/6-x³/24+...`. A stable branch around zero avoids a 0/0 NaN.
For x<0 density grows along the ray; the ratio remains positive. Limit density/optical
depth before overflow rather than clamping the ratio to an incorrect sign.

## 7.9 Phase normalization and direction

For HG with |g|<1 and mu in [-1,1]:
`p(mu)=(1-g²)/(4*pi*(1+g²-2g*mu)^(3/2))`.
Integrating azimuth gives `2*pi*integral_-1^1 p(mu)dmu=1` by substituting
u=1+g²-2g*mu. For g=0 the limit is isotropic 1/(4*pi).

For g>0 the denominator is smallest at mu=1, so the peak is aligned propagation.
If incoming light propagation and outgoing direction toward the camera align, a forward
scattering medium is bright. Using "direction toward light" in place of incoming
propagation flips that relationship. Numerical quadrature verifies normalization;
a backlit fog scene verifies the sign convention. Both tests are needed.

## 7.10 Fog plus transparent coverage

Let a surface at p have opacity a and radiance Lp. Let the opaque background beyond it
have radiance Lb. Denote camera-to-p fog S_p,T_p and p-to-background S_b,T_b.
Under simple coverage, the correct expression is:

```
C = S_p + T_p * [a*Lp + (1-a)*(S_b+T_b*Lb)]
```

The already-fogged background is `Cbg=S_p+T_p*(S_b+T_b*Lb)`. Alpha-over with source
`S_p+T_p*Lp` gives `a*(S_p+T_p*Lp)+(1-a)*Cbg`, which expands to the expression above.
Thus the front scattering term is not doubled. Additive emission has no coverage term:
it contributes T_p*E. Applying S_p again to additive fire would add fog energy per sprite.

This proof assumes the same ray/front medium and simple coverage. Bent refractive rays,
multiple scattering, and semi-transparent volumes require additional assumptions.
It does not justify fogging a refracted already-fogged background twice.

## 7.11 Exposure adaptation independent of frame partition

For fixed target E* and time constant tau, continuous adaptation satisfies
`dE/dt=(E*-E)/tau`, giving `E(t+dt)=E*+(E(t)-E*)*exp(-dt/tau)`.
Therefore alpha=1-exp(-dt/tau) gives the chapter 4 update.

Two timesteps dt1/dt2 multiply the remaining error by
`exp(-dt1/tau)*exp(-dt2/tau)=exp(-(dt1+dt2)/tau)`. Thus fixed-target results agree
for 30/60/120 Hz time partitions, aside from floating error. A changing target or
different brightening/darkening constants changes the piecewise solution and must be
tested separately. A fixed per-frame blend coefficient does not have this property.

## 7.12 Neutral LUT, white adaptation and bloom

For a linear ramp LUT indexed i=0..N-1, stored coordinate value is i/(N-1). To sample
value x in normalized texture coordinates centered on samples, use
`u=(x*(N-1)+0.5)/N`. Trilinear interpolation reproduces an affine identity function
inside the domain. Using u=x samples half-texel-shifted boundaries and can distort ramps.
Test colored axes to detect a swapped LUT axis/order.

White adaptation A=M^-1*diag((M*Wd)/(M*Ws))*M satisfies A*Ws=Wd whenever the cone
components are nonzero. If Ws=Wd, diagonal ratios are one and A is identity. This proves
white-point mapping for that linear model, not universal perceptual color appearance.

For a normalized bloom kernel K with K(constant C)=C, the existing mix
`(1-b)*C+b*K(C)` leaves constant fields unchanged. This is a DC-gain invariant. Finite
image borders, clipped values, and approximate down/up filters can break total-image
energy preservation; do not claim the constant-field test proves global conservation.

## 7.13 Refraction and attenuation

Decompose unit incident I into tangent part It and normal part -cosI*N. The refracted
direction is `eta*It - sqrt(k)*N`, k=1-eta²*(1-cosI²). Its squared length is
`eta²*(1-cosI²)+k=1`. If k<0 no real transmitted direction exists: total internal reflection.
At normal incidence the direction stays straight. At eta=1 it equals I.

For absorption sigma=-ln(C)/D and path length d, `T(d)=exp(-sigma*d)=C^(d/D)`.
Therefore T(D)=C and T(0)=1. Reference C=0.5, d=2D ->0.25. Zero channels require explicit
handling to avoid `log(0)`/`0*infinity`; a finite epsilon approximation must be documented.

These statements prove direction/attenuation identities, not correct screen-space hit
selection, Fresnel normalization, or multiple-interface rendering. Test those separately.

## 7.14 Normalized diffusion kernels

For nonnegative weights wi with positive sum, normalized blur `B=sum(wi*Ci)/sum(wi)`
is a convex combination. Constant C remains C; each channel remains between neighborhood
minimum and maximum. Rejection by depth/profile must be followed by renormalization to
retain this property. A nonzero center weight ensures a defined result.

This prevents artificial darkening at skin-mask boundaries, but is not a proof of global
energy conservation or a physically exact BSSRDF. Spatially varying normalized bilateral
kernels need not conserve the sum of every image pixel.

## 7.15 Anisotropic GGX NDF normalization

For a microfacet normal h on the upper hemisphere, define slopes sx=hx/hz, sy=hy/hz.
The solid-angle Jacobian is `dw=hz^3 dsx dsy`, so `hz*dw=hz^4 dsx dsy`.
The anisotropic NDF in chapter 5 becomes:

```
D(h)*hz*dw = dsx*dsy / (pi*alphaT*alphaB *
                         (1+(sx/alphaT)^2+(sy/alphaB)^2)^2).
```

Substitute qx=sx/alphaT and qy=sy/alphaB. The scale Jacobian cancels alphaT*alphaB.
In polar coordinates the integral is `integral_0^infinity 2*r/(1+r²)^2 dr = 1`.
Thus projected microfacet area is normalized for positive alphaT/alphaB. At equal alpha
values the expression reduces to isotropic GGX.

This proves the NDF condition only. It does not alone prove the full BRDF's reciprocity,
energy preservation, correct multiscattering compensation, or fidelity to hair fibers.
Test NDF quadrature, isotropic limit, grazing stability, and white-furnace response separately.

## 7.16 Proof-to-test checklist

For every numerical algorithm, record:

- input domain, units, coordinate/color conventions;
- finite/zero/degenerate behavior;
- exact identity or bound claimed;
- independent expected values and tolerances;
- approximation cases outside that claim;
- CPU test and GPU fixture exercising the same contract;
- visual/performance checks still required after the mathematical test passes.

Do not label a benchmark a proof, or a normalization derivation a guarantee of visual
quality. Keep those forms of evidence separate in each implementation batch report.
