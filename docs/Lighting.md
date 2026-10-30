# Lighting from the surroundings

Besides its lights, every scene is lit by its surroundings: the light that arrives from
all directions at once. It fills in shadows, and it's what shiny surfaces reflect, so
metals, which have no diffuse color of their own, look like metal rather than black.
This is image-based lighting (IBL). The Environment component sets it up.

## Without an environment map

The surroundings are the Environment's `sky_color` above the horizon and `ground_color`
below it, both times `ambient_intensity`. Matte surfaces get the same blend of the two by
the way they face as they always have. Shiny surfaces now reflect the sky and the ground,
and rough ones a blurred version of them.

## With an environment map

An environment map is a panorama of the surroundings: an equirectangular image, twice as
wide as it is tall, covering every direction. Use a `.hdr` (Radiance) file. Its pixels
can be far brighter than white, so the sun in it stays a bright spot that lights and
reflects properly. A PNG or JPEG works too, but its brightest light is clipped. Free HDR
panoramas are easy to find (Poly Haven has many); 2048 x 1024 is plenty.

1. Put the `.hdr` file in your project's `Content/` folder (say `Content/Sky/park.hdr`).
2. Select the Environment entity and drop the file on Environment map in the Inspector,
   or pick it from the list.

| Setting | Text | Range | What it does |
|---|---|---|---|
| Environment map | `environment_map Sky/park.hdr` | a texture in Content | The panorama; `white` for none |
| Environment intensity | `environment_intensity 1` | 0 to 1000 | Multiplies the map's light |
| Environment rotation | `environment_rotation 0` | -360 to 360 | Turns the map about the vertical axis, in degrees |
| Show environment | `show_environment true` | | Draws the map behind everything instead of `sky_color` |

The middle of the image faces -Z (forward) at rotation 0, and its top row is straight up.
The map replaces `sky_color`, `ground_color` and `ambient_intensity` for lighting. Lights
still add their own light and shadows on top, so a map with a bright sun usually goes
with a directional Light pointing the same way.

In a scene file:

```
entity "Environment"
  Environment
    environment_map Sky/park.hdr
    environment_intensity 1.5
    environment_rotation 120
end
```

`player --check` reports an environment map that isn't twice as wide as it is tall.

## Bloom

Very bright light spreads into its surroundings, the way it does through a camera lens:
glowing emissive materials, the sun in a reflection, a bright sky behind a window. Set it
on the Environment:

| Setting | Text | Range | What it does |
|---|---|---|---|
| Bloom intensity | `bloom_intensity 0.04` | 0 to 1 | How much of the blurred image is mixed into the sharp one; 0 turns bloom off |

There's no brightness threshold: everything blurs a little, and the mix means only light
far brighter than its surroundings shows a visible glow. 0.02 to 0.08 looks natural;
higher values look dreamy. To make something glow, give it an emissive color with an
`emissive_intensity` well above 1. Bloom costs about 0.05 ms a frame at 1280 x 720 on an
RTX 3080 (0.13 ms at 2560 x 1440), and it's skipped in the debug views.

It's the Call of Duty: Advanced Warfare method: the HDR image is filtered down a chain of
half-size levels (`bloom.comp`, `RendererBloom.cpp`) and back up, and the tonemap pass mixes
the result in before exposure.

## Baked lighting

The surroundings alone light a room's inside as brightly as an open field: they don't
know about walls. A bake works out, ahead of time, how much of the surroundings each
surface really sees, and how the light of the sun, lamps and glowing surfaces bounces
off walls and floors onto everything else: the red wall that reddens the ceiling beside
it, the dim corridor lit only by the doorway at its end. It replaces the light from the
surroundings, and only that: lights still light directly, in real time, with their
shadows, and shiny reflections are unchanged.

Two kinds of results, made by one bake:

- **Lightmaps**, for static surfaces: a texture per scene (or a few) holding the light
  that arrives at every point of every wall and floor marked to receive one.
- **Probe volumes**, for everything else: a 3D grid of probes, each holding the light
  arriving from every direction at its point, which moving things (players, props, the
  ball in the fixture) sample at their centre as they move.

### Setting a scene up

1. Give every wall, floor and other non-moving mesh a **BakeGeometry** with
   `mobility static`. Static objects block and bounce light in the bake
   (`cast_indirect`, on by default). Those that should get a lightmap also need
   `receive_lightmap 1`; small or distant detail can stay an occluder only.
2. Give the lights whose bounce matters a **BakeLight** (`include_indirect 1`). Lights
   without one aren't bounced; every light still lights directly.
3. Add a **DiffuseProbeVolume** (Lighting category) wherever moving things go: a box
   (`half_extents`, metres) on its entity's Transform, with probes every `spacing`
   metres. Overlapping volumes: the higher `priority` wins, then the smaller.
   `blend_distance` fades a volume into the surroundings at its edges.
4. Bake: **Window > Lighting > Bake** in the editor, or
   `player --bake-lighting --scene Scenes/Arena.swscene --quality preview|final [--seed N]`.

The editor's first bake adds a **BakedLighting** component pointing at the result
(`asset "Lighting/<scene>.swlighting"`, on the Environment entity), which is what makes
the scene draw it. `enabled 0` keeps the bake but draws without it.

```
entity "Back wall"
  Transform
    position 0 1.5 -8.1
    scale 8.4 3.2 0.2
  MeshRenderer
    mesh cube
  BakeGeometry
    binding_id "3312dbcf894d202f2d72858a487478fd"
    mobility static
    receive_lightmap 1
    texels_per_meter 8
end
```

| Component | Field | Default | What it does |
|---|---|---|---|
| BakeGeometry | `mobility` | `dynamic` | `static` takes part in the bake; `dynamic` is lit by probes |
| | `cast_indirect` | 1 | Blocks and bounces light in the bake |
| | `receive_lightmap` | 0 | Gets a lightmap (static only) |
| | `texels_per_meter` | 16 | Lightmap detail; 4 to 8 suits large walls, 16 to 32 close-up props |
| | `binding_id` | made for you | The object's identity in the bake (below) |
| BakeLight | `include_indirect` | 1 | The light's bounce is baked |
| DiffuseProbeVolume | `half_extents`, `spacing`, `blend_distance`, `priority`, `enabled`, `binding_id` | 4 2 4, 1, 0.5, 0, 1 | See step 3 |
| BakedLighting | `asset`, `enabled` | "", 1 | The bake this scene draws |

### Identity: binding ids

A bake finds its objects again by `binding_id`, a 128-bit id written once when the
component is added in the editor (or by `player --lighting-assign-ids`). Saving,
reloading, renaming and reordering the scene keep it; duplicating an object gives the
copy a fresh one. Prefab copies have a `bake_id` on their Prefab component, and the
objects inside get an id derived from it and their own, so two copies of one prefab are
two different things to the bake. `player --check` reports missing and duplicate ids and
never fixes them by itself; `player --lighting-assign-ids [--scene file] [--output new]`
fills them in, and Window > Lighting has *Assign missing ids*.

### Out of date

Each object is baked with a hash of what its light depends on: its mesh, placement,
material and texels_per_meter. Move or change one receiver and only it stops using its
lightmap (it draws with the surroundings, as if unbaked); everything else keeps its
light, and the bake reads *stale* until you rebake. Changing a light with BakeLight, the
Environment, or the bake settings changes every texel, so the whole bake is set aside
until it's rebaked. Window > Lighting and `player --lighting-info [--scene file] [--json
out]` say which and why; `player --check` warns about a bake that's out of date or not
made yet, and fails on a damaged one.

### Quality, speed and size

`preview` traces 64 paths a texel, `final` 512, each bouncing up to twice. On 4 CPU
threads the fixture room (9 receivers, 324 probes) bakes in about 0.2 s preview and
1.2 s final; preview is blotchy, final smooth. The same scene, settings and seed always
bake the same bytes, on any number of threads. Lightmaps are linear half-float RGBA:
8 bytes a texel, in square power-of-two atlases no bigger than they need to be (the room
is one 512 x 512 atlas, 2 MiB). Probes cost about 600 bytes each. A bake over the
memory budget (256 MiB of lightmaps) or a volume over 32768 probes is refused before
anything is traced.

The files go in `Content/Lighting/`: `<scene>.swlighting` (a readable manifest) and a
`<scene>/` folder with `Lightmap_NNN.ktx2`, `Receivers.swbake` and `Probes.swprobe`.
They're written to a hidden folder first and only swapped in once complete, so a failed
or cancelled bake leaves the previous one in use. docs/LightingSchema.md describes every
file; `player --lighting-export` and `--lighting-import` turn the payloads into JSON
and back.

### Turning it off, and looking at it

The `baked_lighting` graphics setting (on by default) switches all of it off at once:
off, every surface is lit by the surroundings exactly as if the scene had never been
baked, pixel for pixel. `player --view indirect` and the editor's *Indirect diffuse*
shading view show the light from the surroundings alone (baked or not), as the lit
image receives it.

### Limits

- Lightmaps store one colour per texel, not a direction, so normal maps don't pick out
  bounced light's direction (they still shade direct light and reflections).
- A moving object samples the probes once, at its centre: a long object spanning a
  bright and a dark area gets one blend of the two.
- The bake reads base colour textures (sampled with wrap, at full size) and emissive
  colours, but not emissive textures, so a glowing texture lights nothing in the bake.
  Blended (see-through) materials are left out, with a warning.
- Every receiver gets its own space in the lightmap, even copies of the same mesh.
- Baked lighting replaces only diffuse light from the surroundings; reflections still
  come from the environment (local reflection probes are G07).

## How it works

The renderer draws the surroundings into a cube map, then filters it into two more: an
irradiance cube, the light a matte surface facing each way receives, and a prefiltered
specular cube whose mip levels hold blurrier reflections for rougher surfaces. A BRDF
table made at startup completes the split-sum approximation, with multiple-scattering
compensation so rough metals keep their brightness. The cubes are only made again when
the Environment's colors, map, intensity or rotation change, or the map file is
reloaded. The occlusion map of a material darkens this light, as it did the ambient
light before.

`.hdr` files are imported at full range as half floats (RGBA16F) and aren't compressed.
The code is in `Source/Plugins/RendererVulkan/Private/RendererEnvironment.cpp`, with the
`environment_sky.comp`, `environment_filter.comp` and `sky` shaders.

A bake (`Source/Runtime/Lighting`) unwraps each receiver with xatlas into charts at its
texels_per_meter, packs them into atlases, and traces each covered texel's hemisphere
with cosine-weighted paths through a BVH of the static surfaces: a path that leaves the
scene brings back the environment, one that hits a surface brings its emission plus its
diffuse reflectance (albedo x (1 - metallic), at most 0.96) times the baked lights
reaching it and one continued path. Texels whose rays mostly start inside other geometry
are dropped and filled from their neighbours, then gutters are filled ring by ring so
filtering never reads black. Probes project their incoming light onto 9 spherical
harmonics per channel and keep an 8 x 8 octahedral map of distances, so a probe on the
far side of a wall isn't used (Chebyshev visibility, as in DDGI). `mesh.frag` reads the
lightmap or the object's probe sample in place of the irradiance cube, in the same B = E /
pi units, so everything else about shading is unchanged.
