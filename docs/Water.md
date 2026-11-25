# Water

Lakes, ponds and slow rivers: a flat surface that shows how deep it is, ripples, reflects the
sky and foams along the shore. Water is a material family, so any flat mesh can be water:

```
swmaterial 1
family water
water_shallow_color 0.1 0.4 0.4      # where the bottom is close (linear RGB)
water_deep_color 0.02 0.08 0.12      # where it's deep
water_depth_fade 4                   # metres of water from shallow to deep (0.1 to 100)
water_clarity 6                      # metres seen through before the bottom is gone (0.1 to 100)
texture water_normal "textures/ripples.png"   # ripple normal map; none: a flat surface
water_ripple_scale 3                 # metres per repeat of the ripple map (0.1 to 100)
water_ripple_speed 0.3               # metres a second the ripples drift (0 to 10)
water_foam_distance 0.4              # metres of depth where the shore's foam fades out (0 to 5)
```

The values above are the defaults. They're read only with `family water`; on another family
they do nothing and `player --check` warns, the way skin values on glass do. Water draws its
own see-through, so it ignores `alpha_mode blend`. It casts no shadow and takes no decals.

**Built in.** The engine has a water material called `water` (a ripple map called
`water-ripples` and the defaults above), so a scene can write `material water` without a
file. The editor's Hierarchy **Create > Water plane** adds a 10 m plane with it.

## How it draws

Water is drawn in the see-through pass glass uses (docs/Shading.md, Transmission): after the
opaque scene, over a copy of it, with the scene's depth bound read-only so the water can
read what's behind it. Per pixel:

- **Depth.** The distance along the view ray from the surface to the opaque scene behind
  it, from the scene's depth. Over the sky it's endless.
- **Ripples.** The ripple map is tiled over the world's X and Z (its UVs aren't used), every
  `water_ripple_scale` metres, and read twice, drifting in the directions (1, 0.3) and
  (-0.4, 1) at `water_ripple_speed`; the two are added and normalised. The time is the
  game's frame clock (`Renderer::setWindTime`, the one foliage sways with), so a
  `--fixed-step` run ripples the same every time; the editor moves them only while playing.
- **Colour.** The body colour is `mix(shallow, deep, depth / depth_fade)`. The bottom shows
  through in proportion `1 - depth / clarity`, read from the copy at the pixel moved by the
  ripples' tilt (`normal.xz * 0.02` of the screen). Where something nearer than the water is
  at the moved spot (a post standing in it), it reads straight down instead, so nothing above
  the surface smears into it.
- **Reflection.** A smooth dielectric, F0 0.02 and roughness 0.05, lit and reflecting like
  any other surface: the sun and lights, the environment and reflection probes. Fresnel
  takes its share from what the water lets through.
- **Foam.** White, `1 - depth / foam_distance` thick, broken up by the ripple map's blue
  channel.

## Settings

- `water_quality` `low` or `high` (Low and Medium presets: low). Low reads the bottom
  straight down (no refraction), leaves out the foam and reads the ripple map once. Neither
  has a pass or memory of its own.
- `transmission_quality off` turns off the copy water reads, and water then draws as a
  blended stand-in: the halfway colour between shallow and deep, mostly opaque, with its
  ripples and reflections but no depth, refraction or foam. `player --check` warns about
  a scene that has water while transmission is off.

The graphics report counts water draws (`shading.water_draws`, and
`simplified_water_draws` for the stand-in); `--material-info` gives a water material's
`draws_as` at each quality.

## Not done

Waves that move the mesh, buoyancy, swimming, an underwater view and waterfalls. Water
isn't in the screen-space reflections (V1), which are traced from opaque surfaces: it
reflects the probes and the environment. Water behind glass, or glass behind water, shows
the opaque scene only, as for glass behind glass.
