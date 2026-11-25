# Terrain

A level can have heightmap terrain: ground the player walks on, enemies find paths over and
the editor sculpts and paints. A terrain has one material (`swterrain 1`) or up to four
painted layers blended by weights (`swterrain 2`, below).

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

## Painted layers (swterrain 2)

Version 2 paints up to four materials (grass, rock, dirt, sand) instead of one:

```
swterrain 2
samples 513
spacing 1
height_range 0 200
heights "terrain/hills.r16"
layer 0 "materials/grass.swmaterial" 4     # index 0-3, material, metres per texture repeat
layer 1 "materials/rock.swmaterial" 8
layer 3 "materials/sand.swmaterial" 2
weights "terrain/hills.weights.png"
```

- `layer` lines replace `material` and `uv_metres` (version 2 refuses those two). Indices
  are 0 to 3, each at most once, and need not be consecutive; an empty material is the
  default grey surface. At least one layer and `weights` are required.
- The weights are an RGBA8 PNG of `samples` by `samples`: channel *i* (red, green, blue,
  alpha) is layer *i*'s share at that point. Its top row is the row at the lowest Z, like
  the heights. When it's read, each point's shares are normalised to sum to 255: unused
  layers get 0, the rest keep their proportions (the largest remainders round up), and a
  point with no share at all goes to the first used layer.
- A version 1 file still reads, as one layer: its material.
- The weights file sits beside the heights as `name.weights.png`. The asset folder never
  loads a `*.weights.png` as a texture; it's a dependency of its terrain, so it hot
  reloads, cooks and shows in `--asset-dependencies` like the heights.

Drawing: painted chunks draw in the `terrain` shading family. Each pixel reads the weights
(one texel per point, filtered between them), skips layers under 1%, and blends the rest's
base colour, roughness, metallic and normal by weight. Each layer's textures repeat every
its metres over the world's X and Z from the terrain's corner. Each chunk records which
layers have any share above 1% in it, so its pixels never sample the others.

`terrain_quality` (docs/Graphics.md): `high` blends every layer at a pixel with their
normal maps; `low` keeps the two heaviest and leaves out their normal maps. Where at most
two layers mix, low and high are the same picture. The Low and Medium presets use low.
`--view shading` (debug view 14) shows the shares: layer 0 red, 1 green, 2 blue, 3 yellow.

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
submitted, how many draws used a simpler level, and for painted terrains the painted draws,
the layers they sampled and `terrain_quality`. Its settings list has a `terrain_quality` row.

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
- **Paint**: pick a layer, then the Paint brush. Painting raises that layer's share under
  the circle and lowers the others in proportion, so every point keeps summing to 255;
  enough strokes make it 255 and the others 0. **Add layer** (with a material and metres
  per repeat) takes the first free slot, up to four; on a version 1 terrain it first makes
  the terrain version 2, with its material as layer 0 everywhere. **Remove** shares a
  layer's weight among the others in proportion; the last layer stays.
- **Import heights** reads a 16-bit greyscale PNG (8-bit is widened) or a raw `.r16` of the
  terrain's size. The image's top row is the row at the lowest Z.
- **Save terrains** writes every sculpted or painted terrain (weights, heights, then the
  header). Saving the scene does too, and an unsaved stroke marks the scene changed.

Each stroke (mouse down to mouse up), each import and each layer added or removed is one
undo step. Strokes keep their
own history beside the scene snapshots, in one journal, so Ctrl+Z undoes whichever came
last and puts the heights back exactly.

## Command line

```
player --terrain-info terrain/hills.swterrain [--json file]
player --terrain-import heights.png --output Content/terrain/hills.swterrain --spacing 1 --height-range 0 200
```

`--terrain-info` prints the points, spacing, size, lowest and highest heights, the height
range, chunk count, heights file and material, or for a painted terrain its weights file
and each layer with its share of the whole terrain (and writes them as JSON, with a
warning for each material that isn't in Content). `--terrain-import`
makes the header and its `.r16` beside it from a 16-bit PNG or `.r16`, inside the Content
folder that holds `--output`. Neither needs a project.

`player --check` reports: a terrain whose heights or weights file is missing or the wrong
size; a `samples` value terrains don't allow (any parse error, with its line); a material
or a layer's material that isn't loaded; a rotated or scaled terrain; a Terrain collider without a Terrain component,
or on a body that isn't Static.

## Tests

- `terrain` (unit): the format and every error's line, heights between and beyond points,
  chunk counts and levels, skirts, brushes changing only inside their radius, undo bytes,
  reading, writing and importing `.r16`. Layers (W1b): version 2's format and errors, a
  version 1 file as one layer, weights normalising, painting layer 2 to 255 and the others
  to 0 only inside the radius, chunk layer masks, undo and redo of paint and of layers
  added, adding and removing layers, the weights PNG round trip and its errors.
- `physics`: spheres on flat and sloped terrain rest on it (within Jolt's 2 cm penetration
  slop), rays hit it and miss past its edge, a moving Terrain collider is refused.
- `navigation`: a path across a terrain goes round a steep ridge instead of over it, and
  sculpting changes the input hash.
- `graphics_terrain_render` (GPU, `Tests/GraphicsTerrainCheck.cmake`): the fixture
  `terrain-hills` repeats with `--fixed-step`; the same hills under a magenta sky match below
  the horizon (with and without `--lod-colors`), so no crack shows the sky; the report counts
  16 chunks and some simpler levels, and `lod_error_pixels 0` draws none. Painted: the
  fixture `terrain-layers` (four layers, one per quadrant, all four mixed in a disc at the
  centre, seen from above) repeats, shows four distinct colours in four fixed rectangles,
  `terrain_quality low` differs from high only in the disc, and the report counts one
  painted draw sampling four layers.
- `terrain_flow` (`Tests/TerrainFlowCheck.cmake`): Reactor Run's training level with a terrain
  added passes `--check`, lists the terrain and its heights as dependencies and plays through
  the game's whole-run flow; a cut-short heights file fails `--check`.

## Not yet

Holes, terrain streaming beyond 2049 points, rotated or scaled terrains, layers beyond
four, triplanar mapping for cliffs, and height-based blending between layers.
