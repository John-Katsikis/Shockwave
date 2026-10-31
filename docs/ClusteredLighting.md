# Clustered lighting

A scene can have up to 256 lights a frame. Without clustering, every pixel of every surface
tests every point and spot light to see whether it reaches. That's fine with a handful of
lights and slow with a lot of them. Clustered lighting sorts the lights first. The view is
cut into a grid of screen tiles and distances, and each cell (a *cluster*) gets a list of
the lights that can reach it. A pixel then tests only its own cluster's list.

The picture is exactly the same either way, pixel for pixel. Clustering only changes which
lights a pixel tests, never how they light it: the same maths, shadows and colours, added
up in the same order.

It's the player's `light_culling` graphics setting (docs/Graphics.md):

- `auto` (the default) clusters when a frame has 8 or more point and spot lights, and
  tests every light below that, where building the lists costs more than it saves.
- `clustered` always clusters, where clusters apply.
- `brute_force` tests every light everywhere: no cluster pass and no memory for it. This
  is the reference the clustered result is checked against.

```
player --graphics-set light_culling=brute_force
player --view clusters      # how many lights each pixel's cluster lists
```

The editor's *Shading* menu has the same view (*Light clusters*).

## The scene's part: cluster_far

The camera's view has no far end, so clusters need one. Each scene's Environment sets
`cluster_far`: how far from the camera (in metres, 250 by default) lights are sorted into
clusters. Beyond it, pixels test every light again, so nothing goes missing out there. It's
only slower. Keep it around the farthest point or spot light worth seeing; there's no need
to change it in most scenes.

## How it works

The rules are in `Renderer/Clusters.h`, with CPU tests in `Tests/ClusterTest.cpp`.

1. **The grid.** Tiles of 32 x 32 render pixels, and 16 slices of distance from the
   camera's near plane to `cluster_far`. The slices are logarithmic, so each is a little
   deeper than the one before, which keeps near clusters small:
   `slice = floor(log(z / near) / log(far / near) * 16)`. At 1920 x 1080 that's
   60 x 34 x 16 = 32640 clusters. Each lists up to 64 lights, packed four to a word, in
   68 bytes: 2.1 MiB per frame in flight. If the lists would pass 16 MiB, the tiles double
   in size until they fit.
2. **The lists.** A compute pass (`Shaders/clusters.comp`) gives each cluster a box in view
   space: its tile, grown by a pixel each way because TAA's jitter moves samples by up to
   half a pixel, between its slice's distances. It lists every point and spot light whose
   range reaches that box, in the light table's order. Point lights use their range sphere.
   Spot lights use the same sphere, and their cone is checked when shading. Each list
   covers its cluster's whole volume, not just the opaque surfaces in it, so glass and
   other transparent surfaces in front of or behind them find the right lights too.
3. **Shading** (`Shaders/mesh.frag`). Directional lights aren't clustered: every pixel
   tests them first, as a short list. Then the pixel works out its cluster from its screen
   position and distance and loops over that list. The loop over every light and the loop
   over a list call the same function in the same order.

## When every light is tested anyway

A light is never dropped. A pixel tests every point and spot light when:

- its cluster's list overflowed (more than 64 lights reach it); the view shows these in
  magenta;
- it's past `cluster_far`; the view shows these in grey;
- the view is orthographic (the editor's top and side views), or the frame is a
  reflection probe capture;
- the setting is `brute_force`, or `auto` with fewer than 8 point and spot lights.

## Checking it

`player --graphics-report` shows the last frame under `clusters`:

- whether it clustered, or why not;
- the tile size and grid;
- `cluster_far`, the point and spot lights, and the lists' memory;
- from the GPU, a frame or two late: the most lights in one cluster, the average, and how
  many clusters overflowed.

The `light_culling` setting's detail line says the same in a sentence. The lists' memory
shows under *light clusters*.

**The cluster view** (`--view clusters`) colours each pixel by how many lights its cluster
lists:

| Colour | Lights |
|---|---|
| Dark blue | none |
| Blue, then green and yellow | some |
| Red | 32 or more |
| Magenta | overflowed |
| Grey | every light tested |

## Measured

All of these were measured on llvmpipe (the CPU Vulkan driver), so the numbers aren't
absolute.

**Many lights.** The `many-lights` fixture has 200 point lights, 4 spot lights (2 with
shadows) and a sun. At 1280 x 720:

| | Brute force | Clustered |
|---|---|---|
| Scene pass | 168 to 178 ms | 101 to 104 ms |
| Building the lists | none | 3.5 ms |
| Whole GPU frame | 262 ms | 201 ms |

That's about 40% off the scene pass, and 23% off the frame.

**Few lights.** `lights-overlap` has 4 lights. The scene pass is about the same either way
(74 to 88 against 77 ms), and building the lists adds 2.5 ms. That's why `auto` waits for
8 lights.

## Limits

- The global limit is still 256 lights a frame; clustering makes them cheaper, not more
  numerous.
- Spot lights are listed by their range sphere, not their cone, so a cluster may list a
  spot light that points away from it (the cone test while shading still keeps it dark).
- Lists are fixed at 64 lights a cluster. Past that, the cluster tests every light, which
  is correct but slower; a compact variable-length list would remove that.
- Lit particles read the same cluster lists with `particle_lighting scene` (docs/Effects.md), without shadows.
