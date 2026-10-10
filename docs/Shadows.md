# Shadows

The sun, spot lights and point lights all cast soft-edged shadows. You can tune them in
the editor, in the plain-text scene and project files, or with the player's command
line. A scene with no Light gets a default sun that casts shadows, so you see shadows
right away.

## Change shadow quality

| Where | How |
|---|---|
| Editor viewport | View menu > Shadows, or Preferences > Shadows |
| Your game, in the player | Project settings > Player shadows, saved as `shadow_quality high` in the `.swproject` |
| One player run | `--shadows off\|low\|medium\|high\|ultra` |

The editor's quality preference only affects the editor viewport. The player uses the
project's setting, unless `--shadows` overrides it for that run.

```sh
./build/bin/player --scene Games/Sample/Scenes/shadows.swscene --shadows ultra
```

This opens the shadow showcase scene with the highest quality.

## Tune a light's shadow

Select a Light and change these in the Inspector. The text name is what the `.swscene`
file uses.

| Setting | Text | Range | What it does |
|---|---|---|---|
| Cast shadows | `cast_shadows true` | | Whether this light casts shadows |
| Shadow strength | `shadow_strength 1` | 0 to 1 | How dark the shadow is |
| Shadow softness | `shadow_softness 0.3` | 0 to 1 | How blurry the edge is |
| Shadow bias | `shadow_bias 1` | 0 to 10 | Fixes artifacts, see below |

Shadow bias trades two problems against each other. Too little bias gives shadow acne:
speckled stripes of shadow on lit surfaces. Too much gives a gap where the shadow
detaches from the object's base.

## Tune the sun's shadows

These are on the Environment component:

| Setting | Text | Range | What it does |
|---|---|---|---|
| Shadow distance | `shadow_distance 80` | 1 to 10000 | How far from the camera the sun casts shadows |
| Shadow cascades | `shadow_cascades 4` | 1 to 4 | How many cascades the sun uses (see below) |

Cascades split the view into slices by distance from the camera. Each slice gets its own
shadow map, so nearby shadows stay sharp while far ones cover more ground.

## Stop an object casting shadows

Turn off Cast shadows on its Mesh Renderer or Model Renderer, `cast_shadows false` in
text. Transparent (blended) surfaces never cast shadows.

## Quality levels

| Quality | Cascade size | Light tile | Light atlas | Filter taps |
|---|---|---|---|---|
| Off | | | | |
| Low | 1024 | 256 | 2048 | 4 |
| Medium | 2048 | 512 | 4096 | 8 |
| High (default) | 2048 | 512 | 4096 | 16 |
| Ultra | 4096 | 1024 | 4096 | 32 |

Sizes are in pixels. The light atlas is one big shadow texture shared by all spot and
point lights, cut into tiles. Filter taps are how many samples soften each shadow edge.

A spot light uses one tile of the atlas. A point light uses six, one for each face of a
cube around it. If more shadowed lights are in view than the atlas can hold, the ones
farthest from the camera lose their shadows. **Window > Stats** shows how many.

## Check shadows without the editor

```sh
./build/bin/player --scene Games/Sample/Scenes/shadows.swscene --screenshot shadows.png
./build/bin/player --scene Games/Sample/Scenes/shadows.swscene --screenshot cascades.png --view cascades
./build/bin/player --scene Games/Sample/Scenes/shadows.swscene --benchmark 5 --shadows ultra
```

The first command renders the scene and saves `shadows.png`. The second saves
`cascades.png` with the cascade view. The third measures performance for 5 seconds at
ultra quality.

`--view cascades` tints each pixel by which sun cascade it uses. In the editor, the same
view is View > Shading > Shadow cascades.

`Games/Sample/Scenes/shadows.swscene` is a showcase with a low sun, pillars out to 70 m,
a spot light and a point light.

## Measure the cost

The render graph times the `shadows (sun)` and `shadows (lights)` passes on the GPU. See
them in **Window > Stats** or with `player --benchmark`. See
[Frame pacing](FramePacing.md) for more on both.

## How it works

- **Sun.** The view out to the shadow distance is split into up to four cascades, using
  the "practical split" scheme. Each cascade is fitted with a bounding sphere and snapped
  to whole shadow texels (shadow texture pixels), so edges don't shimmer as the camera
  moves. Neighboring cascades blend over the last 15% of each slice. Shadows fade out at
  the shadow distance.
- **Spot and point lights.** Each one gets perspective views into the light atlas.
  Lights closest to the camera are picked first, and a light only gets shadows if it
  touches the camera's view.
- **Soft edges.** Shadows use percentage-closer filtering: several samples in a rotated
  Vogel disk pattern. The rotation changes per pixel, so few samples still look smooth.
  Softness sets the filter radius. Normal-offset and slope-scaled bias keep surfaces free
  of acne.
- **Cost.** Shadow casters are culled per view, so only objects inside each shadow
  view are drawn. Blended surfaces never cast. Shadows use reverse-Z depth, like the
  rest of the scene.
