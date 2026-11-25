# Foliage

A level can be covered in tens of thousands of small meshes: grass, bushes, rocks, small
trees. Drawing each as its own entity would be far too slow, so foliage is one asset holding
every instance and one component that places it.

Foliage is two files in Content and one component:

- `foliage/meadow.swfoliage`: a small text header naming the types (below).
- `foliage/meadow.instances`: the instances, 24 bytes each.
- A `Foliage` component on an entity (category Rendering), naming the `.swfoliage`. The
  instances' positions are relative to the entity's world transform.

Foliage is painted in the editor's **Window > Foliage** (below), or written by tools
(`writeFoliage` in `Assets/FoliageData.h`) and scripts, such as the fixture generator
`Tests/fixtures/graphics/Tools/foliage.py`.

## The .swfoliage format

```
swfoliage 1
instances "foliage/meadow.instances"     # Content path of the instances
type "models/grass.glb"                  # type 0; the indented keys under it are its own
  distance 60                            # metres: drawn up to here (1 to 5000, default 80)
  fade 15                                # metres before distance where it thins out (default 10)
  shadows false                          # casts shadows (default true)
  wind 0.15                              # metres of sway per metre of height (0 to 1, default 0)
type "models/rock.glb"                   # type 1, with the defaults
```

Version 2, which the editor writes, adds three keys per type for painting:

```
swfoliage 2
...
type "models/grass.glb"
  ...
  max_slope 45                           # degrees: Paint skips steeper ground (0 to 90)
  scale_min 0.8                          # Paint picks each new instance's scale between
  scale_max 1.2                          # these (0.01 to 100)
```

The first line is `swfoliage 1` or `swfoliage 2`; `#` starts a comment. The version 2 keys
in a version 1 file are an error. `instances` is required, and there
can be up to 256 types. An unknown key, a key given twice for one type, a value out of range
or a fade longer than its distance is an error naming its line.

Each instance is 24 bytes, little-endian: `uint16 type`, `uint16 unused (0)`, `float x, y,
z`, `float yaw` (radians about +Y), `float scale`. The file's size must be a multiple of 24
and every type must be one the header has. Saving writes the instances first and then the
header, each to a temporary name renamed over the old file.

Each instance gets a fixed number from 0 to 1 from its index in the file (a hash, the same
every run). Density and fade use it, so the same instances are drawn every run, and
re-ordering the file changes which ones.

## How it draws

On load the instances are sorted by type, then into 16 m cells. Each frame, for each type:

- Cells farther than the type's `distance` (times `foliage_distance`) are skipped, and so
  are cells whose box (grown by the model's size) is outside the view.
- Within `fade` metres of the distance, instances thin out instead of popping: at a cell's
  distance a share of them is dropped, rising from none to all, and an instance survives if
  its number is above that share. Farther cells keep a subset of what nearer ones keep.
- An instance is drawn only if its number is below `foliage_density`.
- Everything kept is submitted with one `Renderer::drawMeshInstances` call per model part,
  for the cells nearer than half the distance, and one more for those beyond it, which use
  the model's simplest level of detail.

The renderer works out the material once per call and gives every instance its own entry in
the frame's draw table, so foliage batches, culls, shadows and moves under TAA like any other
draw. Each instance keeps its own key, so temporal effects follow it.

Types with `shadows false` don't cast shadows. Foliage doesn't take impact decals.

### Wind

With `wind` above 0, the vertex shader sways each vertex along a fixed direction (1, 0, 0.3)
by `wind * height * sin(time * 1.7 + x * 0.35 + z * 0.27)`, where height is the vertex's
height in the model (so roots stay still) and x, z its place in the world. The shadow and
depth passes sway the same way, and motion vectors use the previous frame's time. The time
is the game's frame clock (`Renderer::setWindTime`), so a `--fixed-step` run sways the same
every time; the editor sways foliage only while playing.

## Painting

**Window > Foliage** works on the selected entity's Foliage, else the scene's first.

- **Create**: a name (a Content path) and, optionally, the first type's model. It writes an
  empty `.swfoliage` and `.instances` and adds an entity with a Foliage component.
- **Types**: pick the type the brushes use; under each, its distance, fade, wind, max slope,
  scale range and shadows, shown straight away and undone as one step per edit. **Add type**
  takes a model's Content path. Types can't be removed yet.
- **Paint** scatters the chosen type in the circle under the mouse, up to **Density**
  instances per square metre (0.01 to 20): it counts what's already there and adds only
  what's missing, so going over a spot again doesn't pile up. Each new instance gets a point
  spread evenly over the circle from a seeded sequence, drops straight down onto whatever
  collides below it (terrain or any collider), and is skipped where the ground is steeper
  than the type's max slope; its yaw is random and its scale between scale_min and
  scale_max.
- **Erase** removes the chosen type's instances in the circle.

Each stroke is one undo step, in the same journal as scene edits and terrain strokes, so
Ctrl+Z undoes whichever came last. Undo matches instances by value. **Save foliage** (and
Save scene) writes every edited foliage.

When terrain is sculpted under foliage, the instances inside the changed area that stood
on the old ground (within 5 cm) move onto the new height. Instances above the ground, such
as ones painted on a rock, stay where they are.

The editor keeps instances sorted (by type, cell, then their values) and numbers their
random values from that order, which is the order it saves them in, so reading the file back
gives exactly what was drawn. Erasing renumbers the instances after the gap, so with
`foliage_density` below 1 a different subset of them may be drawn afterwards.

`editor --foliage-stroke` (tests) paints one stroke on the scene's first Foliage with the
window's code, undoes and redoes it, erases part of it and undoes that, then saves, and exits
1 if any step disagrees.

## Settings

- `foliage_density` 0 to 1 (presets: Low 0.4, Medium 0.7, High and Ultra 1): the share of
  instances drawn. 0 draws none and makes no draw calls.
- `foliage_distance` 0.5 to 1.5 (default 1): scales every type's distance and fade.

## Tools

```
player --foliage-info foliage/meadow.swfoliage [--json file]
```

prints the instance count, cells and each type's model, distance, fade, shadows, wind,
instances and cells, with a warning for a model that isn't in Content.

`player --check` reports a Foliage that names no asset, a foliage file that doesn't load
(with the reason: a byte count not divisible by 24, a type out of range, a bad header) and a
type whose model isn't loaded.

The graphics report (`--graphics-report`) has a foliage line and a `foliage` object: the
components drawn, cells in range and drawn, instances in range and drawn, the calls made,
the CPU time spent choosing and submitting them (`submit_ms`), and the two settings.

## Not done

- Removing a type in the editor, and painting several types at once.
- The editor drops instances through a physics copy of the scene made when a stroke starts
  or a brush is picked; a collider moved since shows up at the next stroke.
- Per-instance fade distance: fade is worked out per 16 m cell, so a cell thins out as one.
- Instances aren't lit by baked probes, and skinned meshes in a foliage model are skipped.
- The wind is one direction and strength for every type; there are no gusts.
