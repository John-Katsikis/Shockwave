# Models and textures

This page explains how to bring 3D models and images into a Shockwave project, what the
renderer can show today, and what happens when a file changes or fails to load. If you
are new to Shockwave, start with [Tutorial: your first game in Shockwave](Tutorial.md).

## Add a model to your project

Save your files in the project's content folder. That is `Content/` unless the
`assets` line in your `.swproject` names another folder. Assets are named by their path
inside that folder, with forward slashes, such as `models/material-demo.glb`.

The editor and the standalone player both import the same assets before they load the
scene. To try things out, use `Games/Sample/Content/models/material-demo.glb`. It is a
small model with three materials, made for this project.

## Supported file types

| Kind | Formats |
|---|---|
| Images | PNG, JPEG, TGA, BMP, PSD, GIF, HDR, PNM |
| Models | glTF and GLB, FBX, OBJ with MTL, Collada, STL, PLY, Blender `.blend` (see below), and the rest of Assimp's default readers |
| Audio | WAV and Ogg Vorbis (see [Audio](Audio.md)) |

Models are read with Assimp 6.0.5, an open source model import library, built with its
default importers. USD is not enabled. The function `modelExtensions()` returns the list
of formats your build actually supports. A format being listed doesn't mean every
variant of it will load.

## Place a model in a scene

Models appear in the editor's Content panel next to your other files. To put one in the
scene, do any of these:

- Drag it into the viewport.
- Double-click it.
- Use its placement menu.

This creates an entity with a `Transform` and a `ModelRenderer` component. In the
Inspector you can change the model, its visibility and its tint, and undo works as usual.
To swap in a different model, drop it onto the Inspector's Model field.

The Inspector's imported material section shows the material settings from the source
file. You can't edit them in Shockwave. Change them in your modeling tool and save, and
the model reloads.

## What a model keeps when imported

A model keeps:

- every triangle part, with its own material
- its node hierarchy (the parent and child transforms from your modeling tool)
- repeated copies of the same mesh
- vertex colors and the first UV set (UV0)
- normals, which are generated if the file has none

The whole model is one entity in the scene. Its imported nodes are not separate entities
you can edit. If you parent that entity to another one, the whole model moves with it.

The selection outline uses the model's imported bounding box. Clicking in the viewport
tests the actual triangles. A negative scale keeps faces pointing the right way.

## What materials look like today

The renderer currently shows:

- base color, from a color value or a texture
- vertex colors
- opaque surfaces, alpha cutouts and basic alpha blending
- double-sided surfaces

Textures embedded in a GLB and separate image files are both decoded. glTF's texture
orientation is kept.

Some limits apply:

- Transparent parts are sorted by depth. Transparent surfaces that cut through each
  other can still draw in the wrong order.
- Only UV0 is used. Extra UV sets are ignored.
- All textures use one shared sampler that repeats and filters linearly. Per-texture
  sampler settings and texture transforms are not applied.

Some material data is stored but not drawn yet. Metallic and roughness values, and
normal, metallic-roughness, emissive and occlusion maps, are kept in `ModelData`. Full
PBR shading (physically based lighting), normal mapping, emissive light and occlusion
come with a later lighting update. Until then, the import prints a warning when a model
uses them.

Skinned meshes and animated scenes show their static pose, and the import reports that
playback isn't supported. Skeletal animation is not part of this asset support.

## Use Blender files

Assimp's own `.blend` reader is deprecated and doesn't work well with modern Blender
files. So Shockwave runs Blender itself to export a temporary GLB, then imports that.

Install Blender so it is on your PATH, or tell Shockwave where it is with the
`SHOCKWAVE_BLENDER` environment variable:

```sh
SHOCKWAVE_BLENDER=/absolute/path/to/blender ./build/bin/editor
```

This starts the editor and uses that Blender for `.blend` files. The path is passed as
one argument, so spaces in it are fine.

What the conversion does:

- Runs Blender in background mode with automatic script execution turned off.
- Gives up after 120 seconds.
- Works in its own temporary folder, which is deleted after the import.
- Never changes your `.blend` file.
- Watches the images and linked libraries the `.blend` uses, so editing them reloads it.

Blender's glTF exporter decides which Blender features come through. Custom shader
node setups, simulations and animation playback are not supported by this renderer.

If Blender can't run or can't export the file, Shockwave falls back to Assimp's old
reader and reports its limits. Older Blender files may load that way. If you don't have
Blender installed, export glTF or GLB from your tool yourself.

Modern `.blend` conversion was tested with a temporary Blender 4.5.3 build. That build
is not a dependency of the project.

## When a file changes or breaks

Saving any of these files reimports the models that use it:

- the model file
- an image it uses
- a glTF `.bin` file
- an OBJ `.mtl` file

If a file is missing, fixing or restoring it triggers another import attempt.

If an import fails, the model keeps its last working version. A failed model's tile in
the Content panel shows the error. Right-click it and choose **Retry imports**, for
example after you install or set up Blender.

If you delete a file from disk while the editor runs, the model keeps its last working
copy. When you reopen the project, though, the file must exist.

## Use models from game code

Scene files save models by name, not by GPU ID. `ModelRenderer` is registered with the
same reflection system as every other component, so it saves, loads and shows in the
Inspector like the rest.

The editor and player draw models for you. Code that draws the world itself, as the
player and the sandbox do, passes the asset store to `renderWorld`:

```cpp
renderWorld(world, renderer, jobs, alpha, &engine.assets());
```

## Run the tests

```sh
cmake --build build -j 4
ctest --test-dir build --output-on-failure
# Opt-in: requires a desktop and Vulkan GPU, not part of headless CTest.
./build/bin/model_gpu_smoke .
```

The first two commands build everything and run the CPU tests. The last one runs a GPU
test that opens a window, so it needs a desktop and a Vulkan GPU.

The CPU test covers:

- glTF and GLB, with embedded and external PNGs
- OBJ triangulation and MTL files
- nested transforms and materials
- triangle picking
- repairing a missing dependency
- stable asset references and scene saving

The GPU test covers real uploads, repeated reloads, missing buffers and textures, MTL
edits, keeping the last working version, reusing GPU resources, and drawing all three
material parts.

The test models were made for this project. To regenerate them:

```sh
python3 Tests/fixtures/models/generate.py
```

## Audio clips

WAV and Ogg Vorbis files in the content folder are named assets too. They import and
reload in the background, like models, and a failed reload keeps the last working
version. See [Audio](Audio.md) for how to use them.

## How it works

Imports decode on worker threads. The main thread then uploads the finished model to
the GPU and swaps it in between frames. A failed CPU or GPU import keeps the last
working model.

A reloaded model keeps the same handle, so nothing that refers to it breaks. Its old GPU
resources are recycled once the GPU has finished using them. The list of files a model
depends on includes files that are missing, which is why restoring one retries the
import.

Models are imported from their source files every time the project starts. Large
projects will eventually need an import cache that stores converted files on disk.
Shader features, complex dependencies from content tools, and animation are separate
work.
