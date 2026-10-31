# Fog

Fog comes from postprocess profiles ([PostProcess.md](PostProcess.md)): any volume whose
profile gives `fog_density` above 0 fogs the scene where it applies. Keys are listed in
[PostProcessSchema.md](PostProcessSchema.md).

## The model

Fog is height fog with exponential falloff. Its density (extinction, per metre) at
height y is

    sigma(y) = min(fog_density * exp(-fog_height_falloff * (y - fog_base_height)), fog_max_density)

so `fog_height_falloff 0` is the same everywhere, and larger values hug the ground. A
rough guide: things about 3 / density metres away are almost hidden (0.01: 300 m, 0.1:
30 m). `fog_start` leaves clear air in front of the camera.

Along the way to a surface the fog adds the light it scatters toward the camera, S, and
lets through a share T of the surface's own light: what's seen is S + T L. The light it
scatters is its own colour (`fog_color`, the surroundings' light; thick fog fades to it)
plus, scaled by `fog_albedo`, the sun's and lamps' light through a Henyey-Greenstein
phase function: `fog_anisotropy` above 0 glows looking toward a light, as real haze does.
The sky counts as `fog_sky_distance` away. Blended surfaces and particles are fogged
too: see-through surfaces as S + T L, additive and subtractive ones only dimmed by T,
multiply ones faded toward no change; heat haze isn't fogged.

## Quality

The `fog_quality` setting ([Graphics.md](Graphics.md)) picks how it's drawn:

| Setting | What it draws | Cost |
|---|---|---|
| `off` | no fog | nothing: no pass, no memory |
| `analytic` | the closed form per pixel, lit by its colour and the directional lights, unshadowed | a few instructions a pixel |
| `low`, `medium`, `high` | a lit volume to `fog_volumetric_distance` (64 m by default), the closed form past it | a compute pass and a froxel buffer |

The volume is a grid of froxels (screen tiles by depth slices): 16-pixel tiles and 48
slices at low, 12 and 64 at medium, 8 and 64 at high, coarser if a very large screen
would pass a million cells. Slices are spaced logarithmically from the near plane. One
thread per tile marches its column front to back; each cell takes the fog's density and
light at its centre: its colour, every directional light with the sun's cascaded shadow
(one tap, so shafts of light and shadow show in the air), and the point and spot lights
of its light cluster, unshadowed. Surfaces then read the volume at their depth,
interpolated across tiles and slices, and add the closed form for the rest of the way.

Low through high fall back to analytic for orthographic views. Reflection captures are
taken without fog. Presets: Low uses analytic, Medium low, High medium, Ultra high.

## Seeing it

`player --view fog` and the editor's *Fog transmittance* view show T, white for clear
air. `player --graphics-report` gives the quality drawn, why it differs from the setting
(no fog in the scene, an orthographic view), the froxel grid and its memory.

## Limits

No local density volumes or noise, no history between frames (TAA smooths the volume),
lamps light it unshadowed, and the closed form past the volume is lit by the sun
unshadowed.
