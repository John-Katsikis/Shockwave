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
