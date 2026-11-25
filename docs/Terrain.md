# Terrain

A level can have heightmap terrain: ground the player walks on, enemies find paths over and
the editor sculpts. One material covers a terrain (painted layers are the next step, W1b).

A terrain is two files in Content and one component:

- `terrain/hills.swterrain`: a small text header (below).
- `terrain/hills.r16`: the heights, `samples * samples` little-endian 16-bit values.
- A `Terrain` component on an entity, naming the `.swterrain`. With a Static `RigidBody`
  and a `Collider` of shape `Terrain` it also collides and is walkable for navigation.

The terrain's corner with the lowest X and Z sits at the entity's world position. A terrain
is never rotated or scaled: only the position counts, and `player --check` reports a
Terrain entity whose Transform has a rotation or a scale other than 1.

## The .swterrain format

```
swterrain 1
samples 513                         # points along each side: 65, 129, 257, 513, 1025 or 2049
spacing 1                           # metres between points (0.1 to 100)
height_range 0 200                  # metres: height value 0 is the low end, 65535 the high end
heights "terrain/hills.r16"         # Content path of the heights
material "materials/grass.swmaterial"   # optional; the default grey surface without it
uv_metres 4                         # metres per repeat of the material's textures (0.01 to 10000)
```

The first line is `swterrain 1`; `#` starts a comment. `samples`, `spacing`, `height_range`
and `heights` are required. An unknown key, a key given twice or a value out of range is
an error naming its line, as everywhere else.

Height in metres = `low + value / 65535 * (high - low)`. The heights file holds rows along
+X, the first row at the lowest Z: the value for point (x, z) is at index
`z * samples + x`. Its size must be exactly `samples * samples * 2` bytes. A terrain is
`(samples - 1) * spacing` metres along each side: 513 points 1 m apart is 512 m.

Saving writes the heights first and then the header, each to a temporary name that is then
renamed over the old file, so a game or the editor never reads half a file.

## Drawing

Terrains are split into render chunks of 64 by 64 quads (a 513-point terrain has 64
chunks). Each chunk is one mesh with two simpler levels: every 2nd point and every 4th
point. The renderer picks a chunk's level the way it picks a model's (`lod_error_pixels`):
at 1080p with a 60 degree field of view and `lod_error_pixels 1`, a chunk draws every point
up to about 150 m away, every 2nd point to about 400 m, and every 4th beyond. Distances are
measured to the nearest point of the chunk's bounds, and scale with the picture's height:
at 720p the switches come at about two thirds of those distances. `lod_error_pixels 0`
draws every chunk at full detail; `--lod-colors` (or Show levels in the Optimisation
window) tints each level its own colour.

Every level of every chunk has a skirt: a strip of triangles around its edge dropping
`4 * spacing` metres. Where a chunk at full detail meets one at half detail, the edges
don't quite line up; the skirts fill those cracks, so the sky never shows through.

Texture coordinates are the terrain-space position over `uv_metres`. Normals come from
central differences of the heights; tangents point along +X.

The chunk meshes belong to the asset registry (`Assets::terrain`), are made when the asset
loads and destroyed when it unloads. Sculpting rebuilds only the chunks a stroke touches,
plus the one-point border their normals read.

`player --graphics-report` has a `terrain` section: how many terrains, the chunks they
submitted and how many draws used a simpler level.

## Collision

A `Collider` with shape `Terrain` becomes a Jolt height field built from the entity's
`Terrain` asset. It must be on a Static body (or no body, which is static); anything else is
refused with "Terrain colliders must be Static". Jolt wants the sample count to be a
multiple of its block size, so the heights are padded with "no collision" samples to the
next multiple of 4: nothing collides past the terrain's edge. Changing the heights (a
reload, a sculpt) rebuilds the shape.

## Navigation

A Terrain collider (or, with `geometry render_meshes`, a visible Terrain) feeds the
navigation mesh two triangles per quad at every 2nd point. Steep slopes are left out the way
any steep triangle is (`NavMeshSettings` `max_slope`). Sculpting changes the scene's
navigation input hash, so `player --check` reports a saved navigation mesh that's out of
date.

## In the editor

**Window > Terrain**:

- **Create** makes a flat `.swterrain` and its `.r16` in Content (choose the name, points a
  side, spacing and height range) and an entity with a Terrain, a Static RigidBody and a
  Terrain Collider, centred in front of the camera.
- **Sculpt** brushes: Raise, Lower, Smooth and Flatten (to the height under the first
  click). Radius 1 to 200 m, strength 0 to 1, falloff smooth or linear. With a brush picked,
  the circle follows the ground under the mouse; hold the left button to sculpt. Brushes
  work on the selected entity's terrain, or the scene's first.
- **Import heights** reads a 16-bit greyscale PNG (8-bit is widened) or a raw `.r16` of the
  terrain's size. The image's top row is the row at the lowest Z.
- **Save terrains** writes every sculpted terrain. Saving the scene does too, and an
  unsaved stroke marks the scene changed.

Each stroke (mouse down to mouse up) and each import is one undo step. Strokes keep their
own history beside the scene snapshots, in one journal, so Ctrl+Z undoes whichever came
last and puts the heights back exactly.

## Command line

```
player --terrain-info terrain/hills.swterrain [--json file]
player --terrain-import heights.png --output Content/terrain/hills.swterrain --spacing 1 --height-range 0 200
```

`--terrain-info` prints the points, spacing, size, lowest and highest heights, the height
range, chunk count, heights file and material (and writes them as JSON). `--terrain-import`
makes the header and its `.r16` beside it from a 16-bit PNG or `.r16`, inside the Content
folder that holds `--output`. Neither needs a project.

`player --check` reports: a terrain whose heights file is missing or the wrong size; a
`samples` value terrains don't allow (any parse error, with its line); a material that
isn't loaded; a rotated or scaled terrain; a Terrain collider without a Terrain component,
or on a body that isn't Static.

## Tests

- `terrain` (unit): the format and every error's line, heights between and beyond points,
  chunk counts and levels, skirts, brushes changing only inside their radius, undo bytes,
  reading, writing and importing `.r16`.
- `physics`: spheres on flat and sloped terrain rest on it (within Jolt's 2 cm penetration
  slop), rays hit it and miss past its edge, a moving Terrain collider is refused.
- `navigation`: a path across a terrain goes round a steep ridge instead of over it, and
  sculpting changes the input hash.
- `graphics_terrain_render` (GPU, `Tests/GraphicsTerrainCheck.cmake`): the fixture
  `terrain-hills` repeats with `--fixed-step`; the same hills under a magenta sky match below
  the horizon (with and without `--lod-colors`), so no crack shows the sky; the report counts
  16 chunks and some simpler levels, and `lod_error_pixels 0` draws none.
- `terrain_flow` (`Tests/TerrainFlowCheck.cmake`): Reactor Run's training level with a terrain
  added passes `--check`, lists the terrain and its heights as dependencies and plays through
  the game's whole-run flow; a cut-short heights file fails `--check`.

## Not yet

Painted layers (W1b), holes, terrain streaming beyond 2049 points, and rotated or scaled
terrains.
