# Shadows

The sun casts cascaded shadows with soft edges, and spot and point lights cast shadows
from a shared atlas. Everything is set from the editor, from plain text in scenes and
projects, or from the player's command line.

## Settings

| Setting | Editor | Text | Player flag |
|---|---|---|---|
| Quality | View menu > Shadows, Preferences > Shadows; Project settings > Player shadows | `shadow_quality high` in the `.swproject` | `--shadows off\|low\|medium\|high\|ultra` |
| Sun shadow distance | Environment > Shadow distance | `shadow_distance 80` | |
| Sun cascades | Environment > Shadow cascades | `shadow_cascades 4` (1 to 4) | |
| Which lights cast | Light > Cast shadows | `cast_shadows true` | |
| Darkness | Light > Shadow strength | `shadow_strength 1` (0 to 1) | |
| Edge softness | Light > Shadow softness | `shadow_softness 0.3` (0 to 1) | |
| Acne vs. gap tuning | Light > Shadow bias | `shadow_bias 1` (0 to 10) | |
| Which meshes cast | Mesh/Model Renderer > Cast shadows | `cast_shadows true` | |

The editor's quality preference applies to the viewport only. The player uses the
project's setting unless `--shadows` overrides it for that run. A scene with no Light
gets a default sun that casts shadows.

## Qualities

| Quality | Cascade size | Light tile | Light atlas | Filter taps |
|---|---|---|---|---|
| Off | | | | |
| Low | 1024 | 256 | 2048 | 4 |
| Medium | 2048 | 512 | 4096 | 8 |
| High (default) | 2048 | 512 | 4096 | 16 |
| Ultra | 4096 | 1024 | 4096 | 32 |

A spot light takes one tile of the light atlas and a point light takes six (one per cube
face). When more shadowed lights are in view than the atlas holds, the ones farthest
from the camera go unshadowed, and Window > Stats says how many.

## How it works

- **Sun.** The view out to the shadow distance is split into up to four cascades
  (practical split), each fitted with a bounding sphere and snapped to whole shadow
  texels so edges don't shimmer as the camera moves. Neighbouring cascades blend over
  the last 15% of each slice, and shadows fade out at the shadow distance.
- **Spot and point lights.** Each gets perspective views into the light atlas, chosen
  by how close the light is to the camera, and only if it touches the camera's view.
- **Soft edges.** Percentage-closer filtering over a rotated Vogel disk, with the
  rotation varied per pixel so few taps still look smooth. Softness sets the filter
  radius; normal-offset and slope-scaled bias keep surfaces free of acne.
- **Cost.** Shadow casters are frustum-culled per view, blended surfaces never cast, and
  the render graph times the `shadows (sun)` and `shadows (lights)` passes on the GPU
  (Window > Stats, or `player --benchmark`). Everything uses reverse-Z like the scene.

## Checking shadows without the editor

```sh
./build/bin/player --scene Games/Sample/Scenes/shadows.swscene --screenshot shadows.png
./build/bin/player --scene Games/Sample/Scenes/shadows.swscene --screenshot cascades.png --view cascades
./build/bin/player --scene Games/Sample/Scenes/shadows.swscene --benchmark 5 --shadows ultra
```

`--view cascades` (View > Shading > Shadow cascades in the editor) tints the scene by
which sun cascade each pixel uses. `Games/Sample/Scenes/shadows.swscene` is a showcase
with a low sun, pillars out to 70 m, a spotlight and a point light.
