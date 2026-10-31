# Colour grading, exposure and fog

A scene's look past lighting — how bright it's exposed, its white balance, a creative
colour grade, bloom strength and fog — comes from **postprocess profiles** placed with
**PostProcessVolume** components. Walking from a sunny street into a foggy, blue-graded
cave is two volumes with a blend distance between them. Scenes without volumes look
exactly as they always have.

- The profile format: [PostProcessSchema.md](PostProcessSchema.md) (`player --postprocess-schema`).
- Fog in detail: [Fog.md](Fog.md).
- The settings that switch each part off: `color_grading`, `auto_exposure` and
  `fog_quality` in [Graphics.md](Graphics.md) and [GraphicsSchema.md](GraphicsSchema.md).
- The design and the maths: [Graphics/04-ColorAndAtmosphere.md](Graphics/04-ColorAndAtmosphere.md)
  and [Graphics/07-MathematicalContracts.md](Graphics/07-MathematicalContracts.md).

## Profiles

A `.swpostprocess` file anywhere in Content gives any of the keys it wants; the rest come
from its parent, then from the scene's Environment (`exposure`, `bloom_intensity`), then
from defaults that change nothing:

```
swpostprocess 1
parent "postprocess/outdoor.swpostprocess"   # optional
exposure 0.5
white_point 0.4476 0.4074                    # neutralize incandescent light
lut "postprocess/luts/cave.cube"
fog_density 0.05
fog_height_falloff 0.2
fog_color 0.3 0.4 0.5
```

Files are strict: an unknown key, a value out of range, a key given twice or a parent
chain that loops (or is deeper than 8) is an error with its line, reported by
`player --check`, the editor's Inspector and the log. Profiles and LUTs reload by
themselves about half a second after their files change, in the editor and the player.

`player --postprocess-info file.swpostprocess [--json out.json]` prints a profile with
its parents applied and checks its LUT.

## Volumes

`PostProcessVolume` (Create > Postprocess volume in the editor):

| Field | Meaning |
|---|---|
| `profile` | the `.swpostprocess` in Content |
| `enabled` | off applies nothing |
| `unbounded` | applies everywhere (no Transform needed): a scene's base look |
| `half_extents` | the box, in metres before the Transform's scale |
| `priority` | higher blends over lower |
| `blend_distance` | metres inside the box's faces over which it fades in (0: a hard edge) |
| `weight` | 0..1, how much it applies at most |

Each frame the volumes are evaluated at the camera. A volume's share is its weight times
a smooth fade over `blend_distance` inside its nearest face. Volumes blend in priority
order, lowest first, each over what's below it; volumes of equal priority share by
weight. Numbers (exposure, fog density, colours) blend smoothly; choices (metering mode,
LUT encoding) come from whichever volume gives most of them, once volumes giving them
cover at least half. LUTs blend as layers: two at once, and a third overlapping is
approximated by the largest two (`player --graphics-report` says so). The editor's
Inspector shows how much a volume applies at the viewport camera, and the viewport draws
a selected volume's box with its fully-blended inner box.

## Exposure

`exposure` is manual exposure in stops (+1 is twice as bright). With `auto_exposure true`
the renderer meters each frame instead: a 128-bin histogram of log luminance (every other
pixel, `auto_metering average` or `center`), ignoring the darkest `auto_low_percentile`
and brightest past `auto_high_percentile`, picks the exposure that makes the rest average
`auto_target_gray`, plus `auto_compensation`, clamped to `auto_min_ev`..`auto_max_ev`.
It adapts toward that with time constants `auto_brighten_time` (into the dark) and
`auto_darken_time` (into the light); a camera cut (`Renderer::cutCamera`, with
`auto_reset_on_cut`) or a new view jumps straight there. A volume blending auto exposure
in part blends between the manual and metered exposure.

Metering runs on the GPU; reflection captures always use manual exposure. With the
`auto_exposure` setting false, profiles' auto exposure is ignored: the picture is exactly
the manual one, with no metering pass and no memory for it.

## White balance and LUTs

`white_point` is the CIE xy chromaticity of the light to neutralize: a surface lit by it
comes out neutral (Bradford adaptation to D65, the display's white, applied before
exposure). D65 (`0.3127 0.329`) changes nothing. Shifts are limited to 4x in any cone
response.

`lut` names a `.cube` 3D LUT (`LUT_3D_SIZE` 17, 33 or 65; `DOMAIN_MIN`/`DOMAIN_MAX`
allowed; 1D tables refused) applied after tone mapping, in sRGB-encoded values
(`lut_encoding srgb`, what most grading tools export) or display-linear ones
(`linear`), mixed by `lut_intensity`. To make one: `player --lut-export-neutral 33
neutral.cube`, grade a screenshot in your tool of choice with that LUT applied, and
export the result as a `.cube`.

With the `color_grading` setting false, white balance and LUTs are skipped: the picture
is exactly as it is without them.

## Debugging

- `player --view fog` and the editor's *Fog transmittance* shading view show how much of
  each surface the fog lets through (white clear, black hidden).
- `player --graphics-report --json` has a `postprocess` block (exposure used, metered
  target, LUTs, white balance, fog quality and its froxel grid) and the
  `color_grading`, `auto_exposure` and `fog_quality` effective-settings rows; froxels and
  exposure buffers show as *fog* and *exposure* memory.
- Debug shading views show raw values: no grading and no fog except in the fog view.

## Limits

- No local fog density volumes, and the froxel volume keeps no history between frames
  (TAA smooths it). Point and spot lights light the fog unshadowed; the sun's shadow in
  the fog is one tap a cell.
- Auto exposure meters the scene before bloom, so bloom isn't counted.
- Only the camera position decides volumes: split-screen views share one result.
