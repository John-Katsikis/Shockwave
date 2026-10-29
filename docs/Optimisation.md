# Texture compression and levels of detail

Shockwave makes your textures and meshes cheaper to draw without you doing anything.
Textures take a quarter of the GPU memory, and far-away meshes are drawn with fewer
triangles. Both happen at full resolution: Shockwave doesn't upscale, it does less work
for the same picture.

This page shows what happens by default, how to see what it saves, and how to change it
for a whole project or a single asset.

## What happens by default

**Textures are compressed to BC7.** BC7 is a compressed format desktop GPUs sample
directly. A BC7 texture takes a quarter of the memory and bandwidth of an uncompressed
one and looks almost the same.

- The first time a texture is imported, it's compressed in the background. That takes a
  second or so per texture. Until it's done, the texture is drawn uncompressed, then it
  swaps in by itself.
- The compressed copy is kept in `Saved/Cache/Textures/` inside your project. Next time,
  the texture loads compressed straight away.
- Model textures are compressed the same way as texture files.
- Compression uses one low-priority thread, so frames stay smooth while it works.

**Meshes get levels of detail.** A level of detail is a simpler version of a mesh, used
when the mesh is far enough away that you can't tell the difference.

- Every model mesh with at least 256 triangles gets up to 4 simpler levels. Each has
  about half the triangles of the one before.
- For every draw, the renderer picks the simplest level that stays within 1 pixel of the
  full mesh on screen. Closer or bigger objects use more detail.
- The built-in `sphere` mesh has levels too.
- Picking in the editor, physics and navigation always use the full mesh.

**Triangles are reordered.** Each mesh's triangles are put in an order that lets the GPU
reuse work it has already done for shared corners, and draw less of what's hidden. Your
mesh looks exactly the same.

## See what it saves

**In the editor**, open **Window > Optimisation**. It shows:

- the project's settings;
- the texture or model selected in the Content window, with its GPU memory, whether it's
  compressed, and its triangles per level;
- totals for every asset, and how many textures are still being compressed;
- how many triangles the last frame drew, against what full detail would have drawn.

**From the command line**, compress everything now and print a report:

```sh
build/bin/player Games/Sample/Sample.swproject --optimise --renderer vulkan
```

```
Every texture is already in Saved/Cache/Textures
texture  Content/textures/bricks.png  256x256  BC7, 0.08 MB on the GPU (0.33 MB uncompressed); compression high
texture  Content/textures/wood.png  256x256  BC7, 0.08 MB on the GPU (0.33 MB uncompressed); compression high
2 of 2 textures compressed: 0.17 MB on the GPU instead of 0.67 MB (75% less); 0.03 MB of .ktx2 files
```

- Add `--json report.json` to also write the numbers as JSON.
- `--renderer vulkan` checks your GPU. Without it no GPU is used, so sizes are shown
  uncompressed, but the cache is still filled.
- Run it before you ship or commit, so the cache is ready and nobody waits for
  compression the first time they open the project.

## See the levels of detail

To tint each mesh by the level it's drawn with, do either of these:

- In the editor, tick **Show levels of detail** in the Optimisation window.
- In the player, add `--lod-colors`:

  ```sh
  build/bin/player Games/Sample/Sample.swproject --scene Games/Sample/Scenes/lod.swscene --lod-colors
  ```

| Colour | Level |
|---|---|
| Green | Full detail |
| Yellow | 1 (about half the triangles) |
| Orange | 2 |
| Red | 3 |
| Magenta | 4 |
| Blue, cyan, purple | 5 to 7 |

The sample scene `Scenes/lod.swscene` has rows of spheres going into the distance. It's
the quickest way to try this.

## Change the project's settings

Change them in **Window > Optimisation**, or edit these lines in your `.swproject` file.
All three are optional.

```
texture_compression high  # high, small or off
mesh_lods 4               # simpler levels per mesh, 0 to 7; 0 turns them off
lod_error_pixels 1        # how far, in pixels, a level may stray on screen
```

| Key | Default | What it does |
|---|---|---|
| `texture_compression` | `high` | How textures are compressed. See [Pick a compression mode](#pick-a-compression-mode). |
| `mesh_lods` | `4` | How many simpler levels each model mesh gets, `0` to `7`. |
| `lod_error_pixels` | `1` | `0` to `100`. Lower is sharper, higher draws fewer triangles. `0` always draws full detail. |

Changing `texture_compression` or `mesh_lods` re-imports every asset. Changing
`lod_error_pixels` takes effect on the next frame.

## Change one asset

Some textures need different settings, such as pixel art that must stay exact. To
change one asset:

1. Select it in the Content window.
2. In **Window > Optimisation**, under **Selected asset**, pick its compression, its
   number of levels, or whether it's a UI texture.

The editor saves your choice next to the asset, in a file named after it with
`.swimport` added: `bricks.png.swimport`. **Use project settings** removes the file.

You can also write the file yourself. It's plain text, and lists only what differs from
the project:

```
# bricks.png.swimport
shockwave_import 1
compression off
```

| Key | Values | For |
|---|---|---|
| `compression` | `high`, `small`, `off` | Textures, and a model's textures |
| `lods` | `0` to `7` | Models |
| `category` | `material`, `ui` | Textures, and a model's textures. `ui` keeps every level whatever the player's Textures quality; use it for menu art, icons and anything read as text |

- Saving the file re-imports its asset, in the editor and in a running game.
- A mistake in the file, such as an unknown key or a bad value, makes the asset fail to
  import, with the file name and line number in the Console.
- A `.swimport` file whose asset doesn't exist is reported as a problem too, so a
  renamed asset doesn't silently lose its settings.

## Pick a compression mode

| Mode | GPU memory | Cache file size | Looks | Use it for |
|---|---|---|---|---|
| `high` | a quarter | medium | almost identical | Most textures. The default. |
| `small` | a quarter | small | a little softer | Big projects where download size matters. |
| `off` | full | none | exact | Pixel art, colour lookup tables, anything that must stay exact. |

`high` and `small` take the same GPU memory. They differ in how the cache file is stored
(UASTC or ETC1S, two Basis Universal formats), and so in its size and detail.

## Use your own .ktx2 files

You can put Basis Universal `.ktx2` files in `Content/` and use them like any other
texture. They're used as they are, not compressed again. Make them with the `basisu` tool,
with mipmaps:

```sh
basisu -ktx2 -uastc -mipmap wood.png
```

Only 2D textures work. Cube maps and texture arrays don't yet.

## Check your work from the command line

Everything above is text, so scripts and AI tools can do it too:

- Write `texture_compression`, `mesh_lods` and `lod_error_pixels` in the `.swproject`,
  and `.swimport` files next to assets.
- Run `build/bin/player --check`. It fails on a broken or orphaned `.swimport` file, like
  any other import problem.
- Run `build/bin/player --optimise --json report.json` and read the numbers.
- Take a screenshot with `--screenshot out.png --lod-colors` to see the levels used.

## Limits

- A model's normal, metallic-roughness and occlusion maps are compressed as data
  (linear), and its base color and emissive maps as colour (sRGB).
- A GPU that can't sample BC7 draws textures uncompressed. The cache still holds the
  compressed copy. The Optimisation window and `--optimise` tell you when this happens.
- Levels switch from one frame to the next, without blending. At the default of 1 pixel
  you can't see the switch.
- Changing a mesh's levels or a texture's compression re-imports that asset.
- Delete `Saved/Cache/Textures/` whenever you like. It's rebuilt as needed.

## How it works

- **Meshes** (`Assets/MeshOptimise.h`) use [meshoptimizer](https://github.com/zeux/meshoptimizer)
  1.3. Triangles are reordered for the vertex cache and for overdraw, without moving
  vertices. Each level is simplified from the full mesh, with near-identical positions
  welded first so seams can't crack. Levels live in their own index buffer, so
  `MeshData::indices` stays full detail for picking, physics and navigation.
- **Choosing a level** happens per draw in the Vulkan renderer
  (`RendererOptimisation.cpp`). The renderer turns each level's error into pixels from the
  object's distance and size and the camera's field of view, and picks the simplest level
  under `lod_error_pixels`.
- **Textures** (`Assets/TextureCompression.h`) use
  [Basis Universal](https://github.com/BinomialLLC/basis_universal) 2.50. A texture is
  encoded once into a `.ktx2` file with mipmaps, then transcoded to BC7 at load, which
  takes milliseconds. Cache files are named after the asset and a hash of its pixels and
  settings, so an edited texture or a changed setting never picks up a stale copy.
- **Background compression** runs on one low-priority thread of its own, leaving the job
  system free for frames. `--optimise` and **Finish now** use every thread instead.
- Both survive swapping the renderer: levels and compressed textures are replayed into
  the new one.
